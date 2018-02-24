#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

//#include "iot_compat.h"
//#include "evwrap.h"
#include "iot_core.h"
//#include "iot_netcon.h"
#include "iot_threadregistry.h"
#include "iot_moduleregistry.h"
#include "iot_peerconnection.h"
#include "iot_configregistry.h"
#include "iot_deviceregistry.h"
#include "iot_mesh_control.h"

//void kern_notifydriver_removedhwdev(iot_hwdevregistry_item_t* devitem) {
//	printf("busy HWDev removed: contype=%d, unique=%lu\n", devitem->devdata.dev_ident.contype, devitem->devdata.dev_ident.hwid);
//}

iot_thread_registry_t* thread_registry=NULL;
static iot_thread_registry_t _thread_registry; //instantiate singleton class
iot_thread_item_t* main_thread_item=NULL;

uv_thread_t main_thread=0;
uv_loop_t *main_loop=NULL;
volatile sig_atomic_t need_exit=0; //1 means graceful exit after getting SIGTERM or SIGUSR1, 2 means urgent exit after SIGINT or SIGQUIT

iot_modules_registry_t *modules_registry=NULL;
static iot_modules_registry_t _modules_registry; //instantiate singleton

//iot_configregistry_t* config_registry=NULL;

//hwdev_registry_t* hwdev_registry=NULL;
//static hwdev_registry_t _hwdev_registry; //instantiate singleton

#ifndef IOT_SERVER
iot_gwinstance *gwinstance=NULL; //gateways have single gwinstance
int64_t system_clock_offset=0; //nanoseconds to add to system real time clock to get event time. TODO:preserve this var between restarts.
uint32_t last_clock_sync; //timestamp of last clock sync (by clock of time source)
uint16_t last_clock_sync_error; //error of last clock sync in ms

#endif

int64_t mono_clock_offset=0; //nanoseconds to add to monotonic clock to get real time. Is calculated inside iot_init_systime to difference between RT and Monotonic clock
clockid_t mono_clockid;

void iot_init_systime(void) { //must be called on process start
	struct timespec monots, rtts, monots2;
	int err;
	err=clock_gettime(CLOCK_REALTIME, &rtts);
	assert(err==0);
	err=clock_gettime(CLOCK_MONOTONIC, &monots);
	if(expect_false(err==EINVAL)) { //no monotonic support, use RT
		mono_clockid=CLOCK_REALTIME;
		return;
	}
	clock_gettime(CLOCK_MONOTONIC, &monots2);
	clock_gettime(CLOCK_MONOTONIC, &monots2);
	clock_gettime(CLOCK_MONOTONIC, &monots2);
	clock_gettime(CLOCK_MONOTONIC, &monots2);
	clock_gettime(CLOCK_MONOTONIC, &monots2);
	clock_gettime(CLOCK_MONOTONIC, &monots2);
	clock_gettime(CLOCK_MONOTONIC, &monots2);
	clock_gettime(CLOCK_MONOTONIC, &monots2);
	mono_clockid=CLOCK_MONOTONIC;

	uint64_t clock_cost=(monots2.tv_nsec-monots.tv_nsec+1000000000*(monots2.tv_sec-monots.tv_sec))/8; //estimate cost of clock_gettime call
	mono_clock_offset=uint64_t(1000000000*rtts.tv_sec+rtts.tv_nsec)-uint64_t(1000000000*monots.tv_sec+monots.tv_nsec)-clock_cost;
}



//marks module (its current version) as buggy across restarts. Schedules restart of program
void iot_process_module_bug(iot_any_module_item_t *module_item) {
	//TODO
}

iot_modinstance_locker::~iot_modinstance_locker(void) {
		if(modinst) {
			modinst->unlock();
			modinst=NULL;
		}
	}

void iot_modinstance_locker::unlock(void) {
		assert(modinst!=NULL);
		if(modinst) {
			modinst->unlock();
			modinst=NULL;
		}
	}
bool iot_modinstance_locker::lock(iot_modinstance_item_t *inst) { //tries to lock provided structure and increment its refcount. returns false if struture cannot be locked (pending free)
//		assert(inst!=NULL && inst->miid.iid!=0);
		assert(modinst==NULL); //locker must be freed
		if(inst->lock()) { //increments refcount
			modinst=inst;
			return true;
		}
		return false;
	}

