#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include "iot_netproto_mesh.h"
#include "iot_mesh_control.h"
#include "iot_peerconnection.h"
#include "iot_threadregistry.h"
#include "iot_configregistry.h"


iot_netprototype_metaclass_mesh iot_netprototype_metaclass_mesh::object;

//static const char* reqtype_descr[uint8_t(iot_gwproto_reqtype_t::MAX)+1]={"reply", "request", "notification"};

int iot_netproto_config_mesh::instantiate(iot_netconiface* coniface) {
		assert(uv_thread_self()==coniface->thread->thread);
		auto ses=new iot_netproto_session_mesh(coniface, this, object_destroysub_delete);
		if(!ses) return IOT_ERROR_NO_MEMORY;
		return 0;
	}


//const uint8_t iot_netproto_session_mesh::packet_signature[4]={'M', 'e', 's', 'h'};

iot_netproto_session_mesh::iot_netproto_session_mesh(iot_netconiface* coniface, iot_netproto_config_mesh* config, object_destroysub_t destroysub) :
			iot_netproto_session(&iot_netprototype_metaclass_mesh::object, coniface, destroysub), peer_host(config->peer), config(config)
	{
		assert(config!=NULL);
	}

iot_netproto_session_mesh::~iot_netproto_session_mesh() {
	if(peer_host && peer_prev) { //unregister from mesh constroller
		assert(state==STATE_WORKING);
assert(false); //temp
		config->gwinst->meshcontroller->unregister_session(this);
	}

	assert(numroutes==0);

	if(routes) iot_release_memblock(routes);
	routes=NULL;

	outlog_notice("MESH SESSION DESTROYED");
/*		//todo do something with requests in queue
		if(current_outpacket) {
			current_outpacket->on_session_close();
			current_outpacket=NULL;
		}
		for(iot_gwprotoreq* nextreq, *curreq=outpackets_head; curreq; curreq=nextreq) {
			nextreq=curreq->next;
			BILINKLISTWT_REMOVE(curreq, next, prev);
			curreq->on_session_close();
		}
		for(iot_gwprotoreq* nextreq, *curreq=waitingpackets_head; curreq; curreq=nextreq) {
			nextreq=curreq->next;
			BILINKLISTWT_REMOVE(curreq, next, prev);
			curreq->on_session_close();
		}
*/	}

iot_hostid_t iot_netproto_session_mesh::get_hostid(void) const {
		if(peer_host) return peer_host->host_id;
		return 0;
	}

//resizes routes entries list to have space for at least n items
//returns 0 on success, error code on error:
//IOT_ERROR_NO_MEMORY
//IOT_ERROR_INVALID_ARGS
int iot_netproto_session_mesh::resize_routes_buffer(uint32_t n, iot_memallocator* allocator) { //must be called under routing_lock if mesh session is already registered
	if(maxroutes==n) return 0;
	if(!n || numroutes>n) return IOT_ERROR_INVALID_ARGS; //not enough space for existing entries

	//allocate new memory block
	size_t sz=sizeof(iot_meshroute_entry)*n;
	iot_meshroute_entry* newroutes=(iot_meshroute_entry*)allocator->allocate(sz, true);
	if(!newroutes) return IOT_ERROR_NO_MEMORY;
	if(numroutes<n) memset(newroutes+numroutes, 0, (n-numroutes)*sizeof(iot_meshroute_entry)); //zerofill empty entries after numroutes entries

	//copy existing entries and relink them 
	if(numroutes>0) {
		memmove(newroutes, routes, sizeof(iot_meshroute_entry)*(n-numroutes));
		for(uint32_t i=0; i<numroutes; i++) { //relink bilist items to point to new allocated entries
			BILINKLIST_REPLACE(&routes[i], &newroutes[i], next, prev);
			BILINKLIST_REPLACE(&routes[i], &newroutes[i], orig_next, orig_prev);
		}
	}
	if(routes) iot_release_memblock(routes);
	routes=newroutes;
	maxroutes=n;
	return 0;
}

