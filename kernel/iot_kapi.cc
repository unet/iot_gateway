#include <stdlib.h>
#include <pthread.h>
#include <time.h>


//#include "iot_compat.h"
#include "uv.h"
#include "iot_module.h"
#include "iot_devclass_keyboard.h"
#include "iot_devclass_activatable.h"
//#include "iot_devclass_toneplayer.h"
#include "iot_daemonlib.h"
#include "iot_deviceregistry.h"


extern uv_loop_t *main_loop;

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
int iot_device_detector_base::kapi_hwdev_registry_action(enum iot_action_t action, iot_hwdev_localident* ident, iot_hwdev_details* custom_data) {
	//TODO check that ident->contype is listed in .devcontypes field of detector module interface
	iot_modinstance_locker modinstlk=modules_registry->get_modinstance(miid);
	if(!modinstlk) return IOT_ERROR_INVALID_ARGS;

	//TODO make inter-thread message, not direct call !!!
	if(uv_thread_self()==main_thread) {
		return hwdev_registry->list_action(miid, action, ident, custom_data);
	} else {
		assert(false);
		//TODO
	}
	return 0;
}


iot_module_instance_base::iot_module_instance_base(uv_thread_t thread) : thread(thread), miid(0,0) {
	iot_thread_item_t* th=thread_registry->find_thread(thread);
	assert(th!=NULL);
//	memallocator=th->allocator;
	loop=th->loop;
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
	iot_modinstance_locker modinstlk=modules_registry->get_modinstance(miid);
	if(!modinstlk || modinstlk.modinst->instance!=this) {
		assert(false);
		return 0;
	}
	iot_modinstance_item_t* modinst=modinstlk.modinst;
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

int iot_node_base::kapi_update_outputs(const iot_event_id_t *reason_eventid, uint8_t num_values, const uint8_t *valueout_indexes, const iot_valuetype_BASE** values, uint8_t num_msgs, const uint8_t *msgout_indexes, const iot_msgtype_BASE** msgs) {
	iot_modinstance_locker modinstlk=modules_registry->get_modinstance(miid);
	if(!modinstlk || modinstlk.modinst->instance!=this) {
		assert(false);
		return 0;
	}
	iot_modinstance_item_t* modinst=modinstlk.modinst;
	assert(uv_thread_self()==modinst->thread->thread);

	if(!modinst->is_working()) return 0;
	auto model=modinst->data.node.model;
	if(!model->cfgitem) return 0; //model is being stopped and is already detached from config item. ignore signal.

	return model->do_update_outputs(reason_eventid, num_values, valueout_indexes, values, num_msgs, msgout_indexes, msgs);
}

const iot_valuetype_BASE* iot_node_base::kapi_get_outputvalue(uint8_t index) {
	iot_modinstance_locker modinstlk=modules_registry->get_modinstance(miid);
	if(!modinstlk || modinstlk.modinst->instance!=this) {
		assert(false);
		return 0;
	}
	iot_modinstance_item_t* modinst=modinstlk.modinst;
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

iot_hwdevcontype_metaclass::iot_hwdevcontype_metaclass(iot_type_id_t id, const char* vendor, const char* type) {
	assert(type!=NULL);

	iot_hwdevcontype_metaclass* &head=iot_modules_registry_t::devcontype_pendingreg_head(), *cur;
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
	vendor_name=vendor;
	type_name=type;
}



int iot_hwdevcontype_metaclass::from_json(json_object* json, char* buf, size_t bufsize, const iot_hwdev_localident*& obj, iot_type_id_t default_contype) {
	json_object* val=NULL;
	iot_type_id_t contype=default_contype;
	if(json_object_object_get_ex(json, "contype_id", &val)) { //zero or absent value leaves IOT_DEVCONTYPE_ANY
		IOT_JSONPARSE_UINT(json, iot_type_id_t, contype);
	}
	if(!contype) return IOT_ERROR_BAD_DATA;

	const iot_hwdevcontype_metaclass* metaclass=iot_hwdevcontype_metaclass::findby_contype_id(contype);
	if(!metaclass) return IOT_ERROR_NOT_FOUND;

	val=NULL;
	json_object_object_get_ex(json, "ident", &val);
	obj=NULL;
	return metaclass->p_from_json(val, buf, bufsize, obj);
}

const iot_hwdevcontype_metaclass* iot_hwdevcontype_metaclass::findby_contype_id(iot_type_id_t contype_id, bool try_load) {
	return modules_registry->find_devcontype(contype_id, try_load);
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




iot_devifacetype_metaclass::iot_devifacetype_metaclass(iot_type_id_t id, const char* vendor, const char* type) {
	assert(type!=NULL);

printf("!!!%s\n",type);
	iot_devifacetype_metaclass* &head=iot_modules_registry_t::devifacetype_pendingreg_head(), *cur;
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
	vendor_name=vendor;
	type_name=type;
}

const iot_devifacetype_metaclass_keyboard iot_devifacetype_metaclass_keyboard::object;
const iot_devifacetype_metaclass_activatable iot_devifacetype_metaclass_activatable::object;
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
/*
uint32_t iot_devifacetype_toneplayer::get_c2d_maxmsgsize(const char* cls_data) const {
	return iot_deviface__toneplayer_BASE::get_maxmsgsize();
}
uint32_t iot_devifacetype_toneplayer::get_d2c_maxmsgsize(const char* cls_data) const {
	return iot_deviface__toneplayer_BASE::get_maxmsgsize();
}
*/
//use constexpr to guarantee object is initialized at the time when constructors for global objects like configregistry are created
constexpr iot_valuetype_nodeerrorstate iot_valuetype_nodeerrorstate::const_noinst(iot_valuetype_nodeerrorstate::IOT_NODEERRORSTATE_NOINSTANCE);

constexpr iot_valuetype_boolean iot_valuetype_boolean::const_true(true);
constexpr iot_valuetype_boolean iot_valuetype_boolean::const_false(false);

constexpr iot_valuenotion_keycode iot_valuenotion_keycode::iface;
constexpr iot_valuenotion_degcelcius iot_valuenotion_degcelcius::iface;
constexpr iot_valuenotion_degfahrenheit iot_valuenotion_degfahrenheit::iface;




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