#ifndef IOT_DEVCLASS_METER_H
#define IOT_DEVCLASS_METER_H

//Interface for meters and counters, i.e. anything which gives fractional numeric value or specific physical value with specific unit 

#include<stdint.h>
//#include<time.h>
#include<assert.h>
#include "ecb.h"

class iot_deviface_params_meter : public iot_deviface_params {
	friend class iot_devifacetype_metaclass_meter;
	friend class iot_deviface__meter_BASE;
	friend class iot_deviface__meter_DRV;
	friend class iot_deviface__meter_CL;

	union {
		struct {
			iot_type_id_t notion_id; //value notion for all sublines. can be 0 for raw numeric values
			uint8_t num_sublines; //value 0 is not allowed
		} spec;
		struct {
			iot_type_id_t notion_id; //zero means 'any'
			uint8_t min_sublines;
			uint8_t max_sublines; //zero value means 'no upper limit'
		} tmpl;
	};
	bool istmpl;

public:
	iot_deviface_params_meter(iot_type_id_t notion_id, uint8_t num_sublines);
	iot_deviface_params_meter(iot_type_id_t notion_id, uint8_t min_sublines, uint8_t max_sublines);

	static const iot_deviface_params_meter* cast(const iot_deviface_params* params);

	bool inc_sublines(void) {
		if(istmpl || spec.num_sublines>=32) return false;
		spec.num_sublines++;
	}

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
				len+=snprintf(buf+len, bufsize-len, "{TMPL: notion_id=%" IOT_PRIiottypeid ", sublines from %u to %u}", tmpl.notion_id, unsigned(tmpl.min_sublines), unsigned(tmpl.max_sublines));
			else
				len+=snprintf(buf+len, bufsize-len, "{notion_id=%" IOT_PRIiottypeid ", sublines=%u}",spec.notion_id, unsigned(spec.num_sublines));
			if(len>=int(bufsize)) len=bufsize-1;
		}
		if(doff) *doff+=len;
		return buf;
	}
private:
	virtual bool p_matches(const iot_deviface_params* opspec0) const override {
		const iot_deviface_params_meter* opspec=cast(opspec0);
		if(!opspec) return false;
		if(istmpl) return (tmpl.notion_id==0 || tmpl.notion_id==opspec->spec.notion_id) && (opspec->spec.num_sublines>=tmpl.min_sublines && (!tmpl.max_sublines || opspec->spec.num_sublines<=tmpl.max_sublines));
		return spec.notion_id==opspec->spec.notion_id && spec.num_sublines==opspec->spec.num_sublines;
	}
};

class iot_devifacetype_metaclass_meter : public iot_devifacetype_metaclass {
	iot_devifacetype_metaclass_meter(void) : iot_devifacetype_metaclass(0, "meter", IOT_VERSION_COMPOSE(0,0,1)) {}

