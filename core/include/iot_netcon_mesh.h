#ifndef IOT_NETCON_MESH_H
#define IOT_NETCON_MESH_H

#include<stdint.h>
#include<assert.h>

#include "iot_core.h"
#include "iot_netcon.h"

#include "iot_netproto_mesh.h"

#include "iot_mesh_control.h"


class iot_netcon_mesh;

//realizes netcon for connections, tunnelled through mesh net


class iot_netcontype_metaclass_mesh: public iot_netcontype_metaclass {
	iot_netcontype_metaclass_mesh(void) : iot_netcontype_metaclass("mesh", 0, true, true, false) {
	}

public:
	static iot_netcontype_metaclass_mesh object;

	virtual void destroy_netcon(iot_netcon* obj) const { //must destroy netcon object in correct way (metaclass knows how its netcon objects are created)
		obj->~iot_netcon();
		iot_release_memblock(obj);
	}
	static iot_netcon_mesh* allocate_netcon(iot_netproto_config* protoconfig);

private:
	virtual int p_from_json(json_object* json, iot_netproto_config* config, iot_netcon*& obj, bool is_server, iot_netconregistryiface* registry, uint32_t metric) const override {
		return IOT_ERROR_INVALID_ARGS;
	}
};


class iot_netcon_mesh : public iot_netcon {
	friend class iot_meshnet_controller;
	friend class iot_netproto_session_mesh;
public:
	enum event_t : uint8_t {
		EVENT_READABLE=1, //connection is ready for reading (got incoming data or graceful close)
		EVENT_WRITABLE=2,   //connection is ready for writing (connection succeeded or write buffer got space)
		EVENT_ERROR=4,      //connection got error state (connection failed or broken)
		EVENT_GOTROUTE=8    //mesh network got first route to remote host
	};
private:
	iot_hostid_t remote_host=0; //remote host (for connected passive, 0 for listening) or destination host (for non-passive)
//	iot_objref_ptr<iot_peer> remote_peer;

	uint16_t local_port=0; //local port to listen to (for passive) or to bind to (for non-passive) if auto_local_port is false
	uint16_t remote_port=0; //remote destination port (for non-passive). 0 for passive
//	iot_meshconnmapkey_t connection_key=0; //keeps actual key in connmap of protocol
	iot_objref_ptr<iot_meshtun_state> connection_state; //is fully controlled by meshnet controller

	uv_timer_t retry_phase_timer;
	uv_timer_t connect_timer;
	uv_async_t sig_watcher;
	uint64_t retry_delay=0;

	char* readbuf=NULL;
	size_t readbufsize=0;

	iovec write_req_buffer[10]; //preallocated space for iovec's to avoid dynamic allocation
	iovec *write_req_data=NULL; //pointer to array of write_req_numvec iovec structures, NULL if no current write request.
								//points either to write_req_buffer (if space is enough) or to malloced buffer
	int write_req_numvec=0;
	int write_req_curidx=0; //index in write_req_data array showing current position to be written to meshtun

	uint32_t num_quick_retries=0; //in some cases meshtun functions can return IOT_ERROR_TRY_AGAIN requesting immediate retry. protect from possible bugs and limit number of such consecutive retries
	uint32_t connect_errors=0;  //keeps number of connect or sessions instantiation errors since start or successful connection/instantiation. is incremented AFTER evaluating DELAY
	uint32_t connect_maxretries=0;//maximum number of connect or sessions instantiation errors since start or successful connection/instantiation. 0 means indefinite. netcon will destroy itself after reaching limit.
	uint32_t connect_timeout=0; //timeout (in milliseconds) from start or disconnect till successful connect and session instantiation. 0 means indefinite. netcon will destroy itself after reaching limit.
								//during this time reconnects or reinstantiation will be attempted with no more than connect_maxretries times and retry delay
								//according to evaluation (connect_errors is incremented AFTER evaluating DELAY):
								//	DELAY=connect_retry_delay + connect_retry_mult*(connect_errors>connect_retry_limitmult ? connect_retry_limitmult : connect_errors);
								//  connect_errors++;
	uint32_t connect_retry_delay=30*1000;	//used in DELAY evaluation above. in milliseconds. is minimal or constant (if connect_retry_mult==0) delay between reconnects/reinstantiations
	uint32_t connect_retry_mult=3*60*1000;//used in DELAY evaluation above. is step of DELAY increasing in milliseconds
	uint32_t connect_retry_limitmult=10;//used in DELAY evaluation above. limits multiplier for connect_retry_mult

