#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>
#include<stdlib.h>
#include<unistd.h>
#include<assert.h>
#include<new>
#include<cmath>

#include <linux/input.h>

#include "iot_module.h"

IOT_LIBVERSION_DEFINE;

#include "iot_devclass_keyboard.h"


struct kbdinput_instance : public iot_node_base {
	uint32_t node_id=0;
	const iot_conn_clientview *cconn=NULL;
	char rfidbuf[11];
	int rfidlen=0;
	bool rfiderror=false; //when true, KEY_ENTER or KEY_ESC is awaited to reset it

	static int init_instance(iot_node_base**instance, uint32_t node_id, json_object *json_cfg) {
		kbdinput_instance *inst=new kbdinput_instance(node_id);
		*instance=inst;

		return 0;
	}
	static int deinit_instance(iot_node_base* instance) {
		kbdinput_instance *inst=static_cast<kbdinput_instance*>(instance);
		inst->unref();
		return 0;
	}
private:
	kbdinput_instance(uint32_t node_id) : node_id(node_id) {}

	virtual int start(void) override {
		return 0;
	}
	virtual int stop(void) override {
		return 0;
	}

	virtual int device_attached(const iot_conn_clientview* conn) override {
		assert(cconn==NULL);
		cconn=conn;
		return 0;
	}
	virtual int device_detached(const iot_conn_clientview* conn) override {
		cconn=NULL;
		return 0;
	}
	virtual int device_action(const iot_conn_clientview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) override {
		int err;
		if(action_code==IOT_DEVCONN_ACTION_FULLREQUEST) {//new message arrived
			iot_deviface__keyboard_CL iface(conn);
			const iot_deviface__keyboard_CL::msg* msg=iface.parse_event(data, data_size);
			if(!msg) return 0;

			if(msg->event_code!=iface.EVENT_KEYDOWN) return 0;
			kapi_outlog_info("GOT keyboard DOWN for key %d from device index %d", (int)msg->key, int(conn->index));
			if(rfiderror) {
				if(msg->key!=KEY_ENTER && msg->key!=KEY_ESC) return 0;
				rfiderror=false;
				rfidlen=0;
				return 0;
			}
			switch(msg->key) {
				case KEY_1: case KEY_2: case KEY_3: case KEY_4: case KEY_5: case KEY_6: case KEY_7: case KEY_8: case KEY_9:
					if(rfidlen>=10) {rfiderror=true;break;}
					rfidbuf[rfidlen++]='1'+msg->key-KEY_1;
					break;
				case KEY_0:
					if(rfidlen>=10) {rfiderror=true;break;}
					rfidbuf[rfidlen++]='0';
					break;
				case KEY_ESC:
					rfidlen=0;
					break;
				case KEY_ENTER: {
					if(rfidlen<4) {rfidlen=0; break;} //ignore
					rfidbuf[rfidlen]='\0';
					rfidlen=0;
					uint32_t rfid=iot_strtou32(rfidbuf, NULL, 10);
					iot_datavalue_numeric m(rfid, false);
					uint8_t outn=0;
					const iot_datavalue* outm=&m;
					err=kapi_update_outputs(NULL, 0, NULL, NULL, 1, &outn, &outm);
					if(err) {
						kapi_outlog_error("Cannot send message for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
					}
					break;
				}
			}
			return 0;
		}
		kapi_outlog_info("Device action, node_id=%u, act code %u, datasize %u from device index %d", node_id, unsigned(action_code), data_size, int(conn->index));
		return 0;
	}
};

static const iot_deviface_params_keyboard kbd_filter_any(2);

static const iot_deviface_params* kbdinput_devifaces[]={
	&kbd_filter_any
};

static const iot_datatype_metaclass* kbdinput_msgoutclasses[]={
	&iot_datatype_metaclass_numeric::object
};

iot_node_moduleconfig_t IOT_NODE_MODULE_CONF(kbdinput)={
	.version = IOT_VERSION_COMPOSE(0,0,1),
	.cpu_loading = 0,
	.num_devices = 1,
	.num_valueoutputs = 0,
	.num_valueinputs = 0,
	.num_msgoutputs = 1,
	.num_msginputs = 0,
	.is_persistent = 1,
	.is_sync = 0,

	.devcfg={
		{
			.label = "dev",
			.num_devifaces = sizeof(kbdinput_devifaces)/sizeof(kbdinput_devifaces[0]),
			.flag_canauto = 1,
			.flag_localonly = 0,
			.devifaces = kbdinput_devifaces
		}
	},
	.valueoutput={
	},
	.valueinput={
	},
	.msgoutput={
		{
			.label = "out",
			.num_dataclasses = sizeof(kbdinput_msgoutclasses)/sizeof(kbdinput_msgoutclasses[0]),
			.dataclasses = kbdinput_msgoutclasses
		}
	},
	.msginput={
	},

	//methods
	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &kbdinput_instance::init_instance,
	.deinit_instance = &kbdinput_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};

//end of kbdlinux:keys event source  module



struct op_inlist_instance : public iot_node_base {
	uint32_t node_id;
	uint32_t *validlist=NULL; //array of valid numbers
	uint16_t numvalid=0;

/////////////static fields/methods for module instances management
	static int init_instance(iot_node_base** instance, uint32_t node_id, json_object *json_cfg) {
		uint16_t numvalid=0;
		uint32_t *validlist=NULL;
		if(json_cfg) {
			json_object *val=NULL;
			int num;
			if(json_object_object_get_ex(json_cfg, "list", &val) && json_object_is_type(val, json_type_array) && (num=json_object_array_length(val))>0) {
				if(num>100) num=100;
				validlist=new uint32_t[num];
				if(!validlist) return IOT_ERROR_TEMPORARY_ERROR;
				for(int i=0;i<num;i++) {
					json_object* it=json_object_array_get_idx(val, i);
					uint32_t id=0;
					IOT_JSONPARSE_UINT(it, uint32_t, id);
					if(!id) continue;
					validlist[numvalid++]=id;
				}
			}
		}
		kapi_outlog_info("OPERATOR op_inlist INITED node_id=%u, num_valid=%u", node_id, unsigned(numvalid));
		op_inlist_instance *inst=new op_inlist_instance(node_id, numvalid, validlist);
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_node_base* instance) {
		static_cast<op_inlist_instance*>(instance)->unref();
		return 0;
	}
private:
	op_inlist_instance(uint32_t node_id, uint16_t numvalid, uint32_t *validlist) : node_id(node_id), validlist(validlist), numvalid(numvalid) {
	}
	~op_inlist_instance(void) {
		if(validlist) delete validlist;
		validlist=NULL;
	}

