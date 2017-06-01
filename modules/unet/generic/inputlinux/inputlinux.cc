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
#define IOT_BUNDLE generic__inputlinux

#include<iot_module.h>

#include<iot_devclass_keyboard.h>


//#define DRVNAME drv

//list of modules in current bundle, their registered IDs. TODO: put real IDs when they will be allocated through some table in unetcommonsrc
#define BUNDLE_MODULES_MAP(XX) \
	XX(detector, 1)  \
	XX(input_drv, 2)


//build constants like MODULEID_detector which resolve to registered module id
enum {
#define XX(name, id) MODULEID_ ## name = id,
	BUNDLE_MODULES_MAP(XX)
#undef XX
};


// "/dev/input/eventX" device on linuxes
#define DEVCONTYPE_CUSTOM_LINUXINPUT	IOT_DEVCONTYPE_CUSTOM(MODULEID_detector, 1)
#define DEVCONTYPESTR_CUSTOM_LINUXINPUT	"LinuxInput"


//interface for DEVCONTYPE_CUSTOM_LINUXINPUT contype
static struct linuxinput_iface : public iot_hwdevident_iface {
	struct hwid_t {
		input_id input;
		uint32_t cap_bitmap; //value 0xFFFFFFFFu means 'any hwid' (for templates)
	};
	struct addr_t {
		uint8_t event_index; //X in /dev/input/eventX. value 0xFF means 'any' (for templates)
	};
	struct data_t {
		uint32_t format; //version of format and/or magic code. current is 1, and it is the only supported.
		hwid_t hwid;
		addr_t addr;
	};

	linuxinput_iface(void) : iot_hwdevident_iface(DEVCONTYPE_CUSTOM_LINUXINPUT, DEVCONTYPESTR_CUSTOM_LINUXINPUT) {
	}

