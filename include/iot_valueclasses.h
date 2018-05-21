#ifndef IOT_VALUECLASSES_H
#define IOT_VALUECLASSES_H
//declarations for built-in value types used for node inputs and outputs


//#include <stdlib.h>
//#include <stdint.h>

//#ifndef DAEMON_CORE
////////////////////////////////////////////////////////////////////
///////////////////////////Specific declarations for external modules
////////////////////////////////////////////////////////////////////
//macro for generating ID of custom module specific class of state data
//#define IOT_VALUECLASS_CUSTOM(module_id, index) (((module_id)<<8)+((index) & 0x7f)*2)

//#define IOT_MSGCLASS_CUSTOM(module_id, index) (((module_id)<<8)+((index) & 0x7f)*2+1)

//#endif


class iot_datavalue;


class iot_datatype_metaclass {
	friend class iot_libregistry_t;
	iot_type_id_t datatype_id;
	iot_datatype_metaclass* next, *prev; //non-NULL prev means that class is registered and both next and prev are used. otherwise only next is used
													//for position in pending registration list

	PACKED(
		struct serialize_base_t {
			iot_type_id_t datatype_id;
		}
	);
public:
	const uint32_t version; //version of realization of metaclass and all its child classes
	const char *const type_name;
	const char *const parentlib;

	const bool fixedsize_values; //flag that ALL objects of this type have equal datasize. flag is unmeaningful if static_values is true
	const bool static_values;//flag that ALL possible values of data type are precreated statically and thus no memory allocation required


protected:
	iot_datatype_metaclass(iot_type_id_t id, const char* type, uint32_t ver, bool static_values, bool fixedsize_values=true, const char* parentlib=IOT_CURLIBRARY); //id must be zero for non-builtin types. type cannot be NULL

	//constexpr constructor can be used for CORE types only!!! types utilizing this constructor must have manual registration after program start!!!
//	constexpr iot_datatype_metaclass(iot_type_id_t id, bool is_manualreg/*used jsut to select this constructor*/, const char* type, uint32_t ver, bool static_values, bool fixedsize_values=true) :
//			datatype_id(id), version(ver), type_name(type), parentlib("CORE"), fixedsize_values(fixedsize_values), static_values(static_values), next(NULL), prev(NULL)
//	{
//	}

public:
	iot_datatype_metaclass(const iot_datatype_metaclass&) = delete; //block copy-construtors and default assignments

	iot_type_id_t get_id(void) const {
		return datatype_id;
	}

	char* get_fullname(char *buf, size_t bufsize, int *doff=NULL) const { //doff - delta offset. will be incremented on number of written chars
	//returns buf value
		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, "DATATYPE:%s", type_name);
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	static const iot_datatype_metaclass* findby_id(iot_type_id_t datatype_id, bool try_load=true);

	int serialized_size(const iot_datavalue* obj) const {
		int res=p_serialized_size(obj);
		if(res<0) return res;
		return sizeof(serialize_base_t)+res;
	}
	int serialize(const iot_datavalue* obj, char* buf, size_t bufsize) const; //returns error code or 0 on success
	static int deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_datavalue*& obj) {
		//returns negative error code OR number of bytes written to provided buffer OR required buffer size when buf is NULL
		//returned value can be zero (regardless buf was NULL or not) to indicate that buffer was not used (or is not necessary) and that obj was assigned to
		//correct precreated object (may be statically allocated)
		if(datasize<sizeof(serialize_base_t)) return IOT_ERROR_BAD_DATA;
		serialize_base_t *p=(serialize_base_t*)data;
		iot_type_id_t datatype_id=repack_type_id(p->datatype_id);
		if(!datatype_id) return IOT_ERROR_BAD_DATA;
		const iot_datatype_metaclass* metaclass=iot_datatype_metaclass::findby_id(datatype_id);
		if(!metaclass) return IOT_ERROR_NOT_FOUND;
		obj=NULL;
		return metaclass->p_deserialize(data+sizeof(serialize_base_t), datasize-sizeof(serialize_base_t), buf, bufsize, obj);
	}
	//returns negative error code OR number of bytes written to provided buffer OR required buffer size when buf is NULL
	//returned value can be zero (regardless buf was NULL or not) to indicate that buffer was not used (or is not necessary) and that obj was assigned to
	//correct precreated object (may be statically allocated)
	//default_datatype is used when no "datatype_id" property in provided json or it is incorrect. Thus if it is 0, then required.
	static int from_json(json_object* json, char* buf, size_t bufsize, const iot_datavalue*& obj, iot_type_id_t default_datatype=0);

	//returns negative error code (IOT_ERROR_NO_MEMORY) or zero on success
	int to_json(const iot_datavalue* obj, json_object* &dst) const;

	virtual char* sprint(const iot_datavalue* v, char* buf, size_t bufsize, int* doff=NULL) const = 0;
	virtual bool check_eq(const iot_datavalue* v, const iot_datavalue *op) const = 0;
private:
	void set_id(iot_type_id_t id) {
		if(datatype_id>0 || !id) {
			assert(false);
			return;
		}
		datatype_id=id;
	}
	virtual int p_serialized_size(const iot_datavalue* obj) const = 0;
	virtual int p_serialize(const iot_datavalue* obj, char* buf, size_t bufsize) const = 0; //returns error code or 0 on success
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_datavalue*& obj) const = 0;
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_datavalue*& obj) const = 0;
	virtual int p_to_json(const iot_datavalue* obj, json_object* &dst) const = 0;
};


class iot_datavalue { //base class for representing device interface params
	const iot_datatype_metaclass* meta; //keep reference to metaclass here to optimize (exclude virtual function call) requests which are redirected to metaclass
protected:
	uint32_t datasize;			//size of whole value, including sizeof base class
	uint16_t custom_u16;		//unused by this base class data fields which can be used by derived classes (these bytes are busy by 8-byte alignment)
	uint8_t custom_u8;
private:
	uint8_t is_memblock:1;//flag that this object was allocated as memblock and thus release() and incref() will call corresponding memblock methods. otherwise those methods do nothing. cannot be used true together with is_static
public:
	const uint8_t is_fixedsize:1,		//flag that ALL objects of this type have equal datasize (copied from metaclass during construction to speed-up access)
			is_static:1;		//flag that ALL possible values of data type are precreated statically and thus no memory allocation required (copied from metaclass during construction to speed-up access)

protected:
	iot_datavalue(const iot_datatype_metaclass* meta, bool is_memblock, uint32_t datasize, uint16_t custom_u16, uint8_t custom_u8)
			: meta(meta), datasize(datasize), custom_u16(custom_u16), custom_u8(custom_u8), is_memblock(is_memblock), is_fixedsize(meta->fixedsize_values), is_static(meta->static_values) {
		if(is_static) {
			assert(!is_memblock);
		}
	}

