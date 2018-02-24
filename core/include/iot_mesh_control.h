#ifndef IOT_MESH_CONTROL_H
#define IOT_MESH_CONTROL_H

#include<stdint.h>
#include<assert.h>

#include "iot_core.h"
#include "iot_netcon.h"

#include "iot_netproto_mesh.h"
#include "mhbtree.h"

class iot_netcon_meshses;

//keeps routing tables, routes inter-host traffic by most optimal way
class iot_meshnet_controller : public iot_netconregistryiface {
public:
	iot_gwinstance *const gwinst;

	struct connmapkey_t {
		iot_hostid_t host;
		uint16_t remoteport;
		uint16_t localport;

		connmapkey_t(void) = default;

		connmapkey_t(int) { //used to zero-initialize
			host=0;
			remoteport=localport=0;
		}
		uint32_t operator& (uint32_t mask) const { //won't use hashing in tree, so always return 0
			return 0;
		}
		bool operator == (const connmapkey_t& op) const {
			return host==op.host && remoteport==op.remoteport && localport==op.localport;
		}
		bool operator != (const connmapkey_t& op) const {
			return !(*this==op);
		}
		bool operator < (const connmapkey_t& op) const {
			if(host<op.host) return true;
			if(host==op.host) {
				if(remoteport<op.remoteport) return true;
				if(remoteport==op.remoteport) return localport<op.localport;
			}
			return false;
		}
		connmapkey_t& operator = (int) {
			host=0;
			remoteport=localport=0;
			return *this;
		}
	};
	#define IOT_MAXMESHSES_HOLES 2
	#define IOT_MAXMESHSES_BUFSIZE_POWER2 13
	struct meshses_state { //keeps state of tunnelled session over mesh network
		uint64_t ack_sequenced_out; //total number of payload bytes written through this end of session and acknowledged by peer. readpos of buffer_out corresponds to this position in stream
		uint64_t sequenced_out; //total number of payload bytes written through this end of session. >= ack_sequenced_out. sequenced_out-ack_sequenced_out<=buffer_out.pending_read() (and peer_rwnd?)
		uint64_t ack_sequenced_in; //total number of payload bytes read through this end of session and acknowledged to peer. 
		uint64_t sequenced_in; //total number of payload bytes read through this end of session sequentially (without holes)
		uint64_t maxseen_sequenced_in; //maximum seen (and saved)  incoming byte number. ==sequenced_in when no holes (lost packets), >sequenced_in when there are holes
		uint32_t inhole_len[IOT_MAXMESHSES_HOLES]; //length of i-th incoming hole. hole 0 starts from sequenced_in offset (is some start_hole_offset[0]).
													//hole i starts from start_hole_offset[i-1]+hole_len[i-1]+nexthole_offset[i-1]
		uint32_t nextinhole_offset[IOT_MAXMESHSES_HOLES];//offset of (i+1)-th incoming hole after end of i-th hole, so is length of valid incoming data block
														//after i-th hole. for last hole (indexed num_inholes-1) next must be true:
														//start_hole_offset[num_inholes-1]+hole_len[num_inholes-1]+nexthole_offset[num_inholes-1]==maxseen_sequenced_in
		uint32_t outhole_len[IOT_MAXMESHSES_HOLES]; //length of i-th outgoing hole. hole 0 starts from ack_sequenced_out. out holes are reported by peer when it gets
													//incoming holes. holes are then retransmitted
		uint32_t nextouthole_offset[IOT_MAXMESHSES_HOLES];//offset of (i+1)-th outgoing hole after end of i-th hole, so is length of aknowledged outgoing data block
														//after i-th hole. for last hole (indexed num_outholes-1) next must be true:
														//start_hole_offset[num_outholes-1]+hole_len[num_inholes-1]+nexthole_offset[num_inholes-1]==maxseen_sequenced_in
		uint8_t random[8]; //random session identifier
		byte_fifo_buf buffer_in;
		byte_fifo_buf buffer_out;
		uint32_t used_buffer_in; //how many bytes of input data is currently unread in buffer_in. maxseen_sequenced_in-used_buffer_in gives position in stream corresponding to writepos of buffer_in
		uint32_t peer_rwnd; //receive window size of peer. obtained together with each ack_sequenced_out and determines maximum of sequenced_out-ack_sequenced_out. write must stop if this difference is >= than peer_rwnd
		uint32_t creation_time; //timestamp - 1e9 in seconds when connection was created by client side (by clock of client)
		uint32_t acception_time; //timestamp - 1e9 in seconds when connection was accepted by server side (by clock of server)
		uint8_t num_inholes; //number of holes in incoming data
		uint8_t num_outholes; //number of holes in outgoing data
		uint8_t is_server; //shows if this side accepted the connection (is server side)
		enum state_t : uint8_t {
			BEFORE_CONN_REQ=0, //before preparing connection request to peer
			QUEUED_CONN_REQ, //connection request is queued to be sent
			WAITING_CONN_ACK, //connection request was sent through one of mesh connections

