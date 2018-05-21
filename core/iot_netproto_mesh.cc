#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include "iot_netproto_mesh.h"
#include "iot_mesh_control.h"
#include "iot_peerconnection.h"
#include "iot_threadregistry.h"
#include "iot_configregistry.h"


void iot_meshroute_entry::init(iot_netproto_session_mesh* ses, const iot_objref_ptr<iot_peer> &peer) {
		session=ses;
		hostpeer=peer;
		hostid=hostpeer->host_id;
		next=prev=NULL;
		orig_next=orig_prev=NULL;
		fulldelay=realdelay=0;
		pendmod=MOD_NONE;
		is_active=0;
	}


iot_netprototype_metaclass_mesh iot_netprototype_metaclass_mesh::object;

//static const char* reqtype_descr[uint8_t(iot_gwproto_reqtype_t::MAX)+1]={"reply", "request", "notification"};

int iot_netproto_config_mesh::instantiate(iot_netconiface* coniface, iot_hostid_t meshtun_peer) {
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
		config->gwinst->meshcontroller->unregister_session(this);
	}

	assert(numroutes==0);

	if(routes) iot_release_memblock(routes);
	routes=NULL;

	outlog_debug_mesh("MESH SESSION %p to host %" IOT_PRIhostid " DESTROYED", this, peer_host ? peer_host->host_id : 0);
	}

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
		memmove(newroutes, routes, sizeof(iot_meshroute_entry)*numroutes);
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

outlog_debug_mesh("MESH SESSION %p to host %" IOT_PRIhostid " STARTED", this, peer_host ? peer_host->host_id : 0);

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
		assert(err>=0);

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

		if(!closed) {
			//common steps to initiate both types of stop

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
		} else if(closed==1) { //graceful stop in progress. allow upgrade to hard stop
			if(graceful && pclose<2) {
				//repeated graceful request or changing of some state involved by graceful stop. return if need to wait more or upgrade to hard stop
				
				//for now there are no graceful procedure and we should not come here
				assert(false);
			} else { //upgrade to hard stop
				closed=2;
				//initiate hard stop after promoting graceful to hard
			}
		}
		//initiate/continue hard stop
		if(peer_host && peer_prev) { //unregister from mesh constroller
			config->gwinst->meshcontroller->unregister_session(this);
		}
		if(current_outmeshtun) { //outputting of meshtun is in progress
			assert(current_outpacket_cmd==CMD_MESHTUN);
			if(current_outmeshtun->type==TUN_STREAM) {
				process_meshtun_outputaborted(static_cast<iot_meshtun_stream_state*>((iot_meshtun_packet*)current_outmeshtun));
			} else if(current_outmeshtun->type==TUN_FORWARDING) {
				//retry using another session
				auto st=static_cast<iot_meshtun_forwarding*>((iot_meshtun_packet*)current_outmeshtun);
				assert(st->meshses==this);
				st->meshses=NULL;
				st->on_state_update(st->UPD_WRITEREADY);
			} else if(current_outmeshtun->type==TUN_DATAGRAM) {
				assert(false); //TODO
			} else {
				assert(false); //TUN_STREAM_LISTEN cannot be here
			}
			current_outmeshtun=NULL;
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

void iot_netproto_session_mesh::process_service_packet(packet_service* req) {
	outlog_debug_mesh("MESH session got service command %d", int(req->code));
	switch(req->code) {
		case SRVCODE_RTABLE_UPDATED:
			routing_syncing=false;
			config->gwinst->meshcontroller->confirm_routing_table_sync_topeer(this, req->qword_param, req->byte_param);
			break;
		default:
			outlog_notice("MESH session got unknown service command %d", int(req->code));
			assert(false);
			break;
	}
}



void iot_netproto_session_mesh::state_handler_before_auth_srv(hevent_t ev) {
		int err;
		if(ev==HEVENT_CAN_WRITE) return;
		if(ev==HEVENT_WRITE_READY) current_outpacket_cmd=CMD_ILLEGAL;
//restart:
		switch(phase) {
			case PHASE_INITIAL:
				phase=PHASE_WAITING_PEER;
				run_phase_timer(5000);
				break;
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
					break;
				}
				if(ev==HEVENT_NEW_PACKET_READY) {
					assert(current_inpacket_hdr!=NULL);
					assert(current_inpacket_hdr->cmd==CMD_AUTH_REQ);

					outlog_debug_mesh("server MESH session got client AUTH_REQ");

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
					if(!check_authdata((iot_peer*)peer_host, atype, (char*)(req+1), adatasz, (char*)(&statedata.before_auth.auth_cache), sizeof(statedata.before_auth.auth_cache.request_hdr)+offsetof(struct packet_auth_req, reltime_ns))) break;

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
					break;
				}
				break;
			case PHASE_REPLY_BEING_SENT:
				if(ev!=HEVENT_WRITE_READY) {
					outlog_notice("server MESH session got invalid event %d for REPLY_BEING_SENT phase before auth", int(ev));
					goto stopses;
				}
				phase=PHASE_WAITING_PEER_2;
				run_phase_timer(50000);
				break;
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
					break;
				}
				if(ev==HEVENT_NEW_PACKET_READY) {
					assert(current_inpacket_hdr!=NULL);
					assert(current_inpacket_hdr->cmd==CMD_AUTH_FINISH);

					outlog_debug_mesh("server MESH session got client AUTH_FINISH");

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
						)) break;

					outlog_debug_mesh("server MESH session %p from host %" IOT_PRIhostid " to host %" IOT_PRIhostid " established", this, peer_host->host_id, config->gwinst->this_hostid);

					int64_t delay_ns=statedata.before_auth.finish_reltime_ns-repack_uint64(statedata.before_auth.auth_cache.reply.reltime_ns);
					if(delay_ns<=0) {
						assert(false);
						delay_ns=1;
					} else {
						cur_delay_us=uint32_t((delay_ns+500)/1000);
					}

					uint64_t routing_version=repack_uint64(fin->routing_version);
					state_change(STATE_WORKING);
					config->gwinst->meshcontroller->confirm_routing_table_sync_topeer(this, routing_version);

					break;
				}
				outlog_debug_mesh("server MESH session got event %d waiting for AUTH_FINISH packet", int(ev));
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
		if(ev==HEVENT_WRITE_READY) current_outpacket_cmd=CMD_ILLEGAL;
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
				break;
			}
			case PHASE_REQ_BEING_SENT:
				if(ev!=HEVENT_WRITE_READY) {
					outlog_notice("client MESH session got invalid event %d for REQ_BEING_SENT phase before auth", int(ev));
					goto stopses;
				}
				phase=PHASE_WAITING_PEER;
				run_phase_timer(50000);
				break;
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
					break;
				}
				if(ev==HEVENT_NEW_PACKET_READY) {
					assert(current_inpacket_hdr!=NULL);
					assert(current_inpacket_hdr->cmd==CMD_AUTH_REPLY);

					outlog_debug_mesh("client MESH session got client AUTH_REPLY");
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
						sizeof(statedata.before_auth.auth_cache.reply_hdr)+offsetof(struct packet_auth_reply, reltime_ns))) break;

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
					break;
				}
				assert(false);
				break;
			case PHASE_REPLY_BEING_SENT: {
				if(ev!=HEVENT_WRITE_READY) {
					outlog_notice("client MESH session got invalid event %d for REPLY_BEING_SENT phase before auth", int(ev));
					goto stopses;
				}
				outlog_debug_mesh("client MESH session from host %" IOT_PRIhostid " to host %" IOT_PRIhostid " established", config->gwinst->this_hostid, peer_host->host_id);

				int64_t delay_ns=statedata.before_auth.finish_reltime_ns-repack_uint64(statedata.before_auth.auth_cache.request.reltime_ns);
				if(delay_ns<=0) {
					assert(false);
					delay_ns=1;
				} else {
					cur_delay_us=uint32_t((delay_ns+500)/1000);
				}

				uint64_t routing_version=repack_uint64(statedata.before_auth.auth_cache.reply.routing_version);
				state_change(STATE_WORKING);
				config->gwinst->meshcontroller->confirm_routing_table_sync_topeer(this, routing_version);

				break;
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

void iot_netproto_session_mesh::state_handler_working(hevent_t ev) {
		int err;
		if(ev==HEVENT_CAN_WRITE) return;
		cmd_t cmdready; //keeps cmd which is reported as ready for ability to clean in universally
		if(ev==HEVENT_WRITE_READY) {
			cmdready=current_outpacket_cmd;
			current_outpacket_cmd=CMD_ILLEGAL;
		}

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
				break;
			case PHASE_NORMAL:
				if(ev==HEVENT_NEW_PACKET_HEADER) {
					assert(current_inpacket_hdr!=NULL);
					break;
				}
				if(ev==HEVENT_NEW_PACKET_READY) {
					assert(current_inpacket_hdr!=NULL);

					switch(current_inpacket_hdr->cmd) {
						case CMD_RTABLE_REQ: {//incoming routing table update
							packet_rtable_req* req=(packet_rtable_req*)(current_inpacket_hdr+1);
							if(current_inpacket_size!=sizeof(*req)+sizeof(packet_rtable_req::items[0])*repack_uint16(req->num_items)) { //invalid size
								routing_needconfirm=2;
								outlog_error("MESH session got CMD_RTABLE_REQ packet with invalid size %u, must be %u", unsigned(current_inpacket_size), unsigned(sizeof(*req)+sizeof(packet_rtable_req::items[0])*repack_uint16(req->num_items)));
							} else {
								outlog_debug_mesh("MESH session got CMD_RTABLE_REQ from %" IOT_PRIhostid, peer_host->host_id);
								config->gwinst->meshcontroller->sync_routing_table_frompeer(this, req);
								routing_needconfirm=1;
							}
							//TODO check signatures
							on_commandq(); //to send confirmation if possible
							break;
						}
						case CMD_SERVICE: {//service message received
							packet_service* req=(packet_service*)(current_inpacket_hdr+1);
							if(current_inpacket_size!=sizeof(*req)) { //invalid size
								outlog_error("MESH session got CMD_SERVICE packet with invalid size %u, must be %u", unsigned(current_inpacket_size), unsigned(sizeof(*req)));
								break;
							}
							//TODO check signatures
							process_service_packet(req);
							break;
						}
						case CMD_MESHTUN: {//tunnelled packet received. it can be processed locally or reassigned and proxied to another peer
							packet_meshtun_hdr* req=(packet_meshtun_hdr*)(current_inpacket_hdr+1);
							if(current_inpacket_size!=repack_uint32(req->datalen)+repack_uint16(req->hdrlen)) { //invalid size
								outlog_error("MESH session got CMD_MESHTUN packet with invalid size %u, must be %u", unsigned(current_inpacket_size), unsigned(repack_uint32(req->datalen)+repack_uint16(req->hdrlen)));
								assert(false);
								break;
							}
							//TODO check signatures
							if(!process_meshtun_proxy_input(req)) goto stopses;
							break;
						}
						default:
							outlog_notice("MESH session got unknown command %d", int(current_inpacket_hdr->cmd));
							assert(false);
							goto stopses;
					}

					break;
				}
				if(ev==HEVENT_WRITE_READY) {
					switch(cmdready) {
//						case CMD_RTABLE_REQ:
//							routing_syncing=false;
//							config->gwinst->meshcontroller->confirm_routing_table_sync_topeer(this);//temporary until SRVCODE_RTABLE_UPDATED is implemented
//							break;
						case CMD_MESHTUN: {
							assert(current_outmeshtun!=NULL);
							if(current_outmeshtun->type==TUN_STREAM) {
								process_meshtun_outputready(static_cast<iot_meshtun_stream_state*>((iot_meshtun_packet*)current_outmeshtun));
							} else if(current_outmeshtun->type==TUN_FORWARDING) {
								auto st=static_cast<iot_meshtun_forwarding*>((iot_meshtun_packet*)current_outmeshtun);
								assert(st->meshses==this);
								st->meshses=NULL;
								st->set_closed(false);
							} else if(current_outmeshtun->type==TUN_DATAGRAM) {
								assert(false); //TODO
							} else {
								assert(false); //TUN_STREAM_LISTEN cannot be here
							}
							current_outmeshtun=NULL; //release reference as soon as possible (pop_meshtun() on next iteration requires much time)
						}
						default:
							break;
					}
					on_commandq();
					break;
				}
				break;
			default:
				outlog_notice("MESH session in WORKING state got to phase %d, event %d", int(phase), int(ev));
		}

		return;
stopses:
		stop(false);
}


