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


typedef uint32_t iot_type_id_t;		//type for HardWare DEVice CONection TYPE id

#define IOT_DEVCONTYPE_ANY 0xFFFFFFFFu


//Maximum sizeof(iot_hwdev_localident derived classes)
#define IOT_HWDEV_LOCALIDENT_MAXSIZE	128


class iot_hwdev_localident;
class iot_hwdev_details;

class iot_hwdevcontype_metaclass { //base abstract class for specifc device contype metaclass singleton objects 
	iot_type_id_t contype_id;
	uint32_t ver; //version of realization of metaclass and all its child classes
//	const char *vendor_name; //is NULL for built-in types
	const char *type_name;
	const char *parentlib;

	PACKED(
		struct serialize_base_t {
			iot_type_id_t contype_id;
		}
	);
public:
	iot_hwdevcontype_metaclass* next, *prev; //non-NULL prev means that class is registered and both next and prev are used. otherwise only next is used
													//for position in pending registration list

protected:
	iot_hwdevcontype_metaclass(iot_type_id_t id, /*const char* vendor, */const char* type, uint32_t ver, const char* parentlib=IOT_CURLIBRARY); //id must be zero for non-builtin types. vendor is NULL for builtin types. type cannot be NULL

public:
	iot_hwdevcontype_metaclass(const iot_hwdevcontype_metaclass&) = delete; //block copy-construtors and default assignments

	iot_type_id_t get_id(void) const {
		return contype_id;
	}
	const char* get_name(void) const {
		return type_name;
	}
	const char* get_library(void) const {
		return parentlib;
	}
	uint32_t get_version(void) const {
		return ver;
	}
//	const char* get_vendor(void) const {
//		return vendor_name;
//	}
	void set_contype_id(iot_type_id_t id) {
		if(contype_id>0 || !id) {
			assert(false);
			return;
		}
		contype_id=id;
	}
	char* get_fullname(char *buf, size_t bufsize, int *doff=NULL) const { //doff - delta offset. will be incremented on number of written chars
	//returns buf value
		if(!bufsize) return buf;
//		int len=snprintf(buf, bufsize, "CONTYPE:%s:%s", vendor_name ? vendor_name : "BUILTIN", type_name);
		int len=snprintf(buf, bufsize, "CONTYPE:%s", type_name);
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	static const iot_hwdevcontype_metaclass* findby_contype_id(iot_type_id_t contype_id, bool try_load=true);

	int serialized_size(const iot_hwdev_localident* obj) const {
		int res=p_serialized_size(obj);
		if(res<0) return res;
		return sizeof(serialize_base_t)+res;
	}
	int serialize(const iot_hwdev_localident* obj, char* buf, size_t bufsize) const { //returns error code or 0 on success
		assert(contype_id!=0);
		if(bufsize<sizeof(serialize_base_t)) return IOT_ERROR_NO_BUFSPACE;
		serialize_base_t *p=(serialize_base_t*)buf;
		p->contype_id=repack_type_id(contype_id);
		return p_serialize(obj, buf+sizeof(serialize_base_t), bufsize-sizeof(serialize_base_t));
	}
	static int deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) {
		//returns negative error code OR number of bytes written to provided buffer OR required buffer size when buf is NULL
		//returned value can be zero (regardless buf was NULL or not) to indicate that buffer was not used (or is not necessary) and that obj was assigned to
		//correct precreated object (may be statically allocated)
		if(datasize<sizeof(serialize_base_t)) return IOT_ERROR_BAD_DATA;
		serialize_base_t *p=(serialize_base_t*)data;
		iot_type_id_t contype_id=repack_type_id(p->contype_id);
		if(!contype_id) return IOT_ERROR_BAD_DATA;
		const iot_hwdevcontype_metaclass* metaclass=iot_hwdevcontype_metaclass::findby_contype_id(contype_id);
		if(!metaclass) return IOT_ERROR_NOT_FOUND;
		obj=NULL;
		return metaclass->p_deserialize(data+sizeof(serialize_base_t), datasize-sizeof(serialize_base_t), buf, bufsize, obj);
	}
	//returns negative error code OR number of bytes written to provided buffer OR required buffer size when buf is NULL
	//returned value can be zero (regardless buf was NULL or not) to indicate that buffer was not used (or is not necessary) and that obj was assigned to
	//correct precreated object (may be statically allocated)
	//default_contype is used when no "contype_id" property in provided json or it is incorrect. Thus if it is 0, then required.
	static int from_json(json_object* json, char* buf, size_t bufsize, const iot_hwdev_localident*& obj, iot_type_id_t default_contype=0);


private:
	virtual int p_serialized_size(const iot_hwdev_localident* obj) const = 0;
	virtual int p_serialize(const iot_hwdev_localident* obj, char* buf, size_t bufsize) const = 0; //returns error code or 0 on success
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const = 0;
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const = 0;
};

