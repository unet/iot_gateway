#ifndef IOT_CONFIGREGISTRY_H
#define IOT_CONFIGREGISTRY_H
//Contains data structures representing user configuration

#include<stdint.h>
#include<assert.h>

#include <iot_module.h>
#include <kernel/iot_common.h>

struct iot_config_inst_node_t;
struct iot_configregistry_t;
struct iot_config_actlist_item_t;
struct iot_config_group_mode_t;

//#define IOT_MAX_EXECUTOR_DEVICES 1
//#define IOT_MAX_ACTIVATOR_INPUTS 3

#define IOT_CONFIG_MAX_GROUP_ID 16

#include<kernel/iot_configmodel.h>
#include<kernel/iot_deviceregistry.h>
#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_kernel.h>


//represents list of messages to send to several executors when chain of activators is signaled
/*struct iot_config_actlist_item_t {
	iot_config_actlist_item_t *next, *prev; //for list in config_registry->actlists_head;
	uint32_t size; //full actual size of this object. json_args are embedded just after item[] array

	uint32_t iot_id;
//	uint32_t module_id; for now we have only one realization of action list
	uint8_t group_id;
	uint8_t mode_id;
	uint8_t numitems;
	iot_hostid_t host_id;
	iot_config_inst_node_t* actor; //must point to activator
	struct {
		iot_config_inst_node_t* exec; //must point to executor
		uint32_t action;
		const char *json_args; //arguments for action
	} item[];
};
*/
//represents user configuration link between nodes
struct iot_config_inst_outlink_t {
	char out_label[IOT_CONFIG_LINKLABEL_MAXLEN+1];
	iot_id_t group_id, mode_id; //only one should be non-zero
	iot_config_inst_node_t* out_node;
};


//represents user configuration node item
struct iot_config_inst_node_t {
	iot_config_inst_node_t *next, *prev; //for list in config_registry->items_head;
	uint32_t size; //full actual size of this object. json_cfg, dev[], valueinputs, msginputs are embedded

	iot_id_t iot_id;
	iot_hostid_t host_id;
	uint32_t module_id;

	iot_id_t group_id, mode_id; //can both be zero for persistent nodes. only one should be non-zero for temporary nodes

	uint32_t cfg_id; //node config number when props were updated last time

	struct {
		char label[IOT_CONFIG_DEVLABEL_MAXLEN+1]; //up to 7 chars
		uint8_t numidents; //number of items in idents
		iot_hwdev_ident_t *idents; //list of hwdev filters set by user
	} *dev; //internal ident.dev iot_hwdev_localident_t structure or host can be a template
	struct {
		char in_label[IOT_CONFIG_LINKLABEL_MAXLEN+1]; //label without type prefix. up to 7 chars
		uint8_t numouts; //number of items in outs
		iot_config_inst_outlink_t *outs; //list of outputs connected to current input
	} *valueinputs;
	struct {
		char in_label[IOT_CONFIG_LINKLABEL_MAXLEN+1]; //label without type prefix. up to 7 chars
		uint8_t numouts; //number of items in outs
		iot_config_inst_outlink_t *outs; //list of outputs connected to current input
	} *msginputs;

	const char *json_config;


	uint8_t config_ver;
	uint8_t numdevs; //number of valid items in dev[]
	uint8_t numvalueinputs; //number of valid items in valueinputs[]
	uint8_t nummsginputs; //number of valid items in msginputs[]

	iot_nodemodel* nodemodel;

};

//keeps current mode for each group
struct iot_config_group_mode_t {
	iot_config_group_mode_t *next, *prev;
	uint8_t group_id;
	uint8_t mode_id;
	time_t modtime;
};

extern iot_configregistry_t* config_registry;


extern iot_config_inst_node_t item1;

class iot_configregistry_t {
	iot_config_inst_node_t *items_head;
	iot_config_actlist_item_t *actlists_head;
	iot_config_group_mode_t *modes_head;

	uint8_t group_mode[IOT_CONFIG_MAX_GROUP_ID]; //for group_id as index gives current mode of config

	time_t config_modtime;


public:
	iot_configregistry_t(void) : items_head(NULL), actlists_head(NULL), modes_head(NULL), config_modtime(0) {
		assert(config_registry==NULL);
		memset(group_mode, 0, sizeof(group_mode));
		config_registry=this;
	}

	int load_config(const char* file) { //main thread
		iot_current_hostid=1;
//		iot_config_inst_node_t* iitem=(iot_config_inst_node_t*)main_allocator.allocate(sizeof(iot_config_inst_node_t));
//		if(!iitem) return IOT_ERROR_NO_MEMORY;
		BILINKLIST_INSERTHEAD(&item1, items_head, next, prev);

		iot_config_group_mode_t* mitem=modes_head;
		while(mitem) {
			if(mitem->group_id<IOT_CONFIG_MAX_GROUP_ID) group_mode[mitem->group_id]=mitem->mode_id;
			mitem=mitem->next;
		}

		return 0;
	}

	//called ones on startup
	void start_config(void); //main thread

};





#endif //IOT_CONFIGREGISTRY_H
