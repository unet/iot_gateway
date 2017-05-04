#ifndef IOT_MODULE_H
#define IOT_MODULE_H

#include<stdint.h>
#include<time.h>

#include<assert.h>

#include<uv.h>

#include<iot_utils.h>
#include<iot_compat.h>
#include<iot_error.h>

//Log levels
#define LDEBUG  0
#define LINFO   1
#define LNOTICE 2
#define LERROR  3

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

//macro for generating ID of custom module specific hw device connection type
#define IOT_DEVCONTYPE_CUSTOM(module_id, index) (((module_id)<<8)+(index))

//macro for generating ID of custom module specific device interface class
#define IOT_DEVIFACECLASS_CUSTOM(module_id, index) (((module_id)<<8)+(index))

//macro for generating ID of custom module specific class of state data
#define IOT_VALUECLASS_CUSTOM(module_id, index) (((module_id)<<8)+(index))


#define kapi_outlog_error(format... ) do_outlog(__FILE__, __LINE__, __func__, LERROR, format)
#define kapi_outlog_notice(format... ) if(min_loglevel <= 2) do_outlog(__FILE__, __LINE__, __func__, LNOTICE, format)
#define kapi_outlog_info(format... ) if(min_loglevel <= 1) do_outlog(__FILE__, __LINE__, __func__, LINFO, format)
#define kapi_outlog_debug(format... ) if(min_loglevel == 0) do_outlog(__FILE__, __LINE__, __func__, LDEBUG, format)
#define kapi_outlog(level, format... ) if(min_loglevel <= level) do_outlog(__FILE__, __LINE__, __func__, level, format)

#else

////////////////////////////////////////////////////////////////////
///////////////////////////Specific declarations for kernel
////////////////////////////////////////////////////////////////////

//gets module ID from value obtained by IOT_DEVIFACECLASS_CUSTOM macro
#define IOT_DEVIFACECLASS_CUSTOM_MODULEID(clsid) ((clsid)>>8)

//gets module ID from value obtained by IOT_DEVCONTYPE_CUSTOM macro
#define IOT_DEVCONTYPE_CUSTOM_MODULEID(contp) ((contp)>>8)

#endif //DAEMON_KERNEL


////////////////////////////////////////////////////////////////////
///////////////////////////Declarations for everybody
////////////////////////////////////////////////////////////////////



#define IOT_CONFIG_MAX_EVENTSOURCE_DEVICES 2
#define IOT_CONFIG_MAX_CLASSES_PER_DEVICE 4          //max number if 15
#define IOT_CONFIG_MAX_EVENTSOURCE_STATES 4

#define IOT_MEMOBJECT_MAXPLAINSIZE 8192

#define IOT_MEMOBJECT_MAXREF 15

extern uv_thread_t main_thread;
extern uint64_t iot_starttime_ms; //start time of process like returned by uv_now (in ms since unknown point)

void do_outlog(const char*file, int line, const char* func, int level, const char *fmt, ...);


typedef uint32_t iot_hostid_t;				//type for Host ID
#define IOT_HOSTID_ANY 0xffffffffu 			//value meaning 'any host' in templates of hwdevice ident


extern iot_hostid_t iot_current_hostid; //ID of current host in user config

typedef uint16_t iot_iid_t;					//type for running Instance ID
typedef uint16_t iot_connsid_t;				//type for connection struct ID

typedef uint32_t iot_devifaceclass_id_t;	//type for device interface class ID (abstraction over hardware devices provided by device drivers)
typedef uint32_t iot_valueclass_id_t;		//type for event-source state class ID

typedef uint32_t iot_state_error_t;

typedef uint32_t iot_hwdevcontype_t;		//type for HardWare DEVice CONection TYPE id
typedef uint64_t iot_hwdevhwid_t;			//type for unique identificator if HW Device among its connection type




//action code constants for different kapi functions
enum iot_action_t : uint8_t {
	IOT_ACTION_ADD=1,
	IOT_ACTION_REMOVE=2,
	IOT_ACTION_REPLACE=3
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

//make iot_hwdevident_iface be 128 bytes
#define IOT_HWDEV_DATA_MAXSIZE	(128 - sizeof(iot_hwdevcontype_t) - sizeof(uint32_t) - 1 - sizeof(iot_hostid_t))
//struct which locally identifies hardware device depending on connection type. interface to this data is of class iot_hwdevident_iface.
struct iot_hwdevident_iface;

struct iot_hwdev_localident_t {
	iot_hwdevcontype_t contype;			//type of connection identification among predefined IOT_DEVCONTYPE_ constants or DeviceDetector
										//module-specific built by IOT_DEVCONTYPE_CUSTOM macro
	uint32_t detector_module_id;		//id of Device Detector module which added this device
	char data[IOT_HWDEV_DATA_MAXSIZE];	//raw buffer for storing contype-dependent information about device address and hardware id. should be aligned by 4 to allow overwriting with any struct