	iot_datavalue(const iot_datatype_metaclass* meta, bool is_memblock, uint32_t datasize) //variant for classes which do not use custom fields of calculate them in constructor
			: meta(meta), datasize(datasize), is_memblock(is_memblock), is_fixedsize(meta->fixedsize_values), is_static(meta->static_values) {
		if(is_static) {
			assert(!is_memblock);
		}
	}

	constexpr iot_datavalue(const iot_datatype_metaclass* meta, uint32_t datasize, uint16_t custom_u16, uint8_t custom_u8) : 
		meta(meta), datasize(datasize), custom_u16(custom_u16), custom_u8(custom_u8), is_memblock(false),
		is_fixedsize(meta->fixedsize_values), is_static(meta->static_values) { //only derived classes can create instances
	}
public:
	iot_datavalue(void) = delete;

	const iot_datatype_metaclass* get_metaclass(void) const {
		return meta;
	}
	iot_type_id_t get_id(void) const { //returns either valid datatype id OR zero
		return meta->get_id();
	}
//	bool is_valid(void) const {
//		if(!meta) return false;
//		return get_id()!=0;
//	}
//	void invalidate(void) { //can be used on allocated objects of derived classes or on internal buffer of iot_hwdev_ident_buffered
//		meta=NULL;
//	}
	char* get_fulltypename(char *buf, size_t bufsize, int* doff=NULL) const {
		if(meta) return meta->get_fullname(buf, bufsize, doff);

		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, "%s", "DATATYPE:INVALID");
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	const char* get_typename(void) const {
		if(meta) return meta->type_name;
		return "INVALID";
	}

	constexpr uint32_t get_size(void) const {
		return datasize;
	}

	bool operator==(const iot_datavalue &op) const {
		if(&op==this) return true; //same object
		if(meta!=op.meta) return false;
		if(!meta) {
			assert(false);
			return false;
		}
		return meta->check_eq(this, &op);
	}
	bool operator!=(const iot_datavalue &op) const {
		return !(*this==op);
	}

	void release(void) const {
		if(!is_memblock) return;
		assert(!is_static);
		iot_release_memblock(const_cast<iot_datavalue*>(this));
	}
	bool incref(void) const {
		if(!is_memblock) return true;
		assert(!is_static);
		return iot_incref_memblock(const_cast<iot_datavalue*>(this));
	}

	//must copy data value/msg into specified buf. bufsize must be at least get_size() bytes. memblock tells if buf was allocated as memblock
	//returns NULL if bufsize is too small OR current object has is_fixedvals property (so cannot be copied by content, only by address)
	iot_datavalue* copyTo(void* buf, uint32_t bufsize, bool memblock=true) const {
		if(is_static || bufsize<datasize) return NULL;
		iot_datavalue* dst=(iot_datavalue*)buf;
		memcpy(dst, this, get_size());
		dst->is_memblock=memblock ? 1 : 0;
		return dst;
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

//	virtual size_t get_size(void) const = 0; //must return 0 if object is statically precreated and thus must not be copied by value, only by reference
	char* sprint(char* buf, size_t bufsize, int* doff=NULL) const {
		if(meta) return meta->sprint(this, buf, bufsize, doff); //move sprint realization to metaclass to make iot_datavalue and its derivatives NON POLIMORPHIC

		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, "%s", "DATAVALUE:INVALID");
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
};




///////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////  BOOLEAN   /////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////


class iot_datavalue_boolean: public iot_datavalue {
//	friend class iot_datatype_metaclass_boolean;
	//forbid to create class objects other than static constants by making constructor private
	constexpr iot_datavalue_boolean(bool val);// : iot_datavalue(&iot_datatype_metaclass_boolean::object, sizeof(iot_datavalue_boolean), 0, val ? 1 : 0) {}
public:
	static const iot_datavalue_boolean const_true;
	static const iot_datavalue_boolean const_false;

	iot_datavalue_boolean(const iot_datavalue_boolean&) = delete; //forbid implicit copy (and move) constructor

//	iot_datavalue_boolean& operator=(uint32_t st) {
//		state=st;
//		return *this;
//	}
	explicit operator bool(void) const {
		return custom_u8==1;
	}
	bool operator!(void) const {
		return custom_u8==0;
	}
	bool value(void) const {
		return custom_u8==1;
	}
	static const iot_datavalue_boolean* cast(const iot_datavalue *val);// { //if val is not NULL and has correct class, casts pointer to this class
//		if(val && val->get_metaclass()==&iot_datatype_metaclass_boolean::object) return static_cast<const iot_datavalue_boolean*>(val);
//		return NULL;
//	}
};

class iot_datatype_metaclass_boolean : public iot_datatype_metaclass {
	/*constexpr*/ iot_datatype_metaclass_boolean(void) : iot_datatype_metaclass(0, "boolean", IOT_VERSION_COMPOSE(0,0,1), true) {}
	PACKED(
		struct serialize_t {
			uint8_t value;
		}
	);

public:
	static iot_datatype_metaclass_boolean object;

