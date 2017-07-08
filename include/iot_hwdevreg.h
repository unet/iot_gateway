#ifndef IOT_HWDEVREG_H
#define IOT_HWDEVREG_H
//Contains constants, methods and data structures for hardware devices representation


#ifndef DAEMON_KERNEL
////////////////////////////////////////////////////////////////////
///////////////////////////Specific declarations for external modules
////////////////////////////////////////////////////////////////////

//macro for generating ID of custom module specific hw device connection type
#define IOT_DEVCONTYPE_CUSTOM(module_id, index) (((module_id)<<8)+((index) & 0xff))

//macro for generating ID of custom module specific device interface class
#define IOT_DEVIFACETYPE_CUSTOM(module_id, index) (((module_id)<<8)+((index) & 0xff))

#endif

//////////////////////////////////////////////////////////////////////
//////////////////////////hw device connection type and identification
//////////////////////////////////////////////////////////////////////


typedef uint32_t iot_hwdevcontype_t;		//type for HardWare DEVice CONection TYPE id
#define IOT_DEVCONTYPE_ANY 0xFFFFFFFFu



//make iot_hwdevident_iface be 128 bytes
#define IOT_HWDEV_DATA_MAXSIZE	(128 - sizeof(iot_hwdevcontype_t) - sizeof(uint32_t) - 1 - sizeof(iot_hostid_t))
//struct which locally identifies hardware device depending on connection type. interface to this data is of class iot_hwdevident_iface.
struct iot_hwdevident_iface;

struct iot_hwdev_localident_t {
	iot_hwdevcontype_t contype;			//type of connection identification among predefined IOT_DEVCONTYPE_ constants or DeviceDetector
										//module-specific built by IOT_DEVCONTYPE_CUSTOM macro. Value IOT_DEVCONTYPE_ANY means any conntype. data is useless in such case
	uint32_t detector_module_id;		//id of Device Detector module which added this device
	char data[IOT_HWDEV_DATA_MAXSIZE];	//raw buffer for storing contype-dependent information about device address and hardware id. should be aligned by 4 to allow overwriting with any struct

	const iot_hwdevident_iface* find_iface(bool tryload=false) const;//searches for connection type interface class realization in local registry
		//must run in main thread if tryload is true
		//returns NULL if interface not found or cannot be loaded
};

//struct which globally identifies specific detected hw device
struct iot_hwdev_ident_t {
	iot_hwdev_localident_t dev;
	iot_hostid_t hostid; //where device is physically connected. in case this trust is template, value IOT_HOSTID_ANY means any host

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
		assert(contype==dev_ident->contype); //interface class must match left operand
		if(dev_ident->contype != tmpl->contype) return false;
		return compare_hwid(dev_ident->data, tmpl->data);
	}
	bool matches_addr(const iot_hwdev_localident_t* dev_ident, const iot_hwdev_localident_t* tmpl) const { //this function tries to match address from provided
																								//dev_ident data with tmpl (it can be exact specification or template)
		assert(contype==dev_ident->contype); //interface class must match left operand
		if(dev_ident->contype != tmpl->contype) return false;
		return compare_addr(dev_ident->data, tmpl->data);
	}

	bool matches(const iot_hwdev_localident_t* dev_ident, const iot_hwdev_localident_t* tmpl) const { //this function tries to match hwid and address from provided
																								//dev_ident data with tmpl (it can be exact specification or template)
		assert(contype==dev_ident->contype); //interface class must match left operand
		assert(!is_tmpl(dev_ident)); //left operand must be full exact spec

		if(tmpl->contype==IOT_DEVCONTYPE_ANY) return true;
		if(dev_ident->contype != tmpl->contype) return false;
		return compare_addr(dev_ident->data, tmpl->data) && compare_hwid(dev_ident->data, tmpl->data);
	}
	//same for global ident. includes host comparison
	bool matches(const iot_hwdev_ident_t* dev_ident, const iot_hwdev_ident_t* tmpl) const { //this function tries to match HOST, hwid and address from provided
																							//dev_ident data with tmpl (it can be exact specification or template)
		assert(contype==dev_ident->dev.contype); //interface class must match left operand
		assert(!is_tmpl(dev_ident)); //left operand must be full exact spec

		if(tmpl->hostid!=IOT_HOSTID_ANY && dev_ident->hostid!=tmpl->hostid) return false; //compare host first
		return matches(&dev_ident->dev, &tmpl->dev);
	}

	char* sprint_addr(const iot_hwdev_localident_t* dev_ident, char* buf, size_t bufsize) const { //bufsize must include space for NUL
		assert(contype==dev_ident->contype); //interface class must match left operand
		if(!bufsize) return buf;
		print_addr(dev_ident->data, buf, bufsize);
		return buf;
	}
	//same for global ident. adds host to output
	char* sprint_addr(const iot_hwdev_ident_t* dev_ident, char* buf, size_t bufsize) const { //bufsize must include space for NUL
		assert(contype==dev_ident->dev.contype); //interface class must match left operand
		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, dev_ident->hostid==IOT_HOSTID_ANY ? "Any host," : "Host %u,", unsigned(dev_ident->hostid));
		if(len>=int(bufsize-1)) return buf; //buffer is too small, output can be truncated
		print_addr(dev_ident->dev.data, buf+len, bufsize-len);
		return buf;
	}

	char* sprint(const iot_hwdev_localident_t* dev_ident, char* buf, size_t bufsize) const { //bufsize must include space for NUL
		assert(contype==dev_ident->contype); //interface class must match left operand
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
		assert(contype==dev_ident->dev.contype); //interface class must match left operand
		if(!bufsize) return buf;
		int len1=snprintf(buf, bufsize, dev_ident->hostid==IOT_HOSTID_ANY ? "Any host," : "Host %u,", unsigned(dev_ident->hostid));
		if(len1>=int(bufsize-1)) return buf; //buffer is too small, output can be truncated
		bufsize-=len1;
		if(contype==IOT_DEVCONTYPE_ANY) {
			snprintf(buf+len1, bufsize, "%s", "any connection type");
			return buf;
		}
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
		assert(contype==dev_ident->contype); //interface class must match left operand
		return check_istmpl(dev_ident->data);
	}
	//same for global ident. adds host to check
	bool is_tmpl(const iot_hwdev_ident_t* dev_ident) const { //this function checks if dev_ident corresponds to template (i.e. is not valid device ident)
		assert(contype==dev_ident->dev.contype); //interface class must match left operand
		if(dev_ident->hostid==IOT_HOSTID_ANY) return true;
		return check_istmpl(dev_ident->dev.data);
	}
	bool is_valid(const iot_hwdev_localident_t* dev_ident) const { //this function checks if dev_ident has correct data in it with known format
		assert(contype==dev_ident->contype); //interface class must match left operand
		return check_data(dev_ident->data);
	}

	static bool restore_from_json(json_object* obj, iot_hwdev_ident_t &dev_ident); //returns true if data was correct and dev_ident filled with valid info

