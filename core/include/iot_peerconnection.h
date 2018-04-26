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
#include "iot_netproto_mesh.h"
#include "iot_mesh_control.h"
#include "iot_netcon_mesh.h"


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
	friend class iot_meshnet_controller;
	friend class iot_peers_iterator;

public:
	iot_gwinstance* const gwinst;
	const iot_hostid_t host_id;

private:
	iot_peer *next=NULL, *prev=NULL;
	
	iot_remote_driverinst_item_t* drivers_head=NULL; //head of list of driver instances available for connections
//	uint64_t state_time; //time (uv_now) when current state was set
	uint64_t last_sync=0;//time of previous full sync (time of first STATE_INSYNC)

	iot_netcon* cons_head=NULL; //list of active (client) connections initiated towards this peer
	iot_objref_ptr<iot_netproto_config_mesh> meshprotocfg;
	iot_objref_ptr<iot_netproto_config_iotgw> iotprotocfg;
//	iot_netcon_mesh* iotgw_clientcon=NULL; //mesh netcon used to create IOT GW session. this is client connection used to connect to this peer when its host id IS GREATER than this host

	iot_spinlock seslistlock;
	iot_objref_ptr<iot_netproto_session_iotgw> iotsession; //seslistlock protected active IOTGW session reference
	iot_netconiface* iotnetcon=NULL; //seslistlock protected coniface of iotsession

//ROUTING* - means that variable is accessed under corresponding R/W routing lock of mesh controller
	iot_netproto_session_mesh *meshsessions_head=NULL; //ROUTING*. mesh controller controlled list of active MESH sessions to this peer.

	mpsc_queue<iot_meshtun_packet, iot_meshtun_packet, &iot_meshtun_packet::next> meshtunq; //queue of mesh streams destined to this peer with pending write
																									 //reader of this queue is holder of meshtunq_lock
	uv_mutex_t meshtunq_lock; //must be obtained to use consumer methods of meshtunq

	iot_meshorigroute_entry* origroutes=NULL; //ROUTING*. iot_memblock of buffer (sized as maxorigroutes*sizeof(iot_meshorigroute_entry)) with original routing entries obtained from peer
											//cannot contain entries for local host and this peer. entries are sorted by ascending hostid
	volatile std::atomic<uint64_t> origroutes_version={0}; //ROUTING*. version of routing table in origroutes as it was reported by peer
	uint32_t maxorigroutes=0; //ROUTING*. determines size of memory buffer under origroutes
	uint32_t numorigroutes=0; //ROUTING*. number of valid entries inside origroutes array

	iot_meshroute_entry *routeslist_head=NULL; //ROUTING*. Part of local routing table - list of possible routes to this peer sorted by increasing delay (from head to tail)
												//Head entry is used  for routing decisions
	iot_meshroute_entry *routeslist_althead=NULL; //ROUTING*. must point to first entry of routeslist_head which has different next-hop peer than entry at head. Is used to build routing table for peer which is next-hop at head (because it must not get route where it is the next-hop for current host)
	uint64_t routing_actualversionpart=0; //ROUTING*. shows to which version of routing table do routing_actualdelay and routing_actualpathlen correspond. maximum among all such values which are actual for particular peer (thus skipping that specific peer's routing_actualversion) is the one which must be reported to peer
	uint64_t routing_altactualversionpart=0; //ROUTING*. same for alternative route
//	iot_atimer_item noroute_timer_item; //ROUTING*
	iot_fixprec_timer_item<true> noroute_timer_item;

	uint32_t routing_actualdelay; //ROUTING*. Normally - realdelay from iot_meshroute_entry at routeslist_head, but updating that entry's delay (or changing the entry)
								//does not update routing_actualdelay if difference in delay is less than some threshold (so that insignificant change does not
								//initiate routing table resync to peers) this value is reported to peers as path delay (when sending local routing table)
	uint32_t routing_altactualdelay; //ROUTING*. same for alternative route

	volatile std::atomic<uint64_t> lastreported_routingversion={0}; //this is version of local routing table which was sent to peer and confirmed. this sync is done through first appropriate mesh session to that host
	uint64_t current_routingversion=0; //ROUTING*. this is version of local routing table for this peer which is pending to be sent. notify_routing_update must be true is this value is > than lastreported_routingversion
	uint16_t routing_actualpathlen=0; //ROUTING*. pathlen which corresponds to routing_actualdelay. zero value means that there are no route (direct peers have pathlen==1)
	uint16_t routing_altactualpathlen=0; //ROUTING*. pathlen which corresponds to routing_altactualdelay. zero value means that there are no route
	uint16_t cons_num=0; //number of connections in cons_head list

