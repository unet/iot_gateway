#ifndef IOT_MODULE_H
#define IOT_MODULE_H

#include<inttypes.h>
#include<time.h>

#include<assert.h>

#include<uv.h>
#include<json-c/json.h>

#include<iot_utils.h>
#include<iot_compat.h>
#include<iot_error.h>

//Log levels
#define LDEBUG  0
#define LINFO   1
#define LNOTICE 2
#define LERROR  3

#ifdef NDEBUG
	#define LMIN    LNOTICE
#else
	#define LMIN    LDEBUG
#endif

extern int min_loglevel;

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


#define kapi_outlog_error(format... ) do_outlog(__FILE__, __LINE__, __func__, LERROR, format)
#define kapi_outlog_notice(format... ) if(LMIN<=LNOTICE && min_loglevel <= LNOTICE) do_outlog(__FILE__, __LINE__, __func__, LNOTICE, format)
#define kapi_outlog_info(format... ) if(LMIN<=LINFO && min_loglevel <= LINFO) do_outlog(__FILE__, __LINE__, __func__, LINFO, format)
#define kapi_outlog_debug(format... ) if(LMIN<=LDEBUG && min_loglevel <= LDEBUG) do_outlog(__FILE__, __LINE__, __func__, LDEBUG, format)
#define kapi_outlog(level, format... ) if(LMIN<=level && min_loglevel <= level) do_outlog(__FILE__, __LINE__, __func__, level, format)

#else

////////////////////////////////////////////////////////////////////
///////////////////////////Specific declarations for kernel
////////////////////////////////////////////////////////////////////

//gets module ID from value obtained by IOT_DEVIFACETYPE_CUSTOM macro
#define IOT_DEVIFACETYPE_CUSTOM_MODULEID(clsid) ((clsid)>>8)

//gets module ID from value obtained by IOT_DEVCONTYPE_CUSTOM macro
#define IOT_DEVCONTYPE_CUSTOM_MODULEID(contp) ((contp)>>8)

#endif //DAEMON_KERNEL


////////////////////////////////////////////////////////////////////
///////////////////////////Declarations for everybody
////////////////////////////////////////////////////////////////////



#define IOT_CONFIG_MAX_NODE_DEVICES 3
#define IOT_CONFIG_MAX_CLASSES_PER_DEVICE 4          //max number if 15
#define IOT_CONFIG_MAX_NODE_VALUEOUTPUTS 31
#define IOT_CONFIG_MAX_NODE_VALUEINPUTS 31
#define IOT_CONFIG_MAX_NODE_MSGOUTPUTS 8
#define IOT_CONFIG_MAX_NODE_MSGINPUTS 8
#define IOT_CONFIG_MAX_NODE_MSGLINKTYPES 8
#define IOT_CONFIG_DEVLABEL_MAXLEN 7
#define IOT_CONFIG_LINKLABEL_MAXLEN 7

#define IOT_MEMOBJECT_MAXPLAINSIZE 8192

#define IOT_MEMOBJECT_MAXREF 250

extern uv_thread_t main_thread;
extern uint64_t iot_starttime_ms; //start time of process like returned by uv_now (in ms since unknown point)

void do_outlog(const char*file, int line, const char* func, int level, const char *fmt, ...);


typedef uint64_t iot_hostid_t;					//type for Host ID
//printf macro for outputting host id
#define IOT_PRIhostid PRIu64
#define IOT_HOSTID_ANY 0xffffffffffffffffull	//value meaning 'any host' in templates of hwdevice ident
#define iot_hostid_t_MAX UINT64_MAX

typedef uint32_t iot_id_t;						//type for IOT ID
#define IOT_PRIiotid PRIu32
#define iot_id_t_MAX UINT32_MAX

typedef uint32_t iotlink_id_t;					//type for LINK ID between nodes
#define IOT_PRIiotlinkid PRIu32
#define iotlink_id_t_MAX UINT32_MAX
#define uint32_t_MAX UINT32_MAX
#define uint16_t_MAX UINT16_MAX
#define uint8_t_MAX UINT8_MAX


extern iot_hostid_t iot_current_hostid; //ID of current host in user config

typedef uint16_t iot_iid_t;					//type for running Instance ID
typedef uint16_t iot_connsid_t;				//type for connection struct ID