class iot_hwdev_details { //base abstract class for extended device data
	const iot_hwdevcontype_metaclass* meta; //keep reference to metaclass here to optimize (exclude virtual function call) requests which are redirected to metaclass

//?	iot_hwdev_localident(const iot_hwdev_localident&) = delete;
	iot_hwdev_details(void) = delete;
protected:
	constexpr iot_hwdev_details(const iot_hwdevcontype_metaclass* meta): meta(meta) { //only derived classes can create instances
	}
public:
	const iot_hwdevcontype_metaclass* get_metaclass(void) const {
		return meta;
	}
	iot_type_id_t get_id(void) const { //returns either valid contype id OR zero
		return meta ? meta->get_id() : 0;
	}
	bool is_valid(void) const {
		return get_id()!=0;
	}
	virtual size_t get_size(void) const = 0; //must return 0 if object is statically precreated and thus must not be copied by value, only by reference
};

class iot_hwdev_localident { //base abstract class for local device identity
	const iot_hwdevcontype_metaclass* meta; //keep reference to metaclass here to optimize (exclude virtual function call) requests which are redirected to metaclass

//?	iot_hwdev_localident(const iot_hwdev_localident&) = delete;
	iot_hwdev_localident(void) = delete;
protected:
	constexpr iot_hwdev_localident(const iot_hwdevcontype_metaclass* meta): meta(meta) { //only derived classes can create instances
	}
public:
	const iot_hwdevcontype_metaclass* get_metaclass(void) const {
		return meta;
	}
	iot_type_id_t get_id(void) const { //returns either valid contype id OR zero
		return meta->get_id();
	}
	bool is_valid(void) const {
		if(!meta) return false;
		return get_id()!=0;
	}
	void invalidate(void) { //can be used on allocated objects of derived classes or on internal buffer of iot_hwdev_ident_buffered
		meta=NULL;
	}
	char* get_fullname(char *buf, size_t bufsize, int* doff=NULL) const {
		if(meta) return meta->get_fullname(buf, bufsize, doff);

		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, "%s", "CONTYPE:INVALID");
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	const char* get_name(void) const {
		if(meta) return meta->get_name();
		return "INVALID";
	}

	virtual bool is_tmpl(void) const = 0; //check if current objects represents template. otherwise it must be exact connection specification
	virtual size_t get_size(void) const = 0; //must return 0 if object is statically precreated and thus must not be copied by value, only by reference

	bool matches(const iot_hwdev_localident* spec) const {
		if(spec->is_tmpl()) { //right operand must be exact spec
			assert(false);
			return false;
		}
		if(spec==this) return true; //match self
		return p_matches(spec);
	}
	bool matches_hwid(const iot_hwdev_localident* spec) const {
		if(spec->is_tmpl()) { //right operand must be exact spec
			assert(false);
			return false;
		}
		if(spec==this) return true; //match self
		return p_matches_hwid(spec);
	}
	bool matches_addr(const iot_hwdev_localident* spec) const {
		if(spec->is_tmpl()) { //right operand must be exact spec
			assert(false);
			return false;
		}
		if(spec==this) return true; //match self
		return p_matches_addr(spec);
	}
	virtual char* sprint_addr(char* buf, size_t bufsize, int* doff=NULL) const = 0;
	virtual char* sprint_hwid(char* buf, size_t bufsize, int* doff=NULL) const = 0;
	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const { //provide default implementation
		if(!bufsize) return buf;

		int len=0;
		get_fullname(buf, bufsize, &len);
		if(int(bufsize)-len>2) {
			buf[len++]='{';
			sprint_addr(buf+len, bufsize-len, &len);
			if(int(bufsize)-len>2) {
				buf[len++]=',';
				sprint_hwid(buf+len, bufsize-len, &len);
				if(int(bufsize)-len>1) {
					buf[len++]='}';
					buf[len]='\0';
				}
			}
		}
		if(doff) *doff+=len;
		return buf;
	}

