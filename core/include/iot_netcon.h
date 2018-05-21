#ifndef IOT_NETCON_H
#define IOT_NETCON_H
//Contains constants, methods and data structures for managing connections to other gateways


#include<stdint.h>
#include<assert.h>
//#include<time.h>
#include <atomic>
#include "uv.h"

//#include<time.h>

#include "iot_core.h"
//#include "iot_memalloc.h"
#include "iot_threadregistry.h"


class iot_netcontype_metaclass;
class iot_netcon;
class iot_netprototype_metaclass;
class iot_netproto_session;
class iot_netproto_config;

//represents interface used by iot_netproto_session objects to read/write bytes id data. it is derived by iot_netcon class and can be derived by iot_netproto_session-derived classes
class iot_netconiface {
public:
	bool is_passive=false; //shows if this connection is passive side of communication (i.e. it was created as server-side or is result of accepting incoming connection),
					//otherwise this connection is active side (i.e. it was created as client-side, initiating)
	bool is_stream; //shows if connection type guarantees sequencing of packets (e.g. TCP). otherwise UDP or raw ip/ethernet is assumed
	iot_thread_item_t* thread=NULL; //working thread
	uv_loop_t *loop=NULL;
	iot_memallocator* allocator=NULL;
	iot_objref_ptr<iot_netproto_session> protosession; //protocol session instance (created after successful connection)

	iot_netconiface(bool is_stream) : is_stream(is_stream) {
	}

//	iot_netconiface(const iot_netconiface* ref, bool is_passive=true) : is_passive(is_passive), is_stream(ref->is_stream) {
//	}
	virtual ~iot_netconiface(void) {
		assert(protosession==NULL);
	}
	//ALL methods must be run in worker thread

	//check if new write request can be added. must return:
	//1 - write_data() can be called with request data
	//0 - request writing is in progress, so iot_netproto_session::on_write_data_status() must be waited (no need to call can_write_data() again)
////	//IOT_ERROR_NOT_READY - no request being written but netcon object is not ready to write request. iot_netproto_session::on_can_write_data() will be called when netcon is ready
	//IOT_ERROR_INVALID_STATE - netcon is not in RUNNING state
	virtual int can_write_data(void) = 0;

	//try to add new write request. must return:
	//0 - request successfully added. iot_netproto_session::on_write_data_status() will be called later after completion
	//1 - request was finished. iot_netproto_session::on_write_data_status() was NOT CALLED
	//IOT_ERROR_TRY_AGAIN - another request writing is in progress, so iot_netproto_session::on_write_data_status() must be waited
	//IOT_ERROR_INVALID_STATE - (must not occur in debug mode) netcon is not in RUNNING state
////	//IOT_ERROR_NOT_READY - no request being written but netcon object is not ready to write request. iot_netproto_session::on_can_write_data() will be called when netcon is ready
	//IOT_ERROR_INVALID_ARGS - invalid arguments (databuf is NULL or datalen==0)
	virtual int write_data(void *databuf, size_t datalen) = 0;
	virtual int write_data(iovec *databufvec, int veclen) = 0;

	//enable reading into specified data buffer or reconfigure previous buffer. NULL databuf and zero datalen disable reading.
	//0 - reading successfully set. iot_netproto_session::on_read_data_status() will be called when data is available.
	//1 - reading successfully set and ready, possible for blocking mode //CANCELLED::iot_netproto_session::on_read_data_status() was already called before return from read_data()
	//IOT_ERROR_INVALID_ARGS - invalid arguments (databuf is NULL or datalen==0 but not simultaneously)
	virtual int read_data(void *databuf, size_t datalen) = 0;