typedef uint32_t iot_dataclass_id_t;		//type for class ID of value or message for node input/output
typedef iot_dataclass_id_t iot_msgclass_id_t;	//type for message type ID. MUST HAVE 1 in lower bit


//uniquely identifies event in whole cluster
struct iot_event_id_t {
	uint64_t numerator; //from bottom is limited by current timestamp with microseconds - 1e15 (offset timestamp in seconds on 1`000`000`000)
	iot_hostid_t host_id;

	bool operator!(void) const {
		return numerator==0;
	}
};



//action code constants for different kapi functions
enum iot_action_t : uint8_t {
	IOT_ACTION_ADD=1,
	IOT_ACTION_REMOVE=2,
//	IOT_ACTION_REPLACE=3
};

//type which identifies running module instance
struct iot_miid_t {
	uint32_t created; //modified timestamp of creation (iid can overlap)
	iot_iid_t iid;

	iot_miid_t(void) {
	}
	iot_miid_t(uint32_t created_, iot_iid_t iid_) {
		created=created_;
		iid=iid_;
	}
	iot_miid_t(const iot_miid_t &src) {
		iid=src.iid;
		created=src.created;
	}
	iot_miid_t& operator=(const iot_miid_t &src) {
		if(this!=&src) {
			iid=src.iid;
			created=src.created;
		}
		return *this;
	}
	bool operator==(const iot_miid_t &p) const {
		return iid==p.iid && created==p.created;
	}
	bool operator!=(const iot_miid_t &p) const {
		return !(*this==p);
	}
	bool operator!(void) const {
		return iid==0;
	}
	explicit operator bool(void) const {
		return iid!=0;
	}
	void clear(void) {
		created=0;
		iid=0;
	}
};

//type which identifies running module instance WITH SPECIFIC input/output/device_connection
struct iot_mi_inputid_t {
	uint32_t created; //modified timestamp of creation (iid can overlap)
	iot_iid_t iid;
	uint8_t idx; //input/output/device_connection index

	iot_mi_inputid_t(void) {
	}
	iot_mi_inputid_t(const iot_miid_t &miid, uint8_t i) {
		created=miid.created;
		iid=miid.iid;
		idx=i;
	}
	iot_mi_inputid_t(const iot_mi_inputid_t &src) {
		iid=src.iid;
		created=src.created;
		idx=src.idx;
	}
	iot_mi_inputid_t& operator=(const iot_mi_inputid_t &src) {
		if(this!=&src) {
			iid=src.iid;
			created=src.created;
			idx=src.idx;
		}
		return *this;
	}
	bool operator==(const iot_mi_inputid_t &p) const {
		return iid==p.iid && created==p.created && idx==p.idx;
	}
	bool operator==(const iot_miid_t &p) const {
		return iid==p.iid && created==p.created;
	}
	bool operator!=(const iot_mi_inputid_t &p) const {
		return !(*this==p);
	}
	bool operator!=(const iot_miid_t &p) const {
		return !(*this==p);
	}
	bool operator!(void) const {
		return iid==0;
	}
	explicit operator bool(void) const {
		return iid!=0;
	}
	void clear(void) {
		created=0;
		iid=0;
		idx=0;
	}
};


//type which identifies specific connection session between driver and consumer
struct iot_connid_t {
	uint32_t key; //unique serial number to count how many times connection struct was changed (either end (consumer or driver) set or cleared)
	iot_connsid_t id;

	iot_connid_t(void) {
		id=0;
		key=0;
	}
	iot_connid_t(const iot_connid_t &src) {
		id=src.id;
		key=src.key;
	}
	iot_connid_t& operator=(const iot_connid_t &src) {
		if(this!=&src) {
			id=src.id;
			key=src.key;
		}
		return *this;
	}
	bool operator==(const iot_connid_t &p) const volatile {
		return id==p.id && key==p.key;
	}
	bool operator!=(const iot_connid_t &p) const volatile {
		return !(*this==p);
	}
	bool operator!(void) const volatile {
		return id==0;
	}
	explicit operator bool(void) const volatile {
		return id!=0;
	}
	void clear(void) volatile {
		key=0;
		id=0;
	}
};

struct iot_module_instance_base;
struct iot_device_driver_base;
struct iot_driver_client_base;

#include <iot_hwdevreg.h>

