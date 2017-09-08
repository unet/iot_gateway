#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>

#include <json-c/json.h>

#include "iot_module.h"

#include "iot_daemonlib.h"
#include "iot_kernel.h"
#include "iot_moduleregistry.h"
#include "iot_libregistry.h"

#include "iot_dynlibs.cc"


#define MANIFEST_IN "manifest.json"
#define MANIFEST_PART_OUT "manifest.part.json"

#define REGISTRY_PART_OUT "registry.part.json"
#define REGISTRY_OUT "registry.json"

#define BUILD_MODULES_DIR "modules"

json_object* libraryref=NULL;

struct ids_registry {
	const char* filename;
	bool is_dev, is_mod=false;
	int fd=-1;
	json_object *topobj=NULL;
	json_object *ifacetypes=NULL;
	json_object *contypes=NULL;
	json_object *modules=NULL;

	ids_registry(const char *filename, bool is_dev) : filename(filename),is_dev(is_dev) {
	}
	~ids_registry(void) {
		if(is_dev && is_mod) save();
		if(fd) close(fd);
		fd=-1;
	}

	void save(void) {
		assert(is_dev && is_mod && fd>=0 && topobj);
		const char *s=json_object_to_json_string_ext(topobj, JSON_C_TO_STRING_PRETTY);
		size_t len=strlen(s);
		ftruncate(fd, 0);
		write(fd, s, len);
		is_mod=false;
	}

	uint32_t get_uint(json_object* json, const char* key) {
		uint32_t id=0;
		json_object* val=NULL;
		if(!json_object_object_get_ex(json, key, &val)) return 0;
		IOT_JSONPARSE_UINT(val, uint32_t, id);
		return id;
	}
	uint32_t gen_new_dev_id(json_object* json) {
		char arr[1000];
		memset(arr, 0, sizeof(arr));
		json_object_object_foreach(json, key, val) {
			(void)key;
			uint32_t v=0;
			IOT_JSONPARSE_UINT(val, uint32_t, v);
			if(v && v<1000) arr[v]=1;
		}
		//return first hole
		for(uint32_t i=1;i<1000;i++) if(!arr[i]) return i;
		return 0; //all development IDs (1-999) are busy
	}

	iot_type_id_t find_ifacetype(const char* name) {
		return get_uint(ifacetypes, name);
	}
	iot_type_id_t find_contype(const char* name) {
		return get_uint(contypes, name);
	}
	iot_type_id_t find_module(const char* fullname) {
		return get_uint(modules, fullname);
	}

	//returns non-zero on error
	int open(void) {
		if(fd>=0) {
			outlog_error("Trying to open '%s' twice", filename);
			return 1;
		}

		char namebuf[256], errbuf[256];
		snprintf(namebuf, sizeof(namebuf), "%s/%s", bin_dir, filename);

		fd=::open(namebuf, is_dev ? O_RDWR | O_CREAT : O_RDONLY, 0644);
		if(fd<0) {
			if(!is_dev && errno==ENOENT ) {
				ifacetypes=json_object_new_object();
				contypes=json_object_new_object();
				modules=json_object_new_object();
				return 0; //success
			}
			outlog_error("Cannot open '%s': %s", namebuf, strerror_r(errno, errbuf, sizeof(errbuf)));
			return 1;
		}
		if(!is_dev) {
			close(fd);
			fd=-1;
			topobj=iot_configregistry_t::read_jsonfile(NULL, namebuf, "IDs registry");
		} else {
			flock(fd, LOCK_EX);
			struct stat st;
			fstat(fd, &st);
			if(st.st_size==0) {
				topobj=json_object_new_object();
			} else {
				topobj=iot_configregistry_t::read_jsonfile(NULL, namebuf, "development IDs registry");
			}
			is_mod=false;
		}
		if(!topobj) return 1;

		if(json_object_object_get_ex(topobj, "ifacetypes", &ifacetypes)) {
			if(!json_object_is_type(ifacetypes,  json_type_object)) {
				outlog_error("Invalid data in IDs registry '%s': top level 'ifacetypes' item must be a JSON-object", namebuf);
				return 1;
			}
		} else {
			printf("No ifacetypes found in '%s'\n", namebuf);
			ifacetypes=json_object_new_object();
			json_object_object_add(topobj, "ifacetypes", ifacetypes);
		}
		if(json_object_object_get_ex(topobj, "contypes", &contypes)) {
			if(!json_object_is_type(contypes,  json_type_object)) {
				outlog_error("Invalid data in IDs registry '%s': top level 'contypes' item must be a JSON-object", namebuf);
				return 1;
			}
		} else {
			printf("No contypes found in '%s'\n", namebuf);
			contypes=json_object_new_object();
			json_object_object_add(topobj, "contypes", contypes);
		}
		if(json_object_object_get_ex(topobj, "modules", &modules)) {
			if(!json_object_is_type(modules,  json_type_object)) {
				outlog_error("Invalid data in IDs registry '%s': top level 'modules' item must be a JSON-object", namebuf);
				return 1;
			}
		} else {
			printf("No modules found in '%s'\n", namebuf);
			modules=json_object_new_object();
			json_object_object_add(topobj, "modules", contypes);
		}
		return 0;
	}
} reg_ids("reg_ids.json", false), dev_ids("dev_ids.json", true);


