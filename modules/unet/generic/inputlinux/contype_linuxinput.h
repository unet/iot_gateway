#ifndef CONTYPE_LINUXINPUT_H
#define CONTYPE_LINUXINPUT_H


class iot_hwdev_localident_linuxinput : public iot_hwdev_localident {
	friend class iot_hwdevcontype_metaclass_linuxinput;
	struct spec_t {
		uint16_t vendor,
			product,	 //this field can be 0 meaning "any product" or exact product id
			version;	 //used to specify exact product version (only if product is exact) or 0xFFFF meaning "any"
	};
	union {
		struct {
//hwid:
			uint16_t bustype,  //id of physical bus as per BUS_XXX constants in include/linux/input.h (1-0x1f for now)
				vendor,
				product,
				version;
			uint32_t cap_bitmap;
//address:
			char phys[40]; //physical connection spec, empty for any
//			uint8_t event_index; //X in /dev/input/eventX
		} spec;
		struct {
			uint32_t bustype; //bitmask of allowed bus types or 0 for any bus type
			uint32_t cap_bitmap; //bitmap of required caps, so 0 means 'no requirements'
			spec_t spec[6]; //array with specific product requirements
			uint8_t num_specs; //number of valid items in spec[]. zero means that no specific product requirements
			char phys[40]; //physical connection spec, empty for any
		} tmpl;
	};
	bool istmpl;
//
public:
	
	iot_hwdev_localident_linuxinput(void);
	iot_hwdev_localident_linuxinput(/*uint8_t event_index, */uint16_t bustype, uint16_t vendor, uint16_t product, uint16_t version, uint32_t cap_bitmap, const char* phys);
	iot_hwdev_localident_linuxinput(uint32_t bustypemask, uint32_t cap_bitmap, uint8_t num_specs, const spec_t (&spec)[6], const char* phys=NULL);

	static const iot_hwdev_localident_linuxinput* cast(const iot_hwdev_localident* ident);

	int init_spec(/*uint8_t event_index, */uint16_t bustype, uint16_t vendor, uint16_t product, uint16_t version, uint32_t cap_bitmap, const char* phys) {
		if(!cap_bitmap || !phys) return IOT_ERROR_INVALID_ARGS;
		istmpl=false;
//		spec.event_index=event_index;
		spec.bustype=bustype;
		spec.vendor=vendor;
		spec.product=product;
		spec.version=version;
		spec.cap_bitmap=cap_bitmap;
		snprintf(spec.phys, sizeof(spec.phys), "%s", phys);
		return 0;
	}
	int init_tmpl(uint32_t bustypemask, uint32_t cap_bitmap, uint8_t num_specs, const spec_t (&spec)[6], const char* phys=NULL) {
		if(num_specs>6) return IOT_ERROR_INVALID_ARGS;
		istmpl=true;
		tmpl.bustype=bustypemask;
		tmpl.cap_bitmap=cap_bitmap;
		tmpl.num_specs=num_specs;
		if(!phys) tmpl.phys[0]='\0';
			else snprintf(tmpl.phys, sizeof(tmpl.phys), "%s", phys);
		uint8_t i;
		for(i=0;i<num_specs;i++) tmpl.spec[i]=spec[i];
		for(;i<6;i++) tmpl.spec[i]={};
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
			if(!tmpl.phys[0]) len=snprintf(buf, bufsize, "any event index");
				else len=snprintf(buf, bufsize, "phys=%s",tmpl.phys);
		} else {
//			len=snprintf(buf, bufsize, "event index=%u",unsigned(spec.event_index));
			len=snprintf(buf, bufsize, "phys=%s",spec.phys);
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
				len=snprintf(buf, bufsize, "bus=%s,vendor=%04x,product=%04x,ver=%04x,caps=%x",bus,unsigned(spec.vendor), //,phys=%s
					unsigned(spec.product),unsigned(spec.version),unsigned(spec.cap_bitmap)); //,spec.phys
			} else {
				len=snprintf(buf, bufsize, "bus=%u,vendor=%04x,product=%04x,ver=%04x,caps=%x",unsigned(spec.bustype),unsigned(spec.vendor), //,phys=%s
					unsigned(spec.product),unsigned(spec.version),unsigned(spec.cap_bitmap)); //,spec.phys
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
			if(tmpl.bustype && (tmpl.bustype & (1u<<opspec->spec.bustype))==0) return false;
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
			spec.cap_bitmap==opspec->spec.cap_bitmap;// &&
//			strcmp(spec.phys, opspec->spec.phys)==0;
	}
	virtual bool p_matches_addr(const iot_hwdev_localident* opspec0) const override {
		const iot_hwdev_localident_linuxinput* opspec=cast(opspec0);
		if(!opspec) return false;
		if(istmpl) {
			if(!tmpl.phys[0]) return true;
			return strcmp(tmpl.phys, opspec->spec.phys)==0;
		}
//		return spec.event_index==opspec->spec.event_index;
		return strcmp(spec.phys, opspec->spec.phys)==0;
	}
};

