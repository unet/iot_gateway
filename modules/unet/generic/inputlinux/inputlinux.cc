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


#include "iot_module.h"

#include "iot_devclass_keyboard.h"
#include "iot_devclass_activatable.h"
#include "modules/unet/types/di_toneplayer/iot_devclass_toneplayer.h"

// "/dev/input/eventX" device on linuxes
//#define DEVCONTYPE_CUSTOM_LINUXINPUT	IOT_DEVCONTYPE_CUSTOM(MODULEID_inputlinux, 1)
#define DEVCONTYPESTR_CUSTOM_LINUXINPUT	"LinuxInput"


class iot_hwdev_localident_linuxinput : public iot_hwdev_localident {
	friend class iot_hwdevcontype_metaclass_linuxinput;
	union {
		struct {
//hwid:
			uint16_t bustype,  //id of physical bus as per BUS_XXX constants in include/linux/input.h (1-0x1f for now)
				vendor,
				product,
				version;
			uint32_t cap_bitmap;
			char phys[64]; //physical connection spec
//address:
			uint8_t event_index; //X in /dev/input/eventX
		} spec;
		struct {
			uint32_t bustype; //bitmask of allowed bus types or 0 for any bus type
			struct {
				uint16_t vendor,
					product,	 //this field can be 0 meaning "any product" or exact product id
					version;	 //used to specify exact product version or 0xFFFF meaning "any"
			} spec[8]; //array with specific product requirements
			uint32_t cap_bitmap; //bitmap of required caps, so 0 means 'no requirements'
			uint8_t num_specs; //number of valid items in spec[]. zero means that no specific product requirements
		} tmpl;
	};
	bool istmpl;
//
public:
	
	iot_hwdev_localident_linuxinput(void);

	static const iot_hwdev_localident_linuxinput* cast(const iot_hwdev_localident* ident);

	int init_spec(uint8_t event_index, uint16_t bustype, uint16_t vendor, uint16_t product, uint16_t version, uint32_t cap_bitmap, const char* phys) {
		if(!cap_bitmap || !phys) return IOT_ERROR_INVALID_ARGS;
		istmpl=false;
		spec.event_index=event_index;
		spec.bustype=bustype;
		spec.vendor=vendor;
		spec.product=product;
		spec.version=version;
		spec.cap_bitmap=cap_bitmap;
		snprintf(spec.phys, sizeof(spec.phys), "%s", phys);
		return 0;
	}

	virtual bool is_tmpl(void) const override {
		return istmpl;
	}
	virtual size_t get_size(void) const override {
		return sizeof(*this);
	}
	virtual char* sprint_addr(char* buf, size_t bufsize, int* doff=NULL) const override { //actual address printing function. it must return number of written bytes (without NUL)
		if(!bufsize) return buf;
		int len;
		if(istmpl) { //template
			len=snprintf(buf, bufsize, "any event index");
		} else {
			len=snprintf(buf, bufsize, "event index=%u",unsigned(spec.event_index));
		}
		if(doff) *doff += len>=int(bufsize) ? int(bufsize-1) : len;
		return buf;
	}
	virtual char* sprint_hwid(char* buf, size_t bufsize, int* doff=NULL) const override { //actual hw id printing function. it must return number of written bytes (without NUL)
		if(!bufsize) return buf;
		int len;
		if(istmpl) { //template
			len=snprintf(buf, bufsize, "template"); //TODO
		} else {
			const char *bus;
			switch(spec.bustype) {
				case 0x01: bus="PCI";break;
				case 0x03: bus="USB";break;
				case 0x05: bus="BT";break;
				case 0x10: bus="ISA";break;
				case 0x11: bus="PS/2";break;
				case 0x19: bus="Host";break;
				default: bus=NULL;
			}
			if(bus) {
				len=snprintf(buf, bufsize, "bus=%s,vendor=%04x,product=%04x,ver=%04x,caps=%x,phys=%s",bus,unsigned(spec.vendor),
					unsigned(spec.product),unsigned(spec.version),unsigned(spec.cap_bitmap),spec.phys);
			} else {
				len=snprintf(buf, bufsize, "bus=%u,vendor=%04x,product=%04x,ver=%04x,caps=%x,phys=%s",unsigned(spec.bustype),unsigned(spec.vendor),
					unsigned(spec.product),unsigned(spec.version),unsigned(spec.cap_bitmap),spec.phys);
			}
		}
		if(doff) *doff += len>=int(bufsize) ? int(bufsize-1) : len;
		return buf;
	}
private:
	virtual bool p_matches(const iot_hwdev_localident* opspec) const override {
		return iot_hwdev_localident_linuxinput::p_matches_hwid(opspec) && iot_hwdev_localident_linuxinput::p_matches_addr(opspec);
	}
	virtual bool p_matches_hwid(const iot_hwdev_localident* opspec0) const override {
		const iot_hwdev_localident_linuxinput* opspec=cast(opspec0);
		if(!opspec) return false;
		if(istmpl) {
			if(tmpl.bustype && (tmpl.bustype & opspec->spec.bustype)==0) return false;
			if(tmpl.cap_bitmap && (tmpl.cap_bitmap & opspec->spec.cap_bitmap)!=tmpl.cap_bitmap) return false;
			if(!tmpl.num_specs) return true;
			for(unsigned i=0;i<tmpl.num_specs;i++) { //check if any exact spec matches
				if(tmpl.spec[i].vendor==opspec->spec.vendor && (!tmpl.spec[i].product ||
					(tmpl.spec[i].product==opspec->spec.product && (tmpl.spec[i].version==0xFFFF || tmpl.spec[i].version==opspec->spec.version))))
						return true;
			}
			return false;
		}
		return spec.bustype==opspec->spec.bustype && 
			spec.vendor==opspec->spec.vendor && 
			spec.product==opspec->spec.product && 
			spec.version==opspec->spec.version &&
			spec.cap_bitmap==opspec->spec.cap_bitmap &&
			strcmp(spec.phys, opspec->spec.phys)==0;
	}
	virtual bool p_matches_addr(const iot_hwdev_localident* opspec0) const override {
		const iot_hwdev_localident_linuxinput* opspec=cast(opspec0);
		if(!opspec) return false;
		if(istmpl) return true;
		return spec.event_index==opspec->spec.event_index;
	}
};

