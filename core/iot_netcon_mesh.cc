#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include "iot_netcon_mesh.h"
//#include "iot_netproto_mesh.h"
#include "iot_peerconnection.h"
#include "iot_threadregistry.h"
//#include "iot_configregistry.h"
//#include "qsort.h"


void iot_netcon_mesh::do_start_uv(void) {
		assert(uv_thread_self()==thread->thread);

//		if(remote_host && !remote_peer) {
//			remote_peer=controller->gwinst->peers_registry->find_peer(remote_host);
//			if(!remote_peer) {
//				do_stop();
//				return;
//			}
//		}

		assert(phase_timer_state==HS_UNINIT);
		int err=uv_timer_init(loop, &retry_phase_timer);
		assert(err==0);
		retry_phase_timer.data=this;
		phase_timer_state=HS_INIT;

		assert(sig_watcher_state==HS_UNINIT);
		uv_async_init(loop, &sig_watcher, [](uv_async_t* handle) -> void {
			iot_netcon_mesh* obj=(iot_netcon_mesh*)(handle->data);
			obj->on_signal();
		});
		sig_watcher.data=this;
		sig_watcher_state=HS_INIT;

		if(is_passive) {
			if(phase==PHASE_CONNECTED) {
				process_common_phase();
			}
			else if(phase==PHASE_LISTENING || phase==PHASE_INITIAL) process_server_phase();
			else {
				assert(false);
				do_stop();
				return;
			}
		} else {
			assert(phase==PHASE_INITIAL);
			connect_errors=0;
			if(peer_closed_restart) {
				peer_closed_restart=false;
				retry_delay=2000;
				retry_phase();
				return;
			}
			process_client_phase();
		}
	}

int iot_netcon_mesh::do_stop(void) {
		assert(!is_started() || uv_thread_self()==thread->thread);

		int num_closing=0;

		if(session_state>HS_UNINIT) {
			assert(protosession!=NULL);

			assert(phase==PHASE_RUNNING);
			if(session_state>=HS_INIT) {
				session_state=HS_CLOSING;
				num_closing++;
				if(!protosession->stop(false)) { //session won't call on_sesion_closed()
					num_closing--;
					session_state=HS_UNINIT;
				}
			} else num_closing++;
		}

		if(connection_state && !connection_state->is_closed()) {
			if(connection_state->detach_netcon(true)) { //cannot be closed immediately, so detach from state
				connection_state=NULL;
			}
//			controller->meshtun_close(this); //can finish work and nullify connection_state or act async
//			if(connection_state) num_closing++; //close not finished
		}

		if(phase_timer_state>=HS_INIT) {
			phase_timer_state=HS_CLOSING;
			num_closing++;
			uv_close((uv_handle_t*)&retry_phase_timer, [](uv_handle_t* handle) -> void {
				iot_netcon_mesh* obj=(iot_netcon_mesh*)(handle->data);
				assert(obj->phase_timer_state==HS_CLOSING);
				obj->phase_timer_state=HS_UNINIT;
				obj->do_stop();
			});
		} else if(phase_timer_state==HS_CLOSING) num_closing++;

		if(connect_timer_state>=HS_INIT) {
			connect_timer_state=HS_CLOSING;
			num_closing++;
			uv_close((uv_handle_t*)&connect_timer, [](uv_handle_t* handle) -> void {
				iot_netcon_mesh* obj=(iot_netcon_mesh*)(handle->data);
				assert(obj->connect_timer_state==HS_CLOSING);
				obj->connect_timer_state=HS_UNINIT;
				obj->do_stop();
			});
		} else if(connect_timer_state==HS_CLOSING) num_closing++;


		if(sig_watcher_state>=HS_INIT) {
			sig_watcher_state=HS_CLOSING;
			num_closing++;
			uv_close((uv_handle_t*)&sig_watcher, [](uv_handle_t* handle) -> void {
				iot_netcon_mesh* obj=(iot_netcon_mesh*)(handle->data);
				assert(obj->sig_watcher_state==HS_CLOSING);
				obj->sig_watcher_state=HS_UNINIT;
				obj->do_stop();
			});
		} else if(sig_watcher_state==HS_CLOSING) num_closing++;


		if(num_closing>0) return IOT_ERROR_NOT_READY;

		if(write_req_data) {
			if(write_req_data!=write_req_buffer) iot_release_memblock(write_req_data);
			write_req_data=NULL;
		}

		phase=PHASE_INITIAL;
		return on_stopped();
	}

