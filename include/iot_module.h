#ifndef IOT_MODULE_H
#define IOT_MODULE_H

#include<inttypes.h>
#include<time.h>

#include<assert.h>

#include "json.h"

#include "uv.h"
#include "iot_compat.h"
#include "iot_error.h"

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

#define EXPORTSYM __attribute__ ((visibility ("default")))

#define IOT_VERSION_COMPOSE(ver, patchlevel, revision) ((((ver)&0xFF)<<24)|(((patchlevel)&0xFF)<<16)|((revision)&0xFFFF))

#define IOT_CORE_VERSIONNUM 0
#define IOT_CORE_PATCHLEVEL 1
#define IOT_CORE_REVISION 10

#define IOT_CORE_ABI_VERSION 0
#define IOT_CORE_ABI_VERSION_STR ECB_STRINGIFY(IOT_CORE_ABI_VERSION)


#define IOT_CORE_VERSION IOT_VERSION_COMPOSE(IOT_CORE_VERSIONNUM, IOT_CORE_PATCHLEVEL, IOT_CORE_REVISION)
//#define IOT_ABI_TOKEN_NAME ECB_CONCAT(iot_abi_token ## _, ECB_CONCAT(IOT_CORE_VERSIONNUM, ECB_CONCAT(_, IOT_CORE_PATCHLEVEL)))

extern uint32_t IOT_ABI_TOKEN_NAME;

#ifndef DAEMON_CORE
////////////////////////////////////////////////////////////////////
///////////////////////////Specific declarations for external modules
////////////////////////////////////////////////////////////////////

#ifndef IOT_LIBVENDOR
#error IOT_LIBVENDOR must be defined before including iot_module.h
#endif

#ifndef IOT_LIBDIR
#error IOT_LIBDIR must be defined before including iot_module.h
#endif

#ifndef IOT_LIBNAME
#error IOT_LIBNAME must be defined before including iot_module.h
#endif

#define IOT_CURLIBRARY ECB_STRINGIFY(IOT_LIBVENDOR) "/" ECB_STRINGIFY(IOT_LIBDIR) "/" ECB_STRINGIFY(IOT_LIBNAME)

#define IOT_LIBSYMBOL_NAME(prefix, name) ECB_CONCAT(prefix, ECB_CONCAT(IOT_LIBVENDOR, ECB_CONCAT(__, ECB_CONCAT(IOT_LIBDIR, ECB_CONCAT(__, ECB_CONCAT(IOT_LIBNAME, ECB_CONCAT(_, name)))))))
#define IOT_LIBSYMBOL(name) ECB_CONCAT(name, ECB_CONCAT(IOT_LIBVENDOR, ECB_CONCAT(__, ECB_CONCAT(IOT_LIBDIR, ECB_CONCAT(__, IOT_LIBNAME)))))

#define IOT_DETECTOR_MODULE_CONF(modulename) EXPORTSYM IOT_LIBSYMBOL_NAME(ECB_CONCAT(ECB_CONCAT(iot_ ## abi, IOT_CORE_ABI_VERSION), _detector_modconf ## _), _ ## modulename)
#define IOT_DRIVER_MODULE_CONF(modulename) EXPORTSYM IOT_LIBSYMBOL_NAME(ECB_CONCAT(ECB_CONCAT(iot_ ## abi, IOT_CORE_ABI_VERSION), _driver_modconf ## _), _ ## modulename)
#define IOT_NODE_MODULE_CONF(modulename) EXPORTSYM IOT_LIBSYMBOL_NAME(ECB_CONCAT(ECB_CONCAT(iot_ ## abi, IOT_CORE_ABI_VERSION), _node_modconf ## _), _ ## modulename)

//builds unique identifier name for exporting moduleconfig_t object from module bundle
#define IOT_LIBVERSION_VAR IOT_LIBSYMBOL(iot_libversion ## _)

#define IOT_LIBVERSION_DEFINE uint32_t EXPORTSYM IOT_LIBVERSION_VAR=IOT_VERSION_COMPOSE(IOT_LIBVERSION, IOT_LIBPATCHLEVEL, IOT_LIBREVISION)



#define kapi_outlog_error(format... ) do_outlog(__FILE__, __LINE__, __func__, LERROR, format)
#define kapi_outlog_notice(format... ) do {if(LMIN<=LNOTICE && min_loglevel <= LNOTICE) do_outlog(__FILE__, __LINE__, __func__, LNOTICE, format);} while(0)
#define kapi_outlog_info(format... ) do {if(LMIN<=LINFO && min_loglevel <= LINFO) do_outlog(__FILE__, __LINE__, __func__, LINFO, format);} while(0)
#define kapi_outlog_debug(format... ) do {if(LMIN<=LDEBUG && min_loglevel <= LDEBUG) do_outlog(__FILE__, __LINE__, __func__, LDEBUG, format);} while(0)
#define kapi_outlog(level, format... ) do {if(LMIN<=level && min_loglevel <= level) do_outlog(__FILE__, __LINE__, __func__, level, format);} while(0)

