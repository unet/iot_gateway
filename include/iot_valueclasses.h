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

#endif


typedef iot_dataclass_id_t iot_valueclass_id_t;	//type for class ID of value for node input/output. MUST HAVE 0 in lower bit



//Build-in value classes
#define IOT_VALUECLASSID_NODEERRORSTATE	(1 << 1)			//bitmap of error states of node

#define IOT_VALUECLASSID_BOOLEAN		(2 << 1)			//
#define IOT_VALUECLASSID_INTEGER		(3 << 1)			//integer number
#define IOT_VALUECLASSID_NUMBER			(4 << 1)			//fractional number

#define IOT_VALUECLASSID_KBDSTATE		(10 << 1)			//bitmap of keys which are currently down


#ifdef __linux__
	#include <linux/input-event-codes.h>
	#define IOT_KEYBOARD_MAX_KEYCODE KEY_MAX
#else
	#define IOT_KEYBOARD_MAX_KEYCODE 255
#endif
class iot_msgclass_BASE;

class iot_valueclass_BASE {
	iot_valueclass_id_t classid;
protected:
	uint32_t datasize; //size of whole value, including sizeof base class

	iot_valueclass_BASE(iot_valueclass_id_t classid) : classid((classid & 1) ? 0 : classid) {
		datasize=sizeof(*this);
	}
public:
	iot_valueclass_id_t get_classid(void) const {
		return classid;
	}
	uint32_t get_size(void) const {
		return datasize;
	}
	bool operator==(const iot_valueclass_BASE &op) const {
		if(classid!=op.classid) return false;
		return check_eq(&op);
	}
private:
	//required virtual functions to implement in derived classes
	virtual bool check_eq(const iot_valueclass_BASE *op) const = 0;
};

class iot_valueclass_nodeerrorstate: public iot_valueclass_BASE {
	uint32_t state; //bitmap of enabled error states

public:
	enum : uint32_t {
		IOT_NODEERRORSTATE_NOINSTANCE=1,		//module instance still not created
		IOT_NODEERRORSTATE_NODEVICE=2			//required device(s) not connected
	};

	iot_valueclass_nodeerrorstate(void) : iot_valueclass_BASE(IOT_VALUECLASSID_NODEERRORSTATE) {
		datasize=sizeof(*this);
		state=0;
	}
	iot_valueclass_nodeerrorstate& operator=(uint32_t st) {
		state=st;
		return *this;
	}
	explicit operator bool(void) const {
		return state!=0;
	}
	bool operator!(void) const {
		return state==0;
	}
private:
	virtual bool check_eq(const iot_valueclass_BASE *op) const override {
		const iot_valueclass_nodeerrorstate* opc=static_cast<const iot_valueclass_nodeerrorstate*>(op);
		return state==opc->state;
	}
};



class iot_valueclass_kbdstate: public iot_valueclass_BASE {
	uint8_t statesize; //number of items in state
	uint32_t state[]; //map of depressed keys for all consumer events. already includes result of current key action

public:
	iot_valueclass_kbdstate(uint16_t max_keycode=IOT_KEYBOARD_MAX_KEYCODE) : iot_valueclass_BASE(IOT_VALUECLASSID_KBDSTATE) {
		statesize=(max_keycode / 32)+1;
		memset(state, 0, statesize*sizeof(uint32_t));
		datasize=sizeof(*this)+sizeof(uint32_t)*statesize;
	}
	iot_valueclass_kbdstate(uint16_t max_keycode, const uint32_t* statemap, uint8_t statemapsize) : iot_valueclass_BASE(IOT_VALUECLASSID_KBDSTATE) {
		if(max_keycode>IOT_KEYBOARD_MAX_KEYCODE) max_keycode=IOT_KEYBOARD_MAX_KEYCODE;
		assert(max_keycode>0);
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
private:
	virtual bool check_eq(const iot_valueclass_BASE *op) const override {
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