int iot_netproto_session_mesh::start(void) {
		assert(uv_thread_self()==thread->thread);
		assert(coniface!=NULL);

		//allocate space for routes
		int err=resize_routes_buffer(config->gwinst->config_registry->get_num_hosts()+5, coniface->allocator);
		if(err) return err;

		err=uv_timer_init(coniface->loop, &phase_timer);
		assert(err==0);
		phase_timer.data=this;
		phase_timer_state=HS_INIT;

		state_change(STATE_BEFORE_AUTH);

		//enable reading
		err=coniface->read_data(readbuf, sizeof(readbuf));
		assert(err==0);

		return 0;
	}

void iot_netproto_session_mesh::on_stop(bool graceful) {
		assert(uv_thread_self()==thread->thread);
		if(in_processing) { //stop() must be repeated after clearing in_processing
			assert(closed<3);
			if(!pending_close || (!graceful && pending_close==1)) pending_close=graceful ? 1 : 2;
			return;
		}
		uint8_t pclose=pending_close;
		pending_close=0;

		if(closed==3) return; //stop already finished

		if(closed==1) { //graceful stop in progress. allow upgrade to hard stop
			if(graceful && pclose<2) {
				//repeated graceful request or changing of some state involved by graceful stop. return if need to wait more or upgrade to hard stop
				
				//for now there are no graceful procedure and we should not come here
				assert(false);
			} else { //upgrade to hard stop
				closed=2;
				//initiate hard stop after promoting graceful to hard
			}
		} else if(!closed) {
			if(!graceful || pclose>=2) {
				closed=2;
				//initiate hard stop
				//for now just continue
			}
			else {
				closed=1;
				//initiate graceful stop
				//for now always use hard stop
				closed++;
				//for now just continue
			}
		}
		//initiate/continue hard stop
		if(peer_host && peer_prev) { //unregister from mesh constroller
			config->gwinst->meshcontroller->unregister_session(this);
		}

		int8_t num_closing=0;

		if(phase_timer_state>=HS_INIT) {
			phase_timer_state=HS_CLOSING;
			num_closing++;
			uv_close((uv_handle_t*)&phase_timer, [](uv_handle_t* handle) -> void {
				iot_netproto_session_mesh* obj=(iot_netproto_session_mesh*)(handle->data);
				assert(obj->phase_timer_state==HS_CLOSING);
				obj->phase_timer_state=HS_UNINIT;
				obj->on_stop(true);
			});
		} else if(phase_timer_state==HS_CLOSING) num_closing++;

		if(comq_watcher_state>=HS_INIT) {
			comq_watcher_state=HS_CLOSING;
			num_closing++;
			uv_close((uv_handle_t*)&comq_watcher, [](uv_handle_t* handle) -> void {
				iot_netproto_session_mesh* obj=(iot_netproto_session_mesh*)(handle->data);
				assert(obj->comq_watcher_state==HS_CLOSING);
				obj->comq_watcher_state=HS_UNINIT;
				obj->on_stop(true);
			});
		} else if(comq_watcher_state==HS_CLOSING) num_closing++;

		if(num_closing>0) return;

		//can be destroyed from this point
		closed=3;
		self_closed();
	}


