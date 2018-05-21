#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>
#include<stdlib.h>
#include<unistd.h>
#include<assert.h>
#include<new>


#include <linux/input.h>

#include "iot_module.h"

IOT_LIBVERSION_DEFINE; //creates global symbol with library full version spec according to IOT_LIBVERSION, IOT_LIBPATCHLEVEL and IOT_LIBREVISION defines

#include "contype_linuxinput.h"

#include "iot_devclass_keyboard.h"
#include "iot_devclass_activatable.h"
#include "modules/unet/types/di_toneplayer/iot_devclass_toneplayer.h"

// "/dev/input/eventX" device on linuxes
//#define DEVCONTYPE_CUSTOM_LINUXINPUT	IOT_DEVCONTYPE_CUSTOM(MODULEID_inputlinux, 1)
//#define DEVCONTYPESTR_CUSTOM_LINUXINPUT	"LinuxInput"



iot_hwdevcontype_metaclass_linuxinput iot_hwdevcontype_metaclass_linuxinput::object; //the only instance of this class



/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////inputlinux:detector Device Detector module
/////////////////////////////////////////////////////////////////////////////////


//period of checking for available keyboards, in milliseconds
#define DETECTOR_POLL_INTERVAL 5000
//maximum event devices for detection
#define DETECTOR_MAX_DEVS 32

class detector : public iot_device_detector_base {
	bool is_active=false; //true if instance was started
	uv_timer_t timer_watcher={};
	int devinfo_len=0; //number of filled items in devinfo array
	struct devinfo_t { //short device info indexed by event_index field. minimal info necessary to determine change of device
		iot_hwdev_localident_linuxinput ident;
		bool present;
		bool error; //there was persistent error adding this device, so no futher attempts should be done
	} devinfo[DETECTOR_MAX_DEVS]={};

	void on_timer(void) {
		iot_hwdev_details_linuxinput fulldevinfo[DETECTOR_MAX_DEVS];
		int n=get_event_devices(fulldevinfo, DETECTOR_MAX_DEVS);
		if(n==0 && devinfo_len==0) return; //nothing to do

		iot_hwdev_localident_linuxinput ident;

		int i, err;
		int max_n = n>=devinfo_len ? n : devinfo_len;

		for(i=0;i<max_n;i++) { //compare if actual devices changed for common indexes
			if(i<devinfo_len && devinfo[i].present) {
				if(i>=n || !fulldevinfo[i].data_valid) { //new state is absent, so device was removed
					if(!devinfo[i].error) { //device was added to registry, so must be removed
						err=kapi_hwdev_registry_action(IOT_ACTION_REMOVE, &devinfo[i].ident, NULL);
						if(err) kapi_outlog_error("Cannot remove device from registry: %s", kapi_strerror(err));
						if(err==IOT_ERROR_TEMPORARY_ERROR) continue; //retry
						//success or critical error
						devinfo[i].present=false;
					}
					continue;
				}
//				err=ident.init_spec(uint8_t(i), fulldevinfo[i].input.bustype, fulldevinfo[i].input.vendor, fulldevinfo[i].input.product, fulldevinfo[i].input.version, fulldevinfo[i].cap_bitmap, fulldevinfo[i].phys);
				if(!fulldevinfo[i].fill_localident(&ident, sizeof(ident), NULL)) {
					err=IOT_ERROR_INVALID_ARGS;
//				if(err) {
					kapi_outlog_error("Cannot fill device local identity: %s", kapi_strerror(err));
				} else {
					//check if device looks the same
					if(devinfo[i].ident.matches(&ident)) continue; //same

					err=kapi_hwdev_registry_action(IOT_ACTION_ADD, &ident, &fulldevinfo[i]);
					if(err) kapi_outlog_error("Cannot update device in registry: %s", kapi_strerror(err));
				}
				if(err==IOT_ERROR_TEMPORARY_ERROR) continue; //retry
				//success or critical error
				devinfo[i].error = !!err;
				devinfo[i].ident=ident;
			} else  //previous state was absent
				if(i<n && fulldevinfo[i].data_valid) { //new state is present, so NEW DEVICE ADDED
//					err=ident.init_spec(uint8_t(i), fulldevinfo[i].input.bustype, fulldevinfo[i].input.vendor, fulldevinfo[i].input.product, fulldevinfo[i].input.version, fulldevinfo[i].cap_bitmap, fulldevinfo[i].phys);
					if(!fulldevinfo[i].fill_localident(&ident, sizeof(ident), NULL)) {
						err=IOT_ERROR_INVALID_ARGS;
//					if(err) {
						kapi_outlog_error("Cannot fill device local identity: %s", kapi_strerror(err));
					} else {
						err=kapi_hwdev_registry_action(IOT_ACTION_ADD, &ident, &fulldevinfo[i]);
						if(err) kapi_outlog_error("Cannot add new device to registry: %s", kapi_strerror(err));
					}
					if(err==IOT_ERROR_TEMPORARY_ERROR) continue; //retry
					//success or critical error
					devinfo[i].present=true;
					devinfo[i].error = !!err;

					devinfo[i].ident=ident;
					if(devinfo_len<i+1) devinfo_len=i+1;
					continue;
				} //else do nothing
		}
	}

//iot_module_instance_base methods


	//Called to start work of previously inited instance.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, iot_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(void) {
		assert(uv_thread_self()==thread);
		assert(!is_active);
		if(is_active) return 0;

		uv_timer_init(loop, &timer_watcher);
		timer_watcher.data=this;
		ref(); //instance is now additionally reffered in uv handle, so increase refcount to have correct closing of handle

