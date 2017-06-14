#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
//#include<time.h>

#include<iot_module.h>
#include<iot_utils.h>
#include<kernel/iot_daemonlib.h>
#include<kernel/iot_deviceregistry.h>
#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_kernel.h>
#include<kernel/iot_configregistry.h>



iot_configregistry_t* config_registry=NULL;
static iot_configregistry_t _config_registry;

iot_hostid_t iot_current_hostid=0; //ID of current host in user config

/*
extern iot_config_item_link_t link;

iot_config_node_out_t outlink1={
	label : {'v','s','t','a','t','e'},
	ins_head : &link,
	real_index : -1
};

iot_config_item_node_t item1={ NULL, NULL, 
	size : sizeof(iot_config_item_node_t),
	iot_id : 1,
	host_id : 1,
	module_id : 3,
	mode_id : 0,
	group_item : NULL,
	cfg_id : 1,

	dev : NULL,
	inputs : NULL,
	outputs : &outlink1,
	json_config : NULL,

	config_ver: 0,
	numdevs : 0,
	numinputs : 0,
	numoutputs : 1,

	nodemodel : NULL
};

iot_config_node_in_t inlink1={
	label : {'v','i','n'},
	outs_head : &link,
	real_index : -1
};

iot_config_item_node_t item2={ NULL, NULL, 
	size : sizeof(iot_config_item_node_t),
	iot_id : 2,
	host_id : 1,
	module_id : 4,
	mode_id : 0,
	group_item : NULL,
	cfg_id : 1,

	dev : NULL,
	inputs : &inlink1,
	outputs : NULL,
	json_config : NULL,

	config_ver: 0,
	numdevs : 0,
	numinputs : 1,
	numoutputs : 0,

	nodemodel : NULL
};

iot_config_item_link_t link={
	next_input : NULL,
	next_output : NULL,
	in_node : &item2,
	out_node : &item1,
	in : &inlink1,
	out : &outlink1,

	mode_id : 0,
	group_item : NULL
};

*/
json_object* iot_configregistry_t::read_jsonfile(const char* relpath, const char *name) {
	char namebuf[256];
	snprintf(namebuf, sizeof(namebuf), "%s%s", rootpath, relpath);

	int fd=open(namebuf, O_RDONLY);
	if(fd<0) {
		outlog_error("Error opening %s file '%s': %s", name, namebuf, strerror(errno));
		return NULL;
	}

	json_tokener *tok=json_tokener_new_ex(16);
	if(!tok) {
		outlog_error("Lack of memory to start JSON parser for %s file", name);
		return NULL;
	}

	char buf[4096];
	json_object* obj=NULL;
	ssize_t len=0, dlen=0;
	do {
		len+=dlen;
		dlen=read(fd, buf, sizeof(buf));
		if(dlen<0) {
			outlog_error("Error reading %s file '%s': %s", name, namebuf, strerror(errno));
			close(fd);
			json_tokener_free(tok);
			return NULL;
		}
		if(!dlen) {len-=tok->char_offset;break;}
		obj=json_tokener_parse_ex(tok, buf, dlen);
	} while(!obj && json_tokener_get_error(tok) == json_tokener_continue);
	close(fd); fd=-1;
	if(!obj) {
		outlog_error("Error parsing %s file '%s': %s (at byte offset %ld)", name, namebuf,
				json_tokener_get_error(tok) == json_tokener_continue ?
						"unfinished JSON"
						: json_tokener_get_error(tok) == json_tokener_success ?
								"no top JSON-object found"
								: json_tokener_error_desc(json_tokener_get_error(tok)),
				long(len+tok->char_offset)
		);
		json_tokener_free(tok);
		return NULL;
	}
	json_tokener_free(tok); tok=NULL;

	if(!json_object_is_type(obj,  json_type_object)) {
		outlog_error("Invalid %s file '%s', it must have JSON-object as top element", name, namebuf);
		json_object_put(obj);
		return NULL;
	}

	return obj;
}

