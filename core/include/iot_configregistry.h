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
	time_t modes_modtime;
	time_t active_set;
	iot_id_t modes[IOT_CONFIG_MAX_MODES_PER_GROUP]; //array with list of possible modes not including default mode
};


struct iot_config_item_rule_t {
	iot_id_t rule_id;

	iot_id_t mode_id; //can be zero for links from group-common rules
	iot_config_item_group_t *group_item; //must point to structure corresponding to group_id

	bool is_del;

	bool is_active(void) const {
		return group_item && (!mode_id || group_item->activemode_id==mode_id);
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
	iot_id_t node_id;
	iot_config_item_host_t* host=NULL;
	uint32_t module_id=0;

	iot_config_item_rule_t* rule_item=NULL; //parent rule. can be NULL if node is persistent or temporary but rule-independent (in particular rules can be totally unused)

//	iot_id_t mode_id=0; //zero for persistent nodes. can be zero for temporary nodes (which are group-common)
//	iot_config_item_group_t *group_item=NULL; //must point to structure corresponding to group_id for temporary nodes, NULL for persistent

	uint32_t cfg_id=0; //node config number when props were updated last time

	iot_config_node_dev_t *dev=NULL; //linked list of device filters

	iot_config_node_in_t *inputs=NULL; //linked list of input link connections

	iot_config_node_out_t *outputs=NULL; //linked list of output link connections
	iot_config_node_out_t erroutput; //error output link connections

	json_object *json_config=NULL;

	iot_nodemodel* nodemodel=NULL; //always NULL for nodes of other hosts

	iot_threadmsg_t *prealloc_execmsg=NULL;

	uint8_t config_ver=0;

	bool is_del=false;
	bool outputs_connected=false; //flag that there is at least one is_connected output in outputs list


//	Props used during modelling
	bool probing_mark=false; //use during recursive model traversing to mark already checked node. ALWAYS MUST BE CLEARED JUST AFTER traversing
	bool acted=false; //for blocked node shows if it was already executed during event processing
	bool pathset=false; //flag that some in has assigned pathlen (it could be temporary assignment for non-initial node). necessary just to optimize cleaning pathlen

	uint16_t maxpathlen=0;

	iot_modelevent* blockedby=NULL; //non-NULL value if this node is involved in corresponding event processing. event must be present in config_registrr->current_events_head
	iot_config_item_node_t* blocked_next=NULL; //if blockedby is set, then position in blockedby->blocked_nodes_head list
	iot_config_item_node_t* initial_next=NULL, *initial_prev=NULL; //for blocked node can held position in blockedby->initial_nodes_head list

	iot_config_item_node_t* needexec_next=NULL, *needexec_prev=NULL; //position in configregistry->needexec_head or blockedby->needexec_next of nodes which need recount
												//of outputs (with or without resend of input update notification depending on 

	iot_config_item_node_t* waitexec_next=NULL, *waitexec_prev=NULL; //for sync node being executed in complex mode helds position in blockedby->waitexec_head list

	iot_config_item_node_t(iot_id_t node_id) : node_id(node_id), erroutput (this, "v" IOT_CONFIG_NODE_ERROUT_LABEL, &iot_datavalue_nodeerrorstate::const_noinst) {
	}

	bool needs_exec(void) {return needexec_prev!=NULL;}
	void clear_needexec(void) {
		assert(needexec_prev!=NULL);
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
	iot_modelevent *new_errevent=NULL; //uncommited error signals event, new signals are added to it, waits for commit_signals or large reltime difference of next signal

	iot_modelevent *current_events_head=NULL; //list of events being currently processed in parallel. qnext and qprev fields are used, but no tail

//	uint64_t last_eventid_numerator=0; //last used numerator. it is timestamp with microseconds decremented by 1e15 mcs and multiplied by 1000. least 3 
//									//decimal digits are just incremented when number of microsends is not changed (i.e. events appear during same microsecond)

//	iot_threadmsg_t *freemsg_head=NULL; //list of free msg structs allocated as memblocks
//	uint32_t num_freemsgs=0; //number of items in freemsg_head

	uv_check_t events_executor={};
	iot_config_item_node_t* needexec_head=NULL; //list of nodes whose output must be recalculated. uses fields needexec_next/prev

	bool inited=false; //flag that one-time init was done in start_config

public:
	iot_configregistry_t(iot_gwinstance* gwinst_) : gwinst(gwinst_), nodes_index(512, 1), links_index(512, 1) {
		assert(gwinst!=NULL);

		//fill events_freelist from preallocated structs
		memset(eventsbuf, 0, sizeof(eventsbuf));
		for(unsigned i=0;i<sizeof(eventsbuf)/sizeof(eventsbuf[0]);i++)
			ULINKLIST_INSERTHEAD(&eventsbuf[i], events_freelist, qnext);
	}

	static json_object* read_jsonfile(const char* dir, const char* relpath, const char *name); //main thread
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
		assert(h->next==NULL && h->prev==NULL);

		//get axclusive lock on hosts list
		hosts_lock.wrlock();

		BILINKLIST_INSERTHEAD(h, hosts_head, next, prev);
		num_hosts.fetch_add(1, std::memory_order_relaxed);

		hosts_lock.wrunlock();

		if(h->host_id==gwinst->this_hostid) {
			assert(current_host==NULL);
			current_host=h;
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
	}
	void host_delete(iot_config_item_host_t* h, bool skiplock=false) {
		assert(uv_thread_self()==main_thread);

		if(!skiplock) {
			//get axclusive lock on hosts list
			hosts_lock.wrlock();
		}

		//do exclusive work
		BILINKLIST_REMOVE_NOCL(h, next, prev);
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

		assert(node->blockedby!=NULL);
		if(neg->event_id==node->blockedby->id) { //reply is exactly for blocked event, so clear waitexec
			node->clear_waitexec();
			if(!node->blockedby->waitexec_head) event_continue(node->blockedby);
		}
	}
	void inject_signals(iot_modelsignal* sig) {
		assert(sig!=NULL);
		if(sig->out_label[0]=='v' && strcmp(sig->out_label+1, IOT_CONFIG_NODE_ERROUT_LABEL)==0) { //put error signals to separate event
			assert(sig->next==NULL); //error output is only one per node

			if(new_errevent) {
				if(sig->reltime > new_errevent->signals_tail->reltime) { //we have unfinished event and millisecond changed
					commit_event();
				} else if(new_errevent->signals_head) { //check if signal already present in event
					for(iot_modelsignal* cursig=new_errevent->signals_head; cursig; cursig=cursig->next) {
						if(cursig->node_id==sig->node_id && strcmp(cursig->out_label, sig->out_label)==0) { //already used
							commit_event();
							break;
						}
					}
				}
			}
			if(!new_errevent) {
				if(!events_freelist) {
					outlog_error("Event queue overflow, loosing signal");
					iot_modelsignal::release(sig);
					return;
				}
				new_errevent=events_freelist;
				events_freelist=events_freelist->qnext;
				new_errevent->init(gwinst->next_event_numerator());
			}
			new_errevent->add_signals(sig);
			return;
		}
		//non-error signal or pack of signals

		auto node=node_find(sig->node_id);
		if(node && node->module_id==sig->module_id) sig->node_out=node->find_output(sig->out_label);
			else sig->node_out=NULL;
		if(!sig->node_out) { //invalid pack of signals
			iot_modelsignal::release(sig);
			return;
		}

		iot_modelsignal* csig=sig->next;
		//check pack of signals is from same node
		while(csig) {
			if(sig->node_id!=csig->node_id) {
				assert(false);
				iot_modelsignal* t=csig;
				csig=csig->next;
				t->next=NULL; //!!!
				iot_modelsignal::release(t);
				continue;
			}
			csig=csig->next;
		}
		if(node->is_waitexec()) { //this node is now blocked by some event awaiting its reply from delayed sync execution. directed to suspended event
			assert(node->blockedby!=NULL);
			node->blockedby->add_signals(sig);
			if(sig->reason_event==node->blockedby->id) { //reply is exactly for blocked event, so clear waitexec
				node->clear_waitexec();
				if(!node->blockedby->waitexec_head) event_continue(node->blockedby);
			} //otherwise continue to wait for reply (TODO: some timeout)
			return;
		}


		if(new_event) {
			if(sig->reltime > new_event->signals_tail->reltime) { //we have unfinished event and millisecond changed
				commit_event();
			} else if(new_event->signals_head) { //check if any signal already present in event to prevent same signals in one event
				for(iot_modelsignal* cursig=new_event->signals_head; cursig; cursig=cursig->next) {
					if(cursig->node_id!=sig->node_id) continue;
					csig=sig;
					while(csig) {
						if(strcmp(cursig->out_label, csig->out_label)==0) { //already used
							commit_event();
							break;
						}
						csig=csig->next;
					}
					if(csig) break; //propagate break in internal loop
				}
			}
		}
		if(!new_event) {
			if(!events_freelist) {
				outlog_error("Event queue overflow, loosing signal");
				iot_modelsignal::release(sig);
				return;
			}
			new_event=events_freelist;
			events_freelist=events_freelist->qnext;
			new_event->init(gwinst->next_event_numerator());
		}
		new_event->add_signals(sig);
	}
	void commit_event(void) {
		if(!new_event && !new_errevent) return;
		if(new_errevent) {
			new_errevent->is_error=true;
			BILINKLISTWT_INSERTTAIL(new_errevent, events_qhead, events_qtail, qnext, qprev);
			new_errevent=NULL;
		}
		if(new_event) {
			BILINKLISTWT_INSERTTAIL(new_event, events_qhead, events_qtail, qnext, qprev);
			new_event=NULL;
		}
		start_executor();
	}

private:
	void set_needexec(iot_config_item_node_t* node) {
		assert(!node->needs_exec());
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
	bool event_start(iot_modelevent* ev) { //checks if event processing involves any blocked node or rule. if no, then starts processing and returns true
		if(ev->is_blocked) return false;
printf("CHECKING EVENT %" PRIu64 " CAN BE STARTED\n", ev->id.numerator);

		iot_modelsignal* sig;
		//check if event is not blocked by other events being executed. also fill node_out field for all valid signals
		if(ev->signals_head) {
			//assume all probing_mark are reset
			for(sig=ev->signals_head; sig; sig=sig->next) {
				if(sig->node_out) {
					if(sig->node_out->node->node_id!=sig->node_id || sig->node_out->node->module_id!=sig->module_id || strcmp(sig->node_out->label, sig->out_label)!=0) {
						assert(false);
						sig->node_out=NULL;
					}
				}
				if(!sig->node_out) {
					auto node_item=node_find(sig->node_id);
					if(node_item && node_item->module_id==sig->module_id) sig->node_out=node_item->find_output(sig->out_label);
					if(!sig->node_out) continue; //signal is invalid
				}

				if(!sig->node_out->is_connected) continue; //no valid links from this output
//				node_item->probing_mark=true; //this prevents back-links influence which is anyway blocked further
				iot_modelevent* blocker;
				if((blocker=recursive_checkblocked(sig->node_out,1))) { //some node in path of signal is blocked
//					node_item->probing_mark=false;
					ev->wait_for(blocker);
					return false;
				}
//				node_item->probing_mark=false;
			}
		}
		if(needexec_head) { //check if there are delayed back links calculations and involved nodes not blocked by other events
			for(iot_config_item_node_t* node=needexec_head; node; node=node->needexec_next) {
				if(node->blockedby) continue; //skip chains of blocked nodes
				if(node->outputs_connected) {
					iot_config_node_out_t *out;
					for(out=node->outputs; out; out=out->next) { //loop by outputs of node
						if(!out->is_connected) continue; //no valid links
						if(recursive_checkblocked(out,1)) break; //some node in path of signal is blocked, so skip node from needexec list
					}
					if(out) continue; //was break after recursive_checkblocked call, so path is blocked
				}
				//here node signal path is not blocked, so input signal can be added to current event
				//we can block node and its path right here (there must be no exit conditions later, before blocking ev->signals_head dependent nodes!!!) POINT1
				node->blockedby=ev;
				for(iot_config_node_out_t *out=node->outputs; out; out=out->next) //loop by outputs of node
					if(out->is_connected) recursive_block(out, ev, 1);

				if(node->outputs_connected && node->is_sync() && !node->is_initial())
					node->set_initial(ev); //select this node as initial as it is in signalled state
			}
		}

		//NO EXIT conditions must be after POINT1 till this point!!!

		//now block all nodes involved by signals
		for(sig=ev->signals_head; sig; sig=sig->next) {
			if(!sig->node_out) { //signal is invalid
				outlog_notice("Lost invalid signal from node %" IOT_PRIiotid " output '%s'", sig->node_id, sig->out_label);
				continue;
			}
			if(!sig->node_out->is_connected) continue; //no valid links from this output
//			sig->node_out->node->probing_mark=true; //this prevents back-links influence which is anyway blocked further
			recursive_block(sig->node_out,ev,1);
//			sig->node_out->node->probing_mark=false;
		}


		BILINKLISTWT_REMOVE(ev, qnext, qprev);

		if(!ev->blocked_nodes_head && !ev->signals_head) {
			outlog_notice("No involved nodes in event %" PRIu64, ev->id.numerator);
			ev->destroy();
			ULINKLIST_INSERTHEAD(ev, events_freelist, qnext); //return to freelist
			return false;
		}

printf("EXECUTION OF EVENT %" PRIu64 " STARTED\n", ev->id.numerator);
		BILINKLIST_INSERTHEAD(ev, current_events_head, qnext, qprev);


		for(sig=ev->signals_head; sig; sig=sig->next) { //some of signal source nodes can become blocked, but these nodes cannot be reexecuted, so mark them as acted
			if(!sig->node_out) continue;
			auto node=sig->node_out->node;
			if(node->blockedby==ev) {
				node->acted=true;
				if(node->is_initial()) node->clear_initial(); //nodes from needexec_head could be added to initial list
			}
		}

		event_continue(ev);

		return true;
	}
	void event_continue(iot_modelevent *ev) { //do all possible steps of event modelling
		iot_modelsignal* sig;
		bool nomemory;
		switch(ev->continue_phase) {
			case ev->CONT_NONE: break; //start of execution, just continue
			case ev->CONT_NOMEMORY:
				assert(ev->initial_nodes_head!=NULL);
				assert(ev->minpathlen>0);
				goto memory_prealloc;
			case ev->CONT_NOMEMORYASYNC:
				assert(ev->initial_nodes_head==NULL && ev->signals_head==NULL);
				goto nosignals;
			case ev->CONT_WAITEXEC:
				assert(ev->waitexec_head==NULL);
				break;
			case ev->CONT_NOMEMORYSYNCWO:
				assert(ev->initial_nodes_head==NULL);
				goto nosignals;
		}
		if(!ev->signals_head && !ev->initial_nodes_head) goto nosignals;

nextstep: //begin loop of execution steps {

			ev->step++;
			assert(ev->step<=nodes_index.getamount());

printf("STEP %u MODELLING EVENT %" PRIu64 "\n", unsigned(ev->step), ev->id.numerator);

			//PROCESS SIGNALS by assigning updated outputs to corresponding out and propagating to corresponding inputs
			while((sig=ev->get_signal())) {

				if(sig->node_out) {
					if(sig->node_out->node->node_id!=sig->node_id || sig->node_out->node->module_id!=sig->module_id || strcmp(sig->node_out->label, sig->out_label)!=0) {
						assert(false);
						sig->node_out=NULL;
					}
				}
				if(!sig->node_out) {
					auto node_item=node_find(sig->node_id);
					if(node_item && node_item->module_id==sig->module_id) sig->node_out=node_item->find_output(sig->out_label);

					if(!sig->node_out) {
						iot_modelsignal::release(sig);
						continue;
					}
				}
				if(sig->node_out->is_value()) { //is value output
					if(sig->node_out->current_value==sig->data || (sig->node_out->current_value && sig->data && *sig->data==*sig->node_out->current_value)) {
						//value unchanged
						iot_modelsignal::release(sig);
						continue;
					}
					//apply output update
					char buf1[128],buf2[128];
					outlog_debug("Value of output '%s' of node %" IOT_PRIiotid " changed from \"%s\" into \"%s\"", sig->node_out->label+1,
						sig->node_out->node->node_id, sig->node_out->current_value ? sig->node_out->current_value->sprint(buf1, sizeof(buf1)) : "Undef",
						sig->data ? sig->data->sprint(buf2, sizeof(buf2)) : "Undef");

					if(sig->node_out->current_value) sig->node_out->current_value->release();
					if(sig->data) {
						sig->data->incref();
						sig->node_out->current_value=sig->data;
					} else {
						sig->node_out->current_value=NULL;
					}
				} else { //is msg output
					if(!sig->data) {
						assert(false); //should not be
						iot_modelsignal::release(sig);
						continue;
					}

					char buf1[128];
					outlog_debug("New message from output '%s' of node %" IOT_PRIiotid ": \"%s\"", sig->node_out->label+1,
						sig->node_out->node->node_id, sig->data->sprint(buf1, sizeof(buf1)));
				}

				if(!sig->node_out->is_connected) { //no valid links from this output
					//value was already updated, msg is just dropped, so nothing to do more
					iot_modelsignal::release(sig);
					continue;
				}

				for(iot_config_item_link_t* link=sig->node_out->ins_head; link; link=link->next_input) { //loop by inputs connected to provided out
					if(!link->valid() || link->in->node->blockedby!=ev) continue;
					auto dnode=link->in->node;

					if(sig->node_out->is_value()) { //check that input value really changed or can be changed AND UPDATE THEM
						if(link->in->fixed_value) continue; //input value fixed
						if(link->in->current_value==sig->data || (link->in->current_value && sig->data && *sig->data==*link->in->current_value)) continue; //input value unchanged

						char buf1[128],buf2[128];
						outlog_debug("\tValue of input '%s' of node %" IOT_PRIiotid " changed from \"%s\" into \"%s\"", link->in->label+1,
							dnode->node_id, link->in->current_value ? link->in->current_value->sprint(buf1, sizeof(buf1)) : "Undef",
							sig->data ? sig->data->sprint(buf2, sizeof(buf2)) : "Undef");

						//update input
						if(link->in->current_value) link->in->current_value->release();
						if(sig->data) {
							sig->data->incref();
							link->in->current_value=sig->data;
						} else {
							link->in->current_value=NULL;
						}

						if(link->in->real_index<0) continue; //signal to unknown input cannot be delivered

					} else {
						char buf1[128];
						outlog_debug("\tNew message for input '%s' of node %" IOT_PRIiotid ": \"%s\"", link->in->label+1,
							dnode->node_id, sig->data->sprint(buf1, sizeof(buf1)));

						if(link->in->real_index<0) continue; //signal to unknown input cannot be delivered, so drop msg

						//copy msg to list
						if(link->current_msg) {
							char buf[128];
							outlog_notice("Overwriting duplicated input message \"%s\" for node %" IOT_PRIiotid " input '%s'", link->current_msg->sprint(buf, sizeof(buf)), dnode->node_id, link->in->label+1);
							link->current_msg->release();
						}
						sig->data->incref();
						link->current_msg=sig->data;
					}

					link->in->is_undelivered=true;
					if(!dnode->needs_exec()) set_needexec(dnode);

					if(!dnode->acted && dnode->outputs_connected && dnode->is_sync() && !dnode->is_initial())
						dnode->set_initial(ev); //not already executed and has connected outputs and IS SYNC and not already in initial list
				}

				iot_modelsignal::release(sig);
			}

			assert(ev->signals_head==NULL);

			ev->minpathlen=0; //non-zero value here means restoration from memory allocation error
memory_prealloc:
			if(!ev->initial_nodes_head) goto nosignals;

			//PREPARE FOR SEARCH OF OPTIMAL INITIAL NODES
			for(iot_config_item_node_t* node=ev->blocked_nodes_head; node; node=node->blocked_next) {
				if(node->pathset) {
					node->pathset=false;
					for(iot_config_node_in_t *in=node->inputs; in; in=in->next) {
						in->pathlen=0;
						if(in->is_msg()) //for msg links also clear pathlen in links
							for(iot_config_item_link_t* link=in->outs_head; link; link=link->next_output) link->pathlen=0;
					}
				}
				if(node->is_initial()) {
					assert(node->needs_exec());
					assert(!node->acted);
					for(iot_config_node_in_t *in=node->inputs; in; in=in->next) {
						if(!in->is_undelivered) continue;
						if(in->is_msg()) {
							for(iot_config_item_link_t* link=in->outs_head; link; link=link->next_output)
								if(link->is_undelivered) {
									link->pathlen=1;
									in->pathlen=1;
								}
							if(in->inject_msg) in->pathlen=1;
						} else {
							in->pathlen=1;
						}
						if(in->pathlen) node->pathset=true;
					}
					assert(node->pathset); //needexec nodes must always have some input/link in undelivered state
				}
			}

			//SEARCH OPTIMAL INITIAL NODES
			uint32_t minpathlen;
			if(!ev->minpathlen) {
				if(ev->initial_nodes_head->initial_next) { //more than 1 initial node, so must calculate potential paths and get minimal pathlen for initial nodes
					minpathlen=UINT16_MAX+1;
					for(iot_config_item_node_t* node=ev->initial_nodes_head; node; node=node->initial_next) {
						node->probing_mark=true;
						recursive_calcpath(node, 1);
						node->probing_mark=false;

						//find max pathlen among inputs
						for(iot_config_node_in_t *in=node->inputs; in; in=in->next) {//loop by all inputs of node
							if(in->is_msg()) { //pathlen for msg inputs must be calculated as maximum among link
								for(iot_config_item_link_t* link=in->outs_head; link; link=link->next_output) if(link->pathlen>in->pathlen) in->pathlen=link->pathlen;
							}
							if(in->pathlen > node->maxpathlen) node->maxpathlen=in->pathlen;
						}
						if(node->maxpathlen < minpathlen) minpathlen=node->maxpathlen;
					}
				} else minpathlen=ev->initial_nodes_head->maxpathlen;
			} else minpathlen=ev->minpathlen; //restoration when nomemory happened

			//MEMORY PREALLOCATION
			//preallocate memory for signals and thread messages to be sure allocation won't fail later

			nomemory=false;

			for(iot_config_item_node_t* nodenext=NULL, *node=ev->initial_nodes_head; node; node=nodenext) {
				nodenext=node->initial_next;
				if(node->maxpathlen>minpathlen) continue; //not selected for this step
				assert(node->maxpathlen==minpathlen); // minpathlen must be minimal among maxpathlens

				if(!node->prepare_execute()) {
					nomemory=true;
					break;
				}
			}

			if(nomemory) {
				ev->minpathlen=uint16_t(minpathlen); //remember calculated minpathlen
				ev->continue_phase=ev->CONT_NOMEMORY;
				//TODO set retry timer
				return;
			}

			//EXECUTION
			for(iot_config_item_node_t* nodenext=NULL, *node=ev->initial_nodes_head; node; node=nodenext) {
				nodenext=node->initial_next;
				if(node->maxpathlen>minpathlen) continue; //not selected for this step
				assert(node->maxpathlen==minpathlen); // minpathlen must be minimal among maxpathlens

printf("\tSELECTED NODE %" IOT_PRIiotid " for execution on this step\n", node->node_id);
				node->acted=true;
				node->clear_initial();

				node->execute(); //will inject output signals in case node in simple sync or add node to ev->waitexec_head
			}
			if(ev->waitexec_head) { //there are complex sync nodes which must be waited for
				ev->continue_phase=ev->CONT_WAITEXEC;
				//TODO set timeout timer
				return;
			}


//		} end of execution steps loop
		if(ev->signals_head || ev->initial_nodes_head) goto nextstep;



nosignals:
		//execute sync nodes without outputs (!). we MUST wait for their result before sending async requests
		iot_config_item_node_t* node;
		for(node=ev->blocked_nodes_head; node; node=node->blocked_next) {
			if(node->acted || !node->needs_exec() || !node->is_sync()) continue;
			if(!node->prepare_execute()) {
				ev->continue_phase=ev->CONT_NOMEMORYSYNCWO;
				//TODO set retry timer
				return;
			}
			if(node->needs_exec()) node->execute(); //needs_exec can be cleared by prepare_execute()
		}
		if(ev->waitexec_head) { //there are complex sync nodes which must be waited for
			ev->continue_phase=ev->CONT_WAITEXEC;
			//TODO set timeout timer
			return;
		}
		if(ev->signals_head) goto nextstep;

		//execute async pending nodes
		for(node=ev->blocked_nodes_head; node; node=node->blocked_next) {
			if(node->acted || !node->needs_exec()) continue;
			assert(!node->is_sync());
			if(!node->prepare_execute(true)) {
				ev->continue_phase=ev->CONT_NOMEMORYASYNC;
				//TODO set retry timer
				return;
			}
			if(node->needs_exec()) node->execute(true); //needs_exec can be cleared by prepare_execute()
		}

		while((node=ev->blocked_nodes_head)) {
			node->blockedby=NULL;
			ULINKLIST_REMOVEHEAD(ev->blocked_nodes_head, blocked_next);
		}

printf("EXECUTION OF EVENT %" PRIu64 " FINISHED\n", ev->id.numerator);
		ev->continue_phase=ev->CONT_NONE;
		BILINKLIST_REMOVE(ev, qnext, qprev); //remove from current events list
		ev->destroy();
		ULINKLIST_INSERTHEAD(ev, events_freelist, qnext); //return to freelist
	}
	void recursive_calcpath(iot_config_item_node_t* node, int depth) { //calculate potential path for initial nodes

printf("\t\tTracing potential path from node %" IOT_PRIiotid "\n", node->node_id);

		depth++;
		//eval every output of node to all connected inputs
		for(iot_config_node_out_t *out=node->outputs; out; out=out->next) { //loop by outputs of node
			if(!out->is_connected) continue; //no valid links
			for(iot_config_item_link_t* link=out->ins_head; link; link=link->next_input) { //loop by inputs connected to current out
				if(!link->valid()) continue;

				auto dnode=link->in->node;
				if(dnode->probing_mark || dnode->blockedby!=node->blockedby || dnode->acted || !dnode->is_sync()) continue; //skip if used in current potential path or already executed within current event

				if(out->is_msg()) { //for msg inputs do alternative path accounting inside links
					if(link->pathlen>0 && link->pathlen<=depth) continue;
					link->pathlen=depth;
				} else { //value
					if(link->in->pathlen>0 && link->in->pathlen<=depth) continue;
					link->in->pathlen=depth;
				}

				if(dnode->outputs_connected) {
					dnode->probing_mark=true;
					recursive_calcpath(dnode, depth);
					dnode->probing_mark=false;
				}
			}
		}
	}
	void recursive_block(iot_config_node_out_t* out, iot_modelevent* ev, int depth) { //block all connected nodes by specified event
		outlog_debug("(depth %d) Blocking nodes from output '%s' of node %" IOT_PRIiotid, depth, out->label, out->node->node_id);
		if(!out->is_connected) return; //no valid links
		for(iot_config_item_link_t* link=out->ins_head; link; link=link->next_input) { //loop by inputs connected to provided out
			if(!link->valid()) continue;
			auto dnode=link->in->node;
			if(/*dnode->probing_mark || */dnode->blockedby==ev) continue; //probing_mark must be checked here to protect initial signal nodes from blocking
			assert(dnode->blockedby==NULL);
			dnode->blockedby=ev;
printf("Node %" IOT_PRIiotid " blocked\n", dnode->node_id);

			assert(!dnode->is_initial());
			ULINKLIST_INSERTHEAD(dnode, ev->blocked_nodes_head, blocked_next);
			dnode->acted=false;
			dnode->pathset=false;
			for(iot_config_node_in_t *nextin=dnode->inputs; nextin; nextin=nextin->next) {
				nextin->pathlen=0; //loop by all inputs of node which is parent of current input and reset pathlen
				if(nextin->is_msg()) //for msg links also clear pathlen in links
					for(iot_config_item_link_t* link2=nextin->outs_head; link2; link2=link2->next_output) link2->pathlen=0;
			}

			if(out->is_msg()) { //move current msg to prev, prev is dropped
				if(link->prev_msg) {
					char buf[128];
					outlog_notice("Loosing input message \"%s\" for node %" IOT_PRIiotid " input '%s'", link->prev_msg->sprint(buf, sizeof(buf)), dnode->node_id, link->in->label+1);
					link->prev_msg->release();
				}
				link->prev_msg=link->current_msg;
				link->current_msg=NULL;
			}

			if(dnode->outputs_connected)
				for(iot_config_node_out_t *nextout=dnode->outputs; nextout; nextout=nextout->next) { //loop by outputs of node which is parent of current input
					if(nextout->is_connected) recursive_block(nextout, ev, depth+1);
				}
		}
	}
	iot_modelevent* recursive_checkblocked(iot_config_node_out_t* out, int depth) { //check if any node connected to provided out is blocked by event processing
		outlog_debug("(depth %d) Probing blocked nodes from output '%s' of node %" IOT_PRIiotid, depth, out->label, out->node->node_id);
		for(iot_config_item_link_t* link=out->ins_head; link; link=link->next_input) { //loop by inputs connected to provided out
			if(!link->valid() || link->in->node->probing_mark) continue;
			auto dnode=link->in->node;
			if(dnode->blockedby) {
				outlog_debug("blocked by event %" PRIu64 " (common node %" IOT_PRIiotid ")", dnode->blockedby->id.numerator, dnode->node_id);
				return dnode->blockedby;
			}
			if(!dnode->outputs_connected) continue;

			dnode->probing_mark=true; //prevent recursive loop
			for(iot_config_node_out_t *nextout=dnode->outputs; nextout; nextout=nextout->next) { //loop by outputs of node which is parent of current input
				if(!nextout->is_connected) continue; //skip outs without valid links
				iot_modelevent* blocker;
				if((blocker=recursive_checkblocked(nextout, depth+1))) {
					dnode->probing_mark=false;
					return blocker;
				}
			}
			dnode->probing_mark=false;
		}
		return NULL;
	}
};




#endif //IOT_CONFIGREGISTRY_H