	virtual char* sprint(const iot_datavalue* v, char* buf, size_t bufsize, int* doff=NULL) const override { //bufsize must include space for NUL
		assert(v && v->get_metaclass()==this);
//		const iot_datavalue_boolean* val=static_cast<const iot_datavalue_boolean*>(v);
		if(!bufsize) return buf;
//		snprintf(buf, bufsize, "'%s' %s",type_name, val->custom_u8==1 ? "TRUE" : "FALSE");
		int len=snprintf(buf, bufsize, "'%s' %s",type_name, v==&iot_datavalue_boolean::const_true ? "TRUE" : "FALSE");
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	virtual bool check_eq(const iot_datavalue* v, const iot_datavalue *op) const override {
		assert(v && v->get_metaclass()==this && op && op->get_metaclass()==this);
		return v==op; //this class has static value instances
//		const iot_datavalue_boolean* val=static_cast<const iot_datavalue_boolean*>(v);
//		const iot_datavalue_boolean* opval=static_cast<const iot_datavalue_boolean*>(op);
//		return val->custom_u8==opval->custom_u8;
	}
private:
	virtual int p_serialized_size(const iot_datavalue* obj0) const override {
		const iot_datavalue_boolean* obj=iot_datavalue_boolean::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		return sizeof(serialize_t);
	}
	virtual int p_serialize(const iot_datavalue* obj0, char* buf, size_t bufsize) const override {
		const iot_datavalue_boolean* obj=iot_datavalue_boolean::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		if(bufsize<sizeof(serialize_t)) return IOT_ERROR_NO_BUFSPACE;

		serialize_t *s=(serialize_t*)buf;
		s->value=obj->value() ? 1 : 0;
		return 0;
	}
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_datavalue*& obj) const override {
		assert(false);
		return 0;
	}
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_datavalue*& obj) const override {
		assert(false);
		return 0;
	}
	virtual int p_to_json(const iot_datavalue* obj0, json_object* &dst) const override {
		const iot_datavalue_boolean* obj=iot_datavalue_boolean::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		json_object* ob=json_object_new_object();
		if(!ob) return IOT_ERROR_NO_MEMORY;

		json_object* val=json_object_new_boolean(obj->value());
		if(!val) {
			json_object_put(ob);
			return IOT_ERROR_NO_MEMORY;
		}
		json_object_object_add(ob, "value", val);
		dst=ob;
		return 0;
	}
};

constexpr iot_datavalue_boolean::iot_datavalue_boolean(bool val) : iot_datavalue(&iot_datatype_metaclass_boolean::object, sizeof(iot_datavalue_boolean), 0, val ? 1 : 0) {}

inline const iot_datavalue_boolean* iot_datavalue_boolean::cast(const iot_datavalue *val) { //if val is not NULL and has correct class, casts pointer to this class
	if(val && val->get_metaclass()==&iot_datatype_metaclass_boolean::object) return static_cast<const iot_datavalue_boolean*>(val);
	return NULL;
}


///////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////  PULSE (for used in messages) ///////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////

//this datavalue holds no any subvalue, so can represent only availability of itself
class iot_datavalue_pulse: public iot_datavalue {
//	friend class iot_datatype_metaclass_boolean;
	//forbid to create class objects other than static constants by making constructor private
	constexpr iot_datavalue_pulse(void);// : iot_datavalue(&iot_datatype_metaclass_boolean::object, sizeof(iot_datavalue_pulse), 0, val ? 1 : 0) {}
public:
	static const iot_datavalue_pulse object;

	iot_datavalue_pulse(const iot_datavalue_pulse&) = delete; //forbid implicit copy (and move) constructor

	static const iot_datavalue_pulse* cast(const iot_datavalue *val);// { //if val is not NULL and has correct class, casts pointer to this class
//		if(val && val->get_metaclass()==&iot_datatype_metaclass_boolean::object) return static_cast<const iot_datavalue_pulse*>(val);
//		return NULL;
//	}
};

class iot_datatype_metaclass_pulse : public iot_datatype_metaclass {
	/*constexpr*/ iot_datatype_metaclass_pulse(void) : iot_datatype_metaclass(0, "pulse", IOT_VERSION_COMPOSE(0,0,1), true) {}

public:
	static iot_datatype_metaclass_pulse object;

	virtual char* sprint(const iot_datavalue* v, char* buf, size_t bufsize, int* doff=NULL) const override { //bufsize must include space for NUL
		assert(v && v->get_metaclass()==this);
//		const iot_datavalue_pulse* val=static_cast<const iot_datavalue_pulse*>(v);
		if(!bufsize) return buf;
//		snprintf(buf, bufsize, "'%s' %s",type_name, val->custom_u8==1 ? "TRUE" : "FALSE");
		int len=snprintf(buf, bufsize, "'%s'",type_name);
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	virtual bool check_eq(const iot_datavalue* v, const iot_datavalue *op) const override {
		assert(v && v->get_metaclass()==this && op && op->get_metaclass()==this);
		return v==op; //this class has static value instances
//		const iot_datavalue_pulse* val=static_cast<const iot_datavalue_pulse*>(v);
//		const iot_datavalue_pulse* opval=static_cast<const iot_datavalue_pulse*>(op);
//		return val->custom_u8==opval->custom_u8;
	}
private:
	virtual int p_serialized_size(const iot_datavalue* obj0) const override {
		const iot_datavalue_pulse* obj=iot_datavalue_pulse::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		return 0;
	}
	virtual int p_serialize(const iot_datavalue* obj0, char* buf, size_t bufsize) const override {
		const iot_datavalue_pulse* obj=iot_datavalue_pulse::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		return 0;
	}
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_datavalue*& obj) const override {
		assert(false);
		return 0;
	}
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_datavalue*& obj) const override {
		assert(false);
		return 0;
	}
	virtual int p_to_json(const iot_datavalue* obj0, json_object* &dst) const override {
		const iot_datavalue_pulse* obj=iot_datavalue_pulse::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		dst=NULL;
		return 0;
	}
};

constexpr iot_datavalue_pulse::iot_datavalue_pulse(void) : iot_datavalue(&iot_datatype_metaclass_pulse::object, sizeof(iot_datavalue_pulse), 0, 0) {}

inline const iot_datavalue_pulse* iot_datavalue_pulse::cast(const iot_datavalue *val) { //if val is not NULL and has correct class, casts pointer to this class
	if(val && val->get_metaclass()==&iot_datatype_metaclass_pulse::object) return static_cast<const iot_datavalue_pulse*>(val);
	return NULL;
}


///////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////  NODEERRORSTATE   //////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////


class iot_datavalue_nodeerrorstate: public iot_datavalue {
	//uses custom_u16 to store up to 16 flags
public:
	enum : uint16_t {
		IOT_NODEERRORSTATE_NOINSTANCE=1,		//module instance still not created
		IOT_NODEERRORSTATE_NODEVICE=2,			//required device(s) not connected
	};

	static const iot_datavalue_nodeerrorstate const_noinst;

	iot_datavalue_nodeerrorstate(uint16_t st, bool memblock); //normal constructor
	constexpr iot_datavalue_nodeerrorstate(uint16_t st); //constructor for statically precreated values

	explicit operator bool(void) const {
		return custom_u16!=0;
	}
	bool operator!(void) const {
		return custom_u16==0;
	}
	iot_datavalue_nodeerrorstate& set_noinstance(void) {
		custom_u16 |= IOT_NODEERRORSTATE_NOINSTANCE;
		custom_u16 &= ~(IOT_NODEERRORSTATE_NODEVICE);
		return *this;
	}

