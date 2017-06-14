#include<stdint.h>
//#include<time.h>

#include<iot_module.h>
#include<iot_utils.h>
#include<kernel/iot_daemonlib.h>
#include<kernel/iot_deviceregistry.h>
#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_kernel.h>


//#include<ecb.h>


hwdev_registry_t* hwdev_registry=NULL;
static hwdev_registry_t _hwdev_registry; //instantiate singleton
//errors:
//	IOT_ERROR_INVALID_ARGS - provided ident is template or action unknown
//	IOT_ERROR_NOT_FOUND - interface to ident's data not found (invalid or module cannot be loaded)
//	IOT_ERROR_TEMPORARY_ERROR - some temporary error (no memory etc). retry can succeed
int hwdev_registry_t::list_action(const iot_miid_t &detmiid, iot_action_t action, iot_hwdev_localident_t* ident, size_t custom_len, void* custom_data) {
		assert(uv_thread_self()==main_thread);
		char buf[256];
		const iot_hwdevident_iface* ident_iface=ident->find_iface(true);
		if(!ident_iface) {
			outlog_error("Cannot find device connection interface for device from DevDetector %u, contype=%u", ident->detector_module_id,unsigned(ident->contype));
			return IOT_ERROR_NOT_FOUND;
		}
		if(ident_iface->is_tmpl(ident)) {
			outlog_error("Got incomplete device data from DevDetector %u", ident->detector_module_id);
			return IOT_ERROR_INVALID_ARGS;
		}

		iot_hwdevregistry_item_t* it=find_item_byaddr(ident);
		switch(action) {
			case IOT_ACTION_REMOVE:
				if(!it || !it->devdata.ident_iface->matches_hwid(&it->devdata.dev_ident.dev, ident)) return 0; //not found or hwid does not match
				remove_hwdev(it);
				return 0;
			case IOT_ACTION_ADD:
//				if(it) outlog_debug("Replacing duplicate hwdev instead of adding from DevDetector %u: %s", ident->detector_module_id, it->devdata.ident_iface->sprint(&it->devdata.dev_ident.dev,buf, sizeof(buf)));
				break;
			default:
				//here we had useless search, but no such errors must ever happen
				outlog_error("Illegal action code %d", int(action));
				return IOT_ERROR_INVALID_ARGS;
		}
		//do add/replace
		//remove if exists and buffer cannot be reused
		if(it && (it->custom_len_alloced < custom_len || it->devdrv_modinstlk)) { //not enough space in prev buffer for update or item is busy (and thus cannot be updated)
			remove_hwdev(it);
			it=NULL;
		}
		if(!it) {
			it=(iot_hwdevregistry_item_t*)malloc(sizeof(iot_hwdevregistry_item_t)+custom_len);
			if(!it) {
				outlog_error("No memory for hwdev!!! Action %d dropped for DevDetector %u, %s", int(action), ident->detector_module_id, ident_iface->sprint(ident, buf, sizeof(buf)));
				return IOT_ERROR_TEMPORARY_ERROR;
			}
			memset(it, 0, sizeof(*it));
			it->custom_len_alloced=custom_len;
			it->devdata.dev_ident.hostid=iot_current_hostid;
			it->devdata.ident_iface=ident_iface;
			it->devdata.custom_data=it->custom_data;

			BILINKLIST_INSERTHEAD(it, actual_dev_head, next, prev);
		} else {
			it->is_blocked=0; //reset block for chances that device data changed and now some driver can use it
			it->clear_module_block(0);
		}
		it->devdata.dev_ident.dev=*ident; //address part in devdata.dev_ident.dev is the same but hwid can be different
		it->devdata.custom_len=custom_len;
		memcpy(it->custom_data, custom_data, custom_len);
		it->detector_miid=detmiid;
		modules_registry->try_find_driver_for_hwdev(it);
		return 0;
	}

void hwdev_registry_t::remove_hwdev(iot_hwdevregistry_item_t* hwdevitem) {
	assert(uv_thread_self()==main_thread);
	char buf[256];
	auto ident=&hwdevitem->devdata.dev_ident.dev;
	outlog_debug("Removing hwdev from DevDetector %u: %s", ident->detector_module_id, hwdevitem->devdata.ident_iface->sprint(ident,buf, sizeof(buf)));

	BILINKLIST_REMOVE_NOCL(hwdevitem, next, prev);
	if(hwdevitem->devdrv_modinstlk) { //item is busy, move to list of removed items until released by driver
		BILINKLIST_INSERTHEAD(hwdevitem, removed_dev_head, next, prev);
		hwdevitem->is_removed=1;
		hwdevitem->devdrv_modinstlk.modinst->stop(false, true);
	} else {
		free(hwdevitem);
	}

}

void hwdev_registry_t::remove_hwdev_bydetector(const iot_miid_t &miid) {
	iot_hwdevregistry_item_t* it, *itnext=actual_dev_head;

	while((it=itnext)) {
		itnext=itnext->next;
		if(it->detector_miid==miid) remove_hwdev(it);
	}
}

