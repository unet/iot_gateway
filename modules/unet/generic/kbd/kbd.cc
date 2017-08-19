#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>
#include<stdlib.h>
#include<unistd.h>
#include<assert.h>
#include<new>


#include <linux/input.h>

#include "uv.h"
#include "iot_utils.h"
#include "iot_error.h"


//#define IOT_VENDOR unet
//#define IOT_BUNDLE generic__kbd

#include "iot_module.h"

#include "iot_devclass_keyboard.h"
#include "iot_devclass_activatable.h"


/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////kbd:eventsrc (pure event source) node module
/////////////////////////////////////////////////////////////////////////////////
#define EVENTSRC_MAX_DEVICES 3

struct eventsrc_instance : public iot_node_base {
	uint32_t node_id;
	uv_timer_t timer_watcher={};
	struct {
		const iot_conn_clientview *conn;
		uint16_t maxkeycode;
		uint32_t keystate[iot_valuetype_bitmap::get_maxkeycode()/32+1]; //current state of keys of device
	} device[EVENTSRC_MAX_DEVICES]={}; //per device connection state


/////////////static fields/methods for module instances management
	static int init_instance(iot_node_base**instance, uv_thread_t thread, uint32_t node_id, json_object *json_cfg) {
		eventsrc_instance *inst=new eventsrc_instance(thread, node_id);
		*instance=inst;

		return 0;
	}
	static int deinit_instance(iot_node_base* instance) {
		eventsrc_instance *inst=static_cast<eventsrc_instance*>(instance);
		delete inst;
		return 0;
	}
private:
	eventsrc_instance(uv_thread_t thread, uint32_t node_id) : iot_node_base(thread), node_id(node_id) {}
	virtual ~eventsrc_instance(void) {}

//methods from iot_module_instance_base
	//Called to start work of previously inited instance.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, node_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(void) override {
		assert(uv_thread_self()==thread);

		return 0;
	}

	//called to stop work of started instance. call can be followed by deinit or started again (if stop was manual, by user)
	//Return values:
	//0 - driver successfully stopped and can be deinited or restarted
	//IOT_ERROR_TRY_AGAIN - driver requires some time (async operation) to stop gracefully. kapi_self_abort() will be called to notify kernel when stop is finished.
	//						anyway second stop() call must free all resources correctly, may be in a hard way. otherwise module will be blocked and left in hang state (deinit
	//						cannot be called until stop reports OK)
	//any other error is treated as critical bug and driver is blocked for further starts. deinit won't be called for such instance. instance is put into hang state
	virtual int stop(void) override {
		assert(uv_thread_self()==thread);

		return 0;
	}

//methods from iot_node_base
	virtual int device_attached(const iot_conn_clientview* conn) override {
		assert(uv_thread_self()==thread);
		assert(conn->index<EVENTSRC_MAX_DEVICES);
		assert(device[conn->index].conn==NULL);

		device[conn->index].conn=conn;
		memset(device[conn->index].keystate, 0, sizeof(device[conn->index].keystate));
		iot_deviface__keyboard_CL iface(conn);
		device[conn->index].maxkeycode=iface.get_max_keycode();
		return 0;
	}
	virtual int device_detached(const iot_conn_clientview* conn) override {
		assert(uv_thread_self()==thread);
		assert(conn->index<EVENTSRC_MAX_DEVICES);
		assert(device[conn->index].conn!=NULL);

		device[conn->index].conn=NULL;
		update_outputs();
		return 0;
	}
	virtual int device_action(const iot_conn_clientview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) override {
		assert(uv_thread_self()==thread);
		assert(conn->index<EVENTSRC_MAX_DEVICES);
		assert(device[conn->index].conn==conn);

//		int err;
		if(action_code==IOT_DEVCONN_ACTION_FULLREQUEST) {//new message arrived
			iot_deviface__keyboard_CL iface(conn);
			const iot_deviface__keyboard_CL::msg* msg=iface.parse_event(data, data_size);
			if(!msg) return 0;

			switch(msg->event_code) {
				case iface.EVENT_KEYDOWN:
					kapi_outlog_info("GOT keyboard DOWN for key %d from device index %d", (int)msg->key, int(conn->index));
//					if(msg->key==KEY_ESC) {
//						kapi_outlog_info("Requesting state");
//						err=iface.request_state();
//						assert(err==0);
//					}
					break;
				case iface.EVENT_KEYUP:
					kapi_outlog_info("GOT keyboard UP for key %d from device index %d", (int)msg->key, int(conn->index));
					break;
				case iface.EVENT_KEYREPEAT:
					kapi_outlog_info("GOT keyboard REPEAT for key %d from device index %d", (int)msg->key, int(conn->index));
					break;
				case iface.EVENT_SET_STATE:
					kapi_outlog_info("GOT NEW STATE, datasize=%u, statesize=%u from device index %d", data_size, (unsigned)(msg->statesize), int(conn->index));
					break;
				default:
					kapi_outlog_info("Got unknown event %d from device index %d, node_id=%u", int(msg->event_code), int(conn->index), node_id);
					return 0;
			}
			//update key state of device
			memcpy(device[conn->index].keystate, msg->state, msg->statesize*sizeof(uint32_t));
			update_outputs();
			return 0;
		}
		kapi_outlog_info("Device action, node_id=%u, act code %u, datasize %u from device index %d", node_id, unsigned(action_code), data_size, int(conn->index));
		return 0;
	}

