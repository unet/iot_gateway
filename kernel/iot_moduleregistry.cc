#include<stdint.h>
#include <dlfcn.h>
//#include<time.h>
#include <new>

#include<iot_module.h>
#include<iot_utils.h>
#include<kernel/iot_daemonlib.h>
#include<kernel/iot_deviceregistry.h>
#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_configregistry.h>
#include<kernel/iot_peerconnection.h>
#include<kernel/iot_kernel.h>


#define IOT_MODULESDB_BUNDLE_OBJ(vendor, bundle) ECB_CONCAT(iot_moddb_bundle_, ECB_CONCAT(vendor, ECB_CONCAT(__, bundle)))
#define IOT_MODULESDB_BUNDLE_REC(vendor, bundle) static iot_modulesdb_bundle_t IOT_MODULESDB_BUNDLE_OBJ(vendor, bundle)

#define UV_LIB_INIT {NULL, NULL}

#include "bundles-db.h"	

static iot_modulesdb_item_t modules_db[]={
#include "modules-db.h"	
};

#define MODULES_DB_ITEMS (sizeof(modules_db)/sizeof(modules_db[0]))


iot_modules_registry_t *modules_registry=NULL;
static iot_modules_registry_t _modules_registry; //instantiate singleton

static iot_modinstance_item_t iot_modinstances[IOT_MAX_MODINSTANCES]; //zero item is not used
static iot_iid_t last_iid=0; //holds last assigned module instance id

const char* iot_modinsttype_name[IOT_MODINSTTYPE_MAX+1]={
	"detector",
	"driver",
	"event source"
};

//finds module's index in modules DB by module_id
//returns -1 if not found
static inline int module_db_index_by_id(uint32_t module_id) { //TODO make some index for quick search
	for(unsigned i=0;i<MODULES_DB_ITEMS;i++) if(modules_db[i].module_id==module_id) return i;
	return -1;
}

static int bundlepath2symname(const char *path, char *buf, size_t bufsz) {
	char *bufend=buf+bufsz-1;
	char *cur=buf;
	while(*path && cur<bufend) {
		if(*path=='/') {
			*(cur++)='_';
			if(cur<bufend) *(cur++)='_';
			path++;
		} else {
			*(cur++)=*(path++);
		}
	}
	if(cur<=bufend) *cur='\0';
	return int(cur-buf);
}

const iot_hwdevident_iface* iot_hwdev_localident_t::find_iface(bool tryload) const { //searches for connection type interface class realization in local registry
	//must run in main thread if tryload is true
	//returns NULL if interface not found or cannot be loaded
	iot_devcontype_item_t* it=modules_registry->find_devcontype(contype, tryload);
	if(it && it->iface->is_valid(this)) return it->iface;
	return NULL;
}

const iot_devifaceclassdata_iface* iot_devifaceclass_data::find_iface(bool tryload) const { //searches for connection type interface class realization in local registry
	//must run in main thread if tryload is true
	//returns NULL if interface not found or cannot be loaded
	iot_devifaceclass_item_t* it=modules_registry->find_devifaceclass(classid, tryload);
	if(it && it->iface->is_valid(this)) return it->iface;
	return NULL;
}


void iot_modules_registry_t::create_detector_modinstance(iot_module_item_t* module) {
	assert(uv_thread_self()==main_thread);

	iot_modinstance_type_t type=IOT_MODINSTTYPE_DETECTOR;
	assert(module->state[type]==IOT_MODULESTATE_OK);

	if(module->detector_instance) return; //single instance already created

	iot_device_detector_base *inst=NULL;
	iot_modinstance_item_t *modinst=NULL;
	iot_thread_item_t* thread=NULL;
	
	auto iface=module->config->iface_device_detector;
	int err=0;

	if(iface->check_system) { //use this func for precheck if available
		err=iface->check_system();
		if(err) {
			if(err!=IOT_ERROR_DEVICE_NOT_SUPPORTED)
				outlog_error("Detector module '%s::%s' with ID %u got error during check_system: %s", module->dbitem->bundle->name, module->dbitem->module_name, module->config->module_id, kapi_strerror(err));

			if(err!=IOT_ERROR_TEMPORARY_ERROR || module->errors[type]>10) {
				module->state[type]=IOT_MODULESTATE_DISABLED;
				return;
			}
			goto ontemperr;
		}
	} //else assume check_system returned success

	//create instance

	//from here all errors go to onerr

	thread=thread_registry->assign_thread(iface->cpu_loading);
	assert(thread!=NULL);

	err=iface->init_instance(&inst, thread->thread);
	if(err) {
		if(err!=IOT_ERROR_DEVICE_NOT_SUPPORTED)
			outlog_error("Detector instance init for module '%s::%s' with ID %u returned error: %s",module->dbitem->bundle->name, module->dbitem->module_name, module->config->module_id, kapi_strerror(err));

		if(err!=IOT_ERROR_TEMPORARY_ERROR || module->errors[type]>10) {
			module->state[type]=IOT_MODULESTATE_DISABLED;
			goto onerr;
		}
		goto ontemperr;
	}
	if(!inst) {
		//no error and no instance. this is a bug
		outlog_error("Detector instance init for module '%s::%s' with ID %u returned NULL pointer. This is a bug.",module->dbitem->bundle->name, module->dbitem->module_name, module->config->module_id);
		module->state[type]=IOT_MODULESTATE_DISABLED;
		goto onerr;
	}

	modinst=register_modinstance(module, type, thread, inst, NULL);
	if(!modinst) goto ontemperr;
	inst=NULL; //already saved in modinstance structure, so will be deinit when freeing it

	modinst->start(false);
	return;

ontemperr:
	//here we have IOT_ERROR_TEMPORARY_ERROR
	module->state[type]=IOT_MODULESTATE_BLOCKED;
	module->errors[type]++;
	module->timeout[type]=uv_now(main_loop)+module->errors[type]*5*60*1000;
	module->recheck_job(true);

onerr:
	if(modinst) free_modinstance(modinst);
	else {
		if(inst) {
			if((err=iface->deinit_instance(inst))) { //any error from deinit is critical bug
				outlog_error("%s instance DEinit for module '%s::%s' with ID %u returned error: %s. Module is blocked.",iot_modinsttype_name[type], module->dbitem->bundle->name, module->dbitem->module_name, module->config->module_id, kapi_strerror(err));
				module->state[type]=IOT_MODULESTATE_DISABLED;
				iot_process_module_bug(module);
			}
		}
	}
}

//try to start specific driver for specific device
//Returns:
//	0 - success, driver instance created and started
//	IOT_ERROR_NOT_READY - temporary success. instance start is async, result not available right now
//	IOT_ERROR_DEVICE_NOT_SUPPORTED - success result, but module cannot work with provided device
//	IOT_ERROR_TEMPORARY_ERROR - error. module can be retried for this device later
//	IOT_ERROR_MODULE_BLOCKED - error. module was blocked on temporary or constant basis
//	IOT_ERROR_CRITICAL_ERROR - instanciation is invalid
int iot_module_item_t::try_driver_create(iot_hwdevregistry_item_t* hwdev) { //main thread
	assert(uv_thread_self()==main_thread);

	auto drv_iface=config->iface_device_driver;
	int err=0;

	if(drv_iface->check_device) { //use this func for precheck if available
		err=drv_iface->check_device(&hwdev->devdata);
		if(err) {
			if(err==IOT_ERROR_DEVICE_NOT_SUPPORTED) return IOT_ERROR_DEVICE_NOT_SUPPORTED;
			outlog_error("Driver module '%s::%s' with ID %u got error during check_device: %s", dbitem->bundle->name, dbitem->module_name, config->module_id, kapi_strerror(err));
			if(err==IOT_ERROR_INVALID_DEVICE_DATA) return IOT_ERROR_DEVICE_NOT_SUPPORTED; //such errors should be logged, but irrelevant for caller of this func

			if(err==IOT_ERROR_CRITICAL_BUG || err!=IOT_ERROR_TEMPORARY_ERROR || errors[IOT_MODINSTTYPE_DRIVER]>10) {
				state[IOT_MODINSTTYPE_DRIVER]=IOT_MODULESTATE_DISABLED;
			} else {
				state[IOT_MODINSTTYPE_DRIVER]=IOT_MODULESTATE_BLOCKED;
				errors[IOT_MODINSTTYPE_DRIVER]++;
				timeout[IOT_MODINSTTYPE_DRIVER]=uv_now(main_loop)+errors[IOT_MODINSTTYPE_DRIVER]*5*60*1000;
				recheck_job(true);
			}
			return IOT_ERROR_MODULE_BLOCKED;
		}
	} //else assume check_device returned success
	err=modules_registry->create_driver_modinstance(this, hwdev);
	return err;
}

