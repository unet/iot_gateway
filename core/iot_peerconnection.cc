
#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include "iot_peerconnection.h"

#include "iot_configregistry.h"
#include "iot_moduleregistry.h"
#include "iot_threadregistry.h"
#include "iot_netproto_iotgw.h"
#include "iot_netproto_mesh.h"


int iot_peer::on_new_iotsession(iot_netproto_session_iotgw *ses) { //called from any thread
		assert(ses!=NULL);
		assert(ses->peer_host==this);

		seslistlock.lock();

		BILINKLIST_INSERTHEAD(ses, iotsessions_head, peer_next, peer_prev);

		seslistlock.unlock();

		outlog_debug("New IOTGW session to host_id=%" IOT_PRIhostid " registered", host_id);
		return 0;
	}

void iot_peer::on_dead_iotsession(iot_netproto_session_iotgw *ses) { //called from any thread
		assert(ses!=NULL && ses->peer_host==this && ses->peer_prev!=NULL);

		seslistlock.lock();

		BILINKLIST_REMOVE(ses, peer_next, peer_prev);

		seslistlock.unlock();

		outlog_debug("IOTGW session to host_id=%" IOT_PRIhostid " un-registered", host_id);
	}

//resizes origroutes entries list to have space for at least n items
//returns 0 on success, error code on error:
//IOT_ERROR_NO_MEMORY
//IOT_ERROR_INVALID_ARGS
int iot_peer::resize_origroutes_buffer(uint32_t n, iot_memallocator* allocator) { //must be called under routing_lock
	if(maxorigroutes==n) return 0;
	if(!n || numorigroutes>n) return IOT_ERROR_INVALID_ARGS; //no enough space for existing entries

	//allocate new memory block
	size_t sz=sizeof(iot_meshorigroute_entry)*n;
	iot_meshorigroute_entry* newroutes=(iot_meshorigroute_entry*)allocator->allocate(sz, true);
	if(!newroutes) return IOT_ERROR_NO_MEMORY;
	if(numorigroutes<n) memset(newroutes+numorigroutes, 0, (n-numorigroutes)*sizeof(iot_meshorigroute_entry)); //zerofill empty entries after numroutes entries

	//copy existing entries and relink them 
	if(numorigroutes>0) {
		memmove(newroutes, origroutes, sizeof(iot_meshorigroute_entry)*numorigroutes);
		for(uint32_t i=0; i<numorigroutes; i++) { //relink head items to point to new allocated entries
			BILINKLIST_FIXHEAD(newroutes[i].routeslist_head, orig_prev);
		}
	}
	if(origroutes) iot_release_memblock(origroutes);
	origroutes=newroutes;
	maxorigroutes=n;
	return 0;
}



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
		if(!peer || peer->gwinst!=gwinst || !peer->prev) {
			assert(false);
			return IOT_ERROR_INVALID_ARGS;
		}

		if(!json_object_is_type(json, json_type_array)) {
			outlog_error("peer %" IOT_PRIhostid " connections config must be array of objects", peer->host_id);
			return IOT_ERROR_BAD_DATA;
		}
		int len=json_object_array_length(json);

		uv_mutex_lock(&datamutex);

		if(!peer->meshprotocfg) { //init mesh protocol config on first request
			peer->meshprotocfg=iot_objref_ptr<iot_netproto_config_mesh>(true, new iot_netproto_config_mesh(gwinst, peer, object_destroysub_delete, true, peer));
			if(!peer->meshprotocfg) return IOT_ERROR_NO_MEMORY;
//peer->meshprotocfg->debug=true;
		}

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
			err=iot_netcontype_metaclass::from_json(cfg, (iot_netproto_config_mesh*)(peer->meshprotocfg), conobj, false, this, prefer_ownthread); //on success will call pregistry->on_new_connection,
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
				if(int res=oldcons[i]->has_sameparams(conobj); res>0) { //there is non-dying connection with same params. result 1 means everythis is same, result 2 means that metric is different
					if(res==2) oldcons[i]->metric=conobj->metric; //update metric
					oldcons[i]=NULL; //mark connection to be preserved
					//new connection is unnecessary
					conobj->destroy();
					conobj=NULL;
					num++;
					break;
				}
			}
			if(!conobj) continue;
			//check agains full list of connections (existing and those being added) to exclude duplicates
			for(iot_netcon* curcon=peer->cons_head; curcon; curcon=curcon->registry_next) {
				if(curcon==conobj) continue;
				if(int res=curcon->has_sameparams(conobj); res>0) { //there is non-dying connection with same params. result 1 means everythis is same, result 2 means that metric is different
					if(res==2) curcon->metric=conobj->metric; //update metric
					//new connection is unnecessary
					conobj->destroy();
					conobj=NULL;
					break;
				}
			}
			if(!conobj) continue;

			if(conobj->is_uv()) {
//				iot_thread_item_t* thread=thread_registry->assign_thread(conobj->cpu_loading);
//				assert(thread!=NULL);
				conobj->start_uv(NULL);
			} else { //TODO for cases of non-uv
				assert(false);
				conobj->destroy();
				conobj=NULL;
				continue;
			}
			num++;
		}
		uv_mutex_unlock(&datamutex);

		//stop unnecessary old connections
		for(uint16_t i=0;i<numold;i++) {
			if(!oldcons[i]) continue;
			oldcons[i]->destroy();
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
		if(!peer || peer->gwinst!=gwinst || !peer->prev) {
			assert(false);
			return IOT_ERROR_INVALID_ARGS;
		}

		uv_mutex_lock(&datamutex);
		for(iot_netcon *cur, *next=peer->cons_head; (cur=next); ) {
			next=next->registry_next;
			cur->destroy();
		}
		uv_mutex_unlock(&datamutex);
		return 0;
	}