class iot_hwdevcontype_metaclass_linuxinput : public iot_hwdevcontype_metaclass {
	iot_hwdevcontype_metaclass_linuxinput(void) : iot_hwdevcontype_metaclass(0, "unet", "LinuxInput") {}

	PACKED(
		struct serialize_header_t {
			uint32_t format; //format/version of pack
			uint8_t istmpl;
		}
	);
	PACKED(
		struct serialize_spec_t {
			uint8_t event_index; //X in /dev/input/eventX
			uint32_t cap_bitmap;
			uint16_t bustype;  //id of physical bus as per BUS_XXX constants in include/linux/input.h (1-0x1f for now)
			uint16_t vendor;
			uint16_t product;
			uint16_t version;
			uint8_t phys_len; //number of chars in phys excluding NUL-terminator
			char phys[]; //physical connection spec
		}
	);
	PACKED(
		struct serialize_tmpl_t {
			uint8_t num_specs; //number of valid items in spec[]. zero means that no specific product requirements
			uint32_t cap_bitmap; //bitmap of required caps, so 0 means 'no requirements'
			uint32_t bustype; //bitmask of allowed bus types or 0 for any bus type
			struct {
				uint16_t vendor;
				uint16_t product;	 //this field can be 0 meaning "any product" or exact product id
				uint16_t version;	 //used to specify exact product version or 0xFFFF meaning "any"
			} spec[]; //array with specific product requirements
		}
	);

public:
	static const iot_hwdevcontype_metaclass_linuxinput object; //the only instance of this class

private:
	virtual int p_serialized_size(const iot_hwdev_localident* obj0) const override {
		const iot_hwdev_localident_linuxinput* obj=iot_hwdev_localident_linuxinput::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		if(obj->istmpl) return sizeof(serialize_header_t)+sizeof(serialize_tmpl_t)+obj->tmpl.num_specs*sizeof(serialize_tmpl_t::spec[0]);
		return sizeof(serialize_header_t)+sizeof(serialize_spec_t)+strlen(obj->spec.phys);
	}
	virtual int p_serialize(const iot_hwdev_localident* obj0, char* buf, size_t bufsize) const override {
		const iot_hwdev_localident_linuxinput* obj=iot_hwdev_localident_linuxinput::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		if(bufsize<sizeof(serialize_header_t)) return IOT_ERROR_NO_BUFSPACE;
		bufsize-=sizeof(serialize_header_t);

		serialize_header_t *h=(serialize_header_t*)buf;
		if(obj->istmpl) {
			if(bufsize < sizeof(serialize_tmpl_t)+obj->tmpl.num_specs*sizeof(serialize_tmpl_t::spec[0])) return IOT_ERROR_NO_BUFSPACE;
			h->format=repack_uint32(uint32_t(1));
			h->istmpl=1;
			serialize_tmpl_t *t=(serialize_tmpl_t*)(h+1);
			t->num_specs=obj->tmpl.num_specs;
			t->cap_bitmap=repack_uint32(obj->tmpl.cap_bitmap);
			t->bustype=repack_uint32(obj->tmpl.bustype);
			for(uint8_t i=0;i<obj->tmpl.num_specs;i++) {
				t->spec[i].vendor=repack_uint16(obj->tmpl.spec[i].vendor);
				t->spec[i].product=repack_uint16(obj->tmpl.spec[i].product);
				t->spec[i].version=repack_uint16(obj->tmpl.spec[i].version);
			}
		} else {
			size_t len=strlen(obj->spec.phys);
			if(bufsize < sizeof(serialize_spec_t)+len) return IOT_ERROR_NO_BUFSPACE;
			h->format=repack_uint32(uint32_t(1));
			h->istmpl=0;
			serialize_spec_t *s=(serialize_spec_t*)(h+1);
			s->event_index=obj->spec.event_index;
			s->cap_bitmap=repack_uint32(obj->spec.cap_bitmap);
			s->bustype=repack_uint16(obj->spec.bustype);
			s->vendor=repack_uint16(obj->spec.vendor);
			s->product=repack_uint16(obj->spec.product);
			s->version=repack_uint16(obj->spec.version);
			s->phys_len=uint8_t(len);
			if(len>0) memcpy(s->phys, obj->spec.phys, len);
		}
		return 0;
	}
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const override {
		return 0;
	}
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const override {
		return 0;
	}
};