/*	enum : uint8_t {
		STATE_INIT, //initial state after creation
		STATE_INITIALCONNPEND, //first connection is pending
		STATE_CONNPEND, //connection is pending (initial sync was already finished)
		STATE_INITIALSYNCING, //first connection succeeded and data transfer in progress
		STATE_SYNCING, //data transfer in progress (initial sync was already finished)
		STATE_INSYNC, //data in sync (end-of-queue mark received from peer)
	} state=STATE_INIT;*/
	bool notify_routing_update=false; //is set to true if local routing table was updated and lastreported_routingversion here is less than any of appropriate routing_actualversionpart and routing_altactualversionpart (of all other peer structs)
										//first appropriate mesh session is used to sync routing table when session sees this flag. flag is reset after
										//successful confirmation from peer, so it is possible that several sessions to same peer can transfer the same routing table and this is OK
	bool origroutes_unknown_host=false; //true value means that origroutes has at least one entry with unknown host, so when configuration changes and new hosts configured, routes can be applied
	volatile std::atomic<bool> is_unreachable={true}; //ROUTING* shows that there are no routes to this peer for at least IOT_MESHNET_NOROUTETIMEOUT seconds, and thus peer is treated as unreachable

public:
	iot_peer(iot_gwinstance* gwinst, iot_hostid_t hostid, object_destroysub_t destroysub, bool is_dynamic=true) : 
				iot_objectrefable(destroysub, is_dynamic), gwinst(gwinst), host_id(hostid) {

		assert(uv_thread_self()==main_thread);
		assert(gwinst!=NULL);

		int err=uv_mutex_init(&meshtunq_lock);
		assert(!err);

		noroute_timer_item.init(this, [](void *peerp, uint32_t period_id, uint64_t now_ms)->void {
			iot_peer *p=(iot_peer *)peerp;
			p->gwinst->meshcontroller->on_noroute_timer(p);
		});

//		state_time=uv_now(main_loop);
	}
	virtual ~iot_peer(void) {
		assert(uv_thread_self()==main_thread);


		assert(next==NULL && prev==NULL); //must be disconnected from registry

		assert(iotsession==NULL);
		assert(meshsessions_head==NULL);
		assert(cons_head==NULL);

		if(origroutes) {
			iot_release_memblock(origroutes);
			origroutes=NULL;
			maxorigroutes=numorigroutes=0;
		}

		uv_mutex_destroy(&meshtunq_lock);
	}
	bool is_notify_routing_update(void) const {
		return notify_routing_update;
	}
	uint64_t get_origroutes_version(void) const {
		return origroutes_version.load(std::memory_order_relaxed);
	}
	void set_lastreported_routingversion(uint64_t ver) {
		lastreported_routingversion.store(ver, std::memory_order_relaxed);
	}

	int set_connections(json_object *json, bool prefer_ownthread=false); //set list of connections by given JSON array with parameters for iot_netcon-derived classes
	int reset_connections(void);

	int on_new_iotsession(iot_netproto_session_iotgw *ses); //called from any thread
	void on_dead_iotsession(iot_netproto_session_iotgw *ses); //called from any thread

	iot_remote_driverinst_item_t* get_avail_drivers_list(void) {
		return drivers_head;
	}
	bool check_is_reachable(void) {
		return !is_unreachable.load(std::memory_order_relaxed);
	}
	bool push_meshtun(const iot_objref_ptr<iot_meshtun_packet> &st, bool st_islocked=false, iot_hostid_t avoid_peer_id=0, uint16_t maxpathlen=0xffff) { //adds meshtun to outgoing queue for processing, increases refcount
	//returns false if no suitable sessions was found (peer is unroutable or filter was too strict)
		if(!st_islocked) st->lock();
		if(is_unreachable.load(std::memory_order_relaxed)) { //generate immediate error
			if(!st->get_error()) st->set_error(IOT_ERROR_NO_ROUTE);
			if(!st_islocked) st->unlock();
			return false;
		}
		st->ref(); //queue keeps regular pointers, so increase refcount manually
		meshtunq.push((iot_meshtun_packet*)st);
		if(!st_islocked) st->unlock();
		return gwinst->meshcontroller->route_meshtunq(this, avoid_peer_id, maxpathlen);
	}
	bool unpop_meshtun(const iot_objref_ptr<iot_meshtun_packet> &st, bool st_islocked=false, iot_hostid_t avoid_peer_id=0, uint16_t maxpathlen=0xffff) { //adds meshtun to HEAD of outgoing queue for processing, increases refcount
	//returns false if no suitable sessions was found (peer is unroutable or filter was too strict)
		if(!st_islocked) st->lock();
		if(is_unreachable.load(std::memory_order_relaxed)) { //generate immediate error
			if(!st->get_error()) st->set_error(IOT_ERROR_NO_ROUTE);
			if(!st_islocked) st->unlock();
			return false;
		}
		st->ref(); //queue keeps regular pointers, so increase refcount manually
		uv_mutex_lock(&meshtunq_lock);
		meshtunq.unpop((iot_meshtun_packet*)st);
		uv_mutex_unlock(&meshtunq_lock);
		if(!st_islocked) st->unlock();
		return gwinst->meshcontroller->route_meshtunq(this, avoid_peer_id, maxpathlen);
	}
	iot_objref_ptr<iot_meshtun_packet> pop_meshtun(iot_hostid_t sesviahost=0, uint16_t pathlen=0xffff) { //returns next meshtun for processing. can be called from any thread
		iot_meshtun_packet *skipped_head=NULL, *skipped_tail=NULL;
		iot_meshtun_packet* st;
		uv_mutex_lock(&meshtunq_lock);
		do {
			st=meshtunq.pop();
			if(!st || st->type!=TUN_FORWARDING) break;
			iot_meshtun_forwarding *fw=static_cast<iot_meshtun_forwarding*>((iot_meshtun_packet*)st);
			if(!fw->from_host || (fw->from_host!=sesviahost && fw->request->ttl>=pathlen)) break;
			//here we got forwarding request which cannot be forwarded via session to sesviahost or path is too long. request must be skipped
			st->next.store(NULL, std::memory_order_relaxed);
			if(!skipped_tail) {
				skipped_head=skipped_tail=st;
			} else {
				skipped_tail->next.store(st, std::memory_order_relaxed);
				skipped_tail=st;
			}
		} while(1);
		//here st contains allowed request or is NULL if no such requests found
		//skipped_head list of requests must be reinjected
		if(skipped_head) meshtunq.unpop_list(skipped_head);
		uv_mutex_unlock(&meshtunq_lock);
		return iot_objref_ptr<iot_meshtun_packet>(true, st); //additional refcount is kept by iot_objref_ptr
	}
	bool remove_meshtun(iot_meshtun_packet* st) { //tries to find meshtun in queue and remove it. returns true if was found and removed (in such refcount is decreased)
		uv_mutex_lock(&meshtunq_lock);
		bool rval=meshtunq.remove(st);
		uv_mutex_unlock(&meshtunq_lock);
		if(rval) {
			st->unref();
			return true;
		}
		return false;
	}
	void abort_meshtuns(int err) { //removes all meshtuns from queue
		uv_mutex_lock(&meshtunq_lock);
		iot_meshtun_packet* st=meshtunq.pop_all();
		uv_mutex_unlock(&meshtunq_lock);

		while(st) {
			iot_meshtun_packet *next=st->next.load(std::memory_order_relaxed);
			st->lock();
			if(!st->get_error()) st->set_error(err);
			st->unlock();
			st->unref();
			st=next;
		}
	}

	int start_iot_session(void);
	void stop_iot_session(void); //asks IOT session to stop gracefully
