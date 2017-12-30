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


class iot_netcontype_metaclass;
class iot_netcon;
class iot_netprototype_metaclass;
class iot_netproto_session;
class iot_netproto_config;

//represents interface used by iot_netproto_session objects to read/write bytes id data. it is derived by iot_netcon class and can be derived by iot_netproto_session-derived classes
class iot_netconiface {
public:
	bool is_passive; //shows if this connection is passive side of communication (i.e. it was created as server-side or is result of accepting incoming connection),
					//otherwise this connection is active side (i.e. it was created as client-side, initiating)
	uv_thread_t worker_thread;
	iot_thread_item_t* worker_thread_item;
	uv_loop_t *loop;
	iot_memallocator* allocator;
	iot_objref_ptr<iot_netproto_session> protosession; //protocol session instance (created after successful connection)

	iot_netconiface(void) : is_passive(false), worker_thread{}, worker_thread_item(NULL), loop(NULL), allocator(NULL) {
	}
	iot_netconiface(const iot_netconiface* ref, bool is_passive=true) : is_passive(is_passive), worker_thread(ref->worker_thread), worker_thread_item(ref->worker_thread_item), loop(ref->loop), allocator(ref->allocator) {
	}
	virtual ~iot_netconiface();
	//is called by iot_netproto_session::stop() after processing session stop or in iot_netproto_session destructor
	void clear_session(void) {
		protosession=NULL;
		on_clear_session();
	}

	//ALL methods must be run in worker thread

	//check if new write request can be added. must return:
	//1 - write_data() can be called with request data
	//0 - request writing is in progress, so iot_netproto_session::on_write_data_status() must be waited (no need to call can_write_data() again)
	//IOT_ERROR_NOT_READY - no request being written but netcon object is not ready to write request. iot_netproto_session::on_can_write_data() will be called when netcon is ready
	virtual int can_write_data(void) = 0;

	//try to add new write request. must return:
	//0 - request successfully added. iot_netproto_session::on_write_data_status() will be called later after completion
	//1 - request successfully added and ready, iot_netproto_session::on_write_data_status() was already called before return from write_data()
	//IOT_ERROR_TRY_AGAIN - another request writing is in progress, so iot_netproto_session::on_write_data_status() must be waited
	//IOT_ERROR_NOT_READY - no request being written but netcon object is not ready to write request. iot_netproto_session::on_can_write_data() will be called when netcon is ready
	//IOT_ERROR_INVALID_ARGS - invalid arguments (databuf is NULL or datalen==0)
	virtual int write_data(void *databuf, size_t datalen) = 0;
	virtual int write_data(iovec *databufvec, int veclen) = 0;

	//enable reading into specified data buffer or reconfigure previous buffer. NULL databuf and zero datalen disable reading.
	//0 - reading successfully set. iot_netproto_session::on_read_data_status() will be called when data is available.
	//1 - reading successfully set and ready, iot_netproto_session::on_read_data_status() was already called before return from read_data()
	//IOT_ERROR_INVALID_ARGS - invalid arguments (databuf is NULL or datalen==0 but not simultaneously)
	virtual int read_data(void *databuf, size_t datalen) = 0;

	//stop reading side, wait for current write request to finish and then close connection
	virtual void graceful_close(void) = 0;

private:
	//called when session is stopped after being created
	virtual void on_clear_session(void) = 0;
};