const iot_hwdevcontype_metaclass_linuxinput iot_hwdevcontype_metaclass_linuxinput::object; //the only instance of this class


iot_hwdev_localident_linuxinput::iot_hwdev_localident_linuxinput(void) : iot_hwdev_localident(&iot_hwdevcontype_metaclass_linuxinput::object)
{
}
const iot_hwdev_localident_linuxinput* iot_hwdev_localident_linuxinput::cast(const iot_hwdev_localident* ident) {
	if(!ident) return NULL;
	return ident->get_metaclass()==&iot_hwdevcontype_metaclass_linuxinput::object ? static_cast<const iot_hwdev_localident_linuxinput*>(ident) : NULL;
}


/*


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

	linuxinput_iface(void) : iot_hwdevident_iface(DEVCONTYPE_CUSTOM_LINUXINPUT) {
	}

	void init_localident(iot_hwdev_localident_t* dev_ident, uint32_t detector_module_id) { //must be called first to init iot_hwdev_localident_t structure
		dev_ident->contype=contype;
		dev_ident->detector_module_id=detector_module_id;
		data_t *data=(data_t*)dev_ident->data;
		*data={ //init as template
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

	virtual const char* get_name(void) const override {
		return DEVCONTYPESTR_CUSTOM_LINUXINPUT;
	}

private:
	virtual bool from_json(json_object* obj, char* dev_data) const override { //must convert json data into correct binary representation and return true if provided obj is valid
		return false; //TODO
	}
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
	virtual size_t to_json(const char* dev_data, char* buf, size_t bufsize) const override { //actual encoder to json
//		data_t* data=(data_t*)dev_data;

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
*/

class iot_hwdev_details_linuxinput : public iot_hwdev_details {
public:
	char name[256];
	char phys[64];
	input_id input; //__u16 bustype;__u16 vendor;__u16 product;__u16 version;

	uint32_t cap_bitmap; //bitmap of available capabilities. we process: EV_KEY, EV_LED, EV_SW, EV_SND
	uint32_t keys_bitmap[(KEY_CNT+31)/32]; //when EV_KEY capability present, bitmap of available keys as reported by driver (this is NOT physically present buttons but they can be present)
	uint16_t leds_bitmap; //when EV_LED capability present, bitmap of available leds as reported by driver (this is NOT physically present leds but they can be present)
	uint16_t sw_bitmap; //when EV_SW capability present, bitmap of available switch events
	uint8_t snd_bitmap; //when EV_SND capability present, bitmap of available sound capabilities
	uint8_t event_index; //X in /dev/input/eventX
	bool data_valid=false;


	iot_hwdev_details_linuxinput(void) : iot_hwdev_details(&iot_hwdevcontype_metaclass_linuxinput::object) {}