//IOT_ERROR_CRITICAL_ERROR - configuration is invalid
int iot_configregistry_t::load_hosts_config(json_object* cfg) { //main thread
	json_object *hostcfg=NULL;
	if(!json_object_object_get_ex(cfg, "hostcfg", &hostcfg) || !json_object_is_type(hostcfg,  json_type_object)) {
		outlog_error("No mandatory 'hostcfg' JSON-subobject found in config");
		return IOT_ERROR_CRITICAL_ERROR;
	}

	json_object *curhost=NULL;
	json_object *hosts=NULL;
	char curhoststr[22];
	snprintf(curhoststr, sizeof(curhoststr), "%" IOT_PRIhostid, iot_current_hostid);
	if(!json_object_object_get_ex(hostcfg, "hosts", &hosts) || !json_object_is_type(hosts,  json_type_object) || 
	 !json_object_object_get_ex(hosts, curhoststr, &curhost) || !json_object_is_type(curhost,  json_type_object)) {
		outlog_error("No mandatory 'hostcfg.hosts.%s' JSON-subobject with configuration of current host found in config");
		return IOT_ERROR_CRITICAL_ERROR;
	}
	hosts_markdel();

	int err;
	err=host_update(iot_current_hostid, curhost);
	if(err) return err;

	//iterate through hosts
	json_object_object_foreach(hosts, host_id, host) {
		uint64_t u64=iot_strtou64(host_id, NULL, 10);
		if(errno || !u64) {
			outlog_error("Invalid host id '%s' skipped in 'hostcfg.hosts' JSON-subobject of config", host_id);
			continue;
		}
		if(u64==iot_current_hostid) continue;
		err=host_update(u64, host);
		if(err) return err;
	}
	return 0;
}

//IOT_ERROR_CRITICAL_ERROR - configuration is invalid
int iot_configregistry_t::load_config(json_object* cfg, bool skiphosts) { //main thread
	int err;
	if(!skiphosts) {
		err=load_hosts_config(cfg);
		if(err) return err;
	}

//Process groups configuration
	json_object *modecfg=NULL;
	json_object *groups=NULL;
	if(json_object_object_get_ex(cfg, "modecfg", &modecfg) && json_object_is_type(modecfg,  json_type_object) && json_object_object_get_ex(modecfg, "groups", &groups)) {
		if(!json_object_is_type(groups,  json_type_object)) groups=NULL;
	}

	//iterate through groups
	groups_markdel();
	if(groups) {
		json_object_object_foreach(groups, group_id, group) {
			uint64_t u64=iot_strtou64(group_id, NULL, 10);
			if(errno || !u64 || u64>0xFFFFFFFFu) {
				outlog_error("Invalid group id '%s' skipped in 'modecfg.groups' JSON-subobject of config", group_id);
				continue;
			}
			err=group_update(iot_id_t(u64), group);
			if(err) return err;
		}
	}

//Process nodes configuration
	json_object *nodecfg=NULL;
	json_object *nodes=NULL;
	json_object *links=NULL;
	if(json_object_object_get_ex(cfg, "nodecfg", &nodecfg) && json_object_is_type(nodecfg,  json_type_object) && json_object_object_get_ex(nodecfg, "nodes", &nodes)) {
		if(!json_object_is_type(nodes,  json_type_object)) nodes=NULL;
		else if(json_object_object_get_ex(nodecfg, "links", &links)) {  //ignore links if there are no nodes
			if(!json_object_is_type(links,  json_type_object)) links=NULL;
		}
	}

	//iterate through links
	links_markdel();
	if(links) {
		json_object_object_foreach(links, link_id, lnk) {
			uint64_t u64=iot_strtou64(link_id, NULL, 10);
			if(errno || !u64 || u64>0xFFFFFFFFu) {
				outlog_error("Invalid link id '%s' skipped in 'nodecfg.links' JSON-subobject of config", link_id);
				continue;
			}
			err=link_update(iotlink_id_t(u64), lnk);
			if(err) return err;
		}
	}

	//iterate through nodes
	nodes_markdel();
	if(nodes) {
		json_object_object_foreach(nodes, node_id, node) {
			uint64_t u64=iot_strtou64(node_id, NULL, 10);
			if(errno || !u64 || u64>0xFFFFFFFFu) {
				outlog_error("Invalid node id '%s' skipped in 'nodecfg.nodes' JSON-subobject of config", node_id);
				continue;
			}
			err=node_update(iot_id_t(u64), node);
			if(err) return err;
		}
	}



//		iot_config_item_node_t* iitem=(iot_config_item_node_t*)main_allocator.allocate(sizeof(iot_config_item_node_t));
//		if(!iitem) return IOT_ERROR_NO_MEMORY;
//		BILINKLIST_INSERTHEAD(&item1, nodes_head, next, prev);
//		BILINKLIST_INSERTHEAD(&item2, nodes_head, next, prev);

		return 0;
}

