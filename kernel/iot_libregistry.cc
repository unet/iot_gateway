#include<stdint.h>
#include <dlfcn.h>
//#include<time.h>
#include <new>

//#include "iot_module.h"
#include "config.h"
#include "iot_libregistry.h"
#include "iot_utils.h"
#include "iot_daemonlib.h"
#include "iot_common.h"
//#include "iot_deviceregistry.h"
//#include "iot_moduleregistry.h"
//#include "iot_configregistry.h"
//#include "iot_peerconnection.h"
//#include "iot_kernel.h"


//#define IOT_MODULESDB_BUNDLE_OBJ(vendor, bundle) ECB_CONCAT(iot_moddb_bundle_, ECB_CONCAT(vendor, ECB_CONCAT(__, bundle)))
//#define IOT_MODULESDB_BUNDLE_REC(vendor, bundle) static iot_regitem_lib_t IOT_MODULESDB_BUNDLE_OBJ(vendor, bundle)

//#define UV_LIB_INIT {NULL, NULL}

#include "iot_linkedlibs.cc"

const char* iot_modtype_name[IOT_MODTYPE_MAX+1]={
	"detector",
	"driver",
	"node"
};


iot_libregistry_t *libregistry=NULL;
static iot_libregistry_t _libregistry; //instantiate singleton

