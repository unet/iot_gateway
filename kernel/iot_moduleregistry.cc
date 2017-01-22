#include<stdint.h>
#include <dlfcn.h>
//#include<time.h>
#include <new>

#include<iot_kapi.h>
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

void iot_modules_registry_t::start(iot_devifacecls_config_t* devifaceclscfg, uint32_t num) {
	assert(uv_thread_self()==main_thread);

	int err;
	err=register_devifaceclasses(devifaceclscfg, num, 0);
	if(err) {
		outlog_error("No memory to register all built-in Device Iface Classes");
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
		if(it->dbitem->autostart_detector && it->config->iface_device_detector->start) {
			err=it->config->iface_device_detector->start(0,NULL);
			if(err) {
				outlog_error("Error autostarting detector module with ID %u: %s", it->config->module_id, kapi_strerror(err));
			}
		}
		it=it->next_detector;
	}
}

int iot_modules_registry_t::register_devifaceclasses(iot_devifacecls_config_t* cfg, uint32_t num, uint32_t module_id) { //main thread
	//zero module_id is used for built-in device iface classes. It can be used to config class IDs from other modules
	assert(uv_thread_self()==main_thread);
	assert(cfg!=NULL || num==0);
	while(num>0) {
		if(module_id>0 && IOT_DEVIFACECLASS_CUSTOM_MODULEID(cfg->classid)!=module_id) {
			outlog_error("Module with ID %u tries to config Device Iface Class with wrong custom id %u", module_id, cfg->classid);
		} else {
			iot_devifacecls_item_t* has=find_devifaceclass(cfg->classid, false);
			if(has) { //duplicate definition
				if(has->module_id!=0) {
					outlog_error("Module with ID %u tries to give duplicate definition for Device Iface Class id %u", module_id, cfg->classid);
				}
			} else {
				has=(iot_devifacecls_item_t*)malloc(sizeof(iot_devifacecls_item_t));
				if(!has) return IOT_ERROR_NO_MEMORY;
				has->module_id=module_id;
				has->cfg=cfg;
				BILINKLIST_INSERTHEAD(has, devifacecls_head, next, prev);
			}
		}
		num--;
		cfg++;
	}
	return 0;
}


//finds module_id by module's index in modules DB
//returns -1 if not found
static inline int module_db_index_by_id(uint32_t module_id) { //TODO make some index for quick search
	for(unsigned i=0;i<MODULES_DB_ITEMS;i++) if(modules_db[i].module_id==module_id) return i;
	return -1;
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

		if(modules_db[module_index].bundle->linked) {
			if(!main_hmodule) main_hmodule=modules_db[module_index].bundle->hmodule=dlopen(NULL,RTLD_NOW | RTLD_LOCAL);
			else modules_db[module_index].bundle->hmodule=main_hmodule;
			strcpy(buf,"SELF");
		} else {
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
		outlog_error("Module ID %u from executable does not match value %u from DB", cfg->module_id, dbitem->module_id);
		return IOT_ERROR_CRITICAL_ERROR;
	}
	assert(dbitem->item==NULL);

	iot_module_item_t* item=(iot_module_item_t*)malloc(sizeof(iot_module_item_t));
	if(!item) return IOT_ERROR_NO_MEMORY;

	int err;
	//call init_module
	if(cfg->init_module) {
		err=cfg->init_module();
		if(err) {
			outlog_error("Error initializing module with ID %u: %s", cfg->module_id, kapi_strerror(err));
			free(item);
			return IOT_ERROR_NOT_INITED;
		}
	}
	memset(item, 0, sizeof(*item));
	item->config=cfg;
	item->dbitem=dbitem;
	dbitem->item=item;

	BILINKLIST_INSERTHEAD(item, all_head, next, prev);

	if(cfg->iface_device_detector) {
		if(!cfg->iface_device_detector->start || !cfg->iface_device_detector->stop) {
			outlog_error("Module with ID %u has incomplete detector interface", cfg->module_id);
			cfg->iface_device_detector=NULL;
		} else {
			BILINKLIST_INSERTHEAD(item, detectors_head, next_detector, prev_detector);
		}
	}
	if(cfg->iface_device_driver) {
		if(!cfg->iface_device_driver->init_instance || !cfg->iface_device_driver->deinit_instance
			|| !cfg->iface_device_driver->start || !cfg->iface_device_driver->stop) {
			outlog_error("Module with ID %u has incomplete driver interface", cfg->module_id);
			cfg->iface_device_driver=NULL;
			item->driver_blocked=1;
		} else {
			BILINKLIST_INSERTHEAD(item, drivers_head, next_driver, prev_driver);
		}
	}
	if(cfg->iface_event_source) {
		if(!cfg->iface_event_source->init_instance || !cfg->iface_event_source->deinit_instance
			|| !cfg->iface_event_source->start || !cfg->iface_event_source->stop) {
			outlog_error("Module with ID %u has incomplete event source interface", cfg->module_id);
			cfg->iface_event_source=NULL;
		} else {
			if(cfg->iface_event_source->num_devices>0 && (!cfg->iface_event_source->device_attached || !cfg->iface_event_source->device_detached)) {
				outlog_error("Module with ID %u has incomplete device connection interface in its event source part", cfg->module_id);
				cfg->iface_event_source->num_devices=0;
			}
			BILINKLIST_INSERTHEAD(item, evsrc_head, next_evsrc, prev_evsrc);
		}
	}
	//do post actions after filling all item props

	if(cfg->iface_device_driver && !item->driver_blocked) {
		hwdev_registry->try_find_hwdev_for_driver(item);
	}
	return 0;
}

