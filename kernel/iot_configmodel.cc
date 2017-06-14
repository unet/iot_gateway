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


iot_nodemodel* iot_nodemodel::create(iot_config_item_node_t* cfgitem) {
		assert(cfgitem->nodemodel==NULL);

		iot_nodemodel* m;
		size_t sz=sizeof(iot_nodemodel);
		m=(iot_nodemodel*)main_allocator.allocate(sz, true);
		if(!m) return NULL; //no memory
		memset(m, 0, sz);

		m->node_id=cfgitem->node_id;

		m->cfgitem=cfgitem;
		cfgitem->nodemodel=m;

		m->errorstate=m->errorstate.IOT_NODEERRORSTATE_NOINSTANCE;

		m->state=NODESTATE_NOMODULE;
		m->try_create_instance();

		return m;
}

void iot_nodemodel::assign_inputs(void) { //sets correspondence between iot_config_item_node_t inputs/outputs and real module inputs/outputs by assigning real_index
	int16_t j;
	iot_config_item_link_t *link;
	iot_config_node_in_t *curin=cfgitem->inputs;
	while(curin) {
		if(curin->label[0]=='v') {
			for(j=0;j<node_iface->num_valueinputs;j++) {
				if(strcmp(curin->label+1, node_iface->valueinput[j].label)==0) {
					curin->real_index=j;
					//check links for validity
					link=curin->outs_head;
					while(link) {
						if(link->out && link->out->label[0]=='v' && link->out->real_index>=0 && link->out->node->nodemodel->node_iface->valueoutput[link->out->real_index].is_compatible(&node_iface->valueinput[j])) {
							link->is_valid=true;
							curin->is_connected=true;
							link->out->is_connected=true;
						}
						link=link->next_output;
					}
					break;
				}
			}
		} else if(curin->label[0]=='m') {
			for(j=0;j<node_iface->num_msginputs;j++) {
				if(strcmp(curin->label+1, node_iface->msginput[j].label)==0) {
					curin->real_index=j;
					//check links for validity
					link=curin->outs_head;
					while(link) {
						if(link->out && link->out->label[0]=='m' && link->out->real_index>=0 && link->out->node->nodemodel->node_iface->msgoutput[link->out->real_index].is_compatible(&node_iface->msginput[j])) {
							link->is_valid=true;
							curin->is_connected=true;
							link->out->is_connected=true;
						}
						link=link->next_output;
					}
					break;
				}
			}
		}
		if(curin->real_index<0) {
			outlog_error("%s input line labeled '%.7s' of node with iot_id=%u IS NOT FOUND in module config", (curin->label[0]=='v' ? "Value" : curin->label[0]=='m' ? "Message" : "Unknown"), curin->label+1, cfgitem->node_id);
		}
		curin=curin->next;
	}

	iot_config_node_out_t *curout=cfgitem->outputs;
	while(curout) {
		if(curout->label[0]=='v') {
			for(j=0;j<node_iface->num_valueoutputs;j++) {
				if(strcmp(curout->label+1, node_iface->valueoutput[j].label)==0) {
					curout->real_index=j;
					//check links for validity
					link=curout->ins_head;
					while(link) {
						if(link->in && link->in->label[0]=='v' && link->in->real_index>=0 && link->in->node->nodemodel->node_iface->valueinput[link->in->real_index].is_compatible(&node_iface->valueoutput[j])) {
							link->is_valid=true;
							curout->is_connected=true;
							link->in->is_connected=true;
						}
						link=link->next_output;
					}
					break;
				}
			}
		} else if(curout->label[0]=='m') {
			for(j=0;j<node_iface->num_msgoutputs;j++) {
				if(strcmp(curout->label+1, node_iface->msgoutput[j].label)==0) {
					curout->real_index=j;
					//check links for validity
					link=curout->ins_head;
					while(link) {
						if(link->in && link->in->label[0]=='m' && link->in->real_index>=0 && link->in->node->nodemodel->node_iface->msginput[link->in->real_index].is_compatible(&node_iface->msgoutput[j])) {
							link->is_valid=true;
							curout->is_connected=true;
							link->in->is_connected=true;
						}
						link=link->next_output;
					}
					break;
				}
			}
		}
		if(curout->real_index<0) {
			outlog_error("%s output line labeled '%.7s' of node with iot_id=%u IS NOT FOUND in module config", (curout->label[0]=='v' ? "Value" : curout->label[0]=='m' ? "Message" : "Unknown"), curout->label+1, cfgitem->node_id);
		}
		curout=curout->next;
	}
}