class iot_hwdevcontype_metaclass_linuxinput : public iot_hwdevcontype_metaclass {
	iot_hwdevcontype_metaclass_linuxinput(void) : iot_hwdevcontype_metaclass(0, "linuxinput", IOT_VERSION_COMPOSE(0,0,1)) {}

	PACKED(
		struct serialize_header_t {
			uint32_t format; //format/version of pack
			uint8_t istmpl;
		}
	);
	PACKED(
		struct serialize_spec_t {
//			uint8_t event_index; //X in /dev/input/eventX
			uint8_t phys_len; //number of chars in phys excluding NUL-terminator
			uint16_t bustype;  //id of physical bus as per BUS_XXX constants in include/linux/input.h (1-0x1f for now)
			uint32_t cap_bitmap;
			uint16_t vendor;
			uint16_t product;
			uint16_t version;
			char phys[]; //physical connection spec
		}
	);
	PACKED(
		struct serialize_tmpl_t {
			uint8_t num_specs; //number of valid items in spec[]. zero means that no specific product requirements
			uint32_t cap_bitmap; //bitmap of required caps, so 0 means 'no requirements'
			uint32_t bustype; //bitmask of allowed bus types or 0 for any bus type
			char phys[40]; //physical connection spec, NUL-terminated
			struct {
				uint16_t vendor;
				uint16_t product;	 //this field can be 0 meaning "any product" or exact product id
				uint16_t version;	 //used to specify exact product version or 0xFFFF meaning "any"
			} spec[]; //array with specific product requirements
		}
	);

public:
	static iot_hwdevcontype_metaclass_linuxinput object; //the only instance of this class

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
			snprintf(t->phys, sizeof(t->phys), "%s", obj->tmpl.phys);
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
//			s->event_index=obj->spec.event_index;
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
		assert(false);
		return 0;
	}
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const override {
		if(!buf) return sizeof(iot_hwdev_localident_linuxinput);
		if(bufsize<sizeof(iot_hwdev_localident_linuxinput)) return IOT_ERROR_NO_BUFSPACE;

		if(!json_object_is_type(json, json_type_object)) return IOT_ERROR_BAD_DATA;

		iot_hwdev_localident_linuxinput *ident;
		json_object* val=NULL;
		if(json_object_object_get_ex(json, "is_tmpl", &val) && json_object_get_boolean(val)) { //this is template
			assert(false);
		} else { //exact spec
			uint16_t bustype=0,
				vendor=0,
				product=0,
				version=0;
			uint32_t cap_bitmap=0;
//			uint8_t event_index=0; //X in /dev/input/eventX
			const char *phys=NULL;

//			if(json_object_object_get_ex(json, "event_index", &val)) IOT_JSONPARSE_UINT(val, uint8_t, event_index);
			if(json_object_object_get_ex(json, "cap_mask", &val)) IOT_JSONPARSE_UINT(val, uint32_t, cap_bitmap);
			if(json_object_object_get_ex(json, "bustype", &val)) IOT_JSONPARSE_UINT(val, uint16_t, bustype);
			if(json_object_object_get_ex(json, "vendor", &val)) IOT_JSONPARSE_UINT(val, uint16_t, vendor);
			if(json_object_object_get_ex(json, "product", &val)) IOT_JSONPARSE_UINT(val, uint16_t, product);
			if(json_object_object_get_ex(json, "version", &val)) IOT_JSONPARSE_UINT(val, uint16_t, version);
			if(json_object_object_get_ex(json, "phys_path", &val)) phys=json_object_get_string(val);

			ident=new(buf) iot_hwdev_localident_linuxinput();
			if(ident->init_spec(/*event_index, */bustype, vendor, product, version, cap_bitmap, phys)) return IOT_ERROR_BAD_DATA;
		}
		obj=ident;
		return sizeof(iot_hwdev_localident_linuxinput);
	}
	virtual int p_to_json(const iot_hwdev_localident* obj0, json_object* &dst) const override {
		const iot_hwdev_localident_linuxinput* obj=iot_hwdev_localident_linuxinput::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		json_object* ob=json_object_new_object();
		if(!ob) return IOT_ERROR_NO_MEMORY;

		json_object* val;

		if(obj->istmpl) { //{is_tmpl: true, bustype_mask: integer/undefined, cap_mask: integer/undefined, product_specs: [[vendor(integer), product(integer/undefined), version]]/undefined}
			val=json_object_new_boolean(1);
			if(!val) goto nomem;
			json_object_object_add(ob, "is_tmpl", val);
			if(obj->tmpl.bustype>0) {
				val=json_object_new_int64(obj->tmpl.bustype);
				if(!val) goto nomem;
				json_object_object_add(ob, "bustype_mask", val);
			} //alse "any" bustype allowed. represent it as undefined
			if(obj->tmpl.cap_bitmap>0) {
				val=json_object_new_int64(obj->tmpl.cap_bitmap);
				if(!val) goto nomem;
				json_object_object_add(ob, "cap_mask", val);
			} //alse "any" capability allowed. represent it as undefined
			if(obj->tmpl.phys[0]) {
				val=json_object_new_string(obj->tmpl.phys);
				if(!val) goto nomem;
				json_object_object_add(ob, "phys_path", val);
			}
			if(obj->tmpl.num_specs>0) {
				val=json_object_new_array();
				if(!val) goto nomem;
				json_object_object_add(ob, "product_specs", val);
				json_object* arr, *subval;
				for(uint8_t i=0;i<obj->tmpl.num_specs; i++) {
					arr=json_object_new_array();
					if(!arr) goto nomem;
					json_object_array_add(val, arr);
					//add vendor
					subval=json_object_new_int(obj->tmpl.spec[i].vendor);
					if(!subval) goto nomem;
					json_object_array_add(arr, subval);
					if(obj->tmpl.spec[i].product>0) { //product specifed, add it
						subval=json_object_new_int(obj->tmpl.spec[i].product);
						if(!subval) goto nomem;
						json_object_array_add(arr, subval);
						if(obj->tmpl.spec[i].version!=0xFFFF) { //version specifed, add it
							subval=json_object_new_int(obj->tmpl.spec[i].version);
							if(!subval) goto nomem;
							json_object_array_add(arr, subval);
						}
					}
				}
			}
		} else { //{event_index: integer, cap_mask: integer, bustype: integer, vendor: integer, product: integer, version: integer, phys_path: string}
//			val=json_object_new_int(obj->spec.event_index);
//			if(!val) goto nomem;
//			json_object_object_add(ob, "event_index", val);

			val=json_object_new_int64(obj->spec.cap_bitmap);
			if(!val) goto nomem;
			json_object_object_add(ob, "cap_mask", val);

			val=json_object_new_int(obj->spec.bustype);
			if(!val) goto nomem;
			json_object_object_add(ob, "bustype", val);

			val=json_object_new_int(obj->spec.vendor);
			if(!val) goto nomem;
			json_object_object_add(ob, "vendor", val);

			val=json_object_new_int(obj->spec.product);
			if(!val) goto nomem;
			json_object_object_add(ob, "product", val);

			val=json_object_new_int(obj->spec.version);
			if(!val) goto nomem;
			json_object_object_add(ob, "version", val);

			val=json_object_new_string(obj->spec.phys);
			if(!val) goto nomem;
			json_object_object_add(ob, "phys_path", val);
		}
		dst=ob;
		return 0;
nomem:
		json_object_put(ob);
		return IOT_ERROR_NO_MEMORY;
	}
};