//customizable part of interface:
private:
	virtual size_t to_json(const char* dev_data, char* buf, size_t bufsize) const = 0;
	virtual bool from_json(json_object* obj, char* dev_data) const = 0;
	virtual const char* get_vistmpl(void) const = 0;
	virtual bool check_data(const char* dev_data) const = 0; //actual check that data is good by format
	virtual bool check_istmpl(const char* dev_data) const = 0; //actual check that data corresponds to template (so not all data components are specified)
	virtual bool compare_hwid(const char* dev_data, const char* tmpl_data) const = 0; //actual comparison function for hwid component of device ident data
	virtual bool compare_addr(const char* dev_data, const char* tmpl_data) const = 0; //actual comparison function for address component of device ident data
	virtual size_t print_addr(const char* dev_data, char* buf, size_t bufsize) const = 0; //actual address printing function. it must return number of written bytes (without NUL)
	virtual size_t print_hwid(const char* dev_data, char* buf, size_t bufsize) const = 0; //actual hw id printing function. it must return number of written bytes (without NUL)
};


//system connection types


//USB device identified by its USB ids.
#define IOT_DEVCONTYPE_USB			1
#define IOT_DEVCONTYPE_VIRTUAL			2 //TODO



//////////////////////////////////////////////////////////////////////
//////////////////////////hw device interface API type
//////////////////////////////////////////////////////////////////////

typedef uint32_t iot_devifacetype_id_t;	//type for device interface class ID (abstraction over hardware devices provided by device drivers)



#define IOT_DEVIFACETYPE_DATA_MAXSIZE (32-sizeof(iot_devifacetype_id_t))

struct iot_devifacetype_iface;

struct iot_devifacetype final {
	iot_devifacetype_id_t classid;
	char data[IOT_DEVIFACETYPE_DATA_MAXSIZE]; //custom classid-dependent additional data for device classification (subclassing)

	iot_devifacetype(void) {}
	iot_devifacetype(void (*initfunc)(iot_devifacetype* cls_data)) { //this constructor allows to init structure using arbitrary iot_devifacetype_iface subclass
		initfunc(this);
	}

	const iot_devifacetype_iface* find_iface(bool tryload=false) const;//searches for device interface class realization in local registry
		//must run in main thread if tryload is true
		//returns NULL if interface not found or cannot be loaded
};

struct iot_devifacetype_iface { //base class for interfaces to iot_devifacetype
	iot_devifacetype_id_t classid;
	const char* name;