	//called by session when it can be detached from netconiface. CAN INITIATE DESTRUCTOR for session!!!
	void session_closed(void) { //is inline at end of this file
		assert(protosession!=NULL);
		protosession=NULL;
		on_session_closed();
	}
	//can be called from any thread to initiate destroying of netconiface. graceful_stop shows if session must be stopped gracefully
	virtual int destroy(bool always_async=false, bool graceful_stop=false) = 0;
protected:
//	void detach_session(void); //netconiface initiated session close (e.g. if connection is broken)
private:
	//called when session_closed() gets called, after zeroing protosession.
	virtual void on_session_closed(void) = 0;
};


//temporary place for mesh sessions protocol IDs registry
#define MESHTUN_PROTO_IOTGW 1

#define MESHTUN_PROTO_MAX 1

//temporary place for netcontype IDs registry
#define NETCONTYPE_TCP 1



//metaclass which represents type of protocol for sessions
//can be used to create iot_netproto_config-derived objects from JSON or other dynamic sources
class iot_netprototype_metaclass {
	iot_netprototype_metaclass* next;
public:
	const char* const type_name;
	const uint16_t type_id; //non-zero protocol ID. zero value means that protocol has no registered ID and cannot be tunnelled over mesh network.
	const uint16_t meshtun_maxport; //for mesh-able protocols determines maximum multiplicity of connections by this protocol between same pair of hosts over mesh network.
									//so zero value means one connection is posssible, N means (N+1)*(N+1) connections
	const bool can_meshtun; //shows if protocol can be tunnelled over mesh network (if type_id>0)
	const bool meshtun_isstream; //shows if tunnelled mesh stream connection of this protocol is streamable (must guarantee ordering or packets) or not

private:
	static iot_netprototype_metaclass*& get_listhead(void) {
		static iot_netprototype_metaclass* head=NULL;
		return head;
	}
	iot_netprototype_metaclass(const iot_netprototype_metaclass&) = delete; //block copy-construtors and default assignments

protected:
	iot_netprototype_metaclass(const char* type_name, uint16_t type_id=0, bool can_meshtun=false, bool meshtun_isstream=true, uint16_t meshtun_maxport=0):
					type_name(type_name), type_id(type_id), meshtun_maxport(meshtun_maxport), can_meshtun(can_meshtun), meshtun_isstream(meshtun_isstream) {
		iot_netprototype_metaclass*& head=get_listhead();
		next=head;
		head=this;
	}
public:
	static int from_json(json_object* json, iot_netproto_config*& obj) {
		//TODO implement when becomes necessary to create proto config objects from JSON
		assert(false);
		return 0;
	}
private:
//	virtual int p_from_json(json_object* json, iot_netproto_config*& obj) const = 0; //must create netcon object from json params
};

//protocol specific configuration for session. can be shared by several sessions
class iot_netproto_config : public iot_objectrefable {
//	bool is_valid=true;
protected:
	const iot_netprototype_metaclass* meta; //keep reference to metaclass here to optimize (exclude virtual function call) requests which are redirected to metaclass

	iot_netproto_config(void) = delete;

	iot_netproto_config(const iot_netprototype_metaclass* meta, iot_gwinstance *gwinst, object_destroysub_t destroysub, bool is_dynamic, void* customarg=NULL):
				iot_objectrefable(destroysub, is_dynamic), meta(meta), gwinst(gwinst), customarg(customarg) { //only derived classes can create instances
		assert(meta!=NULL);
	}
	virtual ~iot_netproto_config(void) {
	}
public:
	iot_gwinstance *const gwinst;
	void* const customarg; //custom pointer passed to constructor
	const iot_netprototype_metaclass* get_metaclass(void) const {
		return meta;
	}
	const char* get_typename(void) const {
		return meta->type_name;
	}
	uint16_t get_typeid(void) const {
		return meta->type_id;
	}
	virtual uint8_t get_cpu_loading(void) { //must return maximal possible cpu loading caused by session of corresponding protocol with accounting of present config
		//so if current protocol session can create one or several sessions of other protocols, maximal cpu loading of those protocols must be returned
		return 0;
	}
//	void invalidate(void) { //config object can be invalidated if parent object (e.g. creator necassary for protocol activity) is destroyed to stop any interaction with it
//		is_valid=false;
//	}
//	bool check_valid(void) const {
//		return is_valid;
//	}
//	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const = 0;
	virtual int instantiate(iot_netconiface* con, iot_hostid_t meshtun_peer=0) = 0; //must create new session object. meshtun_peer will be set if session is created over mesh tunnel to specified peer host
	virtual void meshtun_set_metadata(iot_meshtun_state* state) { //protocols having true can_meshtun can redefine this method to assign meta data to meshtun before meshtun is established
	}
};


