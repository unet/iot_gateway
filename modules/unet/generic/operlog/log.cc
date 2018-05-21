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

struct and2_pulsealt_instance : public iot_node_base {
	uint32_t node_id;
	uv_timer_t timer_watcher={};
	uint32_t delay=0; //in millisecs
	bool timer_active=false;
	bool input1_active=false; //there is waiting signal on first input
	bool input2_active=false; //there is waiting signal on second input

/////////////static fields/methods for module instances management
	static int init_instance(iot_node_base** instance, uint32_t node_id, json_object *json_cfg) {
		uint32_t delay=1;
		if(json_cfg) {
			json_object *val=NULL;
			if(json_object_object_get_ex(json_cfg, "delay", &val)) IOT_JSONPARSE_UINT(val, uint32_t, delay);
		}
		kapi_outlog_info("OPERATOR and2_pulsealt INITED node_id=%u, delay=%u ms", node_id, unsigned(delay));
		and2_pulsealt_instance *inst=new and2_pulsealt_instance(node_id, delay);
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_node_base* instance) {
		static_cast<and2_pulsealt_instance*>(instance)->unref();
		return 0;
	}
private:
	and2_pulsealt_instance(uint32_t node_id, uint32_t delay) : node_id(node_id), delay(delay) {
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
		and2_pulsealt_instance* obj=static_cast<and2_pulsealt_instance*>(handle->data);
		obj->on_timer();
	}
	void on_timer(void) {
		timer_active=false;
		uint8_t outn;
		const iot_datavalue* outm=&iot_datavalue_pulse::object;
		int err;

		if(input1_active) {
			assert(!input2_active);
			outn=1; //alt1 output
		} else if(input2_active) {
			assert(!input1_active);
			outn=2; //alt2 output
		} else {
			assert(false);
			return;
		}
		err=kapi_update_outputs(NULL, 0, NULL, NULL, 1, &outn, &outm);
		if(err) {
			kapi_outlog_error("Cannot update output value for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
		}
		input1_active=input2_active=false;
	}

//methods from iot_node_base
	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) override {
		assert(num_msginputs==2);
		uint8_t outn;
		const iot_datavalue* outm=&iot_datavalue_pulse::object;
		int err;
		bool reply=false; //anything will be sent

		if(msginputs[0].num>0 && msginputs[1].num>0) { //rare case when two signals arrived at ones
			outn=0; //result output
			reply=true;
		} else if(msginputs[0].num>0) { //signal on first input
			if(input2_active) {
				outn=0; //result output
				reply=true;
			} else { //(re)start timer
				if(delay>0) {
					err=uv_timer_start(&timer_watcher, on_timer_static, delay, 0);
					assert(err==0);
					timer_active=true;
					input1_active=true;
				} else { //send immediate alt reply
					outn=1; //alt1 output
					reply=true;
				}
			}
		} else if(msginputs[1].num>0) { //signal on second input
			if(input1_active) {
				outn=0; //result output
				reply=true;
			} else { //(re)start timer
				if(delay>0) {
					err=uv_timer_start(&timer_watcher, on_timer_static, delay, 0);
					assert(err==0);
					timer_active=true;
					input2_active=true;
				} else { //send immediate alt reply
					outn=2; //alt2 output
					reply=true;
				}
			}
		}

		if(reply) {
			err=kapi_update_outputs(&eventid, 0, NULL, NULL, 1, &outn, &outm);
			if(err) {
				kapi_outlog_error("Cannot update output value for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
			}
			input1_active=input2_active=false;
			if(timer_active) {
				uv_timer_stop(&timer_watcher);
				timer_active=false;
			}
		}

		return 0; // use IOT_ERROR_NOT_READY for debugging async exec
	}
};

//keys_instance* keys_instance::instances_head=NULL;


static const iot_datatype_metaclass* and2_pulsealt_msgclasses[]={
	&iot_datatype_metaclass_pulse::object
};

iot_node_moduleconfig_t IOT_NODE_MODULE_CONF(and2_pulsealt)={
	.version = IOT_VERSION_COMPOSE(0,0,1),
	.cpu_loading = 0,
	.num_devices = 0,
	.num_valueoutputs = 0,
	.num_valueinputs = 0,
	.num_msgoutputs = 3,
	.num_msginputs = 2,
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
			.num_dataclasses = sizeof(and2_pulsealt_msgclasses)/sizeof(and2_pulsealt_msgclasses[0]),
			.dataclasses = and2_pulsealt_msgclasses
		},
		{
			.label = "alt1",
			.num_dataclasses = sizeof(and2_pulsealt_msgclasses)/sizeof(and2_pulsealt_msgclasses[0]),
			.dataclasses = and2_pulsealt_msgclasses
		},
		{
			.label = "alt2",
			.num_dataclasses = sizeof(and2_pulsealt_msgclasses)/sizeof(and2_pulsealt_msgclasses[0]),
			.dataclasses = and2_pulsealt_msgclasses
		}
	},
	.msginput={
		{
			.label = "in1",
			.num_dataclasses = sizeof(and2_pulsealt_msgclasses)/sizeof(and2_pulsealt_msgclasses[0]),
			.dataclasses = and2_pulsealt_msgclasses
		},
		{
			.label = "in2",
			.num_dataclasses = sizeof(and2_pulsealt_msgclasses)/sizeof(and2_pulsealt_msgclasses[0]),
			.dataclasses = and2_pulsealt_msgclasses
		}
	},

	//methods
	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &and2_pulsealt_instance::init_instance,
	.deinit_instance = &and2_pulsealt_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};






