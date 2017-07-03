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
#define IOT_VALUECLASS_CUSTOM(module_id, index) (((module_id)<<8)+((index) & 0x7f)*2)

#define IOT_MSGCLASS_CUSTOM(module_id, index) (((module_id)<<8)+((index) & 0x7f)*2+1)

#endif


typedef iot_dataclass_id_t iot_valueclass_id_t;	//type for class ID of value for node input/output. MUST HAVE 0 in lower bit



//Build-in value classes
#define IOT_VALUECLASSID_NODEERRORSTATE	(1 << 1)			//bitmap of error states of node

#define IOT_VALUECLASSID_BOOLEAN		(2 << 1)			//
#define IOT_VALUECLASSID_NUMBER			(3 << 1)			//fractional number

#define IOT_VALUECLASSID_KBDSTATE		(10 << 1)			//bitmap of keys which are currently down


#ifdef __linux__
	#include <linux/input-event-codes.h>
	#define IOT_KEYBOARD_MAX_KEYCODE KEY_MAX
#else
	#define IOT_KEYBOARD_MAX_KEYCODE 255
#endif

class iot_dataclass_base {
protected:
	const iot_dataclass_id_t classid;
	uint32_t datasize:24, //size of whole value, including sizeof base class
		is_memblock:1; //flag that this object was allocated as memblock and thus release() and incref() will call corresponding memblock methods. otherwise static
	const uint32_t fixed:1, //flag that ALL objects of this type have equal datasize
		fixedvals:1;//flag that ALL possible values of data type are precreated statically and thus no memory allocation required
	uint32_t custom_data:5; //unused by this class bits which can be used by derived classes

	iot_dataclass_base(iot_dataclass_id_t classid, bool ismsg, bool memblock, bool is_fixed, bool is_fixedvals)
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
	constexpr iot_dataclass_base(iot_dataclass_id_t classid, uint32_t datasize, bool ismsg, bool is_fixed, bool is_fixedvals, uint32_t custom_data=0)
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

	bool operator==(const iot_dataclass_base &op) const {
		if(&op==this) return true; //same object
		if(classid!=op.classid) return false;
		return check_eq(&op);
	}
	bool operator!=(const iot_dataclass_base &op) const {
		return !(*this==op);
	}

	void release(void) const {
		if(!is_memblock) return;
		assert(!fixedvals);
		iot_release_memblock(const_cast<iot_dataclass_base*>(this));
	}
	bool incref(void) const {
		if(!is_memblock) return true;
		assert(!fixedvals);
		return iot_incref_memblock(const_cast<iot_dataclass_base*>(this));
	}

	//must copy data value/msg into specified buf. bufsize must be at least get_size() bytes. memblock tells if buf was allocated as memblock
	//returns NULL if bufsize is too small OR current object has is_fixedvals property (so cannot be copied by content, only by address)
	iot_dataclass_base* copyTo(void* buf, uint32_t bufsize, bool memblock=true) const {
		if(is_fixedvals() || bufsize<get_size()) return NULL;
		iot_dataclass_base* dst=(iot_dataclass_base*)buf;
		memcpy(dst, this, get_size());
		dst->is_memblock=memblock ? 1 : 0;
		return dst;
	}
//required virtual functions to implement in derived classes

	virtual char* sprint(char* buf, size_t bufsize) const = 0;
	virtual const char* type_name(void) const = 0; //must return short abbreviation of type name

private:
	virtual bool check_eq(const iot_dataclass_base *op) const = 0;
};


