#ifndef IOT_HWDEVCONTYPE_GPIO_H
#define IOT_HWDEVCONTYPE_GPIO_H
//Representation for hardware connection over any primitive line which gives no systematic ways to identify devices.  in particular serial (COM-port, RS-232, UART)


//up to 152 ports can be addressed per controller
#define IOT_HWDEV_IDENT_GPIO_PORTMASKLEN 19

class iot_hwdev_localident_gpio : public iot_hwdev_localident {
	friend class iot_hwdevcontype_metaclass_gpio;
	struct spec_t {
		uint32_t driver_module_id;//always exact
		uint16_t vendor,
			product,	//unchecked if vendor not exact
			version;	//unchecked if vendor not exact
		uint8_t ctrl_id; //GPIO controller ID
		uint8_t mask; //bitmask showing which fields are exact (bit 0 - vendor, bit 1 - product, bit 2 - version, bit 3 - controller ID)
	};
	union {
		struct {
//hwid:
			uint32_t driver_module_id; //driver module which interprets vendor, product and version
			uint16_t vendor;		//driver-interpreted identification of device.
			uint16_t product;
			uint16_t version;
//address:
			uint8_t ctrl_id; //GPIO controller ID
			uint8_t port_mask[IOT_HWDEV_IDENT_GPIO_PORTMASKLEN];
		} spec;
		struct {
			spec_t spec[8]; //array with specific product requirements
			uint8_t num_specs; //number of valid items in spec[]. zero means that no specific product requirements
		} tmpl;
	};
	bool istmpl;
//
public:
	
	iot_hwdev_localident_gpio(void);
	iot_hwdev_localident_gpio(uint8_t portmask_len, const uint8_t* port_mask, uint8_t ctrl_id, uint32_t driver_module_id, uint16_t vendor, uint16_t product, uint16_t version);
	iot_hwdev_localident_gpio(uint8_t num_specs, const spec_t (&spec)[8]);

	static const iot_hwdev_localident_gpio* cast(const iot_hwdev_localident* ident);

	int init_spec(uint8_t portmask_len, const uint8_t* port_mask, uint8_t ctrl_id, uint32_t driver_module_id, uint16_t vendor, uint16_t product, uint16_t version) {
		if(!portmask_len || portmask_len>IOT_HWDEV_IDENT_GPIO_PORTMASKLEN || !driver_module_id) return IOT_ERROR_INVALID_ARGS;
		//check there is at least one bit set
		uint16_t i;
		for(i=0;i<portmask_len;i++) if(port_mask[i]) break;
		if(i>=portmask_len) return IOT_ERROR_INVALID_ARGS;

		istmpl=false;
		memcpy(spec.port_mask, port_mask, portmask_len);
		if(portmask_len<IOT_HWDEV_IDENT_GPIO_PORTMASKLEN) memset(spec.port_mask+portmask_len,0,IOT_HWDEV_IDENT_GPIO_PORTMASKLEN-portmask_len);
		spec.ctrl_id=ctrl_id;
		spec.driver_module_id=driver_module_id;
		spec.vendor=vendor;
		spec.product=product;
		spec.version=version;
		return 0;
	}
	int init_tmpl(uint8_t num_specs, const spec_t (&spec)[8]) { //portname can be NULL or empty string for "any" port
		if(num_specs>8) return IOT_ERROR_INVALID_ARGS;
		istmpl=true;
		tmpl.num_specs=num_specs;
		uint8_t i;
		for(i=0;i<num_specs;i++) tmpl.spec[i]=spec[i];
		for(;i<8;i++) tmpl.spec[i]={};
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
			len=snprintf(buf, bufsize, "template"); //TODO
		} else {
			len=snprintf(buf, bufsize, "controller=%d,portmask=[ %02X %02X %02X %02X %02X %02X %02X %02X ]",spec.ctrl_id, unsigned(spec.port_mask[0]), unsigned(spec.port_mask[1]), unsigned(spec.port_mask[2]), unsigned(spec.port_mask[3]), unsigned(spec.port_mask[4]), unsigned(spec.port_mask[5]), unsigned(spec.port_mask[6]), unsigned(spec.port_mask[7]));
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
			len=snprintf(buf, bufsize, "driver module id=%u,vendor=%04x,product=%04x,ver=%04x",unsigned(spec.driver_module_id),unsigned(spec.vendor),unsigned(spec.product),unsigned(spec.version));
		}
		if(doff) *doff += len>=int(bufsize) ? int(bufsize-1) : len;
		return buf;
	}
	uint16_t count_ports(void) const { //returns number of set bits in port mask
		if(istmpl) return 0;
		uint16_t num=0;
		for(uint16_t i=0;i<sizeof(spec.port_mask);i++) {
			uint8_t b=spec.port_mask[i];
			for(; b!=0; num++) { //Brian Kernighan algo to count set bits
				b&=b-1; // this clears the LSB-most set bit
			}
		}
		return num;
	}