	const iot_hwdevident_iface* find_iface(bool tryload=false) const;//searches for connection type interface class realization in local registry
		//must run in main thread if tryload is true
		//returns NULL if interface not found or cannot be loaded
};

//struct which globally identifies specific detected hw device
struct iot_hwdev_ident_t {
	iot_hwdev_localident_t dev;
	iot_hostid_t hostid; //where device is physically connected. in case this trust is template, value 0xFFFFFFFFu means any host

	const iot_hwdevident_iface* find_iface(bool tryload=false) const {//searches for connection type interface class realization in local registry
		//must run in main thread if tryload is true
		//returns NULL if interface not found or cannot be loaded
		return dev.find_iface(tryload);
	}

//	iot_hwdev_ident_t& operator=(const iot_hwdev_ident_t &i2) {
//		hostid=i2.hostid;
//		dev=i2.dev;
//		return *this;
//	}
};

//base class for interface to iot_hwdev_localident_t/iot_hwdev_ident_t data
struct iot_hwdevident_iface {
	iot_hwdevcontype_t contype;
	const char* name;

	iot_hwdevident_iface(iot_hwdevcontype_t contype, const char* name) : contype(contype), name(name) {
	}
	bool matches_hwid(const iot_hwdev_localident_t* dev_ident, const iot_hwdev_localident_t* tmpl) const { //this function tries to match hardware id from provided
																								//dev_ident data with tmpl (it can be exact specification or template)
		assert(contype==dev_ident->contype);
		if(dev_ident->contype != tmpl->contype) return false;
		return compare_hwid(dev_ident->data, tmpl->data);
	}
	bool matches_addr(const iot_hwdev_localident_t* dev_ident, const iot_hwdev_localident_t* tmpl) const { //this function tries to match address from provided
																								//dev_ident data with tmpl (it can be exact specification or template)
		assert(contype==dev_ident->contype);
		if(dev_ident->contype != tmpl->contype) return false;
		return compare_addr(dev_ident->data, tmpl->data);
	}
	bool matches(const iot_hwdev_localident_t* dev_ident, const iot_hwdev_localident_t* tmpl) const { //this function tries to match hwid and address from provided
																								//dev_ident data with tmpl (it can be exact specification or template)
		assert(contype==dev_ident->contype);
		if(dev_ident->contype != tmpl->contype) return false;
		return compare_addr(dev_ident->data, tmpl->data) && compare_hwid(dev_ident->data, tmpl->data);
	}
	//same for global ident. includes host comparison
	bool matches(const iot_hwdev_ident_t* dev_ident, const iot_hwdev_ident_t* tmpl) const { //this function tries to match HOST, hwid and address from provided
																							//dev_ident data with tmpl (it can be exact specification or template)
		assert(contype==dev_ident->dev.contype);
		if(dev_ident->dev.contype != tmpl->dev.contype) return false;
		if(tmpl->hostid!=0xFFFFFFFFu && dev_ident->hostid!=tmpl->hostid) return false; //compare host first
		return compare_addr(dev_ident->dev.data, tmpl->dev.data) && compare_hwid(dev_ident->dev.data, tmpl->dev.data);
	}
	char* sprint_addr(const iot_hwdev_localident_t* dev_ident, char* buf, size_t bufsize) const { //bufsize must include space for NUL
		assert(contype==dev_ident->contype);
		if(!bufsize) return buf;
		print_addr(dev_ident->data, buf, bufsize);
		return buf;
	}
	//same for global ident. adds host to output
	char* sprint_addr(const iot_hwdev_ident_t* dev_ident, char* buf, size_t bufsize) const { //bufsize must include space for NUL
		assert(contype==dev_ident->dev.contype);
		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, "Host %u,", unsigned(dev_ident->hostid));
		if(len>=int(bufsize-1)) return buf; //buffer is too small, output can be truncated
		print_addr(dev_ident->dev.data, buf+len, bufsize-len);
		return buf;
	}
	char* sprint(const iot_hwdev_localident_t* dev_ident, char* buf, size_t bufsize) const { //bufsize must include space for NUL
		assert(contype==dev_ident->contype);
		if(!bufsize) return buf;
		size_t len=print_addr(dev_ident->data, buf, bufsize);
		bufsize-=len;
		if(bufsize>2) {
			buf[len]=',';
			bufsize--;
			print_hwid(dev_ident->data, buf+len+1, bufsize);
		}
		return buf;
	}
	//same for global ident. adds host to output
	char* sprint(const iot_hwdev_ident_t* dev_ident, char* buf, size_t bufsize) const { //bufsize must include space for NUL
		assert(contype==dev_ident->dev.contype);
		if(!bufsize) return buf;
		int len1=snprintf(buf, bufsize, "Host %u,", unsigned(dev_ident->hostid));
		if(len1>=int(bufsize-1)) return buf; //buffer is too small, output can be truncated
		bufsize-=len1;
		size_t len=print_addr(dev_ident->dev.data, buf+len1, bufsize);
		bufsize-=len;
		if(bufsize>2) {
			buf[len1+len]=',';
			bufsize--;
			print_hwid(dev_ident->dev.data, buf+len1+len+1, bufsize);
		}
		return buf;
	}
	bool is_tmpl(const iot_hwdev_localident_t* dev_ident) const { //this function checks if dev_ident corresponds to template (i.e. is not valid device ident)
		assert(contype==dev_ident->contype);
		return check_istmpl(dev_ident->data);
	}
	//same for global ident. adds host to check
	bool is_tmpl(const iot_hwdev_ident_t* dev_ident) const { //this function checks if dev_ident corresponds to template (i.e. is not valid device ident)
		assert(contype==dev_ident->dev.contype);
		if(dev_ident->hostid==0xFFFFFFFFu) return true;
		return check_istmpl(dev_ident->dev.data);
	}
	bool is_valid(const iot_hwdev_localident_t* dev_ident) const { //this function checks if dev_ident has correct data in it with known format
		assert(contype==dev_ident->contype);
		return check_data(dev_ident->data);
	}

//customizable part of interface:
private:
	virtual bool check_data(const char* dev_data) const = 0; //actual check that data is good by format
	virtual bool check_istmpl(const char* dev_data) const = 0; //actual check that data corresponds to template (so not all data components are specified)
	virtual bool compare_hwid(const char* dev_data, const char* tmpl_data) const = 0; //actual comparison function for hwid component of device ident data
	virtual bool compare_addr(const char* dev_data, const char* tmpl_data) const = 0; //actual comparison function for address component of device ident data
	virtual size_t print_addr(const char* dev_data, char* buf, size_t bufsize) const = 0; //actual address printing function. it must return number of written bytes (without NUL)
	virtual size_t print_hwid(const char* dev_data, char* buf, size_t bufsize) const = 0; //actual hw id printing function. it must return number of written bytes (without NUL)
};

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


