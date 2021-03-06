#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <string.h>


//#include "iot_compat.h"
#include "iot_core.h"
#include "iot_devclass_keyboard.h"
#include "iot_devclass_activatable.h"
#include "iot_devclass_meter.h"
#include "iot_hwdevcontype_serial.h"
#include "iot_hwdevcontype_zwave.h"
#include "iot_hwdevcontype_gpio.h"
//#include "iot_devclass_toneplayer.h"

#include "iot_deviceregistry.h"
#include "iot_moduleregistry.h"
#include "iot_configmodel.h"
#include "iot_threadregistry.h"


extern uv_loop_t *main_loop;

const uint32_t iot_core_version=IOT_CORE_VERSION;
uint32_t IOT_ABI_TOKEN_NAME=IOT_CORE_VERSION;


uint32_t iot_parse_version(const char* s) {
	uint32_t vers, patch, rev;
	const char* semicol=strchr(s, ':');
	const char* dot;
	char* end;
	if(semicol) {
		rev=iot_strtou32(semicol+1, &end, 10);
		if(rev>65535 || (!rev && end==semicol+1) || *end!='\0') return UINT32_MAX; //error
		dot=(const char*)memchr(s, '.', semicol-s);
	} else {
		rev=0;
		dot=strchr(s, '.');
	}
	if(!dot) {
		patch=0;
	} else {
		patch=iot_strtou32(dot+1, &end, 10);
		if(patch>255 || (!patch && end==dot+1)) return UINT32_MAX; //error
	}
	vers=iot_strtou32(s, &end, 10);
	if(vers>255) return UINT32_MAX; //error
	return IOT_VERSION_COMPOSE(vers, patch, rev);
}


void iot_gen_random(char* buf, uint16_t len) { //temporary, gives very bad random
	static bool inited=false;
	if(!inited) {
		time_t t=time(NULL);
		srandom((unsigned int)(t));
		inited=true;
	}
	long int r;
	while(len>sizeof(int)) {
		r=random();
		memcpy(buf, &r, sizeof(int));
		buf+=sizeof(int);
		len-=sizeof(int);
	}
	if(len>0) {
		r=random();
		memcpy(buf, &r, len);
	}
}

int iot_devifaces_list::add(const iot_deviface_params *cls) {
		if(num>=IOT_CONFIG_MAX_IFACES_PER_DEVICE) return IOT_ERROR_LIMIT_REACHED;
		if(!cls) return IOT_ERROR_INVALID_ARGS;
		if(!cls->is_valid()) {
			char buf[128];
			outlog_info("Cannot use device iface type '%s' without assigned ID", cls->sprint(buf, sizeof(buf)));
			return IOT_ERROR_INVALID_ARGS;
		}
		items[num]=cls;
		num++;
		return 0;
}



//Used by device detector modules for managing registry of available hardware devices
//action is one of {IOT_ACTION_ADD, IOT_ACTION_REMOVE}
//contype must be among those listed in .devcontypes field of detector module interface
//returns:
//	IOT_ERROR_NOT_SUPPORTED - driver instance tries to manage device registry not having 'is_detector' flag in module config
//	IOT_ERROR_INVALID_STATE - acting instance is not started
//	IOT_ERROR_INVALID_ARGS - provided ident is template or action unknown
//	IOT_ERROR_NOT_FOUND - interface to ident's data not found (invalid or module cannot be loaded)
//	IOT_ERROR_TEMPORARY_ERROR - some temporary error (no memory etc). retry can succeed
//
int iot_device_detector_base::kapi_hwdev_registry_action(enum iot_action_t action, const iot_hwdev_localident* ident, iot_hwdev_details* custom_data) {
	//TODO check that ident->contype is listed in .devcontypes field of detector module interface
	if(!miid) return IOT_ERROR_INVALID_STATE;
	iot_objref_ptr<iot_modinstance_item_t> modinstlk=modules_registry->get_modinstance(miid);
	if(!modinstlk) return IOT_ERROR_INVALID_STATE;

	if(modinstlk->type!=IOT_MODTYPE_DETECTOR) {
		if(modinstlk->type!=IOT_MODTYPE_DRIVER || !((iot_driver_module_item_t*)modinstlk->module)->config->is_detector) return IOT_ERROR_NOT_SUPPORTED; //this driver has no detector's functionality
	}

	hwdev_registry_t* reg;

	if(!modinstlk->gwinst) { //or may be just look at ident->guid?
		assert(false);
#ifdef IOT_SERVER
		//TODO. allow detectors on server to find devices for any instance
		//reg=iot_gwinstance::find(ident->guid)->hwdev_registry;
#endif
	} else {
		reg=modinstlk->gwinst->hwdev_registry;
	}

	return reg->list_action(miid, action, ident, custom_data);
}

