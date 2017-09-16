#ifndef IOT_DEVCLASS_KEYBOARD_H
#define IOT_DEVCLASS_KEYBOARD_H
//Contains interface to communicate using IOT_DEVCLASSID_KEYBOARD class

#include<stdint.h>
//#include<time.h>
#include<assert.h>
#include "ecb.h"

class iot_deviface_params_keyboard : public iot_deviface_params {
	friend class iot_devifacetype_metaclass_keyboard;
	friend class iot_deviface__keyboard_BASE;
	friend class iot_deviface__keyboard_DRV;
	friend class iot_deviface__keyboard_CL;

	union {
		struct {
			uint16_t max_keycode;
			uint8_t is_pckbd; //only 0 and 1
		} spec;
		struct {
			uint8_t is_pckbd; //0 and 1 mean exact match, 2 means 'any'
		} tmpl;
	};
	bool istmpl;

public:
	iot_deviface_params_keyboard(bool is_pckbd, uint16_t max_keycode);
	iot_deviface_params_keyboard(uint8_t is_pckbd);

	static const iot_deviface_params_keyboard* cast(const iot_deviface_params* params);

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
		get_fullname(buf, bufsize, &len);
		if(int(bufsize)-len>2) {
			int len1;
			if(istmpl)
				len1=snprintf(buf+len, bufsize-len, "{TMPL: is_pc=%s}", !tmpl.is_pckbd ? "no" : tmpl.is_pckbd==1 ? "yes" : "any");
			else
				len1=snprintf(buf+len, bufsize-len, "{max_keycode=%u, is_pc=%s}",unsigned(spec.max_keycode),!spec.is_pckbd ? "no" : "yes");
			if(len1>=int(bufsize)-len) len=bufsize-1;
				else len+=len1;
		}
		if(doff) *doff+=len;
		return buf;
	}
private:
	virtual bool p_matches(const iot_deviface_params* opspec0) const override {
		const iot_deviface_params_keyboard* opspec=cast(opspec0);
		if(!opspec) return false;
		if(istmpl) return tmpl.is_pckbd==2 || tmpl.is_pckbd==opspec->spec.is_pckbd;
		return spec.max_keycode==opspec->spec.max_keycode && spec.is_pckbd==opspec->spec.is_pckbd;
	}
};

class iot_devifacetype_metaclass_keyboard : public iot_devifacetype_metaclass {
	iot_devifacetype_metaclass_keyboard(void) : iot_devifacetype_metaclass(0, "keyboard", IOT_VERSION_COMPOSE(0,0,1)) {}

	PACKED(
		struct serialize_header_t {
			uint32_t format;
			uint8_t istmpl;
		}
	);
	PACKED(
		struct serialize_spec_t {
			uint8_t is_pckbd; //only 0 and 1
			uint16_t max_keycode;
		}
	);
	PACKED(
		struct serialize_tmpl_t {
			uint8_t is_pckbd; //0 and 1 mean exact match, 2 means 'any'
		}
	);


public:
	static const iot_devifacetype_metaclass_keyboard object; //the only instance of this class

private:
	virtual int p_serialized_size(const iot_deviface_params* obj0) const override {
		const iot_deviface_params_keyboard* obj=iot_deviface_params_keyboard::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		if(obj->istmpl) return sizeof(serialize_header_t)+sizeof(serialize_tmpl_t);
		return sizeof(serialize_header_t)+sizeof(serialize_spec_t);
	}
	virtual int p_serialize(const iot_deviface_params* obj0, char* buf, size_t bufsize) const override {
		const iot_deviface_params_keyboard* obj=iot_deviface_params_keyboard::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		if(bufsize<sizeof(serialize_header_t)) return IOT_ERROR_NO_BUFSPACE;
		bufsize-=sizeof(serialize_header_t);

		serialize_header_t *h=(serialize_header_t*)buf;
		if(obj->istmpl) {
			h->format=repack_uint32(uint32_t(1));
			h->istmpl=1;
			serialize_tmpl_t *t=(serialize_tmpl_t*)(h+1);
			t->is_pckbd=obj->tmpl.is_pckbd;
		} else {
			h->format=repack_uint32(uint32_t(1));
			h->istmpl=0;
			serialize_spec_t *s=(serialize_spec_t*)(h+1);
			s->is_pckbd=obj->spec.is_pckbd;
			s->max_keycode=repack_uint16(obj->spec.max_keycode);
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
		const iot_deviface_params_keyboard* obj=iot_deviface_params_keyboard::cast(obj0);
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
			if(obj->tmpl.is_pckbd<2) {
				val=json_object_new_boolean(obj->tmpl.is_pckbd);
				if(!val) {
					json_object_put(ob);
					return IOT_ERROR_NO_MEMORY;
				}
				json_object_object_add(ob, "is_pckbd", val);
			} //alse "any" is_pckbd allowed. represent it as undefined
		} else { //{is_pckbd: true/false, max_keycode: integer}
			val=json_object_new_boolean(obj->spec.is_pckbd);
			if(!val) {
				json_object_put(ob);
				return IOT_ERROR_NO_MEMORY;
			}
			json_object_object_add(ob, "is_pckbd", val);
			val=json_object_new_int(obj->spec.max_keycode);
			if(!val) {
				json_object_put(ob);
				return IOT_ERROR_NO_MEMORY;
			}
			json_object_object_add(ob, "max_keycode", val);
		}
		dst=ob;
		return 0;
	}
};


