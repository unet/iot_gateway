#ifndef IOT_DEVICEREGISTRY_H
#define IOT_DEVICEREGISTRY_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching


#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include<ecb.h>


#include <iot_kapi.h>
#include <kernel/iot_common.h>


class hwdev_registry_t;
struct iot_hwdevregistry_item_t;
extern hwdev_registry_t* hwdev_registry;

#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_kernel.h>

inline bool operator==(iot_hwdev_localident_t &i1, iot_hwdev_localident_t &i2) {
	if(i1.contype!=i2.contype || i1.unique_refid!=i2.unique_refid) return false;
	if(i1.contype<256) return true; //for built-in connection types ignore detector module in comparison
	return i1.detector_module_id==i2.detector_module_id;
}

inline bool operator==(iot_hwdev_ident_t &i1, iot_hwdev_ident_t &i2) {
	if(i1.hostid!=i2.hostid) return false;
	return i1.dev==i2.dev;
}


struct iot_hwdevregistry_item_t {
	iot_hwdevregistry_item_t *next, *prev;
	//TODO add next-prev fields for locations inside search indexes (by detector_module_id or devcontype) if necessary

	iot_hwdev_data_t devdata; //custom_data field will be assigned to custom_data buffer in current struct

	iot_modinstance_item_t* devdrv_modinst; //NULL if no driver connected or ref to driver module instance

	iot_devifacecls_item_t* devifaces[IOT_MAX_CLASSES_PER_DEVICE]; //list of available device iface classes (APIs for device communication), set by driver during driver init (zero if no driver attached)

	uint32_t custom_len_alloced:24, //real size of allocated space for custom_data (could be allocated with reserve or have more space from previous use)
			num_devifaces:4; //number of items in devifaces array
	alignas(4) char custom_data[];  //depends on devcontype and actual data
};

//singleton class to keep and manage registry of LOCAL HARDWARE devices
class hwdev_registry_t {
	iot_hwdevregistry_item_t* actual_dev_head; //bi-linked list of actual hw devices
	iot_hwdevregistry_item_t* removed_dev_head; //bi-linked list of hw devices which were removed but have active reference in driver modules


public:
	hwdev_registry_t(void) : actual_dev_head(NULL), removed_dev_head(NULL){
		assert(hwdev_registry==NULL);
		hwdev_registry=this;
	}
	void list_action(iot_action_t action, iot_hwdev_localident_t* ident, size_t custom_len, void* custom_data); //main thread

	void try_find_hwdev_for_driver(iot_module_item_t* module); //main thread

	iot_hwdevregistry_item_t* find_item(iot_hwdev_localident_t* ident) {
		iot_hwdevregistry_item_t* it=actual_dev_head;
		while(it) {
			if(it->devdata.dev_ident.dev==*ident) return it;
			it=it->next;
		}
		return NULL;
	}
};




#endif //IOT_DEVICEREGISTRY_H