	int serialized_size(void) const {
		return meta->serialized_size(this);
	}
	int serialize(char* buf, size_t bufsize) const { //returns error code or 0 on success
		return meta->serialize(this, buf, bufsize);
	}
private:
	virtual bool p_matches(const iot_hwdev_localident* spec) const = 0;
	virtual bool p_matches_hwid(const iot_hwdev_localident* spec) const = 0; //must check if hardware identification part of device identity matches current template or exact spec
	virtual bool p_matches_addr(const iot_hwdev_localident* spec) const = 0; //must check if hardware address part of device identity matches current template or exact spec. Hardware adress part must be unique for whole host.
};




//Describe ANY hwdevcontype
//represents special hwdev contype to create filters matching ANY contypes

class iot_hwdevcontype_metaclass_any : public iot_hwdevcontype_metaclass {
	iot_hwdevcontype_metaclass_any(void) : iot_hwdevcontype_metaclass(IOT_DEVCONTYPE_ANY, "any", IOT_VERSION_COMPOSE(0,1,1)) {}
public:
	static iot_hwdevcontype_metaclass_any object; //the only instance of this class

private:
	virtual int p_serialized_size(const iot_hwdev_localident* obj) const override {
		return 0; //this contype requires no additional memory
	}
	virtual int p_serialize(const iot_hwdev_localident* obj, char* buf, size_t bufsize) const override {
		return 0;
	}
//	virtual int p_deserialized_size(const char* data, size_t datasize, const iot_hwdev_localident*& obj) const override;
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const override;
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const override;
};

//this class can be used for templates only. It represents universal template matching ANY other contypes
class iot_hwdev_localident_any : public iot_hwdev_localident {
	iot_hwdev_localident_any(const iot_hwdev_localident_any&) = delete; //block copy constructor because this is singleton
	iot_hwdev_localident_any(void):iot_hwdev_localident(&iot_hwdevcontype_metaclass_any::object) {} //block creation of new objects because this is singleton
public:
	const static iot_hwdev_localident_any object;

	virtual bool is_tmpl(void) const override {
		return true; //this class can be used for templates only
	}
	virtual size_t get_size(void) const override {
		return 0;//sizeof(*this);
	}
	virtual char* sprint_addr(char* buf, size_t bufsize, int* doff=NULL) const override {
		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, "%s", "tmpl:any");
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	virtual char* sprint_hwid(char* buf, size_t bufsize, int* doff=NULL) const override {
		return iot_hwdev_localident_any::sprint_addr(buf, bufsize, doff);
	}
	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const override {
		return iot_hwdev_localident_any::sprint_addr(buf, bufsize, doff);
	}
private:
	virtual bool p_matches(const iot_hwdev_localident* spec) const override {
		return true;
	}
	virtual bool p_matches_hwid(const iot_hwdev_localident* spec) const override {
		return true;
	}
	virtual bool p_matches_addr(const iot_hwdev_localident* spec) const override {
		return true;
	}
};


struct iot_hwdev_ident {
	iot_hostid_t hostid; //where device is physically connected. in case this trust is template, value IOT_HOSTID_ANY means any host
	const iot_hwdev_localident* local;

	PACKED(
		struct serialize_t {
			iot_hostid_t hostid;
		}
	);


