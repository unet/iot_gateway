#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>
#include<stdlib.h>
#include<unistd.h>
#include<assert.h>
#include<new>

#include<uv.h>

#include <linux/input.h>

#include<iot_utils.h>
#include<iot_error.h>


#define IOT_VENDOR unet
#define IOT_BUNDLE generic__kbd

#include<iot_module.h>

#include<iot_devclass_keyboard.h>


//#define DRVNAME drv

//list of modules in current bundle, their registered IDs. TODO: put real IDs when they will be allocated through some table in unetcommonsrc
#define BUNDLE_MODULES_MAP(XX) \
	XX(kbd_src, 3) \
	XX(oper_keys, 4)
//	XX(kbdled_exor, 5) 


//build constants like MODULEID_detector which resolve to registered module id
enum {
#define XX(name, id) MODULEID_ ## name = id,
	BUNDLE_MODULES_MAP(XX)
#undef XX
};


/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////kbd:kbd_src (pure event source) node module
/////////////////////////////////////////////////////////////////////////////////
#define KBD_SRC_MAX_DEVICES 3

struct kbd_src_instance : public iot_node_base {
	uint32_t node_id;
	uv_timer_t timer_watcher={};
	struct {
		const iot_conn_clientview *conn;
		uint16_t maxkeycode;
		uint32_t keystate[IOT_KEYBOARD_MAX_KEYCODE/32+1]; //current state of keys of device
	} device[KBD_SRC_MAX_DEVICES]={}; //per device connection state


/////////////static fields/methods for module instances management
	static int init_module(void) {
		return 0;
	}
	static int deinit_module(void) {
		return 0;
	}
	static int init_instance(iot_node_base**instance, uv_thread_t thread, uint32_t node_id, json_object *json_cfg) {
		kbd_src_instance *inst=new kbd_src_instance(thread, node_id);
		*instance=inst;
		return 0;
	}
	static int deinit_instance(iot_node_base* instance) {
		kbd_src_instance *inst=static_cast<kbd_src_instance*>(instance);
		delete inst;
		return 0;
	}
private:
	kbd_src_instance(uv_thread_t thread, uint32_t node_id) : iot_node_base(thread), node_id(node_id) {}
	virtual ~kbd_src_instance(void) {}

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
		assert(conn->index<KBD_SRC_MAX_DEVICES);
		assert(device[conn->index].conn==NULL);

		device[conn->index].conn=conn;
		memset(device[conn->index].keystate, 0, sizeof(device[conn->index].keystate));
		iot_devifaceclass__keyboard_CL iface(&conn->devclass);
		device[conn->index].maxkeycode=iface.get_max_keycode();
		return 0;
	}
	virtual int device_detached(const iot_conn_clientview* conn) override {
		assert(uv_thread_self()==thread);
		assert(conn->index<KBD_SRC_MAX_DEVICES);
		assert(device[conn->index].conn!=NULL);

		device[conn->index].conn=NULL;
		update_outputs();
		return 0;
	}
	virtual int device_action(const iot_conn_clientview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) override {
		assert(uv_thread_self()==thread);
		assert(conn->index<KBD_SRC_MAX_DEVICES);
		assert(device[conn->index].conn==conn);

//		int err;
		if(action_code==IOT_DEVCONN_ACTION_MESSAGE) {//new message arrived
			iot_devifaceclass__keyboard_CL iface(&conn->devclass);
			const iot_devifaceclass__keyboard_CL::msg* msg=iface.parse_event(data, data_size);
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
		}
		kapi_outlog_info("Device action, node_id=%u, act code %u, datasize %u from device index %d", node_id, unsigned(action_code), data_size, int(conn->index));
		return 0;
	}