inline iot_deviface_params_keyboard::iot_deviface_params_keyboard(bool is_pckbd, uint16_t max_keycode) : iot_deviface_params(&iot_devifacetype_metaclass_keyboard::object),
	istmpl(false) {
		spec.is_pckbd=is_pckbd ? 1 : 0;

		uint32_t maxcode=iot_valuetype_bitmap::get_maxkeycode();
		spec.max_keycode = max_keycode > maxcode ? maxcode : max_keycode;
}
inline iot_deviface_params_keyboard::iot_deviface_params_keyboard(uint8_t is_pckbd) : iot_deviface_params(&iot_devifacetype_metaclass_keyboard::object),
	istmpl(true) {
		tmpl.is_pckbd=is_pckbd<=2 ? is_pckbd : 2;
}
inline const iot_deviface_params_keyboard* iot_deviface_params_keyboard::cast(const iot_deviface_params* params) {
	if(!params) return NULL;
	return params->get_metaclass()==&iot_devifacetype_metaclass_keyboard::object ? static_cast<const iot_deviface_params_keyboard*>(params) : NULL;
}



class iot_deviface__keyboard_BASE {
public:
	enum req_t : uint8_t { //commands (requests) which driver can execute
		REQ_GET_STATE, //request to post EVENT_INIT_STATE with status of all keys. no 'data' is used
	};

	enum event_t : uint8_t { //events (or replies) which driver can send to client
		EVENT_SET_STATE, //reply to REQ_GET_STATE request. provides current state of all keys.
		EVENT_KEYDOWN, //key was down
		EVENT_KEYUP, //key was up
		EVENT_KEYREPEAT, //key was autorepeated
	};

	struct msg { //use same message format for requests and events (but this is not mandatory. each event type can have own structure)
		union { //field is determined by usage context
			req_t req_code;
			event_t event_code;
		};
		uint8_t statesize; //number of items in state
		uint16_t key:15, //key code of depressed/released/repeated key for EVENT_KEYDOWN, EVENT_KEYUP, EVENT_KEYREPEAT
			was_drop:1; //flag that some messages were dropped before this one
		uint32_t state[]; //map of depressed keys for all consumer events. already includes result of current key action
	};
	constexpr static uint32_t get_maxmsgsize(uint16_t max_keycode) {
		return sizeof(msg)+((max_keycode / 32)+1)*sizeof(uint32_t);
	}
protected:
	const iot_deviface_params_keyboard* params;

