#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>
#include<stdlib.h>
#include<unistd.h>
#include<assert.h>

#include<uv.h>

#include <linux/input.h>

#include<iot_utils.h>
#include<iot_error.h>


#define IOT_VENDOR unet
#define IOT_BUNDLE generic__inputlinux

#include<iot_module.h>

#include<iot_devclass_keyboard.h>


//#define DRVNAME drv

//list of modules in current bundle, their registered IDs. TODO: put real IDs when they will be allocated through some table in unetcommonsrc
#define BUNDLE_MODULES_MAP(XX) \
	XX(detector, 1)  \
	XX(input_drv, 2)   \
	XX(kbd_src, 3) \
	XX(kbdled_exor, 4) \
	XX(cond_keyop, 5)


//build constants like MODULEID_detector which resolve to registered module id
enum {
#define XX(name, id) MODULEID_ ## name = id,
	BUNDLE_MODULES_MAP(XX)
#undef XX
};


// "/dev/input/eventX" device on linuxes
#define DEVCONTYPE_CUSTOM_LINUXINPUT	IOT_DEVCONTYPE_CUSTOM(MODULEID_detector, 1)
#define DEVCONTYPESTR_CUSTOM_LINUXINPUT	"LinuxInput"



struct devcontype_linuxinput_t { //represents custom data for devices with DEVCONTYPE_CUSTOM_LINUXINPUT connection type
	char name[256];
	input_id input; //__u16 bustype;__u16 vendor;__u16 product;__u16 version;

	uint32_t cap_bitmap; //bitmap of available capabilities. we process: EV_KEY, EV_LED, EV_SW, EV_SND
	uint32_t keys_bitmap[(KEY_CNT+31)/32]; //when EV_KEY capability present, bitmap of available keys as reported by driver (this is NOT physically present buttons but they can be present)
	uint16_t sw_bitmap; //when EV_SW capability present, bitmap of available switch events
	uint16_t leds_bitmap; //when EV_LED capability present, bitmap of available leds as reported by driver (this is NOT physically present buttons but they can be present)
	uint8_t snd_bitmap; //when EV_SND capability present, bitmap of available sound capabilities
	uint8_t event_index; //X in /dev/input/eventX
};
//unique_refid is just event_index in lower byte and zeros in others


//common functions
//fills devcontype_linuxinput_t struct with input device capabilities
//returns NULL on success or error descr on error (with errno properly set to OS error)
static const char* read_inputdev_caps(int fd, uint8_t index, devcontype_linuxinput_t* cur_dev) {
	memset(cur_dev, 0, sizeof(*cur_dev));
	cur_dev->event_index=index;

	const char* errstr=NULL;
	do { //create block for common error processing
		if(ioctl(fd, EVIOCGID, &cur_dev->input)==-1) {errstr="ioctl for id";break;} //get bus, vendor, product, version

		if(ioctl(fd, EVIOCGNAME(sizeof(cur_dev->name)), cur_dev->name)==-1) {errstr="ioctl for name";break;} //get name
		if(!cur_dev->name[0]) {
			strcpy(cur_dev->name,"N/A"); //ensure name is not empty
		} else {
			cur_dev->name[sizeof(cur_dev->name)-1]='\0'; //ensure NUL-terminated
		}

		if(ioctl(fd, EVIOCGBIT(0,sizeof(cur_dev->cap_bitmap)), &cur_dev->cap_bitmap)==-1) {errstr="ioctl for cap bitmap";break;} //get capability bitmap
		if(bitmap32_test_bit(&cur_dev->cap_bitmap, EV_KEY)) { //has EV_KEY cap
			if(ioctl(fd, EVIOCGBIT(EV_KEY,sizeof(cur_dev->keys_bitmap)), cur_dev->keys_bitmap)==-1) {errstr="ioctl for keys bitmap";break;} //get available keys bitmap
		}
		if(bitmap32_test_bit(&cur_dev->cap_bitmap, EV_LED)) { //has EV_LED cap
			if(ioctl(fd, EVIOCGBIT(EV_LED,sizeof(cur_dev->leds_bitmap)), &cur_dev->leds_bitmap)==-1) {errstr="ioctl for leds bitmap";break;} //get available leds bitmap
		}
		if(bitmap32_test_bit(&cur_dev->cap_bitmap, EV_SW)) { //has EV_SW cap
			if(ioctl(fd, EVIOCGBIT(EV_SW,sizeof(cur_dev->sw_bitmap)), &cur_dev->sw_bitmap)==-1) {errstr="ioctl for sw bitmap";break;} //get available switch events bitmap
		}
		if(bitmap32_test_bit(&cur_dev->cap_bitmap, EV_SND)) { //has EV_SND cap
			if(ioctl(fd, EVIOCGBIT(EV_SND,sizeof(cur_dev->snd_bitmap)), &cur_dev->snd_bitmap)==-1) {errstr="ioctl for snd bitmap";break;} //get available sound caps bitmap
		}
	} while(0);

	return errstr;
}


/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////inputlinux:detector Device Detector module
/////////////////////////////////////////////////////////////////////////////////