void iot_module_item_t::recheck_job(bool no_jobs) { //reschedules timers and/or runs scheduled tasks (if no no_jobs)
	//no_jobs prevents jobs from running, just reschedules timers
	//called in main thread
	assert(uv_thread_self()==main_thread);
	if(in_recheck_job) return;
	in_recheck_job=true;
	uint64_t now=uv_now(main_loop);
	uint64_t delay=0xFFFFFFFFFFFFFFFFul;

	//find most early event
	if(state[IOT_MODINSTTYPE_DETECTOR]==IOT_MODULESTATE_BLOCKED) { //detector start is delayed
		if(timeout[IOT_MODINSTTYPE_DETECTOR]<=now) { //block period elapsed
			state[IOT_MODINSTTYPE_DETECTOR]=IOT_MODULESTATE_OK;
			if(!no_jobs) modules_registry->create_detector_modinstance(this);
				else delay=0;
		}
		if(state[IOT_MODINSTTYPE_DETECTOR]==IOT_MODULESTATE_BLOCKED) {
			if(timeout[IOT_MODINSTTYPE_DETECTOR]>now) {
				if(timeout[IOT_MODINSTTYPE_DETECTOR]-now<delay) delay=timeout[IOT_MODINSTTYPE_DETECTOR]-now;
			}
			else delay=0; //this will choose most quick recheck timer
		}
	}
	if(state[IOT_MODINSTTYPE_DRIVER]==IOT_MODULESTATE_BLOCKED) {
		if(timeout[IOT_MODINSTTYPE_DRIVER]<=now) { //block period elapsed
			state[IOT_MODINSTTYPE_DRIVER]=IOT_MODULESTATE_OK;
			if(!no_jobs) hwdev_registry->try_find_hwdev_for_driver(this);
				else delay=0;
		}
		if(state[IOT_MODINSTTYPE_DRIVER]==IOT_MODULESTATE_BLOCKED) {
			if(timeout[IOT_MODINSTTYPE_DRIVER]>now) {
				if(timeout[IOT_MODINSTTYPE_DRIVER]-now<delay) delay=timeout[IOT_MODINSTTYPE_DRIVER]-now;
			}
			else delay=0; //this will choose most quick recheck timer
		}
	}
/*todo	if(state[IOT_MODINSTTYPE_EVSOURCE]==IOT_MODULESTATE_BLOCKED) {
		if(timeout[IOT_MODINSTTYPE_EVSOURCE]<=now) { //block period elapsed
			state[IOT_MODINSTTYPE_EVSOURCE]=IOT_MODULESTATE_OK;
			if(!no_jobs) config_registry->try_create_evsources(this);
				else delay=0;
		}
		if(state[IOT_MODINSTTYPE_EVSOURCE]==IOT_MODULESTATE_BLOCKED) {
			if(timeout[IOT_MODINSTTYPE_EVSOURCE]>now) {
				if(timeout[IOT_MODINSTTYPE_EVSOURCE]-now<delay) delay=timeout[IOT_MODINSTTYPE_EVSOURCE]-now;
			}
			else delay=0; //this will choose most quick recheck timer
		}
	}
*/
	in_recheck_job=false;
	if(delay<0xFFFFFFFFFFFFFFFFul) {
		if(!no_jobs || !recheck_timer.is_on() || recheck_timer.get_timeout()>now+2000) //for no_jobs mode to not reschedule timer if its activation is close or in past
																					//to avoid repeated moving of job execution time in case of often no_jobs calls
			main_thread_item.schedule_atimer(recheck_timer, delay);
	}
}


void iot_modules_registry_t::start(const iot_devifaceclassdata_iface** devifaceclscfg, uint32_t num_devifaces,const iot_hwdevident_iface** devcontypescfg, uint32_t num_contypes) {
	assert(uv_thread_self()==main_thread);

	int err;
	err=register_devifaceclasses(devifaceclscfg, num_devifaces, 0);
	if(err) {
		outlog_error("No memory to register all built-in Device Iface Classes");
	}
	err=register_devcontypes(devcontypescfg, num_contypes, 0);
	if(err) {
		outlog_error("No memory to register all built-in Device Connection Types");
	}

	for(unsigned i=0;i<MODULES_DB_ITEMS;i++) {
		if(!modules_db[i].autoload)	continue;
		//module requires autoloading
		err=load_module(i, 0, NULL);
		if(err) {
			outlog_error("Error autoloading module with ID %u: %s", modules_db[i].module_id, kapi_strerror(err));
		}
	}
	//start detectors with autostart
	iot_module_item_t* it=detectors_head;
	while(it) {
		if(it->dbitem->autostart_detector && it->state[IOT_MODINSTTYPE_DETECTOR]==IOT_MODULESTATE_OK) create_detector_modinstance(it);
		it=it->next_detector;
	}
}

void iot_modules_registry_t::stop(void) {
	//TODO
}

int iot_modules_registry_t::register_devifaceclasses(const iot_devifaceclassdata_iface** iface, uint32_t num, uint32_t module_id) { //main thread
	//zero module_id is used for built-in device iface classes. It can be used to config class IDs from other modules
	assert(uv_thread_self()==main_thread);
	assert(iface!=NULL || num==0);
	while(num>0) {
		if(module_id>0 && IOT_DEVIFACECLASS_CUSTOM_MODULEID((*iface)->classid)!=module_id) {
			outlog_error("Module with ID %u tries to config Device Iface Class with wrong custom id %u", module_id, (*iface)->classid);
		} else {
			iot_devifaceclass_item_t* has=find_devifaceclass((*iface)->classid, false);
			if(has) { //duplicate definition
				if(has->module_id!=0) {
					outlog_error("Module with ID %u tries to give duplicate definition for Device Iface Class id %u", module_id, (*iface)->classid);
				}
			} else {
				has=(iot_devifaceclass_item_t*)malloc(sizeof(iot_devifaceclass_item_t));
				if(!has) return IOT_ERROR_NO_MEMORY;
				has->module_id=module_id;
				has->iface=(*iface);
				BILINKLIST_INSERTHEAD(has, devifacecls_head, next, prev);
			}
		}
		num--;
		iface++;
	}
	return 0;
}

int iot_modules_registry_t::register_devcontypes(const iot_hwdevident_iface** iface, uint32_t num, uint32_t module_id) { //main thread
	//zero module_id is used for built-in conn types. It can be used to config class IDs from other modules
	assert(uv_thread_self()==main_thread);
	assert(iface!=NULL || num==0);
	while(num>0) {
		if(module_id>0 && IOT_DEVCONTYPE_CUSTOM_MODULEID((*iface)->contype)!=module_id) {
			outlog_error("Module with ID %u tries to config Device Connection Type with wrong custom id %u", module_id, (*iface)->contype);
		} else {
			iot_devcontype_item_t* has=find_devcontype((*iface)->contype, false);
			if(has) { //duplicate definition
				if(has->module_id!=0) {
					outlog_error("Module with ID %u tries to give duplicate definition for Device Connection Type id %u", module_id, (*iface)->contype);
				}
			} else {
				has=(iot_devcontype_item_t*)malloc(sizeof(iot_devcontype_item_t));
				if(!has) return IOT_ERROR_NO_MEMORY;
				has->module_id=module_id;
				has->iface=(*iface);
				BILINKLIST_INSERTHEAD(has, devcontypes_head, next, prev);
			}
		}
		num--;
		iface++;
	}
	return 0;
}



