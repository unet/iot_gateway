#ifndef IOT_CONFIGREGISTRY_H
#define IOT_CONFIGREGISTRY_H
//Contains data structures and methods for device handles management

#include<stdint.h>
#include<assert.h>

#include<ecb.h>

#include <iot_module.h>
#include <iot_kapi.h>
#include <kernel/iot_common.h>

struct iot_config_inst_item_t;
struct iot_configregistry_t;
struct iot_config_actlist_item_t;
struct iot_config_group_mode_t;

//#define IOT_MAX_EXECUTOR_DEVICES 1
//#define IOT_MAX_ACTIVATOR_INPUTS 3

#define IOT_CONFIG_MAX_GROUP_ID 16

#include<kernel/iot_deviceregistry.h>
#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_kernel.h>


//represents list of messages to send to several executors when chain of activators is signaled
struct iot_config_actlist_item_t {
	iot_config_actlist_item_t *next, *prev; //for list in config_registry->actlists_head;
	uint32_t size; //full actual size of this object. json_args are embedded just after item[] array

	uint32_t iot_id;
//	uint32_t module_id; for now we have only one realization of action list
	uint8_t group_id;
	uint8_t mode_id;
	uint8_t numitems;
	uint32_t host_id;
	iot_config_inst_item_t* actor; //must point to activator
	struct {
		iot_config_inst_item_t* exec; //must point to executor
		uint32_t action;
		const char *json_args; //arguments for action
	} item[];
};

//represents user configuration item and its connections
struct iot_config_inst_item_t {
	iot_config_inst_item_t *next, *prev; //for list in config_registry->items_head;
	uint32_t size; //full actual size of this object. json_cfg is embedded just after union

	uint32_t iot_id;
	uint32_t module_id;
	iot_modinstance_type_t type; //IOT_MODINSTTYPE_EVSOURCE, IOT_MODINSTTYPE_ACTIVATOR, IOT_MODINSTTYPE_EXECUTOR
	uint8_t group_id;
	uint8_t mode_id;
	uint8_t numitems; //number of items in dev[] or input[]
	uint32_t host_id;
	iot_modinstance_item_t *modinst; //filled after instantiation if_host id is ours
	time_t modtime;
	const char *json_cfg;
	union {
		struct {
			iot_hwdev_ident_t *dev[]; //can have NULL values if corresponding device not assigned
		} evsrc;
		struct {
			iot_config_inst_item_t *input[];
		} actor;
		struct {
			iot_hwdev_ident_t *dev[];
		} exec;
	};
};

//keeps current mode for each group
struct iot_config_group_mode_t {
	iot_config_group_mode_t *next, *prev;
	uint8_t group_id;
	uint8_t mode_id;
	time_t modtime;
};

extern iot_configregistry_t* config_registry;


extern iot_config_inst_item_t item1;

class iot_configregistry_t {
	iot_config_inst_item_t *items_head;
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
//		iot_config_inst_item_t* iitem=(iot_config_inst_item_t*)main_allocator.allocate(sizeof(iot_config_inst_item_t));
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
