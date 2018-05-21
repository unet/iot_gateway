#ifndef IOT_HWDEVCONTYPE_ZWAVE_H
#define IOT_HWDEVCONTYPE_ZWAVE_H
//Representation for hardware connection over Z-Wave

class iot_hwdev_localident_zwave : public iot_hwdev_localident {
	friend class iot_hwdevcontype_metaclass_zwave;
private:
	struct spec_t {
		uint32_t home_id;
		uint16_t manufacturer_id,
			product_id,   //unchecked if manufacturer_id not exact
			product_type; //unchecked if manufacturer_id not exact
		uint8_t basic_type,
			generic_type,
			specific_type;//unchecked if generic_type not exact
		uint8_t mask; //bitmask showing which fields are exact (bit 0 - home id, bit 1 - manufacturer_id, bit 2 - prod_id, bit 3 - prod_type, bit 4 - basic_type, bit 5 - generic_type, bit 6 - specific_type)
	};
	union {
		struct {
//hwid:
			uint16_t manufacturer_id;
			uint16_t product_id;
			uint16_t product_type;
			uint8_t basic_type;
			uint8_t generic_type; //of instance 0
			uint8_t specific_type;//of instance 0
//address:
			uint8_t node_id;
			uint32_t home_id;
		} spec;
		struct {
			spec_t spec[4]; //array with specific product requirements
			uint8_t num_specs; //number of valid items in spec[]. zero means that no specific product requirements
		} tmpl; //port cannot be templated here!
	};
	bool istmpl;
//
public:
	
	iot_hwdev_localident_zwave(void);
	iot_hwdev_localident_zwave(uint32_t home_id, uint8_t node_id, uint8_t basic_type, uint8_t generic_type, uint8_t specific_type, uint16_t manufacturer_id, uint16_t product_id, uint16_t product_type);
	iot_hwdev_localident_zwave(uint8_t num_specs, const spec_t (&spec)[4]);

	static const iot_hwdev_localident_zwave* cast(const iot_hwdev_localident* ident);

	int init_spec(uint32_t home_id, uint8_t node_id, uint8_t basic_type, uint8_t generic_type, uint8_t specific_type, uint16_t manufacturer_id, uint16_t product_id, uint16_t product_type) {
		if(!basic_type || (!manufacturer_id && !product_id && !product_type)) return IOT_ERROR_INVALID_ARGS;
		istmpl=false;
		spec.manufacturer_id=manufacturer_id;
		spec.product_id=product_id;
		spec.product_type=product_type;
		spec.basic_type=basic_type;
		spec.generic_type=generic_type;
		spec.specific_type=specific_type;
		spec.node_id=node_id;
		spec.home_id=home_id;
		return 0;
	}
	int init_tmpl(uint8_t num_specs, const spec_t (&spec)[4]) {
		if(num_specs>4) return IOT_ERROR_INVALID_ARGS;
		istmpl=true;
		tmpl.num_specs=num_specs;
		uint8_t i;
		for(i=0;i<num_specs;i++) tmpl.spec[i]=spec[i];
		for(;i<4;i++) tmpl.spec[i]={};
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
			if(!tmpl.num_specs) len=snprintf(buf, bufsize, "any home");
				else len=snprintf(buf, bufsize, "template");
		} else {
			len=snprintf(buf, bufsize, "homeId=%u,nodeId=%d", unsigned(spec.home_id), int(spec.node_id));
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
			len=snprintf(buf, bufsize, "basicType=%d,genericType=%d,specificType=%d,manufacturerId=%04x,productType=%04x,productId=%04x",int(spec.basic_type),int(spec.generic_type),int(spec.specific_type),unsigned(spec.manufacturer_id),unsigned(spec.product_type),unsigned(spec.product_id));
		}
		if(doff) *doff += len>=int(bufsize) ? int(bufsize-1) : len;
		return buf;
	}