static int libpath2symname(const char *path, char *buf, size_t bufsz) {
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

void iot_libregistry_t::register_pending_metaclasses(void) {
	assert(uv_thread_self()==main_thread);
	iot_type_id_t type_id;

	iot_devifacetype_metaclass* &ifacetype_head=devifacetype_pendingreg_head(), *ifacetype_next=ifacetype_head, *ifacetype_cur=NULL, *ifacetype_prev=NULL;
	while(ifacetype_next) {
		ifacetype_cur=ifacetype_next;
		ifacetype_next=ifacetype_next->next;

		const iot_devifacetype_metaclass* cur=devifacetypes_head;
		while(cur) {
			if(cur==ifacetype_cur) {
				char namebuf[256];
				outlog_error("Double instanciation of Device Iface Type %s!", ifacetype_cur->get_fullname(namebuf, sizeof(namebuf)));
				assert(false);
				return;
			}
			cur=cur->next;
		}
		type_id=ifacetype_cur->get_id();

		if(!type_id && ifacetypes_table) { //try to find id in registry
			const char* name=ifacetype_cur->get_name();
			json_object* val;
			json_object_object_foreach(ifacetypes_table, idval, data) {
				if(!json_object_is_type(data, json_type_array)) continue;
				val=json_object_array_get_idx(data, 0); //get name
				if(val && json_object_is_type(val, json_type_string) && strcmp(name, json_object_get_string(val))==0) {
					val=json_object_array_get_idx(data, 1); //get library name
					if(val && json_object_is_type(val, json_type_string) && strcmp(ifacetype_cur->get_library(), json_object_get_string(val))==0) {
						IOT_STRPARSE_UINT(idval, iot_type_id_t, type_id);
						if(type_id) break;
					}
				}
			}
			if(!type_id) {
				outlog_info("Cannot find device interface type ID for '%s:%s' in lib registry", ifacetype_cur->get_library(), name);
			} else {
				ifacetype_cur->set_id(type_id);
			}
		}
		if(type_id) {
			ULINKLIST_REMOVE_NOCL(ifacetype_cur, ifacetype_prev, ifacetype_head, next);
			BILINKLIST_INSERTHEAD(ifacetype_cur, devifacetypes_head, next, prev);
		} else {//leave id-less items in pending list
			ifacetype_prev=ifacetype_cur;
		}
	}

	iot_hwdevcontype_metaclass*& contype_head=devcontype_pendingreg_head(), *contype_next=contype_head, *contype_cur=NULL, *contype_prev=NULL;
	while(contype_next) {
		contype_cur=contype_next;
		contype_next=contype_next->next;
		const iot_hwdevcontype_metaclass* cur=devcontypes_head;
		while(cur) {
			if(cur==contype_cur) {
				char namebuf[256];
				outlog_error("Double instanciation of Device Connection Type %s!", contype_cur->get_fullname(namebuf, sizeof(namebuf)));
				assert(false);
				return;
			}
			cur=cur->next;
		}
		type_id=contype_cur->get_id();
		if(!type_id && contypes_table) {
			const char* name=contype_cur->get_name();
			json_object* val;
			json_object_object_foreach(contypes_table, idval, data) {
				if(!json_object_is_type(data, json_type_array)) continue;
				val=json_object_array_get_idx(data, 0); //get name
				if(val && json_object_is_type(val, json_type_string) && strcmp(name, json_object_get_string(val))==0) {
					val=json_object_array_get_idx(data, 1); //get library name
					if(val && json_object_is_type(val, json_type_string) && strcmp(contype_cur->get_library(), json_object_get_string(val))==0) {
						IOT_STRPARSE_UINT(idval, iot_type_id_t, type_id);
						if(type_id) break;
					}
				}
			}
			if(!type_id) {
				outlog_info("Cannot find device connection type ID for '%s:%s' in lib registry", contype_cur->get_library(), name);
			} else {
				contype_cur->set_id(type_id);
			}
		}
		if(type_id) {
			ULINKLIST_REMOVE_NOCL(contype_cur, contype_prev, contype_head, next);
			BILINKLIST_INSERTHEAD(contype_cur, devcontypes_head, next, prev);
		} else {//leave id-less items in pending list
			contype_prev=contype_cur;
		}
	}

}

//returns error code:
//0 - success
//IOT_ERROR_CRITICAL_ERROR - lib cannot be loaded, is misconfigured or init failed
int iot_libregistry_t::load_lib(iot_regitem_lib_t* lib/*, const char* module_name*/) { //tries to load lib
	static void* main_hmodule=NULL;
	char buf[256], versbuf[256];

	//try to load lib
	if(lib->error) return IOT_ERROR_CRITICAL_ERROR; //lib loading is impossible
//	if(lib->duplicate) {
//		outlog_error("lib %s is duplicated for module %s: build configuration is broken", lib->name, module_name ? module_name : "<empty>");
//		lib->error=true;
//		return IOT_ERROR_CRITICAL_ERROR;
//	}

	int off=snprintf(versbuf, sizeof(versbuf), "%s","iot_libversion_");
	off+=libpath2symname(lib->name, versbuf+off, sizeof(versbuf)-off);
	if(off>=int(sizeof(versbuf))) {
		outlog_error("Too small buffer to load version from lib '%s'", lib->name);
		lib->error=true;
		return IOT_ERROR_CRITICAL_ERROR;
	}


	if(lib->linked) { //module is linked into executable
		if(!main_hmodule) main_hmodule=lib->hmodule=dlopen(NULL,RTLD_NOW | RTLD_LOCAL);
			else lib->hmodule=main_hmodule;
		strcpy(buf,"SELF");
	} else { //module is in external library
		off=snprintf(buf, sizeof(buf), "%s/%s%s", modules_dir, lib->name, IOT_SOEXT);
		if(off>=int(sizeof(buf))) {
			outlog_error("Too small buffer to build path for library '%s'", lib->name);
			lib->error=true;
			return IOT_ERROR_CRITICAL_ERROR;
		}
		lib->hmodule=dlopen(buf, RTLD_NOW | RTLD_GLOBAL);
	}
	if(!lib->hmodule) {
		outlog_error("Error loading module library '%s': %s", buf, dlerror());
		lib->error=true;
		return IOT_ERROR_CRITICAL_ERROR;
	}
	//loaded
	//read lib version
	uint32_t *verptr=(uint32_t*)dlsym(lib->hmodule, versbuf);
	if(!verptr) {
		outlog_error("Error loading version symbol '%s' from lib '%s': %s", versbuf, lib->name, dlerror());
		if(!lib->linked) dlclose(lib->hmodule);
		lib->hmodule=NULL;
		lib->error=true;
		return IOT_ERROR_CRITICAL_ERROR;
	}
	lib->version=*verptr;

	register_pending_metaclasses(); //register types from just linked lib
	return 0;
}

//returns error code:
//0 - success
//IOT_ERROR_NOT_FOUND
//IOT_ERROR_CRITICAL_ERROR - module or bundle cannot be loaded, is misconfigured or init failed
//IOT_ERROR_NO_MEMORY
int iot_libregistry_t::find_module_config(iot_module_type_t type, const char* module_name, iot_regitem_lib_t* lib, void* &cfg) { //tries to find module in specified library and returns its config
	char buf[256];
	int err;

	cfg=NULL;
	int off=snprintf(buf, sizeof(buf), "iot_abi" IOT_CORE_ABI_VERSION_STR "_%s_modconf_", iot_modtype_name[type]);
	off+=libpath2symname(lib->name, buf+off, sizeof(buf)-off);
	if(off<int(sizeof(buf))) {
		off+=snprintf(buf+off, sizeof(buf)-off, "__%s",module_name);
	}
	if(off>=int(sizeof(buf))) {
		outlog_error("Too small buffer to load %s  module '%s' from library '%s'", iot_modtype_name[type], module_name, lib->name);
		return IOT_ERROR_CRITICAL_ERROR;
	}

	if(!lib->hmodule) { //lib is not loaded
		err=load_lib(lib);
		if(err) return err;
	}
	//lib is loaded
	//load symbol with module config
	cfg=dlsym(lib->hmodule, buf);
	if(!cfg) {
		outlog_error("Error loading symbol '%s' for %s module '%s' from library '%s': %s", buf, iot_modtype_name[type], module_name, lib->name, dlerror());
		return IOT_ERROR_NOT_FOUND;
	}
	return 0;
}



