#ifndef IOT_DEVCLASS_ACTIVATABLE_H
#define IOT_DEVCLASS_ACTIVATABLE_H
//Contains interface to communicate using IOT_DEVIFACETYPEID_ACTIVATABLE class

#include<stdint.h>
//#include<time.h>
#include<assert.h>
#include "ecb.h"

class iot_deviface_params_activatable : public iot_deviface_params {
	friend class iot_devifacetype_metaclass_activatable;
	friend class iot_deviface__activatable_BASE;
	friend class iot_deviface__activatable_DRV;
	friend class iot_deviface__activatable_CL;
	union {
		struct {
			uint16_t num_sublines; //number of control sublines (or controled subdevices). value 0 is not allowed
		} spec;
		struct {
			uint16_t min_sublines;
			uint16_t max_sublines; //zero value means 'no upper limit'
		} tmpl;
	};
	bool istmpl;

public:
	iot_deviface_params_activatable(uint16_t num_sublines);
	iot_deviface_params_activatable(uint16_t min_sublines, uint16_t max_sublines);
	
	static const iot_deviface_params_activatable* cast(const iot_deviface_params* params);

	virtual bool is_tmpl(void) const override { //check if current objects represents template. otherwise it must be exact connection specification
		return istmpl;
	}
	virtual size_t get_size(void) const override { //must return 0 if object is statically precreated and thus must not be copied by value, only by reference
		return sizeof(*this);
	}
	virtual uint32_t get_d2c_maxmsgsize(void) const override;
	virtual uint32_t get_c2d_maxmsgsize(void) const override;
	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const override {
		if(!bufsize) return buf;

		int len=0;
		get_fulltypename(buf, bufsize, &len);
		if(len<int(bufsize)-2) {
			if(istmpl)
				len+=snprintf(buf+len, bufsize-len, "{TMPL: sublines from %u to %u}", unsigned(tmpl.min_sublines), unsigned(tmpl.max_sublines));
			else
				len+=snprintf(buf+len, bufsize-len, "{sublines=%u}",unsigned(spec.num_sublines));
			if(len>=int(bufsize)) len=int(bufsize)-1;
		}
		if(doff) *doff+=len;
		return buf;
	}
private:
	virtual bool p_matches(const iot_deviface_params* opspec0) const override {
		const iot_deviface_params_activatable* opspec=cast(opspec0);
		if(!opspec) return false;
		if(istmpl) return opspec->spec.num_sublines>=tmpl.min_sublines && (!tmpl.max_sublines || opspec->spec.num_sublines<=tmpl.max_sublines);
		return spec.num_sublines==opspec->spec.num_sublines;
	}
};


class iot_devifacetype_metaclass_activatable : public iot_devifacetype_metaclass {
	iot_devifacetype_metaclass_activatable(void) : iot_devifacetype_metaclass(0, "activatable", IOT_VERSION_COMPOSE(0,0,1)) {}