//struct with connection-type specific data about hardware device
struct iot_hwdev_data_t {
	iot_hwdev_ident_t dev_ident; //identification of device (and its connection type)
	const iot_hwdevident_iface* ident_iface; //contype-specific device identification interface
	uint32_t custom_len; //actual length or custom data
	char* custom_data; //dev_ident.contype dependent additional data about device (its capabilities, name etc), must point to at least 4-byte aligned buffer

//	char* get_descr(char* buf, size_t bufsize) { //get contype-dependent name of device
//		return dev_ident.dev.get_descr(buf, bufsize);
//	}
};




//allocate memory block
void *iot_allocate_memblock(uint32_t size, bool allow_direct=false);
//release memory block allocated by iot_allocate_memblock
void iot_release_memblock(void *memblock);
//increments reference count of memory block allocated by iot_allocate_memblock. returns false if reference count overfilled
bool iot_incref_memblock(void *memblock);


static inline uint32_t get_time32(time_t tm) { //gets modified 32-bit timestamp. it has some offset to allow 32-bit var to work many years
	return uint32_t(tm-1000000000);
}

static inline time_t fix_time32(uint32_t tm) { //restores normal timestamp value
	return time_t(tm)+1000000000;
}





//Used by device detector modules for managing registry of available hardware devices
//action is one of {IOT_ACTION_ADD, IOT_ACTION_REMOVE,IOT_ACTION_REPLACE}
//ident.contype must be among those listed in .devcontypes field of detector module interface
//returns:




#include <iot_valueclasses.h>


// EVENT SOURCE INTERFACE //
//enum iot_value_type {		//type of numeric value for iface_node interface
//	IOT_VALUETYPE_INTEGER,		//unsigned 64 bit integer
//	IOT_VALUETYPE_FLOATING		//floating point result
//};


////Keeps symbolic identifier of module
//typedef struct {
//	const char *vendor,
//		*bundle, //here every ":" must be changed into "__"
//		*module;
//} iot_module_spec_t;



//base class for all module instances (detector, driver, event source etc)
struct iot_module_instance_base {
	uv_thread_t thread; //working thread of this instance after start
	iot_miid_t miid;    //modinstance id of this instance after start

	iot_module_instance_base(uv_thread_t thread) : thread(thread), miid(0,0) {
	}

//called in instance thread:

	//sends request to kernel to stop current instance with specific error code. Also is used to notify kernel when instance is ready to stop if stop was delayed by
	//returning IOT_ERROR_TRY_AGAIN from stop(). Possible error codes:
	//0 - no error. can be used after delayed stop() only
	//IOT_ERROR_CRITICAL_BUG - blocks module. so no new instances of current instance type can be created
	//IOT_ERROR_CRITICAL_ERROR - say kernel about incorrect instanciation (i.e. block driver creation using current module for current hw device) or configuration (iot item config is invalid).
	//							no instanciation will be attempted until configuration or module version update
	//IOT_ERROR_TEMPORARY_ERROR - just request to recreate instance after some time due to some temporary error.
	int kapi_self_abort(int errcode);



	//Called to start work of previously inited instance.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, whole system for detector, iot_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(void)=0; 

	//called to stop work of started instance. call can be followed by deinit or started again (if stop was manual, by user)
	//Return values:
	//0 - driver successfully stopped and can be deinited or restarted
	//IOT_ERROR_TRY_AGAIN - driver requires some time (async operation) to stop gracefully. kapi_self_abort() will be called to notify kernel when stop is finished.
	//						anyway second stop() call must free all resources correctly, may be in a hard way. otherwise module will be blocked and left in hang state (deinit
	//						cannot be called until stop reports OK)
	//any other error is treated as critical bug and driver is blocked for further starts. deinit won't be called for such instance. instance is put into hang state
	virtual int stop(void)=0;
};

////////////////////////////////////////////////////
///////////////////////////DEVICE DETECTOR INTERFACE
////////////////////////////////////////////////////
//additional specific interface for driver module instances
struct iot_device_detector_base : public iot_module_instance_base {
	iot_device_detector_base(uv_thread_t thread) : iot_module_instance_base(thread) {
	}

//interface to kernel:
	int kapi_hwdev_registry_action(enum iot_action_t action, iot_hwdev_localident_t* ident, size_t custom_len, void* custom_data);
};