private:
	virtual bool p_matches(const iot_hwdev_localident* opspec0) const override {
		if(!istmpl) return iot_hwdev_localident_gpio::p_matches_hwid(opspec0) && iot_hwdev_localident_gpio::p_matches_addr(opspec0);
		//is template
		const iot_hwdev_localident_gpio* opspec=cast(opspec0);
		if(!opspec) return false;

		if(!tmpl.num_specs) return true;
		for(unsigned i=0;i<tmpl.num_specs;i++) { //check if any exact spec matches
			//check hwid
			if(tmpl.spec[i].driver_module_id!=opspec->spec.driver_module_id) continue;
			if(tmpl.spec[i].mask & 1) { //vendor exact
				if(tmpl.spec[i].vendor!=opspec->spec.vendor) continue;
				if((tmpl.spec[i].mask & 2) && tmpl.spec[i].product!=opspec->spec.product) continue;
				if((tmpl.spec[i].mask & 4) && tmpl.spec[i].version!=opspec->spec.version) continue;
			}
			//hwid matches. check addr
			if(!(tmpl.spec[i].mask & 8)) return true; //any ctrl_id
			if(tmpl.spec[i].ctrl_id==opspec->spec.ctrl_id) return true;
		}
		return false;
	}
	virtual bool p_matches_hwid(const iot_hwdev_localident* opspec0) const override {
		const iot_hwdev_localident_gpio* opspec=cast(opspec0);
		if(!opspec) return false;
		if(istmpl) {
			if(!tmpl.num_specs) return true;
			for(unsigned i=0;i<tmpl.num_specs;i++) { //check if any exact spec matches
				if(tmpl.spec[i].driver_module_id!=opspec->spec.driver_module_id) continue;
				if(tmpl.spec[i].mask & 1) { //vendor exact
					if(tmpl.spec[i].vendor!=opspec->spec.vendor) continue;
					if((tmpl.spec[i].mask & 2) && tmpl.spec[i].product!=opspec->spec.product) continue;
					if((tmpl.spec[i].mask & 4) && tmpl.spec[i].version!=opspec->spec.version) continue;
				}
				return true;
			}
			return false;
		}
		return spec.driver_module_id==opspec->spec.driver_module_id && 
			spec.vendor==opspec->spec.vendor && 
			spec.product==opspec->spec.product && 
			spec.version==opspec->spec.version;
	}
	virtual bool p_matches_addr(const iot_hwdev_localident* opspec0) const override {
		const iot_hwdev_localident_gpio* opspec=cast(opspec0);
		if(!opspec) return false;
		if(istmpl) {
			if(!tmpl.num_specs) return true;
			for(unsigned i=0;i<tmpl.num_specs;i++) { //check if any exact spec matches
				if(!(tmpl.spec[i].mask & 8)) return true; //any ctrl_id
				if(tmpl.spec[i].ctrl_id==opspec->spec.ctrl_id) return true;
			}
			return false;
		}
		//exact specs. for same controller check that port_mask have intersection
		if(spec.ctrl_id!=opspec->spec.ctrl_id) return false;
		for(uint16_t i=0;i<IOT_HWDEV_IDENT_GPIO_PORTMASKLEN;i++) if(spec.port_mask[i] & opspec->spec.port_mask[i]) return true;
		return false;
	}
};