//period of checking for available keyboards, in milliseconds
#define DETECTOR_POLL_INTERVAL 5000
//maximum event devices for detection
#define DETECTOR_MAX_DEVS 32

class detector;
detector* detector_obj=NULL;

class detector {
	bool is_started;
	uv_timer_t timer_watcher;
	int devinfo_len; //number of filled items in devinfo array
	struct devinfo_t { //short device info indexed by event_index field. minimal info necessary to determine change of device
		input_id input;
		uint32_t cap_bitmap;
		bool present;
	} devinfo[DETECTOR_MAX_DEVS];

	void on_timer(void) {
		devcontype_linuxinput_t fulldevinfo[DETECTOR_MAX_DEVS];
		int n=get_event_devices(fulldevinfo, DETECTOR_MAX_DEVS);
		if(n==0 && devinfo_len==0) return; //nothing to do

		iot_hwdev_localident_t ident;
		ident.detector_module_id=MODULEID_detector;
		ident.contype=DEVCONTYPE_CUSTOM_LINUXINPUT;

		int i, err;
		int max_n = n>=devinfo_len ? n : devinfo_len;

printf("Found %d devices, was %d\n",n, devinfo_len);
		for(i=0;i<max_n;i++) { //compare if actual devices changed for common indexes
			ident.unique_refid=uint64_t(i);
			if(i<devinfo_len && devinfo[i].present) {
				if(i>=n || !fulldevinfo[i].name[0]) { //new state is absent, so device was removed
					kapi_outlog_info("Hwdevice was removed: type=" DEVCONTYPESTR_CUSTOM_LINUXINPUT ", input=%d",i);
					err=kapi_hwdev_registry_action(IOT_ACTION_REMOVE, &ident, 0, NULL);
					if(err>=0) {
						devinfo[i].present=false;
					} else {
						kapi_outlog_error("Cannot remove device from registry: %s", kapi_strerror(err));
					}
					continue;
				}
				//check if device looks the same
				if(devinfo[i].cap_bitmap==fulldevinfo[i].cap_bitmap && memcmp(&devinfo[i].input,&fulldevinfo[i].input,sizeof(devinfo[i].input))==0) continue; //same

				kapi_outlog_debug("bitmap %08x != %08x OR vendor:model %04x:%04x != %04x:%04x", devinfo[i].cap_bitmap, fulldevinfo[i].cap_bitmap, unsigned(devinfo[i].input.vendor),unsigned(devinfo[i].input.product), unsigned(fulldevinfo[i].input.vendor),unsigned(fulldevinfo[i].input.product));

				kapi_outlog_info("Hwdevice was replaced: type=" DEVCONTYPESTR_CUSTOM_LINUXINPUT ", input=%d, new name='%s'",i, fulldevinfo[i].name);
				
				err=kapi_hwdev_registry_action(IOT_ACTION_REPLACE, &ident, sizeof(fulldevinfo[i]), &fulldevinfo[i]);
				if(err>=0) {
					devinfo[i].input=fulldevinfo[i].input;
					devinfo[i].cap_bitmap=fulldevinfo[i].cap_bitmap;
				} else {
					kapi_outlog_error("Cannot update device in registry: %s", kapi_strerror(err));
				}
			} else  //previous state was absent
				if(i<n && fulldevinfo[i].name[0]) { //new state is present, so NEW DEVICE ADDED
					kapi_outlog_info("Detected new hwdevice with type=" DEVCONTYPESTR_CUSTOM_LINUXINPUT ", input=%d, name='%s'",i, fulldevinfo[i].name);
					err=kapi_hwdev_registry_action(IOT_ACTION_ADD, &ident, sizeof(fulldevinfo[i]), &fulldevinfo[i]);
					if(err>=0) {
						devinfo[i].present=true;
						devinfo[i].input=fulldevinfo[i].input;
						devinfo[i].cap_bitmap=fulldevinfo[i].cap_bitmap;
						if(devinfo_len<i+1) devinfo_len=i+1;
					} else {
						kapi_outlog_error("Cannot add new device to registry: %s", kapi_strerror(err));
					}
					continue;
				} //else do nothing
		}
	}
public:
	detector(void) : is_started(false), devinfo_len(0) {
		assert(detector_obj==NULL);

		memset(devinfo, 0, sizeof(devinfo));
		detector_obj=this;
	}
	int start(void) {
		assert(!is_started);

		if(is_started) return 0;
		uv_loop_t* loop=kapi_get_event_loop(uv_thread_self());
		assert(loop!=NULL);

		uv_timer_init(loop, &timer_watcher);
		int err=uv_timer_start(&timer_watcher, [](uv_timer_t* handle) -> void {detector_obj->on_timer();}, 0, DETECTOR_POLL_INTERVAL);
		if(err<0) {
			kapi_outlog_error("Cannot start timer: %s", uv_strerror(err));
			return IOT_ERROR_TEMPORARY_ERROR;
		}
		is_started=true;
		return 0;
	}
	int stop(void) {
		assert(is_started);

		if(!is_started) return 0;
		uv_timer_stop(&timer_watcher);
		is_started=false;
		return 0;
	}