int iot_peers_registry_t::add_listen_connections(json_object *json, bool prefer_ownthread) { //add several server (listening) connections by given JSON array with parameters for iot_netcon-derived classes
		assert(uv_thread_self()==main_thread);
		assert(listenprotocfg!=NULL); //is inited

		if(!json_object_is_type(json, json_type_array)) {
			outlog_error("listening connections config must be array of objects");
			return IOT_ERROR_BAD_DATA;
		}
		int len=json_object_array_length(json);
		uv_mutex_lock(&datamutex);

		int err;
		for(int i=0;i<len;i++) {
			json_object* cfg=json_object_array_get_idx(json, i);
			if(!json_object_is_type(cfg, json_type_object)) {
				outlog_error("listen connection config item must be an object, skipping '%s'", json_object_to_json_string(cfg));
				continue;
			}
			iot_netcon* conobj=NULL;
			err=iot_netcontype_metaclass::from_json(cfg, (iot_netproto_config_mesh*)listenprotocfg, conobj, true, this, prefer_ownthread);
			if(err) {
				if(err==IOT_ERROR_LIMIT_REACHED) break;
				continue;
			}
			assert(conobj!=NULL);

			if(conobj->is_uv()) {
//				iot_thread_item_t* thread=thread_registry->assign_thread(conobj->cpu_loading);
//				assert(thread!=NULL);
				conobj->start_uv(NULL);
			} else { //TODO for cases of non-uv
				assert(false);
				conobj->destroy();
				continue;
			}

		}
		uv_mutex_unlock(&datamutex);
		return 0;
	}

void iot_peers_registry_t::graceful_shutdown(void) { //stop listening connections and reset all peer connections
	assert(uv_thread_self()==main_thread);
	assert(!is_shutdown);
	is_shutdown=true;

	if(iotgw_listencon) {
		iotgw_listencon->destroy();
		iotgw_listencon=NULL;
	}

	uv_rwlock_rdlock(&peers_lock);

	//reset all peer connections
	iot_peer* p=peers_head;
	while(p) {
		p->on_graceful_shutdown();
		p=p->next;
	}
	uv_rwlock_rdunlock(&peers_lock);

	uv_mutex_lock(&datamutex);

	for(iot_netcon *cur, *next=passive_cons_head; (cur=next); ) {
		next=next->registry_next;
		cur->destroy(true);
	}

	uv_mutex_unlock(&datamutex);
}
