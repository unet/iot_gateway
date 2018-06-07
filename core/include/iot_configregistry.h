#ifndef IOT_CONFIGREGISTRY_H
#define IOT_CONFIGREGISTRY_H
//Contains data structures representing user configuration

#include<stdint.h>
#include<assert.h>

#include "json.h"

#define IOTCONFIG_NAME "config.json"

#include "iot_core.h"
#include "mhbtree.h"



struct iot_config_item_node_t;
struct iot_configregistry_t;
struct iot_config_item_group_t;
struct iot_config_node_in_t;
struct iot_config_node_out_t;
struct iot_config_item_host_t;
struct iot_config_item_link_t;


//#define IOT_MAX_EXECUTOR_DEVICES 1
//#define IOT_MAX_ACTIVATOR_INPUTS 3

//hard limit of number of emplicit modes in each group of config
#define IOT_CONFIG_MAX_MODES_PER_GROUP 16

//real output index value to reference implicit error output
#define IOT_CONFIG_NODE_ERROUT_INDEX 255


//#include "iot_deviceregistry.h"
#include "iot_moduleregistry.h"
#include "iot_configmodel.h"
#include "iot_netproto_iotgw.h"



//keeps current mode for each group
struct iot_config_item_group_t {
	iot_config_item_group_t *next, *prev;
	bool is_del;
	uint8_t num_modes; //number of items in modes array. default mode with same ID as group is implicit and not counted here

	iot_id_t group_id, activemode_id; //both zero for persistent nodes. mode_id can be zero for temporary nodes (which are group-common)
	int64_t modes_modtime;
	int64_t active_set;
	iot_id_t modes[IOT_CONFIG_MAX_MODES_PER_GROUP]; //array with list of possible modes not including default mode
};


struct iot_config_item_rule_t {
	iot_id_t rule_id;

	iot_id_t mode_id; //can be zero for links from group-common rules
	iot_config_item_group_t *group_item; //must point to structure corresponding to group_id

	iot_modelevent* blockedby=NULL; //non-NULL value if this rule is involved in corresponding event processing. event must be present in config_registry->current_events_head
	iot_config_item_rule_t* blocked_next=NULL; //if blockedby is set, then position in blockedby->blocked_rules_head list

	bool is_del;

	bool is_active(void) const {
		return group_item && (!mode_id || group_item->activemode_id==mode_id);
	}

	bool is_blocked(void) const {return blockedby!=NULL;}
	bool is_blockedby(const iot_modelevent *ev) const {
		assert(ev!=NULL);
		return blockedby==ev;
	}
	void set_blocked(iot_modelevent *ev) { //marks node as blocked for processing of specific event. node must be unblocked
		assert(ev!=NULL && blockedby==NULL);
		blockedby=ev;
		ULINKLIST_INSERTHEAD(this, ev->blocked_rules_head, blocked_next);
	}
	void clear_blocked(void) {
		assert(blockedby!=NULL);
		ULINKLIST_REMOVE_NOPREV(this, blockedby->blocked_rules_head, blocked_next);
		blockedby=NULL;
	}
	void clear_blocked_athead(iot_modelevent *ev) { //must be blocked by ev. quicker version to remove head item from ev->blocked_rules_head
		assert(blockedby==ev && this==ev->blocked_rules_head);
		ULINKLIST_REMOVEHEAD(ev->blocked_rules_head, blocked_next);
		blockedby=NULL;
	}

};


struct iot_config_node_in_t {
	iot_config_node_in_t* next=NULL; //next pointer in node->inputs list
	iot_config_item_node_t *node; //parent node
	iot_config_item_link_t *outs_head=NULL; //array of outputs connected to current input, next_output field is used as next reference

	union {
		const iot_datavalue* current_value=NULL; //last value, set by event processing. i.e. it is current value of input from modelling point of view. Normally equal to
											//current single corresponding output's value. UPDATED FROM MAIN thread!!!
		const iot_datavalue* inject_msg; //can be used by customer to inject message even to unconnected msg input (which has no links) or without binding to specific source link
	};

	int16_t real_index=-1; //real index of node input (according to module config) or -1 if not found (MUST BE INITED TO -1)
	uint16_t pathlen=0; //used during potential signal path modelling

	bool is_connected=false; //shows if there is at least one valid link in outs_head list
	bool is_undelivered=false; //current input value must be sent to node instance (or host). sending can be delayed due to back reference, another host (or thread) or lack of memory
	bool fixed_value=false; //true value means that current_value IS NOT updated by signals from outputs. it is fixed by customer for debugging reasons
	char label[IOT_CONFIG_LINKLABEL_MAXLEN+1+1]={}; //label with type prefix

	iot_config_node_in_t(iot_config_item_node_t* node, const char* label_=NULL) : node(node) {
		if(label) strcpy(label, label_);
	}

	bool is_msg(void) const {
		return label[0]=='m';
	}
	bool is_value(void) const {
		return !is_msg();
	}
};

