#ifndef IOT_SRCSTATE_H
#define IOT_SRCSTATE_H
//Contains methods for extracting custom data classes from states of Source modules and also definitions of predefined classes

#include <stdlib.h>
#include <stdint.h>

//always set by kernel
#define IOT_STATE_ERROR_INSTANCE_UNREACHABLE	1	//bound instance from another host is unreachable due to host connection problems (actual for activators and actors)
#define IOT_STATE_ERROR_INSTANCE_INVALID		2	//bound instance cannot be instantiated or got critical bug (actual for activators and actors)

//can be set by kernel or instance
#define IOT_STATE_ERROR_NO_DEVICE				64	//no mandatory device connected (actual for event sources, actors and possible activators when device for them ever appears)


struct iot_srcstate_t {
	uint32_t			size;		//size of whole data
	iot_state_error_t	error;		//non-zero value indicates error code for error state of module, 'value' and 'msgpending' can be meaningful or not depending on error code
	struct {
		uint32_t		offset;		//address of value as offset from start of valuedata item.
		uint32_t		len;		//length of value. zero if value is NOT VALID (not configured or instance has error which invalidates it)
	} value[IOT_CONFIG_MAX_EVENTSOURCE_STATES];
	char valuedata[];
};

//maximum number of custom data classes for event source state
//#define IOT_SOURCE_STATE_MAX_CLASSES 5


//Build-in value classes
#define IOT_VALUECLASSID_BOOLEAN		1			//
#define IOT_VALUECLASSID_INTEGER		2			//integer number
#define IOT_VALUECLASSID_NUMBER			3			//fractional number

#define IOT_VALUECLASSID_KEYBOARD		10


//ECB_EXTERN_C_BEG
//
//tries to find specified class of data inside state custom block
//Returns 0 on success and fills startoffset to point to start of data. This offset can then be passed to CLASS::get_size and/or CLASS::extract methods to get actual data
//Possible errors:
//IOT_ERROR_NOT_FOUND - class of data not present in state
//int iot_find_srcstate_dataclass(iot_srcstate_t* state,iot_state_classid clsid, void** startoffset);

//ECB_EXTERN_C_END

#endif //IOT_SRCSTATE_H