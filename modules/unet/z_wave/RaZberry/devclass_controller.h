#ifndef DEVCLASS_CONTROLLER_H
#define DEVCLASS_CONTROLLER_H

class deviface_params_controller : public iot_deviface_params {
	friend class devifacetype_metaclass_controller;
	//no actual params for this iface

	deviface_params_controller(void);
public:

	static const deviface_params_controller object; //statically created object to return from deserialization
	
	static const deviface_params_controller* cast(const iot_deviface_params* params);

	virtual bool is_tmpl(void) const override { //check if current objects represents template. otherwise it must be exact connection specification
		return false;
	}
	virtual size_t get_size(void) const override { //must return 0 if object is statically precreated and thus must not be copied by value, only by reference
		if(this==&object) return 0;
		return sizeof(*this);
	}
	virtual uint32_t get_d2c_maxmsgsize(void) const override {
		assert(false); //TODO
		return 0;
	}
	virtual uint32_t get_c2d_maxmsgsize(void) const override {
		assert(false); //TODO
		return 0;
	}
	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const override {
		if(!bufsize) return buf;

		int len=0;
		get_fulltypename(buf, bufsize, &len);
		if(doff) *doff+=len;
		return buf;
	}
private:
	virtual bool p_matches(const iot_deviface_params* opspec0) const override {
		const deviface_params_controller* opspec=cast(opspec0);
		if(!opspec) return false;
		return true;
	}
};

class devifacetype_metaclass_controller : public iot_devifacetype_metaclass {
	devifacetype_metaclass_controller(void) : iot_devifacetype_metaclass(0, "controller", IOT_VERSION_COMPOSE(0,0,2)) {}

	PACKED(
		struct serialize_header_t {
			uint32_t format;
		}
	);

public:
	static devifacetype_metaclass_controller object; //the only instance of this class

private:
	virtual int p_serialized_size(const iot_deviface_params* obj0) const override {
		const deviface_params_controller* obj=deviface_params_controller::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		return sizeof(serialize_header_t);
	}
	virtual int p_serialize(const iot_deviface_params* obj0, char* buf, size_t bufsize) const override {
		const deviface_params_controller* obj=deviface_params_controller::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		if(bufsize<sizeof(serialize_header_t)) return IOT_ERROR_NO_BUFSPACE;

		serialize_header_t *h=(serialize_header_t*)buf;
		h->format=repack_uint32(uint32_t(1));
		return 0;
	}
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_deviface_params*& obj) const override {
		assert(false);
		return 0;
	}
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_deviface_params*& obj) const override {
		assert(false);
		return 0;
	}
	virtual int p_to_json(const iot_deviface_params* obj0, json_object* &dst) const override {
		const deviface_params_controller* obj=deviface_params_controller::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		dst=NULL; //no params
		return 0;
	}
};


inline deviface_params_controller::deviface_params_controller(void) : iot_deviface_params(&devifacetype_metaclass_controller::object) {
}
inline const deviface_params_controller* deviface_params_controller::cast(const iot_deviface_params* params) {
	if(!params) return NULL;
	return params->get_metaclass()==&devifacetype_metaclass_controller::object ? static_cast<const deviface_params_controller*>(params) : NULL;
}


//TODO classes for communication

#endif //DEVCLASS_CONTROLLER_H
