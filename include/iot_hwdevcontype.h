#ifndef IOT_HWDEVCONTYPE_H
#define IOT_HWDEVCONTYPE_H
//Contains constants, methods and data structures for hardware devices representation


//////////////////////////////////////////////////////////////////////
//////////////////////////hw device connection type and identification
//////////////////////////////////////////////////////////////////////


#define IOT_DEVCONTYPE_ANY 0xFFFFFFFFu


//Maximum sizeof(iot_hwdev_localident derived classes)
#define IOT_HWDEV_LOCALIDENT_MAXSIZE	128


class iot_hwdev_localident;
class iot_hwdev_details;

class iot_hwdevcontype_metaclass { //base abstract class for specifc device contype metaclass singleton objects 
	friend class iot_libregistry_t;
	iot_type_id_t contype_id;
	iot_hwdevcontype_metaclass* next, *prev; //non-NULL prev means that class is registered and both next and prev are used. otherwise only next is used
													//for position in pending registration list

	PACKED(
		struct serialize_base_t {
			iot_type_id_t contype_id;
		}
	);
public:
	const uint32_t version; //version of realization of metaclass and all its child classes
//	const char *vendor_name; //is NULL for built-in types
	const char *const type_name;
	const char *const parentlib;


protected:
	iot_hwdevcontype_metaclass(iot_type_id_t id, /*const char* vendor, */const char* type, uint32_t ver, const char* parentlib=IOT_CURLIBRARY); //id must be zero for non-builtin types. vendor is NULL for builtin types. type cannot be NULL

public:
	iot_hwdevcontype_metaclass(const iot_hwdevcontype_metaclass&) = delete; //block copy-construtors and default assignments

	iot_type_id_t get_id(void) const {
		return contype_id;
	}
//	const char* get_name(void) const {
//		return type_name;
//	}
//	const char* get_library(void) const {
//		return parentlib;
//	}
//	uint32_t get_version(void) const {
//		return ver;
//	}
//	const char* get_vendor(void) const {
//		return vendor_name;
//	}
	char* get_fullname(char *buf, size_t bufsize, int *doff=NULL) const { //doff - delta offset. will be incremented on number of written chars
	//returns buf value
		if(!bufsize) return buf;
//		int len=snprintf(buf, bufsize, "CONTYPE:%s:%s", vendor_name ? vendor_name : "BUILTIN", type_name);
		int len=snprintf(buf, bufsize, "CONTYPE:%s", type_name);
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	static const iot_hwdevcontype_metaclass* findby_id(iot_type_id_t contype_id, bool try_load=true);

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
		const iot_hwdevcontype_metaclass* metaclass=iot_hwdevcontype_metaclass::findby_id(contype_id);
		if(!metaclass) return IOT_ERROR_NOT_FOUND;
		obj=NULL;
		return metaclass->p_deserialize(data+sizeof(serialize_base_t), datasize-sizeof(serialize_base_t), buf, bufsize, obj);
	}
	//returns negative error code OR number of bytes written to provided buffer OR required buffer size when buf is NULL
	//returned value can be zero (regardless buf was NULL or not) to indicate that buffer was not used (or is not necessary) and that obj was assigned to
	//correct precreated object (may be statically allocated)
	//default_contype is used when no "contype_id" property in provided json or it is incorrect. Thus if it is 0, then required.
	static int from_json(json_object* json, char* buf, size_t bufsize, const iot_hwdev_localident*& obj, iot_type_id_t default_contype=0);

	//returns negative error code (IOT_ERROR_NO_MEMORY) or zero on success
	int to_json(const iot_hwdev_localident* obj, json_object* &dst) const;


private:
	void set_id(iot_type_id_t id) {
		if(contype_id>0 || !id) {
			assert(false);
			return;
		}
		contype_id=id;
	}
	virtual int p_serialized_size(const iot_hwdev_localident* obj) const = 0;
	virtual int p_serialize(const iot_hwdev_localident* obj, char* buf, size_t bufsize) const = 0; //returns error code or 0 on success
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const = 0;
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const = 0;
	virtual int p_to_json(const iot_hwdev_localident* obj, json_object* &dst) const = 0;
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
	char* get_fulltypename(char *buf, size_t bufsize, int* doff=NULL) const {
		if(meta) return meta->get_fullname(buf, bufsize, doff);

		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, "%s", "CONTYPE:INVALID");
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	const char* get_typename(void) const {
		if(meta) return meta->type_name;
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
		get_fulltypename(buf, bufsize, &len);
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
	virtual int p_to_json(const iot_hwdev_localident* obj, json_object* &dst) const override {
		dst=NULL; //no ident data
		return 0;
	}

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






//USB device identified by its USB ids.
//#define IOT_DEVCONTYPE_USB			1
//#define IOT_DEVCONTYPE_VIRTUAL			2 //TODO




#endif //IOT_HWDEVCONTYPE_H