		int err=uv_timer_start(&timer_watcher, [](uv_timer_t* handle) -> void {
			detector* obj=(detector*)(handle->data);
			obj->on_timer();
		}, 0, DETECTOR_POLL_INTERVAL);
		if(err<0) {
			uv_close((uv_handle_t*)&timer_watcher, kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'

			kapi_outlog_error("Cannot start timer: %s", uv_strerror(err));
			return IOT_ERROR_TEMPORARY_ERROR;
		}
		is_active=true;
		return 0;
	}
	virtual int stop(void) {
		assert(uv_thread_self()==thread);
		assert(is_active);

		if(!is_active) return 0;
		is_active=false;

		uv_close((uv_handle_t*)&timer_watcher, kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'
		return 0;
	}

public:
	detector(void) {
	}
	virtual ~detector(void) {
	}
	int init() {
		return 0;
	}

	static int init_instance(iot_device_detector_base**instance, json_object *json_cfg, json_object *manual_devices) {
		assert(uv_thread_self()==main_thread);

		detector *inst=new detector();
		if(!inst) return IOT_ERROR_TEMPORARY_ERROR;

		int err=inst->init();
		if(err) { //error
			inst->unref();
			return err;
		}
		*instance=inst;
		return 0;
	}
	//called to deinit instance.
	//Return values:
	//0 - success
	//any other error leaves instance in hang state
	static int deinit_instance(iot_device_detector_base* instance) {
		assert(uv_thread_self()==main_thread);
		detector *inst=static_cast<detector*>(instance);
		inst->unref();
		return 0;
	}
	static int check_system(void) {
		struct stat statbuf;
		int err=stat("/dev/input", &statbuf);
		if(!err) {
			if(S_ISDIR(statbuf.st_mode)) return 0; //dir
			return IOT_ERROR_NOT_SUPPORTED;
		}
		if(err==ENOMEM) return IOT_ERROR_TEMPORARY_ERROR;
		return IOT_ERROR_NOT_SUPPORTED;
	}

	//traverses all /dev/input/eventX devices and reads necessary props
	//returns number of found devices
	static int get_event_devices(iot_hwdev_details_linuxinput* devbuf, int max_devs, int start_index=0) {//takes address for array of devcontype_linuxinput_t structs and size of such array
		char filepath[32];
		int fd, idx, n=0;
		iot_hwdev_details_linuxinput *cur_dev;

		for(idx=0;idx<max_devs;idx++) {
			cur_dev=devbuf+idx;

			snprintf(filepath, sizeof(filepath), "/dev/input/event%d", idx+start_index);
			fd=open(filepath, O_RDONLY | O_NONBLOCK); //O_NONBLOCK help to avoid side-effects of open on Linux when only ioctl is necessary
			if(fd<0) {
				if(errno!=ENOENT && errno!=ENODEV && errno!=ENXIO) //file present but corresponding device not exists
					kapi_outlog_debug("Cannot open %s: %s", filepath, uv_strerror(uv_translate_sys_error(errno)));
				cur_dev->name[0]='\0'; //indicator of skipped device
				continue;
			}

			const char* errstr=cur_dev->read_inputdev_caps(fd, idx+start_index);

			if(errstr) { //was ioctl error
				kapi_outlog_debug("Cannot %s on %s: %s", errstr, filepath, uv_strerror(uv_translate_sys_error(errno)));
			}
			n=idx+1; //must return maximum successful index + 1
			close(fd);
		}
		return n;
	}
};

//static iot_hwdevcontype_t detector_devcontypes[]={DEVCONTYPE_CUSTOM_LINUXINPUT};


iot_detector_moduleconfig_t IOT_DETECTOR_MODULE_CONF(det)={
	.version = IOT_VERSION_COMPOSE(0,0,1),
	.cpu_loading = 0,
	.manual_devices_required = 0,
//	.num_hwdevcontypes = sizeof(detector_devcontypes)/sizeof(detector_devcontypes[0]),

	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &detector::init_instance,
	.deinit_instance = &detector::deinit_instance,
	.check_system = &detector::check_system

//	.hwdevcontypes = detector_devcontypes
};

//static const iot_hwdevident_iface* detector_devcontype_config[]={&linuxinput_iface_obj};




/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////inputlinux:input_drv driver module
/////////////////////////////////////////////////////////////////////////////////
//Driver for /dev/input/eventX input devices abstraction which has EV_KEY and/or EV_LED capabilities, i.e. normal keyboards or other input devices with keys


struct input_drv_instance;

struct input_drv_instance : public iot_device_driver_base {
//	iot_hwdev_ident_buffered dev_ident; //identification of connected hw device
	iot_hwdev_details_linuxinput dev_info; //hw device capabilities reported by detector

	uint32_t keys_state[(KEY_CNT+31)/32]={}; //when EV_KEY capability present, bitmap of current keys state reported by device
	uint16_t leds_state=0; //when EV_LED capability present, bitmap of latest leds state reported by device
	uint16_t sw_state=0; //when EV_SW capability present, bitmap of current switches state
	uint16_t want_leds_bitmap=0; //when EV_LED capability present, bitmap of leds which has been set to either state by node at conn_led
	uint16_t want_leds_state=0; //when EV_LED capability present, bitmap of requested leds state. want_leds_bitmap shows which bits were assigned to particular
								//state by client node. others are copied from current state
	uint8_t snd_state=0; //when EV_SND capability present, bitmap of current snd state

	iot_toneplayer_state* toneplayer=NULL;
	uint64_t tone_stop_after=0;
	uv_timer_t tonetimer_watcher={};
	iot_toneplayer_tone_t current_tone={};
	bool tone_playing=false; //state of ton player
	bool tone_pending=false; //flag that current_tone must be sent to device
	bool started=false;
	bool stopping=false;

	bool have_kbd=false, //iface IOT_DEVIFACETYPEID_KEYBOARD was reported
		have_leds=false, //iface IOT_DEVIFACETYPEID_ACTIVATABLE was reported
		have_tone=false, //iface IOT_DEVIFACETYPEID_TONEPLAYER was reported
		have_sw=false; //iface IOT_DEVIFACETYPEID_HW_SWITCHES was reported