	bool auto_local_port=false; //flag that local port must be found automatically (for non-passive).
	bool read_enabled=false; //reading is enabled, readbuf and readbuf size show where data must be written
	bool waiting_route=false; //becomes true if we got NO_ROUTE error and now wait for route to restore
	bool peer_closed_restart=false; //becomes true if established connection is closed by peer (meaningful for restarts of client connections)

	enum : uint8_t {
		PHASE_UNINITED,		//server or client
		PHASE_INITIAL,		//server or client
		PHASE_LISTENING,	//server (final phase for accepting connection)
		PHASE_CONNECTING,	//client
		PHASE_CONNECTED,	//common
		PHASE_RUNNING		//common
	} phase=PHASE_UNINITED; //phase is used inside worker thread
	h_state_t phase_timer_state=HS_UNINIT;
	h_state_t connect_timer_state=HS_UNINIT;
	h_state_t sig_watcher_state=HS_UNINIT;

	h_state_t session_state=HS_UNINIT;

	volatile std::atomic<uint8_t> event_mask={0};


public:
	iot_netcon_mesh(iot_netproto_config* protoconfig) : iot_netcon(&iot_netcontype_metaclass_mesh::object, protoconfig) {
	}
	~iot_netcon_mesh(void) {
		if(write_req_data) {
			if(write_req_data!=write_req_buffer) iot_release_memblock(write_req_data);
			write_req_data=NULL;
		}
		if(connection_state) connection_state->detach_netcon(); //must be done to clear internal reference to this netcon inside state and to free the state which could be preserved in on_stop()
		connection_state=NULL;
	}

	void set_reconnection_policy(uint32_t connect_maxretries_, uint32_t connect_timeout_, uint32_t connect_retry_delay_, uint32_t connect_retry_mult_=0, uint32_t connect_retry_limitmult_=0) {
		connect_maxretries=connect_maxretries_;
		if(connect_timeout!=connect_timeout_) {
			connect_timeout=connect_timeout_;
			if(connect_timer_state==HS_ACTIVE) {
				if(!connect_timeout_) uv_timer_stop(&connect_timer); //time is active but should not with new settings. stop it
				else setup_connect_timer();
			}
		}
		connect_retry_delay=connect_retry_delay_;
		connect_retry_mult=connect_retry_mult_;
		connect_retry_limitmult=connect_retry_limitmult_;
	}

	static int accept_server(const iot_netcon_mesh* srv) { //creates server instance in connected state (accepts incoming connection)
		if(!srv || !srv->is_passive || !srv->is_stream || srv->phase!=srv->PHASE_LISTENING) return IOT_ERROR_INVALID_ARGS;

		iot_meshnet_controller* controller=static_cast<iot_meshnet_controller*>(srv->registry);

		iot_netcon_mesh* rep=iot_netcontype_metaclass_mesh::allocate_netcon((iot_netproto_config*)srv->protoconfig);
		if(!rep) return IOT_ERROR_NO_MEMORY;

		iot_meshtun_stream_state* st;
		int err=controller->meshtun_create(rep, srv->allocator, TUN_STREAM, true);
		if(err) goto onexit;

		//now try to accept connection
		st=static_cast<iot_meshtun_stream_state*>((iot_meshtun_state*)rep->connection_state);

		err=static_cast<iot_meshtun_stream_listen_state*>((iot_meshtun_state*)srv->connection_state)->accept(st);
		if(err) goto onexit;
		//meshtun is active and able to receive data just after accepting!

		err=rep->init_server(controller, 0, srv->is_stream, true);
		if(err) goto onexit;

		rep->phase=PHASE_CONNECTED;

		if(srv->thread->is_overloaded(rep->get_cpu_loading()))
			err=rep->start_uv(NULL, false, true);
		else
			err=rep->start_uv(srv->thread, false, true);
		if(err==IOT_ERROR_NOT_READY) err=0; //to skip unmark_initing() call below as this is success

onexit:
		if(err) {
			if(!rep->unmark_initing(true)) { //init was not done
				if(rep->connection_state) {
					rep->connection_state->detach_netcon();
					rep->connection_state=NULL;
				}
				rep->meta->destroy_netcon(rep);
			}
		}
		return err;
	}


