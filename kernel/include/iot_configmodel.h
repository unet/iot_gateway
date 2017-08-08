#ifndef IOT_CONFIGMODEL_H
#define IOT_CONFIGMODEL_H
//Contains data structures for configuration modelling

#include<stdint.h>
#include<assert.h>

#include "iot_module.h"
#include "iot_common.h"

//struct iot_config_item_node_t;
//struct iot_configregistry_t;
//struct iot_config_actlist_item_t;
//struct iot_config_item_group_t;

class iot_nodemodel;
class iot_nodelink;
class iot_nodevaluelink;
class iot_nodemsglink;
struct iot_modelsignal;
struct iot_notify_inputsupdate;

#include "iot_configregistry.h"
#include "iot_moduleregistry.h"
#include "iot_kernel.h"

struct iot_modelsignal : public iot_releasable { //represents signal from specific node's output. MUST BE ALLOCATED AS MEMBLOCK
	iot_modelsignal* next=NULL; //for list of signals in iot_modelevent  OR to form list of signals

	uint64_t reltime=0; //monotonic process-scope time to determine relative time between signals
	iot_event_id_t reason_event; //optional event which was the reason of this signal

	iot_config_node_out_t* node_out=NULL; //use as temporary var during event processing to match node by node_id and out_label
	const iot_dataclass_base* data=NULL; //value or message. CAN BE NULL FOR AND ONLY FOR VALUE LINES
	iot_id_t node_id;
	uint32_t module_id;
	alignas(uint32_t) char out_label[IOT_CONFIG_LINKLABEL_MAXLEN+1+1]; //first char is 'm' for msg and 'v' for value output

//	bool is_sync=false; //if true, then reason_event is current event being executed (if hasn't timedout)
//				//if false then says that event is NOT response to sync execution of reason_event (reason_event then can be some old event which became the reason).

	iot_modelsignal(iot_nodemodel *model, const char* out_labeln, uint64_t reltime, const iot_dataclass_base* msgval=NULL, /*bool is_sync=false,*/ const iot_event_id_t* reason=NULL);
	iot_modelsignal(void) : reason_event {}, node_id(0), module_id(0) {
		out_label[0]='\0';
	}
	static void release(iot_modelsignal* &sig) {
		assert(sig);
		sig->iot_modelsignal::releasedata();
		iot_release_memblock(sig);
		sig=NULL;
	}
	void clean(void) { //prepare struct for reuse by zeroing all fields. NOTE! if next is not NULL, all connected signals will be released!!!
		iot_modelsignal::releasedata();
		reltime=0;
		reason_event={};
		node_out=NULL;
		node_id=0;
		module_id=0;
		out_label[0]='\0';
//		is_sync=false;
	}
	virtual void releasedata(void) override { //if ->next is not NULL then ASSUMES every connected item was allocated as memblock and releases it
		if(next) {
			next->releasedata();
			iot_release_memblock(next);
			next=NULL;
		}
		if(data) {
			data->release();
			data=NULL;
		}
	}
};

//represents notification for node instance about update of inputs due to event processing
struct iot_notify_inputsupdate : public iot_releasable {
	iot_event_id_t reason_event={};
	iot_modelsignal* prealloc_signals=NULL; //list of prealloced iot_modelsignal structs
	uint16_t numitems=0, numalloced;
	struct {
		int16_t real_index;
		const iot_dataclass_base* data; //value or message. CAN BE NULL FOR AND ONLY FOR VALUE LINES
	} item[];

	iot_notify_inputsupdate(uint16_t numalloced) : numalloced(numalloced) {
	}


	size_t get_size(void) const {
		return sizeof(*this)+numalloced*sizeof(item[0]);
	}
	static size_t calc_size(uint16_t num) { //calculates necessary memory to keep num items
		return sizeof(iot_notify_inputsupdate)+num*sizeof(item[0]);
	}

	virtual void releasedata(void) override {
		if(prealloc_signals) {
			prealloc_signals->release(prealloc_signals);
			prealloc_signals=NULL;
		}
		for(int i=0;i<numitems;i++)
			if(item[i].data) {
				item[i].data->release();
				item[i].data=NULL;
			}
		numitems=0;
	}
};


