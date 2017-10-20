#ifndef IOT_NETCON_H
#define IOT_NETCON_H
//Contains constants, methods and data structures for managing connections to other gateways


#include<stdint.h>
#include<assert.h>
//#include<time.h>

class iot_netcontype_metaclass;
class iot_netcon;
class iot_netprototype_metaclass;
class iot_netproto_session;
class iot_netproto_config;


#include "iot_core.h"

//represents interface used by iot_netproto_session objects to read/write requests
class iot_netconiface {
public:
	bool is_server; //flag if connection is server (i.e. listening or already accepted), otherwise client (connecting)
	uv_thread_t worker_thread;
	uv_loop_t *loop;
	iot_memallocator* allocator;
	iot_netproto_session* protosession=NULL; //protocol session instance (created after successful connection)

	iot_netconiface(void) : is_server(false), worker_thread{}, loop(NULL), allocator(NULL) {
	}
	iot_netconiface(const iot_netconiface& ref) : is_server(ref.is_server), worker_thread(ref.worker_thread), loop(ref.loop), allocator(ref.allocator) {
	}
	virtual ~iot_netconiface() {
		assert(!protosession); //must be deinstantiated in derived class
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

};


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
};

class iot_netproto_session {
protected:
	const iot_netprototype_metaclass* meta; //keep reference to metaclass here to optimize (exclude virtual function call) requests which are redirected to metaclass
	iot_netconiface* const coniface;

	iot_netproto_session(const iot_netprototype_metaclass* meta, iot_netconiface* coniface): meta(meta), coniface(coniface) { //only derived classes can create instances , is_slave(is_slave)
		assert(meta!=NULL);
	}
public:
	virtual ~iot_netproto_session(void) {
	}
	const iot_netprototype_metaclass* get_metaclass(void) const {
		return meta;
	}
	const char* get_typename(void) const {
		return meta->type_name;
	}
//	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const = 0;
	virtual int start(void) = 0;

//
	virtual void on_write_data_status(int) = 0;
	virtual void on_can_write_data(void) = 0;
	virtual bool on_read_data_status(ssize_t nread, char* &databuf, size_t &databufsize) = 0;
};

class iot_netproto_config : public iot_objectrefable {
//	bool is_valid=true;
protected:
	const iot_netprototype_metaclass* meta; //keep reference to metaclass here to optimize (exclude virtual function call) requests which are redirected to metaclass

	iot_netproto_config(const iot_netprototype_metaclass* meta, object_destroysub_t destroysub): iot_objectrefable(destroysub), meta(meta) { //only derived classes can create instances
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
	virtual int instantiate(iot_netconiface* con)  = 0; //must assign con->protosession
	virtual void deinstantiate(iot_netconiface* con) = 0; //must free con->protosession and clean the pointer
};



//represents interface used by iot_netcon objects to notify about events
class iot_netconregistryiface {
public:
	virtual int on_new_connection(bool listening, iot_netcon *conobj) = 0;
};







class iot_netcontype_metaclass {
	iot_netcontype_metaclass* next;
public:
	const char* const type_name;
	const bool is_uv; //true - uses iot_thread_item_t threads and its event loop, false - uses dedicated thread with custom (own) processing (to use sync processing is no async possible or for better performance)

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
	static int from_json(json_object* json, iot_netproto_config* protoconfig, iot_netcon*& obj, bool is_server, bool prefer_own=false) {
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
		if(!prefer_own != curmeta->is_uv) { //try to find preferable con type
			curmeta=curmeta->next;
			while(curmeta) {
				if(curmeta->is_uv==!prefer_own && strcmp(curmeta->type_name, type)==0) {meta=curmeta;break;}
				curmeta=curmeta->next;
			}
		}

		uint32_t metric=0;
		if(!is_server) {
			if(json_object_object_get_ex(json, "metric", &val)) {
				IOT_JSONPARSE_UINT(json, uint32_t, metric);
			}
		}

		return meta->p_from_json(json, protoconfig, obj, is_server, metric);
	}
	virtual void destroy_netcon(iot_netcon* obj) const = 0; //must destroy netcon object in correct way (metaclass knows how its netcon objects are created)
private:
	virtual int p_from_json(json_object* json, iot_netproto_config* protoconfig, iot_netcon*& obj, bool is_server, uint32_t metric) const = 0; //must create netcon object from json params
};


class iot_netcon : public iot_netconiface { //base class for representing low-level network connection
	friend void iot_thread_registry_t::on_thread_msg(uv_async_t*);
public:
	iot_netcon *registry_next=NULL, *registry_prev=NULL; //can be used by any outer code to put connections in list. on destruction must be NULL
protected:
	iot_netconregistryiface *registry=NULL;
	iot_objid_t objid {iot_objid_t::OBJTYPE_NETCON};
	const iot_netcontype_metaclass* meta; //keep reference to metaclass here to optimize (exclude virtual function call) requests which are redirected to metaclass
	iot_netproto_config* const protoconfig; //protocol configuration pointer
	iot_thread_item_t* const control_thread_item;
//	const bool is_slave; //for server connection means that this is accepted socked, not listening. for client must be false
	volatile enum : uint8_t {
		STATE_UNINITED,
		STATE_INITED,
		STATE_STARTED,
		STATE_STOPPED
	} state=STATE_UNINITED;
	uint32_t metric=0;
//	iot_thread_item_t* worker_thread_item=NULL;
	iot_threadmsg_t *start_msg=NULL;

