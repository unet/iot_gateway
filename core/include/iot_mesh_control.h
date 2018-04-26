#ifndef IOT_MESH_CONTROL_H
#define IOT_MESH_CONTROL_H

#include<stdint.h>
#include<assert.h>

#include "iot_core.h"
#include "iot_netcon.h"

#include "iot_netproto_mesh.h"
#include "mhbtree.h"

class iot_netcon_mesh;

//theoretical max value for distribution is 4 (2 bits are used)
#define IOT_MESHTUN_MAXDISTRIBUTION 3

#define IOT_MESHTUN_BUFSIZE_POWER2 16

enum iot_meshtun_type_t : uint8_t { //real type of object under iot_meshtun_packet
	TUN_STREAM, //non-listening tunnelled stream
	TUN_STREAM_LISTEN, //listening tunnelled stream
	TUN_DATAGRAM, //tunnelled datagram
	TUN_FORWARDING //forwarding of whole meshtun packet to another peer
};


struct iot_meshconnmapkey_t {
		iot_hostid_t host;
		uint16_t remoteport;
		uint16_t localport;

		iot_meshconnmapkey_t(void) = default;
		iot_meshconnmapkey_t(iot_hostid_t host, uint16_t remoteport, uint16_t localport) : host(host), remoteport(remoteport), localport(localport) {}

		iot_meshconnmapkey_t(int zero) { //used to zero-initialize
			assert(zero==0);
			host=0;
			remoteport=localport=0;
		}
		uint32_t operator& (uint32_t mask) const { //won't use hashing in tree, so always return 0
			return 0;
		}
		bool operator == (const iot_meshconnmapkey_t& op) const {
			return host==op.host && remoteport==op.remoteport && localport==op.localport;
		}
		bool operator != (const iot_meshconnmapkey_t& op) const {
			return !(*this==op);
		}
		bool operator < (const iot_meshconnmapkey_t& op) const {
			if(host<op.host) return true;
			if(host==op.host) {
				if(remoteport<op.remoteport) return true;
				if(remoteport==op.remoteport) return localport<op.localport;
			}
			return false;
		}
		bool operator <= (const iot_meshconnmapkey_t& op) const {
			if(host<op.host) return true;
			if(host==op.host) {
				if(remoteport<op.remoteport) return true;
				if(remoteport==op.remoteport) return localport<=op.localport;
			}
			return false;
		}
		iot_meshconnmapkey_t& operator = (int zero) { //can be used to zero this struct
			assert(zero==0);
			host=0;
			remoteport=localport=0;
			return *this;
		}
};


//all mesh streams to particular peer are closed after this number of seconds after loosing last route
#define IOT_MESHNET_NOROUTETIMEOUT 5

//precision of timer in controller
#define IOT_MESHNET_TIMER_PREC 100

//keeps routing tables, routes inter-host traffic by most optimal way
class iot_meshnet_controller {
public:
	iot_gwinstance *const gwinst;

	iot_fixprec_timer<true> timer;

	bool is_shutdown=false;

private:
	iot_netcon_mesh *cons_head=NULL;
	iot_fixprec_timer_item<true> shutdown_timer_item;  //used during graceful shutdown of controller
	iot_fixprec_timer_item<true> keepalive_timer_item; //used to send keepalive on client streamed meshtuns
	iot_fixprec_timer_item<true> routesync_timer_item; //used to resync routing tables to peers


	uv_rwlock_t routing_lock; //protects all routing related staff in iot_peer, iot_netproto_session_mesh, here (noroute_atimer, myroutes_lastversion)
	uv_rwlock_t protostate_lock; //protects cons_head list, proto_state as a whole. NO MESHTUN LOCKS MUST BE OBTAINED INSIDE THIS LOCK!! deadlock will happen
//	iot_atimer noroute_atimer; //action timer to detect no-route event per peer, after fixed period of routes absence

	uint64_t myroutes_lastversion; //updated at startup and when any peer looses last mesh session. gives initial value for calculating outgoing routing table for peers
	uint32_t max_protoid=0;
	uint8_t meshtun_distribution=2; //each mesh stream is distributed over such number of direct mesh sessions (routes). must be at least 1 and not more than IOT_MESHTUN_MAXDISTRIBUTION!

	struct proto_state_t {
		MemHBTree<iot_meshtun_state*, iot_meshconnmapkey_t, 0> connmap; //for particular protocol contains all existing connections in a tree sorted by ascending (RemoteHost,RemotePort,LocalPort) triplet
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