bool iot_netproto_session_mesh::check_privileged_output(void) { //must be called when no output in progress
//returns true if writing was initiated
	assert(!current_outpacket_cmd);
	//do checks for different service output in order of logical importance
	if(routing_needconfirm) {
		send_service_packet(SRVCODE_RTABLE_UPDATED, routing_needconfirm==1 ? 0 : 1, 0, 0, peer_host->get_origroutes_version());
		routing_needconfirm=0;
		return true;
	}
	if(!routing_syncing && peer_host->is_notify_routing_update()) {
		if(config->gwinst->meshcontroller->sync_routing_table_topeer(this)) { //begin syncing routing table immediately
			routing_syncing=true;
			return true;
		}
	}
	return false;
}

bool iot_netproto_session_mesh::process_meshtun_local_input(packet_meshtun_hdr* req) {
	//return value of false means to abort mesh session because of protocol error
	iot_hostid_t srchost=repack_hostid(req->srchost);
	uint16_t flags=repack_uint16(req->flags);


	iot_objref_ptr<iot_meshtun_state> lstate;
	if(!config->gwinst->meshcontroller->meshtun_find_bound(repack_uint16(req->protoid), repack_uint16(req->dstport), srchost, repack_uint16(req->srcport), lstate)) {
		//not found. act accordongly to meshtun type
		if(flags & MTFLAG_STREAM) { //this is stream
			if(!(flags & MTFLAG_STREAM_RESET)) {
				outlog_debug_meshtun("GOT stream packet of non-existing connection with proto %u on local port %u from host %" IOT_PRIhostid ", port %u. will RESET", unsigned(repack_uint16(req->protoid)), unsigned(repack_uint16(req->dstport)), srchost, repack_uint16(req->srcport));
				return reset_meshtun_stream(req);
			}
			//it was RESET, no need to answer
			return true;
		}
		//this is datagram
		assert(false); //TODO
		//TODO
		return true;
	}

	if(flags & MTFLAG_STATUS) {
		//!!!!!! src and dst host and port are correct but stream properties are unchanged!
		if(lstate->type==TUN_STREAM) {
			if(!(flags & MTFLAG_STREAM)) return true; //ignore non streamed packets

			if(!process_meshtun_status(static_cast<iot_meshtun_stream_state*>((iot_meshtun_state*)lstate), req)) return false;
		} else {
			assert(false);
			return false;
		}
		return true;
	}

	if(lstate->type==TUN_STREAM) {
		if(!(flags & MTFLAG_STREAM)) return true; //ignore non streamed packets

		if(!process_meshtun_input(static_cast<iot_meshtun_stream_state*>((iot_meshtun_state*)lstate), req)) return false;
	} else if(lstate->type==TUN_STREAM_LISTEN) {
		if(!(flags & MTFLAG_STREAM)) return true; //ignore non streamed packets

		if(flags & MTFLAG_STREAM_SYN) {
			if(!process_meshtun_input(static_cast<iot_meshtun_stream_listen_state*>((iot_meshtun_state*)lstate), req)) return false;
		} else {
			if(!(flags & MTFLAG_STREAM_RESET)) {
				outlog_debug_meshtun("GOT stream packet of non-existing connection with proto %u on local port %u from host %" IOT_PRIhostid ", port %u. will RESET", unsigned(repack_uint16(req->protoid)), unsigned(repack_uint16(req->dstport)), srchost, repack_uint16(req->srcport));
				return reset_meshtun_stream(req);
			}
		}
	} else if(lstate->type==TUN_DATAGRAM) {
		if(flags & MTFLAG_STREAM) return true; //ignore streamed packets
		assert(false); //TODO
	} else {
		assert(false); //TUN_STREAM_FORWARDING cannot be here
	}

	return true;
}