//models node with several inputs and several outputs. each input/output is iot_nodelinkmodel object, which is instanciated by some node output
struct iot_nodemodel {
	const iot_iface_node_t *node_iface;  //will be NULL if module is not loaded
	iot_module_item_t *module;     //will be NULL if module is not loaded

	enum : uint8_t {
		NODESTATE_NOMODULE, //model started and is in degenerated state, no required code module found (module==NULL)
		NODESTATE_NOINSTANCE, //model started but no module instance created (modinstlk is false)
		NODESTATE_WAITSTART, //model started and instance created but no start notification arrived yet
		NODESTATE_STARTED, //model started and module instance is active
		NODESTATE_STOPPING, //model stopped and waits module instance to stop to be destroyed
		NODESTATE_RELOADING, //model stopped and waits module instance to stop to restart itself with new configuration
	} state;

	struct {
		iot_config_node_out_t* link;
		iot_valueclass_nodeerrorstate* instance_value;
	} erroutput;

	struct {
		iot_config_node_in_t* link;
		const iot_valueclass_BASE* instance_value; //last value, received by module instance. i.e. it is current input value from modinstance point of view
	} *curvalueinput; //array of current value input connections matching by index to node_iface->valueinput. contains
										//node_iface->num_valueinputs items if module is loaded, NULL otherwise
	struct {
		iot_config_node_out_t* link; //directs to all consumers of output value (inputs of other nodes)
		const iot_valueclass_BASE* instance_value; //last value, set by module instance. i.e. it is current outputted value from modinstance point of view. UPDATED FROM instance thread!!!
	} *curvalueoutput; //array of current output values and connections matching by index to node_iface->valueoutput. contains
						//node_iface->num_valueoutputs items if module is loaded, NULL otherwise


	struct {
		iot_config_node_in_t* link;
	} *curmsginput; //array of current msg input connections matching by index to node_iface->msginput. contains
								//node_iface->num_msginputs items if module is loaded, NULL otherwise

	struct {
		iot_config_node_out_t* link;
	} *curmsgoutput; //array of current msg output connections matching by index to node_iface->msgoutput. contains
								//node_iface->num_msgoutputs items if module is loaded, NULL otherwise

	iot_id_t node_id;
	iot_config_item_node_t* cfgitem; //can be NULL when model is being stopped.
	iot_modinstance_locker modinstlk;

	struct {
		iot_event_id_t event_id;
		iot_modelsignal* prealloc_signals;
		iot_threadmsg_t* msg; //in active struct msg can be NULL only if 
		iot_modelsignal* result_signals; //will be filled after call to do_update_outputs in simple sync mode (is_sync==2)

		void init(iot_notify_inputsupdate* notifyupdate, iot_threadmsg_t* execmsg) {
			assert(!active());

			event_id=notifyupdate->reason_event;
			assert(event_id.numerator!=0);
			assert(execmsg!=NULL);
			msg=execmsg;
			prealloc_signals=notifyupdate->prealloc_signals;
			notifyupdate->prealloc_signals=NULL;
			result_signals=(iot_modelsignal*)(intptr_t(-1));
		}
		bool result_set(void) {
			if(!active()) return false;
			return result_signals==(iot_modelsignal*)(intptr_t(-1)) ? false : true;
		}
		void clear_result(void) { //must be called to clear result status
			if(!result_set()) return;
			result_signals=(iot_modelsignal*)(intptr_t(-1));
		}
		bool active(void) { //true if processing of sync execution is in progress. in simple sync mode result_set() says if result was assigned (by do_update_outputs())
			if(!event_id.numerator) return false;
			assert(msg!=NULL);
			return true;
		}
		void clear(void) {
			if(!event_id.numerator) return;
			assert(result_signals==(iot_modelsignal*)(intptr_t(-1)));
			if(msg) {iot_release_msg(msg); msg=NULL;}
			if(prealloc_signals) iot_modelsignal::release(prealloc_signals);
			event_id.numerator=0;
		}
	} syncexec; // state of sync execution. modified from instance thread!!!

	bool links_valid; //flag that all links in curvalueinput, curvalueoutput, curmsginput and curmsgoutput are valid (successfully allocated if necessary)

	uint8_t is_sync; //0 - async (no is_sync flag in node iface config), 1 - sync (there is is_sync flag but simple mode impossible or was reset),
					//2 - simple sync (no instance started yet or it is started and satisfies simple sync mode requirements)


public:
	static iot_nodemodel* create(iot_config_item_node_t* cfgitem_);
	static void destroy(iot_nodemodel* node);