class iot_hwdevcontype_metaclass_gpio : public iot_hwdevcontype_metaclass {
	iot_hwdevcontype_metaclass_gpio(void) : iot_hwdevcontype_metaclass(0, "gpio", IOT_VERSION_COMPOSE(0,0,1)) {}

	PACKED(
		struct serialize_header_t {
			uint32_t format; //format/version of pack
			uint8_t istmpl;
		}
	);
	PACKED(
		struct serialize_spec_t {
			uint32_t driver_module_id; //driver module which interprets vendor, product and version
			uint16_t vendor;
			uint16_t product;
			uint16_t version;
			uint8_t ctrl_id;
			uint8_t portmask_len; //number of chars in portname
			uint8_t port_mask[];
		}
	);
	PACKED(
		struct serialize_tmpl_t {
			uint8_t num_specs; //number of valid items in spec[]. zero means that no specific product requirements
			struct {
				uint32_t driver_module_id;//always exact
				uint16_t vendor;
				uint16_t product;
				uint16_t version;
				uint8_t ctrl_id; //GPIO controller ID
				uint8_t mask; //bitmask showing which fields are exact (bit 0 - vendor, bit 1 - product, bit 2 - version, bit 3 - ctrl_id)
			} spec[]; //array with specific product requirements
		}
	);

public:
	static iot_hwdevcontype_metaclass_gpio object; //the only instance of this class

private:
	virtual int p_serialized_size(const iot_hwdev_localident* obj0) const override {
		const iot_hwdev_localident_gpio* obj=iot_hwdev_localident_gpio::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		if(obj->istmpl) return sizeof(serialize_header_t)+sizeof(serialize_tmpl_t)+obj->tmpl.num_specs*sizeof(serialize_tmpl_t::spec[0]);
		return sizeof(serialize_header_t)+sizeof(serialize_spec_t)+IOT_HWDEV_IDENT_GPIO_PORTMASKLEN;
	}
	virtual int p_serialize(const iot_hwdev_localident* obj0, char* buf, size_t bufsize) const override {
		const iot_hwdev_localident_gpio* obj=iot_hwdev_localident_gpio::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		if(bufsize<sizeof(serialize_header_t)) return IOT_ERROR_NO_BUFSPACE;
		bufsize-=sizeof(serialize_header_t);
/*TODO
		serialize_header_t *h=(serialize_header_t*)buf;
		if(obj->istmpl) {
			if(bufsize < sizeof(serialize_tmpl_t)+obj->tmpl.num_specs*sizeof(serialize_tmpl_t::spec[0])) return IOT_ERROR_NO_BUFSPACE;
			h->format=repack_uint32(uint32_t(1));
			h->istmpl=1;
			serialize_tmpl_t *t=(serialize_tmpl_t*)(h+1);
			t->num_specs=obj->tmpl.num_specs;
			strcpy(t->portname, obj->tmpl.portname);
			for(uint8_t i=0;i<obj->tmpl.num_specs;i++) {
				t->spec[i].vendor=repack_uint16(obj->tmpl.spec[i].vendor);
				t->spec[i].product=repack_uint16(obj->tmpl.spec[i].product);
				t->spec[i].version=repack_uint16(obj->tmpl.spec[i].version);
				t->spec[i].idspace=obj->tmpl.spec[i].idspace;
				t->spec[i].mask=obj->tmpl.spec[i].mask;
			}
		} else {
			size_t len=strlen(obj->spec.portname);
			if(bufsize < sizeof(serialize_spec_t)+len) return IOT_ERROR_NO_BUFSPACE;
			h->format=repack_uint32(uint32_t(1));
			h->istmpl=0;
			serialize_spec_t *s=(serialize_spec_t*)(h+1);
			s->vendor=repack_uint16(obj->spec.vendor);
			s->product=repack_uint16(obj->spec.product);
			s->version=repack_uint16(obj->spec.version);
			s->idspace=obj->spec.idspace;
			s->portname_len=uint8_t(len);
			if(len>0) memcpy(s->portname, obj->spec.portname, len);
		}
*/
		return 0;
	}
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const override {
		assert(false); //TODO
		return 0;
	}
	//returns negative error code OR number of bytes written to provided buffer OR required buffer size when buf is NULL
	//returned value can be zero (regardless buf was NULL or not) to indicate that buffer was not used (or is not necessary) and that obj was assigned to
	//correct precreated object (may be statically allocated)
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const override {
		if(!buf) return sizeof(iot_hwdev_localident_gpio);
		if(bufsize<sizeof(iot_hwdev_localident_gpio)) return IOT_ERROR_NO_BUFSPACE;

		if(!json_object_is_type(json, json_type_object)) return IOT_ERROR_BAD_DATA;

		iot_hwdev_localident_gpio *ident;
		json_object* val=NULL;
		if(json_object_object_get_ex(json, "is_tmpl", &val) && json_object_get_boolean(val)) { //this is template
			assert(false);
		} else { //exact spec
			uint32_t driver_module_id=0;
			uint16_t vendor=0;		//driver-interpreted identification of device.
			uint16_t product=0;
			uint16_t version=0;
			uint8_t ctrl_id=0;
			uint8_t port_mask[IOT_HWDEV_IDENT_GPIO_PORTMASKLEN];
			memset(port_mask, 0, sizeof(port_mask));
			int numpins=0;

			if(json_object_object_get_ex(json, "driver_module", &val)) IOT_JSONPARSE_UINT(val, uint32_t, driver_module_id);
			if(json_object_object_get_ex(json, "vendor", &val)) IOT_JSONPARSE_UINT(val, uint16_t, vendor);
			if(json_object_object_get_ex(json, "product", &val)) IOT_JSONPARSE_UINT(val, uint16_t, product);
			if(json_object_object_get_ex(json, "version", &val)) IOT_JSONPARSE_UINT(val, uint16_t, version);
			if(json_object_object_get_ex(json, "controller_id", &val)) IOT_JSONPARSE_UINT(val, uint8_t, ctrl_id);

			if(json_object_object_get_ex(json, "pins", &val) && json_object_is_type(val, json_type_array) && (numpins=json_object_array_length(val))>0) {
				for(int pi=0;pi<numpins;pi++) {
					json_object* pin=json_object_array_get_idx(val, pi);
					int32_t pinidx=json_object_get_int(pin);
					if(pinidx<=0 || pinidx>int32_t(sizeof(port_mask)*8)) return IOT_ERROR_BAD_DATA;
					uint32_t byteidx=uint32_t(pinidx) >> 3u;
					uint8_t bitidx=pinidx - (byteidx<<3); //0 - 7
					uint8_t bitmask=(1<<bitidx);
					if(port_mask[byteidx] & bitmask) continue; //duplicate pin. just ignore here
					port_mask[byteidx] |= bitmask;
				}
			}
			ident=new(buf) iot_hwdev_localident_gpio();
			if(ident->init_spec(IOT_HWDEV_IDENT_GPIO_PORTMASKLEN, port_mask, ctrl_id, driver_module_id, vendor, product, version)) return IOT_ERROR_BAD_DATA;
		}
		obj=ident;
		return sizeof(iot_hwdev_localident_gpio);
	}
	virtual int p_to_json(const iot_hwdev_localident* obj0, json_object* &dst) const override {
		const iot_hwdev_localident_gpio* obj=iot_hwdev_localident_gpio::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		json_object* ob=json_object_new_object();
		if(!ob) return IOT_ERROR_NO_MEMORY;

/*
		json_object* val;
		if(obj->istmpl) { //{is_tmpl: true, product_specs: [[idspace(integer), vendor(integer), product(integer/undefined), version(integer/undefined)]]/undefined}
			val=json_object_new_boolean(1);
			if(!val) goto nomem;
			json_object_object_add(ob, "is_tmpl", val);
			if(obj->tmpl.portname[0]) {
				val=json_object_new_string(obj->tmpl.portname);
				if(!val) goto nomem;
				json_object_object_add(ob, "portname", val);
			}
			if(obj->tmpl.num_specs>0) {
				val=json_object_new_array();
				if(!val) goto nomem;
				json_object_object_add(ob, "specs", val);
				json_object* arr, *subval;
				for(uint8_t i=0;i<obj->tmpl.num_specs; i++) {
					arr=json_object_new_array();
					if(!arr) goto nomem;
					json_object_array_add(val, arr);
					//add idspace
					subval=json_object_new_int(obj->tmpl.spec[i].idspace);
					if(!subval) goto nomem;
					json_object_array_add(arr, subval);
					//add mask
					subval=json_object_new_int(obj->tmpl.spec[i].mask);
					if(!subval) goto nomem;
					json_object_array_add(arr, subval);
					if(obj->tmpl.spec[i].mask & 1) { //vendor exact
						//add vendor
						subval=json_object_new_int(obj->tmpl.spec[i].vendor);
						if(!subval) goto nomem;
						json_object_array_add(arr, subval);

						if(obj->tmpl.spec[i].mask & (2|4)) { //product specifed OR version specified, add product or 0
							subval=json_object_new_int((obj->tmpl.spec[i].mask & 2) ? obj->tmpl.spec[i].product : 0);
							if(!subval) goto nomem;
							json_object_array_add(arr, subval);
						}
						if(obj->tmpl.spec[i].mask & 4) { //version specifed, add it
							subval=json_object_new_int(obj->tmpl.spec[i].version);
							if(!subval) goto nomem;
							json_object_array_add(arr, subval);
						}
					}
				}
			}
		} else { //{portname: string, idspace: integer, vendor: integer, product: integer, version: integer}
			val=json_object_new_string(obj->spec.portname);
			if(!val) goto nomem;
			json_object_object_add(ob, "portname", val);

			val=json_object_new_int(obj->spec.idspace);
			if(!val) goto nomem;
			json_object_object_add(ob, "idspace", val);

			val=json_object_new_int(obj->spec.vendor);
			if(!val) goto nomem;
			json_object_object_add(ob, "vendor", val);

			val=json_object_new_int(obj->spec.product);
			if(!val) goto nomem;
			json_object_object_add(ob, "product", val);

			val=json_object_new_int(obj->spec.version);
			if(!val) goto nomem;
			json_object_object_add(ob, "version", val);
		}
		dst=ob;
		return 0;
nomem:
		json_object_put(ob);
*/
		return IOT_ERROR_NO_MEMORY;
	}
};


