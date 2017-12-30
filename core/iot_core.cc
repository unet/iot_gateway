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

iot_configregistry_t* config_registry=NULL;

hwdev_registry_t* hwdev_registry=NULL;
static hwdev_registry_t _hwdev_registry; //instantiate singleton

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

iot_gwinstance::iot_gwinstance(uint32_t guid, iot_hostid_t hostid) : guid(guid), this_hostid(hostid), peers_registry(new iot_peers_registry_t(this)) {
		if(!peers_registry) error=IOT_ERROR_NO_MEMORY;
	}

iot_gwinstance::~iot_gwinstance(void) {
		if(peers_registry) delete peers_registry;
	}
