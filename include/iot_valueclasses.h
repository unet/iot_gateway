#ifndef IOT_VALUECLASSES_H
#define IOT_VALUECLASSES_H
//declarations for built-in value types used for node inputs and outputs


//#include <stdlib.h>
//#include <stdint.h>

#ifndef DAEMON_KERNEL
////////////////////////////////////////////////////////////////////
///////////////////////////Specific declarations for external modules
////////////////////////////////////////////////////////////////////
//macro for generating ID of custom module specific class of state data
//#define IOT_VALUECLASS_CUSTOM(module_id, index) (((module_id)<<8)+((index) & 0x7f)*2)

//#define IOT_MSGCLASS_CUSTOM(module_id, index) (((module_id)<<8)+((index) & 0x7f)*2+1)

#endif
/*
class iot_datatype_metaclass {
	iot_type_id_t datatype_id;
	uint32_t ver; //version of realization of metaclass and all its child classes
	const char *type_name;
	const char *parentlib;

	PACKED(
		struct serialize_base_t {
			iot_type_id_t datatype_id;
		}
	);
public:
	iot_datatype_metaclass* next, *prev; //non-NULL prev means that class is registered and both next and prev are used. otherwise only next is used
													//for position in pending registration list

protected:
	iot_datatype_metaclass(iot_type_id_t id, const char* type, uint32_t ver, const char* parentlib=IOT_CURLIBRARY); //id must be zero for non-builtin types. type cannot be NULL

public:
	iot_datatype_metaclass(const iot_datatype_metaclass&) = delete; //block copy-construtors and default assignments

	iot_type_id_t get_id(void) const {
		return datatype_id;
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
	void set_id(iot_type_id_t id) {
		if(datatype_id>0 || !id) {
			assert(false);
			return;
		}
		datatype_id=id;
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

private:
	virtual int p_serialized_size(const iot_datavalue* obj) const = 0;
	virtual int p_serialize(const iot_datavalue* obj, char* buf, size_t bufsize) const = 0; //returns error code or 0 on success
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_datavalue*& obj) const = 0;
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_datavalue*& obj) const = 0;
	virtual int p_to_json(const iot_datavalue* obj, json_object* &dst) const = 0;
};
*/

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




/*
//always set by kernel
#define IOT_STATE_ERROR_INSTANCE_UNREACHABLE	1	//bound instance from another host is unreachable due to host connection problems (actual for activators and actors)
#define IOT_STATE_ERROR_INSTANCE_INVALID		2	//bound instance cannot be instantiated or got critical bug (actual for activators and actors)

//can be set by kernel or instance
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