iot_gwinstance::iot_gwinstance(uint32_t guid, iot_hostid_t hostid, uint64_t eventid_numerator) : guid(guid), this_hostid(hostid),
				peers_registry(new iot_peers_registry_t(this)),
				config_registry(new iot_configregistry_t(this)),
				hwdev_registry(new hwdev_registry_t(this)),
				meshcontroller(new iot_meshnet_controller(this)),
				last_eventid_numerator(eventid_numerator) {
		if(!peers_registry || !hwdev_registry || !config_registry || !meshcontroller) error=IOT_ERROR_NO_MEMORY;
//		::hwdev_registry=hwdev_registry; //temporary
//		::config_registry=config_registry; //temporary
	}

iot_gwinstance::~iot_gwinstance(void) {
		if(peers_registry) delete peers_registry;
		if(hwdev_registry) delete hwdev_registry;
		if(config_registry) {
			config_registry->free_config(); //must stop evaluation of configuration
			delete config_registry;
		}
//		::hwdev_registry=NULL;
//		::config_registry=NULL; //temporary
	}

void iot_gwinstance::remove_modinstance(iot_modinstance_item_t *modinst) {
	assert(modinst && modinst->gwinst==this);
	BILINKLIST_REMOVE(modinst, next_ingwinst, prev_ingwinst); //remove instance from typed gwinst's list
	modinst->gwinst=NULL;

	if(is_shutdown && modinst->type==IOT_MODTYPE_NODE && node_instances_head==NULL) { //last node instance removed, continue graceful shutdown
		graceful_shutdown_step3();
	}
}


void iot_gwinstance::graceful_shutdown(void (*on_shutdown_)(void)) {
		if(is_shutdown) {
			assert(false);
			return;
		}
		is_shutdown=true;
		on_shutdown=on_shutdown_;
//TODO:	config_registry->graceful_shutdown(); //must generate modelling event which locks all local nodes (and dependent external nodes) to set their error state
											//and make outputs undefined. also this call must prevent new events generation (which can raise due to new signals
											//while waiting for lock). when lock obtained, shutdown continues by stopping node modinstances and all peer connections
											//accept for already existing connection to logger host (to be able to log state change of stopped node instances).
		graceful_shutdown_step2(); //temporary until configregistry shutdown is implemented
	}
void iot_gwinstance::graceful_shutdown_step2(void) { //called when modelling is stopped
		//stops all node instances. when ready, calls graceful_shutdown_step3() to sends stop signal to all driver and detector instances and calls callback
		if(!node_instances_head) { //no active node instances, go to next step
			graceful_shutdown_step3();
			return;
		}

		//loop through node modinstance and send stop signal
		iot_modinstance_item_t *modinst, *nextmodinst=node_instances_head; //loop through nodes
		while((modinst=nextmodinst)) {
			nextmodinst=nextmodinst->next_ingwinst;
			modinst->stop(false);
		}

//		if(modules_registry) {
//			modules_registry->graceful_shutdown([](iot_gwinstance* gwinst)->void{gwinst->graceful_shutdown_step3();});   
//		} else {
//			graceful_shutdown_step3();
//		}
	}

void iot_gwinstance::graceful_shutdown_step3(void) { //called when node modinstances stopped
		//send stop signal to driver and detector instances (but doesn't wait stopping to finish) and to peer connections

		//loop through driver modinstance and send stop signal
		iot_modinstance_item_t *modinst, *nextmodinst=driver_instances_head; //loop through nodes
		while((modinst=nextmodinst)) {
			nextmodinst=nextmodinst->next_ingwinst;
			modinst->stop(false);
		}
		//loop through detector modinstance and send stop signal
		nextmodinst=detector_instances_head; //loop through nodes
		while((modinst=nextmodinst)) {
			nextmodinst=nextmodinst->next_ingwinst;
			modinst->stop(false);
		}

		if(peers_registry) peers_registry->graceful_shutdown();//send stop signal to peer connections (skipping logger host)
		//TODO: logger must continue data transfer until all pending data is sent or graceful timeout expires

		//TODO: decrement some counter of unfinished instances in some registry of gwinstances so that on_shutdown callback could check it
		if(on_shutdown) on_shutdown();
	}

