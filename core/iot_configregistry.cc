#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
//#include<time.h>

#include "iot_module.h"
#include "iot_utils.h"
#include "iot_daemonlib.h"
#include "iot_deviceregistry.h"
#include "iot_moduleregistry.h"
#include "iot_core.h"
#include "iot_configregistry.h"



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


json_object* iot_configregistry_t::read_jsonfile(const char* dir, const char* relpath, const char *name) {
	char namebuf[512];
	if(dir)
		snprintf(namebuf, sizeof(namebuf), "%s/%s", dir, relpath);
		else
		snprintf(namebuf, sizeof(namebuf), "%s", relpath);

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
		iot_hostid_t id=0;
		IOT_STRPARSE_UINT(host_id, iot_hostid_t, id);
		if(!id) {
			outlog_error("Invalid host id '%s' skipped in 'hostcfg.hosts' JSON-subobject of config", host_id);
			continue;
		}
		if(id==iot_current_hostid) continue;
		err=host_update(id, host);
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

	json_object *rules=NULL;
	json_object *nodes;
	json_object *links=NULL;
	if(json_object_object_get_ex(cfg, "nodecfg", &nodecfg) && json_object_is_type(nodecfg,  json_type_object) && 
					json_object_object_get_ex(nodecfg, "nodes", &nodes) && json_object_is_type(nodes,  json_type_object)) {
		if(json_object_object_get_ex(nodecfg, "rules", &rules))   //ignore rules if there are no nodes
			if(!json_object_is_type(rules,  json_type_object)) rules=NULL;

		if(json_object_object_get_ex(nodecfg, "links", &links))  //ignore links if there are no nodes
			if(!json_object_is_type(links,  json_type_object)) links=NULL;
	} else nodes=NULL;

	//iterate through rules
	rules_markdel();
	if(rules) {
		json_object_object_foreach(rules, rule_id, rule) {
			iot_id_t id=0;
			IOT_STRPARSE_UINT(rule_id, iot_id_t, id);
			if(!id) {
				outlog_error("Invalid rule id '%s' skipped in 'nodecfg.rules' JSON-subobject of config", rule_id);
				continue;
			}
			err=rule_update(id, rule);
			if(err) return err;
		}
	}

	//iterate through links
	links_markdel();
	if(links) {
		json_object_object_foreach(links, link_id, lnk) {
			iotlink_id_t id=0;
			IOT_STRPARSE_UINT(link_id, iotlink_id_t, id);
			if(!id) {
				outlog_error("Invalid link id '%s' skipped in 'nodecfg.links' JSON-subobject of config", link_id);
				continue;
			}
			err=link_update(id, lnk);
			if(err) return err;
		}
	}

	//iterate through nodes
	nodes_markdel();
	if(nodes) {
		json_object_object_foreach(nodes, node_id, node) {
			iot_id_t id=0;
			IOT_STRPARSE_UINT(node_id, iot_id_t, id);
			if(!id) {
				outlog_error("Invalid node id '%s' skipped in 'nodecfg.nodes' JSON-subobject of config", node_id);
				continue;
			}
			err=node_update(id, node);
			if(err) return err;
		}
	}

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
int iot_configregistry_t::rule_update(iot_id_t ruleid, json_object* obj) {
	json_object *val=NULL;
	iot_id_t group_id=0, mode_id=0;
	if(json_object_object_get_ex(obj, "group_id", &val)) IOT_JSONPARSE_UINT(val, iot_id_t, group_id)
	iot_config_item_group_t* group=NULL;
	if(group_id>0) group=group_find(group_id);
	if(!group || group->is_del) {
		outlog_error("Rule at config path nodecfg.rules.%" IOT_PRIiotid " refers to non-existing group %" IOT_PRIiotid ", skipping it", ruleid, group_id);
		return IOT_ERROR_NOT_FOUND;
	}

	if(json_object_object_get_ex(obj, "mode_id", &val)) IOT_JSONPARSE_UINT(val, iot_id_t, mode_id)

	if(mode_id>0 && mode_id!=group_id) { //check that mode matches group. mode can be always equal to group id to mean group's default mode
		uint8_t i;
		for(i=0; i<group->num_modes; i++) if(group->modes[i]==mode_id) break;
		if(i>=group->num_modes) {
			outlog_error("Rule at config path nodecfg.rules.%" IOT_PRIiotid " refers to illegal mode %" IOT_PRIiotid ", skipping it", ruleid, mode_id);
			return IOT_ERROR_NOT_FOUND;
		}
	}

	iot_config_item_rule_t** prule=NULL;
	iot_config_item_rule_t* rule=NULL;
	decltype(rules_index)::treepath path;
	int res=rules_index.find_add(ruleid, &prule, rule, &path);
	if(res==-4) return IOT_ERROR_NO_MEMORY;
	if(res<0) {
		assert(false);
		return IOT_ERROR_CRITICAL_BUG;
	}
	if(res==0) rule=*prule; //was found

	if(!rule) {
		size_t sz=sizeof(iot_config_item_rule_t); //+additional bytes
		rule=(iot_config_item_rule_t*)main_allocator.allocate(sz);
		if(!rule) {
			res=rules_index.remove(ruleid, NULL, &path);
			assert(res==1);
			return IOT_ERROR_NO_MEMORY;
		}
		memset(rule, 0, sz);
		rule->rule_id=ruleid;
		*prule=rule;
	} else {
		assert(rule->rule_id==ruleid);
		rule->is_del=0;
	}
	rule->group_item=group;
	rule->mode_id=mode_id;

	return 0;
}


//errors:
//	IOT_ERROR_NOT_FOUND - group_id not found
//	IOT_ERROR_CRITICAL_BUG - (in release mode only, in debug will assert) some error with tree index
//	IOT_ERROR_NO_MEMORY
int iot_configregistry_t::link_update(iotlink_id_t linkid, json_object* obj) {
	iot_id_t rule_id=0;
	IOT_JSONPARSE_UINT(obj, iot_id_t, rule_id);

	iot_config_item_rule_t* rule=NULL;
	if(rule_id>0 || errno) {
		if(rule_id>0) rule=rule_find(rule_id);

		if(!rule || rule->is_del) {
			outlog_error("Link at config path nodecfg.links.%" IOT_PRIiotlinkid " refers to non-existing rule '%s', skipping it", linkid, json_object_get_string(obj));
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
	lnk->rule=rule;

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

	iot_id_t rule_id=0;
	iot_config_item_rule_t* rule=NULL;

	if(json_object_object_get_ex(obj, "rule_id", &val)) {
		IOT_JSONPARSE_UINT(val, iot_id_t, rule_id)
		if(rule_id>0 || errno) {
			if(rule_id>0) rule=rule_find(rule_id);

			if(!rule || rule->is_del) {
				outlog_error("node at config path nodecfg.nodes.%" IOT_PRIiotid " refers to non-existing rule '%s', skipping it", nodeid, json_object_get_string(val));
				return IOT_ERROR_NOT_FOUND;
			}
		}
	}

	iot_hostid_t host_id=0;

	if(json_object_object_get_ex(obj, "host_id", &val)) IOT_JSONPARSE_UINT(val, iot_hostid_t, host_id)

	iot_config_item_host_t* host=NULL;
	if(host_id>0 && host_id!=IOT_HOSTID_ANY) host=host_find(host_id, true); //fo rnow di not increase refcount of host in nodes
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
		new(node) iot_config_item_node_t(nodeid);
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
				size_t sz=sizeof(iot_config_node_dev_t)+len*sizeof(iot_config_node_dev_t::idents[0]);
				curdev=(iot_config_node_dev_t*)main_allocator.allocate(sz, true);
				if(!curdev) {err=IOT_ERROR_NO_MEMORY; goto on_exit;}
				memset(curdev, 0, sz);
				strcpy(curdev->label, label);
				curdev->maxidents=len;
			} else curdev->numidents=0;
			
			for(int i=0;i<len;i++) {
				json_object* flt=json_object_array_get_idx(dev, i);
				if(!json_object_is_type(flt, json_type_object)) continue;

				if(iot_hwdev_ident_buffered::from_json(flt, &curdev->idents[curdev->numidents])) curdev->numidents++;
			}
			curdev->next=node->dev;
			node->dev=curdev;
		}
	}

	if(json_object_object_get_ex(obj, "inputs", &inputs) && json_object_is_type(inputs, json_type_object)) {
		json_object_object_foreach(inputs, label, links) {
			if(label[0]!='v' && label[0]!='m') continue; //label must have type prefix
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
				cur=new(cur) iot_config_node_in_t(node, label);
			} else {
				//disconnect old links
				while(cur->outs_head) {
					cur->outs_head->in=NULL;
					cur->outs_head->invalidate();
					cur->outs_head=cur->outs_head->next_output;
				}
			}
			//process list of link ids
			for(int i=0;i<len;i++) {
				iotlink_id_t link_id=0;
				IOT_JSONPARSE_UINT(json_object_array_get_idx(links, i), iotlink_id_t, link_id)
				iot_config_item_link_t* lnk=NULL;
				if(link_id>0) lnk=link_find(link_id);

				if(!lnk || lnk->is_del) {
					outlog_error("Node at config path nodecfg.nodes.%" IOT_PRIiotid ".inputs.%s refers to non-existing or illegal link %" IOT_PRIiotlinkid ", skipping it", nodeid, label, link_id);
					continue;
				}
				if(rule && lnk->rule!=rule) {
					outlog_error("Rule-bound node at config path nodecfg.nodes.%" IOT_PRIiotid ".inputs.%s refers to link %" IOT_PRIiotlinkid " from another rule, skipping link", nodeid, label, link_id);
					continue;
				}
				lnk->next_output=cur->outs_head;
				cur->outs_head=lnk;
				lnk->in=cur;
				lnk->invalidate();
			}
			cur->next=node->inputs;
			node->inputs=cur;
			cur->is_connected=false;
		}
	}
	//oldin can still contain unconnected or previously connected inputs, which has corrent index and thus exist in node instanciation, they must be preserved
	if(oldin) {
		iot_config_node_in_t* cur=oldin, *prev=NULL;
		while(cur) {
			if(cur->real_index>=0) {
				//found, remove from old list
				if(prev) prev->next=cur->next;
					else oldin=cur->next;
				//add to new list
				cur->next=node->inputs;
				node->inputs=cur;

				cur->is_connected=false;
				while(cur->outs_head) {
					cur->outs_head->in=NULL;
					cur->outs_head->invalidate();
					cur->outs_head=cur->outs_head->next_output;
				}

				cur=prev ? prev->next : oldin;
				continue;
			}
			prev=cur;
			cur=cur->next;
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
				cur=new(cur) iot_config_node_out_t(node, label);
			} else {
				//disconnect old links
				while(cur->ins_head) {
					cur->ins_head->out=NULL;
					cur->ins_head->invalidate();
					cur->ins_head=cur->ins_head->next_input;
				}
			}
			//process list of link ids
			for(int i=0;i<len;i++) {
				iotlink_id_t link_id=0;
				IOT_JSONPARSE_UINT(json_object_array_get_idx(links, i), iotlink_id_t, link_id)
				iot_config_item_link_t* lnk=NULL;
				if(link_id>0) lnk=link_find(link_id);

				if(!lnk || lnk->is_del) {
					outlog_error("Node at config path nodecfg.nodes.%" IOT_PRIiotid ".outputs.%s refers to non-existing or illegal link %" IOT_PRIiotlinkid ", skipping it", nodeid, label, link_id);
					continue;
				}
				if(rule && lnk->rule!=rule) {
					outlog_error("Rule-bound node at config path nodecfg.nodes.%" IOT_PRIiotid ".outputs.%s refers to link %" IOT_PRIiotlinkid " from another rule, skipping link", nodeid, label, link_id);
					continue;
				}

				lnk->next_input=cur->ins_head;
				cur->ins_head=lnk;
				lnk->out=cur;
				lnk->invalidate();
			}
			cur->next=node->outputs;
			node->outputs=cur;
			cur->is_connected=false;
		}
	}
	//oldout can still contain unconnected or previously connected outputs, which has corrent index and thus exist in node instanciation, they must be preserved
	if(oldout) {
		iot_config_node_out_t* cur=oldout, *prev=NULL;
		while(cur) {
			if(cur->real_index>=0) {
				//found, remove from old list
				if(prev) prev->next=cur->next;
					else oldout=cur->next;
				//add to new list
				cur->next=node->outputs;
				node->outputs=cur;

				cur->is_connected=false;
				while(cur->ins_head) {
					cur->ins_head->out=NULL;
					cur->ins_head->invalidate();
					cur->ins_head=cur->ins_head->next_input;
				}

				cur=prev ? prev->next : oldout;
				continue;
			}
			prev=cur;
			cur=cur->next;
		}
	}

	node->host=host;

	node->module_id=0;
	if(json_object_object_get_ex(obj, "module_id", &val)) IOT_JSONPARSE_UINT(val, uint32_t, node->module_id)

	node->rule_item=rule;

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
			oldin->outs_head->invalidate();
			oldin->outs_head=oldin->outs_head->next_output;
		}
		iot_release_memblock(oldin);
		oldin=next;
	}
	while(oldout) {
		iot_config_node_out_t* next=oldout->next;
		while(oldout->ins_head) {
			oldout->ins_head->out=NULL;
			oldout->ins_head->invalidate();
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
	for(; res==1; res=nodes_index.get_next(NULL, &pnode, path)) {
		if(!(*pnode)->is_del) continue;

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
				oldin->outs_head->invalidate();
				oldin->outs_head=oldin->outs_head->next_output;
			}
			if(oldin->is_value()) {
				if(oldin->current_value) {oldin->current_value->release();oldin->current_value=NULL;}
			} else {
				if(oldin->inject_msg) {oldin->inject_msg->release();oldin->inject_msg=NULL;}
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
				oldout->ins_head->invalidate();
				oldout->ins_head=oldout->ins_head->next_input;
			}
			if(oldout->current_value) {oldout->current_value->release();oldout->current_value=NULL;}
			if(oldout->prealloc_signal) iot_modelsignal::release(oldout->prealloc_signal); //auto nullified
			
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
		if(node->prealloc_execmsg) iot_release_msg(node->prealloc_execmsg); //auto nullified

		iot_release_memblock(node);
	}

	//clean links
	iot_config_item_link_t** plink=NULL;
	decltype(links_index)::treepath lpath;
	res=links_index.get_first(NULL, &plink, lpath);
	assert(res>=0);
	for(; res==1; res=links_index.get_next(NULL, &plink, lpath)) {
		if(!(*plink)->is_del) continue;

		iot_config_item_link_t *lnk=*plink;
		res=links_index.remove(lnk->link_id, NULL, &lpath);
		assert(res==1);

		assert(lnk->in==NULL && lnk->out==NULL);

		iot_release_memblock(lnk);
	}

	//clean groups
	iot_config_item_group_t* cur_group=groups_head;
	while(cur_group) {
		iot_config_item_group_t* next=cur_group->next;
		if(cur_group->is_del) {
			BILINKLIST_REMOVE_NOCL(cur_group, next, prev);
			iot_release_memblock(cur_group);
		}
		cur_group=next;
	}

	//clean hosts
	hosts_clean();
}