	//traverses all /dev/input/eventX devices and reads necessary props
	//returns number of found devices
	static int get_event_devices(devcontype_linuxinput_t* devbuf, int max_devs, int start_index=0) {//takes address for array of devcontype_linuxinput_t structs and size of such array
		char filepath[32];
		int fd, idx;
		devcontype_linuxinput_t *cur_dev;

		for(idx=0;idx<max_devs;idx++) {
			cur_dev=devbuf+idx;

			snprintf(filepath, sizeof(filepath), "/dev/input/event%d", idx+start_index);
			fd=open(filepath, O_RDONLY | O_NONBLOCK); //O_NONBLOCK help to avoid side-effects of open on Linux when only ioctl is necessary
			if(fd<0) {
//				if(errno==ENOENT) break; //stop on first non-existing file
				if(errno!=ENOENT && errno!=ENODEV && errno!=ENXIO) //file present but corresponding device not exists
					kapi_outlog_debug("Cannot open %s: %s", filepath, uv_strerror(uv_translate_sys_error(errno)));
				cur_dev->name[0]='\0'; //indicator of skipped device
				continue;
			}

			const char* errstr=read_inputdev_caps(fd, idx+start_index, cur_dev);

			if(errstr) { //was ioctl error
				kapi_outlog_debug("Cannot %s on %s: %s", errstr, filepath, uv_strerror(uv_translate_sys_error(errno)));
				cur_dev->name[0]='\0'; //indicator of skipped device
			}
			close(fd);
		}
		return idx;
	}
};
static detector _detector_obj; //instantiate singleton class

static iot_hwdevcontype_t detector_devcontypes[]={DEVCONTYPE_CUSTOM_LINUXINPUT};

static iot_iface_device_detector_t detector_iface = {
	.num_hwdevcontypes = sizeof(detector_devcontypes)/sizeof(detector_devcontypes[0]),
	.cpu_loading = 0,

	.start = [](time_t cfg_lastmod, const char *json_cfg) -> int {return detector_obj->start();},
	.stop = [](void) -> int {return detector_obj->stop();},

	.hwdevcontypes = detector_devcontypes
};

iot_moduleconfig_t IOT_MODULE_CONF(detector)={
	.module_id = MODULEID_detector, //Registered ID of this module. Must correspond to its full name in registry
	.version = 0x000100001,
	.num_devifaces = 0,
	.init_module = [](void) -> int {return 0;},
	.deinit_module = [](void) -> int {return 0;},
	.deviface_config = NULL,
	.iface_event_source = NULL,
	.iface_device_driver = NULL,
	.iface_device_detector = &detector_iface
};



/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////inputlinux:input_drv driver module
/////////////////////////////////////////////////////////////////////////////////
//Driver for /dev/input/eventX input devices abstraction which has EV_KEY and/or EV_LED capabilities, i.e. normal keyboards or other input devices with keys


struct input_drv_instance;

struct input_drv_instance {
	input_drv_instance* next_instance;
	uv_thread_t thread; //working thread of this instance after start
	iot_iid_t iid;
	bool is_active; //true if instance was started
	iot_hwdev_ident_t dev_ident; //identification of connected hw device
	devcontype_linuxinput_t dev_info; //hw device capabilities reported by detector
	uint32_t keys_state[(KEY_CNT+31)/32]; //when EV_KEY capability present, bitmap of current keys state
	uint16_t sw_state; //when EV_SW capability present, bitmap of current switches state
	uint16_t leds_state; //when EV_LED capability present, bitmap of current leds state
	uint16_t want_leds_bitmap; //when EV_LED capability present, bitmap of leds which have executor connected
	uint16_t want_leds_state; //when EV_LED capability present, bitmap of requested leds state
	uint8_t snd_state; //when EV_SND capability present, bitmap of current snd state