	iot_netcon(const iot_netcontype_metaclass* meta, iot_netproto_config* protoconfig, iot_thread_item_t* ctrl_thread):
			meta(meta), protoconfig(protoconfig), control_thread_item(ctrl_thread) { //only derived classes can create instances , is_slave(false)
		assert(meta!=NULL);
		assert(protoconfig!=NULL);
		assert(control_thread_item!=NULL);
		protoconfig->ref();
	}
	//for initing slave netcons
	iot_netcon(const iot_netcontype_metaclass* meta, iot_netproto_config* protoconfig, iot_thread_item_t* ctrl_thread, const iot_netconiface &master):
			iot_netconiface(master), meta(meta), protoconfig(protoconfig), control_thread_item(ctrl_thread) { //only derived classes can create instances , is_slave(true)
		assert(meta!=NULL);
		assert(protoconfig!=NULL);
		assert(control_thread_item!=NULL);
		protoconfig->ref();
	}
public:
	virtual ~iot_netcon(void) {
		assert(registry_next==NULL && registry_prev==NULL);
		protoconfig->deinstantiate(this);
		protoconfig->unref();
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
	int stop(void) {
		assert(false);
		return 0;
	}
	void assign_registryiface(iot_netconregistryiface *iface) {
		assert(registry==NULL);
		registry=iface;
	}
	void assign_objid(const iot_objid_t &id) {
		assert(!objid);
		assert(id.type==objid.type);
		objid=id;
	}
private:
	virtual void do_start_uv(void) {
		assert(false);
	}
	virtual void do_stop(void) {
		assert(false);
	}
};


//represents set of iot_netcon objects with common protocol
class iot_netconregistry : public iot_netconregistryiface {
	mutable volatile std::atomic_flag datamutex=ATOMIC_FLAG_INIT; //lock to protect critical sections and allow thread safety
	mutable volatile uint8_t datamutex_recurs=0;
	mutable volatile uv_thread_t datamutex_by={};
	const char* name;
	const uint32_t maxcons;
	iot_netcon **cons=NULL; //space for maximum number of connections (all of them must be either in listening_cons_head or connected_cons_head list)
	uint32_t *objid_keys=NULL;
	uint32_t last_objid=0;

	iot_netcon* listening_cons_head=NULL; //explicit and slave server iot_netcon objects which are or will be listening (they accept connections from outside but cannot trasfer requests)
	iot_netcon* connected_cons_head=NULL; //explicit client iot_netcon objects or connected server iot_netcon objects (can transfer requests)

