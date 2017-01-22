#include <stdlib.h>
#include <pthread.h>
#include <time.h>


//#include <iot_compat.h>
//#include "evwrap.h"
#include <uv.h>
#include <ecb.h>
#include <iot_kapi.h>
#include <iot_error.h>
#include <kernel/iot_deviceregistry.h>


extern uv_loop_t *main_loop;

//Used by device detector modules for managing registry of available hardware devices
//action is one of {IOT_ACTION_ADD, IOT_ACTION_REMOVE,IOT_ACTION_REPLACE}
//contype must be among those listed in .devcontypes field of detector module interface
//hostid field of ident is ignored by kernel (it always assigns current host)
//returns:
int kapi_hwdev_registry_action(enum iot_action_t action, iot_hwdev_localident_t* ident, size_t custom_len, void* custom_data) {
	//TODO check that ident->contype is listed in .devcontypes field of detector module interface

	//TODO make inter-thread message, not direct call !!!
	hwdev_registry->list_action(action, ident, custom_len, custom_data);
	return 0;
}

void kapi_devdriver_self_abort(iot_iid_t iid, void *instance, int errcode) {
	//TODO find driver by instance and stop it
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

const char* kapi_devclassid_str(uint32_t clsid) {
	switch(clsid) {
#define XX(nm, cc, txt) case cc: return txt;
		IOT_DEVCLASSID_MAP(XX)
#undef XX
	}
	return "UNKNOWN DEVICE CLASS"; //TODO. for module-specific classes find module's method for getting stringified class id
}


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