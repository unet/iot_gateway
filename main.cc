#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>

#include <json-c/json.h>

//#include "iot_devclass_keyboard.h"
//#include "iot_devclass_activatable.h"
//#include "iot_devclass_toneplayer.h"

#include "iot_core.h"
#include "iot_moduleregistry.h"
#include "iot_configregistry.h"
#include "iot_threadregistry.h"
#include "iot_netcon.h"
#include "iot_netproto_iotgw.h"
#include "iot_peerconnection.h"

#define PIDFILE_NAME "daemon.pid"
#define LOGFILE_NAME "daemon.log"
#define REGISTRYFILE_NAME "registry.json"
#define OWNCONFIGFILE_NAME "ownconfig.json"


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



//iot_peers_conregistry *peers_conregistry=NULL;

volatile sig_atomic_t need_restart=0;
void onsignal (uv_signal_t *w, int signum);

struct daemon_setup_t {
	uint32_t guid=0;
	iot_hostid_t host_id=0;
	bool daemonize=true;
	int min_loglevel=-1;
	json_object* listencfg=NULL;
//	uint16_t listen_port=12000;

	~daemon_setup_t(void) {
		if(listencfg) {json_object_put(listencfg);listencfg=NULL;}
	}
} daemon_setup;


bool parse_setup(void) {
	char namebuf[256];
	snprintf(namebuf, sizeof(namebuf), "%s/%s", conf_dir, "setup.json");

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

	json_tokener *tok=json_tokener_new_ex(5);
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

	if(json_object_object_get_ex(obj, "guid", &val)) IOT_JSONPARSE_UINT(val, uint32_t, daemon_setup.guid);
	if(!daemon_setup.guid) {
		fprintf(stderr, "Required setup value 'guid' not found or is invalid in setup file '%s'\n", namebuf);
		json_object_put(obj); obj = NULL;
		return false;
	}

	if(json_object_object_get_ex(obj, "host_id", &val)) IOT_JSONPARSE_UINT(val, iot_hostid_t, daemon_setup.host_id);
	if(!daemon_setup.host_id) {
		fprintf(stderr, "Required setup value 'host_id' not found or is invalid in setup file '%s'\n", namebuf);
		json_object_put(obj); obj = NULL;
		return false;
	}

	if(json_object_object_get_ex(obj, "loglevel", &val)) IOT_JSONPARSE_UINT(val, uint8_t, daemon_setup.min_loglevel);
	if(daemon_setup.min_loglevel<LDEBUG || daemon_setup.min_loglevel>LERROR) daemon_setup.min_loglevel=-1;


	if(json_object_object_get_ex(obj, "daemonize", &val)) {
		daemon_setup.daemonize = json_object_get_boolean(val) ? true : false;
	}
//	if(json_object_object_get_ex(obj, "listen_port", &val)) {
//		errno=0;
//		int32_t i32=json_object_get_int(val);
//		if(!errno && i32>0 && i32<65536) daemon_setup.listen_port=uint16_t(i32);
//			else fprintf(stderr, "Invalid value '%s' for 'listen_port' in setup file '%s' was ignored\n",  json_object_get_string(val), namebuf);
//	}
	if(json_object_object_get_ex(obj, "listen", &val)) {
		if(!json_object_is_type(val,  json_type_array)) {
			fprintf(stderr, "Invalid 'listen' configuration in setup file '%s', it must have JSON-array\n", namebuf);
			json_object_put(obj); obj = NULL;
			return false;
		}
		daemon_setup.listencfg=json_object_get(val);
	}

	json_object_put(obj); obj = NULL;
	return true;
}