int iot_configregistry_t::host_update(iot_hostid_t hostid, json_object* obj) {
	json_object *val=NULL;
	iot_config_item_host_t* host=host_find(hostid);
	if(!host) {
		size_t sz=sizeof(iot_config_item_host_t); //+additional bytes
		host=(iot_config_item_host_t*)main_allocator.allocate(sz, true);
		if(!host) return IOT_ERROR_NO_MEMORY;
		memset(host, 0, sz);
		host->host_id=hostid;

		BILINKLIST_INSERTHEAD(host, hosts_head, next, prev);
		if(hostid==iot_current_hostid) {
			assert(current_host==NULL);
			current_host=host;
		}
	} else host->is_del=0;

	uint32_t cfg_id=0;
	if(json_object_object_get_ex(obj, "cfg_id", &val)) IOT_JSONPARSE_UINT(val, uint32_t, cfg_id)

	if(host->cfg_id>=cfg_id) return 0;
	//update this data only if cfg_id was incremented
	host->cfg_id=cfg_id;

	host->listen_port=0;
	if(json_object_object_get_ex(obj, "listen_port", &val)) IOT_JSONPARSE_UINT(val, uint16_t, host->listen_port)

	return 0;
}

int iot_configregistry_t::group_update(iot_id_t groupid, json_object* obj) {
	json_object *val=NULL;
	time_t modes_modtime=0, active_set=0;
	iot_id_t active_mode=0;

	if(json_object_object_get_ex(obj, "modes_modtime", &val)) IOT_JSONPARSE_UINT(val, uint32_t, modes_modtime)
	if(json_object_object_get_ex(obj, "active_set", &val)) IOT_JSONPARSE_UINT(val, uint32_t, active_set)
	if(json_object_object_get_ex(obj, "active_mode", &val)) IOT_JSONPARSE_UINT(val, iot_id_t, active_mode)

	json_object *modes=NULL;
	uint8_t num_modes=0;
	if(json_object_object_get_ex(obj, "modes", &modes) && json_object_is_type(modes,  json_type_array)) {
		int len=json_object_array_length(modes);
		if(len>0) num_modes=uint8_t(len > IOT_CONFIG_MAX_MODES_PER_GROUP ? IOT_CONFIG_MAX_MODES_PER_GROUP : len);
	}

	iot_config_item_group_t* group=group_find(groupid);
	if(!group) {
		size_t sz=sizeof(iot_config_item_group_t); //necessary size
		group=(iot_config_item_group_t*)main_allocator.allocate(sz, true);
		if(!group) return IOT_ERROR_NO_MEMORY;
		memset(group, 0, sz);
		group->group_id=groupid;
		BILINKLIST_INSERTHEAD(group, groups_head, next, prev);
	} else {
		group->is_del=0;
		if(group->active_set==active_set && group->activemode_id==active_mode && group->modes_modtime==modes_modtime) return 0; //nothing changed
	}
	if(group->modes_modtime<modes_modtime) {
		group->num_modes=0;
		for(int i=0;i<num_modes; i++) {
			val=json_object_array_get_idx(modes, i);
			if(val && json_object_is_type(val, json_type_int)) IOT_JSONPARSE_UINT(val, iot_id_t, group->modes[group->num_modes++])
		}
		group->modes_modtime=modes_modtime;
	}
	if(group->active_set<active_set) {
		if(group->activemode_id!=active_mode) {
			//ADD EVENT ABOUT CHANGING MODE?
			group->activemode_id=active_mode;
		}
		group->active_set=active_set;
	}

	return 0;
}

