#ifndef IOT_HWDEVCONTYPE_SERIAL_H
#define IOT_HWDEVCONTYPE_SERIAL_H
//Representation for hardware connection over any primitive line which gives no systematic ways to identify devices.  in particular serial (COM-port, RS-232, UART)


//maximum length of serial port name
#define IOT_HWDEV_IDENT_SERIAL_PORTLEN 10
class iot_hwdev_localident_serial : public iot_hwdev_localident {
	friend class iot_hwdevcontype_metaclass_serial;
public:
	enum idspace_t : uint8_t { //identification of space for vendor and product ids
		IDSPACE_UNSET=0,
		IDSPACE_ZWAVE,   //numeration from Z-Wave alliance, 'product' matches "Product Id", version matches "Product Type"

		IDSPACE_MAX=IDSPACE_ZWAVE
	};
private:
	struct spec_t {
		uint8_t mask; //bitmask showing which fields are exact (bit 0 - vendor, bit 1 - product, bit 2 - version)
		idspace_t idspace;//always exact
		uint16_t vendor,
			product,	//unchecked if vendor not exact
			version;	//unchecked if vendor not exact
	};
	union {
		struct {
//hwid:
			uint16_t vendor;
			uint16_t product; //assumed to be non-0 for exact spec!
			uint16_t version; //assumed to be != 0xffff for exact spec!
			idspace_t idspace;
//address:
			char portname[IOT_HWDEV_IDENT_SERIAL_PORTLEN+1]; //NUL terminated name of serial port without /dev/ suffics (on windows it is like COM1)
		} spec;
		struct {
			spec_t spec[8]; //array with specific product requirements
			uint8_t num_specs; //number of valid items in spec[]. zero means that no specific product requirements
			char portname[IOT_HWDEV_IDENT_SERIAL_PORTLEN+1]; //Empty for any port OR NUL terminated name of serial port without /dev/ suffics (on windows it is like COM1)
		} tmpl;
	};
	bool istmpl;
//
public:
	
	iot_hwdev_localident_serial(void);
	iot_hwdev_localident_serial(const char* portname, idspace_t idspace, uint16_t vendor, uint16_t product, uint16_t version);
	iot_hwdev_localident_serial(const char* portname, uint8_t num_specs, const spec_t (&spec)[8]);

	static const iot_hwdev_localident_serial* cast(const iot_hwdev_localident* ident);

	int init_spec(const char* portname, idspace_t idspace, uint16_t vendor, uint16_t product, uint16_t version) {
		if(!portname || idspace>IDSPACE_MAX) return IOT_ERROR_INVALID_ARGS;
		size_t len=strlen(portname);
		if(!len || len>IOT_HWDEV_IDENT_SERIAL_PORTLEN) return IOT_ERROR_INVALID_ARGS;

		istmpl=false;
		memcpy(spec.portname, portname, len+1);
		spec.idspace=idspace;
		spec.vendor=vendor;
		spec.product=product;
		spec.version=version;
		return 0;
	}
	int init_tmpl(const char* portname, uint8_t num_specs, const spec_t (&spec)[8]) { //portname can be NULL or empty string for "any" port
		if(num_specs>8) return IOT_ERROR_INVALID_ARGS;
		if(portname) {
			size_t len=strlen(portname);
			if(len>IOT_HWDEV_IDENT_SERIAL_PORTLEN) return IOT_ERROR_INVALID_ARGS;
			memcpy(tmpl.portname, portname, len+1);
		} else tmpl.portname[0]='\0';
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
			if(!tmpl.portname[0])  len=snprintf(buf, bufsize, "any port");
			else len=snprintf(buf, bufsize, "port=%s",tmpl.portname);
		} else {
			len=snprintf(buf, bufsize, "port=%s",spec.portname);
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
			switch(spec.idspace) {
				case IDSPACE_ZWAVE:
					len=snprintf(buf, bufsize, "Z-Wave vendor=%04x,productType=%04x,productId=%04x",unsigned(spec.vendor),unsigned(spec.version),unsigned(spec.product));
					break;
				default:
					len=snprintf(buf, bufsize, "id space=%u,vendor=%04x,product=%04x,ver=%04x",unsigned(spec.idspace),unsigned(spec.vendor),unsigned(spec.product),unsigned(spec.version));
					break;
			}
		}
		if(doff) *doff += len>=int(bufsize) ? int(bufsize-1) : len;
		return buf;
	}