			CLOSED   //state can be reused for reconnects
		} state;

		void init_client(void) {
			ack_sequenced_out=sequenced_out=ack_sequenced_in=sequenced_in=maxseen_sequenced_in=0;
			used_buffer_in=peer_rwnd=acception_time=0;
			num_inholes=num_outholes=0;
			is_server=0;
			state=BEFORE_CONN_REQ;
			iot_gen_random((char*)random, sizeof(random));
			creation_time=((iot_get_systime()+500000000)/1000000000)-1000000000; //round to integer seconds and offset by 1e9 seconds
		}
	};


private:
	iot_netcon_meshses *cons_head=NULL;

	uv_rwlock_t routing_lock;
	uv_rwlock_t protostate_lock; //protects cons_head list, proto_state as a whole

	uint64_t myroutes_lastversion; //updated at startup and when any peer looses last mesh session. gives initial value for calculating outgoing routing table for peers
	uv_timer_t routesync_timer; //used to resync routing tables to peers
	h_state_t routesync_timer_state=HS_UNINIT;
	uint32_t max_protoid=0;

	struct proto_state_t {
		MemHBTree<iot_netcon_meshses*, connmapkey_t, 0> connmap; //for particular protocol contains all existing connections in a tree sorted by ascending (RemoteHost,RemotePort,LocalPort) triplet
																 //Listening connections have host==0 and remoteport==0
	} **proto_state=NULL; //per-protocol ID state of connections.
						  //is allocated in init() as array of max_protoid pointers (zero ID is impossible, so proto_state[0] is for protocol 1)
						  //each item is further instantiated during creation of first connection

public:

	iot_meshnet_controller(iot_gwinstance *gwinst) : gwinst(gwinst) {
		assert(uv_thread_self()==main_thread);

		int err=uv_rwlock_init(&routing_lock);
		assert(err==0);

		err=uv_rwlock_init(&protostate_lock);
		assert(err==0);

		myroutes_lastversion=gwinst->next_event_numerator();

	}
	int init(void) {
		max_protoid=MESHSES_PROTO_MAX; //TODO change MESHSES_PROTO_MAX usage into some registry query about maximum ID of loaded protocols
		if(max_protoid>0) {
			//allocate space for max_protoid pointers in proto_state
			size_t sz=sizeof(proto_state_t*) * max_protoid;
			proto_state=(proto_state_t**)malloc(sz);
			if(!proto_state) return IOT_ERROR_NO_MEMORY;
			memset(proto_state, 0, sz);
		}

		int err=uv_timer_init(main_thread_item->loop, &routesync_timer);
		assert(err==0);
		routesync_timer.data=this;
		routesync_timer_state=HS_INIT;

		uv_timer_start(&routesync_timer, [](uv_timer_t* handle)->void {
			iot_meshnet_controller* obj=(iot_meshnet_controller*)handle->data;
			obj->try_sync_routing_tables();
		}, 2*1000, 2*1000); //TODO start timer by msg or async when really necessary?
		return 0;
	}
	~iot_meshnet_controller(void) {
		if(proto_state) {
			for(uint32_t i=0;i<max_protoid;i++) {
				if(proto_state[i]) {
					delete proto_state[i];
					proto_state[i]=NULL;
				}
			}
			free(proto_state);
			proto_state=NULL;
		}
		max_protoid=0;
		uv_rwlock_destroy(&routing_lock);
		uv_rwlock_destroy(&protostate_lock);

		if(routesync_timer_state==HS_INIT) {
			routesync_timer_state=HS_UNINIT;
			uv_close((uv_handle_t*)&routesync_timer, NULL); //TODO move to graceful shutdown when such function appears
		}
	}