	void init_localident(iot_hwdev_localident_t* dev_ident, uint32_t detector_module_id) { //must be called first to init iot_hwdev_localident_t structure
		dev_ident->contype=contype;
		dev_ident->detector_module_id=detector_module_id;
		*((data_t*)dev_ident->data)={ //init as template
			.format = 1,
			.hwid = {
				.input = {0, 0, 0, 0},
				.cap_bitmap = 0xFFFFFFFFu
			},
			.addr = {
				.event_index = 0xFF
			}
		};
	}
	void set_hwid(iot_hwdev_localident_t* dev_ident, hwid_t* hwid) {
		assert(dev_ident->contype==contype);
		assert(check_data(dev_ident->data));
		data_t* data=(data_t*)dev_ident->data;
		data->hwid=*hwid;
	}
	void set_addr(iot_hwdev_localident_t* dev_ident, addr_t* addr) {
		assert(dev_ident->contype==contype);
		assert(check_data(dev_ident->data));
		data_t* data=(data_t*)dev_ident->data;
		data->addr=*addr;
	}

private:
	virtual bool check_data(const char* dev_data) const override { //actual check that data is good by format
		data_t* data=(data_t*)dev_data;
		return data->format==1;
	}
	virtual bool check_istmpl(const char* dev_data) const override { //actual check that data corresponds to template (so not all data components are specified)
		data_t* data=(data_t*)dev_data;
		return data->hwid.cap_bitmap==0xFFFFFFFFu || data->addr.event_index==0xFF;
	}
	virtual bool compare_hwid(const char* dev_data, const char* tmpl_data) const override { //actual comparison function for hwid component of device ident data
		data_t* data=(data_t*)dev_data;
		data_t* tmpl=(data_t*)tmpl_data;
		return tmpl->hwid.cap_bitmap==0xFFFFFFFFu || !memcmp(&tmpl->hwid, &data->hwid, sizeof(tmpl->hwid));
	}
	virtual bool compare_addr(const char* dev_data, const char* tmpl_data) const override { //actual comparison function for address component of device ident data
		data_t* data=(data_t*)dev_data;
		data_t* tmpl=(data_t*)tmpl_data;
		return tmpl->addr.event_index==data->addr.event_index || tmpl->addr.event_index==0xFF;
	}
	virtual size_t print_addr(const char* dev_data, char* buf, size_t bufsize) const override { //actual address printing function. it must return number of written bytes (without NUL)
		data_t* data=(data_t*)dev_data;
		int len;
		if(data->addr.event_index==0xFF) { //template
			len=snprintf(buf, bufsize, "LinuxInput:any input");
		} else {
			len=snprintf(buf, bufsize, "LinuxInput:input=%u",unsigned(data->addr.event_index));
		}
		return len>=int(bufsize) ? bufsize-1 : len;
	}
	virtual size_t print_hwid(const char* dev_data, char* buf, size_t bufsize) const override { //actual hw id printing function. it must return number of written bytes (without NUL)
		data_t* data=(data_t*)dev_data;
		int len;
		if(data->hwid.cap_bitmap==0xFFFFFFFFu) { //template
			len=snprintf(buf, bufsize, "any hwid");
		} else {
			const char *bus;
			switch(data->hwid.input.bustype) {
				case 0x01: bus="PCI";break;
				case 0x03: bus="USB";break;
				case 0x05: bus="BT";break;
				case 0x10: bus="ISA";break;
				case 0x11: bus="PS/2";break;
				case 0x19: bus="Host";break;
				default: bus=NULL;
			}
			if(bus) {
				len=snprintf(buf, bufsize, "bus=%s,vendor=%04x,product=%04x,ver=%04x,caps=%x",bus,unsigned(data->hwid.input.vendor),
					unsigned(data->hwid.input.product),unsigned(data->hwid.input.version),unsigned(data->hwid.cap_bitmap));
			} else {
				len=snprintf(buf, bufsize, "bus=%u,vendor=%04x,product=%04x,ver=%04x,caps=%x",unsigned(data->hwid.input.bustype),unsigned(data->hwid.input.vendor),
					unsigned(data->hwid.input.product),unsigned(data->hwid.input.version),unsigned(data->hwid.cap_bitmap));
			}
		}
		return len>=int(bufsize) ? bufsize-1 : len;
	}
	virtual size_t to_json(const char* dev_data, char* buf, size_t bufsize) const override { //actual encoder to json
//		data_t* data=(data_t*)dev_data;
/*
{
	tmpl: absent (meaning 0) or 1 to show if this data refers to template. 
	addr: {
		i: uint8 from 0 to 254 to mean specific input line or "*" to mean 'any line' in template
	},
	hwid: {
		bus: name of specific bus or "*" in template
		vendor: vendor code from 0 to 65534 or "*" in template
		model: model code from 0 to 65534 or "*" in template
		ver: model version code from 0 to 65534 or "*" in template
		caps: subhash from {'key':1,'led':1,'snd':1,'sw':1,'rel':1} or for templates can be "*" of list with all required caps like ['key','led'].
	}
*/
		return 0;
	}
	virtual const char* get_vistmpl(void) const override { //actual visualization template generator
		return R"!!!({
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
})!!!";
	}
} linuxinput_iface_obj;


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

class detector : public iot_device_detector_base {
	bool is_active=false; //true if instance was started
	uv_timer_t timer_watcher={};
	int devinfo_len=0; //number of filled items in devinfo array
	struct devinfo_t { //short device info indexed by event_index field. minimal info necessary to determine change of device
		input_id input;
		uint32_t cap_bitmap;
		bool present;
	} devinfo[DETECTOR_MAX_DEVS]={};