void iot_module_instance_base::kapi_process_uv_close(uv_handle_t *handle) {
		iot_module_instance_base* inst=(iot_module_instance_base*)handle->data;
		assert(inst!=NULL && inst->thread==uv_thread_self());
		inst->unref(); //inside this call instance object can be deallocated!
	}


int iot_module_instance_base::kapi_lib_rundir(char *buffer, size_t buffer_size, bool create) {
	if(!buffer || buffer_size<2) return IOT_ERROR_INVALID_ARGS;
	if(!miid) return IOT_ERROR_INVALID_STATE;
	iot_objref_ptr<iot_modinstance_item_t> modinstlk=modules_registry->get_modinstance(miid);
	if(!modinstlk || modinstlk->instance!=this) {
		assert(false);
		return IOT_ERROR_CRITICAL_BUG;
	}
	int rval=snprintf(buffer, buffer_size, "%s/%s", run_dir, modinstlk->module->dbitem->bundle->name);
	if(rval<0) return IOT_ERROR_INVALID_ARGS;
	if(size_t(rval)>=buffer_size) { //output was truncated, creation not possible
		return int(rval)-1;
	}
	if(!create) return rval+1;
	struct stat st;
	if(stat(buffer, &st)==0) {
		if(!S_ISDIR(st.st_mode)) {
			errno=ENOTDIR;
			return IOT_ERROR_CRITICAL_ERROR;
		}
		return rval+1; //dir exists
	}
	//here dir does not exists
	char *p=buffer+strlen(run_dir)+1; //set pointer just after 'run_dir/' part in buffer
	for(int i=0; i<2; i++) {
		p=strchr(p, '/'); //find end of 'run_dir/vendor' (i==0) or 'run_dir/vendor/dir' (i==1)
		if(!p) return IOT_ERROR_CRITICAL_BUG; //something is wrong with assumption about bundle->name structure
		*p='\0';
		if(mkdir(buffer, 0755)<0 && errno!=EEXIST) {
			int err=errno;
			outlog_error("Cannot create directory '%s': %s", p, uv_strerror(uv_translate_sys_error(err)));
			*p='/';
			return IOT_ERROR_CRITICAL_ERROR;
		}
		*p='/';
		p++;
	}
	//create the last level
	if(mkdir(buffer, 0755)<0 && errno!=EEXIST) {
		int err=errno;
		outlog_error("Cannot create directory '%s': %s", buffer, uv_strerror(uv_translate_sys_error(err)));
		return IOT_ERROR_CRITICAL_ERROR;
	}
	return rval+1;
}

int iot_module_instance_base::kapi_lib_datadir(char *buffer, size_t buffer_size, const char* libname) {
	if(!buffer || buffer_size<2) return IOT_ERROR_INVALID_ARGS;
	if(!miid) return IOT_ERROR_INVALID_STATE;
	iot_objref_ptr<iot_modinstance_item_t> modinstlk=modules_registry->get_modinstance(miid);
	if(!modinstlk || modinstlk->instance!=this) {
		assert(false);
		return IOT_ERROR_CRITICAL_BUG;
	}
	int rval;
	if(!libname) { //get datadir of current instance library
		rval=snprintf(buffer, buffer_size, "%s/%s", modules_dir, modinstlk->module->dbitem->bundle->name);
	} else {
		iot_regitem_lib_t* libitem=iot_regitem_lib_t::find_item(libname);
		if(!libitem) return IOT_ERROR_NOT_FOUND;
		rval=snprintf(buffer, buffer_size, "%s/%s", modules_dir, libitem->name);
	}
	if(rval<0) return IOT_ERROR_INVALID_ARGS;
	if(size_t(rval)>=buffer_size) { //output was truncated, creation not possible
		return int(rval)-1;
	}
	return rval+1;
}