	const iot_conn_drvview *conn_kbd=NULL; //connection with IOT_DEVIFACETYPEID_KEYBOARD if any
	const iot_conn_drvview *conn_leds=NULL; //connection with IOT_DEVIFACETYPEID_ACTIVATABLE if any
	const iot_conn_drvview *conn_tone=NULL; //connection with IOT_DEVIFACETYPEID_TONEPLAYER if any

/////////////OS device communication internals
	char device_path[32];
	int eventfd=-1; //FD of opened /dev/input/eventX or <0 if not opened. 
	uv_tcp_t io_watcher={}; //watcher over eventfd
	uv_timer_t timer_watcher={}; //to count different retries
	h_state_t io_watcher_state=HS_UNINIT;
	enum internal_error_t : uint8_t {
		ERR_NONE=0,
		ERR_OPEN_TEMP=1,
		ERR_EVENT_MGR=2,
	} internal_error=ERR_NONE;
	int error_count=0;
	char internal_error_descr[256]="";
	input_event read_buf[32]; //event struct from <linux/input.h>
	input_event write_buf[32];
	uv_buf_t write_buf_data[1];
	bool write_inprogress=false; //if true, then write_req and write_buf are busy
	bool write_repeat=false; //if true, write_todevice() will be repeated just after finishing current write in progress
	bool resync_state=false;
	uv_write_t write_req;

//These vars keep state of reading process, so must be reset during reconnect
	ssize_t read_buf_offset=0; //used during reading into read_buf in case of partial read of last record. always will be < sizeof(input_event)
	bool read_waitsyn=false; //true if all events from device must be ignored until next EV_SYN
////


/////////////static fields/methods for driver instances management
	static int init_instance(iot_device_driver_base**instance, const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data, iot_devifaces_list* devifaces, json_object *json_params) {
		assert(uv_thread_self()==main_thread);

		//FILTER HW DEVICE CAPABILITIES
		int err=check_device(dev_ident, dev_data);
		if(err) return err;
		const iot_hwdev_details_linuxinput *devinfo=iot_hwdev_details_linuxinput::cast(dev_data);
		//END OF FILTER

		//determine interfaces which this instance can provide for provided hwdevice
		bool have_kbd=false, //iface IOT_DEVCLASSID_KEYBOARD was reported
			have_leds=false, //iface IOT_DEVIFACETYPEID_ACTIVATABLE was reported
			have_tone=false, //iface IOT_DEVCLASSID_BASIC_SPEAKER was reported
			have_sw=false; //iface IOT_DEVCLASSID_HW_SWITCHES was reported

		if(bitmap32_test_bit(&devinfo->cap_bitmap, EV_KEY)) {
			//find max key code in bitmap
			int code=-1;
			for(int i=sizeof(devinfo->keys_bitmap)/sizeof(devinfo->keys_bitmap[0])-1;i>=0;i--)
				if(devinfo->keys_bitmap[i]) {
					for(int j=31;j>=0;j--) if(bitmap32_test_bit(&devinfo->keys_bitmap[i],j)) {code=j+i*32;break;}
					break;
				}
			if(code>=0) {
				bool is_pckbd=bitmap32_test_bit(devinfo->keys_bitmap,KEY_LEFTSHIFT) && bitmap32_test_bit(devinfo->keys_bitmap,KEY_LEFTCTRL);
				iot_deviface_params_keyboard params(is_pckbd, code);
				if(devifaces->add(&params)==0) have_kbd=true;
			}
		}
		if(bitmap32_test_bit(&devinfo->cap_bitmap, EV_LED)) {
			//find max led index in bitmap
			int code=-1;
			if(devinfo->leds_bitmap) {
				for(int j=15;j>=0;j--) if(devinfo->leds_bitmap & (1<<j)) {code=j;break;}
			}
			if(code>=0) {
				iot_deviface_params_activatable params(uint16_t(code+1));
				if(devifaces->add(&params)==0) have_leds=true;
			}
		}
		if(bitmap32_test_bit(&devinfo->cap_bitmap, EV_SND) && (devinfo->snd_bitmap & (1<<SND_TONE))) {
			if(devifaces->add(&iot_deviface_params_toneplayer::object)==0) have_tone=true;
		}
//		if(bitmap32_test_bit(&devinfo->cap_bitmap, EV_SW)) {if(devifaces->add(IOT_DEVIFACETYPEID_HW_SWITCHES, NULL)==0) have_sw=true;}

		if(!devifaces->num) return IOT_ERROR_NOT_SUPPORTED;

		input_drv_instance *inst=new input_drv_instance(dev_ident, dev_data, have_kbd, have_leds, have_tone, have_sw);
		if(!inst) return IOT_ERROR_TEMPORARY_ERROR;

		*instance=inst;

		char descr[256]="";
		char buf[128];
		int off=0;
		for(unsigned i=0;i<devifaces->num;i++) {
			const iot_deviface_params *iface=devifaces->items[i].data;
			if(!iface || !iface->is_valid()) {
//				assert(false);
				continue;
			}
			off+=snprintf(descr+off, sizeof(descr)-off, "%s%s", i==0 ? "" : ", ", iface->sprint(buf,sizeof(buf)));
			if(off>=int(sizeof(descr))) break;
		}
		kapi_outlog_info("Driver inited for device with name='%s', contype=%s, devifaces='%s'", devinfo->name, dev_ident->local->sprint(buf,sizeof(buf)), descr);
		return 0;
	}