inline iot_hwdev_localident_gpio::iot_hwdev_localident_gpio(void)
: iot_hwdev_localident(&iot_hwdevcontype_metaclass_gpio::object)
{
}

inline iot_hwdev_localident_gpio::iot_hwdev_localident_gpio(uint8_t portmask_len, const uint8_t* port_mask, uint8_t ctrl_id, uint32_t driver_module_id, uint16_t vendor, uint16_t product, uint16_t version)
: iot_hwdev_localident(&iot_hwdevcontype_metaclass_gpio::object)
{
	if(init_spec(portmask_len, port_mask, ctrl_id, driver_module_id, vendor, product, version)) {
		assert(false);
	}
}
inline iot_hwdev_localident_gpio::iot_hwdev_localident_gpio(uint8_t num_specs, const spec_t (&spec)[8])
: iot_hwdev_localident(&iot_hwdevcontype_metaclass_gpio::object)
{
	if(init_tmpl(num_specs, spec)) {
		assert(false);
	}
}

inline const iot_hwdev_localident_gpio* iot_hwdev_localident_gpio::cast(const iot_hwdev_localident* ident) {
	if(!ident) return NULL;
	return ident->get_metaclass()==&iot_hwdevcontype_metaclass_gpio::object ? static_cast<const iot_hwdev_localident_gpio*>(ident) : NULL;
}