	bool have_kbd, //iface IOT_DEVCLASSID_KEYBOARD was reported
		have_keys, //iface IOT_DEVCLASSID_KEYS was reported
		have_leds, //iface IOT_DEVCLASSID_LEDS was reported
		have_spk, //iface IOT_DEVCLASSID_BASIC_SPEAKER was reported
		have_sw; //iface IOT_DEVCLASSID_HW_SWITCHES was reported
	iot_connid_t connid_kbd; //id of connection with IOT_DEVCLASSID_KEYBOARD iface, if connected. conflicts with connid_keys
	iot_connid_t connid_keys; //id of connection with IOT_DEVCLASSID_KEYS iface, if connected. conflicts with connid_kbd


/////////////static fields/methods for driver instances management
	static input_drv_instance* instances_head;
	static int init_module(void) {
		instances_head=NULL;
		return 0;
	}
	static int deinit_module(void) {
		assert(instances_head==NULL); //all instances should be deinited
		return 0;
	}
	static int init_instance(void**instance, uv_thread_t thread, iot_hwdev_data_t* dev_data, iot_deviface_classid* devifaces, uint8_t max_devifaces) {
		assert(uv_thread_self()==main_thread);

		//FILTER HW DEVICE CAPABILITIES
		int err=check_device(dev_data);
		if(err) return err;
		devcontype_linuxinput_t *devinfo=(devcontype_linuxinput_t*)(dev_data->custom_data);
		//END OF FILTER

		//determine interfaces which this instance can provide for provided hwdevice
		int num=0;
		bool have_kbd=false, //iface IOT_DEVCLASSID_KEYBOARD was reported
			have_keys=false, //iface IOT_DEVCLASSID_KEYS was reported
			have_leds=false, //iface IOT_DEVCLASSID_LEDS was reported
			have_spk=false, //iface IOT_DEVCLASSID_BASIC_SPEAKER was reported
			have_sw=false; //iface IOT_DEVCLASSID_HW_SWITCHES was reported

		if(bitmap32_test_bit(&devinfo->cap_bitmap, EV_KEY)) {
			if(bitmap32_test_bit(devinfo->keys_bitmap,KEY_LEFTSHIFT) && bitmap32_test_bit(devinfo->keys_bitmap,KEY_LEFTCTRL)) {devifaces[num++]=IOT_DEVCLASSID_KEYBOARD;have_kbd=true;}
			if(num<max_devifaces) {devifaces[num++]=IOT_DEVCLASSID_KEYS;have_keys=true;}
		}
		if(num<max_devifaces && bitmap32_test_bit(&devinfo->cap_bitmap, EV_LED)) {devifaces[num++]=IOT_DEVCLASSID_LEDS;have_leds=true;}
		if(num<max_devifaces && bitmap32_test_bit(&devinfo->cap_bitmap, EV_SND)) {devifaces[num++]=IOT_DEVCLASSID_BASIC_SPEAKER;have_spk=true;}
		if(num<max_devifaces && bitmap32_test_bit(&devinfo->cap_bitmap, EV_SW)) {devifaces[num++]=IOT_DEVCLASSID_HW_SWITCHES;have_sw=true;}

		if(!num) return IOT_ERROR_DEVICE_NOT_SUPPORTED;

		input_drv_instance *inst=new input_drv_instance(thread);
		if(!inst) return IOT_ERROR_NO_MEMORY;
		inst->have_kbd=have_kbd;
		inst->have_keys=have_keys;
		inst->have_leds=have_leds;
		inst->have_spk=have_spk;
		inst->have_sw=have_sw;

		err=inst->init(dev_data);
		if(err) { //error
			delete inst;
			return err;
		}

		ULINKLIST_INSERTHEAD(inst, instances_head, next_instance);
		*instance=inst;

		char descr[256]="";
		int off=0;
		for(int i=0;i<num;i++) {
			off+=snprintf(descr+off, sizeof(descr)-off, "%s%s", i==0 ? "" : ", ",kapi_devclassid_str(devifaces[i]));
			if(off>=int(sizeof(descr))) break;
		}
		kapi_outlog_info("Driver started for device with type=" DEVCONTYPESTR_CUSTOM_LINUXINPUT ", input=%d, name='%s', caps='%s'", int(devinfo->event_index), devinfo->name, descr);
		return num;
	}

	static int deinit_instance(void* instance) {
		assert(uv_thread_self()==main_thread);
		input_drv_instance *inst=(input_drv_instance*) instance;

		assert(!inst->is_active); //must be stopped

		inst->deinit();
		ULINKLIST_REMOVE(inst, instances_head, next_instance);
		delete inst;
		return 0;
	}
	static int check_device(const iot_hwdev_data_t* dev_data) {
		if(dev_data->dev_ident.dev.contype!=DEVCONTYPE_CUSTOM_LINUXINPUT || dev_data->dev_ident.dev.detector_module_id!=MODULEID_detector) return IOT_ERROR_DEVICE_NOT_SUPPORTED;
		if(dev_data->custom_len!=sizeof(devcontype_linuxinput_t)) return IOT_ERROR_INVALID_DEVICE_DATA; //devcontype_linuxinput_t has fixed size
//		devcontype_linuxinput_t *devinfo=(devcontype_linuxinput_t*)(dev_data->custom_data);
//		if(!(devinfo->cap_bitmap & (EV_KEY | EV_LED))) return IOT_ERROR_DEVICE_NOT_SUPPORTED; NOW SUPPORT ALL DEVICES DETECTED BY OUR DETECTOR
		return 0;
	}
/////////////public methods
	int start(iot_iid_t _iid) {
		assert(uv_thread_self()==thread);
		assert(!is_active);

		if(is_active) return 0; //even in release mode just return success

		iid=_iid;
		eventfd=-1;
		internal_error=ERR_NONE;
		error_count=0;
		is_active=true;

		int err=setup_device_polling();
		if(err) {
			is_active=false;
			return err;
		}

		return 0;
	}
	int stop(void) {
		assert(uv_thread_self()==thread);
		assert(is_active);

		if(!is_active) return 0; //even in release mode just do nothing

		if(eventfd>=0) {
			uv_close((uv_handle_t*)&io_watcher, [](uv_handle_t* handle)->void{});
			eventfd=-1;
		} else {
			uv_timer_stop(&timer_watcher);
		}
		is_active=false;
		return 0;
	}