private:
	virtual bool p_matches(const iot_hwdev_localident* opspec) const override {
		return iot_hwdev_localident_serial::p_matches_hwid(opspec) && iot_hwdev_localident_serial::p_matches_addr(opspec);
	}
	virtual bool p_matches_hwid(const iot_hwdev_localident* opspec0) const override {
		const iot_hwdev_localident_serial* opspec=cast(opspec0);
		if(!opspec) return false;
		if(istmpl) {
			if(!tmpl.num_specs) return true;
			for(unsigned i=0;i<tmpl.num_specs;i++) { //check if any exact spec matches
				if(tmpl.spec[i].idspace!=opspec->spec.idspace) continue;
				if(tmpl.spec[i].mask & 1) { //vendor exact
					if(tmpl.spec[i].vendor!=opspec->spec.vendor) continue;
					if((tmpl.spec[i].mask & 2) && tmpl.spec[i].product!=opspec->spec.product) continue;
					if((tmpl.spec[i].mask & 4) && tmpl.spec[i].version!=opspec->spec.version) continue;
				}
				return true;
			}
			return false;
		}
		return spec.idspace==opspec->spec.idspace && 
			spec.vendor==opspec->spec.vendor && 
			spec.product==opspec->spec.product && 
			spec.version==opspec->spec.version;
	}
	virtual bool p_matches_addr(const iot_hwdev_localident* opspec0) const override {
		const iot_hwdev_localident_serial* opspec=cast(opspec0);
		if(!opspec) return false;
		if(istmpl) {
			return !tmpl.portname[0] || strcmp(tmpl.portname,opspec->spec.portname)==0;
		}
		return strcmp(spec.portname,opspec->spec.portname)==0;
	}
};

class iot_hwdevcontype_metaclass_serial : public iot_hwdevcontype_metaclass {
	iot_hwdevcontype_metaclass_serial(void) : iot_hwdevcontype_metaclass(0, "serial", IOT_VERSION_COMPOSE(0,0,1)) {}

	PACKED(
		struct serialize_header_t {
			uint32_t format; //format/version of pack
			uint8_t istmpl;
		}
	);
	PACKED(
		struct serialize_spec_t {
			uint16_t vendor;
			uint16_t product;
			uint16_t version;
			uint8_t idspace;
			uint8_t portname_len; //number of chars in portname
			char portname[];
		}
	);
	PACKED(
		struct serialize_tmpl_t {
			uint8_t num_specs; //number of valid items in spec[]. zero means that no specific product requirements
			char portname[IOT_HWDEV_IDENT_SERIAL_PORTLEN+1]; //NUL terminated name of serial port without /dev/ suffics (on windows it is like COM1)
			struct {
				uint16_t vendor;
				uint16_t product;
				uint16_t version;
				uint8_t idspace;
				uint8_t mask; //bitmask showing which fields are exact (bit 0 - vendor, bit 1 - product, bit 2 - version)
			} spec[]; //array with specific product requirements
		}
	);

public:
	static iot_hwdevcontype_metaclass_serial object; //the only instance of this class

private:
	virtual int p_serialized_size(const iot_hwdev_localident* obj0) const override {
		const iot_hwdev_localident_serial* obj=iot_hwdev_localident_serial::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		if(obj->istmpl) return sizeof(serialize_header_t)+sizeof(serialize_tmpl_t)+obj->tmpl.num_specs*sizeof(serialize_tmpl_t::spec[0]);
		return sizeof(serialize_header_t)+sizeof(serialize_spec_t)+strlen(obj->spec.portname);
	}
	virtual int p_serialize(const iot_hwdev_localident* obj0, char* buf, size_t bufsize) const override {
		const iot_hwdev_localident_serial* obj=iot_hwdev_localident_serial::cast(obj0);
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
		return 0;
	}
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const override {
		assert(false); //TODO
		return 0;
	}
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const override {
		assert(false); //TODO
		return 0;
	}
	virtual int p_to_json(const iot_hwdev_localident* obj0, json_object* &dst) const override {
		const iot_hwdev_localident_serial* obj=iot_hwdev_localident_serial::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		json_object* ob=json_object_new_object();
		if(!ob) return IOT_ERROR_NO_MEMORY;

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
		return IOT_ERROR_NO_MEMORY;
	}
};


inline iot_hwdev_localident_serial::iot_hwdev_localident_serial(void)
: iot_hwdev_localident(&iot_hwdevcontype_metaclass_serial::object)
{
}

inline iot_hwdev_localident_serial::iot_hwdev_localident_serial(const char* portname, idspace_t idspace, uint16_t vendor, uint16_t product, uint16_t version)
: iot_hwdev_localident(&iot_hwdevcontype_metaclass_serial::object)
{
	if(init_spec(portname, idspace, vendor, product, version)) {
		assert(false);
	}
}
inline iot_hwdev_localident_serial::iot_hwdev_localident_serial(const char* portname, uint8_t num_specs, const spec_t (&spec)[8])
: iot_hwdev_localident(&iot_hwdevcontype_metaclass_serial::object)
{
	if(init_tmpl(portname, num_specs, spec)) {
		assert(false);
	}
}