int iot_modules_registry_t::load_module(int module_index, uint32_t module_id, iot_module_item_t**rval) { //tries to load module and register. module's module_init method is also called
//module_index is index of module's db item in modules_db array. It can be <0 to request search by provided module_id
//module_id is ignored if module_index>=0 provided. otherwise used to find modules_db index with module data
//rval if provided is filled with address of module item on success
	assert(uv_thread_self()==main_thread);

	static void* main_hmodule=NULL;
	char buf[256];
	if(module_index<0) {
		if((module_index=module_db_index_by_id(module_id))<0) return IOT_ERROR_NOT_FOUND;
	}

	if(modules_db[module_index].item) { //already loaded
		if(rval) *rval=modules_db[module_index].item;
		return 0;
	}

	if(!modules_db[module_index].bundle->hmodule) { //bundle is not loaded
		//try to load bundle
		if(modules_db[module_index].bundle->error) return IOT_ERROR_CRITICAL_ERROR; //bundle loading is impossible

		if(modules_db[module_index].bundle->linked) { //module is linked into executable
			if(!main_hmodule) main_hmodule=modules_db[module_index].bundle->hmodule=dlopen(NULL,RTLD_NOW | RTLD_LOCAL);
				else modules_db[module_index].bundle->hmodule=main_hmodule;
			strcpy(buf,"SELF");
		} else { //module is in external library
			snprintf(buf, sizeof(buf), "%smodules/%s%s", rootpath, modules_db[module_index].bundle->name, IOT_SOEXT);
			modules_db[module_index].bundle->hmodule=dlopen(buf, RTLD_NOW | RTLD_GLOBAL);
		}
		if(!modules_db[module_index].bundle->hmodule) {
			outlog_error("Error loading module bundle %s: %s", buf, dlerror());
			modules_db[module_index].bundle->error=true;
			return IOT_ERROR_CRITICAL_ERROR;
		}
	}
	//bundle is loaded
	//load symbol with module config
	int off=snprintf(buf, sizeof(buf), "%s","iot_modconf_");
	off+=bundlepath2symname(modules_db[module_index].bundle->name, buf+off, sizeof(buf)-off);
	if(off<int(sizeof(buf))) {
		off+=snprintf(buf+off, sizeof(buf)-off, "__%s",modules_db[module_index].module_name);
	}
	if(off>=int(sizeof(buf))) {
		outlog_error("Too small buffer to load module '%s' from bundle '%s'", modules_db[module_index].module_name, modules_db[module_index].bundle->name);
		return IOT_ERROR_CRITICAL_ERROR;
	}
	iot_moduleconfig_t* cfg=NULL;
	cfg=(iot_moduleconfig_t*)dlsym(modules_db[module_index].bundle->hmodule, buf);
	if(!cfg) {
		outlog_error("Error loading symbol '%s' for module '%s' from bundle '%s': %s", buf, modules_db[module_index].module_name, modules_db[module_index].bundle->name, dlerror());
		return IOT_ERROR_CRITICAL_ERROR;
	}
	int err=register_module(cfg, &modules_db[module_index]);
	if(!err) {
		if(rval) *rval=modules_db[module_index].item;
	}
	return err;
}

//creates module entry in the registry of loaded and inited modules
int iot_modules_registry_t::register_module(iot_moduleconfig_t* cfg, iot_modulesdb_item_t *dbitem) {
	assert(uv_thread_self()==main_thread);

	if(dbitem->module_id != cfg->module_id) {
		outlog_error("Module '%s::%s' ID %u from executable does not match value %u from DB", dbitem->bundle->name, dbitem->module_name, cfg->module_id, dbitem->module_id);
		return IOT_ERROR_CRITICAL_ERROR;
	}
	assert(dbitem->item==NULL);

	iot_module_item_t* item=new iot_module_item_t(cfg, dbitem);
	if(!item) return IOT_ERROR_NO_MEMORY;

	int err;
	//call init_module
	if(cfg->init_module) {
		err=cfg->init_module();
		if(err) {
			outlog_error("Error initializing module '%s::%s' with ID %u: %s", dbitem->bundle->name, dbitem->module_name, cfg->module_id, kapi_strerror(err));
			delete item;
			return IOT_ERROR_NOT_INITED;
		}
	}
	dbitem->item=item;
	BILINKLIST_INSERTHEAD(item, all_head, next, prev);

	//register provided custom device connection types
	if(cfg->devcontype_config && cfg->num_devcontypes>0) {
		register_devcontypes(cfg->devcontype_config, cfg->num_devcontypes, cfg->module_id);
	}

	if(cfg->iface_device_detector) {
		if(!cfg->iface_device_detector->init_instance || !cfg->iface_device_detector->deinit_instance) {
			outlog_error("Module '%s::%s' with ID %u has incomplete detector interface", dbitem->bundle->name, dbitem->module_name, cfg->module_id);
		} else {
			item->state[IOT_MODINSTTYPE_DETECTOR]=IOT_MODULESTATE_OK;
			BILINKLIST_INSERTHEAD(item, detectors_head, next_detector, prev_detector);
		}
	}
	if(cfg->iface_device_driver) {
		if(!cfg->iface_device_driver->init_instance || !cfg->iface_device_driver->deinit_instance) {
			outlog_error("Module '%s::%s' with ID %u has incomplete driver interface", dbitem->bundle->name, dbitem->module_name, cfg->module_id);
		} else {
			item->state[IOT_MODINSTTYPE_DRIVER]=IOT_MODULESTATE_OK;
			BILINKLIST_INSERTHEAD(item, drivers_head, next_driver, prev_driver);
		}
	}
	if(cfg->iface_event_source) {
		if(!cfg->iface_event_source->init_instance || !cfg->iface_event_source->deinit_instance) {
			outlog_error("Module '%s::%s' with ID %u has incomplete event source interface", dbitem->bundle->name, dbitem->module_name, cfg->module_id);
		} else {
			item->state[IOT_MODINSTTYPE_EVSOURCE]=IOT_MODULESTATE_OK;
			BILINKLIST_INSERTHEAD(item, evsrc_head, next_evsrc, prev_evsrc);
		}
	}
	//do post actions after filling all item props

	if(item->state[IOT_MODINSTTYPE_DRIVER]==IOT_MODULESTATE_OK) { //during initial module loading HW devices are not known yet, so this branch is only meaningful for later module loading
		hwdev_registry->try_find_hwdev_for_driver(item);
	}
	return 0;
}

iot_modinstance_item_t* iot_modules_registry_t::register_modinstance(iot_module_item_t* module, iot_modinstance_type_t type, iot_thread_item_t *thread, iot_module_instance_base* instance,
		iot_config_inst_item_t* cfgitem) {
	assert(uv_thread_self()==main_thread);
	//find free index
	for(iot_iid_t i=0;i<IOT_MAX_MODINSTANCES;i++) {
		last_iid=(last_iid+1)%IOT_MAX_MODINSTANCES;
		if(!last_iid || iot_modinstances[last_iid].miid.iid) continue;

		uint32_t curtime=get_time32(time(NULL));
		if(iot_modinstances[last_iid].miid.created>=curtime) curtime=iot_modinstances[last_iid].miid.created+1; //guarantee different creation time for same iid

//		bool waslock=iot_modinstances[last_iid].acclock.test_and_set(std::memory_order_acquire);
//		assert(waslock==false); //this is the only place to change iid from zero to nonzero, so once iot_modinstances[last_iid].miid.iid is false, it must not become true somewhere else

		if(!iot_modinstances[last_iid].init(iot_miid_t(curtime, last_iid), module, type, thread, instance, cfgitem)) {
//			iot_modinstances[last_iid].acclock.clear(std::memory_order_release);
			return NULL;
		}

//		iot_modinstances[last_iid].acclock.clear(std::memory_order_release);
		return &iot_modinstances[last_iid];
	}
	return NULL;
}