	uint16_t value(void) const {
		return custom_u16;
	}
	static const iot_datavalue_nodeerrorstate* cast(const iot_datavalue *val);
};

class iot_datatype_metaclass_nodeerrorstate : public iot_datatype_metaclass {
	/*constexpr */iot_datatype_metaclass_nodeerrorstate(void) : iot_datatype_metaclass(0, "nodeerrorstate", IOT_VERSION_COMPOSE(0,0,1), false, true) {}
	PACKED(
		struct serialize_t {
			uint16_t value;
		}
	);

public:
	static iot_datatype_metaclass_nodeerrorstate object;

	virtual char* sprint(const iot_datavalue* v, char* buf, size_t bufsize, int* doff=NULL) const override { //bufsize must include space for NUL
		assert(v && v->get_metaclass()==this);
		if(!bufsize) return buf;
		const iot_datavalue_nodeerrorstate* val=static_cast<const iot_datavalue_nodeerrorstate*>(v);
		int len=snprintf(buf, bufsize, "'%s' 0x%04x",type_name, val->value());
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	virtual bool check_eq(const iot_datavalue* v, const iot_datavalue *op) const override {
		assert(v && v->get_metaclass()==this && op && op->get_metaclass()==this);
		const iot_datavalue_nodeerrorstate* val=static_cast<const iot_datavalue_nodeerrorstate*>(v);
		const iot_datavalue_nodeerrorstate* opval=static_cast<const iot_datavalue_nodeerrorstate*>(op);
		return val->value()==opval->value();
	}
private:
	virtual int p_serialized_size(const iot_datavalue* obj0) const override {
		const iot_datavalue_nodeerrorstate* obj=iot_datavalue_nodeerrorstate::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		return sizeof(serialize_t);
	}
	virtual int p_serialize(const iot_datavalue* obj0, char* buf, size_t bufsize) const override {
		const iot_datavalue_nodeerrorstate* obj=iot_datavalue_nodeerrorstate::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		if(bufsize<sizeof(serialize_t)) return IOT_ERROR_NO_BUFSPACE;

		serialize_t *s=(serialize_t*)buf;
		s->value=repack_uint16(obj->value());
		return 0;
	}
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_datavalue*& obj) const override {
		assert(false);
		return 0;
	}
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_datavalue*& obj) const override {
		assert(false);
		return 0;
	}
	virtual int p_to_json(const iot_datavalue* obj0, json_object* &dst) const override {
		const iot_datavalue_nodeerrorstate* obj=iot_datavalue_nodeerrorstate::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		json_object* ob=json_object_new_object();
		if(!ob) return IOT_ERROR_NO_MEMORY;

		json_object* val=json_object_new_int(obj->value());
		if(!val) {
			json_object_put(ob);
			return IOT_ERROR_NO_MEMORY;
		}
		json_object_object_add(ob, "value", val);
		dst=ob;
		return 0;
	}
};

inline iot_datavalue_nodeerrorstate::iot_datavalue_nodeerrorstate(uint16_t val, bool is_memblock) : 
		iot_datavalue(&iot_datatype_metaclass_nodeerrorstate::object, is_memblock, sizeof(iot_datavalue_nodeerrorstate), val, 0) {}
constexpr iot_datavalue_nodeerrorstate::iot_datavalue_nodeerrorstate(uint16_t val) : iot_datavalue(&iot_datatype_metaclass_nodeerrorstate::object, sizeof(iot_datavalue_nodeerrorstate), val, 0) {}

inline const iot_datavalue_nodeerrorstate* iot_datavalue_nodeerrorstate::cast(const iot_datavalue *val) { //if val is not NULL and has correct class, casts pointer to this class
	if(val && val->get_metaclass()==&iot_datatype_metaclass_nodeerrorstate::object) return static_cast<const iot_datavalue_nodeerrorstate*>(val);
	return NULL;
}


///////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////  BITMAP   //////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////

#ifdef __linux__
	#include <linux/input-event-codes.h>
#endif


class iot_datavalue_bitmap: public iot_datavalue { //DYNAMICALL SIZED OBJECT!!! MEMORY FOR THIS OBJECT MUST BE ALLOCATED EXPLICITELY after 
															//call to iot_valuetype_bitmap::calc_datasize. Constructor  on allocated memory called
															//like 'new(memptr) iot_valuetype_bitmap(arguments)'
	//uses custom_u16 to store statesize
#define statesize custom_u16
//	uint16_t statesize; //number of items in state
	uint32_t state[]; //bitmap of currently depressed keys

public:
	iot_datavalue_bitmap(uint32_t max_code, bool memblock);
	iot_datavalue_bitmap(uint32_t max_code, const uint32_t* statemap, uint16_t statemapsize, bool memblock);

	static const iot_datavalue_bitmap* cast(const iot_datavalue *val);

	static size_t calc_datasize(uint32_t max_code) { //allows to determine necessary memory for object to hold state map with specific max key code
		//returns 0 on unallowed value
		if(max_code>=65535*32) max_code=65535*32-1;
		return sizeof(iot_datavalue_bitmap)+sizeof(uint32_t)*((max_code / 32)+1);
	}
	constexpr static uint32_t get_maxkeycode(void) { //in cases when bitmap represents key states, this method can be used to obtain maximum possible key code to calculate maximum bitmap size
#ifdef __linux__
		return KEY_MAX;
#else
		return 255;  //TODO for other OSes
#endif
	}

	uint32_t get_maxcode(void) const {
		return statesize*32-1;
	}
	uint16_t get_statesize(void) const {
		return statesize;
	}
	const uint32_t* get_statebuf(void) const {
		return state;
	}
	uint32_t test_code(uint32_t code) const {
		if(code>=statesize*32) return 0;
		return bitmap32_test_bit(state, code);
	}

	void set_code(uint32_t code) {
		if(code>=statesize*32) {
			assert(false);
			return;
		}
		bitmap32_set_bit(state, code);
	}
	void clear_code(uint32_t code) {
		if(code>=statesize*32) {
			assert(false);
			return;
		}
		bitmap32_clear_bit(state, code);
	}
	iot_datavalue_bitmap& operator= (const iot_datavalue_bitmap& op) {
		if(&op==this) return *this;
		if(statesize<=op.statesize) {
			memcpy(state, op.state, statesize*sizeof(uint32_t));
		} else {
			memcpy(state, op.state, op.statesize*sizeof(uint32_t));
			memset(state+op.statesize, 0, (statesize-op.statesize)*sizeof(uint32_t));
		}
		return *this;
	}
	iot_datavalue_bitmap& operator|= (const iot_datavalue_bitmap& op) {
		if(&op==this) return *this;
		if(statesize<=op.statesize) {
			for(uint32_t i=0;i<statesize;i++) state[i]|=op.state[i];
		} else {
			for(uint32_t i=0;i<op.statesize;i++) state[i]|=op.state[i];
		}
		return *this;
	}
	explicit operator bool(void) const {
		for(uint32_t i=0;i<statesize;i++) if(state[i]) return true;
		return false;
	}
#undef statesize
};

