#include <stdlib.h>
#include <pthread.h>
#include <time.h>


//#include <iot_compat.h>
//#include "evwrap.h"
#include <uv.h>
#include <ecb.h>
#include <iot_module.h>
#include <iot_devclass_keyboard.h>
#include <iot_devclass_activatable.h>
#include <iot_devclass_toneplayer.h>
#include <kernel/iot_daemonlib.h>
#include <kernel/iot_deviceregistry.h>


extern uv_loop_t *main_loop;

//Used by device detector modules for managing registry of available hardware devices
//action is one of {IOT_ACTION_ADD, IOT_ACTION_REMOVE}
//contype must be among those listed in .devcontypes field of detector module interface
//returns:
int iot_device_detector_base::kapi_hwdev_registry_action(enum iot_action_t action, iot_hwdev_localident_t* ident, size_t custom_len, void* custom_data) {
	//TODO check that ident->contype is listed in .devcontypes field of detector module interface
	iot_modinstance_locker modinstlk=modules_registry->get_modinstance(miid);
	if(!modinstlk) return IOT_ERROR_INVALID_ARGS;

	//TODO make inter-thread message, not direct call !!!
	if(uv_thread_self()==main_thread) {
		return hwdev_registry->list_action(miid, action, ident, custom_len, custom_data);
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

int iot_node_base::kapi_update_outputs(const iot_event_id_t *reason_eventid, uint8_t num_values, const uint8_t *valueout_indexes, const iot_valueclass_BASE** values, uint8_t num_msgs, const uint8_t *msgout_indexes, const iot_msgclass_BASE** msgs) {
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

uint32_t iot_devifacetype_keyboard::get_d2c_maxmsgsize(const char* cls_data) const {
	data_t* data=(data_t*)cls_data;
	return iot_devifaceclass__keyboard_BASE::get_maxmsgsize(data->max_keycode);
}
uint32_t iot_devifacetype_keyboard::get_c2d_maxmsgsize(const char* cls_data) const {
	data_t* data=(data_t*)cls_data;
	return iot_devifaceclass__keyboard_BASE::get_maxmsgsize(data->max_keycode);
}

uint32_t iot_devifacetype_activatable::get_c2d_maxmsgsize(const char* cls_data) const {
	return iot_devifaceclass__activatable_BASE::get_maxmsgsize();
}
uint32_t iot_devifacetype_activatable::get_d2c_maxmsgsize(const char* cls_data) const {
	return iot_devifaceclass__activatable_BASE::get_maxmsgsize();
}

uint32_t iot_devifacetype_toneplayer::get_c2d_maxmsgsize(const char* cls_data) const {
	return iot_devifaceclass__toneplayer_BASE::get_maxmsgsize();
}
uint32_t iot_devifacetype_toneplayer::get_d2c_maxmsgsize(const char* cls_data) const {
	return iot_devifaceclass__toneplayer_BASE::get_maxmsgsize();
}

//use constexpr to guarantee object is initialized at the time when constructors for global objects like configregistry are created
constexpr iot_valueclass_nodeerrorstate iot_valueclass_nodeerrorstate::const_noinst(iot_valueclass_nodeerrorstate::IOT_NODEERRORSTATE_NOINSTANCE);

constexpr iot_valueclass_boolean iot_valueclass_boolean::const_true(true);
constexpr iot_valueclass_boolean iot_valueclass_boolean::const_false(false);

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