//generates iot_meshtun_forwarding packet to send RESET to sender of provided stream packet
bool iot_netproto_session_mesh::reset_meshtun_stream(packet_meshtun_hdr* req) { //must have MTFLAG_STREAM flag
	//return value of false means to abort mesh session because of protocol error
	uint16_t reqflags=repack_uint16(req->flags);
	assert(reqflags & MTFLAG_STREAM);

	if(reqflags & MTFLAG_STREAM_RESET) return true; //do nothing. this is RESET request

	uint16_t metasize;
	packet_meshtun_stream *stream;

	metasize=uint16_t(req->metalen64)<<3;
	if(repack_uint16(req->hdrlen)<=sizeof(*req)+metasize+sizeof(packet_meshtun_stream)) { //signature size not included in sum, so no check for ==
		assert(false); //invalid packet structure
		return false;
	}


	iot_hostid_t dsthost=repack_hostid(req->dsthost);
	iot_hostid_t srchost=repack_hostid(req->srchost);
	
	if(srchost==peer_host->gwinst->this_hostid) { //something is wrong if source host is ours
		assert(false);
		return false;
	}

	outlog_debug_meshtun("GENERATING RESET to streamed meshtun FROM HOST %" IOT_PRIhostid, srchost);

	iot_meshtun_forwarding* fw;
	uint32_t sz=sizeof(iot_meshtun_forwarding)+repack_uint16(req->hdrlen);
	fw=(iot_meshtun_forwarding*)coniface->allocator->allocate(sz);
	if(!fw) {
		outlog_debug_meshtun("Cannot allocate %u bytes to send reset to host %" IOT_PRIhostid ", dropping it", sz, srchost);
		return true;
	}
	packet_meshtun_hdr* myreq;
	myreq=(packet_meshtun_hdr*)(fw+1);
	memcpy(myreq, req, repack_uint16(req->hdrlen)); //save headers in the same buffer as iot_meshtun_forwarding

	//swap source and destination
	myreq->dsthost=req->srchost;
	myreq->srchost=req->dsthost;
	myreq->dstport=req->srcport;
	myreq->srcport=req->dstport;

	//set RESET flag
	myreq->flags=repack_uint16((reqflags & ~(MTFLAG_STREAM_SYN|MTFLAG_STREAM_FIN|MTFLAG_STREAM_ACK|MTFLAG_STREAM_WANTACK)) | MTFLAG_STREAM_RESET);

	myreq->datalen=0;
	myreq->ttl=0; //mark that ttl must be assigned during output

	stream=(packet_meshtun_stream *)(((char*)(myreq+1))+metasize);

	stream->data_sequence=stream->ack_sequence;
	stream->ack_sequence=0;
	stream->num_sack_ranges=0;
	stream->statuscode=0;
	stream->reserved=0;

	new(fw) iot_meshtun_forwarding(config->gwinst->meshcontroller, srchost, dsthost, 0, myreq);

	iot_objref_ptr<iot_meshtun_forwarding> fwptr=iot_objref_ptr<iot_meshtun_forwarding>(true, fw);

	int err=fw->forward(NULL);
	if(err) {
		assert(false);
		return true; //fwptr will be released
	}

	//on success request will be referenced by queue, so must not de deallocated
	return true;
}



bool iot_netproto_session_mesh::process_meshtun_proxy_input(packet_meshtun_hdr* req) {
	//return value of false means to abort mesh session because of protocol error
	iot_hostid_t dsthost=repack_hostid(req->dsthost);
	iot_hostid_t srchost=repack_hostid(req->srchost);
	
	if(srchost==peer_host->gwinst->this_hostid) { //something is wrong if source host is ours
		assert(false);
		return false;
	}

	if(dsthost==peer_host->gwinst->this_hostid) {
		outlog_debug_meshtun("GOT LOCAL MESHTUN FROM HOST %" IOT_PRIhostid, srchost);
		if(!process_meshtun_local_input(req)) return false;
		return true;
	} else {
		outlog_debug_meshtun("GOT NON-LOCAL MESHTUN FROM HOST %" IOT_PRIhostid " TO HOST %" IOT_PRIhostid, srchost, dsthost);
	}

	if(!srchost || !dsthost) {
		assert(false);
		return true; //ignore
	}

	if(req->ttl<2) { //must be at least 2 so that after decrement it became at least 1
		outlog_debug_meshtun("Forwarded packet from host %" IOT_PRIhostid " to host %" IOT_PRIhostid " dropped due to TTL", srchost, dsthost);
		return true;
	}

	iot_meshtun_forwarding* fw;
	uint32_t sz=sizeof(iot_meshtun_forwarding)+repack_uint16(req->hdrlen);
	fw=(iot_meshtun_forwarding*)coniface->allocator->allocate(sz);
	if(!fw) {
		outlog_debug_meshtun("Cannot allocate %u bytes to forward packet from host %" IOT_PRIhostid " to host %" IOT_PRIhostid ", dropping it", sz, srchost, dsthost);
		return true;
	}
	memcpy(fw+1, req, repack_uint16(req->hdrlen)); //save headers in the same buffer as iot_meshtun_forwarding
	((packet_meshtun_hdr*)(fw+1))->ttl--; //will be at least 1
	new(fw) iot_meshtun_forwarding(config->gwinst->meshcontroller, dsthost, srchost, peer_host->host_id, (packet_meshtun_hdr*)(fw+1));

	iot_objref_ptr<iot_meshtun_forwarding> fwptr=iot_objref_ptr<iot_meshtun_forwarding>(true, fw);

	//now allocate memory for request body
	sz=repack_uint32(req->datalen);
	void *data;
	if(sz>0) {
		data=coniface->allocator->allocate(sz, true);
		if(!data) { //no memory, drop packet
			outlog_debug_meshtun("Cannot allocate %u bytes for data to forward packet from host %" IOT_PRIhostid " to host %" IOT_PRIhostid ", dropping it", sz, srchost, dsthost);
			return true; //fwptr will be released
		}
		memcpy(data, ((char*)req)+repack_uint16(req->hdrlen), sz);
	} else data=NULL;

	int err=fw->forward(data);
	if(data) iot_release_memblock(data);
	if(err) {
		assert(false);
		return true; //fwptr will be released
	}

	//on success request will be referenced by queue, so must not de deallocated
	return true;
}