#define IOT_DEVIFACECLASS_DATA_MAXSIZE (32-sizeof(iot_devifaceclass_id_t))

struct iot_devifaceclassdata_iface;

struct iot_devifaceclass_data {
	iot_devifaceclass_id_t classid;
	char data[IOT_DEVIFACECLASS_DATA_MAXSIZE]; //custom classid-dependent additional data for device classification (subclassing)

	iot_devifaceclass_data(void) {}
	iot_devifaceclass_data(void (*initfunc)(iot_devifaceclass_data* cls_data)) { //this constructor allows to init structure using arbitrary iot_devifaceclassdata_iface subclass
		initfunc(this);
	}

	const iot_devifaceclassdata_iface* find_iface(bool tryload=false) const;//searches for device interface class realization in local registry
		//must run in main thread if tryload is true
		//returns NULL if interface not found or cannot be loaded
};

struct iot_devifaceclassdata_iface {
	iot_devifaceclass_id_t classid;
	const char* name;

	char* sprint(const iot_devifaceclass_data* cls_data, char* buf, size_t bufsize) const { //bufsize must include space for NUL
		assert(classid==cls_data->classid);
		if(!bufsize) return buf;
		print_data(cls_data->data, buf, bufsize);
		return buf;
	}
	uint32_t get_d2c_maxmsgsize(const iot_devifaceclass_data* cls_data) const {
		assert(classid==cls_data->classid);
		return get_d2c_maxmsgsize(cls_data->data);
	}
	uint32_t get_c2d_maxmsgsize(const iot_devifaceclass_data* cls_data) const {
		assert(classid==cls_data->classid);
		return get_c2d_maxmsgsize(cls_data->data);
	}
	bool is_valid(const iot_devifaceclass_data* cls_data) const { //this function checks if cls_data has correct data in it with known format
		assert(classid==cls_data->classid);
		return check_data(cls_data->data);
	}
	bool matches(const iot_devifaceclass_data* cls_data, const iot_devifaceclass_data* cls_tmpl) const { //this function tries to match one iface class spec with another (it can be exact specification or template)
		assert(classid==cls_data->classid);
		if(cls_data->classid != cls_tmpl->classid) return false;
		return compare(cls_data->data, cls_data->data);
	}
protected:
	iot_devifaceclassdata_iface(iot_devifaceclass_id_t classid, const char* name) : classid(classid), name(name) {
	}
//customizable part of interface:
private:
	virtual bool check_data(const char* cls_data) const { //actual check that data is good by format
		//by default assume there is no any data
		return true;
	}
	virtual size_t print_data(const char* cls_data, char* buf, size_t bufsize) const { //actual class data printing function. it must return number of written bytes (without NUL)
		//by default print just name of class. suits for classes without additional data
		int len=snprintf(buf, bufsize, "%s",name);
		return len>=int(bufsize) ? bufsize-1 : len;
	}
	virtual uint32_t get_d2c_maxmsgsize(const char* cls_data) const =0;
	virtual uint32_t get_c2d_maxmsgsize(const char* cls_data) const =0;
	virtual bool compare(const char* cls_data, const char* tmpl_data) const { //actual comparison function
		//by default assume there is no any data
		return true;
	}
};