void iot_nodemodel::try_create_instance(void) {
	int err;

	if(!module) { //retry to load module first
		iot_module_item_t *module_=NULL;
		err=modules_registry->load_module(-1, cfgitem->module_id, &module_);

		if(err) {
			outlog_error("Error loading module with ID %u to instantiate node_id %" IOT_PRIiotid ": %s", cfgitem->module_id, node_id, kapi_strerror(err));
			return; //TODO retry later
		}

		node_iface=module_->config->iface_node;
		if(!node_iface) {
			outlog_error("Incapable instanciation of module ID %u as NODE for node_id %" IOT_PRIiotid, cfgitem->module_id, node_id);
			return; //TODO retry only if module is reloaded
		}
		module=module_;
	}
	if(!curvalueinput) { //retry memory allocation
		size_t sz=sizeof(curvalueinput[0])*node_iface->num_valueinputs+
				sizeof(curvalueoutput[0])*node_iface->num_valueoutputs+
				sizeof(curmsgoutput[0])*node_iface->num_msgoutputs;
		void *ptr=main_allocator.allocate(sz, true);
		if(!ptr) {
			return; //TODO retry later
		}
		memset(ptr, 0, sz);

		curvalueinput=static_cast<decltype(curvalueinput)>(ptr);

		char* off=(char*)ptr;

		if(node_iface->num_valueinputs>0) {
			off+=sizeof(curvalueinput[0])*node_iface->num_valueinputs;
		}

		if(node_iface->num_valueoutputs>0) {
			curvalueoutput=static_cast<decltype(curvalueoutput)>((void*)off);
			off+=sizeof(curvalueoutput[0])*node_iface->num_valueoutputs;
		}

		if(node_iface->num_msgoutputs>0) {
			curmsgoutput=static_cast<decltype(curmsgoutput)>((void*)off);
			off+=sizeof(curmsgoutput[0])*node_iface->num_msgoutputs;
		}

		assert(off==(char*)ptr + sz);

		assign_inputs();
	}
	err=modules_registry->create_node_modinstance(module, this);
	if(err && err!=IOT_ERROR_NOT_READY) {
		outlog_error("Node module with ID %u got error during init: %s", module->config->module_id, kapi_strerror(err));
//		if(err==IOT_ERROR_CRITICAL_BUG) module->node_blocked=1;
		//TODO set error state for this iot_id to be able to report to server
		//TODO setup retry if some recoverable error happened
	} else {
		state=NODESTATE_STARTED;
	}
}

void iot_nodemodel::destroy(iot_nodemodel* node) {
	if(node->curvalueinput) {
		for(unsigned i=0;i<node->node_iface->num_valueinputs;i++) {
			if(node->curvalueinput[i].instance_value) iot_release_memblock(node->curvalueinput[i].instance_value);
		}
		for(unsigned i=0;i<node->node_iface->num_valueoutputs;i++) {
			if(node->curvalueoutput[i].instance_value) iot_release_memblock(node->curvalueoutput[i].instance_value);
			if(node->curvalueoutput[i].current_value) iot_release_memblock(node->curvalueoutput[i].current_value);
		}
		iot_release_memblock(node->curvalueinput);
		node->curvalueinput=NULL;
		node->curvalueoutput=NULL;
		node->curmsgoutput=NULL;
	}
	if(node->modinstlk) node->modinstlk.unlock();
	iot_release_memblock(node);
}

void iot_nodemodel::on_instance_destroy(iot_nodemodel* node, iot_modinstance_item_t* modinst) { //called when node instance is released
	if(!node->modinstlk) return;
	assert(node->modinstlk.modinst==modinst);

	if(node->state==NODESTATE_STARTED) node->stop();

	modinst->data.node.model=NULL;

	node->state=NODESTATE_NOINSTANCE;

	node->modinstlk.unlock();

	iot_nodemodel::destroy(node);
	//TODO  do we need locker here at all? restart node instance if possible
}
bool iot_nodemodel::stop(void) {
	if(cfgitem) { //detach from configuration
		cfgitem->nodemodel=NULL;
		cfgitem=NULL;
	}
	switch(state) {
		case NODESTATE_NOMODULE: //model started and is in degenerated state, no required code module found (module==NULL)
		case NODESTATE_NOINSTANCE: //model started but no module instance created (modinstlk is false)
			return true;
		case NODESTATE_STARTED: //model started and module instance is active
			state=NODESTATE_STOPPING;
			modinstlk.modinst->stop(false);
		case NODESTATE_STOPPING: //model stopped and wait module instance to stop to be destroyed
			return false;
		case NODESTATE_RELOADING: //model stopped and wait module instance to stop to restart itself with new configuration
			state=NODESTATE_STOPPING;
			return false;
	}
	assert(false);
	return true;
}



iot_modelsignal::iot_modelsignal(iot_nodemodel *model, uint16_t out_index, uint64_t reltime, iot_valueclass_BASE* value, iot_event_id_t* reason) : reltime(reltime), out_index(out_index), value(value) {
		assert(model->cfgitem!=NULL);
		node_id=model->node_id;
		module_id=model->cfgitem->module_id;
		config_ver=model->cfgitem->config_ver;
		is_msg=false;
		if(reason) reason_event=*reason;
			else reason_event={};
	}

iot_modelsignal::iot_modelsignal(iot_nodemodel *model, uint16_t out_index, uint64_t reltime, iot_msgclass_BASE* msg, iot_event_id_t* reason) : reltime(reltime), out_index(out_index), msg(msg) {
		assert(model->cfgitem!=NULL);
		node_id=model->node_id;
		module_id=model->cfgitem->module_id;
		config_ver=model->cfgitem->config_ver;
		is_msg=true;
		if(reason) reason_event=*reason;
			else reason_event={};
	}