bool iot_netproto_session_mesh::process_meshtun_input(iot_meshtun_stream_listen_state *st, packet_meshtun_hdr* req) {
	st->lock();
	int err;
	uint16_t metasize;
	uint16_t flags;
	packet_meshtun_stream *stream;
	iot_meshtun_stream_listen_state::listenq_item it;

	outlog_debug_meshtun("STREAM CONN REQUEST FROM HOST %" IOT_PRIhostid " via %" IOT_PRIhostid, repack_hostid(req->srchost), peer_host->host_id);

	if(st->input_closed) {
		err=IOT_ERROR_INVALID_STATE;
		goto onexit;
	}
	assert(st->con!=NULL);

	metasize=uint16_t(req->metalen64)<<3;
	flags=repack_uint16(req->flags);
	if(repack_uint16(req->hdrlen)<=sizeof(*req)+metasize+sizeof(packet_meshtun_stream)) { //signature size not included in sum, so no check for ==
		assert(false);
		err=IOT_ERROR_CRITICAL_ERROR;
		goto onexit;
	}
	if(flags & (MTFLAG_STREAM_FIN | MTFLAG_STREAM_ACK | MTFLAG_STREAM_RESET)) { //check flags are correct for initial SYN request (STREAM and SYN checked in caller function)
		err=IOT_ERROR_TRY_AGAIN;
		goto onexit;
	}

	if(st->listenq.avail_write()<sizeof(it)+metasize) { //listen queue is overfilled
		err=IOT_ERROR_TRY_AGAIN;
		goto onexit;
	}

	stream=(packet_meshtun_stream *)(((char*)(req+1))+metasize);

	it.remotehost=repack_hostid(req->srchost);
	it.initial_sequence=repack_uint64(stream->data_sequence);
	it.creation_time=0;
	it.request_time=((iot_get_systime()+500000000ull)/1000000000ull)-1000000000ull; //round to integer seconds and offset by 1e9 seconds
	it.peer_rwnd=repack_uint32(stream->rwndsize);
	it.remoteport=repack_uint16(req->srcport);
	it.metasize=metasize;
	memcpy(it.random, stream->streamrandom, IOT_MESHPROTO_TUNSTREAM_IDLEN);
	uint32_t res;
	res=st->listenq.write(&it, sizeof(it));
	if(res!=sizeof(it)) {
		assert(false);
		//force this listen socket to be closed
		if(!st->get_error()) st->set_error(IOT_ERROR_CRITICAL_ERROR);
		err=IOT_ERROR_INVALID_STATE;
		goto onexit;
	}
	if(metasize>0) {
		res=st->listenq.write(req->meta, metasize);
		if(res!=metasize) {
			assert(false);
			//force this listen socket to be closed
			if(!st->get_error()) st->set_error(IOT_ERROR_CRITICAL_ERROR);
			err=IOT_ERROR_INVALID_STATE;
			goto onexit;
		}
	}
	st->input_pending=1;
	st->on_state_update(st->UPD_READREADY);
	err=0;
onexit:
	st->unlock();

	if(err==IOT_ERROR_CRITICAL_ERROR) { //mesh session should be aborted
		outlog_debug_meshtun("CONNECTING TO LISTENING got protocol error");
		return false;
	}
	if(err) {
		outlog_debug_meshtun("CONNECTING TO LISTENING got error: %s, ignoring request", kapi_strerror(err));
		return true;
	}
	//here netcon must be notified abount connection request
	return true;
}

bool iot_netproto_session_mesh::process_meshtun_status(iot_meshtun_stream_state *st, packet_meshtun_hdr* req) { //notification from some proxy about forwarding error
	st->lock();
	int err=0;
	uint16_t metasize;
	packet_meshtun_stream *stream;
	uint64_t data_seq;

	//got some error (for now only NO_ROUTE can arrive), so try to find corresponding inprog block and repeat it immediately. it is assumed that routing table is already updated
	//imitate time out event

	outlog_debug_meshtun("STREAM STATUS FOR PEER HOST %" IOT_PRIhostid " via %" IOT_PRIhostid " [", st->connection_key.host, peer_host->host_id);

	metasize=uint16_t(req->metalen64)<<3;
	if(repack_uint16(req->hdrlen)<=sizeof(*req)+metasize+sizeof(packet_meshtun_stream)) { //signature size not included in sum, so no check for ==
		assert(false);
		err=IOT_ERROR_CRITICAL_ERROR;
		goto onexit;
	}

	stream=(packet_meshtun_stream *)(((char*)(req+1))+metasize);

	if(memcmp(stream->streamrandom, st->random, IOT_MESHPROTO_TUNSTREAM_IDLEN)!=0) {
		outlog_debug_meshtun(" <!%" IOT_PRIhostid " INVALID STREAM ID, IGNORING", st->connection_key.host);
		goto onexit;
	}

	assert(st->state==st->ST_SENDING_SYN || st->state==st->ST_ESTABLISHED);

	if(repack_int16(stream->statuscode)!=IOT_ERROR_NO_ROUTE) {
		assert(false);
		//TODO other errors processing
		goto onexit;
	}
	//process IOT_ERROR_NO_ROUTE

	data_seq=repack_uint64(stream->data_sequence)-st->initial_sequence_in;

	for(uint16_t i=0; i<st->num_output_inprog; i++) {
		if(st->output_inprogress[i].seq_pos<data_seq) continue;
		if(st->output_inprogress[i].seq_pos>data_seq) break;
		//seq_pos matches data seq

		//inprog block must be non-ack and non-pending, be waiting for retry timer
		if(st->output_inprogress[i].is_ack || st->output_inprogress[i].is_pending || st->output_inprogress[i].meshses || st->output_inprogress[i].retry_after==UINT64_MAX) break;

		st->output_inprogress[i].retry_after=UINT64_MAX;
		st->output_inprogress[i].is_pending=1;
		st->reset_timer(uv_now(thread->loop));
		assert(st->output_ack_pending);
		st->on_state_update(st->UPD_WRITEREADY);
		break;
	}

onexit:
	st->unlock();
	if(err) {
		if(err==IOT_ERROR_CRITICAL_ERROR) { //mesh session should be aborted
			outlog_debug_meshtun(" <!%" IOT_PRIhostid " ], PROCESSING STATUS got protocol error", st->connection_key.host);
			return false;
		}
		outlog_debug_meshtun(" <!%" IOT_PRIhostid " ], PROCESSING STATUS got error: %s, ignoring request", st->connection_key.host, kapi_strerror(err));
		return true;
	}
	outlog_debug_meshtun(" <!%" IOT_PRIhostid " ]", st->connection_key.host);
	//here netcon must be notified abount connection request
	return true;
}