	static const iot_hwdev_details_linuxinput* cast(const iot_hwdev_details* data) {
		if(!data || !data->is_valid()) return NULL;
		return data->get_metaclass()==&iot_hwdevcontype_metaclass_linuxinput::object ? static_cast<const iot_hwdev_details_linuxinput*>(data) : NULL;
	}

	virtual size_t get_size(void) const override {
		return sizeof(*this);
	}

	bool operator==(const iot_hwdev_details_linuxinput &op) {
		if(&op==this) return true;
		if(data_valid!=op.data_valid) return false;
		if(!data_valid) return true; //both invalid

		if(strcmp(name, op.name)!=0) return false;
		if(strcmp(phys, op.phys)!=0) return false;
		if(memcmp(&input, &op.input, sizeof(input))) return false;
		if(cap_bitmap!=op.cap_bitmap || leds_bitmap!=op.leds_bitmap || sw_bitmap!=op.sw_bitmap || snd_bitmap!=op.snd_bitmap || 
			memcmp(keys_bitmap, op.keys_bitmap, sizeof(keys_bitmap))) return false;
		return true;
	}
	bool operator!=(const iot_hwdev_details_linuxinput &op) {
		return !((*this)==op);
	}

	const char* read_inputdev_caps(int fd, uint8_t index) {
		data_valid=false;
		event_index=index;

		const char* errstr=NULL;
		do { //create block for common error processing
			if(ioctl(fd, EVIOCGID, &input)==-1) {errstr="ioctl for id";break;} //get bus, vendor, product, version

			if(ioctl(fd, EVIOCGNAME(sizeof(name)), name)==-1) {errstr="ioctl for name";break;} //get name
			if(!name[0]) {
				strcpy(name,"N/A"); //ensure name is not empty
			} else {
				name[sizeof(name)-1]='\0'; //ensure NUL-terminated
			}
			if(ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys)==-1) {errstr="ioctl for phys";break;} //get phys
			phys[sizeof(phys)-1]='\0'; //ensure NUL-terminated

			if(ioctl(fd, EVIOCGBIT(0,sizeof(cap_bitmap)), &cap_bitmap)==-1) {errstr="ioctl for cap bitmap";break;} //get capability bitmap
			if(bitmap32_test_bit(&cap_bitmap, EV_KEY)) { //has EV_KEY cap
				if(ioctl(fd, EVIOCGBIT(EV_KEY,sizeof(keys_bitmap)), keys_bitmap)==-1) {errstr="ioctl for keys bitmap";break;} //get available keys bitmap
			} else memset(keys_bitmap, 0, sizeof(keys_bitmap));
			if(bitmap32_test_bit(&cap_bitmap, EV_LED)) { //has EV_LED cap
				if(ioctl(fd, EVIOCGBIT(EV_LED,sizeof(leds_bitmap)), &leds_bitmap)==-1) {errstr="ioctl for leds bitmap";break;} //get available leds bitmap
			} else leds_bitmap=0;
			if(bitmap32_test_bit(&cap_bitmap, EV_SW)) { //has EV_SW cap
				if(ioctl(fd, EVIOCGBIT(EV_SW,sizeof(sw_bitmap)), &sw_bitmap)==-1) {errstr="ioctl for sw bitmap";break;} //get available switch events bitmap
			} else sw_bitmap=0;
			if(bitmap32_test_bit(&cap_bitmap, EV_SND)) { //has EV_SND cap
				if(ioctl(fd, EVIOCGBIT(EV_SND,sizeof(snd_bitmap)), &snd_bitmap)==-1) {errstr="ioctl for snd bitmap";break;} //get available sound caps bitmap
			} else snd_bitmap=0;
			data_valid=true;
		} while(0);
		return errstr;
	}

};
/*
struct devcontype_linuxinput_t { //represents custom data for devices with DEVCONTYPE_CUSTOM_LINUXINPUT connection type
	char name[256];
	input_id input; //__u16 bustype;__u16 vendor;__u16 product;__u16 version;

	uint32_t cap_bitmap; //bitmap of available capabilities. we process: EV_KEY, EV_LED, EV_SW, EV_SND
	uint32_t keys_bitmap[(KEY_CNT+31)/32]; //when EV_KEY capability present, bitmap of available keys as reported by driver (this is NOT physically present buttons but they can be present)
	uint16_t leds_bitmap; //when EV_LED capability present, bitmap of available leds as reported by driver (this is NOT physically present leds but they can be present)
	uint16_t sw_bitmap; //when EV_SW capability present, bitmap of available switch events
	uint8_t snd_bitmap; //when EV_SND capability present, bitmap of available sound capabilities
	uint8_t event_index; //X in /dev/input/eventX
};

*/
//common functions
//fills devcontype_linuxinput_t struct with input device capabilities
//returns NULL on success or error descr on error (with errno properly set to OS error)


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
					kapi_outlog_info("Hwdevice was removed: type=" DEVCONTYPESTR_CUSTOM_LINUXINPUT ", input=%d",i);
					if(!devinfo[i].error) { //device was added to registry, so must be removed
						err=kapi_hwdev_registry_action(IOT_ACTION_REMOVE, &devinfo[i].ident, NULL);
						if(err) kapi_outlog_error("Cannot remove device from registry: %s", kapi_strerror(err));
						if(err==IOT_ERROR_TEMPORARY_ERROR) continue; //retry
						//success or critical error
						devinfo[i].present=false;
					}
					continue;
				}
				err=ident.init_spec(uint8_t(i), fulldevinfo[i].input.bustype, fulldevinfo[i].input.vendor, fulldevinfo[i].input.product, fulldevinfo[i].input.version, fulldevinfo[i].cap_bitmap, fulldevinfo[i].phys);
				if(err) {
					kapi_outlog_error("Cannot fill device local identity: %s", kapi_strerror(err));
				} else {
					//check if device looks the same
					if(devinfo[i].ident.matches(&ident)) continue; //same

					kapi_outlog_info("Hwdevice was replaced: type=" DEVCONTYPESTR_CUSTOM_LINUXINPUT ", input=%d, new name='%s'",i, fulldevinfo[i].name);

					err=kapi_hwdev_registry_action(IOT_ACTION_ADD, &ident, &fulldevinfo[i]);
					if(err) kapi_outlog_error("Cannot update device in registry: %s", kapi_strerror(err));
				}
				if(err==IOT_ERROR_TEMPORARY_ERROR) continue; //retry
				//success or critical error
				devinfo[i].error = !!err;
				devinfo[i].ident=ident;
			} else  //previous state was absent
				if(i<n && fulldevinfo[i].data_valid) { //new state is present, so NEW DEVICE ADDED
					kapi_outlog_info("Detected new hwdevice with type=" DEVCONTYPESTR_CUSTOM_LINUXINPUT ", input=%d, name='%s'",i, fulldevinfo[i].name);
					err=ident.init_spec(uint8_t(i), fulldevinfo[i].input.bustype, fulldevinfo[i].input.vendor, fulldevinfo[i].input.product, fulldevinfo[i].input.version, fulldevinfo[i].cap_bitmap, fulldevinfo[i].phys);
					if(err) {
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
	static int deinit_instance(iot_device_detector_base* instance) {
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


static iot_iface_device_detector_t detector_iface = {
//	.num_hwdevcontypes = sizeof(detector_devcontypes)/sizeof(detector_devcontypes[0]),
	.accepts_manual = 0,
	.cpu_loading = 0,

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
	enum internal_error_t {
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
	static int init_instance(iot_device_driver_base**instance, uv_thread_t thread, const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data, iot_devifaces_list* devifaces) {
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

		if(!devifaces->num) return IOT_ERROR_DEVICE_NOT_SUPPORTED;

		input_drv_instance *inst=new input_drv_instance(thread, dev_ident, dev_data, have_kbd, have_leds, have_tone, have_sw);
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
		delete inst;

		return 0;
	}
	static int check_device(const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data) {
		if(!iot_hwdev_localident_linuxinput::cast(dev_ident->local)) return IOT_ERROR_DEVICE_NOT_SUPPORTED;
		if(!dev_data || !iot_hwdev_details_linuxinput::cast(dev_data)) return IOT_ERROR_INVALID_DEVICE_DATA; //devcontype_linuxinput_t has fixed size
//		devcontype_linuxinput_t *devinfo=(devcontype_linuxinput_t*)(dev_data->custom_data);
//		if(!(devinfo->cap_bitmap & (EV_KEY | EV_LED))) return IOT_ERROR_DEVICE_NOT_SUPPORTED; NOW SUPPORT ALL DEVICES DETECTED BY OUR DETECTOR
		return 0;
	}
/////////////public methods


private:
	input_drv_instance(uv_thread_t thread, const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data, bool have_kbd, bool have_leds, bool have_tone, bool have_sw): 
			iot_device_driver_base(thread), have_kbd(have_kbd), have_leds(have_leds), have_tone(have_tone), have_sw(have_sw)
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

		uv_timer_init(loop, &tonetimer_watcher);
		tonetimer_watcher.data=this;

		int err=setup_device_polling();
		started=true;
		return err;
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
		if(!stopping && tone_playing && uv_is_active((uv_handle_t*)&io_watcher)) {
			stopping=true;
			toneplay_stop();
			return IOT_ERROR_TRY_AGAIN;
		}

		stop_device_polling(false);

		uv_close((uv_handle_t*)&timer_watcher, NULL);
		return 0;
	}

//iot_device_driver_base methods
	virtual int device_open(const iot_conn_drvview* conn) {
		assert(uv_thread_self()==thread);
		kapi_notify_write_avail(conn, true);
		const iot_devifacetype_metaclass* ifacetype=conn->deviface->get_metaclass();
		if(ifacetype==&iot_devifacetype_metaclass_keyboard::object) {
			if(!have_kbd) return IOT_ERROR_DEVICE_NOT_SUPPORTED;
			if(conn_kbd) return IOT_ERROR_LIMIT_REACHED;
			conn_kbd=conn;

			iot_deviface__keyboard_DRV iface(conn);
			int err=iface.send_set_state(keys_state);
			assert(err==0);
		} else if(ifacetype==&iot_devifacetype_metaclass_activatable::object) {
			if(!have_leds) return IOT_ERROR_DEVICE_NOT_SUPPORTED;
			if(conn_leds) return IOT_ERROR_LIMIT_REACHED;
			conn_leds=conn;
			want_leds_bitmap=want_leds_state=0;

			iot_deviface__activatable_DRV iface(conn);
			int err=iface.send_current_state(leds_state, dev_info.leds_bitmap);
			assert(err==0);
		} else if(ifacetype==&iot_devifacetype_metaclass_toneplayer::object) {
			if(!have_tone) return IOT_ERROR_DEVICE_NOT_SUPPORTED;
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
			return IOT_ERROR_DEVICE_NOT_SUPPORTED;
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
			write_todevice();
			return 0;
		} while(0);
		//common error processing
		stop_device_polling(criterror, temperr, retrytm);
		if(criterror) return IOT_ERROR_CRITICAL_ERROR; //driver instance should be deinited
		return 0;
	}
	void stop_device_polling(bool criterror, internal_error_t temperr=ERR_NONE, int retrytm=30) {
		if(uv_is_active((uv_handle_t*)&io_watcher)) uv_close((uv_handle_t*)&io_watcher, NULL);
		if(eventfd>=0) {
			close(eventfd);
			eventfd=-1;
		}

		if(criterror) {
			if(started) kapi_self_abort(IOT_ERROR_CRITICAL_ERROR);
			return;
		}

		if(temperr!=ERR_NONE) {
			if(temperr==internal_error) error_count++;
			else {
				internal_error=temperr;
				error_count=1;
			}
			uv_timer_stop(&timer_watcher);
			uv_timer_start(&timer_watcher, on_timer_static, (error_count>10 ? 10 : error_count)*retrytm*1000, 0);
			return;
		}
		//here no errors, just stopped polling and closed device
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
		if(uv_is_active((uv_handle_t*)&io_watcher)) {
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
		if(!uv_is_active((uv_handle_t*)&io_watcher)) return; //currently no connection to device
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

static iot_iface_device_driver_t driver_iface = {
//	.num_devclassids = 0,
	.num_hwdevcontypes = 0,
	.cpu_loading = 3,

	.hwdevcontypes = NULL,
	.init_instance = &input_drv_instance::init_instance,
	.deinit_instance = &input_drv_instance::deinit_instance,
	.check_device = &input_drv_instance::check_device
};


iot_moduleconfig_t IOT_MODULE_CONF(inputlinux)={
	.title = "Linux Input devices support",
	.descr = "Allows to utilize devices provided by Linux 'input' abstraction layer. Requires 'evdev' kernel module.",
//	.module_id = MODULEID_inputlinux, //Registered ID of this module. Must correspond to its full name in registry
	.version = 0x000100001,
	.config_version = 0,
//	.num_devifaces = 0,
//	.num_devcontypes = 1,
	.init_module = NULL,
	.deinit_module = NULL,
//	.deviface_config = NULL,
//	.devcontype_config = detector_devcontype_config,
	.iface_node = NULL,
	.iface_device_driver = &driver_iface,
	.iface_device_detector = &detector_iface
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