//own methods
	int update_outputs(void) { //set current common key state on output
		uint16_t maxkeycode=0;
		uint32_t keystate[IOT_KEYBOARD_MAX_KEYCODE/32+1]; //current state of keys of device
		memset(keystate, 0, sizeof(keystate));

		for(int i=0; i<KBD_SRC_MAX_DEVICES; i++) {
			if(!device[i].conn) continue;
			if(device[i].maxkeycode>maxkeycode) maxkeycode=device[i].maxkeycode;
			unsigned n=device[i].maxkeycode/32+1;
			for (unsigned j=0; j<n; j++) keystate[j]|=device[i].keystate[j];
		}

		char valbuf[iot_valueclass_kbdstate::calc_datasize(maxkeycode)];
		uint8_t outn=0;
		const iot_valueclass_BASE* outv=new(valbuf) iot_valueclass_kbdstate(maxkeycode, keystate, maxkeycode/32+1, false);
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


static iot_devifacetype kbd_src_devclasses[]={
	iot_devifacetype([](iot_devifacetype* cls_data) -> void {
		iot_devifacetype_keyboard::init_classdata(cls_data, 0, 1);
	})
};


static iot_iface_node_t kbd_src_iface_node = {
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
			.num_devclasses = sizeof(kbd_src_devclasses)/sizeof(kbd_src_devclasses[0]),
			.flag_canauto = 1,
			.flag_localonly = 1,
			.devclasses = kbd_src_devclasses
		},
		{
			.label = "input2",
			.descr = "Any device with Keyboard interface",
			.num_devclasses = sizeof(kbd_src_devclasses)/sizeof(kbd_src_devclasses[0]),
			.flag_canauto = 1,
			.flag_localonly = 1,
			.devclasses = kbd_src_devclasses
		},
		{
			.label = "input3",
			.descr = "Any device with Keyboard interface",
			.num_devclasses = sizeof(kbd_src_devclasses)/sizeof(kbd_src_devclasses[0]),
			.flag_canauto = 1,
			.flag_localonly = 1,
			.devclasses = kbd_src_devclasses
		}
	},
	.valueoutput={
		{
			.label = "state",
			.descr = "State of all keys",
			.unit = NULL,
			.vclass_id = IOT_VALUECLASSID_KBDSTATE
		}
	},
	.valueinput={
	},
	.msgoutput={
	},
	.msginput={
	},

	//methods
	.init_instance = &kbd_src_instance::init_instance,
	.deinit_instance = &kbd_src_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};

iot_moduleconfig_t IOT_MODULE_CONF(kbd_src)={
	.title = "Keys Event Source",
	.descr = "Processes events from keyboards",
	.module_id = MODULEID_kbd_src, //Registered ID of this module. Must correspond to its full name in registry
	.version = 0x000100001,
	.config_version = 0,
	.num_devifaces = 0,
	.num_devcontypes = 0,
//	.flags = IOT_MODULEFLAG_IFACE_SOURCE,
	.init_module = kbd_src_instance::init_module,
	.deinit_module = kbd_src_instance::deinit_module,
	.deviface_config = NULL,
	.devcontype_config = NULL,
	.iface_node = &kbd_src_iface_node,
	.iface_device_driver = NULL,
	.iface_device_detector = NULL
};

//end of kbdlinux:keys event source  module



/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////kbd:oper_keys (operator) node module
/////////////////////////////////////////////////////////////////////////////////


struct oper_keys_instance : public iot_node_base {
	uint32_t node_id;
	bool is_active=false; //true if instance was started
	uint16_t key=0;

/////////////static fields/methods for module instances management
	static int init_module(void) {
		return 0;
	}
	static int deinit_module(void) {
		return 0;
	}

