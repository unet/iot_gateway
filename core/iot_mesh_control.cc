#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include "iot_mesh_control.h"
#include "iot_netproto_mesh.h"
#include "iot_peerconnection.h"
#include "iot_threadregistry.h"
#include "iot_configregistry.h"
#include "qsort.h"


//absolute change below this value of microseconds does not initiate routing table sync to peers
#define ROUTEDELAY_ABSTHRESHOLD 50

//relative change below this value of percents does not initiate routing table sync to peers
#define ROUTEDELAY_RELTHRESHOLD 10

iot_netcontype_metaclass_meshses iot_netcontype_metaclass_meshses::object;


void iot_meshroute_entry::init(iot_netproto_session_mesh* ses, const iot_objref_ptr<iot_peer> &peer) {
		session=ses;
		hostpeer=peer;
		hostid=hostpeer->host_id;
		next=prev=NULL;
		orig_next=orig_prev=NULL;
		fulldelay=realdelay=0;
		pendmod=MOD_NONE;
	}

void iot_meshnet_controller::sync_routing_table_frompeer(iot_netproto_session_mesh* ses, iot_netproto_session_mesh::packet_rtable_req *req) {  //called from session's thread
		assert(uv_thread_self()==ses->thread->thread);
		assert(ses->peer_host!=NULL && ses->state==ses->STATE_WORKING);

		iot_peer* peer=(iot_peer*)ses->peer_host;
		uint64_t ver=repack_uint64(req->version);
		uint32_t numroutes=repack_uint16(req->num_items);
		int err;

		//quick check before locking
		if(ver<=peer->origroutes_version.load(std::memory_order_relaxed)) return; //nothing to do

		uv_rwlock_wrlock(&routing_lock);
		if(ver<=peer->origroutes_version.load(std::memory_order_relaxed)) { //recheck inside WRITE lock
			uv_rwlock_wrunlock(&routing_lock);
			return;
		}
outlog_notice("Got routing table from %" IOT_PRIhostid " with %u items, version %llu:", peer->host_id, numroutes, ver);
		uint32_t maxhosts=gwinst->config_registry->get_num_hosts();
		uint32_t maxpathlen=maxhosts > 1 ? maxhosts-2 : 0;

		if(peer->maxorigroutes < numroutes) { //check there is enough space for new routes
			uint32_t nmax=maxhosts+5;
			if(numroutes+4 > nmax) nmax=numroutes+4;
			err=peer->resize_origroutes_buffer(nmax, ses->coniface->allocator); //corresponds to session's thread
			if(err) {
				outlog_error("Got error resizing original routes buffer: %s", kapi_strerror(err));
				uv_rwlock_wrunlock(&routing_lock);
				return;
			}
		}

		if(!repack_is_nullop) { //fix byteorder inplace
			for(uint32_t i=0;i<numroutes;i++) {
				req->items[i].hostid=repack_hostid(req->items[i].hostid);
				req->items[i].delay=repack_uint32(req->items[i].delay);
				req->items[i].pathlen=repack_uint16(req->items[i].pathlen);
			}
		}
		if(numroutes>1) { //sort items by ascending hostid for quicker comparison with existing records
			Quicksort<iot_netproto_session_mesh::packet_rtable_req::item_t, iot_hostid_t, &iot_netproto_session_mesh::packet_rtable_req::item_t::hostid>(req->items, numroutes);
		}

		//here new list of routes is sorted. existing list is sorted too because it is created after sorted list
		//so we can use two index variables to move through both lists in single pass
		uint32_t newidx, oldidx; //index in new list and old list
		uint32_t numoldroutes=peer->numorigroutes;
		uint32_t fixheadfrom=0xffffffff; //when < 0xffffffff shows that head pointers must be fixed in new origroutes starting from this index due to insertion or deletion
		bool wasinsert=false; //true value means that additional pass over final origroutes must be done to finish inserts into sessions' route entries
		bool wasunknownhost=false;
		newidx=oldidx=0;
		while(newidx<numroutes && !req->items[newidx].hostid) newidx++; //skip illegal zero host id (list is sorted, so such values can be at beginning only)

		iot_hostid_t newhostid,prevnewhostid=0;
		while(newidx<numroutes || oldidx<numoldroutes) { //while any of lists not finished
			iot_meshorigroute_entry *olditem=&peer->origroutes[oldidx];
			if(newidx<numroutes) { //list of new routes is not exhausted
				iot_netproto_session_mesh::packet_rtable_req::item_t &newitem=req->items[newidx];
outlog_notice("item dst=%" IOT_PRIhostid ", delay=%u", newitem.hostid, newitem.delay);

				newhostid=newitem.hostid;
				if(newhostid==prevnewhostid || newhostid==peer->host_id || newhostid==gwinst->this_hostid || newitem.delay>ROUTEENTRY_MAXDELAY || newitem.pathlen>maxpathlen) {newidx++; continue;} //skip duplicate or illegal hosts or routes
				if(oldidx>=numoldroutes || olditem->hostid > newhostid) { //new entry must be added
					//make space for new entry
					if(oldidx<numoldroutes) {
						memmove(&peer->origroutes[oldidx+1], olditem, sizeof(*olditem)*(numoldroutes-oldidx));
						if(fixheadfrom==0xffffffff) fixheadfrom=oldidx+1;
					}
					numoldroutes++;
					*olditem={
						.routeslist_head=NULL,
						.hostid=newhostid,
						.hostpeer=gwinst->peers_registry->find_peer(newhostid), //NULL can be here is host is still unknown
						.delay=newitem.delay,
						.pathlen=newitem.pathlen
					};
					if(!olditem->hostpeer) wasunknownhost=true; //mark there is at least one unknown host
						else wasinsert=true;
					oldidx++;
					//insert will be finished in additional pass after this cycle
					newidx++;
					prevnewhostid=newhostid; //remember already processed host to protect against repetitions
					continue;
				}
				if(olditem->hostid == newhostid) { //entry must be updated
					olditem->delay=newitem.delay;
					olditem->pathlen=newitem.pathlen;

					if(olditem->hostpeer) {
						for(iot_meshroute_entry *curentry=olditem->routeslist_head; curentry; curentry=curentry->orig_next) {
							curentry->update(newitem.delay, newitem.pathlen+1, curentry->session->cur_delay_us);
						}
					} else {
						assert(olditem->routeslist_head==NULL);
					}
					oldidx++;
					newidx++;
					prevnewhostid=newhostid; //remember already processed host to protect against repetitions
					continue;
				}
			}
			//here list of new routes is exhausted OR (olditem->hostid < newhostid), oldentry must be removed
			assert(oldidx<numoldroutes);

			//mark existing route entries for deletion and disconnect them from old orig entry
			if(olditem->hostpeer) {
				if(fixheadfrom<=oldidx) BILINKLIST_FIXHEAD(olditem->routeslist_head, orig_prev);
				iot_meshroute_entry *curentry, *nextentry=olditem->routeslist_head;
				while((curentry=nextentry)) {
					nextentry=nextentry->orig_next;
					curentry->pendmod=curentry->MOD_REMOVE;
					BILINKLIST_REMOVE(curentry, orig_next, orig_prev);
				}
				olditem->hostpeer=NULL; //clear peer reference
			}
			assert(olditem->routeslist_head==NULL);

			//remove oldentry
			if(oldidx+1<numoldroutes) { //not the last entry
				memmove(olditem, &peer->origroutes[oldidx+1], sizeof(*olditem)*(numoldroutes-oldidx-1));
				if(fixheadfrom>oldidx) fixheadfrom=oldidx;
			}
			numoldroutes--;
		}
		peer->numorigroutes=numoldroutes;
		peer->origroutes_unknown_host=wasunknownhost;
		peer->origroutes_version.store(ver, std::memory_order_relaxed);

		//do final actions
		for(uint32_t i=fixheadfrom;i<numoldroutes;i++) {
			//fix head because of orig entry relocation
			BILINKLIST_FIXHEAD(peer->origroutes[i].routeslist_head, orig_prev);
		}

		//apply changes
		uint64_t new_routes_version;
		new_routes_version=gwinst->next_event_numerator();

		for(iot_netproto_session_mesh *curses=peer->meshsessions_head; curses; curses=curses->peer_next) {
			//apply before doing insert to guarantee free space in sessions' route lists
			apply_session_routes_change(curses, true, new_routes_version);
			if(curses->maxroutes < 1+numoldroutes) { //check there is enough space for direct route and all original routes
				uint32_t nmax=maxhosts+5;
				if(1+numoldroutes+4 > nmax) nmax=1+numoldroutes+4;
				err=curses->resize_routes_buffer(nmax, ses->coniface->allocator); //corresponds to session's thread
				if(err) continue; //? log something?
			}
		}
		if(wasinsert) { 
			for(uint32_t i=0;i<numoldroutes;i++) {
				iot_meshorigroute_entry &item=peer->origroutes[i];
				if(!item.hostpeer || item.routeslist_head) continue;
				for(iot_netproto_session_mesh *curses=peer->meshsessions_head; curses; curses=curses->peer_next) {
					//try to use existing hole in list of sessions routes
					uint32_t j;
					for(j=0; j<curses->numroutes; j++) if(curses->routes[j].hostid==0) break;
					//j contains index of hole or of free record at the end
					if(j>=curses->numroutes) {
						assert(j==curses->numroutes);
						if(j>=curses->maxroutes) continue; //was error resizing routes buffer
						curses->numroutes++;
					}
					curses->routes[j].init(curses, item.hostpeer);
					BILINKLIST_INSERTHEAD(&curses->routes[j], item.routeslist_head, orig_next, orig_prev);
					curses->routes[j].update(item.delay, item.pathlen+1, curses->cur_delay_us);
				}
			}
			//apply inserts
			for(iot_netproto_session_mesh *curses=peer->meshsessions_head; curses; curses=curses->peer_next) {
				apply_session_routes_change(curses, true, new_routes_version);
			}
		}

//onexit:
		uv_rwlock_wrunlock(&routing_lock);
	}