	virtual int start(void) override {
		return 0;
	}

	virtual int stop(void) override {
		return 0;
	}

//methods from iot_node_base
	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) override {
		assert(num_msginputs==1);
		uint8_t outn;
		const iot_datavalue* outm;

		outm=&iot_datavalue_pulse::object;

		if(!msginputs[0].num) return 0;
		const iot_datavalue_numeric *input=iot_datavalue_numeric::cast(msginputs[0].msgs[0]); //ignore other messages except first one
		if(!input) return 0; //unknown type of message

		uint32_t i;
		for(i=0;i<numvalid;i++) {
			if(validlist[i]==input->get_value()) break;
		}
		if(i<numvalid) { //valid
			outn=0; //send pulse on output 'out'
		} else { //invalid
			outn=1; //send purse on output 'neg'
		}

		int err=kapi_update_outputs(&eventid, 0, NULL, NULL, 1, &outn, &outm);
		if(err) {
			kapi_outlog_error("Cannot update output value for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
		}
		return 0; // use IOT_ERROR_NOT_READY for debugging async exec
	}
};

//keys_instance* keys_instance::instances_head=NULL;


static const iot_datatype_metaclass* op_inlist_msgoutclasses[]={
	&iot_datatype_metaclass_pulse::object
};

static const iot_datatype_metaclass* op_inlist_msginclasses[]={
	&iot_datatype_metaclass_numeric::object
};

iot_node_moduleconfig_t IOT_NODE_MODULE_CONF(op_inlist)={
	.version = IOT_VERSION_COMPOSE(0,0,1),
	.cpu_loading = 0,
	.num_devices = 0,
	.num_valueoutputs = 0,
	.num_valueinputs = 0,
	.num_msgoutputs = 2,
	.num_msginputs = 1,
	.is_persistent = 0,
	.is_sync = 1,

	.devcfg={},
	.valueoutput={
	},
	.valueinput={
	},
	.msgoutput={
		{
			.label = "out",
			.num_dataclasses = sizeof(op_inlist_msgoutclasses)/sizeof(op_inlist_msgoutclasses[0]),
			.dataclasses = op_inlist_msgoutclasses
		},
		{
			.label = "neg",
			.num_dataclasses = sizeof(op_inlist_msgoutclasses)/sizeof(op_inlist_msgoutclasses[0]),
			.dataclasses = op_inlist_msgoutclasses
		}
	},
	.msginput={
		{
			.label = "in",
			.num_dataclasses = sizeof(op_inlist_msginclasses)/sizeof(op_inlist_msginclasses[0]),
			.dataclasses = op_inlist_msginclasses
		}
	},

	//methods
	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &op_inlist_instance::init_instance,
	.deinit_instance = &op_inlist_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};


