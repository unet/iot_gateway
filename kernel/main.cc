#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <uv.h>

#include <iot_module.h>
#include <iot_devclass_keyboard.h>

#include <kernel/iot_daemonlib.h>
#include <kernel/iot_kernel.h>
#include <kernel/iot_moduleregistry.h>
#include <kernel/iot_configregistry.h>

uint64_t iot_starttime_ms; //start time of process like returned by uv_now (monotonic in ms since unknown point)
static timeval _start_timeval;

static iot_devifacetype_keyboard builtin_deviface_keyboard;

//list of built-in device interface types to register at startup
static const iot_devifacetype_iface* builtin_deviface_classes[]={
	&builtin_deviface_keyboard
};

//iot_devifaceclass__keyboard_ATTR iot_devifaceclass__keyboard_ATTR::def(IOT_KEYBOARD_MAX_KEYCODE, true);
//uint32_t iot_devifaceclass__keyboard_BASE::maxmsgsize=sizeof(iot_devifaceclass__keyboard_BASE::msg)+(IOT_KEYBOARD_MAX_KEYCODE/32+1)*32;

volatile sig_atomic_t need_restart=0;
void onsignal (uv_signal_t *w, int signum);

#define PIDFILE_PATH "daemon.pid"

int main(int argn, char **arg) {
	int err;
	bool pidcreated=false;

	assert(sizeof(iot_threadmsg_t)==64);
	assert(sizeof(iot_hwdev_ident_t)==128);

	if(!init_log("daemon.log")) {
		printf("Cannot init log, exiting\n");
		return 1;
	}

#ifndef _WIN32
//	printf("Daemonizing...\n");
//	if(daemon(1,0)==-1) {
//		outlog(LERROR,"error becoming a daemon: %s",strerror(errno));
//		goto onexit;
//	}

	if(!create_pidfile(PIDFILE_PATH)) goto onexit;
	pidcreated=true;

	//block SIGPIPE
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	////

#endif

	if(!main_loop) {
		outlog_error("Cannot init libuv, exiting\n");
		goto onexit;
	}

	iot_starttime_ms=uv_now(main_loop);
	gettimeofday(&_start_timeval, NULL);

	outlog_debug("Started");

	err=config_registry->load_config("config.js");
	if(err) {
		outlog_error("Cannot load config: %s", kapi_strerror(err));
		//try to continue and get config from server or start with empty config
	}


	//Here setup server connection to actualize config before instantiation
	//

	//Assume config was actualized or no server connection and some config got from file or we wait while server connection succeeds

	//load modules with autoload. autoload could be modified by config (TODO)
	modules_registry->start(builtin_deviface_classes, sizeof(builtin_deviface_classes)/sizeof(builtin_deviface_classes[0]), NULL, 0);

//	uv_run(main_loop, UV_RUN_ONCE);


	config_registry->start_config();

	uv_signal_t sigint_watcher,sighup_watcher,sigusr1_watcher,sigterm_watcher,sigquit_watcher;

	uv_signal_init(main_loop, &sigint_watcher);
	uv_signal_init(main_loop, &sighup_watcher);
	uv_signal_init(main_loop, &sigusr1_watcher);
	uv_signal_init(main_loop, &sigterm_watcher);
	uv_signal_init(main_loop, &sigquit_watcher);

	uv_signal_start(&sigint_watcher,onsignal,SIGINT);
	uv_signal_start(&sighup_watcher,onsignal,SIGHUP);
	uv_signal_start(&sigusr1_watcher,onsignal,SIGUSR1);
	uv_signal_start(&sigterm_watcher,onsignal,SIGTERM);
	uv_signal_start(&sigquit_watcher,onsignal,SIGQUIT);

	uv_timer_t shutdown_watcher;
	uv_timer_init(main_loop, &shutdown_watcher);


//	bool shuttingdown; //true when graceful shutdown was scheduled
//	shuttingdown=false;

	while(!need_exit) {
		uv_run(main_loop, UV_RUN_DEFAULT);

		if(need_exit==1) {// && !shuttingdown) { //graceful shutdown
//			shuttingdown=true; //to make repeated request urgent

			uv_timer_start(&shutdown_watcher,[](uv_timer_t *w)->void { //starts when graceful shutdown is initiated
				outlog_notice("Graceful shutdown timed out, terminating forcibly");
				need_exit=2; //forces urgent shutdown
				uv_stop (main_loop);
			},5000 , 0);

			outlog_notice("Graceful shutdown initiated");

			//Do graceful stop
			thread_registry->graceful_shutdown();

			uv_run(main_loop, UV_RUN_DEFAULT);
			break;
		}
	}

onexit:
	outlog_info("Exiting...");
	//do hard stop

//	config_registry->stop_config(); //must stop evaluation of configuration

//	stop all additional threads

//  modules_registry->stop(); //must clean modules registry

//  close connections to all peers


	outlog_notice("Terminated%s",need_restart ? ", restart requested" : "");
	close_log();
#ifndef _WIN32
	if(pidcreated) remove_pidfile(PIDFILE_PATH);
	if(need_restart && daemon(1,0)==0) {
		execv(arg[0],arg);
	}
#endif

	return 0;
}


void onsignal (uv_signal_t *w, int signum) {
	if(signum==SIGINT || signum==SIGQUIT) { //terminate program urgently, without graceful period
		need_exit=1;//2;
		uv_stop (main_loop);
		return;
	}
	if(signum==SIGTERM || signum==SIGUSR1 || signum==SIGHUP) { //terminate or restart program gracefully
		need_exit=1;
		if(signum==SIGUSR1) need_restart=1;
		uv_stop (main_loop);
		return;
	}
}