bool parse_ownconfig(json_object* obj) {
	if(!json_object_is_type(obj,  json_type_object)) {
		outlog_error("Invalid ownconfig file '%s', it must have JSON-object as top element\n", OWNCONFIGFILE_NAME);
		return false;
	}

	json_object *val=NULL;

	if(json_object_object_get_ex(obj, "modtime", &val)) ownconfig.modtime=json_object_get_int64(val);

	if(json_object_object_get_ex(obj, "load_drivers", &val)) {
		if(!json_object_is_type(val, json_type_object)) {
			outlog_error("Invalid 'load_drivers' configuration in ownconfig file '%s', it must have JSON-object", OWNCONFIGFILE_NAME);
			return false;
		}
		ownconfig.load_drivers=json_object_get(val);
	}

	if(json_object_object_get_ex(obj, "detectors", &val)) {
		if(!json_object_is_type(val, json_type_object)) {
			outlog_error("Invalid 'detectors' configuration in ownconfig file '%s', it must have JSON-object", OWNCONFIGFILE_NAME);
			return false;
		}
		ownconfig.detectors=json_object_get(val);
		json_object_object_foreach(ownconfig.detectors, modid, moddata) {
			if(!json_object_is_type(moddata, json_type_object)) {
				outlog_error("Invalid data in ownconfig! Values in 'detectors' object must be JSON-objects, but value under '%s' is not", modid);
				return false;
			}
			uint32_t module_id=0;
			IOT_STRPARSE_UINT(modid, uint32_t, module_id);
			if(!module_id) continue;
			iot_regitem_module_t* regitem=iot_regitem_module_t::find_item(IOT_MODTYPE_DETECTOR, module_id);
			if(!regitem) continue;

			if(json_object_object_get_ex(moddata, "start", &val)) { //modify autoload only if 'start' exists
				regitem->autoload=json_object_get_boolean(val) ? true : false;
			}
			if(json_object_object_get_ex(moddata, "manual", &val)) {
				if(!json_object_is_type(val, json_type_array)) {
					outlog_error("Invalid data in ownconfig! Value in 'detectors.ID.manual' must be JSON-array, but value under 'detectors.%s.manual' is not", modid);
					return false;
				}
				regitem->params.detector.manual=val; //NOT SAFE!!! this reference will be lost if ownconfig is updated! TODO
				regitem->params.detector.manual_modtime=0; //NOT SAFE!!! this reference will be lost if ownconfig is updated! TODO

				if(json_object_object_get_ex(moddata, "manual_modtime", &val)) regitem->params.detector.manual_modtime=json_object_get_int64(val);
			}
		}
	}

	if(json_object_object_get_ex(obj, "hwdevices", &val)) {
		if(!json_object_is_type(val, json_type_object)) {
			outlog_error("Invalid 'hwdevices' configuration in ownconfig file '%s', it must have JSON-object", OWNCONFIGFILE_NAME);
			return false;
		}
		ownconfig.hwdevices=json_object_get(val);
	}

	if(json_object_object_get_ex(obj, "peer_connect_map", &val)) {
		if(!json_object_is_type(val, json_type_object)) {
			outlog_error("Invalid 'peer_connect_map' configuration in ownconfig file '%s', it must have JSON-object", OWNCONFIGFILE_NAME);
			return false;
		}
		ownconfig.connect_map=json_object_get(val);
	}
	return true;
}


