#ifndef IOT_DEVICEREGISTRY_H
#define IOT_DEVICEREGISTRY_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching


#include<stdint.h>
#include<stdlib.h>
#include<assert.h>
//#include<time.h>

#include "iot_core.h"


struct iot_hwdevregistry_item_t;
class hwdev_registry_t;

//#include "iot_moduleregistry.h"

#define IOT_CONFIG_MAX_BLOCKED_MODULES_PER_HWDEV 8


struct iot_hwdevregistry_item_t {
	iot_hwdevregistry_item_t *next, *prev; //position in actual_dev_head or removed_dev_head (when is_removed true)
	//TODO add next-prev fields for locations inside search indexes (by detector_module_id or devcontype) if necessary

	iot_gwinstance* gwinst;

	iot_hwdev_ident_buffered dev_ident;
	iot_hwdev_details* dev_data; //will be assigned to custom_data buffer in current struct
	iot_miid_t detector_miid; //miid of detector instance

	iot_objref_ptr<iot_modinstance_item_t> devdrv_modinstlk; //NULL if no driver connected or ref to driver module instance

	struct {
		uint32_t module_id; //zero for unused slot
		uint32_t tm;		//0xFFFFFFFF if blocked forever
	} blocked[IOT_CONFIG_MAX_BLOCKED_MODULES_PER_HWDEV];

	uint32_t custom_len_alloced:24, //real size of allocated space for custom_data (could be allocated with reserve or have more space from previous use)
			is_blocked:1,		//flag that hw device is blocked from finding a driver
			is_new:1,			//flag that hw device was recently added and not yet tried to be connected to driver
			is_removed:1;		//flag that this item is in removed_dev_head list
	alignas(sizeof(void*)) char custom_data[];  //depends on devcontype and actual data


	bool is_module_blocked(uint32_t module_id, uint32_t now32) {
		if(!module_id) {
			assert(false);
			return false;
		}
		for(int i=0;i<IOT_CONFIG_MAX_BLOCKED_MODULES_PER_HWDEV;i++) {
			if(blocked[i].module_id!=module_id) continue;
			if(blocked[i].tm==0xFFFFFFFFu || blocked[i].tm>now32) return true;
			//timeout ended
			blocked[i].module_id=0;
			break;
		}
		return false;
	}
	void clear_module_block(uint32_t module_id) { //zero value clears for all
		if(!module_id) {
			memset(blocked,0,sizeof(blocked));
			return;
		}
		for(int i=0;i<IOT_CONFIG_MAX_BLOCKED_MODULES_PER_HWDEV;i++) {
			if(blocked[i].module_id!=module_id) continue;
			blocked[i].module_id=0;
			break;
		}
	}
	bool block_module(uint32_t module_id, uint32_t till, uint32_t now32) {
		//returns false if all slots are busy
		if(!module_id) {
			assert(false);
			return false;
		}
		for(int i=0;i<IOT_CONFIG_MAX_BLOCKED_MODULES_PER_HWDEV;i++) {
			if(!blocked[i].module_id || blocked[i].module_id==module_id || blocked[i].tm<=now32) {
				blocked[i]={.module_id=module_id, .tm=till};
				return true;
			}
		}
		return false;
	}
	void on_driver_destroy(iot_modinstance_item_t* modinst); //called when driver instance is freed
};

//singleton class to keep and manage registry of LOCAL HARDWARE devices
class hwdev_registry_t {
	iot_hwdevregistry_item_t* actual_dev_head=NULL; //bi-linked list of actual hw devices
	iot_hwdevregistry_item_t* removed_dev_head=NULL; //bi-linked list of hw devices which were removed but have active reference in driver modules
	uv_async_t newdev_watcher; //gets signaled when new hwdev is added to start search for driver
	uv_rwlock_t devlist_lock; //protects access to actual_dev_head and removed_dev_head

public:
	iot_gwinstance* const gwinst;
	bool have_unconnected_devs=false; //flag that there are hw devices without driver and some drivers were delayed due to temp errors, so periodic search must be attempted
								//TODO. make periodic recheck every 2 minutes

	hwdev_registry_t(iot_gwinstance* gwinst_) : gwinst(gwinst_) {
		assert(uv_thread_self()==main_thread);
		uv_async_init(main_loop, &newdev_watcher, [](uv_async_t* handle) -> void {
					hwdev_registry_t* obj=(hwdev_registry_t*)(handle->data);
					obj->on_newdev();
				});
		newdev_watcher.data=this;
		int err=uv_rwlock_init(&devlist_lock);
		assert(err==0);
	}
	~hwdev_registry_t(void) {
		uv_rwlock_destroy(&devlist_lock);
		uv_close((uv_handle_t*)&newdev_watcher, NULL);
	}
	void signal_newdev(void) {
		uv_async_send(&newdev_watcher);
	}
	void on_newdev(void);
	int list_action(const iot_miid_t &detmiid, iot_action_t action, const iot_hwdev_localident* ident, const iot_hwdev_details* custom_data); //any thread
	//finish removal of removed device after stopping bound driver
	void finish_hwdev_removal(iot_hwdevregistry_item_t* it) { //main thread
		uv_rwlock_wrlock(&devlist_lock);
		assert(it->is_removed);
		assert(!it->devdrv_modinstlk);
		BILINKLIST_REMOVE(it, next, prev); //remove from removed list
		uv_rwlock_wrunlock(&devlist_lock);

		if(it->dev_data) {
			it->dev_data->~iot_hwdev_details(); //destruct previous details
			it->dev_data=NULL;
		}
		free(it);
	}

	void try_find_hwdev_for_driver(iot_driver_module_item_t* module); //main thread
	int try_connect_local_driver(iot_device_connection_t* conn);

	iot_hwdevregistry_item_t* find_item_bytmpl(iot_hwdev_localident* tmpl) { //looks for device item by contype and address
		assert(false); //TODO correct locking of devlist_lock. for now application of this func is unknown
		iot_hwdevregistry_item_t* it=actual_dev_head;
		while(it) {
			if(tmpl->matches(it->dev_ident.local)) return it;
			it=it->next;
		}
		return NULL;
	}
	void remove_hwdev_bydetector(const iot_miid_t &miid);
private:
	void remove_hwdev(iot_hwdevregistry_item_t* hwdevitem); //devlist_lock must be W-locked!!!
	iot_hwdevregistry_item_t* find_item_byaddr(const iot_hwdev_localident* ident) { //looks for device item by contype and address
		//devlist_lock MUST BE LOCKED!
		iot_hwdevregistry_item_t* it=actual_dev_head;
		while(it) {
			if(it->dev_ident.local->matches_addr(ident)) return it;
			it=it->next;
		}
		return NULL;
	}
};




#endif //IOT_DEVICEREGISTRY_H