iot_modinstance_item_t* iot_modules_registry_t::register_modinstance(iot_module_item_t* module, iot_modinstance_type_t type, 
	iot_thread_item_t *thread, void* instance, iot_config_inst_item_t* cfgitem) {
	assert(uv_thread_self()==main_thread);
	//find free index
	for(iot_iid_t i=0;i<IOT_MAX_MODINSTANCES;i++) {
		last_iid=(last_iid+1)%IOT_MAX_MODINSTANCES;
		if(!last_iid || iot_modinstances[last_iid].miid.iid) continue;

		uint32_t curtime=get_time32(time(NULL));
		if(iot_modinstances[last_iid].miid.created==curtime) curtime++; //guarantee different creation time for same miid

		memset(&iot_modinstances[last_iid], 0, sizeof(iot_modinstances[last_iid]));
		iot_modinstances[last_iid].module=module;
		iot_modinstances[last_iid].thread=thread;
		iot_modinstances[last_iid].miid.created=curtime;
		iot_modinstances[last_iid].miid.iid=last_iid;
		iot_modinstances[last_iid].type=type;
		iot_modinstances[last_iid].instance=instance;
		switch(type) {
			case IOT_MODINSTTYPE_DRIVER:
				BILINKLIST_INSERTHEAD(&iot_modinstances[last_iid], module->driver_instances_head, next_inmod, prev_inmod);
				iot_modinstances[last_iid].cpu_loading=module->config->iface_device_driver->cpu_loading;
				break;
			case IOT_MODINSTTYPE_EVSOURCE:
				iot_modinstances[last_iid].cfgitem=cfgitem;
				cfgitem->modinst=&iot_modinstances[last_iid];
				BILINKLIST_INSERTHEAD(&iot_modinstances[last_iid], module->evsrc_instances_head, next_inmod, prev_inmod);
				iot_modinstances[last_iid].cpu_loading=module->config->iface_event_source->cpu_loading;
				break;
		}
		thread_registry->add_modinstance(&iot_modinstances[last_iid], thread);
		return &iot_modinstances[last_iid];
	}
	return NULL;
}


//frees iid and removes modinstance from module's list of instances
void iot_modules_registry_t::free_modinstance(iot_modinstance_item_t* modinst) {
	assert(uv_thread_self()==main_thread);
	assert(modinst!=NULL && modinst->miid.iid!=0);

	BILINKLIST_REMOVE(modinst, next_inmod, prev_inmod); //remove instance from typed module's list
	thread_registry->remove_modinstance(modinst, modinst->thread); //remove instance from thread's list
//	switch(modinst->type) {
//		case IOT_MODINSTTYPE_DRIVER:
//			break;
//		case IOT_MODINSTTYPE_EVSOURCE:
//			break;
//	}
	modinst->miid.iid=0; //mark record of iot_modinstances array as free
	//modinst->miid.created MUST BE PRESERVED to guarantee uniqueness of created-miid pair
}