struct iot_iface_device_detector_t {
	const char *descr; //text description of module detector functionality. If begins with '[', then must be evaluated as template
	const char *params_tmpl; //set of templates for showing/editing user params of detector. can be NULL if module has no params
	uint32_t num_hwdevcontypes:2,				//number of items in hwdevcontypes array in this struct. must be at least 1
			cpu_loading:2;						//average level of cpu loading of started detector. 0 - minimal loading (unlimited such tasks can work in same working thread), 3 - very high loading (this module requires separate working thread for detector)

	//Called to create single instance of detector. Must check provided device and return status telling if device is supported. Always called in main thread
	//Return values:
	//0 - device is supported, instance was successfully created and saved to *instance.
	//IOT_ERROR_DEVICE_NOT_SUPPORTED - device is not supported, so next driver module should be tried
	//IOT_ERROR_INVALID_DEVICE_DATA - provided custom_len is invalid or custom_data has invalid structure. next driver can be tried
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be disabled. next driver should be tried
	//IOT_ERROR_TEMPORARY_ERROR - driver init should be retried after some time (with progressive interval). next driver should be tried immediately.
	//other errors treated as IOT_ERROR_CRITICAL_BUG
	int (*init_instance)(iot_device_detector_base**instance, uv_thread_t thread);

	//called to deinit single instance.
	//Return values:
	//0 - success
	//any other error leaves instance in hang state
	int (*deinit_instance)(iot_module_instance_base*instance);		//always called in main thread


	//Can be called to check if detector can work on current system. Can be called from any thread and is not connected with specific instance.
	//Return values:
	//0 - system is supported, instance can be created
	//IOT_ERROR_DEVICE_NOT_SUPPORTED - device is not supported
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked
	//IOT_ERROR_TEMPORARY_ERROR - check failed for temporary reason
	//other errors treated as IOT_ERROR_CRITICAL_BUG
	int (*check_system)(void);

	const iot_hwdevcontype_t* hwdevcontypes;			//pointer to array (with num_devcontypes items) of device connection types this module can manipulate
};





////////////////////////////////////////////////////
///////////////////////////DEVICE DRIVER INTERFACE
////////////////////////////////////////////////////

enum iot_devconn_action_t : uint8_t {
	IOT_DEVCONN_ACTION_MESSAGE, //new message (in full) came from another side of connection. data_size contains full size of message.
								//data contains address of temporary buffer with message body. message must be interpreted by corresponding device interface class with corresponding attributes
	IOT_DEVCONN_ACTION_READY	//sent to DRIVER side when connection is fully inited and messages can be sent over it (in 'open' handler connection is not ready still)
};

//globally (between all itogateways and unet) identifies instance of module, like HOST-PID identifies OS processes in cluster on machines
struct iot_process_ident_t {
	iot_hostid_t hostid;	//host where module instance is running
	uint32_t module_id;		//module_id of instance in miptr and/or miid.
	iot_miid_t miid;		//hostid-local ID of module instance (just opaque enumerator like pid for OS processes)
};

//globally (between all itogateways and unet) identifies some input/output/device_connection of instance of module
struct iot_process_inputident_t {
	iot_hostid_t hostid;	//host where module instance is running
	uint32_t module_id;		//module_id of instance in miptr and/or miid.
	iot_mi_inputid_t mi_inputid;		//hostid-local ID of module instance input/output/devcon
};

//driver-instance view of connection
struct iot_conn_drvview {
	iot_connid_t id;									//ID of this connection
	int index;											//index if this connection for driver instance
	iot_devifacetype devclass;					//specific device iface class among supported by driver instance
	iot_process_ident_t client;							//identification of consumer module instance
};


//represents consumer-side connection to specific driver instance (TODO remove it if same as iot_conn_drvview)
struct iot_conn_clientview {
	iot_connid_t id;									//ID of this connection
	int index;											//index if this driver connection for consumer instance
	iot_devifacetype devclass;					//specific device iface class among supported by driver instance
	iot_process_ident_t driver;							//universal identification of driver instance
};


//struct with operations map, supported by driver for specific device interface class
//all fields are optional. all functions are called in working thread of driver instance
//typedef struct {
//	int (*open)(iot_device_handle_t* handle); //Can be used to init module-specific data in handle
//	int (*close)(iot_device_handle_t* handle);//Can be used to deinit module-specific data in handle
//	int (*action)(iot_device_handle_t* handle, uint32_t action_code, uint32_t data_size, void* data);
//} iot_device_operations_t;