private:
	virtual bool p_matches(const iot_hwdev_localident* opspec0) const override {
		if(!istmpl) return iot_hwdev_localident_zwave::p_matches_hwid(opspec0) && iot_hwdev_localident_zwave::p_matches_addr(opspec0);
		//is tmpl
		const iot_hwdev_localident_zwave* opspec=cast(opspec0);
		if(!opspec) return false;

		if(!tmpl.num_specs) return true;
		for(unsigned i=0;i<tmpl.num_specs;i++) { //check if any exact spec matches
			//chec hwid
			if((tmpl.spec[i].mask & (1<<4)) && tmpl.spec[i].basic_type!=opspec->spec.basic_type) continue;
			if(tmpl.spec[i].mask & (1<<5)) { //generic type exact
				if(tmpl.spec[i].generic_type!=opspec->spec.generic_type) continue;
				if((tmpl.spec[i].mask & (1<<6)) && tmpl.spec[i].specific_type!=opspec->spec.specific_type) continue;
			}

			if(!(tmpl.spec[i].mask & (1<<1))) return true; //any vendor
			//vendor exact
			if(tmpl.spec[i].manufacturer_id!=opspec->spec.manufacturer_id) continue;
			if((tmpl.spec[i].mask & (1<<2)) && tmpl.spec[i].product_id!=opspec->spec.product_id) continue;
			if((tmpl.spec[i].mask & (1<<3)) && tmpl.spec[i].product_type!=opspec->spec.product_type) continue;

			//hwid matches, check addr
			if(!(tmpl.spec[i].mask & 1)) return true; //any home
			//home exact
			if(tmpl.spec[i].home_id==opspec->spec.home_id) return true;
		}
		return false;
	}
	virtual bool p_matches_hwid(const iot_hwdev_localident* opspec0) const override {
		const iot_hwdev_localident_zwave* opspec=cast(opspec0);
		if(!opspec) return false;
		if(istmpl) {
			if(!tmpl.num_specs) return true;
			for(unsigned i=0;i<tmpl.num_specs;i++) { //check if any exact spec matches
				if((tmpl.spec[i].mask & (1<<4)) && tmpl.spec[i].basic_type!=opspec->spec.basic_type) continue;
				if(tmpl.spec[i].mask & (1<<5)) { //generic type exact
					if(tmpl.spec[i].generic_type!=opspec->spec.generic_type) continue;
					if((tmpl.spec[i].mask & (1<<6)) && tmpl.spec[i].specific_type!=opspec->spec.specific_type) continue;
				}

				if(!(tmpl.spec[i].mask & (1<<1))) return true; //any vendor
				//vendor exact
				if(tmpl.spec[i].manufacturer_id!=opspec->spec.manufacturer_id) continue;
				if((tmpl.spec[i].mask & (1<<2)) && tmpl.spec[i].product_id!=opspec->spec.product_id) continue;
				if((tmpl.spec[i].mask & (1<<3)) && tmpl.spec[i].product_type!=opspec->spec.product_type) continue;
				return true;
			}
			return false;
		}
		return spec.basic_type==opspec->spec.basic_type &&
			spec.generic_type==opspec->spec.generic_type &&
			spec.specific_type==opspec->spec.specific_type &&
			spec.manufacturer_id==opspec->spec.manufacturer_id &&
			spec.product_id==opspec->spec.product_id && 
			spec.product_type==opspec->spec.product_type;
	}
	virtual bool p_matches_addr(const iot_hwdev_localident* opspec0) const override {
		const iot_hwdev_localident_zwave* opspec=cast(opspec0);
		if(!opspec) return false;
		if(istmpl) {
			if(!tmpl.num_specs) return true;
			for(unsigned i=0;i<tmpl.num_specs;i++) { //check if any exact spec matches
				if(!(tmpl.spec[i].mask & 1)) return true; //any home
				//home exact
				if(tmpl.spec[i].home_id==opspec->spec.home_id) return true;
			}
			return false;
		}
		return spec.home_id==opspec->spec.home_id && 
			spec.node_id==opspec->spec.node_id;
	}
};

class iot_hwdevcontype_metaclass_zwave : public iot_hwdevcontype_metaclass {
	iot_hwdevcontype_metaclass_zwave(void) : iot_hwdevcontype_metaclass(0, "zwave", IOT_VERSION_COMPOSE(0,0,1)) {}