void iot_netproto_session_mesh::state_handler_working(hevent_t ev) {
		int err;
		if(ev==HEVENT_CAN_WRITE) return;
		switch(phase) {
			case PHASE_INITIAL:
				uv_async_init(coniface->loop, &comq_watcher, [](uv_async_t* handle) -> void {
					iot_netproto_session_mesh* obj=(iot_netproto_session_mesh*)(handle->data);
					obj->on_commandq();
				});
				comq_watcher.data=this;
				comq_watcher_state=HS_INIT;

				err=config->gwinst->meshcontroller->register_session(this);
				if(err) goto stopses;
				phase=PHASE_NORMAL;
				return;
			case PHASE_NORMAL:
				if(ev==HEVENT_NEW_PACKET_HEADER) {
					assert(current_inpacket_hdr!=NULL);
					return;
				}
				if(ev==HEVENT_NEW_PACKET_READY) {
					assert(current_inpacket_hdr!=NULL);

					switch(current_inpacket_hdr->cmd) {
						case CMD_RTABLE_REQ: {//routing table update
							packet_rtable_req* req=(packet_rtable_req*)(current_inpacket_hdr+1);
							if(current_inpacket_size!=sizeof(*req)+sizeof(packet_rtable_req::items[0])*repack_uint16(req->num_items)) { //invalid size
								outlog_error("MESH session got CMD_RTABLE_REQ packed with invalid size %u, must be %u", unsigned(current_inpacket_size), unsigned(sizeof(*req)+sizeof(packet_rtable_req::items[0])*repack_uint16(req->num_items)));
								break;
							}
							outlog_notice("MESH session got CMD_RTABLE_REQ from %" IOT_PRIhostid, peer_host->host_id);
							config->gwinst->meshcontroller->sync_routing_table_frompeer(this, req);
							break;
						}
						default:
							outlog_notice("MESH session got unknown command %d", int(current_inpacket_hdr->cmd));
							break;
					}

//					packet_auth_req *req=(packet_auth_req *)(current_inpacket_hdr+1);
					return;
				}
				if(ev==HEVENT_WRITE_READY) {
					switch(current_outpacket_cmd) {
						case CMD_RTABLE_REQ:
							routing_pending=false;
							config->gwinst->meshcontroller->confirm_routing_table_sync_topeer(this);//temporary until SRVCODE_RTABLE_UPDATED is implemented
							break;
						default:
							break;
					}
				}

			default:
				outlog_notice("MESH session in WORKING state got to phase %d, event %d", int(phase), int(ev));
		}
		return;
stopses:
		stop(false);
}