//object of this class is used during driver instance creation to provide kernel with info about supported interface classes of device
struct iot_devifaces_list {
	iot_devifaceclass_data items[IOT_CONFIG_MAX_CLASSES_PER_DEVICE];
	int num;

	iot_devifaces_list(void) : num(0) {}
	//return 0 on success, one of error code otherwise: IOT_ERROR_LIMIT_REACHED, IOT_ERROR_INVALID_ARGS
	int add(iot_devifaceclass_data *cls) {
		if(num>=IOT_CONFIG_MAX_CLASSES_PER_DEVICE) return IOT_ERROR_LIMIT_REACHED;
		if(!cls || !cls->classid) return IOT_ERROR_INVALID_ARGS;
		items[num]=*cls;
		num++;
		return 0;
	}
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



int kapi_modinstance_self_abort(const iot_miid_t &miid, const iot_module_instance_base *instance, int errcode);

int kapi_connection_send_client_msg(const iot_connid_t &connid, iot_module_instance_base *drv_inst, iot_devifaceclass_id_t classid, const void* data, uint32_t datasize);
int kapi_connection_send_driver_msg(const iot_connid_t &connid, iot_module_instance_base *client_inst, iot_devifaceclass_id_t classid, const void* data, uint32_t datasize);

//Used by device detector modules for managing registry of available hardware devices
//action is one of {IOT_ACTION_ADD, IOT_ACTION_REMOVE,IOT_ACTION_REPLACE}
//ident.contype must be among those listed in .devcontypes field of detector module interface
//returns:




#include <iot_srcstate.h>


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



//base class for all module instances (detector, driver, event source etc)
struct iot_module_instance_base {
	uv_thread_t thread; //working thread of this instance after start
	iot_miid_t miid;    //modinstance id of this instance after start

//called in instance thread:
	iot_module_instance_base(uv_thread_t thread) : thread(thread), miid(0,0) {
	}

	//Called to start work of previously inited instance.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, whole system for detector, iot_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(const iot_miid_t &miid_)=0; //must assign miid=miid_

	//called to stop work of started instance. call can be followed by deinit or started again (if stop was manual, by user)
	//Return values:
	//0 - driver successfully stopped and can be deinited or restarted
	//IOT_ERROR_TRY_AGAIN - driver requires some time (async operation) to stop gracefully. kapi_modinstance_self_abort() will be called to notify kernel when stop is finished.
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

	iot_hwdevcontype_t* hwdevcontypes;			//pointer to array (with num_devcontypes items) of device connection types this module can manipulate
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
	iot_devifaceclass_data devclass;					//specific device iface class among supported by driver instance
	iot_process_ident_t client;							//identification of consumer module instance
};


//represents consumer-side connection to specific driver instance (TODO remove it if same as iot_conn_drvview)
struct iot_conn_clientview {
	iot_connid_t id;									//ID of this connection
	int index;											//index if this driver connection for consumer instance
	iot_devifaceclass_data devclass;					//specific device iface class among supported by driver instance
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
//called in instance thread:
	iot_device_driver_base(uv_thread_t thread) : iot_module_instance_base(thread) {
	}

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

//additional specific interface for evsrc module instances
struct iot_driver_client_base : public iot_module_instance_base {