	PACKED(
		struct serialize_header_t {
			uint32_t format;
			uint8_t istmpl;
		}
	);
	PACKED(
		struct serialize_spec_t {
			iot_type_id_t notion_id; //value notion for all sublines. can be 0 for raw numeric values
			uint8_t num_sublines; //value 0 is not allowed
		}
	);
	PACKED(
		struct serialize_tmpl_t {
			iot_type_id_t notion_id; //zero means 'any'
			uint8_t min_sublines;
			uint8_t max_sublines; //zero value means 'no upper limit'
		}
	);


public:
	static iot_devifacetype_metaclass_meter object; //the only instance of this class

private:
	virtual int p_serialized_size(const iot_deviface_params* obj0) const override {
		const iot_deviface_params_meter* obj=iot_deviface_params_meter::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		if(obj->istmpl) return sizeof(serialize_header_t)+sizeof(serialize_tmpl_t);
		return sizeof(serialize_header_t)+sizeof(serialize_spec_t);
	}
	virtual int p_serialize(const iot_deviface_params* obj0, char* buf, size_t bufsize) const override {
		const iot_deviface_params_meter* obj=iot_deviface_params_meter::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		if(bufsize<sizeof(serialize_header_t)) return IOT_ERROR_NO_BUFSPACE;
		bufsize-=sizeof(serialize_header_t);

		serialize_header_t *h=(serialize_header_t*)buf;
		if(obj->istmpl) {
			if(bufsize<sizeof(serialize_tmpl_t)) return IOT_ERROR_NO_BUFSPACE;
			h->format=repack_uint32(uint32_t(1));
			h->istmpl=1;
			serialize_tmpl_t *t=(serialize_tmpl_t*)(h+1);
			t->notion_id=repack_type_id(obj->tmpl.notion_id);
			t->min_sublines=obj->tmpl.min_sublines;
			t->max_sublines=obj->tmpl.max_sublines;
		} else {
			if(bufsize<sizeof(serialize_spec_t)) return IOT_ERROR_NO_BUFSPACE;
			h->format=repack_uint32(uint32_t(1));
			h->istmpl=0;
			serialize_spec_t *s=(serialize_spec_t*)(h+1);
			s->notion_id=repack_type_id(obj->spec.notion_id);
			s->num_sublines=obj->spec.num_sublines;
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
		const iot_deviface_params_meter* obj=iot_deviface_params_meter::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		json_object* ob=json_object_new_object();
		if(!ob) return IOT_ERROR_NO_MEMORY;

		json_object* val;

		if(obj->istmpl) { //{is_tmpl: true, is_pckbd: true/false/non-existent}
			val=json_object_new_boolean(1);
			if(!val) {
				json_object_put(ob);
				return IOT_ERROR_NO_MEMORY;
			}
			json_object_object_add(ob, "is_tmpl", val);
			if(obj->tmpl.notion_id!=0) {
				val=json_object_new_int64(obj->tmpl.notion_id);
				if(!val) {
					json_object_put(ob);
					return IOT_ERROR_NO_MEMORY;
				}
				json_object_object_add(ob, "notion_id", val);
			} //alse "any" is_pckbd allowed. represent it as undefined
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
		} else { //{is_pckbd: true/false, max_keycode: integer}
			val=json_object_new_int64(obj->spec.notion_id);
			if(!val) {
				json_object_put(ob);
				return IOT_ERROR_NO_MEMORY;
			}
			json_object_object_add(ob, "notion_id", val);

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



inline iot_deviface_params_meter::iot_deviface_params_meter(iot_type_id_t notion_id, uint8_t num_sublines)
		: iot_deviface_params(&iot_devifacetype_metaclass_meter::object), istmpl(false) {
	spec.notion_id=notion_id;
	if(num_sublines > 32) num_sublines=32;
	spec.num_sublines=num_sublines;
}
inline iot_deviface_params_meter::iot_deviface_params_meter(iot_type_id_t notion_id, uint8_t min_sublines, uint8_t max_sublines)
		: iot_deviface_params(&iot_devifacetype_metaclass_meter::object), istmpl(true) {
	tmpl.notion_id=notion_id;
	tmpl.min_sublines=min_sublines;
	tmpl.max_sublines=max_sublines;
}

inline const iot_deviface_params_meter* iot_deviface_params_meter::cast(const iot_deviface_params* params) {
	if(!params) return NULL;
	return params->get_metaclass()==&iot_devifacetype_metaclass_meter::object ? static_cast<const iot_deviface_params_meter*>(params) : NULL;
}



class iot_deviface__meter_BASE {
public:
	enum req_t : uint8_t { //commands (requests) which driver can execute
		REQ_GET_STATE, //request to post EVENT_SET_STATE with all values. no 'data' is used
	};

	enum event_t : uint8_t { //events (or replies) which driver can send to client
		EVENT_SET_STATE, //reply to REQ_GET_STATE request. provides current values.
	};

	struct msg { //use same message format for requests and events (but this is not mandatory. each event type can have own structure)
		union { //field is determined by usage context
			req_t req_code;
			event_t event_code;
		};
		uint8_t statesize; //number of items in state
		double state[];
	};
	constexpr static uint32_t get_maxmsgsize(void) {
		return sizeof(msg)+32*sizeof(double);
	}
protected:
	const iot_deviface_params_meter* params;

	iot_deviface__meter_BASE(void) : params(NULL) {
	}
	bool init(const iot_deviface_params *deviface) {
		params=iot_deviface_params_meter::cast(deviface);
		if(!params || params->is_tmpl()) return false; //illegal interface type or is template
		return true;
	}
	const msg* parse_event(const void *data, uint32_t data_size) const {
		uint32_t statesize=params->spec.num_sublines;
		if(data_size != sizeof(msg)+statesize*sizeof(double)) return NULL;
		return static_cast<const msg*>(data);
	}
};

class iot_deviface__meter_DRV : public iot_deviface__DRVBASE, public iot_deviface__meter_BASE {

public:
	iot_deviface__meter_DRV(const iot_conn_drvview *conn=NULL) {
		init(conn);
	}
	bool init(const iot_conn_drvview *conn=NULL) {
		if(!conn) { //uninit request
			//do uninit
			iot_deviface__DRVBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		if(!iot_deviface__meter_BASE::init(conn->deviface)) {
			iot_deviface__DRVBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		return iot_deviface__DRVBASE::init(conn);
	}
//	bool is_inited(void) inherited from iot_deviface__DRVBASE

	const msg* parse_req(const void *data, uint32_t data_size) const {
		if(!is_inited()) return NULL;
		return iot_deviface__meter_BASE::parse_event(data, data_size);
	}

	//outgoing events (from driver to client)
	int send_set_state(double *state) const {
		return send_event(EVENT_SET_STATE, state);
	}

private:
	int send_event(event_t event_code, double *state) const {
		if(!is_inited()) return IOT_ERROR_NOT_INITED;
		uint32_t statesize=params->spec.num_sublines;
		char buf[sizeof(msg)+statesize*sizeof(double)];
		msg* msgp=(msg*)buf;
		memset(msgp, 0, sizeof(buf));
		msgp->event_code = event_code;
		msgp->statesize = uint8_t(statesize);
		if(state) memcpy(msgp->state, state, statesize*sizeof(double));

		return send_client_msg(buf, sizeof(buf));
	}
};


class iot_deviface__meter_CL : public iot_deviface__CLBASE, public iot_deviface__meter_BASE {

public:
	iot_deviface__meter_CL(const iot_conn_clientview *conn=NULL) {
		init(conn);
	}
	bool init(const iot_conn_clientview *conn=NULL) {
		if(!conn) { //uninit request
			//do uninit
			iot_deviface__CLBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		if(!iot_deviface__meter_BASE::init(conn->deviface)) {
			iot_deviface__CLBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		return iot_deviface__CLBASE::init(conn);
	}
//	bool is_inited(void) inherited from iot_deviface__CLBASE

	const msg* parse_event(const void *data, uint32_t data_size) const {
		if(!is_inited()) return NULL;
		return iot_deviface__meter_BASE::parse_event(data, data_size);
	}

	int request_state(void) const {
		return send_request(REQ_GET_STATE);
	}
	uint8_t get_statesize(void) const {
		if(!is_inited()) return 0;
		return params->spec.num_sublines;
	}

private:
	int send_request(req_t req_code) const {
		if(!is_inited()) return IOT_ERROR_NOT_INITED;

		uint32_t statesize=params->spec.num_sublines;
		char buf[sizeof(msg)+statesize*sizeof(double)];
		msg* msgp=(msg*)buf;
		memset(msgp, 0, sizeof(buf));
		msgp->req_code = req_code;
		msgp->statesize = 0;//uint8_t(statesize);

		return send_driver_msg(buf, sizeof(buf));
	}

};


#endif //IOT_DEVCLASS_METER_H