class iot_datatype_metaclass_bitmap : public iot_datatype_metaclass {
	iot_datatype_metaclass_bitmap(void) : iot_datatype_metaclass(0, "bitmap", IOT_VERSION_COMPOSE(0,0,1), false, false) {}
	PACKED(
		struct serialize_t {
			uint16_t statesize;
			uint32_t state[];
		}
	);

public:
	static iot_datatype_metaclass_bitmap object;

	virtual char* sprint(const iot_datavalue* v, char* buf, size_t bufsize, int* doff=NULL) const override { //bufsize must include space for NUL
		assert(v && v->get_metaclass()==this);
		if(!bufsize) return buf;
		const iot_datavalue_bitmap* val=static_cast<const iot_datavalue_bitmap*>(v);
		int len=snprintf(buf, bufsize, "'%s' size=%u bits, data [", type_name, val->get_maxcode()+1);
		int statesize=val->get_statesize();
		const uint32_t* state=val->get_statebuf();
		for(int i=statesize-1; i>=0 && len<int(bufsize); i--) {
			uint32_t b=repack_uint32(state[i]); //get little endian
			len+=snprintf(buf+len, bufsize-len, " %02x %02x %02x %02x", unsigned(b>>24), unsigned((b>>16)&0xff), unsigned((b>>8)&0xff), unsigned(b&0xff));
		}
		if(doff) *doff += len>=int(bufsize) ? int(bufsize)-1 : len;
		return buf;
	}
	virtual bool check_eq(const iot_datavalue* v, const iot_datavalue *op) const override {
		assert(v && v->get_metaclass()==this && op && op->get_metaclass()==this);
		const iot_datavalue_bitmap* val=static_cast<const iot_datavalue_bitmap*>(v);
		const iot_datavalue_bitmap* opval=static_cast<const iot_datavalue_bitmap*>(op);

		return val->get_statesize()==opval->get_statesize() && memcmp(val->get_statebuf(), opval->get_statebuf(), val->get_statesize()*sizeof(uint32_t))==0;
	}
private:
	virtual int p_serialized_size(const iot_datavalue* obj0) const override {
		const iot_datavalue_bitmap* obj=iot_datavalue_bitmap::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		return sizeof(serialize_t);
	}
	virtual int p_serialize(const iot_datavalue* obj0, char* buf, size_t bufsize) const override {
		const iot_datavalue_bitmap* obj=iot_datavalue_bitmap::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
//		if(bufsize<sizeof(serialize_t)) return IOT_ERROR_NO_BUFSPACE;

//		serialize_t *s=(serialize_t*)buf;
//		s->value=repack_uint16(obj->value());
		assert(false);
		return 0;
	}
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_datavalue*& obj) const override {
		assert(false);
		return 0;
	}
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_datavalue*& obj) const override {
		assert(false);
		return 0;
	}
	virtual int p_to_json(const iot_datavalue* obj0, json_object* &dst) const override {
		const iot_datavalue_bitmap* obj=iot_datavalue_bitmap::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

//		json_object* ob=json_object_new_object();
//		if(!ob) return IOT_ERROR_NO_MEMORY;

//		json_object* val=json_object_new_int(obj->value());
//		if(!val) {
//			json_object_put(ob);
//			return IOT_ERROR_NO_MEMORY;
//		}
//		json_object_object_add(ob, "value", val);
//		dst=ob;
		assert(false);
		return 0;
	}
};

inline iot_datavalue_bitmap::iot_datavalue_bitmap(uint32_t max_code, bool is_memblock) : 
				iot_datavalue(&iot_datatype_metaclass_bitmap::object, is_memblock, sizeof(iot_datavalue_bitmap)) {
		if(max_code>=65535*32) max_code=65535*32-1;
		custom_u16=(max_code / 32)+1;
		memset(state, 0, custom_u16*sizeof(uint32_t));
		datasize=sizeof(*this)+sizeof(uint32_t)*custom_u16;
	}
inline iot_datavalue_bitmap::iot_datavalue_bitmap(uint32_t max_code, const uint32_t* statemap, uint16_t statemapsize, bool is_memblock) : 
				iot_datavalue(&iot_datatype_metaclass_bitmap::object, is_memblock, sizeof(iot_datavalue_bitmap)) {
		if(max_code>=65535*32) max_code=65535*32-1;
		custom_u16=(max_code / 32)+1;
		if(statemapsize<custom_u16) { //provided statemap must be of correct size or larger
			assert(false);
			if(statemapsize>0) memcpy(state, statemap, statemapsize*sizeof(uint32_t));
			memset(state+statemapsize, 0, (custom_u16-statemapsize)*sizeof(uint32_t));
		} else {
			assert(statemap!=NULL);
			memcpy(state, statemap, custom_u16*sizeof(uint32_t));
		}
		datasize=sizeof(*this)+sizeof(uint32_t)*custom_u16;
	}

inline const iot_datavalue_bitmap* iot_datavalue_bitmap::cast(const iot_datavalue *val) { //if val is not NULL and has correct class, casts pointer to this class
	if(val && val->get_metaclass()==&iot_datatype_metaclass_bitmap::object) return static_cast<const iot_datavalue_bitmap*>(val);
	return NULL;
}




///////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////  NUMERIC   /////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////

class iot_datavalue_numeric: public iot_datavalue {
	double value;

public:
	iot_datavalue_numeric(double val, bool memblock);

	static const iot_datavalue_numeric* cast(const iot_datavalue *val);

	double get_value(void) const {
		return value;
	}

	void set_value(double val) {
		value=val;
	}
	iot_datavalue_numeric& operator= (const iot_datavalue_numeric& op) {
		if(&op==this) return *this;
		value=op.value;
		return *this;
	}
};

class iot_datatype_metaclass_numeric : public iot_datatype_metaclass {
	iot_datatype_metaclass_numeric(void) : iot_datatype_metaclass(0, "numeric", IOT_VERSION_COMPOSE(0,0,1), false, true) {}
	PACKED(
		struct serialize_t {
			double value;
		}
	);

public:
	static iot_datatype_metaclass_numeric object;