//try to create driver instance for provided real device
int iot_modules_registry_t::create_driver_modinstance(iot_module_item_t* module, iot_hwdevregistry_item_t* devitem) {
	assert(uv_thread_self()==main_thread);

	assert(module->config->iface_device_driver!=NULL);

	void *inst=NULL;
	iot_modinstance_item_t *modinst=NULL;
	int err;
	iot_threadmsg_t *msg=(iot_threadmsg_t*)main_allocator.allocate(sizeof(iot_threadmsg_t));
	if(!msg) return IOT_ERROR_NO_MEMORY;
	else {
		memset(msg, 0, sizeof(*msg));
		msg->is_msgmemblock=1;
	}
	//from here all errors go to onerr

	iot_thread_item_t* thread=thread_registry->assign_thread(module->config->iface_device_driver->cpu_loading);
	assert(thread!=NULL);

	iot_deviface_classid deviface_ids[IOT_MAX_CLASSES_PER_DEVICE];

	err=module->config->iface_device_driver->init_instance(&inst, thread->thread, &devitem->devdata, deviface_ids, IOT_MAX_CLASSES_PER_DEVICE);
	if(err<0) goto onerr;
	if(!inst) {
		outlog_error("Driver instance init for module %u returned NULL pointer. This is a bug.", module->config->module_id);
		err=IOT_ERROR_CRITICAL_BUG; //no error and no instance. this is a bug
		goto onerr;
	}
	//err>=0 and contains number of added items to deviface_ids
	int num_deviface_ids;
	num_deviface_ids=err;

	modinst=register_modinstance(module, IOT_MODINSTTYPE_DRIVER, thread, inst, NULL);
	if(!modinst) {
		err=IOT_ERROR_NO_MEMORY; //here registering failed which means memory error (or not enough iid space. TODO)
		goto onerr;
	}

	err=iot_prepare_msg(msg, IOT_MSG_START_MODINSTANCE, modinst, 0, NULL, 0, IOT_THREADMSG_DATAMEM_STATIC, &main_allocator, true);
	if(err) goto onerr;

	//check returned device iface ids
	devitem->num_devifaces=0;
	int i;
	for(i=0;i<num_deviface_ids;i++) {
		if(!deviface_ids[i]) continue;
		iot_devifacecls_item_t* deviface;
		deviface=find_devifaceclass(deviface_ids[i], true);
		if(deviface) devitem->devifaces[devitem->num_devifaces++]=deviface;
	}
	if(!devitem->num_devifaces) {
		outlog_error("Driver instance init for module %u returned no suitabe device interface. This is configurational bug.", module->config->module_id);
		err=IOT_ERROR_CRITICAL_BUG; //no error and no instance. this is a bug
		goto onerr;
	}

	//finish driver instanciation
	modinst->driver_hwdev=devitem;
	devitem->devdrv_modinst=modinst;

	//send signal to start
	modinst->started=uv_now(main_loop); //must be done before actually starting. main thread must know is start command was sent, start is async process
	thread->send_msg(msg);

	try_connect_driver_to_consumer(modinst);

	return 0;

onerr:
	if(modinst) free_modinstance(modinst);
	if(inst) module->config->iface_device_driver->deinit_instance(inst);
	iot_release_memblock(msg);
	return err;
}

//try to create evsrc instance
int iot_modules_registry_t::create_evsrc_modinstance(iot_module_item_t* module, iot_config_inst_item_t* item) {
	assert(uv_thread_self()==main_thread);

	assert(module->config->iface_event_source!=NULL);

	void *inst=NULL;
	iot_modinstance_item_t *modinst=NULL;
	int err;
	iot_threadmsg_t *msg=(iot_threadmsg_t*)main_allocator.allocate(sizeof(*msg));
	if(!msg) return IOT_ERROR_NO_MEMORY;
	else {
		memset(msg, 0, sizeof(*msg));
		msg->is_msgmemblock=1;
	}
	//from here all errors go to onerr

	iot_thread_item_t* thread=thread_registry->assign_thread(module->config->iface_event_source->cpu_loading);

	assert(thread!=NULL);
	err=module->config->iface_event_source->init_instance(&inst, thread->thread, item->iot_id, item->json_cfg);
	if(err) goto onerr;
	if(!inst) {
		outlog_error("Event source instance init for module %u returned NULL pointer. This is a bug.", module->config->module_id);
		err=IOT_ERROR_CRITICAL_BUG; //no error and no instance. this is a bug
		goto onerr;
	}

	modinst=register_modinstance(module, IOT_MODINSTTYPE_EVSOURCE, thread, inst, item);
	if(!modinst) {
		err=IOT_ERROR_NO_MEMORY; //here registering failed which means memory error (or not enough iid space. TODO)
		goto onerr;
	}
	err=iot_prepare_msg(msg, IOT_MSG_START_MODINSTANCE, modinst, 0, NULL, 0, IOT_THREADMSG_DATAMEM_STATIC, &main_allocator, true);
	if(err) goto onerr;

	//send signal to start
	modinst->started=uv_now(main_loop); //must be done before actually starting. main thread must know if start command was sent, start is async process
	thread->send_msg(msg);

	//now try to find configured or suitable device driver
	uint8_t num_devs;
	num_devs=module->config->iface_event_source->num_devices;
	uint8_t i;
	for(i=0;i<num_devs;i--)
		try_connect_evsrc_to_driver(modinst, i);

	return 0;

onerr:
	if(modinst) free_modinstance(modinst);
	if(inst) module->config->iface_event_source->deinit_instance(inst);
	iot_release_memblock(msg);
	return err;
}