	PACKED(
		struct serialize_header_t {
			uint32_t format;
			uint8_t istmpl;
		}
	);
	PACKED(
		struct serialize_spec_t {
			uint16_t num_sublines;
		}
	);
	PACKED(
		struct serialize_tmpl_t {
			uint16_t min_sublines;
			uint16_t max_sublines;
		}
	);


public:
	static iot_devifacetype_metaclass_activatable object; //the only instance of this class

private:
	virtual int p_serialized_size(const iot_deviface_params* obj0) const override {
		const iot_deviface_params_activatable* obj=iot_deviface_params_activatable::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		if(obj->istmpl) return sizeof(serialize_header_t)+sizeof(serialize_tmpl_t);
		return sizeof(serialize_header_t)+sizeof(serialize_spec_t);
	}
	virtual int p_serialize(const iot_deviface_params* obj0, char* buf, size_t bufsize) const override {
		const iot_deviface_params_activatable* obj=iot_deviface_params_activatable::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		if(bufsize<sizeof(serialize_header_t)) return IOT_ERROR_NO_BUFSPACE;
		bufsize-=sizeof(serialize_header_t);

		serialize_header_t *h=(serialize_header_t*)buf;
		if(obj->istmpl) {
			if(bufsize<sizeof(serialize_tmpl_t)) return IOT_ERROR_NO_BUFSPACE;
			h->format=repack_uint32(uint32_t(1));
			h->istmpl=1;
			serialize_tmpl_t *t=(serialize_tmpl_t*)(h+1);
			t->min_sublines=repack_uint16(obj->tmpl.min_sublines);
			t->max_sublines=repack_uint16(obj->tmpl.max_sublines);
		} else {
			if(bufsize<sizeof(serialize_spec_t)) return IOT_ERROR_NO_BUFSPACE;
			h->format=repack_uint32(uint32_t(1));
			h->istmpl=0;
			serialize_spec_t *s=(serialize_spec_t*)(h+1);
			s->num_sublines=repack_uint16(obj->spec.num_sublines);
		}
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
		const iot_deviface_params_activatable* obj=iot_deviface_params_activatable::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		json_object* ob=json_object_new_object();
		if(!ob) return IOT_ERROR_NO_MEMORY;

		json_object* val;

		if(obj->istmpl) { //{is_tmpl: true, min_lines: positive/non-existent, max_lines: positive/non-existent}
			val=json_object_new_boolean(1);
			if(!val) {
				json_object_put(ob);
				return IOT_ERROR_NO_MEMORY;
			}
			json_object_object_add(ob, "is_tmpl", val);
			if(obj->tmpl.min_sublines>0) {
				val=json_object_new_int(obj->tmpl.min_sublines);
				if(!val) {
					json_object_put(ob);
					return IOT_ERROR_NO_MEMORY;
				}
				json_object_object_add(ob, "min_lines", val);
			}
			if(obj->tmpl.max_sublines>0) {
				val=json_object_new_int(obj->tmpl.max_sublines);
				if(!val) {
					json_object_put(ob);
					return IOT_ERROR_NO_MEMORY;
				}
				json_object_object_add(ob, "max_lines", val);
			}
		} else { //{num_lines: integer}
			val=json_object_new_int(obj->spec.num_sublines);
			if(!val) {
				json_object_put(ob);
				return IOT_ERROR_NO_MEMORY;
			}
			json_object_object_add(ob, "num_lines", val);
		}
		dst=ob;
		return 0;
	}
};

inline iot_deviface_params_activatable::iot_deviface_params_activatable(uint16_t num_sublines) : iot_deviface_params(&iot_devifacetype_metaclass_activatable::object), istmpl(false) {
	if(num_sublines > 32) num_sublines=32;
	spec.num_sublines=num_sublines;
}
inline iot_deviface_params_activatable::iot_deviface_params_activatable(uint16_t min_sublines, uint16_t max_sublines) : iot_deviface_params(&iot_devifacetype_metaclass_activatable::object), istmpl(true) {
	tmpl.min_sublines=min_sublines;
	tmpl.max_sublines=max_sublines;
}
inline const iot_deviface_params_activatable* iot_deviface_params_activatable::cast(const iot_deviface_params* params) {
	if(!params) return NULL;
	return params->get_metaclass()==&iot_devifacetype_metaclass_activatable::object ? static_cast<const iot_deviface_params_activatable*>(params) : NULL;
}


class iot_deviface__activatable_BASE {
public:
	enum req_t : uint8_t { //commands (requests) which driver can execute
		REQ_GET_STATE, //request to post EVENT_CURRENT_STATE with status of all sublines. no 'data' is used
		REQ_SET_STATE //request to activate and/or deactivate specific sublines
	};

	enum event_t : uint8_t { //events (or replies) which driver can send to client
		EVENT_CURRENT_STATE //reply to REQ_GET_STATE request. provides current state of all sublines.
	};

	struct reqmsg { //use same message format for requests and events (but this is not mandatory. each event type can have own structure)
		req_t req_code;
		uint32_t activate_mask, deactivate_mask; //used for REQ_SET_STATE to indicate which subline must be activated and deactivated. 
												//if same bit is set in both masks, nothing is done to corresponding line
	};
	struct eventmsg { //use same message format for requests and events (but this is not mandatory. each event type can have own structure)
		event_t event_code;
		uint32_t state_mask, valid_mask;
	};
	constexpr static uint32_t get_maxmsgsize(void) {
		return sizeof(reqmsg) > sizeof(eventmsg) ? sizeof(reqmsg) : sizeof(eventmsg);
	}

protected:
	const iot_deviface_params_activatable *params;