bool iot_config_item_node_t::prepare_execute(bool forceasync) { //must be called to preallocate memory before execute()
	//returns false on memory error, true on success BUT needexec and initial flags can be cleared
		assert(blockedby!=NULL);
		assert(needs_exec());

		//calculate necessary items
		uint16_t num_insignals=0;
		for(iot_config_node_in_t *in=inputs; in; in=in->next) { //loop by all inputs of node and calculate updated inputs
			if(!in->is_undelivered) continue; //real_index can be negative here but it can become valid during execute()
			if(in->is_msg()) { //msg
				if(in->inject_msg) num_insignals++;
				for(iot_config_item_link_t* link=in->outs_head; link; link=link->next_output) { //loop by all valid outputs connected to current input
					if(link->prev_msg) num_insignals++;
					if(link->current_msg) num_insignals++;
				}
			} else num_insignals++; //value
		}
		if(num_insignals==0) {
			assert(false);
			clear_needexec();
			if(is_initial()) clear_initial();
			return true;
		}

		if(is_sync()) { //for sync nodes preallocate space for output signals
			//prealloc signal structs for outputs
			for(iot_config_node_out_t *out=outputs; out; out=out->next) { //loop by outputs of node
				//real_index can be negative here but it can become valid during execute()
				//ensure there is preallocated signal struct
				if(!out->prealloc_signal) {
					out->prealloc_signal=(iot_modelsignal*)main_allocator.allocate(sizeof(iot_modelsignal));
					if(!out->prealloc_signal) return false;
					out->prealloc_signal=new(out->prealloc_signal) iot_modelsignal();
				}
			}
		}

		iot_notify_inputsupdate* notifyupdate;
		if(prealloc_execmsg && prealloc_execmsg->data) { //there is preallocated msg struct and iot_notify_inputsupdate object in it
			assert(prealloc_execmsg->is_releasable==1);
			notifyupdate=static_cast<iot_notify_inputsupdate*>((iot_releasable*)prealloc_execmsg->data);
			if(notifyupdate->numalloced<num_insignals) { //no enough space, release msg data (this will release iot_notify_inputsupdate object if it was memblock and releasedata in it)
				iot_release_msg(prealloc_execmsg, true);
				notifyupdate=NULL;
			} else {
				assert(notifyupdate->numitems==0 && !notifyupdate->prealloc_signals); //must be released
				notifyupdate->releasedata();
			}
		} else notifyupdate=NULL;

		if(!notifyupdate) {
			size_t sz=iot_notify_inputsupdate::calc_size(num_insignals);
			alignas(iot_notify_inputsupdate) char notifyupdatebuf[sz];
			notifyupdate=new(notifyupdatebuf) iot_notify_inputsupdate(num_insignals);

			int err=iot_prepare_msg_releasable(prealloc_execmsg, IOT_MSG_NOTIFY_INPUTSUPDATED, NULL, 0, notifyupdate, sz, IOT_THREADMSG_DATAMEM_TEMP, false, &main_allocator);
			//miid and bytearg MUST BE ASSIGNED correctly before sending msg
			if(err) {
				assert(err==IOT_ERROR_NO_MEMORY);
				return false;
			}
		}
		return true;
	}