	int handle_open(iot_connid_t connid, iot_deviface_classid classid, void **privdata) {
		switch(classid) {
			case IOT_DEVCLASSID_KEYBOARD:
				if(!have_kbd) return IOT_ERROR_DEVICE_NOT_SUPPORTED;
				if(connid_kbd || connid_keys) return IOT_ERROR_LIMIT_REACHED;
				connid_kbd=connid;
				break;
			case IOT_DEVCLASSID_KEYS:
				if(!have_keys) return IOT_ERROR_DEVICE_NOT_SUPPORTED;
				if(connid_kbd || connid_keys) return IOT_ERROR_LIMIT_REACHED;
				connid_keys=connid;
				break;
			default:
				return IOT_ERROR_DEVICE_NOT_SUPPORTED;
		}
		return 0;
	}
	int handle_close(iot_connid_t connid, iot_deviface_classid classid, void *privdata) {
		switch(classid) {
			case IOT_DEVCLASSID_KEYBOARD:
				if(connid_kbd==connid) connid_kbd=0;
				break;
			case IOT_DEVCLASSID_KEYS:
				if(connid_keys==connid) connid_keys=0;
				break;
			default:
				break;
		}
		return 0;
	}
	int handle_action(iot_connid_t connid, iot_deviface_classid classid, void *privdata, iot_devconn_action_t action_code, uint32_t data_size, void* data) {
		if(action_code==IOT_DEVCONN_ACTION_MESSAGE) {
			if(classid==IOT_DEVCLASSID_KEYBOARD) {
//				iot_devclass_keyboard* msg;
//				if(data_size<sizeof(*msg)) return IOT_ERROR_INVALID_ARGS;
//				msg=(iot_devclass_keyboard*)data;
//				switch(msg->req_code) {
//					case msg->REQ_GET_STATE:
//				}
			}
		}
		return 0;
	}

private:
	input_drv_instance(uv_thread_t thread_) {
		memset(this, 0, sizeof(*this));
		thread=thread_;
	}

	int init(iot_hwdev_data_t* dev_data) {
		uv_loop_t* loop=kapi_get_event_loop(thread);
		assert(loop!=NULL);

		uv_tcp_init(loop, &io_watcher);
		io_watcher.data=this;
		uv_timer_init(loop, &timer_watcher);
		timer_watcher.data=this;

		memcpy(&dev_ident, &dev_data->dev_ident, sizeof(dev_ident));

		assert(dev_data->dev_ident.dev.contype==DEVCONTYPE_CUSTOM_LINUXINPUT);
		memcpy(&dev_info, dev_data->custom_data, sizeof(dev_info));
		return 0;
	}
	void deinit(void) {
		//nothing to deinit in this module
	}


/////////////OS device communication internals
	int eventfd; //FD of opened /dev/input/eventX or <0 if not opened. 
	uv_tcp_t io_watcher; //watcher over eventfd
	uv_timer_t timer_watcher; //to count different retries
	enum {
		ERR_NONE=0,
		ERR_OPEN_TEMP=1,
		ERR_EVENT_MGR=2,
	} internal_error;
	int error_count;
	char internal_error_descr[256];
	input_event read_buf[32]; //event struct from <linux/input.h>
//These vars keep state of reading process, so must be reset during reconnect
	ssize_t read_buf_offset; //used during reading into read_buf in case of partial read of last record. always will be < sizeof(input_event)
	bool read_waitsyn; //true if all events from device must be ignored until next EV_SYN
////