//tries to setup connection of driver to appropriate consumer
void iot_modules_registry_t::try_connect_driver_to_consumer(iot_modinstance_item_t *drvinst) {
	assert(uv_thread_self()==main_thread);

	int err;
	iot_module_item_t* mod, *nextmod=modules_registry->all_head;
	while(nextmod) {
		mod=nextmod;
		nextmod=nextmod->next;

		iot_modinstance_item_t *modinst=mod->evsrc_instances_head; //loop through ev sources
		while(modinst) {
			auto iface=mod->config->iface_event_source;
			uint8_t num_devs=iface->num_devices;
			for(uint8_t dev_idx=0;dev_idx<num_devs;dev_idx++) {
				iot_device_connection_t* &conn=modinst->evsrc_devconn[dev_idx];
				if(conn && conn->state!=conn->IOT_DEVCONN_INIT) continue; //connection is set or in process of being set

				iot_hwdev_ident_t* devident;
				if(modinst->cfgitem->numitems>dev_idx) {
					devident=modinst->cfgitem->evsrc.dev[dev_idx];
				} else {
					devident=NULL;
				}
				if(devident) { //there is configured exact device
					if(!(*devident == drvinst->driver_hwdev->devdata.dev_ident)) continue;
				} else { //no exact device configured
					if(!iface->devcfg[dev_idx].flag_canauto) continue; //auto search not allowed
				}
				//here we have device match or can use auto detection

				if(!conn) {
					conn=iot_create_connection(modinst, dev_idx);
					if(!conn) { //no connection slots?
						outlog_error("Cannot allocate driver connection %d of evsrv iot_id=%u", int(dev_idx)+1, modinst->cfgitem->iot_id);
						continue;
					}
				}
				//here conn is in init state
				err=conn->connect_local(drvinst, iface->devcfg[dev_idx].classids, iface->devcfg[dev_idx].num_classids);
				if(!err) continue;
				//TODO react to errors
			}
			modinst=modinst->next_inmod;
		}

		//TODO iot_modinstance_item_t *modinst=mod->executor_instances_head; //loop through executors
	}
}