void iot_meshnet_controller::sync_routing_table_topeer(iot_netproto_session_mesh* ses) { //called from session's thread
		assert(uv_thread_self()==ses->thread->thread);
		assert(ses->peer_host!=NULL && ses->state==ses->STATE_WORKING);

		iot_peer* peer=(iot_peer*)ses->peer_host;

		if(!peer->notify_routing_update && peer->current_routingversion<=peer->lastreported_routingversion.load(std::memory_order_relaxed)) return;

		uv_rwlock_wrlock(&routing_lock);
		if(peer->current_routingversion<=peer->lastreported_routingversion.load(std::memory_order_relaxed)) { //recheck inside WRITE lock to be able to reset notify_routing_update
			peer->notify_routing_update=false;
			ses->routing_pending=false;
			uv_rwlock_wrunlock(&routing_lock);
			return;
		}

		uint32_t maxnum_peers=gwinst->peers_registry->get_num_peers();
		iot_peer** peerbuf=NULL;
		uint32_t num_peers=0, num_routes=0, cur_route=0;
		if(maxnum_peers>0) {
			size_t sz=maxnum_peers*sizeof(iot_peer*);
			peerbuf=(iot_peer**)alloca(sz);
			num_peers=gwinst->peers_registry->copy_meshpeers(peerbuf, sz, gwinst->peers_registry->MODE_ROUTABLE); //list of peers with mesh sessions cannot change while routing is locked
		}

		//count size of routing table
		for(uint32_t i=0;i<num_peers;i++) {
			iot_peer *p=peerbuf[i];
			assert(p->routing_actualpathlen>0); //copy_meshpeers must skip peers without routes
			assert(p->routeslist_head!=NULL);

			if(p==peer) continue;
			if(p->routeslist_head->session->peer_host==peer) { //must use altrouting
				if(p->routing_altactualpathlen) {
					assert(p->routeslist_althead!=NULL);
					num_routes++;
				}
			} else {
				num_routes++;
			}
		}
		if(num_routes>65535) {
			assert(false);
			num_routes=65535;
		}

		uint32_t packetsize=sizeof(iot_netproto_session_mesh::packet_rtable_req)+num_routes*sizeof(iot_netproto_session_mesh::packet_rtable_req::items[0]);
		iot_netproto_session_mesh::packet_hdr* hdr=ses->prepare_packet_header(ses->CMD_RTABLE_REQ, packetsize);
		iot_netproto_session_mesh::packet_rtable_req *req=(iot_netproto_session_mesh::packet_rtable_req*)(hdr+1);
		memset(req, 0, sizeof(*req));
		req->version=repack_uint64(peer->current_routingversion);
		req->num_items=repack_uint16(num_routes);

		for(uint32_t i=0;i<num_peers;i++) {
			iot_peer *p=peerbuf[i];
			if(p==peer) continue;
			if(p->routeslist_head->session->peer_host==peer) { //must use altrouting
				if(p->routing_altactualpathlen) {
					req->items[cur_route++]={.hostid = repack_hostid(p->host_id), .delay=repack_uint32(p->routing_altactualdelay), .pathlen=repack_uint16(p->routing_altactualpathlen), .reserved=0};
				}
			} else {
				req->items[cur_route++]={.hostid = repack_hostid(p->host_id), .delay=repack_uint32(p->routing_actualdelay), .pathlen=repack_uint16(p->routing_actualpathlen), .reserved=0};
			}
		}

		uv_rwlock_wrunlock(&routing_lock);

		int err;
		err=ses->coniface->write_data(hdr, sizeof(*hdr)+packetsize);
		assert(err==0);
}