bool iot_netproto_session_mesh::process_meshtun_input(iot_meshtun_stream_state *st, packet_meshtun_hdr* req) {
	st->lock();
	int err=0;
	uint16_t metasize;
	uint16_t flags;
	packet_meshtun_stream *stream;

	outlog_debug_meshtun("STREAM INPUT FROM HOST %" IOT_PRIhostid " via %" IOT_PRIhostid " [", st->connection_key.host, peer_host->host_id);

	metasize=uint16_t(req->metalen64)<<3;
	flags=repack_uint16(req->flags);
	if(repack_uint16(req->hdrlen)<=sizeof(*req)+metasize+sizeof(packet_meshtun_stream)) { //signature size not included in sum, so no check for ==
		assert(false);
		err=IOT_ERROR_CRITICAL_ERROR;
		goto onexit;
	}

	stream=(packet_meshtun_stream *)(((char*)(req+1))+metasize);

	if(memcmp(stream->streamrandom, st->random, IOT_MESHPROTO_TUNSTREAM_IDLEN)!=0) {
		uint16_t f=flags & (MTFLAG_STREAM_SYN | MTFLAG_STREAM_RESET | MTFLAG_STREAM_FIN | MTFLAG_STREAM_ACK); //slice of flags
		if(st->state==st->ST_ESTABLISHED && f==MTFLAG_STREAM_SYN) { //this looks like connection request?
			//enforce ACK to force valid peer to send us correct reset
			st->input_ack_pending=1; //mark that ACK must be sent
			st->on_state_update(st->UPD_WRITEREADY); //TODO do by timer to wait some small time for additional output
			goto onexit;
		}
		
		outlog_debug_meshtun(" <%" IOT_PRIhostid " INVALID STREAM ID, WILL SEND RESET", st->connection_key.host);
		if(!(flags & MTFLAG_STREAM_RESET)) reset_meshtun_stream(req);
		goto onexit;
	}

	assert(st->state==st->ST_SENDING_SYN || st->state==st->ST_ESTABLISHED);

	bool writeready;
	uint64_t now;
	now=uv_now(thread->loop);
	writeready=false;

	uint64_t data_seq;
	data_seq=repack_uint64(stream->data_sequence);

	if(flags & MTFLAG_STREAM_RESET) { //incoming RESET
		outlog_debug_meshtun(" <%" IOT_PRIhostid " RESET with sequence=%llu", st->connection_key.host, data_seq);
		//TODO check data_sequence is acceptable
		if(!st->input_closed && int64_t(data_seq-st->initial_sequence_in)>0) { //RESET can be applied to current stream
			data_seq-=st->initial_sequence_in; //calculate relative seq number
			if(data_seq>=st->maxseen_sequenced_in && data_seq<st->maxseen_sequenced_in + st->buffer_in.getsize()) { //buffer_in.getsize is maximum rwnd size. RESET is valid
				if(!st->get_error()) st->set_error(IOT_ERROR_CONN_RESET);
			} else {
				outlog_debug_meshtun(" <%" IOT_PRIhostid " RESET had invalid seq", st->connection_key.host);
			}
		}
		err=0;
		goto onexit;
	}


	//apply possible SYN from PASSIVE peer
	if((flags & MTFLAG_STREAM_SYN)) {
		outlog_debug_meshtun(" <%" IOT_PRIhostid " SYN with sequence = %llu %s", st->connection_key.host, data_seq, st->input_closed && st->maxseen_sequenced_in==0 && !st->is_passive_stream ? "" : "IGNORED");
		if(st->input_closed && st->maxseen_sequenced_in==0 && !st->is_passive_stream) {
			st->input_closed=0;
			st->initial_sequence_in=data_seq;
			st->sequenced_in=st->maxseen_sequenced_in=1; //account for received SYN
		}
		st->input_ack_pending=1; //mark that ACK must be sent
		writeready=true;
		data_seq++;
	}

	if(int64_t(data_seq-st->initial_sequence_in)>0) {
		uint32_t datalen;
		datalen=repack_uint32(req->datalen);
		uint32_t has_fin=(flags & MTFLAG_STREAM_FIN) ? 1 : 0;
#ifndef NDEBUG
		outlog_debug_meshtun(" <%" IOT_PRIhostid " DATA sequence = %llu", st->connection_key.host, data_seq);
		if(datalen>0) outlog_debug_meshtun(" <%" IOT_PRIhostid " DATA LEN=%u bytes", st->connection_key.host, unsigned(datalen));
		if(has_fin) outlog_debug_meshtun(" <%" IOT_PRIhostid " FIN", st->connection_key.host);
#endif
		data_seq-=st->initial_sequence_in; //calculate relative seq number

		if(datalen && st->input_closed) { //new incoming data after stream was closed. need to reset. BUT ALLOW (datalen to be 0) repeated FIN processing even with input closed
			st->set_state(st->ST_RESET);
			goto onexit;
		}
		if(datalen || has_fin) {
			char* data=((char*)req)+repack_uint16(req->hdrlen);
			st->indata_merge(data_seq, datalen, data, has_fin);
			st->input_ack_pending=1; //mark that ACK must be sent
		}
	}

	//apply ACK
	outlog_debug_meshtun(" <%" IOT_PRIhostid " ACK = %llu %s", st->connection_key.host, repack_uint64(stream->ack_sequence), st->output_ack_pending && (flags & MTFLAG_STREAM_ACK) ? "" : "IGNORED");
	if(st->output_ack_pending && (flags & MTFLAG_STREAM_ACK)) {
		//reformat SACK data from packet
		uint64_t sack_seq[IOT_MESHTUNSTREAM_MAXHOLES];
		uint32_t sack_len[IOT_MESHTUNSTREAM_MAXHOLES];
		uint64_t ack=repack_uint64(stream->ack_sequence)-st->initial_sequence_out;
		if(stream->num_sack_ranges>0) {
			assert(repack_uint32(stream->sack_ranges[0].offset)>0 && repack_uint32(stream->sack_ranges[0].len)>0);
			sack_seq[0]=ack+repack_uint32(stream->sack_ranges[0].offset);
			sack_len[0]=repack_uint32(stream->sack_ranges[0].len);
			for(uint16_t i=1; i<stream->num_sack_ranges; i++) {
				assert(repack_uint32(stream->sack_ranges[i].offset)>0 && repack_uint32(stream->sack_ranges[i].len)>0);
				sack_seq[i]=sack_seq[i-1]+sack_len[i-1]+repack_uint32(stream->sack_ranges[i].offset);
				sack_len[i]=repack_uint32(stream->sack_ranges[i].len);
			}

			if(sack_seq[stream->num_sack_ranges-1]+sack_len[stream->num_sack_ranges-1]>st->sequenced_out) { //got ACK greater than outputted sequence
				assert(false);
				err=IOT_ERROR_CRITICAL_ERROR;
				goto onexit;
			}
		} else {
			if(ack>st->sequenced_out) { //got ACK greater than outputted sequence
				assert(false);
				err=IOT_ERROR_CRITICAL_ERROR;
				goto onexit;
			}
		}
		//APPLY ACK
		st->ack_sequence_out_merge(ack, stream->num_sack_ranges, sack_seq, sack_len, now);

		if(!st->output_ack_pending && st->output_closed) { //all output ready and no new output allowed, so check if state can be closed by calling on_state_update(UPD_WRITEREADY)
			writeready=true;
		}
	}

	st->peer_rwnd=repack_uint32(stream->rwndsize);
	outlog_debug_meshtun(" <%" IOT_PRIhostid " RWND SIZE = %u", st->connection_key.host, unsigned(st->peer_rwnd));

	if(writeready) {
		st->on_state_update(st->UPD_WRITEREADY); //TODO do by timer to wait some small time for additional output
	} else if(st->input_ack_pending) { //ACK must be sent to peer but no new output requested right here, so schedule input_ack_expires timer
		if(st->output_closed || (flags & MTFLAG_STREAM_WANTACK)) { //no further output OR immediate ACK requested, print ACK immediately
			st->on_state_update(st->UPD_WRITEREADY); //TODO do by timer to wait some small time for additional output
		} else if(st->input_ack_expires==UINT64_MAX) { //timer is not scheduled already
			st->input_ack_expires=now+IOT_MESHTUNSTREAM_INPUTACK_DELAY;
			st->set_timer(now, IOT_MESHTUNSTREAM_INPUTACK_DELAY);
		}
	}

onexit:
	st->unlock();

	if(err) {
		if(err==IOT_ERROR_CRITICAL_ERROR) { //mesh session should be aborted
			outlog_debug_meshtun(" <%" IOT_PRIhostid " ], PROCESSING READ got protocol error", st->connection_key.host);
			return false;
		}
		outlog_debug_meshtun(" <%" IOT_PRIhostid " ], PROCESSING READ got error: %s, ignoring request", st->connection_key.host, kapi_strerror(err));
		return true;
	}
	outlog_debug_meshtun(" <%" IOT_PRIhostid " ]", st->connection_key.host);
	//here netcon must be notified abount connection request
	return true;
}


