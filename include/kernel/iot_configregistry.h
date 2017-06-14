#ifndef IOT_CONFIGREGISTRY_H
#define IOT_CONFIGREGISTRY_H
//Contains data structures representing user configuration

#include<stdint.h>
#include<assert.h>

#include <json-c/json.h>

#define IOTCONFIG_PATH "config.json"

#include <iot_module.h>
#include <mhbtree.h>
#include <kernel/iot_common.h>

#define IOT_JSONPARSE_UINT(jsonval, typename, varname) { \
	if(typename ## _MAX == UINT32_MAX) {		\
		errno=0;	\
		int64_t i64=json_object_get_int64(jsonval);		\
		if(!errno && i64>0 && i64<=UINT32_MAX) varname=(typename)i64;	\
	} else if(typename ## _MAX == UINT64_MAX) {							\
		uint64_t u64=iot_strtou64(json_object_get_string(jsonval), NULL, 10);	\
		if(!errno && u64>0 && (u64<INT64_MAX || json_object_is_type(jsonval, json_type_string))) varname=(typename)(u64);	\
	} else if(typename ## _MAX == UINT16_MAX || typename ## _MAX == UINT8_MAX) {							\
		int i=json_object_get_int(jsonval);		\
		if(!errno && i>0 && (uint32_t)i <= typename ## _MAX) varname=(typename)i;	\
	} else {	\
		assert(false);	\
	}	\
}


struct iot_config_item_node_t;
struct iot_configregistry_t;
struct iot_config_item_group_t;
struct iot_config_node_in_t;
struct iot_config_node_out_t;
struct iot_config_item_host_t;

//#define IOT_MAX_EXECUTOR_DEVICES 1
//#define IOT_MAX_ACTIVATOR_INPUTS 3

//hard limit of number of emplicit modes in each group of config
#define IOT_CONFIG_MAX_MODES_PER_GROUP 16

#include<kernel/iot_deviceregistry.h>
#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_kernel.h>
#include<kernel/iot_configmodel.h>

struct iot_config_item_link_t {
	iotlink_id_t link_id; //necessary 
	bool is_del;

	iot_config_item_link_t *next_input, *next_output;
	iot_config_node_in_t* in;
	iot_config_node_out_t* out;

	iot_id_t mode_id; //can be zero for links from group-common rules
	iot_config_item_group_t *group_item; //must point to structure corresponding to group_id

	bool is_valid; //shows if link can transfer signals, i.e. that both in and out have real indexes and data types do match
};

struct iot_config_node_in_t {
	iot_config_node_in_t* next; //next pointer in node->inputs list
	iot_config_item_node_t *node; //parent node
	iot_config_item_link_t *outs_head; //array of outputs connected to current input, next_output field is used as next reference
	int16_t real_index; //real index of node input (according to module config) or -1 if not found (MUST BE INITED TO -1)
	bool is_connected; //shows if there is at least one valid link in outs_head list

	char label[IOT_CONFIG_LINKLABEL_MAXLEN+1+1]; //label with type prefix

};

struct iot_config_node_out_t {
	iot_config_node_out_t* next; //next pointer in node->outputs list
	iot_config_item_node_t *node; //parent node
	iot_config_item_link_t *ins_head; //array of inputs connected to current output, next_input field is used as next reference
	int16_t real_index; //real index of node output (according to module config) or -1 if not found (MUST BE INITED TO -1)
	bool is_connected; //shows if there is at least one valid link in ins_head list

	char label[IOT_CONFIG_LINKLABEL_MAXLEN+1+1]; //label with type prefix
};

struct iot_config_node_dev_t {
	iot_config_node_dev_t* next; //next pointer in parent node dev list
	char label[IOT_CONFIG_DEVLABEL_MAXLEN+1]; //up to 7 chars
	uint8_t numidents, maxidents; //actual number of items in idents, number of allocated items
	iot_hwdev_ident_t idents[]; //array of hwdev filters set by user. internal ident[i].dev iot_hwdev_localident_t structure or host can be a template
};

//represents user configuration node item
struct iot_config_item_node_t {
//	iot_config_item_node_t *next, *prev; //for list in config_registry->items_head;
	bool is_del;

	iot_id_t node_id;
	iot_config_item_host_t* host;
	uint32_t module_id;

	iot_id_t mode_id; //zero for persistent nodes. can be zero for temporary nodes (which are group-common)
	iot_config_item_group_t *group_item; //must point to structure corresponding to group_id for temporary nodes, NULL for persistent

	uint32_t cfg_id; //node config number when props were updated last time

	iot_config_node_dev_t *dev; //linked list of device filters

	iot_config_node_in_t *inputs; //linked list of input link connections

	iot_config_node_out_t *outputs; //linked list of output link connections

	json_object *json_config;

	uint8_t config_ver;
//	uint8_t numdevs; //number of valid items in dev[]
//	uint8_t numinputs; //number of valid items in inputs[]
//	uint8_t numoutputs; //number of valid items in outputs[]

	iot_nodemodel* nodemodel; //always NULL for nodes of other hosts
};

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

//represents configuration host item
struct iot_config_item_host_t {
	iot_config_item_host_t *next, *prev; //for list in config_registry->hosts_head;
	bool is_del;