	int init_server(iot_meshnet_controller* controller, uint16_t port_, bool force_stream=false, bool noinit=false, iot_hostid_t remote_host_=0, uint16_t remote_port_=0) { //true noinit to leave object in INITING state on success
		//for listening socket remote_port_ and remote_host_ should be zero
		if(!controller) return IOT_ERROR_INVALID_ARGS;
		const iot_netprototype_metaclass* protometa=protoconfig->get_metaclass();
		if(!protometa->can_meshtun || !protometa->type_id) return IOT_ERROR_NOT_SUPPORTED;
		if(port_>protometa->meshtun_maxport || (remote_host_!=0 && remote_port_>protometa->meshtun_maxport)) return IOT_ERROR_INVALID_ARGS;

		if(!trymark_initing()) {
			//must not fail if init is done in one thread just after construction
			assert(false);
			return IOT_ERROR_CRITICAL_BUG;
		}
		//now state is INITING and only current thread will do it
		assert(phase==PHASE_UNINITED);

		is_passive=true;
		is_stream=force_stream ? force_stream : protometa->meshtun_isstream;
		remote_host=remote_host_;
		remote_port=remote_port_; //is not meaningful when remote_host is 0
		local_port=port_;
		auto_local_port=false;

		phase=PHASE_INITIAL;
		int err=assign_registryiface(controller);
		if(err) goto onerror;

		if(!noinit) mark_inited();
		return 0;
onerror:
		unmark_initing();
		return err;
	}

	int init_client(iot_meshnet_controller* controller, iot_hostid_t host_, uint16_t port_, bool force_stream=false, bool auto_local_port_=true, uint16_t localport_=0) {
		//force_stream can true for non-streamable protocols to force strict packet ordering. for streamable protocols does not matter
		if(!controller) return IOT_ERROR_INVALID_ARGS;
		const iot_netprototype_metaclass* protometa=protoconfig->get_metaclass();
		if(!protometa->can_meshtun || !protometa->type_id) return IOT_ERROR_NOT_SUPPORTED;
		if(!host_ || port_>protometa->meshtun_maxport || (!auto_local_port_ && localport_>protometa->meshtun_maxport)) return IOT_ERROR_INVALID_ARGS;

		if(!trymark_initing()) {
			//must not fail if init is done in one thread
			assert(false);
			return IOT_ERROR_CRITICAL_BUG;
		}
		//now state is INITED and only current thread will do it
		assert(phase==PHASE_UNINITED);

		is_passive=false;
		is_stream=force_stream ? force_stream : protometa->meshtun_isstream;
		remote_host=host_;
		remote_port=port_;
		local_port=auto_local_port_ ? 0 : localport_;
		auto_local_port=auto_local_port_;

		phase=PHASE_INITIAL;
		int err=assign_registryiface(controller);
		if(err) goto onerror;

		mark_inited();

		return 0;
onerror:
		unmark_initing();
		return err;
	}
	void on_meshtun_event(uint8_t events) { //necessary to notify netcon about change in connection or routing state. events is ORed mask of event bits from event_t
		//any thread
		event_mask.fetch_or(events, std::memory_order_relaxed); //remember event for even non-started netcon (possible after accept)
		if(sig_watcher_state<HS_INIT) return;
		uv_async_send(&sig_watcher);
	}

private:
	uint32_t calc_reconnect_delay(void) {
		uint64_t delay=connect_retry_delay + uint64_t(connect_retry_mult)*(connect_errors>connect_retry_limitmult ? connect_retry_limitmult : connect_errors);
		if(delay>0x7FFFFFFF) return 0x7FFFFFFF;
		return uint32_t(delay);
	}

	virtual uint8_t p_get_cpu_loading(void) override { //must return pure cpu loading of netcon layer implementation
		return 3; //test value to force separate thread
	}
	virtual void do_start_uv(void) override;
	virtual int do_stop(void) override;
	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const override;