	uv_thread_t control_thread={};

public:
	iot_netconregistry(const char* name, uint32_t maxcons)
			: name(name), maxcons(maxcons), control_thread(uv_thread_self()) {
		assert(maxcons>0);
	}
	virtual ~iot_netconregistry(void) {
		assert(uv_thread_self()==control_thread);
		for(iot_netcon *next, *cur=listening_cons_head; cur; cur=next) {
			next=cur->registry_next;

			BILINKLIST_REMOVE(cur, registry_next, registry_prev);

			cur->get_metaclass()->destroy_netcon(cur);
		}
		for(iot_netcon *next, *cur=connected_cons_head; cur; cur=next) {
			next=cur->registry_next;

			BILINKLIST_REMOVE(cur, registry_next, registry_prev);

			cur->get_metaclass()->destroy_netcon(cur);
		}

		if(cons) {
			free(cons);
			cons=NULL;
			objid_keys=NULL;
		}
	}
	int init(void) { //allocate memory for connections list
		assert(uv_thread_self()==control_thread);

		assert(cons==NULL);
		size_t sz=(sizeof(iot_netcon *)+sizeof(uint32_t))*maxcons;
		cons=(iot_netcon **)malloc(sz);
		if(!cons) return IOT_ERROR_NO_MEMORY;
		memset(cons, 0, sz);
		objid_keys=(uint32_t *)(cons+maxcons);
		return 0;
	}
	int add_connections(bool listening, json_object *json, iot_netproto_config* protoconfig, bool prefer_own=false) { //add several server (listening) connections by given JSON array with parameters for iot_netcon-derived classes
		assert(uv_thread_self()==control_thread);
		assert(cons!=NULL);

		const char* tp=listening ? "listening" : "outgoing";
		if(!json_object_is_type(json, json_type_array)) {
			outlog_error("netconregistry '%s' error: %s connections config must be array of objects", name, tp);
			return IOT_ERROR_BAD_DATA;
		}
		lock_datamutex();

		int len=json_object_array_length(json);
		int err;
		for(int i=0;i<len;i++) {
			json_object* cfg=json_object_array_get_idx(json, i);
			if(!json_object_is_type(cfg, json_type_object)) {
				outlog_error("netconregistry '%s' error: %s connection config item must be an objects, skipping '%s'", name, tp, json_object_to_json_string(cfg));
				continue;
			}
			int32_t newidx=find_free_index();
			if(newidx<0) {
				outlog_error("netconregistry '%s' error: connections overflow", name);
				break;
			}
			iot_netcon* conobj=NULL;
			err=iot_netcontype_metaclass::from_json(cfg, protoconfig, conobj, listening, prefer_own);
			if(err) continue;

			assign_con(conobj, newidx);
			conobj->assign_registryiface(this);
			conobj->assign_objid(iot_objid_t(iot_objid_t::OBJTYPE_NETCON, newidx, objid_keys[newidx]));

			if(listening)
				BILINKLIST_INSERTHEAD(conobj, listening_cons_head, registry_next, registry_prev);
			else
				BILINKLIST_INSERTHEAD(conobj, connected_cons_head, registry_next, registry_prev);
			if(conobj->is_uv()) conobj->start_uv(thread_registry->find_thread(control_thread)); //TODO
		}
		unlock_datamutex();
		return 0;
	}
	virtual int on_new_connection(bool listening, iot_netcon *conobj) override { //called from any thread		part of iot_netconregistryiface
//IOT_ERROR_LIMIT_REACHED
		assert(cons!=NULL);

		lock_datamutex();
		int32_t newidx=find_free_index();
		int err=0;
		if(newidx<0) {
			outlog_error("netconregistry '%s' error: connections overflow", name);
			err=IOT_ERROR_LIMIT_REACHED;
			goto onexit;
		}
		assign_con(conobj, newidx);
		conobj->assign_registryiface(this);
		conobj->assign_objid(iot_objid_t(iot_objid_t::OBJTYPE_NETCON, newidx, objid_keys[newidx]));

		if(listening)
			BILINKLIST_INSERTHEAD(conobj, listening_cons_head, registry_next, registry_prev);
		else
			BILINKLIST_INSERTHEAD(conobj, connected_cons_head, registry_next, registry_prev);
onexit:
		unlock_datamutex();
		return err;
	}
private:
	void lock_datamutex(void) const { //wait for datamutex mutex
		if(datamutex_by==uv_thread_self()) { //this thread already owns lock increase recursion level
			datamutex_recurs++;
			assert(datamutex_recurs<5); //limit recursion to protect against endless loops
			return;
		}
		uint8_t c=0;
		while(datamutex.test_and_set(std::memory_order_acquire)) {
			//busy wait
			c++;
			if((c & 0x3F)==0x3F) sched_yield();
		}
		datamutex_by=uv_thread_self();
		assert(datamutex_recurs==0);
	}
	void unlock_datamutex(void) const { //free reflock mutex
		if(datamutex_by!=uv_thread_self()) {
			assert(false);
			return;
		}
		if(datamutex_recurs>0) { //in recursion
			datamutex_recurs--;
			return;
		}
		datamutex_by={};
		datamutex.clear(std::memory_order_release);
	}
	void assign_con(iot_netcon* conobj, uint32_t idx) {
		cons[idx]=conobj;
		if(!++objid_keys[idx]) objid_keys[idx]++; //avoid zero value
	}
	int32_t find_free_index(void) { //must be called under datamutex !!!
		assert(datamutex.test_and_set(std::memory_order_acquire));

		uint32_t i=maxcons;
		for(; i>0; i--) {
			last_objid=(last_objid + 1) % maxcons;
			if(!cons[last_objid]) return int32_t(last_objid);
		}
		return -1;
	}
};


#endif // IOT_NETCON_H