//own methods
	int update_outputs(void) { //set current common key state on output
		uint16_t maxkeycode=0;
		uint32_t keystate[iot_valuetype_bitmap::get_maxkeycode()/32+1]; //current state of keys of device
		memset(keystate, 0, sizeof(keystate));

		for(int i=0; i<EVENTSRC_MAX_DEVICES; i++) {
			if(!device[i].conn) continue;
			if(device[i].maxkeycode>maxkeycode) maxkeycode=device[i].maxkeycode;
			unsigned n=device[i].maxkeycode/32+1;
			for (unsigned j=0; j<n; j++) keystate[j]|=device[i].keystate[j];
		}

		char valbuf[iot_valuetype_bitmap::calc_datasize(maxkeycode)];
		uint8_t outn=0;
		const iot_valuetype_BASE* outv=new(valbuf) iot_valuetype_bitmap(maxkeycode, keystate, maxkeycode/32+1, false);
		int err=kapi_update_outputs(NULL, 1, &outn, &outv);
		if(err) {
			kapi_outlog_error("Cannot update output value for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
		}
		return err;
	}
};

//keys_instance* keys_instance::instances_head=NULL;


//static iot_module_spec_t drvmodspec={
//	.vendor=ECB_STRINGIFY(IOT_VENDOR),
//	.bundle=ECB_STRINGIFY(IOT_BUNDLE),
//	.module=ECB_STRINGIFY(DRVNAME),
//};

static const iot_deviface_params_keyboard kbd_filter_pconly(1);

static const iot_deviface_params* eventsrc_devifaces[]={
	&kbd_filter_pconly
};


static iot_iface_node_t eventsrc_iface_node = {
	.descr = NULL,
	.params_tmpl = NULL,
	.num_devices = 3,
	.num_valueoutputs = 1,
	.num_valueinputs = 0,
	.num_msgoutputs = 0,
	.num_msginputs = 0,
	.cpu_loading = 0,
	.is_persistent = 1,
	.is_sync = 0,

	.devcfg={
		{
			.label = "input1",
			.descr = "Any device with Keyboard interface",
			.num_devifaces = sizeof(eventsrc_devifaces)/sizeof(eventsrc_devifaces[0]),
			.flag_canauto = 1,
			.flag_localonly = 1,
			.devifaces = eventsrc_devifaces
		},
		{
			.label = "input2",
			.descr = "Any device with Keyboard interface",
			.num_devifaces = sizeof(eventsrc_devifaces)/sizeof(eventsrc_devifaces[0]),
			.flag_canauto = 1,
			.flag_localonly = 1,
			.devifaces = eventsrc_devifaces
		},
		{
			.label = "input3",
			.descr = "Any device with Keyboard interface",
			.num_devifaces = sizeof(eventsrc_devifaces)/sizeof(eventsrc_devifaces[0]),
			.flag_canauto = 1,
			.flag_localonly = 1,
			.devifaces = eventsrc_devifaces
		}
	},
	.valueoutput={
		{
			.label = "state",
			.descr = "State of all keys",
			.notion = IOT_VALUENOTION_KEYCODE,
			.vclass_id = IOT_VALUECLASSID_BITMAP
		}
	},
	.valueinput={
	},
	.msgoutput={
	},
	.msginput={
	},

	//methods
	.init_instance = &eventsrc_instance::init_instance,
	.deinit_instance = &eventsrc_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};