int proc_module(const char* modname, iot_moduleconfig_t* modcfg, iot_regitem_lib_t* lib, json_object* data, json_object* regpart, const char* manifest_path) {
	uint32_t id;
	char fullname[256];
	int len=snprintf(fullname, sizeof(fullname), "%s:%s", lib->name, modname);
	if(len>=int(sizeof(fullname))) {
		outlog_error("Buffer is too small to build full name of module '%s' in library '%s'", modname, lib->name);
		return 1; //buffer is too small
	}

	id=reg_ids.find_module(fullname);
	if(!id) {
		id=dev_ids.find_module(fullname);
		if(!id) {
			id=dev_ids.gen_new_dev_id(dev_ids.modules);
			if(id) { //free ID found, remember it
				json_object_object_add(dev_ids.modules, fullname, json_object_new_int64(id));
				dev_ids.is_mod=true;
				outlog_debug("New id %u for module %s was generated", id, modname);
			}
		}
	}
	if(id) {
		outlog_debug("Found id %u for module %s", id, modname);
	} else {
		outlog_error("No ID for module %s", modname);
		return 1;
	}

	char verbuf[32];
	iot_version_str(modcfg->version, verbuf, sizeof(verbuf));
	char idbuf[16];
	snprintf(idbuf, sizeof(idbuf), "%u", id);
	json_object* arr=json_object_new_array();
	json_object_object_add(regpart, idbuf, arr);
	json_object_array_add(arr, json_object_new_string(modname));
	json_object_array_add(arr, libraryref);
	json_object_array_add(arr, json_object_new_string(verbuf));

	json_object_object_add(data, "id", json_object_new_int64(id));
	json_object_object_add(data, "version", json_object_new_string(verbuf));
	//check provided role(s)
	json_object* provides=NULL;
	if(!json_object_object_get_ex(data, "provides", &provides) || !json_object_is_type(provides,  json_type_object)) {
		outlog_error("No 'provides' sub-object for module '%s' in library '%s'", modname, lib->name);
		return 1;
	}

	int num_roles=0;
	json_object_object_foreach(provides, role, roledata) {
		if(strcmp(role, "node")==0) {
			if(!modcfg->iface_node) {
				outlog_error("Unrealized role '%s' provided for module '%s' in library '%s'", role, modname, lib->name);
				return 1;
			}
			num_roles++;
			//process device connections
			json_object* subdata=NULL;
			if(json_object_object_get_ex(roledata, "device_connections", &subdata) && json_object_is_type(subdata, json_type_object)) {
				for(unsigned i=0;i<modcfg->iface_node->num_devices;i++) {
					const iot_deviceconn_filter_t *cfg=&modcfg->iface_node->devcfg[i];
					assert(cfg->label!=NULL);
					json_object* devcfg=NULL;
					if(!json_object_object_get_ex(subdata, cfg->label, &devcfg) || !json_object_is_type(devcfg,  json_type_object)) {
						outlog_error("No device connection '%s' of node module '%s' in library '%s' described in manifest", cfg->label, modname, lib->name);
						return 1;
					}
					if(cfg->flag_canauto) json_object_object_add(devcfg, "can_auto", json_object_new_boolean(1));
					if(cfg->flag_localonly) json_object_object_add(devcfg, "is_localonly", json_object_new_boolean(1));
					json_object* ifaces=json_object_new_array();
					json_object_object_add(devcfg, "interfaces", ifaces);
					for(unsigned j=0;j<cfg->num_devifaces;j++) {
						const iot_deviface_params* ifaceparams=cfg->devifaces[j];
						assert(ifaceparams!=NULL);
						if(!ifaceparams->get_id()) {
							outlog_error("Cannot find ID for device interface type '%s', check that dependencies for library '%s' are correct", ifaceparams->get_name(), lib->name);
							return 1;
						}
						json_object* json=NULL;
						if(int err=ifaceparams->to_json(json)) {
							outlog_error("Error converting interface parameters for device connection '%s' of node module '%s' in library '%s': %s", cfg->label, modname, lib->name, kapi_strerror(err));
							return 1;
						}
						json_object_array_add(ifaces, json);
					}
				}
			} else if(modcfg->iface_node->num_devices>0) {
				outlog_error("No device connections of node module '%s' in library '%s' described in manifest", modname, lib->name);
				return 1;
			}

			//process value inputs
			if(json_object_object_get_ex(roledata, "value_inputs", &subdata) && json_object_is_type(subdata, json_type_object)) {
				for(unsigned i=0;i<modcfg->iface_node->num_valueinputs;i++) {
					const iot_node_valuelinkcfg_t *cfg=&modcfg->iface_node->valueinput[i];
					assert(cfg->label!=NULL);
					json_object* cfgob=NULL;
					if(!json_object_object_get_ex(subdata, cfg->label, &cfgob) || !json_object_is_type(cfgob,  json_type_object)) {
						outlog_error("No value input '%s' of node module '%s' in library '%s' described in manifest", cfg->label, modname, lib->name);
						return 1;
					}
					if(cfg->notion_id) json_object_object_add(cfgob, "notion_id", json_object_new_int64(cfg->notion_id));
					json_object_object_add(cfgob, "datatype_id", json_object_new_int64(cfg->vclass_id));
				}
			} else if(modcfg->iface_node->num_valueinputs>0) {
				outlog_error("No value inputs of node module '%s' in library '%s' described in manifest", modname, lib->name);
				return 1;
			}

			//process value outputs
			if(json_object_object_get_ex(roledata, "value_outputs", &subdata) && json_object_is_type(subdata, json_type_object)) {
				for(unsigned i=0;i<modcfg->iface_node->num_valueoutputs;i++) {
					const iot_node_valuelinkcfg_t *cfg=&modcfg->iface_node->valueoutput[i];
					assert(cfg->label!=NULL);
					json_object* cfgob=NULL;
					if(!json_object_object_get_ex(subdata, cfg->label, &cfgob) || !json_object_is_type(cfgob,  json_type_object)) {
						outlog_error("No value output '%s' of node module '%s' in library '%s' described in manifest", cfg->label, modname, lib->name);
						return 1;
					}
					if(cfg->notion_id) json_object_object_add(cfgob, "notion_id", json_object_new_int64(cfg->notion_id));
					json_object_object_add(cfgob, "datatype_id", json_object_new_int64(cfg->vclass_id));
				}
			} else if(modcfg->iface_node->num_valueoutputs>0) {
				outlog_error("No value outputs of node module '%s' in library '%s' described in manifest", modname, lib->name);
				return 1;
			}


		} else if(strcmp(role, "driver")==0) {
			if(!modcfg->iface_device_driver) {
				outlog_error("Unrealized role '%s' provided for module '%s' in library '%s'", role, modname, lib->name);
				return 1;
			}
			num_roles++;
		} else if(strcmp(role, "detector")==0) {
			if(!modcfg->iface_device_detector) {
				outlog_error("Unrealized role '%s' provided for module '%s' in library '%s'", role, modname, lib->name);
				return 1;
			}
			num_roles++;
		} else {
			outlog_error("Invalid role '%s' provided for module '%s' in library '%s'", role, modname, lib->name);
			return 1;
		}
	}
	//check if all roles were described
	if(modcfg->iface_node) num_roles--;
	if(modcfg->iface_device_driver) num_roles--;
	if(modcfg->iface_device_detector) num_roles--;
	if(num_roles<0) {
		outlog_error("Not all roles of module '%s' in library '%s' were described in manifest", modname, lib->name);
		return 1;
	}
	return 0;
}