	//called to deinit instance.
	//Return values:
	//0 - success
	//any other error leaves instance in hang state
	static int deinit_instance(iot_device_driver_base* instance) {
		assert(uv_thread_self()==main_thread);
		input_drv_instance *inst=static_cast<input_drv_instance*>(instance);
		inst->unref();;

		return 0;
	}
	static int check_device(const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data) {
		if(!iot_hwdev_localident_linuxinput::cast(dev_ident->local)) return IOT_ERROR_NOT_SUPPORTED;
		if(!dev_data || !iot_hwdev_details_linuxinput::cast(dev_data)) return IOT_ERROR_INVALID_DEVICE_DATA; //devcontype_linuxinput_t has fixed size
//		devcontype_linuxinput_t *devinfo=(devcontype_linuxinput_t*)(dev_data->custom_data);
//		if(!(devinfo->cap_bitmap & (EV_KEY | EV_LED))) return IOT_ERROR_NOT_SUPPORTED; NOW SUPPORT ALL DEVICES DETECTED BY OUR DETECTOR
		return 0;
	}
/////////////public methods


private:
	input_drv_instance(const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data, bool have_kbd, bool have_leds, bool have_tone, bool have_sw): 
			have_kbd(have_kbd), have_leds(have_leds), have_tone(have_tone), have_sw(have_sw)
	{
//		memcpy(&dev_ident, &dev_data->dev_ident, sizeof(dev_ident));

//		assert(dev_data->dev_ident.dev.contype==DEVCONTYPE_CUSTOM_LINUXINPUT);
		const iot_hwdev_details_linuxinput* data=iot_hwdev_details_linuxinput::cast(dev_data);
		assert(data!=NULL);
		dev_info=*data;
	}
	virtual ~input_drv_instance(void) {
	}

//iot_module_instance_base methods


	//Called to start work of previously inited instance.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, iot_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(void) {
		assert(uv_thread_self()==thread);

		uv_timer_init(loop, &timer_watcher);
		timer_watcher.data=this;
		ref(); //instance is now additionally reffered in uv handle, so increase refcount to have correct closing of handle

		uv_timer_init(loop, &tonetimer_watcher);
		tonetimer_watcher.data=this;
		ref(); //instance is now additionally reffered in uv handle, so increase refcount to have correct closing of handle

