#ifndef IOT_MODULEREGISTRY_H
#define IOT_MODULEREGISTRY_H
//Contains constants, methods and data structures for modules management

#include<stdint.h>
#include<assert.h>
#include <atomic>

#include "iot_core.h"
#include "iot_libregistry.h"
#include "iot_memalloc.h"

struct iot_node_module_item_t;
struct iot_driver_module_item_t;
struct iot_detector_module_item_t;
struct iot_modinstance_item_t;


enum iot_modinstance_state_t : uint8_t {
	IOT_MODINSTSTATE_INITED=0,	//inited but not ever started or successfully stopped
	IOT_MODINSTSTATE_STARTED,	//start succeeded
	IOT_MODINSTSTATE_STOPPING,	//stop was delayed
	IOT_MODINSTSTATE_HUNG		//was started but stop failed, so instance is in indeterminate state and cannot be released
};

enum iot_module_state_t : uint8_t {
	IOT_MODULESTATE_NO=0, //specific TYPE is not realized by module or realization is invalid by interface
	IOT_MODULESTATE_OK,
	IOT_MODULESTATE_BLOCKED,	//module is blocked from new instantiation till TYPE_timeout time
	IOT_MODULESTATE_DISABLED,	//module is blocked from new instantiation forever
};

class iot_modules_registry_t;

struct iot_devifaceclass_item_t;
struct iot_thread_item_t;
struct iot_hwdevregistry_item_t;
struct iot_device_connection_t;
struct iot_config_item_node_t;
struct iot_nodemodel;
struct iot_remote_driverinst_item_t;

#define IOT_MAX_MODINSTANCES 8192
#define IOT_MAX_DRIVER_CLIENTS 3

#define IOT_MSGSTRUCTS_PER_MODINST 2

#include "iot_core.h"


struct iot_device_entry_t { //keeps either iot_modinstance_item_t of local driver or iot_remote_driverinst_item_t
	union {
		iot_modinstance_item_t* local;
		iot_remote_driverinst_item_t* remote;
	};
	bool is_local;

	bool operator==(const iot_device_entry_t &it) const {
		if(is_local!=it.is_local) return false;
		if(is_local) return local==it.local;
		return remote==it.remote;
	}
};

//keeps data about device connection for driver clients (event sources, executors, etc)
struct iot_driverclient_conndata_t {
	iot_device_connection_t* conn; //connections to device
	dbllist_list<iot_device_entry_t, iot_mi_inputid_t, uint32_t, 2> retry_drivers; //list of driver instances which can be retried later or blocked forever.
					//value is uv_now in seconds after which retry can be attempted. special value 0xFFFFFFFF means block forever,
					//0xFFFFFFFE means blocked until driver frees connection slot (set after IOT_MAX_DRIVER_CLIENTS connections to driver exceeded),
					//0xFFFFFFFD means block until driver closes any its established connection (set after IOT_ERROR_LIMIT_REACHED from device_open)
	uint32_t retry_drivers_timeout; //non-zero value tells that any driver search retry is waiting
	uint8_t actual:1,		//flag that this connection data structure is actual for modinstance, i.e. that index of this struct is < module->config->[iface_event_source | executor]->num_devices
		block:1; //flag that corresponding actual connection must not be retried until configured device changes

	static dbllist_node<iot_device_entry_t, iot_mi_inputid_t, uint32_t>* null_retry_node(void) { //just 'auto' type initializer
		return (dbllist_node<iot_device_entry_t, iot_mi_inputid_t, uint32_t>*)NULL;
	}
	dbllist_node<iot_device_entry_t, iot_mi_inputid_t, uint32_t>* find_local_driver_retry_node(iot_modinstance_item_t* drvinst) {
		iot_device_entry_t de={drvinst, true};
		return retry_drivers.find_key1(de);
	}
	dbllist_node<iot_device_entry_t, iot_mi_inputid_t, uint32_t>* find_remote_driver_retry_node(iot_remote_driverinst_item_t* rdrvinst) {
		iot_device_entry_t de;
		de.remote=rdrvinst;
		de.is_local=false;
		return retry_drivers.find_key1(de);
	}
	dbllist_node<iot_device_entry_t, iot_mi_inputid_t, uint32_t>* create_local_driver_retry_node(iot_modinstance_item_t* drvinst, const iot_mi_inputid_t &mi, uint32_t tm, iot_memallocator* allocator) {
		iot_device_entry_t de={drvinst, true};
		return retry_drivers.new_node(de, mi, tm, allocator);
	}

