#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>
#include<stdlib.h>
#include<unistd.h>
#include<assert.h>
#include<new>
#include<cmath>

#include "iot_module.h"

IOT_LIBVERSION_DEFINE;


struct pulse_bool_instance : public iot_node_base {
	uint32_t node_id;
	uv_timer_t timer_watcher={};
	uint32_t num_pulses;
	uint32_t high_dur;
	uint32_t low_dur;
	bool restart_onrepeat;

	bool timer_active=false; //true value means generating is in progress
	bool cur_output=false;
	bool restart_pending=false;
	int32_t pulses_left=0;

/////////////static fields/methods for module instances management
	static int init_instance(iot_node_base** instance, uint32_t node_id, json_object *json_cfg) {
		uint32_t num_pulses=1;
		uint32_t high_dur=1000;
		uint32_t low_dur=1000;
		bool restart_onrepeat=false;

		if(json_cfg) {
			json_object *val=NULL;
			if(json_object_object_get_ex(json_cfg, "num_pulses", &val)) IOT_JSONPARSE_UINT(val, uint32_t, num_pulses);
			if(num_pulses==0) num_pulses=1;
				else if(num_pulses>1000) num_pulses=1000;
			if(json_object_object_get_ex(json_cfg, "true_dur", &val)) IOT_JSONPARSE_UINT(val, uint32_t, high_dur);
			if(high_dur==0) high_dur=1;
				else if(high_dur>3600000) high_dur=3600000;
			if(json_object_object_get_ex(json_cfg, "false_dur", &val)) IOT_JSONPARSE_UINT(val, uint32_t, low_dur);
			if(low_dur==0 && num_pulses>1) low_dur=1; //allow zero low_dur when num_pulses==1
				else if(low_dur>3600000) low_dur=3600000;
			if(json_object_object_get_ex(json_cfg, "allow_restart", &val) && json_object_get_boolean(val)) restart_onrepeat=true;
		}
		kapi_outlog_info("OPERATOR pulse_bool INITED node_id=%u, num_pulses=%u, true_dur=%u ms, false_dur=%u ms, allow_restart=%d", node_id, unsigned(num_pulses), unsigned(high_dur), unsigned(low_dur), restart_onrepeat ? "true" : "false");
		pulse_bool_instance *inst=new pulse_bool_instance(node_id, num_pulses, high_dur, low_dur, restart_onrepeat);
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_node_base* instance) {
		static_cast<pulse_bool_instance*>(instance)->unref();
		return 0;
	}
private:
	pulse_bool_instance(uint32_t node_id, uint32_t num_pulses, uint32_t high_dur, uint32_t low_dur, bool restart_onrepeat) : node_id(node_id), num_pulses(num_pulses), high_dur(high_dur), low_dur(low_dur), restart_onrepeat(restart_onrepeat) {
	}
	virtual int start(void) override {
		uv_timer_init(loop, &timer_watcher);
		timer_watcher.data=this;
		ref(); //instance is now additionally reffered in uv handle, so increase refcount to have correct closing of handle
		update_output();
		return 0;
	}
	virtual int stop(void) override {
		uv_close((uv_handle_t*)&timer_watcher, kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'
		return 0;
	}

	static void on_timer_static(uv_timer_t* handle) {
		pulse_bool_instance* obj=static_cast<pulse_bool_instance*>(handle->data);
		obj->on_timer();
	}
	void on_timer(void) {
//		timer_active=false;

		if(cur_output) { //was high level
			cur_output=false;

			int err=uv_timer_start(&timer_watcher, on_timer_static, low_dur, 0);
			assert(err==0);
			
		} else { //was low level
			assert(pulses_left>0);
			pulses_left--;
			if(!pulses_left) {
				if(restart_pending) {
					start_gen();
				} else {
					timer_active=false;
				}
				return;
			}

			cur_output=true;

			int err=uv_timer_start(&timer_watcher, on_timer_static, high_dur, 0);
			assert(err==0);
		}
		update_output();
	}

//methods from iot_node_base
	void update_output(void) {
		uint8_t outn=0;
		const iot_datavalue* outv=cur_output ? &iot_datavalue_boolean::const_true : &iot_datavalue_boolean::const_false;

		int err=kapi_update_outputs(NULL, 1, &outn, &outv);
		if(err) {
			kapi_outlog_error("Cannot update output value for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
		}
	}
	void start_gen(void) {
		pulses_left=num_pulses;
		restart_pending=false;
		cur_output=true;

		int err=uv_timer_start(&timer_watcher, on_timer_static, high_dur, 0);
		assert(err==0);

		timer_active=true;
		update_output();
	}
	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) override {
		assert(num_msginputs==1);
		if(!msginputs[0].num) return 0;

		if(!timer_active) {
			start_gen();
			return 0;
		}
		if(restart_onrepeat) restart_pending=true;

		return 0; // use IOT_ERROR_NOT_READY for debugging async exec
	}
};

//keys_instance* keys_instance::instances_head=NULL;


static const iot_datatype_metaclass* pulse_bool_msgclasses[]={
	&iot_datatype_metaclass_pulse::object
};

iot_node_moduleconfig_t IOT_NODE_MODULE_CONF(pulse_bool)={
	.version = IOT_VERSION_COMPOSE(0,0,1),
	.cpu_loading = 0,
	.num_devices = 0,
	.num_valueoutputs = 1,
	.num_valueinputs = 0,
	.num_msgoutputs = 0,
	.num_msginputs = 1,
	.is_persistent = 0,
	.is_sync = 0,

	.devcfg={},
	.valueoutput={
		{
			.label = "out",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		},
	},
	.valueinput={
	},
	.msgoutput={
	},
	.msginput={
		{
			.label = "in",
			.num_dataclasses = sizeof(pulse_bool_msgclasses)/sizeof(pulse_bool_msgclasses[0]),
			.dataclasses = pulse_bool_msgclasses
		}
	},

	//methods
	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &pulse_bool_instance::init_instance,
	.deinit_instance = &pulse_bool_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};



struct pulse_instance : public iot_node_base {
	uint32_t node_id;
	uv_timer_t timer_watcher={};
	uint32_t num_pulses;
	uint32_t period_dur;
	bool restart_onrepeat;