/*	bool is_failed(void) { //check if peer can be treated as dead
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
*/
private:
	int resize_origroutes_buffer(uint32_t n, iot_memallocator* allocator);
	void on_graceful_shutdown(void) {
		reset_connections();
//		if(iotgw_clientcon) {
//			iotgw_clientcon->destroy();
//			iotgw_clientcon=NULL;
//		}
	}
};


class iot_peers_iterator {
	iot_peer *peer;
	iot_peers_registry_t *reg;

public:
	iot_peers_iterator(iot_peers_registry_t *reg);
	~iot_peers_iterator(void);
//	bool operator!=(const iot_peers_iterator& op) const {
//		return peer!=op.peer;
//	}
	bool operator!=(const iot_peer *p) const {
		return peer!=p;
	}
	iot_peer* operator*(void) const {
		return peer;
	}
	iot_peers_iterator& operator++(void) {
		assert(peer!=NULL);
		peer=peer->next;
		return *this;
	}
};

class iot_peers_registry_t : public iot_netconregistryiface {
	friend class iot_peers_iterator;

	iot_peer *peers_head=NULL;
	uv_mutex_t datamutex; //used to protect local 'cons', 'objid_keys', last_objid, passive_cons_head and iot_peer's 'cons_head', 'cons_num'
	uv_rwlock_t peers_lock; //use to protect peers_head list

//	const uint32_t maxcons;
//	iot_netcon **cons=NULL; //space for maximum number of connections (all of them must be either in listening_cons_head or connected_cons_head list)
//	uint32_t *objid_keys=NULL;
//	uint32_t last_objid=0;
	volatile std::atomic<uint32_t> num_peers={0};

