#include<stdint.h>
//#include<time.h>

#include "iot_deviceregistry.h"
#include "iot_moduleregistry.h"
#include "iot_deviceconn.h"

void hwdev_registry_t::on_newdev(void) {
		assert(uv_thread_self()==main_thread);
		uv_rwlock_rdlock(&devlist_lock);

		iot_hwdevregistry_item_t* it, *itnext=actual_dev_head;

		while((it=itnext)) {
			itnext=itnext->next;

			if(!it->is_new || it->devdrv_modinstlk || it->is_blocked) continue;
			it->is_new=0;
			modules_registry->try_find_driver_for_hwdev(it);
		}

		uv_rwlock_rdunlock(&devlist_lock);
	}

//'ident' is mandatory unless 'custom_data' provided and custom_data->fill_localident() will succeed
//errors:
//	IOT_ERROR_INVALID_ARGS - provided ident is template or action unknown
//	IOT_ERROR_NOT_FOUND - interface to ident's data not found (invalid or module cannot be loaded)
//	IOT_ERROR_TEMPORARY_ERROR - some temporary error (no memory etc). retry can succeed
//	IOT_ERROR_ACTION_CANCELLED - action IOT_ACTION_TRYREMOVE was used and device already had bound driver
int hwdev_registry_t::list_action(const iot_miid_t &detmiid, iot_action_t action, const iot_hwdev_localident* ident, const iot_hwdev_details* devdetails) {
//any thread		assert(uv_thread_self()==main_thread);
		iot_hwdev_ident_buffered identbuf(iot_current_hostid); //will be used if no ident provided, only devdetails
		if(!ident) { //try to fill from devdetails
			if(!devdetails || !identbuf.init_fromdetails(devdetails)) return IOT_ERROR_INVALID_ARGS;
			ident=identbuf.local;
		}
		char buf[256];
		int err=0;
		if(!ident->is_valid()) {
			outlog_error("Cannot find device connection interface for contype=%s", ident->get_typename());
			return IOT_ERROR_NOT_FOUND;
		}
		if(ident->is_tmpl()) {
			outlog_error("Got incomplete device data from DevDetector instance %d", unsigned(detmiid.iid));
			return IOT_ERROR_INVALID_ARGS;
		}
		size_t custom_len;
		uv_rwlock_wrlock(&devlist_lock);


		iot_hwdevregistry_item_t* it=find_item_byaddr(ident);
		switch(action) {
			case IOT_ACTION_REMOVE:
				if(!it || !it->dev_ident.local->matches_hwid(ident)) goto onexit; //not found or hwid does not match
				remove_hwdev(it);
				goto onexit;
			case IOT_ACTION_TRYREMOVE:
				if(!it || !it->dev_ident.local->matches_hwid(ident)) goto onexit; //not found or hwid does not match
				if(it->devdrv_modinstlk) {
					err=IOT_ERROR_ACTION_CANCELLED;
				} else {
					remove_hwdev(it);
				}
				goto onexit;
			case IOT_ACTION_ADD:
				if(it) {
					if(it->dev_ident.local->matches_hwid(ident) && (devdetails==it->dev_data /*only if both NULL*/ || (it->dev_data && devdetails && *it->dev_data==*devdetails))) {
						//same device, do nothing
						goto onexit;
					}
					outlog_debug_devreg("Replacing hwdev from DevDetector %u: %s", unsigned(it->detector_miid.iid), it->dev_ident.local->sprint(buf, sizeof(buf)));
				}
				break;
			default:
				//here we had useless search, but no such errors must ever happen
				outlog_error("Illegal action code %d", int(action));
				err=IOT_ERROR_INVALID_ARGS;
				goto onexit;
		}
		//do add/replace
		custom_len=devdetails ? devdetails->get_size() : 0;
		//remove if exists and buffer cannot be reused
		if(it && (it->custom_len_alloced < custom_len || it->devdrv_modinstlk)) { //not enough space in prev buffer for update or item is busy, so item cannot be reused
			remove_hwdev(it);
			it=NULL;
		}
		if(!it) {
			it=(iot_hwdevregistry_item_t*)malloc(sizeof(iot_hwdevregistry_item_t)+custom_len);
			if(!it) {
				outlog_error("No memory for hwdev!!! Action %d dropped for DevDetector instance %u, %s", int(action), unsigned(detmiid.iid), ident->sprint(buf, sizeof(buf)));
				err=IOT_ERROR_TEMPORARY_ERROR;
				goto onexit;
			}
			memset(it, 0, sizeof(*it));
			it->gwinst=gwinst;
			it->custom_len_alloced=custom_len;

			BILINKLIST_INSERTHEAD(it, actual_dev_head, next, prev);
		} else {
			it->is_blocked=0; //reset block for chances that device data changed and now some driver can use it
			it->is_removed=0;
			it->clear_module_block(0);
			if(it->dev_data) {
				it->dev_data->~iot_hwdev_details(); //destruct previous details
				it->dev_data=NULL;
			}
		}
		new(&it->dev_ident) iot_hwdev_ident_buffered(iot_current_hostid, ident);
		if(custom_len) {
			devdetails->copy_to(it->custom_data, custom_len);
			it->dev_data=(iot_hwdev_details*)it->custom_data;
		}
		it->detector_miid=detmiid;
		it->is_new=1;
		outlog_debug_devreg("Added new hwdev from DevDetector %u: %s", unsigned(it->detector_miid.iid),ident->sprint(buf, sizeof(buf)));

		uv_rwlock_wrunlock(&devlist_lock);

		signal_newdev();
		return 0;
onexit:
		uv_rwlock_wrunlock(&devlist_lock);
		return err;
	}

