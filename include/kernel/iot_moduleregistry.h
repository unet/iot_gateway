#ifndef IOT_MODULEREGISTRY_H
#define IOT_MODULEREGISTRY_H
//Contains constants, methods and data structures for modules management

#include<stdint.h>
#include<assert.h>

#include<ecb.h>

#include<iot_module.h>
#include <iot_kapi.h>
#include <kernel/iot_common.h>

struct iot_module_item_t;
struct iot_modinstance_item_t;

enum iot_modinstance_type_t : uint8_t {
	IOT_MODINSTTYPE_DRIVER=1,	//module which realizes interface of hardware device driver iface_device_driver
	IOT_MODINSTTYPE_EVSOURCE=2,	//module which can be source of events. Realizes iface_event_source interface
//	IOT_MODINSTTYPE_ACTIVATOR=3,	//module which takes N-sources and generates true/false output for another activator or action list. Realizes iface_activator interface
//	IOT_MODINSTTYPE_EXECUTOR=4,	//module which can be manipulated by conditions and also can be source of events. Realizes both iface_event_source and iface_cond_target
//#define IOT_MODULETYPE_DATA			5	//module which realizes interface of data source iface_data_source
};

class iot_modules_registry_t;
extern iot_modules_registry_t *modules_registry;

struct iot_devifacecls_item_t;

#include<kernel/iot_deviceregistry.h>
#include<kernel/iot_configregistry.h>
#include<kernel/iot_deviceconn.h>
#include<kernel/iot_kernel.h>

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
	iot_module_item_t *next_evsrc, *prev_evsrc;//position in iot_modules_registry_t::evsrc_head list
	iot_moduleconfig_t *config;
	iot_modulesdb_item_t *dbitem;
	uint8_t driver_blocked:1, //driver returned critical bug and should not be retried
		evsrc_blocked:1; //event source returnec critical bug and should not be retried

	iot_modinstance_item_t* driver_instances_head; //list of existing driver instances (module must have driver interface)
	iot_modinstance_item_t* evsrc_instances_head; //list of existing driver instances (module must have driver interface)
};

#define IOT_MAX_MODINSTANCES 8192
#define IOT_MAX_DRIVER_CLIENTS 3

struct iot_modinstance_item_t {
	iot_modinstance_item_t *next_inmod, *prev_inmod; //next instance of same type and module_id  for the list with head in iot_module_item_t::[driver|evsource|executor|activator]_instances_head
	iot_modinstance_item_t *next_inthread, *prev_inthread; //next instance of same type and module_id  for the list with head in iot_module_item_t::[driver|evsource|executor|activator]_instances_head
	iot_module_item_t *module;
	iot_thread_item_t *thread;
	iot_config_inst_item_t* cfgitem;
	void *instance;
	uint64_t started; //non-zero if instance is running
	uint64_t start_after; //for non-running instances with scheduled restart, time of restart
	iot_miid_t miid; //module instance id (index in iot_modinstances array and creation time). zero iid field indicates unused structure.
	iot_modinstance_type_t type;
	uint8_t cpu_loading; //copied from corresponding iface from module's config
	union { //type-dependent additional data about instance
		struct {
			iot_hwdevregistry_item_t* driver_hwdev; //ref to hw device item for driver instance
			iot_device_connection_t* driver_conn[IOT_MAX_DRIVER_CLIENTS]; //connections from clients
		};
		struct {
			iot_device_connection_t* evsrc_devconn[IOT_CONFIG_MAX_EVENTSOURCE_DEVICES]; //connections to devices
			uint8_t evsrc_devblock[IOT_CONFIG_MAX_EVENTSOURCE_DEVICES]; //flag that corresponding connection must not be retried until configured device changes
		};
	};
};


struct iot_devifacecls_item_t {
	iot_devifacecls_item_t *next, *prev; //for position in iot_modules_registry_t::devifacecls_head
	iot_devifacecls_config_t* cfg;
	uint32_t module_id; //module which gave this definition. zero for built-in classes
};


class iot_modules_registry_t {
	iot_module_item_t *all_head;
	iot_module_item_t *detectors_head;
	iot_module_item_t *drivers_head;
	iot_module_item_t *evsrc_head;
	iot_devifacecls_item_t *devifacecls_head;

	int register_module(iot_moduleconfig_t* cfg, iot_modulesdb_item_t *dbitem); //main thread

	iot_modinstance_item_t* register_modinstance(iot_module_item_t* module, iot_modinstance_type_t type, iot_thread_item_t *thread, void* instance, iot_config_inst_item_t *cfgitem); //main thread

	void free_modinstance(iot_modinstance_item_t* modinst); //main thread
	int register_devifaceclasses(iot_devifacecls_config_t* cfg, uint32_t num, uint32_t module_id); //main thread


public:
	iot_modules_registry_t(void) : all_head(NULL), detectors_head(NULL), drivers_head(NULL), evsrc_head(NULL) {
		assert(modules_registry==NULL);
		modules_registry=this;
	}

	iot_devifacecls_item_t* find_devifaceclass(iot_deviface_classid clsid, bool tryload) {
		iot_devifacecls_item_t* item=devifacecls_head;
		while(item) {
			if(item->cfg->classid==clsid) return item;
			item=item->next;
		}
		if(tryload && IOT_DEVIFACECLASS_CUSTOM_MODULEID(clsid)>0) {
			if(!load_module(-1, IOT_DEVIFACECLASS_CUSTOM_MODULEID(clsid), NULL)) return find_devifaceclass(clsid, false);
		}
		return NULL;
	}

	//loads modules marked for autoload, starts detectors. must be called once during startup
	void start(iot_devifacecls_config_t* devifaceclscfg, uint32_t num); //main thread

	//tries to load module and register. module's module_init method is also called
	int load_module(int module_index, uint32_t module_id, iot_module_item_t**rval); //main thread

	//after detecting new hw device tries to find appropriate driver
	void try_find_driver_for_hwdev(iot_hwdevregistry_item_t* devitem);  //main thread

	void try_connect_driver_to_consumer(iot_modinstance_item_t *drvinst);

	//having new non-started instance of event source tries to find configured or suitable driver for device with index idx
	int try_connect_evsrc_to_driver(iot_modinstance_item_t *modinst, uint8_t idx); //main thread

	//try to create driver instance for provided real device
	int create_driver_modinstance(iot_module_item_t* module, iot_hwdevregistry_item_t* devitem); //main thread

	int create_evsrc_modinstance(iot_module_item_t* module, iot_config_inst_item_t* item); //main thread

//METHODS CALLED IN OTHER THREADS

	//tries to start module instance. if fails, schedules restart.
	void start_modinstance(iot_modinstance_item_t *modinstance); //any thread
	void notify_device_attached(iot_device_connection_t* conn);
	void process_device_attached(iot_device_connection_t* conn);
};


#endif //IOT_MODULEREGISTRY_H
