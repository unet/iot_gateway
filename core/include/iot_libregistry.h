#ifndef IOT_LIBREGISTRY_H
#define IOT_LIBREGISTRY_H

#include<stdint.h>
#include<assert.h>
#include<string.h>

#include "iot_core.h"

enum iot_module_type_t : uint8_t {
	IOT_MODTYPE_DETECTOR=0,
	IOT_MODTYPE_DRIVER=1,	//module which realizes interface of hardware device driver iface_device_driver
	IOT_MODTYPE_NODE=2,	//module which can be source of events. Realizes iface_event_source interface
	IOT_MODTYPE_MAX=2
};

extern const char* iot_modtype_name[];


class iot_libregistry_t;
extern iot_libregistry_t* libregistry;

//#include "iot_deviceregistry.h"
//#include "iot_configregistry.h"
//#include "iot_deviceconn.h"
//#include "iot_peerconnection.h"

//struct iot_regitem_module_t;
struct iot_any_module_item_t;
struct iot_regitem_lib_t {
	static iot_regitem_lib_t*& get_listhead(void) {
		static iot_regitem_lib_t* listhead=NULL;
		return listhead;
	}

	iot_regitem_lib_t* next;
//	iot_regitem_module_t* modules_listhead=NULL;
	char name[IOT_LIBNAME_MAXLEN+1]; //like "vendor/subdirs/libname". extension (.so) must be appended
	int namelen;
	void *hmodule=NULL; //handle of dynamically loaded file or of main executable when linked is true

	uint32_t version; //is checked when bundle is loaded (hmodule assigned). value UINT32_MAX means that check is skipped (actual version stored adter load)
	bool linked; //bundle is statically linked
	bool error=false; //was error loading, so do not try again
	bool signature_set=false;
	char signature[IOT_SIGLEN]; //binary signature
//	bool duplicate=false;

	iot_regitem_lib_t(const char* libname, bool linked, uint32_t version=UINT32_MAX) : version(version), linked(linked) {
		namelen=snprintf(name, sizeof(name), "%s", libname);
		assert(namelen<(int)sizeof(name));
		iot_regitem_lib_t* &listhead=get_listhead();
//		//check there are no same bundle already present
//		iot_regitem_lib_t* p=listhead;
//		while(p) {
//			if(strcmp(p->name, name)==0) {
//				linked=false;
//				duplicate=true; //this will force error during module loading
//				return;
//			}
//			p=p->next;
//		}
		next=listhead;
		listhead=this;
	}
	static iot_regitem_lib_t* find_item(const char* name) {
		int len=(int)strlen(name);
		iot_regitem_lib_t* lib=get_listhead();
		while(lib) {
			if(lib->namelen==len && memcmp(lib->name, name, len)==0) break;
			lib=lib->next;
		}
		return lib;
	}
};


struct iot_regitem_module_t {
	static iot_regitem_module_t*& get_listhead(iot_module_type_t type) {
		static iot_regitem_module_t* listhead[IOT_MODTYPE_MAX+1]={};
		assert(type<=IOT_MODTYPE_MAX);
		return listhead[type];
	}

	iot_regitem_module_t* next;
	iot_regitem_lib_t* bundle;
	iot_any_module_item_t *item=NULL; //assigned during module loading
	int namelen;
	uint32_t module_id;
	char module_name[IOT_MODULENAME_MAXLEN+1]; //pure module name   //////(if bundle defined) or full name with bundle path in it (if bundle is NULL)
	bool autoload; //module's config must be auto loaded (after loading appropriate bundle into memory)
	iot_module_type_t type;