	bool is_tmpl(void) const {
		if(hostid==IOT_HOSTID_ANY) return true;
		return local->is_tmpl();
	}
	bool is_valid(void) const {
		return hostid!=0 && local && local->is_valid();
	}
	bool matches(const iot_hwdev_ident* spec) const {
		if(!spec->is_valid()) {
			assert(false);
			return false;
		}
		if(spec->is_tmpl()) { //right operand must be exact spec
			assert(false);
			return false;
		}
		if(spec==this) return true; //match self
		if(hostid!=IOT_HOSTID_ANY && hostid!=spec->hostid) return false; //compare host first
		return local->matches(spec->local);
	}
	bool matches_hwid(const iot_hwdev_ident* spec) const {
		if(!spec->is_valid()) {
			assert(false);
			return false;
		}
		if(spec->is_tmpl()) { //right operand must be exact spec
			assert(false);
			return false;
		}
		if(spec==this) return true; //match self
		if(hostid!=IOT_HOSTID_ANY && hostid!=spec->hostid) return false; //compare host first
		return local->matches_hwid(spec->local);
	}
	bool matches_addr(const iot_hwdev_ident* spec) const {
		if(!spec->is_valid()) {
			assert(false);
			return false;
		}
		if(spec->is_tmpl()) { //right operand must be exact spec
			assert(false);
			return false;
		}
		if(spec==this) return true; //match self
		if(hostid!=IOT_HOSTID_ANY && hostid!=spec->hostid) return false; //compare host first
		return local->matches_addr(spec->local);
	}
//	int sprint_addr(char* buf, size_t bufsize) const {
//		if(!bufsize) return 0;
//		//TODO
//		return 0;
//	}
//	int sprint_hwid(char* buf, size_t bufsize) const {
//		if(!bufsize) return 0;
//		//TODO
//		return 0;
//	}
	char* sprint(char* buf, size_t bufsize, int* doff=NULL) const {
		if(!bufsize) return buf;
		int len;
		if(hostid==IOT_HOSTID_ANY) {
			len=snprintf(buf, bufsize, "Host=Any,");
		} else {
			len=snprintf(buf, bufsize, "Host=%" IOT_PRIhostid ",", hostid);
		}
		if(int(bufsize)-len>2) {
			local->sprint(buf+len, bufsize-len, &len);
		}
		if(doff) *doff+=len;
		return buf;
	}
	int serialized_size(void) const {
		int res=local->serialized_size();
		if(res<0) return res;
		return sizeof(serialize_t)+res;
	}
	int serialize(char* buf, size_t bufsize) const { //returns false if provided buffer wasn't enough
		if(bufsize<sizeof(serialize_t)) return IOT_ERROR_NO_BUFSPACE;
		serialize_t *p=(serialize_t*)buf;
		p->hostid=repack_hostid(hostid);
		return local->serialize(buf+sizeof(serialize_t), bufsize-sizeof(serialize_t));
	}
	static int deserialize(const char* data, size_t datasize, iot_hwdev_ident* obj, char* identbuf, size_t bufsize) { //obj must point to somehow allocated iot_hwdev_ident struct
		//returns negative error code OR number of bytes written to provided  identobj buffer OR required buffer size if obj was NULL
		if(datasize<sizeof(serialize_t)) return IOT_ERROR_BAD_DATA;

		serialize_t *p=(serialize_t*)data;
		iot_hostid_t hostid=repack_hostid(p->hostid);
		if(!hostid) return IOT_ERROR_BAD_DATA;

		int rval;
		const iot_hwdev_localident* localp;
		if(!obj) { //request to calculate bufsize
			localp=NULL;
			rval=iot_hwdevcontype_metaclass::deserialize(data+sizeof(serialize_t), datasize-sizeof(serialize_t), NULL, 0, localp);
			return rval;
		}
		localp=(iot_hwdev_localident*)identbuf;

		rval=iot_hwdevcontype_metaclass::deserialize(data+sizeof(serialize_t), datasize-sizeof(serialize_t), identbuf, bufsize, localp); //can return 0 and reassign localp to address of static object

		if(rval<0) return rval;
		//all is good
		obj->hostid=hostid;
		obj->local=localp;
		return rval;
	}
	static int from_json(json_object* json, iot_hwdev_ident* obj, char* identbuf, size_t bufsize, iot_hostid_t default_hostid=0, iot_type_id_t default_contype=0); //obj must point to somehow (from stack etc.) allocated iot_hwdev_ident struct

};

struct iot_hwdev_ident_buffered : public iot_hwdev_ident { //same as iot_hwdev_ident but has builtin buffer for restoring any type of iot_hwdev_localident
	alignas(iot_hwdev_localident) char localbuf[IOT_HWDEV_LOCALIDENT_MAXSIZE]; //preallocated buffer 

	iot_hwdev_ident_buffered(iot_hostid_t hostid_, const iot_hwdev_localident* ident) {
		assert(ident!=NULL);
		hostid=hostid_;
		size_t sz=ident->get_size();
		if(!sz) {
			local=ident;
		} else {
			if(sz>sizeof(localbuf)) {
				assert(false);
				local=NULL;
				return;
			}
			memcpy(localbuf, ident, sz);
			local=(iot_hwdev_localident*)localbuf;
		}
	}
	iot_hwdev_ident_buffered(iot_hostid_t hostid_=0) {
		hostid=hostid_;
		iot_hwdev_localident* p=(iot_hwdev_localident*)localbuf;
		local=p;
		p->invalidate();
	}