	void on_timer(void) {
		devcontype_linuxinput_t fulldevinfo[DETECTOR_MAX_DEVS];
		int n=get_event_devices(fulldevinfo, DETECTOR_MAX_DEVS);
		if(n==0 && devinfo_len==0) return; //nothing to do

		iot_hwdev_localident_t ident;
		linuxinput_iface_obj.init_localident(&ident, MODULEID_detector);
		linuxinput_iface::addr_t addr;
		linuxinput_iface::hwid_t hwid;

		int i, err;
		int max_n = n>=devinfo_len ? n : devinfo_len;

		for(i=0;i<max_n;i++) { //compare if actual devices changed for common indexes
			addr.event_index=i;
			linuxinput_iface_obj.set_addr(&ident, &addr);
			if(i<devinfo_len && devinfo[i].present) {
				if(i>=n || !fulldevinfo[i].name[0]) { //new state is absent, so device was removed
					kapi_outlog_info("Hwdevice was removed: type=" DEVCONTYPESTR_CUSTOM_LINUXINPUT ", input=%d",i);
					hwid={devinfo[i].input, devinfo[i].cap_bitmap};
					linuxinput_iface_obj.set_hwid(&ident, &hwid);
					err=kapi_hwdev_registry_action(IOT_ACTION_REMOVE, &ident, 0, NULL);
					if(err>=0) {
						devinfo[i].present=false;
					} else {
						kapi_outlog_error("Cannot remove device from registry: %s", kapi_strerror(err));
					}
					continue;
				}
				//check if device looks the same
				if(devinfo[i].cap_bitmap==fulldevinfo[i].cap_bitmap && !memcmp(&devinfo[i].input, &fulldevinfo[i].input, sizeof(devinfo[i].input))) continue; //same

				kapi_outlog_debug("bitmap %08x != %08x OR vendor:model %04x:%04x != %04x:%04x", devinfo[i].cap_bitmap, fulldevinfo[i].cap_bitmap, unsigned(devinfo[i].input.vendor),unsigned(devinfo[i].input.product), unsigned(fulldevinfo[i].input.vendor),unsigned(fulldevinfo[i].input.product));

				kapi_outlog_info("Hwdevice was replaced: type=" DEVCONTYPESTR_CUSTOM_LINUXINPUT ", input=%d, new name='%s'",i, fulldevinfo[i].name);
				
				hwid={fulldevinfo[i].input, fulldevinfo[i].cap_bitmap};
				linuxinput_iface_obj.set_hwid(&ident, &hwid);
				err=kapi_hwdev_registry_action(IOT_ACTION_ADD, &ident, sizeof(fulldevinfo[i]), &fulldevinfo[i]);
				if(err>=0) {
					devinfo[i].input=fulldevinfo[i].input;
					devinfo[i].cap_bitmap=fulldevinfo[i].cap_bitmap;
				} else {
					kapi_outlog_error("Cannot update device in registry: %s", kapi_strerror(err));
				}
			} else  //previous state was absent
				if(i<n && fulldevinfo[i].name[0]) { //new state is present, so NEW DEVICE ADDED
					kapi_outlog_info("Detected new hwdevice with type=" DEVCONTYPESTR_CUSTOM_LINUXINPUT ", input=%d, name='%s'",i, fulldevinfo[i].name);
					hwid={fulldevinfo[i].input, fulldevinfo[i].cap_bitmap};
					linuxinput_iface_obj.set_hwid(&ident, &hwid);
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

//iot_module_instance_base methods


	//Called to start work of previously inited instance.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, iot_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(const iot_miid_t &miid_) {
		assert(uv_thread_self()==thread);
		assert(!is_active);
		if(is_active) return 0;

		miid=miid_;

		uv_loop_t* loop=kapi_get_event_loop(thread);
		assert(loop!=NULL);

		uv_timer_init(loop, &timer_watcher);
		int err=uv_timer_start(&timer_watcher, [](uv_timer_t* handle) -> void {detector_obj->on_timer();}, 0, DETECTOR_POLL_INTERVAL);
		if(err<0) {
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

		uv_close((uv_handle_t*)&timer_watcher, NULL);
		is_active=false;
		return 0;
	}

public:
	detector(uv_thread_t thread) : iot_device_detector_base(thread) {
		assert(detector_obj==NULL);
		detector_obj=this;
	}
	virtual ~detector(void) {
		detector_obj=NULL;
	}
	int init() {
		return 0;
	}
	int deinit(void) {
		assert(!is_active); //must be stopped
		return 0;
	}

	static int init_instance(iot_device_detector_base**instance, uv_thread_t thread) {
		assert(uv_thread_self()==main_thread);

		detector *inst=new detector(thread);
		if(!inst) return IOT_ERROR_TEMPORARY_ERROR;

		int err=inst->init();
		if(err) { //error
			delete inst;
			return err;
		}
		*instance=inst;
		return 0;
	}
	//called to deinit instance.
	//Return values:
	//0 - success
	//any other error leaves instance in hang state
	static int deinit_instance(iot_module_instance_base* instance) {
		assert(uv_thread_self()==main_thread);
		detector *inst=static_cast<detector*>(instance);
		int err=inst->deinit();
		if(err) return err;
		delete inst;
		return 0;
	}
	static int check_system(void) {
		struct stat statbuf;
		int err=stat("/dev/input", &statbuf);
		if(!err) {
			if(S_ISDIR(statbuf.st_mode)) return 0; //dir
			return IOT_ERROR_DEVICE_NOT_SUPPORTED;
		}
		if(err==ENOMEM) return IOT_ERROR_TEMPORARY_ERROR;
		return IOT_ERROR_DEVICE_NOT_SUPPORTED;
	}

	//traverses all /dev/input/eventX devices and reads necessary props
	//returns number of found devices
	static int get_event_devices(devcontype_linuxinput_t* devbuf, int max_devs, int start_index=0) {//takes address for array of devcontype_linuxinput_t structs and size of such array
		char filepath[32];
		int fd, idx, n=0;
		devcontype_linuxinput_t *cur_dev;

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

			const char* errstr=read_inputdev_caps(fd, idx+start_index, cur_dev);

			if(errstr) { //was ioctl error
				kapi_outlog_debug("Cannot %s on %s: %s", errstr, filepath, uv_strerror(uv_translate_sys_error(errno)));
				cur_dev->name[0]='\0'; //indicator of skipped device
			}
			n=idx+1; //must return maximum successful index + 1
			close(fd);
		}
		return n;
	}
};

static iot_hwdevcontype_t detector_devcontypes[]={DEVCONTYPE_CUSTOM_LINUXINPUT};

static const iot_hwdevident_iface* detector_devcontype_config[]={&linuxinput_iface_obj};

static iot_iface_device_detector_t detector_iface = {
	.descr = NULL,
	.params_tmpl = NULL,
	.num_hwdevcontypes = sizeof(detector_devcontypes)/sizeof(detector_devcontypes[0]),
	.cpu_loading = 0,

	.init_instance = &detector::init_instance,
	.deinit_instance = &detector::deinit_instance,
	.check_system = &detector::check_system,

	.hwdevcontypes = detector_devcontypes
};

iot_moduleconfig_t IOT_MODULE_CONF(detector)={
	.title = "Detector of Linux Input devices",
	.descr = "Detects Linux input devices provided by evdev kernel module",
	.module_id = MODULEID_detector, //Registered ID of this module. Must correspond to its full name in registry
	.version = 0x000100001,
	.config_version = 0,
	.num_devifaces = 0,
	.num_devcontypes = 1,
	.init_module = [](void) -> int {return 0;},
	.deinit_module = [](void) -> int {return 0;},
	.deviface_config = NULL,
	.devcontype_config = detector_devcontype_config,
	.iface_node = NULL,
	.iface_device_driver = NULL,
	.iface_device_detector = &detector_iface
};



/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////inputlinux:input_drv driver module
/////////////////////////////////////////////////////////////////////////////////
//Driver for /dev/input/eventX input devices abstraction which has EV_KEY and/or EV_LED capabilities, i.e. normal keyboards or other input devices with keys


struct input_drv_instance;

struct input_drv_instance : public iot_device_driver_base {
	input_drv_instance* next_instance=NULL;
	bool is_active=false; //true if instance was started
	iot_hwdev_ident_t dev_ident; //identification of connected hw device
	devcontype_linuxinput_t dev_info; //hw device capabilities reported by detector
	uint32_t keys_state[(KEY_CNT+31)/32]={}; //when EV_KEY capability present, bitmap of current keys state
	uint16_t sw_state=0; //when EV_SW capability present, bitmap of current switches state
	uint16_t leds_state=0; //when EV_LED capability present, bitmap of current leds state
	uint16_t want_leds_bitmap=0; //when EV_LED capability present, bitmap of leds which have executor connected
	uint16_t want_leds_state=0; //when EV_LED capability present, bitmap of requested leds state
	uint8_t snd_state=0; //when EV_SND capability present, bitmap of current snd state

	bool have_kbd=false, //iface IOT_DEVIFACETYPEID_KEYBOARD was reported
		have_leds=false, //iface IOT_DEVIFACETYPEID_LEDS was reported
		have_spk=false, //iface IOT_DEVIFACETYPEID_BASIC_SPEAKER was reported
		have_sw=false; //iface IOT_DEVIFACETYPEID_HW_SWITCHES was reported
	iot_connid_t connid_kbd={}; //id of connection with IOT_DEVIFACETYPEID_KEYBOARD iface, if connected
	const iot_devifacetype *attr_kbd=NULL; //interface class attribute object for connected connid_kbd

	uv_loop_t* loop=NULL;

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
	static int init_instance(iot_device_driver_base**instance, uv_thread_t thread, iot_hwdev_data_t* dev_data, iot_devifaces_list* devifaces) {
		assert(uv_thread_self()==main_thread);

		//FILTER HW DEVICE CAPABILITIES
		int err=check_device(dev_data);
		if(err) return err;
		devcontype_linuxinput_t *devinfo=(devcontype_linuxinput_t*)(dev_data->custom_data);
		//END OF FILTER

		//determine interfaces which this instance can provide for provided hwdevice
		bool have_kbd=false, //iface IOT_DEVCLASSID_KEYBOARD was reported
			have_leds=false, //iface IOT_DEVCLASSID_LEDS was reported
			have_spk=false, //iface IOT_DEVCLASSID_BASIC_SPEAKER was reported
			have_sw=false; //iface IOT_DEVCLASSID_HW_SWITCHES was reported

		iot_devifacetype classdata;

		if(bitmap32_test_bit(&devinfo->cap_bitmap, EV_KEY)) {
			//find max key code in bitmap
			uint32_t code=0;
			for(int i=sizeof(devinfo->keys_bitmap)/sizeof(devinfo->keys_bitmap[0])-1;i>0;i--) {
				if(devinfo->keys_bitmap[i]) {
					for(int j=31;j>0;j--) if(bitmap32_test_bit(&devinfo->keys_bitmap[i],j)) {code=j+i*32;break;}
					break;
				}
			}
			bool is_pckbd=bitmap32_test_bit(devinfo->keys_bitmap,KEY_LEFTSHIFT) && bitmap32_test_bit(devinfo->keys_bitmap,KEY_LEFTCTRL);
			iot_devifacetype_keyboard::init_classdata(&classdata, code, is_pckbd);
			if(devifaces->add(&classdata)==0) have_kbd=true;
		}
//		if(bitmap32_test_bit(&devinfo->cap_bitmap, EV_LED)) {
//			if(devifaces->add(IOT_DEVIFACETYPEID_LEDS, NULL)==0) have_leds=true;
//		}
//		if(bitmap32_test_bit(&devinfo->cap_bitmap, EV_SND)) {if(devifaces->add(IOT_DEVIFACETYPEID_BASIC_SPEAKER, NULL)==0) have_spk=true;}
//		if(bitmap32_test_bit(&devinfo->cap_bitmap, EV_SW)) {if(devifaces->add(IOT_DEVIFACETYPEID_HW_SWITCHES, NULL)==0) have_sw=true;}

		if(!devifaces->num) return IOT_ERROR_DEVICE_NOT_SUPPORTED;

		input_drv_instance *inst=new input_drv_instance(thread, have_kbd, have_leds, have_spk, have_sw);
		if(!inst) return IOT_ERROR_TEMPORARY_ERROR;

		err=inst->init(dev_data);
		if(err) { //error
			delete inst;
			return err;
		}

		*instance=inst;

		char descr[256]="";
		char buf[128];
		int off=0;
		for(int i=0;i<devifaces->num;i++) {
			const iot_devifacetype_iface *iface=devifaces->items[i].find_iface();
			if(!iface) continue;
			off+=snprintf(descr+off, sizeof(descr)-off, "%s%s", i==0 ? "" : ", ", iface->sprint(&devifaces->items[i],buf,sizeof(buf)));
			if(off>=int(sizeof(descr))) break;
		}
		kapi_outlog_info("Driver inited for device with type=" DEVCONTYPESTR_CUSTOM_LINUXINPUT ", input=%d, name='%s', caps='%s'", int(devinfo->event_index), devinfo->name, descr);
		return 0;
	}

	//called to deinit instance.
	//Return values:
	//0 - success
	//any other error leaves instance in hang state
	static int deinit_instance(iot_module_instance_base* instance) {
		assert(uv_thread_self()==main_thread);
		input_drv_instance *inst=static_cast<input_drv_instance*>(instance);
		int err=inst->deinit();
		if(err) return err;

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


private:
	input_drv_instance(uv_thread_t thread, bool have_kbd, bool have_leds, bool have_spk, bool have_sw): iot_device_driver_base(thread), have_kbd(have_kbd),
			have_leds(have_leds), have_spk(have_spk), have_sw(have_sw) {
		ULINKLIST_INSERTHEAD(this, instances_head, next_instance);
	}
	virtual ~input_drv_instance(void) {
		ULINKLIST_REMOVE(this, instances_head, next_instance);
	}

	int init(iot_hwdev_data_t* dev_data) {

		memcpy(&dev_ident, &dev_data->dev_ident, sizeof(dev_ident));

		assert(dev_data->dev_ident.dev.contype==DEVCONTYPE_CUSTOM_LINUXINPUT);
		memcpy(&dev_info, dev_data->custom_data, sizeof(dev_info));
		return 0;
	}
	int deinit(void) {
		assert(!is_active); //must be stopped
		//nothing to deinit in this module
		
		return 0;
	}

//iot_module_instance_base methods


	//Called to start work of previously inited instance.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, iot_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(const iot_miid_t &miid_) {
		assert(uv_thread_self()==thread);
		assert(!is_active);

		if(is_active) return 0; //even in release mode just return success
		is_active=true;

		miid=miid_;
		loop=kapi_get_event_loop(thread);
		assert(loop!=NULL);

		eventfd=-1;
		internal_error=ERR_NONE;
		error_count=0;


		uv_timer_init(loop, &timer_watcher);
		timer_watcher.data=this;

		int err=setup_device_polling();
		if(err) {
			is_active=false;
			return err;
		}

		return 0;
	}
	//called to stop work of started instance. call can be followed by deinit or started again (if stop was manual, by user)
	//Return values:
	//0 - driver successfully stopped and can be deinited or restarted
	//IOT_ERROR_TRY_AGAIN - driver requires some time (async operation) to stop gracefully. kapi_self_abort() will be called to notify kernel when stop is finished.
	//						anyway second stop() call must free all resources correctly, may be in a hard way. otherwise module will be blocked and left in hang state (deinit
	//						cannot be called until stop reports OK)
	//any other error is treated as critical bug and driver is blocked for further starts. deinit won't be called for such instance. instance is put into hang state
	virtual int stop(void) {
		assert(uv_thread_self()==thread);
		assert(is_active);

		if(!is_active) return 0; //even in release mode just do nothing

		if(eventfd>=0) {
			uv_close((uv_handle_t*)&io_watcher, NULL);
			eventfd=-1;
		}
		uv_close((uv_handle_t*)&timer_watcher, NULL);
		is_active=false;
		return 0;
	}

//iot_device_driver_base methods
	virtual int device_open(const iot_conn_drvview* conn) {
		switch(conn->devclass.classid) {
			case IOT_DEVIFACETYPEID_KEYBOARD: {
				if(!have_kbd) return IOT_ERROR_DEVICE_NOT_SUPPORTED;
				if(connid_kbd) return IOT_ERROR_LIMIT_REACHED;
				connid_kbd=conn->id;
				attr_kbd=&conn->devclass;

				iot_devifaceclass__keyboard_DRV iface(attr_kbd);
				int err=iface.send_set_state(connid_kbd, this, keys_state);
				assert(err==0);

				break;
			}
			default:
				return IOT_ERROR_DEVICE_NOT_SUPPORTED;
		}
		return 0;
	}
	virtual int device_close(const iot_conn_drvview* conn) {
		switch(conn->devclass.classid) {
			case IOT_DEVIFACETYPEID_KEYBOARD:
				if(connid_kbd==conn->id) {connid_kbd.clear(); attr_kbd=NULL;}
				break;
			default:
				break;
		}
		return 0;
	}
	virtual int device_action(const iot_conn_drvview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) {
//		int err;
		if(action_code==IOT_DEVCONN_ACTION_MESSAGE) {
			if(conn->id==connid_kbd) {
				iot_devifaceclass__keyboard_DRV iface(attr_kbd);
				const iot_devifaceclass__keyboard_DRV::msg* msg=iface.parse_event(data, data_size);
				if(!msg) return IOT_ERROR_MESSAGE_IGNORED;

				if(msg->req_code==iface.REQ_GET_STATE) {
					kapi_outlog_info("Driver GOT keyboard GET STATE");
					int err=iface.send_set_state(connid_kbd, this, keys_state);
					assert(err==0);
					return 0;
				}
				return IOT_ERROR_MESSAGE_IGNORED;
			}
//		} else if(action_code==IOT_DEVCONN_ACTION_READY) {
//			if(conn->id==connid_kbd) {
//				iot_devifaceclass__keyboard_DRV iface(attr_kbd);
//				err=iface.send_set_state(connid_kbd, this, keys_state);
//				assert(err==0);
//			}
		}
		return IOT_ERROR_UNKNOWN_ACTION;
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
		int err;
		bool criterror=false;
		do {
			if(eventfd<0) { //open device file
				char path[32];
				snprintf(path,sizeof(path),"/dev/input/event%d",dev_info.event_index);
				eventfd=open(path, O_RDWR | O_NONBLOCK);
				if(eventfd<0) {
					err=errno;
					kapi_outlog_error("Cannot open '%s': %s", path, uv_strerror(uv_translate_sys_error(err)));
					if(err==ENOENT || err==ENXIO || err==ENODEV) return IOT_ERROR_CRITICAL_ERROR; //driver instance should be deinited
					temperr=ERR_OPEN_TEMP;
					retrytm=5;
					snprintf(internal_error_descr, sizeof(internal_error_descr), "Cannot open '%s': %s", path, uv_strerror(uv_translate_sys_error(err)));
					break;
				}

				uv_tcp_init(loop, &io_watcher);
				io_watcher.data=this;
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
				
				uv_close((uv_handle_t*)&io_watcher, NULL);
				close(eventfd);
				eventfd=-1;
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
			obj->kapi_self_abort(err);
		}
	}


	void dev_onread(ssize_t nread) {
		if(nread<=0) { //error
			if(nread==0) return; //EAGAIN
			kapi_outlog_error("Error reading uv stream: %s", uv_strerror(nread));
			uv_close((uv_handle_t*)&io_watcher, NULL);
			eventfd=-1;
			int err=setup_device_polling();
			if(err) {
				kapi_self_abort(err);
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
					iot_devifaceclass__keyboard_DRV iface(attr_kbd);
					switch(ev->value) {
						case 0:
							err=iface.send_keyup(connid_kbd, this, ev->code, keys_state);
							break;
						case 1:
							err=iface.send_keydown(connid_kbd, this, ev->code, keys_state);
							break;
						case 2:
//							err=iface.send_keyrepeat(connid_kbd, this, ev->code, keys_state);
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
//				kapi_outlog_info("Key with code %d is %s", ev->code, ev->value == 1 ? "down" : ev->value==0 ? "up" : "repeated");
				break;
			case EV_LED: //led event
				kapi_outlog_info("LED with code %d is %s", ev->code, ev->value == 1 ? "on" : "off");
				break;
			case EV_SW: //led event
				kapi_outlog_info("SW with code %d valued %d", ev->code, (int)ev->value);
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

input_drv_instance* input_drv_instance::instances_head;

static iot_iface_device_driver_t input_drv_iface_device_driver = {
	.descr = NULL,
//	.num_devclassids = 0,
	.cpu_loading = 3,

	.init_instance = &input_drv_instance::init_instance,
	.deinit_instance = &input_drv_instance::deinit_instance,
	.check_device = &input_drv_instance::check_device
};

iot_moduleconfig_t IOT_MODULE_CONF(input_drv)={
	.title = "Driver for Linux Input devices",
	.descr = "Supports keyboards, speaker, hardware switches",
	.module_id = MODULEID_input_drv, //Registered ID of this module. Must correspond to its full name in registry
	.version = 0x000100001,
	.config_version = 0,
	.num_devifaces = 0,
	.num_devcontypes = 0,
//	.flags = IOT_MODULEFLAG_IFACE_DEVDRIVER,
	.init_module = &input_drv_instance::init_module,
	.deinit_module = &input_drv_instance::deinit_module,
	.deviface_config = NULL,
	.devcontype_config = NULL,
	.iface_node = NULL,
	.iface_device_driver = &input_drv_iface_device_driver,
	.iface_device_detector = NULL
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