		int err=setup_device_polling();
		if(err<0) {
			stop();
			return err;
		}
		started=true;
		return 0;
	}
	//called to stop work of started instance. call can be followed by deinit or started again (if stop was manual, by user)
	//Return values:
	//0 - driver successfully stopped and can be deinited or restarted
	//IOT_ERROR_TRY_AGAIN - driver requires some time (async operation) to stop gracefully. kapi_self_abort() will be called to notify core when stop is finished.
	//						anyway second stop() call must free all resources correctly, may be in a hard way. otherwise module will be blocked and left in hang state (deinit
	//						cannot be called until stop reports OK)
	//any other error is treated as critical bug and driver is blocked for further starts. deinit won't be called for such instance. instance is put into hang state
	virtual int stop(void) {
		assert(uv_thread_self()==thread);
		if(!stopping && tone_playing && io_watcher_state==HS_ACTIVE) {
			stopping=true;
			toneplay_stop();
			return IOT_ERROR_TRY_AGAIN;
		}
		started=false;

		stop_device_polling(false);

		uv_close((uv_handle_t*)&timer_watcher, kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'
		uv_close((uv_handle_t*)&tonetimer_watcher, kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'
		return 0;
	}

//iot_device_driver_base methods
	virtual int device_open(const iot_conn_drvview* conn) {
		assert(uv_thread_self()==thread);
		kapi_notify_write_avail(conn, true);
		const iot_devifacetype_metaclass* ifacetype=conn->deviface->get_metaclass();
		if(ifacetype==&iot_devifacetype_metaclass_keyboard::object) {
			if(!have_kbd) return IOT_ERROR_NOT_SUPPORTED;
			if(conn_kbd) return IOT_ERROR_LIMIT_REACHED;
			conn_kbd=conn;

			iot_deviface__keyboard_DRV iface(conn);
			int err=iface.send_set_state(keys_state);
			assert(err==0);
		} else if(ifacetype==&iot_devifacetype_metaclass_activatable::object) {
			if(!have_leds) return IOT_ERROR_NOT_SUPPORTED;
			if(conn_leds) return IOT_ERROR_LIMIT_REACHED;
			conn_leds=conn;
			want_leds_bitmap=want_leds_state=0;

			iot_deviface__activatable_DRV iface(conn);
			int err=iface.send_current_state(leds_state, dev_info.leds_bitmap);
			assert(err==0);
		} else if(ifacetype==&iot_devifacetype_metaclass_toneplayer::object) {
			if(!have_tone) return IOT_ERROR_NOT_SUPPORTED;
			if(conn_tone) return IOT_ERROR_LIMIT_REACHED;

			//check current state of device
			if(eventfd>=0) { //device handle opened
				if(ioctl(eventfd, EVIOCGSND(sizeof(snd_state)), &snd_state)==-1) { //get current snd state
					kapi_outlog_error("Cannot ioctl '%s' for EVIOCGSND: %s", device_path, uv_strerror(uv_translate_sys_error(errno)));
					close(eventfd);
					eventfd=-1;
					kapi_self_abort(IOT_ERROR_CRITICAL_ERROR);
					return IOT_ERROR_TEMPORARY_ERROR;
				}
				if(snd_state & ((1<<SND_TONE)|(1<<SND_BELL))) { //set request to stop playing
					current_tone={};
					tone_pending=true;
				}
			}

			toneplayer=new iot_toneplayer_state;
			if(!toneplayer) {
				kapi_outlog_notice("Cannot allocate memory for toneplayer state");
				return IOT_ERROR_TEMPORARY_ERROR;
			}
			conn_tone=conn;

		} else {
			return IOT_ERROR_NOT_SUPPORTED;
		}
		return 0;
	}
	virtual int device_close(const iot_conn_drvview* conn) {
		assert(uv_thread_self()==thread);
		const iot_devifacetype_metaclass* ifacetype=conn->deviface->get_metaclass();
		if(ifacetype==&iot_devifacetype_metaclass_keyboard::object) {
			if(conn==conn_kbd) conn_kbd=NULL;
		} else if(ifacetype==&iot_devifacetype_metaclass_activatable::object) {
			if(conn==conn_leds) {
				conn_leds=NULL;
				want_leds_bitmap=want_leds_state=0;
			}
		} else if(ifacetype==&iot_devifacetype_metaclass_toneplayer::object) {
			if(conn==conn_tone) {
				conn_tone=NULL;
				delete toneplayer;
				toneplayer=NULL;
			}
		}
		return 0;
	}
	virtual int device_action(const iot_conn_drvview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) {
		assert(uv_thread_self()==thread);
		int err;
//		if(action_code==IOT_DEVCONN_ACTION_CANWRITE) {
//		} else 
		if(action_code==IOT_DEVCONN_ACTION_FULLREQUEST) {
			if(conn==conn_kbd) {
				iot_deviface__keyboard_DRV iface(conn);
				const iot_deviface__keyboard_DRV::msg* msg=iface.parse_req(data, data_size);
				if(!msg) return IOT_ERROR_MESSAGE_IGNORED;

				if(msg->req_code==iface.REQ_GET_STATE) {
					err=iface.send_set_state(keys_state);
					assert(err==0);
					return 0;
				}
				return IOT_ERROR_MESSAGE_IGNORED;
			} else if(conn==conn_leds) {
				iot_deviface__activatable_DRV iface(conn);
				const iot_deviface__activatable_DRV::reqmsg* msg=iface.parse_req(data, data_size);
				if(!msg) return IOT_ERROR_MESSAGE_IGNORED;

				if(msg->req_code==iface.REQ_GET_STATE) {
					kapi_outlog_info("Driver GOT leds GET STATE");
					err=iface.send_current_state(leds_state, dev_info.leds_bitmap);
					assert(err==0);
					return 0;
				}
				else if(msg->req_code==iface.REQ_SET_STATE) {
					kapi_outlog_info("Driver GOT leds SET STATE activate=%04x, deactivate=%04x", msg->activate_mask, msg->deactivate_mask);
					uint16_t activate_mask=msg->activate_mask & dev_info.leds_bitmap, deactivate_mask=msg->deactivate_mask & dev_info.leds_bitmap;

					want_leds_bitmap = activate_mask | deactivate_mask;
					uint16_t clr=activate_mask & deactivate_mask; //find bits set in both masks
					if(clr) { //reset common bits
						activate_mask&=~clr;
						deactivate_mask&=~clr;
					}
					want_leds_state=uint16_t((want_leds_state | activate_mask) & ~deactivate_mask); //set activated bits, reset deactivated, reset invalid

/*					if(eventfd>=0) { //device handle opened
						if(ioctl(eventfd, EVIOCGLED(sizeof(leds_state)), &leds_state)==-1) { //get current led state
							kapi_outlog_error("Cannot ioctl '%s' for EVIOCGLED: %s", device_path, uv_strerror(uv_translate_sys_error(errno)));
							stop_device_polling(true);
							return 0;
						}
						want_leds_state=(want_leds_state & want_leds_bitmap) | (leds_state & ~want_leds_bitmap);
					}
*/
					write_todevice();
					return 0;
				}
				return IOT_ERROR_MESSAGE_IGNORED;
			} else if(conn==conn_tone) {
				iot_deviface__toneplayer_DRV iface(conn);
				iot_deviface__toneplayer_DRV::req_t req;
				const void* obj=iface.parse_req(data, data_size, req);
				if(!obj) return IOT_ERROR_MESSAGE_IGNORED;
				switch(req) {
					case iface.REQ_SET_SONG: {
						auto song=(iot_deviface__toneplayer_DRV::req_set_song*)obj;
						err=toneplayer->set_song(song->index, song->title, song->num_tones, song->tones);
						if(err<0) {
							kapi_outlog_notice("Got error: %s", kapi_strerror(err));
							return IOT_ERROR_MESSAGE_IGNORED;
						}
						break;
					}
					case iface.REQ_UNSET_SONG: {
						auto song=(iot_deviface__toneplayer_DRV::req_unset_song*)obj;
						toneplayer->unset_song(song->index);
						break;
					}
					case iface.REQ_PLAY: {
						auto play=(iot_deviface__toneplayer_DRV::req_play*)obj;
						toneplayer->set_playmode(play->mode);
						toneplayer->rewind(play->song_index, play->tone_index);
						if(play->stop_after) tone_stop_after=uv_now(loop)+play->stop_after*1000;
							else tone_stop_after=0;
						toneplay_continue();
						break;
					}
					case iface.REQ_STOP:
						toneplay_stop();
						break;
					case iface.REQ_GET_STATUS: {
						iot_toneplayer_status_t st;
						toneplayer->get_status(&st);
						st.is_playing=tone_playing;
						err=iface.send_status(&st);
						assert(err==0);
						break;
					}
				}
				return 0;
			}
		}// else if(action_code==IOT_DEVCONN_ACTION_READY) {
//			if(conn->id==connid_kbd) {
//				iot_deviface__keyboard_DRV iface(attr_kbd);
//				err=iface.send_set_state(connid_kbd, this, keys_state);
//				assert(err==0);
//			}
//		}
		kapi_outlog_info("Device action in driver inst %u, act code %u, datasize %u from device index %d", miid.iid, unsigned(action_code), data_size, int(conn->index));
		return 0;
	}

	void toneplay_continue(void) {
		assert(toneplayer!=NULL);
		if(tone_stop_after>0 && uv_now(loop)>=tone_stop_after) {toneplay_stop(); return;}
		const iot_toneplayer_tone_t *tone=toneplayer->get_nexttone();
		if(!tone) {toneplay_stop(); return;}
		current_tone=*tone;
		tone_pending=true;
		write_todevice();
	}
	void toneplay_stop(void) {
		if(!tone_playing) return;
		current_tone={};
		tone_pending=true;
		write_todevice();
	}


	//recreates polling procedure initially or after device disconnect
	int setup_device_polling(void) {
		assert(uv_thread_self()==thread);

		uv_timer_stop(&timer_watcher);
		int retrytm=0; //assigned in case of temp error to set retry period
		internal_error_t temperr=ERR_NONE; //assigned in case of temp error to set internal error
		int err;
		bool criterror=false;
		do {
			if(eventfd<0) { //open device file
				if(io_watcher_state!=HS_UNINIT) { //io handle still not closed, retry on next event loop iteration
					temperr=ERR_EVENT_MGR;
					retrytm=0;
					break;
				}

				snprintf(device_path,sizeof(device_path),"/dev/input/event%d",dev_info.event_index);
				eventfd=open(device_path, O_RDWR | O_NONBLOCK);
				if(eventfd<0) {
					err=errno;
					kapi_outlog_error("Cannot open '%s': %s", device_path, uv_strerror(uv_translate_sys_error(err)));
					if(err==ENOENT || err==ENXIO || err==ENODEV) {criterror=true;break;} //driver instance should be deinited
					temperr=ERR_OPEN_TEMP;
					retrytm=10;
					snprintf(internal_error_descr, sizeof(internal_error_descr), "Cannot open '%s': %s", device_path, uv_strerror(uv_translate_sys_error(err)));
					break;
				}

				uv_tcp_init(loop, &io_watcher);
				io_watcher_state=HS_INIT;
				io_watcher.data=this;
				ref();
				//attach device file FD to io watcher
				err=uv_tcp_open(&io_watcher, eventfd);
				if(err<0) {
					kapi_outlog_error("Cannot open uv stream: %s", uv_strerror(err));
					temperr=ERR_EVENT_MGR;
					retrytm=30;
					snprintf(internal_error_descr, sizeof(internal_error_descr), "Cannot open event listener: %s", uv_strerror(err));
					break;
				}

				//recheck caps of opened device
				iot_hwdev_details_linuxinput dev_info2;
				const char* errstr=dev_info2.read_inputdev_caps(eventfd, dev_info.event_index);
				if(errstr) {
					kapi_outlog_error("Cannot %s on '%s': %s", errstr, device_path, uv_strerror(uv_translate_sys_error(errno)));
					criterror=true;
					break;
				}
				if(dev_info!=dev_info2) { //another device was connected?
					kapi_outlog_info("Another device '%s' on '%s'", dev_info2.name, device_path);
					criterror=true;
					break;
				}

				//get current state of events
				if(bitmap32_test_bit(&dev_info.cap_bitmap, EV_KEY)) { //has EV_KEY cap
					if(ioctl(eventfd, EVIOCGKEY(sizeof(keys_state)), keys_state)==-1) { //get current keys state
						kapi_outlog_error("Cannot ioctl '%s' for EVIOCGKEY: %s", device_path, uv_strerror(uv_translate_sys_error(errno)));
						criterror=true;
						break;
					}
				}
				if(bitmap32_test_bit(&dev_info.cap_bitmap, EV_LED)) { //has EV_LED cap
					if(ioctl(eventfd, EVIOCGLED(sizeof(leds_state)), &leds_state)==-1) { //get current led state
						kapi_outlog_error("Cannot ioctl '%s' for EVIOCGLED: %s", device_path, uv_strerror(uv_translate_sys_error(errno)));
						criterror=true;
						break;
					}
//					if(conn_leds) want_leds_state=(want_leds_state & want_leds_bitmap) | (leds_state & ~want_leds_bitmap);
				}
				if(bitmap32_test_bit(&dev_info.cap_bitmap, EV_SW)) { //has EV_SW cap
					if(ioctl(eventfd, EVIOCGSW(sizeof(sw_state)), &sw_state)==-1) { //get current switches state
						kapi_outlog_error("Cannot ioctl '%s' for EVIOCGSW: %s", device_path, uv_strerror(uv_translate_sys_error(errno)));
						criterror=true;
						break;
					}
				}
				if(bitmap32_test_bit(&dev_info.cap_bitmap, EV_SND)) { //has EV_SND cap
					if(ioctl(eventfd, EVIOCGSND(sizeof(snd_state)), &snd_state)==-1) { //get current snd state
						kapi_outlog_error("Cannot ioctl '%s' for EVIOCGSND: %s", device_path, uv_strerror(uv_translate_sys_error(errno)));
						criterror=true;
						break;
					}
					if(conn_tone && !tone_pending) { //explicit stop if not playing
						current_tone={};
						tone_pending=true;
					}
				}
			}

			read_buf_offset=0;
			read_waitsyn=false;
			write_inprogress=false;
			//start polling for read events on device
			err=uv_read_start((uv_stream_t*)&io_watcher, &dev_alloc_cb, [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)->void{
				static_cast<input_drv_instance*>(stream->data)->dev_onread(nread);
			});
			if(err<0) {
				kapi_outlog_error("Cannot start read on uv stream: %s", uv_strerror(err));
				temperr=ERR_EVENT_MGR;
				retrytm=30;
				snprintf(internal_error_descr, sizeof(internal_error_descr), "Cannot set read event listener: %s", uv_strerror(err));
				break;
			}
			io_watcher_state=HS_ACTIVE;
			write_todevice();
			return 0;
		} while(0);
		//common error processing
		stop_device_polling(criterror, temperr, retrytm);
		if(criterror) return IOT_ERROR_CRITICAL_ERROR; //driver instance should be deinited
		return 0;
	}
	void stop_device_polling(bool criterror, internal_error_t temperr=ERR_NONE, int retrytm=30) {
		if(io_watcher_state>=HS_INIT) {
			io_watcher_state=HS_CLOSING;
			uv_close((uv_handle_t*)&io_watcher, [](uv_handle_t* handle)->void {
				input_drv_instance* inst=(input_drv_instance*)handle->data;
				inst->io_watcher_state=HS_UNINIT;
				inst->kapi_process_uv_close(handle);
			});
		}
		if(eventfd>=0) {
			close(eventfd);
			eventfd=-1;
		}

		if(criterror) {
			if(started) kapi_self_abort(IOT_ERROR_CRITICAL_ERROR);
			return;
		}

		if(temperr==ERR_NONE) return; //no errors, just stopped polling and closed device

		if(temperr==internal_error) error_count++;
		else {
			internal_error=temperr;
			error_count=1;
		}
		uv_timer_stop(&timer_watcher);
		uv_timer_start(&timer_watcher, on_timer_static, (error_count>10 ? 10 : error_count)*retrytm*1000, 0);
	}

	static void on_timer_static(uv_timer_t* handle) {
		input_drv_instance* obj=static_cast<input_drv_instance*>(handle->data);
		obj->on_timer();
	}
	static void on_tonetimer_static(uv_timer_t* handle) {
		input_drv_instance* obj=static_cast<input_drv_instance*>(handle->data);
		obj->toneplay_continue();
	}
	void on_timer(void) {
		int err;
		if(io_watcher_state==HS_ACTIVE) {
			if(resync_state) {
				resync_state=false;
				if(bitmap32_test_bit(&dev_info.cap_bitmap, EV_KEY)) { //has EV_KEY cap
					if(ioctl(eventfd, EVIOCGKEY(sizeof(keys_state)), keys_state)==-1) { //get current keys state
						kapi_outlog_error("Cannot ioctl '%s' for EVIOCGKEY: %s", device_path, uv_strerror(uv_translate_sys_error(errno)));
						stop_device_polling(true);
						return;
					}
					if(conn_kbd) {
						iot_deviface__keyboard_DRV iface(conn_kbd);
						err=iface.send_set_state(keys_state);
						assert(err==0);
						kapi_outlog_debug("Resyncing key state from device '%s'", device_path);
					}
				}
				if(bitmap32_test_bit(&dev_info.cap_bitmap, EV_LED)) { //has EV_LED cap
					if(ioctl(eventfd, EVIOCGLED(sizeof(leds_state)), &leds_state)==-1) { //get current led state
						kapi_outlog_error("Cannot ioctl '%s' for EVIOCGLED: %s", device_path, uv_strerror(uv_translate_sys_error(errno)));
						stop_device_polling(true);
						return;
					}
//					if(conn_leds) {
//						want_leds_state=(want_leds_state & want_leds_bitmap) | (leds_state & ~want_leds_bitmap);
//					}
				}
			}
			write_todevice();
			return;
		}
		setup_device_polling();
	}


	void write_todevice(void) {
		if(io_watcher_state!=HS_ACTIVE) return; //currently no connection to device
		if(write_inprogress) { //write request already pending. it can be already made but not notified, so schedule recheck
			write_repeat=true;
			return;
		}
		unsigned idx=0;
		if(conn_leds) {
			//refresh leds state
			if(ioctl(eventfd, EVIOCGLED(sizeof(leds_state)), &leds_state)==-1) {
				kapi_outlog_error("Cannot ioctl '%s' for EVIOCGLED: %s", device_path, uv_strerror(uv_translate_sys_error(errno)));
				stop_device_polling(true);
				return;
			}
			if((leds_state & want_leds_bitmap) != (want_leds_state & want_leds_bitmap)) { //leds state must be updated
				uint16_t dif=(leds_state ^ want_leds_state) & want_leds_bitmap;
				for(int j=0;j<=15;j++) {
					if(dif & (1<<j)) {
						write_buf[idx++]={time:{}, type: EV_LED, code: uint16_t(j), value: (want_leds_state & (1<<j)) ? 1 : 0};
					}
				}
			}
		}
		if(tone_pending) {
			write_buf[idx++]={time:{}, type: EV_SND, code: SND_TONE, value: current_tone.freq};
		}
		if(!idx) return;
		write_buf[idx++]={time:{}, type: EV_SYN, code: SYN_REPORT, value: 0};
		write_buf_data[0].base=(char*)write_buf;
		write_buf_data[0].len=idx*sizeof(write_buf[0]);
		int err=uv_write(&write_req, (uv_stream_t*)&io_watcher, write_buf_data, 1, [](uv_write_t* req, int status)->void{
			static_cast<input_drv_instance*>(req->data)->dev_onwrite(status);
		});
		if(!err) {
			write_req.data=this;
			leds_state=want_leds_state;
			if(tone_pending) {
				uv_timer_stop(&tonetimer_watcher);
				if(current_tone.len>0) {
					uv_timer_start(&tonetimer_watcher, on_tonetimer_static, 1000u*current_tone.len/32, 0);
					tone_playing=true;
				} else tone_playing=false;
				tone_pending=false;
			}
			write_inprogress=true;
			write_repeat=false;
			return;
		}
		//error
		kapi_outlog_error("Cannot write uv request: %s", uv_strerror(err));
		snprintf(internal_error_descr, sizeof(internal_error_descr), "Cannot add write request: %s", uv_strerror(err));

		if(ERR_EVENT_MGR==internal_error) error_count++;
		else {
			internal_error=ERR_EVENT_MGR;
			error_count=1;
		}
		uv_timer_stop(&timer_watcher);
		uv_timer_start(&timer_watcher, on_timer_static, (error_count>10 ? 10 : error_count)*10*1000, 0);
	}


	void dev_onwrite(int status) {
		assert(write_inprogress);
		write_inprogress=false;
		if(stopping) {
			kapi_self_abort(0);
			return;
		}
		if(status<0) {
			kapi_outlog_error("Error writing request: %s", uv_strerror(status));
			stop_device_polling(false, ERR_EVENT_MGR, 0);
			return;
		}
		if(write_repeat) write_todevice();
	}

	void dev_onread(ssize_t nread) {
		if(nread<=0) { //error
			if(nread==0) return; //EAGAIN
			kapi_outlog_error("Error reading uv stream: %s", uv_strerror(nread));
			stop_device_polling(false, ERR_EVENT_MGR, 0);
			return;
		}
		int idx=0;
		while(nread>=int(sizeof(input_event))) {
			process_device_event(&read_buf[idx]);
			nread-=sizeof(input_event);
			idx++;
		}
		read_buf_offset=nread; //can be >0 if last record was not read in full
		if(nread>0) { //there is partial data
			memcpy(&read_buf[0], &read_buf[idx], nread);
		}
	}
	void process_device_event(input_event* ev) {
		int err;
		if(read_waitsyn) {
			if(ev->type==EV_SYN) {
				read_waitsyn=false;
				resync_state=true;
				uv_timer_stop(&timer_watcher);
				uv_timer_start(&timer_watcher, on_timer_static, 0, 0);
			}

			return;
		}
		switch(ev->type) {
			case EV_SYN:
				if(ev->code==SYN_DROPPED) read_waitsyn=true;
				break;
			case EV_KEY: //key event
				assert(ev->code<sizeof(keys_state)*8);
				if(ev->value==1) { //down
					bitmap32_set_bit(keys_state, ev->code);
				} else if(ev->value==0) {
					bitmap32_clear_bit(keys_state, ev->code);
				}
				if(conn_kbd) {
					iot_deviface__keyboard_DRV iface(conn_kbd);
					switch(ev->value) {
						case 0:
							err=iface.send_keyup(ev->code, keys_state);
							break;
						case 1:
							err=iface.send_keydown(ev->code, keys_state);
							break;
						case 2:
//							err=iface.send_keyrepeat(ev->code, keys_state);
							break;
						default:
							err=0;
							break;
					}
					if(err) {
						//TODO remember
					} else {
						//TODO remember dropped message state
					}
				}
				kapi_outlog_debug("Key with code %d is %s", ev->code, ev->value == 1 ? "down" : ev->value==0 ? "up" : "repeated");
				break;
			case EV_LED: //led event
				assert(ev->code<16);
				if(ev->value == 1) leds_state|=(1u<<ev->code);
					else leds_state&=~(1u<<ev->code);
				kapi_outlog_info("LED with code %d is %s", ev->code, ev->value == 1 ? "on" : "off");
				uv_timer_stop(&timer_watcher);
				uv_timer_start(&timer_watcher, on_timer_static, 0, 0);
				break;
			case EV_SW: //led event
				kapi_outlog_info("SW with code %d valued %d", ev->code, (int)ev->value);
				break;
			case EV_SND:
				kapi_outlog_info("SND with code %d valued %d", ev->code, (int)ev->value);
				break;
			case EV_MSC: //MISC event. ignore
				break;
			default:
				kapi_outlog_info("Unhandled event with type %d, code %d valued %d", (int)ev->type, (int)ev->code, (int)ev->value);
				break;
		}
	}
	static void dev_alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
		input_drv_instance *inst=(input_drv_instance*)(handle->data);
		buf->base=((char*)inst->read_buf)+inst->read_buf_offset;
		buf->len=sizeof(inst->read_buf)-inst->read_buf_offset;
	}

};

//	iot_hwdev_localident_linuxinput(uint8_t event_index, uint16_t bustype, uint16_t vendor, uint16_t product, uint16_t version, uint32_t cap_bitmap, const char* phys);
//	iot_hwdev_localident_linuxinput(uint32_t bustypemask, uint32_t cap_bitmap, uint8_t num_specs, const spec_t (&spec)[8]);


static const iot_hwdev_localident_linuxinput devfilter1(0,0,0,{});
static const iot_hwdev_localident* driver_devidents[]={
	&devfilter1
};

static const iot_devifacetype_metaclass* driver_ifaces[]={
	&iot_devifacetype_metaclass_keyboard::object,
	&iot_devifacetype_metaclass_activatable::object,
	&iot_devifacetype_metaclass_toneplayer::object
};


iot_driver_moduleconfig_t IOT_DRIVER_MODULE_CONF(drv)={
	.version = IOT_VERSION_COMPOSE(0,0,1),

	.cpu_loading = 3,
	.num_hwdev_idents = sizeof(driver_devidents)/sizeof(driver_devidents[0]),
	.num_dev_ifaces = sizeof(driver_ifaces)/sizeof(driver_ifaces[0]),
	.is_detector = 0,

	.hwdev_idents = driver_devidents,
	.dev_ifaces = driver_ifaces,

	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &input_drv_instance::init_instance,
	.deinit_instance = &input_drv_instance::deinit_instance,
	.check_device = &input_drv_instance::check_device
};



//end of kbdlinux:input_drv driver module






//Windows TODO:
/*
HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\services\kbdclass\Enum
contains list of keyboards

 device =\Device\KeyBoardClass0
    SetUnicodeStr(fn,device) 
    h_device:=NtCreateFile(fn,0+0x00000100+0x00000080+0x00100000,1,1,0x00000040+0x00000020,0)
    }

  VarSetCapacity( output_actual, 4, 0 )
  input_size = 4
  VarSetCapacity( input, input_size, 0 )

  If Cmd= switch  ;switches every LED according to LEDvalue
   KeyLED:= LEDvalue
  If Cmd= on  ;forces all choosen LED's to ON (LEDvalue= 0 ->LED's according to keystate)
   KeyLED:= LEDvalue | (GetKeyState("ScrollLock", "T") + 2*GetKeyState("NumLock", "T") + 4*GetKeyState("CapsLock", "T"))
  If Cmd= off  ;forces all choosen LED's to OFF (LEDvalue= 0 ->LED's according to keystate)
    {
    LEDvalue:= LEDvalue ^ 7
    KeyLED:= LEDvalue & (GetKeyState("ScrollLock", "T") + 2*GetKeyState("NumLock", "T") + 4*GetKeyState("CapsLock", "T"))
    }
  ; EncodeInteger( KeyLED, 1, &input, 2 ) ;input bit pattern (KeyLED): bit 0 = scrolllock ;bit 1 = numlock ;bit 2 = capslock
  input := Chr(1) Chr(1) Chr(KeyLED)
  input := Chr(1)
  input=
  success := DllCall( "DeviceIoControl"
              , "uint", h_device
              , "uint", CTL_CODE( 0x0000000b     ; FILE_DEVICE_KEYBOARD
                        , 2
                        , 0             ; METHOD_BUFFERED
                        , 0  )          ; FILE_ANY_ACCESS
              , "uint", &input
              , "uint", input_size
              , "uint", 0
              , "uint", 0
              , "uint", &output_actual
              , "uint", 0 )
}

CTL_CODE( p_device_type, p_function, p_method, p_access )
{
  Return, ( p_device_type << 16 ) | ( p_access << 14 ) | ( p_function << 2 ) | p_method
}
*/