	static int deserialize(const char* data, size_t datasize, iot_hwdev_ident_buffered* obj) { //obj must point to somehow allocated iot_hwdev_ident_buffered struct
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		return iot_hwdev_ident::deserialize(data, datasize, obj, obj->localbuf, sizeof(obj->localbuf));
	}
	static int from_json(json_object* json, iot_hwdev_ident_buffered *obj, iot_hostid_t default_hostid=0, iot_type_id_t default_contype=0) { //obj must point to somehow (from stack etc.) allocated iot_hwdev_ident struct
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		return iot_hwdev_ident::from_json(json, obj, obj->localbuf, sizeof(obj->localbuf), default_hostid, default_contype);
	}
};




/*
//make iot_hwdevident_iface be 128 bytes
#define IOT_HWDEV_DATA_MAXSIZE	(128 - sizeof(iot_hwdevcontype_t) - sizeof(uint32_t) - 1 - sizeof(iot_hostid_t))
//struct which locally identifies hardware device depending on connection type. interface to this data is of class iot_hwdevident_iface.
struct iot_hwdevident_iface;

struct iot_hwdev_localident_t {
	iot_hwdevcontype_t contype;			//type of connection identification among predefined IOT_DEVCONTYPE_ constants or DeviceDetector
										//module-specific built by IOT_DEVCONTYPE_CUSTOM macro. Value IOT_DEVCONTYPE_ANY means any contype. data is useless in such case
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

	iot_hwdevident_iface(iot_hwdevcontype_t contype) : contype(contype) {
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

	virtual const char* get_name(void) const = 0;
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

*/
//system connection types


//USB device identified by its USB ids.
#define IOT_DEVCONTYPE_USB			1
#define IOT_DEVCONTYPE_VIRTUAL			2 //TODO



//////////////////////////////////////////////////////////////////////
//////////////////////////hw device interface API type
//////////////////////////////////////////////////////////////////////

//typedef uint32_t iot_devifacetype_id_t;	//type for device interface class ID (abstraction over hardware devices provided by device drivers)

//#define IOT_DEVIFACETYPE_PENDING 0xFFFFFFFEu
//#define IOT_DEVIFACETYPE_ERROR 0xFFFFFFFDu

#define IOT_DEVIFACE_PARAMS_MAXSIZE 32

class iot_deviface_params;

class iot_devifacetype_metaclass { //base abstract class for specifc device interface metaclass singleton objects
	iot_type_id_t ifacetype_id;
	uint32_t ver; //version of realization of metaclass and all its child classes
//	const char *vendor_name; //is NULL for built-in types
	const char *type_name;
	const char *parentlib;

	PACKED(
		struct serialize_base_t {
			iot_type_id_t ifacetype_id;
		}
	);
public:
	iot_devifacetype_metaclass* next, *prev; //non-NULL prev means that class is registered and both next and prev are used. otherwise only next is used
													//for position in pending registration list

protected:
	iot_devifacetype_metaclass(iot_type_id_t id, /*const char* vendor, */const char* type, uint32_t ver, const char* parentlib=IOT_CURLIBRARY); //id must be zero for non-builtin types. vendor is NULL for builtin types. type cannot be NULL

public:
	iot_devifacetype_metaclass(const iot_devifacetype_metaclass&) = delete; //block copy-construtors and default assignments