	static int init_instance(iot_node_base** instance, uv_thread_t thread, uint32_t node_id, json_object *json_cfg) {
		uint16_t key=0;
		if(json_cfg) {
			json_object *val=NULL;
			if(json_object_object_get_ex(json_cfg, "key", &val)) {
				errno=0;
				int i=json_object_get_int(val);
				if(!errno && i>0 && i<=IOT_KEYBOARD_MAX_KEYCODE) key=uint16_t(i);
			}
		}

		kapi_outlog_info("OPERATOR oper_keys INITED id=%u, key=%u", node_id, unsigned(key));

		oper_keys_instance *inst=new oper_keys_instance(thread, node_id, key);
		int err=inst->init();
		if(err) { //error
			delete inst;
			return err;
		}
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_node_base* instance) {
		oper_keys_instance *inst=static_cast<oper_keys_instance*>(instance);
		delete inst;
		return 0;
	}
private:
	oper_keys_instance(uv_thread_t thread, uint32_t node_id, uint16_t key) : iot_node_base(thread), node_id(node_id), key(key)
	{
	}

	virtual ~oper_keys_instance(void) {
	}

	int init(void) {
		return 0;
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
		assert(!is_active);

		if(is_active) return 0; //even in release mode just return success

		is_active=true;

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
		assert(is_active);

		if(!is_active) return 0; //even in release mode just return success

		is_active=false;
		return 0;
	}

	virtual int device_attached(const iot_conn_clientview* conn) override {
		assert(uv_thread_self()==thread);
		return 0;
	}
	virtual int device_detached(const iot_conn_clientview* conn) override {
		assert(uv_thread_self()==thread);
		return 0;
	}
	virtual int device_action(const iot_conn_clientview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) override {
		assert(uv_thread_self()==thread);
		return 0;
	}

//methods from iot_node_base
	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) override {
		assert(num_valueinputs==1);
		uint8_t outn=0;
		const iot_valueclass_BASE* outv;
		if(!key) { //no correct key configured, always false
			outv=&iot_valueclass_boolean::const_false;
		} else {
			const iot_valueclass_kbdstate *state=iot_valueclass_kbdstate::cast(valueinputs[0].new_value);
			if(!state) outv=NULL; //undef value OR incorrect value class
				else outv=state->test_key(key) ? &iot_valueclass_boolean::const_true : &iot_valueclass_boolean::const_false;
		}
		int err=kapi_update_outputs(&eventid, 1, &outn, &outv);
		if(err) {
			kapi_outlog_error("Cannot update output value for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
		}
		return IOT_ERROR_NOT_READY;
	}

};

//keys_instance* keys_instance::instances_head=NULL;


//static iot_module_spec_t drvmodspec={
//	.vendor=ECB_STRINGIFY(IOT_VENDOR),
//	.bundle=ECB_STRINGIFY(IOT_BUNDLE),
//	.module=ECB_STRINGIFY(DRVNAME),
//};


static iot_iface_node_t oper_keys_iface_node = {
	.descr = NULL,
	.params_tmpl = NULL,
	.num_devices = 3,
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
			.unit = NULL,
			.vclass_id = IOT_VALUECLASSID_BOOLEAN
		}
	},
	.valueinput={
		{
			.label = "in",
			.descr = "Input for keyboard state",
			.unit = NULL,
			.vclass_id = IOT_VALUECLASSID_KBDSTATE
		}
	},
	.msgoutput={},
	.msginput={},

	//methods
	.init_instance = &oper_keys_instance::init_instance,
	.deinit_instance = &oper_keys_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};

iot_moduleconfig_t IOT_MODULE_CONF(oper_keys)={
	.title = "Operator to check keyboard state",
	.descr = "Checks if specific keys are depressed",
	.module_id = MODULEID_oper_keys, //Registered ID of this module. Must correspond to its full name in registry
	.version = 0x000100001,
	.config_version = 0,
	.num_devifaces = 0,
	.num_devcontypes = 0,
	.init_module = oper_keys_instance::init_module,
	.deinit_module = oper_keys_instance::deinit_module,
	.deviface_config = NULL,
	.devcontype_config = NULL,
	.iface_node = &oper_keys_iface_node,
	.iface_device_driver = NULL,
	.iface_device_detector = NULL
};

//end of kbdlinux:keys event source  module





