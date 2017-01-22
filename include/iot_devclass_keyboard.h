#ifndef IOT_DEVCLASS_KEYBOARD_H
#define IOT_DEVCLASS_KEYBOARD_H
//Contains interface to communicate using IOT_DEVCLASSID_KEYBOARD class

#include<stdint.h>
//#include<time.h>

#include<ecb.h>

#include <linux/input.h>




struct iot_deviface_class_keyboard_msg_t {
	enum req_t { //commands (requests) which driver can execute
		REQ_GET_STATE, //request to post EVENT_INIT_STATE with status of all keys. no 'data' is used
	};
	enum event_t { //events (or replies) which driver can send to client
		EVENT_SET_STATE, //reply to REQ_GET_STATE request. provides current state of all keys. data.init_state is used for data struct
		EVENT_KEYDOWN, //key was down
		EVENT_KEYUP, //key was up
		EVENT_KEYREPEAT, //key was autorepeated
	};

	union { //field is determined by usage context
		req_t req_code;
		event_t event_code;
	};
	union { //field is determined by req_code or event_code
		struct state_t {
			uint8_t shift_state:1,
				ctrl_state:1,
				alt_state:1,
				meta_state:1;
			uint32_t map[(KEY_CNT+31)/32]; //map of depressed keys
		} state;
		struct keyact_t { //state includes result of key action
			uint16_t key:15,
				was_drop:1; //flag that some messages were dropped before this one
			struct state_t state;
		} keyevent;
	} data;
};

iot_devifacecls_config_t iot_deviface_classcfg_keyboard={
	.classid = IOT_DEVCLASSID_KEYBOARD,
	.d2c_maxmsgsize = 128,
	.c2d_maxmsgsize = 32
};


#endif //IOT_DEVCLASS_KEYBOARD_H
