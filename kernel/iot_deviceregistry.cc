#include<stdint.h>
//#include<time.h>

#include<iot_kapi.h>
#include<iot_utils.h>
#include<kernel/iot_daemonlib.h>
#include<kernel/iot_deviceregistry.h>
#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_kernel.h>


//#include<ecb.h>


hwdev_registry_t* hwdev_registry=NULL;
static hwdev_registry_t _hwdev_registry; //instantiate singleton

void hwdev_registry_t::list_action(iot_action_t action, iot_hwdev_localident_t* ident, size_t custom_len, void* custom_data) {
		assert(uv_thread_self()==main_thread);

		iot_hwdevregistry_item_t* it=find_item(ident);
		switch(action) {
			case IOT_ACTION_REMOVE:
				if(!it) {
					outlog_debug("Attempt to remove non-existing hwdev from DevDetector %u, unique_id %lu", ident->detector_module_id, ident->unique_refid);
					return;
				}
				outlog_debug("Removing hwdev from DevDetector %u, unique_id %lu", ident->detector_module_id, ident->unique_refid);
				BILINKLIST_REMOVE_NOCL(it, next, prev); //remove from actual list

				if(it->devdrv_modinst) { //item if busy, move to list of removed items until released by driver
					BILINKLIST_INSERTHEAD(it, removed_dev_head, next, prev);
//					kern_notifydriver_removedhwdev(it);//TODO. must schedule stop request for driver with subsequent deinit
				} else {
					free(it);
				}
				return;
			case IOT_ACTION_ADD:
				if(it) outlog_debug("Replacing duplicate hwdev instead of adding from DevDetector %u, unique_id %lu", ident->detector_module_id, ident->unique_refid);
				break;
			case IOT_ACTION_REPLACE:
				if(!it) outlog_debug("Adding new hwdev instead of replacing from DevDetector %u, unique_id %lu", ident->detector_module_id, ident->unique_refid);
				break;
			default:
				//here we have useless search, but no such errors must ever happen
				outlog_error("Illegal action code %d", int(action));
				return;
		}
		//do add/replace
		//remove if exists and buffer cannot be reused
		if(it && (it->custom_len_alloced < custom_len || it->devdrv_modinst)) { //not enough space in prev buffer for update or item is busy (and thus cannot be updated)
			BILINKLIST_REMOVE_NOCL(it, next, prev);
			if(it->devdrv_modinst) { //item is busy, move to list of removed items until released by driver
				BILINKLIST_INSERTHEAD(it, removed_dev_head, next, prev);
//				kern_notifydriver_removedhwdev(it);//TODO. must schedule stop request for driver with subsequent deinit
			} else {
				free(it);
			}
			it=NULL;
		}
		if(!it) {
			it=(iot_hwdevregistry_item_t*)malloc(sizeof(iot_hwdevregistry_item_t)+custom_len);
			if(!it) {
				outlog_error("No memory for hwdev!!! Action %d dropped for DevDetector %u, unique_id %llu", int(action), ident->detector_module_id, ident->unique_refid);
				return;
			}
			memset(it, 0, sizeof(*it));
			it->custom_len_alloced=custom_len;
			it->devdata.dev_ident.hostid=iot_current_hostid;
			it->devdata.dev_ident.dev=*ident;
			it->devdata.custom_data=it->custom_data;
		} //else dev_ident is the same and custom_data buffer is enough for new custom data
		it->devdata.custom_len=custom_len;
		memmove(it->custom_data, custom_data, custom_len);
		BILINKLIST_INSERTHEAD(it, actual_dev_head, next, prev);
		modules_registry->try_find_driver_for_hwdev(it);
	}


//after loading new driver tries to find suitable hw device
void hwdev_registry_t::try_find_hwdev_for_driver(iot_module_item_t* module) {
	assert(uv_thread_self()==main_thread);

	iot_hwdevregistry_item_t* it, *itnext=actual_dev_head;
	auto drv_iface=module->config->iface_device_driver;

	int err;
	while(itnext) {
		it=itnext;
		itnext=itnext->next;

		if(it->devdrv_modinst) continue; //device with connected driver

		if(drv_iface->check_device) { //use this func for precheck if available
			err=drv_iface->check_device(&it->devdata);
			if(err) {
				if(err==IOT_ERROR_DEVICE_NOT_SUPPORTED) continue;
				outlog_error("Driver module with ID %u got error during check_device: %s", module->config->module_id, kapi_strerror(err));
				if(err==IOT_ERROR_CRITICAL_BUG) module->driver_blocked=1;
				return;
			}
		}
		err=modules_registry->create_driver_modinstance(module, it);
		if(!err) return;

		if(err==IOT_ERROR_DEVICE_NOT_SUPPORTED) continue;
		outlog_error("Driver module with ID %u got error during init: %s", module->config->module_id, kapi_strerror(err));
		if(err==IOT_ERROR_CRITICAL_BUG) module->driver_blocked=1;

		//TODO setup retry if some driver returned recoverable error
		return;
	}
}