class iot_hwdev_details_gpio : public iot_hwdev_details {
public:
	char name[256];
	json_object* params; //params for driver. driver must interpret it according to vendor/product/version
	uint32_t driver_module_id;
	uint16_t vendor;
	uint16_t product;
	uint16_t version;

	uint8_t ctrl_id; //GPIO controller ID
	uint8_t num_ports; //number of items in ports[] array
	uint8_t ports[32]; //list of GPIO unique port ids relative to controller (0-based)

	iot_hwdev_details_gpio(void) : iot_hwdev_details(&iot_hwdevcontype_metaclass_gpio::object) {
		params=NULL;
		driver_module_id=0;
		vendor=product=version=0;
		ctrl_id=0;
		name[0]='\0';
		num_ports=0;
	}
	iot_hwdev_details_gpio(const iot_hwdev_details_gpio& ob) : iot_hwdev_details(ob) {
		params=ob.params;
		if(params) json_object_get(params);
		memcpy(name, ob.name, sizeof(name));
		driver_module_id=ob.driver_module_id;
		vendor=ob.vendor;
		product=ob.product;
		version=ob.version;
		ctrl_id=ob.ctrl_id;
		num_ports=ob.num_ports;
		if(num_ports>0) memcpy(ports, ob.ports, sizeof(ports[0])*num_ports);
	}
	~iot_hwdev_details_gpio(void) {
		if(params) {
			json_object_put(params);
			params=NULL;
		}
	}