	iot_deviface__keyboard_BASE(void) : params(NULL) {
	}
	bool init(const iot_deviface_params *deviface) {
		params=iot_deviface_params_keyboard::cast(deviface);
		if(!params || params->is_tmpl()) return false; //illegal interface type or is template
		return true;
	}
	const msg* parse_event(const void *data, uint32_t data_size) const {
		uint32_t statesize=(params->spec.max_keycode / 32)+1;
		if(data_size != sizeof(msg)+statesize*sizeof(uint32_t)) return NULL;
		return static_cast<const msg*>(data);
	}
};


class iot_deviface__keyboard_DRV : public iot_deviface__DRVBASE, public iot_deviface__keyboard_BASE {

public:
	iot_deviface__keyboard_DRV(const iot_conn_drvview *conn=NULL) {
		init(conn);
	}
	bool init(const iot_conn_drvview *conn=NULL) {
		if(!conn) { //uninit request
			//do uninit
			iot_deviface__DRVBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		if(!iot_deviface__keyboard_BASE::init(conn->deviface)) {
			iot_deviface__DRVBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		return iot_deviface__DRVBASE::init(conn);
	}
//	bool is_inited(void) inherited from iot_deviface__DRVBASE

	const msg* parse_req(const void *data, uint32_t data_size) const {
		if(!is_inited()) return NULL;
		return iot_deviface__keyboard_BASE::parse_event(data, data_size);
	}

	//outgoing events (from driver to client)
	int send_keydown(uint16_t keycode, uint32_t *state) const {
		return send_event(EVENT_KEYDOWN, keycode, state);
	}
	int send_keyup(uint16_t keycode, uint32_t *state) const {
		return send_event(EVENT_KEYUP, keycode, state);
	}
	int send_keyrepeat(uint16_t keycode, uint32_t *state) const {
		return send_event(EVENT_KEYREPEAT, keycode, state);
	}
	int send_set_state(uint32_t *state) const {
		return send_event(EVENT_SET_STATE, 0, state);
	}

private:
	int send_event(event_t event_code, uint16_t key_code, uint32_t *state) const {
		if(!is_inited()) return IOT_ERROR_NOT_INITED;
		uint32_t statesize=(params->spec.max_keycode / 32)+1;
		char buf[sizeof(msg)+statesize*sizeof(uint32_t)];
		msg* msgp=(msg*)buf;
		memset(msgp, 0, sizeof(buf));
		msgp->event_code = event_code;
		msgp->statesize = uint8_t(statesize);
		msgp->key = key_code;
		if(state) memcpy(msgp->state, state, statesize*sizeof(uint32_t));

		return send_client_msg(buf, sizeof(buf));
	}
};

class iot_deviface__keyboard_CL : public iot_deviface__CLBASE, public iot_deviface__keyboard_BASE {

public:
	iot_deviface__keyboard_CL(const iot_conn_clientview *conn=NULL) {
		init(conn);
	}
	bool init(const iot_conn_clientview *conn=NULL) {
		if(!conn) { //uninit request
			//do uninit
			iot_deviface__CLBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		if(!iot_deviface__keyboard_BASE::init(conn->deviface)) {
			iot_deviface__CLBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		return iot_deviface__CLBASE::init(conn);
	}
//	bool is_inited(void) inherited from iot_deviface__CLBASE

	const msg* parse_event(const void *data, uint32_t data_size) const {
		if(!is_inited()) return NULL;
		return iot_deviface__keyboard_BASE::parse_event(data, data_size);
	}

	int request_state(void) const {
		return send_request(REQ_GET_STATE);
	}
	uint16_t get_max_keycode(void) const {
		if(!is_inited()) return 0;
		return params->spec.max_keycode;
	}

private:
	int send_request(req_t req_code) const {
		if(!is_inited()) return IOT_ERROR_NOT_INITED;

		uint32_t statesize=(params->spec.max_keycode / 32)+1;
		char buf[sizeof(msg)+statesize*sizeof(uint32_t)];
		msg* msgp=(msg*)buf;
		memset(msgp, 0, sizeof(buf));
		msgp->req_code = req_code;
		msgp->statesize = uint8_t(statesize);
		msgp->key = 0;

		return send_driver_msg(buf, sizeof(buf));
	}

};


#endif //IOT_DEVCLASS_KEYBOARD_H