struct iot_config_node_out_t {
	iot_config_node_out_t* next=NULL; //next pointer in node->outputs list
	iot_config_item_node_t *node; //parent node
	iot_config_item_link_t *ins_head=NULL; //array of inputs connected to current output, next_input field is used as next reference

	const iot_datavalue* current_value=NULL; //last value, set by event processing. i.e. it is current value of output from modelling point of view. UPDATED FROM MAIN thread!!!
	iot_modelsignal* prealloc_signal=NULL;

	int16_t real_index=-1; //real index of node output (according to module config) or -1 if not found (MUST BE INITED TO -1)
	bool is_connected=false; //shows if there is at least one valid link in ins_head list

	char label[IOT_CONFIG_LINKLABEL_MAXLEN+1+1]={}; //label with type prefix

	iot_config_node_out_t(iot_config_item_node_t* node, const char* label_=NULL, const iot_datavalue* current_value=NULL) : node(node), current_value(current_value) {
		if(label) strcpy(label, label_);
	}

	bool is_msg(void) const {
		return label[0]=='m';
	}
	bool is_value(void) const {
		return !is_msg();
	}
};



struct iot_config_node_dev_t {
	iot_config_node_dev_t* next; //next pointer in parent node dev list
	char label[IOT_CONFIG_DEVLABEL_MAXLEN+1]; //up to 7 chars
	uint8_t numidents, maxidents; //actual number of items in idents, number of allocated items
	iot_hwdev_ident_buffered idents[]; //array of hwdev filters set by user. internal ident[i].dev iot_hwdev_localident_t structure or host can be a template
};

//represents user configuration node item
struct iot_config_item_node_t {
	iot_config_item_host_t* host=NULL;
	iot_id_t node_id;
	uint32_t module_id=0;
	uint32_t cfg_id=0; //node config number when props were updated last time

	iot_config_item_rule_t* rule_item=NULL; //parent rule. can be NULL if node is persistent or temporary but rule-independent (in particular rules can be totally unused)

//	iot_id_t mode_id=0; //zero for persistent nodes. can be zero for temporary nodes (which are group-common)
//	iot_config_item_group_t *group_item=NULL; //must point to structure corresponding to group_id for temporary nodes, NULL for persistent


	iot_config_node_dev_t *dev=NULL; //linked list of device filters

	iot_config_node_in_t *inputs=NULL; //linked list of input link connections

	iot_config_node_out_t *outputs=NULL; //linked list of output link connections
	iot_config_node_out_t erroutput; //error output link connections

	json_object *json_config=NULL;

	iot_nodemodel* nodemodel=NULL; //always NULL for nodes of other hosts

	iot_threadmsg_t *prealloc_execmsg=NULL;


	iot_modelevent* blockedby=NULL; //non-NULL value if this node is involved in corresponding event processing. event must be present in config_registry->current_events_head
	iot_config_item_node_t* blocked_next=NULL; //if blockedby is set, then position in blockedby->blocked_nodes_head list
	iot_config_item_node_t* tmp_blocked_next=NULL; //if tmp_blocked is true, then position in new_event->tmp_blocked_nodes_head or new_errevent->tmp_blocked_nodes_head list
	iot_config_item_node_t* initial_next=NULL, *initial_prev=NULL; //for blocked node can held position in blockedby->initial_nodes_head list

	iot_config_item_node_t* needexec_next=NULL, *needexec_prev=NULL; //position in configregistry->needexec_head or blockedby->needexec_next of nodes which need recount
												//of outputs. at least one input must be in undelivered state or execution will assert

	iot_config_item_node_t* waitexec_next=NULL, *waitexec_prev=NULL; //for sync node being executed in complex mode helds position in blockedby->waitexec_head list

	uint16_t maxpathlen=0;

	uint8_t config_ver=0;
	uint8_t tmp_blocked=0; //used when forming new event from input signals to mark nodes, which would be blocked by new_event or new_errevent
							//value 1 means that only specific output or outputs are blocked (new_event->signals_head must be traversed to know which outputs)
							//value >1 means that whole node is blocked

	bool is_del=false;
	bool outputs_connected=false; //flag that there is at least one is_connected output in outputs list


//	Props used during modelling
	bool probing_mark=false; //use during recursive model traversing to mark already checked node. ALWAYS MUST BE CLEARED JUST AFTER traversing
	bool acted=false; //for blocked node shows if it was already executed during event processing
	bool pathset=false; //flag that some in has assigned pathlen (it could be temporary assignment for non-initial node). necessary just to optimize cleaning pathlen


	iot_config_item_node_t(iot_id_t node_id) : node_id(node_id), erroutput (this, "v" IOT_CONFIG_NODE_ERROUT_LABEL, &iot_datavalue_nodeerrorstate::const_noinst) {
	}

	bool needs_exec(void) {return needexec_prev!=NULL;}
	void clear_needexec(void) {
		assert(needexec_prev!=NULL);
		outlog_debug_modelling("Node %u cleared execution mark", node_id);
		BILINKLIST_REMOVE(this, needexec_next, needexec_prev);
	}

