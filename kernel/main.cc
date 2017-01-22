#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <uv.h>

#include <iot_module.h>
#include <iot_kapi.h>
#include <kernel/iot_daemonlib.h>
#include <kernel/iot_kernel.h>
#include <kernel/iot_moduleregistry.h>
#include <kernel/iot_configregistry.h>

uint64_t iot_starttime_ms; //start time of process like returned by uv_now (monotonic in ms since unknown point)
static timeval _start_timeval;

static iot_devifacecls_config_t builtin_deviface_classes[]={
	{
		.classid = IOT_DEVCLASSID_KEYBOARD,
		.d2c_maxmsgsize = 128,
		.c2d_maxmsgsize = 32
	},
	{
		.classid = IOT_DEVCLASSID_KEYS,
		.d2c_maxmsgsize = 128,
		.c2d_maxmsgsize = 32
	}
};


int main(int argn, char **arg) {
	int err;
	if(!main_loop) {
		printf("Cannot init libuv, exiting\n");
		return 1;
	}
	iot_starttime_ms=uv_now(main_loop);
	gettimeofday(&_start_timeval, NULL);

	if(!init_log("daemon.log")) {
		printf("Cannot init log, exiting\n");
		return 1;
	}

	outlog_debug("Started");

	err=config_registry->load_config("config.js");
	if(err) {
		outlog_error("Cannot load config: %s", kapi_strerror(err));
		//try to continue and get config from server or start with empty config
	}

outlog_error("sizeof iot_threadmsg_t = %u", unsigned(sizeof(iot_threadmsg_t)));

	//Here setup server connection to actualize config before instantiation
	//

	//Assume config was actualized or no server connection and some config got from file or we wait while server connection succeeds

	//load modules with autoload. autoload could be modified by config (TODO)
	modules_registry->start(builtin_deviface_classes, sizeof(builtin_deviface_classes)/sizeof(builtin_deviface_classes[0]));

//	uv_run(main_loop, UV_RUN_ONCE);


	config_registry->start_config();

	uv_run(main_loop, UV_RUN_DEFAULT);

	return 0;
}