//returns false of item cannot be freed immediately and was moved to pending removal list
void hwdev_registry_t::remove_hwdev(iot_hwdevregistry_item_t* hwdevitem) { 
	//devlist_lock MUST BE W-locked!!!

//	assert(uv_thread_self()==main_thread); 

	outlog_debug_devreg_vars(char buf[256],"Removing hwdev from DevDetector instance %u: %s", unsigned(hwdevitem->detector_miid.iid), hwdevitem->dev_ident.local->sprint(buf, sizeof(buf)));

	BILINKLIST_REMOVE_NOCL(hwdevitem, next, prev);
	if(hwdevitem->devdrv_modinstlk) { //item is busy, move to list of removed items until released by driver
		BILINKLIST_INSERTHEAD(hwdevitem, removed_dev_head, next, prev);
		hwdevitem->is_removed=1;
		hwdevitem->devdrv_modinstlk.modinst->stop(false, true);
		return;
	}
	if(hwdevitem->dev_data) {
		hwdevitem->dev_data->~iot_hwdev_details(); //destruct previous details
		hwdevitem->dev_data=NULL;
	}
	free(hwdevitem);
}

void hwdev_registry_t::remove_hwdev_bydetector(const iot_miid_t &miid) {
	uv_rwlock_wrlock(&devlist_lock);

	iot_hwdevregistry_item_t* it, *itnext=actual_dev_head;

	while((it=itnext)) {
		itnext=itnext->next;
		if(it->detector_miid==miid) remove_hwdev(it);
	}
	uv_rwlock_wrunlock(&devlist_lock);
}

//after loading new driver module tries to find suitable hw device
void hwdev_registry_t::try_find_hwdev_for_driver(iot_driver_module_item_t* module) {
	assert(uv_thread_self()==main_thread);
	assert(module->state==IOT_MODULESTATE_OK);

	if(need_exit) return;

	uv_rwlock_rdlock(&devlist_lock);

	uint64_t now=uv_now(main_loop);
	uint32_t now32=uint32_t((now+500)/1000);
	char namebuf[256];

	iot_hwdevregistry_item_t* it, *itnext=actual_dev_head;

	while((it=itnext)) {
		itnext=itnext->next;

		if(it->devdrv_modinstlk) continue; //device with already connected driver
		if(it->is_blocked) continue;

		//check if module is not blocked for this specific hw device
		if(it->is_module_blocked(module->dbitem->module_id, now32)) continue;

		int err=module->try_driver_create(it);
		if(!err) break; //success
		if(err==IOT_ERROR_MODULE_BLOCKED) break;
		if(err==IOT_ERROR_TEMPORARY_ERROR || err==IOT_ERROR_NOT_READY ||  err==IOT_ERROR_CRITICAL_ERROR) { //for async starts block module too. it will be unblocked after getting success confirmation
																		//and nothing must be done in case of error
			if(!it->block_module(module->dbitem->module_id,  err==IOT_ERROR_CRITICAL_ERROR ? 0xFFFFFFFFu : now32+2*60*1000, now32)) { //delay retries of this module for current hw device
				outlog_error("HWDev used all module-bloking slots and was blocked: %s", it->dev_ident.local->sprint(namebuf, sizeof(namebuf)));
				it->is_blocked=1;
			}
			if(err==IOT_ERROR_NOT_READY) break;
		}
		//else IOT_ERROR_NOT_SUPPORTED so just continue search
	}
	uv_rwlock_rdunlock(&devlist_lock);
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
	uv_rwlock_rdlock(&devlist_lock);

	//finds among local hwdevices with started driver
	iot_hwdevregistry_item_t* it, *itnext=actual_dev_head;
	int err;
	bool wastemperr=false;

	while((it=itnext)) {
		itnext=itnext->next;

		if(!it->devdrv_modinstlk || !it->devdrv_modinstlk.modinst->is_working_not_stopping()) continue; //skip devices without driver or with non-started driver

		err=conn->connect_local(it->devdrv_modinstlk.modinst);
		if(!err || err==IOT_ERROR_NOT_READY || err==IOT_ERROR_NO_MEMORY) goto onexit; //success or fatal error  //   || err==IOT_ERROR_CRITICAL_ERROR
		if(err==IOT_ERROR_TEMPORARY_ERROR || err==IOT_ERROR_HARD_LIMIT_REACHED) {
			wastemperr=true;
			continue;
		}
		assert(err==IOT_ERROR_NOT_SUPPORTED); //just continue
	}
	err=wastemperr ? IOT_ERROR_TEMPORARY_ERROR : IOT_ERROR_NOT_FOUND;
onexit:
	uv_rwlock_rdunlock(&devlist_lock);
	return err;
}

void iot_hwdevregistry_item_t::on_driver_destroy(iot_modinstance_item_t* modinst) { //called when driver instance is freed
		assert(uv_thread_self()==main_thread);

		if(!devdrv_modinstlk) return;
		assert(devdrv_modinstlk.modinst==modinst);

		devdrv_modinstlk.unlock();
		if(modinst->state==IOT_MODINSTSTATE_INITED) { //stopped successfully or not ever started
			if(is_removed) gwinst->hwdev_registry->finish_hwdev_removal(this);
				else modules_registry->try_find_driver_for_hwdev(this);
		} else { //hung
			assert(modinst->state==IOT_MODINSTSTATE_HUNG);
			if(is_removed) { //allow to clean removed devices from registry even with hung driver
				gwinst->hwdev_registry->finish_hwdev_removal(this);
			}
		}
	}