void iot_netproto_session_mesh::state_handler_before_auth_srv(hevent_t ev) {
		int err;
		if(ev==HEVENT_CAN_WRITE) return;
//restart:
		switch(phase) {
			case PHASE_INITIAL:
				phase=PHASE_WAITING_PEER;
				run_phase_timer(5000);
				return;
			case PHASE_WAITING_PEER:
				if(ev==HEVENT_PHASE_TIMER) {
					outlog_notice("MESH session got timeout in state=%d, phase=%d waiting for client AUTH_REQ", int(state), int(phase));
					goto stopses;
				}
				if(ev==HEVENT_NEW_PACKET_HEADER) {
					assert(current_inpacket_hdr!=NULL);
					if(current_inpacket_hdr->cmd!=CMD_AUTH_REQ) {
						outlog_notice("server MESH session got illegal cmd %d waiting for client AUTH_REQ", int(current_inpacket_hdr->cmd));
						goto stopses;
					}
					//TODO check packet for validity
					if(current_inpacket_size<sizeof(packet_auth_req) || current_inpacket_size>sizeof(packet_auth_req)+max_authdata_size) {
						outlog_notice("server MESH session got AUTH_REQ with illegal packet size %u", unsigned(current_inpacket_size));
						goto stopses;
					}
					//just wait for full packet, phase timer remains
					return;
				}
				if(ev==HEVENT_NEW_PACKET_READY) {
					assert(current_inpacket_hdr!=NULL);
					assert(current_inpacket_hdr->cmd==CMD_AUTH_REQ);

					outlog_notice("server MESH session got client AUTH_REQ");

					stop_phase_timer();
					packet_auth_req *req=(packet_auth_req *)(current_inpacket_hdr+1);

					//TODO validity checks
					if(repack_hostid(req->dsthost)!=config->gwinst->this_hostid) {
						outlog_notice("server MESH session got AUTH_REQ with invalid dsthost");
						goto stopses;
					}
					if(repack_uint32(req->guid)!=config->gwinst->guid) {
						outlog_notice("server MESH session got AUTH_REQ with invalid guid");
						goto stopses;
					}
					peer_host=config->gwinst->peers_registry->find_peer(repack_hostid(req->srchost));
					if(!peer_host) {
						outlog_notice("server MESH session got AUTH_REQ with unknown srchost %" IOT_PRIhostid, repack_hostid(req->srchost));
						goto stopses;
					}

					//preserve request for ability to make common signature
					statedata.before_auth.auth_cache.request_hdr = *current_inpacket_hdr;
					statedata.before_auth.auth_cache.request = *req;

					authtype_t atype=authtype_t(req->authtype);
					uint32_t adatasz=current_inpacket_size-sizeof(packet_auth_req);

					//check signature
					if(!check_authdata((iot_peer*)peer_host, atype, (char*)(req+1), adatasz, (char*)(&statedata.before_auth.auth_cache), sizeof(statedata.before_auth.auth_cache.request_hdr)+offsetof(struct packet_auth_req, reltime_ns))) return;

					atype=AUTHTYPE_UNSET;
					if(!choose_authtype(atype, adatasz)) {
						outlog_notice("server MESH session creation failed to select auth type");
						goto stopses;
					}

					packet_hdr* hdr=prepare_packet_header(CMD_AUTH_REPLY, sizeof(packet_auth_reply)+adatasz);
					packet_auth_reply *reply=(packet_auth_reply*)(hdr+1);
					memset(reply, 0, sizeof(*reply));

					reply->flags=repack_uint16(0);
					reply->authtype=atype;
					reply->time_synced=repack_uint32(last_clock_sync);
					reply->time_accuracy_ms=repack_uint16(iot_get_systime_error());
					reply->timestamp_ns=repack_uint64(iot_get_systime());
					iot_gen_random((char*)reply->random, sizeof(reply->random));
					reply->routing_version=repack_uint64(peer_host->get_origroutes_version());

					statedata.before_auth.auth_cache.reply_hdr = *hdr;
					statedata.before_auth.auth_cache.reply = *reply;
					if(!fill_authdata(atype, (char*)(reply+1), adatasz, (char*)(&statedata.before_auth.auth_cache), sizeof(statedata.before_auth.auth_cache.request_hdr)+sizeof(statedata.before_auth.auth_cache.request)+sizeof(statedata.before_auth.auth_cache.reply_hdr)+offsetof(struct packet_auth_reply, reltime_ns))) {
						outlog_notice("server MESH session failed to fill authdata");
						goto stopses;
					}
					
					reply->reltime_ns=repack_uint64(iot_get_reltime());
					statedata.before_auth.auth_cache.reply.reltime_ns=reply->reltime_ns; //do separately to minimize operation time (whole reply structure copying will be longer)

					phase=PHASE_REPLY_BEING_SENT;
					err=coniface->write_data(hdr, sizeof(*hdr)+sizeof(packet_auth_reply)+adatasz);
					assert(err==0);
//					if(err==1) goto restart;
					return;
				}
				break;
			case PHASE_REPLY_BEING_SENT:
				if(ev!=HEVENT_WRITE_READY) {
					outlog_notice("server MESH session got invalid event %d for REPLY_BEING_SENT phase before auth", int(ev));
					goto stopses;
				}
				phase=PHASE_WAITING_PEER_2;
				run_phase_timer(50000);
				return;
			case PHASE_WAITING_PEER_2:
				if(ev==HEVENT_PHASE_TIMER) {
					outlog_notice("server MESH session got timeout in state=%d, phase=%d waiting for client AUTH_FINISH", int(state), int(phase));
					goto stopses;
				}
				if(ev==HEVENT_NEW_PACKET_HEADER) {
					assert(current_inpacket_hdr!=NULL);
					statedata.before_auth.finish_reltime_ns=iot_get_reltime();
					if(current_inpacket_hdr->cmd!=CMD_AUTH_FINISH) {
						outlog_notice("server MESH session got illegal cmd %d waiting for client AUTH_FINISH", int(current_inpacket_hdr->cmd));
						goto stopses;
					}
					//TODO check packet for validity
					if(current_inpacket_size<sizeof(packet_auth_finish) || current_inpacket_size>sizeof(packet_auth_finish)+max_authdata_size) {
						outlog_notice("server MESH session got AUTH_FINISH with illegal packet size %u", unsigned(current_inpacket_size));
						goto stopses;
					}
					//just wait for full packet, phase timer remains
					return;
				}
				if(ev==HEVENT_NEW_PACKET_READY) {
					assert(current_inpacket_hdr!=NULL);
					assert(current_inpacket_hdr->cmd==CMD_AUTH_FINISH);

					outlog_notice("server MESH session got client AUTH_FINISH");

					stop_phase_timer();
					packet_auth_finish *fin=(packet_auth_finish *)(current_inpacket_hdr+1);

					//TODO validity checks

					authtype_t atype=authtype_t(fin->authtype);
					if(atype!=statedata.before_auth.auth_cache.request.authtype) {
						outlog_notice("server MESH session creation failed: AUTH_FINISH uses different authtype than AUTH_REQ");
						goto stopses;
					}

					statedata.before_auth.auth_cache.fin_hdr = *current_inpacket_hdr;
					statedata.before_auth.auth_cache.fin = *fin;

					uint32_t adatasz=current_inpacket_size-sizeof(packet_auth_finish);

					//check signature
					if(!check_authdata((iot_peer*)peer_host, atype, (char*)(fin+1), adatasz, (char*)(&statedata.before_auth.auth_cache),
						sizeof(statedata.before_auth.auth_cache.request_hdr)+sizeof(statedata.before_auth.auth_cache.request)+
						sizeof(statedata.before_auth.auth_cache.reply_hdr)+sizeof(statedata.before_auth.auth_cache.reply)+
						sizeof(statedata.before_auth.auth_cache.fin_hdr)+sizeof(statedata.before_auth.auth_cache.fin)
						)) return;

					outlog_notice("server MESH session from host %" IOT_PRIhostid " to host %" IOT_PRIhostid " established", peer_host->host_id, config->gwinst->this_hostid);

					int64_t delay_ns=statedata.before_auth.finish_reltime_ns-repack_uint64(statedata.before_auth.auth_cache.reply.reltime_ns);
					if(delay_ns<=0) {
						assert(false);
						delay_ns=1;
					} else {
						cur_delay_us=uint32_t((delay_ns+500)/1000);
					}

					peer_host->set_lastreported_routingversion(repack_uint64(fin->routing_version));

					state_change(STATE_WORKING);
					return;
				}
				outlog_notice("server MESH session got event %d waiting for AUTH_FINISH packet", int(ev));
				break;
			default:
				outlog_notice("server MESH session got to phase %d, event %d", int(phase), int(ev));
		}
		return;
stopses:
		stop(false);
	}