//errors:
//	IOT_ERROR_NOT_FOUND - group_id not found
//	IOT_ERROR_CRITICAL_BUG - (in release mode only, in debug will assert) some error with tree index
//	IOT_ERROR_NO_MEMORY
int iot_configregistry_t::link_update(iotlink_id_t linkid, json_object* obj) {
	json_object *val=NULL;
	iot_id_t group_id=0, mode_id=0;
	if(json_object_object_get_ex(obj, "group_id", &val)) IOT_JSONPARSE_UINT(val, iot_id_t, group_id)
	iot_config_item_group_t* group=NULL;
	if(group_id>0) group=group_find(group_id);
	if(!group || group->is_del) {
		outlog_error("Link at config path nodecfg.links.%" IOT_PRIiotlinkid " refers to non-existing group %" IOT_PRIiotid ", skipping it", linkid, group_id);
		return IOT_ERROR_NOT_FOUND;
	}

	if(json_object_object_get_ex(obj, "mode_id", &val)) IOT_JSONPARSE_UINT(val, iot_id_t, mode_id)

	if(mode_id>0 && mode_id!=group_id) { //check that mode matches group. mode can be always equal to group id to mean group's default mode
		uint8_t i;
		for(i=0; i<group->num_modes; i++) if(group->modes[i]==mode_id) break;
		if(i>=group->num_modes) {
			outlog_error("Link at config path nodecfg.links.%" IOT_PRIiotlinkid " refers to illegal mode %" IOT_PRIiotid ", skipping it", linkid, mode_id);
			return IOT_ERROR_NOT_FOUND;
		}
	}

	iot_config_item_link_t** plnk=NULL;
	iot_config_item_link_t* lnk=NULL;
	decltype(links_index)::treepath path;
	int res=links_index.find_add(linkid, &plnk, lnk, &path);
	if(res==-4) return IOT_ERROR_NO_MEMORY;
	if(res<0) {
		assert(false);
		return IOT_ERROR_CRITICAL_BUG;
	}
	if(res==0) lnk=*plnk; //was found

	if(!lnk) {
		size_t sz=sizeof(iot_config_item_link_t); //+additional bytes
		lnk=(iot_config_item_link_t*)main_allocator.allocate(sz, true);
		if(!lnk) {
			res=links_index.remove(linkid, NULL, &path);
			assert(res==1);
			return IOT_ERROR_NO_MEMORY;
		}
		memset(lnk, 0, sz);
		lnk->link_id=linkid;
		*plnk=lnk;
	} else {
		assert(lnk->link_id==linkid);
		lnk->is_del=0;
	}
	lnk->group_item=group;
	lnk->mode_id=mode_id;

	return 0;
}