void iot_meshnet_controller::confirm_routing_table_sync_topeer(iot_netproto_session_mesh* ses) { //called from session's thread
		assert(uv_thread_self()==ses->thread->thread);
		assert(ses->peer_host!=NULL && ses->state==ses->STATE_WORKING);

		uv_rwlock_rdlock(&routing_lock);

		ses->peer_host->lastreported_routingversion.store(ses->peer_host->current_routingversion, std::memory_order_relaxed);

		uv_rwlock_rdunlock(&routing_lock);
}

int iot_meshnet_controller::register_session(iot_netproto_session_mesh* ses) { //called from session's thread
		assert(uv_thread_self()==ses->thread->thread);
		assert(ses->peer_host!=NULL && ses->peer_prev==NULL && ses->state==ses->STATE_WORKING && ses->numroutes==0);
		int err=0;

		iot_peer* peer=(iot_peer*)ses->peer_host;
		uint32_t numorigroutes=peer->numorigroutes;

		uv_rwlock_wrlock(&routing_lock);

		if(ses->maxroutes < 1+numorigroutes) { //check there is enough space for direct route and all existing original routes from peer
			uint32_t nmax=gwinst->config_registry->get_num_hosts()+5;
			if(1+numorigroutes+4 > nmax) nmax=1+numorigroutes+4;
			err=ses->resize_routes_buffer(nmax, ses->coniface->allocator); //corresponds to session's thread
			if(err) goto onexit;
		}
		//add route to direct peer
		ses->routes[0].init(ses, peer);
		ses->routes[0].update(0, 0+1, ses->cur_delay_us);

		//add entry for each existing original route
		for(uint32_t i=0;i<numorigroutes;i++) {
			iot_meshroute_entry* entry=&ses->routes[i+1];
			iot_meshorigroute_entry* origentry=&peer->origroutes[i];

			entry->init(ses, origentry->hostpeer);
			entry->update(origentry->delay, origentry->pathlen+1, ses->cur_delay_us);

			BILINKLIST_INSERTHEAD(entry, origentry->routeslist_head, orig_next, orig_prev); //mark dependence of entry on origentry
		}
		ses->numroutes=numorigroutes+1;

		BILINKLIST_INSERTHEAD(ses, peer->meshsessions_head, peer_next, peer_prev);
		outlog_debug("New MESH session to host_id=%" IOT_PRIhostid " registered", peer->host_id);

		apply_session_routes_change(ses, true);

		if(!peer->notify_routing_update && peer->current_routingversion>peer->lastreported_routingversion.load(std::memory_order_relaxed))
			peer->notify_routing_update=true;

		uv_rwlock_wrunlock(&routing_lock);

		if(peer->notify_routing_update) ses->send_signal();
		return 0;

onexit:
		uv_rwlock_wrunlock(&routing_lock);
		return err;
	}