//tries to setup connection of event source to one of its driver
//returns 0 if no retry necessary or error code which must in some way determine retry delay
int iot_modules_registry_t::try_connect_evsrc_to_driver(iot_modinstance_item_t *modinst, uint8_t idx) {
	assert(uv_thread_self()==main_thread);
	auto iface=modinst->module->config->iface_event_source;
	assert(iface!=NULL);
	assert(idx<sizeof(modinst->evsrc_devconn)/sizeof(modinst->evsrc_devconn[0]));
	assert(idx<iface->num_devices);

	if(modinst->evsrc_devblock[idx]) return 0; //this device connection is blocked. flag should be reset when configured device changes

	auto devcfg=&iface->devcfg[idx];
	//check if device ident provided by user config
	iot_hwdev_ident_t* devident;
	if(modinst->cfgitem->numitems>idx) {
		devident=modinst->cfgitem->evsrc.dev[idx];
	} else {
		devident=NULL;
	}
	if(!devident && !devcfg->flag_canauto) {
		modinst->evsrc_devblock[idx]=1; //avoid repeated tries until devident changes
		return 0; //no dev assigned and auto search not allowed
	}

	iot_device_connection_t* &conn=modinst->evsrc_devconn[idx];
	if(!conn) {
		conn=iot_create_connection(modinst, idx);
		if(!conn) { //no connection slots?
			outlog_error("Cannot allocate driver connection %d of evsrv iot_id=%u", int(idx)+1, modinst->cfgitem->iot_id);
			return IOT_ERROR_NO_MEMORY;
		}
	} else {
		if(conn->state!=conn->IOT_DEVCONN_INIT) return 0; //TODO check if this is correct
	}

	int err;
	if(devident) do { //there is device configured. try to find exactly it
		if(devident->hostid != iot_current_hostid) { //device is on another gateway
			if(devcfg->flag_localonly) {
				//saved device cannot be used
				if(!devcfg->flag_canauto) {
					modinst->evsrc_devblock[idx]=1;
					return 0;
				}
				break;
			}
			iot_peer_link_t *plink=peers_registry->find_peer_link(devident->hostid);
			if(!plink) {//no link established.
//				if(uv_now(main_loop)-iot_starttime_ms>10000) break; //10 secs passed after startup, treat host as dead TODO?
				return IOT_ERROR_NO_PEER;  //must wait for some time before attempting to find another device TODO?
			}
			if(plink->is_failed()) break; //peer now is treated as dead
			if(!plink->is_data_trusted()) return IOT_ERROR_TEMPORARY_ERROR; //cannot be trusted. retry later TODO?
			iot_remote_driverinst_item_t* drvitem=plink->get_avail_drivers_list();
			while(drvitem) { //TODO move this search to plink
				if(drvitem->devident==devident->dev) {
					err=conn->connect_remote(&drvitem->miid, devcfg->classids, devcfg->num_classids);
					if(!err) return 0;
					//TODO react to errors
					return err;
				}
				drvitem=drvitem->next;
			}
		} else { //device is local
			iot_hwdevregistry_item_t* devitem=hwdev_registry->find_item(&devident->dev);
			if(devitem && devitem->devdrv_modinst) {
				err=conn->connect_local(devitem->devdrv_modinst, devcfg->classids, devcfg->num_classids);
				if(!err) return 0;
				//TODO react to errors
				return err;
			}
			if(uv_now(main_loop)-iot_starttime_ms<2000) return IOT_ERROR_TEMPORARY_ERROR; //small time after startup, device can just not be found yet
		}
	} while(0);
	//from here only auto detection can work
	if(!devcfg->flag_canauto) { //auto search not allowed
		modinst->evsrc_devblock[idx]=1; //avoid repeated tries until devident changes
		return 0; //no dev assigned and auto search not allowed
	}
	//start from local devices
	iot_module_item_t* drvmod=modules_registry->drivers_head;
	while(drvmod) {
		iot_modinstance_item_t *drvinst=drvmod->driver_instances_head;
		while(drvinst) {
			err=conn->connect_local(drvinst, devcfg->classids, devcfg->num_classids);
			if(!err) return 0;
			//TODO react to some errors
			drvinst=drvinst->next_inmod;
		}
		drvmod=drvmod->next_driver;
	}

	//find among remote devices
	//TODO
	return 0;
}

//after detecting new hw device tries to find appropriate driver
void iot_modules_registry_t::try_find_driver_for_hwdev(iot_hwdevregistry_item_t* devitem) {
	assert(uv_thread_self()==main_thread);
	assert(devitem->devdrv_modinst==NULL);

	iot_module_item_t* it, *itnext=drivers_head;
	int err;
	while(itnext) {
		it=itnext;
		itnext=itnext->next_driver;

		if(it->driver_blocked) continue;

		if(it->config->iface_device_driver->check_device) { //use this func for precheck if available
			err=it->config->iface_device_driver->check_device(&devitem->devdata);
			if(err) {
				if(err==IOT_ERROR_DEVICE_NOT_SUPPORTED) continue;
				outlog_error("Driver module with ID %u got error during check_device: %s", it->config->module_id, kapi_strerror(err));
				if(err==IOT_ERROR_CRITICAL_BUG) it->driver_blocked=1;
				continue;
			}
		}
		err=create_driver_modinstance(it, devitem);
		if(!err) return;

		if(err==IOT_ERROR_DEVICE_NOT_SUPPORTED) continue;
		outlog_error("Driver module with ID %u got error during init: %s", it->config->module_id, kapi_strerror(err));
		if(err==IOT_ERROR_CRITICAL_BUG) it->driver_blocked=1;

		//TODO setup retry if some driver returned recoverable error
	}
	//TODO setup retry if some driver returned recoverable error
	outlog_info("no driver for new HWDev found: contype=%d, unique=%lu\n", devitem->devdata.dev_ident.dev.contype, devitem->devdata.dev_ident.dev.unique_refid);
}