//errors:
//	IOT_ERROR_NOT_FOUND - group_id or host_id not found
//	IOT_ERROR_CRITICAL_BUG - (in release mode only, in debug will assert) some error with tree index
//	IOT_ERROR_NO_MEMORY
int iot_configregistry_t::node_update(iot_id_t nodeid, json_object* obj) {
	int err=0;
	iot_config_item_node_t** pnode=NULL;
	iot_config_item_node_t* node=NULL;
	decltype(nodes_index)::treepath path;
	int res=nodes_index.find_add(nodeid, &pnode, node, &path);
	if(res==-4) return IOT_ERROR_NO_MEMORY;
	if(res<0) {
		assert(false);
		return IOT_ERROR_CRITICAL_BUG;
	}
	if(res==0) node=*pnode; //was found

	json_object *val=NULL;
	uint32_t cfg_id=0;
	if(json_object_object_get_ex(obj, "cfg_id", &val)) IOT_JSONPARSE_UINT(val, uint32_t, cfg_id)

	if(node && node->cfg_id>=cfg_id) {node->is_del=false;return 0;} //not updated

	iot_id_t group_id=0;
	iot_hostid_t host_id=0;

	if(json_object_object_get_ex(obj, "group_id", &val)) IOT_JSONPARSE_UINT(val, iot_id_t, group_id)
	iot_config_item_group_t* group=NULL;
	if(group_id>0) {
		group=group_find(group_id);
		if(!group || group->is_del) {
			outlog_error("node at config path nodecfg.nodes.%" IOT_PRIiotid " refers to non-existing group %" IOT_PRIiotid ", skipping it", nodeid, group_id);
			return IOT_ERROR_NOT_FOUND;
		}
	}

	if(json_object_object_get_ex(obj, "host_id", &val)) IOT_JSONPARSE_UINT(val, iot_hostid_t, host_id)

	iot_config_item_host_t* host=NULL;
	if(host_id>0 && host_id!=IOT_HOSTID_ANY) host=host_find(host_id);
	if(!host || host->is_del) {
		outlog_error("node at config path nodecfg.nodes.%" IOT_PRIiotid " refers to non-existing host " IOT_PRIhostid ", skipping it", nodeid, host_id);
		return IOT_ERROR_NOT_FOUND;
	}

	if(!node) {
		size_t sz=sizeof(iot_config_item_node_t);
		node=(iot_config_item_node_t*)main_allocator.allocate(sz, true);
		if(!node) {
			res=nodes_index.remove(nodeid, NULL, &path);
			assert(res==1);
			return IOT_ERROR_NO_MEMORY;
		}
		memset(node, 0, sz);
		node->node_id=nodeid;
		*pnode=node;
	} else {
		assert(node->node_id==nodeid);
		node->is_del=0;
	}

	iot_config_node_dev_t *olddev=node->dev;
	iot_config_node_in_t *oldin=node->inputs;
	iot_config_node_out_t *oldout=node->outputs;
	node->dev=NULL;
	node->inputs=NULL;
	node->outputs=NULL;
	json_object *devices=NULL;
	json_object *inputs=NULL;
	json_object *outputs=NULL;
	if(json_object_object_get_ex(obj, "devices", &devices) && json_object_is_type(devices, json_type_object)) {
		json_object_object_foreach(devices, label, dev) {
			size_t llen=strlen(label);
			if(!json_object_is_type(dev, json_type_array) || llen>IOT_CONFIG_DEVLABEL_MAXLEN) continue;
			int len=json_object_array_length(dev);
			//try to find existing filter for same device connection
			iot_config_node_dev_t* curdev=olddev, *prevdev=NULL;
			while(curdev) {
				if(memcmp(curdev->label, label, llen+1)==0) {
					//found, remove from old list
					if(prevdev) prevdev->next=curdev->next;
						else olddev=curdev->next;
					break;
				}
				prevdev=curdev;
				curdev=curdev->next;
			}
			//here curdev is NULL or contains old device filter for same connection
			if(curdev && curdev->maxidents<len) { //not enough allocated space, reallocated item
				iot_release_memblock(curdev);
				curdev=NULL;
			}
			if(!curdev) {
				size_t sz=sizeof(iot_config_node_dev_t)+len*sizeof(iot_hwdev_ident_t);
				curdev=(iot_config_node_dev_t*)main_allocator.allocate(sz, true);
				if(!curdev) {err=IOT_ERROR_NO_MEMORY; goto on_exit;}
				memset(curdev, 0, sz);
				strcpy(curdev->label, label);
				curdev->maxidents=len;
			} else curdev->numidents=0;
			
			for(int i=0;i<len;i++) {
				json_object* flt=json_object_array_get_idx(dev, i);
				if(!json_object_is_type(flt, json_type_object)) continue;

				if(iot_hwdevident_iface::restore_from_json(flt, curdev->idents[curdev->numidents])) curdev->numidents++;
			}
			curdev->next=node->dev;
			node->dev=curdev;
		}
	}

	if(json_object_object_get_ex(obj, "inputs", &inputs) && json_object_is_type(inputs, json_type_object)) {
		json_object_object_foreach(inputs, label, links) {
			if(label[0]!='v' && label[0]!='m') continue; //label has type prefix
			size_t llen=strlen(label);
			if(!json_object_is_type(links, json_type_array) || llen>IOT_CONFIG_LINKLABEL_MAXLEN+1) continue;
			int len=json_object_array_length(links);
			//try to find existing input connection with same label
			iot_config_node_in_t* cur=oldin, *prev=NULL;
			while(cur) {
				if(memcmp(cur->label, label, llen+1)==0) {
					//found, remove from old list
					if(prev) prev->next=cur->next;
						else oldin=cur->next;
					break;
				}
				prev=cur;
				cur=cur->next;
			}
			//here cur is NULL or contains old connection with same label
			if(!cur) {
				size_t sz=sizeof(iot_config_node_in_t);
				cur=(iot_config_node_in_t*)main_allocator.allocate(sz, true);
				if(!cur) {err=IOT_ERROR_NO_MEMORY; goto on_exit;}
				memset(cur, 0, sz);
				strcpy(cur->label, label);
				cur->node=node;
				cur->real_index=-1;
			} else {
				//disconnect old links
				while(cur->outs_head) {
					cur->outs_head->in=NULL;
					cur->outs_head->is_valid=false;
					cur->outs_head=cur->outs_head->next_input;
				}
			}
			//process list of link ids
			for(int i=0;i<len;i++) {
				val=json_object_array_get_idx(links, i);
				errno=0;
				int64_t i64=json_object_get_int64(val);
				if(!errno && i64>0 && i64<=UINT32_MAX) {
					iotlink_id_t link_id=iotlink_id_t(i64);

					iot_config_item_link_t* lnk=link_find(link_id);
					if(!lnk || lnk->is_del) {
						outlog_error("Node at config path nodecfg.nodes.%" IOT_PRIiotid ".inputs.%s refers to non-existing link %" IOT_PRIiotlinkid ", skipping it", nodeid, label, link_id);
						continue;
					}
					lnk->next_input=cur->outs_head;
					cur->outs_head=lnk;
					lnk->in=cur;
					lnk->is_valid=false;
				}
			}
			cur->next=node->inputs;
			node->inputs=cur;
			cur->is_connected=false;
		}
	}
	if(json_object_object_get_ex(obj, "outputs", &outputs) && json_object_is_type(outputs, json_type_object)) {
		json_object_object_foreach(outputs, label, links) {
			if(label[0]!='v' && label[0]!='m') continue; //label has type prefix
			size_t llen=strlen(label);
			if(!json_object_is_type(links, json_type_array) || llen>IOT_CONFIG_LINKLABEL_MAXLEN+1) continue;
			int len=json_object_array_length(links);
			//try to find existing output connection with same label
			iot_config_node_out_t* cur=oldout, *prev=NULL;
			while(cur) {
				if(memcmp(cur->label, label, llen+1)==0) {
					//found, remove from old list
					if(prev) prev->next=cur->next;
						else oldout=cur->next;
					break;
				}
				prev=cur;
				cur=cur->next;
			}
			//here cur is NULL or contains old connection with same label
			if(!cur) {
				size_t sz=sizeof(iot_config_node_out_t);
				cur=(iot_config_node_out_t*)main_allocator.allocate(sz, true);
				if(!cur) {err=IOT_ERROR_NO_MEMORY; goto on_exit;}
				memset(cur, 0, sz);
				strcpy(cur->label, label);
				cur->node=node;
				cur->real_index=-1;
			} else {
				//disconnect old links
				while(cur->ins_head) {
					cur->ins_head->in=NULL;
					cur->ins_head->is_valid=false;
					cur->ins_head=cur->ins_head->next_output;
				}
			}
			//process list of link ids
			for(int i=0;i<len;i++) {
				val=json_object_array_get_idx(links, i);
				errno=0;
				int64_t i64=json_object_get_int64(val);
				if(!errno && i64>0 && i64<=UINT32_MAX) {
					iotlink_id_t link_id=iotlink_id_t(i64);

					iot_config_item_link_t* lnk=link_find(link_id);
					if(!lnk || lnk->is_del) {
						outlog_error("Node at config path nodecfg.nodes.%" IOT_PRIiotid ".outputs.%s refers to non-existing link %" IOT_PRIiotlinkid ", skipping it", nodeid, label, link_id);
						continue;
					}
					lnk->next_output=cur->ins_head;
					cur->ins_head=lnk;
					lnk->out=cur;
					lnk->is_valid=false;
				}
			}
			cur->next=node->outputs;
			node->outputs=cur;
			cur->is_connected=false;
		}
	}

	node->host=host;

	node->module_id=0;
	if(json_object_object_get_ex(obj, "module_id", &val)) IOT_JSONPARSE_UINT(val, uint32_t, node->module_id)

	node->mode_id=0;
	if(json_object_object_get_ex(obj, "mode_id", &val)) IOT_JSONPARSE_UINT(val, iot_id_t, node->mode_id)

	node->group_item=group;

	node->cfg_id=cfg_id;

	if(node->json_config) {
		json_object_put(node->json_config); //decrement ref counter
		node->json_config=NULL;
	}
	if(json_object_object_get_ex(obj, "params", &node->json_config)) {
		json_object_get(node->json_config); //increment ref counter
	}
	node->config_ver=0;
	if(json_object_object_get_ex(obj, "modulecfg_ver", &val)) IOT_JSONPARSE_UINT(val, uint8_t, node->config_ver)


on_exit:
	while(olddev) {
		iot_config_node_dev_t* nextdev=olddev->next;
		iot_release_memblock(olddev);
		olddev=nextdev;
	}
	while(oldin) {
		iot_config_node_in_t* next=oldin->next;
		while(oldin->outs_head) {
			oldin->outs_head->in=NULL;
			oldin->outs_head->is_valid=false;
			oldin->outs_head=oldin->outs_head->next_input;
		}
		iot_release_memblock(oldin);
		oldin=next;
	}
	while(oldout) {
		iot_config_node_out_t* next=oldout->next;
		while(oldout->ins_head) {
			oldout->ins_head->out=NULL;
			oldout->ins_head->is_valid=false;
			oldout->ins_head=oldout->ins_head->next_input;
		}
		iot_release_memblock(oldout);
		oldout=next;
	}

	return err;
}