bool iot_modinstance_item_t::init(const iot_miid_t &miid_, iot_module_item_t* module_, iot_modinstance_type_t type_, iot_thread_item_t *thread_, iot_module_instance_base* instance_,
		iot_config_inst_item_t* cfgitem_) {
	memset(this, 0, sizeof(*this));
	miid=miid_;
	state=IOT_MODINSTSTATE_INITED;

	for(unsigned i=0;i<sizeof(msgp)/sizeof(msg_structs[0]);i++) {
		msg_structs[i]=main_allocator.allocate_threadmsg();
		if(!msg_structs[i]) {
			deinit();
			return false;
		}
	}

	instrecheck_timer.init(this, [](void* param)->void {((iot_modinstance_item_t*)param)->recheck_job(false);});

//	refcount=0;
//	pendfree=0;
//	in_recheck_job=false;

//	memset(&data,0, sizeof(data));
	module=module_;
	type=type_;
	instance=instance_;
//	thread=NULL;

//	started=0;
//	state_timeout=0;
//	aborted_error=0;

	target_state=IOT_MODINSTSTATE_STARTED;

	switch(type) {
		case IOT_MODINSTTYPE_DETECTOR: {
			cfgitem=NULL;
			auto iface=module->config->iface_device_detector;
			assert(iface!=NULL);
			assert(module->detector_instance==NULL);
			BILINKLIST_INSERTHEAD(this, module->detector_instance, next_inmod, prev_inmod);
			cpu_loading=iface->cpu_loading;
			break;
		}
		case IOT_MODINSTTYPE_DRIVER: {
			cfgitem=NULL;
			auto iface=module->config->iface_device_driver;
			assert(iface!=NULL);
			BILINKLIST_INSERTHEAD(this, module->driver_instances_head, next_inmod, prev_inmod);
			cpu_loading=iface->cpu_loading;
			break;
		}
		case IOT_MODINSTTYPE_EVSOURCE: {
			cfgitem=cfgitem_;
			cfgitem->miid=miid;
			auto iface=module->config->iface_event_source;
			assert(iface!=NULL);
			BILINKLIST_INSERTHEAD(this, module->evsrc_instances_head, next_inmod, prev_inmod);
			cpu_loading=iface->cpu_loading;
			for(int i=0;i<iface->num_devices;i++) data.evsrc.dev[i].actual=1;
			break;
		}
	}
	thread_registry->add_modinstance(this, thread_); //assigns thread
	return true;
}

void iot_modinstance_item_t::deinit(void) {
	assert(uv_thread_self()==main_thread);
	assert(miid && state==IOT_MODINSTSTATE_INITED && !instance && refcount==0);

	if(module) {
		BILINKLIST_REMOVE(this, next_inmod, prev_inmod); //remove instance from typed module's list
		module=NULL;
	}
	if(thread) {
		thread_registry->remove_modinstance(this); //remove instance from thread's list
	}

	instrecheck_timer.unschedule();

	for(unsigned i=0;i<sizeof(msgp)/sizeof(msg_structs[0]);i++) {
		if(msg_structs[i]) {
			iot_release_msg(msg_structs[i]);
			msg_structs[i]=NULL;
		}
	}

	miid.iid=0; //mark record of iot_modinstances array as free
	//modinst->miid.created MUST BE PRESERVED to guarantee uniqueness of created-miid pair
}

//find running module instance by miid. for use by kernel code in main thread only!!! does not lock modinstance structure
iot_modinstance_item_t* iot_modules_registry_t::find_modinstance_byid(const iot_miid_t &miid) {
	assert(uv_thread_self()==main_thread);
	if(!miid.iid || miid.iid>=IOT_MAX_MODINSTANCES) return NULL; //incorrect miid

	iot_modinstance_item_t* it=&iot_modinstances[miid.iid];
	if(it->miid != miid) return NULL; //no running instance with provided miid
	return it;
}

//find running module instance by miid struct AND LOCK modinstance structure
//returns false object if instance not found or pending release
iot_modinstance_locker iot_modules_registry_t::get_modinstance(const iot_miid_t &miid) { //can run in any thread
	if(!miid || miid.iid>=IOT_MAX_MODINSTANCES) return iot_modinstance_locker(); //incorrect miid

	iot_modinstance_item_t* it=&iot_modinstances[miid.iid];

	//access to miid field must be protected
	if(!it->lock()) return iot_modinstance_locker(); //pending release
	if(it->get_miid() != miid) {
		//no running instance with provided miid
		it->unlock();
		return iot_modinstance_locker();
	}
	return iot_modinstance_locker(it); //lock will be freed when returned object gets destroyed
}

//frees iid and removes modinstance from module's list of instances
void iot_modules_registry_t::free_modinstance(iot_modinstance_item_t* modinst) {
	assert(uv_thread_self()==main_thread);
	assert(modinst!=NULL && modinst->miid);
	assert(modinst->state==IOT_MODINSTSTATE_INITED || modinst->state==IOT_MODINSTSTATE_HUNG);
	int err;

	iot_module_item_t *module=modinst->module;

	//type dependent actions to clear unnecessary references to modinstance (both locked and unlocked)
	switch(modinst->type) {
		case IOT_MODINSTTYPE_DETECTOR:
			break;
		case IOT_MODINSTTYPE_DRIVER: {
			//clean retry timeouts data
			modinst->data.driver.retry_clients.remove_all();

			iot_hwdevregistry_item_t* devitem=modinst->data.driver.hwdev;

			//disconnect from hwdevice
			if(modinst->state==IOT_MODINSTSTATE_INITED) { //stopped successfully or not ever started
				if(devitem) {
					devitem->devdrv_modinstlk.unlock();
					modinst->data.driver.hwdev=NULL;
					if(devitem->is_removed) hwdev_registry->finish_hwdev_removal(devitem);
						else modules_registry->try_find_driver_for_hwdev(devitem);
				}
			} else { //hung
				if(devitem->is_removed) { //allow to clean removed devices from registry even with hung driver
					devitem->devdrv_modinstlk.unlock();
					modinst->data.driver.hwdev=NULL;
					hwdev_registry->finish_hwdev_removal(devitem);
				}
			}
			break;
		}
		case IOT_MODINSTTYPE_EVSOURCE: {
			//clean retry timeouts data
			for(uint8_t i=0;i<IOT_CONFIG_MAX_EVENTSOURCE_DEVICES && modinst->data.evsrc.dev[i].actual;i--)
				modinst->data.evsrc.dev[i].retry_drivers.remove_all();

			break;
		}
	}

	//type-common actions
	if(modinst->state!=IOT_MODINSTSTATE_INITED) { //hung
		//leave such instance as is, but schedule daemon restart

		thread_registry->remove_modinstance(modinst); //will move to list of hang instances of thread
		modinst->instrecheck_timer.unschedule(); //??

		iot_process_module_bug(module);
		return;
	}

	//only non-hung instances here !!!!!!!!!!

	if(modinst->cfgitem) {
		modinst->cfgitem->miid.clear();
		modinst->cfgitem=NULL;
	}


	if(!modinst->mark_pendfree()) {
		//cannot be freed right now. last lock holder will notify when structure is unlocked
		return;
	}

	//from here get_modinstance returns NULL

	//deinit module instance data
	if(modinst->instance) {
		switch(modinst->type) {
			case IOT_MODINSTTYPE_DETECTOR: {
				auto iface=module->config->iface_device_detector;
				err=iface->deinit_instance(modinst->instance);
				break;
			}
			case IOT_MODINSTTYPE_DRIVER: {
				auto iface=module->config->iface_device_driver;
				err=iface->deinit_instance(modinst->instance);
				break;
			}
			case IOT_MODINSTTYPE_EVSOURCE: {
				auto iface=module->config->iface_event_source;
				err=iface->deinit_instance(modinst->instance);
				break;
			}
			default: err=0;
		}

		if(err) { //any error from deinit is critical bug
			outlog_error("%s instance DEinit for module '%s::%s' with ID %u returned error: %s. Module is blocked.",iot_modinsttype_name[modinst->type], module->dbitem->bundle->name, module->dbitem->module_name, module->config->module_id, kapi_strerror(err));
			module->state[modinst->type]=IOT_MODULESTATE_DISABLED;
			iot_process_module_bug(module);
		}
		modinst->instance=NULL;
	}

	modinst->deinit();


}