	iot_netcon* passive_cons_head=NULL; //explicit and automatically added passive (listening and connected listening) iot_netcon objects

	iot_objref_ptr<iot_netproto_config_mesh> listenprotocfg;
	iot_objref_ptr<iot_netproto_config_iotgw> iotlistenprotocfg;

//	iot_netcon_mesh* iotgw_listencon=NULL; //listening mesh netcon used to accept IOT GW sessions. connecting peer must have LOWER host id than this host

public:
	iot_gwinstance* const gwinst;
	bool is_shutdown=false;

	enum copypeers_mode {
		MODE_ALL,
		MODE_MESHSES,
		MODE_ROUTABLE
	};

	iot_peers_registry_t(iot_gwinstance* gwinst/*, uint32_t maxcons=256*/) : /*maxcons(maxcons), */gwinst(gwinst) {
		assert(gwinst!=NULL);
//		assert(maxcons>0);
		int err=uv_mutex_init_recursive(&datamutex);
		assert(!err);
		err=uv_rwlock_init(&peers_lock);
		assert(!err);
	}

	virtual ~iot_peers_registry_t(void) {
		assert(uv_thread_self()==main_thread);
		//release all peer items
		int err=uv_rwlock_trywrlock(&peers_lock);
		assert(!err);
		err=uv_mutex_trylock(&datamutex);
		assert(!err);

		iot_peer* pnext=peers_head, *p;
		while((p=pnext)) {
			pnext=pnext->next;

			BILINKLIST_REMOVE(p, next, prev);
			num_peers.fetch_sub(1, std::memory_order_relaxed);
			p->meshprotocfg.clear();
			p->iotprotocfg.clear();

			for(iot_netcon *cur, *next=p->cons_head; (cur=next); ) {
				next=next->registry_next;
				BILINKLIST_REMOVE(cur, registry_next, registry_prev);
			}

			p->unref();
		}
		assert(num_peers.load(std::memory_order_relaxed)==0);

		for(iot_netcon *cur, *next=passive_cons_head; (cur=next); ) {
			next=next->registry_next;

			BILINKLIST_REMOVE(cur, registry_next, registry_prev);
//			cur->get_metaclass()->destroy_netcon(cur);  //destructor of netcon must not be called directly, it requires correct stopping
		}
//		if(cons) {
//			free(cons);
//			cons=NULL;
//			objid_keys=NULL;
//		}
//		if(iotgw_listencon) {
//			iotgw_listencon->destroy();
//			iotgw_listencon=NULL;
//		}

		uv_mutex_unlock(&datamutex);
		uv_rwlock_wrunlock(&peers_lock);

		uv_mutex_destroy(&datamutex);
		uv_rwlock_destroy(&peers_lock);
	}

	iot_peers_iterator begin(void) {
		return iot_peers_iterator(this);
	}
	constexpr iot_peer* end(void) {
		return NULL;
	}

