#ifndef IOT_CONFIGMODEL_H
#define IOT_CONFIGMODEL_H
//Contains data structures for configuration modelling

#include<stdint.h>
#include<assert.h>

#include <iot_module.h>
#include <kernel/iot_common.h>

//struct iot_config_item_node_t;
//struct iot_configregistry_t;
//struct iot_config_actlist_item_t;
//struct iot_config_item_group_t;

class iot_nodemodel;
class iot_nodelink;
class iot_nodevaluelink;
class iot_nodemsglink;

#include<kernel/iot_configregistry.h>
#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_kernel.h>

//models node with several inputs and several outputs. each input/output is iot_nodelinkmodel object, which is instanciated by some node output
struct iot_nodemodel {
	const iot_iface_node_t *node_iface;  //will be NULL if module is not loaded
	iot_module_item_t *module;     //will be NULL if module is not loaded

	enum : uint8_t {
		NODESTATE_NOMODULE, //model started and is in degenerated state, no required code module found (module==NULL)
		NODESTATE_NOINSTANCE, //model started but no module instance created (modinstlk is false)
		NODESTATE_STARTED, //model started and module instance is active
		NODESTATE_STOPPING, //model stopped and wait module instance to stop to be destroyed
		NODESTATE_RELOADING, //model stopped and wait module instance to stop to restart itself with new configuration
	} state;
	iot_config_node_out_t* errorout_link;
	iot_valueclass_nodeerrorstate errorstate; //errorvalue.value points to this item

	struct {
		iot_config_node_in_t* link;
		iot_valueclass_BASE* instance_value; //last value, received by module instance. i.e. it is current input value from modinstance point of view
	} *curvalueinput; //array of current value input connections matching by index to node_iface->valueinput. contains
										//node_iface->num_valueinputs items if module is loaded, NULL otherwise
	struct {
		iot_config_node_out_t* link; //directs to all consumers of output value (inputs of other nodes)
		iot_valueclass_BASE* instance_value; //last value, set by module instance. i.e. it is current outputted value from modinstance point of view. UPDATED FROM instance thread!!!
		iot_valueclass_BASE* current_value; //last value, set by event processing. i.e. it is current value of output from modelling point of view. UPDATED FROM MAIN thread!!!
	} *curvalueoutput; //array of current output values and connections matching by index to node_iface->valueoutput. contains
						//node_iface->num_valueoutputs items if module is loaded, NULL otherwise


	iot_config_node_out_t* *curmsgoutput; //array of current msg output connections matching by index to node_iface->msgoutput. contains
								//node_iface->num_msgoutputs items if module is loaded, NULL otherwise

	iot_id_t node_id;
	iot_config_item_node_t* cfgitem; //can be NULL when model is being stopped.
	iot_modinstance_locker modinstlk;

	static iot_nodemodel* create(iot_config_item_node_t* cfgitem_);
	static void destroy(iot_nodemodel* node);

	static void on_instance_destroy(iot_nodemodel* node, iot_modinstance_item_t* modinst); //called when node instance is released
	bool stop(void); //instructs model to stop for destruction. returns if destroy() can be called for it right now. otherwise it wait module instance to stop and destroy later

	void assign_inputs(void);

private:
	void try_create_instance(void); //called to recreate node module instance
};

struct iot_modelsignal { //represents signal from specific node's output
	iot_modelsignal* next=NULL; //for list of signals in iot_modelevent

	uint64_t reltime; //monotonic process-scope time to determine relative time between signals
	iot_event_id_t reason_event; //optional event which was the reason of this signal

	iot_id_t node_id;
	uint32_t module_id;
	uint16_t out_index; //index of output according to node's module config
	uint8_t config_ver; //version of module's config to known if out_index is still correct
	bool is_msg;

	union {
		iot_valueclass_BASE* value;
		iot_msgclass_BASE* msg;
	};

	iot_modelsignal(iot_nodemodel *model, uint16_t out_index, uint64_t reltime, iot_valueclass_BASE* value, iot_event_id_t* reason=NULL);
	iot_modelsignal(iot_nodemodel *model, uint16_t out_index, uint64_t reltime, iot_msgclass_BASE* msg, iot_event_id_t* reason=NULL);
	static void release(iot_modelsignal* sig) {
		assert(sig);
		assert(sig->next==NULL); //must be detached
		if(sig->is_msg) iot_release_memblock(sig->msg);
			else iot_release_memblock(sig->value);
		iot_release_memblock(sig);
	}
};

struct iot_modelevent {
	iot_event_id_t id;
	iot_modelevent *qnext, *qprev; //position in events_q
	iot_modelsignal *signals_head, *signals_tail; //added to tail, processed from head

	void init(uint64_t numerator) {
		qnext=qprev=NULL;
		signals_head=signals_tail=NULL;
		id.numerator=numerator;
		id.host_id=iot_current_hostid;
	}
	void add_signal(iot_modelsignal* sig) {
		sig->next=NULL; //??
		if(!signals_tail) {
			assert(signals_head==NULL);
			signals_head=signals_tail=sig;
			return;
		}
		signals_tail->next=sig;
		signals_tail=sig;
	}
	void destroy(void) { //free all signals and other dynamic memory before releasing this event struct or putting it to freelist
		iot_modelsignal* sig;
		while(signals_head) {
			sig=signals_head;
			signals_head=sig->next;
			sig->next=NULL;
			iot_modelsignal::release(sig);
		}
		signals_tail=NULL;
	}
};


#endif //IOT_CONFIGMODEL_H
