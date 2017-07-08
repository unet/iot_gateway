#ifndef IOT_DEVCLASS_ACTIVATABLE_H
#define IOT_DEVCLASS_ACTIVATABLE_H
//Contains interface to communicate using IOT_DEVIFACETYPEID_ACTIVATABLE class

#include<stdint.h>
//#include<time.h>
#include<assert.h>
#include<ecb.h>

struct iot_devifacetype_activatable : public iot_devifacetype_iface {
	struct data_t { //format of interface class data (class attributes)
		uint32_t format; //version of format or magic code
		uint32_t is_tmpl:1,
			num_sublines:10; //number of control sublines (or controled subdevices). value 0 is allowed in template only to mean 'any number'. non-zero values
							//means "up to" specified value (i.e. 2 means that device with 1 or 2 subdevices suits)
	};
	iot_devifacetype_activatable(void) : iot_devifacetype_iface(IOT_DEVIFACETYPEID_ACTIVATABLE, "Activatable") {
	}
	static void init_classdata(iot_devifacetype* devclass, bool is_tmpl, uint16_t num_sublines) {
		if(num_sublines > 32) num_sublines=32;
		devclass->classid=IOT_DEVIFACETYPEID_ACTIVATABLE;
		if(!is_tmpl && num_sublines==0) num_sublines=1;
		*((data_t*)devclass->data)={
			.format=1,
			.is_tmpl=is_tmpl,
			.num_sublines=num_sublines
		};
	}
	const data_t* parse_classdata(const char* cls_data) const {
		const data_t* d=(const data_t*)cls_data;
		if(d->format!=1) return NULL;
		return d;
	}

private:
	virtual bool check_data(const char* cls_data) const override { //actual check that data is good by format
		data_t* data=(data_t*)cls_data;
		return data->format==1;
	}
	virtual bool check_istmpl(const char* cls_data) const override { //actual check that data corresponds to template (so not all data components are specified)
		data_t* data=(data_t*)cls_data;
		return data->is_tmpl==1;
	}
	virtual size_t print_data(const char* cls_data, char* buf, size_t bufsize) const override { //actual class data printing function. it must return number of written bytes (without NUL)
		data_t* data=(data_t*)cls_data;
		int len;
		if(check_istmpl(cls_data)) {
			if(!data->num_sublines)
				len=snprintf(buf, bufsize, "%s (TMPL: sublines=any)",name);
			else
				len=snprintf(buf, bufsize, "%s (TMPL: sublines=up to %u)",name, unsigned(data->num_sublines));
		} else
			len=snprintf(buf, bufsize, "%s (sublines=%u)",name,unsigned(data->num_sublines));
		return len>=int(bufsize) ? bufsize-1 : len;
	}
	virtual uint32_t get_d2c_maxmsgsize(const char* cls_data) const override;
	virtual uint32_t get_c2d_maxmsgsize(const char* cls_data) const override;
	virtual bool compare(const char* cls_data, const char* tmpl_data) const override { //actual comparison function
		data_t* data=(data_t*)cls_data;
		data_t* tmpl=(data_t*)tmpl_data;
		if(check_istmpl(tmpl_data)) return tmpl->num_sublines==0 || tmpl->num_sublines>=data->num_sublines;
		return !check_istmpl(cls_data) && tmpl->num_sublines==data->num_sublines;
	}
};


class iot_devifaceclass__activatable_BASE {
public:
	enum req_t : uint8_t { //commands (requests) which driver can execute
		REQ_GET_STATE, //request to post EVENT_CURRENT_STATE with status of all sublines. no 'data' is used
		REQ_SET_STATE //request to activate and/or deactivate specific sublines
	};

	enum event_t : uint8_t { //events (or replies) which driver can send to client
		EVENT_CURRENT_STATE //reply to REQ_GET_STATE request. provides current state of all sublines.
	};