void iot_netproto_session_mesh::state_handler_before_auth_cl(hevent_t ev) {
		int err;
		if(ev==HEVENT_CAN_WRITE) return;
//restart:
		switch(phase) {
			case PHASE_INITIAL: {//prepare AUTH_REQ
				authtype_t atype;
				uint32_t adatasz;
				atype=AUTHTYPE_UNSET;
				if(!choose_authtype(atype, adatasz)) {
					outlog_notice("MESH session creation failed to select auth type");
					goto stopses;
				}
				packet_hdr* hdr=prepare_packet_header(CMD_AUTH_REQ, sizeof(packet_auth_req)+adatasz);
				packet_auth_req *req=(packet_auth_req*)(hdr+1);
				memset(req, 0, sizeof(*req));
				req->guid=repack_uint32(peer_host->gwinst->guid);
				req->flags=repack_uint16(0);
				req->authtype=atype;
				req->srchost=repack_hostid(peer_host->gwinst->this_hostid);
				req->dsthost=repack_hostid(peer_host->host_id);
				req->timestamp_ns=repack_uint64(iot_get_systime());
				iot_gen_random((char*)req->random, sizeof(req->random));
				req->time_synced=repack_uint32(last_clock_sync);
				req->time_accuracy_ms=repack_uint16(iot_get_systime_error());

				statedata.before_auth.auth_cache.request_hdr = *hdr;
				statedata.before_auth.auth_cache.request = *req;
				if(!fill_authdata(atype, (char*)(req+1), adatasz, (char*)(&statedata.before_auth.auth_cache),
						sizeof(statedata.before_auth.auth_cache.request_hdr)+offsetof(struct packet_auth_req, reltime_ns))) {
					outlog_notice("client MESH session failed to fill authdata");
					goto stopses;
				}

				req->reltime_ns=repack_uint64(iot_get_reltime());
				statedata.before_auth.auth_cache.request.reltime_ns=req->reltime_ns; //do separately to minimize operation time (whole req structure copying will be longer)

				phase=PHASE_REQ_BEING_SENT;
				err=coniface->write_data(hdr, sizeof(*hdr)+sizeof(packet_auth_req)+adatasz);
				assert(err==0);
//				if(err==1) goto restart;
				return;
			}
			case PHASE_REQ_BEING_SENT:
				if(ev!=HEVENT_WRITE_READY) {
					outlog_notice("client MESH session got invalid event %d for REQ_BEING_SENT phase before auth", int(ev));
					goto stopses;
				}
				phase=PHASE_WAITING_PEER;
				run_phase_timer(50000);
				return;
			case PHASE_WAITING_PEER:
				if(ev==HEVENT_PHASE_TIMER) {
					outlog_notice("client MESH session got timeout in state=%d, phase=%d waiting for client AUTH_REPLY", int(state), int(phase));
					goto stopses;
				}
				if(ev==HEVENT_NEW_PACKET_HEADER) {
					assert(current_inpacket_hdr!=NULL);
					statedata.before_auth.finish_reltime_ns=iot_get_reltime();
					if(current_inpacket_hdr->cmd!=CMD_AUTH_REPLY) {
						outlog_notice("client MESH session got illegal cmd %d waiting for server AUTH_REPLY", int(current_inpacket_hdr->cmd));
						goto stopses;
					}
					//TODO check packet for validity
					if(current_inpacket_size<sizeof(packet_auth_reply) || current_inpacket_size>sizeof(packet_auth_reply)+max_authdata_size) {
						outlog_notice("client MESH session got AUTH_REPLY with illegal packet size %u", unsigned(current_inpacket_size));
						goto stopses;
					}
					//just wait for full packet, phase timer remains
					return;
				}
				if(ev==HEVENT_NEW_PACKET_READY) {
					assert(current_inpacket_hdr!=NULL);
					assert(current_inpacket_hdr->cmd==CMD_AUTH_REPLY);

					outlog_notice("client MESH session got client AUTH_REPLY");
					stop_phase_timer();
					packet_auth_reply *reply=(packet_auth_reply *)(current_inpacket_hdr+1);

					//TODO validity checks

					//move reply data to make common signature
					statedata.before_auth.auth_cache.reply_hdr = *current_inpacket_hdr;
					statedata.before_auth.auth_cache.reply = *reply;

					authtype_t atype=authtype_t(reply->authtype);
					uint32_t adatasz=current_inpacket_size-sizeof(packet_auth_reply);

					//check signature
					if(!check_authdata((iot_peer*)peer_host, atype, (char*)(reply+1), adatasz, (char*)(&statedata.before_auth.auth_cache),
						sizeof(statedata.before_auth.auth_cache.request_hdr)+sizeof(statedata.before_auth.auth_cache.request)+
						sizeof(statedata.before_auth.auth_cache.reply_hdr)+offsetof(struct packet_auth_reply, reltime_ns))) return;

					atype=authtype_t(statedata.before_auth.auth_cache.request.authtype); //req and finish must have same authtype
					if(!choose_authtype(atype, adatasz)) {
						outlog_notice("server MESH session creation failed to select auth type");
						goto stopses;
					}

					packet_hdr* hdr=prepare_packet_header(CMD_AUTH_FINISH, sizeof(packet_auth_finish)+adatasz);
					packet_auth_finish *fin=(packet_auth_finish*)(hdr+1);
					memset(fin, 0, sizeof(*fin));

					fin->flags=repack_uint16(0);
					fin->authtype=atype;
					fin->routing_version=repack_uint64(peer_host->get_origroutes_version());

					statedata.before_auth.auth_cache.fin_hdr = *hdr;
					statedata.before_auth.auth_cache.fin = *fin;

					if(!fill_authdata(atype, (char*)(fin+1), adatasz, (char*)(&statedata.before_auth.auth_cache),
							sizeof(statedata.before_auth.auth_cache.request_hdr)+sizeof(statedata.before_auth.auth_cache.request)+
							sizeof(statedata.before_auth.auth_cache.reply_hdr)+sizeof(statedata.before_auth.auth_cache.reply)+
							sizeof(statedata.before_auth.auth_cache.fin_hdr)+sizeof(statedata.before_auth.auth_cache.fin)
						)) {
						outlog_notice("client MESH session failed to fill authdata for finish");
						goto stopses;
					}
					
					phase=PHASE_REPLY_BEING_SENT;
					err=coniface->write_data(hdr, sizeof(*hdr)+sizeof(packet_auth_finish)+adatasz);
					assert(err==0);
//					if(err==1) goto restart;
					return;
				}
				assert(false);
				break;
			case PHASE_REPLY_BEING_SENT: {
				if(ev!=HEVENT_WRITE_READY) {
					outlog_notice("client MESH session got invalid event %d for REPLY_BEING_SENT phase before auth", int(ev));
					goto stopses;
				}
				outlog_notice("client MESH session from host %" IOT_PRIhostid " to host %" IOT_PRIhostid " established", config->gwinst->this_hostid, peer_host->host_id);

				int64_t delay_ns=statedata.before_auth.finish_reltime_ns-repack_uint64(statedata.before_auth.auth_cache.request.reltime_ns);
				if(delay_ns<=0) {
					assert(false);
					delay_ns=1;
				} else {
					cur_delay_us=uint32_t((delay_ns+500)/1000);
				}

				peer_host->set_lastreported_routingversion(repack_uint64(statedata.before_auth.auth_cache.reply.routing_version));

				state_change(STATE_WORKING);
				return;
			}
			default:
				outlog_notice("client MESH session got to phase %d", int(phase));
		}
		return;
stopses:
		stop(false);
	}