//try to create driver instance for provided real device
//Returns:
//	0 - success, driver instance created and started
//	IOT_ERROR_NOT_READY - temporary success. instance start is async, result not available right now
//	IOT_ERROR_DEVICE_NOT_SUPPORTED - success result, but module cannot work with provided device
//	IOT_ERROR_TEMPORARY_ERROR - temporary kernel code error. module can be retried for this device later
//	IOT_ERROR_MODULE_BLOCKED - error. module was blocked on temporary or constant basis
//	IOT_ERROR_CRITICAL_ERROR - instanciation is invalid
int iot_modules_registry_t::create_driver_modinstance(iot_module_item_t* module, iot_hwdevregistry_item_t* devitem) {
	assert(uv_thread_self()==main_thread);

	iot_device_driver_base *inst=NULL;
	iot_modinstance_item_t *modinst=NULL;
	iot_modinstance_type_t type=IOT_MODINSTTYPE_DRIVER;
	int err;
	auto iface=module->config->iface_device_driver;
	//from here all errors go to onerr

	iot_thread_item_t* thread=thread_registry->assign_thread(iface->cpu_loading);
	assert(thread!=NULL);

	iot_devifaces_list deviface_list;

	err=iface->init_instance(&inst, thread->thread, &devitem->devdata, &deviface_list);
	if(err) {
		if(err==IOT_ERROR_DEVICE_NOT_SUPPORTED) goto onerr;
		outlog_error("Driver instance init for module '%s::%s' with ID %u returned error: %s",module->dbitem->bundle->name, module->dbitem->module_name, module->config->module_id, kapi_strerror(err));
		if(err==IOT_ERROR_INVALID_DEVICE_DATA) { //such errors should be logged, but irrelevant for caller of this func
			err=IOT_ERROR_DEVICE_NOT_SUPPORTED;
			goto onerr;
		}

		if(err!=IOT_ERROR_TEMPORARY_ERROR || module->errors[type]>10) {
			module->state[type]=IOT_MODULESTATE_DISABLED;
		} else {
			module->state[type]=IOT_MODULESTATE_BLOCKED;
			module->errors[type]++;
			module->timeout[type]=uv_now(main_loop)+module->errors[type]*5*60*1000;
			module->recheck_job(true);
		}
		err=IOT_ERROR_MODULE_BLOCKED;
		goto onerr;
	}
	if(!inst) {
		//no error and no instance. this is a bug
		outlog_error("Driver instance init for module '%s::%s' with ID %u returned NULL pointer. This is a bug.",module->dbitem->bundle->name, module->dbitem->module_name, module->config->module_id);
		module->state[type]=IOT_MODULESTATE_DISABLED;
		err=IOT_ERROR_MODULE_BLOCKED;
		goto onerr;
	}

	modinst=register_modinstance(module, type, thread, inst, NULL);
	if(!modinst) {
		err=IOT_ERROR_TEMPORARY_ERROR; //here registering failed which means memory error (or not enough iid space. TODO)
		goto onerr;
	}
	inst=NULL; //already saved in modinstance structure, so will be deinit when freeing it

	//check returned device iface ids
	int num_devclasses;
	int i;
	for(num_devclasses=0,i=0;i<deviface_list.num;i++) {
		const iot_devifaceclassdata_iface* iface=deviface_list.items[i].find_iface();
		if(iface) {
			memmove(&modinst->data.driver.devclasses[num_devclasses].data, &deviface_list.items[i], sizeof(deviface_list.items[i]));
			modinst->data.driver.devclasses[num_devclasses].iface=iface;
			num_devclasses++;
		}
	}
	if(!num_devclasses) {
		outlog_error("Driver instance init for module %u returned no suitabe device interface. This is configurational or module bug.", module->config->module_id);
		err=IOT_ERROR_DEVICE_NOT_SUPPORTED;
		goto onerr;
	}

	//finish driver instanciation
	modinst->data.driver.hwdev=devitem;
	devitem->devdrv_modinstlk.lock(modinst);

	return modinst->start(false);

onerr:
	if(modinst) free_modinstance(modinst);
	else {
		if(inst) {
			if((err=iface->deinit_instance(inst))) { //any error from deinit is critical bug
				outlog_error("%s instance DEinit for module '%s::%s' with ID %u returned error: %s. Module is blocked.",iot_modinsttype_name[type], module->dbitem->bundle->name, module->dbitem->module_name, module->config->module_id, kapi_strerror(err));
				module->state[type]=IOT_MODULESTATE_DISABLED;
				iot_process_module_bug(module);
			}
		}
	}
	if(err!=IOT_MODULESTATE_BLOCKED && module->state[type]==IOT_MODULESTATE_DISABLED) err=IOT_MODULESTATE_BLOCKED; //module could become blocked after 'onerr' point
	return err;
}

//try to create evsrc instance for specific config item
//Returns:
//  0 - success, driver instance created and started
//  IOT_ERROR_NOT_READY - temporary success. instance start is async, result not available right now
//  IOT_ERROR_TEMPORARY_ERROR - error. module can be retried for this device later
//  IOT_ERROR_MODULE_BLOCKED - error. module was blocked on temporary or constant basis
//  IOT_ERROR_CRITICAL_ERROR - instanciation is invalid (error in saved config of config item?)
int iot_modules_registry_t::create_evsrc_modinstance(iot_module_item_t* module, iot_config_inst_item_t* item) {
	assert(uv_thread_self()==main_thread);

	iot_event_source_base *inst=NULL;
	iot_modinstance_item_t *modinst=NULL;
	iot_modinstance_type_t type=IOT_MODINSTTYPE_EVSOURCE;
	int err;
	auto iface=module->config->iface_event_source;
	//from here all errors go to onerr
	iot_thread_item_t* thread=thread_registry->assign_thread(iface->cpu_loading);
	assert(thread!=NULL);

	err=iface->init_instance(&inst, thread->thread, item->iot_id, item->json_cfg);
	if(err) {
		outlog_error("Event source instance init for module '%s::%s' with ID %u returned error: %s",module->dbitem->bundle->name, module->dbitem->module_name, module->config->module_id, kapi_strerror(err));
		if(err!=IOT_ERROR_TEMPORARY_ERROR || module->errors[type]>10) {
			module->state[type]=IOT_MODULESTATE_DISABLED;
		} else {
			module->state[type]=IOT_MODULESTATE_BLOCKED;
			module->errors[type]++;
			module->timeout[type]=uv_now(main_loop)+module->errors[type]*5*60*1000;
			module->recheck_job(true);
		}
		err=IOT_ERROR_MODULE_BLOCKED;
		goto onerr;
	}
	if(!inst) {
		//no error and no instance. this is a bug
		outlog_error("Event source instance init for module '%s::%s' with ID %u returned NULL pointer. This is a bug.",module->dbitem->bundle->name, module->dbitem->module_name, module->config->module_id);
		module->state[type]=IOT_MODULESTATE_DISABLED;
		err=IOT_ERROR_MODULE_BLOCKED;
		goto onerr;
	}

	modinst=register_modinstance(module, type, thread, inst, item);
	if(!modinst) {
		err=IOT_ERROR_TEMPORARY_ERROR; //here registering failed which means memory error (or not enough iid space. TODO)
		goto onerr;
	}
	inst=NULL; //already saved in modinstance structure, so will be deinit when freeing it

	return modinst->start(false);

onerr:
	if(modinst) free_modinstance(modinst);
	else {
		if(inst) {
			if((err=iface->deinit_instance(inst))) { //any error from deinit is critical bug
				outlog_error("%s instance DEinit for module '%s::%s' with ID %u returned error: %s. Module is blocked.",iot_modinsttype_name[type], module->dbitem->bundle->name, module->dbitem->module_name, module->config->module_id, kapi_strerror(err));
				module->state[type]=IOT_MODULESTATE_DISABLED;
				iot_process_module_bug(module);
			}
		}
	}
	if(err!=IOT_MODULESTATE_BLOCKED && module->state[type]==IOT_MODULESTATE_DISABLED) err=IOT_MODULESTATE_BLOCKED; //module could become blocked after 'onerr' point
	return err;
}