	PACKED(
		struct serialize_header_t {
			uint32_t format; //format/version of pack
			uint8_t istmpl;
		}
	);
	PACKED(
		struct serialize_spec_t {
			uint32_t home_id;
			uint16_t manufacturer_id;
			uint16_t product_id;
			uint16_t product_type;
			uint8_t basic_type;
			uint8_t generic_type;
			uint8_t specific_type;
			uint8_t node_id;
		}
	);
	PACKED(
		struct serialize_tmpl_t {
			uint8_t num_specs; //number of valid items in spec[]. zero means that no specific product requirements
			struct {
				uint32_t home_id;
				uint16_t manufacturer_id;
				uint16_t product_id;
				uint16_t product_type;
				uint8_t basic_type;
				uint8_t generic_type;
				uint8_t specific_type;
				uint8_t mask; //bitmask showing which fields are exact (bit 0 - home id, bit 1 - manufacturer_id, bit 2 - prod_id, bit 3 - prod_type, bit 4 - basic_type, bit 5 - generic_type, bit 6 - specific_type)
			} spec[]; //array with specific product requirements
		}
	);

public:
	static iot_hwdevcontype_metaclass_zwave object; //the only instance of this class

private:
	virtual int p_serialized_size(const iot_hwdev_localident* obj0) const override {
		const iot_hwdev_localident_zwave* obj=iot_hwdev_localident_zwave::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		if(obj->istmpl) return sizeof(serialize_header_t)+sizeof(serialize_tmpl_t)+obj->tmpl.num_specs*sizeof(serialize_tmpl_t::spec[0]);
		return sizeof(serialize_header_t)+sizeof(serialize_spec_t);
	}
	virtual int p_serialize(const iot_hwdev_localident* obj0, char* buf, size_t bufsize) const override {
		const iot_hwdev_localident_zwave* obj=iot_hwdev_localident_zwave::cast(obj0);
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
			for(uint8_t i=0;i<obj->tmpl.num_specs;i++) {
				t->spec[i].home_id=repack_uint32(obj->tmpl.spec[i].home_id);
				t->spec[i].manufacturer_id=repack_uint16(obj->tmpl.spec[i].manufacturer_id);
				t->spec[i].product_id=repack_uint16(obj->tmpl.spec[i].product_id);
				t->spec[i].product_type=repack_uint16(obj->tmpl.spec[i].product_type);
				t->spec[i].basic_type=obj->tmpl.spec[i].basic_type;
				t->spec[i].generic_type=obj->tmpl.spec[i].generic_type;
				t->spec[i].specific_type=obj->tmpl.spec[i].specific_type;
				t->spec[i].mask=obj->tmpl.spec[i].mask;
			}
		} else {
			if(bufsize < sizeof(serialize_spec_t)) return IOT_ERROR_NO_BUFSPACE;
			h->format=repack_uint32(uint32_t(1));
			h->istmpl=0;
			serialize_spec_t *s=(serialize_spec_t*)(h+1);
			s->home_id=repack_uint32(obj->spec.home_id);
			s->manufacturer_id=repack_uint16(obj->spec.manufacturer_id);
			s->product_id=repack_uint16(obj->spec.product_id);
			s->product_type=repack_uint16(obj->spec.product_type);
			s->basic_type=obj->spec.basic_type;
			s->generic_type=obj->spec.generic_type;
			s->specific_type=obj->spec.specific_type;
			s->node_id=obj->spec.node_id;
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
		const iot_hwdev_localident_zwave* obj=iot_hwdev_localident_zwave::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		json_object* ob=json_object_new_object();
		if(!ob) return IOT_ERROR_NO_MEMORY;

		json_object* val;

		if(obj->istmpl) { //{is_tmpl: true, product_specs: [[idspace(integer), vendor(integer), product(integer/undefined), version(integer/undefined)]]/undefined}
			val=json_object_new_boolean(1);
			if(!val) goto nomem;
			json_object_object_add(ob, "is_tmpl", val);
			if(obj->tmpl.num_specs>0) {
				val=json_object_new_array();
				if(!val) goto nomem;
				json_object_object_add(ob, "specs", val);

				json_object* subob, *subval;
				for(uint8_t i=0;i<obj->tmpl.num_specs; i++) {
					subob=json_object_new_object();
					if(!subob) goto nomem;
					json_object_array_add(val, subob);

					if((obj->tmpl.spec[i].mask & (1<<0))) { //homeId exact
						subval=json_object_new_int64(obj->tmpl.spec[i].home_id);
						if(!subval) goto nomem;
						json_object_object_add(subob, "home_id", subval);
					}

					if(obj->tmpl.spec[i].mask & (1<<1)) { //vendor exact
						//add vendor
						subval=json_object_new_int(obj->tmpl.spec[i].manufacturer_id);
						if(!subval) goto nomem;
						json_object_object_add(subob, "manufacturer_id", subval);

						if(obj->tmpl.spec[i].mask & (1<<2)) { //product id specifed
							subval=json_object_new_int(obj->tmpl.spec[i].product_id);
							if(!subval) goto nomem;
							json_object_object_add(subob, "product_id", subval);
						}
						if(obj->tmpl.spec[i].mask & (1<<3)) { //product type specifed
							subval=json_object_new_int(obj->tmpl.spec[i].product_type);
							if(!subval) goto nomem;
							json_object_object_add(subob, "product_type", subval);
						}
					}

					if((obj->tmpl.spec[i].mask & (1<<4))) { //basic type exact
						subval=json_object_new_int(obj->tmpl.spec[i].basic_type);
						if(!subval) goto nomem;
						json_object_object_add(subob, "basic_type", subval);
					}
					if((obj->tmpl.spec[i].mask & (1<<5))) { //generic type exact
						subval=json_object_new_int(obj->tmpl.spec[i].generic_type);
						if(!subval) goto nomem;
						json_object_object_add(subob, "generic_type", subval);

						if((obj->tmpl.spec[i].mask & (1<<6))) { //specific type exact
							subval=json_object_new_int(obj->tmpl.spec[i].specific_type);
							if(!subval) goto nomem;
							json_object_object_add(subob, "specific_type", subval);
						}
					}
				}
			}
		} else { //{portname: string, idspace: integer, vendor: integer, product: integer, version: integer}
			val=json_object_new_int64(obj->spec.home_id);
			if(!val) goto nomem;
			json_object_object_add(ob, "home_id", val);

			val=json_object_new_int64(obj->spec.node_id);
			if(!val) goto nomem;
			json_object_object_add(ob, "node_id", val);

			val=json_object_new_int64(obj->spec.basic_type);
			if(!val) goto nomem;
			json_object_object_add(ob, "basic_type", val);

			val=json_object_new_int64(obj->spec.generic_type);
			if(!val) goto nomem;
			json_object_object_add(ob, "generic_type", val);

			val=json_object_new_int64(obj->spec.specific_type);
			if(!val) goto nomem;
			json_object_object_add(ob, "specific_type", val);

			val=json_object_new_int(obj->spec.manufacturer_id);
			if(!val) goto nomem;
			json_object_object_add(ob, "manufacturer_id", val);

			val=json_object_new_int(obj->spec.product_id);
			if(!val) goto nomem;
			json_object_object_add(ob, "product_id", val);

			val=json_object_new_int(obj->spec.product_type);
			if(!val) goto nomem;
			json_object_object_add(ob, "product_type", val);
		}
		dst=ob;
		return 0;
nomem:
		json_object_put(ob);
		return IOT_ERROR_NO_MEMORY;
	}
};