//represents session of particular protocol
class iot_netproto_session : public iot_objectrefable {
protected:
	const iot_netprototype_metaclass* meta; //keep reference to metaclass here to optimize (exclude virtual function call) requests which are redirected to metaclass
	iot_netconiface* coniface;
	iot_thread_item_t *thread;

	iot_netproto_session(const iot_netprototype_metaclass* meta, iot_netconiface* coniface, object_destroysub_t destroysub):
				iot_objectrefable(destroysub, true), meta(meta), coniface(coniface) { //only derived classes can create instances , is_slave(is_slave)
		assert(meta!=NULL);
		assert(coniface!=NULL);
		assert(coniface->protosession==NULL);

		coniface->protosession=iot_objref_ptr<iot_netproto_session>(true, this);
		thread=coniface->thread;
	}

	void self_closed(void) { //must be called by session code when session stopped and can be freed (destructor can be called during processing of this call if coniface holds the only reference)
		assert(uv_thread_self()==thread->thread);
		if(coniface) {
			auto c=coniface;
			coniface=NULL;
			c->session_closed();
		}
	}

public:
	virtual ~iot_netproto_session(void) {
//		if(coniface) coniface->clear_session();
	}
	const iot_netprototype_metaclass* get_metaclass(void) const {
		return meta;
	}
	const char* get_typename(void) const {
		return meta->type_name;
	}
//	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const = 0;
	virtual int start(void) = 0;
	bool stop(bool graceful=true) { //request session to stop. coniface will be notified by calling session_closed() if not detached (returns true in such case). session_closed() can be called immediately!!!
		//returns false if coniface is already detached and thus session_closed() won't be called OR if call is made from non-session thread
		assert(uv_thread_self()==thread->thread);
		bool rval = coniface!=NULL;
		on_stop(graceful);
		return rval;
	}

//	void coniface_detach(void) {
//		assert(uv_thread_self()==thread->thread);
//
//		coniface=NULL;
//		on_coniface_detached();
//	}

	iot_netconiface* get_coniface(void) const {
		return coniface;
	}

	virtual void on_write_data_status(int) = 0;
//	virtual void on_can_write_data(void) = 0;
	virtual bool on_read_data_status(ssize_t nread, char* &databuf, size_t &databufsize) = 0;

private:
//	virtual void on_coniface_detached(void) = 0; //notification to session that coniface was nullified and cannot be interacted
	virtual void on_stop(bool graceful) = 0; //request session to stop. coniface will be notified by calling session_closed() if not detached
};

#include "iot_core.h"



//represents interface used by iot_netcon objects to notify about events. this interface is derived by iot_netconregistry class
class iot_netconregistryiface {
public:
	virtual ~iot_netconregistryiface(void) {}

	virtual int on_new_connection(iot_netcon *conobj) = 0;
	virtual void on_destroyed_connection(iot_netcon *conobj) = 0;
};



