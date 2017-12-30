
#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include "iot_peerconnection.h"

#include "iot_configregistry.h"
#include "iot_moduleregistry.h"
#include "iot_threadregistry.h"

int iot_peers_registry_t::set_peer_connections(iot_peer* peer, json_object *json, bool prefer_ownthread) { //set list of connections by given JSON array with parameters for iot_netcon-derived classes
//IOT_ERROR_INVALID_THREAD
//IOT_ERROR_INVALID_ARGS
//IOT_ERROR_BAD_DATA
//IOT_ERROR_NO_MEMORY
//IOT_ERROR_LIMIT_REACHED
		if(uv_thread_self()!=main_thread) {
			assert(false);
			return IOT_ERROR_INVALID_THREAD;
		}
		if(!peer || peer->pregistry!=this) {
			assert(false);
			return IOT_ERROR_INVALID_ARGS;
		}

		if(!json_object_is_type(json, json_type_array)) {
			outlog_error("peer %" IOT_PRIhostid " connections config must be array of objects", peer->host_id);
			return IOT_ERROR_BAD_DATA;
		}

		if(!peer->protocfg) { //init iot protocol config on first request
			peer->protocfg=iot_objref_ptr<iot_netproto_config_iotgw>(true, new iot_netproto_config_iotgw(gwinst, peer, object_destroysub_delete, true));
			if(!peer->protocfg) return IOT_ERROR_NO_MEMORY;
		}
		int len=json_object_array_length(json);
		lock_datamutex();

		//remember existing connections in temporary array, whose items will be cleared if any new connection has same params as existing
		uint16_t numold=peer->cons_num;
		iot_netcon* oldcons[numold]={};
		if(numold>0) {
			uint16_t i=0;
			for(iot_netcon* curcon=peer->cons_head; curcon && i<numold; curcon=curcon->registry_next, i++) {
				oldcons[i]=curcon;
			}
		}
		int err=0;
		int num=0; //new number of connections
		for(int i=0;i<len;i++) {
			json_object* cfg=json_object_array_get_idx(json, i);
			if(!json_object_is_type(cfg, json_type_object)) {
				outlog_error("peer %" IOT_PRIhostid " connection config item must be an object, skipping '%s'", peer->host_id, json_object_to_json_string(cfg));
				continue;
			}
			iot_netcon* conobj=NULL;
			err=iot_netcontype_metaclass::from_json(cfg, peer->protocfg, conobj, false, this, prefer_ownthread); //on success will call pregistry->on_new_connection,
				//so new connection is already in cons_head and cons_num updated
			if(err) {
				if(err==IOT_ERROR_LIMIT_REACHED) break;
				continue;
			}
			assert(conobj!=NULL);
			//here new connection object is inited and can be used to check if there is another one with same params

			//first check against existing connections to know if one must be preserved
			for(uint16_t i=0;i<numold;i++) {
				if(!oldcons[i]) continue;
				int res;
				if((res=oldcons[i]->has_sameparams(conobj))) { //there is non-dying connection with same params. result 1 means everythis is same, result 2 means that metric is different
					if(res==2) oldcons[i]->metric=conobj->metric; //update metric
					oldcons[i]=NULL; //mark connection to be preserved
					//new connection is unnecessary
					conobj->stop();
					conobj=NULL;
					num++;
					break;
				}
			}
			if(!conobj) continue;
			//check agains full list of connections (existing and those being added) to exclude duplicates
			for(iot_netcon* curcon=peer->cons_head; curcon; curcon=curcon->registry_next) {
				if(curcon==conobj) continue;
				int res;
				if((res=curcon->has_sameparams(conobj))) { //there is non-dying connection with same params. result 1 means everythis is same, result 2 means that metric is different
					if(res==2) curcon->metric=conobj->metric; //update metric
					//new connection is unnecessary
					conobj->stop();
					conobj=NULL;
					break;
				}
			}
			if(!conobj) continue;

			//if(conobj->is_uv())
			conobj->start_uv(thread_registry->find_thread(main_thread)); //TODO
			num++;
		}
		unlock_datamutex();

		//stop unnecessary old connections
		for(uint16_t i=0;i<numold;i++) {
			if(!oldcons[i]) continue;
			oldcons[i]->stop();
		}

		if(num>0) return 0;
		if(err==IOT_ERROR_LIMIT_REACHED) return err; //no connection were created due to limit
		return 0;
	}

int iot_peers_registry_t::reset_peer_connections(iot_peer* peer) {
//IOT_ERROR_INVALID_THREAD
//IOT_ERROR_INVALID_ARGS
		if(uv_thread_self()!=main_thread) {
			assert(false);
			return IOT_ERROR_INVALID_THREAD;
		}
		if(!peer || peer->pregistry!=this) {
			assert(false);
			return IOT_ERROR_INVALID_ARGS;
		}

		lock_datamutex();
		for(iot_netcon* curcon=peer->cons_head; curcon; curcon=curcon->registry_next) {
			curcon->stop();
		}
		unlock_datamutex();
		return 0;
	}


int iot_peers_registry_t::add_listen_connections(json_object *json, bool prefer_ownthread) { //add several server (listening) connections by given JSON array with parameters for iot_netcon-derived classes
		assert(uv_thread_self()==main_thread);
		assert(cons!=NULL); //is inited

		if(!json_object_is_type(json, json_type_array)) {
			outlog_error("listening connections config must be array of objects");
			return IOT_ERROR_BAD_DATA;
		}
		int len=json_object_array_length(json);
		lock_datamutex();

		int err;
		for(int i=0;i<len;i++) {
			json_object* cfg=json_object_array_get_idx(json, i);
			if(!json_object_is_type(cfg, json_type_object)) {
				outlog_error("listen connection config item must be an object, skipping '%s'", json_object_to_json_string(cfg));
				continue;
			}
			iot_netcon* conobj=NULL;
			err=iot_netcontype_metaclass::from_json(cfg, listenprotocfg, conobj, true, this, prefer_ownthread);
			if(err) {
				if(err==IOT_ERROR_LIMIT_REACHED) break;
				continue;
			}
			assert(conobj!=NULL);

			//if(conobj->is_uv())
			conobj->start_uv(thread_registry->find_thread(main_thread)); //TODO
		}
		unlock_datamutex();
		return 0;
	}