	void retry_phase(void) { //schedules call of process_[client|server] phase() after delay ms in retry_delay var
		assert(uv_thread_self()==thread->thread);
		phase_timer_state=HS_ACTIVE;
		uv_timer_start(&retry_phase_timer, [](uv_timer_t* h) -> void {
			iot_netcon_mesh* obj=(iot_netcon_mesh*)(h->data);

			if(obj->phase_timer_state==HS_ACTIVE) {
				obj->phase_timer_state=HS_INIT;
				if(!obj->is_passive)
					obj->process_client_phase();
				else
					obj->process_server_phase();
			}
		}, retry_delay, 0);
	}
	void cancel_retry_phase(void) {
		assert(uv_thread_self()==thread->thread);
		if(phase_timer_state!=HS_ACTIVE) return;
		phase_timer_state=HS_INIT;
		uv_timer_stop(&retry_phase_timer);
	}

	void setup_connect_timer(void) {
		assert(uv_thread_self()==thread->thread);
		if(!connect_timeout) return;

		connect_timer_state=HS_ACTIVE;
		uv_timer_start(&connect_timer, [](uv_timer_t* h) -> void {
			iot_netcon_mesh* obj=(iot_netcon_mesh*)(h->data);
			obj->connect_timer_state=HS_INIT;
			if(!obj->connect_timeout) return;
			outlog_notice("Connect timeout for host '%" IOT_PRIhostid "' expired", obj->remote_host);
			obj->destroy();
		}, connect_timeout, 0);
	}

	void on_reconnects_exceeded(void) {
		outlog_notice("Reconnection attempts for host '%" IOT_PRIhostid "' exceeded", remote_host);
		destroy();
	}
	void process_client_phase(void) {
		assert(uv_thread_self()==thread->thread);
		int err;
		iot_meshnet_controller* controller=static_cast<iot_meshnet_controller*>(registry);
		cancel_retry_phase();
//again:
		switch(phase) {
			case PHASE_INITIAL:
				phase=PHASE_CONNECTING;
			case PHASE_CONNECTING:
outlog_notice("meshtun in connecting phase");
				waiting_route=false;
				if(is_stream) {
					if(!connection_state) err=controller->meshtun_create(this, allocator, TUN_STREAM, is_passive);
					else {
						assert(connection_state->is_closed()); //just closed?
						err=0;
						event_mask.store(0, std::memory_order_relaxed);
					}
					if(!err) {
						protoconfig->meshtun_set_metadata((iot_meshtun_state*)connection_state);
						auto st=static_cast<iot_meshtun_stream_state*>((iot_meshtun_state*)connection_state);
						err=st->connect(protoconfig->get_metaclass(), remote_host, remote_port, auto_local_port, local_port);
//						remote_peer=st->get_peer(); //will contain correct peer even on error. NULL is possible if remote_host is incorrect
					}
				} else { //datagram
					if(!connection_state) err=controller->meshtun_create(this, allocator, TUN_DATAGRAM);
						else err=0;
					if(!err) {
						assert(false); //TODO
//						protoconfig->meshtun_set_metadata((iot_meshtun_state*)connection_state);
//						auto st=static_cast<iot_meshtun_stream_state*>((iot_meshtun_state*)connection_state);
//						err=st->connect(protoconfig->get_metaclass(), remote_host, remote_port, auto_local_port, local_port);
//						remote_peer=st->remote_peer;
					}
				}
				if(err) on_connect_status(err);
				//on_connect_status() will be called if 0 is returned
				break;
			default:
				outlog_error("%s() called for illegal phase %d for host '%" IOT_PRIhostid "', aborting", __func__, int(phase), remote_host);
				do_stop();
				break;
		}
		return;
	}
	void process_server_phase(void) {
		assert(uv_thread_self()==thread->thread);
		int err;
		iot_meshnet_controller* controller=static_cast<iot_meshnet_controller*>(registry);
//again:
		switch(phase) {
			case PHASE_INITIAL:
				phase=PHASE_LISTENING;
			case PHASE_LISTENING: {
outlog_notice("meshtun in listening phase");
				if(is_stream) {
					if(!connection_state) err=controller->meshtun_create(this, allocator, TUN_STREAM_LISTEN);
					else {
						assert(connection_state->is_closed()); //just closed?
						err=0;
						event_mask.store(0, std::memory_order_relaxed);
					}
					if(!err) {
						auto st=static_cast<iot_meshtun_stream_listen_state*>((iot_meshtun_state*)connection_state);
						err=st->listen(protoconfig->get_metaclass(), local_port);
					}
				} else { //datagram
					if(!connection_state) err=controller->meshtun_create(this, allocator, TUN_DATAGRAM);
						else err=0;
					if(!err) {
						assert(false); //TODO
//						auto st=static_cast<iot_meshtun_datagram_state*>((iot_meshtun_state*)connection_state);
//						err=st->listen(protoconfig->get_metaclass(), local_port);
					}
				}

				if(err) {
					retry_delay=calc_reconnect_delay();
					connect_errors++;
//					num_quick_retries=0;
					outlog_notice("Error creating listening mesh tunnel: %s, retrying in %u msecs", kapi_strerror(err), retry_delay);
					retry_phase();
					return;
				}
				outlog_notice("Now listening for incoming mesh tunnel connections on port %u", unsigned(local_port));
				connect_errors=0;
				return;
			}
			default:
				outlog_error("%s() called for illegal phase %d, aborting", __func__, int(phase));
				do_stop();
				return;
		}
	}