//tries to setup connection of LOCAL driver to appropriate LOCAL consumer
//returns:
//	0 on successful connection creation
//	IOT_ERROR_NOT_READY - connection creation is in progress
//	IOT_ERROR_TEMPORARY_ERROR - attempt can be repeated later (lack of resources)
//	IOT_ERROR_NOT_FOUND - no attempt should be scheduled, i.e. do nothing
int iot_modules_registry_t::try_connect_driver_to_consumer(iot_modinstance_item_t *drvinst) {
	assert(uv_thread_self()==main_thread);
	if(need_exit) return IOT_ERROR_NOT_FOUND;

	if(!drvinst->is_working_not_stopping()) return IOT_ERROR_NOT_FOUND; //driver instance must be started

	int err;
	bool wastemperr=false;
	iot_driverclient_conndata_t* conndata;
	iot_module_item_t* mod, *nextmod=modules_registry->all_head;
	while(nextmod) {
		mod=nextmod;
		nextmod=nextmod->next;

		iot_modinstance_item_t *modinst=mod->evsrc_instances_head; //loop through ev sources
		while(modinst) {
			auto iface=mod->config->iface_event_source;
			uint8_t num_devs=iface->num_devices;
			for(uint8_t dev_idx=0;dev_idx<num_devs;dev_idx++) {
				conndata=&modinst->data.evsrc.dev[dev_idx];
				iot_device_connection_t* conn=conndata->conn;
				if(conndata->block || (conn && conn->state!=conn->IOT_DEVCONN_INIT)) continue; //connection blocked, is set or in process of being set

				if(!conn) {
					conn=iot_create_connection(modinst, dev_idx); //updates conndata->conn on success
					if(!conn) { //no connection slots?
						outlog_error("Cannot allocate connection for device line %d of %s iot_id=%u", int(dev_idx)+1, iot_modinsttype_name[modinst->type], modinst->cfgitem->iot_id);
						wastemperr=true;
						continue;
					}
				}

				if((!conn->client_hwdevident && !conn->client_devifaceclassfilter->flag_canauto) || //no hwdevice bound and auto detection forbidden
					(conn->client_hwdevident && conn->client_devifaceclassfilter->flag_localonly && conn->client_hwdevident->hostid!=IOT_HOSTID_ANY &&
																						conn->client_hwdevident->hostid != iot_current_hostid)) { //another host specified but not allowed
					conndata->block=1;
					continue;
				}

				if(!conn->client_hwdevident || conn->client_hwdevident->hostid==IOT_HOSTID_ANY || conn->client_hwdevident->hostid == iot_current_hostid) { //any device host allowed or current host matches
					//can try to connect to current local driver
					err=conn->connect_local(drvinst);
					if(err==IOT_ERROR_TEMPORARY_ERROR) {
						wastemperr=true;
						continue;
					}
					if(err!=IOT_ERROR_DEVICE_NOT_SUPPORTED) return err;	// !err || err==IOT_ERROR_NOT_READY //////////////////|| err==IOT_ERROR_CRITICAL_ERROR 
				}
			}
			modinst=modinst->next_inmod;
		}

		//TODO iot_modinstance_item_t *modinst=mod->executor_instances_head; //loop through executors
	}
	return wastemperr ? IOT_ERROR_TEMPORARY_ERROR : IOT_ERROR_NOT_FOUND;
}

//tries to setup connection of driver client (event source or executor) to one of its driver
//returns:
//	0 on successful connection creation
//	IOT_ERROR_NOT_READY - connection creation is in progress
//	IOT_ERROR_TEMPORARY_ERROR - attempt can be repeated later (lack of resources)
//	IOT_ERROR_NOT_FOUND - no attempt will be repeated until new driver appeares, i.e. do nothing (just wait for new driver to start)
int iot_modules_registry_t::try_connect_consumer_to_driver(iot_modinstance_item_t *modinst, uint8_t idx) {
	assert(uv_thread_self()==main_thread);
	if(need_exit) return IOT_ERROR_NOT_FOUND;

	iot_driverclient_conndata_t* conndata;

	switch(modinst->type) {
		case IOT_MODINSTTYPE_EVSOURCE: {
			assert(idx<IOT_CONFIG_MAX_EVENTSOURCE_DEVICES);
			conndata=&modinst->data.evsrc.dev[idx];
			break;
		}
		case IOT_MODINSTTYPE_DRIVER:
		case IOT_MODINSTTYPE_DETECTOR:
			//list forbidden values
			assert(false);
			return IOT_ERROR_NOT_FOUND;
	}
	uint32_t now32=uint32_t((uv_now(main_loop)+500)/1000);
	if(!conndata->actual || conndata->block) return IOT_ERROR_NOT_FOUND; //this device connection is blocked. flag should be reset when configured device is changed by customer
	if(conndata->retry_drivers_timeout>now32) return IOT_ERROR_TEMPORARY_ERROR;

	iot_device_connection_t* conn=conndata->conn;
	if(!conn) {
		conn=iot_create_connection(modinst, idx); //updates conndata->conn on success
		if(!conn) { //no connection slots?
			outlog_error("Cannot allocate connection for device line %d of %s iot_id=%u", int(idx)+1, iot_modinsttype_name[modinst->type], modinst->cfgitem->iot_id);

			if(conndata->retry_drivers_timeout<now32+30) {
				conndata->retry_drivers_timeout=now32+30;
				modinst->recheck_job(true);
			} //else recheck already set on later time, so do nothing here

			return IOT_ERROR_TEMPORARY_ERROR;
		}
	} else {
		if(conn->state!=conn->IOT_DEVCONN_INIT) return IOT_ERROR_NOT_READY;
	}

	if((!conn->client_hwdevident && !conn->client_devifaceclassfilter->flag_canauto) || //no hwdevice bound and auto detection forbidden
		(conn->client_hwdevident && conn->client_devifaceclassfilter->flag_localonly && conn->client_hwdevident->hostid!=IOT_HOSTID_ANY &&
																			conn->client_hwdevident->hostid != iot_current_hostid)) { //another host specified but not allowed
		conndata->block=1;
		return IOT_ERROR_NOT_FOUND;
	}

	int err=IOT_ERROR_NOT_FOUND;
	bool wastemperr=false;

	if(!conn->client_hwdevident || conn->client_hwdevident->hostid==IOT_HOSTID_ANY || conn->client_hwdevident->hostid == iot_current_hostid) { //any device host allowed or current host matches
		//try local devices
		err=hwdev_registry->try_connect_local_driver(conn);
		if(!err || err==IOT_ERROR_NOT_READY) return err; //success
		if(err==IOT_ERROR_NO_MEMORY) return IOT_ERROR_TEMPORARY_ERROR; //no sense to continue, system lacks memory
		if(err==IOT_ERROR_TEMPORARY_ERROR) {
			wastemperr=true;
			err=IOT_ERROR_NOT_FOUND;
			//continue with local drivers
		} else {
			assert(err==IOT_ERROR_NOT_FOUND);
		}
	}

	if(!conn->client_hwdevident || conn->client_hwdevident->hostid != iot_current_hostid) { //any device host allowed or current host does not matches (or any host allowed, IOT_HOSTID_ANY!=iot_current_hostid)
		//try remote devices
//		err=peers_registry->try_connect_remote_driver(conn);
/*
			iot_peer_link_t *plink=peers_registry->find_peer_link(devident->hostid);
			if(!plink) {//no link established.
//				if(uv_now(main_loop)-iot_starttime_ms>10000) break; //10 secs passed after startup, treat host as dead TODO?
				return IOT_ERROR_NO_PEER;  //must wait for some time before attempting to find another device TODO?
			}
			if(plink->is_failed()) break; //peer now is treated as dead
			if(!plink->is_data_trusted()) return IOT_ERROR_TEMPORARY_ERROR; //cannot be trusted. retry later TODO?
			iot_remote_driverinst_item_t* drvitem=plink->get_avail_drivers_list();
			while(drvitem) { //TODO move this search to plink
				if(drvitem->ident_iface->matches(&drvitem->devident, &devident->dev)) {
					err=conn->connect_remote(drvitem->miid, devcfg->classids, devcfg->num_classids);
					if(!err) return 0;
					//TODO react to errors
					return err;
				}
				drvitem=drvitem->next;
			}
*/
		err=IOT_ERROR_NOT_FOUND;

		if(!err || err==IOT_ERROR_NOT_READY) return err; //success
		if(err==IOT_ERROR_NO_MEMORY) return IOT_ERROR_TEMPORARY_ERROR; //no sense to continue, system lacks memory
		if(err==IOT_ERROR_TEMPORARY_ERROR) {
			wastemperr=true;
			err=IOT_ERROR_NOT_FOUND;
		} else {
			assert(err==IOT_ERROR_NOT_FOUND);
		}
	}
	return wastemperr ? IOT_ERROR_TEMPORARY_ERROR : IOT_ERROR_NOT_FOUND;
}