//metaclass which represents type of physical connection
//can be used to instantiate iot_netcon-derived objects
class iot_netcontype_metaclass {
	iot_netcontype_metaclass* next;
public:
	const char* const type_name;
	const uint16_t type_id; //non-zero protocol ID. zero value means that protocol has no registered ID and cannot be proxied over mesh.
	const bool is_uv; //true - uses iot_thread_item_t threads and its event loop, false - uses dedicated thread with custom (own) processing 
					//(to use sync processing if no async possible or for better performance)
	const bool is_stream; //shows if connection type guarantees sequencing of packets (e.g. TCP). otherwise UDP or raw ip/ethernet is assumed
	const bool can_meshproxy; //true if this netcontype can be proxied through mesh stream (if type_id > 0). if there are several netcontype implementation (UV and nonUV), both must have same value for this flag

private:
	static iot_netcontype_metaclass*& get_listhead(void) {
		static iot_netcontype_metaclass* head=NULL;
		return head;
	}

	iot_netcontype_metaclass(const iot_netcontype_metaclass&) = delete; //block copy-construtors and default assignments

protected:
	iot_netcontype_metaclass(const char* type_name, uint16_t type_id, bool is_uv, bool is_stream, bool can_meshproxy=false): type_name(type_name), type_id(type_id), is_uv(is_uv), is_stream(is_stream), can_meshproxy(can_meshproxy) {
		iot_netcontype_metaclass*& head=get_listhead();
		next=head;
		head=this;
	}

public:
	static int from_json(json_object* json, iot_netproto_config* protoconfig, iot_netcon*& obj, bool is_server=false, iot_netconregistryiface* registry=NULL, bool prefer_ownthread=false, bool allow_meshproxy=false) {
		//allow_meshproxy shows if "proxy" key is searched in provided json to set up mesh proxied tunnel to provided point (i.e. connect to some IP and TCP port through another host, which is reachable by mesh network from this host)
		json_object* val=NULL;
		const char *type="tcp";//iot_netcontype_metaclass_tcp::object.type_name; //defalt type is 'tcp'
		if(json_object_object_get_ex(json, "type", &val)) {
			if(!json_object_is_type(val, json_type_string)) {
				outlog_error("'type' field must be a string in connection spec");
				return IOT_ERROR_BAD_DATA;
			}
			type=json_object_get_string(val);
		}

		//find metaclass
		iot_netcontype_metaclass* curmeta=get_listhead(), *meta=NULL;
		while(curmeta) {
			if(strcmp(curmeta->type_name, type)==0) {meta=curmeta;break;}
			curmeta=curmeta->next;
		}
		if(!curmeta) {
			outlog_error("connection type '%s' is unknown", type);
			return IOT_ERROR_NOT_FOUND;
		}
		iot_hostid_t proxy=0;
		if(allow_meshproxy && json_object_object_get_ex(json, "proxy", &val)) {
			IOT_JSONPARSE_UINT(val, iot_hostid_t, proxy);
			if(proxy) {
				if(!meta->can_meshproxy || !meta->type_id) {
					outlog_error("Connection type '%s' cannot be proxied through another host");
					return IOT_ERROR_BAD_DATA;
				}
			}
		}

		if(!proxy && !prefer_ownthread != meta->is_uv) { //try to find preferable con type. not applicable to proxied connections
			curmeta=meta->next;
			while(curmeta) {
				if(curmeta->is_uv==!prefer_ownthread && strcmp(curmeta->type_name, type)==0) {meta=curmeta;break;}
				curmeta=curmeta->next;
			}
		}

		uint32_t metric=0;
		if(!is_server) {
			if(json_object_object_get_ex(json, "metric", &val)) {
				IOT_JSONPARSE_UINT(val, uint32_t, metric);
			}
		}
		if(proxy) return iot_netcontype_metaclass::meshproxy_from_json(meta, proxy, json, protoconfig, obj, is_server, registry, metric);

		return meta->p_from_json(json, protoconfig, obj, is_server, registry, metric);
	}
	virtual void destroy_netcon(iot_netcon* obj) const = 0; //must destroy netcon object in correct way (metaclass knows how its netcon objects are created)
private:
	virtual int p_from_json(json_object* json, iot_netproto_config* protoconfig, iot_netcon*& obj, bool is_server, iot_netconregistryiface* registry, uint32_t metric) const = 0; //must create netcon object from json params