	virtual char* sprint(const iot_datavalue* v, char* buf, size_t bufsize, int* doff=NULL) const override { //bufsize must include space for NUL
		assert(v && v->get_metaclass()==this);
		if(!bufsize) return buf;
		const iot_datavalue_numeric* val=static_cast<const iot_datavalue_numeric*>(v);
		int len=snprintf(buf, bufsize, "'%s' value=%.12g", type_name, val->get_value());
		if(doff) *doff += len>=int(bufsize) ? int(bufsize)-1 : len;
		return buf;
	}
	virtual bool check_eq(const iot_datavalue* v, const iot_datavalue *op) const override {
		assert(v && v->get_metaclass()==this && op && op->get_metaclass()==this);
		const iot_datavalue_numeric* val=static_cast<const iot_datavalue_numeric*>(v);
		const iot_datavalue_numeric* opval=static_cast<const iot_datavalue_numeric*>(op);

		return val->get_value()==opval->get_value();
	}
private:
	virtual int p_serialized_size(const iot_datavalue* obj0) const override {
		const iot_datavalue_numeric* obj=iot_datavalue_numeric::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		return sizeof(serialize_t);
	}
	virtual int p_serialize(const iot_datavalue* obj0, char* buf, size_t bufsize) const override {
		const iot_datavalue_numeric* obj=iot_datavalue_numeric::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
//		if(bufsize<sizeof(serialize_t)) return IOT_ERROR_NO_BUFSPACE;

//		serialize_t *s=(serialize_t*)buf;
//		s->value=repack_uint16(obj->value());
		assert(false);
		return 0;
	}
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_datavalue*& obj) const override {
		assert(false);
		return 0;
	}
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_datavalue*& obj) const override {
		assert(false);
		return 0;
	}
	virtual int p_to_json(const iot_datavalue* obj0, json_object* &dst) const override {
		const iot_datavalue_numeric* obj=iot_datavalue_numeric::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

//		json_object* ob=json_object_new_object();
//		if(!ob) return IOT_ERROR_NO_MEMORY;

//		json_object* val=json_object_new_int(obj->value());
//		if(!val) {
//			json_object_put(ob);
//			return IOT_ERROR_NO_MEMORY;
//		}
//		json_object_object_add(ob, "value", val);
//		dst=ob;
		assert(false);
		return 0;
	}
};

inline iot_datavalue_numeric::iot_datavalue_numeric(double val, bool is_memblock) : 
				iot_datavalue(&iot_datatype_metaclass_numeric::object, is_memblock, sizeof(iot_datavalue_numeric)) {
		value=val;
	}

inline const iot_datavalue_numeric* iot_datavalue_numeric::cast(const iot_datavalue *val) { //if val is not NULL and has correct class, casts pointer to this class
	if(val && val->get_metaclass()==&iot_datatype_metaclass_numeric::object) return static_cast<const iot_datavalue_numeric*>(val);
	return NULL;
}




