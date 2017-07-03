#ifndef IOT_DEVCLASS_ACTIVATABLE_H
#define IOT_DEVCLASS_ACTIVATABLE_H
//Contains interface to communicate using IOT_DEVIFACETYPEID_ACTIVATABLE class

#include<stdint.h>
//#include<time.h>
#include<assert.h>
#include<ecb.h>

struct iot_devifacetype_activatable : public iot_devifacetype_iface {
	iot_devifacetype_activatable(void) : iot_devifacetype_iface(IOT_DEVIFACETYPEID_ACTIVATABLE, "Activatable") {
	}
private:
	virtual bool check_data(const char* cls_data) const override { //actual check that data is good by format
		return true;
	}
	virtual bool check_istmpl(const char* cls_data) const override { //actual check that data corresponds to template (so not all data components are specified)
		return false;
	}
	virtual size_t print_data(const char* cls_data, char* buf, size_t bufsize) const override { //actual class data printing function. it must return number of written bytes (without NUL)
		int len=snprintf(buf, bufsize, "%s",name);
		return len>=int(bufsize) ? bufsize-1 : len;
	}
	virtual uint32_t get_d2c_maxmsgsize(const char* cls_data) const override {
		return 0;
	}
	virtual uint32_t get_c2d_maxmsgsize(const char* cls_data) const override;
	virtual bool compare(const char* cls_data, const char* tmpl_data) const override { //actual comparison function
		return true;
	}
};


class iot_devifaceclass__activatable_BASE {
public:
	enum req_t : uint8_t { //commands (requests) which driver can execute
		REQ_ACTIVATE,
		REQ_DEACTIVATE
	};

	struct msg { //use same message format for requests and events (but this is not mandatory. each event type can have own structure)
		req_t req_code;
	};
	const msg* parse_event(const void *data, uint32_t data_size) {
		if(data_size != sizeof(msg)) return NULL;
		return static_cast<const msg*>(data);
	}
	static uint32_t get_maxmsgsize(void) {
		return sizeof(msg);
	}
protected:

	iot_devifaceclass__activatable_BASE(const iot_devifacetype *devclass) {
		const iot_devifacetype_iface* iface=devclass->find_iface();
		assert(iface!=NULL);
		assert(iface->classid==IOT_DEVIFACETYPEID_ACTIVATABLE);
	}
};


class iot_devifaceclass__activatable_DRV : public iot_devifaceclass__DRVBASE, public iot_devifaceclass__activatable_BASE {

public:
	iot_devifaceclass__activatable_DRV(const iot_devifacetype *devclass) : iot_devifaceclass__DRVBASE(devclass),
																				iot_devifaceclass__activatable_BASE(devclass) {
	}

	//outgoing events (from driver to client)
	//NONE
};

class iot_devifaceclass__activatable_CL : public iot_devifaceclass__CLBASE, public iot_devifaceclass__activatable_BASE {

public:
	iot_devifaceclass__activatable_CL(const iot_devifacetype *devclass) : iot_devifaceclass__CLBASE(devclass),
																				iot_devifaceclass__activatable_BASE(devclass) {
	}

	int activate(const iot_connid_t &connid, iot_driver_client_base *client_inst) {
		return send_request(connid, client_inst, REQ_ACTIVATE);
	}
	int deactivate(const iot_connid_t &connid, iot_driver_client_base *client_inst) {
		return send_request(connid, client_inst, REQ_DEACTIVATE);
	}

private:
	int send_request(const iot_connid_t &connid, iot_driver_client_base *client_inst, req_t req_code) {
		char buf[sizeof(msg)];
		msg* msgp=(msg*)buf;
		memset(msgp, 0, sizeof(buf));
		msgp->req_code = req_code;

		return send_driver_msg(connid, client_inst, buf, sizeof(buf));
	}

};


#endif //IOT_DEVCLASS_ACTIVATABLE_H