	bool is_initial(void) {return initial_prev!=NULL;}
	void set_initial(iot_modelevent *ev) {
		assert(initial_prev==NULL);
		BILINKLIST_INSERTHEAD(this, ev->initial_nodes_head, initial_next, initial_prev);
	}
	void clear_initial(void) {
		assert(initial_prev!=NULL);
		BILINKLIST_REMOVE(this, initial_next, initial_prev);
	}

	bool is_waitexec(void) {return waitexec_prev!=NULL;}
	void set_waitexec(iot_modelevent *ev) {
		assert(waitexec_prev==NULL);
		BILINKLIST_INSERTHEAD(this, ev->waitexec_head, waitexec_next, waitexec_prev);
	}
	void clear_waitexec(void) {
		assert(waitexec_prev!=NULL);
		BILINKLIST_REMOVE(this, waitexec_next, waitexec_prev);
	}

	bool is_blocked(void) const {return blockedby!=NULL;}
	bool is_blockedby(const iot_modelevent *ev) const {
		assert(ev!=NULL);
		return blockedby==ev;
	}
	void set_blocked(iot_modelevent *ev) { //marks node as blocked for processing of specific event. node must be unblocked
		assert(ev!=NULL && blockedby==NULL);
		blockedby=ev;
		ULINKLIST_INSERTHEAD(this, ev->blocked_nodes_head, blocked_next);
		if(rule_item && !rule_item->is_blockedby(ev)) { //rule, when present, must be either blocked by same event or unblocked
			assert(!rule_item->is_blocked());
			rule_item->set_blocked(ev);
		}
	}
	void clear_blocked(void) { //will not unblock rules!!!
		assert(blockedby!=NULL);
		ULINKLIST_REMOVE_NOPREV(this, blockedby->blocked_nodes_head, blocked_next);
		blockedby=NULL;
	}
	void clear_blocked_athead(iot_modelevent *ev) { //must be blocked by ev. quicker version to remove head item from ev->blocked_nodes_head
		assert(blockedby==ev && this==ev->blocked_nodes_head);
		ULINKLIST_REMOVEHEAD(ev->blocked_nodes_head, blocked_next);
		blockedby=NULL;
	}

	bool is_tmpblocked(void) const {return tmp_blocked!=0;}
	bool is_tmpblocked_outs(void) const {return tmp_blocked==1;}
	void set_tmpblocked(iot_modelevent *ev) { //marks node as temporary fully blocked for specific event being formed. node must be unblocked
		assert(ev!=NULL && !tmp_blocked);
		tmp_blocked=2;
		ULINKLIST_INSERTHEAD(this, ev->tmp_blocked_nodes_head, tmp_blocked_next);
	}
	void set_tmpblocked_outs(iot_modelevent *ev) { //marks node as with temporary blocked outs for specific event being formed. node must be unblocked
		assert(ev!=NULL && !tmp_blocked);
		tmp_blocked=1;
		ULINKLIST_INSERTHEAD(this, ev->tmp_blocked_nodes_head, tmp_blocked_next);
	}
	void clear_tmpblocked(iot_modelevent *ev) { //must be tmp blocked by ev
		assert(tmp_blocked);
		ULINKLIST_REMOVE_NOPREV(this, ev->tmp_blocked_nodes_head, tmp_blocked_next);
		tmp_blocked=0;
	}
	void clear_tmpblocked_athead(iot_modelevent *ev) { //must be tmp blocked by ev. quicker version to remove head item from ev->tmp_blocked_nodes_head
		assert(tmp_blocked && this==ev->tmp_blocked_nodes_head);
		ULINKLIST_REMOVEHEAD(ev->tmp_blocked_nodes_head, tmp_blocked_next);
		tmp_blocked=0;
	}

	bool prepare_execute(bool forceasync=false); //must be called to preallocate memory before execute(). returns false on memory error, true on success BUT needexec and initial flags can be cleared

	void execute(bool forceasync=false);
	bool is_sync(void) { //says if this node can give output signals synchronously
		return !nodemodel || nodemodel->node_iface->is_sync;
	}

	iot_config_node_out_t* find_output(const char* label) { //find output by label with type prefix
		for(iot_config_node_out_t* out=outputs; out; out=out->next)
			if(strcmp(out->label, label)==0) return out;
		return NULL;
	}
};


struct iot_config_item_link_t {
	iotlink_id_t link_id; //necessary when traversing index

	iot_config_item_link_t *next_input, *next_output;
	iot_config_node_in_t* in;
	iot_config_node_out_t* out;

	iot_config_item_rule_t* rule; //parent rule. can be NULL if link is rule-independent (in particular rules can be totally unused)

//	iot_id_t mode_id; //can be zero for links from group-common rules
//	iot_config_item_group_t *group_item; //must point to structure corresponding to group_id

	const iot_datavalue* current_msg;//can be assigned for valid link only
	const iot_datavalue* prev_msg; //if new msg comes when current_msg is not NULL, value from current_msg goes here. can be assigned for valid link only

	uint16_t pathlen=0; //used during potential signal path modelling FOR MSG LINKS ONLY