	static int meshproxy_from_json(iot_netcontype_metaclass* meta, iot_hostid_t proxyhost, json_object* json, iot_netproto_config* protoconfig, iot_netcon*& obj, bool is_server, iot_netconregistryiface* registry, uint32_t metric); //must create netcon object from json params
};



//base class for representing low-level network. connection always has specific protocol determined by protoconfig
class iot_netcon : public iot_netconiface {
	friend class iot_thread_registry_t;
public:
	enum state_t : uint8_t {
		STATE_UNINITED,
		STATE_INITING,
		STATE_INITED, //is changed either to DESTROYING or STARTING
		STATE_DESTROYING, //final state
		STATE_STARTING,
		STATE_STARTED //final state
	};

	const iot_netcontype_metaclass* const meta; //keep reference to metaclass here to optimize (exclude virtual function call) requests which are redirected to metaclass
	iot_netcon *registry_next=NULL, *registry_prev=NULL; //can be used by any outer code (it should be code of registry) to put connections in list. on destruction must be NULL
	iot_netcon *next_inthread=NULL, *prev_inthread=NULL;
	uint32_t metric=0;
	uint8_t cpu_loading; //keeps actual cpu_loading which was assigned to working thread
	iot_objref_ptr<iot_netproto_config> const protoconfig; //protocol configuration pointer
	iot_netconregistryiface *registry=NULL;
//	iot_objid_t objid {iot_objid_t::OBJTYPE_PEERCON};
private:
	iot_threadmsg_t com_msg={};

//	iot_spinlock statelock; //used to protect com_msg and stop_pending in started state
	volatile bool destroy_pending=false; //becomes true after stop(true) is called
	volatile bool graceful_sesstop=false; //shows if destroying with graceful session stop was requested
	volatile std::atomic_flag stop_pending=ATOMIC_FLAG_INIT; //becomes true after restart(_, false) is called
	volatile std::atomic_flag gracefulstop_pending=ATOMIC_FLAG_INIT; //becomes true after restart() is called
	volatile std::atomic_flag commsg_pending=ATOMIC_FLAG_INIT; //becomes true if com_msg is busy
	volatile std::atomic<state_t> state={STATE_UNINITED};

protected:

//	iot_thread_item_t* const control_thread_item; //thread which does init, start and stop
//	const bool is_slave; //for server connection means that this is accepted socked, not listening. for client must be false

//	iot_thread_item_t* thread=NULL;

	iot_netcon(const iot_netcon &) = delete; //disable copy constructor. it can cause problems if randomly used

	iot_netcon(	const iot_netcontype_metaclass* meta_,
				iot_netproto_config* protoconfig
//				iot_thread_item_t* ctrl_thread=NULL
			) : iot_netconiface(meta_->is_stream), meta(meta_), protoconfig(protoconfig) {
		assert(meta!=NULL);
		assert(protoconfig!=NULL);
//		assert(control_thread_item!=NULL);
	}
/*	//for initing slave netcons of server instances
	iot_netcon(const iot_netcon *master):
			iot_netconiface(master, use_thread, true), protoconfig(master->protoconfig), meta(master->meta), control_thread_item(master->control_thread_item) { //only derived classes can create instances , is_slave(true)
		if(use_thread) { //is thread provided, register current instance in it
			cpu_loading=get_cpu_loading();
			use_thread->add_netcon(this);
		}
	}
*/
public:
	virtual ~iot_netcon(void) {
//		assert(uv_thread_self()==control_thread_item->thread);

		assert(registry_next==NULL && registry_prev==NULL);
		assert(next_inthread==NULL && prev_inthread==NULL);
	}
	const iot_netcontype_metaclass* get_metaclass(void) const {
		return meta;
	}
	const char* get_typename(void) const {
		return meta->type_name;
	}
	bool is_uv(void) const {
		return meta->is_uv;
	}
	uint32_t get_metric(void) const {
		return metric;
	}
	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const = 0;
	int start_uv(iot_thread_item_t* use_thread=NULL, bool always_async=false, bool finish_init=false);
	void on_startstop_msg(void);
	int restart(bool always_async=false, bool graceful_stop=false);