struct and2_boolordered_instance : public iot_node_base {
	uint32_t node_id;
	uv_timer_t timer_watcher={};
	uint32_t out_delay=0; //in millisecs
	uint32_t ordering_delay=0; //in millisecs
	bool timer_active=false;
	bool cur_output=false;
	uint64_t last_input1_rise=0; //time when input1 has last rising signal edge
	uint64_t last_input2_rise=0;

/////////////static fields/methods for module instances management
	static int init_instance(iot_node_base** instance, uint32_t node_id, json_object *json_cfg) {
		uint32_t out_delay=0;
		uint32_t ordering_delay=0; //zero value disables alt1 and alt2 forming
		if(json_cfg) {
			json_object *val=NULL;
			if(json_object_object_get_ex(json_cfg, "out_delay", &val)) IOT_JSONPARSE_UINT(val, uint32_t, out_delay);
			if(json_object_object_get_ex(json_cfg, "ordering_delay", &val)) IOT_JSONPARSE_UINT(val, uint32_t, ordering_delay);
		}
		kapi_outlog_info("OPERATOR and2_boolordered INITED node_id=%u, out_delay=%u ms, ordering_delay=%u ms", node_id, unsigned(out_delay), unsigned(ordering_delay));
		and2_boolordered_instance *inst=new and2_boolordered_instance(node_id, out_delay, ordering_delay);
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_node_base* instance) {
		static_cast<and2_boolordered_instance*>(instance)->unref();
		return 0;
	}
private:
	and2_boolordered_instance(uint32_t node_id, uint32_t out_delay, uint32_t ordering_delay) : node_id(node_id), out_delay(out_delay), ordering_delay(ordering_delay) {
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
		and2_boolordered_instance* obj=static_cast<and2_boolordered_instance*>(handle->data);
		obj->on_timer();
	}
	void on_timer(void) {
		timer_active=false;

		cur_output=true;

		update_output();
	}
	void update_output(void) {
		uint8_t outn=0;
		const iot_datavalue* outv=cur_output ? &iot_datavalue_boolean::const_true : &iot_datavalue_boolean::const_false;

		int err=kapi_update_outputs(NULL, 1, &outn, &outv);
		if(err) {
			kapi_outlog_error("Cannot update output value for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
		}
	}

//methods from iot_node_base
	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) override {
		assert(num_valueinputs==2);

		bool in1=false, in2=false;
//		bool rise1=false, rise2=false;
		bool fall1=false, fall2=false;
		uint64_t evtime=eventid.get_reltime_ms();

		uint8_t outvi=0, numoutv=0;
		const iot_datavalue* outv=&iot_datavalue_boolean::const_false;

		uint8_t outmi, numoutm=0;
		const iot_datavalue* outm=&iot_datavalue_pulse::object;
		int err;


		const iot_datavalue_boolean* v=iot_datavalue_boolean::cast(valueinputs[0].new_value);
		if(v && *v) { //current value is true
			in1=true;
			if(ordering_delay>0) {
				v=iot_datavalue_boolean::cast(valueinputs[0].prev_value);
				if(v && !*v) { //prev value was false (not undef)
					last_input1_rise=evtime;
//					rise1=true;
				}
			}
		} else if(v && !*v) { //current value is false
			if(ordering_delay>0) {
				v=iot_datavalue_boolean::cast(valueinputs[0].prev_value);
				if(v && *v) { //prev value was true
					fall1=true;
				}
			}
		}

		v=iot_datavalue_boolean::cast(valueinputs[1].new_value);
		if(v && *v) { //current value is true
			in2=true;
			if(ordering_delay>0) {
				v=iot_datavalue_boolean::cast(valueinputs[1].prev_value);
				if(v && !*v) { //prev value was false (not undef)
					last_input2_rise=evtime;
//					rise2=true;
				}
			}
		} else if(v && !*v) { //current value is false
			if(ordering_delay>0) {
				v=iot_datavalue_boolean::cast(valueinputs[1].prev_value);
				if(v && *v) { //prev value was true
					fall2=true;
				}
			}
		}

		if(ordering_delay>0) {
//			if(rise1 && !rise2 && last_input2_rise>0 && last_input2_rise<last_input1_rise && last_input1_rise-last_input2_rise<=ordering_delay) {outmi=1;numoutm=1;} //ord2 pulse output
//			else if(rise2 && !rise1 && last_input1_rise>0 && last_input1_rise<last_input2_rise && last_input2_rise-last_input1_rise<=ordering_delay) {outmi=0;numoutm=1;} //ord1 pulse output
			if(!in1 && !in2 && last_input1_rise>0 && last_input2_rise>0) {
				if(fall1 && last_input2_rise<last_input1_rise && evtime-last_input2_rise<=ordering_delay) {outmi=1;numoutm=1;} //ord2 pulse output
				else if(fall2 && last_input1_rise<last_input2_rise && evtime-last_input1_rise<=ordering_delay) {outmi=0;numoutm=1;} //ord1 pulse output
			}
		}

		if(!in1 || !in2) { //out must be/become false
kapi_outlog_notice("AND2ORDERED: inputs false, in1=%d, in2=%d, cur_output=%d", int(in1), int(in2), int(cur_output));
			if(cur_output) {numoutv=1;cur_output=false;}
			else { //remains false
				if(timer_active) {uv_timer_stop(&timer_watcher);timer_active=false;}
			}
		} else  { //output remains true, already waits for timer, or timer must be set
kapi_outlog_notice("AND2ORDERED: inputs true, cur_output=%d", int(cur_output));
			if(!cur_output) { //current output is not true
				if(!timer_active) { //set timer to set output true
					err=uv_timer_start(&timer_watcher, on_timer_static, out_delay, 0);
					assert(err==0);
					timer_active=true;
				} //else timer already set
			} //else output remains true
		}

		err=kapi_update_outputs(&eventid, numoutv, numoutv ? &outvi : NULL, numoutv ? &outv : NULL, numoutm, numoutm ? &outmi : NULL, numoutm ? &outm : NULL);
		if(err) {
			kapi_outlog_error("Cannot update outputs for node_id=%" IOT_PRIiotid ": %s, event lost, numv=%d, numm=%d", node_id, kapi_strerror(err), int(numoutv), int(numoutm));
		}

		return 0; // use IOT_ERROR_NOT_READY for debugging async exec
	}
};