	iot_regitem_module_t(iot_module_type_t type, const char* modname, iot_regitem_lib_t* lib, uint32_t module_id, bool autoload=false)
		: bundle(lib), module_id(module_id), autoload(autoload), type(type)
	{
		namelen=snprintf(module_name, sizeof(module_name), "%s", modname);
		assert(namelen<(int)sizeof(module_name));
		iot_regitem_module_t* &listhead=get_listhead(type);
		next=listhead;
		listhead=this;
	}
	static iot_regitem_module_t* find_item(iot_module_type_t type, const char* name, iot_regitem_lib_t* lib) {
		int len=(int)strlen(name);
		iot_regitem_module_t* module=get_listhead(type);
		while(module) {
			if(module->bundle==lib && module->namelen==len && memcmp(module->module_name, name, len)==0) break;
			module=module->next;
		}
		return module;
	}
	static iot_regitem_module_t* find_item(iot_module_type_t type, uint32_t module_id) {
		iot_regitem_module_t* module=get_listhead(type);
		while(module) {
			if(module->module_id==module_id) break;
			module=module->next;
		}
		return module;
	}
};

class iot_libregistry_t {
	iot_hwdevcontype_metaclass *devcontypes_head=NULL; //list of registered hwdevcontypes as list of addresses of metaclass instances
	iot_devifacetype_metaclass *devifacetypes_head=NULL; //list of registered devifacetypes as list of addresses of metaclass instances
	iot_datatype_metaclass *datatypes_head=NULL; //list of registered hwdevcontypes as list of addresses of metaclass instances
	iot_valuenotion *notiontypes_head=NULL; //list of registered devifacetypes as list of addresses of metaclass instances

	json_object* contypes_table=NULL;
	json_object* ifacetypes_table=NULL;
	json_object* datatypes_table=NULL;
	json_object* notiontypes_table=NULL;
	bool core_signature_valid=false; //TODO

public:

	iot_libregistry_t(void) {
		assert(libregistry==NULL);
		libregistry=this;
		//manual registration for several datatype metaclasses (which use constexpr object instance)
//		iot_datatype_metaclass* &head=iot_libregistry_t::datatype_pendingreg_head();
//		ULINKLIST_INSERTHEAD(const_cast<iot_datatype_metaclass_boolean*>(&iot_datatype_metaclass_boolean::object), head, next);
//		ULINKLIST_INSERTHEAD(const_cast<iot_datatype_metaclass_nodeerrorstate*>(&iot_datatype_metaclass_nodeerrorstate::object), head, next);
	}
	~iot_libregistry_t(void) {
		if(contypes_table) {
			json_object_put(contypes_table);
			contypes_table=NULL;
		}
		if(ifacetypes_table) {
			json_object_put(ifacetypes_table);
			ifacetypes_table=NULL;
		}
		if(datatypes_table) {
			json_object_put(datatypes_table);
			datatypes_table=NULL;
		}
	}
	bool is_libname_valid(const char* libname) { //check if provided libname is valid like "vendor/libdir/name" and each component starts with letter and contains only [A-Za-z0-9_], max len is 64
		size_t len=strlen(libname);
		if(len>IOT_LIBNAME_MAXLEN) return false;
		if(strstr(libname, "__")) return false; //must not have two underscores one after one
		if(strspn(libname,"QWERTYUIOPASDFGHJKLZXCVBNMqwertyuiopasdfghjklzxcvbnm0123456789_/")!=len) return false;
		const char *s=libname, *e;
		//check vendor
		if(!((*s>='A' && *s<='Z') || (*s>='a' && *s<='z'))) return false;
		e=strchr(s, '/');
		if(!e) return false;
		s=e+1;
		//check libdir
		if(!((*s>='A' && *s<='Z') || (*s>='a' && *s<='z'))) return false;
		e=strchr(s, '/');
		if(!e) return false;
		s=e+1;
		//check name
		if(!((*s>='A' && *s<='Z') || (*s>='a' && *s<='z'))) return false;
		e=strchr(s, '/');
		if(e) return false; //excess dir level
		return true;
	}
	int apply_registry(json_object *reg, bool process_modules=false) {
		if(!reg) {
			register_pending_metaclasses();
			return 0;
		}
		if(json_object_object_get_ex(reg, "contypes", &contypes_table)) {
			if(!json_object_is_type(contypes_table,  json_type_object)) {
				outlog_error("Invalid data in lib registry! Top level 'contypes' item must be a JSON-object");
				contypes_table=NULL;
				return IOT_ERROR_BAD_DATA;
			} else {
				json_object_get(contypes_table);
			}
		}
		if(json_object_object_get_ex(reg, "ifacetypes", &ifacetypes_table)) {
			if(!json_object_is_type(ifacetypes_table,  json_type_object)) {
				outlog_error("Invalid data in lib registry! Top level 'ifacetypes' item must be a JSON-object");
				ifacetypes_table=NULL;
				return IOT_ERROR_BAD_DATA;
			} else {
				json_object_get(ifacetypes_table);
			}
		}
		if(json_object_object_get_ex(reg, "datatypes", &datatypes_table)) {
			if(!json_object_is_type(datatypes_table,  json_type_object)) {
				outlog_error("Invalid data in lib registry! Top level 'datatypes' item must be a JSON-object");
				datatypes_table=NULL;
				return IOT_ERROR_BAD_DATA;
			} else {
				json_object_get(datatypes_table);
			}
		}
		if(json_object_object_get_ex(reg, "notiontypes", &notiontypes_table)) {
			if(!json_object_is_type(notiontypes_table,  json_type_object)) {
				outlog_error("Invalid data in lib registry! Top level 'notiontypes' item must be a JSON-object");
				notiontypes_table=NULL;
				return IOT_ERROR_BAD_DATA;
			} else {
				json_object_get(notiontypes_table);
			}
		}
		register_pending_metaclasses();
		if(!process_modules) return 0;
		json_object* ob;
		if(json_object_object_get_ex(reg, "libs", &ob)) {
			if(!json_object_is_type(ob,  json_type_object)) {
				outlog_error("Invalid data in lib registry! Top level 'libs' item must be a JSON-object");
				return IOT_ERROR_BAD_DATA;
			}
			json_object* val;
			json_object_object_foreach(ob, libname, libdata) {
				if(!json_object_is_type(libdata, json_type_array)) {
					outlog_error("Invalid data in lib registry! Values in 'libs' object must be JSON-arrays, but 'libs.\"%s\"' is not", libname);
					return IOT_ERROR_BAD_DATA;
				}
				val=json_object_array_get_idx(libdata,1);
				uint32_t version=val && json_object_is_type(val, json_type_string) ? iot_parse_version(json_object_get_string(val)) : 0;

				if(strcmp(libname, "CORE")==0) {
					//TODO check current binary signature and version
					continue;
				}
				if(!is_libname_valid(libname)) {
					outlog_notice("Invalid data in lib registry. Library name '%s' is unacceptable", libname);
					continue;
				}
				bool linked=json_object_get_boolean(json_object_array_get_idx(libdata,0)) ? true : false;
				iot_regitem_lib_t* libitem=iot_regitem_lib_t::find_item(libname);
				if(libitem) { //should be true for linked-in libs only
					assert(libitem->linked);
					if(libitem->linked!=linked) outlog_notice("Invalid data in lib registry. Library linked state is incorrect for 'libs.\"%s\"'", libname);
					if(libitem->hmodule) { //already loaded
						if(libitem->version!=version) outlog_notice("Invalid data in lib registry. Library version mismatch for 'libs.\"%s\"'", libname);
					} else {
						libitem->version=version; //will be checked during load
					}
				} else {
					if(linked) outlog_notice("Invalid data in lib registry. Library linked state is incorrect for 'libs.\"%s\"'", libname);
					libitem=new iot_regitem_lib_t(libname, false, version);
					if(!libitem) {
						outlog_error("Cannot allocate memory for library item '%s'", libname);
						return IOT_ERROR_NO_MEMORY;
					}
					//TODO signature
					val=json_object_array_get_idx(libdata,3); //hex-encoded signature
					if(val && json_object_is_type(val, json_type_string) && json_object_get_string_len(val)==IOT_SIGLEN*2) {
						const char *sig=json_object_get_string(val);
						memcpy(libitem->signature, sig, IOT_SIGLEN); //TODO!!!! decode HEX encoding
						libitem->signature_set=true;
					}
				}
			}
		}

		char keybuf[32];
		for(uint8_t tp=0; tp<=IOT_MODTYPE_MAX; tp++) {
			snprintf(keybuf, sizeof(keybuf), "%s_modules", iot_modtype_name[tp]);

			if(json_object_object_get_ex(reg, keybuf, &ob)) {
				if(!json_object_is_type(ob,  json_type_object)) {
					outlog_error("Invalid data in lib registry! Top level '%s' item must be a JSON-object", keybuf);
					return IOT_ERROR_BAD_DATA;
				}
				json_object* val;
				json_object_object_foreach(ob, modid, moddata) {
					if(!json_object_is_type(moddata, json_type_array)) {
						outlog_error("Invalid data in lib registry! Values in '%s' object must be JSON-arrays, but value under '%s' is not", keybuf, modid);
						return IOT_ERROR_BAD_DATA;
					}
					val=json_object_array_get_idx(moddata,0);
					if(!val || !json_object_is_type(val, json_type_string)) {
						outlog_notice("Ignoring invalid module record '%s.%s' in lib registry. Module name must be a string.", keybuf, modid);
						continue;
					}
					int namelen=json_object_get_string_len(val);
					if(!namelen || namelen>IOT_MODULENAME_MAXLEN) {
						outlog_notice("Ignoring invalid module record '%s.%s' in lib registry. Module name length must be from 0 to %d.", keybuf, modid, IOT_MODULENAME_MAXLEN);
						continue;
					}
					const char* name=json_object_get_string(val);

					val=json_object_array_get_idx(moddata,1);
					if(!val || !json_object_is_type(val, json_type_string)) continue;
					const char* libname=json_object_get_string(val);
					iot_regitem_lib_t* libitem=iot_regitem_lib_t::find_item(libname);
					if(!libitem) continue;

					uint32_t module_id=0;
					IOT_STRPARSE_UINT(modid, uint32_t, module_id);
					if(!module_id) continue;

					iot_regitem_module_t* moditem=iot_regitem_module_t::find_item(iot_module_type_t(tp), name, libitem);
					if(moditem) {
						if(moditem->module_id==module_id) continue;
						outlog_error("Invalid data in lib registry! %s module '%s:%s' has several records with different IDs %" PRIu32 " and %" PRIu32, iot_modtype_name[tp], libname, name, moditem->module_id, module_id);
						return IOT_ERROR_BAD_DATA;
					}
					moditem=new iot_regitem_module_t(iot_module_type_t(tp), name, libitem, module_id, tp==IOT_MODTYPE_NODE ? false : true); //TODO set autoload according to config
					if(!moditem) {
						outlog_error("Cannot allocate memory for %s module item '%s:%s'", iot_modtype_name[tp], libname, name);
						return IOT_ERROR_NO_MEMORY;
					}
				}
			}
		}
		return 0;
	}

