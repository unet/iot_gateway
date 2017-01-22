#ifndef IOT_SRCSTATE_H
#define IOT_SRCSTATE_H
//Contains methods for extracting custom data classes from states of Source modules and also definitions of predefined classes

#include <stdlib.h>
#include <stdint.h>

#include <ecb.h>


//macro for generating ID of custom module specific class of state data
#define IOT_SRCSTATE_CUSTOMCLASSID(module_id, index) (((module_id)<<8)+(index))

typedef uint32_t iot_state_classid;

typedef uint32_t iot_srcstate_error_t;

//no device connected
#define IOT_SRCSTATE_ERROR_NO_DEVICE	1


typedef struct {
	union {
		uint64_t	integer;
		double		floating;
	} value;
	iot_srcstate_error_t	error;		//non-zero value indicates error code for error state of module, 'value' and 'msgpending' can be meaningful or not depending on error code
	uint8_t					msgpending;	//flag that new message is pending
	uint16_t				custom_len;	//size of custom data that follows
	char					custom[];	//start of custom data which can be inspected using event source interface method parse_state_byclassid
} iot_srcstate_t;

//maximum number of custom data classes for event source state
#define IOT_SOURCE_STATE_MAX_CLASSES 5


#define IOT_SRCSTATE_CLASSID_ERROR		1
#define IOT_SRCSTATE_CLASSID_BOOLEAN	2
#define IOT_SRCSTATE_CLASSID_INTEGER	3
#define IOT_SRCSTATE_CLASSID_NUMBER		4

#define IOT_SRCSTATE_CLASSID_BUTTONS	10
#define IOT_SRCSTATE_CLASSID_KEYBOARD	11


ECB_EXTERN_C_BEG

//tries to find specified class of data inside state custom block
//Returns 0 on success and fills startoffset to point to start of data. This offset can then be passed to CLASS::get_size and/or CLASS::extract methods to get actual data
//Possible errors:
//IOT_ERROR_NOT_FOUND - class of data not present in state
int iot_find_srcstate_dataclass(iot_srcstate_t* state,iot_state_classid clsid, void** startoffset);

ECB_EXTERN_C_END

#endif //IOT_SRCSTATE_H