//after detecting new hw device tries to find appropriate driver
void iot_modules_registry_t::try_find_driver_for_hwdev(iot_hwdevregistry_item_t* devitem) {
	assert(uv_thread_self()==main_thread);
	assert(!devitem->devdrv_modinstlk);
	char namebuf[256];

	if(devitem->is_blocked || need_exit) return; //device is blocked

	uint64_t now=uv_now(main_loop);
	uint32_t now32=uint32_t((now+500)/1000);

	bool have_blocked=false;
	iot_module_item_t* it, *itnext=drivers_head;
	while((it=itnext)) {
		itnext=itnext->next_driver;

		//skip blocked driver
		if(it->state[IOT_MODINSTTYPE_DRIVER]==IOT_MODULESTATE_BLOCKED) {
			if(it->timeout[IOT_MODINSTTYPE_DRIVER]>now) {
				have_blocked=true;
				continue;
			}
			it->state[IOT_MODINSTTYPE_DRIVER]=IOT_MODULESTATE_OK;
		}
		else if(it->state[IOT_MODINSTTYPE_DRIVER]!=IOT_MODULESTATE_OK) continue;

		//check if module is not blocked for this specific hw device
		if(devitem->is_module_blocked(it->config->module_id, now32)) {
			have_blocked=true;
			continue;
		}

		int err=it->try_driver_create(devitem);
		if(!err) return; //success
		if(err==IOT_ERROR_TEMPORARY_ERROR || err==IOT_ERROR_NOT_READY || err==IOT_ERROR_CRITICAL_ERROR) { //for async starts block module too. it will be unblocked after getting success confirmation
																		//and nothing must be done in case of error
			if(!devitem->block_module(it->config->module_id, err==IOT_ERROR_CRITICAL_ERROR ? 0xFFFFFFFFu : now32+2*60*1000, now32)) { //delay retries of this module for current hw device
				outlog_error("HWDev used all module-bloking slots and was blocked: %s", devitem->devdata.ident_iface->sprint(&devitem->devdata.dev_ident.dev, namebuf, sizeof(namebuf)));
				devitem->is_blocked=1;
				return;
			}
			have_blocked=true;
			if(err==IOT_ERROR_NOT_READY) return; //potential success
			//continue to next driver
		}
		//else IOT_ERROR_DEVICE_NOT_SUPPORTED || IOT_ERROR_MODULE_BLOCKED so just continue search
	}
	if(have_blocked) hwdev_registry->have_unconnected_devs=true;
	outlog_info("no driver for new HWDev found: %s", devitem->devdata.ident_iface->sprint(&devitem->devdata.dev_ident.dev, namebuf, sizeof(namebuf)));
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////                                     ///////////////////////////////////////////////////////
///////////////////////////////////        iot_modinstance_item_t       ///////////////////////////////////////////////////////
///////////////////////////////////                                     ///////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


//possible return status:
//	0 - success, modinstance has been started or was already started
//	IOT_ERROR_NOT_READY - async request to start successfully sent  (only without isasync)
//	IOT_ERROR_TEMPORARY_ERROR - start operation failed by internal gateway fault
//	IOT_ERROR_MODULE_BLOCKED - module in blocked or disabled state
//	IOT_ERROR_CRITICAL_ERROR - instanciation is invalid
int iot_modinstance_item_t::start(bool isasync) { //called in working thread of instance or main thread. tries to start module instance and returns status
	//isasync should be true only when processing IOT_MSG_START_MODINSTANCE command

	int err;
	uint64_t now;
	if(!isasync) { //initial call from main thread
		assert(uv_thread_self()==main_thread);

		if(uv_thread_self()!=thread->thread) { //not working thread of instance, so must be main thread. async start
			iot_threadmsg_t* msg=msgp.start;
			if(!msg) {
				assert(false);
				return IOT_ERROR_NOT_READY; //msg already sent? impossible
			}
			err=iot_prepare_msg(msg, IOT_MSG_START_MODINSTANCE, this, 0, NULL, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
			if(err) {
				assert(false);
				err=IOT_ERROR_TEMPORARY_ERROR;
				goto onerr;
			}
			msgp.start=NULL;
			//send signal to start
			thread->send_msg(msg);
			return IOT_ERROR_NOT_READY; //successful status for case with different threads
		}
	} else {
		assert(uv_thread_self()==thread->thread);
	}
	//this is working thread of modinstance

	assert(state==IOT_MODINSTSTATE_INITED);

	if(module->state[type]!=IOT_MODULESTATE_OK) {
		err=IOT_ERROR_MODULE_BLOCKED;
		goto onerr;
	}
	if(state!=IOT_MODINSTSTATE_INITED || target_state!=IOT_MODINSTSTATE_STARTED || need_exit) { //start request was overriden
		err=IOT_ERROR_ACTION_CANCELLED; //this error is internal for this func and on_start_status() and is returned as IOT_ERROR_TEMPORARY_ERROR (to distinguish from same result of start() func)
		goto onerr;
	}

	now=uv_now(thread->loop);
	err=instance->start(miid);
	if(err) {
		outlog_error("Error starting %s instance of module '%s::%s' with ID %u (%d error(s) so far): %s", iot_modinsttype_name[type], module->dbitem->bundle->name, module->dbitem->module_name, module->config->module_id, int(module->errors[type]+1), kapi_strerror(err));
		if(err!=IOT_ERROR_TEMPORARY_ERROR && err!=IOT_ERROR_CRITICAL_ERROR) err=IOT_ERROR_CRITICAL_BUG;
		goto onerr;
	}
	//just started
	started=now;
	state=IOT_MODINSTSTATE_STARTED;
	outlog_debug("%s instance of module '%s::%s' ID %u started with ID %u",iot_modinsttype_name[type], module->dbitem->bundle->name, module->dbitem->module_name, module->dbitem->module_id, unsigned(miid.iid));
	err=0;

onerr:
	if(isasync) {
		iot_threadmsg_t* msg=msgp.start;
		assert(msg!=NULL);
		err=iot_prepare_msg(msg, IOT_MSG_MODINSTANCE_STARTSTATUS, this, 0, NULL, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
		if(err) {
			assert(false);
		} else {
			msgp.start=NULL;
			msg->intarg=err;
			main_thread_item.send_msg(msg);
		}
		return IOT_ERROR_NOT_READY;
	}
	return on_start_status(err, false);
}



//possible return status:
//	0 - success, modinstance has been started or was already started
//	IOT_ERROR_TEMPORARY_ERROR - start operation failed by internal gateway fault
//	IOT_ERROR_MODULE_BLOCKED - module in blocked or disabled state
//	IOT_ERROR_CRITICAL_ERROR - instanciation is invalid

int iot_modinstance_item_t::on_start_status(int err, bool isasync) { //processes result of modinstance start in main thread
	assert(uv_thread_self()==main_thread);
	if(!err) {
		module->errors[type]=0;
		switch(type) {
			case IOT_MODINSTTYPE_DETECTOR:
				break;
			case IOT_MODINSTTYPE_DRIVER:
				if(isasync) { //during async start module is blocked in specific hw device after getting NOT_READY
					data.driver.hwdev->clear_module_block(module->config->module_id); //reset block period for this module in hwdev's data
				}
				modules_registry->try_connect_driver_to_consumer(this);
				break;
			case IOT_MODINSTTYPE_EVSOURCE: {
				for(uint8_t i=0;i<IOT_CONFIG_MAX_EVENTSOURCE_DEVICES && data.evsrc.dev[i].actual;i--) modules_registry->try_connect_consumer_to_driver(this, i);
				break;
			}
		}
		return 0;
	}
	//normalize error code
	if(err==IOT_ERROR_ACTION_CANCELLED) {
		err=IOT_ERROR_TEMPORARY_ERROR; //such error will be reported
	}
	else if(err!=IOT_ERROR_CRITICAL_ERROR) { //this error is preserved as is
		if(err==IOT_ERROR_CRITICAL_BUG || module->errors[type]>10) {
			module->state[type]=IOT_MODULESTATE_DISABLED;
		} else {
			module->state[type]=IOT_MODULESTATE_BLOCKED;
			module->errors[type]++;
			module->timeout[type]=uv_now(main_loop)+module->errors[type]*5*60*1000;
			module->recheck_job(true);
		}
		err=IOT_ERROR_MODULE_BLOCKED; //such error will be reported
	}

	//do post processing of normalized errors
	switch(type) {
		case IOT_MODINSTTYPE_DETECTOR:
			break;
		case IOT_MODINSTTYPE_DRIVER:
			if(isasync) { //during async start module is blocked in specific hw device after getting NOT_READY
				iot_hwdevregistry_item_t *devitem=data.driver.hwdev;
				assert(devitem!=NULL);

				if(err==IOT_ERROR_CRITICAL_ERROR) { //block module permanently for current hw device
					uint32_t now32=uint32_t((uv_now(main_loop)+500)/1000);
					devitem->block_module(module->config->module_id, 0xFFFFFFFFu, now32);
				} //else IOT_ERROR_TEMPORARY_ERROR, IOT_ERROR_MODULE_BLOCKED - just resume search of driver, current module already blocked for its hw device
			}
			break;
		case IOT_MODINSTTYPE_EVSOURCE:
			//TODO
			if(isasync) {
				if(err==IOT_ERROR_CRITICAL_ERROR) { //block iot_id instanciation permanently until config update
				}
			}
			break;
	}

	modules_registry->free_modinstance(this);

	return err;
}


void iot_modinstance_item_t::recheck_job(bool no_jobs) {
	//no_jobs prevents jobs from running, just reschedules timers
	//called in instance thread
	assert(uv_thread_self()==thread->thread);
	if(in_recheck_job) return;
	in_recheck_job=true;
	uint64_t now=uv_now(thread->loop);
	uint64_t delay=0xFFFFFFFFFFFFFFFFul;

	//find most early event
	//check state-related timeouts
	if(state==IOT_MODINSTSTATE_STOPPING) {
		if(state_timeout<=now) {
			if(!no_jobs) stop(true);
			else delay=0;
		} else {
			if(state_timeout-now<delay) delay=state_timeout-now;
		}
	}

	in_recheck_job=false;
	if(delay<0xFFFFFFFFFFFFFFFFul) {
		if(!no_jobs || !instrecheck_timer.is_on() || instrecheck_timer.get_timeout()>now+2000) //for no_jobs mode to not reschedule timer if its activation is close or in past
																					//to avoid repeated moving of job execution time in case of often no_jobs calls
			thread->schedule_atimer(instrecheck_timer, delay); //zero delay always means at least 1 sec now!!!
	}
}



//possible return status:
//0 - success, modinstance has been started or was already started
//	IOT_ERROR_NOT_READY - async request to stop successfully sent or stop is delayed by module code
//	IOT_ERROR_TEMPORARY_ERROR - stop operation failed by internal gateway fault
//	IOT_ERROR_CRITICAL_ERROR - module instance is hung or instance is invalid

int iot_modinstance_item_t::stop(bool isasync, bool forcemsg) { //called in working thread of instance or main thread. tries to stop module instance and returns status
	//isasync should be true only when processing IOT_MSG_START_MODINSTANCE command or call is made from modinst itself or by timer
	//forcemsg can be used to force async action even from main thread (isasync must be false)
	int err;
	if(!isasync) { //initial call from main thread
		assert(uv_thread_self()==main_thread);
		target_state=IOT_MODINSTSTATE_INITED;

		if(uv_thread_self()!=thread->thread || forcemsg) { //not working thread of instance, so must be async start
			iot_threadmsg_t* msg=msgp.stop;
			if(!msg) return IOT_ERROR_NOT_READY; //msg already sent?
			err=iot_prepare_msg(msg, IOT_MSG_STOP_MODINSTANCE, this, 0, NULL, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
			if(err) {
				assert(false);
				return err;
			}

			msgp.stop=NULL;
			//send signal to start
			thread->send_msg(msg);
			return IOT_ERROR_NOT_READY; //successful status for case with different threads
		}
	} else {
		assert(uv_thread_self()==thread->thread);
		target_state=IOT_MODINSTSTATE_INITED;
	}
	//this is working thread of modinstance

	if(state==IOT_MODINSTSTATE_INITED) goto onsucc;
	assert(state==IOT_MODINSTSTATE_STARTED || state==IOT_MODINSTSTATE_STOPPING);

	err=instance->stop();
	if(err==IOT_ERROR_TRY_AGAIN && state==IOT_MODINSTSTATE_STARTED) { //module asked some time to make graceful stop
		state=IOT_MODINSTSTATE_STOPPING;
		state_timeout=uv_now(thread->loop)+3*1000;
		recheck_job(true);
		return IOT_ERROR_NOT_READY;
	}
	if(err) {
		state=IOT_MODINSTSTATE_HUNG;
		outlog_error("Error stopping %s instance of module '%s::%s' with ID %u: %s. This is a bug. Module hung.", iot_modinsttype_name[type], module->dbitem->bundle->name, module->dbitem->module_name, module->config->module_id, kapi_strerror(err));
		goto onsucc;
	}
	//just stopped
	started=0;
	state=IOT_MODINSTSTATE_INITED;
	outlog_debug("%s instance of module '%s::%s' ID %u stopped (iid %u)",iot_modinsttype_name[type], module->dbitem->bundle->name, module->dbitem->module_name, module->dbitem->module_id, unsigned(miid.iid));
onsucc:
	err=0;
	if(isasync) {
		iot_threadmsg_t* msg=msgp.stopstatus;
		assert(msg!=NULL);
		err=iot_prepare_msg(msg, IOT_MSG_MODINSTANCE_STOPSTATUS, this, 0, NULL, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
		if(err) {
			assert(false);
		} else {
			msgp.stopstatus=NULL;
			msg->intarg=err;
			main_thread_item.send_msg(msg);
		}
		return IOT_ERROR_NOT_READY;
	}
	return on_stop_status(err, false);
}

//possible return status:
//	0 - success, modinstance has been started or was already started
//	IOT_ERROR_TEMPORARY_ERROR
//	IOT_ERROR_CRITICAL_ERROR - module instance is hung or instance is invalid
int iot_modinstance_item_t::on_stop_status(int err, bool isasync) { //processes result of modinstance stop in main thread
	assert(uv_thread_self()==main_thread);
	assert(state==IOT_MODINSTSTATE_INITED || state==IOT_MODINSTSTATE_HUNG);

	int i;
	if(!err) err=aborted_error;
	if(state==IOT_MODINSTSTATE_HUNG) {
		err=IOT_ERROR_CRITICAL_BUG;
	}

	switch(type) {
		case IOT_MODINSTTYPE_DETECTOR:
			break;
		case IOT_MODINSTTYPE_DRIVER: {
			assert(data.driver.hwdev!=NULL);
			if(err==IOT_ERROR_TEMPORARY_ERROR || err==IOT_ERROR_CRITICAL_ERROR) {
				uint32_t now32=uint32_t((uv_now(main_loop)+500)/1000);
				data.driver.hwdev->block_module(module->config->module_id, err==IOT_ERROR_CRITICAL_ERROR ? 0xFFFFFFFFu : now32+2*60*100, now32);
			} else if(err==IOT_ERROR_CRITICAL_BUG) {
				data.driver.hwdev->clear_module_block(module->config->module_id);
			}
			//close connections
			for(i=0;i<IOT_MAX_DRIVER_CLIENTS;i++) {
				if(data.driver.conn[i]) data.driver.conn[i]->close();
			}
			break;
		}
		case IOT_MODINSTTYPE_EVSOURCE:
			//close connections
			for(i=0;i<IOT_CONFIG_MAX_EVENTSOURCE_DEVICES;i++) {
				if(data.evsrc.dev[i].actual && data.evsrc.dev[i].conn) data.evsrc.dev[i].conn->close();
			}
			break;
	}

	if(err==IOT_ERROR_CRITICAL_BUG) {
		module->state[type]=IOT_MODULESTATE_DISABLED;
	}

	if(state==IOT_MODINSTSTATE_HUNG) {
		err=IOT_ERROR_CRITICAL_ERROR; //such error will be reported
	} else err=0;

	modules_registry->free_modinstance(this);
	return err;
}

void iot_modinstance_item_t::unlock(void) { //unlocked previously locked structure. CANNOT BE called if lock() returned false
		uint8_t c=0;
		bool notify=false;
		while(acclock.test_and_set(std::memory_order_acquire)) {
			//busy wait
			c++;
			if((c & 0x3F)==0x3F) sched_yield();
		}
		assert(refcount>0);
		refcount--;
		if(refcount==0 && pendfree) notify=true;
		acclock.clear(std::memory_order_release);
		if(notify) {
			if(uv_thread_self()==main_thread) { //can call directly
				modules_registry->free_modinstance(this);
			} else {
				iot_threadmsg_t* msg=msgp.stopstatus;
				assert(msg!=NULL);
				int err=iot_prepare_msg(msg, IOT_MSG_FREE_MODINSTANCE, this, 0, NULL, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
				assert(err==0);
				msgp.stopstatus=NULL;
				main_thread_item.send_msg(msg);
			}
		}
	}

