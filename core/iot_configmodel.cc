#include<stdint.h>
//#include<time.h>

#include "iot_configmodel.h"
#include "iot_deviceregistry.h"
#include "iot_moduleregistry.h"
#include "iot_configregistry.h"
#include "iot_threadregistry.h"
#include "iot_peerconnection.h"


iot_nodemodel* iot_nodemodel::create(iot_config_item_node_t* cfgitem, iot_gwinstance *gwinst_) {
		assert(cfgitem->nodemodel==NULL);
		assert(gwinst_!=NULL);

		iot_nodemodel* m;
		size_t sz=sizeof(iot_nodemodel);
		m=(iot_nodemodel*)main_allocator.allocate(sz, true);
		if(!m) return NULL; //no memory
		memset(m, 0, sz);

		m->gwinst=gwinst_;
		m->node_id=cfgitem->node_id;

		m->cfgitem=cfgitem;
		cfgitem->nodemodel=m;

//		m->errorstate=m->errorstate.IOT_NODEERRORSTATE_NOINSTANCE;

		m->state=NODESTATE_NOMODULE;
		m->is_sync=2;
		m->try_create_instance();

		return m;
}

bool iot_nodemodel::validate_links(void) { //check that every input/output has corresponding in/out link in cfgitem, allocate link if necessary
//return false on lack of memory
	//create unconnected module inputs
	for(int j=0;j<node_iface->num_valueinputs;j++) {
		if(curvalueinput[j].link) continue;

		size_t sz=sizeof(iot_config_node_in_t);
		iot_config_node_in_t* in=(iot_config_node_in_t*)main_allocator.allocate(sz, true);
		if(!in) return false;

		curvalueinput[j].link=in;
		memset(in, 0, sz);
		in->label[0]='v';
		strcpy(in->label+1, node_iface->valueinput[j].label);
		in->node=cfgitem;
		in->real_index=j;

		in->next=cfgitem->inputs;
		cfgitem->inputs=in;
	}

	for(int j=0;j<node_iface->num_msginputs;j++) {
		if(curmsginput[j].link) continue;

		size_t sz=sizeof(iot_config_node_in_t);
		iot_config_node_in_t* in=(iot_config_node_in_t*)main_allocator.allocate(sz, true);
		if(!in) return false;

		curmsginput[j].link=in;
		memset(in, 0, sz);
		in->label[0]='m';
		strcpy(in->label+1, node_iface->msginput[j].label);
		in->node=cfgitem;
		in->real_index=j;

		in->next=cfgitem->inputs;
		cfgitem->inputs=in;
	}

	//create unconnected module outputs
	for(int j=0;j<node_iface->num_valueoutputs;j++) {
		if(curvalueoutput[j].link) continue;

		size_t sz=sizeof(iot_config_node_out_t);
		iot_config_node_out_t* out=(iot_config_node_out_t*)main_allocator.allocate(sz, true);
		if(!out) return false;

		curvalueoutput[j].link=out;
		memset(out, 0, sz);
		out->label[0]='v';
		strcpy(out->label+1, node_iface->valueoutput[j].label);
		out->node=cfgitem;
		out->real_index=j;

		out->next=cfgitem->outputs;
		cfgitem->outputs=out;
	}

	for(int j=0;j<node_iface->num_msgoutputs;j++) {
		if(curmsgoutput[j].link) continue;

		size_t sz=sizeof(iot_config_node_out_t);
		iot_config_node_out_t* out=(iot_config_node_out_t*)main_allocator.allocate(sz, true);
		if(!out) return false;

		curmsgoutput[j].link=out;
		memset(out, 0, sz);
		out->label[0]='m';
		strcpy(out->label+1, node_iface->msgoutput[j].label);
		out->node=cfgitem;
		out->real_index=j;

		out->next=cfgitem->outputs;
		cfgitem->outputs=out;
	}
	links_valid=true;
	return true;
}