//accepted error codes:
//	0 - for case of delayed stop (state of modinst must be IOT_MODINSTSTATE_STOPPING)
//	IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be disabled
//	IOT_ERROR_TEMPORARY_ERROR - for driver instances device communication problem. module is temporary blocked from using for current device
//	IOT_ERROR_CRITICAL_ERROR - instanciation is invalid. driver module must not be used for current device, other module instances must not be used for current iot config
//other are equivalent to IOT_ERROR_CRITICAL_BUG

//returned status:
//	0 - success, instance IS queuered to be stopped or is not running.
//	IOT_ERROR_NOT_READY
int iot_module_instance_base::kapi_self_abort(int errcode) { //can be called in modinstance thread
	iot_objref_ptr<iot_modinstance_item_t> modinstlk=modules_registry->get_modinstance(miid);
	if(!modinstlk || modinstlk->instance!=this) {
		assert(false);
		return 0;
	}
	iot_modinstance_item_t* modinst=(iot_modinstance_item_t*)modinstlk;
	assert(uv_thread_self()==modinst->thread->thread);

	auto state=modinst->state;
	if(!modinst->is_working()) return 0;

	if((errcode==0 && state!=IOT_MODINSTSTATE_STOPPING) ||
					(errcode!=IOT_ERROR_CRITICAL_BUG && errcode!=IOT_ERROR_TEMPORARY_ERROR && errcode!=IOT_ERROR_CRITICAL_ERROR)) {
		errcode=IOT_ERROR_CRITICAL_BUG;
		auto module=modinst->module;
		outlog_error("kapi_self_abort() called by driver instance of module '%s::%s' with illegal error '%s' (%d). This is a bug in module", module->dbitem->bundle->name, module->dbitem->module_name, kapi_strerror(errcode), errcode);
	}
	modinst->aborted_error=errcode;
	return modinst->stop(false, true);
}

int iot_node_base::kapi_update_outputs(const iot_event_id_t *reason_eventid, uint8_t num_values, const uint8_t *valueout_indexes, const iot_datavalue** values, uint8_t num_msgs, const uint8_t *msgout_indexes, const iot_datavalue** msgs) {
	iot_objref_ptr<iot_modinstance_item_t> modinstlk=modules_registry->get_modinstance(miid);
	if(!modinstlk || modinstlk->instance!=this) {
		assert(false);
		return 0;
	}
	iot_modinstance_item_t* modinst=(iot_modinstance_item_t*)modinstlk;
	assert(uv_thread_self()==modinst->thread->thread);

	if(!modinst->is_working()) return 0;
	auto model=modinst->data.node.model;
	if(!model->cfgitem) return 0; //model is being stopped and is already detached from config item. ignore signal.

	return model->do_update_outputs(reason_eventid, num_values, valueout_indexes, values, num_msgs, msgout_indexes, msgs);
}

const iot_datavalue* iot_node_base::kapi_get_outputvalue(uint8_t index) {
	iot_objref_ptr<iot_modinstance_item_t> modinstlk=modules_registry->get_modinstance(miid);
	if(!modinstlk || modinstlk->instance!=this) {
		assert(false);
		return 0;
	}
	iot_modinstance_item_t* modinst=(iot_modinstance_item_t*)modinstlk;
	assert(uv_thread_self()==modinst->thread->thread);

	if(!modinst->is_working()) return 0;
	auto model=modinst->data.node.model;

	return model->get_outputvalue(index);
}


uv_loop_t* kapi_get_event_loop(uv_thread_t thread) {
	return thread_registry->find_loop(thread);
}


const char* kapi_strerror(int err) {
	switch(err) {
#define XX(nm, cc, txt) case cc: return txt;
		IOT_ERROR_MAP(XX)
#undef XX
	}
	return "UNKNOWN ERROR";
}

const char* kapi_err_name(int err) {
	switch(err) {
#define XX(nm, cc, txt) case cc: return #nm;
		IOT_ERROR_MAP(XX)
#undef XX
	}
	return "UNKNOWN";
}