	bool timer_active=false; //true value means generating is in progress
	bool restart_pending=false;
	int32_t pulses_left=0;

/////////////static fields/methods for module instances management
	static int init_instance(iot_node_base** instance, uint32_t node_id, json_object *json_cfg) {
		uint32_t num_pulses=1;
		uint32_t period_dur=1000;
		bool restart_onrepeat=false;

		if(json_cfg) {
			json_object *val=NULL;
			if(json_object_object_get_ex(json_cfg, "num_pulses", &val)) IOT_JSONPARSE_UINT(val, uint32_t, num_pulses);
			if(num_pulses==0) num_pulses=1;
				else if(num_pulses>1000) num_pulses=1000;
			if(json_object_object_get_ex(json_cfg, "period_dur", &val)) IOT_JSONPARSE_UINT(val, uint32_t, period_dur);
			if(period_dur==0) period_dur=1;
				else if(period_dur>3600000) period_dur=3600000;
			if(json_object_object_get_ex(json_cfg, "allow_restart", &val) && json_object_get_boolean(val)) restart_onrepeat=true;
		}
		kapi_outlog_info("OPERATOR pulse INITED node_id=%u, num_pulses=%u, period_dur=%u ms, allow_restart=%d", node_id, unsigned(num_pulses), unsigned(period_dur),  restart_onrepeat ? "true" : "false");
		pulse_instance *inst=new pulse_instance(node_id, num_pulses, period_dur, restart_onrepeat);
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_node_base* instance) {
		static_cast<pulse_instance*>(instance)->unref();
		return 0;
	}
private:
	pulse_instance(uint32_t node_id, uint32_t num_pulses, uint32_t period_dur, bool restart_onrepeat) : node_id(node_id), num_pulses(num_pulses), period_dur(period_dur), restart_onrepeat(restart_onrepeat) {
	}
	virtual int start(void) override {
		uv_timer_init(loop, &timer_watcher);
		timer_watcher.data=this;
		ref(); //instance is now additionally reffered in uv handle, so increase refcount to have correct closing of handle
		return 0;
	}
	virtual int stop(void) override {
		uv_close((uv_handle_t*)&timer_watcher, kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'
		return 0;
	}

	static void on_timer_static(uv_timer_t* handle) {
		pulse_instance* obj=static_cast<pulse_instance*>(handle->data);
		obj->on_timer();
	}
	void on_timer(void) {
//		timer_active=false;

		assert(pulses_left>0);
		pulses_left--;
		if(!pulses_left) {
			if(restart_pending) {
				start_gen();
			} else {
				timer_active=false;
			}
			return;
		}

		int err=uv_timer_start(&timer_watcher, on_timer_static, period_dur, 0);
		assert(err==0);
		output_pulse();
	}

//methods from iot_node_base
	void output_pulse(void) {
		uint8_t outn=0;
		const iot_datavalue* outv=&iot_datavalue_pulse::object;

		int err=kapi_update_outputs(NULL, 0, NULL, NULL, 1, &outn, &outv);
		if(err) {
			kapi_outlog_error("Cannot send msg for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
		}
	}
	void start_gen(void) {
		pulses_left=num_pulses;
		restart_pending=false;

		int err=uv_timer_start(&timer_watcher, on_timer_static, period_dur, 0);
		assert(err==0);

		timer_active=true;
		output_pulse();
	}
	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) override {
		assert(num_msginputs==1);
		if(!msginputs[0].num) return 0;

		if(!timer_active) {
			start_gen();
			return 0;
		}
		if(restart_onrepeat) restart_pending=true;

		return 0; // use IOT_ERROR_NOT_READY for debugging async exec
	}
};

//keys_instance* keys_instance::instances_head=NULL;


static const iot_datatype_metaclass* pulse_msgclasses[]={
	&iot_datatype_metaclass_pulse::object
};

iot_node_moduleconfig_t IOT_NODE_MODULE_CONF(pulse)={
	.version = IOT_VERSION_COMPOSE(0,0,1),
	.cpu_loading = 0,
	.num_devices = 0,
	.num_valueoutputs = 0,
	.num_valueinputs = 0,
	.num_msgoutputs = 1,
	.num_msginputs = 1,
	.is_persistent = 0,
	.is_sync = 0,

	.devcfg={},
	.valueoutput={
	},
	.valueinput={
	},
	.msgoutput={
		{
			.label = "out",
			.num_dataclasses = sizeof(pulse_msgclasses)/sizeof(pulse_msgclasses[0]),
			.dataclasses = pulse_msgclasses
		}
	},
	.msginput={
		{
			.label = "in",
			.num_dataclasses = sizeof(pulse_msgclasses)/sizeof(pulse_msgclasses[0]),
			.dataclasses = pulse_msgclasses
		}
	},

	//methods
	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &pulse_instance::init_instance,
	.deinit_instance = &pulse_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};




struct cpulse_instance : public iot_node_base {
	uint32_t node_id;
	uv_timer_t timer_watcher={};
	uint32_t period_dur;