inline const iot_hwdev_localident_serial* iot_hwdev_localident_serial::cast(const iot_hwdev_localident* ident) {
	if(!ident) return NULL;
	return ident->get_metaclass()==&iot_hwdevcontype_metaclass_serial::object ? static_cast<const iot_hwdev_localident_serial*>(ident) : NULL;
}

class iot_hwdev_details_serial_subclass {
};


//should be subclassed because serial connected device have no common ways of obtaining detailed data, here are most common props
class iot_hwdev_details_serial : public iot_hwdev_details {
public:
	const iot_hwdev_details_serial_subclass* subclass; //can be NULL
	char name[256];
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
	iot_hwdev_localident_serial::idspace_t idspace;
	char portname[IOT_HWDEV_IDENT_SERIAL_PORTLEN+1];

	iot_hwdev_details_serial(const iot_hwdev_details_serial_subclass* subclass) : iot_hwdev_details(&iot_hwdevcontype_metaclass_serial::object), subclass(subclass) {
		vendor=product=version=0;
		idspace=iot_hwdev_localident_serial::IDSPACE_UNSET;
		name[0]=portname[0]='\0';
	}
	iot_hwdev_details_serial(const iot_hwdev_details_serial& ob, const iot_hwdev_details_serial_subclass* subclass) : iot_hwdev_details(ob), subclass(subclass) {
		memcpy(name, ob.name, sizeof(name));
		vendor=ob.vendor;
		product=ob.product;
		version=ob.version;
		idspace=ob.idspace;
		memcpy(portname, ob.portname, sizeof(portname));
	}
	iot_hwdev_details_serial(const iot_hwdev_details_serial& ob) : iot_hwdev_details_serial(ob, ob.subclass) {} //delegating constructor

	static const iot_hwdev_details_serial* cast(const iot_hwdev_details* data) {
		if(!data || !data->is_valid()) return NULL;
		return data->get_metaclass()==&iot_hwdevcontype_metaclass_serial::object ? static_cast<const iot_hwdev_details_serial*>(data) : NULL;
	}

protected:
	bool operator==(const iot_hwdev_details_serial& op) const { //for use inside subclasses. subclass assumed to be equal, so not checked here
		if(strcmp(name, op.name)!=0) return false;
		if(vendor!=op.vendor || product!=op.product || version!=op.version || idspace!=op.idspace) return false;
		if(strcmp(portname, op.portname)!=0) return false;
		return true;
	}
public:
	virtual size_t get_size(void) const override { //can be called with NULL subclass only
		assert(!subclass);
		return sizeof(*this);
	}
	virtual bool copy_to(void* buffer, size_t buffer_size) const override { //can be called with NULL subclass only
		assert(!subclass);
		if(buffer_size<sizeof(*this)) return false;
		if((char*)this == buffer) {
			assert(false);
			return false;
		}
		new(buffer) iot_hwdev_details_serial(*this);
		return true;
	}
	virtual bool fill_localident(void *buffer, size_t buffer_size, const iot_hwdev_localident** identptr) const override {
//THIS FUNCTION CAN WORK FOR ALL SUBCLASSES TOO because serial ident contains only fields from this class 		assert(!subclass);
		//function must know how to calculate size of localident object. in THIS class this size is fixed
		if(buffer_size<sizeof(iot_hwdev_localident_serial)) return false;
		iot_hwdev_localident_serial *ident=new(buffer) iot_hwdev_localident_serial();
		int err=ident->init_spec(portname, idspace, vendor, product, version);
		if(err) return false;
		if(identptr) *identptr=ident;
		return true;
	}
	virtual bool operator==(const iot_hwdev_details& _op) const override { //can be called with NULL subclass only
		assert(!subclass);
		if(&_op==this) return true;
		const iot_hwdev_details_serial* op=cast(&_op); //check type
		if(!op) return false;

		return iot_hwdev_details_serial::operator==(*op);
	}
	void set_serial_port(const char* portname_) {
		if(!portname_) {
			portname[0]='\0';
			return;
		}
		snprintf(portname, sizeof(portname), "%s", portname_);
	}
	void set_serial_data(iot_hwdev_localident_serial::idspace_t idspace_, uint16_t vendor_, uint16_t product_, uint16_t version_, const char* name_=NULL) {
		idspace=idspace_;
		vendor=vendor_;
		product=product_;
		version=version_;
		if(name_) snprintf(name, sizeof(name), "%s", name_);
	}
};



#endif //IOT_HWDEVCONTYPE_SERIAL_H