	iot_type_id_t get_id(void) const {
		return ifacetype_id;
	}
	uint32_t get_version(void) const {
		return ver;
	}
	const char* get_name(void) const {
		return type_name;
	}
	const char* get_library(void) const {
		return parentlib;
	}
//	const char* get_vendor(void) const {
//		return vendor_name;
//	}
	void set_ifacetype_id(iot_type_id_t id) {
		if(ifacetype_id>0 || !id) {
			assert(false);
			return;
		}
		ifacetype_id=id;
	}
	char* get_fullname(char *buf, size_t bufsize, int *doff=NULL) const { //doff - delta offset. will be incremented on number of written chars
	//returns buf value
		if(!bufsize) return buf;
//		int len=snprintf(buf, bufsize, "IFACETYPE:%s:%s", vendor_name ? vendor_name : "BUILTIN", type_name);
		int len=snprintf(buf, bufsize, "IFACETYPE:%s", type_name);
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	static const iot_devifacetype_metaclass* findby_ifacetype_id(iot_type_id_t ifacetype_id, bool try_load=true);

	int serialized_size(const iot_deviface_params* obj) const {
		int res=p_serialized_size(obj);
		if(res<0) return res;
		return sizeof(serialize_base_t)+res;
	}
	int serialize(const iot_deviface_params* obj, char* buf, size_t bufsize) const; //returns error code or 0 on success
	static int deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_deviface_params*& obj) {
		//returns negative error code OR number of bytes written to provided buffer OR required buffer size when buf is NULL
		//returned value can be zero (regardless buf was NULL or not) to indicate that buffer was not used (or is not necessary) and that obj was assigned to
		//correct precreated object (may be statically allocated)
		if(datasize<sizeof(serialize_base_t)) return IOT_ERROR_BAD_DATA;
		serialize_base_t *p=(serialize_base_t*)data;
		iot_type_id_t ifacetype_id=repack_type_id(p->ifacetype_id);
		if(!ifacetype_id) return IOT_ERROR_BAD_DATA;
		const iot_devifacetype_metaclass* metaclass=iot_devifacetype_metaclass::findby_ifacetype_id(ifacetype_id);
		if(!metaclass) return IOT_ERROR_NOT_FOUND;
		obj=NULL;
		return metaclass->p_deserialize(data+sizeof(serialize_base_t), datasize-sizeof(serialize_base_t), buf, bufsize, obj);
	}
	//returns negative error code OR number of bytes written to provided buffer OR required buffer size when buf is NULL
	//returned value can be zero (regardless buf was NULL or not) to indicate that buffer was not used (or is not necessary) and that obj was assigned to
	//correct precreated object (may be statically allocated)
	//default_ifacetype is used when no "ifacetype_id" property in provided json or it is incorrect. Thus if it is 0, then required.
	static int from_json(json_object* json, char* buf, size_t bufsize, const iot_deviface_params*& obj, iot_type_id_t default_ifacetype=0);

	//returns negative error code (IOT_ERROR_NO_MEMORY) or zero on success
	int to_json(const iot_deviface_params* obj, json_object* &dst) const;

private:
	virtual int p_serialized_size(const iot_deviface_params* obj) const = 0;
	virtual int p_serialize(const iot_deviface_params* obj, char* buf, size_t bufsize) const = 0; //returns error code or 0 on success
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_deviface_params*& obj) const = 0;
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_deviface_params*& obj) const = 0;
	virtual int p_to_json(const iot_deviface_params* obj, json_object* &dst) const = 0;
};


class iot_deviface_params { //base class for representing device interface params
	const iot_devifacetype_metaclass* meta; //keep reference to metaclass here to optimize (exclude virtual function call) requests which are redirected to metaclass

	iot_deviface_params(void) = delete;
protected:
	constexpr iot_deviface_params(const iot_devifacetype_metaclass* meta): meta(meta) { //only derived classes can create instances
	}
public:
	const iot_devifacetype_metaclass* get_metaclass(void) const {
		return meta;
	}
	iot_type_id_t get_id(void) const { //returns either valid ifacetype id OR zero
		return meta->get_id();
	}
	bool is_valid(void) const {
		if(!meta) return false;
		return get_id()!=0;
	}
	void invalidate(void) { //can be used on allocated objects of derived classes or on internal buffer of iot_hwdev_ident_buffered
		meta=NULL;
	}
	char* get_fullname(char *buf, size_t bufsize, int* doff=NULL) const {
		if(meta) return meta->get_fullname(buf, bufsize, doff);

		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, "%s", "IFACETYPE:INVALID");
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	const char* get_name(void) const {
		if(meta) return meta->get_name();
		return "INVALID";
	}


	bool matches(const iot_deviface_params* spec) const {
		if(!spec->is_valid()) {
			assert(false);
			return false;
		}
		if(spec->is_tmpl()) { //right operand must be exact spec
			assert(false);
			return false;
		}
		if(spec==this) return true; //match self
		return p_matches(spec);
	}

	int serialized_size(void) const {
		return meta->serialized_size(this);
	}
	int serialize(char* buf, size_t bufsize) const { //returns error code or 0 on success
		return meta->serialize(this, buf, bufsize);
	}
	int to_json(json_object* &dst) const { //returns error code or 0 on success
		return meta->to_json(this, dst);
	}