void iot_configregistry_t::start_config(void) {
		if(inited) {
			assert(false);
			return;
		}
		inited=true;
		uv_check_init(main_loop, &events_executor);

		iot_config_item_node_t** node=NULL;
		decltype(nodes_index)::treepath path;
		int res=nodes_index.get_first(NULL, &node, path);
		assert(res>=0);
		while(res==1) {
			if((*node)->host==current_host) {//skip nodes of other hosts

				iot_nodemodel* model=iot_nodemodel::create((*node));
				if(!model) {
					outlog_error("Not enough memory to create configuration model, aborting");
					//TODO
					return;
				}
			}

			res=nodes_index.get_next(NULL, &node, path);
		}
		//create model links
}

void iot_configregistry_t::free_config(void) {
	hosts_markdel();
	groups_markdel();
	links_markdel();
	nodes_markdel();

	clean_config();

/*		iot_config_item_node_t* item, *nextitem=nodes_head;
		while((item=nextitem)) {
			nextitem=nextitem->next;

			if(item->nodemodel) {
				iot_nodemodel::destroy(item->nodemodel);
				item->nodemodel=NULL;
			}


			BILINKLIST_REMOVE(item, next, prev);
			//TODO
			//free item
		}
*/		
	}

void iot_configregistry_t::clean_config(void) { //free all items marked for deletion
	//clean nodes first
	iot_config_item_node_t** pnode=NULL;
	decltype(nodes_index)::treepath path;
	int res=nodes_index.get_first(NULL, &pnode, path);
	assert(res>=0);
	while(res==1) {
		if((*pnode)->is_del) {
			iot_config_item_node_t *node=*pnode;
			res=nodes_index.remove(node->node_id, NULL, &path);
			assert(res==1);

			//free device filters
			iot_config_node_dev_t *olddev=node->dev;
			node->dev=NULL;
			while(olddev) {
				iot_config_node_dev_t* nextdev=olddev->next;
				iot_release_memblock(olddev);
				olddev=nextdev;
			}

			//free input connectors and references to links
			iot_config_node_in_t *oldin=node->inputs;
			node->inputs=NULL;
			while(oldin) {
				iot_config_node_in_t* next=oldin->next;
				while(oldin->outs_head) {
					oldin->outs_head->in=NULL;
					oldin->outs_head->is_valid=false;
					oldin->outs_head=oldin->outs_head->next_input;
				}
				iot_release_memblock(oldin);
				oldin=next;
			}

			//free output connectors and references to links
			iot_config_node_out_t *oldout=node->outputs;
			node->outputs=NULL;
			while(oldout) {
				iot_config_node_out_t* next=oldout->next;
				while(oldout->ins_head) {
					oldout->ins_head->out=NULL;
					oldout->ins_head->is_valid=false;
					oldout->ins_head=oldout->ins_head->next_input;
				}
				iot_release_memblock(oldout);
				oldout=next;
			}

			if(node->nodemodel) {
				auto model=node->nodemodel;
				if(model->stop()) { //will clean node->nodemodel pointer!!
					iot_nodemodel::destroy(model);
				}
			}

			if(node->json_config) {
				json_object_put(node->json_config);
				node->json_config=NULL;
			}

			iot_release_memblock(node);
		}
		res=nodes_index.get_next(NULL, &pnode, path);
	}

	//clean links
	iot_config_item_link_t** plink=NULL;
	decltype(links_index)::treepath lpath;
	res=links_index.get_first(NULL, &plink, lpath);
	assert(res>=0);
	while(res==1) {
		if((*plink)->is_del) {
			iot_config_item_link_t *lnk=*plink;
			res=links_index.remove(lnk->link_id, NULL, &lpath);
			assert(res==1);

			assert(lnk->in==NULL && lnk->out==NULL);

			iot_release_memblock(lnk);
		}
		res=links_index.get_next(NULL, &plink, lpath);
	}

	//clean groups
	iot_config_item_group_t* cur_group=groups_head;
	while(cur_group) {
		iot_config_item_group_t* next=cur_group->next;
		if(cur_group->is_del) {
			iot_release_memblock(cur_group);
			if(cur_group==groups_head) groups_head=next;
		}
		cur_group=next;
	}

	//clean hosts
	iot_config_item_host_t* cur_host=hosts_head;
	while(cur_host) {
		iot_config_item_host_t* next=cur_host->next;
		if(cur_host->is_del) {
			iot_release_memblock(cur_host);
			if(cur_host==current_host) current_host=NULL;
			if(cur_host==hosts_head) hosts_head=next;
		}
		cur_host=next;
	}

}