iot_hwdevcontype_metaclass::iot_hwdevcontype_metaclass(iot_type_id_t id, /*const char* vendor, */const char* type, uint32_t ver_, const char* parentlib_) :
				version(ver_), type_name(type), parentlib(parentlib_) {
	assert(type!=NULL && parentlib_!=NULL);

	iot_hwdevcontype_metaclass* &head=iot_libregistry_t::devcontype_pendingreg_head(), *cur;
	cur=head;
	while(cur) {
		if(cur==this) {
			outlog_error("Double instanciation of Device Connection Type %s!", type);
			assert(false);
			return;
		}
		cur=cur->next;
	}
	next=head;
	head=this;

	prev=NULL;
	contype_id=id;
//	vendor_name=vendor;
//	type_name=type;
//	ver=ver_;
//	parentlib=parentlib_;
}



int iot_hwdevcontype_metaclass::from_json(json_object* json, char* buf, size_t bufsize, const iot_hwdev_localident*& obj, iot_type_id_t default_contype) {
	json_object* val=NULL;
	iot_type_id_t contype=default_contype;
	if(json_object_object_get_ex(json, "contype_id", &val)) { //zero or absent value leaves IOT_DEVCONTYPE_ANY
		IOT_JSONPARSE_UINT(val, iot_type_id_t, contype);
	}
	if(!contype) return IOT_ERROR_BAD_DATA;

	const iot_hwdevcontype_metaclass* metaclass=iot_hwdevcontype_metaclass::findby_id(contype);
	if(!metaclass) return IOT_ERROR_NOT_FOUND;

	val=NULL;
	json_object_object_get_ex(json, "ident", &val);
	obj=NULL;
	return metaclass->p_from_json(val, buf, bufsize, obj);
}

const iot_hwdevcontype_metaclass* iot_hwdevcontype_metaclass::findby_id(iot_type_id_t contype_id, bool try_load) {
	return libregistry->find_devcontype(contype_id, try_load);
}


/*int iot_hwdevcontype_metaclass_any::p_deserialized_size(const char* data, size_t datasize, const iot_hwdev_localident*& obj) const {
	if(datasize>0) return IOT_ERROR_BAD_DATA;
	//for static object no additional memory required
	obj=&iot_hwdev_localident_any::object;
	return 0; //localident of this metaclass is singleton
}
*/
int iot_hwdevcontype_metaclass_any::p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const {
	if(datasize>0) return IOT_ERROR_BAD_DATA;
	//for static object no additional memory required
	obj=&iot_hwdev_localident_any::object;
	return 0; //zero means that obj was reassigned to statically allocated object
}

int iot_hwdevcontype_metaclass_any::p_from_json(json_object* json, char* buf, size_t bufsize, const iot_hwdev_localident*& obj) const {
	if(json && !json_object_is_type(json, json_type_null) && (!json_object_is_type(json, json_type_object) || json_object_object_length(json)>0)) return IOT_ERROR_BAD_DATA; //must be absent or be empty object
	//for static object no additional memory required
	obj=&iot_hwdev_localident_any::object;
	return 0; //zero means that obj was reassigned to statically allocated object
}

iot_hwdevcontype_metaclass_any iot_hwdevcontype_metaclass_any::object;
const iot_hwdev_localident_any iot_hwdev_localident_any::object;

int iot_hwdev_ident::from_json(json_object* json, iot_hwdev_ident* obj, char* identbuf, size_t bufsize, iot_hostid_t default_hostid, iot_type_id_t default_contype) { //obj must point to somehow (from stack etc.) allocated iot_hwdev_ident struct
	//returns negative error code OR number of bytes written to provided  identobj buffer OR required buffer size if obj was NULL

	json_object* val=NULL;
	iot_hostid_t hostid=default_hostid;
	if(json_object_object_get_ex(json, "host_id", &val)) { //zero or absent value leaves default_hostid
		IOT_JSONPARSE_UINT(val, iot_hostid_t, hostid);
	}

	int rval;
	const iot_hwdev_localident* localp;
	if(!obj) { //request to calculate bufsize
		localp=NULL;
		rval=iot_hwdevcontype_metaclass::from_json(json, NULL, 0, localp, default_contype);
		return rval;
	}
	localp=(iot_hwdev_localident*)identbuf;

	rval=iot_hwdevcontype_metaclass::from_json(json, identbuf, bufsize, localp, default_contype); //can return 0 and reassign localp to address of static object

	if(rval<0) return rval;
	//all is good
	obj->hostid=hostid;
	obj->local=localp;
	return rval;
}