	iot_devifacetype_metaclass* find_devifacetype(iot_type_id_t ifacetp, bool tryload) { //must run in main thread if tryload is true
		if(!ifacetp) return NULL;
		iot_devifacetype_metaclass* item=devifacetypes_head;
		while(item) {
			if(item->get_id()==ifacetp) return item;
			item=item->next;
		}
		if(tryload) {// && IOT_DEVCONTYPE_CUSTOM_MODULEID(ifacetp)>0) 
			//TODO search in registry for bundle name
//			if(!load_module(IOT_DEVCONTYPE_CUSTOM_MODULEID(ifacetp), NULL)) return find_devcontype(ifacetp, false);
		}
		return NULL;
	}
	iot_devifacetype_metaclass* find_devifacetype(const char* name) {
		iot_devifacetype_metaclass* item=devifacetypes_head;
		while(item) {
			if(strcmp(item->type_name, name)==0) return item;
			item=item->next;
		}
		//for name search need to check pending list too
		item=devifacetype_pendingreg_head();
		while(item) {
			if(strcmp(item->type_name, name)==0) return item;
			item=item->next;
		}
		return NULL;
	}
	iot_hwdevcontype_metaclass* find_devcontype(iot_type_id_t contp, bool tryload) { //must run in main thread if tryload is true
		if(!contp) return NULL;
		iot_hwdevcontype_metaclass* item=devcontypes_head;
		while(item) {
			if(item->get_id()==contp) return item;
			item=item->next;
		}
		if(tryload) {// && IOT_DEVCONTYPE_CUSTOM_MODULEID(contp)>0) 
			//TODO search in registry for bundle name
//			if(!load_module(IOT_DEVCONTYPE_CUSTOM_MODULEID(contp), NULL)) return find_devcontype(contp, false);
		}
		return NULL;
	}
	iot_hwdevcontype_metaclass* find_devcontype(const char* name) {
		iot_hwdevcontype_metaclass* item=devcontypes_head;
		while(item) {
			if(strcmp(item->type_name, name)==0) return item;
			item=item->next;
		}
		//for name search need to check pending list too
		item=devcontype_pendingreg_head();
		while(item) {
			if(strcmp(item->type_name, name)==0) return item;
			item=item->next;
		}
		return NULL;
	}
	iot_datatype_metaclass* find_datatype(iot_type_id_t datatp, bool tryload) { //must run in main thread if tryload is true
		if(!datatp) return NULL;
		iot_datatype_metaclass* item=datatypes_head;
		while(item) {
			if(item->get_id()==datatp) return item;
			item=item->next;
		}
		if(tryload) {// && IOT_DEVCONTYPE_CUSTOM_MODULEID(datatp)>0) 
			//TODO search in registry for bundle name
//			if(!load_module(IOT_DEVCONTYPE_CUSTOM_MODULEID(datatp), NULL)) return find_devcontype(datatp, false);
		}
		return NULL;
	}
	iot_datatype_metaclass* find_datatype(const char* name) {
		iot_datatype_metaclass* item=datatypes_head;
		while(item) {
			if(strcmp(item->type_name, name)==0) return item;
			item=item->next;
		}
		//for name search need to check pending list too
		item=datatype_pendingreg_head();
		while(item) {
			if(strcmp(item->type_name, name)==0) return item;
			item=item->next;
		}
		return NULL;
	}

