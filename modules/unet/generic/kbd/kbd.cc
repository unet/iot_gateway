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
	XX(kbdled_exor, 4) \
	XX(cond_keyop, 5)


//build constants like MODULEID_detector which resolve to registered module id
enum {
#define XX(name, id) MODULEID_ ## name = id,
	BUNDLE_MODULES_MAP(XX)
#undef XX
};


/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////inputlinux:kbd_src event source  module
/////////////////////////////////////////////////////////////////////////////////

static iot_devifaceclass_data kbd_src_devclasses[]={
	iot_devifaceclass_data([](iot_devifaceclass_data* cls_data) -> void {
		iot_devifaceclassdata_keyboard::init_classdata(cls_data, 0, 2);
	})
};

struct kbd_src_instance;

struct kbd_src_instance : public iot_event_source_base {
	uv_thread_t thread;
	iot_miid_t miid;
	uint32_t iot_id;
	bool is_active; //true if instance was started
	uv_timer_t timer_watcher;
	const iot_conn_clientview *device;
/////////////evsource state:
	iot_state_error_t error_code;


/////////////static fields/methods for module instances management
	static int init_module(void) {
		return 0;
	}
	static int deinit_module(void) {
		return 0;
	}

	static int init_instance(iot_event_source_base**instance, uv_thread_t thread, uint32_t iot_id, const char *json_cfg) {
		assert(uv_thread_self()==main_thread);

		kapi_outlog_info("EVENT SOURCE INITED id=%u", iot_id);

		kbd_src_instance *inst=new kbd_src_instance(thread, iot_id);
		int err=inst->init();
		if(err) { //error
			delete inst;
			return err;
		}
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_module_instance_base* instance) {
		assert(uv_thread_self()==main_thread);

		kbd_src_instance *inst=static_cast<kbd_src_instance*>(instance);
		delete inst;
		return 0;
	}
private:
	kbd_src_instance(uv_thread_t thread, uint32_t iot_id) :
		thread(thread),
		miid(0,0),
		iot_id(iot_id),
		is_active(false),
		device(NULL),
		error_code(0) {
	}

	virtual ~kbd_src_instance(void) {
	}

	int init(void) {
		uv_loop_t* loop=kapi_get_event_loop(thread);
		assert(loop!=NULL);

		uv_timer_init(loop, &timer_watcher);
		timer_watcher.data=this;
		return 0;
	}

//methods from iot_module_instance_base
	//Called to start work of previously inited instance.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, iot_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(iot_miid_t _miid) {
		assert(uv_thread_self()==thread);
		assert(!is_active);

		if(is_active) return 0; //even in release mode just return success

		miid=_miid;
		is_active=true;

		if(!device) uv_timer_start(&timer_watcher, [](uv_timer_t* handle)->void {
			kbd_src_instance* obj=static_cast<kbd_src_instance*>(handle->data);
			obj->on_timer();
		}, 2000, 0); //give 2 secs for device to connect

		return 0;
	}

	//called to stop work of started instance. call can be followed by deinit or started again (if stop was manual, by user)
	//Return values:
	//0 - driver successfully stopped and can be deinited or restarted
	//IOT_ERROR_TRY_AGAIN - driver requires some time (async operation) to stop gracefully. kapi_modinstance_self_abort() will be called to notify kernel when stop is finished.
	//						anyway second stop() call must free all resources correctly, may be in a hard way. otherwise module will be blocked and left in hang state (deinit
	//						cannot be called until stop reports OK)
	//any other error is treated as critical bug and driver is blocked for further starts. deinit won't be called for such instance. instance is put into hang state
	virtual int stop(void) {
		assert(uv_thread_self()==thread);
		assert(is_active);

		if(!is_active) return 0; //even in release mode just return success

		kapi_outlog_info("EVENT SOURCE STOPPED id=%u", iot_id);

		uv_timer_stop(&timer_watcher);

		is_active=false;
		return 0;
	}

