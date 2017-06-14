#ifndef IOT_MODULEREGISTRY_H
#define IOT_MODULEREGISTRY_H
//Contains constants, methods and data structures for modules management

#include<stdint.h>
#include<assert.h>
#include <atomic>

#include<ecb.h>

#include<iot_module.h>
#include <kernel/iot_common.h>
#include <kernel/iot_daemonlib.h>

struct iot_module_item_t;
struct iot_modinstance_item_t;

enum iot_modinstance_type_t : uint8_t {
	IOT_MODINSTTYPE_DETECTOR=0,
	IOT_MODINSTTYPE_DRIVER=1,	//module which realizes interface of hardware device driver iface_device_driver
	IOT_MODINSTTYPE_NODE=2,	//module which can be source of events. Realizes iface_event_source interface
//	IOT_MODINSTTYPE_ACTIVATOR=3,	//module which takes N-sources and generates true/false output for another activator or action list. Realizes iface_activator interface
//	IOT_MODINSTTYPE_EXECUTOR=4,	//module which can be manipulated by conditions and also can be source of events. Realizes both iface_event_source and iface_cond_target
//	IOT_MODINSTTYPE__DATA=5	//module which realizes interface of data source iface_data_source

	IOT_MODINSTTYPE_MAX=2
};

extern const char* iot_modinsttype_name[];


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
extern iot_modules_registry_t *modules_registry;

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

#include<kernel/iot_kernel.h>


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

	iot_modinstance_item_t *next_inmod, *prev_inmod; //next instance of same type and module_id  for the list with head in iot_module_item_t::[driver|evsource|executor|activator]_instances_head
	iot_modinstance_item_t *next_inthread, *prev_inthread; //next instance for the list with head in iot_thread_item_t::instances_head
	iot_module_item_t *module;
	iot_thread_item_t *thread;
	iot_module_instance_base *instance;
	uint64_t started; //non-zero if instance was requested to start. this property is assigned in main thread.
	uint64_t state_timeout; //for state-related delayed tasks time of recheck (exact task is determined by state/target_state)
	iot_atimer_item instrecheck_timer; //used to recheck all delayed tasks by checking all error states
	int aborted_error; //contains abortion error code in case modinst aborted itself

	volatile iot_modinstance_state_t state; //inited to IOT_MODINSTSTATE_INITED main thread, updated in working thread
	volatile iot_modinstance_state_t target_state; //assigned in main or working thread
	iot_modinstance_type_t type;
	uint8_t cpu_loading; //copied from corresponding iface from module's config
	volatile std::atomic_flag stopmsglock; //lock protecting access to msgp.stop
	union {
		struct {
			iot_threadmsg_t *start, //for start (from main thread) and for start status (from working thread)
							*stop, //for stop (from main thread)
							*stopstatus; //for stop status (from working thread) and delayed free instance (from any thread, protected by acclock)
		} msgp;
		iot_threadmsg_t* msg_structs[];
	};

	union modinsttype_data_t { //type-dependent additional data about instance. non-main thread can access it for started instance only
		struct { //driver
			iot_hwdevregistry_item_t* hwdev; //ref to hw device item for driver instance
			iot_device_connection_t* conn[IOT_MAX_DRIVER_CLIENTS]; //connections from clients
			dbllist_list<iot_device_entry_t, iot_mi_inputid_t, uint32_t, 1> retry_clients; //list of local client instances which can be retried later or blocked forever
			struct {
				iot_devifacetype type;
				const iot_devifacetype_iface* iface; //first NULL value here ends the list
			} devclasses[IOT_CONFIG_MAX_CLASSES_PER_DEVICE]; //list of available device iface classes (APIs for device communication)
			uint32_t retry_clients_timeout; //non-zero value tells that retry for consumer connections search is waiting
			uint8_t announce_connfree_once:1, //flag that after clearing any conn[] pointer search for other clients must be reattempted ONCE (like after driver start)
											//before this retry_clients must be cleared from special values 0xFFFFFFFE. other hosts must be notified using some
											//special event to clear same special values and recheck their clients 
					announce_connclose:1; //flag that after closing any establixhed connection search for other clients must be reattempted (like after driver start)
											//before this retry_clients must be cleared from special values 0xFFFFFFFD. other hosts must be notified using some
											//special event to clear same special values and recheck their clients. This flag is NOT
											//cleared until moment when ALL client connections are closed
		} driver;

		struct { //ev src
			iot_driverclient_conndata_t dev[IOT_CONFIG_MAX_NODE_DEVICES]; //data per each possible driver connection
			iot_nodemodel *model;
		} node;

		modinsttype_data_t(void) {} //to make compiler happy
		~modinsttype_data_t(void) {} //to make compiler happy
	} data;