	iot_valuenotion* find_notiontype(const char* name) {
		iot_valuenotion* item=notiontypes_head;
		while(item) {
			if(strcmp(item->type_name, name)==0) return item;
			item=item->next;
		}
		//for name search need to check pending list too
		item=notiontype_pendingreg_head();
		while(item) {
			if(strcmp(item->type_name, name)==0) return item;
			item=item->next;
		}
		return NULL;
	}




	static iot_devifacetype_metaclass*& devifacetype_pendingreg_head(void) {
		static iot_devifacetype_metaclass *head=NULL; //use function-scope static to guarantee initialization before first use
		return head;
	}
	static iot_hwdevcontype_metaclass*& devcontype_pendingreg_head(void) {
		static iot_hwdevcontype_metaclass *head=NULL; //use function-scope static to guarantee initialization before first use
		return head;
	}
	static iot_datatype_metaclass*& datatype_pendingreg_head(void) {
		static iot_datatype_metaclass *head=NULL; //use function-scope static to guarantee initialization before first use
		return head;
	}
	static iot_valuenotion*& notiontype_pendingreg_head(void) {
		static iot_valuenotion *head=NULL; //use function-scope static to guarantee initialization before first use
		return head;
	}

	int load_lib(iot_regitem_lib_t* lib/*, const char* module_name*/); //tries to load library
	int find_module_config(iot_module_type_t type, const char* module_name, iot_regitem_lib_t* lib, void* &cfg);

//	static iot_regitem_module_t* find_modulesdb_item(const char* name, const iot_regitem_lib_t* bundle); //name must be pure module's name when bundle defined and full otherwise
	void register_pending_metaclasses(void); //main thread
};


#endif //IOT_MODULEREGISTRY_H