bool iot_netproto_session_mesh::process_meshtun_output(iot_meshtun_stream_state *st, uint32_t delay, uint16_t pathlen) {
	//here st must have refcount which accounts for current thread
	bool rval=false;
	st->lock();

	int err;
	outlog_debug_meshtun("STREAM OUTPUT TO HOST %" IOT_PRIhostid " USING MESHSES TO HOST %" IOT_PRIhostid " [", st->connection_key.host, peer_host->host_id);
	switch(st->state) {
		case st->ST_SENDING_SYN: //new client connection request
		case st->ST_ESTABLISHED: {
			iot_meshtun_stream_state::output_inprogress_t inprogbuf;
			auto inprog=st->lock_output_inprogress(this, delay, 3, inprogbuf);
			if(!inprog) break; //output already being done. just release reference

			outlog_debug_meshtun(" >%" IOT_PRIhostid " DATA sequence=%llu, rwnd size=%u", st->connection_key.host, inprog->seq_pos+st->initial_sequence_out, st->local_rwnd.load(std::memory_order_relaxed));

			uint8_t actual_syn=inprog->has_syn || (st->ack_sequenced_out==0 && inprog->seq_pos==1); //no ACK to our SYN received and output of first data block is attempted
			uint8_t actual_fin=inprog->has_fin;

			uint16_t flags=MTFLAG_STREAM;

			uint32_t datalen=inprog->seq_len - inprog->has_syn - inprog->has_fin;
			assert(datalen<IOT_MESHTUNSTREAM_MTU);

			if(datalen) {
				if(datalen>st->peer_rwnd) { //peer window is less than pending data
					outlog_debug_meshtun(" >%" IOT_PRIhostid " DATA LEN = %u bytes was shrinked to %u by peer's rwnd", st->connection_key.host, unsigned(datalen), unsigned(st->peer_rwnd));
					datalen=st->peer_rwnd;
					actual_fin=0;
				} else outlog_debug_meshtun(" >%" IOT_PRIhostid " DATA LEN = %u bytes", st->connection_key.host, unsigned(datalen));
			}

			if(actual_syn) {
				outlog_debug_meshtun(" >%" IOT_PRIhostid " SYN%s", st->connection_key.host, !inprog->has_syn ? " enforced" : "");
				flags|=MTFLAG_STREAM_SYN;
			}

			if(actual_fin) {
				outlog_debug_meshtun(" >%" IOT_PRIhostid " FIN", st->connection_key.host);
				flags|=MTFLAG_STREAM_FIN;
			}

			authtype_t atype=AUTHTYPE_UNSET; //TODO
			uint32_t adatasz;
			if(!choose_authtype(atype, adatasz)) {
				st->unlock_output_inprogress(this, true);
				st->set_error(IOT_ERROR_CRITICAL_ERROR);
				break;
			}

			uint32_t metasz=0;
			if(st->outmeta_data && inprog->seq_len>0) {
				metasz=st->outmeta_size;
				assert(metasz>0);
				if(metasz%8!=0) metasz += 8 - metasz%8; //align to 8-byte
			}

			uint32_t headlen=sizeof(packet_meshtun_hdr)+metasz+sizeof(packet_meshtun_stream)+adatasz;

			if(!st->input_closed || st->input_ack_pending) {
				flags|=MTFLAG_STREAM_ACK;
				headlen+=sizeof(packet_meshtun_stream::sack_range_t)*st->num_in_dis;
				outlog_debug_meshtun(" >%" IOT_PRIhostid " ACK %llu", st->connection_key.host, st->sequenced_in+st->initial_sequence_in);
			}
			if(st->buffer_out.avail_write() > st->buffer_out.getsize()/2) flags|=MTFLAG_STREAM_WANTACK; //out buffer is filled more than a half, request immediate ACK

			if(headlen>0xFFFF) {
				assert(false);
				st->unlock_output_inprogress(this, true);
				st->set_error(IOT_ERROR_CRITICAL_ERROR);
				break;
			}

			packet_hdr* hdr=prepare_packet_header(CMD_MESHTUN, headlen+datalen);
			packet_meshtun_hdr *req=(packet_meshtun_hdr*)(hdr+1);
			char* nextpart=((char*)(req+1))+metasz;

			memset(req, 0, sizeof(*req)+metasz);
			req->srchost=repack_hostid(peer_host->gwinst->this_hostid);
			req->dsthost=repack_hostid(st->connection_key.host);

			req->srcport=repack_uint16(st->connection_key.localport);
			req->dstport=repack_uint16(st->connection_key.remoteport);
			req->protoid=repack_uint16(st->protometa->type_id);
			req->flags=repack_uint16(flags);

			req->datalen=repack_uint32(datalen);
			req->hdrlen=repack_uint16(uint16_t(headlen));

			if(metasz>0) {
				req->metalen64=uint8_t(metasz>>3);
				memcpy(req->meta, st->outmeta_data, st->outmeta_size);
			}
			uint32_t ttl=config->gwinst->peers_registry->get_num_peers();
			if(ttl>pathlen+4u) ttl=pathlen+4u;
			req->ttl=ttl>256 ? 255 : ttl < 1 ? 1 : uint8_t(ttl);

			packet_meshtun_stream *req_s=(packet_meshtun_stream*)nextpart;
			nextpart=(char*)(req_s+1)+sizeof(sizeof(packet_meshtun_stream::sack_range_t))*st->num_in_dis;
			memset(req_s, 0, sizeof(*req_s));

			memcpy(req_s->streamrandom, st->random, sizeof(st->random));

			assert(!inprog->has_syn || st->num_in_dis==0); //must not have incoming holes during SYN

			if(flags & MTFLAG_STREAM_ACK) {
				uint64_t ack=st->sequenced_in;
				req_s->ack_sequence=repack_uint64(ack+st->initial_sequence_in);
				for(uint16_t i=0;i<st->num_in_dis; i++) {
					assert(st->sequenced_in_dis[i]>ack);
					assert(st->sequenced_in_dis_len[i]>0);
					req_s->sack_ranges[i].offset=repack_uint32(uint32_t(st->sequenced_in_dis[i]-ack));
					req_s->sack_ranges[i].len=repack_uint32(uint32_t(st->sequenced_in_dis_len[i]));
					ack=st->sequenced_in_dis[i]+st->sequenced_in_dis_len[i];
				}
				req_s->num_sack_ranges=st->num_in_dis;
			} else {
//				req_s->ack_sequence=repack_uint64(0); 0 is implied by memset
//				req_s->num_sack_ranges=0; 0 is implied by memset
			}

			req_s->data_sequence=repack_uint64(inprog->seq_pos+st->initial_sequence_out-uint64_t(actual_syn && !inprog->has_syn ? 1 : 0));
			req_s->rwndsize=repack_uint32(st->local_rwnd.load(std::memory_order_relaxed));

			//add signature after all headers
			if(!fill_authdata(atype, nextpart, adatasz, (char*)hdr, sizeof(*hdr)+headlen-adatasz)) {
				st->unlock_output_inprogress(this, true);
				st->set_error(IOT_ERROR_CRITICAL_ERROR);
				break;
			}
			if(datalen>0) { //there is data to send
				nextpart+=adatasz;
				assert(nextpart==(char*)(hdr+1)+headlen);

				assert(inprog->seq_pos>=st->ack_sequenced_out);
				uint32_t sz=st->buffer_out.peek(nextpart, datalen, (inprog->seq_pos + inprog->has_syn) - (st->ack_sequenced_out > 0 ? st->ack_sequenced_out : 1)); //ack_sequenced_out corresponds to readpos of buffer_out
				assert(sz==datalen);
			}

			st->input_ack_pending=0; //any output always send current pending input ACK, so always reset here
			if(st->input_ack_expires!=UINT64_MAX) {
				st->input_ack_expires=UINT64_MAX;
				st->reset_timer(uv_now(thread->loop));
			}

//			st->on_state_update(st->UPD_WRITTEN);//=st->ST_SENDING_SYN;
			st->unlock();

			err=coniface->write_data(hdr, sizeof(*hdr)+headlen+datalen);
			assert(err==0);
			outlog_debug_meshtun(" >%" IOT_PRIhostid " ]", st->connection_key.host);
			return true;
		}
		case st->ST_RESET: {
			iot_meshtun_stream_state::output_inprogress_t inprogbuf;
			auto inprog=st->lock_output_inprogress(this, delay, 2, inprogbuf);
			if(!inprog) break; //output already being done. just release reference

			outlog_debug_meshtun("WRITING RESET TO HOST %" IOT_PRIhostid, st->connection_key.host);

			authtype_t atype=AUTHTYPE_UNSET; //TODO
			uint32_t adatasz;
			if(!choose_authtype(atype, adatasz)) {
				st->unlock_output_inprogress(this, true);
				st->set_error(IOT_ERROR_CRITICAL_ERROR);
				break;
			}

			uint32_t headlen=sizeof(packet_meshtun_hdr)+sizeof(packet_meshtun_stream)+adatasz;

			if(headlen>0xFFFF) {
				assert(false);
				st->unlock_output_inprogress(this, true);
				st->set_error(IOT_ERROR_CRITICAL_ERROR);
				break;
			}
			uint16_t flags=MTFLAG_STREAM | MTFLAG_STREAM_RESET;

			packet_hdr* hdr=prepare_packet_header(CMD_MESHTUN, headlen);
			packet_meshtun_hdr *req=(packet_meshtun_hdr*)(hdr+1);
			char* nextpart=((char*)(req+1));

			memset(req, 0, sizeof(*req));
			req->srchost=repack_hostid(peer_host->gwinst->this_hostid);
			req->dsthost=repack_hostid(st->connection_key.host);

			req->srcport=repack_uint16(st->connection_key.localport);
			req->dstport=repack_uint16(st->connection_key.remoteport);
			req->protoid=repack_uint16(st->protometa->type_id);
			req->flags=repack_uint16(flags);

//			req->datalen=repack_uint32(0); 0 is implied
			req->hdrlen=repack_uint16(uint16_t(headlen));

			uint32_t ttl=config->gwinst->peers_registry->get_num_peers();
			if(ttl>pathlen+4u) ttl=pathlen+4u;
			req->ttl=ttl>256 ? 255 : ttl < 1 ? 1 : uint8_t(ttl);

			packet_meshtun_stream *req_s=(packet_meshtun_stream*)nextpart;
			nextpart=(char*)(req_s+1);
			memset(req_s, 0, sizeof(*req_s));

			memcpy(req_s->streamrandom, st->random, sizeof(st->random));

//			req_s->ack_sequence=repack_uint64(0); 0 is implied
//			req_s->num_sack_ranges=0; 0 is implied

			req_s->data_sequence=repack_uint64(inprog->seq_pos+st->initial_sequence_out);
//			req_s->rwndsize=repack_uint32(0); 0 is implied

			//add signature after all headers
			if(!fill_authdata(atype, nextpart, adatasz, (char*)hdr, sizeof(*hdr)+headlen-adatasz)) {
				st->unlock_output_inprogress(this, true);
				st->set_error(IOT_ERROR_CRITICAL_ERROR);
				break;
			}

//MUST NOT BE DONE HERE for RESET			st->on_state_update(st->UPD_WRITTEN);
			st->unlock();

			err=coniface->write_data(hdr, sizeof(*hdr)+headlen);
			assert(err==0);
			outlog_debug_meshtun(" >%" IOT_PRIhostid " ]", st->connection_key.host);
			return true;
		}
		case st->ST_CLOSED:
			break;
		default:
			assert(false);
	}
//onexit:
	outlog_debug_meshtun(" >%" IOT_PRIhostid " ]", st->connection_key.host);
	st->unlock();
	return rval;
}