iot_moduleconfig_t IOT_MODULE_CONF(eventsrc)={
	.title = "Keys Event Source",
	.descr = "Processes events from keyboards",
//	.module_id = MODULEID_eventsrc, //Registered ID of this module. Must correspond to its full name in registry
	.version = 0x000100001,
	.config_version = 0,
//	.num_devifaces = 0,
//	.num_devcontypes = 0,
	.init_module = NULL,
	.deinit_module = NULL,
//	.deviface_config = NULL,
//	.devcontype_config = NULL,
	.iface_node = &eventsrc_iface_node,
	.iface_device_driver = NULL,
	.iface_device_detector = NULL
};

//end of kbdlinux:keys event source  module



/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////kbd:oper_keystate (operator) node module
/////////////////////////////////////////////////////////////////////////////////


struct keystate_shift_t { //descriptor of possible shift key
	const char *cfgkey; //key in json config which can be values either true (to mean 'any') or 'left' or 'right' or false (to mean 'none') or be undefined
	uint16_t keycode1, keycode2; //key codes for left and right variant. keycode2 can be 0 if there is single variant of shift key
} keystate_shifts[] = {
	{"shift", 42, 54},
	{"ctrl", 29, 97},
	{"alt", 56, 100},
	{"meta", 125, 126}
};

#define NUM_KEYSTATE_SHIFTS (sizeof(keystate_shifts)/sizeof(keystate_shifts[0]))

struct oper_keystate_instance : public iot_node_base {
	uint32_t node_id;
	uint16_t key=0;
	int shift_conf[NUM_KEYSTATE_SHIFTS]; //configured conditions for shifts: -1 for undefined, 0 for false, 1 for left, 2 for right, 3 for true

/////////////static fields/methods for module instances management
	static int init_instance(iot_node_base** instance, uv_thread_t thread, uint32_t node_id, json_object *json_cfg) {
		uint16_t key=0;
		int shift_conf[NUM_KEYSTATE_SHIFTS];
		for(unsigned i=0;i<NUM_KEYSTATE_SHIFTS;i++) shift_conf[i]=-1;
		if(json_cfg) {
			json_object *val=NULL;
			if(json_object_object_get_ex(json_cfg, "key", &val)) {
				errno=0;
				int i=json_object_get_int(val);
				if(!errno && i>0 && i<=65536) key=uint16_t(i);
			}
			for(unsigned i=0;i<NUM_KEYSTATE_SHIFTS;i++) {
				if(json_object_object_get_ex(json_cfg, keystate_shifts[i].cfgkey, &val)) {
					if(json_object_is_type(val, json_type_string)) {
						const char* str=json_object_get_string(val);
						if(strcmp(str, "left")==0) {
							shift_conf[i]=1;
						} else if(strcmp(str, "right")==0) {
							shift_conf[i]=2;
						}
					} else {
						shift_conf[i]=json_object_get_boolean(val) ? 3 : 0;
					}
				}
			}

		}
		kapi_outlog_info("OPERATOR oper_keystate INITED id=%u, key=%u", node_id, unsigned(key));
		oper_keystate_instance *inst=new oper_keystate_instance(thread, node_id, key, shift_conf);
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_node_base* instance) {
		delete static_cast<oper_keystate_instance*>(instance);
		return 0;
	}
private:
	oper_keystate_instance(uv_thread_t thread, uint32_t node_id, uint16_t key, int* shift) : iot_node_base(thread), node_id(node_id), key(key) {
		for(unsigned i=0;i<NUM_KEYSTATE_SHIFTS;i++) shift_conf[i]=shift[i];
	}
	virtual ~oper_keystate_instance(void) {}

	virtual int start(void) override {
		assert(uv_thread_self()==thread);
		return 0;
	}