	void process_common_phase(void) {
		assert(uv_thread_self()==thread->thread);
		int err;
		switch(phase) {
			case PHASE_CONNECTED:
outlog_notice("meshtun in connected phase");
				assert(protosession==NULL);

				iot_hostid_t peer_id;
				if(is_stream) {
					auto st=static_cast<iot_meshtun_stream_state*>((iot_meshtun_state*)connection_state);
					peer_id=st->get_peer_id();
				} else {
					peer_id=0; //TODO for datagrams
				}

				err=protoconfig->instantiate(this, peer_id); //meshtun metadata can be accessed inside instantiate
				if(err) {
					outlog_error("Cannot initialize protocol '%s': %s", protoconfig->get_typename(), kapi_strerror(err));
					if(is_passive) do_stop();
						else on_connect_status(err, true);
					return;
				}
				assert(protosession!=NULL);

				phase=PHASE_RUNNING;
				session_state=HS_ACTIVE;
				err=protosession->start();
				if(err) {
					outlog_error("Cannot start session of protocol '%s': %s", protoconfig->get_typename(), kapi_strerror(err));
					do_stop();
					return;
				} else {
					if(event_mask.load(std::memory_order_relaxed)) on_signal();
				}
			case PHASE_RUNNING:
				return;
			default:
				outlog_error("%s() called for illegal phase %d, aborting", __func__, int(phase));
				do_stop();
				return;
		}
	}
	void on_connect_status(int err, bool from_instantiate=false) { //from_instantiate means that actually error occured during session instantiation, to skip some checks and messages
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

	void on_signal(void); //is called to notify about connection readability/writability/incoming connection/closing by peer

	void stop_data_read(void) {
		if(phase!=PHASE_RUNNING) return;
//		if(h_tcp_state==HS_ACTIVE) {
//			int err=uv_read_stop((uv_stream_t*)&h_tcp);
//			assert(err==0);
//		}
		read_enabled=false;
//		if(connection_state) connection_state->input_wanted=0;
		readbuf=NULL;
		readbufsize=0;
	}


//iot_netconiface methods:
	//enable reading into specified data buffer or reconfigure previous buffer. NULL databuf and zero datalen disable reading.
	//0 - reading successfully set. iot_netproto_session::on_read_data_status() will be called when data is available.
	//1 - reading successfully set and ready, iot_netproto_session::on_read_data_status() was already called before return from read_data()
	//IOT_ERROR_INVALID_ARGS - invalid arguments (databuf is NULL or datalen==0 but not simultaneously)
	virtual int read_data(void *databuf, size_t datalen) override {
		if(phase!=PHASE_RUNNING || !connection_state) return IOT_ERROR_NOT_READY;
		if(!databuf) {
			if(!datalen) { //disable reading
				if(read_enabled) stop_data_read();
				return 0;
			}
			return IOT_ERROR_INVALID_ARGS;
		}
		if(!datalen) return IOT_ERROR_INVALID_ARGS;

		readbuf=(char*)databuf;
		readbufsize=datalen;

		if(read_enabled) return 0;
		read_enabled=true;

		return read_data_int();
	}
	int read_data_int(void) {
		if(is_stream) {
			auto st=static_cast<iot_meshtun_stream_state*>((iot_meshtun_state*)connection_state);
			ssize_t res=st->do_read(readbuf, readbufsize);
			if(res<0) {
				if(res==IOT_ERROR_TRY_AGAIN) { //no data to read, wait for readready signal
					return 0;
				}
				do_stop();
				return 0;
			}
			if(!res) { //EOF
				stop_data_read();
				peer_closed_restart=true;
				do_stop();
				return 0;
			}
			if(!protosession->on_read_data_status(res, readbuf, readbufsize)) stop_data_read();
			return 1;
		} else {
			assert(false); //TODO
		}
		return 0;
	}


/*	void on_read(ssize_t nread, const uv_buf_t* buf) {
		assert(protosession!=NULL);
		if(nread==0) return;

		if(nread==UV_EOF || nread<0) { //connection closed by other side or some error
			h_tcp_state=HS_INIT;
			stop_data_read();
			peer_closed_restart=true;
//			detach_session();
//			session_state=HS_UNINIT;
			do_stop();
//			protosession->on_read_data_status(0, readbuf, readbufsize);
			return;
		}
		if(!protosession->on_read_data_status(nread, readbuf, readbufsize)) stop_data_read();
	}
*/

	//check if new write request can be added. must return:
	//1 - write_data() can be called with request data
	//0 - request writing is in progress, so iot_netproto_session::on_write_data_status() must be waited (no need to call can_write_data() again)
	//IOT_ERROR_NOT_READY - no request being written but netcon object is not ready to write request. iot_netproto_session::on_can_write_data() will be called when netcon is ready
	virtual int can_write_data(void) override {
		if(phase!=PHASE_RUNNING || !connection_state) return IOT_ERROR_NOT_READY;
		if(write_req_data) return 0;
		return 1;
	}

	//try to add new write request. must return:
	//0 - request successfully added. iot_netproto_session::on_write_data_status() will be called later after completion
	//1 - request successfully added and ready, iot_netproto_session::on_write_data_status() was already called before return from write_data()
	//IOT_ERROR_TRY_AGAIN - another request writing is in progress, so iot_netproto_session::on_write_data_status() must be waited
	//IOT_ERROR_NOT_READY - no request being written but netcon object is not ready to write request. iot_netproto_session::on_can_write_data() will be called when netcon is ready
	//IOT_ERROR_INVALID_ARGS - invalid arguments (databuf is NULL or datalen==0)
	virtual int write_data(void *databuf, size_t datalen) override {
		if(!databuf || !datalen) return IOT_ERROR_INVALID_ARGS;
		iovec vec;
		vec.iov_base=databuf;
		vec.iov_len=datalen;

		return iot_netcon_mesh::write_data(&vec, 1);
	}

	virtual int write_data(iovec *databufvec, int veclen) override {
		if(!databufvec || veclen<=0) return IOT_ERROR_INVALID_ARGS;
		if(phase!=PHASE_RUNNING || !connection_state) return IOT_ERROR_NOT_READY;
		if(write_req_data) return IOT_ERROR_TRY_AGAIN; //there is writing request in progress or no write buf space

		if(veclen<=int(sizeof(write_req_buffer)/sizeof(write_req_buffer[0]))) { //space of prealloc buffer is enough
			write_req_data=write_req_buffer;
		} else {
			write_req_data=(iovec *)allocator->allocate(veclen*sizeof(iovec), true);
			if(!write_req_data) return IOT_ERROR_NO_MEMORY;
		}

		int vecused; //number of fully used vectors will be returned here
		size_t offsetused; //when vecused < veclen, number of used bytes of databufvec[vecused] will be returned here

		if(is_stream) {
			auto st=static_cast<iot_meshtun_stream_state*>((iot_meshtun_state*)connection_state);
			ssize_t res=st->do_write(databufvec, veclen, &vecused, &offsetused);
			if(res<0) {
				if(res==IOT_ERROR_TRY_AGAIN) { //no write buf space, wait for writeready signal
					memcpy(write_req_data, databufvec, veclen*sizeof(iovec));
					write_req_curidx=0;
					write_req_numvec=veclen;
					return 0;
				}
				do_stop();
				return 0;
			}
			if(vecused==veclen) {
				if(write_req_data!=write_req_buffer) iot_release_memblock(write_req_data);
				write_req_data=NULL;
//				protosession->on_write_data_status(0);
//				if(!write_req_data) { //no additional write_data() called in on_write_data_status()
//					protosession->on_can_write_data();
//				}
//				return 0;
				return 1; //write completed in full and in sync
			}
			if(write_req_data!=write_req_buffer && veclen-vecused<=int(sizeof(write_req_buffer)/sizeof(write_req_buffer[0]))) { //space of prealloc buffer is enough NOW but was not enough. use static buffer
				iot_release_memblock(write_req_data);
				write_req_data=write_req_buffer;
			}

			memcpy(write_req_data, databufvec+vecused, (veclen-vecused)*sizeof(iovec));
			write_req_curidx=0;
			write_req_numvec=veclen-vecused;
			if(offsetused>0) { //first iovec was partially written
				write_req_data[0].iov_base=((char*)write_req_data[0].iov_base) + offsetused;
				assert(write_req_data[0].iov_len>offsetused);
				write_req_data[0].iov_len-=offsetused;
			}
			//here write request is not finished and on_write_data_status() will be called later
		} else {
			assert(false); //TODO for datagram
		}
		return 0;
	}

/*	void on_write_status(int status) {
		assert(protosession!=NULL);
		h_writereq_valid=false;
		if(!status) {
			protosession->on_write_data_status(0);
			if(!h_writereq_valid) { //no additional write_data() called in on_write_data_status()
				protosession->on_can_write_data();
			}
			return;
		}
		do_stop();
		//todo
	}
*/

	void on_writeready(void) {
		if(!write_req_data) return;
		//there is unfinished write request. try to continue it
		assert(write_req_numvec>0 && write_req_curidx<write_req_numvec);
		int vecused; //number of fully used vectors will be returned here
		size_t offsetused; //when vecused < veclen, number of used bytes of databufvec[vecused] will be returned here

		if(is_stream) {
			auto st=static_cast<iot_meshtun_stream_state*>((iot_meshtun_state*)connection_state);
			ssize_t res=st->do_write(write_req_data+write_req_curidx, write_req_numvec-write_req_curidx, &vecused, &offsetused);
			if(res<0) {
				if(res!=IOT_ERROR_TRY_AGAIN) do_stop();
				//no write buf space, wait for writeready signal
				return;
			}
			write_req_curidx+=vecused;
			if(write_req_curidx==write_req_numvec) { //write completed in full
				if(write_req_data!=write_req_buffer) iot_release_memblock(write_req_data);
				write_req_data=NULL;
				protosession->on_write_data_status(0);
//				if(!write_req_data) { //no additional write_data() called in on_write_data_status()
//					protosession->on_can_write_data();
//				}
				return;
			}
			if(offsetused>0) { //first iovec was partially written
				write_req_data[write_req_curidx].iov_base=((char*)write_req_data[write_req_curidx].iov_base) + offsetused;
				assert(write_req_data[write_req_curidx].iov_len>offsetused);
				write_req_data[write_req_curidx].iov_len-=offsetused;
			}
		} else {
			assert(false); //TODO for datagram
		}
	}

	virtual void on_session_closed(void) override {
		session_state=HS_UNINIT;
		do_stop();
	}

};

inline iot_netcon_mesh* iot_netcontype_metaclass_mesh::allocate_netcon(iot_netproto_config* protoconfig) {
	assert(protoconfig!=NULL);
	iot_netcon_mesh *con=(iot_netcon_mesh *)iot_allocate_memblock(sizeof(iot_netcon_mesh), true);
	if(!con) return NULL;
	return new(con) iot_netcon_mesh(protoconfig);
}



#endif // IOT_MESH_CONTROL_H