private:
	volatile std::atomic_flag acclock; //lock protecting access to next 2 fields
	int8_t refcount; //how many times this struct was locked. can be accessed under acclock only
	uint8_t pendfree; //flag that this struct in waiting for zero in refcount to be freed. can be accessed under acclock only
	iot_miid_t miid; //module instance id (index in iot_modinstances array and creation time). zero iid field indicates unused structure.
	bool in_recheck_job;

public:
	iot_modinstance_item_t(void) {
	}
	bool init(const iot_miid_t &miid_, iot_module_item_t* module_, iot_modinstance_type_t type_, iot_thread_item_t *thread_, iot_module_instance_base* instance_);
	void deinit(void);

	const iot_miid_t& get_miid(void) const {return miid;}
	bool is_working_not_stopping(void) { //check if instance is started and not going to be stopped
		return state==IOT_MODINSTSTATE_STARTED && target_state==IOT_MODINSTSTATE_STARTED;
	}
	bool is_working(void) { //check if instance is started
		return state==IOT_MODINSTSTATE_STARTED || state==IOT_MODINSTSTATE_STOPPING;
	}
	bool lock(void) { //tries to lock structure from releasing. returns true if structure can be accessed
		uint8_t c=0;
		while(acclock.test_and_set(std::memory_order_acquire)) {
			//busy wait
			c++;
			if((c & 0x3F)==0x3F) sched_yield();
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
		uint8_t c=0;
		bool canfree=false;
		while(acclock.test_and_set(std::memory_order_acquire)) {
			//busy wait
			c++;
			if((c & 0x3F)==0x3F) sched_yield();
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

//holds locked state of mod instance structure
//does not allow to copy itself, only to move
struct iot_modinstance_locker {
	iot_modinstance_item_t *modinst;
	iot_modinstance_locker(iot_modinstance_item_t *inst) { //bind already locked modinstance structure to locker object. refcount must have been just increased
		assert(inst!=NULL && inst->miid.iid!=0);
		assert(inst->refcount>0); //structure must be locked AND ITS refcount must be already incremented outside
		modinst=inst;
	}
	iot_modinstance_locker(void) {
		modinst=NULL;
	}
	iot_modinstance_locker(const iot_modinstance_locker&) = delete;
	iot_modinstance_locker(iot_modinstance_locker&& src) { //move constructor
		modinst=src.modinst;
		src.modinst=NULL; //invalidate src
	}
	~iot_modinstance_locker(void) {
		if(modinst) {
			modinst->unlock();
			modinst=NULL;
		}
	}
	void unlock(void) {
		assert(modinst!=NULL);
		if(modinst) {
			modinst->unlock();
			modinst=NULL;
		}
	}
	bool lock(iot_modinstance_item_t *inst) { //tries to lock provided structure and increment its refcount. returns false if struture cannot be locked (pending free)
		assert(inst!=NULL && inst->miid.iid!=0);
		assert(modinst==NULL); //locker must be freed
		if(inst->lock()) { //increments refcount
			modinst=inst;
			return true;
		}
		return false;
	}

	iot_modinstance_locker& operator=(iot_modinstance_item_t *inst) = delete; //without this assignment like "iot_modinstance_locker L=NULL or (iot_modinstance_item_t *) is possible)
	iot_modinstance_locker& operator=(const iot_modinstance_locker&) = delete;
	iot_modinstance_locker& operator=(iot_modinstance_locker&& src) noexcept {  //move assignment
		if(this!=&src) {
			modinst=src.modinst;
			src.modinst=NULL; //invalidate src
		}
		return *this;
	}
	
	explicit operator bool() const {
		return modinst!=NULL;
	}
	bool operator !(void) const {
		return modinst==NULL;
	}
};

#include<kernel/iot_deviceregistry.h>
#include<kernel/iot_configregistry.h>
#include<kernel/iot_deviceconn.h>
#include<kernel/iot_peerconnection.h>


inline void iot_driverclient_conndata_t::block_driver(dbllist_node<iot_device_entry_t, iot_mi_inputid_t, uint32_t>* node, uint32_t till) {
		assert(node!=NULL);
		node->val=till;
		retry_drivers.insert_head(node);
		//add to driver's list
		if(node->key1.is_local) {
			node->key1.local->data.driver.retry_clients.insert_head(node);
		} else {
			node->key1.remote->retry_clients.insert_head(node);
		}
	}




//builds unique identifier name for exporting moduleconfig_t object from full module specification
#define IOT_MODULE_CONF(vendor, bundle, name) ECB_CONCAT(ECB_CONCAT(iot_modconf_, ECB_CONCAT(vendor, ECB_CONCAT(__, ECB_CONCAT(bundle, __)))), name)

#define IOT_SOEXT ".so"

struct iot_modulesdb_bundle_t {
	const char *name; //like "vendor/subdirs/bundlename". extension (.so) must be appended
	bool linked; //bundle is statically linked
	bool error; //was error loading, so do not try again
	void *hmodule; //handle of dynamically loaded file or of main executable when linked is true
};


struct iot_modulesdb_item_t {
	uint32_t module_id;
	iot_modulesdb_bundle_t* bundle;
	const char *module_name;
	bool autoload; //module's config must be auto loaded (after loading appropriate bundle into memory)
	bool autostart_detector; //if module has detector interface, it must be started automatically after load
	iot_module_item_t *item; //assigned during module loading
};


//represents loaded and inited module
struct iot_module_item_t {
	iot_module_item_t *next, *prev;  //position in iot_modules_registry_t::all_head list
	iot_module_item_t *next_detector, *prev_detector; //position in iot_modules_registry_t::detectors_head list
	iot_module_item_t *next_driver, *prev_driver;//position in iot_modules_registry_t::drivers_head list
	iot_module_item_t *next_node, *prev_node;//position in iot_modules_registry_t::node_head list
	iot_moduleconfig_t *config;
	iot_modulesdb_item_t *dbitem;
	iot_module_state_t state[IOT_MODINSTTYPE_MAX+1]; //zero index used for detector
	uint8_t errors[IOT_MODINSTTYPE_MAX+1]; //critical errors counter to implement increasing retry period and limit number of retries
	uint64_t timeout[IOT_MODINSTTYPE_MAX+1]; //used when corresponding state is IOT_MODULESTATE_BLOCKED. contains value consistent with uv_now
	iot_atimer_item recheck_timer; //used to recheck all delayed tasks by checking all error states

	iot_modinstance_item_t* detector_instance;
	iot_modinstance_item_t* driver_instances_head; //list of existing driver instances (module must have driver interface)
	iot_modinstance_item_t* node_instances_head; //list of existing node instances (module must have node interface)

	iot_module_item_t(iot_moduleconfig_t* cfg, iot_modulesdb_item_t *dbitem_) : recheck_timer(this, [](void* param)->void {((iot_module_item_t*)param)->recheck_job(false);}) {
		next=prev=next_detector=prev_detector=next_driver=prev_driver=next_node=prev_node=NULL;
		config=cfg;
		dbitem=dbitem_;
		memset(state,0,sizeof(state));
		memset(errors,0,sizeof(state));
		memset(timeout,0,sizeof(state));
		detector_instance=driver_instances_head=node_instances_head=NULL;
		in_recheck_job=false;
	}
	void recheck_job(bool);

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

private:
	bool in_recheck_job;
};




struct iot_devifaceclass_item_t {
	iot_devifaceclass_item_t *next, *prev; //for position in iot_modules_registry_t::devifacecls_head
	const iot_devifacetype_iface* iface;
	uint32_t module_id; //module which gave this definition. zero for built-in classes
};

struct iot_devcontype_item_t {
	iot_devcontype_item_t *next, *prev; //for position in iot_modules_registry_t::devcontypes_head
	const iot_hwdevident_iface* iface;
	uint32_t module_id; //module which gave this definition. zero for built-in classes
};


class iot_modules_registry_t {
	iot_module_item_t *all_head;
	iot_module_item_t *detectors_head;
	iot_module_item_t *drivers_head;
	iot_module_item_t *node_head;

	iot_devifaceclass_item_t *devifacecls_head;
	iot_devcontype_item_t *devcontypes_head;


public:
	bool is_shutdown;

	iot_modules_registry_t(void) : all_head(NULL), detectors_head(NULL), drivers_head(NULL), node_head(NULL), devifacecls_head(NULL), devcontypes_head(NULL) {
		assert(modules_registry==NULL);
		modules_registry=this;
		is_shutdown=false;
	}
	void free_modinstance(const iot_miid_t &miid) { //main thread
		iot_modinstance_item_t* modinst=find_modinstance_byid(miid);
		if(modinst) free_modinstance(modinst);
	}
	void free_modinstance(iot_modinstance_item_t* modinst); //main thread

	//inits some object data, loads modules marked for autoload, starts detectors. must be called once during startup
	void start(const iot_devifacetype_iface** devifaceclscfg, uint32_t num_devifaces,const iot_hwdevident_iface** devcontypescfg, uint32_t num_contypes); //main thread
	//stops detectors and driver instances
	void stop(void); //main thread

	iot_devifaceclass_item_t* find_devifaceclass(iot_devifacetype_id_t clsid, bool tryload) {
		iot_devifaceclass_item_t* item=devifacecls_head;
		while(item) {
			if(item->iface->classid==clsid) return item;
			item=item->next;
		}
		if(tryload && IOT_DEVIFACETYPE_CUSTOM_MODULEID(clsid)>0) {
			if(!load_module(-1, IOT_DEVIFACETYPE_CUSTOM_MODULEID(clsid), NULL)) return find_devifaceclass(clsid, false);
		}
		return NULL;
	}
	iot_devcontype_item_t* find_devcontype(iot_hwdevcontype_t contp, bool tryload) { //must run in main thread if tryload is true
		iot_devcontype_item_t* item=devcontypes_head;
		while(item) {
			if(item->iface->contype==contp) return item;
			item=item->next;
		}
		if(tryload && IOT_DEVCONTYPE_CUSTOM_MODULEID(contp)>0) {
			if(!load_module(-1, IOT_DEVCONTYPE_CUSTOM_MODULEID(contp), NULL)) return find_devcontype(contp, false);
		}
		return NULL;
	}


	//tries to load module and register. module's module_init method is also called
	int load_module(int module_index, uint32_t module_id, iot_module_item_t**rval); //main thread

	//after detecting new hw device tries to find appropriate driver
	void try_find_driver_for_hwdev(iot_hwdevregistry_item_t* devitem);  //main thread

	void try_connect_driver_to_consumer(iot_modinstance_item_t *drvinst);

	//having new non-started instance of event source tries to find configured or suitable driver for device with index idx
	int try_connect_consumer_to_driver(iot_modinstance_item_t *modinst, uint8_t idx); //main thread

	//try to create driver instance for provided real device
	int create_driver_modinstance(iot_module_item_t* module, iot_hwdevregistry_item_t* devitem); //main thread

	int create_node_modinstance(iot_module_item_t* module, iot_nodemodel* nodemodel); //main thread
	void create_detector_modinstance(iot_module_item_t* module); //main thread

	void graceful_shutdown(void) { //must be called by hwdev_registry->graceful_shutdown after stopping all modinstances
		assert(!is_shutdown);
		is_shutdown=true;
		outlog_debug("in graceful_shutdown");
		uv_stop (main_loop);
	}

//METHODS CALLED IN OTHER THREADS

	iot_modinstance_locker get_modinstance(const iot_miid_t &miid); //any thread


private:
	int register_devifaceclasses(const iot_devifacetype_iface** iface, uint32_t num, uint32_t module_id); //main thread
	int register_devcontypes(const iot_hwdevident_iface** iface, uint32_t num, uint32_t module_id); //main thread

	int register_module(iot_moduleconfig_t* cfg, iot_modulesdb_item_t *dbitem); //main thread

	iot_modinstance_item_t* register_modinstance(iot_module_item_t* module, iot_modinstance_type_t type, iot_thread_item_t *thread, iot_module_instance_base* instance); //main thread
	iot_modinstance_item_t* find_modinstance_byid(const iot_miid_t &miid); //kernel code in main thread

};


#endif //IOT_MODULEREGISTRY_H