int proc_contype(const iot_hwdevcontype_metaclass* meta, json_object* data, json_object* regpart, const char* manifest_path) {
	iot_type_id_t id=meta->get_id();
	if(!id) { //must find or generate ID
		id=reg_ids.find_contype(meta->get_name());
		if(!id) {
			id=dev_ids.find_contype(meta->get_name());
			if(!id) {
				id=dev_ids.gen_new_dev_id(dev_ids.contypes);
				if(id) { //free ID found, remember it
					json_object_object_add(dev_ids.contypes, meta->get_name(), json_object_new_int64(id));
					dev_ids.is_mod=true;
					outlog_debug("New id %u for connection type %s was generated", id, meta->get_name());
				}
			}

		}
		if(id) {
			outlog_debug("Found id %u for connection type %s", id, meta->get_name());
		} else {
			outlog_error("No ID for connection type %s", meta->get_name());
			return 1;
		}
	} else { //id is built into declaration, so must be present in reg_ids
		if(reg_ids.find_contype(meta->get_name())!=id) {
			outlog_error("connection type '%s' has incorporated ID=%u which is not registered", meta->get_name(), id);
			return 1;
		}
	}
	json_object_object_add(data, "id", json_object_new_int64(id));
	char verbuf[32];
	iot_version_str(meta->get_version(), verbuf, sizeof(verbuf));
	json_object_object_add(data, "version", json_object_new_string(verbuf));

	char idbuf[16];
	snprintf(idbuf, sizeof(idbuf), "%u", id);
	json_object* arr=json_object_new_array();
	json_object_object_add(regpart, idbuf, arr);
	json_object_array_add(arr, json_object_new_string(meta->get_name()));
	json_object_array_add(arr, libraryref);
	json_object_array_add(arr, json_object_new_string(verbuf));
	return 0;
}