void iot_nodemodel::assign_inputs(void) { //sets correspondence between iot_config_item_node_t inputs/outputs and real module inputs/outputs by assigning real_index
	int16_t j;
	iot_config_item_link_t *link;
	iot_config_node_in_t *curin=cfgitem->inputs;
	while(curin) {
		if(curin->is_value()) {
			for(j=0;j<node_iface->num_valueinputs;j++) {
				if(strcmp(curin->label+1, node_iface->valueinput[j].label)==0) {
					curin->real_index=j;
					curvalueinput[j].link=curin;
					//revalidate links
					for(link=curin->outs_head; link; link=link->next_output) link->check_validity();
					break;
				}
			}
		} else { //message input
			for(j=0;j<node_iface->num_msginputs;j++) {
				if(strcmp(curin->label+1, node_iface->msginput[j].label)==0) {
					curin->real_index=j;
					curmsginput[j].link=curin;
					//revalidate links
					for(link=curin->outs_head; link; link=link->next_output) link->check_validity();
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
					curvalueoutput[j].link=curout;
					//revalidate links
					for(link=curout->ins_head; link; link=link->next_input) link->check_validity();
					break;
				}
			}
		} else if(curout->label[0]=='m') {
			for(j=0;j<node_iface->num_msgoutputs;j++) {
				if(strcmp(curout->label+1, node_iface->msgoutput[j].label)==0) {
					curout->real_index=j;
					//revalidate links
					for(link=curout->ins_head; link; link=link->next_input) link->check_validity();
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
		iot_node_module_item_t *module_=NULL;
		err=modules_registry->load_node_module(cfgitem->module_id, &module_);

		if(err) {
			outlog_error("Error loading module with ID %u to instantiate node_id %" IOT_PRIiotid ": %s", cfgitem->module_id, node_id, kapi_strerror(err));
			return; //TODO retry later
		}

		node_iface=module_->config;
		if(!node_iface) {
			outlog_error("Incapable instanciation of module ID %u as NODE for node_id %" IOT_PRIiotid, cfgitem->module_id, node_id);
			return; //TODO retry only if module is reloaded
		}
		module=module_;
		state=NODESTATE_NOINSTANCE;
	}
	if(!curvalueinput) { //retry memory allocation
		size_t sz=sizeof(curvalueinput[0])*node_iface->num_valueinputs+
				sizeof(curvalueoutput[0])*node_iface->num_valueoutputs+
				sizeof(curmsginput[0])*node_iface->num_msginputs+
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

		if(node_iface->num_msginputs>0) {
			curmsginput=static_cast<decltype(curmsginput)>((void*)off);
			off+=sizeof(curmsginput[0])*node_iface->num_msginputs;
		}

		if(node_iface->num_msgoutputs>0) {
			curmsgoutput=static_cast<decltype(curmsgoutput)>((void*)off);
			off+=sizeof(curmsgoutput[0])*node_iface->num_msgoutputs;
		}

		assert(off==(char*)ptr + sz);

		assign_inputs();
	}
	if(!links_valid) {
		if(!validate_links()) return; //no memory TODO retry later
	}
	err=modules_registry->create_node_modinstance(module, this);
	if(err && err!=IOT_ERROR_NOT_READY) {
		outlog_error("Node module with ID %u got error during init: %s", module->dbitem->module_id, kapi_strerror(err));
//		if(err==IOT_ERROR_CRITICAL_BUG) module->node_blocked=1;
		//TODO set error state for this iot_id to be able to report to server
		//TODO setup retry if some recoverable error happened
	} else if(err==IOT_ERROR_NOT_READY) {
		state = NODESTATE_WAITSTART;
	}
}

void iot_nodemodel::destroy(iot_nodemodel* node) {
	if(node->curvalueinput) {
		for(unsigned i=0;i<node->node_iface->num_valueinputs;i++) {
			if(node->curvalueinput[i].instance_value) node->curvalueinput[i].instance_value->release();
		}
		for(unsigned i=0;i<node->node_iface->num_valueoutputs;i++) {
			if(node->curvalueoutput[i].instance_value) node->curvalueoutput[i].instance_value->release();
		}
		iot_release_memblock(node->curvalueinput);
		node->curvalueinput=NULL;
		node->curvalueoutput=NULL;
		node->curmsginput=NULL;
		node->curmsgoutput=NULL;
	}
	if(node->modinstlk) node->modinstlk.unlock();
	iot_release_memblock(node);
}

void iot_nodemodel::on_instance_start(iot_modinstance_item_t* modinst) {
	assert(uv_thread_self()==main_thread);

	assert(modinstlk.modinst==modinst);
	assert(state==NODESTATE_WAITSTART || state==NODESTATE_NOINSTANCE);

	state=NODESTATE_STARTED;

	if(node_iface->is_sync && node_iface->num_valueoutputs+node_iface->num_msgoutputs>0) {
		if(modinst->cpu_loading==0 && modinst->thread==main_thread_item) is_sync=2; //initial conditions for simple sync mode satisfied
		else is_sync=1;
	} else is_sync=0;

}

void iot_nodemodel::on_instance_destroy(iot_nodemodel* node, iot_modinstance_item_t* modinst) { //called when node instance is released
	assert(uv_thread_self()==main_thread);

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
	assert(uv_thread_self()==main_thread);
	if(cfgitem) { //detach from configuration
		cfgitem->nodemodel=NULL;
		cfgitem=NULL;
	}
	is_sync=2;
	switch(state) {
		case NODESTATE_NOMODULE: //model started and is in degenerated state, no required code module found (module==NULL)
		case NODESTATE_NOINSTANCE: //model started but no module instance created (modinstlk is false)
			return true;
		case NODESTATE_STARTED: //model started and module instance is active
		case NODESTATE_WAITSTART: //model started and module instance is starting
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

bool iot_nodemodel::execute(bool isasync, iot_threadmsg_t *&msg, iot_modelsignal *&outsignals) {
	//true isasync requests async execution. this turns off simple sync mode if enabled for started nodes
	//returns true in sync mode if outsignals is actual, i.e. reply is immediate
	assert(uv_thread_self()==main_thread);
	assert(msg!=NULL && msg->code==IOT_MSG_NOTIFY_INPUTSUPDATED && msg->data!=NULL && msg->is_releasable);

	iot_notify_inputsupdate* notifyupdate=static_cast<iot_notify_inputsupdate*>((iot_releasable*)msg->data);
	assert(notifyupdate->numitems>0);

	if(state!=NODESTATE_STARTED) {
		notifyupdate->releasedata();
		//keep allocated msg with empty but valid iot_notify_inputsupdate object pointer inside
		if(!is_sync) return false;
		return true; //to say immediately there are no changes in outputs
	}
	assert(modinstlk.modinst!=NULL);

	msg->miid=modinstlk.modinst->get_miid();

	if(!is_sync) {
//		assert(!is_sync); //sync modules should not be called in async mode
		if(is_sync==2) is_sync=1; //disable simple sync mode (in release build)
		msg->bytearg=1;

		modinstlk.modinst->thread->send_msg(msg);
		msg=NULL;
		return false;
	}
	//sync mode
	if(is_sync==1) { //delayed sync execution
		outlog_debug_modelling("Scheduling delayed sync execution for node %" IOT_PRIiotid, node_id);
		msg->bytearg=0;
		modinstlk.modinst->thread->send_msg(msg);
		msg=NULL;
		return false;
	}
	//simple sync mode
	assert(modinstlk.modinst->thread==main_thread_item);

	outlog_debug_modelling("Doing simple sync execution for node %" IOT_PRIiotid, node_id);
	return do_execute(false, msg, outsignals);
}

bool iot_nodemodel::do_execute(bool isasync, iot_threadmsg_t *&msg, iot_modelsignal *&outsignals) {
	//returns true if outsignals is actual, i.e. reply is immediate
	assert(state==NODESTATE_STARTED);
	iot_modinstance_item_t *modinst=modinstlk.modinst;
	assert(uv_thread_self()==modinst->thread->thread);

	assert(msg!=NULL && msg->code==IOT_MSG_NOTIFY_INPUTSUPDATED && msg->data!=NULL && msg->is_releasable);

	iot_notify_inputsupdate* notifyupdate=static_cast<iot_notify_inputsupdate*>((iot_releasable*)msg->data);
	assert(notifyupdate->numitems>0);


	iot_node_base::iot_value_signal valuesignals[node_iface->num_valueinputs>0 ? node_iface->num_valueinputs : 1];
	bool valueset[node_iface->num_valueinputs>0 ? node_iface->num_valueinputs : 1]; //keep track of used real indexes for values

	//init valuesignals as if there were no updates
	for(uint16_t i=0; i<node_iface->num_valueinputs; i++) {
		valuesignals[i]={curvalueinput[i].instance_value, curvalueinput[i].instance_value};
		valueset[i]=false;
	}

	uint16_t nummsgs=0; //counts how many msgs in notifyupdate
	//fill value updates and count msgs in parallel
	for(uint16_t i=0; i<notifyupdate->numitems; i++) {
		auto item=&notifyupdate->item[i];
		auto j=item->real_index;
		if(item->is_msg) { //msg signal
			assert(item->data!=NULL);
			if(item->data!=NULL) nummsgs++;
		} else { //value signal
			if(j>=node_iface->num_valueinputs) {
				assert(false);
				continue;
			}
			//check value type is compatible
			if(!node_iface->valueinput[j].is_compatible(item->data)) {
				outlog_debug_modelling("New value for input %u of node %" IOT_PRIiotid " is not compatible with config (is type '%s', must be type '%s')", unsigned(j), node_id, item->data ? item->data->get_typename() : "UNDEF", node_iface->valueinput[j].dataclass->type_name);
				continue;
			}
			if(item->data==valuesignals[j].prev_value || (item->data && valuesignals[j].prev_value && *(item->data)==*(valuesignals[j].prev_value))) continue; //value unchanged
			if(valueset[j]) {
				assert(false);
				if(curvalueinput[j].instance_value) curvalueinput[j].instance_value->release(); //be ready to bug and correctly release unnecessary ref
			} else {
				valueset[j]=true;
			}
			curvalueinput[j].instance_value = valuesignals[j].new_value = item->data;
			item->data=NULL; //value moved from notifyupdate to instance_value, so no refcount change (valuesignals structure is temporary and not accounted)
		}
	}

	//now msgs
	iot_node_base::iot_msg_signal msgsignals[node_iface->num_msginputs>0 ? node_iface->num_msginputs : 1];
	memset(msgsignals, 0, node_iface->num_msginputs*sizeof(iot_node_base::iot_msg_signal));
	const iot_datavalue* msgs[nummsgs > 0 ? nummsgs : 1];
	uint16_t msgidx=0;

	for(uint16_t i=0; i<notifyupdate->numitems; i++) {
		auto item=&notifyupdate->item[i];
		auto j=item->real_index;
		if(!item->is_msg || !item->data) continue;
		if(j>=node_iface->num_msginputs) {
			assert(false);
			continue;
		}
		if(msgsignals[j].num==0) { 
			msgsignals[j].msgs=&msgs[msgidx];
		} else {
			assert(i>0 && notifyupdate->item[i-1].real_index==j); //msgs with same real_index MUST GO SEQUENTIALLY!!!
			continue;
		}
		msgs[msgidx]=item->data;
		item->data=NULL;
		msgidx++;
		msgsignals[j].num++;
	}

	iot_event_id_t eventid=notifyupdate->reason_event;
	if(isasync) {
		syncexec.clear();
		iot_release_msg(msg); //will call releasedata for notifyupdate and release uncleared data pointers in it
		msg=NULL;
	} else {
		syncexec.init(notifyupdate, msg);
		notifyupdate->releasedata(); //will release uncleared data pointers in notifyupdate
	}

	int err=static_cast<iot_node_base*>(modinst->instance)->process_input_signals(eventid, node_iface->num_valueinputs, valuesignals, node_iface->num_msginputs, msgsignals);
	outlog_debug_modelling("Got error %d from process_input_signals of node %" IOT_PRIiotid, err, node_id);
	//now release previous values (instance will incref if it needs to keep some)
	for(uint16_t i=0; i<node_iface->num_valueinputs; i++)
		if(valuesignals[i].prev_value && valuesignals[i].prev_value!=valuesignals[i].new_value) valuesignals[i].prev_value->release();

//	if(err && err!=IOT_ERROR_NOT_READY) TODO

	if(isasync) return false;
	//sync

	if(!syncexec.active()) { //there were several calls to do_update_outputs() or one call with non-current reason_event, so reply was sent using msg
		msg=NULL;
		if(modinst->thread==main_thread_item && is_sync==2) is_sync=1; //disable simple mode if it was enabled

		return false;
	}
	//was one call to do_update_outputs() or none
	if(err==IOT_ERROR_NOT_READY && modinst->thread==main_thread_item && is_sync==2) is_sync=1; //disable simple mode if it was enabled
	if(syncexec.result_set()) { //was one call
		outsignals=syncexec.result_signals;
		syncexec.clear_result();
		err=0; //to force return true
	} else outsignals=NULL;
	syncexec.msg=NULL; //detach msg pointer to avoid release by clear()
	syncexec.clear();
	return err==0 ? true : false;
}

int iot_nodemodel::do_update_outputs(const iot_event_id_t *reason_eventid, uint8_t num_values, const uint8_t *valueout_indexes, const iot_datavalue** values, uint8_t num_msgs, const uint8_t *msgout_indexes, const iot_datavalue** msgs) {
	assert(modinstlk.modinst!=NULL);
	assert(uv_thread_self()==modinstlk.modinst->thread->thread);
	auto allocator=modinstlk.modinst->thread->allocator;
	uint64_t tm=uv_now(modinstlk.modinst->thread->loop);

	if(num_values>node_iface->num_valueoutputs || num_msgs>node_iface->num_msgoutputs) return IOT_ERROR_INVALID_ARGS;
	//check types and indexes
	bool usedval[node_iface->num_valueoutputs>0 ? node_iface->num_valueoutputs : 1]={}; //track repeated value indexes
	for(uint8_t i=0;i<num_values;i++) {
		if(valueout_indexes[i]>=node_iface->num_valueoutputs) return IOT_ERROR_INVALID_ARGS;
		if(usedval[valueout_indexes[i]]) return IOT_ERROR_INVALID_ARGS; //repeated specification of same output
		usedval[valueout_indexes[i]]=true;
		if(!node_iface->valueoutput[valueout_indexes[i]].is_compatible(values[i])) return IOT_ERROR_INVALID_ARGS;
	}
	bool usedmsg[node_iface->num_msgoutputs>0 ? node_iface->num_msgoutputs : 1]={}; //track repeated msg indexes
	for(uint8_t i=0;i<num_msgs;i++) {
		if(msgout_indexes[i]>=node_iface->num_msgoutputs) return IOT_ERROR_INVALID_ARGS;
		if(usedmsg[msgout_indexes[i]]) return IOT_ERROR_INVALID_ARGS; //repeated specification of same output
		usedmsg[msgout_indexes[i]]=true;
		if(!node_iface->msgoutput[msgout_indexes[i]].is_compatible(msgs[i])) return IOT_ERROR_INVALID_ARGS;
	}

	bool sync; //2 - simple sync
	if(syncexec.active()) { //this is sync processing
		if(!syncexec.result_set() && reason_eventid && syncexec.event_id==*reason_eventid) { //this is first call to this func
			sync=true;//modinstlk.modinst->thread==main_thread_item ? sync : 1; //this->is_sync can be accessed from main thread only
		} else { //this is repeated call or with another reason event
			outlog_debug_modelling("Node %" IOT_PRIiotid " sets output for event %" PRIu64 " during sync execution of event %" PRIu64, reason_eventid ? reason_eventid->numerator : 0, syncexec.event_id);
			sync=false;
			if(syncexec.result_set()) { //there was first call with correct reason event. must send it out
				iot_release_msg(syncexec.msg, true); //leave only msg struct
				int err;
				if(syncexec.result_signals) {
					err=iot_prepare_msg_releasable(syncexec.msg, IOT_MSG_EVENTSIG_OUT, NULL, 0, static_cast<iot_releasable*>(syncexec.result_signals), 0, IOT_THREADMSG_DATAMEM_MEMBLOCK_NOOPT, true);
				} else { //empty updates
					iot_modelnegsignal neg={event_id : syncexec.event_id, node_id : node_id};
					err=iot_prepare_msg(syncexec.msg, IOT_MSG_EVENTSIG_NOUPDATE, NULL, 0, &neg, sizeof(neg), IOT_THREADMSG_DATAMEM_TEMP_NOALLOC, true);
				}
				assert(err==0);
				main_thread_item->send_msg(syncexec.msg);
				syncexec.msg=NULL;
				syncexec.clear_result();
			} //else do nothing special. instance can send correct reply later or in another call
			syncexec.clear();
		}
	} else sync=false;

	//allocate signal structs and necessary msg/value objects
	iot_modelsignal* outsignals=NULL; //in case of memory error this list will be released together with data values
	const iot_datavalue* newvalues[node_iface->num_valueoutputs>0 ? node_iface->num_valueoutputs : 1];
	for(uint8_t i=0;i<node_iface->num_valueoutputs;i++) newvalues[i]=curvalueoutput[i].instance_value; //init new values from current instance value

	for(uint8_t i=0;i<num_values;i++) {
		const iot_datavalue* &newvalue=newvalues[valueout_indexes[i]];
		if(values[i]==newvalue || (newvalue && values[i] && *(values[i])==*newvalue)) continue; //no change
		//output updated

		//allocate memory for data if necessary
		if(values[i] && !values[i]->is_static) { //need allocation
			void *mem=allocator->allocate(values[i]->get_size(), true);
			if(!mem) goto nomem;
			newvalue=values[i]->copyTo(mem, values[i]->get_size(), true);
			assert(newvalue!=NULL);
		} else newvalue=values[i];

		iot_modelsignal* sig;
		if(sync && syncexec.prealloc_signals) { //there is preallocated signal struct, move it to outsignals
			sig=syncexec.prealloc_signals;
			syncexec.prealloc_signals=sig->next;
		} else { //need allocation
			sig=(iot_modelsignal*)allocator->allocate(sizeof(iot_modelsignal));
			if(!sig) {
				if(newvalue) newvalue->release();
				goto nomem;
			}
		}
		sig=new(sig) iot_modelsignal(this, node_iface->valueoutput[valueout_indexes[i]].label, false, tm, newvalue, /*sync!=0,*/ reason_eventid);
		sig->next=outsignals;
		outsignals=sig;
	}
	for(uint8_t i=0;i<num_msgs;i++) {
		//allocate memory for data msg if necessary
		assert(msgs[i]!=NULL);
		const iot_datavalue* newmsg;
		if(!msgs[i]->is_static) { //need allocation
			void *mem=allocator->allocate(msgs[i]->get_size(), true);
			if(!mem) goto nomem;
			newmsg=msgs[i]->copyTo(mem, msgs[i]->get_size(), true);
			assert(newmsg!=NULL);
		} else newmsg=msgs[i];

		iot_modelsignal* sig;
		if(sync && syncexec.prealloc_signals) { //there is preallocated signal struct, move it to outsignals
			sig=syncexec.prealloc_signals;
			syncexec.prealloc_signals=sig->next;
		} else { //need allocation
			sig=(iot_modelsignal*)allocator->allocate(sizeof(iot_modelsignal));
			if(!sig) {
				if(newmsg) newmsg->release();
				goto nomem;
			}
		}
		sig=new(sig) iot_modelsignal(this, node_iface->msgoutput[msgout_indexes[i]].label, true, tm, newmsg, /*sync!=0,*/ reason_eventid);
		sig->next=outsignals;
		outsignals=sig;
	}

	if(!sync) { //allocate, fill and send thread msg
		iot_threadmsg_t* msg=NULL;
		int err=0;
		if(outsignals) {
			err=iot_prepare_msg_releasable(msg, IOT_MSG_EVENTSIG_OUT, NULL, 0, static_cast<iot_releasable*>(outsignals), 0, IOT_THREADMSG_DATAMEM_MEMBLOCK_NOOPT, true, allocator);
		} else if(reason_eventid && reason_eventid->numerator>0 && node_iface->is_sync) { //this can be disordered reply to old sync execution
			iot_modelnegsignal neg={event_id : *reason_eventid, node_id : node_id};
			err=iot_prepare_msg(msg, IOT_MSG_EVENTSIG_NOUPDATE, NULL, 0, &neg, sizeof(neg), IOT_THREADMSG_DATAMEM_TEMP_NOALLOC, true, allocator);
		}
		if(err) {
			if(err==IOT_ERROR_NO_MEMORY) goto nomem;
			assert(false);
		} else if(msg){
			main_thread_item->send_msg(msg);
		}
	}
	//no errors after this point

	//update refcounts and write instance_values
	for(uint8_t i=0;i<num_values;i++) {
		auto &instance_value=curvalueoutput[valueout_indexes[i]].instance_value;
		const iot_datavalue* newvalue=newvalues[valueout_indexes[i]];
		if(instance_value==newvalue) continue;

		if(newvalue) newvalue->incref(); //value is now copied to sig, so will have 2 refs
		if(instance_value) instance_value->release();

		instance_value=newvalue;
	}

	if(sync) {
		syncexec.result_signals=outsignals;
	}
	return 0;
nomem:
	if(outsignals) iot_modelsignal::release(outsignals);
	return IOT_ERROR_NO_MEMORY;
}

const iot_datavalue* iot_nodemodel::get_outputvalue(uint8_t index) {
	assert(modinstlk.modinst!=NULL);
	assert(uv_thread_self()==modinstlk.modinst->thread->thread);
	if(index>=node_iface->num_valueoutputs) return NULL;
	return curvalueoutput[index].instance_value;
}


iot_modelsignal::iot_modelsignal(iot_nodemodel *model, const char* out_labeln, bool is_msg, uint64_t reltime, const iot_datavalue* msgval, /*bool is_sync,*/ const iot_event_id_t* reason) : 
	reltime(reltime), data(msgval)/*, is_sync(is_sync)*/ {
		assert(model->cfgitem!=NULL);
		node_id=model->node_id;
		module_id=model->cfgitem->module_id;
		if(reason) reason_event=*reason;
		else {
//			assert(!is_sync); //reason required for sync signals
			reason_event={};
		}
//		if(!out_labeln) { //special sync signal telling 'no change'
//			assert(is_sync && !msgval);
//			out_label[0]='\0';
//		} else {
			out_label[0]=is_msg ? 'm' : 'v';
			snprintf(out_label+1, sizeof(out_label)-1, "%s", out_labeln); //out_labeln is without type prefix
//		}
	}