iot_devifacetype_metaclass::iot_devifacetype_metaclass(iot_type_id_t id, /*const char* vendor, */const char* type, uint32_t ver_, const char* parentlib_) : 
				version(ver_), type_name(type), parentlib(parentlib_) {
	assert(type!=NULL && parentlib_!=NULL);

	iot_devifacetype_metaclass* &head=iot_libregistry_t::devifacetype_pendingreg_head(), *cur;
	cur=head;
	while(cur) {
		if(cur==this) {
			outlog_error("Double instanciation of Device Iface Type %s!", type);
			assert(false);
			return;
		}
		cur=cur->next;
	}
	next=head;
	head=this;

	prev=NULL;
	ifacetype_id=id;
//	vendor_name=vendor;
//	type_name=type;
//	ver=ver_;
//	parentlib=parentlib_;
}
const iot_devifacetype_metaclass* iot_devifacetype_metaclass::findby_id(iot_type_id_t ifacetype_id, bool try_load) {
	return libregistry->find_devifacetype(ifacetype_id, try_load);
}

int iot_devifacetype_metaclass::from_json(json_object* json, char* buf, size_t bufsize, const iot_deviface_params*& obj, iot_type_id_t default_ifacetype) {
	json_object* val=NULL;
	iot_type_id_t ifacetype=default_ifacetype;
	if(json_object_object_get_ex(json, "ifacetype_id", &val)) {
		IOT_JSONPARSE_UINT(val, iot_type_id_t, ifacetype);
	}
	if(!ifacetype) return IOT_ERROR_BAD_DATA;

	const iot_devifacetype_metaclass* metaclass=iot_devifacetype_metaclass::findby_id(ifacetype, true);
	if(!metaclass) return IOT_ERROR_NOT_FOUND;

	val=NULL;
	json_object_object_get_ex(json, "params", &val);
	obj=NULL;
	return metaclass->p_from_json(val, buf, bufsize, obj);
}
int iot_devifacetype_metaclass::to_json(const iot_deviface_params* obj, json_object* &dst) const {
	assert(ifacetype_id!=0 && obj);

	json_object* ob=json_object_new_object();
	if(!ob) return IOT_ERROR_NO_MEMORY;
	json_object* idob=json_object_new_int64(ifacetype_id);
	if(!idob) {
		json_object_put(ob);
		return IOT_ERROR_NO_MEMORY;
	}
	json_object_object_add(ob, "ifacetype_id", idob);
	json_object* paramsob=NULL;
	int err=p_to_json(obj, paramsob);
	if(err) {
		json_object_put(ob);
		return err;
	}
	if(paramsob) { //can stay NULL if iface type has no params
		json_object_object_add(ob, "params", paramsob);
	}
	dst=ob;
	return 0;
}

int iot_devifacetype_metaclass::serialize(const iot_deviface_params* obj, char* buf, size_t bufsize) const { //returns error code or 0 on success
	assert(ifacetype_id!=0 && obj);
	if(bufsize<sizeof(serialize_base_t)) return IOT_ERROR_NO_BUFSPACE;
	serialize_base_t *p=(serialize_base_t*)buf;
	p->ifacetype_id=repack_type_id(ifacetype_id);
	return p_serialize(obj, buf+sizeof(serialize_base_t), bufsize-sizeof(serialize_base_t));
}

iot_hwdevcontype_metaclass_serial iot_hwdevcontype_metaclass_serial::object;
iot_hwdevcontype_metaclass_zwave iot_hwdevcontype_metaclass_zwave::object;
iot_hwdevcontype_metaclass_gpio iot_hwdevcontype_metaclass_gpio::object;

iot_devifacetype_metaclass_keyboard iot_devifacetype_metaclass_keyboard::object;
iot_devifacetype_metaclass_activatable iot_devifacetype_metaclass_activatable::object;
iot_devifacetype_metaclass_meter iot_devifacetype_metaclass_meter::object;