	virtual bool is_tmpl(void) const = 0; //check if current objects represents template. otherwise it must be exact connection specification
	virtual size_t get_size(void) const = 0; //must return 0 if object is statically precreated and thus must not be copied by value, only by reference
	virtual uint32_t get_d2c_maxmsgsize(void) const = 0;
	virtual uint32_t get_c2d_maxmsgsize(void) const = 0;
	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const = 0;
private:
	virtual bool p_matches(const iot_deviface_params* spec) const = 0;
};


struct iot_deviface_params_buffered { //same as iot_deviface_params but has builtin buffer for storing any type of iot_deviface_params
	const iot_deviface_params* data;
	alignas(iot_deviface_params) char databuf[IOT_DEVIFACE_PARAMS_MAXSIZE]; //preallocated buffer 

	iot_deviface_params_buffered(void) {
		iot_deviface_params* p=(iot_deviface_params*)databuf;
		data=p;
		p->invalidate();
	}
	iot_deviface_params_buffered(const iot_deviface_params_buffered &op) {
		size_t sz=op.data->get_size();
		if(op.data==(iot_deviface_params*)op.databuf) {
			assert(sz>0);
			if(sz>sizeof(databuf)) {
				assert(false);
				data=NULL;
				return;
			}
			memcpy(databuf, op.data, sz);
			data=(iot_deviface_params*)databuf;
		} else {
			assert(sz==0);
			data=op.data;
		}
	}

	bool is_valid(void) const {
		if(!data) return false;
		return data->is_valid();
	}
	iot_deviface_params_buffered& operator=(const iot_deviface_params* pars) {
		assert(pars!=NULL);
		size_t sz=pars->get_size();
		if(!sz) {
			data=pars;
		} else {
			if(sz>sizeof(databuf)) {
				assert(false);
				data=NULL;
				return *this;
			}
			memcpy(databuf, pars, sz);
			data=(iot_deviface_params*)databuf;
		}
		return *this;
	}
	iot_deviface_params_buffered& operator=(const iot_deviface_params_buffered &op) {
		size_t sz=op.data->get_size();
		if(op.data==(iot_deviface_params*)op.databuf) {
			assert(sz>0);
			if(sz>sizeof(databuf)) {
				assert(false);
				data=NULL;
				return *this;
			}
			memcpy(databuf, op.data, sz);
			data=(iot_deviface_params*)databuf;
		} else {
			assert(sz==0);
			data=op.data;
		}
		return *this;
	}

	static int deserialize(const char* data, size_t datasize, iot_deviface_params_buffered* obj) { //obj must point to somehow allocated iot_deviface_params_buffered struct
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		const iot_deviface_params* pdata=(iot_deviface_params*)obj->databuf;
		int rval=iot_devifacetype_metaclass::deserialize(data, datasize, obj->databuf, sizeof(obj->databuf), pdata);

		if(rval<0) return rval;
		//all is good
		obj->data=pdata;
		return rval;
	}
	static int from_json(json_object* json, iot_deviface_params_buffered *obj, iot_type_id_t default_ifacetype=0) { //obj must point to somehow (from stack etc.) allocated iot_hwdev_ident struct
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		const iot_deviface_params* pdata=(iot_deviface_params*)obj->databuf;
		int rval=iot_devifacetype_metaclass::from_json(json, obj->databuf, sizeof(obj->databuf), pdata, default_ifacetype);

		if(rval<0) return rval;
		//all is good
		obj->data=pdata;
		return rval;
	}
};