inline iot_hwdev_localident_zwave::iot_hwdev_localident_zwave(void)
: iot_hwdev_localident(&iot_hwdevcontype_metaclass_zwave::object)
{
}

inline iot_hwdev_localident_zwave::iot_hwdev_localident_zwave(uint32_t home_id, uint8_t node_id, uint8_t basic_type, uint8_t generic_type, uint8_t specific_type, uint16_t manufacturer_id, uint16_t product_id, uint16_t product_type)
: iot_hwdev_localident(&iot_hwdevcontype_metaclass_zwave::object)
{
	if(init_spec(home_id, node_id, basic_type, generic_type, specific_type, manufacturer_id, product_id, product_type)) {
		assert(false);
	}
}
inline iot_hwdev_localident_zwave::iot_hwdev_localident_zwave(uint8_t num_specs, const spec_t (&spec)[4])
: iot_hwdev_localident(&iot_hwdevcontype_metaclass_zwave::object)
{
	if(init_tmpl(num_specs, spec)) {
		assert(false);
	}
}

inline const iot_hwdev_localident_zwave* iot_hwdev_localident_zwave::cast(const iot_hwdev_localident* ident) {
	if(!ident) return NULL;
	return ident->get_metaclass()==&iot_hwdevcontype_metaclass_zwave::object ? static_cast<const iot_hwdev_localident_zwave*>(ident) : NULL;
}