iot_datatype_metaclass_boolean iot_datatype_metaclass_boolean::object;
iot_datatype_metaclass_nodeerrorstate iot_datatype_metaclass_nodeerrorstate::object;
iot_datatype_metaclass_bitmap iot_datatype_metaclass_bitmap::object;
iot_datatype_metaclass_numeric iot_datatype_metaclass_numeric::object;
iot_datatype_metaclass_pulse iot_datatype_metaclass_pulse::object;
//iot_devifacetype_toneplayer iot_devifacetype_toneplayer::object;

uint32_t iot_deviface_params_keyboard::get_d2c_maxmsgsize(void) const {
	assert(!istmpl);
	return iot_deviface__keyboard_BASE::get_maxmsgsize(spec.max_keycode);
}
uint32_t iot_deviface_params_keyboard::get_c2d_maxmsgsize(void) const {
	assert(!istmpl);
	return iot_deviface__keyboard_BASE::get_maxmsgsize(spec.max_keycode);
}

uint32_t iot_deviface_params_activatable::get_c2d_maxmsgsize(void) const {
	assert(!istmpl);
	return iot_deviface__activatable_BASE::get_maxmsgsize();
}
uint32_t iot_deviface_params_activatable::get_d2c_maxmsgsize(void) const {
	assert(!istmpl);
	return iot_deviface__activatable_BASE::get_maxmsgsize();
}

uint32_t iot_deviface_params_meter::get_c2d_maxmsgsize(void) const {
	assert(!istmpl);
return 0;
//	return iot_deviface__meter_BASE::get_maxmsgsize();
}
uint32_t iot_deviface_params_meter::get_d2c_maxmsgsize(void) const {
	assert(!istmpl);
return 0;
//	return iot_deviface__meter_BASE::get_maxmsgsize();
}


/*
uint32_t iot_devifacetype_toneplayer::get_c2d_maxmsgsize(const char* cls_data) const {
	return iot_deviface__toneplayer_BASE::get_maxmsgsize();
}
uint32_t iot_devifacetype_toneplayer::get_d2c_maxmsgsize(const char* cls_data) const {
	return iot_deviface__toneplayer_BASE::get_maxmsgsize();
}
*/


iot_datatype_metaclass::iot_datatype_metaclass(iot_type_id_t id, const char* type, uint32_t ver_, bool static_values, bool fixedsize_values, const char* parentlib_) : 
				version(ver_), type_name(type), parentlib(parentlib_), fixedsize_values(fixedsize_values), static_values(static_values) {
	assert(type!=NULL && parentlib_!=NULL);

	iot_datatype_metaclass* &head=iot_libregistry_t::datatype_pendingreg_head(), *cur;
	cur=head;
	while(cur) {
		if(cur==this) {
			outlog_error("Double instanciation of Data Type %s!", type);
			assert(false);
			return;
		}
		cur=cur->next;
	}
	next=head;
	head=this;

	prev=NULL;
	datatype_id=id;
//	vendor_name=vendor;
//	type_name=type;
//	ver=ver_;
//	parentlib=parentlib_;
}

iot_notiongroup_temperature::iot_notiongroup_temperature(void) : iot_notiongroup_numeric(0, "temperature", &iot_valuenotion_degcelcius::object, IOT_VERSION_COMPOSE(1,0,0)) {}
const iot_notiongroup_temperature iot_notiongroup_temperature::object;



iot_notiongroup::iot_notiongroup(iot_type_id_t id, const char* type, const iot_valuenotion *basic_notion, uint32_t ver_, const char* parentlib_) : 
				basic_notion(basic_notion), version(ver_), type_name(type), parentlib(parentlib_) {
	assert(type!=NULL && parentlib_!=NULL);

	iot_notiongroup* &head=iot_libregistry_t::notiongroup_pendingreg_head(), *cur;
	cur=head;
	while(cur) {
		if(cur==this) {
			outlog_error("Double instanciation of Notion Group %s!", type);
			assert(false);
			return;
		}
		cur=cur->next;
	}
	next=head;
	head=this;

	prev=NULL;
	notiongroup_id=id;
}