class iot_msgclass_BASE : public iot_dataclass_base {
protected:
	iot_msgclass_BASE(iot_msgclass_id_t classid, bool memblock, bool is_fixed, bool is_fixedvals) : iot_dataclass_base(classid, true, memblock, is_fixed, is_fixedvals) {
		datasize=sizeof(*this);
	}
	constexpr iot_msgclass_BASE(bool isconstexpr, iot_msgclass_id_t classid, uint32_t datasize, bool is_fixed, bool is_fixedvals, uint32_t custom_data=0) : iot_dataclass_base(classid, datasize, true, is_fixed, is_fixedvals, custom_data) {
	}
public:
	constexpr iot_msgclass_id_t get_classid(void) const {
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


class iot_valueclass_BASE : public iot_dataclass_base {
protected:
	iot_valueclass_BASE(iot_valueclass_id_t classid, bool memblock, bool is_fixed, bool is_fixedvals) : iot_dataclass_base(classid, false, memblock, is_fixed, is_fixedvals) {
		datasize=sizeof(*this);
	}
	constexpr iot_valueclass_BASE(bool isconstexpr, iot_valueclass_id_t classid, uint32_t datasize, bool is_fixed, bool is_fixedvals, uint32_t custom_data=0) : iot_dataclass_base(classid, datasize, false, is_fixed, is_fixedvals, custom_data) {
	}
public:
	constexpr iot_valueclass_id_t get_classid(void) const {
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


class iot_valueclass_nodeerrorstate: public iot_valueclass_BASE {
	uint32_t state; //bitmap of enabled error states

public:
	enum : uint32_t {
		IOT_NODEERRORSTATE_NOINSTANCE=1,		//module instance still not created
		IOT_NODEERRORSTATE_NODEVICE=2,			//required device(s) not connected
	};

	static const iot_valueclass_nodeerrorstate const_noinst;

	iot_valueclass_nodeerrorstate(bool memblock=true) : iot_valueclass_BASE(IOT_VALUECLASSID_NODEERRORSTATE, memblock, true, false) {
		datasize=sizeof(*this);
		state=0;
	}
	constexpr iot_valueclass_nodeerrorstate(uint32_t st) : iot_valueclass_BASE(true, IOT_VALUECLASSID_NODEERRORSTATE, sizeof(iot_valueclass_nodeerrorstate), true, false), state(st) {
	}
//	iot_valueclass_nodeerrorstate& operator=(uint32_t st) {
//		state=st;
//		return *this;
//	}
	constexpr explicit operator bool(void) const {
		return state!=0;
	}
	constexpr bool operator!(void) const {
		return state==0;
	}
	iot_valueclass_nodeerrorstate& set_noinstance(void) {
		state |= IOT_NODEERRORSTATE_NOINSTANCE;
		state &= ~(IOT_NODEERRORSTATE_NODEVICE);
		return *this;
	}
	virtual const char* type_name(void) const { //must return short abbreviation of type name
		return "NodeErrorState";
	}
private:
	virtual bool check_eq(const iot_dataclass_base *op) const override {
		const iot_valueclass_nodeerrorstate* opc=static_cast<const iot_valueclass_nodeerrorstate*>(op);
		return state==opc->state;
	}
};


class iot_valueclass_boolean: public iot_valueclass_BASE {
	//forbid to create class objects other than static constants by making constructor private
	constexpr iot_valueclass_boolean(bool val) : iot_valueclass_BASE(true, IOT_VALUECLASSID_BOOLEAN, sizeof(iot_valueclass_boolean), true, true, val ? 1 : 0) {}
public:
	static const iot_valueclass_boolean const_true;
	static const iot_valueclass_boolean const_false;

	iot_valueclass_boolean(const iot_valueclass_boolean&) = delete; //forbid implicit copy (and move) constructor

//	iot_valueclass_boolean& operator=(uint32_t st) {
//		state=st;
//		return *this;
//	}
	constexpr explicit operator bool(void) const {
		return custom_data==1;
	}
	constexpr bool operator!(void) const {
		return custom_data==0;
	}
	virtual const char* type_name(void) const { //must return short abbreviation of type name
		return "Boolean";
	}
	virtual char* sprint(char* buf, size_t bufsize) const override { //bufsize must include space for NUL
		if(!bufsize) return buf;
		snprintf(buf, bufsize, "'%s' %s",iot_valueclass_boolean::type_name(), custom_data==1 ? "TRUE" : "FALSE");
		return buf;
	}
private:
	virtual bool check_eq(const iot_dataclass_base *op) const override {
		const iot_valueclass_boolean* opc=static_cast<const iot_valueclass_boolean*>(op);
		return custom_data==opc->custom_data;
	}
};




class iot_valueclass_kbdstate: public iot_valueclass_BASE { //DYNAMICALL SIZED OBJECT!!! MEMORY FOR THIS OBJECT MUST BE ALLOCATED EXPLICITELY after 
															//call to iot_valueclass_kbdstate::calc_datasize. Constructor  on allocated memory called
															//like 'new(memptr) iot_valueclass_kbdstate(arguments)'
	uint32_t statesize; //number of items in state
	uint32_t state[]; //bitmap of currently depressed keys

public:
	iot_valueclass_kbdstate(uint16_t max_keycode=IOT_KEYBOARD_MAX_KEYCODE, bool memblock=true) : iot_valueclass_BASE(IOT_VALUECLASSID_KBDSTATE, memblock, false, false) {
		if(max_keycode>IOT_KEYBOARD_MAX_KEYCODE) max_keycode=IOT_KEYBOARD_MAX_KEYCODE;
		statesize=(max_keycode / 32)+1;
		memset(state, 0, statesize*sizeof(uint32_t));
		datasize=sizeof(*this)+sizeof(uint32_t)*statesize;
	}
	iot_valueclass_kbdstate(uint16_t max_keycode, const uint32_t* statemap, uint8_t statemapsize, bool memblock=true) : iot_valueclass_BASE(IOT_VALUECLASSID_KBDSTATE, memblock, false, false) {
		if(max_keycode>IOT_KEYBOARD_MAX_KEYCODE) max_keycode=IOT_KEYBOARD_MAX_KEYCODE;
		statesize=(max_keycode / 32)+1;
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

	static size_t calc_datasize(uint16_t max_keycode) { //allows to determine necessary memory for object to hold state map with specific max key code
		//returns 0 on unallowed value
		if(max_keycode>IOT_KEYBOARD_MAX_KEYCODE) max_keycode=IOT_KEYBOARD_MAX_KEYCODE;
		if(!max_keycode) return 0;
		return sizeof(iot_valueclass_kbdstate)+sizeof(uint32_t)*((max_keycode / 32)+1);
	}
	static const iot_valueclass_kbdstate* cast(const iot_valueclass_BASE*val) { //if val is not NULL and has correct class, casts pointer to this class
		if(val && val->get_classid()==IOT_VALUECLASSID_KBDSTATE) return static_cast<const iot_valueclass_kbdstate*>(val);
		return NULL;
	}

	uint32_t test_key(uint16_t keycode) const {
		if(keycode>=statesize*32) return 0;
		return bitmap32_test_bit(state, keycode);
	}

	void set_key(uint16_t keycode) {
		if(keycode>=statesize*32) {
			assert(false);
			return;
		}
		bitmap32_set_bit(state, keycode);
	}
	void clear_key(uint16_t keycode) {
		if(keycode>=statesize*32) {
			assert(false);
			return;
		}
		bitmap32_clear_bit(state, keycode);
	}
	iot_valueclass_kbdstate& operator= (const iot_valueclass_kbdstate& op) {
		if(&op==this) return *this;
		if(statesize<=op.statesize) {
			memcpy(state, op.state, statesize*sizeof(uint32_t));
		} else {
			memcpy(state, op.state, op.statesize*sizeof(uint32_t));
			memset(state+op.statesize, 0, (statesize-op.statesize)*sizeof(uint32_t));
		}
		return *this;
	}
	iot_valueclass_kbdstate& operator|= (const iot_valueclass_kbdstate& op) {
		if(&op==this) return *this;
		if(statesize<=op.statesize) {
			for(uint8_t i=0;i<statesize;i++) state[i]|=op.state[i];
		} else {
			for(uint8_t i=0;i<op.statesize;i++) state[i]|=op.state[i];
		}
		return *this;
	}
	explicit operator bool(void) const {
		for(uint8_t i=0;i<statesize;i++) if(state[i]) return true;
		return false;
	}
	virtual const char* type_name(void) const { //must return short abbreviation of type name
		return "KeyboardState";
	}
private:
	virtual bool check_eq(const iot_dataclass_base *op) const override {
		const iot_valueclass_kbdstate* opc=static_cast<const iot_valueclass_kbdstate*>(op);
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
//int iot_find_srcstate_dataclass(iot_srcstate_t* state,iot_state_classid clsid, void** startoffset);

//ECB_EXTERN_C_END

#endif //IOT_VALUECLASSES_H