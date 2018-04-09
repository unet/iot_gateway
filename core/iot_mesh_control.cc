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

iot_netcontype_metaclass_mesh iot_netcontype_metaclass_mesh::object;



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
						if(j>=curses->maxroutes) continue;
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

bool iot_meshnet_controller::sync_routing_table_topeer(iot_netproto_session_mesh* ses) { //called from session's thread
//returns true is writing was initiated
		assert(uv_thread_self()==ses->thread->thread);
		assert(ses->peer_host!=NULL && ses->state==ses->STATE_WORKING);

		iot_peer* peer=(iot_peer*)ses->peer_host;

		if(!peer->notify_routing_update && peer->current_routingversion<=peer->lastreported_routingversion.load(std::memory_order_relaxed)) return false;

		uv_rwlock_wrlock(&routing_lock);
		if(peer->current_routingversion<=peer->lastreported_routingversion.load(std::memory_order_relaxed)) { //recheck inside WRITE lock to be able to reset notify_routing_update
			peer->notify_routing_update=false;
//			ses->routing_pending=false;
			uv_rwlock_wrunlock(&routing_lock);
			return false;
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

		size_t packetsize=sizeof(iot_netproto_session_mesh::packet_rtable_req)+num_routes*sizeof(iot_netproto_session_mesh::packet_rtable_req::items[0]);
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
		return true;
}

void iot_meshnet_controller::confirm_routing_table_sync_topeer(iot_netproto_session_mesh* ses, uint64_t version, uint8_t waserror) { //called from session's thread
		assert(uv_thread_self()==ses->thread->thread);
		assert(ses->peer_host!=NULL && ses->state==ses->STATE_WORKING);

		ses->peer_host->lastreported_routingversion.store(version, std::memory_order_relaxed);

		uv_rwlock_rdlock(&routing_lock);

		if(ses->peer_host->current_routingversion>ses->peer_host->lastreported_routingversion.load(std::memory_order_relaxed)) { //table still not actual
			ses->peer_host->notify_routing_update=true;
			if(!waserror) ses->send_signal(); //on error new attempt to send routing table will be done during any other output is attempted
		} else {
			ses->peer_host->notify_routing_update=false;
		}

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

//in list of routes of ses finds active ones and puts corresponding peer to provided array
int iot_meshnet_controller::fill_peers_from_active_routes(iot_netproto_session_mesh* ses, iot_objref_ptr<iot_peer> *peers, uint32_t *delays, uint16_t *pathlens, size_t max_peers) { //called from session's thread
	assert(uv_thread_self()==ses->thread->thread);
	if(max_peers==0) {
		assert(false);
		return 0;
	}

	uv_rwlock_rdlock(&routing_lock);

	int num=0;

	if(ses->numroutes>0) {
		ses->lastroute=(ses->lastroute+1) % ses->numroutes; //cycle through peers

		uint32_t idx=ses->lastroute;
		for(uint32_t i=0; i<ses->numroutes ; i++, idx=(idx+1)%ses->numroutes) {
			if(!ses->routes[idx].is_active || !ses->routes[idx].hostid) continue;
			if(delays) delays[num]=ses->routes[idx].realdelay;
			if(pathlens) pathlens[num]=ses->routes[idx].pathlen;
			peers[num]=ses->routes[idx].hostpeer;
			num++;
			if(num>=int(max_peers)) break;
		}
	}

	if(!num) ses->has_activeroutes.store(false, std::memory_order_release);

	uv_rwlock_rdunlock(&routing_lock);
	return num;
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

		bool need_resync=false; //shows if actual routing table (first routing entry in any peer's routeslist_head or althead) has changed
		iot_peer** peerbuf=NULL; //set when need_resync becomes true
		uint32_t num_peers=0; //set when need_resync becomes true

		bool locked=waslocked; //lock will be acquired on first necessity
		bool wasremove=false;
		uint32_t newnumroutes=ses->numroutes;

		for(uint32_t i=ses->numroutes-1; i!=0xffffffffu; i--) { //go backward to be able to remove all items
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
			bool need_reactivate=false; //shows if position of any meshroute_entry of ses has changed (used when meshtun_distribution>1 and top_updated is not enough to determine active state of route)
			bool wasfirst=false; //becomes true if first route to peer was added
			bool waslast=false; //becomes true if last route to peer was removed
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
				wasfirst=peer->routeslist_head==NULL;
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
				need_reactivate=true; //any insert means that active state of routes for current peer must be rechecked
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
						waslast=peer->routeslist_head==NULL; //last route was just removed
						e.hostid=0;
						wasremove=true;
						//hostpeer will be cleared outside rwlock
						if(i==newnumroutes-1) newnumroutes--;
						need_reactivate=true; //any deletion means that active state of routes for current peer must be rechecked
						e.is_active=0;
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

							need_reactivate=true; //any moving means that active state of routes for current peer must be rechecked

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
							need_reactivate=true; //any moving means that active state of routes for current peer must be rechecked
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
			if(need_reactivate && (meshtun_distribution>1 || top_updated)) { //with multiple distibution any move requires reactivation of routes. without distribution only moving of top route entry is meaningful)
				//re assign is_active to meshtun_distribution first items
				auto entry=peer->routeslist_head;
				uint32_t pos=0;
				while(entry && pos<meshtun_distribution) { //loop over entries which must be active
					if(!entry->is_active) { //activation
						entry->is_active=true;
						bool was=false;
						if(ses->has_activeroutes.compare_exchange_strong(was, true, std::memory_order_acq_rel, std::memory_order_relaxed)) ses->send_signal(); //wake up session if we've just added first active route
					}
					entry=entry->next;
					pos++;
				}
				//rest of entries are inactive
				while(entry) {
					entry->is_active=false; //session will disable has_activeroutes by itself when finds no active routes
					entry=entry->next;
				}
			}
			if(wasfirst) { //first route to peer's just appeared
				assert(!waslast);
				peer->noroute_timer_item.unschedule(); //disable noroute timer for this peer
				peer->is_unreachable.store(false, std::memory_order_relaxed);
				peer->on_meshroute_set();
				//TODO loop over all netcon_mesh with remote_host of current peer and send them event signal so they could wake up from NO_ROUTE error. now 
				//peer->on_meshroute_set will only wake up its netcon for iot session
			} else if(waslast) { //last route to peer's just disappeared
				timer.schedule(peer->noroute_timer_item, IOT_MESHNET_NOROUTETIMEOUT*1000); //start noroute timer
				peer->on_meshroute_reset();
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


//notifies mesh sessions from active routes that there are mesh streams ready for output
void iot_meshnet_controller::route_meshtunq(iot_peer* peer) { //called from any thread
		uv_rwlock_rdlock(&routing_lock);
		int left=meshtun_distribution;

		for(auto entry=peer->routeslist_head; entry && left>0; entry=entry->next) { //loop over active entries stopping when meshtun_distribution notified
			if(!entry->is_active) continue;
			entry->session->send_signal(); //wake up session
			left--;
		}

		uv_rwlock_rdunlock(&routing_lock);
}

//fired when some time passes after loosing last route to peer
void iot_meshnet_controller::on_noroute_timer(iot_peer *peer) { //peer can be NULL to set NO_ROUTE error FOR ALL bound meshtuns
	//will be called from main thread (thread which inited controller)
	if(peer) {
		uv_rwlock_wrlock(&routing_lock);
		if(peer->routeslist_head) { //route appeared when timer event was already scheduled to run, so just return
			uv_rwlock_wrunlock(&routing_lock);
			return;
		}
		peer->is_unreachable.store(true, std::memory_order_relaxed); //now treat this peer as unreachable
		uv_rwlock_wrunlock(&routing_lock);
	}

	//start by aborting all meshtuns which are currently in peer's queue, requesting output
//todo optimization	peer->abort_meshtuns();
	
	int64_t totaln=0;
	iot_meshconnmapkey_t keyfrom(peer ? peer->host_id : 0, 0, 0);
	iot_meshconnmapkey_t keyto(peer ? peer->host_id : iot_hostid_t_MAX, 65535, 65535);
	iot_meshtun_state** buf=NULL;

	uv_rwlock_rdlock(&protostate_lock);

	//count necessary space for values
	for(uint16_t protoid=1;protoid<=max_protoid;protoid++) {
		if(!proto_state[protoid-1]) continue;
		int64_t rval=proto_state[protoid-1]->connmap.find_mult_values(keyfrom, keyto, NULL, 0, true, true);
		assert(rval>=0);
		totaln+=rval;
	}

	if(totaln>0) {
		buf=(iot_meshtun_state**)alloca(sizeof(iot_meshtun_state*)*totaln);
		int64_t totaln2=0;

		//fetch values
		for(uint16_t protoid=1;protoid<=max_protoid;protoid++) {
			if(!proto_state[protoid-1]) continue;
			int64_t rval=proto_state[protoid-1]->connmap.find_mult_values(keyfrom, keyto, buf, totaln-totaln2, true, false);
			assert(rval>=0);
			totaln2+=rval;
		}
		assert(totaln2==totaln);

		//increment refcount for all obtained states while being inside protostate_lock (outside the lock iot_meshtun_state can be immediately released)
		for(int i=0;i<totaln;i++) {
			buf[i]->ref();
		}
	}

	uv_rwlock_rdunlock(&protostate_lock);

	//outside protostate_lock individual states can be locked. (otherwise deadlock is possible with state bind/unbind, where state lock is obtained first and protostate_lock second)

	for(int i=0;i<totaln;i++) {
		buf[i]->lock();
		if(buf[i]->is_bound && !buf[i]->get_error()) buf[i]->set_error(IOT_ERROR_NO_ROUTE);
		buf[i]->unlock();
		buf[i]->unref();
	}
}

bool iot_meshnet_controller::meshtun_find_bound(uint16_t protoid, uint16_t local_port, iot_hostid_t remote_host, uint16_t remote_port, iot_objref_ptr<iot_meshtun_state> &result) {
	//returns true if found and assigns result
	if(!protoid || !remote_host || protoid>max_protoid) return false;

	//first try to find connected meshtun
	iot_meshconnmapkey_t key(remote_host, remote_port, local_port);
	iot_meshtun_state **pstate=NULL;
	bool rval=true;
	int err;

	decltype(proto_state[0]->connmap) *connmap;

	uv_rwlock_rdlock(&protostate_lock);

	if(!proto_state[protoid-1]) {
		rval=false;
		goto onexit;
	}

	connmap=&proto_state[protoid-1]->connmap;

	err=connmap->find(key,&pstate);
	if(err<0 || err>1) {
		assert(false);
		rval=false;
		goto onexit;
	}
	if(err==1) { //found connected meshtun
		assert(pstate!=NULL);
		result=*pstate;
		goto onexit;
	}

	//try to find listening meshtun
	key.host=0;
	key.remoteport=0;

	err=connmap->find(key,&pstate);
	if(err<0 || err>1) {
		assert(false);
		rval=false;
		goto onexit;
	}
	if(err==1) { //found listening meshtun
		assert(pstate!=NULL);
		result=*pstate;
	} else { //not found
		rval=false;
	}

onexit:
	uv_rwlock_rdunlock(&protostate_lock);
	return rval;

}

int iot_meshnet_controller::meshtun_bind(iot_meshtun_state *tunstate, const iot_netprototype_metaclass* protometa, iot_hostid_t remote_host, uint16_t remote_port, bool auto_local_port, uint16_t local_port) {
	//when auto_local_port is true, local_port is ignored
	uint16_t protoid=protometa->type_id;
	assert(protometa->can_meshtun && protoid>0);
	if(!protometa->can_meshtun || !protoid || protoid>max_protoid) return IOT_ERROR_INVALID_ARGS; //TODO: may be resize protocol state dynamically if new protocol can be loaded on request
	int err=0;

	decltype(proto_state[0]->connmap) *connmap;
	decltype(proto_state[0]->connmap)::treepath path;

	uv_rwlock_wrlock(&protostate_lock);

	auto &pstate=proto_state[protoid-1];

	if(!pstate) { //protocol state was not yet allocated
		pstate=new proto_state_t;
		if(!pstate) {
			err=IOT_ERROR_NO_MEMORY;
			goto onexit;
		}
		if(!pstate->connmap.is_valid()) { //tree was unable to initialize
			delete pstate;
			pstate=NULL;
			err=IOT_ERROR_NO_MEMORY;
			goto onexit;
		}
	}
	connmap=&pstate->connmap;

	iot_meshconnmapkey_t key;
	key.host=remote_host;
	key.remoteport=remote_port;
	if(!auto_local_port) {
		key.localport=local_port;
	} else { //local port must be auto selected
		key.localport=0;
		err=connmap->find(key,NULL,&path);
		if(err<0 || err>1) {
			assert(false);
			err=IOT_ERROR_CRITICAL_BUG;
			goto onexit;
		}
		if(err==1) { //zero localport is busy. find first hole
			const iot_meshconnmapkey_t *curkey;
			while(key.localport<protometa->meshtun_maxport) {
				key.localport++; //this value is least suitable if won't be found
				err=connmap->get_next(&curkey, NULL, path);
				if(err<0 || err>1) {
					assert(false);
					err=IOT_ERROR_CRITICAL_BUG;
					goto onexit;
				}
				if(!err || (err==1 && *curkey!=key)) { //no more keys or next found key is not least suitable (it must be larger)
					break; //success
				}
			}
			if(key.localport>=protometa->meshtun_maxport) { //no free hole found
				err=IOT_ERROR_ADDRESS_INUSE;
				goto onexit;
			}
		} //else zero localport is free, so can be used
	}
	//insert tunstate into connmap
	err=connmap->find_add(key, NULL, tunstate);
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
		assert(!auto_local_port);
		err=IOT_ERROR_ADDRESS_INUSE;
		goto onexit;
	}
	//new item was inserted
	uv_rwlock_wrunlock(&protostate_lock);

	tunstate->bind_base(protometa, key);

	return 0;

onexit:
	uv_rwlock_wrunlock(&protostate_lock);
	return err;

}

void iot_meshnet_controller::meshtun_unbind(iot_meshtun_state *tunstate) {
	uint16_t protoid=tunstate->protometa->type_id;
	if(!protoid || protoid>max_protoid) {
		assert(false);
		return;
	}

	uv_rwlock_wrlock(&protostate_lock);

	assert(proto_state[protoid-1]!=NULL);
	auto &connmap=proto_state[protoid-1]->connmap;

	//remove from tree but ensure that exactly tunstate is removed
	int err=connmap.remove(tunstate->connection_key, NULL, NULL, [](iot_meshtun_state** valptr, void* need)->int {
		return static_cast<iot_meshtun_state*>(need)==*valptr;
	}, tunstate);

	assert(err==1);

	uv_rwlock_wrunlock(&protostate_lock);
}



int iot_meshnet_controller::on_new_connection(iot_netcon *conobj) {
	if(!conobj || conobj->get_metaclass()!=&iot_netcontype_metaclass_mesh::object) return IOT_ERROR_INVALID_ARGS;
	int err=0;

	iot_netcon_mesh* con=static_cast<iot_netcon_mesh*>(conobj);

	uv_rwlock_wrlock(&protostate_lock);

	if(is_shutdown) {
		err=IOT_ERROR_ACTION_CANCELLED;
		goto onexit;
	}

	//duplicate connection in linked list
	BILINKLIST_INSERTHEAD(con, cons_head, registry_next, registry_prev);

onexit:
	uv_rwlock_wrunlock(&protostate_lock);
	return err;
}

void iot_meshnet_controller::on_destroyed_connection(iot_netcon *conobj) {
	if(!conobj || conobj->get_metaclass()!=&iot_netcontype_metaclass_mesh::object) return;

	iot_netcon_mesh* con=static_cast<iot_netcon_mesh*>(conobj);

	uv_rwlock_wrlock(&protostate_lock);

	BILINKLIST_REMOVE(con, registry_next, registry_prev);
	bool waslast=!cons_head;

	uv_rwlock_wrunlock(&protostate_lock);

	if(waslast && is_shutdown) 
		timer.schedule(shutdown_timer_item, 0); //continue shutdown step 2
}


void iot_meshnet_controller::graceful_shutdown(void) {
	is_shutdown=true;
	uv_rwlock_rdlock(&protostate_lock);

	if(!cons_head) {
		uv_rwlock_rdunlock(&protostate_lock);
		graceful_shutdown_step2();
		return;
	}
	for(iot_netcon *cur, *next=cons_head; (cur=next); ) {
		next=next->registry_next;
		cur->destroy(true);
	}

	uv_rwlock_rdunlock(&protostate_lock);

	timer.schedule(shutdown_timer_item, 2000); //give max 2 sec to all meshtuns
}



//create appropriate meshtun_state object for netcon_mesh
//possible errors:
//IOT_ERROR_NO_MEMORY
int iot_meshnet_controller::meshtun_create(iot_netcon_mesh* con, iot_memallocator* allocator, iot_meshtun_type_t type, bool is_passive) { //called from netcon's thread
	assert(!con->thread || uv_thread_self()==con->thread->thread); //allow non-started netcons to create meshtun (necessary for accept)
	if(con->connection_state) return IOT_ERROR_NO_ACTION;

	size_t sz, bufsize;
	switch(type) {
		case TUN_STREAM: {
			iot_meshtun_stream_state* st;
			bufsize=uint32_t(1)<<(IOT_MESHTUN_BUFSIZE_POWER2);
			sz=sizeof(iot_meshtun_stream_state)+2*bufsize;
			st=(iot_meshtun_stream_state*)allocator->allocate(sz, true);
			if(!st) return IOT_ERROR_NO_MEMORY;
			new(st) iot_meshtun_stream_state(con, this, is_passive, IOT_MESHTUN_BUFSIZE_POWER2, (char*)(st+1), IOT_MESHTUN_BUFSIZE_POWER2, ((char*)(st+1)) + bufsize);
			con->connection_state=iot_objref_ptr<iot_meshtun_state>(true, st);
			break;
		}
		case TUN_STREAM_LISTEN: {
			iot_meshtun_stream_listen_state* st;
			bufsize=uint32_t(1)<<16;
			sz=sizeof(iot_meshtun_stream_listen_state)+bufsize;
			st=(iot_meshtun_stream_listen_state*)allocator->allocate(sz, true);
			if(!st) return IOT_ERROR_NO_MEMORY;
			new(st) iot_meshtun_stream_listen_state(con, this, 16, (char*)(st+1));
			con->connection_state=iot_objref_ptr<iot_meshtun_state>(true, st);
			break;
		}
		default:
			assert(false);
	}
	return 0;
}

int iot_meshtun_forwarding::set_state(state_t newstate) {
		assert(uv_thread_self()==lockowner); //must be called INSIDE LOCK

		switch(newstate) {
			case ST_CLOSED:
				if(request_body) iot_release_memblock(request_body);
				request_body=NULL;

				//remove from peer's queue
				if(in_queue) {
					if(state==ST_FORWARDING) {
						if(dst_peer) dst_peer->remove_meshtun(this);
						else assert(false);
					}
					else {
						assert(state==ST_RESET);
						if(src_peer) src_peer->remove_meshtun(this);
						else assert(false);
					}
					assert(!in_queue);
				}
				state=newstate;
				break;
			case ST_FORWARDING:
				assert(state==ST_CLOSED);
				state=newstate;
				dst_peer=controller->gwinst->peers_registry->find_peer(dst_host);
				if(!dst_peer) {
					set_error(IOT_ERROR_NO_ROUTE);
					break;
				}
				dst_peer->push_meshtun(this, true);
				break;
			case ST_RESET:
				assert(state==ST_FORWARDING);
				if(request_body) iot_release_memblock(request_body);
				request_body=NULL;

				state=newstate;

				if(get_error()!=IOT_ERROR_NO_ROUTE) { //only no route is reported to source peer. other errors silently drop the packet
					set_closed();
					break;
				}

				src_peer=controller->gwinst->peers_registry->find_peer(src_host);
				if(!src_peer) {
					set_closed();
					break;
				}
				
				set_error(0);
				src_peer->push_meshtun(this, true);
				break;
			default:
				assert(false);
				set_closed();
				break;
		}
		return 0;
	}
void iot_meshtun_forwarding::on_state_update(update_t upd) { //must be called INSIDE LOCK. reacts to setting error, detaching, changing substate flags
		assert(uv_thread_self()==lockowner); //must be called INSIDE LOCK
		switch(state) {
			case ST_CLOSED:
				break;
			case ST_FORWARDING:
				if(get_error()) {
					set_state(ST_RESET);
					break;
				}
				assert(false); //only error can be reason for call?
				break;
			case ST_RESET:
				if(get_error()) { //error during sending error to source host, just drop
					set_closed();
					break;
				}
				assert(false); //only error can be reason for call?
				break;
			default:
				assert(false);
				set_closed();
				break;
		}
	}




void iot_meshtun_state::set_outmeta(void* meta_data_, uint16_t meta_size_) { //used to set new or reset meta data. meta_data_ must be memblock (if not NULL)! its refcount will be increased
		//SETTING MUST BE INITIATED by netcon thread
		lock();
		if(outmeta_data) { //have prev value, release it
			if(outmeta_data==meta_data_ && outmeta_size==meta_size_) goto onexit; //no change
			iot_release_memblock(outmeta_data);
			outmeta_data=NULL;
		}
		if(meta_data_) {
			assert(con && uv_thread_self()==con->thread->thread);

			if(!meta_size_ || meta_size_>255*8) {
				assert(false);
				goto onexit;
			}
			if(!iot_incref_memblock(meta_data_)) { //too many refs?
				assert(false);
				goto onexit;
			}
			outmeta_data=meta_data_;
			outmeta_size=meta_size_;
		}
onexit:
		unlock();
	}


int iot_meshtun_stream_state::connect(const iot_netprototype_metaclass* protometa, iot_hostid_t remote_host, uint16_t remote_port, bool auto_local_port, uint16_t local_port) {
		if(!remote_host) return IOT_ERROR_INVALID_ARGS;
		lock();
		int err;

		if(state!=ST_CLOSED || is_passive_stream) {
			err=IOT_ERROR_INVALID_STATE;
			goto onexit;
		}
		if(!con) {
			err=IOT_ERROR_NOT_INITED;
			goto onexit;
		}

		set_error(0);
		remote_peer=controller->gwinst->peers_registry->find_peer(remote_host);
		if(!remote_peer) {
			err=IOT_ERROR_NO_ROUTE;
			goto onexit;
		}

//		output_closed=0;

		err=controller->meshtun_bind(this, protometa, remote_host, remote_port, auto_local_port, local_port);
		if(err) goto onexit;

		buffer_out.clear();
		buffer_in.clear();
		total_output=total_input=0;
		initial_sequence_out=1000; //can be random
		initial_sequence_in=0;
		ack_sequenced_out=sequenced_out=0;
		ack_sequenced_in=sequenced_in=maxseen_sequenced_in=0;
//		used_buffer_in=0;
		peer_rwnd=0;
		local_rwnd=buffer_in.getsize();
//		acception_time=0;
		num_in_dis=0;
		num_output_inprog=0;
		iot_gen_random((char*)random, IOT_MESHPROTO_TUNSTREAM_IDLEN);
		creation_time=((iot_get_systime()+500000000)/1000000000)-1000000000; //round to integer seconds and offset by 1e9 seconds

		err=set_state(ST_SENDING_SYN);
onexit:
		unlock();
		return err;
	}


int iot_meshtun_stream_state::accept_connection(iot_meshtun_stream_listen_state::listenq_item* item, const iot_netprototype_metaclass* protometa_, uint16_t local_port, void* inmeta, uint16_t inmeta_size_) {
		if(!item || !item->remotehost) {
			if(inmeta) iot_release_memblock(inmeta);
			return IOT_ERROR_INVALID_ARGS;
		}
		lock();
		int err;

		if(state!=ST_CLOSED || !is_passive_stream) {
			assert(false);
			err=IOT_ERROR_INVALID_STATE;
			goto onexit;
		}
		if(!con) {
			assert(false);
			err=IOT_ERROR_NOT_INITED;
			goto onexit;
		}

		set_error(0);
		remote_peer=controller->gwinst->peers_registry->find_peer(item->remotehost);
		if(!remote_peer) {
			err=IOT_ERROR_NO_ROUTE;
			goto onexit;
		}

		input_closed=0;
//		output_closed=0;

		err=controller->meshtun_bind(this, protometa_, item->remotehost, item->remoteport, false, local_port);
		if(err) goto onexit;

		buffer_out.clear();
		buffer_in.clear();
		total_output=total_input=0;
		initial_sequence_out=100; //can be random
		initial_sequence_in=item->initial_sequence;
		ack_sequenced_out=sequenced_out=0;

		ack_sequenced_in=0;
		sequenced_in=maxseen_sequenced_in=1; //account for received SYN
//		used_buffer_in=0;
		peer_rwnd=item->peer_rwnd;
		local_rwnd=buffer_in.getsize();
//		acception_time=0;
		num_in_dis=0;
		num_output_inprog=0;
		memcpy(random, item->random, IOT_MESHPROTO_TUNSTREAM_IDLEN);
		creation_time=((iot_get_systime()+500000000)/1000000000)-1000000000; //round to integer seconds and offset by 1e9 seconds

		if(inmeta) {
			assert(!inmeta_data && inmeta_size_>0);
			inmeta_data=inmeta;
			inmeta_size=inmeta_size_;
		}

		set_state(ST_SENDING_SYN);
		set_state(ST_ESTABLISHED);

		err=0;
onexit:
		unlock();
		if(err && inmeta) iot_release_memblock(inmeta);
		return err;
	}


void iot_meshtun_stream_state::on_state_update(update_t upd) { //reacts to setting error, detaching, changing substate flags, shutting down
		assert(uv_thread_self()==lockowner);
outlog_notice("!!!! UPDATING STATE %u, event %u", unsigned(state), unsigned(upd));

		if(upd==UPD_ERROR_SET && con && get_error()) {
			con->on_meshtun_event(con->EVENT_ERROR); //must sent async signal to netcon
		}

		switch(state) {
			case ST_CLOSED: //attach/detach could occur, no actions
				assert(output_closed && input_closed && !output_pending && !output_ack_pending && !input_pending && !input_ack_pending && !in_queue);
				break;
			case ST_SENDING_SYN:
//				assert(!output_ack_pending && input_closed && !input_pending && !input_ack_pending);

				//UPD_SHUTDOWN does nothing for this state (only output_closed is set to true in base class)
				if(upd==UPD_WRITEREADY) { //SYN must be sent or repeated
					if(output_closed) goto try_close;
					check_output_inprogress();
					break;
				}
				if(upd==UPD_WRITTEN) { //SYN or SYN-ACK was sent
					assert(num_output_inprog==1);
//					if(sequenced_out>ack_sequenced_out) output_ack_pending=1; //we're waiting ACK to our SYN from peer
					input_ack_pending=0; //any output send current pending input ACK, so always reset here
					break;
				}
				if(upd==UPD_ERROR_SET && get_error()) { //error was just set. notification to attached netcon already signaled
					output_closed=1;
					output_pending=0;
					output_ack_pending=0;

					input_closed=1;
					input_ack_pending=0;
					//input_pending can remain true if con is not detached
					goto try_close;
				}
				if(upd==UPD_SHUTDOWN) { //output was just closed, netcon not detached
					if(output_ack_pending) { //SYN can be already sent by client
						if(is_passive_stream) {
							//do graceful stop
							sequenced_out++; //for FIN bit
							output_ack_pending=1; //for STREAM_FIN
							check_output_inprogress();
						} else {
							set_state(ST_RESET);
						}
						return;
					}
					output_pending=0;
					//output_ack_pending is checked to be false
					goto try_close;
				}
				if(upd==UPD_DETACH) { //netcon detached
					if(output_ack_pending) { //SYN can be already sent
						set_state(ST_RESET);
						return;
					}
					output_closed=1;
					output_pending=0;
					//output_ack_pending is checked to be false

					input_closed=1;
					buffer_in.clear();
					input_pending=0;
					input_ack_pending=0;

					goto try_close;
				}
				break;
			case ST_ESTABLISHED:
				switch(upd) {
					case UPD_WRITESPACEFREED: //got free space in buffer_out and there was failed write
						if(con && !output_closed && output_wanted) con->on_meshtun_event(con->EVENT_WRITABLE);
						break;
					case UPD_READREADY: //input is available for reading
						if(con && input_wanted) con->on_meshtun_event(con->EVENT_READABLE);
						break;
					case UPD_WRITEREADY: //output must be sent or repeated or output closed and ACKed
						if(output_closed) goto try_close;
						check_output_inprogress();
						break;
					case UPD_WRITTEN: //some output is to be written to socket
						input_ack_pending=0; //any output always sendd current pending input ACK, so always reset here
						break;
					case UPD_ERROR_SET: //error was just set. notification to attached netcon already signaled
						if(!get_error()) break;
						output_closed=1;
						output_pending=0;
						output_ack_pending=0;

						input_closed=1;
						input_ack_pending=0;
						//input_pending can remain true if con is not detached
						goto try_close;
					case UPD_DETACH: //netcon detached
						if(!input_closed) { //no error occured and peer did not close connection
							input_closed=1;
							if(buffer_in.pending_read()>0) { //there is unread data, need to reset even if graceful shutdown is in progress (it will be interrupted)
								set_state(ST_RESET);
								return;
							}
						}
						buffer_in.clear();
						input_pending=0;
						if(output_closed) goto try_close;
						output_closed=1;
						//pass through and do graceful stop
					case UPD_SHUTDOWN: //output was just closed, netcon not detached
						//do graceful stop
						sequenced_out++; //for FIN bit
						output_ack_pending=1; //for STREAM_FIN
						check_output_inprogress();
						break;
					default: break;
				}
				break;
			case ST_RESET:
				assert(output_closed && input_closed && !output_pending && !input_pending && !input_ack_pending);
				if(get_error() || upd==UPD_WRITTEN) { //RESET sent successfully or error is set
					output_ack_pending=0;
				}
				if(!output_ack_pending) goto try_close;
				if(upd==UPD_WRITEREADY) { //output must be sent or repeated
					check_output_inprogress();
				}
				//UPD_WRITTEN is sent when RESET was written successfuly
				break;
			default:
				assert(false);
		}
		return;
try_close:
		//remove from peer's queue if no output pending
		if(in_queue && !output_ack_pending && !input_ack_pending) {
			remote_peer->remove_meshtun(this);
			assert(!in_queue);
		}
		if(!unbind_base() || in_queue) return; //try to unbind
		//here is unbound
		if(output_closed && input_closed && !output_pending && !output_ack_pending && !input_pending && !input_ack_pending) {
			if(!con || (con && get_refcount()==1)) { //netcon is not detached and refcount==1, so netcon has the only reference to meshtun. it can be closed immediately
				set_closed();
				return;
			}
		}
}

iot_hostid_t iot_meshtun_stream_state::get_peer_id(void) const {
	return remote_peer ? remote_peer->host_id : 0;
}


int iot_meshtun_stream_state::set_state(state_t newstate) {
		assert(uv_thread_self()==lockowner);
outlog_notice("!!!! SETTING STATE %u", unsigned(newstate));

		switch(newstate) {
			case ST_CLOSED:
				assert(state!=ST_CLOSED);
				assert(output_closed && input_closed && !output_pending && !output_ack_pending && !input_pending && !input_ack_pending && !in_queue);
				unbind_base();
				buffer_out.clear();
				buffer_in.clear();
				cancel_timer();
				remote_peer=NULL;
				break;
			case ST_SENDING_SYN: //SYN or SYN-ACK is to be queued. output_closed must be 0, input_closed will be zero if this is SYN-ACK (is_passive_stream will be true)
				if(state!=ST_CLOSED) {
					assert(false);
					return IOT_ERROR_INVALID_STATE;
				}
//				assert(!output_ack_pending && input_closed && !input_pending && !input_ack_pending);

				assert(is_bound);
				if(is_passive_stream) {
					assert(!input_closed);
					input_ack_pending=1; //mark that ACK must be added
				} else {
					output_wanted=1; //connection status will be signaled to netcon
				}
				assert(ack_sequenced_out==0 && sequenced_out==0);
				output_closed=0;
				sequenced_out=1; //this thiggers SYN sending
				output_ack_pending=1; //pending STREAM_SYN request   (in established state this flag is set when buffer_out can be read)

				state=newstate;
				on_state_update(UPD_WRITEREADY);
				//push_meshtun in on_state_update can raise NO_ROUTE error and reset state to closed, and signal will be sent to netcon, so return 0
				return 0; //confirm state change without error
			case ST_ESTABLISHED:
				assert(state==ST_SENDING_SYN);
				assert(is_bound);
				assert(!output_closed);
				assert(con);
				assert(!get_error());

				state=newstate;
				if(output_wanted) con->on_meshtun_event(con->EVENT_WRITABLE); //must sent async signal to netcon about successful connection
				return 0; //confirm state change without error
			case ST_RESET: //RESET request must be sent to peer
				assert(state==ST_SENDING_SYN || state==ST_ESTABLISHED);
				//invalidate input
				input_closed=1;
				buffer_in.clear();
				input_pending=0;
				input_ack_pending=0;
				//invalidate output
				output_closed=1;
				output_pending=0;
				output_ack_pending=1; //for RESET request

				cancel_timer();
				for(uint64_t i=0; i<num_output_inprog; i++) {
					if(output_inprogress[i].meshses) output_inprogress[i].meshses->current_outmeshtun_segment=-1;
				}
				num_output_inprog=1;
				output_inprogress[0]={
					.meshses=NULL,
					.seq_pos=sequenced_out,
					.retry_after=UINT64_MAX,
					.seq_len=0,
					.has_syn=0,
					.has_fin=0,
					.is_ack=0,
					.is_pending=1,
					.retries=0
				};
				state=ST_RESET;
				on_state_update(UPD_WRITEREADY);
				return 0;
			default:
				assert(false);
		}
		state=newstate;
		return 0;
}


//creates new output_inprogress (after sequenced_out increase), checks timer expiration for retries, schedules output of pending block
//must be called after:
//- outputting (locking by mesh session) of some block
void iot_meshtun_stream_state::check_output_inprogress(void) {
		//MUST BE CALLED under lock()
		assert(uv_thread_self()==lockowner);
		if(get_error()) return;
		if(state==ST_RESET) {
			assert(num_output_inprog==1);
			if(in_queue || !output_inprogress[0].is_pending || output_inprogress[0].meshses) return;
			remote_peer->push_meshtun(this, true);
			return;
		}

		assert(state==ST_ESTABLISHED || state==ST_SENDING_SYN);

		if(output_pending) { //netcon has written something to buffer_out
			if(!output_closed) {
				assert(sequenced_out>0); //when output_closed becomes 0, sequenced_out becomes 1 (for SYN bit)
				assert(sequenced_out-ack_sequenced_out<UINT32_MAX);
				uint32_t pendout=buffer_out.pending_read(); //readpos of buffer corresponds to ack_sequenced_out when it is >=1
				uint32_t unackout;
				if(!ack_sequenced_out) {
					unackout=sequenced_out-1;
				} else {
					unackout=sequenced_out-ack_sequenced_out;
				}
				assert(pendout>=unackout);
				if(pendout>unackout) {
					sequenced_out+=pendout-unackout;
					output_ack_pending=1;
				}
			}
			output_pending=0;
		}

		uint64_t len;
		if(!num_output_inprog) len=sequenced_out-ack_sequenced_out;
		else {
			assert(num_output_inprog<=IOT_MESHTUNSTREAM_MAXOUTPUTSPLIT);

			auto &last_inprog=output_inprogress[num_output_inprog-1];

			len=sequenced_out-(last_inprog.seq_pos+last_inprog.seq_len);
			if(int64_t(len)<0) { //must not happen
				assert(false);
				sequenced_out=last_inprog.seq_pos+last_inprog.seq_len;
				len=0;
			} else if(len>0 && last_inprog.is_pending && last_inprog.retries==0) { //new output can be appended to last inprog block if it is pending, untried and MTU allows
				assert(!last_inprog.is_ack); //ACKed blocks must not be pending
				int32_t deltalen=uint32_t(IOT_MESHTUNSTREAM_MTU)-uint32_t(last_inprog.seq_len-last_inprog.has_syn-last_inprog.has_fin); //how many sequence can be appended
				assert(deltalen>=0);
				if(deltalen>0) {
					if(uint32_t(deltalen)>len) deltalen=len;
					last_inprog.seq_len+=deltalen; //enlarge last inprog block
					len-=deltalen;
				}
			}
		}
		//add new blocks if possible and necessary (i.e. if there is space and new output is available)
		while(len>0 && num_output_inprog<IOT_MESHTUNSTREAM_MAXOUTPUTSPLIT) { //new block can be added
			uint64_t start=sequenced_out-len;
			uint8_t hassyn = start==0 ? 1 : 0;
			uint8_t hasfin = sequenced_out>1 && output_closed ? 1 : 0;
			output_inprogress[num_output_inprog]={
				.meshses=NULL,
				.seq_pos=start,
				.retry_after=UINT64_MAX,
				.seq_len=uint32_t(len-hassyn-hasfin >= IOT_MESHTUNSTREAM_MTU ? IOT_MESHTUNSTREAM_MTU+hassyn+hasfin : len), //datalen is limited by IOT_MESHTUNSTREAM_MTU
				.has_syn=hassyn,
				.has_fin=hasfin,
				.is_ack=0,
				.is_pending=1,
				.retries=0
			};
			len-=output_inprogress[num_output_inprog].seq_len;
			num_output_inprog++;
		}

		if(!in_queue) { //find pending output
			int num_busy_ses=0; //how many mesh sessions are currently busy doing output
			bool have_pending=input_ack_pending;
			for(uint16_t i=0; i<num_output_inprog; i++) {
				if(!output_inprogress[i].is_pending) continue;
				assert(!output_inprogress[i].is_ack);
				if(output_inprogress[i].meshses) { //already locked by meshses
					num_busy_ses++;
					continue;
				}
				have_pending=true;
			}
			if(num_busy_ses<controller->get_meshtun_distribution() && have_pending) { //there is block ready for output and distribution allows to use one more mesh session
				remote_peer->push_meshtun(this, true);
			}
		}
	}



void iot_meshtun_stream_listen_state::on_state_update(update_t upd) { //reacts to setting error, detaching, changing substate flags, shutting down
		assert(uv_thread_self()==lockowner);
outlog_notice("!!!! UPDATING STATE OF LISTEN %u, event %u", unsigned(state), unsigned(upd));

		if(upd==UPD_ERROR_SET && con && get_error()) {
			con->on_meshtun_event(con->EVENT_ERROR); //must sent async signal to netcon
		}

		switch(state) {
			case ST_CLOSED: //attach/detach could occur, no actions
				assert(output_closed && input_closed && !output_pending && !output_ack_pending && !input_pending && !input_ack_pending && !in_queue);
				break;
			case ST_LISTENING: //detach could occur, network error, shutdown
				if(upd==UPD_ERROR_SET && get_error()) { //error was just set. notification to attached netcon already signaled
					listenq.clear();
					input_closed=1;
					input_pending=0;
					goto try_close;
				}
				if(!con) { //detach (shutdown not possible as output is always closed)
					listenq.clear();
					input_closed=1;
					input_pending=0;
					goto try_close;
				}
				if(upd==UPD_READREADY && input_wanted) {
					assert(input_pending!=0);
					con->on_meshtun_event(con->EVENT_READABLE); //must sent async signal to netcon
				}
				break;
			default: //other states impossible
				assert(false);
		}
		return;
try_close:
		assert(!in_queue); //listen meshtuns cannot be queued
		if(!unbind_base()) return; //try to unbind
		//here is unbound
		if(output_closed && input_closed && !output_pending && !output_ack_pending && !input_pending && !input_ack_pending) {
			if(!con || (con && get_refcount()==1)) { //netcon is not detached and refcount==1, so netcon has the only reference to meshtun. it can be closed immediately
				set_closed();
				return;
			}
		}
}


int iot_meshtun_stream_listen_state::set_state(state_t newstate) {
		assert(uv_thread_self()==lockowner);
outlog_notice("!!!! SETTING STATE OF LISTEN %u", unsigned(newstate));

		switch(newstate) {
			case ST_CLOSED:
				assert(state!=ST_CLOSED);
				assert(output_closed && input_closed && !output_pending && !output_ack_pending && !input_pending && !input_ack_pending && !in_queue);
				unbind_base();
				listenq.clear();
				break;
			case ST_LISTENING: //connection request or confirmation is to be queued
				assert(state==ST_CLOSED);

				assert(is_bound);
				assert(!input_closed);

				input_wanted=1; //connection requests will be signaled to netcon
				break;
			default: //other states impossible
				assert(false);
				return IOT_ERROR_INVALID_ARGS;
		}
		state=newstate;
		return 0;
}

int iot_meshtun_stream_listen_state::accept(iot_meshtun_stream_state* rep) { //tries to accept new connection on current listening stream meshtun into provided stream meshtun rep.
		//rep must be closed, unbound, attached to netcon. it must not be engaged in another thread until this function returns
		if(!rep || !rep->is_passive_stream || !rep->is_closed() || rep->is_detached()) {
			assert(false);
			return IOT_ERROR_INVALID_ARGS;
		}
		lock();
		int err=IOT_ERROR_TRY_AGAIN;
		listenq_item it;

		if(state!=ST_LISTENING || !con || !input_pending || !is_bound) goto onexit;
		assert(uv_thread_self()==con->thread->thread);
		uint32_t now;
		now=((iot_get_systime()+500000000)/1000000000)-1000000000;

		uint32_t pending;
		pending=listenq.pending_read();

		while(pending>0) {
			if(pending<sizeof(it)) { //must not happen as whole listenq_item struct must always be present in non-empty queue
				assert(false);
				err=IOT_ERROR_CRITICAL_ERROR;
				break;
			}

			uint32_t len=listenq.peek(&it, sizeof(it)); //do peek so we could get metadata size
			if(len!=sizeof(it)) { //must not happen as we checked pending_read() before on locked state
				assert(false);
				err=IOT_ERROR_CRITICAL_ERROR;
				break;
			}
			pending-=sizeof(it);
			if(now>it.request_time && now-it.request_time>=accept_timeout) { //request timed out, just remove it and continue
				if(it.metasize>0 && pending<it.metasize) { //must not happen because means error when adding connection to queue
					assert(false);
					err=IOT_ERROR_CRITICAL_ERROR;
					break;
				}
				pending-=it.metasize;
				len=listenq.read(NULL, sizeof(it)+it.metasize); //remove data from queue
				if(len!=sizeof(it)+it.metasize) {
					assert(false);
					err=IOT_ERROR_CRITICAL_ERROR;
					break;
				}
				continue;
			}
			//here request is not timed out
			void *metadata;
			if(it.metasize>0) {
				if(pending<it.metasize) { //must not happen because means error when adding connection to queue
					assert(false);
					err=IOT_ERROR_CRITICAL_ERROR;
					break;
				}
				metadata=con->allocator->allocate(it.metasize, true);
				if(!metadata) {
					err=IOT_ERROR_NO_MEMORY;
					break;
				}
				len=listenq.read(metadata, it.metasize, sizeof(it));
				if(len!=it.metasize) {
					assert(false);
					iot_release_memblock(metadata);
					err=IOT_ERROR_CRITICAL_ERROR;
					break;
				}
			} else {
				len=listenq.read(NULL, sizeof(it)); //remove data from queue
				if(len!=sizeof(it)) {
					assert(false);
					err=IOT_ERROR_CRITICAL_ERROR;
					break;
				}
				metadata=NULL;
			}

			err=rep->accept_connection(&it, protometa, connection_key.localport, metadata, it.metasize);
			goto onexit;
		}
		input_pending=0;
onexit:
		unlock();
		return err;
	}