	//recreates polling procedure initially or after device disconnect
	int setup_device_polling(void) {
		assert(uv_thread_self()==thread);
		assert(is_active);

		uv_timer_stop(&timer_watcher);
		int retrytm=0; //assigned in case of temp error to set retry period
		int temperr=0; //assigned in case of temp error to set internal error
		bool criterror=false;
		do {
			if(eventfd<0) { //open device file
				char path[32];
				snprintf(path,sizeof(path),"/dev/input/event%d",dev_info.event_index);
				eventfd=open(path, O_RDWR | O_NONBLOCK);
				if(eventfd<0) {
					int err=errno;
					kapi_outlog_error("Cannot open '%s': %s", path, uv_strerror(uv_translate_sys_error(err)));
					if(err==ENOENT || err==ENXIO || err==ENODEV) return IOT_ERROR_CRITICAL_ERROR; //driver instance should be deinited
					temperr=ERR_OPEN_TEMP;
					retrytm=5;
					snprintf(internal_error_descr, sizeof(internal_error_descr), "Cannot open '%s': %s", path, uv_strerror(uv_translate_sys_error(err)));
					break;
				}
				//recheck caps of opened device
				devcontype_linuxinput_t dev_info2;
				const char* errstr=read_inputdev_caps(eventfd, dev_info.event_index, &dev_info2);
				if(errstr) {
					kapi_outlog_error("Cannot %s on '%s': %s", errstr, path, uv_strerror(uv_translate_sys_error(errno)));
					criterror=true;
					break;
				}
				if(memcmp(&dev_info, &dev_info2, sizeof(dev_info))!=0) { //another device was connected?
					kapi_outlog_info("Another device '%s' on '%s'", dev_info2.name, path);
					criterror=true;
					break;
				}

				//get current state of events
				if(bitmap32_test_bit(&dev_info.cap_bitmap, EV_KEY)) { //has EV_KEY cap
					if(ioctl(eventfd, EVIOCGKEY(sizeof(keys_state)), keys_state)==-1) { //get current keys state
						kapi_outlog_error("Cannot ioctl '%s' for EVIOCGKEY: %s", path, uv_strerror(uv_translate_sys_error(errno)));
						criterror=true;
						break;
					}
				}
				if(bitmap32_test_bit(&dev_info.cap_bitmap, EV_SW)) { //has EV_SW cap
					if(ioctl(eventfd, EVIOCGSW(sizeof(sw_state)), &sw_state)==-1) { //get current switches state
						kapi_outlog_error("Cannot ioctl '%s' for EVIOCGSW: %s", path, uv_strerror(uv_translate_sys_error(errno)));
						criterror=true;
						break;
					}
				}
				if(bitmap32_test_bit(&dev_info.cap_bitmap, EV_LED)) { //has EV_LED cap
					if(ioctl(eventfd, EVIOCGLED(sizeof(leds_state)), &leds_state)==-1) { //get current led state
						kapi_outlog_error("Cannot ioctl '%s' for EVIOCGLED: %s", path, uv_strerror(uv_translate_sys_error(errno)));
						criterror=true;
						break;
					}
					want_leds_state=(want_leds_state & want_leds_bitmap) | (leds_state & ~want_leds_bitmap);
				}
				if(bitmap32_test_bit(&dev_info.cap_bitmap, EV_SND)) { //has EV_SND cap
					if(ioctl(eventfd, EVIOCGSND(sizeof(snd_state)), &snd_state)==-1) { //get current led state
						kapi_outlog_error("Cannot ioctl '%s' for EVIOCGSND: %s", path, uv_strerror(uv_translate_sys_error(errno)));
						criterror=true;
						break;
					}
				}
			}

			//attach device file FD to io watcher
			int err=uv_tcp_open(&io_watcher, eventfd);
			if(err<0) {
				kapi_outlog_error("Cannot open uv stream: %s", uv_strerror(err));
				temperr=ERR_EVENT_MGR;
				retrytm=30;
				snprintf(internal_error_descr, sizeof(internal_error_descr), "Cannot open event listener: %s", uv_strerror(err));
				break;
			}

			read_buf_offset=0;
			read_waitsyn=false;
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
			return 0;
		} while(0);
		//common error processing
		if(criterror) {
			if(eventfd>=0) close(eventfd);
			eventfd=-1;
			return IOT_ERROR_CRITICAL_ERROR; //driver instance should be deinited
		}

		if(temperr==internal_error) error_count++;
		else {
			temperr=internal_error;
			error_count=1;
		}
		uv_timer_start(&timer_watcher, on_timer, (error_count>10 ? 10 : error_count)*retrytm*1000, 0);
		return 0;
	}
	static void on_timer(uv_timer_t* handle) {
		input_drv_instance* obj=static_cast<input_drv_instance*>(handle->data);
		int err=obj->setup_device_polling();
		if(err) {
			kapi_devdriver_self_abort(obj->iid, obj, err);
		}
	}