#else

#define outlog_error(format... ) do_outlog(__FILE__, __LINE__, __func__, LERROR, format)

#if LMIN<=LNOTICE
#define outlog_notice(format... ) do {if(min_loglevel <= LNOTICE) do_outlog(__FILE__, __LINE__, __func__, LNOTICE, format);} while(0)
#else
#define outlog_notice(format... )
#endif

#if LMIN<=LINFO
#define outlog_info(format... ) do {if(min_loglevel <= LINFO) do_outlog(__FILE__, __LINE__, __func__, LINFO, format);} while(0)
#else
#define outlog_info(format... )
#endif

#if LMIN<=LDEBUG
#define outlog_debug(format... ) do {if(LMIN<=LDEBUG && min_loglevel <= LDEBUG) do_outlog(__FILE__, __LINE__, __func__, LDEBUG, format);} while(0)
#else
#define outlog_debug(format... )
#endif

#define outlog(level, format... )do { if(LMIN<=level && min_loglevel <= level) do_outlog(__FILE__, __LINE__, __func__, level, format);} while(0)


////////////////////////////////////////////////////////////////////
///////////////////////////Specific declarations for core
////////////////////////////////////////////////////////////////////

#define IOT_CURLIBRARY "CORE"

//gets module ID from value obtained by IOT_DEVIFACETYPE_CUSTOM macro
//#define IOT_DEVIFACETYPE_CUSTOM_MODULEID(clsid) ((clsid)>>8)

//gets module ID from value obtained by IOT_DEVCONTYPE_CUSTOM macro
//#define IOT_DEVCONTYPE_CUSTOM_MODULEID(contp) ((contp)>>8)

#endif //DAEMON_CORE

void do_outlog(const char*file, int line, const char* func, int level, const char *fmt, ...);

#include "iot_utils.h"

////////////////////////////////////////////////////////////////////
///////////////////////////Declarations for everybody
////////////////////////////////////////////////////////////////////



#define IOT_CONFIG_MAX_NODE_DEVICES 3				//max possible is 7
#define IOT_CONFIG_MAX_IFACES_PER_DEVICE 4         //max possible is 15
#define IOT_CONFIG_MAX_NODE_VALUEOUTPUTS 31			//max possible is 255
#define IOT_CONFIG_MAX_NODE_VALUEINPUTS 31			//max possible is 255
#define IOT_CONFIG_MAX_NODE_MSGOUTPUTS 8			//max possible is 255
#define IOT_CONFIG_MAX_NODE_MSGINPUTS 8
#define IOT_CONFIG_MAX_NODE_MSGLINKTYPES 8			//max possible is 254
#define IOT_CONFIG_DEVLABEL_MAXLEN 7
#define IOT_CONFIG_LINKLABEL_MAXLEN 6


#define IOT_CONFIG_NODE_ERROUT_LABEL "err"			//label for implicit error output of node

#define IOT_LIBNAME_MAXLEN 64
#define IOT_MODULENAME_MAXLEN 60
#define IOT_TYPENAME_MAXLEN 32

#define IOT_MEMOBJECT_MAXPLAINSIZE 16384

#define IOT_MEMOBJECT_MAXREF 65536

extern const uint32_t iot_core_version;
extern uv_thread_t main_thread;
extern uint64_t iot_starttime_ms; //start time of process like returned by uv_now (in ms since unknown point)



typedef uint64_t iot_hostid_t;					//type for Host ID
//printf macro for outputting host id
#define IOT_PRIhostid PRIu64
#define IOT_HOSTID_ANY 0xffffffffffffffffull	//value meaning 'any host' in templates of hwdevice ident
#define iot_hostid_t_MAX UINT64_MAX
#define repack_hostid repack_uint64

typedef uint32_t iot_id_t;						//type for IOT ID
#define IOT_PRIiotid PRIu32
#define iot_id_t_MAX UINT32_MAX
#define repack_iot_id repack_uint32

typedef uint32_t iotlink_id_t;					//type for LINK ID between nodes
#define IOT_PRIiotlinkid PRIu32
#define iotlink_id_t_MAX UINT32_MAX
#define repack_iotlink_id repack_uint32

typedef uint32_t iot_type_id_t;					//type for type (or class) ID for customizable types such as: device connection type, device iface type,
												//data type (common ID space for value and messages), value notion type
#define IOT_PRIiottypeid PRIu32
#define iot_type_id_t_MAX UINT32_MAX
#define repack_type_id repack_uint32

#define uint32_t_MAX UINT32_MAX
#define uint16_t_MAX UINT16_MAX
#define uint8_t_MAX UINT8_MAX


