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
class iot_thread_item_t;
class iot_thread_registry_t;
class iot_modules_registry_t;
class iot_memallocator;
class iot_peer;
class iot_peers_registry_t;
struct iot_configregistry_t;
class hwdev_registry_t;
class iot_netcon;
class iot_meshnet_controller;
class iot_netproto_session_mesh;
class iot_meshtun_packet;
class iot_meshtun_state;
class iot_meshtun_forwarding;
struct iot_meshtun_stream_state;
struct iot_meshtun_stream_listen_state;

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
//extern iot_configregistry_t* config_registry;
//extern hwdev_registry_t* hwdev_registry;

#ifndef IOT_SERVER
extern iot_gwinstance *gwinstance; //gateways have single gwinstance
extern int64_t system_clock_offset; //TODO. preserve this var between restarts. nanoseconds to add to system real time clock to get event numerator
extern uint32_t last_clock_sync; //timestamp of last clock sync (by clock of time source)
extern uint16_t last_clock_sync_error; //error of last clock sync in ms
#endif

extern int64_t mono_clock_offset; //nanoseconds to add to monotonic clock to get real time. Is calculated on process start as difference between RT and Monotonic clock
extern clockid_t mono_clockid;

#include "iot_threadmsg.h"

void iot_init_systime(void);

inline uint64_t iot_get_systime(void) { //gets real time synchronized within hosts (in nanoseconds)
	struct timespec ts;
	clock_gettime(mono_clockid, &ts);
	return uint64_t(ts.tv_sec*1000000000+ts.tv_nsec)+mono_clock_offset+system_clock_offset;
}

inline uint64_t iot_get_reltime(void) { //gets relative monotonic time not affected by time correction.
										//Only difference between two consecutive values taken IN SAME PROCESS is meaningful
	struct timespec ts;
	clock_gettime(mono_clockid, &ts);
	return uint64_t(ts.tv_sec*1000000000+ts.tv_nsec);
}


inline uint16_t iot_get_systime_error(void) {
#ifndef IOT_SERVER
	return last_clock_sync_error; //TODO. add theoretical time difference on basis of value of time correction made last time
#else
	return 0;
#endif
}


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
	const uint32_t guid;
	const iot_hostid_t this_hostid; //ID of current host in user config

	iot_spinlock event_lock; //MUST COME BEFORE registries!!!! otherwise lock will be uninitialized during construction of registries (meshcontroller uses this lock)
	iot_peers_registry_t *const peers_registry;
	iot_configregistry_t *const config_registry;
	hwdev_registry_t *const hwdev_registry;
	iot_meshnet_controller *const meshcontroller;

	iot_modinstance_item_t *node_instances_head=NULL, *driver_instances_head=NULL, *detector_instances_head=NULL;

	uint64_t last_eventid_numerator=0; //must be preserved during restarts

	int error=0;
	bool is_shutdown=false;
	void (*on_shutdown)(void)=NULL; //will be called after finishing graceful shutdown sequence


	iot_gwinstance(uint32_t guid, iot_hostid_t hostid, uint64_t eventid_numerator);
	~iot_gwinstance(void);

	uint64_t next_event_numerator(void) { //returns next event numerator
		uint64_t low=iot_get_systime()-1'000'000'000ull*1'000'000'000ull; //decrease number of seconds since EPOCH by 1e9 seconds for cases of event id
		low-=low % 1000; //zero nanoseconds part to get microseconds multiplied by 1000

		event_lock.lock();
		if(last_eventid_numerator < low) last_eventid_numerator=low;
			else last_eventid_numerator++;
		uint64_t rval=last_eventid_numerator;
		event_lock.unlock();
printf("EVENT %" PRIu64 " allocated\n", rval);
		return rval;
	}

	void remove_modinstance(iot_modinstance_item_t *modinst);
	void graceful_shutdown(void (*on_shutdown_)(void));
	void graceful_shutdown_step2(void);
	void graceful_shutdown_step3(void);
	void graceful_shutdown_step4(void);
};


#endif //IOT_CORE_H