void iot_modules_registry_t::start_modinstance(iot_modinstance_item_t *modinst) { //called in necessary thread. tries to start module instance. if fails, schedules restart.
	//runs in any thread
	iot_module_item_t* module=modinst->module;
	int err;
	switch(modinst->type) {
		case IOT_MODINSTTYPE_DRIVER:
			err=module->config->iface_device_driver->start(modinst->instance, modinst->miid.iid);
			if(!err) {
				outlog_debug("Driver instance of module ID %u started",module->dbitem->module_id);
				return;
			}
			modinst->started=0; //TODO cannot assign it here, send msg to main thread. to avoid msg-allocation error, precreate some msgs for each instance!!!
			if(err==IOT_ERROR_CRITICAL_BUG) {
				outlog_error("Got critical error starting driver '%s'", module->dbitem->module_name);
				module->driver_blocked=1;
			}
			else {
				modinst->start_after=uv_now(main_loop)+300*1000;
			}
			//TODO deinit instance and try another driver?
			break;
		case IOT_MODINSTTYPE_EVSOURCE:
			err=module->config->iface_event_source->start(modinst->instance, modinst->miid.iid);
			if(!err) {
				outlog_debug("Event Source instance of module ID %u with iot_id=%u started",module->dbitem->module_id, modinst->cfgitem->iot_id);
				return;
			}
			modinst->started=0; //TODO cannot assign it here, send msg to main thread
			if(err==IOT_ERROR_CRITICAL_BUG) {
				outlog_error("Got critical error starting event source '%s'", module->dbitem->module_name);
				module->evsrc_blocked=1;
			}
			else {
				modinst->start_after=uv_now(main_loop)+300*1000;
			}
			//TODO deinit instance and set error in state?
			break;
	}
}

//called after driver instance accepts consumer connection
void iot_modules_registry_t::notify_device_attached(iot_device_connection_t* conn) {
	//runs in driver's instance thread
	assert(conn!=NULL);
	assert(conn->state==conn->IOT_DEVCONN_READY);
	assert(conn->driver_host==iot_current_hostid);
	assert(uv_thread_self()==conn->driver.modinst->thread->thread);

	//for now require local consumer connection. TODO for remote connections
	assert(conn->client_host==iot_current_hostid);

	iot_modinstance_item_t *modinst=conn->client.modinst;

	//send msg
	iot_threadmsg_t *msg=NULL;
	int err=iot_prepare_msg(msg, IOT_MSG_DRV_CONNECTION_READY, modinst, conn->client.dev_idx, NULL, 0, IOT_THREADMSG_DATAMEM_STATIC, conn->driver.modinst->thread->allocator, true);
	if(err) {
		if(msg) iot_release_memblock(msg);
		//TODO. retry?
		return;
	}
	modinst->thread->send_msg(msg);
}

//sends to device consumer instance signal about device connection
void iot_modules_registry_t::process_device_attached(iot_device_connection_t* conn) {
	//runs in consumer instance thread
	assert(conn!=NULL);
	assert(conn->state==conn->IOT_DEVCONN_READY);
	assert(conn->client_host==iot_current_hostid);

	iot_modinstance_item_t *modinst=conn->client.modinst;
	assert(uv_thread_self()==modinst->thread->thread);

	iot_device_conn_t c;
	if(conn->driver_host==iot_current_hostid) {
		c={conn->deviface->cfg->classid, {iot_current_hostid, conn->driver.modinst->module->config->module_id, conn->driver.modinst->miid}};
	} else {
		c={conn->deviface->cfg->classid, {conn->driver_host, conn->driver.remote.module_id, conn->driver.remote.miid}};
	}
	switch(modinst->type) {
		case IOT_MODINSTTYPE_EVSOURCE: {
			auto iface=modinst->module->config->iface_event_source;
			outlog_debug("Device input %d of event source module %u attached", int(conn->client.dev_idx)+1, modinst->module->config->module_id);
			iface->device_attached(modinst->instance, conn->client.dev_idx, conn->connid, &c);
			break;
			}
		case IOT_MODINSTTYPE_DRIVER: //list all illegal types
			assert(false);
			break;
	}
}