//additional specific interface for driver module instances
struct iot_device_driver_base : public iot_module_instance_base {
	iot_device_driver_base(uv_thread_t thread) : iot_module_instance_base(thread) {
	}
//called in instance thread:

//device handle operations map

	//Called when connection is being established.
	//Return values:
	//	0 - success
	//	IOT_ERROR_DEVICE_NOT_SUPPORTED - device is not supported. connection will be cancelled
	//	IOT_ERROR_LIMIT_REACHED - device is supported but module's internal limit on number of connections reached. connection will be cancelled
	//	IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked. instance will be destroyed
	//	IOT_ERROR_TEMPORARY_ERROR - call failed for temporary reason and will be retried
	//other errors treated as IOT_ERROR_CRITICAL_BUG
	virtual int device_open(const iot_conn_drvview* conn)=0;

	//Notifies driver when connection is closed. Connection cannot be read or written anymore.
	//Return values:
	//	0 - success
	//	IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked. instance will be destroyed
	//other errors treated as IOT_ERROR_CRITICAL_BUG
	virtual int device_close(const iot_conn_drvview* conn)=0;

	//Notifies when some event over connection has arrived. 
	//Return values:
	//	0 - success
	//	IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked. instance will be destroyed
	//other errors treated as IOT_ERROR_CRITICAL_BUG
	virtual int device_action(const iot_conn_drvview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data)=0;
};



//object of this class is used during driver instance creation to provide kernel with info about supported interface classes of device
struct iot_devifaces_list {
	iot_devifacetype items[IOT_CONFIG_MAX_CLASSES_PER_DEVICE];
	int num;

	iot_devifaces_list(void) : num(0) {}
	//return 0 on success, one of error code otherwise: IOT_ERROR_LIMIT_REACHED, IOT_ERROR_INVALID_ARGS
	int add(iot_devifacetype *cls) {
		if(num>=IOT_CONFIG_MAX_CLASSES_PER_DEVICE) return IOT_ERROR_LIMIT_REACHED;
		if(!cls || !cls->classid) return IOT_ERROR_INVALID_ARGS;
		items[num]=*cls;
		num++;
		return 0;
	}
};


struct iot_iface_device_driver_t {
	const char *descr; //text description of module driver functionality. If begins with '[', then must be evaluated as template
	uint32_t //num_devclassids:4,					//number of classes of data in state struct and number of items in stclassids list in this struct. can be 0 if driver can only be used directly in same bundle
			cpu_loading:2;						//average level of cpu loading of started driver instance. 0 - minimal loading (unlimited such tasks can work in same working thread), 3 - very high loading (this module requires separate working thread per instance)

	//Called to create instance of driver. Must check provided device and return status telling if device is supported. Always called in main thread
	//Return values:
	//0 - device is supported, instance was successfully created and saved to *instance.
	//IOT_ERROR_DEVICE_NOT_SUPPORTED - device is not supported, so next driver module should be tried
	//IOT_ERROR_INVALID_DEVICE_DATA - provided custom_len is invalid or custom_data has invalid structure. next driver can be tried
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be disabled. next driver should be tried
	//IOT_ERROR_TEMPORARY_ERROR - driver init should be retried after some time (with progressive interval). next driver should be tried immediately.
	//other errors treated as IOT_ERROR_CRITICAL_BUG
	int (*init_instance)(iot_device_driver_base**instance, uv_thread_t thread, iot_hwdev_data_t* dev_data, iot_devifaces_list* devifaces);

	//called to deinit instance.
	//Return values:
	//0 - success
	//any other error leaves instance in hang state
	int (*deinit_instance)(iot_module_instance_base*instance);		//always called in main thread


	//Can be called to check if driver can work with specific device. Can be called from any thread and is not connected with specific instance.
	//Return values:
	//0 - device is supported, instance can be created
	//IOT_ERROR_DEVICE_NOT_SUPPORTED - device is not supported
	//IOT_ERROR_INVALID_DEVICE_DATA - provided custom_len is invalid or custom_data has invalid structure
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked
	//IOT_ERROR_TEMPORARY_ERROR - check failed for temporary reason
	//other errors treated as IOT_ERROR_CRITICAL_BUG
	int (*check_device)(const iot_hwdev_data_t* dev_data);

//	iot_devifacetype_id_t* devclassids;			//pointer to array (with num_devclassids items) of class ids of device interfaces which this module provides

//	iot_device_operations_t* devops;			//pointer to array (with num_devclassids items) of map of supported operations for corresponding device class
};