//metaclass which represents type of protocol for sessions
//can be used to create iot_netproto_config-derived objects from JSON or other dynamic sources
class iot_netprototype_metaclass {
	iot_netprototype_metaclass* next;
public:
	const char* const type_name;

private:
	static iot_netprototype_metaclass*& get_listhead(void) {
		static iot_netprototype_metaclass* head=NULL;
		return head;
	}
	iot_netprototype_metaclass(const iot_netprototype_metaclass&) = delete; //block copy-construtors and default assignments

protected:
	iot_netprototype_metaclass(const char* type_name): type_name(type_name) {
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

	iot_netproto_config(const iot_netprototype_metaclass* meta, object_destroysub_t destroysub, bool is_dynamic): iot_objectrefable(destroysub, is_dynamic), meta(meta) { //only derived classes can create instances
		assert(meta!=NULL);
	}
	virtual ~iot_netproto_config(void) {
	}
public:
	const iot_netprototype_metaclass* get_metaclass(void) const {
		return meta;
	}
	const char* get_typename(void) const {
		return meta->type_name;
	}
//	void invalidate(void) { //config object can be invalidated if parent object (e.g. creator necassary for protocol activity) is destroyed to stop any interaction with it
//		is_valid=false;
//	}
//	bool check_valid(void) const {
//		return is_valid;
//	}
//	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const = 0;
	virtual int instantiate(iot_netconiface* con, iot_objref_ptr<iot_netproto_session> &ses) = 0; //must assign provided ses with new session object
};


//represents session of particular protocol
class iot_netproto_session : public iot_objectrefable {
protected:
	const iot_netprototype_metaclass* meta; //keep reference to metaclass here to optimize (exclude virtual function call) requests which are redirected to metaclass
	iot_netconiface* coniface;

	iot_netproto_session(const iot_netprototype_metaclass* meta, iot_netconiface* coniface, object_destroysub_t destroysub): iot_objectrefable(destroysub, true), meta(meta), coniface(coniface) { //only derived classes can create instances , is_slave(is_slave)
		assert(meta!=NULL);
		assert(coniface!=NULL);
	}
public:
	virtual ~iot_netproto_session(void) {
		if(coniface) coniface->clear_session();
	}
	const iot_netprototype_metaclass* get_metaclass(void) const {
		return meta;
	}
	const char* get_typename(void) const {
		return meta->type_name;
	}
//	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const = 0;
	virtual int start(void) = 0;
	void stop(bool netcon_failed=false) { //netcon_failed shows if stop was called due to failed netcon. should be set by coniface
		if(netcon_failed) coniface=NULL;
		on_stop();
		if(coniface) {
			coniface->clear_session();
			coniface=NULL;
		}
	}
//
	virtual void on_write_data_status(int) = 0;
	virtual void on_can_write_data(void) = 0;
	virtual bool on_read_data_status(ssize_t nread, char* &databuf, size_t &databufsize) = 0;
	virtual void on_stop(void) = 0;
};

#include "iot_core.h"



//represents interface used by iot_netcon objects to notify about events. this interface is derived by iot_netconregistry class
class iot_netconregistryiface {
public:
	virtual ~iot_netconregistryiface(void) {}

	virtual int on_new_connection(iot_netcon *conobj) = 0;
};



//metaclass which represents type of physical connection
//can be used to instantiate iot_netcon-derived objects
class iot_netcontype_metaclass {
	iot_netcontype_metaclass* next;
public:
	const char* const type_name;
	const bool is_uv; //true - uses iot_thread_item_t threads and its event loop, false - uses dedicated thread with custom (own) processing 
					//(to use sync processing if no async possible or for better performance)

private:
	static iot_netcontype_metaclass*& get_listhead(void) {
		static iot_netcontype_metaclass* head=NULL;
		return head;
	}