class iot_zwave_device_handle;
//base class for zwave CC data
class iot_zwave_cc_data_common {
	friend class iot_zwave_device_handle;
	iot_zwave_device_handle *parent_device;
protected:
	iot_zwave_cc_data_common* next=NULL;
	uint8_t instance; //instance index
	uint8_t cc; //Command Class index
//	uint8_t is_tracked; //flag showing if CC data updates should be tracked and driver's callback called
//	time_t last_update=0;
	void *custom_arg; //custom argument for callback
public:
	iot_zwave_cc_data_common(iot_zwave_device_handle *parent_device, uint8_t instance, uint8_t cc, void *custom_arg) : parent_device(parent_device),instance(instance), cc(cc), custom_arg(custom_arg) {
		assert(parent_device!=NULL);
	}
//	virtual size_t get_size(void) const = 0;
};

//Multilevel Sensor Command Class (49/0x31)
class iot_zwave_cc_multilevel_sensor_data_t { //: public iot_zwave_cc_data_common {
public:
	static const iot_valuenotion* const scale2notion[][4];
	static const size_t scale2notion_size;
	uint8_t num_types;
	struct type_data_t {
		double value;
		uint8_t is_empty; //true value shows that value is undefined
		uint8_t scale_id; //range is 0-3. interpretation is type-dependent
		uint8_t type_id;
	} type_data[];

//	iot_zwave_cc_multilevel_sensor_data_t(/*iot_zwave_device_handle *parent_device,uint8_t instance, */uint8_t num_types)
//	: /*iot_zwave_cc_data_common(parent_device, instance, 49), (*/num_types(num_types) {
//		if(num_types==0) {
//			assert(false);
//			return;
//		}
//		memset(type_data, 0, sizeof(type_data_t)*num_types);
//	}
	static size_t calc_size(uint8_t num_types) {
		return sizeof(iot_zwave_cc_multilevel_sensor_data_t)+sizeof(type_data_t)*num_types;
	}
//	static iot_zwave_cc_multilevel_sensor_data_t* cast(iot_zwave_cc_data_common *common) {
//		if(!common || common->cc!=49) return NULL;
//		return static_cast<iot_zwave_cc_multilevel_sensor_data_t*>(common);
//	}
//private:
//	virtual size_t get_size(void) const override {
//		return sizeof(iot_zwave_cc_multilevel_sensor_data_t)+sizeof(type_data_t)*num_types;
//	}
};

//interface class for zwave device to use for communication with zwave controller
class iot_zwave_device_handle : public iot_objectrefable {
	iot_zwave_cc_data_common *cc_data_head=NULL; //point to unilink list of CC data structures per Instance-Command Class. Each structure is separate memblock
protected:
	iot_zwave_cc_data_common* find_cc_data(uint8_t inst, uint8_t cc_) {
		for(iot_zwave_cc_data_common* p=cc_data_head; p; p=p->next) {
			if(p->instance==inst && p->cc==cc_) return p;
		}
		return NULL;
	}
	void remove_cc_data(iot_zwave_cc_data_common* p) {
		ULINKLIST_REMOVE_NOPREV(p, cc_data_head, next);
		p->parent_device=NULL;
	}
	void add_cc_data(iot_zwave_cc_data_common* p) {
		ULINKLIST_INSERTHEAD(p, cc_data_head, next);
	}
public:
	iot_zwave_device_handle(object_destroysub_t destroy_sub = object_destroysub_delete) : iot_objectrefable(destroy_sub, true) {}