enum h_state_t : uint8_t { //libuv handle state
	HS_UNINIT=0,
	HS_CLOSING,
	HS_INIT,
	HS_ACTIVE
}; //activation way: UNINIT -> INIT -> ACTIVE  , stop: ACTIVE -> CLOSING -> UNINIT   , reactivation: (stop) -> (activate)


extern iot_hostid_t iot_current_hostid; //ID of current host in user config

typedef uint32_t iot_iid_t;					//type for running Instance ID
typedef uint32_t iot_connsid_t;				//type for Connection Struct ID


typedef iot_type_id_t iot_datatype_id_t;		//type for class ID of value or message for node input/output
typedef iot_datatype_id_t iot_msgtype_id_t;	//type for message type ID. MUST HAVE 1 in lower bit
typedef iot_type_id_t iot_valuenotion_id_t;			//type for data value notion ID of value for node input/output


//uniquely identifies event in whole cluster
struct iot_event_id_t {
	uint64_t numerator; //from bottom is limited by current timestamp with microseconds - 1e15 (offset timestamp in seconds for 1`000`000`000) multiplied by 1000
	iot_hostid_t host_id;

	bool operator!(void) const {
		return numerator==0;
	}
	explicit operator bool(void) const {
		return numerator!=0;
	}
	bool operator==(const iot_event_id_t &op) {
		return numerator==op.numerator && host_id==op.host_id;
	}
	uint64_t get_reltime_ms(void) {
		return ((numerator/1000)+500)/1000;
	}
};

//base class for object ids - a way to make reference to 
struct iot_objid_t {
	enum type_t : uint8_t {
		OBJTYPE_NONE=0, //used to init objid with illegal type
		OBJTYPE_PEERCON=1, //iot_netcon-derived object used for peer connection, stored at 
	};
	uint32_t idx:24; //index of object in corresponding array of objects, idx==0 is valid
	uint32_t type:8; //id of object type from type_t
	uint32_t key; //unique serial number to count how many times particular idx was used. key==0 marks INVALID if

	iot_objid_t(type_t type) : idx(0), type(type), key(0) {
	}
	iot_objid_t(uint8_t type, uint32_t idx_, uint32_t key) : type(type), key(key) {
		assert(idx_ < 1<<24);
		idx=idx_;
	}
	iot_objid_t(const iot_objid_t &src) {
		idx=src.idx;
		type=src.type;
		key=src.key;
	}
	bool operator==(const iot_objid_t &p) const {
		return idx==p.idx && type==p.type && key==p.key;
	}
	bool operator!=(const iot_objid_t &p) const {
		return !(*this==p);
	}
	bool operator!(void) const {
		return key==0;
	}
	explicit operator bool(void) const {
		return key!=0;
	}
//	void clear(void) {
//		created=0;
//		iid=0;
//	}
};



//action code constants for different kapi functions
enum iot_action_t : uint8_t {
	IOT_ACTION_ADD=1,
	IOT_ACTION_REMOVE=2,
	IOT_ACTION_TRYREMOVE=3,
//	IOT_ACTION_REPLACE=3
};

//type which identifies running module instance
struct iot_miid_t {
	uint32_t created; //modified timestamp of creation (iid can overlap)
	iot_iid_t iid;