	bool is_undelivered=false; //current and prev msgs must be sent to node instance (or host). sending can be delayed due to back reference, another host (or thread) or lack of memory
	bool is_del;
private:
	bool is_valid; //shows if link can transfer signals, i.e. that both in and out are present and data types do match AND LINK IS ACTIVE

public:
	bool valid(void) {return is_valid;}
	void validate(void) {
		if(is_valid) return;
		assert(in && out && in->is_msg()==out->is_msg());
		assert(!rule || rule->is_active());
		is_valid=true;
		in->is_connected=out->is_connected=true;
		if(out!=&out->node->erroutput) out->node->outputs_connected=true;
	}
	void invalidate(void) {
		if(!is_valid) return;
		if(prev_msg) {prev_msg->release(); prev_msg=NULL;}
		if(current_msg) {current_msg->release(); current_msg=NULL;}
		is_valid=false;
		iot_config_item_link_t* link;
		if(in) {
			for(link=in->outs_head; link; link=link->next_output) if(link->valid()) break;
			if(!link) in->is_connected=false;
		}
		if(out) {
			for(link=out->ins_head; link; link=link->next_input) if(link->valid()) break;
			if(!link) {
				out->is_connected=false;
				if(out!=&out->node->erroutput) {
					//check node has at least one connected output
					iot_config_node_out_t* curout;
					for(curout=out->node->outputs; curout; curout=curout->next) if(curout->is_connected) break;
					if(!curout) out->node->outputs_connected=false;
				}
			}
		}
	}
	void check_validity(void) {
		if(!in || !out || in->is_msg()!=out->is_msg() || (rule && !rule->is_active())) {invalidate();return;}
		if(in->real_index>=0 && out->real_index>=0 && out->node->nodemodel && in->node->nodemodel) { //can compare data types
			if(in->is_value()) {
				if(!out->node->nodemodel->node_iface->valueoutput[out->real_index].is_compatible(&in->node->nodemodel->node_iface->valueinput[in->real_index])) {invalidate();return;}
			}
			else {//is msg
				if(!out->node->nodemodel->node_iface->msgoutput[out->real_index].is_compatible(&in->node->nodemodel->node_iface->msginput[in->real_index])) {invalidate();return;}
			}
		}
		validate();
	}

};


//represents configuration host item
struct iot_config_item_host_t {
	iot_config_item_host_t *next=NULL, *prev=NULL; //for list in configregistry->hosts_head;
	bool is_del=false;
	bool is_current;

	const iot_hostid_t host_id;
	uint32_t cfg_id=0; //hosts config number when props were updated last time
	uint32_t index; //relative index of this host used to represent set of hosts like a bitmap. all indexes are changed if set of hosts changes. range if [0; iot_configregistry_t::num_hosts-1]
	iot_objref_ptr<iot_peer> peer;

	json_object* manual_connect_params=NULL; //manually specified by user ways to connect to host, common for all other hosts

	iot_config_item_host_t(const iot_config_item_host_t&) = delete;

	iot_config_item_host_t(bool is_current, iot_hostid_t host_id, const iot_objref_ptr<iot_peer> &peer_): is_current(is_current), host_id(host_id), peer(peer_) {
		assert(host_id!=0);
		assert((peer!=NULL && !is_current) || (peer==NULL && is_current));
	}

	~iot_config_item_host_t(void) {
		assert(next==NULL && prev==NULL);
		if(manual_connect_params) {
			json_object_put(manual_connect_params);
			manual_connect_params=NULL;
		}
	}

	int update_from_json(json_object *obj);
};




class iot_configregistry_t {
public:
	iot_config_item_host_t *current_host=NULL;
private:
	iot_gwinstance* gwinst=NULL;
	iot_config_item_group_t *groups_head=NULL;
	iot_config_item_host_t *hosts_head=NULL;
	mutable iot_spinrwlock hosts_lock;

	MemHBTree<iot_config_item_node_t*, iot_id_t, 0, 0, true, 10> nodes_index;
	MemHBTree<iot_config_item_link_t*, iotlink_id_t, 0, 0, true, 10> links_index;
	MemHBTree<iot_config_item_rule_t*, iot_id_t, 0, 0, true, 10> rules_index;

	uint32_t nodecfg_id=0, hostcfg_id=0, modecfg_id=0, owncfg_modtime=0; //current numbers of config parts
	iot_hostid_t logger_host_id=0;
	volatile std::atomic<uint32_t> num_hosts={0}; //current number of entries in hosts_head list. (will become unnecessary if linked list will be changed to a tree)

	iot_modelevent eventsbuf[100]; //preallocated model event structs
	iot_modelevent *events_freelist=NULL; //only ->qnext is used for list iterating

	iot_modelevent *events_qhead=NULL, *events_qtail=NULL; //queue of commited events. processed from head, added to tail. uses qnext and qprev item fields

	iot_modelevent *new_event=NULL; //uncommited normal event, new signals are added to it, waits for commit_signals or large reltime difference of next signal
	bitmap32* new_event_hbitmap=NULL; //memblock-allocated bitmap object to store involved hosts for new_event
	iot_modelevent *new_errevent=NULL; //uncommited error signals event, new signals are added to it, waits for commit_signals or large reltime difference of next signal
	bitmap32* new_errevent_hbitmap=NULL; //memblock-allocated bitmap object to store involved hosts for new_errevent