		shutdown_timer_item.init(this, [](void *controller, uint32_t period_id, uint64_t now_ms)->void {
			iot_meshnet_controller *c=(iot_meshnet_controller *)controller;
			c->graceful_shutdown_step2();
		});
		keepalive_timer_item.init(this, [](void *controller, uint32_t period_id, uint64_t now_ms)->void {
			iot_meshnet_controller *c=(iot_meshnet_controller *)controller;
			c->on_meshtun_keepalive_timer();
			c->timer.schedule(c->keepalive_timer_item, 30'000);
		});
		routesync_timer_item.init(this, [](void *controller, uint32_t period_id, uint64_t now_ms)->void {
			iot_meshnet_controller *c=(iot_meshnet_controller *)controller;
			c->try_sync_routing_tables();
			c->timer.schedule(c->routesync_timer_item, 2'000);
		});
	}
	int init(void) {
		assert(uv_thread_self()==main_thread);

		max_protoid=MESHTUN_PROTO_MAX; //TODO change MESHTUN_PROTO_MAX usage into some registry query about maximum ID of loaded protocols
		if(max_protoid>0) {
			//allocate space for max_protoid pointers in proto_state
			size_t sz=sizeof(proto_state_t*) * max_protoid;
			proto_state=(proto_state_t**)malloc(sz);
			if(!proto_state) return IOT_ERROR_NO_MEMORY;
			memset(proto_state, 0, sz);
		}

		int err=timer.init(IOT_MESHNET_TIMER_PREC, 60'000);
		if(err) return err;

		timer.schedule(keepalive_timer_item, 30'000);
		timer.schedule(routesync_timer_item, 2'000);

//		noroute_atimer.init(IOT_MESHNET_NOROUTETIMEOUT*1000, main_thread_item->loop, true);

		return 0;
	}
	~iot_meshnet_controller(void) {
		timer.deinit();

		if(proto_state) {
			on_noroute_timer(NULL); //tries to set NO_ROUTE error for all bound meshtuns. this action MUST unbind them immediately
			if(proto_state) { //on_noroute_timer can free proto state
				for(uint32_t i=0;i<max_protoid;i++) {
					if(proto_state[i]) {
						assert(proto_state[i]->connmap.getamount()==0); //ensure tree is empty
						delete proto_state[i];
						proto_state[i]=NULL;
					}
				}
				free(proto_state);
				proto_state=NULL;
			}
		}
		max_protoid=0;

		uv_rwlock_destroy(&routing_lock);
		uv_rwlock_destroy(&protostate_lock);
	}
	void graceful_shutdown(void);
	void graceful_shutdown_step2(void) {
outlog_notice("STEP 2 of mesh controll shutting down initiated");
		is_shutdown=true;

		if(proto_state) {
			on_noroute_timer(NULL); //tries to set NO_ROUTE error for all bound meshtuns. this action MUST unbind them immediately
		}
		gwinst->graceful_shutdown_step4();
	}

	uint8_t get_meshtun_distribution(void) const {
		return meshtun_distribution;
	}
	void on_noroute_timer(iot_peer *peer);
	void on_meshtun_keepalive_timer(iot_hostid_t hostid=0);

	int register_session(iot_netproto_session_mesh* ses); //called from session's thread

	void unregister_session(iot_netproto_session_mesh* ses); //must be called in ses's thread if ses isn't closed, in any thread otherwise

	void confirm_routing_table_sync_topeer(iot_netproto_session_mesh* ses, uint64_t version, uint8_t waserror=0); //notify controller that session's peer has particular routing table version from this host. can be used to do initial routing table sync

	void apply_session_routes_change(iot_netproto_session_mesh* ses, bool waslocked=false, uint64_t = 0); //must be called in ses's thread if ses isn't closed, in any thread otherwise

	void print_routingtable(iot_peer* forpeer, bool waslocked); //non-null forpeer means print routing table for specified host

	bool sync_routing_table_topeer(iot_netproto_session_mesh* ses);

	void sync_routing_table_frompeer(iot_netproto_session_mesh* ses, iot_netproto_session_mesh::packet_rtable_req *req);

	int fill_peers_from_active_routes(iot_netproto_session_mesh* ses, iot_objref_ptr<iot_peer> *peers, uint32_t *delays, uint16_t *pathlens, size_t max_peers); //called from session's thread

	bool route_meshtunq(iot_peer* peer, iot_hostid_t avoid_peer_id=0, uint16_t maxpathlen=0xffff);

	//binds provided iot_meshtun_state to connection map.
	int meshtun_bind(iot_meshtun_state *tunstate, const iot_netprototype_metaclass* protometa, iot_hostid_t remote_host, uint16_t remote_port, bool auto_local_port=true, uint16_t local_port=0);
	void meshtun_unbind(iot_meshtun_state *tunstate);
	bool meshtun_find_bound(uint16_t protoid, uint16_t local_port, iot_hostid_t remote_host, uint16_t remote_port, iot_objref_ptr<iot_meshtun_state> &result);


//meshtun operations:
	int meshtun_create(iot_netcon_mesh* con, iot_memallocator* allocator, iot_meshtun_type_t type, bool is_passive=false);
	int meshtun_connect(iot_netcon_mesh* con, bool is_stream);

	int on_new_meshtun(iot_netcon_mesh *conobj);
	void on_destroyed_meshtun(iot_netcon_mesh *conobj);

private:

	void try_sync_routing_tables(void);

};


//base class for objects which are used to send data to particular host. they can be queued in peer's output queue
class iot_meshtun_packet : public iot_objectrefable { //keeps state of tunnelled session over mesh network
//	friend class iot_meshnet_controller;
	friend class iot_netproto_session_mesh;

public:
	volatile std::atomic<iot_meshtun_packet*> next=NULL; //points to next stream state in peer's queue
	iot_meshnet_controller* const controller;

	enum state_t : uint8_t {
		ST_CLOSED,			//initial state or state before reusage. input and output must be both closed and without pendings
		ST_SENDING_SYN,		//SYN request is to be written or was already written and now ACK is awaited. output_ack_pending shows if any write was sent (and thus ACK can arrive)
//		ST_AFTER_SEND_SYN,	//connection request was sent (written to socket) through one of mesh sessions, ACK from peer is being awaited

		ST_ESTABLISHED,

		ST_LISTENING,
		ST_RESET,			//requests to send RESET to peer of streamed tunnel OR error report to source of forwarded packet
		ST_FORWARDING,
	};

	enum update_t : uint8_t {
		UPD_ERROR_SET,		//error was set
		UPD_SHUTDOWN,		//shutdown was called. output_closed already set to 1
		UPD_DETACH,			//netcon was detached. con already nullified
		UPD_WRITTEN,		//write operation was sent
		UPD_WRITEREADY,		//output should be scheduled
		UPD_READREADY,		//netcon should be notified about read
		UPD_WRITESPACEFREED,//netcon should be notified about ability to write
	};
protected:
	uv_mutex_t mutex; //protects almost all internal data when item is referred outside netcon. exceptions: next, in_queue
#ifndef NDEBUG
	uv_thread_t lockowner=0;
#endif

	volatile std::atomic<int> error={0};
	//IOT_ERROR_CRITICAL_ERROR
	//IOT_ERROR_TRY_AGAIN - quick retry must be attempted (like after EINTR for normal sockets)

public:
	const uint8_t type:2, //value from iot_meshtun_type_t showing real type of object
			is_passive_stream:1; //for TUN_STREAM shows if this end is passive (was accepted) or active (client)

protected:
	volatile state_t state=ST_CLOSED;
	volatile uint8_t
			in_queue=0;				//shows if item is in peer's queue. when state is locked, false value guarantees that item is not in queue, but true
									//value DOES NOT guarantee that item IS STILL in queue (it can be removed from queue by another thread at any time if
									//peer's meshtunq lock is not held)


	iot_meshtun_packet(void) = delete;
	iot_meshtun_packet(const iot_meshtun_packet &) = delete; //forbid copy-constructor (mutex, next, con cannot be copied)
	iot_meshtun_packet(iot_meshnet_controller* controller, iot_meshtun_type_t type, bool is_passive_stream=false)
			: iot_objectrefable(object_destroysub_memblock, true), controller(controller), type(type), is_passive_stream(is_passive_stream)
	{
		assert(controller!=NULL);
		uv_mutex_init(&mutex);
	}

	virtual ~iot_meshtun_packet() {
		assert(state==ST_CLOSED);

		assert(!in_queue);

#ifndef NDEBUG
		int er=uv_mutex_trylock(&mutex); //ensure object is unlocked
		assert(!er);
		uv_mutex_unlock(&mutex);
#endif

		uv_mutex_destroy(&mutex);
	}
	void set_closed(bool is_locked=true) {
		if(!is_locked) {
			lock();
		}
		set_state(ST_CLOSED);
		if(!is_locked) {
			unlock();
		}
/*		//MUST BE CALLED under lock()
		assert(uv_thread_self()==lockowner);

		assert(state!=ST_CLOSED);
		assert(output_closed && input_closed && !output_pending && !output_ack_pending && !input_pending && !input_ack_pending && !in_queue);
		state=ST_CLOSED;
		if(is_bound) unbind_base();*/
	}

public:
	bool is_closed(void) const { //thread safe if called from same thread which can re-activate meshtun
		return state==ST_CLOSED;
	}
	state_t get_state(void) const {
		return state;
	}
	void clear_in_queue(void) {
		in_queue=0;
	}

	void set_in_queue(void) {
		in_queue=1;
	}

	bool check_in_queue(void) const {
		return in_queue!=0;
	}

	void lock(void) {
		//returns false if abort_detached==true and state is to be closed and must be unrefed if possible. when false is returned, state is not locked, so cannot be accessed
		uv_mutex_lock(&mutex);
#ifndef NDEBUG
		lockowner=uv_thread_self();
#endif
//		if(con || !abort_detached) return true;
//		unlock();
//		return false;
	}
	void unlock(void) {
#ifndef NDEBUG
		lockowner=0;
#endif
		uv_mutex_unlock(&mutex);
	}
	void set_error(int e) {
		//MUST BE CALLED under lock()
		assert(uv_thread_self()==lockowner);
		if(!e) {
			error.store(0, std::memory_order_relaxed);
			return;
		}
		assert(!get_error());
		error.store(e, std::memory_order_relaxed);
		on_state_update(UPD_ERROR_SET);
	}
	int get_error(void) const {
		return error.load(std::memory_order_relaxed);
	}

protected:
	virtual int set_state(state_t newstate) = 0; //must be called INSIDE LOCK
	virtual void on_state_update(update_t upd) = 0; //must be called INSIDE LOCK. reacts to setting error, detaching, changing substate flags

};


class iot_meshtun_forwarding : public iot_meshtun_packet { //keeps task for packets forwarding to another host (proxying)
	friend class iot_peer;;
	friend class iot_netproto_session_mesh;

	iot_hostid_t dst_host, src_host;
	iot_hostid_t from_host; //host which proxied this packet to this host
	iot_objref_ptr<iot_peer> dst_peer;
	iot_objref_ptr<iot_peer> src_peer;
	void *request_body=NULL;
	iot_netproto_session_mesh::packet_meshtun_hdr* request; //will point to address just after this object (i.e. in the same memory block)
	iot_netproto_session_mesh *meshses=NULL;
	iot_fixprec_timer_item<true> timer_item;

	int retries;

public:
	iot_meshtun_forwarding(iot_meshnet_controller* controller, iot_hostid_t dst_host, iot_hostid_t src_host, iot_hostid_t from_host, iot_netproto_session_mesh::packet_meshtun_hdr *req)
		 : iot_meshtun_packet(controller, TUN_FORWARDING), dst_host(dst_host), src_host(src_host), from_host(from_host), request(req)
	{
		assert(dst_host>0 && src_host>0 && req!=NULL); //from_host can be zero if RESET to stream is generated
		assert(dst_host!=from_host);

		timer_item.init(this, [](void *stp, uint32_t period_id, uint64_t now_ms)->void {
			iot_meshtun_forwarding *st=(iot_meshtun_forwarding *)stp;
			st->on_timer();
		});
	}

	~iot_meshtun_forwarding() {
		if(request_body) iot_release_memblock(request_body);
		request_body=NULL;

		timer_item.unschedule();
	}
	int forward(void *data) { //data - pointer to separate memblock with meshtun data (without any packet_meshtun_hdr headers). can be NULL of data size is zero
		//reference to data memblock will be increased on success
		lock();
		int err=0;
		if(state!=ST_CLOSED) {
			err=IOT_ERROR_INVALID_STATE;
			goto onexit;
		}
		if(!dst_host || !src_host || !request) {
			err=IOT_ERROR_INVALID_ARGS;
			goto onexit;
		}
		set_error(0);
		if(request_body) iot_release_memblock(request_body);
		if(data) iot_incref_memblock(data);
		request_body=data;

		set_state(ST_FORWARDING);

		err=controller->timer.schedule(timer_item, 5000); //TODO calculate timeout somehow?
		assert(err==0); //must not have concurrent problem because of single-thread use of current function
onexit:
		unlock();
		return err;
	}
private:
	void on_timer(void) {
		//for this timer is used to close request only
		ref(); //meshtun queue helds the only reference, so current object will be removed inside set_closed which will lead to segfault
		lock();
		if(state==ST_FORWARDING || state==ST_RESET) {
outlog_debug_meshtun("CLOSING %s packet to %" IOT_PRIhostid " due to timeout", state==ST_FORWARDING ? "forwarding" : "status", state==ST_FORWARDING ? dst_host : src_host);
			set_closed();
		}
		unlock();
		unref();
	}

	virtual int set_state(state_t newstate) override;//must be called INSIDE LOCK
	virtual void on_state_update(update_t upd) override; //must be called INSIDE LOCK. reacts to setting error, detaching, changing substate flags

/*	virtual int on_shutdown(void) override { //called INSIDE LOCK when shutdown() is called to close writing part of this meshtun
		assert(uv_thread_self()==lockowner);

		assert(false); //should not be ever called because output_closed is always true
		return IOT_ERROR_NO_PEER;
	}
	virtual void on_detach(void) override { //called INSIDE LOCK when netcon is detached from non-closed meshtun.  must direct meshtun into CLOSED state
		//no much work here
		assert(uv_thread_self()==lockowner);

		assert(state==ST_LISTENING);

//		input_closed=1; ??
		listenq.clear();
		input_pending=0;
		set_closed();
	}
*/


};


//abstract base class for tunneled streams and datagrams
class iot_meshtun_state : public iot_meshtun_packet { //keeps state of tunnelled session over mesh network
	friend class iot_meshnet_controller;
	friend class iot_netproto_session_mesh;


protected:
	iot_netcon_mesh* con=NULL; //bound netcon if any
	void* inmeta_data=NULL; //must be memblock if non-NULL!
	void* outmeta_data=NULL; //must be memblock if non-NULL!

	iot_meshconnmapkey_t connection_key=0;
	const iot_netprototype_metaclass* protometa=NULL;

	uint16_t inmeta_size=0;
	uint16_t outmeta_size=0;

	volatile uint8_t
			output_closed=1,		//if true, no new output is possible and output_wanted is ignored,
									//but output buffer can have data for sending if output_pending is true. FINALIZE bit will be set in last portion of data to peer
			input_closed=1,			//if true, FINALIZE request was received from peer (in ST_ESTABLISHED state, so reading will return 0 after input buffer exhausted) OR no SYN received (in non ST_ESTABLISHED state, so reading will return NO_PEER), 
			input_pending=0,		//if true, input buffer has data to be read ???????TODO move to non-volatile???

			input_wanted=0,			//if true, netcon gets notification when input_pending is true
			output_wanted=0;		//if true, netcon must be notified when there is free space in output buffer or ESTABLISHED state acquired

	uint8_t
			output_pending=0,		//if true, output buffer has data to be sent.
			output_ack_pending=0,	//if true, there is unacknowledged output (for TUN_STREAM only), so space in output buffer is still busy with unacknowledged data
			input_ack_pending=0,	//if true, there is unacknowledged input (for TUN_STREAM only)
			is_bound=0;				//if true, connection_key and proto_typeid are valid and shows positions in controller's connmap

	iot_meshtun_state(iot_meshnet_controller* controller, iot_meshtun_type_t type, bool is_passive_stream=false)
			: iot_meshtun_packet(controller, type, is_passive_stream)
	{
	}

	void bind_base(const iot_netprototype_metaclass* protometa_, const iot_meshconnmapkey_t &connection_key_) { //must be done after successful insertion into connmap by derived class
		//MUST BE CALLED under lock()
		assert(uv_thread_self()==lockowner);

		assert(state==ST_CLOSED);
		assert(!is_bound);
		assert(con!=NULL);

		protometa=protometa_;
		connection_key=connection_key_;
		ref(); //increase refcount assuming object was just inserted to connmap
		is_bound=1;
	}

	bool unbind_base(void) {
		//MUST BE CALLED under lock()
		//returns false if state cannot be unbind in current substate. true is already unbound or was just unbound
		assert(uv_thread_self()==lockowner);

		if(!is_bound) return true;
		if(!(output_closed && !output_pending && !output_ack_pending && input_closed && !input_ack_pending)) return false;
		controller->meshtun_unbind(this);
		is_bound=0;
		unref(); //reverse to ref() in bind_base()
		return true;
	}

public:
	virtual ~iot_meshtun_state() {
		set_outmeta(NULL); //release meta data if still set

		assert(!is_bound);
		assert(!con);
	}
	void attach_netcon(iot_netcon_mesh* con_) {
		assert(state==ST_CLOSED);
		assert(con==NULL && con_!=NULL);
		con=con_;
	}
	bool detach_netcon(bool cancel_ifclosed=false) { //must be called before clearing last reference so that destructor was not called before this method
	//THIS METHOD MEANS THE SAME AS close() for last handle in POSIX sockets
	//cancel_ifclosed can be true to not detach if state was changed to reusable (ST_CLOSED) immediately
	//returns whether was actually detached so that reference in netcon could be cleared
		lock();
		assert(con!=NULL);
		output_wanted=input_wanted=0;
		bool rval;
		if(cancel_ifclosed) { //netcon wants to reuse meshtun state if possible
			if(!output_closed) {
				assert(state!=ST_CLOSED);
				output_closed=1;
				on_state_update(UPD_SHUTDOWN); //actually does shutdown
			}
			if(state==ST_CLOSED) {
				rval=false;
				goto onexit;
			}
		}
		con=NULL;
		on_state_update(UPD_DETACH);

/*		if(state!=ST_CLOSED) {
			on_detach(); //can nullify con and/or change state to closed
		}
		if(state==ST_CLOSED) {
			if(cancel_ifclosed && con) { //detach is cancelled
				assert(is_bound);
				rval=false;
				goto onexit;
			}
			if(is_bound) unbind_base();
		}
		con=NULL;*/
		rval=true;
onexit:
		unlock();
		return rval;
	}
	//returns:
	//0 - shutdown was initiated successfully
	//IOT_ERROR_NO_PEER - meshtun is not connected or shutdown was already called
	///////IOT_ERROR_NOT_READY - shutdown cannot finish immediately (for attached netcon only). signal will be sent when finished
	int shutdown(void) { //closes local output side of connection. must be initiated by netcon
		lock();
		int err;
		if(!con) { //must be initiated directly or indirectly by netcon, so netcom must be attached
			assert(false);
			err=IOT_ERROR_NO_PEER;
			goto onexit;
		}
		if(state==ST_CLOSED || output_closed) {
			err=IOT_ERROR_NO_PEER;
			goto onexit;
		}
		output_closed=1;
		on_state_update(UPD_SHUTDOWN);
		err=0;
onexit:
		unlock();
		return err;
	}
	bool is_detached(void) const {
		return !con;
	}

	void set_outmeta(void* meta_data_, uint16_t meta_size_=0); //MUST BE INITIATED by netcon thread. used to set new or reset outbound meta data. meta_data_ must be memblock (if not NULL)! its refcount will be increased
	void get_inmeta(void* &meta_data_, uint16_t &meta_size_) { //used to get (and reset) inbound meta data. on success meta_data_ will set to memblock (if not NULL)! 
		lock();
		if(!inmeta_data) {
			meta_data_=NULL;
			meta_size_=0;
		} else {
			meta_data_=inmeta_data;
			meta_size_=inmeta_size;
			inmeta_data=NULL;
			inmeta_size=0;
		}
		unlock();
	}

	bool has_inmeta(void) {
		return inmeta_data!=NULL;
	}

};

struct iot_meshtun_stream_listen_state : public iot_meshtun_state { //keeps state of listening tunnelled stream
	friend class iot_netproto_session_mesh;

	struct listenq_item { //such struct is written to listenq when incoming request to setup tunnelled stream arrives
		iot_hostid_t remotehost;
		uint64_t initial_sequence; //data sequence used in SYN request
		uint32_t creation_time; //UNUSED FOR NOW, set to 0   timestamp - 1e9 in seconds when connection was created by client side (by clock of client)
		uint32_t request_time; //timestamp - 1e9 in seconds when connection request arrived by local clock
		uint32_t peer_rwnd; //receive window size of peer
		uint16_t remoteport;
		uint16_t metasize;
		uint8_t random[IOT_MESHPROTO_TUNSTREAM_IDLEN]; //random session identifier
		char metadata[];
	};
	byte_fifo_buf listenq;
	uint32_t accept_timeout=30; //after such seconds listenq_item is removed from listenq

	iot_meshtun_stream_listen_state(iot_netcon_mesh* con_, iot_meshnet_controller* controller, uint8_t listenq_size_power, char* listenq_buf)
			: iot_meshtun_state(controller, TUN_STREAM_LISTEN)
	{
		bool rval=listenq.setbuf(listenq_size_power, listenq_buf);
		assert(rval);
		attach_netcon(con_);
	}

//	int enqueue_request(iot_netproto_session_mesh::packet_meshtun_hdr* req); //tries to put connection request data to listenq

	int accept(iot_meshtun_stream_state* rep); //tries to accept new connection on current listening stream meshtun into provided stream meshtun rep.
											//rep must be closed, unbound, attached to netcon. it must not be engaged in another thread until this function returns

	int listen(const iot_netprototype_metaclass* protometa, uint16_t local_port_) {
		lock();
		int err=0;
		if(state!=ST_CLOSED) {
			err=IOT_ERROR_INVALID_STATE;
			goto onexit;
		}
		if(!con) {
			err=IOT_ERROR_NOT_INITED;
			goto onexit;
		}
		set_error(0);

		input_closed=0;

		err=controller->meshtun_bind(this, protometa, 0, 0, false, local_port_); //in case of success this meshtun can get incoming connection immediately, so must be locked
		if(err) goto onexit;

		set_state(ST_LISTENING);

onexit:
		unlock();
		return err;
	}
private:
	virtual int set_state(state_t newstate) override; //must be called INSIDE LOCK
	virtual void on_state_update(update_t upd) override; //must be called INSIDE LOCK. reacts to setting error, detaching, changing substate flags

/*	virtual int on_shutdown(void) override { //called INSIDE LOCK when shutdown() is called to close writing part of this meshtun
		assert(uv_thread_self()==lockowner);

		assert(false); //should not be ever called because output_closed is always true
		return IOT_ERROR_NO_PEER;
	}
	virtual void on_detach(void) override { //called INSIDE LOCK when netcon is detached from non-closed meshtun.  must direct meshtun into CLOSED state
		//no much work here
		assert(uv_thread_self()==lockowner);

		assert(state==ST_LISTENING);

//		input_closed=1; ??
		listenq.clear();
		input_pending=0;
		set_closed();
	}
*/
};

//checks if seq1 is less than seq2 with account of 64-bit overflow
static inline bool stream_sequence_before(uint64_t seq1, uint64_t seq2) {
	return int64_t(seq2 - seq1)>0;
}

#define stream_sequence_after(seq1, seq2) stream_sequence_before(seq2, seq1)

//determines how many blocks of output can be between ack_sequence_out and sequence_out
//this is also maximum number of unACKed blocks in fly
#define IOT_MESHTUNSTREAM_MAXOUTPUTSPLIT 8

//such maximum number of bytes of data can be outputted in single meshtun packet
#define IOT_MESHTUNSTREAM_MTU 32768

//timeout in ms to get FIN of input stream after output is finished and no data input allowed
#define IOT_MESHTUNSTREAM_INPUTFIN_TIMEOUT 500

//timeout in ms to wait for some output before sending ACK for input
#define IOT_MESHTUNSTREAM_INPUTACK_DELAY 1


class iot_meshtun_stream_state : public iot_meshtun_state { //keeps state of connected or accepted tunnelled stream
	friend class iot_netproto_session_mesh;

	iot_objref_ptr<iot_peer> remote_peer;
	iot_fixprec_timer_item<true> timer_item;

	uint64_t timer_expires=UINT64_MAX; //show time when timer_item is set to expire

	uint64_t finalize_expires=UINT64_MAX; //time when input is forcibly finalized if not getting FIN from peer
	uint64_t input_ack_expires=UINT64_MAX; //time when pure ACK is sent to peer if no any output occured (normally ACK after input is delayed for the change to be combined with some output)

	uint8_t random[IOT_MESHPROTO_TUNSTREAM_IDLEN]; //random session identifier
	byte_fifo_buf buffer_in;
	byte_fifo_buf buffer_out;
//	iot_netproto_session_mesh* output_inprogress[IOT_MESHTUN_MAXDISTRIBUTION]; //list of mesh sessions which now do actual output of data.
																			//their prop current_outmeshtun_segment contains exact ranges of data being written

//	uint32_t used_buffer_in; //how many bytes of input data is currently unread in buffer_in. maxseen_sequenced_in-used_buffer_in gives position in stream corresponding to writepos of buffer_in
	uint32_t peer_rwnd; //receive window size of peer. obtained together with each ack_sequenced_out and determines maximum of sequenced_out-ack_sequenced_out. write must stop if this difference is >= than peer_rwnd
	volatile std::atomic<uint32_t> local_rwnd; //local receive window size to tell it to peer
	uint32_t creation_time; //timestamp - 1e9 in seconds when connection was created by client side (by clock of client) or accepted by server side (by clock of server)
//	uint32_t acception_time; //timestamp - 1e9 in seconds when connection was accepted by server side (by clock of server)

	uint64_t total_output; //total outputed bytes (unacknowledged). total_output+1 (for SYN bit) must == sequenced_out, FIN bit adds one more.
	uint64_t total_input; //total inputed bytes (can have holes)
	uint64_t initial_sequence_out; //starting output sequence number used in SYN packet. it is added to outgoing data_sequence when forming packets and substracted from incoming ack_sequence values received from peer.
	uint64_t initial_sequence_in; //starting input sequence number received in peer's SYN packet. it is substracted from incoming data_sequence values received from peer and added to ack_sequence when forming packets


	uint64_t ack_sequenced_out; //0-based greatest acknowledged by peer output sequence number. readpos of buffer_out corresponds to this position in stream
	uint64_t sequenced_out; //0-based greatest used output sequence number - 1 (so is next value to use). >= ack_sequenced_out. sequenced_out-ack_sequenced_out<=buffer_out.pending_read() (and peer_rwnd?)
	uint64_t ack_sequenced_in; //0-based greatest acknowledged to peer input sequence number without holes.
	uint64_t sequenced_in; //0-based greatest received input sequence number without holes. writepos of buffer_out corresponds to this position in stream
	uint64_t maxseen_sequenced_in; //0-based maximum seen (and saved)  incoming sequence number. ==sequenced_in when no holes (lost packets), >sequenced_in when there are holes
									//matches end of last sequenced_in_dis[] block

	uint64_t sequenced_in_dis[IOT_MESHTUNSTREAM_MAXHOLES]; //ordered array of disordered (not adjusent to sequenced_in) blocks of incoming data
												//incoming hole means that writepos of buffer_in was not updated, but write was made after it.
	uint32_t sequenced_in_dis_len[IOT_MESHTUNSTREAM_MAXHOLES];//length of disordered block of incoming data. for last block next must be true:
													//sequenced_in_dis[num_inholes-1]+sequenced_in_dis_len[num_inholes-1]==maxseen_sequenced_in

	struct output_inprogress_t {
		iot_netproto_session_mesh *meshses; //non-NULL if this block is being written. meshses->current_outmeshtun_segment must point to this struct
		uint64_t seq_pos;
		uint64_t retry_after; //keeps time (according to uv_now()) when is_pending must be set to true if is_ack==0
		uint32_t seq_len; //length of sequence covered by this item. it is equal to data len plus 1 for SYN (has_syn) and 1 for FIN (has_fin) bits
		uint8_t has_syn:1,
				has_fin:1,
				is_ack:1,	//true if this block is ACKed
				is_pending:1; //true is this block must be outputted as soon as possible
		uint8_t retries; //how many output retries was made
//		uint8_t had_state; //what state had current_outmeshtun when meshses began writing
	} output_inprogress[IOT_MESHTUNSTREAM_MAXOUTPUTSPLIT]; //list of blocks of output between ack_sequenced_out and sequenced_out which should be written, are being written, awaiting ACK or already ACKed with hole between ack_sequenced_out

//	uint64_t sequenced_out_sack[IOT_MESHTUNSTREAM_MAXHOLES]; //ordered array of disordered (not adjusent to ack_sequenced_out.) acknowledged blocks of sent data
//													//outgoing holes (which are between SACKed ranges) are created when peer does ACK with holes (selective ACK, SACK),
//													//so they describe ranges of output sequence number which were not received (acknowledged) by peer and must be
//													//resent if not acknowledged within reasonable time comes
//													//(TODO such timer)
//	uint32_t sequenced_out_sack_len[IOT_MESHTUNSTREAM_MAXHOLES];//length of SACKed block of sent data. for last block next must be true:
//													//sequenced_out_sack[num_outholes-1]+sequenced_out_sack_len[num_inholes-1]==sequenced_out 

	uint32_t timer_period_id=0xffffffff;

//	uint8_t in_dis_ack[IOT_MESHTUNSTREAM_MAXHOLES]; //flag that sequence range corresponding to sequenced_in_dis was acknowledged to peer using SACK ranges
//	uint8_t out_hole_pending[IOT_MESHTUNSTREAM_MAXHOLES+1]; //flag that hole in front of corresponding sequenced_out_sack range must be resent to peer (is set
															//after some time (TODO) after getting most old (since reseting this flag) ACK not acknowledging
															//particular hole). out_hole_pending[num_out_sack] is related to hole after last sequenced_out_sack range
															//if such hole exists. in particular when num_out_sack==0 it is related to single hole between ack_sequenced_out
															//and sequenced_out.

	uint8_t num_in_dis:4, //number of items in sequenced_in_dis[], sequenced_in_dis_len[], in_dis_ack[])
			num_output_inprog:4, //number of items in sequenced_out_sack[], sequenced_out_sack_len[], out_hole_pending[+1]
			input_finalized=0;		//when sequenced_in > 0 (and thus SYN was received) shows if STREAM_FIN request was received and maxseen_sequenced_in cannot grow.


//	uint8_t syn_retries:3; //number of made syn retries
//			_reserv:6; //number of items in output_inprogress[]
//	enum timer_type_t : uint8_t {
//		TIMER_NONE=0,
//		TIMER_SYNRETRY
//	} timer_type;

public:
	iot_meshtun_stream_state(iot_netcon_mesh* con_, iot_meshnet_controller* controller, bool is_passive, uint8_t inbuf_size_power, char* inbuf, uint8_t outbuf_size_power, char* outbuf)
			: iot_meshtun_state(controller, TUN_STREAM, is_passive)
	{
		bool rval=buffer_in.setbuf(inbuf_size_power, inbuf);
		assert(rval);
		rval=buffer_out.setbuf(outbuf_size_power, outbuf);
		assert(rval);
		attach_netcon(con_);

		timer_item.init(this, [](void *stp, uint32_t period_id, uint64_t now_ms)->void {
			iot_meshtun_stream_state *st=(iot_meshtun_stream_state *)stp;
			st->on_timer(period_id, now_ms);
		});
//		timer_type=TIMER_NONE;
	}
	~iot_meshtun_stream_state() {
//		timer_type=TIMER_NONE;
		timer_item.unschedule();
	}

	int accept_connection(iot_meshtun_stream_listen_state::listenq_item* item, const iot_netprototype_metaclass* protometa, uint16_t local_port, void* inmeta, uint16_t inmeta_size_);

	int connect(const iot_netprototype_metaclass* protometa, iot_hostid_t remote_host, uint16_t remote_port, bool auto_local_port=true, uint16_t local_port=0);

	void keepalive_check(uint64_t now) { //schedules async keepalive check if appropriate
		lock();
		if(state==ST_ESTABLISHED && input_ack_expires==UINT64_MAX) { //timer is not scheduled already
			input_ack_pending=1; //mark that ACK must be sent
			input_ack_expires=now;
			set_timer(now, 0);
		}
		unlock();
	}

	const iot_objref_ptr<iot_peer>& get_peer(void) const {
		return remote_peer;
	}
	iot_hostid_t get_peer_id(void) const;
	uint16_t get_remote_port(void) const {
		if(!is_bound) return 0;
		return connection_key.remoteport;
	}
	bool is_writable(void) { //should be called from netcon thread
		assert(con!=NULL);
		if(output_closed) return false; //this var can become true just after this check but this is not harmful (it shouldn't)
		if(buffer_in.avail_write()>0) return true;
		return false;
	}

//	ssize_t do_write(const void *databuf, size_t datalen) { //should be called from netcon thread
	ssize_t do_write(iovec *databufvec, int veclen, int *vecused=NULL, size_t *offsetused=NULL) { //should be called from netcon thread
		//on success returns value which is 0 <= value <= datalen
		//on error negative error core:
		//	IOT_ERROR_INVALID_ARGS
		//	IOT_ERROR_STREAM_CLOSED
		//	IOT_ERROR_NO_PEER
		//	IOT_ERROR_TRY_AGAIN - buffer is full. try again when signal WRITEREADY arrives
		//	IOT_ERROR_NO_ROUTE
		//	IOT_ERROR_CONN_RESET
		//	IOT_ERROR_ACTION_TIMEOUT - connection was broken because of timeout waiting for ACK
		assert(con!=NULL);
		uint32_t sz=0;
		if(!databufvec || veclen<0) return IOT_ERROR_INVALID_ARGS;
		if(output_closed) goto onerr;
		int i;
		if(offsetused) *offsetused=0;
		output_wanted=0;
		for(i=0; i<veclen; i++) {
			uint32_t dsz=buffer_out.write(databufvec[i].iov_base, databufvec[i].iov_len);
			sz+=dsz;
			if(dsz<databufvec[i].iov_len) {
				output_wanted=1; //not full write, netcon must be notified when write space available
				if(!sz) return IOT_ERROR_TRY_AGAIN; //no space in buf, wait for write readiness signal
				if(offsetused) *offsetused=dsz;
				break;
			}
		}
		if(vecused) *vecused=i;
		if(!sz) return 0; //no error but empty output provided

		lock();
		if(!output_closed) {
			output_pending=1;
			check_output_inprogress();
			unlock();
			return sz;
		}
		unlock();
onerr:
		int err;
		if((err=get_error())) return err;
		if(state==ST_ESTABLISHED) return IOT_ERROR_STREAM_CLOSED;
		return IOT_ERROR_NO_PEER;
	}

	ssize_t do_read(void *databuf, size_t datalen) { //should be called from netcon thread
		//on success returns value which is 0 <= value <= datalen
		//on error negative error core:
		//	IOT_ERROR_INVALID_ARGS
		//	IOT_ERROR_NO_PEER
		//	IOT_ERROR_TRY_AGAIN - read buffer is empty. try again when signal WRITEREADY arrives
		//	IOT_ERROR_NO_ROUTE
		//	IOT_ERROR_CONN_RESET
		//	IOT_ERROR_ACTION_TIMEOUT - connection was broken because of timeout waiting for ACK
		assert(con!=NULL);
		if(!databuf) return IOT_ERROR_INVALID_ARGS;
		if(input_closed) goto onerr;
		input_wanted=1;

		uint32_t sz;
		sz=buffer_in.read(databuf, datalen);
		if(!sz) return IOT_ERROR_TRY_AGAIN;

		local_rwnd.fetch_add(sz, std::memory_order_relaxed); //increase receive window if readpos is moved

		return sz;

onerr:
		int err;
		if((err=get_error())) return err;
		if(state==ST_ESTABLISHED) return 0;
		return IOT_ERROR_NO_PEER;
	}

private:
	void on_timer(uint32_t period_id, uint64_t now_ms) {
		lock();

//outlog_debug_meshtun("TIMER signalled, type=%u, got per_id=%u, must have per_id=%u", unsigned(timer_type), unsigned(period_id), unsigned(timer_period_id));
outlog_debug_meshtun("TIMER signalled for meshtun to HOST %" IOT_PRIhostid, is_bound ? connection_key.host : 0);

		uint64_t acttime; //greater among now_ms and timer_expires gives actual time which determines event activation
		uint64_t mintime; //gives minimal among future events

		if(timer_period_id!=period_id || timer_expires==UINT64_MAX) goto onexit; //this is cancelled timer invocation (cancelled/updated after point of non-prevention)

/*		switch(timer_type) {
			case TIMER_NONE:
				break;
			case TIMER_SYNRETRY:
				if(syn_retries<3) {
					set_timer(TIMER_SYNRETRY);
					on_state_update(UPD_WRITEREADY);
				} else {
					timer_type=TIMER_NONE;
					set_error(IOT_ERROR_ACTION_TIMEOUT);
				}
				break;
		}*/
		acttime=timer_expires>=now_ms ? timer_expires : now_ms; //greater among now_ms and timer_expires gives actual time which determines event activation
		mintime=UINT64_MAX; //gives minimal among future events

		bool check_output;
		check_output=false;
		for(uint16_t i=0; i<num_output_inprog; i++) {
			if(output_inprogress[i].is_ack) {
				assert(!output_inprogress[i].meshses);
				continue;
			}
			if(output_inprogress[i].retry_after>acttime) { //event in future or disabled
				if(output_inprogress[i].retry_after<mintime) mintime=output_inprogress[i].retry_after;
				continue;
			}
			//fired event
			output_inprogress[i].retry_after=UINT64_MAX; //disable event
			//process event
			if(output_inprogress[i].meshses) {
				output_inprogress[i].meshses->current_outmeshtun_segment=-1; //cancel for mesh session
				output_inprogress[i].meshses=NULL;
			}
			output_inprogress[i].is_pending=1;
			assert(output_ack_pending);
			check_output=true;
		}
		if(finalize_expires>acttime) { //event in future or disabled
			if(finalize_expires<mintime) mintime=finalize_expires;
		} else { //finalize timer fired
			finalize_expires=UINT64_MAX; //disable timer
			input_finalized=1;
			check_output=true;
		}

		if(input_ack_expires>acttime) { //event in future or disabled
			if(input_ack_expires<mintime) mintime=input_ack_expires;
		} else { //input ack timer fired
outlog_debug_meshtun("input ACK event fired");
			input_ack_expires=UINT64_MAX; //disable timer
			if(input_ack_pending) check_output=true;
		}

		timer_expires=mintime;
		if(mintime<UINT64_MAX) { //reschedule timer for existing future events
outlog_debug_meshtun("TIMER rescheduled after %u ms for meshtun to HOST %" IOT_PRIhostid, unsigned(mintime-now_ms), is_bound ? connection_key.host : 0);
			assert(mintime>now_ms);
			int err=controller->timer.schedule(timer_item, mintime-now_ms, &timer_period_id);
			assert(err==0);
		}

		if(check_output) on_state_update(UPD_WRITEREADY);
onexit:
		unlock();
	}
	void reset_timer(uint64_t now) { //scans all timer consumers to determine nearest event time and reschedule timer
		//MUST BE CALLED under lock()
		assert(uv_thread_self()==lockowner);

		uint64_t mintime=UINT64_MAX;
		//check output retries
		for(uint16_t i=0; i<num_output_inprog; i++) {
			if(output_inprogress[i].is_ack) continue;
			if(output_inprogress[i].retry_after<mintime) mintime=output_inprogress[i].retry_after;
		}
		if(finalize_expires<mintime) mintime=finalize_expires;
		if(input_ack_expires<mintime) mintime=input_ack_expires;

		if(timer_expires==mintime) return; //no change in timer schedule
		if(mintime==UINT64_MAX) {
			cancel_timer();
			return;
		}
		uint32_t delay = mintime<=now ? 0 : mintime-now;

		timer_expires=mintime;
outlog_debug_meshtun("TIMER rescheduled after %u ms for meshtun to HOST %" IOT_PRIhostid, unsigned(delay), is_bound ? connection_key.host : 0);
		int err=controller->timer.schedule(timer_item, delay, &timer_period_id);
		assert(err==0); //must not have concurrent problem because of single-thread use of current function
	}
	void cancel_timer(void) {
		//MUST BE CALLED under lock()
		assert(uv_thread_self()==lockowner);

//		timer_type=TIMER_NONE;
		timer_item.unschedule();
		timer_expires=UINT64_MAX;
outlog_debug_meshtun("TIMER cancelled for meshtun to HOST %" IOT_PRIhostid, is_bound ? connection_key.host : 0);
	}
//	void set_timer(timer_type_t tp) {
	void set_timer(uint64_t now, uint32_t delay_ms) { //now must be taken from uv_now()
		//MUST BE CALLED under lock()
		assert(uv_thread_self()==lockowner);

outlog_debug_meshtun("TIMER set to expire after %u ms for meshtun to HOST %" IOT_PRIhostid, unsigned(delay_ms), is_bound ? connection_key.host : 0);
		if(now+delay_ms>=timer_expires) return;

		int err=controller->timer.schedule(timer_item, delay_ms, &timer_period_id);
		assert(err==0); //must not have concurrent problem because of single-thread use of current function
		timer_expires=now+delay_ms;
	}

	output_inprogress_t* lock_output_inprogress(iot_netproto_session_mesh* ses, uint32_t rtt_us, uint8_t max_retries, output_inprogress_t &inputack_inprog) {
		//MUST BE CALLED under lock() in sessions thread (to access loop)
		//returns false if block with pending output not found
		//inputack_inprog must provide buffer for returning fake output_inprogress_t suitable for sending pure ACK when input ACK is pending
		assert(uv_thread_self()==lockowner);
		assert(uv_thread_self()==ses->thread->thread); 

		if(!input_ack_pending && !output_ack_pending) return NULL; //there is something to out OR not all output was ACKed (and thus can be repeated)
		assert(ses && !ses->current_outmeshtun);

		//try to find block with pending output
		uint8_t i;
		for(i=0; i<num_output_inprog; i++) {
			if(!output_inprogress[i].is_pending) continue;
			assert(!output_inprogress[i].is_ack);

			if(ack_sequenced_out==0 && output_inprogress[i].seq_pos>1) break; //while our SYN is not ACKed, allow to send only first chunk of output (SYN can be prepended to it) or to repeat SYN

			if(output_inprogress[i].meshses) { //is being processed (being writed) by another session
				assert(output_inprogress[i].meshses!=ses);
				continue;
			}
			if(output_inprogress[i].retries>=max_retries) {
				set_error(IOT_ERROR_ACTION_TIMEOUT);
				return NULL;
			}
			uint64_t now=uv_now(ses->thread->loop);

			if(output_inprogress[i].has_syn) {
				if(rtt_us<100'000) rtt_us=100'000; //ensure SYN has larger minimum timeout
			} else {
				if(rtt_us<10'000) rtt_us=10'000;
			}

			uint32_t delay=(rtt_us*3)*(1u << output_inprogress[i].retries)/1000;
			if(delay<IOT_MESHNET_TIMER_PREC) delay=IOT_MESHNET_TIMER_PREC;
				else if(delay>10000) delay=10000;
			output_inprogress[i].retry_after=now+delay;
			output_inprogress[i].retries++;
			output_inprogress[i].meshses=ses;
			ses->current_outmeshtun=this;
			ses->current_outmeshtun_segment=i;
			if(output_inprogress[i].retries==2) {
				outlog_debug_meshtun("ku");
			}
			set_timer(now, delay);
outlog_debug_meshtun("PENDING OUTPUT FOR BLOCK %d: offset=%u, len=%u, seq_out=%llu, ack_out=%llu, retries=%d", int(i), unsigned(output_inprogress[i].seq_pos), output_inprogress[i].seq_len, sequenced_out, ack_sequenced_out, int(output_inprogress[i].retries));
			return &output_inprogress[i];
		}
		if(input_ack_pending) {
			inputack_inprog={
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
			ses->current_outmeshtun=this;
			ses->current_outmeshtun_segment=-1;
			return &inputack_inprog;
		}
		return NULL;
	}

	void unlock_output_inprogress(iot_netproto_session_mesh* ses, bool clear_pending) {
		//MUST BE CALLED under lock()
		assert(uv_thread_self()==lockowner);
		assert(uv_thread_self()==ses->thread->thread); 
		assert(ses && ses->current_outmeshtun==this);

		auto idx=ses->current_outmeshtun_segment; //this var is accessed under lock of current_outmeshtun only!!!
		if(idx<0) return; //locking was cancelled outside session thread OR it was pure output of input ACK

		if(idx<num_output_inprog) {
			if(output_inprogress[idx].meshses==ses) {
				output_inprogress[idx].meshses=NULL;
				ses->current_outmeshtun_segment=-1;
outlog_debug_meshtun("OUTPUT BLOCK %d %s", int(idx), clear_pending ? "non-pending" : "will retry");
				if(clear_pending) output_inprogress[idx].is_pending=0; //pending should be cleared if output was successful OR critical error is set or to be set
				else { //session broken? retry immediately. revert previous try
					if(output_inprogress[idx].retries>0 && state!=ST_RESET) output_inprogress[idx].retries--;
					output_inprogress[idx].retry_after=UINT64_MAX;
				}
				return;
			} else {
				assert(false);
			}
		} else {
			assert(false);
		}
	}
	//creates new output_inprogress (after sequenced_out increase), schedules output of pending blocks
	//must be called after:
	//- outputting of some block
	void check_output_inprogress(void); //MUST BE CALLED under lock()

/*
	//checks if specific range of output sequence was acknowledged by peer
	bool is_sequence_out_ACKed(uint64_t seq_out, uint32_t len) {
		seq_out+=len;
		return seq_out<=ack_sequenced_out;
	}

	//merges in provided outbound sequence range [start; start+len)
	bool sequence_out_merge(uint64_t start, uint32_t len) {
		if(start>sequenced_out) {
			assert(false);
			//hole provided. must not happen
			return false;
		}
		//here start <= sequenced_out, so only end in meaningful
		start+=len;
		if(start>sequenced_out) sequenced_out=start;
		return true;
	}
*/

	//merges in provided inbound data and sequence range [start; start+len)
	void indata_merge(uint64_t seq_start, uint32_t datalen, const char* data, uint32_t has_fin) { //
		uint64_t diff;
		uint16_t i;
		if(seq_start<sequenced_in) {
			diff=sequenced_in-seq_start;
			if(diff>datalen) return; //this is repetition or invalid range. ignore.
			datalen-=diff;
			data+=diff;
			seq_start=sequenced_in;
		}
		if(input_finalized) { //maxseen_sequenced_in cannot be increased
			if(seq_start>=maxseen_sequenced_in /*this is protection from overflow*/ || (seq_start+datalen)+has_fin>maxseen_sequenced_in) {
				assert(false); //got data outside of stream range. ignore.
				return;
			}
		} else if(has_fin) { //contains FIN so end of provided range cannot be less than current maxseen_sequenced_in
			if((seq_start+datalen)+1 < maxseen_sequenced_in) {
				assert(false); //got invalid range
				return;
			}
		}
		uint16_t dis_idx=0; //index in sequenced_in_dis[] and sequenced_in_dis_len[]
		if(seq_start==sequenced_in) { //contiguous range continues. will move sequenced_in and writepos of buffer_in
			uint64_t old_sequenced_in=sequenced_in;
			uint64_t new_sequenced_in=sequenced_in+datalen;
			
			for(; dis_idx<num_in_dis && new_sequenced_in>=sequenced_in_dis[dis_idx]; dis_idx++) {
				assert(sequenced_in_dis_len[dis_idx]>0);
				assert(sequenced_in_dis[dis_idx]+sequenced_in_dis_len[dis_idx]<=maxseen_sequenced_in);

				//this dis-range must be removed
				uint32_t holelen=sequenced_in_dis[dis_idx]-sequenced_in;
				assert(holelen>0 && holelen<=datalen); //disordered ranges must have holes before them

				//fill hole with incoming data
				uint32_t sz=buffer_in.write(data, holelen);
				assert(sz==holelen); //there is data over current writepos, so write must return exactly requested size
				data+=holelen;
				datalen-=holelen;
				sequenced_in+=holelen;

				//skip already received data

				if(sequenced_in_dis[dis_idx]+sequenced_in_dis_len[dis_idx]<new_sequenced_in+has_fin) { //dis range is fully covered by new_sequenced_in, so whole len is skipped
					sz=buffer_in.write(NULL, sequenced_in_dis_len[dis_idx]);
					assert(sz==sequenced_in_dis_len[dis_idx]);
					sequenced_in+=sequenced_in_dis_len[dis_idx];
					assert(sequenced_in==sequenced_in_dis[dis_idx]+sequenced_in_dis_len[dis_idx]);

					data+=sequenced_in_dis_len[dis_idx];
					assert(datalen>=sequenced_in_dis_len[dis_idx]);
					datalen-=sequenced_in_dis_len[dis_idx];
				} else { //datalen is ended inside of this dis-range. if has_fin, then datalen must be exactly 1 step before end of dis-range (case of equality will be in previous if())
					assert(new_sequenced_in-sequenced_in_dis[dis_idx]==datalen);
					assert(!has_fin || datalen==sequenced_in_dis_len[dis_idx]-1);

					sz=buffer_in.write(NULL, sequenced_in_dis_len[dis_idx]-has_fin);
					assert(sz==sequenced_in_dis_len[dis_idx]-has_fin);

					sequenced_in+=sequenced_in_dis_len[dis_idx]-has_fin;

					datalen=0;
					dis_idx++;
					break;
				}
			}
			if(datalen>0) { //there is data for a hole
				uint32_t sz=buffer_in.write(data, datalen);
				if(sz<datalen) {
					assert(dis_idx==num_in_dis); //write can be partial if writing after last dis-range only
					has_fin=0; //not all data bytes were saved, so FIN must be dropped in any case
				}
				sequenced_in+=sz;
				datalen-=sz;
			}
			//process fin
			if(has_fin) {
				assert(dis_idx==num_in_dis);
				sequenced_in++;
				assert(maxseen_sequenced_in<=sequenced_in);
				maxseen_sequenced_in=sequenced_in;
				input_finalized=1;
				input_closed=1;
				if(finalize_expires!=UINT64_MAX) {
					finalize_expires=UINT64_MAX;
				}
			} else {
				if(sequenced_in>maxseen_sequenced_in) maxseen_sequenced_in=sequenced_in;
				if(input_finalized && sequenced_in==maxseen_sequenced_in) {
					input_closed=1;
				}
			}

			//dis_idx items can be removed from sequenced_in_dis
			if(dis_idx>0) {
				if(dis_idx==num_in_dis) num_in_dis=0;
				else {
					memmove(sequenced_in_dis, sequenced_in_dis+dis_idx, (num_in_dis-dis_idx)*sizeof(sequenced_in_dis[0]));
					memmove(sequenced_in_dis_len, sequenced_in_dis_len+dis_idx, (num_in_dis-dis_idx)*sizeof(sequenced_in_dis_len[0]));
					num_in_dis-=dis_idx;
				}
			}
			if(sequenced_in==old_sequenced_in) return; //receive window won't change
			on_state_update(UPD_READREADY);
			goto recalc_rwnd;
		}
		//seq_start>sequenced_in, there is or will be hole after sequenced_in



		//here we need to merge data range with existing dis-ranges, not touching sequenced_in
		uint64_t seq_end;
		seq_end=seq_start+datalen+has_fin;
		for(; dis_idx<num_in_dis && sequenced_in_dis[dis_idx]+sequenced_in_dis_len[dis_idx]<seq_start; dis_idx++) { //skip dis-ranges which are before data range
			assert(sequenced_in_dis_len[dis_idx]>0);
			assert(sequenced_in_dis[dis_idx]+sequenced_in_dis_len[dis_idx]<=maxseen_sequenced_in);
		}

		if(dis_idx>=num_in_dis || seq_end<sequenced_in_dis[dis_idx]) { //no intersection. new hole must be created at current position in sequenced_in_dis[]
			if(num_in_dis>=IOT_MESHTUNSTREAM_MAXHOLES) return; //no new holes can be created. just ignore packet

			if(dis_idx<num_in_dis) {
				if(has_fin) { //data range with FIN MUST be the last
					assert(false);
					return;
				}

				memmove(sequenced_in_dis+dis_idx+1, sequenced_in_dis+dis_idx, (num_in_dis-dis_idx)*sizeof(sequenced_in_dis[0]));
				memmove(sequenced_in_dis_len+dis_idx+1, sequenced_in_dis_len+dis_idx, (num_in_dis-dis_idx)*sizeof(sequenced_in_dis_len[0]));

				uint32_t sz=buffer_in.poke(data, datalen, seq_start-sequenced_in);
				assert(sz==datalen); //there is data over current writepos, so write must return exactly requested size

				sequenced_in_dis[dis_idx]=seq_start;
				sequenced_in_dis_len[dis_idx]=datalen;
			} else { //adding new dis-range at end. size of buffer in limits range which can be created
				uint32_t sz=buffer_in.poke(data, datalen, seq_start-sequenced_in);
				if(!sz) return; //no input bytes can be saved. receive window overflowed. just ignore
				if(sz<datalen) has_fin=0; //not all data bytes were saved, so FIN must be dropped

				sequenced_in_dis[dis_idx]=seq_start;
				sequenced_in_dis_len[dis_idx]=sz+has_fin;
			}
			num_in_dis++;
		} else { //ranges have intersection or touch and can be combined
			uint64_t right=sequenced_in_dis[dis_idx]+sequenced_in_dis_len[dis_idx]; //get right edge of present range

			if(seq_start<sequenced_in_dis[dis_idx]) { //extend left edge of dis-range and fill hole with data
				//fill hole with incoming data
				uint32_t holelen=sequenced_in_dis[dis_idx]-seq_start;
				assert(datalen>=holelen);
				uint32_t sz=buffer_in.poke(data, holelen, seq_start-sequenced_in);
				assert(sz==holelen); //there is data over current writepos, so write must return exactly requested size

				sequenced_in_dis[dis_idx]=seq_start;
				sequenced_in_dis_len[dis_idx]+=holelen;
			}
			//here seq_start>=sequenced_in_dis[dis_idx]
			if(seq_end>right) { //right edge of current data range is greater
				//will extend length of dis-range till seq_end or further. holes must be filled while extending
				uint32_t deltalen=right-seq_start;
				assert(datalen>=deltalen); //has_fin is accounted here for '=' sign
				datalen-=deltalen;
				data+=deltalen;

//				sequenced_in_dis_len[dis_idx]=uint32_t(seq_end-sequenced_in_dis[dis_idx]);
				//check if new right edge spans neighbour sequenced_in_dis items
				for(i=dis_idx+1; i<num_in_dis && seq_end>=sequenced_in_dis[i]; i++) { //while next range is touched/covered by data range
					assert(sequenced_in_dis_len[i]>0);
					assert(sequenced_in_dis[i]+sequenced_in_dis_len[i]<=maxseen_sequenced_in);
							
					uint32_t holelen=sequenced_in_dis[i]-right;
					assert(holelen>0 && holelen<=datalen); //disordered ranges must have holes before them

					//fill hole with incoming data
					uint32_t sz=buffer_in.poke(data, holelen, right-sequenced_in);
					assert(sz==holelen); //there is data over current writepos, so write must return exactly requested size
					data+=holelen;
					datalen-=holelen;

					right=sequenced_in_dis[i]+sequenced_in_dis_len[i];

					if(seq_end<=right) { //data range ends inside existing dis-order, so cannot influence anything accept rwnd size
						datalen=0;
						has_fin=0;
						i++;
						break;
					}
					assert(datalen>=sequenced_in_dis_len[i]); //has_fin is accounted here for '=' sign
					datalen-=sequenced_in_dis_len[i];
					data+=sequenced_in_dis_len[i];
				}
				if(datalen>0) {
					uint32_t sz=buffer_in.poke(data, datalen, right-sequenced_in);
					if(sz<datalen) {
						assert(i==num_in_dis); //write can be partial if writing after last dis-range only
						has_fin=0; //not all data bytes were saved, so FIN must be dropped
					}
					right+=sz;
				}
				if(has_fin) {
					assert(i==num_in_dis); //fin can be set in last dis-range only
					right++;
				}
				sequenced_in_dis_len[dis_idx]=right-sequenced_in_dis[dis_idx];

				if(i>dis_idx+1) { //remove unnecessary dis-ranges
					if(i==num_in_dis) num_in_dis=dis_idx+1; //all the rest must be spliced
					else {
						memmove(sequenced_in_dis+dis_idx+1, sequenced_in_dis+i, (num_in_dis-i)*sizeof(sequenced_in_dis[0]));
						memmove(sequenced_in_dis_len+dis_idx+1, sequenced_in_dis_len+i, (num_in_dis-i)*sizeof(sequenced_in_dis_len[0]));
						num_in_dis-=i-(dis_idx+1);
					}
				}
			} else { //right>=seq_end, data range ends inside existing dis-order, so cannot influence anything accept rwnd size
				has_fin=0;
			}
		}

		assert(num_in_dis>0);
		uint64_t right;
		right=sequenced_in_dis[num_in_dis-1]+sequenced_in_dis_len[num_in_dis-1];
		if(right>maxseen_sequenced_in) {
			assert(!input_finalized);
			maxseen_sequenced_in=right;
		}
		if(has_fin) input_finalized=1;

recalc_rwnd:
		//receive window is all space after write_pos (which corresponds to sequenced_in) with substraction of on_dis blocks
		uint32_t rwnd=buffer_in.avail_write();
		if(num_in_dis>0) {
			rwnd+=input_finalized; //if input is finalized, then last in_dis block has length inclusive with FIN bit and it will be substracted below
			for(i=0; i<num_in_dis; i++) {
				if(rwnd<sequenced_in_dis_len[i]) {
					assert(false);
					rwnd=0;
					break;
				}
				rwnd-=sequenced_in_dis_len[i];
			}
		}
		local_rwnd.store(rwnd, std::memory_order_relaxed);
	}


	//merges in ACKs from peer updating ack_sequenced_out and output_inprogress[]
	void ack_sequence_out_merge(uint64_t ack, int16_t num_sack, uint64_t *sack_seq, uint32_t* sack_len, uint64_t now_ms) {
		int16_t inprog_idx=0; //holds current running index in output_inprogress
		int16_t req_idx=0; //holds current running index in sack_seq
		int16_t i;
		int16_t inprog_num=num_output_inprog; //make it signed

		uint64_t new_ack_sequenced_out=ack_sequenced_out;

		//first block must be unACKed and exactly at ack_sequenced_out, last block must end at sequenced_out
		assert((inprog_num==0 && new_ack_sequenced_out==sequenced_out) || 
			(inprog_num>0 && !output_inprogress[0].is_ack && output_inprogress[0].seq_pos==new_ack_sequenced_out && output_inprogress[inprog_num-1].seq_pos+output_inprogress[inprog_num-1].seq_len==sequenced_out));

		//first merge continious ACKs with ranges
		bool restart_timer=false; //any timers were cancelled, so must be recalculated
		do { //repeat cycle until new_ack_sequenced_out and ack becomes equal
			if(ack>new_ack_sequenced_out) { //new_ack_sequenced_out must be moved forward. check existing inprogress blocks if they can be removed or updated
				new_ack_sequenced_out=ack;
				//all inprog block whose end <= ack can be removed
				for(;inprog_idx<inprog_num-1 && ack>=output_inprogress[inprog_idx+1].seq_pos;inprog_idx++) {
					//use the fact that start of next block is at end of previous, ensure no bugs
					assert(output_inprogress[inprog_idx+1].seq_pos==output_inprogress[inprog_idx].seq_pos+output_inprogress[inprog_idx].seq_len);
				}
				for(;inprog_idx<inprog_num && ack>=output_inprogress[inprog_idx].seq_pos+output_inprogress[inprog_idx].seq_len;inprog_idx++);

				if(inprog_idx>=inprog_num) break;
				//here ack DOES NOT reach end of block

				//ACKed block whose start is <= ack will move new_ack_sequenced_out till its end. then it can be removed too
				if(output_inprogress[inprog_idx].is_ack) {
					if(ack>=output_inprogress[inprog_idx].seq_pos) { //ack touches start of ACKed block, so new_ack_sequenced_out can be moved forward
						new_ack_sequenced_out=output_inprogress[inprog_idx].seq_pos+output_inprogress[inprog_idx].seq_len;
						inprog_idx++;
						continue;
					}
				} //else process possible partial ACK
				else if(ack>output_inprogress[inprog_idx].seq_pos) {
					//causes problems if ack is not exactly at start. need to split this block so it could be partially ACKed
					outlog_debug_meshtun("GOT PARTIAL ACK");
					auto &cur=output_inprogress[inprog_idx];
					uint32_t deltaacklen=uint32_t(ack-cur.seq_pos); //part of this block which is ACKed
					cur.seq_pos+=deltaacklen;
					assert(cur.seq_len>deltaacklen);
					cur.seq_len-=deltaacklen;
					cur.has_syn=0; //if this block has syn, it could be at first offset only, so now surely gone
					cur.retries=0; //partial ACK from left can mean that receive window of peer was too small for whole block, thus rest of data must be retried again
					if(!cur.is_pending) { //make it pending immediately
						assert(cur.meshses==NULL);
						cur.is_pending=1;
						cur.retry_after=UINT64_MAX;
						restart_timer=true;
					}
					//meshses remain unchanged
				}
			} else if(ack<new_ack_sequenced_out) { //new_ack_sequenced_out is greater, so check if sack_seq[] items can be removed (by increasing req_idx)
				ack=new_ack_sequenced_out;
				for(; req_idx<num_sack && new_ack_sequenced_out>=sack_seq[req_idx]+sack_len[req_idx]; req_idx++); //fully consumed ranges cannot update ack
				if(req_idx<num_sack && new_ack_sequenced_out>=sack_seq[req_idx]) { //not fully consumed range
					ack=sack_seq[req_idx]+sack_len[req_idx];
					req_idx++;
					continue;
				}
			} //else new_ack_sequenced_out == ack
			break;
		} while(1);

		if(req_idx>0) {
			assert(req_idx<=num_sack);
			sack_seq+=req_idx;
			sack_len+=req_idx;
			num_sack-=req_idx;
			req_idx=0; //holds current running index in sack_seq
		}

		if(inprog_idx>0) { //do actual splice of output_inprogress[0]
outlog_debug_meshtun("ACKed %d blocks", inprog_idx);
			for(i=0; i<inprog_idx; i++) { //cancel timers and mesh session reference
				if(output_inprogress[i].retry_after!=UINT64_MAX) restart_timer=true;
				if(output_inprogress[i].meshses) output_inprogress[i].meshses->current_outmeshtun_segment=-1; //cancel for mesh session
			}
			if(inprog_idx==inprog_num) inprog_num=0;
			else {
				memmove(output_inprogress, output_inprogress+inprog_idx, (inprog_num-inprog_idx)*sizeof(output_inprogress[0]));
				inprog_num-=inprog_idx;

			}
			assert(inprog_num>=0);
			inprog_idx=0;
		}

		//first block must be unACKed and exactly at new_ack_sequenced_out, last block must end at sequenced_out
		assert((inprog_num==0 && new_ack_sequenced_out==sequenced_out) || 
			(inprog_num>0 && !output_inprogress[0].is_ack && output_inprogress[0].seq_pos==new_ack_sequenced_out && output_inprogress[inprog_num-1].seq_pos+output_inprogress[inprog_num-1].seq_len==sequenced_out));

		assert(num_sack==0 || sack_seq[0]>new_ack_sequenced_out);

		////////////////////////////
		///////////////////////ADVANCING readpos of buffer_out
		////////////////////////////
		int64_t readpos_move=new_ack_sequenced_out-ack_sequenced_out;
		if(readpos_move>0) { //buffer_out space can be freed
			if(ack_sequenced_out==0) {
				readpos_move--; //sequence 0 is for SYN bit, which is not in buffer
				if(state==ST_SENDING_SYN) set_state(ST_ESTABLISHED); //ACK of sequence number 0 received
			}
			ack_sequenced_out=new_ack_sequenced_out;

			if(ack_sequenced_out==sequenced_out) { //all currently made output was ACKed
				output_ack_pending=0;
				if(output_closed) { //output was closed
					if(input_closed && sequenced_in>0 && !input_finalized && finalize_expires==UINT64_MAX) { //input is closed too (due to detach) and input was really opened, but no FIN received
						//wait sometime for FIN to do graceful close for peer
						finalize_expires=now_ms+IOT_MESHTUNSTREAM_INPUTFIN_TIMEOUT; //input_finalized will be set to true after this time
						restart_timer=true;
					}
					if(sequenced_out>1) { //output was closed after outputting SYN, so FIN must be present too
						if(readpos_move>0) {
							readpos_move--; //last sequence is for FIN bit, which is not in buffer
						} else assert(false);
					}
				}
			}

			if(readpos_move>0) { //move readpos of buffer_out. this frees some write space for netcon
				assert(readpos_move<=UINT32_MAX);
				uint32_t sz=buffer_out.read(NULL, readpos_move);
				assert(sz==readpos_move);
				on_state_update(UPD_WRITESPACEFREED);
			}
		} else assert(readpos_move==0);


		//here we need to merge ranges, not touching ack and ack_sequenced_out
		//first make ACK (with possible combining) of inprog blocks which does not require adding new blocks
		while(req_idx<num_sack && inprog_idx<inprog_num) {
			uint64_t right=output_inprogress[inprog_idx].seq_pos+output_inprogress[inprog_idx].seq_len; //get right edge of current inprog block

			//if current inprog block lies completely before current sack range (no intersection)
			if(right<=sack_seq[req_idx]) {
				//current inprog block remains unchanged, req's range must be checked against next one
				inprog_idx++;
				continue;
			}
			//must have intersection and start of req's range must be inside current inprog block (not before because if range starts in another inprog block it must have been processed on previous iteration)
			assert(output_inprogress[inprog_idx].seq_pos<=sack_seq[req_idx]);

			uint64_t right_req=sack_seq[req_idx]+sack_len[req_idx]; //get right edge of req's range

			if(output_inprogress[inprog_idx].is_ack) { //current inprog block is already ACKed and req's range is inside it (so can be removed) or lasts after it (so can be updated)
				if(right_req<=right) { //req's range is completely inside current inprog block
					sack_len[req_idx]=0; //mark range as removed
					req_idx++;
					//inprog_idx is unchanged as next req's range can intersect with it too
				} else { //update req's range left edge and reuse on next iteration agains next inprog block
					uint32_t deltalen=right-sack_seq[req_idx];
					sack_seq[req_idx]+=deltalen;
					assert(sack_len[req_idx]>deltalen);
					sack_len[req_idx]-=deltalen;
					inprog_idx++;
				}
				continue;
			}
			//here current inprog block is unACKed

			if(right_req<right) { //req's range is completely inside current inprog block
				//this is partial ACK, it requires to add new inprog block or two, process in separate loop
				req_idx++;
				continue;
			}
			//try to find any futher inprog block fully covered by current req's range (to make it ACKed) OR to touch any already ACKed block. in such case whole set of found blocks becomes ACKed


			bool found_ack=false; //flag that we found at least one fully covered block or touched ACKed block
			if(output_inprogress[inprog_idx].seq_pos==sack_seq[req_idx]) i=inprog_idx; //current inprog block is fully covered by req's range, so must become ACKed. use the same loop
				else i=inprog_idx+1;
			for(; i<inprog_num && right_req>=output_inprogress[i].seq_pos; i++) { //while req's range crosses or touches inprog block
				if(output_inprogress[i].is_ack) { //found ACKed block (crossed or touched)
					found_ack=true;
//					if(right_req<output_inprogress[i].seq_pos+output_inprogress[i].seq_len) { //it wasn't full crossed
//						break; //avoid increasing i in such case
//					}
					continue;
				}
				if(right_req>=output_inprogress[i].seq_pos+output_inprogress[i].seq_len) { //found full crossing of non ACKed
					//it becomes ACKed
					found_ack=true;
					output_inprogress[i].is_ack=1;
					output_inprogress[i].is_pending=0;
					if(output_inprogress[i].retry_after!=UINT64_MAX) {output_inprogress[i].retry_after=UINT64_MAX;restart_timer=true;}
					if(output_inprogress[i].meshses) {output_inprogress[i].meshses->current_outmeshtun_segment=-1;output_inprogress[i].meshses=NULL;} //cancel for mesh session
					continue;
				}
				//touch of non ACKed block
				if(!found_ack) {
					assert(i==inprog_idx+1); //can happen on first iteration only?
					break;
				}
				outlog_debug_meshtun("GOT PARTIAL ACK");

				//this block can be narrowed from left to mark partial ACK. ACKed block must be to the left!
				assert(output_inprogress[i-1].is_ack);
				if(right_req==output_inprogress[i].seq_pos) break; //must be some overlapping
				//it remains non-ACKed
				uint32_t deltalen=right_req-output_inprogress[i].seq_pos;
				output_inprogress[i].seq_pos+=deltalen;
				assert(output_inprogress[i].seq_len>deltalen);
				output_inprogress[i].seq_len-=deltalen;
/*				output_inprogress[i].retries=0; //partial ACK from left can mean that receive window of peer was too small for whole block, thus rest of data must be retried again
				if(!output_inprogress[i].is_pending) { //make it pending immediately
					assert(output_inprogress[i].meshses==NULL);
					output_inprogress[i].is_pending=1;
					output_inprogress[i].retry_after=UINT64_MAX;
					restart_timer=true;
				} NOT ACTUAL FOR NON CONTIGUOUS SEQUENCE*/
				output_inprogress[i-1].seq_len+=deltalen; //enlarge ACKed range
				//meshses remain
				break;
			}

			if(!found_ack) {
				//this is partial ACK, it requires to add new inprog block, process in separate loop
				req_idx++;
				inprog_idx++;
				continue;
			}

			if(!output_inprogress[inprog_idx].is_ack) { //current block can be partially ACKed by giving its end to next ACKed
				assert(i>inprog_idx+1); //loop must have finished at least one iteration
				assert(output_inprogress[inprog_idx+1].is_ack);

				outlog_debug_meshtun("GOT PARTIAL ACK");
				//narrow current block from right
				uint32_t deltalen=right-sack_seq[req_idx];
				assert(output_inprogress[inprog_idx].seq_len>deltalen);
				output_inprogress[inprog_idx].seq_len-=deltalen;
				//widen next block from left
				assert(output_inprogress[inprog_idx+1].seq_pos>deltalen);
				output_inprogress[inprog_idx+1].seq_pos-=deltalen;
				output_inprogress[inprog_idx+1].seq_len+=deltalen;
				inprog_idx++;
				//go on
			} //else current block was ACKed
			if(i-inprog_idx>1) { //number of sequential ACKed blocks > 1, so they must be combined
				assert(output_inprogress[i-1].is_ack);
				
				output_inprogress[inprog_idx].seq_len=(output_inprogress[i-1].seq_pos+output_inprogress[i-1].seq_len)-output_inprogress[inprog_idx].seq_pos;
				//remove excess block(s)
				if(i<inprog_num-i)
					memmove(output_inprogress+inprog_idx+1, output_inprogress+i, (inprog_num-i)*sizeof(output_inprogress[0]));
				inprog_num-=i-inprog_idx-1;
			}
			sack_len[req_idx]=0; //sack range was fully used
			req_idx++;
			//inprog_idx unchanged to recheck it agains new req_idx
		}
		//here all possible inprog blocks was removed

		//now do one more loop but now add new inprog block when necessary
		//only cases when req's range is fully inside unACKed inprog block OR covers two sequencial unACKed blocks must remain
		req_idx=0;
		inprog_idx=0;
		while(req_idx<num_sack && inprog_idx<inprog_num) {
			if(!sack_len[req_idx]) { //skip disabled ranges
				req_idx++;
				continue;
			}
			uint64_t right=output_inprogress[inprog_idx].seq_pos+output_inprogress[inprog_idx].seq_len; //get right edge of current inprog block

			//if current inprog block lies completely before current sack range (no intersection)
			if(right<=sack_seq[req_idx]) {
				//current inprog block remains unchanged, req's range must be checked against next one
				inprog_idx++;
				continue;
			}
			//must have intersection and start of req's range must be inside current inprog block (not before because if range starts in another inprog block it must have been processed on previous iteration)
			assert(output_inprogress[inprog_idx].seq_pos<=sack_seq[req_idx]);

			assert(!output_inprogress[inprog_idx].is_ack); //MUST NOT have instersection with ACKed blocks in this loop

			uint64_t right_req=sack_seq[req_idx]+sack_len[req_idx]; //get right edge of req's range

			if(output_inprogress[inprog_idx].seq_pos==sack_seq[req_idx]) { //left edges coinside
				assert(right_req<right); //must not be full crossing of inprog block (it must have been ACKed in previous loop)
				if(inprog_num>=IOT_MESHTUNSTREAM_MAXOUTPUTSPLIT) break; //stop in no new inprog block can be added

				//add new block to make split into ACKed and non-ACKed part
				memmove(output_inprogress+inprog_idx+1, output_inprogress+inprog_idx, (inprog_num-inprog_idx)*sizeof(output_inprogress[0]));
				inprog_num++;
				//[inprog_idx] becomes ACKed
				output_inprogress[inprog_idx].is_ack=1;
				output_inprogress[inprog_idx].is_pending=0;
				output_inprogress[inprog_idx].seq_len=sack_len[req_idx]; //seq_pos remains
				output_inprogress[inprog_idx].has_fin=0;
				output_inprogress[inprog_idx].retry_after=UINT64_MAX;
				output_inprogress[inprog_idx].meshses=NULL;
				//[inprog_idx+1] remains non-ACKed
				inprog_idx++;
				output_inprogress[inprog_idx].seq_pos+=sack_len[req_idx];
				assert(output_inprogress[inprog_idx].seq_len>sack_len[req_idx]);
				output_inprogress[inprog_idx].seq_len-=sack_len[req_idx];
				output_inprogress[inprog_idx].has_syn=0;
/*				output_inprogress[inprog_idx].retries=0; //partial ACK from left can mean that receive window of peer was too small for whole block, thus rest of data must be retried again
				if(!output_inprogress[inprog_idx].is_pending) { //make it pending immediately
					assert(output_inprogress[inprog_idx].meshses==NULL);
					output_inprogress[inprog_idx].is_pending=1;
					output_inprogress[inprog_idx].retry_after=UINT64_MAX;
					restart_timer=true;
				} NOT ACTUAL FOR NON CONTIGUOUS SEQUENCE */

				//meshses remain
				req_idx++;
				continue;
			}

			if(right_req<right) { //req's range is completely inside current inprog block, not touching edges. two new blocks must be added
				if(inprog_num+1>=IOT_MESHTUNSTREAM_MAXOUTPUTSPLIT) break; //stop in no 2 new inprog block can be added
				uint32_t len1=sack_seq[req_idx]-output_inprogress[inprog_idx].seq_pos;
				uint32_t len3=(output_inprogress[inprog_idx].seq_len-len1)-sack_len[req_idx];
				assert(sack_len[req_idx]+uint64_t(len1)+uint64_t(len3)==output_inprogress[inprog_idx].seq_len);
				//add 2 new blocks to make split into ACKed and non-ACKed part
				memmove(output_inprogress+inprog_idx+2, output_inprogress+inprog_idx, (inprog_num-inprog_idx)*sizeof(output_inprogress[0]));
				inprog_num+=2;

				//[inprog_idx] remains non-ACKed
				output_inprogress[inprog_idx].seq_len=len1;
				output_inprogress[inprog_idx].has_fin=0;
				//is_pending, retry_after, retries, meshses remain

				//[inprog_idx+1] becomes ACKed
				inprog_idx++;
				output_inprogress[inprog_idx]={
					.meshses=NULL,
					.seq_pos=output_inprogress[inprog_idx-1].seq_pos+len1,
					.retry_after=UINT64_MAX,
					.seq_len=sack_len[req_idx],
					.has_syn=0,
					.has_fin=0,
					.is_ack=1,
					.is_pending=0,
					.retries=0
				};
				//[inprog_idx+2] remains non-ACKed
				inprog_idx++;
				output_inprogress[inprog_idx].seq_pos=output_inprogress[inprog_idx-1].seq_pos+sack_len[req_idx];
				output_inprogress[inprog_idx].seq_len=len3;
				output_inprogress[inprog_idx].has_syn=0;
				output_inprogress[inprog_idx].meshses=NULL;
//NOT ACTUAL FOR NON CONTIGUOUS SEQUENCE				output_inprogress[inprog_idx].retries=0; //strange case of split. just reset retries count but let timer wait
				//is_pending, retry_after remain
				req_idx++;
				continue;
			}
			//req's range is at end of inprog block and optionally touches next unACKed block. new block must be added
			if(inprog_num>=IOT_MESHTUNSTREAM_MAXOUTPUTSPLIT) break; //stop in no new inprog block can be added

			//add new block to make split into ACKed and non-ACKed part
			memmove(output_inprogress+inprog_idx+1, output_inprogress+inprog_idx, (inprog_num-inprog_idx)*sizeof(output_inprogress[0]));
			inprog_num++;

			uint32_t deltalen=right-sack_seq[req_idx];
			//[inprog_idx] remains non-ACKed
			assert(output_inprogress[inprog_idx].seq_len>deltalen);
			output_inprogress[inprog_idx].seq_len-=deltalen; //seq_pos remains
			output_inprogress[inprog_idx].has_fin=0;
			//is_pending, retry_after, retries, meshses remain

			//[inprog_idx+1] becomes ACKed
			inprog_idx++;
			output_inprogress[inprog_idx].is_ack=1;
			output_inprogress[inprog_idx].is_pending=0;
			output_inprogress[inprog_idx].seq_pos+=deltalen;
			output_inprogress[inprog_idx].seq_len=deltalen;
			output_inprogress[inprog_idx].has_syn=0;
			output_inprogress[inprog_idx].retry_after=UINT64_MAX;
			output_inprogress[inprog_idx].meshses=NULL;

			inprog_idx++;
			if(sack_len[req_idx]>deltalen) {
				//there must be next unACKed block which can be partially ACKed
				assert(inprog_idx<inprog_num);
				assert(!output_inprogress[inprog_idx].is_ack);
				deltalen=sack_len[req_idx]-deltalen;

				output_inprogress[inprog_idx].seq_pos+=deltalen;
				assert(output_inprogress[inprog_idx].seq_len>deltalen);
				output_inprogress[inprog_idx].seq_len-=deltalen;
/*				output_inprogress[inprog_idx].retries=0; //partial ACK from left can mean that receive window of peer was too small for whole block, thus rest of data must be retried again
				if(!output_inprogress[inprog_idx].is_pending) { //make it pending immediately
					assert(output_inprogress[inprog_idx].meshses==NULL);
					output_inprogress[inprog_idx].is_pending=1;
					output_inprogress[inprog_idx].retry_after=UINT64_MAX;
					restart_timer=true;
				} NOT ACTUAL FOR NON CONTIGUOUS SEQUENCE */
				//meshses remain
			}
			req_idx++;
		}

		//first block must be unACKed and exactly at ack_sequenced_out, last block must end at sequenced_out
		assert((inprog_num==0 && ack_sequenced_out==sequenced_out) || 
			(inprog_num>0 && !output_inprogress[0].is_ack && output_inprogress[0].seq_pos==ack_sequenced_out && output_inprogress[inprog_num-1].seq_pos+output_inprogress[inprog_num-1].seq_len==sequenced_out));

		//normalize indexes for mesh sessions
		for(i=0; i<inprog_num; i++) {
			if(output_inprogress[i].meshses) {
				assert(!output_inprogress[i].is_ack);
				output_inprogress[i].meshses->current_outmeshtun_segment=i; //fix block index
			}
		}
		num_output_inprog=uint8_t(inprog_num);
		if(restart_timer) reset_timer(now_ms);
	}


	virtual int set_state(state_t newstate) override; //must be called INSIDE LOCK
	virtual void on_state_update(update_t upd) override; //must be called INSIDE LOCK. reacts to setting error, detaching, changing substate flags
};
/*
struct iot_meshtun_datagram_state : public iot_meshtun_state { //keeps state of connected or accepted tunnelled stream
	byte_fifo_buf buffer_in;
	byte_fifo_buf buffer_out;
//	uint32_t used_buffer_in; //how many bytes of input data is currently unread in buffer_in. maxseen_sequenced_in-used_buffer_in gives position in stream corresponding to writepos of buffer_in
	uint32_t creation_time; //timestamp - 1e9 in seconds when connection was created by client side (by clock of client) or accepted by server side (by clock of server)

	iot_meshtun_state(uint8_t inbuf_size_power, char* inbuf, uint8_t outbuf_size_power, char* outbuf) : iot_objectrefable(object_destroysub_memblock, true){
		bool rval=buffer_in.setbuf(inbuf_size_power, inbuf);
		assert(rval);
		rval=buffer_out.setbuf(outbuf_size_power, outbuf);
		assert(rval);
		uv_mutex_init(&mutex);
	}
	~iot_meshtun_state() {
		uv_mutex_destroy(&mutex);
	}


};

*/



#endif // IOT_MESH_CONTROL_H