	iot_miid_t(void) {
		created=0;
		iid=0;
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

struct iot_modinstance_item_t;

struct iot_module_instance_base;
struct iot_device_driver_base;
struct iot_driver_client_base;
struct iot_conn_clientview;
struct iot_conn_drvview;

#include "iot_hwdevcontype.h"
#include "iot_deviface.h"

//struct with connection-type specific data about hardware device
//struct iot_hwdev_fulldata_t {
//	iot_hwdev_ident ident; //identification of device (and its connection type)
//	iot_hwdev_details *data; //custom contype-specific data about device (its capabilities, name etc)
//};




//allocate memory block
void *iot_allocate_memblock(uint32_t size, bool allow_direct=false);
//release memory block allocated by iot_allocate_memblock
void iot_release_memblock(void *memblock);
//increments reference count of memory block allocated by iot_allocate_memblock. returns false if reference count overfilled
bool iot_incref_memblock(void *memblock);







//Used by device detector modules for managing registry of available hardware devices
//action is one of {IOT_ACTION_ADD, IOT_ACTION_REMOVE,IOT_ACTION_REPLACE}
//ident.contype must be among those listed in .devcontypes field of detector module interface
//returns:




#include "iot_valuenotion.h"
#include "iot_valueclasses.h"


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
struct iot_module_instance_base : public iot_objectrefable {
	uv_thread_t thread=0; //working thread of this instance after start
	iot_miid_t miid={};    //modinstance id of this instance after start. becomes false after instance is stopped
	uv_loop_t *loop=NULL;  //correct loop for STARTED instance

//called in instance thread:

	//sends request to core to stop current instance with specific error code. Also is used to notify core when instance is ready to stop if stop was delayed by
	//returning IOT_ERROR_TRY_AGAIN from stop(). Possible error codes:
	//0 - no error. can be used after delayed stop() only
	//IOT_ERROR_CRITICAL_BUG - blocks module. so no new instances of current instance type can be created
	//IOT_ERROR_CRITICAL_ERROR - say core about incorrect instanciation (i.e. block driver creation using current module for current hw device) or configuration (iot item config is invalid).
	//							no instanciation will be attempted until configuration or module version update
	//IOT_ERROR_TEMPORARY_ERROR - just request to recreate instance after some time due to some temporary error.
	int kapi_self_abort(int errcode);

	//Returns full path to library(bundle)-specific run-directory. Module instances can use it to store runtime data. Module isolation must be done by module code if necessary (e.g. by adding more subdirs)
	//'create' shows if directory must be created if doesn't exists already. Creation does not occur if return value>buffer_size
	//Returns
	//positive number of bytes written to buffer. If this value is less than buffer_size, then buffer was not enough. Path will be NUL-terminated anyway
	//negative error code:
	//IOT_ERROR_INVALID_ARGS - invalid buffer pointer or buffer_size is less than 2
	//IOT_ERROR_CRITICAL_ERROR - filesystem-level error. errno is actual
	//IOT_ERROR_CRITICAL_BUG
	int kapi_lib_rundir(char *buffer, size_t buffer_size, bool create);

	//Returns full path to library(bundle)-specific shared data directory (read-only data which were provided by lib vendor).
	//libname defaults to library of module of this instance. format of libname is "vendor/libdir/name"
	//Returns
	//positive number of bytes written to buffer. If this value is less than buffer_size, then buffer was not enough. Path will be NUL-terminated anyway
	//negative error code:
	//IOT_ERROR_INVALID_ARGS - invalid buffer pointer or buffer_size is less than 2
	//IOT_ERROR_CRITICAL_ERROR - filesystem-level error. errno is actual
	//IOT_ERROR_NOT_FOUND - libname was non-NULL and no library found with that full name
	//IOT_ERROR_CRITICAL_BUG
	int kapi_lib_datadir(char *buffer, size_t buffer_size, const char* libname=NULL);


	//Called to start work of previously inited instance.
	//Accepted return values:
	//0 - instance successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, whole system for detector, node_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(void)=0;

	//called to stop work of started instance. call is always followed by instance_deinit
	//Return values:
	//0 - instance successfully stopped and can be deinited
	//IOT_ERROR_TRY_AGAIN - instance requires some time (async operation) to stop gracefully. kapi_self_abort() will be called to notify core when stop is finished.
	//						anyway second stop() call must free all resources correctly, may be in a hard way. otherwise module will be blocked and left in hung state (deinit
	//						cannot be called until stop reports OK)
	//any other error is treated as critical bug and driver is blocked for further starts. deinit won't be called for such instance. instance is put into hang state
	virtual int stop(void)=0;

	

	static void kapi_process_uv_close(uv_handle_t *handle);

//	~iot_module_instance_base(void) {}
protected:
	iot_module_instance_base(object_destroysub_t destroy_sub=object_destroysub_delete, bool is_dynamic=true) : iot_objectrefable(destroy_sub, is_dynamic) {
	}

private:
	friend struct iot_modinstance_item_t;
//	iot_modinstance_item_t *modinst=NULL;
	void kapi_internal_init(const iot_miid_t& miid_, const uv_thread_t& thread_, uv_loop_t* loop_) { //will be called by core just before start
		miid=miid_;
		thread=thread_;
		loop=loop_;
	}
};

////////////////////////////////////////////////////
///////////////////////////DEVICE DETECTOR INTERFACE
////////////////////////////////////////////////////
//additional specific interface for driver module instances
struct iot_device_detector_base : public iot_module_instance_base {
//interface to core:
	int kapi_hwdev_registry_action(enum iot_action_t action, const iot_hwdev_localident* ident, iot_hwdev_details* custom_data);

	//notification that user params for detector/driver was updated so they could be applied without restarting detector instance and re-attaching slave devices
	//Return values:
	//	0 - success
	//	IOT_ERROR_CRITICAL_ERROR - provided params are invalid, detector will be stopped. new start can be attempted if new params arrive
	//	IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked. instance will be destroyed
	//	IOT_ERROR_TEMPORARY_ERROR - call failed for temporary reason and will be retried several times
	//	IOT_ERROR_TRY_AGAIN - instance must be restarted to apply new params
	//other errors treated as IOT_ERROR_CRITICAL_BUG
	virtual int update_params(json_object *json_params) {
		return IOT_ERROR_TRY_AGAIN;
	}


	//notification that manual device specs for this detector were changed by user so they could be applied without restarting detector instance and re-attaching slave devices
	//Return values:
	//	0 - success
	//	IOT_ERROR_CRITICAL_ERROR - provided params are invalid, detector will be stopped. new start can be attempted if new params arrive
	//	IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked. instance will be destroyed
	//	IOT_ERROR_TEMPORARY_ERROR - call failed for temporary reason and will be retried several times
	//	IOT_ERROR_TRY_AGAIN - instance must be restarted to apply new params
	//other errors treated as IOT_ERROR_CRITICAL_BUG
	virtual int update_manual_devices(json_object *manual_devices) {
		return IOT_ERROR_TRY_AGAIN;
	}

protected:
	iot_device_detector_base(object_destroysub_t destroy_sub=object_destroysub_delete, bool is_dynamic=true) : iot_module_instance_base(destroy_sub, is_dynamic) {
	}

};



//struct iot_iface_device_detector_t {
//};





////////////////////////////////////////////////////
///////////////////////////DEVICE DRIVER INTERFACE
////////////////////////////////////////////////////

enum iot_devconn_action_t : uint8_t {
	IOT_DEVCONN_ACTION_FULLREQUEST, //new message (in full) came from another side of connection. data_size contains full size of message.
								//data contains address of temporary buffer with message body. message must be interpreted by corresponding device interface class with corresponding attributes
	IOT_DEVCONN_ACTION_CANWRITE,//write_avail_notify(true) was called and there appeared free space in send buffer, so new write attempt can be made
	IOT_DEVCONN_ACTION_CANREADNEW,
	IOT_DEVCONN_ACTION_CANREADCONT
//	IOT_DEVCONN_ACTION_READY	//sent to DRIVER side when connection is fully inited and messages can be sent over it (in 'open' handler connection is not ready still)
};

//globally (between all itogateways and unet) identifies instance of module, like HOST-PID identifies OS processes in cluster on machines
struct iot_process_ident_t {
	iot_hostid_t hostid;	//host where module instance is running
	uint32_t module_id;		//module_id of instance in miid.
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
	const iot_deviface_params* deviface;				//specific device iface class among supported by driver instance
	iot_process_ident_t client;							//identification of consumer module instance
};


//represents consumer-side connection to specific driver instance (TODO remove it if same as iot_conn_drvview)
struct iot_conn_clientview {
	iot_connid_t id;									//ID of this connection
	int index;											//index if this driver connection for consumer instance
	const iot_deviface_params* deviface;					//specific device iface class among supported by driver instance
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
struct iot_device_driver_base : public iot_device_detector_base {
//called in instance thread:

	//enables or disables notifications about free space in send buffer
	int kapi_notify_write_avail(const iot_conn_drvview* conn, bool enable);

//device handle operations map

	//Called when connection is being established.
	//Return values:
	//	0 - success
	//	IOT_ERROR_NOT_SUPPORTED - device is not supported. connection will be cancelled
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

protected:
	iot_device_driver_base(object_destroysub_t destroy_sub=object_destroysub_delete, bool is_dynamic=true) : iot_device_detector_base(destroy_sub, is_dynamic) {
	}
};



//object of this class is used during driver instance creation to provide core with info about supported interface classes of device
struct iot_devifaces_list {
	iot_deviface_params_buffered items[IOT_CONFIG_MAX_IFACES_PER_DEVICE];
	unsigned num;

	iot_devifaces_list(void) : num(0) {}
	//return 0 on success, one of error code otherwise: IOT_ERROR_LIMIT_REACHED, IOT_ERROR_INVALID_ARGS
	int add(const iot_deviface_params *cls);
};


//struct iot_iface_device_driver_t {
//};


///////////////////////////NODE INTERFACE

//additional specific interface for node module instances
struct iot_driver_client_base : public iot_module_instance_base {

	//enables or disables notifications about free space in send buffer
	int kapi_notify_write_avail(const iot_conn_clientview* conn, bool enable);


//device handle operations map
	virtual int device_attached(const iot_conn_clientview* conn) {return 0;}
	virtual int device_detached(const iot_conn_clientview* conn) {return 0;}
	virtual int device_action(const iot_conn_clientview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) {return 0;}
};

struct iot_node_base : public iot_driver_client_base {
	struct iot_value_signal {
		const iot_datavalue* new_value, *prev_value;
	};

	struct iot_msg_signal {
		const iot_datavalue** msgs;
		uint16_t num;
	};

//called in instance thread:

	//Updates specific value outputs and sends messages from specific msg outputs. Both zeros in num_values and num_msgs is valid when 'no update' should answered by sync nodes in regular sync mode of execution.
	//	IOT_ERROR_INVALID_ARGS - provided index or type of value are illegal.
	//	IOT_ERROR_NO_MEMORY - no memory to process value change. try later.
	int kapi_update_outputs(const iot_event_id_t *reason_eventid, uint8_t num_values, const uint8_t *valueout_indexes, const iot_datavalue** values, uint8_t num_msgs=0, const uint8_t *msgout_indexes=NULL, const iot_datavalue** msgs=NULL);

	//Returns last output value which was set by last call to kapi_update_outputs()
	//Returned NULL value can mean either undefined value or illegal index
	const iot_datavalue* kapi_get_outputvalue(uint8_t index);


	//During processing of this event one or several calls to kapi_update_outputs can be made. For async nodes (is_sync is 0 in module config) this is irrelevant, but
	//sync nodes loose their simple sync mode of execution if kapi_update_outputs() was called more than ones or for another eventid or if return value was not 0. 
	//
	//Allowed return values:
	//0 - success. For sync node also means that no outputs were changed if no kapi_update_outputs() call was made (it could also be called with correct eventid and all rest zeros to show absence of update).
	//IOT_ERROR_NOT_READY - for async nodes just means success (same as 0 return value). For sync node, which HAD NOT DONE CALL to kapi_update_outputs() with
	//						passed eventid, means that kapi_update_outputs() WILL BE CALLED LATER (processing of event is stopped until such call is made). For
	//						nodes in simple sync mode such error also disables simple mode (node is turned into regular sync node) even if kapi_update_outputs()
	//						was called with passed eventid.
	//other errors equivalent to IOT_ERROR_NOT_READY. TODO: may be treat other errors as critical
	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) {
		return 0; //by default say that outputs are not changed
	}

};


struct iot_deviceconn_filter_t { //represents general filter for selecting device driver for node device connections
	const char *label; //unique label to name device connection. device conn in iot configuration is matched by this label. maximum length is IOT_CONFIG_DEVLABEL_MAXLEN
//	const char *descr; //text description of connection. If begins with '[', then must be evaluated as template
	uint8_t num_devifaces:4,					//number of allowed classes for current device (number of items in devifaces list in this struct). can be zero when item is unused (num_devices <= index of current struct) of when ANY classid is suitable.
		flag_canauto:1,						//flag that current device can be automatically selected by core according to classids. otherwise only user can select device
		flag_localonly:1;					//flag that current driver (and thus hwdevice) must run on same host as node instance
	const iot_deviface_params **devifaces;		//pointer to array (with num_devifaces items) of device interface params this module can be connected to
};


struct iot_node_valuelinkcfg_t {
	const char *label; //unique label to name input/output ('err' is reserved for implicit error output). node link in iot configuration is matched by this label plus 'v' as beginning. maximum length is IOT_CONFIG_LINKLABEL_MAXLEN.
	const iot_valuenotion* notion;//unit/notion for values if not NULL
	const iot_datatype_metaclass* dataclass;

	bool is_compatible(const iot_datatype_metaclass* cls) const {
		if(!cls) return false;
		return dataclass==cls;
	}
	bool is_compatible(const iot_datavalue *val) const { //NULL means undef and is compatible with any value type
		return val==NULL || is_compatible(val->get_metaclass());
	}
	bool is_compatible(const iot_node_valuelinkcfg_t *op) const {
		return dataclass==op->dataclass;
	}
}; //describes type of value for corresponding labeled VALUE input/output

struct iot_node_msglinkcfg_t {
	const char *label; //unique label to name input (can overlap with labels of outputs but not with value inputs). node input in iot configuration is matched by this label. max length is IOT_CONFIG_LINKLABEL_MAXLEN
	uint8_t num_dataclasses; //number of items dataclasses[]. maximum is IOT_CONFIG_MAX_NODE_MSGLINKTYPES. value 0 (allowed for input msg link only) means any message is accepted, dataclasses is ignored in such case
	const iot_datatype_metaclass* *dataclasses; //pointer to array (with num_dataclasses items) of message type meta classes this link can accept/transmit

	bool is_compatible(const iot_datatype_metaclass* cls) const {
		if(!cls) return false;
		if(num_dataclasses==0) return true;
		for(uint8_t i=0;i<num_dataclasses;i++) if(dataclasses[i]==cls) return true;
		return false;
	}
	bool is_compatible(const iot_datavalue *val) const {
		return val!=NULL && is_compatible(val->get_metaclass());
	}
	bool is_compatible(const iot_node_msglinkcfg_t* op) const { //checks if msgtype_id are compatible (there is at least one common class ID)
		if(num_dataclasses==0) return op->num_dataclasses>0 ? true : false;
		for(uint8_t i=0;i<num_dataclasses;i++) if(op->is_compatible(dataclasses[i])) return true;
		return false;
	}
};

//struct iot_iface_node_t {
//};
/////////////////////////////



struct iot_node_moduleconfig_t {
	uint32_t version;								//module version (0xHHLLREVI = HH.LL:REVI), use macro IOT_VERSION_COMPOSE(version, patchlevel, revision)


	uint8_t cpu_loading,						//average level of cpu loading of started instance. 0 - minimal loading (unlimited number of such tasks can work
			num_devices,						//number of devices this module should be connected to. limited by IOT_CONFIG_MAX_NODE_DEVICES
			num_valueoutputs,					//number of output value lines this module provides. limited by IOT_CONFIG_MAX_NODE_VALUEOUTPUTS. "error" output
												//is always present and not counted here
			num_valueinputs,					//number of input value lines this module accepts. limited by IOT_CONFIG_MAX_NODE_VALUEINPUTS
			num_msgoutputs,						//number of message output lines this module provides.. limited by IOT_CONFIG_MAX_NODE_MSGOUTPUTS.
			num_msginputs;						//number of message input lines this module accepts. limited by IOT_CONFIG_MAX_NODE_MSGINPUTS.
												//in same working thread), 3 - very high loading (this module requires separate working thread per instance)
	uint8_t is_persistent,						//flag that node is persistent (event source or executor). otherwise (when 0) it is operator
			is_sync;							//flag that node (with at least one explicit output) can and promises to transform input signals into output explicitly
												//and unambiguously. i.e. after getting notification about input signals update such node must either give corresponding
												//output signals immediately (or say 'no change') or give promise to answer later. such nodes can generate output
												//signals unrelated to inputs change BUT they must be ready for loosing intermediate signals (i.e. in series of
												//some value output values or msg output messages only latest will be noticed and processed) when node is blocked
												//during some related rule processing.
												//Additionally nodes with this flag and zero cpu_loading start in 'simple synchronous mode' if they are started 
												//in modelling (now it is main) thread. In simple mode instance gets input change notification directly during event processing
												//bypassing thread message queue and gives outputs directly to event processing routine. This greatly speeds up
												//processing. But simple mode is disabled when instance gives delayed answer or generates unrelated signal for the
												//first time
	iot_deviceconn_filter_t devcfg[IOT_CONFIG_MAX_NODE_DEVICES];

	iot_node_valuelinkcfg_t valueoutput[IOT_CONFIG_MAX_NODE_VALUEOUTPUTS]; //describes type of value for corresponding labeled VALUE output.
																			//There is also one implicit error value output for every node, its label is 'err'
	iot_node_valuelinkcfg_t valueinput[IOT_CONFIG_MAX_NODE_VALUEINPUTS]; //describes type of value for corresponding labeled VALUE input
	iot_node_msglinkcfg_t msgoutput[IOT_CONFIG_MAX_NODE_MSGOUTPUTS]; //describes possible messages this node can output through each msg output
	iot_node_msglinkcfg_t msginput[IOT_CONFIG_MAX_NODE_MSGINPUTS]; //describes possible messages this node can process from each msg input

	
//	enum iot_value_type value_type;				//specifies type of 'value' field of node_state_t
//	size_t state_size;							//real size of state struct (sizeof(iot_srcstate_t) + custom_len)

	int (*init_module)(void);						//always called in main thread. once after loading (if loaded dynamically) or during startup (if compiled in statically)
	int (*deinit_module)(void);						//called in main thread before unloading dynamically loaded module

	//reads current state of module into provided statebuf. size of statebuf must be at least state_size. Can be called from any thread. returns 0 on success or negative error code
//	int (*get_state)(void* instance, iot_srcstate_t* statebuf, size_t bufsize);
	int (*init_instance)(iot_node_base** instance, uint32_t iot_id, json_object *json_cfg); //always called in main thread

	//called to deinit instance.
	//Return values:
	//0 - success
	//any other error leaves instance in hang state
	int (*deinit_instance)(iot_node_base* instance);		//always called in main thread

//	iot_state_classid* stclassids;				//pointer to array (with num_stclassids items) of class ids of state data which this module provides
};

struct iot_driver_moduleconfig_t {
	uint32_t version;								//module version (0xHHLLREVI = HH.LL:REVI), use macro IOT_VERSION_COMPOSE(version, patchlevel, revision)

	uint8_t cpu_loading;							//average level of cpu loading of started driver instance. 0 - minimal loading (unlimited such tasks can work in same working thread), 3 - very high loading (this module requires separate working thread per instance)
	uint8_t num_hwdev_idents;						//number of items in hwdev_idents array in this struct. can be zero if hwdevices with any contype must be tried
	uint8_t num_dev_ifaces;							//number of items in dev_ifaces array in this struct. should not be zero for non-detecting drivers as this disables ability to find driver matching requirements of some node
	uint8_t is_detector;							//this driver can detect hardware devices. actual if described driver is for some device hub (e.g. z-wave)

	const iot_hwdev_localident** hwdev_idents;		//pointer to array (with num_hwdev_idents items) of device idents this module can probe. i.e. module knows how to check hwdevice identity for such contypes or can filter by vendor etc.
	const iot_devifacetype_metaclass** dev_ifaces;	//pointer to array (with num_dev_ifaces items) of device iface metaclasses this driver can provide. used for manifests only

	int (*init_module)(void);						//always called in main thread. once after loading (if loaded dynamically) or during startup (if compiled in statically)
	int (*deinit_module)(void);						//called in main thread before unloading dynamically loaded module


	//Called to create instance of driver. Must check provided device and return status telling if device is supported. Always called in main thread
	//dev_ident and dev_data are given by some detector. json_params can be provided by user. it is passed if dev_ident matches some record in ownconfig.hwdevices.DEVICEIDENTCRC.driver_params.DRVMODULE_ID
	//Return values:
	//0 - device is supported, instance was successfully created and saved to *instance, deviface was fillec with supported interfaces
	//IOT_ERROR_NOT_SUPPORTED - device is not supported, so next driver module should be tried
	//IOT_ERROR_INVALID_DEVICE_DATA - provided custom_len is invalid or custom_data has invalid structure. next driver can be tried
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be disabled. next driver should be tried
	//IOT_ERROR_TEMPORARY_ERROR - driver init should be retried after some time (with progressive interval). next driver should be tried immediately.
	//other errors treated as IOT_ERROR_CRITICAL_BUG
	int (*init_instance)(iot_device_driver_base**instance, const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data, iot_devifaces_list* devifaces, json_object *json_params);

	//called to deinit instance.
	//Return values:
	//0 - success
	//any other error leaves instance in hang state
	int (*deinit_instance)(iot_device_driver_base* instance);		//always called in main thread


	//Can be called to check if driver can work with specific device. Can be called from any thread and is not connected with specific instance.
	//Return values:
	//0 - device is supported, instance can be created
	//IOT_ERROR_NOT_SUPPORTED - device is not supported
	//IOT_ERROR_INVALID_DEVICE_DATA - provided custom_len is invalid or custom_data has invalid structure
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked
	//IOT_ERROR_TEMPORARY_ERROR - check failed for temporary reason
	//other errors treated as IOT_ERROR_CRITICAL_BUG
	int (*check_device)(const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data);

//	iot_device_operations_t* devops;			//pointer to array (with num_devclassids items) of map of supported operations for corresponding device class
};

struct iot_detector_moduleconfig_t {
	uint32_t version;								//module version (0xHHLLREVI = HH.LL:REVI), use macro IOT_VERSION_COMPOSE(version, patchlevel, revision)

	uint8_t cpu_loading;						//average level of cpu loading of started detector. 0 - minimal loading (unlimited such tasks can work in same working thread), 3 - very high loading (this module requires separate working thread for detector)
//	uint32_t //num_hwdevcontypes:2,				//number of items in hwdevcontypes array in this struct. 
	uint8_t manual_devices_required;			//flag that this module requires non-empty manual params (so cannot be used for auto detection of devices)

	int (*init_module)(void);						//always called in main thread. once after loading (if loaded dynamically) or during startup (if compiled in statically)
	int (*deinit_module)(void);						//called in main thread before unloading dynamically loaded module

	//Called to create single instance of detector. Must check provided device and return status telling if device is supported. Always called in main thread
	//Return values:
	//0 - device is supported, instance was successfully created and saved to *instance.
	//IOT_ERROR_NOT_SUPPORTED - device is not supported, so next driver module should be tried
	//IOT_ERROR_INVALID_DEVICE_DATA - provided custom_len is invalid or custom_data has invalid structure. next driver can be tried
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be disabled. next driver should be tried
	//IOT_ERROR_TEMPORARY_ERROR - driver init should be retried after some time (with progressive interval). next driver should be tried immediately.
	//other errors treated as IOT_ERROR_CRITICAL_BUG
	int (*init_instance)(iot_device_detector_base** instance, json_object *json_params, json_object *manual_devices);

	//called to deinit single instance.
	//Return values:
	//0 - success
	//any other error leaves instance in hang state
	int (*deinit_instance)(iot_device_detector_base* instance);		//always called in main thread


	//Can be called to check if detector can work on current system. Can be called from any thread and is not connected with specific instance.
	//Return values:
	//0 - system is supported, instance can be created
	//IOT_ERROR_NOT_SUPPORTED - device is not supported
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked
	//IOT_ERROR_TEMPORARY_ERROR - check failed for temporary reason
	//other errors treated as IOT_ERROR_CRITICAL_BUG
	int (*check_system)(void);

//	const iot_type_id_t* hwdevcontypes;			//pointer to array (with num_devcontypes items) of device connection types this module can manipulate
};


#endif //IOT_MODULE_H