//keys_instance* keys_instance::instances_head=NULL;


static const iot_datatype_metaclass* and2_boolordered_msgclasses[]={
	&iot_datatype_metaclass_pulse::object
};

iot_node_moduleconfig_t IOT_NODE_MODULE_CONF(and2_boolordered)={
	.version = IOT_VERSION_COMPOSE(0,0,1),
	.cpu_loading = 0,
	.num_devices = 0,
	.num_valueoutputs = 1,
	.num_valueinputs = 2,
	.num_msgoutputs = 2,
	.num_msginputs = 0,
	.is_persistent = 0,
	.is_sync = 0,

	.devcfg={},
	.valueoutput={
		{
			.label = "out",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		}
	},
	.valueinput={
		{
			.label = "in1",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		},
		{
			.label = "in2",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		}
	},
	.msgoutput={
		{
			.label = "ord1",
			.num_dataclasses = sizeof(and2_boolordered_msgclasses)/sizeof(and2_boolordered_msgclasses[0]),
			.dataclasses = and2_boolordered_msgclasses
		},
		{
			.label = "ord2",
			.num_dataclasses = sizeof(and2_boolordered_msgclasses)/sizeof(and2_boolordered_msgclasses[0]),
			.dataclasses = and2_boolordered_msgclasses
		}
	},
	.msginput={
	},

	//methods
	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &and2_boolordered_instance::init_instance,
	.deinit_instance = &and2_boolordered_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};


//simplest synchronous boolean values OR operator
struct or4_bool_instance : public iot_node_base {
	uint32_t node_id;

/////////////static fields/methods for module instances management
	static int init_instance(iot_node_base** instance, uint32_t node_id, json_object *json_cfg) {
		or4_bool_instance *inst=new or4_bool_instance(node_id);
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_node_base* instance) {
		static_cast<or4_bool_instance*>(instance)->unref();
		return 0;
	}
private:
	or4_bool_instance(uint32_t node_id) : node_id(node_id) {
	}
	virtual int start(void) override {
		return 0;
	}
	virtual int stop(void) override {
		return 0;
	}

	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) override {
		const iot_datavalue* outv=&iot_datavalue_boolean::const_false;

		for(uint16_t i=0; i<num_valueinputs; i++) {
			const iot_datavalue_boolean* v=iot_datavalue_boolean::cast(valueinputs[i].new_value);
			//any True in gives immediate True output
			if(v && *v) {outv=&iot_datavalue_boolean::const_true;break;}
			//any undef changes output to undef
			if(!valueinputs[i].new_value) outv=NULL;
		}
		//all false inputs leaves false output
		uint8_t outvi=0;

		int err=kapi_update_outputs(&eventid, 1, &outvi, &outv);
		if(err) {
			kapi_outlog_error("Cannot update outputs for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
		}

		return 0; // use IOT_ERROR_NOT_READY for debugging async exec
	}
};