	iot_driver_client_base(uv_thread_t thread) : iot_module_instance_base(thread) {
	}

//device handle operations map
	virtual int device_attached(const iot_conn_clientview* conn)=0;
	virtual int device_detached(const iot_conn_clientview* conn)=0;
	virtual int device_action(const iot_conn_clientview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data)=0;
};

struct iot_event_source_base : public iot_driver_client_base {
//called in instance thread:
	iot_event_source_base(uv_thread_t thread) : iot_driver_client_base(thread) {
	}
};


#include <iot_hwdevreg.h>


struct iot_iface_device_driver_t {
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

//	iot_devifaceclass_id_t* devclassids;			//pointer to array (with num_devclassids items) of class ids of device interfaces which this module provides

//	iot_device_operations_t* devops;			//pointer to array (with num_devclassids items) of map of supported operations for corresponding device class
};


///////////////////////////EVENT SOURCE INTERFACE

struct iot_deviceconn_filter_t { //represents general filter for selecting device driver
	uint8_t num_devclasses:4,					//number of allowed classes for current device (number of items in devclasses list in this struct). can be zero when item is unused (num_devices <= index of current struct) of when ANY classid is suitable.
		flag_canauto:1,						//flag that current device can be automatically selected by kernel according to classids. otherwise only user can select device
		flag_localonly:1;					//flag that current driver (and thus hwdevice) must run on same host as evsrc instance
	iot_devifaceclass_data *devclasses;		//pointer to array (with num_classids items) of class ids of devices this module can be connected to
};


struct iot_iface_event_source_t {
	uint32_t num_devices:2,						//number of devices this module should be connected to, 0, 1 or 2. (limited by IOT_CONFIG_MAX_EVENTSOURCE_DEVICES)
			num_values:4,						//number of output values this module provides. can be 0 if only messages can be generated. limited by IOT_CONFIG_MAX_EVENTSOURCE_STATES. "error" output is always present and not counted here
			cpu_loading:2;						//average level of cpu loading of started instance. 0 - minimal loading (unlimited number of such tasks can work in same working thread), 3 - very high loading (this module requires separate working thread per instance)
	iot_deviceconn_filter_t devcfg[IOT_CONFIG_MAX_EVENTSOURCE_DEVICES];
	struct {
		const char *label; //label to name output. event source output in iot configuration is matched by this label. "err" is reserved for error state
		iot_valueclass_id_t vclass_id;
	} statevaluecfg[IOT_CONFIG_MAX_EVENTSOURCE_STATES];
//	enum iot_value_type value_type;				//specifies type of 'value' field of event_source_state_t
//	size_t state_size;							//real size of state struct (sizeof(iot_srcstate_t) + custom_len)

	//reads current state of module into provided statebuf. size of statebuf must be at least state_size. Can be called from any thread. returns 0 on success or negative error code
	int (*init_instance)(iot_event_source_base**instance, uv_thread_t thread, uint32_t iot_id, const char *json_cfg); //always called in main thread
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
	uint32_t module_id;								//system assigned unique module ID for consistency check
	uint32_t version;								//module version (0xHHLLREVI = HH.LL.REVI)
	uint32_t num_devifaces:2,						//number of items in deviface_config array. can be 0
			num_devcontypes:2;						//number of items in devcontype_config array. can be 0
	int (*init_module)(void);						//always called in main thread. once after loading (if loaded dynamically) or during startup (if compiled in statically)
	int (*deinit_module)(void);						//called in main thread before unloading dynamically loaded module

	const iot_devifaceclassdata_iface **deviface_config;//optional array of module-defined device interface classes. can be NULL if num_devifaces==0
	const iot_hwdevident_iface **devcontype_config;	//optional array of module-defined device connection types. can be NULL if num_devcontypes==0

//Role interfaces
	const iot_iface_event_source_t *iface_event_source;
	const iot_iface_device_driver_t* iface_device_driver;
	const iot_iface_device_detector_t *iface_device_detector;
//TODO other roles
} iot_moduleconfig_t;


#endif //IOT_MODULE_H