	virtual int destroy(bool always_async=false, bool graceful_stop=false) override;

	int on_stopped(void); //finishes STOP operation in control thread
	int assign_registryiface(iot_netconregistryiface *iface) {
		assert(registry==NULL && iface!=NULL);
		int err=iface->on_new_connection(this);
		if(err) return err;
		registry=iface;
		return 0;
	}
//	void assign_objid(const iot_objid_t &id) {
//		assert((!objid && id) || (objid && !id)); //allow to set or to clear
//		assert(id.type==objid.type);
//		objid=id;
//	}
//	const iot_objid_t& get_objid(void) const {
//		return objid;
//	}

	int has_sameparams(iot_netcon* op) {
		if(!op || get_metaclass()!=op->get_metaclass() || is_passive!=op->is_passive) return 0;
		if(destroy_pending || op->destroy_pending) return 0;
		if(protoconfig!=op->protoconfig) return 0;
//		if(protoconfig->get_metaclass()!=op->protoconfig->get_metaclass()) return 0; //TODO compare protoconfigs using proto method if metaclass matches?
		if(!do_compare_params(op)) return 0;
		
		if(metric!=op->metric) return 2;
		return 1;
	}
protected:
	bool trymark_initing(void) { //must be called from derived class before doing init to set proper state (and check for previous uninited state)
	//returns true if state was atomically changed from UNINITED to INITING
		state_t prevstate=STATE_UNINITED;
		return state.compare_exchange_strong(prevstate, STATE_INITING, std::memory_order_acq_rel, std::memory_order_relaxed);
	}
	void mark_inited(void) { //must be called from derived class after finishing init. previous state must be INITING
		state_t prevstate=STATE_INITING;
		if(!state.compare_exchange_strong(prevstate, STATE_INITED, std::memory_order_acq_rel, std::memory_order_relaxed)) {
			assert(false);
		}
	}
	bool unmark_initing(bool destroy=false) { //must be called to reset state to UNINITED (or to destroy) from INITING. returns false if state was not initing
		state_t prevstate=STATE_INITING;
		if(state.compare_exchange_strong(prevstate, destroy ? STATE_DESTROYING : STATE_UNINITED, std::memory_order_acq_rel, std::memory_order_relaxed)) {
			if(destroy) do_stop();
			return true;
		}
		return false;
	}
	bool is_started(void) {
		return state.load(std::memory_order_relaxed)==STATE_STARTED;
	}
	uint8_t get_cpu_loading(void) {
		assert(protoconfig!=NULL);
		uint8_t cfg_loading=protoconfig->get_cpu_loading();
		uint8_t own_loading=p_get_cpu_loading();
		return cfg_loading>=own_loading ? cfg_loading : own_loading;
	}
private:

	virtual uint8_t p_get_cpu_loading(void)=0; //must return pure cpu loading of netcon layer implementation
	virtual void do_start_uv(void) {
		assert(false);
	}
	virtual int do_stop(void) {
		assert(false);
	}
	virtual bool do_compare_params(iot_netcon* op) = 0; //must return true if parameters of same typed netcons are the same. is_passive is already checked to be the same
};

//inline void iot_netconiface::detach_session(void) { //netconiface initiated session close (e.g. if connection is broken)
//		if(!protosession) return;
//		protosession->coniface_detach(); //on_session_closed cannot be called because coniface pointer is nullified here
//		protosession=NULL;
//	}



#endif // IOT_NETCON_H