int iot_notiongroup::convert_datavalue(const iot_datavalue* srcval, const iot_valuenotion* srcnotion, const iot_valuenotion* resultnotion, char* resultbuf, size_t resultbufsize, const iot_datavalue*& resultval) const {
		//TODO validity checks
		if(!srcval || !srcnotion || !resultnotion || srcnotion->get_group()!=resultnotion->get_group() || srcnotion->get_group()!=this) return IOT_ERROR_INVALID_ARGS;
		if(resultnotion==srcnotion) {
			return IOT_ERROR_NO_ACTION;
		}
		return p_convert_datavalue(srcval, srcnotion, resultnotion, resultbuf, resultbufsize, resultval);
	}


iot_valuenotion::iot_valuenotion(iot_type_id_t id, const char* type, const iot_notiongroup *notion_group, uint32_t ver_, const char* parentlib_) : 
				notion_group(notion_group), version(ver_), type_name(type), parentlib(parentlib_) {
	assert(type!=NULL && parentlib_!=NULL);

	iot_valuenotion* &head=iot_libregistry_t::notiontype_pendingreg_head(), *cur;
	cur=head;
	while(cur) {
		if(cur==this) {
			outlog_error("Double instanciation of Value Notion %s!", type);
			assert(false);
			return;
		}
		cur=cur->next;
	}
	next=head;
	head=this;

	prev=NULL;
	notion_id=id;
}



//use constexpr to guarantee object is initialized at the time when constructors for global objects like configregistry are created
//const iot_valuetype_nodeerrorstate iot_valuetype_nodeerrorstate::const_noinst(iot_valuetype_nodeerrorstate::IOT_NODEERRORSTATE_NOINSTANCE);

//const iot_valuetype_boolean iot_valuetype_boolean::const_true(true);
//const iot_valuetype_boolean iot_valuetype_boolean::const_false(false);

iot_notiongroup_luminance::iot_notiongroup_luminance(void) : iot_notiongroup_numeric(0, "luminance", &iot_valuenotion_percentlum::object, IOT_VERSION_COMPOSE(1,0,0)) {}
const iot_notiongroup_luminance iot_notiongroup_luminance::object;


const iot_valuenotion_keycode iot_valuenotion_keycode::object;
const iot_valuenotion_degcelcius iot_valuenotion_degcelcius::object;
const iot_valuenotion_degfahrenheit iot_valuenotion_degfahrenheit::object;
const iot_valuenotion_percentlum iot_valuenotion_percentlum::object;



const/*expr*/ iot_datavalue_boolean iot_datavalue_boolean::const_true(true);
const/*expr*/ iot_datavalue_boolean iot_datavalue_boolean::const_false(false);
const/*expr*/ iot_datavalue_pulse iot_datavalue_pulse::object;

//use constexpr to guarantee object is initialized at the time when constructors for global objects like configregistry are created
const/*expr*/ iot_datavalue_nodeerrorstate iot_datavalue_nodeerrorstate::const_noinst(iot_datavalue_nodeerrorstate::IOT_NODEERRORSTATE_NOINSTANCE);

void object_destroysub_memblock(iot_objectrefable* obj) {
	obj->~iot_objectrefable();
	iot_release_memblock(obj);
}
void object_destroysub_delete(iot_objectrefable* obj) {
	delete obj;
}
void object_destroysub_staticmem(iot_objectrefable* obj) {
	obj->~iot_objectrefable();
}


template <bool shared> int iot_fixprec_timer<shared>::init(uint32_t prec_, uint64_t maxdelay_) {
		assert(!wheelbuf); //forbid double init

		if(prec_==0 || maxdelay_<prec_) return IOT_ERROR_INVALID_ARGS;
		uint64_t num=(maxdelay_ + prec_/2) / prec_ + 2;
		if(num>2048) return IOT_ERROR_INVALID_ARGS;
		thread=thread_registry->find_thread(uv_thread_self());
		if(!thread) {
			assert(false);
			return IOT_ERROR_INVALID_THREAD;
		}
		loop=thread->loop;

		wheelbuf=(iot_fixprec_timer_item<shared> **)thread->allocator->allocate(sizeof(iot_fixprec_timer_item<shared> *) * num);
		if(!wheelbuf) return IOT_ERROR_NO_MEMORY;
		numitems=(uint32_t)num;
		prec=prec_;
		maxerror=prec_ > 3 ? prec_/2 : prec_;
		maxdelay=maxdelay_;
		curitem=0;
		period_id=0;
		memset(wheelbuf, 0, sizeof(iot_fixprec_timer_item<shared> *) * num);

		uv_timer_init(thread->loop, &timer);
		timer.data=this;

		starttime=uv_now(thread->loop); //is reset every time curitem is set again to 0
		uv_timer_start(&timer, [](uv_timer_t*handle) -> void {
			iot_fixprec_timer* th=(iot_fixprec_timer*)handle->data;
			th->ontimer();
		}, 0, prec_);
		return 0;
	}

