#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>

#include <json-c/json.h>

#include "uv.h"
#include "iot_module.h"
//#include "iot_devclass_keyboard.h"
//#include "iot_devclass_activatable.h"
//#include "iot_devclass_toneplayer.h"

#include "iot_daemonlib.h"
#include "iot_kernel.h"
#include "iot_moduleregistry.h"
#include "iot_configregistry.h"


#define PIDFILE_PATH "run/daemon.pid"
#define TYPESDB_PATH "typesdb.json"


uint64_t iot_starttime_ms; //start time of process like returned by uv_now (monotonic in ms since unknown point)
static timeval _start_timeval;

//static iot_devifacetype_keyboard builtin_deviface_keyboard;
//static iot_devifacetype_activatable builtin_deviface_activatable;
//static iot_devifacetype_toneplayer builtin_deviface_toneplayer;

//list of built-in device interface types to register at startup
//static const iot_devifacetype_iface* builtin_deviface_classes[]={
//	&builtin_deviface_keyboard,
//	&builtin_deviface_activatable,
//	&builtin_deviface_toneplayer
//};




volatile sig_atomic_t need_restart=0;
void onsignal (uv_signal_t *w, int signum);

struct daemon_setup_t {
	iot_hostid_t host_id=0;
	bool daemonize=true;
	int min_loglevel=-1;
	uint16_t listen_port=12000;
} daemon_setup;


bool parse_setup(void) {
	char namebuf[256];
	snprintf(namebuf, sizeof(namebuf), "%s%s", rootpath, "setup.json");

	int fd=open(namebuf, O_RDONLY);
	if(fd<0) {
		fprintf(stderr, "Error opening setup file '%s': %s\n", namebuf, strerror(errno));
		return false;
	}
	struct stat stat;
	if(fstat(fd, &stat)) {
		fprintf(stderr, "Error doing stat('%s'): %s\n", namebuf, strerror(errno));
		close(fd);
		return false;
	}
	if(stat.st_size>32767) {
		fprintf(stderr, "Size of setup file '%s' exceeds 32767 bytes\n", namebuf);
		close(fd);
		return false;
	}
	char buf[stat.st_size+1];
	ssize_t len=read(fd, buf, stat.st_size);
	if(len<0) {
		fprintf(stderr, "Error reading setup file '%s': %s\n", namebuf, strerror(errno));
		close(fd);
		return false;
	}
	close(fd);
	buf[len]='\0';

	json_tokener *tok=json_tokener_new_ex(3);
	if(!tok) {
		fprintf(stderr, "Lack of memory to start JSON parser\n");
		return false;
	}

	json_object* obj=json_tokener_parse_ex(tok, buf, len);

	if(json_tokener_get_error(tok) != json_tokener_success || !obj) {
		fprintf(stderr, "Error parsing setup file '%s': %s\n", namebuf,  json_tokener_error_desc(json_tokener_get_error(tok)));
		if (obj != NULL) json_object_put(obj);
		obj = NULL;
		json_tokener_free(tok);
		return false;
	}
	json_tokener_free(tok);
	if(!json_object_is_type(obj,  json_type_object)) {
		fprintf(stderr, "Invalid setup file '%s', it must have JSON-object as top element\n", namebuf);
		json_object_put(obj); obj = NULL;
		return false;
	}

	json_object *val=NULL;
	if(json_object_object_get_ex(obj, "host_id", &val)) {
		uint64_t u64=iot_strtou64(json_object_get_string(val), NULL, 10);
		if(!errno && u64>0 && (u64<INT64_MAX || json_object_is_type(val, json_type_string))) daemon_setup.host_id=iot_hostid_t(u64);
	}
	if(!daemon_setup.host_id) {
		fprintf(stderr, "Required setup value 'host_id' not found or is invalid in setup file '%s'\n", namebuf);
		json_object_put(obj); obj = NULL;
		return false;
	}
	if(json_object_object_get_ex(obj, "loglevel", &val)) {
		errno=0;
		int32_t i32=json_object_get_int(val);
		if(!errno && i32>=LDEBUG && i32<=LERROR) daemon_setup.min_loglevel=i32;
	}
	if(json_object_object_get_ex(obj, "daemonize", &val)) {
		daemon_setup.daemonize = json_object_get_boolean(val) ? true : false;
	}
	if(json_object_object_get_ex(obj, "listen_port", &val)) {
		errno=0;
		int32_t i32=json_object_get_int(val);
		if(!errno && i32>0 && i32<65536) daemon_setup.listen_port=uint16_t(i32);
			else fprintf(stderr, "Invalid value '%s' for 'listen_port' in setup file '%s' was ignored\n",  json_object_get_string(val), namebuf);
	}

	json_object_put(obj); obj = NULL;
	return true;
}



int main(int argn, char **arg) {
	int err;
	bool pidcreated=false;

	assert(sizeof(iot_threadmsg_t)==64);

	if(!parse_args(argn, arg, "run")) {
		return 1;
	}

	if(!parse_setup()) {
		return 1;
	}

	if(min_loglevel<0) {
		if(daemon_setup.min_loglevel>=0) min_loglevel=daemon_setup.min_loglevel;
		else min_loglevel=LMIN;
	}

	if(!init_log("run/daemon.log")) {
		fprintf(stderr, "Cannot init log, exiting\n");
		return 1;
	}

#ifndef _WIN32
	if(daemon_setup.daemonize) {
		printf("Daemonizing...\n");
		if(daemon(1,0)==-1) {
			outlog(LERROR,"error becoming a daemon: %s",strerror(errno));
			goto onexit;
		}
	}

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

	iot_current_hostid=daemon_setup.host_id;
	outlog_debug("Started, my host id is %" IOT_PRIhostid, iot_current_hostid);

	json_object* cfg;
	cfg=config_registry->read_jsonfile(IOTCONFIG_PATH, "config");
	if(!cfg) goto onexit;

	err=config_registry->load_hosts_config(cfg);
	if(err) {
		outlog_error("Cannot load config: %s", kapi_strerror(err));
		json_object_put(cfg);
		goto onexit;
	}

	//Here setup server connection to actualize config before instantiation
	//

	err=config_registry->load_config(cfg, true);
	if(err) {
		outlog_error("Cannot load config: %s", err==IOT_ERROR_NOT_FOUND ? "inconsistent data" : kapi_strerror(err));
		//try to continue and get config from server or start with empty config
	}
	json_object_put(cfg);
	cfg=NULL;

	//Assume config was actualized or no server connection and some config got from file or we wait while server connection succeeds



	cfg=config_registry->read_jsonfile(TYPESDB_PATH, "typesdb");
	if(!cfg) goto onexit;

	//load modules with autoload. autoload could be modified by config (TODO)
	modules_registry->start(cfg);
	if(cfg) {
		json_object_put(cfg); //modules_registry->start must increment references to necessary sub-objects
		cfg=NULL;
	}


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

			config_registry->free_config(); //must stop evaluation of configuration

			//Do graceful stop
			thread_registry->graceful_shutdown();

			uv_run(main_loop, UV_RUN_DEFAULT);
			break;
		}
	}

onexit:
	outlog_info("Exiting...");
	//do hard stop

	config_registry->free_config(); //must stop evaluation of configuration

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