	iot_modelevent *current_events_head=NULL; //list of events being currently processed in parallel. qnext and qprev fields are used, but no tail

//	uint64_t last_eventid_numerator=0; //last used numerator. it is timestamp with microseconds decremented by 1e15 mcs and multiplied by 1000. least 3 
//									//decimal digits are just incremented when number of microsends is not changed (i.e. events appear during same microsecond)

//	iot_threadmsg_t *freemsg_head=NULL; //list of free msg structs allocated as memblocks
//	uint32_t num_freemsgs=0; //number of items in freemsg_head

	uv_check_t events_executor={};
	iot_config_item_node_t* needexec_head=NULL; //list of nodes whose output must be recalculated. uses fields needexec_next/prev

	bool host_indexes_invalid=false; //will be set to true inside host_delete after removing some host to mark that all host indexes must be recalculated in host_add
	bool inited=false; //flag that one-time init was done in start_config

public:
	iot_configregistry_t(iot_gwinstance* gwinst_) : gwinst(gwinst_), nodes_index(512, 1), links_index(512, 1) {
		assert(gwinst!=NULL);

		//fill events_freelist from preallocated structs
		memset(eventsbuf, 0, sizeof(eventsbuf));
		for(unsigned i=0;i<sizeof(eventsbuf)/sizeof(eventsbuf[0]);i++)
			ULINKLIST_INSERTHEAD(&eventsbuf[i], events_freelist, qnext);
	}

	static json_object* read_jsonfile(const char* dir, const char* relpath, const char *name, bool allow_empty=false); //main thread
	int load_config(json_object* cfg, bool skiphosts);
	int load_hosts_config(json_object* cfg);

	//called ones on startup
	void start_config(void); //main thread

	void free_config(void); //main thread
	void clean_config(void); //main thread. free all config items marked for deletion