	iot_deviface__activatable_BASE(void) : params(NULL) {
	}
	bool init(const iot_deviface_params *deviface) {
		params=iot_deviface_params_activatable::cast(deviface);
		if(!params || params->istmpl) return false; //illegal interface type or is template
		return true;
	}
};


class iot_deviface__activatable_DRV : public iot_deviface__DRVBASE, public iot_deviface__activatable_BASE {

public:
	iot_deviface__activatable_DRV(const iot_conn_drvview *conn=NULL) {
		init(conn);
	}
	bool init(const iot_conn_drvview *conn=NULL) {
		if(!conn) { //uninit request
			//do uninit
			iot_deviface__DRVBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		if(!iot_deviface__activatable_BASE::init(conn->deviface)) {
			iot_deviface__DRVBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		return iot_deviface__DRVBASE::init(conn);
	}
//	bool is_inited(void) inherited from iot_deviface__DRVBASE

	const reqmsg* parse_req(const void *data, uint32_t data_size) const {
		if(!is_inited()) return NULL;
		if(data_size != sizeof(reqmsg)) return NULL;
		return static_cast<const reqmsg*>(data);
	}

	//outgoing events (from driver to client)
	int send_current_state(uint32_t state_mask, uint32_t valid_mask) const {
		if(!is_inited()) return IOT_ERROR_NOT_INITED;
		uint32_t mask=(0xFFFFFFFFu >> (32 - params->spec.num_sublines)); //make 1 in lower params->spec.num_sublines bits
		state_mask&=mask; //reset excess higher bits
		valid_mask&=mask; //always mark as invalid excess higher bits

		eventmsg msg;
		memset(&msg, 0, sizeof(msg));
		msg.event_code = EVENT_CURRENT_STATE;
		msg.state_mask=state_mask;
		msg.valid_mask=valid_mask;

		return send_client_msg(&msg, sizeof(msg));
	}
};

class iot_deviface__activatable_CL : public iot_deviface__CLBASE, public iot_deviface__activatable_BASE {

public:
	iot_deviface__activatable_CL(const iot_conn_clientview *conn=NULL) {
		init(conn);
	}
	bool init(const iot_conn_clientview *conn=NULL) {
		if(!conn) { //uninit request
			//do uninit
			iot_deviface__CLBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		if(!iot_deviface__activatable_BASE::init(conn->deviface)) {
			iot_deviface__CLBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		return iot_deviface__CLBASE::init(conn);
	}
//	bool is_inited(void) inherited from iot_deviface__CLBASE

	const eventmsg* parse_event(const void *data, uint32_t data_size) const {
		if(!is_inited()) return NULL;
		if(data_size != sizeof(eventmsg)) return NULL;
		return static_cast<const eventmsg*>(data);
	}

	int get_state(void) const {
		return send_request(REQ_GET_STATE, 0, 0);
	}
	int set_state(uint32_t activate_mask, uint32_t deactivate_mask) const {
		return send_request(REQ_SET_STATE, activate_mask, deactivate_mask);
	}

private:
	int send_request(req_t req_code, uint32_t activate_mask, uint32_t deactivate_mask) const {
		if(!is_inited()) return IOT_ERROR_NOT_INITED;
		uint32_t mask=(0xFFFFFFFFu >> (32 - params->spec.num_sublines)); //make 1 in lower params->spec.num_sublines bits
		activate_mask&=mask;
		deactivate_mask&=mask;
		if(!activate_mask && !deactivate_mask) return 0; //no action

		reqmsg msg;
		memset(&msg, 0, sizeof(msg));
		msg.req_code = req_code;
		msg.activate_mask=activate_mask;
		msg.deactivate_mask=deactivate_mask;
		return send_driver_msg(&msg, sizeof(msg));
	}

};


#endif //IOT_DEVCLASS_ACTIVATABLE_H