	static const iot_hwdev_details_gpio* cast(const iot_hwdev_details* data) {
		if(!data || !data->is_valid()) return NULL;
		return data->get_metaclass()==&iot_hwdevcontype_metaclass_gpio::object ? static_cast<const iot_hwdev_details_gpio*>(data) : NULL;
	}

public:
	virtual size_t get_size(void) const override {
		return sizeof(*this);
	}
	virtual bool copy_to(void* buffer, size_t buffer_size) const override {
		if(buffer_size<sizeof(*this)) return false;
		if((char*)this == buffer) {
			assert(false);
			return false;
		}
		new(buffer) iot_hwdev_details_gpio(*this);
		return true;
	}
	virtual bool fill_localident(void *buffer, size_t buffer_size, const iot_hwdev_localident** identptr) const override {
		//function must know how to calculate size of localident object. in THIS class this size is fixed
		if(buffer_size<sizeof(iot_hwdev_localident_gpio)) return false;
		uint8_t port_mask[IOT_HWDEV_IDENT_GPIO_PORTMASKLEN];
		memset(port_mask, 0, sizeof(port_mask));

		for(uint16_t pi=0;pi<num_ports;pi++) {
			if(ports[pi]>sizeof(port_mask)*8) return false;

			uint32_t byteidx=uint32_t(ports[pi]) >> 3u;
			uint8_t bitidx=ports[pi]  - (byteidx<<3); //0 - 7
			uint8_t bitmask=(1<<bitidx);
			if(port_mask[byteidx] & bitmask) return false; //duplicate pin
			port_mask[byteidx] |= bitmask;
		}

		iot_hwdev_localident_gpio *ident=new(buffer) iot_hwdev_localident_gpio();
		int err=ident->init_spec(IOT_HWDEV_IDENT_GPIO_PORTMASKLEN, port_mask,ctrl_id,  driver_module_id, vendor, product, version);
		if(err) return false;
		if(identptr) *identptr=ident;
		return true;
	}
	virtual bool operator==(const iot_hwdev_details& _op) const override {
		if(&_op==this) return true;
		const iot_hwdev_details_gpio* op=cast(&_op); //check type
		if(!op) return false;

		if(params!=op->params) return false; //can only compare JSON by address
		if(strcmp(name, op->name)!=0) return false;
		if(driver_module_id!=op->driver_module_id || vendor!=op->vendor || product!=op->product || version!=op->version) return false;
		if(num_ports>0 && memcmp(ports, op->ports, sizeof(ports[0])*num_ports)!=0) return false;
		return true;
	}
	void set(const char* name_, json_object* params_, uint8_t num_ports_, const uint8_t* ports_, uint8_t ctrl_id_, uint32_t driver_module_id_,int16_t vendor_,uint16_t product_, uint16_t version_) {
		if(name_) snprintf(name, sizeof(name), "%s", name_);
			else name[0]='\0';
		if(params!=params_) {
			if(params) json_object_put(params);
			params=params_;
			if(params) json_object_get(params);
		}
		if(num_ports_>0) {
			if(num_ports_>32) num_ports_=32;
			memcpy(ports, ports_, sizeof(ports[0])*num_ports_);
		}
		num_ports=num_ports_;
		
		ctrl_id=ctrl_id_;
		driver_module_id=driver_module_id_;
		vendor=vendor_;
		product=product_;
		version=version_;
	}
};



#endif //IOT_HWDEVCONTYPE_GPIO_H