void iot_meshnet_controller::unregister_session(iot_netproto_session_mesh* ses) { //called from session's thread
		assert(ses!=NULL);
		if(!ses->peer_host || !ses->peer_prev) return; //nothing to do. non-started session or already unregistered

		assert(ses->closed==3 || uv_thread_self()==ses->thread->thread); //allow any thread for stopped sessions
		iot_peer* peer=(iot_peer*)ses->peer_host;

		uv_rwlock_wrlock(&routing_lock);

		BILINKLIST_REMOVE(ses, peer_next, peer_prev);
		outlog_debug("MESH session to host_id=%" IOT_PRIhostid " un-registered", peer->host_id);

		uint64_t ver;
		if(!peer->meshsessions_head) {
			ver=gwinst->next_event_numerator(); //increase version when last mesh session to some peer is closed
			myroutes_lastversion=ver;
		}
		else ver=0;

		if(ses->numroutes>0) {
			for(uint32_t i=0; i<ses->numroutes; i++) {
				if(ses->routes[i].prev) ses->routes[i].pendmod=ses->routes[i].MOD_REMOVE;
			}
			apply_session_routes_change(ses, true, ver);
			assert(ses->numroutes==0);
		}

		if(!peer->meshsessions_head) { //last session was closed, so no need to keep origroutes from peer
			for(uint32_t i=0;i<peer->numorigroutes;i++) {
				iot_meshorigroute_entry &item=peer->origroutes[i];
				assert(item.routeslist_head==NULL); //closing of sessions must clean this pointer
				item.hostpeer=NULL; //clear hostpeer pointer to release reference count
			}
			peer->numorigroutes=0;
			peer->origroutes_unknown_host=false;
			peer->origroutes_version.store(0, std::memory_order_relaxed);

			//no need to keep flag about routes resync to this peer
			peer->notify_routing_update=false;
		}

		uv_rwlock_wrunlock(&routing_lock);
}

static inline iot_meshroute_entry* find_quicker_route_entry(iot_meshroute_entry *listhead, uint32_t delay) { //finds last entry whose realdelay is <= than provided
		if(!listhead || listhead->realdelay>delay) return NULL; //indicates that item with provided delay must become new head
		iot_meshroute_entry *e=listhead;

		while(e->next && e->next->realdelay<=delay) e=e->next;

		return e;
	}

static inline iot_meshroute_entry* find_althead_entry(iot_meshroute_entry *startentry) { //find fist entry after startentry with different next-hop peer
	assert(startentry!=NULL);
	iot_meshroute_entry *e=startentry->next;
	while(e && e->session->peer_host==startentry->session->peer_host) e=e->next;
	return e;
}