	bool is_driver_blocked(dbllist_node<iot_device_entry_t, iot_mi_inputid_t, uint32_t>* node, uint32_t now32) {
		assert(node!=NULL);
		if(now32>0xFFFFFFF0) { //32-bit overflow. must not occur
			assert(false);
			return true;
		}
		if(node->val>now32) return true;
		//timeout ended
		return false;
	}
	void block_driver(dbllist_node<iot_device_entry_t, iot_mi_inputid_t, uint32_t>* node, uint32_t till);
};


struct iot_modinstance_item_t {
	friend class iot_modules_registry_t;
	friend struct iot_modinstance_locker;

	iot_modinstance_item_t *next_inmod, *prev_inmod; //next instance of same type and module_id  for the list with head in iot_[node|driver|detector]_module_item_t::[node|driver|detector]_instance[s_head|]
	iot_any_module_item_t *module;

	iot_modinstance_item_t *next_inthread, *prev_inthread; //next instance for the list with head in iot_thread_item_t::instances_head
	iot_thread_item_t *thread;

	iot_modinstance_item_t *next_ingwinst, *prev_ingwinst; //next instance for the list with head in iot_gwinstance::[node|driver|detector]_instances_head
	iot_gwinstance *gwinst;

	iot_module_instance_base *instance;
	uint64_t started; //non-zero if instance was requested to start. this property is assigned in main thread.
	uint64_t state_timeout; //for state-related delayed tasks time of recheck (exact task is determined by state/target_state)
	iot_atimer_item instrecheck_timer; //used to recheck all delayed tasks by checking all error states
	int aborted_error; //contains abortion error code in case modinst aborted itself

	volatile iot_modinstance_state_t state; //inited to IOT_MODINSTSTATE_INITED main thread, updated in working thread
	volatile iot_modinstance_state_t target_state; //assigned in main or working thread
	iot_module_type_t type;
	uint8_t cpu_loading; //copied from corresponding iface from module's config
	volatile std::atomic_flag stopmsglock=ATOMIC_FLAG_INIT; //lock protecting access to msgp.stop
	union {
		struct {
			iot_threadmsg_t *start, //for start (from main thread) and for start status (from working thread)
							*stop, //for stop (from main thread)
							*stopstatus; //for stop status (from working thread) and delayed free instance (from any thread, protected by acclock)
		} msgp;
		iot_threadmsg_t* msg_structs[3];
	};

	union modinsttype_data_t { //type-dependent additional data about instance. non-main thread can access it for started instance only
		struct { //driver
			iot_hwdevregistry_item_t* hwdev; //ref to hw device item for driver instance
			iot_device_connection_t* conn[IOT_MAX_DRIVER_CLIENTS]; //connections from clients
			dbllist_list<iot_device_entry_t, iot_mi_inputid_t, uint32_t, 1> retry_clients; //list of local client instances which can be retried later or blocked forever
			iot_deviface_params_buffered devifaces[IOT_CONFIG_MAX_IFACES_PER_DEVICE]; //list of available device iface classes (APIs for device communication)
			uint32_t retry_clients_timeout; //non-zero value tells that retry for consumer connections search is waiting
			uint8_t announce_connfree_once:1, //flag that after clearing any conn[] pointer search for other clients must be reattempted ONCE (like after driver start)
											//before this retry_clients must be cleared from special values 0xFFFFFFFE. other hosts must be notified using some
											//special event to clear same special values and recheck their clients 
					announce_connclose:1, //flag that after closing any established connection, search for other clients must be reattempted (like after driver start)
											//before this retry_clients must be cleared from special values 0xFFFFFFFD. other hosts must be notified using some
											//special event to clear same special values and recheck their clients. This flag is NOT
											//cleared until moment when ALL client connections are closed
					num_devifaces:4;
		} driver;

