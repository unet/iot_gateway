#include<stdint.h>
//#include<time.h>

#include<iot_module.h>
#include<iot_utils.h>
#include<kernel/iot_daemonlib.h>
#include<kernel/iot_deviceregistry.h>
#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_kernel.h>
#include<kernel/iot_configregistry.h>
#include<kernel/iot_configmodel.h>


iot_nodemodel* iot_nodemodel::create(iot_config_inst_node_t* cfgitem_, iot_module_item_t *module) {
		assert(module!=NULL && cfgitem_!=NULL && cfgitem_->nodemodel==NULL);
		auto iface=module->config->iface_node;
		assert(iface!=NULL);

		uint8_t num_valueoutputs=iface->num_valueoutputs;
		uint8_t num_valueinputs=iface->num_valueinputs;
		uint8_t num_msgoutputs=iface->num_msgoutputs;
		uint8_t num_msginputs=iface->num_msginputs;

		if(num_valueoutputs>IOT_CONFIG_MAX_NODE_VALUEOUTPUTS) num_valueoutputs=IOT_CONFIG_MAX_NODE_VALUEOUTPUTS;
		if(num_valueinputs>IOT_CONFIG_MAX_NODE_VALUEINPUTS) num_valueinputs=IOT_CONFIG_MAX_NODE_VALUEINPUTS;
		if(num_msgoutputs>IOT_CONFIG_MAX_NODE_MSGOUTPUTS) num_msgoutputs=IOT_CONFIG_MAX_NODE_MSGOUTPUTS;
		if(num_msginputs>IOT_CONFIG_MAX_NODE_MSGINPUTS) num_msginputs=IOT_CONFIG_MAX_NODE_MSGINPUTS;

		size_t sz=sizeof(iot_nodemodel)+sizeof(valueoutput_t)*num_valueoutputs+sizeof(valueinput_t)*num_valueinputs+sizeof(msgoutput_t)*num_msgoutputs+sizeof(msginput_t)*num_msginputs;

		iot_nodemodel* m=(iot_nodemodel*)main_allocator.allocate(sz, true);
		if(!m) return NULL; //no memory
		memset(m, 0, sz);

		m->num_valueoutputs=num_valueoutputs;
		m->num_valueinputs=num_valueinputs;
		m->num_msgoutputs=num_msgoutputs;
		m->num_msginputs=num_msginputs;
		m->errorvalue.value=&m->errorstate;

		uint8_t i;
		char* off=(char*)&m[1];

		if(num_valueoutputs>0) {
			m->valueoutput=(valueoutput_t*)off;
			for(i=0;i<num_valueoutputs;i++) {m->valueoutput[i].cfg=&iface->valueoutput[i]; off+=sizeof(valueoutput_t);}
		}

		if(num_valueinputs>0) {
			m->valueinput=(valueinput_t*)off;
			for(i=0;i<num_valueinputs;i++) {m->valueinput[i].cfg=&iface->valueinput[i]; off+=sizeof(valueinput_t);}
		}

		if(num_msgoutputs>0) {
			m->msgoutput=(msgoutput_t*)off;
			for(i=0;i<num_msgoutputs;i++) {m->msgoutput[i].cfg=&iface->msgoutput[i]; off+=sizeof(msgoutput_t);}
		}

		if(num_msginputs>0) {
			m->msginput=(msginput_t*)off;
			for(i=0;i<num_msginputs;i++) {m->msginput[i].cfg=&iface->msginput[i]; off+=sizeof(msginput_t);}
		}

		assert(off==(char*)m + sz);

		m->cfgitem=cfgitem_;
		cfgitem_->nodemodel=m;

		m->errorstate=m->errorstate.IOT_NODEERRORSTATE_NOINSTANCE;
		return m;
}