/*




struct iot_devifacetype_iface;

struct iot_devifacetype final {
	iot_devifacetype_id_t classid=0; //IOT_DEVIFACETYPE_PENDING value selects 'pending' struct, otherwise 'data' is actual. zero value means invalid struct
	union {
		alignas(8) char data[IOT_DEVIFACETYPE_DATA_MAXSIZE]; //custom classid-dependent additional data for device classification (subclassing)
		struct {
			const char* bundle_name;
//			void (*init_func)(iot_devifacetype* cls_data); //this function will be called after successful bundle load
		} pending; //used for bundle-custom classes when interface class is defined in another bundle and thus cannot be used until that bundle is loaded
	};

	iot_devifacetype(void) {} //init struct as invalid
	iot_devifacetype(void (*initfunc)(iot_devifacetype* cls_data)) { //this constructor allows to init structure using arbitrary built-in or own iot_devifacetype_iface subclass
		initfunc(this);
	}
//	iot_devifacetype(const char* bundle, void (*initfunc)(iot_devifacetype* cls_data)) : classid(IOT_DEVIFACETYPE_PENDING) { //this constructor allows to init structure using arbitrary iot_devifacetype_iface subclass
//		pending.bundle_name=bundle;
//		pending.init_func=initfunc;
//	}

	const iot_devifacetype_iface* find_iface(bool tryload=false) const;//searches for device interface class realization in local registry
		//must run in main thread if tryload is true
		//returns NULL if interface not found or cannot be loaded
};


struct iot_devifacetype_iface { //base class for interfaces to iot_devifacetype
	iot_devifacetype_id_t classid;
//	uint8_t relative_index=0;
//	const char* bundle_name=NULL;
	iot_devifacetype_iface* next, *prev; //non-NULL prev means that class is registered and both next and prev are used. otherwise only next is used
													//for position in pending registration list

protected:
	iot_devifacetype_iface(iot_devifacetype_id_t classid); //for built-in classes with fixed class IDs
//	iot_devifacetype_iface(const char* bundle, uint8_t rel_idx); //for bundle custom classes

public:
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
//customizable part of interface:
public:
	virtual const char* get_name(void) const = 0;
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
		int len=snprintf(buf, bufsize, "%s",get_name());
		return len>=int(bufsize) ? bufsize-1 : len;
	}
	virtual uint32_t get_d2c_maxmsgsize(const char* cls_data) const =0;
	virtual uint32_t get_c2d_maxmsgsize(const char* cls_data) const =0;
	virtual bool compare(const char* cls_data, const char* tmpl_data) const { //actual comparison function
		//by default assume there is no any data
		return true;
	}
};
*/

//IOT DEVCLASSID codes with type iot_devifacetype_id_t
#define IOT_DEVIFACETYPE_IDMAP(XX) \
	XX(KEYBOARD, 1000) 	/*of keys or standard keyboard (with SHIFT, CTRL and ALT keys)*/	\
	XX(ACTIVATABLE, 1001)	/*simplest interface of set of devices which can be activated or deactivated without current status information */	\


enum iot_deviface_basic_ids : iot_type_id_t {
#define XX(nm, cc) IOT_DEVIFACETYPEID_ ## nm = cc,
	IOT_DEVIFACETYPE_IDMAP(XX)
#undef XX
	IOT_DEVIFACETYPEID_MAX = 1001
};





//////////////////////////////////////////////////////////////////////
//////////////////////////hw device interface
//////////////////////////////////////////////////////////////////////


//int kapi_connection_send_client_msg(const iot_connid_t &connid, iot_module_instance_base *drv_inst, iot_devifacetype_id_t classid, const void* data, uint32_t datasize);
//int kapi_connection_send_driver_msg(const iot_connid_t &connid, iot_module_instance_base *client_inst, iot_devifacetype_id_t classid, const void* data, uint32_t datasize);


class iot_deviface__DRVBASE {
	const iot_conn_drvview* drvconn;
protected:
	iot_deviface__DRVBASE(void) : drvconn(NULL) {}
//	iot_deviface__DRVBASE(const iot_conn_drvview* conn_=NULL) {
//		init(conn_);
//	}
	bool init(const iot_conn_drvview* conn_=NULL) {
		drvconn=conn_;
		return !!conn_;
	}
	int send_client_msg(const void *msg, uint32_t msgsize) const;
	int read_client_req(void* buf, uint32_t bufsize, uint32_t &dataread, uint32_t &szleft) const;
public:
	bool is_inited(void) const {
		return drvconn!=NULL;
	}
};

class iot_deviface__CLBASE {
	const iot_conn_clientview* clconn;
protected:
	iot_deviface__CLBASE(void): clconn(NULL) {}
//	iot_deviface__CLBASE(const iot_conn_clientview* conn_=NULL) {
//		init(conn_);
//	}
	bool init(const iot_conn_clientview* conn_=NULL) {
		clconn=conn_;
		return !!conn_;
	}
	int send_driver_msg(const void *msg, uint32_t msgsize) const;
	int32_t start_driver_req(const void *data, uint32_t datasize, uint32_t fulldatasize=0) const;
	int32_t continue_driver_req(const void *data, uint32_t datasize) const;
public:
	bool is_inited(void) const {
		return clconn!=NULL;
	}
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