/*




typedef iot_datatype_id_t iot_valuetype_id_t;	//type for class ID of value for node input/output. MUST HAVE 0 in lower bit





//Build-in value classes
#define IOT_VALUECLASSID_NODEERRORSTATE	(1 << 1)			//bitmap of error states of node

#define IOT_VALUECLASSID_BOOLEAN		(2 << 1)			//
#define IOT_VALUECLASSID_NUMBER			(3 << 1)			//fractional number

#define IOT_VALUECLASSID_BITMAP		(10 << 1)			//bitmap of keys which are currently down


class iot_datatype_base {
protected:
	const iot_datatype_id_t classid;
	uint32_t datasize:24, //size of whole value, including sizeof base class
		is_memblock:1; //flag that this object was allocated as memblock and thus release() and incref() will call corresponding memblock methods. otherwise static
	const uint32_t fixed:1, //flag that ALL objects of this type have equal datasize
		fixedvals:1;//flag that ALL possible values of data type are precreated statically and thus no memory allocation required
	uint32_t custom_data:5; //unused by this class bits which can be used by derived classes

	iot_datatype_base(iot_datatype_id_t classid, bool ismsg, bool memblock, bool is_fixed, bool is_fixedvals)
			: classid(classid), is_memblock(memblock), fixed(is_fixed), fixedvals(is_fixedvals), custom_data(0) {
		if(ismsg != (classid & 1)) {
			classid=0;
			assert(false);
		}
		if(is_fixedvals) {
			assert(!memblock);
		}
		datasize=sizeof(*this);
	}
	constexpr iot_datatype_base(iot_datatype_id_t classid, uint32_t datasize, bool ismsg, bool is_fixed, bool is_fixedvals, uint32_t custom_data=0)
			: classid(classid), datasize(datasize), is_memblock(false), fixed(is_fixed), fixedvals(is_fixedvals), custom_data(custom_data) {
	}

public:
	constexpr uint32_t get_size(void) const {
		return datasize;
	}
	constexpr bool is_msg(void) const {
		return (classid & 1) ? true : false;
	}
	constexpr bool is_fixed(void) const {
		return fixed;
	}
	constexpr bool is_fixedvals(void) const {
		return fixedvals;
	}

	bool operator==(const iot_datatype_base &op) const {
		if(&op==this) return true; //same object
		if(classid!=op.classid) return false;
		return check_eq(&op);
	}
	bool operator!=(const iot_datatype_base &op) const {
		return !(*this==op);
	}

	void release(void) const {
		if(!is_memblock) return;
		assert(!fixedvals);
		iot_release_memblock(const_cast<iot_datatype_base*>(this));
	}
	bool incref(void) const {
		if(!is_memblock) return true;
		assert(!fixedvals);
		return iot_incref_memblock(const_cast<iot_datatype_base*>(this));
	}

	//must copy data value/msg into specified buf. bufsize must be at least get_size() bytes. memblock tells if buf was allocated as memblock
	//returns NULL if bufsize is too small OR current object has is_fixedvals property (so cannot be copied by content, only by address)
	iot_datatype_base* copyTo(void* buf, uint32_t bufsize, bool memblock=true) const {
		if(is_fixedvals() || bufsize<get_size()) return NULL;
		iot_datatype_base* dst=(iot_datatype_base*)buf;
		memcpy(dst, this, get_size());
		dst->is_memblock=memblock ? 1 : 0;
		return dst;
	}
//required virtual functions to implement in derived classes

	virtual char* sprint(char* buf, size_t bufsize) const = 0;
	virtual const char* type_name(void) const = 0; //must return short abbreviation of type name

private:
	virtual bool check_eq(const iot_datatype_base *op) const = 0;
};


class iot_msgtype_BASE : public iot_datatype_base {
protected:
	iot_msgtype_BASE(iot_msgtype_id_t classid, bool memblock, bool is_fixed, bool is_fixedvals) : iot_datatype_base(classid, true, memblock, is_fixed, is_fixedvals) {
		datasize=sizeof(*this);
	}
	constexpr iot_msgtype_BASE(bool isconstexpr, iot_msgtype_id_t classid, uint32_t datasize, bool is_fixed, bool is_fixedvals, uint32_t custom_data=0) : iot_datatype_base(classid, datasize, true, is_fixed, is_fixedvals, custom_data) {
	}
public:
	constexpr iot_msgtype_id_t get_classid(void) const {
		return classid;
	}

	virtual char* sprint(char* buf, size_t bufsize) const override { //bufsize must include space for NUL
		//default realization just prints type and binary content
		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, "'%s' msg [",type_name());
		if(len>=int(bufsize)) return buf; //buffer is full
		int off=len;
		bufsize-=len;
		for(size_t i=sizeof(*this);i<datasize;i++) {
			len=snprintf(buf+off, bufsize, " %02x", unsigned(reinterpret_cast<const unsigned char*>(this)[i]));
			if(len>=int(bufsize)) return buf; //buffer is full
			off+=len;
			bufsize-=len;
		}
		snprintf(buf+off, bufsize, " ]");
		return buf;
	}

};


class iot_valuetype_BASE : public iot_datatype_base {
protected:
	iot_valuetype_BASE(iot_valuetype_id_t classid, bool memblock, bool is_fixed, bool is_fixedvals) : iot_datatype_base(classid, false, memblock, is_fixed, is_fixedvals) {
		datasize=sizeof(*this);
	}
	constexpr iot_valuetype_BASE(bool isconstexpr, iot_valuetype_id_t classid, uint32_t datasize, bool is_fixed, bool is_fixedvals, uint32_t custom_data=0) : iot_datatype_base(classid, datasize, false, is_fixed, is_fixedvals, custom_data) {
	}
public:
	constexpr iot_valuetype_id_t get_classid(void) const {
		return classid;
	}

	virtual char* sprint(char* buf, size_t bufsize) const override { //bufsize must include space for NUL
		//default realization just prints type and binary content
		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, "'%s' value [",type_name());
		if(len>=int(bufsize)) return buf; //buffer is full
		int off=len;
		bufsize-=len;
		for(size_t i=sizeof(*this);i<datasize;i++) {
			len=snprintf(buf+off, bufsize, " %02x", unsigned(reinterpret_cast<const unsigned char*>(this)[i]));
			if(len>=int(bufsize)) return buf; //buffer is full
			off+=len;
			bufsize-=len;
		}
		snprintf(buf+off, bufsize, " ]");
		return buf;
	}
};



//////////////BUILD-IN VALUE CLASSES


class iot_valuetype_nodeerrorstate: public iot_valuetype_BASE {
	uint32_t state; //bitmap of enabled error states

public:
	enum : uint32_t {
		IOT_NODEERRORSTATE_NOINSTANCE=1,		//module instance still not created
		IOT_NODEERRORSTATE_NODEVICE=2,			//required device(s) not connected
	};

	static const iot_valuetype_nodeerrorstate const_noinst;

	iot_valuetype_nodeerrorstate(bool memblock=true) : iot_valuetype_BASE(IOT_VALUECLASSID_NODEERRORSTATE, memblock, true, false) {
		datasize=sizeof(*this);
		state=0;
	}
	constexpr iot_valuetype_nodeerrorstate(uint32_t st) : iot_valuetype_BASE(true, IOT_VALUECLASSID_NODEERRORSTATE, sizeof(iot_valuetype_nodeerrorstate), true, false), state(st) {
	}
//	iot_valuetype_nodeerrorstate& operator=(uint32_t st) {
//		state=st;
//		return *this;
//	}
	constexpr explicit operator bool(void) const {
		return state!=0;
	}
	constexpr bool operator!(void) const {
		return state==0;
	}
	iot_valuetype_nodeerrorstate& set_noinstance(void) {
		state |= IOT_NODEERRORSTATE_NOINSTANCE;
		state &= ~(IOT_NODEERRORSTATE_NODEVICE);
		return *this;
	}
	virtual const char* type_name(void) const { //must return short abbreviation of type name
		return "NodeErrorState";
	}
private:
	virtual bool check_eq(const iot_datatype_base *op) const override {
		const iot_valuetype_nodeerrorstate* opc=static_cast<const iot_valuetype_nodeerrorstate*>(op);
		return state==opc->state;
	}
};


class iot_valuetype_boolean: public iot_valuetype_BASE {
	//forbid to create class objects other than static constants by making constructor private
	constexpr iot_valuetype_boolean(bool val) : iot_valuetype_BASE(true, IOT_VALUECLASSID_BOOLEAN, sizeof(iot_valuetype_boolean), true, true, val ? 1 : 0) {}
public:
	static const iot_valuetype_boolean const_true;
	static const iot_valuetype_boolean const_false;

	iot_valuetype_boolean(const iot_valuetype_boolean&) = delete; //forbid implicit copy (and move) constructor

//	iot_valuetype_boolean& operator=(uint32_t st) {
//		state=st;
//		return *this;
//	}
	constexpr explicit operator bool(void) const {
		return custom_data==1;
	}
	constexpr bool operator!(void) const {
		return custom_data==0;
	}
	constexpr bool value(void) const {
		return custom_data==1;
	}
	static const iot_valuetype_boolean* cast(const iot_valuetype_BASE*val) { //if val is not NULL and has correct class, casts pointer to this class
		if(val && val->get_classid()==IOT_VALUECLASSID_BOOLEAN) return static_cast<const iot_valuetype_boolean*>(val);
		return NULL;
	}
	virtual const char* type_name(void) const { //must return short abbreviation of type name
		return "Boolean";
	}
	virtual char* sprint(char* buf, size_t bufsize) const override { //bufsize must include space for NUL
		if(!bufsize) return buf;
		snprintf(buf, bufsize, "'%s' %s",iot_valuetype_boolean::type_name(), custom_data==1 ? "TRUE" : "FALSE");
		return buf;
	}
private:
	virtual bool check_eq(const iot_datatype_base *op) const override {
		const iot_valuetype_boolean* opc=static_cast<const iot_valuetype_boolean*>(op);
		return custom_data==opc->custom_data;
	}
};


#ifdef __linux__
	#include <linux/input-event-codes.h>
#endif

class iot_valuetype_bitmap: public iot_valuetype_BASE { //DYNAMICALL SIZED OBJECT!!! MEMORY FOR THIS OBJECT MUST BE ALLOCATED EXPLICITELY after 
															//call to iot_valuetype_bitmap::calc_datasize. Constructor  on allocated memory called
															//like 'new(memptr) iot_valuetype_bitmap(arguments)'
	uint16_t statesize; //number of items in state
	uint32_t state[]; //bitmap of currently depressed keys

public:
	iot_valuetype_bitmap(uint32_t max_code, bool memblock=true) : iot_valuetype_BASE(IOT_VALUECLASSID_BITMAP, memblock, false, false) {
		if(max_code>=65535*32) max_code=65535*32-1;
		statesize=(max_code / 32)+1;
		memset(state, 0, statesize*sizeof(uint32_t));
		datasize=sizeof(*this)+sizeof(uint32_t)*statesize;
	}
	iot_valuetype_bitmap(uint32_t max_code, const uint32_t* statemap, uint8_t statemapsize, bool memblock=true) : iot_valuetype_BASE(IOT_VALUECLASSID_BITMAP, memblock, false, false) {
		if(max_code>=65535*32) max_code=65535*32-1;
		statesize=(max_code / 32)+1;
		if(statemapsize<statesize) { //provided statemap must be of correct size or larger
			assert(false);
			if(statemapsize>0) memcpy(state, statemap, statemapsize*sizeof(uint32_t));
			memset(state+statemapsize, 0, (statesize-statemapsize)*sizeof(uint32_t));
		} else {
			assert(statemap!=NULL);
			memcpy(state, statemap, statesize*sizeof(uint32_t));
		}
		datasize=sizeof(*this)+sizeof(uint32_t)*statesize;
	}

	static size_t calc_datasize(uint32_t max_code) { //allows to determine necessary memory for object to hold state map with specific max key code
		//returns 0 on unallowed value
		if(max_code>=65535*32) max_code=65535*32-1;
		return sizeof(iot_valuetype_bitmap)+sizeof(uint32_t)*((max_code / 32)+1);
	}
	static const iot_valuetype_bitmap* cast(const iot_valuetype_BASE*val) { //if val is not NULL and has correct class, casts pointer to this class
		if(val && val->get_classid()==IOT_VALUECLASSID_BITMAP) return static_cast<const iot_valuetype_bitmap*>(val);
		return NULL;
	}
	constexpr static uint32_t get_maxkeycode(void) { //in cases when bitmap represents key states, this method can be used to obtain maximum possible key code to calculate maximum bitmap size
#ifdef __linux__
		return KEY_MAX;
#else
		return 255;  //TODO for other OSes
#endif
	}

	uint32_t test_code(uint32_t code) const {
		if(code>=statesize*32) return 0;
		return bitmap32_test_bit(state, code);
	}

	void set_code(uint32_t code) {
		if(code>=statesize*32) {
			assert(false);
			return;
		}
		bitmap32_set_bit(state, code);
	}
	void clear_code(uint32_t code) {
		if(code>=statesize*32) {
			assert(false);
			return;
		}
		bitmap32_clear_bit(state, code);
	}
	iot_valuetype_bitmap& operator= (const iot_valuetype_bitmap& op) {
		if(&op==this) return *this;
		if(statesize<=op.statesize) {
			memcpy(state, op.state, statesize*sizeof(uint32_t));
		} else {
			memcpy(state, op.state, op.statesize*sizeof(uint32_t));
			memset(state+op.statesize, 0, (statesize-op.statesize)*sizeof(uint32_t));
		}
		return *this;
	}
	iot_valuetype_bitmap& operator|= (const iot_valuetype_bitmap& op) {
		if(&op==this) return *this;
		if(statesize<=op.statesize) {
			for(uint32_t i=0;i<statesize;i++) state[i]|=op.state[i];
		} else {
			for(uint32_t i=0;i<op.statesize;i++) state[i]|=op.state[i];
		}
		return *this;
	}
	explicit operator bool(void) const {
		for(uint32_t i=0;i<statesize;i++) if(state[i]) return true;
		return false;
	}
	virtual const char* type_name(void) const { //must return short abbreviation of type name
		return "Bitmap";
	}
private:
	virtual bool check_eq(const iot_datatype_base *op) const override {
		const iot_valuetype_bitmap* opc=static_cast<const iot_valuetype_bitmap*>(op);
		return statesize==opc->statesize && memcmp(state, opc->state, statesize*sizeof(state[0]))==0;
	}
};
*/