void iot_meshnet_controller::apply_session_routes_change(iot_netproto_session_mesh* ses, bool waslocked, uint64_t new_routes_version) { //can be called in any thread
		assert(ses!=NULL);
		if(!ses->numroutes) return; //nothing to do. non-started session or already stopped

		bool need_resync=false; //shows if actual routing table (first routing entry in any peer's routeslist_head) has changed
		iot_peer** peerbuf=NULL; //set when need_resync becomes true
		uint32_t num_peers=0; //set when need_resync becomes true

		bool locked=waslocked; //lock will be acquired on first necessity
		bool wasremove=false;
		uint32_t newnumroutes=ses->numroutes;

		for(uint32_t i=ses->numroutes-1; i!=0xffffffff; i--) { //go backward to be able to remove all items
			iot_meshroute_entry &e=ses->routes[i];
			iot_peer* peer=(iot_peer*)e.hostpeer;
			if(e.hostid==0) { //unused entry
				assert(peer==NULL && !e.prev && !e.orig_prev && e.pendmod==e.MOD_NONE);
				if(i==newnumroutes-1) newnumroutes--;
				continue;
			}
			if(e.pendmod==e.MOD_NONE) continue; //unusable entry (e.g. delay exceeds maximum) in case e.prev==NULL or just unchanged or entry for unknown peer (iot_peer not found)
			assert(peer!=NULL);

			bool top_updated=false; //flag that realdelay of head item at routeslist_head was changed and check must be done if actual delay was changed
			bool alttop_updated=false; //flag that realdelay of althead item at routeslist_althead was changed and check must be done if need_resync should be activated
			iot_peer* routing_actualnexthop=peer->routeslist_head ? (iot_peer*)peer->routeslist_head->session->peer_host : NULL; //remember current nexthop of top route

			if(!e.prev) { //not inserted entry. can only be inserted
				if(e.pendmod!=e.MOD_INSERT) {
					assert(false); //unallowed pendmod
					continue;
				}
				if(!locked) {
					uv_rwlock_wrlock(&routing_lock);
					locked=true;
				}

				//find correct place to insert after
				iot_meshroute_entry* after=find_quicker_route_entry(peer->routeslist_head, e.realdelay);
				if(!after) { //current entry must become new head
					if(peer->routeslist_head) {
						if(peer->routeslist_head->session->peer_host->host_id != e.session->peer_host->host_id) { //previous head becomes new althead
							peer->routeslist_althead=peer->routeslist_head;
							alttop_updated=true;
						} //else althead is unchanged
					} else { //althead must be NULL
						assert(peer->routeslist_althead==NULL);
					}
					BILINKLIST_INSERTHEAD(&e, peer->routeslist_head, next, prev);
					top_updated=true;
				} else {
					if(!peer->routeslist_althead) { //means that all existing entries have same next-hop peer as head entry
						if(peer->routeslist_head->session->peer_host != e.session->peer_host) { //inserted entry becomes new althead
							peer->routeslist_althead=&e;
							alttop_updated=true;
						} //else althead remains empty
					} else {
						if(peer->routeslist_althead->realdelay>e.realdelay) { //means that new entry will be inserted before althead. all entries before althead have same next-hop peer as head
							if(peer->routeslist_head->session->peer_host != e.session->peer_host) { //inserted entry becomes new althead
								peer->routeslist_althead=&e;
								alttop_updated=true;
							} //else althead remains empty
						}
					}
					BILINKLIST_INSERTAFTER(&e, after, next, prev);
				}
			} else { //entry is in peer's list
				if(e.pendmod==e.MOD_INSERT) {
					assert(false); //unallowed pendmod
					continue;
				}
				if(!locked) {
					uv_rwlock_wrlock(&routing_lock);
					locked=true;
				}
				switch(e.pendmod) {
					case e.MOD_REMOVE:
						if(e.orig_prev) BILINKLIST_REMOVE(&e, orig_next, orig_prev);
						if(&e==peer->routeslist_head) { //head (i.e. most quick) route is removed
							if(e.next) {
								if(e.next->session->peer_host != e.session->peer_host) { //next item must be current althead, and it becomes head. find new althead
									assert(peer->routeslist_althead==e.next);
									peer->routeslist_althead=find_althead_entry(peer->routeslist_althead); //will find next entry with different next-hop peer than at current althead. althead can become NULL
									alttop_updated=true;
								} //else althead remains the same
							} else { //there is single entry, althead must be empty
								assert(peer->routeslist_althead==NULL);
							}
							top_updated=true;
						} else if(&e==peer->routeslist_althead) { //althead entry is removed
							if(e.next) {
								if(e.next->session->peer_host == peer->routeslist_head->session->peer_host) { //next entry after current althead has same next-hop as head so is not suitable as new althead
									peer->routeslist_althead=find_althead_entry(e.next); //will find next entry with different next-hop peer than at current head. althead can become NULL
								} else { //next entry is suitable to be althead
									peer->routeslist_althead=e.next;
								}
							} else { //all remaining entries must have same next-hop
								peer->routeslist_althead=NULL;
							}
							alttop_updated=true;
						}
						BILINKLIST_REMOVE(&e, next, prev);
						e.hostid=0;
						wasremove=true;
						//hostpeer will be cleared outside rwlock
						if(i==newnumroutes-1) newnumroutes--;
						break;
					case e.MOD_INC:
						//check if entry must be moved down the list
						if(e.next && e.next->realdelay<e.realdelay) { //next item is now quicker, so find new pos
							iot_meshroute_entry* findaltafter=NULL;
							if(&e==peer->routeslist_head) top_updated=true;
							else if(&e==peer->routeslist_althead) {
								if(e.next->session->peer_host == peer->routeslist_head->session->peer_host) { //next entry after current althead has same next-hop as head so is not suitable as new althead but is starting entry for search
									findaltafter=e.next; //mark that new althead must be search after finishing list operation
								} else { //next entry is suitable to be althead
									peer->routeslist_althead=e.next;
								}
								alttop_updated=true;
							}

							iot_meshroute_entry* after=find_quicker_route_entry(e.next, e.realdelay);
							assert(after!=NULL);

							BILINKLIST_REMOVE_NOCL(&e, next, prev);
							BILINKLIST_INSERTAFTER(&e, after, next, prev);

							if(top_updated) {
								if(peer->routeslist_head==peer->routeslist_althead) { //check if althead became new head (if head was moved after it and no entries were between head and althead)
									peer->routeslist_althead=find_althead_entry(peer->routeslist_head); //will find next entry with different next-hop peer than at current head. althead can become NULL
									alttop_updated=true;
								}
							} else if(findaltafter) {
								peer->routeslist_althead=find_althead_entry(findaltafter); //will find next entry with different next-hop. can remain pointing at same entry. should not become NULL
								assert(peer->routeslist_althead!=NULL);
							}
						} else { //no moving. check if head or althead was updated
							if(&e==peer->routeslist_head) top_updated=true;
							else if(&e==peer->routeslist_althead) alttop_updated=true;
						}
						break;
					case e.MOD_DEC:
						if(&e==peer->routeslist_head) top_updated=true;
						else if(e.prev->realdelay>e.realdelay) { //check if entry must be moved up the list
							BILINKLIST_REMOVE_NOCL(&e, next, prev);
							iot_meshroute_entry* after=find_quicker_route_entry(peer->routeslist_head, e.realdelay);
							if(!after) { //current entry must become new head
								if(peer->routeslist_head->session->peer_host!=e.session->peer_host) { //some entry (possible althead) with different next-hop than head becomes new head, so head becomes new althead
									peer->routeslist_althead=peer->routeslist_head;
									alttop_updated=true;
								}
								BILINKLIST_INSERTHEAD(&e, peer->routeslist_head, next, prev);
								top_updated=true;
							} else {
								BILINKLIST_INSERTAFTER(&e, after, next, prev);
								if(&e==peer->routeslist_althead) alttop_updated=true; //moving althead up (and not to head) cannot change it, but delay was updated
								else {
									iot_meshroute_entry* tmp=find_althead_entry(peer->routeslist_head); //do full search of althead because no optimization is seen
									if(tmp!=peer->routeslist_althead) {
										peer->routeslist_althead=tmp;
										alttop_updated=true;
									}
								}
							}
						}
						else if(&e==peer->routeslist_althead) alttop_updated=true;
						break;
					default:
						assert(false); //unknown pendmod
						break;
				}
			}
			e.pendmod=e.MOD_NONE;
			if(top_updated) { //determine if change was too small to update actual routing params and set need_resync
				bool actualize=false;
				auto top_entry=peer->routeslist_head;

				if(!top_entry) actualize=true; //last route removed
				else if(peer->routing_actualpathlen != top_entry->pathlen) actualize=true;//always actualize if pathlen changes
				else { //check that delay was changed considerably
					int32_t diff=peer->routing_actualdelay - top_entry->realdelay;
					if(diff<0) diff=-diff;
					if(diff>ROUTEDELAY_ABSTHRESHOLD && (diff*100)/peer->routing_actualdelay>=ROUTEDELAY_RELTHRESHOLD) actualize=true; //require at least 10us and 10% of difference
				}
				if(actualize) {
					need_resync=true;
					if(!new_routes_version) new_routes_version=gwinst->next_event_numerator();

					if(!top_entry) {
						peer->routing_actualpathlen=0;
						peer->routing_actualdelay=ROUTEENTRY_MAXDELAY;
					} else {
						peer->routing_actualpathlen=top_entry->pathlen;
						peer->routing_actualdelay=top_entry->realdelay;
					}
					peer->routing_actualversionpart=new_routes_version;
				} else { //here top_entry cannot be NULL because actualize would be true in such case
					if(top_entry->session->peer_host!=routing_actualnexthop) { //only nexthop updated. have to resync too, without generating new version
						need_resync=true;
					}
				}
			}
			if(alttop_updated) { //determine if change was too small to update actual routing params and set need_resync
				bool actualize=false;
				auto top_entry=peer->routeslist_althead;

				if(!top_entry) actualize=true; //alt route removed
				else if(peer->routing_altactualpathlen != top_entry->pathlen) actualize=true;//always actualize if pathlen changes
				else { //check that delay was changed considerably
					int32_t diff=peer->routing_altactualdelay - top_entry->realdelay;
					if(diff<0) diff=-diff;
					if(diff>ROUTEDELAY_ABSTHRESHOLD && (diff*100)/peer->routing_altactualdelay>=ROUTEDELAY_RELTHRESHOLD) actualize=true; //require at least 10us and 10% of difference
				}
				if(actualize) {
					need_resync=true;
					if(!new_routes_version) new_routes_version=gwinst->next_event_numerator();

					if(!top_entry) {
						peer->routing_altactualpathlen=0;
						peer->routing_altactualdelay=ROUTEENTRY_MAXDELAY;
					} else {
						peer->routing_altactualpathlen=top_entry->pathlen;
						peer->routing_altactualdelay=top_entry->realdelay;
					}
					peer->routing_altactualversionpart=new_routes_version;
				}
			}
		}

		print_routingtable(NULL, true);
		if(need_resync) {
			uint32_t maxnum_peers=gwinst->peers_registry->get_num_peers();
			if(maxnum_peers>0) {
				size_t sz=maxnum_peers*sizeof(iot_peer*);
				peerbuf=(iot_peer**)alloca(sz);
				num_peers=gwinst->peers_registry->copy_meshpeers(peerbuf, sz, gwinst->peers_registry->MODE_ALL);

				//determine which peers must be notified about new routing table
				for(uint32_t to=0; to<num_peers; to++) { //routing table for this peer is checked 
					iot_peer *to_peer=peerbuf[to];
					if(!to_peer->meshsessions_head) continue; //only peers with active mesh sessions musts be tracked for routing table update

					uint64_t ver=myroutes_lastversion;
					//find greatest version among other peers with mesh sessions
					for(uint32_t dst=0; dst<num_peers; dst++) {
						if(to==dst) continue;
						if(!peerbuf[dst]->routeslist_head || peerbuf[dst]->routeslist_head->session->peer_host!=to_peer) {//use actual routing
							if(ver<peerbuf[dst]->routing_actualversionpart) ver=peerbuf[dst]->routing_actualversionpart;
						}
						else if(ver<peerbuf[dst]->routing_altactualversionpart) ver=peerbuf[dst]->routing_altactualversionpart;
					}
					if(to_peer->current_routingversion<ver) { //routing table for peer changed
						print_routingtable(to_peer, true);

						to_peer->current_routingversion=ver;
						to_peer->notify_routing_update=true;
						assert(to_peer->meshsessions_head!=NULL);
//						TIMER WILL DO THIS to_peer->meshsessions_head->send_signal(); //use first session for syncing routes
					}
				}
			}
		}

		if(locked && !waslocked) uv_rwlock_wrunlock(&routing_lock);
		if(wasremove) { //finish entry remove by nullifying hostpeer
			for(uint32_t i=0; i<ses->numroutes; i++) {
				iot_meshroute_entry &e=ses->routes[i];
				if(e.hostid==0 && e.hostpeer!=NULL) {
					assert(!e.prev && !e.orig_prev);
					e.hostpeer=NULL;
				}
			}
		}
		ses->numroutes=newnumroutes;

	}