	iot_netcontype_metaclass(const iot_netcontype_metaclass&) = delete; //block copy-construtors and default assignments

protected:
	iot_netcontype_metaclass(const char* type_name, bool is_uv): type_name(type_name), is_uv(is_uv) {
		iot_netcontype_metaclass*& head=get_listhead();
		next=head;
		head=this;
	}

public:
	static int from_json(json_object* json, iot_netproto_config* protoconfig, iot_netcon*& obj, bool is_server=false, iot_netconregistryiface* registry=NULL, bool prefer_ownthread=false) {
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
		if(!prefer_ownthread != curmeta->is_uv) { //try to find preferable con type
			curmeta=curmeta->next;
			while(curmeta) {
				if(curmeta->is_uv==!prefer_ownthread && strcmp(curmeta->type_name, type)==0) {meta=curmeta;break;}
				curmeta=curmeta->next;
			}
		}

		uint32_t metric=0;
		if(!is_server) {
			if(json_object_object_get_ex(json, "metric", &val)) {
				IOT_JSONPARSE_UINT(json, uint32_t, metric);
			}
		}

		return meta->p_from_json(json, protoconfig, obj, is_server, registry, metric);
	}
	virtual void destroy_netcon(iot_netcon* obj) const = 0; //must destroy netcon object in correct way (metaclass knows how its netcon objects are created)
private:
	virtual int p_from_json(json_object* json, iot_netproto_config* protoconfig, iot_netcon*& obj, bool is_server, iot_netconregistryiface* registry, uint32_t metric) const = 0; //must create netcon object from json params
};



//base class for representing low-level network. connection always has specific protocol determined by protoconfig
class iot_netcon : public iot_netconiface {
	friend class iot_thread_registry_t;
public:
	iot_netcon *registry_next=NULL, *registry_prev=NULL; //can be used by any outer code (it should be code of registry) to put connections in list. on destruction must be NULL
	uint32_t metric=0;
	iot_objref_ptr<iot_netproto_config> const protoconfig; //protocol configuration pointer
protected:
	iot_netconregistryiface *registry=NULL;
	iot_objid_t objid {iot_objid_t::OBJTYPE_PEERCON};
	const iot_netcontype_metaclass* meta; //keep reference to metaclass here to optimize (exclude virtual function call) requests which are redirected to metaclass
	iot_thread_item_t* const control_thread_item; //thread which does init, start and stop
//	const bool is_slave; //for server connection means that this is accepted socked, not listening. for client must be false
	volatile enum : uint8_t {
		STATE_UNINITED,
		STATE_INITED,
		STATE_STARTED,
//		STATE_STOPPED
	} state=STATE_UNINITED;
	volatile bool stop_pending=false; //becomes true after stop() is called
	bool destroy_pending=false; //becomes true after stop(true) is called

//	iot_thread_item_t* worker_thread_item=NULL;
	iot_threadmsg_t *start_msg=NULL;

	iot_netcon(const iot_netcon &) = delete; //disable copy constructor. it can cause problems if randomly used

	iot_netcon(	const iot_netcontype_metaclass* meta,
				iot_netproto_config* protoconfig,
				iot_thread_item_t* ctrl_thread=NULL
			);
	//for initing slave netcons of server instances
	iot_netcon(const iot_netcon *master):
			iot_netconiface(master, true), protoconfig(master->protoconfig), meta(master->meta), control_thread_item(master->control_thread_item) { //only derived classes can create instances , is_slave(true)
	}
public:
	virtual ~iot_netcon(void) {
		assert(registry_next==NULL && registry_prev==NULL);
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
	int start_uv(iot_thread_item_t* use_thread);
	int stop(bool destroy=false);
	int on_stopped(bool wasasync=false); //finishes STOP operation in control thread
	int assign_registryiface(iot_netconregistryiface *iface) {
		assert(registry==NULL && iface!=NULL);
		registry=iface;
		return registry->on_new_connection(this);
	}
	void assign_objid(const iot_objid_t &id) {
		assert(!objid);
		assert(id.type==objid.type);
		objid=id;
	}

	bool has_sameparams(iot_netcon* ob) {
		assert(false);
		//TODO
		return false;
	}
protected:
	void mark_inited(void) { //must be called by from derived class after init is done to set proper state (and check for previous uninited state)
		assert(state==STATE_UNINITED);
		state=STATE_INITED;
	}
private:
	virtual void do_start_uv(void) {
		assert(false);
	}
	virtual void do_stop(void) {
		assert(false);
	}
};




#endif // IOT_NETCON_H