		struct { //node
			iot_driverclient_conndata_t dev[IOT_CONFIG_MAX_NODE_DEVICES]; //data per each possible driver connection
			iot_nodemodel *model;
		} node;

		modinsttype_data_t(void) {} //to make compiler happy
		~modinsttype_data_t(void) {} //to make compiler happy
	} data;
private:
	volatile std::atomic_flag acclock=ATOMIC_FLAG_INIT; //lock protecting access to next 2 fields
	int8_t refcount; //how many times this struct was locked. can be accessed under acclock only
	uint8_t pendfree; //flag that this struct in waiting for zero in refcount to be freed. can be accessed under acclock only
	iot_miid_t miid; //module instance id (index in iot_modinstances array and creation time). zero iid field indicates unused structure.
	bool in_recheck_job;

public:
	iot_modinstance_item_t(void) {
	}
	bool init(iot_gwinstance* gwinst_, const iot_miid_t &miid_, iot_any_module_item_t* module_, iot_module_type_t type_, iot_thread_item_t *thread_, iot_module_instance_base* instance_, uint8_t cpu_loading_);
	void deinit(void);

	const iot_miid_t& get_miid(void) const {return miid;}
	bool is_working_not_stopping(void) { //check if instance is started and not going to be stopped
		return state==IOT_MODINSTSTATE_STARTED && target_state==IOT_MODINSTSTATE_STARTED;
	}
	bool is_working(void) { //check if instance is started
		return state==IOT_MODINSTSTATE_STARTED || state==IOT_MODINSTSTATE_STOPPING;
	}
	bool lock(void) { //tries to lock structure from releasing. returns true if structure can be accessed
		uint16_t c=1;
		while(acclock.test_and_set(std::memory_order_acquire)) {
			//busy wait
			if(!(c++ & 1023)) sched_yield();
		}
		if(pendfree) { //cannot be locked
			acclock.clear(std::memory_order_release);
			return false;
		}
		assert(refcount<100);
		refcount++;
		acclock.clear(std::memory_order_release);
		return true;
	}
	bool mark_pendfree(void) { //marks structure as pending to be freed and returns true if it can be freed immediately (when refcount is zero)
		uint16_t c=1;
		bool canfree=false;
		while(acclock.test_and_set(std::memory_order_acquire)) {
			//busy wait
			if(!(c++ & 1023)) sched_yield();
		}
		pendfree=1;
		if(refcount==0) canfree=true;
		acclock.clear(std::memory_order_release);
		return canfree;
	}
	void unlock(void); //unlocked previously locked structure. CANNOT BE called if lock() returned false
/*	iot_threadmsg_t* try_get_msgreserv(void) { //tries to lock and returns iot_threadmsg_t struct from statically allocated reserv. returns NULL if no available
		//can work in any thread
		for(int i=0;i<IOT_MSGSTRUCTS_PER_MODINST;i++) {
			if(!msgstruct_usage[i].test_and_set(std::memory_order_acquire)) { //lock obtained (prev value was false)
				memset(&msgstructs[i], 0, sizeof(msgstructs[i]));
				msgstructs[i].is_msginstreserv=1;
				msgstructs[i].miid=miid;
				return &msgstructs[i];
			}
		}
		return NULL;
	}

	void release_msgreserv(iot_threadmsg_t* msg) { //mark provided msg struct from msgstructs as free
		//can work in any thread
		assert(msg);
		assert(msg->is_msginstreserv && msg->miid==miid);
		for(int i=0;i<IOT_MSGSTRUCTS_PER_MODINST;i++) {
			if(msg==&msgstructs[i]) {
				msgstruct_usage[i].clear(std::memory_order_release);
				return;
			}
		}
		assert(false); //must not happen
	}*/
	//tries to start module instance. can schedules retry depending on error and instance type
	//returns 0 if instance was successfully started in sync way
	//returns error code otherwise:
	//
	int start(bool isasync); //thread of instance
	int on_start_status(int err, bool isasync); //main thread
	//stops module instance.
	int stop(bool isasync, bool forcemsg=false); //thread of instance
	int on_stop_status(int err, bool isasync); //main thread