void iot_meshnet_controller::try_sync_routing_tables(void) {
	bool have=false;
	for(iot_peer *peer : *gwinst->peers_registry) {
		if(!peer->notify_routing_update) continue;
		have=true;
		break;
	}
	if(!have) return;
outlog_notice("Will sync routing table");
	uv_rwlock_rdlock(&routing_lock);
	for(iot_peer *peer : *gwinst->peers_registry) {
		if(!peer->notify_routing_update) continue;
outlog_notice("to peer %" IOT_PRIhostid " : %d", peer->host_id, int(peer->meshsessions_head!=NULL));

		if(!peer->meshsessions_head)
			peer->notify_routing_update=false;
		else
			peer->meshsessions_head->send_signal(); //use first session for syncing routes
	}
	uv_rwlock_rdunlock(&routing_lock);
}

void iot_meshnet_controller::print_routingtable(iot_peer* forpeer, bool waslocked) { //non-null forpeer means print routing table for specified host
		if(!waslocked) uv_rwlock_rdlock(&routing_lock);

		uint32_t maxnum_peers=gwinst->peers_registry->get_num_peers();
		iot_peer** peerbuf=NULL;
		uint32_t num_peers=0;
		if(maxnum_peers>0) {
			size_t sz=maxnum_peers*sizeof(iot_peer*);
			peerbuf=(iot_peer**)alloca(sz);
			num_peers=gwinst->peers_registry->copy_meshpeers(peerbuf, sz, gwinst->peers_registry->MODE_ROUTABLE); //list of peers with mesh sessions cannot change while routing is locked
		}
		if(!num_peers) {
			outlog_notice("ROUTING TABLE of %" IOT_PRIhostid " IS EMPTY", gwinst->this_hostid);
			goto onexit;
		}

		if(forpeer)
			outlog_notice("ROUTING TABLE of %" IOT_PRIhostid " for %" IOT_PRIhostid ":", gwinst->this_hostid, forpeer->host_id);
		else
			outlog_notice("ROUTING TABLE of %" IOT_PRIhostid ":", gwinst->this_hostid);

		for(uint32_t i=0;i<num_peers;i++) {
			iot_peer *p=peerbuf[i];
			assert(p->routing_actualpathlen>0); //copy_meshpeers must skip peers without routes
			assert(p->routeslist_head!=NULL);

			if(forpeer) { //print table which would be sent to forpeer
				if(p==forpeer) continue;
				if(p->routeslist_head->session->peer_host==forpeer) { //must use altrouting
					if(p->routing_altactualpathlen) {
						assert(p->routeslist_althead!=NULL);
						outlog_notice("dst=%" IOT_PRIhostid ", gw=%" IOT_PRIhostid ", delay=%u, len=%d, ver=%llu", p->host_id, p->routeslist_althead->session->peer_host->host_id, p->routing_altactualdelay, p->routing_altactualpathlen, p->routing_altactualversionpart);
					}
				} else {
					outlog_notice("dst=%" IOT_PRIhostid ", gw=%" IOT_PRIhostid ", delay=%u, len=%d, ver=%llu", p->host_id, p->routeslist_head->session->peer_host->host_id, p->routing_actualdelay, p->routing_actualpathlen, p->routing_actualversionpart);
				}
			} else { //print local routing table
				outlog_notice("dst=%" IOT_PRIhostid ", ACTUAL gw=%" IOT_PRIhostid ", delay=%u, len=%d, ver=%llu", p->host_id, p->routeslist_head->session->peer_host->host_id, p->routing_actualdelay, p->routing_actualpathlen, p->routing_actualversionpart);
				if(p->routing_altactualpathlen) {
					assert(p->routeslist_althead!=NULL);
					outlog_notice("dst=%" IOT_PRIhostid ", ALT ACTUAL gw=%" IOT_PRIhostid ", delay=%u, len=%d, ver=%llu", p->host_id, p->routeslist_althead->session->peer_host->host_id, p->routing_altactualdelay, p->routing_altactualpathlen, p->routing_altactualversionpart);
				}
				for(auto e=p->routeslist_head; e; e=e->next)
					outlog_notice("dst=%" IOT_PRIhostid ", gw=%" IOT_PRIhostid " (%p), delay=%u, len=%d%s", p->host_id, e->session->peer_host->host_id, e->session, e->realdelay, e->pathlen, e==p->routeslist_althead ? " ISALT" : "");
			}
		}
onexit:
		if(!waslocked) uv_rwlock_rdunlock(&routing_lock);
	}