//keys_instance* keys_instance::instances_head=NULL;


iot_node_moduleconfig_t IOT_NODE_MODULE_CONF(or4_bool)={
	.version = IOT_VERSION_COMPOSE(1,0,0),
	.cpu_loading = 0,
	.num_devices = 0,
	.num_valueoutputs = 1,
	.num_valueinputs = 4,
	.num_msgoutputs = 0,
	.num_msginputs = 0,
	.is_persistent = 0,
	.is_sync = 1,

	.devcfg={},
	.valueoutput={
		{
			.label = "out",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		}
	},
	.valueinput={
		{
			.label = "in1",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		},
		{
			.label = "in2",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		},
		{
			.label = "in3",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		},
		{
			.label = "in4",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		}
	},
	.msgoutput={
	},
	.msginput={
	},

	//methods
	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &or4_bool_instance::init_instance,
	.deinit_instance = &or4_bool_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};


//synchronous Numeric range checking operator, gives 2 boolean outputs - positive and negative. from <= NUMERIC <= to
struct num_inrange_instance : public iot_node_base {
	uint32_t node_id;
	double lower_limit, higher_limit;

/////////////static fields/methods for module instances management
	static int init_instance(iot_node_base** instance, uint32_t node_id, json_object *json_cfg) {
		double lower_limit=0, higher_limit=INFINITY; //by default checks that value >=0
		if(json_cfg) {
			json_object *val=NULL;
			if(json_object_object_get_ex(json_cfg, "from", &val)) lower_limit=json_object_get_double(val);
			if(json_object_object_get_ex(json_cfg, "to", &val)) higher_limit=json_object_get_double(val);
		}
		kapi_outlog_info("OPERATOR num_inrange INITED node_id=%u, from=%.10g, to=%.10g", node_id, lower_limit, higher_limit);
		num_inrange_instance *inst=new num_inrange_instance(node_id, lower_limit, higher_limit);
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_node_base* instance) {
		static_cast<num_inrange_instance*>(instance)->unref();
		return 0;
	}
private:
	num_inrange_instance(uint32_t node_id, double lower_limit, double higher_limit) : node_id(node_id),lower_limit(lower_limit),higher_limit(higher_limit) {
	}
	virtual int start(void) override {
		return 0;
	}
	virtual int stop(void) override {
		return 0;
	}

	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) override {
		assert(num_valueinputs==1);
		uint8_t outvi[2]={0,1};
		const iot_datavalue* outv[2];

		const iot_datavalue_numeric* v=iot_datavalue_numeric::cast(valueinputs[0].new_value);

		if(!v) { //undef input or of invalid type, both outputs undef
			outv[0]=outv[1]=NULL;
		} else if(v->get_value()>=lower_limit && v->get_value()<=higher_limit) { //true
			outv[0]=&iot_datavalue_boolean::const_true;
			outv[1]=&iot_datavalue_boolean::const_false;
		} else { //false
			outv[0]=&iot_datavalue_boolean::const_false;
			outv[1]=&iot_datavalue_boolean::const_true;
		}

		int err=kapi_update_outputs(&eventid, 2, outvi, outv);
		if(err) {
			kapi_outlog_error("Cannot update outputs for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
		}

		return 0; // use IOT_ERROR_NOT_READY for debugging async exec
	}
};

//keys_instance* keys_instance::instances_head=NULL;


iot_node_moduleconfig_t IOT_NODE_MODULE_CONF(num_inrange)={
	.version = IOT_VERSION_COMPOSE(1,0,0),
	.cpu_loading = 0,
	.num_devices = 0,
	.num_valueoutputs = 2,
	.num_valueinputs = 1,
	.num_msgoutputs = 0,
	.num_msginputs = 0,
	.is_persistent = 0,
	.is_sync = 1,

	.devcfg={},
	.valueoutput={
		{
			.label = "out",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		},
		{
			.label = "neg",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		}
	},
	.valueinput={
		{
			.label = "in",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_numeric::object
		}
	},
	.msgoutput={
	},
	.msginput={
	},

	//methods
	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &num_inrange_instance::init_instance,
	.deinit_instance = &num_inrange_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};