int proc_ifacetype(const iot_devifacetype_metaclass* meta, json_object* data, json_object* regpart, const char* manifest_path) {
	iot_type_id_t id=meta->get_id();
	if(!id) { //must find or generate ID
		id=reg_ids.find_ifacetype(meta->get_name());
		if(!id) {
			id=dev_ids.find_ifacetype(meta->get_name());
			if(!id) {
				id=dev_ids.gen_new_dev_id(dev_ids.ifacetypes);
				if(id) { //free ID found, remember it
					json_object_object_add(dev_ids.ifacetypes, meta->get_name(), json_object_new_int64(id));
					dev_ids.is_mod=true;
					outlog_debug("New id %u for iface type %s was generated", id, meta->get_name());
				}
			}

		}
		if(id) {
			outlog_debug("Found id %u for iface type %s", id, meta->get_name());
		} else {
			outlog_error("No ID for iface type %s", meta->get_name());
			return 1;
		}
	} else { //id is built into declaration, so must be present in reg_ids
		if(reg_ids.find_ifacetype(meta->get_name())!=id) {
			outlog_error("iface type '%s' has incorporated ID=%u which is not registered", meta->get_name(), id);
			return 1;
		}
	}
	json_object_object_add(data, "id", json_object_new_int64(id));
	char verbuf[32];
	iot_version_str(meta->get_version(), verbuf, sizeof(verbuf));
	json_object_object_add(data, "version", json_object_new_string(verbuf));

	char idbuf[16];
	snprintf(idbuf, sizeof(idbuf), "%u", id);
	json_object* arr=json_object_new_array();
	json_object_object_add(regpart, idbuf, arr);
	json_object_array_add(arr, json_object_new_string(meta->get_name()));
	json_object_array_add(arr, libraryref);
	json_object_array_add(arr, json_object_new_string(verbuf));
	return 0;
}

#define NUM_SECTS 4
const char* registry_sections[NUM_SECTS]={"libs","ifacetypes","contypes","modules"};


