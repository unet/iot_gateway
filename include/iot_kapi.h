#ifndef IOT_KAPI_H
#define IOT_KAPI_H

#include<stdint.h>
#include<stddef.h>

#include "uv.h"
#include "ecb.h"

#include "iot_compat.h"
#include "iot_error.h"


//Event subscription


//FD watcher functions
/*
struct kapi_fd_watcher;

typedef void (*kapi_fd_watcher_cb)(kapi_fd_watcher* w, SOCKET fd, bool have_read, bool have_write);

struct kapi_fd_watcher {
	void* _private; //hides real realization from modules

	kapi_fd_watcher_cb cb;
	void *data; //custom user data

	kapi_fd_watcher(void) : _private(NULL), cb(NULL), data(NULL) {} //does nothing exclusive
	~kapi_fd_watcher(void);

	//must be called and return success before calling other methods
	//returns 0 on success or negative error code
	int init(kapi_fd_watcher_cb cb, SOCKET fd, bool want_read, bool want_write, void* customdata=NULL);

	//start watching for events on provided fd
	//returns 0 on success or negative error code
	int start(void);

	//stop watching for events on provided fd
	//returns 0 on success or negative error code
	int stop(void);

	//test if watcher is started
	//returns:
	// 1 if started
	// 0 if NOT started
	// negative error code on error
	int is_active(void);

};
*/
#endif // IOT_KAPI_H
