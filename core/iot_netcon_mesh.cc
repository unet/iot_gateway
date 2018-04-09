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
//			remote_peer=static_cast<iot_meshnet_controller*>(registry)->gwinst->peers_registry->find_peer(remote_host);
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

			assert(connect_timer_state==HS_UNINIT);
			err=uv_timer_init(loop, &connect_timer);
			assert(err==0);
			connect_timer.data=this;
			connect_timer_state=HS_INIT;

			assert(phase==PHASE_INITIAL);
			if(peer_closed_restart) {
				connect_errors=1;
				peer_closed_restart=false;
			} else {
				connect_errors=0;
			}
			setup_connect_timer();
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
				if(!protosession->stop(true)) { //session won't call on_sesion_closed()
					num_closing--;
					session_state=HS_UNINIT;
				}
			} else num_closing++;
		}

		if(connection_state && !connection_state->is_closed()) {
			if(connection_state->detach_netcon(true)) { //cannot be closed immediately, so detach from state
				connection_state=NULL;
			}
//			static_cast<iot_meshnet_controller*>(registry)->meshtun_close(this); //can finish work and nullify connection_state or act async
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


char* iot_netcon_mesh::sprint(char* buf, size_t bufsize, int* doff) const {
		if(!bufsize) return buf;

		int len;
		if(is_passive)
			len=snprintf(buf, bufsize, "{%s server:local port=%d, remote host=%" IOT_PRIhostid ", remote port=%d}",meta->type_name, int(local_port), remote_host, int(remote_port));
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
outlog_notice("got signal with eventmask=%u", unsigned(events));

		if(!connection_state || connection_state->is_closed()) { //meshtun can be already detached if detach occured after signal was sent. or this is notification about new route
			//process events which can occur with closed meshtun
			if((events & EVENT_GOTROUTE) && waiting_route && remote_host && phase==PHASE_CONNECTING) { //check that route really appeared
				iot_objref_ptr<iot_peer> peer=static_cast<iot_meshnet_controller*>(registry)->gwinst->peers_registry->find_peer(remote_host);
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
outlog_notice("CONNECTING err=%d", err);
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
					if(err==IOT_ERROR_CRITICAL_ERROR) { //critical erro on listening meshtun
						do_stop();
						return;
					}
				} while(!err);
		} else if(connection_state->type==TUN_DATAGRAM) {
			assert(false); //TODO
		} else { //forwarding cannot be here
			assert(false);
		}
	}