	static void on_instance_destroy(iot_nodemodel* node, iot_modinstance_item_t* modinst); //called when node instance is released
	void on_instance_start(iot_modinstance_item_t* modinst); //called when node instance is started
	bool stop(void); //instructs model to stop for destruction. returns if destroy() can be called for it right now. otherwise it wait module instance to stop and destroy later

	bool validate_links(void); //check that every input/output has corresponding in/out link in cfgitem, allocate link if necessary
	void assign_inputs(void);

	bool execute(bool forceasync, iot_threadmsg_t *&msg, iot_modelsignal *&outsignals); //main thread
	bool do_execute(bool isasync, iot_threadmsg_t *&msg, iot_modelsignal *&outsignals); //instance thread
	int do_update_outputs(const iot_event_id_t *reason_eventid, uint8_t num_values, const uint8_t *valueout_indexes, const iot_valueclass_BASE** values, uint8_t num_msgs, const uint8_t *msgout_indexes, const iot_msgclass_BASE** msgs);
	const iot_valueclass_BASE* get_outputvalue(uint8_t index);

private:
	void try_create_instance(void); //called to recreate node module instance
};


struct iot_modelnegsignal { //reply to delayed sync execution telling 'no updates'
	iot_event_id_t event_id;
	iot_id_t node_id;
};

struct iot_modelevent {
	iot_event_id_t id;
	iot_modelevent *qnext, *qprev; //position in events_qhead/events_qtail
	iot_modelevent *waiters_head, //list of events waiting for this event to finish
		*wnext; //position in waiters_head if this event waits (is_blocked will be true). ONLY one event can be waited for!!!

	iot_modelsignal *signals_head, *signals_tail; //added to tail (to preserve natural order of signals), processed from head
	iot_config_item_node_t *blocked_nodes_head; //list of nodes involved in event processing
	iot_config_item_node_t *initial_nodes_head; //list of initial nodes of current modelling step
	iot_config_item_node_t *waitexec_head; //list of initial nodes of current modelling step
	uint16_t step;
	uint16_t minpathlen; //keeps calculated minpathlen is processing was interrupted by lack of memory
	bool is_error; //flag that this event started by error signal(s)
	bool is_blocked; //flag that whis event waits for some another event to finish
	enum : uint8_t {
		CONT_NONE=0,
		CONT_NOMEMORY=1, //no memory for sync nodes execution
		CONT_WAITEXEC=2,
		CONT_NOMEMORYASYNC=3, //no memory for async nodes execution
		CONT_NOMEMORYSYNCWO=4, //no memory for sync nodes without outputs execution
	} continue_phase;

	void init(uint64_t numerator) {
		qnext=qprev=waiters_head=wnext=NULL;
		signals_head=signals_tail=NULL;
		waitexec_head=NULL;
		blocked_nodes_head=NULL;
		initial_nodes_head=NULL;
		id.numerator=numerator;
		id.host_id=iot_current_hostid;
		step=0;
		minpathlen=0;
		is_error=is_blocked=false;
		continue_phase=CONT_NONE;
	}
	void wait_for(iot_modelevent* ev) { //add this event to list of waiters for event ev (to ev->waiters_head)
		if(is_blocked) { //ONLY one event can be waited for!!!
			assert(false);
			return;
		}
		ULINKLIST_INSERTHEAD(this, ev->waiters_head, wnext);
	}
	void unblock_waiters(void) {
		while(waiters_head) ULINKLIST_REMOVEHEAD(waiters_head, wnext);
	}
	void add_signals(iot_modelsignal* sig) { //add one or several signals. sig->next MUST BE NULL when adding single signal
		if(!signals_tail) {
			assert(signals_head==NULL);
			signals_head=sig;
		} else {
			signals_tail->next=sig;
		}
		while(sig->next) sig=sig->next; //find last signal
		signals_tail=sig;
	}
	iot_modelsignal* get_signal(void) { //get head signal and remove it from list
		if(!signals_head) return NULL;
		iot_modelsignal* sig=signals_head;
		signals_head=sig->next;
		sig->next=NULL;
		if(!signals_head) signals_tail=NULL;
		return sig;
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
		unblock_waiters();
	}
};


#endif //IOT_CONFIGMODEL_H
