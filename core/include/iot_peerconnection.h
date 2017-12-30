#ifndef IOT_PEERCONNECTION_H
#define IOT_PEERCONNECTION_H
//Contains constants, methods and data structures for managing connections to other gateways


#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include "iot_core.h"

class iot_peer_link_t;
struct iot_remote_driverinst_item_t;
class iot_peers_registry_t;
class iot_peers_conregistry;

//enum iot_peerreq {
//	IOT_PEERREQ_
//};

//#include "iot_moduleregistry.h"
//#include "iot_core.h"
//#include "iot_configregistry.h"
#include "iot_netcon.h"
#include "iot_netproto_iotgw.h"


//manages all sessions to particular peer host. iot_peer instances aggregate object of this class
class iot_gwproto_sesregistry {
	mutable volatile std::atomic_flag datamutex=ATOMIC_FLAG_INIT; //lock to protect critical sections and allow thread safety
	mutable volatile uint8_t datamutex_recurs=0;
	mutable volatile uv_thread_t datamutex_by={};

	iot_netproto_session_iotgw *sessions_head=NULL;

public:
	iot_gwproto_sesregistry(void) {
	}
	~iot_gwproto_sesregistry(void) {
		assert(sessions_head==NULL);
	}

	int on_new_session(iot_netproto_session_iotgw *ses); //called from any thread
	void on_dead_session(iot_netproto_session_iotgw *ses); //called from any thread


private:
	void lock_datamutex(void) const { //wait for datamutex mutex
		if(datamutex_by==uv_thread_self()) { //this thread already owns lock increase recursion level
			datamutex_recurs++;
			assert(datamutex_recurs<5); //limit recursion to protect against endless loops
			return;
		}
		uint16_t c=1;
		while(datamutex.test_and_set(std::memory_order_acquire)) {
			//busy wait
			if(!(c++ & 1023)) sched_yield();
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

};



struct iot_remote_driverinst_item_t {
	iot_remote_driverinst_item_t *next, *prev;

	iot_hwdev_localident* devident;
//	const iot_hwdevident_iface* ident_iface;
	dbllist_list<iot_device_entry_t, iot_mi_inputid_t, uint32_t, 1> retry_clients; //list of local client instances which can be retried later or blocked forever

	iot_deviface_params_buffered devifaces[IOT_CONFIG_MAX_IFACES_PER_DEVICE];
	iot_miid_t miid;
};


//represents logical data channel to another gateway
class iot_peer : public iot_objectrefable {
	friend class iot_peers_registry_t;
	iot_peers_registry_t* pregistry=NULL;

	iot_peer *next=NULL, *prev=NULL;
	
	iot_remote_driverinst_item_t* drivers_head=NULL; //head of list of driver instances available for connections
	enum {
		STATE_INIT, //initial state after creation
		STATE_INITIALCONNPEND, //first connection is pending
		STATE_CONNPEND, //connection is pending (initial sync was already finished)
		STATE_INITIALSYNCING, //first connection succeeded and data transfer in progress
		STATE_SYNCING, //data transfer in progress (initial sync was already finished)
		STATE_INSYNC, //data in sync (end-of-queue mark received from peer)
	} state=STATE_INIT;
	uint64_t state_time; //time (uv_now) when current state was set
	uint64_t last_sync=0;//time of previous full sync (time of first STATE_INSYNC)

	iot_netcon* cons_head=NULL; //list of active (client) connections initiated towards this peer
	uint16_t cons_num=0; //number of connections in cons_head list
	iot_objref_ptr<iot_netproto_config_iotgw> protocfg;
	
public:
	const iot_hostid_t host_id;
	iot_gwproto_sesregistry sesreg; //registry of currently opened sessions

	iot_peer(iot_peers_registry_t* pregistry, iot_hostid_t hostid, object_destroysub_t destroysub, bool is_dynamic=true) : 
				iot_objectrefable(destroysub, is_dynamic), pregistry(pregistry), host_id(hostid) {

		assert(uv_thread_self()==main_thread);
		assert(pregistry!=NULL);

		state_time=uv_now(main_loop);
	}
	virtual ~iot_peer(void) {
		assert(uv_thread_self()==main_thread);

		assert(next==NULL && prev==NULL); //must be disconnected from registry
	}

	int set_connections(json_object *json, bool prefer_ownthread=false); //set list of connections by given JSON array with parameters for iot_netcon-derived classes
	int reset_connections(void);
	iot_remote_driverinst_item_t* get_avail_drivers_list(void) {
		return drivers_head;
	}
	bool is_failed(void) { //check if peer can be treated as dead
		if(state<STATE_INITIALSYNCING && uv_now(main_loop)-state_time>10000) return true;
		return false;
	}
	bool is_data_trusted(void) { //check if data is fresh enough and can be trusted
		if(state==STATE_INSYNC) return true;
//TODO		if(state==STATE_SYNCING) {
//			if(uv_now(main_loop)-last_sync<5000) return true; //we had full sync not long ago
//			return false; //wait for end of sync
//		}
		return false;
	}
};


class iot_peers_registry_t : public iot_netconregistryiface {
	iot_peer *peers_head=NULL;

	mutable volatile std::atomic_flag datamutex=ATOMIC_FLAG_INIT; //lock to protect critical sections and allow thread safety
	mutable volatile uint8_t datamutex_recurs=0;
	mutable volatile uv_thread_t datamutex_by={};

	const uint32_t maxcons;
	iot_netcon **cons=NULL; //space for maximum number of connections (all of them must be either in listening_cons_head or connected_cons_head list)
	uint32_t *objid_keys=NULL;
	uint32_t last_objid=0;

	iot_netcon* passive_cons_head=NULL; //explicit and automatically added passive (listening and connected listening) iot_netcon objects

	iot_objref_ptr<iot_netproto_config_iotgw> listenprotocfg;

public:
	iot_gwinstance* const gwinst;

	iot_peers_registry_t(iot_gwinstance* gwinst, uint32_t maxcons=256) : maxcons(maxcons), gwinst(gwinst) {
		assert(gwinst!=NULL);
		assert(maxcons>0);
	}

	virtual ~iot_peers_registry_t(void) {
		assert(uv_thread_self()==main_thread);
		//release all peer items
		iot_peer* pnext=peers_head, *p;
		while((p=pnext)) {
			pnext=pnext->next;

			BILINKLIST_REMOVE(p, next, prev);
			p->unref();
		}

		for(iot_netcon *next, *cur=passive_cons_head; cur; cur=next) {
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
		assert(uv_thread_self()==main_thread);
		assert(cons==NULL);

		listenprotocfg=iot_objref_ptr<iot_netproto_config_iotgw>(true, new iot_netproto_config_iotgw(gwinst, NULL, object_destroysub_delete, true));
		if(!listenprotocfg) return IOT_ERROR_NO_MEMORY;

		size_t sz=(sizeof(iot_netcon *)+sizeof(uint32_t))*maxcons;
		cons=(iot_netcon **)malloc(sz);
		if(!cons) return IOT_ERROR_NO_MEMORY;
		memset(cons, 0, sz);
		objid_keys=(uint32_t *)(cons+maxcons);
		return 0;
	}

	iot_peer* find_peer(iot_hostid_t hostid, bool create=false) {
		assert(uv_thread_self()==main_thread);

		if(!hostid) return NULL;

		lock_datamutex();

		iot_peer* p=peers_head;
		while(p) {
			if(p->host_id==hostid) break;
			p=p->next;
		}
		if(!p && create) {
			//do create
			p=(iot_peer*)main_allocator.allocate(sizeof(iot_peer), true);
			if(p) {
				new(p) iot_peer(this, hostid, object_destroysub_memblock, true); //refcount will be 1
				BILINKLIST_INSERTHEAD(p, peers_head, next, prev);
			}
		}
		
		unlock_datamutex();
		return p;
	}

	int set_peer_connections(iot_peer* peer, json_object *json, bool prefer_ownthread); //set list of connections by given JSON array with parameters for iot_netcon-derived classes
	int reset_peer_connections(iot_peer* peer);

	int add_listen_connections(json_object *json, bool prefer_ownthread=false); //add several server (listening) connections by given JSON array with parameters for iot_netcon-derived classes

	virtual int on_new_connection(iot_netcon *conobj) override { //called from any thread           part of iot_netconregistryiface
//IOT_ERROR_NOT_INITED - registry wasn't inited
//IOT_ERROR_LIMIT_REACHED
		if(!cons) {
			assert(false); //registry must be inited
			return IOT_ERROR_NOT_INITED;
		}

		lock_datamutex();
		int32_t newidx=find_free_index();
		int err=0;
		if(newidx<0) {
			outlog_error("peer connection registry error: connections overflow");
			err=IOT_ERROR_LIMIT_REACHED;
			goto onexit;
		}
		assign_con(conobj, newidx);
		conobj->assign_objid(iot_objid_t(iot_objid_t::OBJTYPE_PEERCON, newidx, objid_keys[newidx]));
		if(conobj->is_passive || !conobj->protoconfig || strcmp(conobj->protoconfig->get_typename(), "iotgw")!=0) {
			BILINKLIST_INSERTHEAD(conobj, passive_cons_head, registry_next, registry_prev);
		} else {
			iot_netproto_config_iotgw* cfg=static_cast<iot_netproto_config_iotgw*>((iot_netproto_config*)(conobj->protoconfig));
			if(cfg->peer)
				BILINKLIST_INSERTHEAD(conobj, cfg->peer->cons_head, registry_next, registry_prev);
			else 
				BILINKLIST_INSERTHEAD(conobj, passive_cons_head, registry_next, registry_prev);
		}
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
		uint16_t c=1;
		while(datamutex.test_and_set(std::memory_order_acquire)) {
			//busy wait
			if(!(c++ & 1023)) sched_yield();
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

inline int iot_peer::set_connections(json_object *json, bool prefer_ownthread) { //set list of connections by given JSON array with parameters for iot_netcon-derived classes
		if(!pregistry) return 0;
		return pregistry->set_peer_connections(this, json, prefer_ownthread);
	}
inline int iot_peer::reset_connections(void) {
		if(!pregistry) return 0;
		return pregistry->reset_peer_connections(this);
	}


/*
//represents set of iot_netcon objects to iotgw peers
class iot_peers_conregistry : public iot_netconregistryiface {
	mutable volatile std::atomic_flag datamutex=ATOMIC_FLAG_INIT; //lock to protect critical sections and allow thread safety
	mutable volatile uint8_t datamutex_recurs=0;
	mutable volatile uv_thread_t datamutex_by={};
	const char* name;
	const uint32_t maxcons;
	iot_netcon **cons=NULL; //space for maximum number of connections (all of them must be either in listening_cons_head or connected_cons_head list)
	uint32_t *objid_keys=NULL;
	uint32_t last_objid=0;

	iot_netcon* cons_head=NULL; //explicit client iot_netcon objects or connected server iot_netcon objects (can transfer requests)

	uv_thread_t control_thread={};

public:
	iot_peers_conregistry(const char* name, uint32_t maxcons)
			: name(name), maxcons(maxcons), control_thread(uv_thread_self()) {
		assert(maxcons>0);
	}
	virtual ~iot_peers_conregistry(void) {
		assert(uv_thread_self()==control_thread);
		for(iot_netcon *next, *cur=cons_head; cur; cur=next) {
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
	int add_connections(bool listening, json_object *json, iot_netproto_config* protoconfig, bool prefer_ownthread=false); //add several server (listening) connections by given JSON array with parameters for iot_netcon-derived classes

	virtual int on_new_connection(iot_netcon *conobj) override { //called from any thread           part of iot_netconregistryiface
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
		conobj->assign_objid(iot_objid_t(iot_objid_t::OBJTYPE_PEERCON, newidx, objid_keys[newidx]));
		BILINKLIST_INSERTHEAD(conobj, cons_head, registry_next, registry_prev);
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
		uint16_t c=1;
		while(datamutex.test_and_set(std::memory_order_acquire)) {
			//busy wait
			if(!(c++ & 1023)) sched_yield();
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

*/
#endif //IOT_PEERCONNECTION_H