	virtual int stop(void) override {
		assert(uv_thread_self()==thread);
		return 0;
	}

//methods from iot_node_base
	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) override {
		assert(num_valueinputs==1);
		uint8_t outn=0;
		const iot_valuetype_BASE* outv;
		const iot_valuetype_bitmap *state=iot_valuetype_bitmap::cast(valueinputs[0].new_value);
		if(!state) outv=NULL;
		else if(!key) { //no correct key configured, check only shifts
			unsigned i=0;
			//check shifts
			for(;i<NUM_KEYSTATE_SHIFTS;i++) {
				switch(shift_conf[i]) {
					case 0: //must be both left and right disabled
						if(state->test_code(keystate_shifts[i].keycode1) || (keystate_shifts[i].keycode2>0 && state->test_code(keystate_shifts[i].keycode2))) break;
						continue;
					case 1: //left must be enabled, right disabled
						if(!state->test_code(keystate_shifts[i].keycode1) || (keystate_shifts[i].keycode2>0 && state->test_code(keystate_shifts[i].keycode2))) break;
						continue;
					case 2: //right must be enabled, left disabled
						if(!keystate_shifts[i].keycode2 || !state->test_code(keystate_shifts[i].keycode2) || state->test_code(keystate_shifts[i].keycode1)) break;
						continue;
					case 3: //any must be enabled
						if((!keystate_shifts[i].keycode2 || !state->test_code(keystate_shifts[i].keycode2)) && !state->test_code(keystate_shifts[i].keycode1)) break;
						continue;
					case -1:
						continue;
				}
				break;
			}
			if(i>=NUM_KEYSTATE_SHIFTS) outv=&iot_valuetype_boolean::const_true;
				else outv=&iot_valuetype_boolean::const_false;
		} else {
			const iot_valuetype_BASE* cur_output=kapi_get_outputvalue(outn);

			if(!cur_output || cur_output==&iot_valuetype_boolean::const_false) {
				outv=&iot_valuetype_boolean::const_false;
				//condition for enabling output
				if(state->test_code(key)) {
					unsigned i=0;
					//check shifts
					for(;i<NUM_KEYSTATE_SHIFTS;i++) {
						switch(shift_conf[i]) {
							case 0: //must be both left and right disabled
								if(state->test_code(keystate_shifts[i].keycode1) || (keystate_shifts[i].keycode2>0 && state->test_code(keystate_shifts[i].keycode2))) break;
								continue;
							case 1: //left must be enabled, right disabled
								if(!state->test_code(keystate_shifts[i].keycode1) || (keystate_shifts[i].keycode2>0 && state->test_code(keystate_shifts[i].keycode2))) break;
								continue;
							case 2: //right must be enabled, left disabled
								if(!keystate_shifts[i].keycode2 || !state->test_code(keystate_shifts[i].keycode2) || state->test_code(keystate_shifts[i].keycode1)) break;
								continue;
							case 3: //any must be enabled
								if((!keystate_shifts[i].keycode2 || !state->test_code(keystate_shifts[i].keycode2)) && !state->test_code(keystate_shifts[i].keycode1)) break;
								continue;
							case -1:
								continue;
						}
						break;
					}
					if(i>=NUM_KEYSTATE_SHIFTS) outv=&iot_valuetype_boolean::const_true;
				}
			} else {
				//condition for disabling. look just as the state of 'key'
				outv=state->test_code(key) ? &iot_valuetype_boolean::const_true : &iot_valuetype_boolean::const_false;
			}
		}
		int err=kapi_update_outputs(&eventid, 1, &outn, &outv);
		if(err) {
			kapi_outlog_error("Cannot update output value for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
		}
		return 0; // use IOT_ERROR_NOT_READY for debugging async exec
	}
};

//keys_instance* keys_instance::instances_head=NULL;


//static iot_module_spec_t drvmodspec={
//	.vendor=ECB_STRINGIFY(IOT_VENDOR),
//	.bundle=ECB_STRINGIFY(IOT_BUNDLE),
//	.module=ECB_STRINGIFY(DRVNAME),
//};


static iot_iface_node_t oper_keystate_iface_node = {
	.descr = NULL,
	.params_tmpl =  R"!!!({
"shortDescr":	["concatws", " ",
					["data", "hwid.bus"],
					["case", 
						[["hash_exists", ["data", "hwid.caps"], "key", "rel"],		["txt","dev_mouse"]],
						[["hash_exists", ["data", "hwid.caps"], "key"],				["txt","dev_keyboard"]],
						[["hash_exists", ["data", "hwid.caps"], "led"],				["txt","dev_led"]],
						[["hash_exists", ["data", "hwid.caps"], "snd"],				["txt","dev_snd"]],
						[["hash_exists", ["data", "hwid.caps"], "sw"],				["txt","dev_sw"]]
					],
					["vendor_name", ["data", "hwid.vendor"]]
				],
"longDescr":
"propList":
"newDialog":
"editDialog":
})!!!",
	.num_devices = 0,
	.num_valueoutputs = 1,
	.num_valueinputs = 1,
	.num_msgoutputs = 0,
	.num_msginputs = 0,
	.cpu_loading = 0,
	.is_persistent = 0,
	.is_sync = 1,

	.devcfg={},
	.valueoutput={
		{
			.label = "out",
			.descr = "Logical result of state comparison",
			.notion = 0,
			.vclass_id = IOT_VALUECLASSID_BOOLEAN
		}
	},
	.valueinput={
		{
			.label = "in",
			.descr = "Input for keyboard state",
			.notion = IOT_VALUENOTION_KEYCODE,
			.vclass_id = IOT_VALUECLASSID_BITMAP
		}
	},
	.msgoutput={},
	.msginput={},

	//methods
	.init_instance = &oper_keystate_instance::init_instance,
	.deinit_instance = &oper_keystate_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};

