#ifndef IOT_MODULE_H
#define IOT_MODULE_H

#include<stdint.h>
#include<time.h>

#include<ecb.h>

#include<uv.h>


#include <iot_kapi.h>


#ifndef DAEMON_KERNEL
////////////////////////////////////////////////////////////////////
///////////////////////////Specific declarations for external modules
////////////////////////////////////////////////////////////////////

//BEFORE INCLUDING THIS FILE BY MODULE SOURCE SUCH DEFINES MUST BE DONE:
//#define IOT_VENDOR vendor		//"unet" in "unet:generic:sensor:temperature" type spec
//#define IOT_BUNDLE bundle		//"generic:sensor" in same type spec with ":" changed into "__"

#ifndef IOT_VENDOR
#error IOT_VENDOR must be defined before including iot_module.h
#endif

#ifndef IOT_BUNDLE
#error IOT_BUNDLE must be defined before including iot_module.h
#endif

//builds unique identifier name for exporting moduleconfig_t object from module bundle
#define IOT_MODULE_CONF(name) ECB_CONCAT(ECB_CONCAT(iot_modconf_, ECB_CONCAT(IOT_VENDOR, ECB_CONCAT(__, ECB_CONCAT(IOT_BUNDLE, __)))), name)



#endif //DAEMON_KERNEL


////////////////////////////////////////////////////////////////////
///////////////////////////Declarations for everybody
////////////////////////////////////////////////////////////////////



//action code constants for different kapi functions
enum iot_action_t {
	IOT_ACTION_ADD=1,
	IOT_ACTION_REMOVE=2,
	IOT_ACTION_REPLACE=3
};


#define IOT_CONFIG_MAX_EVENTSOURCE_DEVICES 2

extern iot_hostid_t iot_current_hostid; //ID of current host in user config

extern uint64_t iot_starttime_ms; //start time of process like returned by uv_now (in ms since unknown point)



typedef struct {
	uint32_t created;
	iot_iid_t iid;
} iot_miid_t; //type which identifies module instance. has embedded modified timestamp of creation


#include <iot_srcstate.h>
#include <iot_hwdevreg.h>


//#define IOT_MODULETYPE_EVSOURCE		0	//module which can be source of events. Realizes iface_event_source interface
//#define IOT_MODULETYPE_EXECUTOR		1	//module which can be manipulated by conditions and also can be source of events. Realizes both iface_event_source and iface_cond_target
//#define IOT_MODULETYPE_ACTIVATOR	2	//module which takes N-sources and generates true/false output for another activator or action list. Realizes iface_activator interface
//#define IOT_MODULETYPE_DEVDRIVER	3	//module which realizes interface of hardware device driver iface_device_driver
//#define IOT_MODULETYPE_DEVDETECTOR	4	//module which realizes interface of hardware device driver iface_device_driver
//#define IOT_MODULETYPE_DATA			5	//module which realizes interface of data source iface_data_source




// EVENT SOURCE INTERFACE //
enum iot_value_type {		//type of numeric value for iface_event_source interface
	IOT_VALUETYPE_INTEGER,		//unsigned 64 bit integer
	IOT_VALUETYPE_FLOATING		//floating point result
};


////Keeps symbolic identifier of module
//typedef struct {
//	const char *vendor,
//		*bundle, //here every ":" must be changed into "__"
//		*module;
//} iot_module_spec_t;

////////////////////////////////////////////////////
///////////////////////////DEVICE DETECTOR INTERFACE
////////////////////////////////////////////////////

typedef struct {
	uint32_t num_hwdevcontypes:2,				//number of items in hwdevcontypes array in this struct. must be at least 1
			cpu_loading:2;						//average level of cpu loading of started detector. 0 - minimal loading (unlimited such tasks can work in same working thread), 3 - very high loading (this module requires separate working thread for detector)

	int (*start)(time_t cfg_lastmod, const char *json_cfg);	//function to start detector. called in some working thread
	int (*stop)(void);							//function to stop detector. called in same working thread as start

	iot_hwdevcontype_t* hwdevcontypes;			//pointer to array (with num_devcontypes items) of device connection types this module can manipulate
} iot_iface_device_detector_t;