	int init(void) { //allocate memory for connections list
		assert(uv_thread_self()==main_thread);
//		assert(cons==NULL);

		listenprotocfg=iot_objref_ptr<iot_netproto_config_mesh>(true, new iot_netproto_config_mesh(gwinst, NULL, object_destroysub_delete, true, NULL));
		if(!listenprotocfg) return IOT_ERROR_NO_MEMORY;

		iotlistenprotocfg=iot_objref_ptr<iot_netproto_config_iotgw>(true, new iot_netproto_config_iotgw(gwinst, NULL, object_destroysub_delete, true));
		if(!iotlistenprotocfg) return IOT_ERROR_NO_MEMORY;

//		size_t sz=(sizeof(iot_netcon *)+sizeof(uint32_t))*maxcons;
//		cons=(iot_netcon **)malloc(sz);
//		if(!cons) return IOT_ERROR_NO_MEMORY;
//		memset(cons, 0, sz);
//		objid_keys=(uint32_t *)(cons+maxcons);
		return 0;
	}
	void graceful_shutdown(void);

	uint32_t get_num_peers(void) {
		return num_peers.load(std::memory_order_relaxed);
	}
	uint32_t copy_meshpeers(iot_peer** buf, size_t bufsize, copypeers_mode mode) { //copy list of peers which have mesh sessions into provided memory buffer. should be called within routing lock
		uv_rwlock_rdlock(&peers_lock);
		iot_peer* p;
		uint32_t maxnum=uint32_t(bufsize/sizeof(iot_peer*));
		uint32_t num=0;
		switch(mode) {
			case MODE_MESHSES: //hosts with existing mesh sessions
				for(p=peers_head; p!=NULL && num<maxnum; p=p->next) {
					if(p->meshsessions_head) buf[num++]=p;
				}
				break;
			case MODE_ROUTABLE: //hosts with existing routes to them
				for(p=peers_head; p!=NULL && num<maxnum; p=p->next) {
					if(p->routing_actualpathlen) buf[num++]=p;
				}
				break;
			case MODE_ALL:
			default:
				for(p=peers_head; p!=NULL && num<maxnum; p=p->next) buf[num++]=p;
				break;
		}
		uv_rwlock_rdunlock(&peers_lock);
		return num;
	}

	iot_objref_ptr<iot_peer> find_peer(iot_hostid_t hostid, bool create=false) {
		iot_objref_ptr<iot_peer> rval;
		if(!hostid) return rval;

		uv_rwlock_rdlock(&peers_lock); //always start from read lock

		iot_peer* p;
		for(p=peers_head; p!=NULL; p=p->next) {
			if(p->host_id==hostid) goto onexit;
		}
		if(create) {
			if(num_peers.load(std::memory_order_relaxed)>255) goto onexit; //THERE IS alloca() call depending on current number of peers!
			//reaquire lock in write mode. have to repeat search
			uv_rwlock_rdunlock(&peers_lock);

			uv_rwlock_wrlock(&peers_lock);

			for(p=peers_head; p!=NULL; p=p->next) {
				if(p->host_id==hostid) break;
			}

			if(!p) {
				//do create
				p=(iot_peer*)main_allocator.allocate(sizeof(iot_peer), true);
				if(p) {
					new(p) iot_peer(gwinst, hostid, object_destroysub_memblock, true); //refcount will be 1
					BILINKLIST_INSERTHEAD(p, peers_head, next, prev);
					num_peers.fetch_add(1, std::memory_order_relaxed);

					p->start_iot_session();
//p->debug=true;
				}
			}

			rval=p;
			uv_rwlock_wrunlock(&peers_lock);
			return rval;
		}
onexit:
		rval=p;
		uv_rwlock_rdunlock(&peers_lock);
		return rval;
	}

	int set_peer_connections(iot_peer* peer, json_object *json, bool prefer_ownthread); //set list of connections by given JSON array with parameters for iot_netcon-derived classes
	int reset_peer_connections(iot_peer* peer);

	int add_listen_connections(json_object *json, bool prefer_ownthread=false); //add several server (listening) connections by given JSON array with parameters for iot_netcon-derived classes

	void on_meshroute_set(iot_peer* peer);
	void on_meshroute_reset(iot_peer* peer);