	void recheck_job(bool);
};


//#include "iot_deviceregistry.h"
//#include "iot_configregistry.h"
//#include "iot_deviceconn.h"
//#include "iot_peerconnection.h"





//builds unique identifier name for exporting moduleconfig_t object from full module specification
//#define IOT_MODULE_CONF(vendor, bundle, name) ECB_CONCAT(ECB_CONCAT(iot_modconf_, ECB_CONCAT(vendor, ECB_CONCAT(__, ECB_CONCAT(bundle, __)))), name)


struct iot_any_module_item_t {
	iot_regitem_module_t *dbitem;
	iot_atimer_item recheck_timer; //used to recheck all delayed tasks by checking all error states
	uint64_t timeout=0; //used when corresponding state is IOT_MODULESTATE_BLOCKED. contains value consistent with uv_now
	iot_module_state_t state=IOT_MODULESTATE_NO;
	uint8_t errors=0; //critical errors counter to implement increasing retry period and limit number of retries

	iot_any_module_item_t(iot_regitem_module_t* dbitem, void (*ontimer)(void*)) : dbitem(dbitem), recheck_timer(this, ontimer) {
	}
	virtual ~iot_any_module_item_t(void) {
	}
	virtual void recheck_job(bool)=0;
protected:
	bool in_recheck_job=false;
};

//represents loaded and inited module
struct iot_node_module_item_t : public iot_any_module_item_t {
	iot_node_module_item_t *next_node=NULL, *prev_node=NULL;//position in iot_modules_registry_t::node_head list
	const iot_node_moduleconfig_t *config;

	iot_modinstance_item_t* node_instances_head=NULL; //list of existing node instances (module must have node interface)

	iot_node_module_item_t(const iot_node_moduleconfig_t* cfg, iot_regitem_module_t *dbitem) : 
								iot_any_module_item_t(dbitem, [](void* param)->void {((iot_node_module_item_t*)param)->recheck_job(false);}),
								config(cfg) {
	}
	virtual void recheck_job(bool) override;
};

struct iot_driver_module_item_t : public iot_any_module_item_t {
	iot_driver_module_item_t *next_driver, *prev_driver;//position in iot_modules_registry_t::drivers_head list
	const iot_driver_moduleconfig_t *config;

	iot_modinstance_item_t* driver_instances_head=NULL; //list of existing driver instances (module must have driver interface)

	iot_driver_module_item_t(const iot_driver_moduleconfig_t* cfg, iot_regitem_module_t *dbitem) :
								iot_any_module_item_t(dbitem, [](void* param)->void {((iot_driver_module_item_t*)param)->recheck_job(false);}),
								config(cfg) {
	}
	virtual void recheck_job(bool) override;

	//try to start specific driver for specific device
	int try_driver_create(iot_hwdevregistry_item_t* hwdev); //main thread

	void stop_all_drivers(bool asyncmsg) {
		assert(uv_thread_self()==main_thread);
		iot_modinstance_item_t *modinst, *next=driver_instances_head;
		while((modinst=next)) {
			next=modinst->next_inmod;
			modinst->stop(false, asyncmsg);
		}
	}
};