//after loading new driver module tries to find suitable hw device
void hwdev_registry_t::try_find_hwdev_for_driver(iot_module_item_t* module) {
	assert(uv_thread_self()==main_thread);
	assert(module->state[IOT_MODINSTTYPE_DRIVER]==IOT_MODULESTATE_OK);

	if(need_exit) return;

	uint64_t now=uv_now(main_loop);
	uint32_t now32=uint32_t((now+500)/1000);
	char namebuf[256];

	iot_hwdevregistry_item_t* it, *itnext=actual_dev_head;

	while((it=itnext)) {
		itnext=itnext->next;

		if(it->devdrv_modinstlk) continue; //device with already connected driver
		if(it->is_blocked) continue;

		//check if module is not blocked for this specific hw device
		if(it->is_module_blocked(module->config->module_id, now32)) continue;

		int err=module->try_driver_create(it);
		if(!err) break; //success
		if(err==IOT_ERROR_MODULE_BLOCKED) return;
		if(err==IOT_ERROR_TEMPORARY_ERROR || err==IOT_ERROR_NOT_READY ||  err==IOT_ERROR_CRITICAL_ERROR) { //for async starts block module too. it will be unblocked after getting success confirmation
																		//and nothing must be done in case of error
			if(!it->block_module(module->config->module_id,  err==IOT_ERROR_CRITICAL_ERROR ? 0xFFFFFFFFu : now32+2*60*1000, now32)) { //delay retries of this module for current hw device
				outlog_error("HWDev used all module-bloking slots and was blocked: %s", it->devdata.ident_iface->sprint(&it->devdata.dev_ident.dev,namebuf, sizeof(namebuf)));
				it->is_blocked=1;
			}
			if(err==IOT_ERROR_NOT_READY) return;
		}
		//else IOT_ERROR_DEVICE_NOT_SUPPORTED so just continue search
	}
}

//returns:
//	0 - driver found and connection established
//	IOT_ERROR_NOT_READY - driver found and connection is in progress
//	IOT_ERROR_NO_MEMORY - not enough memory, retry scheduled. search should be stopped
//	IOT_ERROR_NOT_FOUND - no driver found (or program needs exit)
//	IOT_ERROR_TEMPORARY_ERROR - no driver found but some driver(s) returned temp error, so task will be retried
//////	IOT_ERROR_CRITICAL_ERROR - client instance cannot setup connection due to bad or absent client_hwdevident, so current device connection should be blocked until update
//								of client_hwdevident. connection struct should be released!!!
int hwdev_registry_t::try_connect_local_driver(iot_device_connection_t* conn) { //tries to find driver to setup connection for local? client (remote clients have similar loop on their host).
//connection must be in INIT state
	assert(uv_thread_self()==main_thread);
	assert(conn->state==conn->IOT_DEVCONN_INIT);

	if(need_exit) return IOT_ERROR_NOT_FOUND;

	//finds among local hwdevices with started driver
	iot_hwdevregistry_item_t* it, *itnext=actual_dev_head;
	int err;
	bool wastemperr=false;

	while((it=itnext)) {
		itnext=itnext->next;

		if(!it->devdrv_modinstlk || !it->devdrv_modinstlk.modinst->is_working_not_stopping()) continue; //skip devices without driver or with non-started driver

		err=conn->connect_local(it->devdrv_modinstlk.modinst);
		if(!err || err==IOT_ERROR_NOT_READY || err==IOT_ERROR_NO_MEMORY) return err; //success or fatal error  //   || err==IOT_ERROR_CRITICAL_ERROR
		if(err==IOT_ERROR_TEMPORARY_ERROR || err==IOT_ERROR_HARD_LIMIT_REACHED) {
			wastemperr=true;
			continue;
		}
		assert(err==IOT_ERROR_DEVICE_NOT_SUPPORTED); //just continue
	}
	return wastemperr ? IOT_ERROR_TEMPORARY_ERROR : IOT_ERROR_NOT_FOUND;
}

void iot_hwdevregistry_item_t::on_driver_destroy(iot_modinstance_item_t* modinst) { //called when driver instance is freed
		if(!devdrv_modinstlk) return;
		assert(devdrv_modinstlk.modinst==modinst);

		devdrv_modinstlk.unlock();
		if(modinst->state==IOT_MODINSTSTATE_INITED) { //stopped successfully or not ever started
			if(is_removed) hwdev_registry->finish_hwdev_removal(this);
				else modules_registry->try_find_driver_for_hwdev(this);
		} else { //hung
			assert(modinst->state==IOT_MODINSTSTATE_HUNG);
			if(is_removed) { //allow to clean removed devices from registry even with hung driver
				hwdev_registry->finish_hwdev_removal(this);
			}
		}
	}