inline iot_hwdev_localident_linuxinput::iot_hwdev_localident_linuxinput(void)
: iot_hwdev_localident(&iot_hwdevcontype_metaclass_linuxinput::object)
{
}
inline iot_hwdev_localident_linuxinput::iot_hwdev_localident_linuxinput(/*uint8_t event_index, */uint16_t bustype, uint16_t vendor, uint16_t product, uint16_t version, uint32_t cap_bitmap, const char* phys)
: iot_hwdev_localident(&iot_hwdevcontype_metaclass_linuxinput::object)
{
	if(init_spec(/*event_index, */bustype, vendor, product, version, cap_bitmap, phys)) {
		assert(false);
	}
}
inline iot_hwdev_localident_linuxinput::iot_hwdev_localident_linuxinput(uint32_t bustypemask, uint32_t cap_bitmap, uint8_t num_specs, const spec_t (&spec)[6], const char* phys)
: iot_hwdev_localident(&iot_hwdevcontype_metaclass_linuxinput::object)
{
	if(init_tmpl(bustypemask, cap_bitmap, num_specs, spec, phys)) {
		assert(false);
	}
}

inline const iot_hwdev_localident_linuxinput* iot_hwdev_localident_linuxinput::cast(const iot_hwdev_localident* ident) {
	if(!ident) return NULL;
	return ident->get_metaclass()==&iot_hwdevcontype_metaclass_linuxinput::object ? static_cast<const iot_hwdev_localident_linuxinput*>(ident) : NULL;
}



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
	iot_hwdev_details_linuxinput(const iot_hwdev_details_linuxinput& ob) : iot_hwdev_details(ob) {
		memcpy(name, ob.name, sizeof(name));
		memcpy(phys, ob.phys, sizeof(phys));
		input=ob.input;
		cap_bitmap=ob.cap_bitmap;
		memcpy(keys_bitmap, ob.keys_bitmap, sizeof(keys_bitmap));
		leds_bitmap=ob.leds_bitmap;
		sw_bitmap=ob.sw_bitmap;
		snd_bitmap=ob.snd_bitmap;
		event_index=ob.event_index;
		data_valid=ob.data_valid;
	}

	static const iot_hwdev_details_linuxinput* cast(const iot_hwdev_details* data) {
		if(!data || !data->is_valid()) return NULL;
		return data->get_metaclass()==&iot_hwdevcontype_metaclass_linuxinput::object ? static_cast<const iot_hwdev_details_linuxinput*>(data) : NULL;
	}

	virtual size_t get_size(void) const override {
		return sizeof(*this);
	}
	virtual bool copy_to(void* buffer, size_t buffer_size) const override {
		if(buffer_size<sizeof(*this)) return false;
		if((char*)this == buffer) { //copy to myself?
			assert(false);
			return false;
		}
		new(buffer) iot_hwdev_details_linuxinput(*this);
		return true;
	}
	virtual bool fill_localident(void *buffer, size_t buffer_size, const iot_hwdev_localident** identptr) const override {
		//function must know how to calculate size of localident object. in THIS class this size is fixed
		if(buffer_size<sizeof(iot_hwdev_localident_linuxinput)) return false;
		iot_hwdev_localident_linuxinput *ident=new(buffer) iot_hwdev_localident_linuxinput();
		int err=ident->init_spec(/*event_index, */input.bustype, input.vendor, input.product, input.version, cap_bitmap, phys);
		if(err) return false;
		if(identptr) *identptr=ident;
		return true;
	}
	virtual bool operator==(const iot_hwdev_details& _op) const override {
		if(&_op==this) return true;
		const iot_hwdev_details_linuxinput* op=cast(&_op); //check type
		if(!op) return false;

		if(data_valid!=op->data_valid) return false;
		if(!data_valid) return true; //both invalid

		if(memcmp(&input, &op->input, sizeof(input))) return false;
		if(strcmp(name, op->name)!=0) return false;
		if(strcmp(phys, op->phys)!=0) return false;
		if(cap_bitmap!=op->cap_bitmap || leds_bitmap!=op->leds_bitmap || sw_bitmap!=op->sw_bitmap || snd_bitmap!=op->snd_bitmap || 
			memcmp(keys_bitmap, op->keys_bitmap, sizeof(keys_bitmap))) return false;
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

#endif // CONTYPE_LINUXINPUT_H