	int start_iot_listen(void) { //must be started to start listening for IOTGW connections over mesh network
//		if(iotgw_listencon) return IOT_ERROR_NO_ACTION;
		if(!iotlistenprotocfg) {
			assert(false);
			return IOT_ERROR_NOT_INITED;
		}
		auto iotgw_listencon=iot_netcontype_metaclass_mesh::allocate_netcon((iot_netproto_config_iotgw*)iotlistenprotocfg);
		if(!iotgw_listencon) return IOT_ERROR_NO_MEMORY;
		int err;
		err=iotgw_listencon->init_server(gwinst->meshcontroller, 0, this);
		if(err) { //invalid args? no slots? TODO
			iotgw_listencon->meta->destroy_netcon(iotgw_listencon);
//			iotgw_listencon=NULL;
			assert(false);
			return err;
		}
		err=iotgw_listencon->start_uv(NULL, false);
		if(err && err!=IOT_ERROR_NOT_READY) {
			assert(false);
			return err;
		}
		return 0;
	}

	virtual int on_new_connection(iot_netcon *conobj) override { //called from any thread, part of iot_netconregistryiface
//IOT_ERROR_NOT_INITED - registry wasn't inited
//IOT_ERROR_LIMIT_REACHED
		if(!listenprotocfg) {
			assert(false); //registry must be inited
			return IOT_ERROR_NOT_INITED;
		}

		uv_mutex_lock(&datamutex);
//		int32_t newidx=find_free_index();
		int err=0;
//		if(newidx<0) {
//			outlog_error("peer connection registry error: connections overflow");
//			err=IOT_ERROR_LIMIT_REACHED;
//			goto onexit;
//		}
//		assign_con(conobj, newidx);
//		conobj->assign_objid(iot_objid_t(iot_objid_t::OBJTYPE_PEERCON, newidx, objid_keys[newidx]));

//		if(conobj->protoconfig && conobj->protoconfig->get_metaclass()==&iot_netprototype_metaclass_iotgw::object) {
//		}

		if(!conobj->protoconfig || !conobj->protoconfig->customarg) {
			BILINKLIST_INSERTHEAD(conobj, passive_cons_head, registry_next, registry_prev);
		} else {
			iot_peer *p=(iot_peer*)conobj->protoconfig->customarg;
			BILINKLIST_INSERTHEAD(conobj, p->cons_head, registry_next, registry_prev);
		}
//onexit:
		uv_mutex_unlock(&datamutex);
		return err;
	}
	virtual void on_destroyed_connection(iot_netcon *conobj) override { //called from any thread, part of iot_netconregistryiface
		if(!listenprotocfg) {
			assert(false); //registry must be inited
			return;
		}
		assert(conobj->registry_prev!=NULL);
		uv_mutex_lock(&datamutex);

		//free the index
//		const iot_objid_t& id=conobj->get_objid();
//		if(id) {
//			assert(cons[id.idx]==conobj);
//			cons[id.idx]=NULL;
//			conobj->assign_objid(iot_objid_t(iot_objid_t::OBJTYPE_PEERCON));
//		}

		BILINKLIST_REMOVE(conobj, registry_next, registry_prev);
		uv_mutex_unlock(&datamutex);
	}


private:
//	void assign_con(iot_netcon* conobj, uint32_t idx) {
//		cons[idx]=conobj;
//		if(!++objid_keys[idx]) objid_keys[idx]++; //avoid zero value
//	}
//	int32_t find_free_index(void) { //must be called under datamutex !!!
//		uint32_t i=maxcons;
//		for(; i>0; i--) {
//			last_objid=(last_objid + 1) % maxcons;
//			if(!cons[last_objid]) return int32_t(last_objid);
//		}
//		return -1;
//	}

};

inline int iot_peer::set_connections(json_object *json, bool prefer_ownthread) { //set list of connections by given JSON array with parameters for iot_netcon-derived classes
		if(!prev) return 0; //checks if peer still in registry
		return gwinst->peers_registry->set_peer_connections(this, json, prefer_ownthread);
	}
inline int iot_peer::reset_connections(void) {
		if(!prev) return 0; //checks if peer still in registry
		return gwinst->peers_registry->reset_peer_connections(this);
	}

inline iot_peers_iterator::iot_peers_iterator(iot_peers_registry_t *reg) : reg(reg) {
		uv_mutex_lock(&reg->datamutex);
		peer=reg->peers_head;
	}
inline iot_peers_iterator::~iot_peers_iterator(void) {
		uv_mutex_unlock(&reg->datamutex);
	}


/*
//represents set of iot_netcon objects to iotgw peers
class iot_peers_conregistry : public iot_netconregistryiface {
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