	int register_session(iot_netproto_session_mesh* ses); //called from session's thread

	void unregister_session(iot_netproto_session_mesh* ses); //must be called in ses's thread if ses isn't closed, in any thread otherwise

	void confirm_routing_table_sync_topeer(iot_netproto_session_mesh*);

	void apply_session_routes_change(iot_netproto_session_mesh* ses, bool waslocked=false, uint64_t = 0); //must be called in ses's thread if ses isn't closed, in any thread otherwise

	void print_routingtable(iot_peer* forpeer, bool waslocked); //non-null forpeer means print routing table for specified host

	void sync_routing_table_topeer(iot_netproto_session_mesh* ses);

	void sync_routing_table_frompeer(iot_netproto_session_mesh* ses, iot_netproto_session_mesh::packet_rtable_req *req);


//meshses operations:
	int meshses_connect(iot_netcon_meshses* con);
	int meshses_close(iot_netcon_meshses* con) {
		return 0;
	}

private:

	void try_sync_routing_tables(void);

	virtual int on_new_connection(iot_netcon *conobj) override;
	virtual void on_destroyed_connection(iot_netcon *conobj) override;
};

//realizes netcon for sessions, tunnelled through mesh net


class iot_netcontype_metaclass_meshses: public iot_netcontype_metaclass {
	iot_netcontype_metaclass_meshses(void) : iot_netcontype_metaclass("meshses", true, true) {
	}

public:
	static iot_netcontype_metaclass_meshses object;

	virtual void destroy_netcon(iot_netcon* obj) const { //must destroy netcon object in correct way (metaclass knows how its netcon objects are created)
		obj->~iot_netcon();
		iot_release_memblock(obj);
	}
	static iot_netcon_meshses* allocate_netcon(iot_netproto_config* protoconfig);

private:
	virtual int p_from_json(json_object* json, iot_netproto_config* config, iot_netcon*& obj, bool is_server, iot_netconregistryiface* registry, uint32_t metric) const override {
		return IOT_ERROR_INVALID_ARGS;
	}
};


class iot_netcon_meshses : public iot_netcon {
	friend class iot_meshnet_controller;

	iot_hostid_t remote_host; //remote host (for connected passive, 0 for listening) or destination host (for non-passive)
	iot_objref_ptr<iot_peer> remote_peer;

	int32_t local_port; //local port to listen to (for passive) or to bind to (for non-passive). <0 for non-passive means auto binding
	int32_t remote_port; //remote port (for connected passive) or remote destination port (for non-passive). <0 for passive means listening or unconnected
	iot_meshnet_controller::connmapkey_t connection_key=0; //keeps actual key in connmap of protocol
	iot_meshnet_controller::meshses_state *connection_state=NULL; //is fully controlled by meshnet controller

	uv_timer_t retry_phase_timer={};
	uint64_t retry_delay=0;

	uint32_t connect_errors=0;

	enum : uint8_t {
		PHASE_UNINITED,		//server or client
		PHASE_INITIAL,		//server or client
		PHASE_LISTENING,	//server (final phase for accepting connection)
		PHASE_CONNECTING,	//client
		PHASE_CONNECTED,	//common
		PHASE_RUNNING		//common
	} phase=PHASE_UNINITED; //phase is used inside worker thread
	h_state_t h_phase_timer_state=HS_UNINIT;
	h_state_t session_state=HS_UNINIT;

	iot_netcon_meshses(iot_netproto_config* protoconfig) : iot_netcon(&iot_netcontype_metaclass_meshses::object, protoconfig) {
	}

	int init_server(iot_meshnet_controller* controller, uint16_t port_, bool noinit=false) { //true noinit to leave object in INITING state on success
		if(!controller) return IOT_ERROR_INVALID_ARGS;
		const iot_netprototype_metaclass* protometa=protoconfig->get_metaclass();
		if(!protometa->meshses_protoid) return IOT_ERROR_NOT_SUPPORTED;
		if(port_>protometa->meshses_maxport) return IOT_ERROR_INVALID_ARGS;

		if(!trymark_initing()) {
			//must not fail if init is done in one thread just after construction
			assert(false);
			return IOT_ERROR_CRITICAL_BUG;
		}
		//now state is INITING and only current thread will do it
		assert(phase==PHASE_UNINITED);

		is_passive=true;
		remote_host=0;
		local_port=port_;
		remote_port=-1; //is not meaningful when remote_host is 0

		phase=PHASE_INITIAL;
		int err=assign_registryiface(controller);
		if(err) goto onerror;

		if(!noinit) mark_inited();
		return 0;
onerror:
		unmark_initing();
		return err;
	}