///////////////////////////NODE INTERFACE

//additional specific interface for node module instances
struct iot_driver_client_base : public iot_module_instance_base {

	iot_driver_client_base(uv_thread_t thread) : iot_module_instance_base(thread) {
	}

//device handle operations map
	virtual int device_attached(const iot_conn_clientview* conn)=0;
	virtual int device_detached(const iot_conn_clientview* conn)=0;
	virtual int device_action(const iot_conn_clientview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data)=0;
};

struct iot_node_base : public iot_driver_client_base {
	iot_node_base(uv_thread_t thread) : iot_driver_client_base(thread) {
	}

//called in instance thread:
	//sets value for output.
	//Possible errors:
	int kapi_set_value_output(uint8_t index, iot_valueclass_BASE* value);

};


struct iot_deviceconn_filter_t { //represents general filter for selecting device driver for node device connections
	const char *label; //unique label to name device connection. device conn in iot configuration is matched by this label. maximum length is IOT_CONFIG_DEVLABEL_MAXLEN
	const char *descr; //text description of connection. If begins with '[', then must be evaluated as template
	uint8_t num_devclasses:4,					//number of allowed classes for current device (number of items in devclasses list in this struct). can be zero when item is unused (num_devices <= index of current struct) of when ANY classid is suitable.
		flag_canauto:1,						//flag that current device can be automatically selected by kernel according to classids. otherwise only user can select device
		flag_localonly:1;					//flag that current driver (and thus hwdevice) must run on same host as node instance
	const iot_devifacetype *devclasses;		//pointer to array (with num_classids items) of device interfaces this module can be connected to
};


struct iot_node_valuelinkcfg_t {
	const char *label; //unique label to name input/output ('err' is reserved for implicit error output). node link in iot configuration is matched by this label plus 'v' as beginning. maximum length is IOT_CONFIG_LINKLABEL_MAXLEN.
	const char *descr; //text description of link. If begins with '[', then must be evaluated as template
	const char *unit; //unit for values if appropriate. If begins with '[', then must be evaluated as template
	iot_valueclass_id_t vclass_id;

	bool is_compatible(iot_valueclass_id_t cls) const {
		return vclass_id==cls;
	}
	bool is_compatible(const iot_valueclass_BASE *val) const { //NULL means undef and is compatible with any value type
		return val==NULL || vclass_id==val->get_classid();
	}
	bool is_compatible(const iot_node_valuelinkcfg_t *op) const {
		return vclass_id==op->vclass_id;
	}
}; //describes type of value for corresponding labeled VALUE input/output

struct iot_node_msglinkcfg_t {
	const char *label; //unique label to name input (can overlap with labels of outputs but not with value inputs). node input in iot configuration is matched by this label. max length is IOT_CONFIG_LINKLABEL_MAXLEN
	const char *descr; //text description of link. If begins with '[', then must be evaluated as template
	uint8_t num_msgclasses; //number of items in msgclass_id[]. maximum is IOT_CONFIG_MAX_NODE_MSGLINKTYPES. value 255 means any message is accepted, msgclass_id can be NULL in such case
	const iot_msgclass_id_t *msgclass_id; //pointer to array (with num_msgclasses items) of message class ids this link can accept/transmit

	bool is_compatible(iot_msgclass_id_t cls) const { //checks if provided data class ID is compatible (is present in list)
		for(uint8_t i=0;i<num_msgclasses;i++) if(msgclass_id[i]==cls) return true;
		return false;
	}
	bool is_compatible(const iot_node_msglinkcfg_t* op) const { //checks if msgclass_id are compatible (there is at least one common class ID)
		for(uint8_t i=0;i<num_msgclasses;i++) if(op->is_compatible(msgclass_id[i])) return true;
		return false;
	}
};