void iot_netproto_session_mesh::process_meshtun_outputready(iot_meshtun_stream_state *st) {
	st->lock();
	switch(st->state) {
		case st->ST_RESET:
			st->output_ack_pending=0; //this stops futher tries to send RESET
		case st->ST_SENDING_SYN: //new SYN request was sent
		case st->ST_ESTABLISHED:// {
////			if(!st->output_ack_pending || st->get_state()!=st->ST_SENDING_SYN) break;  //connection request was cancelled (got error, shutdown or detach or promoted to established)
////			st->sequence_out_merge(current_outmeshtun_segment.data_sequence, 1 /*SYN occupies 1 byte len in sequence*/);
//			break;
//		}
			st->unlock_output_inprogress(this, true);
			st->on_state_update(st->UPD_WRITEREADY);
//			st->on_state_update(st->UPD_WRITTEN);
//			if(!st->get_error()) st->set_error(IOT_ERROR_NO_PEER); //to finish stream existance
			break;
		case st->ST_CLOSED: //stream can become closed while finishing writing of input ACK if final output ACK arrives
			break;
		default:
			assert(false);
	}

	st->unlock();
}

//mesh session failed
void iot_netproto_session_mesh::process_meshtun_outputaborted(iot_meshtun_stream_state *st) {
	st->lock();
	switch(st->state) {
		case st->ST_SENDING_SYN: //new SYN request was sent
		case st->ST_RESET:
		case st->ST_ESTABLISHED:
			st->unlock_output_inprogress(this, false);
			st->on_state_update(st->UPD_WRITEREADY);
//			if(!st->output_ack_pending || st->get_state()!=st->ST_SENDING_SYN ||  //connection request was cancelled (got error, shutdown or detach or promoted to established)
//				st->is_sequence_out_ACKed(current_outmeshtun_segment.data_sequence, 1 /*SYN occupies 1 byte len in sequence*/)  //SYN-ACK was received through another mesh session before end of write reported
//			) break;// just release meshtun
//			//else return temporary error to tell netcon to do quick retry
//			st->set_error(IOT_ERROR_TRY_AGAIN);
			break;
		case st->ST_CLOSED: //stream can become closed while finishing writing of input ACK if final output ACK arrives
			break;
		default: //illegal state
			assert(false);
	}
	st->unlock();
}