	virtual int lock(void) const = 0;
	virtual void unlock(void) const = 0;

	bool is_ok(void) const {
		int err=lock();
		if(err) return false;
		unlock();
		return true;
	}

	virtual bool try_device_driver_attach(iot_device_driver_base *drvinst, void (*driverStateCallback_)(iot_device_driver_base*, uint8_t instance, uint8_t cc, void *custom_arg)) = 0;
	virtual void device_driver_detach(iot_device_driver_base *drvinst) = 0;

	//Multilevel Sensor Command Class
	//GET
	//Returns:
	//	0 - success, dataptr
	virtual int cc_multilevel_sensor_get(uint8_t instance, iot_zwave_cc_multilevel_sensor_data_t **dataptr, void* custom_arg=NULL) = 0;
};

class iot_hwdev_details_zwave : public iot_hwdev_details {
public:
	iot_objref_ptr<iot_zwave_device_handle> handle;
	struct inst_cc_pair_t {
		uint8_t inst; //instance index
		uint8_t cc; //command class index
	};
	struct inst_prop_t {
		uint8_t inst; //instance index
		uint8_t generic_type;
		uint8_t specific_type;
		uint16_t cc_pair_start; //start index in inst_cc_pairs with command classes for this instance
		uint8_t num_cc_pair;
	};
	char name[128];
	char imgurl[160];
	uint32_t home_id;
	uint16_t manufacturer_id;
	uint16_t product_id;
	uint16_t product_type;
	uint16_t num_inst_cc_pairs;
	
	uint8_t node_id;
	uint8_t basic_type;
	uint8_t proto_major; //zwave protocol version
	uint8_t proto_minor;
	uint8_t app_major; //application version
	uint8_t app_minor;
	uint8_t num_inst; //number of instances (at least 1 as 0 instance always present)
	inst_prop_t inst_prop[]; //num_inst items
//embedded  inst_cc_pair_t inst_cc_pairs[];

	iot_hwdev_details_zwave(uint32_t home_id=0, uint8_t node_id=0) : iot_hwdev_details(&iot_hwdevcontype_metaclass_zwave::object), home_id(home_id), node_id(node_id) {
		name[0]=imgurl[0]='\0';
		manufacturer_id=product_id=product_type=0;
		num_inst_cc_pairs=0;
		proto_major=proto_minor=app_major=app_minor=0;
		num_inst=0;
	}
	iot_hwdev_details_zwave(const iot_hwdev_details_zwave& ob) : iot_hwdev_details(ob) {
		handle=ob.handle;
		memcpy(name, ob.name, sizeof(name));
		memcpy(imgurl, ob.imgurl, sizeof(imgurl));
		home_id=ob.home_id;
		manufacturer_id=ob.manufacturer_id;
		product_id=ob.product_id;
		product_type=ob.product_type;
		num_inst_cc_pairs=ob.num_inst_cc_pairs;
		node_id=ob.node_id;
		proto_major=ob.proto_major;
		proto_minor=ob.proto_minor;
		app_major=ob.app_major;
		app_minor=ob.app_minor;
		num_inst=ob.num_inst;
		if(num_inst>0) memcpy(inst_prop, ob.inst_prop, sizeof(inst_prop_t)*num_inst);
		if(num_inst_cc_pairs>0) memcpy(get_inst_cc_pairs_address(), ob.get_inst_cc_pairs_address(), sizeof(inst_cc_pair_t)*num_inst_cc_pairs);
	}

/*	inst_cc_pair_t *get_inst_cc_pairs_address(void) {
		return (inst_cc_pair_t*)(((char*)(this+1))+sizeof(inst_prop_t)*num_inst);
	}*/
	inst_cc_pair_t *get_inst_cc_pairs_address(void) const {
		return (inst_cc_pair_t*)(((char*)(this+1))+sizeof(inst_prop_t)*num_inst);
	}