//methods from iot_event_source_base
	virtual int device_attached(const iot_conn_clientview* conn) {
		assert(uv_thread_self()==thread);
		assert(device==NULL);
		device=conn;

		kapi_outlog_info("Device attached, iot_id=%u, driver inst id=%u", iot_id, (unsigned)device->driver.miid.iid);
		return 0;
	}
	virtual int device_detached(const iot_conn_clientview* conn) {
		assert(uv_thread_self()==thread);
		assert(device!=NULL);
		device=NULL;

		kapi_outlog_info("Device detached, iot_id=%u", iot_id);
		return 0;
	}
	virtual int device_action(const iot_conn_clientview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) {
		assert(uv_thread_self()==thread);
		assert(device!=NULL);
		int err;
		switch(action_code) {
			case IOT_DEVCONN_ACTION_MESSAGE: //new message arrived
				if(device->devclass.classid==IOT_DEVIFACECLASSID_KEYBOARD) {
					iot_devifaceclass__keyboard_CL iface(&device->devclass);
					const iot_devifaceclass__keyboard_CL::msg* msg=iface.parse_event(data, data_size);
					if(!msg) return 0;

					if(msg->event_code==iface.EVENT_KEYDOWN) {
						kapi_outlog_info("GOT keyboard DOWN for key %d", (int)msg->key);
						if(msg->key==KEY_ESC) {
							kapi_outlog_info("Requesting state");
							err=iface.request_state(device->id, this);
							assert(err==0);
						}
						return 0;
					} else if(msg->event_code==iface.EVENT_KEYUP) {
						kapi_outlog_info("GOT keyboard UP for key %d", (int)msg->key);
						return 0;
					} else if(msg->event_code==iface.EVENT_KEYREPEAT) {
						kapi_outlog_info("GOT keyboard REPEAT for key %d", (int)msg->key);
						return 0;
					} else if(msg->event_code==iface.EVENT_SET_STATE) {
						kapi_outlog_info("GOT NEW STATE, datasize=%u, statesize=%u", data_size, (unsigned)(msg->statesize));
						return 0;
					}
				}
				break;
			default:
				break;
		}
		kapi_outlog_info("Device action, iot_id=%u, act code %u, datasize %u", iot_id, unsigned(action_code), data_size);
		return 0;
	}

	void on_timer(void) {
		error_code=IOT_STATE_ERROR_NO_DEVICE;
	}
};

//keys_instance* keys_instance::instances_head=NULL;


//static iot_module_spec_t drvmodspec={
//	.vendor=ECB_STRINGIFY(IOT_VENDOR),
//	.bundle=ECB_STRINGIFY(IOT_BUNDLE),
//	.module=ECB_STRINGIFY(DRVNAME),
//};


static iot_iface_event_source_t kbd_src_iface_event_source = {
	.num_devices = 1,
	.num_values = 1,
	.cpu_loading = 0,

	.devcfg={
		{
			.num_devclasses = sizeof(kbd_src_devclasses)/sizeof(kbd_src_devclasses[0]),
			.flag_canauto = 1,
			.flag_localonly = 1,
			.devclasses = kbd_src_devclasses
		}
	},
	.statevaluecfg={
		{
			.label = "st",
			.vclass_id = IOT_VALUECLASSID_KEYBOARD
		}
	},

	//methods
	.init_instance = &kbd_src_instance::init_instance,
	.deinit_instance = &kbd_src_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};

iot_moduleconfig_t IOT_MODULE_CONF(kbd_src)={
	.module_id = MODULEID_kbd_src, //Registered ID of this module. Must correspond to its full name in registry
	.version = 0x000100001,
	.num_devifaces = 0,
	.num_devcontypes = 0,
//	.flags = IOT_MODULEFLAG_IFACE_SOURCE,
	.init_module = [](void) -> int {return 0;},
	.deinit_module = [](void) -> int {return 0;},
	.deviface_config = NULL,
	.devcontype_config = NULL,
	.iface_event_source = &kbd_src_iface_event_source,
	.iface_device_driver = NULL,
	.iface_device_detector = NULL
};

//end of kbdlinux:keys event source  module