template int iot_fixprec_timer<true>::init(uint32_t prec_, uint64_t maxdelay_);


template <bool shared> bool iot_fixprec_timer<shared>::thread_valid(void) {
	return uv_thread_self()==thread->thread;
}


const iot_valuenotion* const iot_zwave_cc_multilevel_sensor_data_t::scale2notion[][4]={ {} /*type 0*/,
	{&iot_valuenotion_degcelcius::object, &iot_valuenotion_degfahrenheit::object, 0, 0}, /*type 1 - air temperature*/
	{},																					 /*type 2*/
	{&iot_valuenotion_percentlum::object, 0, 0, 0}, /*type 3 - ambient luminance*/
};
const size_t iot_zwave_cc_multilevel_sensor_data_t::scale2notion_size=sizeof(iot_zwave_cc_multilevel_sensor_data_t::scale2notion)/sizeof(iot_valuenotion*)/4;


/*
//kapi_fd_watcher implementation

struct kapi_fd_watcher_private {
	ev_io w; //backend watcher
	ev_loop* loop; //backend loop
	pthread th; //thread in which loop operates
};

#define IOT_ERROR_NO_MEMORY			-1			//memory allocation error
#define IOT_ERROR_NOT_INITED			-2			//object wasn't properly inited
#define IOT_ERROR_INITED_TWICE			-3			//object was already inited
#define IOT_ERROR_INVALID_THREAD		-4			//function called from unacceptable thread


static void kapi_fd_watcher_cb_priv (struct ev_loop *loop, ev_io *priv_w, int revents) {
	kapi_fd_watcher* w=(kapi_fd_watcher*)priv_w->data;

	kapi_fd_watcher_private* priv=(kapi_fd_watcher_private*)w->_private;

	w->cb(w, priv_w->fd, revents & EV_READ ? true : false, revents & EV_WRITE ? true : false);
}

int kapi_fd_watcher::init(kapi_fd_watcher_cb cb_, SOCKET fd, bool want_read, bool want_write, void* customdata=NULL) {
	if(_private) return IOT_ERROR_INITED_TWICE;

	_private=malloc(sizeof(kapi_fd_watcher_private));
	if(!_private) return IOT_ERROR_NO_MEMORY;
	memset(_private, 0, sizeof(kapi_fd_watcher_private));

	kapi_fd_watcher_private* priv=(kapi_fd_watcher_private*)_private;

	ev_io_init(&priv->w, &kapi_fd_watcher_cb_priv, fd, (want_read ? EV_READ : 0) | (want_write ? EV_WRITE : 0));
	priv->w.data=this;
	cb=cb_;
	data=customdata;
	return 0;
}

int kapi_fd_watcher::start(void) {
	if(!_private) return IOT_ERROR_NOT_INITED;

	kapi_fd_watcher_private* priv=(kapi_fd_watcher_private*)_private;

	if(ev_is_active(&priv->w)) return 0;

	assert(priv->loop==NULL);

	priv->loop=main_loop; //TODO select correct loop depending on current thread???
	priv->th=pthread_self();

	ev_io_start(priv->loop, &priv->w);
	return 0;
}
int kapi_fd_watcher::stop(void) {
	if(!_private) return IOT_ERROR_NOT_INITED;

	kapi_fd_watcher_private* priv=(kapi_fd_watcher_private*)_private;

	if(pthread_self()!=priv->th) return IOT_ERROR_INVALID_THREAD;
	if(!ev_is_active(&priv->w)) return 0;

	ev_io_stop(priv->loop, &priv->w);
	priv->loop=NULL;
	return 0;
}

int kapi_fd_watcher::is_active(void) {
	if(!_private) return IOT_ERROR_NOT_INITED;

	kapi_fd_watcher_private* priv=(kapi_fd_watcher_private*)_private;

	return ev_is_active(&priv->w) ? 1 : 0;
}

*/