	static const iot_hwdev_details_zwave* cast(const iot_hwdev_details* data) {
		if(!data || !data->is_valid()) return NULL;
		return data->get_metaclass()==&iot_hwdevcontype_metaclass_zwave::object ? static_cast<const iot_hwdev_details_zwave*>(data) : NULL;
	}

public:
	static size_t calc_size(uint8_t num_inst, uint16_t num_inst_cc_pairs) { //calculates necessary memory
		return sizeof(iot_hwdev_details_zwave)+sizeof(inst_prop_t)*num_inst+sizeof(inst_cc_pair_t)*num_inst_cc_pairs;
	}
	virtual size_t get_size(void) const override {
		return sizeof(*this)+sizeof(inst_prop_t)*num_inst+sizeof(inst_cc_pair_t)*num_inst_cc_pairs;
	}
	virtual bool copy_to(void* buffer, size_t buffer_size) const override {
		if(buffer_size < iot_hwdev_details_zwave::get_size()) return false;
		if((char*)this == buffer) {
			assert(false);
			return false;
		}
		new(buffer) iot_hwdev_details_zwave(*this);
		return true;
	}
	virtual bool fill_localident(void *buffer, size_t buffer_size, const iot_hwdev_localident** identptr) const override {
		//function must know how to calculate size of localident object. in THIS class this size is fixed
		if(buffer_size<sizeof(iot_hwdev_localident_zwave)) return false;
		iot_hwdev_localident_zwave *ident=new(buffer) iot_hwdev_localident_zwave();
		int err;
		if(num_inst>0) { //get types from instance 0
			err=ident->init_spec(home_id, node_id, basic_type, inst_prop[0].generic_type, inst_prop[0].specific_type, manufacturer_id, product_id, product_type);
		} else {
			err=ident->init_spec(home_id, node_id, 0, 0, 0, manufacturer_id, product_id, product_type);
		}
		if(err) return false;
		if(identptr) *identptr=ident;
		return true;
	}
	virtual bool operator==(const iot_hwdev_details& _op) const override { //can be called with NULL subclass only
		if(&_op==this) return true;
		const iot_hwdev_details_zwave* op=cast(&_op); //check type
		if(!op) return false;

		if(home_id!=op->home_id || node_id!=op->node_id || manufacturer_id!=op->manufacturer_id || product_id!=op->product_id || product_type!=op->product_type) return false;
		if(num_inst!=op->num_inst || num_inst_cc_pairs!=op->num_inst_cc_pairs) return false;
		if(proto_major!=op->proto_major || proto_minor!=op->proto_minor || app_major!=op->app_major || app_minor!=op->app_minor) return false;
		if(strcmp(name, op->name)!=0) return false;
		if(strcmp(imgurl, op->imgurl)!=0) return false;

		if(num_inst>0) if(memcmp(inst_prop, op->inst_prop, sizeof(inst_prop_t)*num_inst)!=0) return false;
		if(num_inst_cc_pairs>0) if(memcmp(get_inst_cc_pairs_address(), op->get_inst_cc_pairs_address(), sizeof(inst_cc_pair_t)*num_inst_cc_pairs)!=0) return false;

		return true;
	}

	void set_instances_data(uint8_t _num_inst, inst_prop_t* _inst_prop, uint16_t _num_inst_cc_pairs, inst_cc_pair_t* inst_cc_pairs) {
		//ENSURE _num_inst and _num_inst_cc_pairs ARE THE SAME AS USED FOR calc_size()!!!! or less
		num_inst=_num_inst;
		num_inst_cc_pairs=_num_inst_cc_pairs;
		if(num_inst>0) memcpy(inst_prop, _inst_prop, sizeof(inst_prop_t)*num_inst);
		if(num_inst_cc_pairs>0) memcpy(get_inst_cc_pairs_address(), inst_cc_pairs, sizeof(inst_cc_pair_t)*num_inst_cc_pairs);
	}
};



#endif //IOT_HWDEVCONTYPE_ZWAVE_H