	struct msg { //use same message format for requests and events (but this is not mandatory. each event type can have own structure)
		union { //field is determined by usage context
			struct {
				req_t req_code;
				uint32_t activate_mask, deactivate_mask; //used for REQ_SET_STATE to indicate which subline must be activated and deactivated. 
														//if same bit is set in both masks, nothing is done to corresponding line
			};
			struct {
				event_t event_code;
				uint32_t state_mask, valid_mask;
			};
		};
	};
	const msg* parse_event(const void *data, uint32_t data_size) {
		if(data_size != sizeof(msg)) return NULL;
		return static_cast<const msg*>(data);
	}
	static uint32_t get_maxmsgsize(void) {
		return sizeof(msg);
	}
protected:
	const iot_devifacetype_activatable::data_t *attr;

	iot_devifaceclass__activatable_BASE(const iot_devifacetype *devclass) {
		const iot_devifacetype_iface* iface=devclass->find_iface();
		if(iface && iface->classid==IOT_DEVIFACETYPEID_ACTIVATABLE) {
			attr=static_cast<const iot_devifacetype_activatable*>(iface)->parse_classdata(devclass->data);
			assert(attr!=NULL && attr->num_sublines>0);
		} else {
			attr=NULL;
		}
	}
};


class iot_devifaceclass__activatable_DRV : public iot_devifaceclass__DRVBASE, public iot_devifaceclass__activatable_BASE {

public:
	iot_devifaceclass__activatable_DRV(const iot_devifacetype *devclass) : iot_devifaceclass__DRVBASE(devclass),
																				iot_devifaceclass__activatable_BASE(devclass) {
	}

	//outgoing events (from driver to client)
	int send_current_state(const iot_conn_drvview *conn, uint32_t state_mask, uint32_t valid_mask) {
		if(!attr) return IOT_ERROR_NOT_INITED;
		uint32_t mask=(0xFFFFFFFFu >> (32 - attr->num_sublines)); //make 1 in lower attr->num_sublines bits
		state_mask&=mask; //reset excess higher bits
		valid_mask&=mask; //always mark as invalid excess higher bits

		msg msg;
		memset(&msg, 0, sizeof(msg));
		msg.event_code = EVENT_CURRENT_STATE;
		msg.state_mask=state_mask;
		msg.valid_mask=valid_mask;

		return send_client_msg(conn, &msg, sizeof(msg));
	}
};

class iot_devifaceclass__activatable_CL : public iot_devifaceclass__CLBASE, public iot_devifaceclass__activatable_BASE {

public:
	iot_devifaceclass__activatable_CL(const iot_devifacetype *devclass) : iot_devifaceclass__CLBASE(devclass),
																				iot_devifaceclass__activatable_BASE(devclass) {
	}

	int get_state(const iot_conn_clientview* conn) {
		return send_request(conn, REQ_GET_STATE, 0, 0);
	}
	int set_state(const iot_conn_clientview* conn, uint32_t activate_mask, uint32_t deactivate_mask) {
		return send_request(conn, REQ_SET_STATE, activate_mask, deactivate_mask);
	}

private:
	int send_request(const iot_conn_clientview* conn, req_t req_code, uint32_t activate_mask, uint32_t deactivate_mask) {
		if(!attr) return IOT_ERROR_NOT_INITED;
		uint32_t mask=(0xFFFFFFFFu >> (32 - attr->num_sublines)); //make 1 in lower attr->num_sublines bits
		activate_mask&=mask;
		deactivate_mask&=mask;
		if(!activate_mask && !deactivate_mask) return 0; //no action

		msg msg;
		memset(&msg, 0, sizeof(msg));
		msg.req_code = req_code;
		msg.activate_mask=activate_mask;
		msg.deactivate_mask=deactivate_mask;
		return send_driver_msg(conn, &msg, sizeof(msg));
	}

};


#endif //IOT_DEVCLASS_ACTIVATABLE_H