iot_moduleconfig_t IOT_MODULE_CONF(oper_keystate)={
	.title = "Operator to check keyboard state",
	.descr = "Checks if specific keys are depressed",
//	.module_id = MODULEID_oper_keystate, //Registered ID of this module. Must correspond to its full name in registry
	.version = 0x000100001,
	.config_version = 0,
//	.num_devifaces = 0,
//	.num_devcontypes = 0,
	.init_module = NULL,
	.deinit_module = NULL,
//	.deviface_config = NULL,
//	.devcontype_config = NULL,
	.iface_node = &oper_keystate_iface_node,
	.iface_device_driver = NULL,
	.iface_device_detector = NULL
};




/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////kbd:leds node module
/////////////////////////////////////////////////////////////////////////////////


struct leds_instance : public iot_node_base {
	uint32_t node_id;
	uint32_t activate_bitmap=0, deactivate_bitmap=0;
	struct {
		const iot_conn_clientview *conn;
	} device={}; //per device connection state

/////////////static fields/methods for module instances management
	static int init_module(void) {
		return 0;
	}
	static int deinit_module(void) {
		return 0;
	}

	static int init_instance(iot_node_base** instance, uv_thread_t thread, uint32_t node_id, json_object *json_cfg) {
		kapi_outlog_info("NODE leds INITED id=%u", node_id);

		leds_instance *inst=new leds_instance(thread, node_id);
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_node_base* instance) {
		leds_instance *inst=static_cast<leds_instance*>(instance);
		delete inst;
		return 0;
	}
private:
	leds_instance(uv_thread_t thread, uint32_t node_id) : iot_node_base(thread), node_id(node_id)
	{
	}

	virtual ~leds_instance(void) {
	}

//methods from iot_module_instance_base
	//Called to start work of previously inited instance.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, node_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(void) override {
		assert(uv_thread_self()==thread);

		return 0;
	}

	//called to stop work of started instance. call can be followed by deinit or started again (if stop was manual, by user)
	//Return values:
	//0 - driver successfully stopped and can be deinited or restarted
	//IOT_ERROR_TRY_AGAIN - driver requires some time (async operation) to stop gracefully. kapi_self_abort() will be called to notify kernel when stop is finished.
	//						anyway second stop() call must free all resources correctly, may be in a hard way. otherwise module will be blocked and left in hang state (deinit
	//						cannot be called until stop reports OK)
	//any other error is treated as critical bug and driver is blocked for further starts. deinit won't be called for such instance. instance is put into hang state
	virtual int stop(void) override {
		assert(uv_thread_self()==thread);
		return 0;
	}

	virtual int device_attached(const iot_conn_clientview* conn) override {
		assert(uv_thread_self()==thread);
		assert(device.conn==NULL);

		device.conn=conn;
		iot_deviface__activatable_CL iface(conn);
		int err=iface.set_state(activate_bitmap, deactivate_bitmap );
		assert(err==0);
		return 0;
	}
	virtual int device_detached(const iot_conn_clientview* conn) override {
		assert(uv_thread_self()==thread);
		assert(device.conn!=NULL);

		device.conn=NULL;
		return 0;
	}
	virtual int device_action(const iot_conn_clientview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) override {
		assert(uv_thread_self()==thread);
		assert(device.conn==conn);
/*
//		int err;
		if(action_code==IOT_DEVCONN_ACTION_FULLREQUEST) {//new message arrived
			iot_deviface__keyboard_CL iface(&conn->deviface);
			const iot_deviface__keyboard_CL::msg* msg=iface.parse_event(data, data_size);
			if(!msg) return 0;

			switch(msg->event_code) {
				case iface.EVENT_KEYDOWN:
					kapi_outlog_info("GOT keyboard DOWN for key %d from device index %d", (int)msg->key, int(conn->index));
//					if(msg->key==KEY_ESC) {
//						kapi_outlog_info("Requesting state");
//						err=iface.request_state(conn->id, this);
//						assert(err==0);
//					}
					break;
				case iface.EVENT_KEYUP:
					kapi_outlog_info("GOT keyboard UP for key %d from device index %d", (int)msg->key, int(conn->index));
					break;
				case iface.EVENT_KEYREPEAT:
					kapi_outlog_info("GOT keyboard REPEAT for key %d from device index %d", (int)msg->key, int(conn->index));
					break;
				case iface.EVENT_SET_STATE:
					kapi_outlog_info("GOT NEW STATE, datasize=%u, statesize=%u from device index %d", data_size, (unsigned)(msg->statesize), int(conn->index));
					break;
				default:
					kapi_outlog_info("Got unknown event %d from device index %d, node_id=%u", int(msg->event_code), int(conn->index), node_id);
					return 0;
			}
			//update key state of device
			memcpy(device[conn->index].keystate, msg->state, msg->statesize*sizeof(uint32_t));
			update_outputs();
			return 0;
		}*/
		kapi_outlog_info("Device action, node_id=%u, act code %u, datasize %u from device index %d", node_id, unsigned(action_code), data_size, int(conn->index));
		return 0;
	}