struct iot_detector_module_item_t : public iot_any_module_item_t {
	iot_detector_module_item_t *next_detector, *prev_detector; //position in iot_modules_registry_t::detectors_head list
	const iot_detector_moduleconfig_t *config;

	iot_modinstance_item_t* detector_instance=NULL;
	iot_detector_module_item_t(const iot_detector_moduleconfig_t* cfg, iot_regitem_module_t *dbitem) :
								iot_any_module_item_t(dbitem, [](void* param)->void {((iot_detector_module_item_t*)param)->recheck_job(false);}),
								config(cfg) {
	}
	virtual void recheck_job(bool) override;
};


class iot_modules_registry_t {
	iot_detector_module_item_t *detectors_head=NULL;
	iot_driver_module_item_t *drivers_head=NULL;
	iot_node_module_item_t *node_head=NULL;

public:
	iot_modules_registry_t(void) {
		assert(modules_registry==NULL);
		modules_registry=this;
	}
	void free_modinstance(const iot_miid_t &miid) { //main thread
		iot_modinstance_item_t* modinst=find_modinstance_byid(miid);
		if(modinst) free_modinstance(modinst);
	}
	void free_modinstance(iot_modinstance_item_t* modinst); //main thread

	//inits some object data, loads modules marked for autoload, starts detectors. must be called once during startup
	void start(void); //main thread
	//stops detectors and driver instances
	void stop(void); //main thread

	//tries to load module and register. module's module_init method is also called
	int load_node_module(uint32_t module_id, iot_node_module_item_t**rval); //main thread
	int load_driver_module(uint32_t module_id, iot_driver_module_item_t**rval); //main thread
	int load_detector_module(uint32_t module_id, iot_detector_module_item_t**rval); //main thread

	//after detecting new hw device tries to find appropriate driver
	void try_find_driver_for_hwdev(iot_hwdevregistry_item_t* devitem);  //main thread

	void try_connect_driver_to_consumer(iot_modinstance_item_t *drvinst);

	//having new non-started instance of event source tries to find configured or suitable driver for device with index idx
	int try_connect_consumer_to_driver(iot_modinstance_item_t *modinst, uint8_t idx); //main thread

	//try to create driver instance for provided real device
	int create_driver_modinstance(iot_driver_module_item_t* module, iot_hwdevregistry_item_t* devitem); //main thread

	int create_node_modinstance(iot_node_module_item_t* module, iot_nodemodel* nodemodel); //main thread

	void create_detector_modinstance(iot_detector_module_item_t* module, iot_gwinstance* gwinst=NULL); //main thread. on server detectors can (or always are?) gwinstance-independent (so gwinst is NULL)

	void graceful_shutdown(void) {
//		assert(!is_shutdown);
//		is_shutdown=true;
		outlog_debug("module registry in graceful_shutdown");

		//stop global detector instances
		iot_detector_module_item_t *mod=detectors_head;
		while(mod) {
			if(mod->detector_instance && !mod->detector_instance->gwinst) mod->detector_instance->stop(false);
			mod=mod->next_detector;
		}
	}


//METHODS CALLED IN OTHER THREADS

	iot_modinstance_locker get_modinstance(const iot_miid_t &miid); //any thread


private:
	int register_module(iot_node_moduleconfig_t* cfg, iot_regitem_module_t *dbitem); //main thread
	int register_module(iot_driver_moduleconfig_t* cfg, iot_regitem_module_t *dbitem); //main thread
	int register_module(iot_detector_moduleconfig_t* cfg, iot_regitem_module_t *dbitem); //main thread

	iot_modinstance_item_t* register_modinstance(iot_gwinstance* gwinst_, iot_any_module_item_t* module, iot_module_type_t type, iot_thread_item_t *thread, iot_module_instance_base* instance, uint8_t cpu_loading); //main thread
	iot_modinstance_item_t* find_modinstance_byid(const iot_miid_t &miid); //core code in main thread

};


#endif //IOT_MODULEREGISTRY_H
