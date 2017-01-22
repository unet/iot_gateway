#include<stdint.h>
//#include<time.h>

#include<iot_kapi.h>
#include<iot_utils.h>
#include<kernel/iot_daemonlib.h>
#include<kernel/iot_deviceregistry.h>
#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_kernel.h>
#include<kernel/iot_configregistry.h>



iot_configregistry_t* config_registry=NULL;
static iot_configregistry_t _config_registry;

iot_hostid_t iot_current_hostid=0; //ID of current host in user config


iot_config_inst_item_t item1={ NULL, NULL, 
	size : sizeof(iot_config_inst_item_t),
	iot_id : 1,
	module_id : 3,
	type: IOT_MODINSTTYPE_EVSOURCE,
	group_id : 0,
	mode_id : 0,
	numitems : 0,
	host_id : 1,
	modinst : NULL,
	modtime : 1,
	json_cfg : NULL,
	{evsrc : {}}
};


//int iot_configregistry_t::load_config(const char* file) {
//	iot_current_hostid=1;
//}


void iot_configregistry_t::start_config(void) {
		iot_config_inst_item_t* item, *nextitem=items_head;
		//scan for items whose mode_id is actual for their group and try to instantiate
		iot_module_item_t *module;
		int err;
		while(nextitem) {
			item=nextitem;
			nextitem=nextitem->next;

			assert(item->group_id<IOT_CONFIG_MAX_GROUP_ID);

			if(item->mode_id!=group_mode[item->group_id]) continue; //non-actual mode
			err=modules_registry->load_module(-1, item->module_id, &module);
			if(err) {
				outlog_error("Error loading module with ID %u to instantiate iot_item %u: %s", item->module_id, item->iot_id, kapi_strerror(err));
				//TODO set error state for this iot_id to be able to report to server
				continue;
			}

			switch(item->type) {
				case IOT_MODINSTTYPE_DRIVER:
					assert(false);
					break;
				case IOT_MODINSTTYPE_EVSOURCE:
					if(!module->config->iface_event_source) {
						outlog_error("Incapable instantiation of module ID %u as EVENT SOURCE for iot_item %u", item->module_id, item->iot_id);
						//TODO set error state for this iot_id to be able to report to server
						break;
					}
					err=modules_registry->create_evsrc_modinstance(module, item);
					if(!err) break;

					outlog_error("Event source module with ID %u got error during init: %s", module->config->module_id, kapi_strerror(err));
					if(err==IOT_ERROR_CRITICAL_BUG) module->evsrc_blocked=1;
					//TODO set error state for this iot_id to be able to report to server
					//TODO setup retry if some recoverable error happened
					break;
			}

		}

	}