void iot_config_item_node_t::execute(bool forceasync) {
		assert(blockedby!=NULL);
		assert(needs_exec());
		assert(prealloc_execmsg!=NULL && prealloc_execmsg->data!=NULL && prealloc_execmsg->is_releasable==1);

		clear_needexec();

		iot_notify_inputsupdate* notifyupdate=static_cast<iot_notify_inputsupdate*>((iot_releasable*)prealloc_execmsg->data);

		//FILL INPUT SIGNALS
		uint16_t num_insignals=0;
		for(iot_config_node_in_t *in=inputs; in; in=in->next) { //loop by all inputs of node and calculate updated inputs
			if(!in->is_undelivered) continue;
			in->is_undelivered=false;
			if(in->real_index<0) { //inputs without index cannot be sent to instance (or host)
				if(in->is_msg()) { //msgs must be dropped
					if(in->inject_msg) {in->inject_msg->release(); in->inject_msg=NULL;}
					for(iot_config_item_link_t* link=in->outs_head; link; link=link->next_output) { //loop by all valid outputs connected to current input
						if(!link->is_undelivered) continue;
						link->is_undelivered=false;
						if(link->prev_msg) {link->prev_msg->release(); link->prev_msg=NULL;}
						if(link->current_msg) {link->current_msg->release(); link->current_msg=NULL;}
					}
				}
				continue;
			}
			if(in->is_msg()) { //msg
				if(in->inject_msg) {
					assert(num_insignals<notifyupdate->numalloced);
					notifyupdate->item[num_insignals++]={in->real_index, true, in->inject_msg}; //incref unchanged because we MOVE value
					in->inject_msg=NULL;
				}
				for(iot_config_item_link_t* link=in->outs_head; link; link=link->next_output) { //loop by all valid outputs connected to current input
					if(!link->is_undelivered) continue;
					link->is_undelivered=false;
					if(link->prev_msg) {
						assert(num_insignals<notifyupdate->numalloced);
						notifyupdate->item[num_insignals++]={in->real_index, true, link->prev_msg};
						link->prev_msg=NULL; //incref unchanged because we MOVE value
					}
					if(link->current_msg) {
						assert(num_insignals<notifyupdate->numalloced);
						notifyupdate->item[num_insignals++]={in->real_index, true, link->current_msg};
						link->current_msg=NULL; //incref unchanged because we MOVE value
					}
				}
			} else {
				assert(num_insignals<notifyupdate->numalloced);
				notifyupdate->item[num_insignals++]={in->real_index, false, in->current_value}; //COPY value
				if(in->current_value) in->current_value->incref(); //incref
			}
		}
		if(num_insignals==0) { //no real inputs updated or no instance
			return;
		}
		notifyupdate->numitems=num_insignals;
		notifyupdate->reason_event=blockedby->id;

		if(is_sync()) {
			for(iot_config_node_out_t *out=outputs; out; out=out->next) { //loop by outputs of node
				if(out->real_index<0) continue; //outputs without index cannot be updated by instance
				assert(out->prealloc_signal!=NULL);

				ULINKLIST_INSERTHEAD(out->prealloc_signal, notifyupdate->prealloc_signals, next);
				out->prealloc_signal=NULL; //MOVE signal struct into notifyupdate
			}
		}

		if(!nodemodel) {
			if(host!=config_registry->current_host) {
				//TODO
				return;
			}
			//model still not created
			assert(false); //must have negative real_indexes for inputs and not be here
			return;
		}
		iot_modelsignal *signals=NULL; //will be updated in case of simple sync execution if there are any signals (outputs update)

		if(!is_sync()) {
			nodemodel->execute(true, prealloc_execmsg, signals);
		} else {
			if(!nodemodel->execute(false, prealloc_execmsg, signals)) {
				//node is not simple sync, so must wait
				set_waitexec(blockedby);
			} else { //simple sync
				if(signals) blockedby->add_signals(signals);
			}
		}
	}