	uint32_t get_num_hosts(void) {
		return num_hosts.load(std::memory_order_relaxed);
	}
//host management
	void host_add(iot_config_item_host_t* h) {
		assert(uv_thread_self()==main_thread);
		assert(!new_event && !new_errevent); //modelling must not be in stage of event forming
		assert(h->next==NULL && h->prev==NULL);

		if(host_indexes_invalid) { //need to reassign host indexes
			uint32_t idx=0;
			for(iot_config_item_host_t* h=hosts_head; h; h=h->next, idx++) h->index=idx;
			host_indexes_invalid=false;
		}

		//get axclusive lock on hosts list
		hosts_lock.wrlock();

		BILINKLIST_INSERTHEAD(h, hosts_head, next, prev);
		h->index=num_hosts.fetch_add(1, std::memory_order_relaxed);

		hosts_lock.wrunlock();

		if(h->host_id==gwinst->this_hostid) {
			assert(current_host==NULL);
			current_host=h;
		}
		if(new_event_hbitmap && !new_event_hbitmap->has_space(get_num_hosts())) {
			iot_release_memblock(new_event_hbitmap);
			new_event_hbitmap=NULL;
		}
		if(new_errevent_hbitmap && !new_errevent_hbitmap->has_space(get_num_hosts())) {
			iot_release_memblock(new_errevent_hbitmap);
			new_errevent_hbitmap=NULL;
		}
	}
	iot_config_item_host_t* host_find(iot_hostid_t hostid) const { //thread safe
		iot_config_item_host_t* h;
		if(uv_thread_self()==main_thread) { //no lock necessary as only main thread can modify list
			for(h=hosts_head; h; h=h->next)
				if(h->host_id==hostid) return h;
			return NULL;
		}
		hosts_lock.rdlock();

		//do shared work
		for(h=hosts_head; h; h=h->next) if(h->host_id==hostid) break;
		//release lock
		hosts_lock.rdunlock();

		return h;
	}
	int host_update(iot_hostid_t hostid, json_object* obj);
	void hosts_clean(void) { //remove hosts with is_del flag
		assert(uv_thread_self()==main_thread);
		//get exclusive lock on hosts list
		bool locked=false;

		iot_config_item_host_t* cur_host=hosts_head;
		while(cur_host) {
			iot_config_item_host_t* next=cur_host->next;
			if(cur_host->is_del) {
				if(!locked) { //lock when first deleted host is found
					locked=true;
					hosts_lock.wrlock();
				}
				host_delete(cur_host, true);
			}
			cur_host=next;
		}

		//release lock
		if(locked) hosts_lock.wrunlock();

		if(new_event_hbitmap) {
			iot_release_memblock(new_event_hbitmap);
			new_event_hbitmap=NULL;
		}
		if(new_errevent_hbitmap) {
			iot_release_memblock(new_errevent_hbitmap);
			new_errevent_hbitmap=NULL;
		}

	}
	void host_delete(iot_config_item_host_t* h, bool skiplock=false) {
		assert(uv_thread_self()==main_thread);
		assert(!new_event && !new_errevent); //modelling must not be in stage of event forming

		if(!skiplock) {
			//get axclusive lock on hosts list
			hosts_lock.wrlock();
		}

		//do exclusive work
		BILINKLIST_REMOVE_NOCL(h, next, prev); //??? is there a sense with regard to locking? TODO. check usage of this
		assert(num_hosts>0);
		num_hosts.fetch_sub(1, std::memory_order_relaxed);

		if(!skiplock) {
			//release lock
			hosts_lock.wrunlock();
		}

		if(h==current_host) current_host=NULL;
		h->next=h->prev=NULL;

		h->~iot_config_item_host_t();
		iot_release_memblock(h);
		host_indexes_invalid=true;
	}
	void hosts_markdel(void) const { //set is_del mark for all hosts
		assert(uv_thread_self()==main_thread);
		iot_config_item_host_t* h=hosts_head;
		while(h) {
			h->is_del=true;
			h=h->next;
		}
	}
	iot_config_item_host_t* hosts_listhead(void) {
		assert(uv_thread_self()==main_thread);
		return hosts_head;
	}

//group/mode management
	iot_config_item_group_t* group_find(iot_id_t groupid) {
		iot_config_item_group_t* h=groups_head;
		while(h) {
			if(h->group_id==groupid) return h;
			h=h->next;
		}
		return NULL;
	}
	int group_update(iot_id_t groupid, json_object* obj);
	void groups_markdel(void) { //set is_del mark for all groups
		iot_config_item_group_t* h=groups_head;
		while(h) {
			h->is_del=true;
			h=h->next;
		}
	}

//rule management
	iot_config_item_rule_t* rule_find(iot_id_t ruleid) {
		iot_config_item_rule_t** rule=NULL;
		int res=rules_index.find(ruleid, &rule);
		assert(res>=0);
		if(res<=0) return NULL;
		assert(*rule!=NULL);
		return *rule;
	}
	int rule_update(iot_id_t ruleid, json_object* obj);
	void rules_markdel(void) { //set is_del mark for all groups
		iot_config_item_rule_t** rule=NULL;
		decltype(rules_index)::treepath path;
		int res=rules_index.get_first(NULL, &rule, path);
		assert(res>=0);
		while(res==1) {
			(*rule)->is_del=true;
			res=rules_index.get_next(NULL, &rule, path);
		}
	}

//link management
	iot_config_item_link_t* link_find(iotlink_id_t linkid) {
		iot_config_item_link_t** lnk=NULL;
		int res=links_index.find(linkid, &lnk);
		assert(res>=0);
		if(res<=0) return NULL;
		assert(*lnk!=NULL);
		return *lnk;
	}
	int link_update(iotlink_id_t linkid, json_object* obj);
	void links_markdel(void) { //set is_del mark for all groups
		iot_config_item_link_t** lnk=NULL;
		decltype(links_index)::treepath path;
		int res=links_index.get_first(NULL, &lnk, path);
		assert(res>=0);
		while(res==1) {
			(*lnk)->is_del=true;
			res=links_index.get_next(NULL, &lnk, path);
		}
	}


//node management
	iot_config_item_node_t* node_find(iot_id_t nodeid) {
		iot_config_item_node_t** lnk=NULL;
		int res=nodes_index.find(nodeid, &lnk);
		assert(res>=0);
		if(res<=0) return NULL;
		assert(*lnk!=NULL);
		return *lnk;
	}
	int node_update(iot_id_t nodeid, json_object* obj);
	void nodes_markdel(void) { //set is_del mark for all groups
		iot_config_item_node_t** node=NULL;
		decltype(nodes_index)::treepath path;
		int res=nodes_index.get_first(NULL, &node, path);
		assert(res>=0);
		while(res==1) {
			(*node)->is_del=true;
			res=nodes_index.get_next(NULL, &node, path);
		}
	}



//Modelling
	void inject_negative_signal(iot_modelnegsignal* neg) { //notification from sync node about 'no updates'
		assert(neg!=NULL);
		auto node=node_find(neg->node_id);
		if(!node || !node->is_waitexec()) return;

		assert(node->is_blocked());
		if(neg->event_id==node->blockedby->id) { //reply is exactly for blocked event, so clear waitexec
			node->clear_waitexec();
			if(!node->blockedby->waitexec_head) event_continue(node->blockedby);
		}
	}
	void inject_signals(iot_modelsignal* sig) { //injects signal from LOCAL node only
		assert(sig!=NULL);
		uint32_t numhosts=get_num_hosts();
		char bitmapbuf[bitmap32::calc_size(numhosts)];
		bitmap32* tmpbitmap=new(bitmapbuf) bitmap32(numhosts);

		auto node=node_find(sig->node_id);
		if(!node || node->module_id!=sig->module_id) { //invalid pack of signals
			iot_modelsignal::release(sig);
			return;
		}
		assert(node->host==current_host);
		auto node_id=node->node_id;

		if(sig->out_label[0]=='v' && strcmp(sig->out_label+1, IOT_CONFIG_NODE_ERROUT_LABEL)==0) { //put error signals to separate event
			assert(sig->next==NULL); //error output is only one per node

			if(new_errevent) {
				assert(new_errevent->signals_head!=NULL);
				if(sig->reltime > new_errevent->signals_head->reltime) { //we have unfinished event and millisecond changed
					commit_event(); //will nullify new_errevent and release tmp blocks
					goto after_errcheck;
				}
				if(node->is_tmpblocked()) { //node has full (or outs, but error out is only one) blocking, so singnals cannot be combined
					commit_event(); //will nullify new_errevent and release tmp blocks
					goto after_errcheck;
				}
				//check that any dependent node is blocked
				if(node->erroutput.is_connected) {
					if(recursive_checktmpblocked(&node->erroutput, 1, tmpbitmap)) {
						commit_event(); //will nullify new_errevent and release tmp blocks
						goto after_errcheck;
					}
					//for now do not allow to combine new chain of signals if it involves more hosts than event already has
					*tmpbitmap-=*new_errevent_hbitmap;
					if(*tmpbitmap) { //current signals involves additional hosts
						commit_event(); //will nullify new_errevent and release tmp blocks
						goto after_errcheck;
					}
				}
			}
after_errcheck:
			if(!new_errevent) {
				if(!events_freelist) {
					outlog_error("Event queue overflow, loosing signal");
					iot_modelsignal::release(sig);
					return;
				}
				new_errevent=events_freelist;
				events_freelist=events_freelist->qnext;
				new_errevent->init(gwinst->next_event_numerator());
				if(!new_errevent_hbitmap) {
					new_errevent_hbitmap=(bitmap32*)main_allocator.allocate(bitmap32::calc_size(numhosts));
					if(!new_errevent_hbitmap) {
						outlog_error("No memory for host bitmap, loosing signal"); //TODO prevent loosing by doing retry somehow
						iot_modelsignal::release(sig);
						return;
					}
					new(new_errevent_hbitmap) bitmap32(numhosts);
				} else {
					new_errevent_hbitmap->clear();
				}
			} else {
				assert(new_errevent_hbitmap && new_errevent_hbitmap->has_space(numhosts));
			}
			//do tmp block
			node->set_tmpblocked_outs(new_errevent);
			if(node->erroutput.is_connected) recursive_tmpblock(&node->erroutput, new_errevent, 1, new_errevent_hbitmap);

			new_errevent->add_signals(sig);
			return;
		}

		//non-error signal or pack of signals
		iot_modelsignal* csig=sig->next;
		if(!(sig->node_out=node->find_output(sig->out_label))) {
			sig->next=NULL;
			iot_modelsignal::release(sig);
			sig=csig;
		}
		//check pack of signals is from same node, assign node_out
		while(csig) {
			if(node_id!=csig->node_id || !(csig->node_out=node->find_output(csig->out_label))) {
				assert(false); //must be from same node_id
				//in release mode just remove invalid signal
				iot_modelsignal* t=csig;
				csig=csig->next;
				if(t==sig) sig=csig;
				t->next=NULL; //!!!
				iot_modelsignal::release(t);
				continue;
			}
			csig=csig->next;
		}
		if(!sig) return; //all signals have unknown out label

		if(node->is_waitexec()) { //this node is now blocked by some event awaiting its reply from delayed sync execution. directed to suspended event
			assert(node->is_blocked());
assert(node->is_sync()); //only sync nodes should block events (temporary for check)
			node->blockedby->add_signals(sig);
			if(sig->reason_event==node->blockedby->id) { //reply is exactly for blocked event, so clear waitexec
				node->clear_waitexec();
				if(!node->blockedby->waitexec_head) {//list of awaited nodes became empty, so can continue
					event_continue(node->blockedby);
				}
			} //otherwise continue to wait for reply (TODO: some timeout)
			return;
		}


		if(new_event) {
			assert(new_event->signals_head!=NULL);
			if(sig->reltime > new_event->signals_head->reltime) { //we have unfinished event and millisecond changed
				commit_event(); //will nullify new_event and release tmp blocks
				goto after_check;
			}
			if(node->is_tmpblocked_outs()) { //node already has some outs blocked for current event. check that list of outputs in current signals pack has no intersections with already added initial signals
				for(iot_modelsignal* cursig=new_event->signals_head; cursig; cursig=cursig->next) { //loop by initial signals
					if(cursig->node_id!=node_id) continue;
					for(csig=sig; csig; csig=csig->next) { //loop by outputs in current pack of signals
						if(strcmp(cursig->out_label, csig->out_label)==0) { //output already used
							commit_event(); //will nullify new_event and release tmp blocks
							goto after_check;
						}
					}
				}
				//different set of outputs provided in new pack. they can involve same nodes without risk to create backlinks-like behaviour, so do not check tmp blocked, but do tmp block
			} else if(node->is_tmpblocked()) { //node has full blocking, so singnals cannot be combined
				commit_event(); //will nullify new_event and release tmp blocks
				goto after_check;
			} else {
				node->set_tmpblocked_outs(new_event);
				//check that any dependent node is blocked
/*				for(csig=sig; csig; csig=csig->next) { //loop by outputs in current pack of signals
					if(!sig->node_out->is_connected) continue; //no valid links from this output

					tmpbitmap->clear();
					if(recursive_checktmpblocked(csig->node_out, 1, tmpbitmap)) {
						commit_event(); //will nullify new_event and release tmp blocks
						goto after_check;
					}

					//for now do not allow to combine new chain of signals if it involves more hosts than event already has
					*tmpbitmap-=*new_event_hbitmap;
					if(*tmpbitmap) { //current signals involves additional hosts
						commit_event(); //will nullify new_errevent and release tmp blocks
						goto after_check;
					}
				}*/
			}
		}
after_check:
		if(!new_event) {
			if(!events_freelist) {
				outlog_error("Event queue overflow, loosing signal"); //TODO prevent loosing by doing retry somehow
				iot_modelsignal::release(sig);
				return;
			}
			new_event=events_freelist;
			events_freelist=events_freelist->qnext;
			new_event->init(gwinst->next_event_numerator());
			if(!new_event_hbitmap) {
				new_event_hbitmap=(bitmap32*)main_allocator.allocate(bitmap32::calc_size(numhosts));
				if(!new_event_hbitmap) {
					outlog_error("No memory for host bitmap, loosing signal"); //TODO prevent loosing by doing retry somehow
					iot_modelsignal::release(sig);
					return;
				}
				new(new_event_hbitmap) bitmap32(numhosts);
			} else {
				new_event_hbitmap->clear();
			}
		} else {
			assert(new_event_hbitmap && new_event_hbitmap->has_space(numhosts));
		}

		//do tmp block
/*		for(csig=sig; csig; csig=csig->next) { //loop by outputs in current pack of signals
			if(!sig->node_out->is_connected) continue; //no valid links from this output
			recursive_tmpblock(csig->node_out, new_event, 1, new_event_hbitmap);
		}*/
		new_event->add_signals(sig);
	}
	void commit_event(void) { //commits event with initial signals from LOCAL nodes only
		if(!new_event && !new_errevent) return;
		iot_config_item_node_t* node;
		if(new_errevent) {
			new_event_hbitmap->set_bit(current_host->index); //add curent host to bitmap of involved hosts
			while((node=new_errevent->tmp_blocked_nodes_head)) {
				node->clear_tmpblocked_athead(new_errevent);
			}

			new_errevent->is_error=true;
			BILINKLISTWT_INSERTTAIL(new_errevent, events_qhead, events_qtail, qnext, qprev);
			new_errevent=NULL;
		}
		if(new_event) {
			new_event_hbitmap->set_bit(current_host->index); //add curent host to bitmap of involved hosts
			while((node=new_event->tmp_blocked_nodes_head)) {
				node->clear_tmpblocked_athead(new_event);
			}

			new_event->is_error=false;
			BILINKLISTWT_INSERTTAIL(new_event, events_qhead, events_qtail, qnext, qprev);
			new_event=NULL;
		}
		start_executor();
	}

private:
	void set_needexec(iot_config_item_node_t* node) {
		assert(!node->needs_exec());
		outlog_debug_modelling("Node %u is pending execution", node->node_id);
		BILINKLIST_INSERTHEAD(node, needexec_head, needexec_next, needexec_prev);
	}
	void stop_executor(void) {
		if(uv_is_active((uv_handle_t*)&events_executor)) uv_check_stop(&events_executor);
	}
	void start_executor(void) {
		if(uv_is_active((uv_handle_t*)&events_executor)) return;
		uv_check_start(&events_executor, [](uv_check_t* handle)->void {
			iot_configregistry_t *reg=(iot_configregistry_t*)(handle->data);
			reg->process_events();
		});
	}
	void process_events(void) { //for now process first event from queue
		if(events_qhead) {
			iot_modelevent* ev, *evnext=events_qhead;
			do {
				ev=evnext;
				evnext=evnext->qnext;
				if(event_start(ev)) break; //event was started and moved from events_qhead list to current_events_head or removed
			} while(ev!=events_qtail);
		}
		if(!events_qhead) stop_executor();
			else start_executor();
	}
	bool event_start(iot_modelevent* ev); //checks if event processing involves any blocked node or rule. if no, then starts processing and returns true
	void event_continue(iot_modelevent *ev); //do all possible steps of event modelling
	void recursive_calcpath(iot_config_item_node_t* node, int depth); //calculate potential path for initial nodes
	void recursive_block(iot_config_node_out_t* out, iot_modelevent* ev, int depth); //block all connected nodes by specified event
	iot_modelevent* recursive_checkblocked(iot_config_node_out_t* out, int depth); //check if any node connected to provided out is blocked by event processing
	void recursive_tmpblock(iot_config_node_out_t* out, iot_modelevent* ev, int depth, bitmap32* hbitmap); //temporary block all connected nodes by specified event
	bool recursive_checktmpblocked(iot_config_node_out_t* out, int depth, bitmap32* hbitmap); //check if any node connected to provided out is temporary blocked by event forming
};




#endif //IOT_CONFIGREGISTRY_H
