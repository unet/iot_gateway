#ifndef IOT_CONFIGMODEL_H
#define IOT_CONFIGMODEL_H
//Contains data structures for configuration modelling

#include<stdint.h>
#include<assert.h>

#include <iot_module.h>
#include <kernel/iot_common.h>

//struct iot_config_inst_node_t;
//struct iot_configregistry_t;
//struct iot_config_actlist_item_t;
//struct iot_config_group_mode_t;

class iot_nodemodel;
class iot_nodelink;
class iot_nodevaluelink;
class iot_nodemsglink;

#include<kernel/iot_configregistry.h>
#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_kernel.h>

//models node with several inputs and several outputs. each input/output is iot_nodelinkmodel object, which is instanciated by some node output
class iot_nodemodel {
	uint8_t num_valueinputs, num_valueoutputs;
	uint8_t num_msginputs, num_msgoutputs;

	struct valueoutput_t {
		const iot_node_valuelinkcfg_t *cfg;
		iot_nodevaluelink* link; //directs to all consumers of output value (inputs of other nodes)
		iot_valueclass_BASE* value;
	} *valueoutput; //points to array with exactly num_valueoutputs items
	valueoutput_t errorvalue;
	iot_valueclass_nodeerrorstate errorstate; //errorvalue.value points to this item

	struct valueinput_t {
		const iot_node_valuelinkcfg_t *cfg;
		iot_nodevaluelink* link; //refers to current value of input directing to necessary output of some node
	} *valueinput; //points to array with exactly num_valueinputs items

	struct msgoutput_t {
		const iot_node_msglinkcfg_t *cfg;
		iot_nodemsglink* link; //directs to all consumers of output messages
	} *msgoutput; //points to array with exactly num_msgoutputs items

	struct msginput_t {
		const iot_node_msglinkcfg_t *cfg;
		iot_nodemsglink* link; //directs to all sources of messages (can be several sources for msg lines)
	} *msginput; //points to array with exactly num_msginputs items

	iot_config_inst_node_t* cfgitem;
	iot_modinstance_locker modinst;

public:
	static iot_nodemodel* create(iot_config_inst_node_t* cfgitem_, iot_module_item_t *module);
};


#endif //IOT_CONFIGMODEL_H