//Merges data from provided partpath into provided sections in reg_sect array (must have NUM_SECTS elems).
int merge_registry_part(json_object** reg_sect, const char* partpath) {
	json_object* json=iot_configregistry_t::read_jsonfile(NULL, partpath, "registry part");
	if(!json) return 1;
	for(int i=0;i<NUM_SECTS;i++) {
		json_object* src=NULL;
		if(!json_object_object_get_ex(json, registry_sections[i], &src) || !json_object_is_type(src, json_type_object)) continue;
		if(!reg_sect[i]) reg_sect[i]=json_object_new_object();
		json_object_object_foreach(src, key, val) {
			//check same key already exists in dest
			if(json_object_object_get_ex(reg_sect[i], key, NULL)) {
				outlog_error("Sub-property '%s.%s' has multiple values (second value is from '%s')", registry_sections[i], key, partpath);
				return 1;
			}
			json_object_object_add(reg_sect[i], key, json_object_get(val));
		}
	}
	json_object_put(json);
	return 0;
}

int build_registry(int argn, char **arg) {
	char registry_path[512];
	snprintf(registry_path, sizeof(registry_path), "%s/%s", bin_dir, REGISTRY_OUT);

	json_object* reg=json_object_new_object();

	json_object* reg_sect[NUM_SECTS]={};

	for(int argi=2; argi<argn; argi++) {
		if(merge_registry_part(reg_sect, arg[argi])) return 1;
	}

	for(int i=0;i<NUM_SECTS;i++) {
		if(reg_sect[i]) json_object_object_add(reg, registry_sections[i], reg_sect[i]);
	}

	const char *reg_json=json_object_to_json_string_ext(reg, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);

	//save registry part
	char errbuf[256];
	int fd_out=::open(registry_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if(fd_out<0) {
		outlog_error("Cannot open '%s': %s", registry_path, strerror_r(errno, errbuf, sizeof(errbuf)));
		return 1;
	}
	size_t result_len=strlen(reg_json);
	ftruncate(fd_out, 0);
	ssize_t write_res=write(fd_out, reg_json, result_len);
	if(write_res<0) {
		outlog_error("Cannot write '%s': %s", registry_path, strerror_r(errno, errbuf, sizeof(errbuf)));
		unlink(registry_path);
		return 1;
	}
	if(size_t(write_res)!=result_len) {
		outlog_error("Cannot write '%s' in full with %u bytes. Disk full?", registry_path, result_len);
		unlink(registry_path);
		return 1;
	}
	close(fd_out);
	fd_out=-1;
	return 0;
}

int main(int argn, char **arg) {

	if(!parse_args(argn, arg)) {
		return 1;
	}

#ifdef NDEBUG
	min_loglevel=LERROR;
#else
	min_loglevel=LDEBUG;
#endif

	if(argn<2 || !arg[1]) {
		outlog_error("Action REGISTRY with list of file paths to parts of registry OR library spec like 'vendor/dir/name' or 'CORE' required");
		return 1;
	}
	if(strcmp(arg[1],"REGISTRY")==0) { //must be followed by list of paths to all registry parts
		return build_registry(argn, arg);
	}
	char manifest_path[512];
	char manifestout_path[512];
	char registry_path[512];
	iot_regitem_lib_t* bundle;
	json_object* regpart=json_object_new_object();
	json_object* regpart_ifacetypes=NULL;
	json_object* regpart_contypes=NULL;
	json_object* regpart_modules=NULL;
	json_object* regpart_libs=json_object_new_object();
	json_object_object_add(regpart, "libs", regpart_libs);
	json_object* libprops=json_object_new_array();
	json_object* libdeps=NULL;

	char verbuf[32];

	libregistry->register_pending_metaclasses();
	uint32_t version;
	if(strcmp(arg[1],"CORE")==0) {
		snprintf(manifest_path, sizeof(manifest_path), "%s/%s", bin_dir, MANIFEST_IN);
		snprintf(manifestout_path, sizeof(manifestout_path), "%s/%s", bin_dir, MANIFEST_PART_OUT);
		snprintf(registry_path, sizeof(registry_path), "%s/%s", bin_dir, REGISTRY_PART_OUT);
		bundle=NULL;
		libraryref=json_object_new_string("CORE");
		json_object_object_add(regpart_libs, "CORE", libprops);
		version=iot_kernel_version;
		json_object_array_add(libprops, json_object_new_boolean(1));
	} else {
		bundle=libregistry->find_lib_item(arg[1]);
		if(!bundle) {
			outlog_error("No bundle with spec '%s' enabled for build", arg[1]);
			return 1;
		}
		int err=libregistry->load_lib(bundle);
		if(err) return 1;

		//process dependency libraries. build partial registry for them and process it for types with dynamic IDs
		json_object* tmpreg=json_object_new_object();
		json_object* tmpreg_sect[NUM_SECTS]={};

		//include CORE always
		snprintf(registry_path, sizeof(registry_path), "%s/%s", bin_dir, REGISTRY_PART_OUT);
		if(merge_registry_part(tmpreg_sect, registry_path)) return 1;
		//include other dependency libs
		if(argn>2) {
			libdeps=json_object_new_array();
			for(int argi=2; argi<argn; argi++) {
				json_object_array_add(libdeps, json_object_new_string(arg[argi]));

				snprintf(registry_path, sizeof(registry_path), "%s/%s/%s/%s", bin_dir, BUILD_MODULES_DIR, arg[argi], REGISTRY_PART_OUT);
				if(merge_registry_part(tmpreg_sect, registry_path)) return 1;
			}
		}
		for(int i=0;i<NUM_SECTS;i++) {
			if(tmpreg_sect[i]) json_object_object_add(tmpreg, registry_sections[i], tmpreg_sect[i]);
		}
		libregistry->apply_registry(tmpreg);
		json_object_put(tmpreg);

		libraryref=json_object_new_string(bundle->name);

		json_object_object_add(regpart_libs, bundle->name, libprops);
		version=bundle->version;
		json_object_array_add(libprops, json_object_new_boolean(bundle->linked ? 1 : 0));

		snprintf(manifest_path, sizeof(manifest_path), "%s/%s/%s/%s", bin_dir, BUILD_MODULES_DIR, bundle->name, MANIFEST_IN);
		snprintf(manifestout_path, sizeof(manifestout_path), "%s/%s/%s/%s", bin_dir, BUILD_MODULES_DIR, bundle->name, MANIFEST_PART_OUT);
		snprintf(registry_path, sizeof(registry_path), "%s/%s/%s/%s", bin_dir, BUILD_MODULES_DIR, bundle->name, REGISTRY_PART_OUT);
	}
	iot_version_str(version, verbuf, sizeof(verbuf));
	json_object_array_add(libprops, json_object_new_string(verbuf));
	json_object_array_add(libprops, libdeps);

	json_object* json=iot_configregistry_t::read_jsonfile(NULL, manifest_path, "manifest");
	if(!json) return 1;

	if(reg_ids.open()) return 1;
	if(dev_ids.open()) return 1;


	json_object_object_foreach(json, subkey, subval) {
		if(strcmp(subkey, "modules")==0 && bundle) {
			if(!json_object_is_type(subval, json_type_object)) {
				outlog_error("'%s' sub-field of manifest '%s' must be JSON object", subkey, manifest_path);
				return 1;
			}
			json_object_object_foreach(subval, modname, moddata) {
				iot_moduleconfig_t* modcfg=NULL;
				if(libregistry->find_module_config(modname, bundle, modcfg)) {
					outlog_error("Module '%s' mentioned in manifest '%s' not found", modname, manifest_path);
					return 1;
				}
				if(!regpart_modules) {
					regpart_modules=json_object_new_object();
				}
				if(proc_module(modname, modcfg, bundle, moddata, regpart_modules, manifest_path)) return 1;
			}
		} else if(strcmp(subkey, "ifacetypes")==0) {
			if(!json_object_is_type(subval, json_type_object)) {
				outlog_error("'%s' sub-field of manifest '%s' must be JSON object", subkey, manifest_path);
				return 1;
			}
			json_object_object_foreach(subval, ifacetypename, ifacetypedata) {
				const iot_devifacetype_metaclass* meta=libregistry->find_devifacetype(ifacetypename);
				if(!meta) {
					outlog_error("Device Interface Type '%s' mentioned in manifest '%s' not found", ifacetypename, manifest_path);
					return 1;
				}
				if(strcmp(meta->get_library(), arg[1])!=0) {
					outlog_error("Device Interface Type '%s' mentioned in manifest '%s' is not in '%s' library but in '%s'", ifacetypename, manifest_path, arg[1], meta->get_library());
					return 1;
				}
				if(!regpart_ifacetypes) {
					regpart_ifacetypes=json_object_new_object();
				}
				if(proc_ifacetype(meta, ifacetypedata, regpart_ifacetypes, manifest_path)) return 1;
			}
		} else if(strcmp(subkey, "contypes")==0) {
			if(!json_object_is_type(subval, json_type_object)) {
				outlog_error("'%s' sub-field of manifest '%s' must be JSON object", subkey, manifest_path);
				return 1;
			}
			json_object_object_foreach(subval, contypename, contypedata) {
				const iot_hwdevcontype_metaclass* meta=libregistry->find_devcontype(contypename);
				if(!meta) {
					outlog_error("Device Connection Type '%s' mentioned in manifest '%s' not found", contypename, manifest_path);
					return 1;
				}
				if(strcmp(meta->get_library(), arg[1])!=0) {
					outlog_error("Device Connection Type '%s' mentioned in manifest '%s' is not in '%s' library but in '%s'", contypename, manifest_path, arg[1], meta->get_library());
					return 1;
				}
				if(!regpart_contypes) {
					regpart_contypes=json_object_new_object();
				}
				if(proc_contype(meta, contypedata, regpart_contypes, manifest_path)) return 1;
			}
		} else {
			outlog_error("Unknown sub-field '%s' in manifest '%s'", subkey, manifest_path);
			return 1;
		}
	}

	json_object_object_add(json, "version", json_object_new_string(verbuf));

	if(regpart_ifacetypes) json_object_object_add(regpart, "ifacetypes", regpart_ifacetypes);
	if(regpart_contypes) json_object_object_add(regpart, "contypes", regpart_contypes);
	if(regpart_modules) json_object_object_add(regpart, "modules", regpart_modules);

	//OK. save resulting files
	const char *result_json=json_object_to_json_string_ext(json, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
	const char *regpart_json=json_object_to_json_string_ext(regpart, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
	if(!result_json || !regpart_json) {
		outlog_error("Cannot allocate JSON string");
		return 1;
	}
	char errbuf[256];
	//save full manifest
	int fd_out=::open(manifestout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if(fd_out<0) {
		outlog_error("Cannot open '%s': %s", manifestout_path, strerror_r(errno, errbuf, sizeof(errbuf)));
		return 1;
	}

	size_t result_len=strlen(result_json);
	ftruncate(fd_out, 0);
	ssize_t write_res=write(fd_out, result_json, result_len);
	if(write_res<0) {
		outlog_error("Cannot write '%s': %s", manifestout_path, strerror_r(errno, errbuf, sizeof(errbuf)));
		unlink(manifestout_path);
		return 1;
	}
	if(size_t(write_res)!=result_len) {
		outlog_error("Cannot write '%s' in full with %u bytes. Disk full?", manifestout_path, result_len);
		unlink(manifestout_path);
		return 1;
	}
	close(fd_out);
	fd_out=-1;

	//save registry part
	fd_out=::open(registry_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if(fd_out<0) {
		outlog_error("Cannot open '%s': %s", registry_path, strerror_r(errno, errbuf, sizeof(errbuf)));
		return 1;
	}
	result_len=strlen(regpart_json);
	ftruncate(fd_out, 0);
	write_res=write(fd_out, regpart_json, result_len);
	if(write_res<0) {
		outlog_error("Cannot write '%s': %s", registry_path, strerror_r(errno, errbuf, sizeof(errbuf)));
		unlink(registry_path);
		return 1;
	}
	if(size_t(write_res)!=result_len) {
		outlog_error("Cannot write '%s' in full with %u bytes. Disk full?", registry_path, result_len);
		unlink(registry_path);
		return 1;
	}
	close(fd_out);
	fd_out=-1;
	return 0;
}