void iot_netcon_mesh::on_connect_status(int err, bool from_instantiate) { //from_instantiate means that actually error occured during session instantiation, to skip some checks and messages
		if(!err) { //successful connection
			assert(!from_instantiate); //success of instantiation cannot go here
			connect_errors=0;
			num_quick_retries=0;
			phase=PHASE_CONNECTED;
			process_common_phase();
			return;
		}
		if(err==IOT_ERROR_NO_ROUTE) { //allow quick retry when route appears
			waiting_route=true;
			if(phase==PHASE_CONNECTING) { //check that route really appeared
				iot_objref_ptr<iot_peer> peer=controller->gwinst->peers_registry->find_peer(remote_host);
				if(peer && peer->check_is_reachable()) {
					waiting_route=false;
					err=IOT_ERROR_TRY_AGAIN; //do quick retry as route has restored after signaling this netcon
				}
			}
		}

		if(connection_state->detach_netcon(true)) { //cannot be closed immediately, so detach from state
			connection_state=NULL;
		}

		if(from_instantiate) {
			phase=PHASE_CONNECTING;
			goto doretry;
		}

		//there is connection error
		if(err==IOT_ERROR_TRY_AGAIN) {
			if(num_quick_retries<10) {
				retry_delay=100;
				num_quick_retries++;
				outlog_notice("Got TRY_AGAIN creating mesh tunnel to host '%" IOT_PRIhostid "', retrying in %u msecs", remote_host, retry_delay);
				retry_phase();
				return;
			}
		}
doretry:
		retry_delay=calc_reconnect_delay();
		connect_errors++;
		num_quick_retries=0;
		if(connect_maxretries>0 && connect_errors>=connect_maxretries) {
			on_reconnects_exceeded();
			return;
		}
		if(!from_instantiate) outlog_notice("Error creating mesh tunnel to host '%" IOT_PRIhostid "': %s, retrying in %u msecs", remote_host, kapi_strerror(err), retry_delay);
		retry_phase();
	}


char* iot_netcon_mesh::sprint(char* buf, size_t bufsize, int* doff) const {
		if(!bufsize) return buf;

		int len;
		if(is_passive)
			len=snprintf(buf, bufsize, "{%s %s server:local port=%d, remote host=%" IOT_PRIhostid ", remote port=%d}",meta->type_name, remote_host==0 ? "LISTENING" : "ACCEPTED", int(local_port), remote_host, int(remote_port));
			else
			len=snprintf(buf, bufsize, "{%s client:remote host=%" IOT_PRIhostid ", remote port=%d}",meta->type_name, remote_host, int(remote_port));
		if(len>=int(bufsize)) len=int(bufsize)-1;
		if(doff) *doff+=len;
		return buf;
	}


void iot_netcon_mesh::on_signal(void) { //is called to notify about connection readability/writability/incoming connection/closing by peer
		assert(uv_thread_self()==thread->thread);
		int err;
		uint8_t events=event_mask.exchange(0, std::memory_order_relaxed);
outlog_debug_meshtun("got signal with eventmask=%u", unsigned(events));

		if(!connection_state || connection_state->is_closed()) { //meshtun can be already detached if detach occured after signal was sent. or this is notification about new route
			//process events which can occur with closed meshtun
			if((events & EVENT_GOTROUTE) && waiting_route && remote_host && phase==PHASE_CONNECTING) { //check that route really appeared
				iot_objref_ptr<iot_peer> peer=controller->gwinst->peers_registry->find_peer(remote_host);
				if(peer && peer->check_is_reachable()) {
					process_client_phase();
					return;
				}
			}
			if(!connection_state) return;
		}

		if(connection_state->type==TUN_STREAM) {
			iot_meshtun_stream_state *st=static_cast<iot_meshtun_stream_state*>((iot_meshtun_state*)connection_state);
			err=st->get_error();
			
			if(phase==PHASE_CONNECTING) {
outlog_debug_meshtun("CONNECTING err=%d", err);
				if(err) on_connect_status(err);
				else if((events & EVENT_WRITABLE) && st->is_writable()) on_connect_status(0);
				return;
			}
			if(phase==PHASE_CONNECTED || phase==PHASE_RUNNING) {
				if(err) {
					stop_data_read();
					peer_closed_restart=true;
					do_stop();
					return;
				}
				if(events & EVENT_NEEDCONNCHECK) {
					st->keepalive_check(uv_now(loop));
				}
				if(read_enabled && (events & EVENT_READABLE)) { //try to read
					read_data_int();
				}
				if((events & EVENT_WRITABLE)) {
					on_writeready();
				}
			}

		} else if(connection_state->type==TUN_STREAM_LISTEN) {
			assert(phase==PHASE_LISTENING);
//			iot_meshtun_stream_listen_state *st=static_cast<iot_meshtun_stream_listen_state*>((iot_meshtun_state*)connection_state);
			err=connection_state->get_error();
			if(err) {
				do_stop();
				return;
			}
			if(events & EVENT_READABLE)
				do {
					err=accept_server(this);
					if(err) {
						if(err==IOT_ERROR_CRITICAL_ERROR) { //critical erro on listening meshtun
							do_stop();
							return;
						}
						if(err!=IOT_ERROR_TRY_AGAIN) outlog_notice("Error accepting new meshtun connection: %s", kapi_strerror(err));
					}
				} while(!err);
		} else if(connection_state->type==TUN_DATAGRAM) {
			assert(false); //TODO
		} else { //forwarding cannot be here
			assert(false);
		}
	}