	int init_client(iot_meshnet_controller* controller, iot_hostid_t host_, uint16_t port_, bool bindlocalport=false, uint16_t localport_=0) {
		if(!controller) return IOT_ERROR_INVALID_ARGS;
		const iot_netprototype_metaclass* protometa=protoconfig->get_metaclass();
		if(!protometa->meshses_protoid) return IOT_ERROR_NOT_SUPPORTED;
		if(!host_ || port_>protometa->meshses_maxport || (bindlocalport && localport_>protometa->meshses_maxport)) return IOT_ERROR_INVALID_ARGS;

		if(!trymark_initing()) {
			//must not fail if init is done in one thread
			assert(false);
			return IOT_ERROR_CRITICAL_BUG;
		}
		//now state is INITED and only current thread will do it
		assert(phase==PHASE_UNINITED);

		is_passive=false;
		remote_host=host_;
		local_port=bindlocalport ? localport_ : -1;
		remote_port=port_;

		phase=PHASE_INITIAL;
		int err=assign_registryiface(controller);
		if(err) goto onerror;

		mark_inited();

		return 0;
onerror:
		unmark_initing();
		return err;
	}

	virtual void do_start_uv(void) override;
	virtual int do_stop(void) override {
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

		if(connection_state) {
			num_closing++;
			static_cast<iot_meshnet_controller*>(registry)->meshses_close(this);
			//assume async close
		}

		if(h_phase_timer_state>=HS_INIT) {
			h_phase_timer_state=HS_CLOSING;
			num_closing++;
			uv_close((uv_handle_t*)&retry_phase_timer, [](uv_handle_t* handle) -> void {
				iot_netcon_meshses* obj=(iot_netcon_meshses*)(handle->data);
				assert(obj->h_phase_timer_state==HS_CLOSING);
				obj->h_phase_timer_state=HS_UNINIT;
				obj->do_stop();
			});
		} else if(h_phase_timer_state==HS_CLOSING) num_closing++;

		if(num_closing>0) return IOT_ERROR_NOT_READY;

		phase=PHASE_INITIAL;
		return on_stopped();
	}


	void retry_phase(void) { //schedules call of process_[client|server] phase() after delay ms in retry_delay var
		assert(uv_thread_self()==thread->thread);
		h_phase_timer_state=HS_ACTIVE;
		uv_timer_start(&retry_phase_timer, [](uv_timer_t* h) -> void {
			iot_netcon_meshses* obj=(iot_netcon_meshses*)(h->data);

			obj->h_phase_timer_state=HS_INIT;
//			if(!obj->is_passive)
				obj->process_client_phase();
//			else
//				obj->process_server_phase();
		}, retry_delay, 0);
	}

	void process_client_phase(void) {
		assert(uv_thread_self()==thread->thread);
		int err;
		uint32_t delay;
		iot_meshnet_controller* controller=static_cast<iot_meshnet_controller*>(registry);
//again:
		switch(phase) {
			case PHASE_INITIAL:
				phase=PHASE_CONNECTING;
			case PHASE_CONNECTING:
outlog_notice("meshses in connecting phase");
				err=controller->meshses_connect(this); //will call on_connect_status() if 0 is returned
				if(!err) {
					return;
				}

				delay=30+(connect_errors>10 ? 10 : connect_errors)*3*60;
				outlog_notice("Error calling meshses_connect() for host '%" IOT_PRIhostid "': %s, retrying in %u secs", remote_host, kapi_strerror(err), delay);
				connect_errors++;
				retry_delay=delay*1000;
				retry_phase();
				return;
			default:
				outlog_error("%s() called for illegal phase %d for host '%" IOT_PRIhostid "', aborting", __func__, int(phase), remote_host);
				do_stop();
				return;
		}
	}


};


#endif // IOT_MESH_CONTROL_H