	char* sprint(const iot_devifacetype* cls_data, char* buf, size_t bufsize) const { //bufsize must include space for NUL
		assert(classid==cls_data->classid);
		if(!bufsize) return buf;
		print_data(cls_data->data, buf, bufsize);
		return buf;
	}
	uint32_t get_d2c_maxmsgsize(const iot_devifacetype* cls_data) const {
		assert(classid==cls_data->classid);
		return get_d2c_maxmsgsize(cls_data->data);
	}
	uint32_t get_c2d_maxmsgsize(const iot_devifacetype* cls_data) const {
		assert(classid==cls_data->classid);
		return get_c2d_maxmsgsize(cls_data->data);
	}
	bool is_valid(const iot_devifacetype* cls_data) const { //this function checks if cls_data has correct data in it with known format
		assert(classid==cls_data->classid);
		return check_data(cls_data->data);
	}
	bool is_tmpl(const iot_devifacetype* cls_data) const { //this function checks if cls_data corresponds to template (i.e. is not valid device interface type)
		assert(classid==cls_data->classid);
		return check_istmpl(cls_data->data);
	}
	bool matches(const iot_devifacetype* cls_data, const iot_devifacetype* cls_tmpl) const { //this function tries to match one iface class spec with another (it can be exact specification or template)
		assert(classid==cls_data->classid);
		assert(!is_tmpl(cls_data)); //left operand must be full exact spec
		if(cls_data->classid != cls_tmpl->classid) return false;
		return compare(cls_data->data, cls_tmpl->data);
	}
protected:
	iot_devifacetype_iface(iot_devifacetype_id_t classid, const char* name) : classid(classid), name(name) {
	}
//customizable part of interface:
private:
	virtual bool check_data(const char* cls_data) const { //actual check that data is good by format
		//by default assume there is no any data
		return true;
	}
	virtual bool check_istmpl(const char* cls_data) const { //actual check that data corresponds to template (so not all data components are specified)
		//by default assume there is no any data and thus pure classid is not template
		return false;
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


//IOT DEVCLASSID codes with type iot_devifacetype_id_t
#define IOT_DEVIFACETYPE_IDMAP(XX) \
	XX(KEYBOARD, 1) /*sizeof(iot_devifaceclass__keyboard_ATTR) set of keys or standard keyboard (with SHIFT, CTRL and ALT keys)*/	\
	XX(ACTIVATABLE, 2) /*simplest interface of set of devices which can be activated or deactivated without current status information */	\
	XX(TONEPLAYER, 3) /*basic sound source which can play list of tone specifications (lengh + frequency) */						\
	XX(HW_SWITCHES, 4) /*set of hardware switchers like notebook lid opening sensor*/												

enum iot_devifaceclass_basic_ids : uint8_t {
#define XX(nm, cc) IOT_DEVIFACETYPEID_ ## nm = cc,
	IOT_DEVIFACETYPE_IDMAP(XX)
#undef XX
	IOT_DEVIFACETYPEID_MAX = 255
};





//////////////////////////////////////////////////////////////////////
//////////////////////////hw device interface
//////////////////////////////////////////////////////////////////////


//int kapi_connection_send_client_msg(const iot_connid_t &connid, iot_module_instance_base *drv_inst, iot_devifacetype_id_t classid, const void* data, uint32_t datasize);
//int kapi_connection_send_driver_msg(const iot_connid_t &connid, iot_module_instance_base *client_inst, iot_devifacetype_id_t classid, const void* data, uint32_t datasize);


class iot_devifaceclass__DRVBASE {
	const iot_devifacetype* devclass;
protected:
	iot_devifaceclass__DRVBASE(const iot_devifacetype* devclass) : devclass(devclass) {
	}
	int send_client_msg(const iot_conn_drvview *conn, const void *msg, uint32_t msgsize);
	int read_client_req(const iot_conn_drvview *conn_, void* buf, uint32_t bufsize, uint32_t &dataread, uint32_t &szleft);
};

class iot_devifaceclass__CLBASE {
	const iot_devifacetype* devclass;
protected:
	iot_devifaceclass__CLBASE(const iot_devifacetype* devclass) : devclass(devclass) {
	}
	int send_driver_msg(const iot_conn_clientview* conn, const void *msg, uint32_t msgsize);
	int32_t start_driver_req(const iot_conn_clientview* conn_, const void *data, uint32_t datasize, uint32_t fulldatasize=0);
	int32_t continue_driver_req(const iot_conn_clientview* conn_, const void *data, uint32_t datasize);
};




typedef struct { //represents custom data for devices with IOT_DEVCONTYPE_USB connection type
	uint8_t busid;  //optional bus id to help find device or 0 to find by vendor:product
	uint8_t connid; //optional bus connection id to help find device or 0 to find by vendor:product
	uint16_t vendor;//vendor code of device
	uint16_t product;//product code of device
} iot_hwdevcontype_usb_t;




//typedef struct { //represents ID of device with IOT_DEVCONTYPE_SOCKET connection type
//	char conn[64]; //connection string like '[unix|tcp|upd]:[path|hostname:port|IPV4:port|IPV6:port]
//} iot_hwdevcontype_socket_t;


#endif //IOT_HWDEVREG_H