bool iot_netproto_session_mesh::check_authdata(iot_peer* peer, authtype_t atype, const char* authdata, uint32_t authdatasize, const char* data, uint32_t datasize) {
		switch(atype) {
			case AUTHTYPE_PSK_SHA256:
				if(authdatasize!=sizeof(hash_sha256)) goto closeconn;
				break;
			case AUTHTYPE_RSA_SHA256:
				if(authdatasize!=sizeof(authtype_rsa_sign)+sizeof(hash_sha256)) goto closeconn;
				break;
			default:
				goto closeconn;
		}
		return true;
closeconn:
		outlog_notice("got incorrect authdata or its size");
		stop(false);
		return false;
}

bool iot_netproto_session_mesh::fill_authdata(authtype_t atype, char* buf, uint32_t bufsize, const char* data, uint32_t datasize) {
		//TODO
		switch(atype) {
			case AUTHTYPE_PSK_SHA256:
				if(bufsize!=sizeof(hash_sha256)) return false;
				break;
			case AUTHTYPE_RSA_SHA256:
				if(bufsize!=sizeof(authtype_rsa_sign)+sizeof(hash_sha256)) return false;
				break;
			default:
				return false;
		}
		memset(buf, 1, bufsize);
		return true;
	}

void iot_netproto_session_mesh::on_commandq(void) {
	assert(uv_thread_self()==thread->thread);
	assert(state==STATE_WORKING);

	if(peer_host->is_notify_routing_update() && !routing_pending) {
		routing_pending=true;
		if(!current_outpacket_cmd) config->gwinst->meshcontroller->sync_routing_table_topeer(this); //begin syncing immediately if nothing being sent right now
	}

//	iot_meshses_command* com, *nextcom=comq.pop_all();
//	while(nextcom) {
//		com=nextcom;
//		nextcom=(nextcom->next).load(std::memory_order_relaxed);
//	}
}