int iot_meshnet_controller::on_new_connection(iot_netcon *conobj) {
	if(!conobj || conobj->get_metaclass()!=&iot_netcontype_metaclass_meshses::object) return IOT_ERROR_INVALID_ARGS;
	const iot_netprototype_metaclass* protometa=conobj->protoconfig->get_metaclass();
	uint16_t protoid=protometa->meshses_protoid;
	assert(protoid>0);
	if(!protoid || protoid>max_protoid) return IOT_ERROR_INVALID_ARGS; //TODO: may be resize protocol state dynamically if new protocol can be loaded on request
	int err=0;

	iot_netcon_meshses* con=static_cast<iot_netcon_meshses*>(conobj);

	decltype(proto_state[protoid-1]->connmap) *connmap;
	decltype(proto_state[protoid-1]->connmap)::treepath path;

	uv_rwlock_wrlock(&protostate_lock);

	if(!proto_state[protoid-1]) {
		proto_state[protoid-1]=new proto_state_t;
		if(!proto_state[protoid-1]) {
			err=IOT_ERROR_NO_MEMORY;
			goto onexit;
		}
		if(!proto_state[protoid-1]->connmap.is_valid()) {
			delete proto_state[protoid-1];
			proto_state[protoid-1]=NULL;
			err=IOT_ERROR_NO_MEMORY;
			goto onexit;
		}
	}
	connmap=&proto_state[protoid-1]->connmap;

	connmapkey_t key;
	key.host=con->remote_host;
	if(con->is_passive) { //either new listening connection or accepted connection
		assert(con->local_port>=0);
		key.localport=uint16_t(con->local_port);
		if(!con->remote_host) { //listening
			key.remoteport=0;
		} else { //accepted
			assert(con->remote_port>=0 && con->remote_host>0);
			key.remoteport=uint16_t(con->remote_port);
		}
	} else {
		assert(con->remote_port>=0 && con->remote_host>0);
		key.remoteport=uint16_t(con->remote_port);
		if(con->local_port>=0) { //bound source port
			key.localport=uint16_t(con->local_port);
		} else { //source port must be auto selected
			key.localport=0;
			err=connmap->find(key,NULL,&path);
			if(err<0 || err>1) {
				assert(false);
				err=IOT_ERROR_CRITICAL_BUG;
				goto onexit;
			}
			if(err==0) { //zero localport is free, so can be used
				con->local_port=0;
			} else { //==1, zero localport is busy. find first hole
				const connmapkey_t *curkey;
				while(key.localport<protometa->meshses_maxport) {
					key.localport++; //this value is least suitable if won't be found
					err=connmap->get_next(&curkey, NULL, path);
					if(err<0 || err>1) {
						assert(false);
						err=IOT_ERROR_CRITICAL_BUG;
						goto onexit;
					}
					if(!err || (err==1 && *curkey!=key)) { //no more keys or next found key is not least suitable (it must be larger)
						con->local_port=key.localport;
						break;
					}
				}
				if(con->local_port<0) {
					err=IOT_ERROR_ADDRESS_INUSE;
					goto onexit;
				}
			}
		}
	}
	//here try to create new connection
	err=connmap->find_add(key, NULL, con);
	if(err==-4) {
		err=IOT_ERROR_NO_MEMORY;
		goto onexit;
	}
	if(err<0 || err>1) {
		assert(false);
		err=IOT_ERROR_CRITICAL_BUG;
		goto onexit;
	}
	if(!err) { //address in use
		err=IOT_ERROR_ADDRESS_INUSE;
		goto onexit;
	}
	//new item was inserted
	con->connection_key=key;

	//duplicate connection in linked list
	BILINKLIST_INSERTHEAD(con, cons_head, registry_next, registry_prev);

onexit:
	uv_rwlock_wrunlock(&protostate_lock);
	return err;
}