	bool timer_active=false; //true value means generating is in progress
	int32_t pulses_left=0;

/////////////static fields/methods for module instances management
	static int init_instance(iot_node_base** instance, uint32_t node_id, json_object *json_cfg) {
		uint32_t period_dur=1000;

		if(json_cfg) {
			json_object *val=NULL;
			if(json_object_object_get_ex(json_cfg, "period_dur", &val)) IOT_JSONPARSE_UINT(val, uint32_t, period_dur);
			if(period_dur==0) period_dur=1;
				else if(period_dur>3600000) period_dur=3600000;
		}
		kapi_outlog_info("OPERATOR cpulse INITED node_id=%u, period_dur=%u ms", node_id, unsigned(period_dur));
		cpulse_instance *inst=new cpulse_instance(node_id, period_dur);
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_node_base* instance) {
		static_cast<cpulse_instance*>(instance)->unref();
		return 0;
	}
private:
	cpulse_instance(uint32_t node_id, uint32_t period_dur) : node_id(node_id), period_dur(period_dur) {
	}
	virtual int start(void) override {
		uv_timer_init(loop, &timer_watcher);
		timer_watcher.data=this;
		ref(); //instance is now additionally reffered in uv handle, so increase refcount to have correct closing of handle
		return 0;
	}
	virtual int stop(void) override {
		uv_close((uv_handle_t*)&timer_watcher, kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'
		return 0;
	}

	static void on_timer_static(uv_timer_t* handle) {
		cpulse_instance* obj=static_cast<cpulse_instance*>(handle->data);
		obj->on_timer();
	}
	void on_timer(void) {
		start_gen();
	}

//methods from iot_node_base
	void output_pulse(void) {
		uint8_t outn=0;
		const iot_datavalue* outv=&iot_datavalue_pulse::object;

		int err=kapi_update_outputs(NULL, 0, NULL, NULL, 1, &outn, &outv);
		if(err) {
			kapi_outlog_error("Cannot send msg for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
		}
	}
	void start_gen(void) {
		int err=uv_timer_start(&timer_watcher, on_timer_static, period_dur, 0);
		assert(err==0);

		timer_active=true;
		output_pulse();
	}
	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) override {
		assert(num_valueinputs==1);
		if(!valueinputs[0].new_value) return 0;
		const iot_datavalue_boolean* v=iot_datavalue_boolean::cast(valueinputs[0].new_value);
		if(!v) return 0; //not boolean?
		if(*v) { //enable generator
			if(!timer_active) start_gen();
		} else { //disable
			if(timer_active) {
				uv_timer_stop(&timer_watcher);
				timer_active=false;
			}
		}
		return 0;
	}
};

//keys_instance* keys_instance::instances_head=NULL;


static const iot_datatype_metaclass* cpulse_msgclasses[]={
	&iot_datatype_metaclass_pulse::object
};

iot_node_moduleconfig_t IOT_NODE_MODULE_CONF(cpulse)={
	.version = IOT_VERSION_COMPOSE(0,0,1),
	.cpu_loading = 0,
	.num_devices = 0,
	.num_valueoutputs = 0,
	.num_valueinputs = 1,
	.num_msgoutputs = 1,
	.num_msginputs = 0,
	.is_persistent = 0,
	.is_sync = 0,

	.devcfg={},
	.valueoutput={
	},
	.valueinput={
		{
			.label = "in",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		}
	},
	.msgoutput={
		{
			.label = "out",
			.num_dataclasses = sizeof(cpulse_msgclasses)/sizeof(cpulse_msgclasses[0]),
			.dataclasses = cpulse_msgclasses
		}
	},
	.msginput={
	},

	//methods
	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &cpulse_instance::init_instance,
	.deinit_instance = &cpulse_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};