////////////////////////////////////////////////////
///////////////////////////DEVICE DRIVER INTERFACE
////////////////////////////////////////////////////

typedef enum {
	IOT_DEVCONN_ACTION_MESSAGE, //new message came from another side of connection. data_size contains full size of message. data unused
} iot_devconn_action_t;

//globally (between all itogateways and unet) identifies instance of module, like HOST-PID identifies OS processes in cluster on machines
typedef struct {
	iot_hostid_t hostid; //host where module instance is running
	uint32_t module_id;  //module_id of instance in miptr and/or miid.
	iot_miid_t miid; //hostid-local ID of module instance (just opaque enumerator like pid for OS processes)
//	void* miptr; //can be non-NULL if hostid==iot_current_hostid and initiator requested local-only connection to device (it is possible if initiator and driver work in same thread)
} iot_process_ident_t;


//represents driver-side connection of specific driver consumer instance (event source, executor etc) to specific driver instance using specific device interface among supported by driver
typedef struct {
	void *driver_instance;					//set by kernel during creation of handle. instance of Device Driver module where handle is opened
	iot_deviface_classid devclassid;	//set by kernel during creation of handle. specific device iface class among supported by driver instance, selected by creating process when opening handle. Can be zero if no iface was requested and direct access will be used (for local same-thread connection)
	iot_process_ident_t proc;				//set by kernel during creation of handle. identification of process, which created handle (instance of some module)
	void *private_data;						//can be used by driver to store own data connected with handle
} iot_device_handle_t;

//represents consumer-side connection to specific driver instance
typedef struct {
	iot_deviface_classid classid;	//specific device iface class among supported by driver instance
	iot_process_ident_t driver;				//universal identification of driver instance
} iot_device_conn_t;


//struct with operations map, supported by driver for specific device interface class
//all fields are optional. all functions are called in working thread of driver instance
//typedef struct {
//	int (*open)(iot_device_handle_t* handle); //Can be used to init module-specific data in handle
//	int (*close)(iot_device_handle_t* handle);//Can be used to deinit module-specific data in handle
//	int (*action)(iot_device_handle_t* handle, uint32_t action_code, uint32_t data_size, void* data);
//} iot_device_operations_t;

typedef struct {
	uint32_t //num_devclassids:4,					//number of classes of data in state struct and number of items in stclassids list in this struct. can be 0 if driver can only be used directly in same bundle
			cpu_loading:2;						//average level of cpu loading of started driver instance. 0 - minimal loading (unlimited such tasks can work in same working thread), 3 - very high loading (this module requires separate working thread per instance)

	//Called to create instance of driver. Must check provided device and return status telling if device is supported. Always called in main thread
	//Return values:
	//>0 - device is supported, instance was successfully created and saved to *instance.
	//		Value shows how many device iface class ids were added to devifaces array (which has space for max_devifaces and MUST NOT be overrun)
	//IOT_ERROR_DEVICE_NOT_SUPPORTED - device is not supported, so next driver module should be tried
	//IOT_ERROR_INVALID_DEVICE_DATA - provided custom_len is invalid or custom_data has invalid structure. next driver can be tried
	//IOT_ERROR_NO_MEMORY - cannot allocate dynamic data. operation can be retried in a short time. next driver should be tried immediately.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked (may be for some time). next driver should be tried
	//IOT_ERROR_TEMPORARY_ERROR - driver init should be retried after some time (with progressive interval). next driver should be tried immediately.
	int (*init_instance)(void**instance, uv_thread_t thread, iot_hwdev_data_t* dev_data, iot_deviface_classid* devifaces, uint8_t max_devifaces);

	int (*deinit_instance)(void* instance);		//always called in main thread

	//Can be called to check if driver can work with specific device. Can be called from any thread and is not connected with specific instance.
	//Return values:
	//0 - device is supported, instance can be created
	//IOT_ERROR_DEVICE_NOT_SUPPORTED - device is not supported
	//IOT_ERROR_INVALID_DEVICE_DATA - provided custom_len is invalid or custom_data has invalid structure
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked
	//IOT_ERROR_TEMPORARY_ERROR - check failed for temporary reason
	int (*check_device)(const iot_hwdev_data_t* dev_data);

	//Called to start work of previously inited instance. Called in working thread.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked (may be for some time). this instance must be deinited. next driver should be tried
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. this instance must be deinited and retried later. next driver should be tried.
	int (*start)(void* instance, iot_iid_t iid);
	int (*stop)(void* instance);				//called in working thread

//	iot_deviface_classid* devclassids;			//pointer to array (with num_devclassids items) of class ids of device interfaces which this module provides

//device handle operations map
	int (*open)(void* instance, iot_connid_t connid, iot_deviface_classid classid, void **privdata); //Can be used to init module-specific private_data in handle
	int (*close)(void* instance, iot_connid_t connid, iot_deviface_classid classid, void* privdata);//Can be used to deinit module-specific private_data in handle
	int (*action)(void* instance, iot_connid_t connid, iot_deviface_classid classid, void* privdata, iot_devconn_action_t action_code, uint32_t data_size, void* data);

//	iot_device_operations_t* devops;			//pointer to array (with num_devclassids items) of map of supported operations for corresponding device class
} iot_iface_device_driver_t;


