#ifndef IOT_CORE_H
#define IOT_CORE_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching

//#include <stdint.h>
//#include <atomic>
#include "uv.h"

//#include<time.h>

#include "iot_config.h"
#include "iot_module.h"
#include "iot_common.h"
#include "iot_daemonlib.h"

struct iot_gwinstance;


struct iot_modinstance_item_t;
struct iot_any_module_item_t;
struct iot_threadmsg_t;
struct iot_thread_item_t;
class iot_thread_registry_t;
class iot_modules_registry_t;
class iot_memallocator;
class iot_peer;
class iot_peers_registry_t;
struct iot_configregistry_t;
class hwdev_registry_t;

struct iot_node_module_item_t;
struct iot_driver_module_item_t;
struct iot_detector_module_item_t;
struct iot_modinstance_item_t;
struct iot_driverclient_conndata_t;
struct iot_device_entry_t;
struct iot_device_connection_t;
struct iot_netproto_session_iotgw;

extern uv_loop_t *main_loop;
extern iot_thread_item_t* main_thread_item; //prealloc main thread item
extern iot_thread_registry_t* thread_registry;
extern volatile sig_atomic_t need_exit;
extern iot_memallocator main_allocator;
extern iot_modules_registry_t *modules_registry;
extern iot_configregistry_t* config_registry;
extern hwdev_registry_t* hwdev_registry;


#include "iot_threadmsg.h"


//void kern_notifydriver_removedhwdev(iot_hwdevregistry_item_t*);

void iot_process_module_bug(iot_any_module_item_t *module_item);

//holds locked state of mod instance structure
//does not allow to copy itself, only to move
struct iot_modinstance_locker {
	iot_modinstance_item_t *modinst;
	iot_modinstance_locker(iot_modinstance_item_t *inst) { //bind already locked modinstance structure to locker object. refcount must have been just increased
//		assert(inst!=NULL && inst->miid.iid!=0);
//		assert(inst->refcount>0); //structure must be locked AND ITS refcount must be already incremented outside
		modinst=inst;
	}
	iot_modinstance_locker(void) {
		modinst=NULL;
	}
	iot_modinstance_locker(const iot_modinstance_locker&) = delete;
	iot_modinstance_locker(iot_modinstance_locker&& src) { //move constructor
		modinst=src.modinst;
		src.modinst=NULL; //invalidate src
	}
	~iot_modinstance_locker(void);
	void unlock(void);
	bool lock(iot_modinstance_item_t *inst); //tries to lock provided structure and increment its refcount. returns false if struture cannot be locked (pending free)

	iot_modinstance_locker& operator=(iot_modinstance_item_t *inst) = delete; //without this assignment like "iot_modinstance_locker L=NULL or (iot_modinstance_item_t *) is possible)
	iot_modinstance_locker& operator=(const iot_modinstance_locker&) = delete;
	iot_modinstance_locker& operator=(iot_modinstance_locker&& src) noexcept {  //move assignment
		if(this!=&src) {
			modinst=src.modinst;
			src.modinst=NULL; //invalidate src
		}
		return *this;
	}
	
	explicit operator bool() const {
		return modinst!=NULL;
	}
	bool operator !(void) const {
		return modinst==NULL;
	}
};


struct iot_gwinstance { //represents IOT gateway per-user state instance
	const uint32_t guid=0;
	const iot_hostid_t this_hostid=0; //ID of current host in user config

	iot_peers_registry_t *const peers_registry=NULL;
	int error=0;

	iot_gwinstance(uint32_t guid, iot_hostid_t hostid);
	~iot_gwinstance(void);
};


#endif //IOT_CORE_H