//methods from iot_node_base
	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) override {
		activate_bitmap=deactivate_bitmap=0;
		for(int i=0;i<num_valueinputs;i++) {
			if(!valueinputs[i].new_value) continue;
			const iot_valuetype_boolean* v=iot_valuetype_boolean::cast(valueinputs[i].new_value);
			if(!v) continue;
			if(*v) activate_bitmap|=1<<i;
				else deactivate_bitmap|=1<<i;
		}
		if(!device.conn) return 0;

		iot_deviface__activatable_CL iface(device.conn);
		int err=iface.set_state(activate_bitmap, deactivate_bitmap );
		assert(err==0);

		return 0;
	}

};

//keys_instance* keys_instance::instances_head=NULL;


//static iot_module_spec_t drvmodspec={
//	.vendor=ECB_STRINGIFY(IOT_VENDOR),
//	.bundle=ECB_STRINGIFY(IOT_BUNDLE),
//	.module=ECB_STRINGIFY(DRVNAME),
//};

static const iot_deviface_params_activatable leds_filter_min3(3,0);

static const iot_deviface_params* leds_devifaces[]={
	&leds_filter_min3
};


static iot_iface_node_t leds_iface_node = {
	.descr = NULL,
	.params_tmpl = NULL,
	.num_devices = 1,
	.num_valueoutputs = 0,
	.num_valueinputs = 3,
	.num_msgoutputs = 0,
	.num_msginputs = 0,
	.cpu_loading = 0,
	.is_persistent = 1,
	.is_sync = 0,

	.devcfg={
		{
			.label = "dev",
			.descr = "Any device with Activatable interface",
			.num_devifaces = sizeof(leds_devifaces)/sizeof(leds_devifaces[0]),
			.flag_canauto = 1,
			.flag_localonly = 1,
			.devifaces = leds_devifaces
		}
	},
	.valueoutput={},
	.valueinput={
		{
			.label = "numlk",
			.descr = "NumLock LED state input",
			.notion = 0,
			.vclass_id = IOT_VALUECLASSID_BOOLEAN
		},
		{
			.label = "capslk",
			.descr = "CapsLock LED state input",
			.notion = 0,
			.vclass_id = IOT_VALUECLASSID_BOOLEAN
		},
		{
			.label = "scrlk",
			.descr = "ScrollLock LED state input",
			.notion = 0,
			.vclass_id = IOT_VALUECLASSID_BOOLEAN
		}
	},
	.msgoutput={},
	.msginput={},

	//methods
	.init_instance = &leds_instance::init_instance,
	.deinit_instance = &leds_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};

iot_moduleconfig_t IOT_MODULE_CONF(leds)={
	.title = "Keyboard LEDs control",
	.descr = "Module to control 3 LEDs of typical PC keyboards: Caps Lock, Num Lock and Scroll Lock",
//	.module_id = MODULEID_leds, //Registered ID of this module. Must correspond to its full name in registry
	.version = 0x000100001,
	.config_version = 0,
//	.num_devifaces = 0,
//	.num_devcontypes = 0,
	.init_module = leds_instance::init_module,
	.deinit_module = leds_instance::deinit_module,
//	.deviface_config = NULL,
//	.devcontype_config = NULL,
	.iface_node = &leds_iface_node,
	.iface_device_driver = NULL,
	.iface_device_detector = NULL
};