int main(int argn, char **arg) {
	int err;
	bool pidcreated=false;
	json_object* cfg_json=NULL;
	json_object* registry_json=NULL;
	json_object* owncfg_json=NULL;

	assert(sizeof(iot_threadmsg_t)==64);

	iot_init_systime();

	if(!parse_args(argn, arg)) {
		return 1;
	}

	if(mkdir(run_dir, 0755) && errno!=EEXIST) {
		fprintf(stderr,"Error creating '%s': %s\n", run_dir, strerror(errno));
		return 1;
	}


	if(!parse_setup()) {
		return 1;
	}

	if(min_loglevel<0) {
		if(daemon_setup.min_loglevel>=0) min_loglevel=daemon_setup.min_loglevel;
		else min_loglevel=LMIN;
	}

	if(!init_log(run_dir, LOGFILE_NAME)) {
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

	if(!create_pidfile(run_dir, PIDFILE_NAME)) goto onexit;
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

	iot_current_hostid=daemon_setup.host_id;
	outlog_debug("Started, my host id is %" IOT_PRIhostid, iot_current_hostid);

	gwinstance=new iot_gwinstance(daemon_setup.guid, daemon_setup.host_id, 0); //TODO: read saved system time offset
	if(!gwinstance || gwinstance->error) {
		outlog_error("Cannot allocate memory for initial data structures");
		goto onexit;
	}
	err=gwinstance->peers_registry->init();
	if(err) {
		outlog_error("Error initializing peers registry: %s", kapi_strerror(err));
		goto onexit;
	}
	err=gwinstance->meshcontroller->init();
	if(err) {
		outlog_error("Error initializing mesh controller: %s", kapi_strerror(err));
		goto onexit;
	}

	//read config to get address of "cfgmanager_host" server
	cfg_json=iot_configregistry_t::read_jsonfile(conf_dir, IOTCONFIG_NAME, "config");
	if(!cfg_json) goto onexit;
	//parse hosts to get "cfgmanager_host" server
	err=gwinstance->config_registry->load_hosts_config(cfg_json);
	if(err) {
		outlog_error("Cannot load config: %s", kapi_strerror(err));
		goto onexit;
	}

	//Here setup server connection to actualize config before instantiation. if "cfgmanager_host" has higher host ID listening socks are not necessary

	//create listening IOT GW netcon
	err=gwinstance->peers_registry->start_iot_listen();
	if(err) {
		outlog_error("Error starting listening IOTGW connections: %s", kapi_strerror(err));
		goto onexit;
	}

	//create listening MESH session netcon
	if(daemon_setup.listencfg) {
		err=gwinstance->peers_registry->add_listen_connections(daemon_setup.listencfg, true);
		if(err) {
			outlog_error("Error adding connections to listen for peers: %s", kapi_strerror(err));
			goto onexit;
		}
	}

	//Here setup server connection to actualize config before instantiation if "cfgmanager_host" has LOWER host ID and listening socks are necessary

	//Here assume config, ownconfig and registry were actualized from "cfgmanager_host" server if possible

	registry_json=iot_configregistry_t::read_jsonfile(conf_dir, REGISTRYFILE_NAME, "registry");
	if(!registry_json) goto onexit;
	if(libregistry->apply_registry(registry_json, true)) goto onexit;
	json_object_put(registry_json); //libregistry->apply_registry must increment references to necessary sub-objects
	registry_json=NULL;

//server has now ownconfig as it does not work with hwdevices and drivers
#ifndef IOT_SERVER
	owncfg_json=iot_configregistry_t::read_jsonfile(conf_dir, OWNCONFIGFILE_NAME, "ownconfig", true);
	if(!owncfg_json) goto onexit;
	if(!parse_ownconfig(owncfg_json)) goto onexit;
	json_object_put(owncfg_json); //parse_ownconfig must increment references to necessary sub-objects
	owncfg_json=NULL;
#endif

	err=gwinstance->config_registry->load_config(cfg_json, true);
	if(err) {
		outlog_error("Cannot load config: %s", err==IOT_ERROR_NOT_FOUND ? "inconsistent data" : kapi_strerror(err));
		//try to continue and get config from server or start with empty config
	}
	json_object_put(cfg_json); cfg_json=NULL;

	//Assume config was actualized or no server connection and some config got from file or we wait while server connection succeeds



	//load detector and driver modules with autoload. autoload could be modified by config (TODO)
	modules_registry->start();


//	uv_run(main_loop, UV_RUN_ONCE);


	gwinstance->config_registry->start_config();



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
			},4000 , 0);

			outlog_notice("Graceful shutdown initiated");

//#ifdef IOT_SERVER
//			Iterate through all guids in gwinstances:
//#endif
			gwinstance->graceful_shutdown([](void)->void{
//#ifdef IOT_SERVER
//				if(gwinst_registry->num_active==0) thread_registry->graceful_shutdown()
				modules_registry->graceful_shutdown(); //causes stop signal to global detector instances
//#else
				thread_registry->graceful_shutdown(); //causes termination of unbusy threads
//#endif
			});

			uv_run(main_loop, UV_RUN_DEFAULT);
			break;
		}
	}

onexit:
	outlog_info("Exiting...");
	//do hard stop

	if(cfg_json) {json_object_put(cfg_json); cfg_json=NULL;}
	if(registry_json) {json_object_put(registry_json); registry_json=NULL;}
	if(owncfg_json) {json_object_put(owncfg_json); owncfg_json=NULL;}

	thread_registry->shutdown(); //causes termination of threads


	//TODO: for server loop though all gwinstaces
	if(gwinstance) {delete gwinstance; gwinstance=NULL;}

//	stop all additional threads

//  modules_registry->stop(); //must clean modules registry

//  close connections to all peers

//	if(peers_conregistry) {delete peers_conregistry; peers_conregistry=NULL;}

	outlog_notice("Terminated%s",need_restart ? ", restart requested" : "");
	close_log();
#ifndef _WIN32
	if(pidcreated) remove_pidfile(run_dir, PIDFILE_NAME);
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