	void dev_onread(ssize_t nread) {
		if(nread<=0) { //error
			if(nread==0) return; //EAGAIN
			kapi_outlog_error("Error reading uv stream: %s", uv_strerror(nread));
			uv_close((uv_handle_t*)&io_watcher, [](uv_handle_t* handle)->void{});
			eventfd=-1;
			int err=setup_device_polling();
			if(err) {
				kapi_devdriver_self_abort(iid, this, err);
			}
			return;
		}
		int idx=0;
		while(nread>=int(sizeof(input_event))) {
			process_device_event(&read_buf[idx]);
			nread-=sizeof(input_event);
			idx++;
		}
		read_buf_offset=nread; //can be >0 if last record was not read in full
	}
	void process_device_event(input_event* ev) {
		int err;
		if(read_waitsyn) {
			if(ev->type==EV_SYN) read_waitsyn=false;
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
				if(connid_kbd) {
					iot_deviface_class_keyboard_msg_t msg;
					memset(&msg, 0, sizeof(msg));
					msg.event_code = ev->value == 1 ?
										iot_deviface_class_keyboard_msg_t::EVENT_KEYDOWN : 
										ev->value==0 ? 
											iot_deviface_class_keyboard_msg_t::EVENT_KEYUP : 
											iot_deviface_class_keyboard_msg_t::EVENT_KEYREPEAT;
					msg.data.keyevent.key=ev->code;
					memcpy(msg.data.keyevent.state.map, keys_state, sizeof(msg.data.keyevent.state.map));
					if(bitmap32_test_bit(keys_state, KEY_LEFTSHIFT) | bitmap32_test_bit(keys_state, KEY_RIGHTSHIFT)) msg.data.keyevent.state.shift_state=1;
					if(bitmap32_test_bit(keys_state, KEY_LEFTALT) | bitmap32_test_bit(keys_state, KEY_RIGHTALT)) msg.data.keyevent.state.alt_state=1;
					if(bitmap32_test_bit(keys_state, KEY_LEFTCTRL) | bitmap32_test_bit(keys_state, KEY_RIGHTCTRL)) msg.data.keyevent.state.ctrl_state=1;
					if(bitmap32_test_bit(keys_state, KEY_LEFTMETA) | bitmap32_test_bit(keys_state, KEY_RIGHTMETA)) msg.data.keyevent.state.meta_state=1;
					err=kapi_connection_send_client_msg(connid_kbd, this, IOT_DEVCLASSID_KEYBOARD, &msg, sizeof(msg));
					if(err) {
						//TODO remember
					} else {
						//TODO remember dropped message state
					}
					break;
				}
				kapi_outlog_info("Key with code %d is %s", ev->code, ev->value == 1 ? "down" : ev->value==0 ? "up" : "repeated");
				break;
			case EV_LED: //led event
				kapi_outlog_info("LED with code %d is %s", ev->code, ev->value == 1 ? "on" : "off");
				break;
			case EV_SW: //led event
				kapi_outlog_info("SW with code %d valued %d", ev->code, (int)ev->value);
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

input_drv_instance* input_drv_instance::instances_head;

static iot_iface_device_driver_t input_drv_iface_device_driver = {
//	.num_devclassids = 0,
	.cpu_loading = 0,

	.init_instance = &input_drv_instance::init_instance,
	.deinit_instance = &input_drv_instance::deinit_instance,
	.check_device = &input_drv_instance::check_device,
	.start = [](void* instance,iot_iid_t iid) -> int {return ((input_drv_instance*)instance)->start(iid);},
	.stop = [](void* instance) -> int {return ((input_drv_instance*)instance)->stop();},

//	.devclassids = NULL,
	.open = [](void* instance, iot_connid_t connid, iot_deviface_classid classid, void **privdata) -> int {return ((input_drv_instance*)instance)->handle_open(connid, classid, privdata);},
	.close = [](void* instance, iot_connid_t connid, iot_deviface_classid classid, void* privdata) -> int {return ((input_drv_instance*)instance)->handle_close(connid, classid, privdata);},
	.action = [](void* instance, iot_connid_t connid, iot_deviface_classid classid, void* privdata, iot_devconn_action_t action_code, uint32_t data_size, void* data) -> int {return ((input_drv_instance*)instance)->handle_action(connid, classid, privdata, action_code, data_size, data);},
};

iot_moduleconfig_t IOT_MODULE_CONF(input_drv)={
	.module_id = MODULEID_input_drv, //Registered ID of this module. Must correspond to its full name in registry
	.version = 0x000100001,
	.num_devifaces = 0,
//	.flags = IOT_MODULEFLAG_IFACE_DEVDRIVER,
	.init_module = &input_drv_instance::init_module,
	.deinit_module = &input_drv_instance::deinit_module,
	.deviface_config = NULL,
	.iface_event_source = NULL,
	.iface_device_driver = &input_drv_iface_device_driver,
	.iface_device_detector = NULL
};

//end of kbdlinux:input_drv driver module






/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////inputlinux:kbd_src event source  module
/////////////////////////////////////////////////////////////////////////////////

static iot_deviface_classid kbd_src_devclassids[]={IOT_DEVCLASSID_KEYBOARD};

static iot_state_classid kbd_src_stclassids[]={IOT_SRCSTATE_CLASSID_BUTTONSTATE};

struct kbd_src_instance;

struct kbd_src_instance {
	uv_thread_t thread;
	iot_iid_t iid;
	uint32_t iot_id;
	bool is_active; //true if instance was started
	iot_connid_t device_connid;
	uv_timer_t timer_watcher;
	iot_device_conn_t device;
/////////////evsource state:
	iot_srcstate_error_t error_code;


/////////////static fields/methods for module instances management
	static int init_module(void) {
		return 0;
	}
	static int deinit_module(void) {
		return 0;
	}

	static int init_instance(void**instance, uv_thread_t thread, uint32_t iot_id, const char *json_cfg) {
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

	static int deinit_instance(void* instance) {
		assert(uv_thread_self()==main_thread);

		kbd_src_instance *inst=(kbd_src_instance*) instance;
		delete inst;
		return 0;
	}

/////////////public methods

	int start(iot_iid_t _iid) {
		assert(uv_thread_self()==thread);
		assert(!is_active);

		if(is_active) return 0; //even in release mode just return success

		iid=_iid;
		kapi_outlog_info("EVENT SOURCE STARTED id=%u", iot_id);
		is_active=true;

		if(!device_connid) uv_timer_start(&timer_watcher, [](uv_timer_t* handle)->void {
			kbd_src_instance* obj=static_cast<kbd_src_instance*>(handle->data);
			obj->on_timer();
		}, 2000, 0); //give 2 secs for device to connect

		return 0;
	}
//	int get_state(iot_srcstate_t* statebuf, size_t bufsize) {
//		//can be called from any thread
//		return 0;
//	}
	int stop(void) {
		assert(uv_thread_self()==thread);
		assert(is_active);

		if(!is_active) return 0; //even in release mode just return success


		is_active=false;
		return 0;
	}
////
	void device_attached(iot_connid_t connid, iot_device_conn_t *devconn) {
		assert(uv_thread_self()==thread);
		assert(!device_connid);
		device_connid=connid;
		device=*devconn;

		kapi_outlog_info("Device attached, iot_id=%u", iot_id);
	}
	void device_detached(void) {
		assert(uv_thread_self()==thread);
		assert(device_connid!=0);
		device_connid=0;

		kapi_outlog_info("Device detached, iot_id=%u", iot_id);
	}
	void device_action(iot_devconn_action_t action_code, uint32_t data_size, void* data) {
		assert(uv_thread_self()==thread);
		assert(device_connid!=0);
		int err;
		switch(action_code) {
			case IOT_DEVCONN_ACTION_MESSAGE: //new message arrived
				if(device.classid==IOT_DEVCLASSID_KEYBOARD) {
					iot_deviface_class_keyboard_msg_t *msg;
					if(data_size!=sizeof(*msg)) return;
					msg=(iot_deviface_class_keyboard_msg_t*)data;

					if(msg->event_code==iot_deviface_class_keyboard_msg_t::EVENT_KEYDOWN) {
						kapi_outlog_info("GOT keyboard DOWN for key %d", (int)msg->data.keyevent.key);
						return;
					} else if(msg->event_code==iot_deviface_class_keyboard_msg_t::EVENT_KEYUP) {
						kapi_outlog_info("GOT keyboard UP for key %d", (int)msg->data.keyevent.key);
						return;
					} else if(msg->event_code==iot_deviface_class_keyboard_msg_t::EVENT_KEYREPEAT) {
						kapi_outlog_info("GOT keyboard REPEAT for key %d", (int)msg->data.keyevent.key);
						return;
					}
				}
				break;
			default:
				break;
		}
		kapi_outlog_info("Device action, iot_id=%u, act code %u, datasize %u", iot_id, unsigned(action_code), data_size);
	}


private:
	kbd_src_instance(uv_thread_t thread, uint32_t iot_id) :
		thread(thread),
		iid(0),
		iot_id(iot_id),
		is_active(false),
		device_connid(0),
		error_code(0) {}

	int init(void) {
		uv_loop_t* loop=kapi_get_event_loop(thread);
		assert(loop!=NULL);

		uv_timer_init(loop, &timer_watcher);
		timer_watcher.data=this;
		return 0;
	}
	void on_timer(void) {
		error_code=IOT_SRCSTATE_ERROR_NO_DEVICE;
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
	.num_stclassids = sizeof(kbd_src_stclassids)/sizeof(kbd_src_stclassids[0]),
	.cpu_loading = 0,

	.devcfg={
		{
			.num_classids = sizeof(kbd_src_devclassids)/sizeof(kbd_src_devclassids[0]),
			.flag_canauto = 1,
			.flag_localonly = 1,
			.classids = kbd_src_devclassids
		}
	},

	.value_type = IOT_VALUETYPE_INTEGER,

	//methods
	.init_instance = &kbd_src_instance::init_instance,
	.deinit_instance = &kbd_src_instance::deinit_instance,
	.start = [](void* instance,iot_iid_t iid) -> int {return ((kbd_src_instance*)instance)->start(iid);},
	.stop = [](void* instance) -> int {return ((kbd_src_instance*)instance)->stop();},
	.device_attached = [](void* instance, int index, iot_connid_t connid, iot_device_conn_t *devconn) -> void {((kbd_src_instance*)instance)->device_attached(connid, devconn);},
	.device_detached = [](void* instance, int index) -> void {((kbd_src_instance*)instance)->device_detached();},
	.device_action = [](void* instance, int index, iot_deviface_classid classid, iot_devconn_action_t action_code, uint32_t data_size, void* data) -> void {((kbd_src_instance*)instance)->device_action(action_code, data_size, data);},


//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},

	//configuration for module state
	.stclassids = kbd_src_stclassids,
};

iot_moduleconfig_t IOT_MODULE_CONF(kbd_src)={
	.module_id = MODULEID_kbd_src, //Registered ID of this module. Must correspond to its full name in registry
	.version = 0x000100001,
	.num_devifaces = 0,
//	.flags = IOT_MODULEFLAG_IFACE_SOURCE,
	.init_module = [](void) -> int {return 0;},
	.deinit_module = [](void) -> int {return 0;},
	.deviface_config = NULL,
	.iface_event_source = &kbd_src_iface_event_source,
	.iface_device_driver = NULL,
	.iface_device_detector = NULL
};

//end of kbdlinux:keys event source  module





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