/*
//always set by core
#define IOT_STATE_ERROR_INSTANCE_UNREACHABLE	1	//bound instance from another host is unreachable due to host connection problems (actual for activators and actors)
#define IOT_STATE_ERROR_INSTANCE_INVALID		2	//bound instance cannot be instantiated or got critical bug (actual for activators and actors)

//can be set by core or instance
#define IOT_STATE_ERROR_NO_DEVICE				64	//no mandatory device connected


struct iot_srcstate_t {
	uint32_t			size;		//size of whole data
	iot_state_error_t	error;		//non-zero value indicates error code for error state of module, 'value' and 'msgpending' can be meaningful or not depending on error code
	struct {
		uint32_t		offset;		//address of value as offset from start of valuedata item.
		uint32_t		len;		//length of value. zero if value is NOT VALID (not configured or instance has error which invalidates it)
	} value[IOT_CONFIG_MAX_NODE_VALUEOUTPUTS];
	char valuedata[];
};
*/
//maximum number of custom data classes for event source state
//#define IOT_SOURCE_STATE_MAX_CLASSES 5




//ECB_EXTERN_C_BEG
//
//tries to find specified class of data inside state custom block
//Returns 0 on success and fills startoffset to point to start of data. This offset can then be passed to CLASS::get_size and/or CLASS::extract methods to get actual data
//Possible errors:
//IOT_ERROR_NOT_FOUND - class of data not present in state
//int iot_find_srcstate_datatype(iot_srcstate_t* state,iot_state_classid clsid, void** startoffset);

//ECB_EXTERN_C_END

#endif //IOT_VALUECLASSES_H