///////////////////////////EVENT SOURCE INTERFACE
typedef struct {
	uint32_t num_devices:2,						//number of devices this module should be connected to, 0, 1 or 2. (limited by IOT_CONFIG_MAX_EVENTSOURCE_DEVICES)
			num_stclassids:4,					//number of classes of data in state struct and number of items in stclassids list in this struct. can be 0
			cpu_loading:2;						//average level of cpu loading of started instance. 0 - minimal loading (unlimited number of such tasks can work in same working thread), 3 - very high loading (this module requires separate working thread per instance)
	struct {
		uint8_t num_classids:4,					//number of allowed classes for current device (number of items in classids list in this struct). can be zero when item is unused (num_devices <= index of current struct) of when ANY classid is suitable.
			flag_canauto:1,						//flag that current device can be automatically selected by kernel according to classids. otherwise only user can select device
			flag_localonly:1;					//flag that current driver (and thus hwdevice) must run on same host as evsrc instance
		iot_deviface_classid* classids;		//pointer to array (with num_classids items) of class ids of devices this module can be connected to
	} devcfg[IOT_CONFIG_MAX_EVENTSOURCE_DEVICES];
	enum iot_value_type value_type;				//specifies type of 'value' field of event_source_state_t
//	size_t state_size;							//real size of state struct (sizeof(iot_srcstate_t) + custom_len)

	//reads current state of module into provided statebuf. size of statebuf must be at least state_size. Can be called from any thread. returns 0 on success or negative error code
	int (*init_instance)(void**instance, uv_thread_t thread, uint32_t iot_id, const char *json_cfg); //always called in main thread
	int (*deinit_instance)(void* instance);			//called in main thread
	int (*start)(void* instance, iot_iid_t iid);				//called in working thread
	int (*stop)(void* instance);				//called in working thread
	void (*device_attached)(void* instance, int index, iot_connid_t connid, iot_device_conn_t *devconn);	//called in working thread
	void (*device_detached)(void* instance, int index);	//called in working thread
	void (*device_action)(void* instance, int index, iot_deviface_classid classid, iot_devconn_action_t action_code, uint32_t data_size, void* data);	//called in working thread
//	int (*get_state)(void* instance, iot_srcstate_t* statebuf, size_t bufsize);

	iot_state_classid* stclassids;				//pointer to array (with num_stclassids items) of class ids of state data which this module provides
} iot_iface_event_source_t;
/////////////////////////////




typedef struct {
	uint32_t module_id;								//system assigned unique module ID for consistency check
	uint32_t version;								//module version (0xHHLLREVI = HH.LL.REVI)
	uint32_t num_devifaces:2;						//number of items in deviface_config array. can be 0
	int (*init_module)(void);						//always called in main thread. once after loading (if loaded dynamically) or during startup (if compiled in statically)
	int (*deinit_module)(void);						//called in main thread before unloading dynamically loaded module
	iot_devifacecls_config_t *deviface_config;		//optional array of module-defined device interface classes. can be NULL if num_devifaces==0
	iot_iface_event_source_t *iface_event_source;
	iot_iface_device_driver_t* iface_device_driver;
	iot_iface_device_detector_t *iface_device_detector;
} iot_moduleconfig_t;


#endif //IOT_MODULE_H