	iot_hostid_t host_id;
	uint32_t cfg_id; //hosts config number when props were updated last time

	uint16_t listen_port;
};


extern iot_configregistry_t* config_registry;


class iot_configregistry_t {
//	iot_config_item_group_t *nodes_head=NULL;
	iot_config_item_group_t *groups_head=NULL;
	iot_config_item_host_t *hosts_head=NULL;
	iot_config_item_host_t *current_host=NULL;

	MemHBTree<iot_config_item_node_t*, iot_id_t, 0, 0, true, 10> nodes_index;
	MemHBTree<iot_config_item_link_t*, iotlink_id_t, 0, 0, true, 10> links_index;

	uint32_t nodecfg_id=0, hostcfg_id=0, modecfg_id=0, owncfg_modtime=0; //current numbers of config parts

	iot_modelevent eventsbuf[100]; //preallocated model event structs
	iot_modelevent *events_freelist=NULL; //only ->qnext is used for list iterating

	iot_modelevent *events_q=NULL, *events_t=NULL; //queue of commited events. processed from head, added to tail. uses qnext and qprev item fields
	iot_modelevent *new_event=NULL; //uncommited event, new signals are added to it, waits for commit_signals or large reltime difference of next signal

	uint64_t last_eventid_numerator=0; //last used numerator. it is timestamp with microseconds decremented by 1e15 mcs and multiplied by 1000. least 3 
									//decimal digits are just incremented when number of microsends is not changed (i.e. events appear during same microsecond)

	uv_check_t events_executor={};
	bool inited=false; //flag that one-time init was done in start_config

public:
	iot_configregistry_t(void) : nodes_index(512, 1), links_index(512, 1) {
		assert(config_registry==NULL);
		config_registry=this;

		//fill events_freelist from preallocated structs
		for(unsigned i=0;i<sizeof(eventsbuf)/sizeof(eventsbuf[0]);i++) {
			eventsbuf[i].qnext=events_freelist;
			eventsbuf[i].qprev=NULL;
			events_freelist=&eventsbuf[i];
		}

	}

	json_object* read_jsonfile(const char* relpath, const char *name); //main thread
	int load_config(json_object* cfg, bool skiphosts);
	int load_hosts_config(json_object* cfg);

	//called ones on startup
	void start_config(void); //main thread

	void free_config(void); //main thread
	void clean_config(void); //main thread. free all config items marked for deletion

//host management
	iot_config_item_host_t* host_find(iot_hostid_t hostid) {
		iot_config_item_host_t* h=hosts_head;
		while(h) {
			if(h->host_id==hostid) return h;
			h=h->next;
		}
		return NULL;
	}
	int host_update(iot_hostid_t hostid, json_object* obj);
	void hosts_markdel(void) { //set is_del mark for all hosts
		iot_config_item_host_t* h=hosts_head;
		while(h) {
			h->is_del=true;
			h=h->next;
		}
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
	uint64_t next_event_numerator(void) { //returns next event numerator
		timeval tv;
		gettimeofday(&tv, NULL);
		uint64_t low=((tv.tv_sec-1000000000)*1000000+tv.tv_usec)*1000;
		if(last_eventid_numerator < low) last_eventid_numerator=low;
			else last_eventid_numerator++;
printf("EVENT %lu allocated\n", last_eventid_numerator);
		return last_eventid_numerator;
	}
	void inject_signal(iot_modelsignal* sig) {
		assert(sig!=NULL);
		if(new_event && sig->reltime > new_event->signals_tail->reltime) { //we have unfinished event and millisecond changed
			commit_event();
		}
		if(!new_event) {
			if(!events_freelist) {
				outlog_error("Event queue overflow, loosing signal");
				iot_modelsignal::release(sig);
				return;
			}
			new_event=events_freelist;
			events_freelist=events_freelist->qnext;
			new_event->init(next_event_numerator());
		}
		new_event->add_signal(sig);
	}
	void commit_event(void) {
		if(!new_event) return;
		BILINKLISTWT_INSERTTAIL(new_event, events_q, events_t, qnext, qprev);
		new_event=NULL;
		start_executor();
	}

private:
	void stop_executor(void) {
		if(uv_is_active((uv_handle_t*)&events_executor)) uv_check_stop(&events_executor);
	}
	void start_executor(void) {
		if(uv_is_active((uv_handle_t*)&events_executor)) return;
		uv_check_start(&events_executor, [](uv_check_t* handle)->void {
			config_registry->process_events();
		});
	}
	void process_events(void) { //for now process first event from queue
		if(events_q) {
			iot_modelevent* ev=events_q;
			BILINKLISTWT_REMOVE(ev, qnext, qprev);
			execute_event(ev);
		}
		if(!events_q) stop_executor();
			else start_executor();
	}
	void execute_event(iot_modelevent* ev) { //ev must be already removed from events_q
		//
		printf("EVENT %lu EXECUTED\n", ev->id.numerator);
		ev->destroy();
		//return to freelist
		ev->qnext=events_freelist;
		events_freelist=ev;
	}
};





#endif //IOT_CONFIGREGISTRY_H