void iot_meshnet_controller::on_destroyed_connection(iot_netcon *conobj) {
	if(!conobj || conobj->get_metaclass()!=&iot_netcontype_metaclass_meshses::object) return;
	const iot_netprototype_metaclass* protometa=conobj->protoconfig->get_metaclass();
	uint16_t protoid=protometa->meshses_protoid;
	assert(protoid>0 && protoid<=max_protoid);

	iot_netcon_meshses* con=static_cast<iot_netcon_meshses*>(conobj);

	uv_rwlock_wrlock(&protostate_lock);

	assert(proto_state[protoid-1]!=NULL);
	auto &connmap=proto_state[protoid-1]->connmap;

	//remove from tree but ensure that exactly con is removed
	int err=connmap.remove(con->connection_key, NULL, NULL, [](iot_netcon_meshses** valptr, void* need)->int {
		return static_cast<iot_netcon_meshses*>(need)==*valptr;
	}, con);

	assert(err==1);

	BILINKLIST_REMOVE(con, registry_next, registry_prev);

	uv_rwlock_wrunlock(&protostate_lock);
}

int iot_meshnet_controller::meshses_connect(iot_netcon_meshses* con) { //called from netcon's thread
	assert(con && con->phase==con->PHASE_CONNECTING);
	assert(uv_thread_self()==con->thread->thread);

	size_t sz, bufsize;
	if(!con->connection_state) {
		bufsize=uint32_t(1)<<(IOT_MAXMESHSES_BUFSIZE_POWER2);
		sz=sizeof(meshses_state)+2*bufsize;
		con->connection_state=(meshses_state*)con->allocator->allocate(sz, true);
		if(!con->connection_state) return IOT_ERROR_NO_MEMORY;
		new(con->connection_state) meshses_state;
		con->connection_state->buffer_in.setbuf(IOT_MAXMESHSES_BUFSIZE_POWER2, (char*)&con->connection_state[1]);
		con->connection_state->buffer_out.setbuf(IOT_MAXMESHSES_BUFSIZE_POWER2, (char*)&con->connection_state[1] + bufsize);
	} else {
		assert(con->connection_state->state==con->connection_state->CLOSED);
	}
	con->connection_state->init_client();

	return 0;
}




void iot_netcon_meshses::do_start_uv(void) {
		assert(uv_thread_self()==thread->thread);

		if(remote_host && !remote_peer) {
			remote_peer=static_cast<iot_meshnet_controller*>(registry)->gwinst->peers_registry->find_peer(remote_host);
			if(!remote_peer) {
				do_stop();
				return;
			}
		}

		assert(h_phase_timer_state==HS_UNINIT);
		int err=uv_timer_init(loop, &retry_phase_timer);
		assert(err==0);
		retry_phase_timer.data=this;
		h_phase_timer_state=HS_INIT;

/*		if(is_passive) {
			if(phase==PHASE_CONNECTED) {
//				if(accepted_fd!=INVALID_SOCKET) { //this is duplicated raw socket
//					err=uv_tcp_open(&h_tcp, accepted_fd);
//					assert(err==0);
//					accepted_fd=INVALID_SOCKET;
//					h_tcp_state=HS_ACTIVE;
//				}
				process_common_phase();
			}
			else if(phase==PHASE_LISTENING || phase==PHASE_INITIAL) process_server_phase();
			else {
				assert(false);
				do_stop();
				return;
			}
		} else {*/
			assert(phase==PHASE_INITIAL);
//			if(peer_closed_restart) {
//				connect_errors=1;
//				peer_closed_restart=false;
//			} else {
				connect_errors=0;
//			}
			process_client_phase();
//		}
	}