struct iot_iface_node_t {
	const char *descr; //text description of module node functionality. If begins with '[', then must be evaluated as template
	const char *params_tmpl; //set of templates for showing/editing user params of node. can be NULL if module has no params
	uint32_t num_devices:2,						//number of devices this module should be connected to. limited by IOT_CONFIG_MAX_NODE_DEVICES
			num_valueoutputs:5,					//number of output value lines this module provides. limited by IOT_CONFIG_MAX_NODE_VALUEOUTPUTS. "error" output
												//is always present and not counted here
			num_valueinputs:5,					//number of input value lines this module accepts. limited by IOT_CONFIG_MAX_NODE_VALUEINPUTS
			num_msgoutputs:5,					//number of message output lines this module provides.. limited by IOT_CONFIG_MAX_NODE_MSGOUTPUTS.
			num_msginputs:5,					//number of message input lines this module accepts. limited by IOT_CONFIG_MAX_NODE_MSGINPUTS.
			cpu_loading:2,						//average level of cpu loading of started instance. 0 - minimal loading (unlimited number of such tasks can work
												//in same working thread), 3 - very high loading (this module requires separate working thread per instance)
			is_persistent:1,					//flag that node is persistent (event source or executor). otherwise (when 0) it is operator
			is_sync:1;							//flag that node can transform input signals into output synchronously and thus its instance will be started in
												//main thread. node must have both inputs and explicit outputs
	iot_deviceconn_filter_t devcfg[IOT_CONFIG_MAX_NODE_DEVICES];

	iot_node_valuelinkcfg_t valueoutput[IOT_CONFIG_MAX_NODE_VALUEOUTPUTS]; //describes type of value for corresponding labeled VALUE output.
																			//There is also one implicit error value output for every node, its label is 'err'
	iot_node_valuelinkcfg_t valueinput[IOT_CONFIG_MAX_NODE_VALUEINPUTS]; //describes type of value for corresponding labeled VALUE input
	iot_node_msglinkcfg_t msgoutput[IOT_CONFIG_MAX_NODE_MSGOUTPUTS]; //describes possible messages this node can output through each msg output
	iot_node_msglinkcfg_t msginput[IOT_CONFIG_MAX_NODE_MSGINPUTS]; //describes possible messages this node can process from each msg input

	
//	enum iot_value_type value_type;				//specifies type of 'value' field of node_state_t
//	size_t state_size;							//real size of state struct (sizeof(iot_srcstate_t) + custom_len)

	//reads current state of module into provided statebuf. size of statebuf must be at least state_size. Can be called from any thread. returns 0 on success or negative error code
	int (*init_instance)(iot_node_base**instance, uv_thread_t thread, uint32_t iot_id, json_object *json_cfg); //always called in main thread
//	int (*get_state)(void* instance, iot_srcstate_t* statebuf, size_t bufsize);

	//called to deinit instance.
	//Return values:
	//0 - success
	//any other error leaves instance in hang state
	int (*deinit_instance)(iot_module_instance_base*instance);		//always called in main thread

//	iot_state_classid* stclassids;				//pointer to array (with num_stclassids items) of class ids of state data which this module provides
};
/////////////////////////////




typedef struct {
	const char *title; //text title of module. If begins with '[', then must be evaluated as template. max length is 64 bytes, only latin1
	const char *descr; //text description of module. If begins with '[', then must be evaluated as template

	uint32_t module_id;								//system assigned unique module ID for consistency check
	uint32_t version;								//module version (0xHHLLREVI = HH.LL.REVI)
	uint8_t config_version;							//separate version of module configuration. must be increased when any configuration of module or any of its
													//interfaces is changed (e.g. node input/outputs/device lines changed)
	uint8_t num_devifaces;							//number of items in deviface_config array. can be 0
	uint8_t num_devcontypes;						//number of items in devcontype_config array. can be 0

	int (*init_module)(void);						//always called in main thread. once after loading (if loaded dynamically) or during startup (if compiled in statically)
	int (*deinit_module)(void);						//called in main thread before unloading dynamically loaded module

	const iot_devifacetype_iface **deviface_config;//optional array of module-defined device interface classes. can be NULL if num_devifaces==0
	const iot_hwdevident_iface **devcontype_config;	//optional array of module-defined device connection types. can be NULL if num_devcontypes==0

//Role interfaces
	const iot_iface_node_t *iface_node;
	const iot_iface_device_driver_t* iface_device_driver;
	const iot_iface_device_detector_t *iface_device_detector;
//TODO other roles
} iot_moduleconfig_t;


#endif //IOT_MODULE_H