bool iot_netproto_session_mesh::process_meshtun_output(iot_meshtun_forwarding *st, uint16_t pathlen) {
	//here st must have refcount which accounts for current thread
	bool rval=false;
	st->lock();

	int err;
	switch(st->state) {
		case st->ST_RESET: //send back error code
		case st->ST_FORWARDING: { //send packet to next hop
			assert(!current_outmeshtun);
			if(st->meshses) { //packet is already being sent by another session
				assert(st->meshses!=this);
				break;
			}

			if(!st->from_host) { //this is originating forwarding (e.g. RESET) or return of status so ttl must be set
				uint32_t ttl=config->gwinst->peers_registry->get_num_peers();
				if(ttl>pathlen+4u) ttl=pathlen+4u;
				st->request->ttl=ttl>256 ? 255 : ttl < 1 ? 1 : uint8_t(ttl);
			} else { //this is forwarding
				if(peer_host->host_id==st->from_host || st->request->ttl<pathlen) {
outlog_debug_meshtun("CANNOT FORWARD PACKET VIA HOST %" IOT_PRIhostid ":from=%" IOT_PRIhostid ",pathlen=%d,ttl=%d", peer_host->host_id, st->from_host, int(pathlen), int(st->request->ttl));
					//this session cannot be used to forward this packet as it leads to sender of it or to too long path
					st->on_state_update(st->UPD_WRITEREADY); //retry with another session
					break;
				}
			}

outlog_debug_meshtun("%sPACKET TO HOST %" IOT_PRIhostid, st->state==st->ST_RESET ? "RETURNING STATUS ": "FORWARDING ", peer_host->host_id);

			packet_meshtun_hdr* sreq=st->request;

			authtype_t atype=AUTHTYPE_UNSET; //TODO
			uint32_t adatasz;
			if(!choose_authtype(atype, adatasz)) {
				st->set_error(IOT_ERROR_CRITICAL_ERROR);
				break;
			}

			uint32_t headlen=repack_uint16(sreq->hdrlen);
			uint32_t datalen=repack_uint32(sreq->datalen);

			assert(datalen==0 || st->request_body!=NULL);

			packet_hdr* hdr=prepare_packet_header(CMD_MESHTUN, headlen+datalen);
			packet_meshtun_hdr *req=(packet_meshtun_hdr*)(hdr+1);

			memcpy(req, sreq, headlen); //copy whole header

			//update signature of headers
			if(!fill_authdata(atype, ((char*)req)+headlen-adatasz, adatasz, (char*)hdr, sizeof(*hdr)+headlen-adatasz)) { //TODO fix. must ensure datasz here and in proxied packet are the same!
				st->set_error(IOT_ERROR_CRITICAL_ERROR);
				break;
			}

			if(datalen>0) {
				memcpy(((char*)req)+headlen, st->request_body, datalen);
			}

			current_outmeshtun=st; //will increase refcount!
			st->meshses=this;

			st->unlock();
			err=coniface->write_data(hdr, sizeof(*hdr)+headlen+datalen);
			assert(err==0);
			return true;
		}
		case st->ST_CLOSED:
			break;
		default:
			assert(false);
	}
//onexit:
	st->unlock();
	return rval;
}


void iot_netproto_session_mesh::on_commandq(void) {
	assert(uv_thread_self()==thread->thread);
	assert(state==STATE_WORKING);

	if(current_outpacket_cmd) return;
	//can send immediately, check if any service mesh-session packet must be sent
	if(check_privileged_output()) return;

	//check mesh streams which wants output
	if(has_activeroutes.load(std::memory_order_relaxed)) {
		iot_objref_ptr<iot_peer> peers[IOT_MESHTUN_MAXDISTRIBUTION];
		uint32_t delays[IOT_MESHTUN_MAXDISTRIBUTION];
		uint16_t pathlens[IOT_MESHTUN_MAXDISTRIBUTION];
		int num_peers=config->gwinst->meshcontroller->fill_peers_from_active_routes(this, peers, delays, pathlens, IOT_MESHTUN_MAXDISTRIBUTION); //will disable has_activeroutes if no active routes
		//check mesh stream queue of peers in this order
		for(int i=0;i<num_peers;i++) {
			iot_objref_ptr<iot_meshtun_packet> tunpacket;
			while((tunpacket=peers[i]->pop_meshtun(peer_host->host_id, pathlens[i]))) {
				if(tunpacket->type==TUN_STREAM) {
					if(process_meshtun_output(static_cast<iot_meshtun_stream_state*>((iot_meshtun_packet*)tunpacket), delays[i], pathlens[i])) return; //stop if some output was initiated
				} else if(tunpacket->type==TUN_FORWARDING) {
					if(process_meshtun_output(static_cast<iot_meshtun_forwarding*>((iot_meshtun_packet*)tunpacket), pathlens[i])) return; //stop if some output was initiated
				} else if(tunpacket->type==TUN_DATAGRAM) {
					assert(false); //TODO
				} else {
					assert(false); //TUN_STREAM_LISTEN cannot be here
				}
				tunpacket=NULL; //release reference as soon as possible (pop_meshtun() on next iteration requires much time)
			}
		}
	}

//	iot_meshses_command* com, *nextcom=comq.pop_all();
//	while(nextcom) {
//		com=nextcom;
//		nextcom=(nextcom->next).load(std::memory_order_relaxed);
//	}
}


