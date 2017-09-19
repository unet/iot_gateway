#ifndef IOT_PEERCONNECTION_H
#define IOT_PEERCONNECTION_H
//Contains constants, methods and data structures for managing connections to other gateways


#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include "iot_kapi.h"
#include "iot_common.h"

class iot_peer_link_t;
struct iot_remote_driverinst_item_t;
class iot_peers_registry_t;

//enum iot_peerreq {
//	IOT_PEERREQ_
//};



#include "iot_moduleregistry.h"
#include "iot_core.h"
#include "iot_configregistry.h"

struct iot_remote_driverinst_item_t {
	iot_remote_driverinst_item_t *next, *prev;

	iot_hwdev_localident* devident;
//	const iot_hwdevident_iface* ident_iface;
	dbllist_list<iot_device_entry_t, iot_mi_inputid_t, uint32_t, 1> retry_clients; //list of local client instances which can be retried later or blocked forever

	iot_deviface_params_buffered devifaces[IOT_CONFIG_MAX_IFACES_PER_DEVICE];
	iot_miid_t miid;
};

//represents data channel to another gateway
class iot_peer_link_t {
	iot_peer_link_t *next, *prev;
	
	iot_remote_driverinst_item_t* drivers_head; //head of list of driver instances available for connections
	enum {
		STATE_INIT, //initial state after creation
		STATE_INITIALCONNPEND, //first connection is pending
		STATE_CONNPEND, //connection is pending (initial sync was already finished)
		STATE_INITIALSYNCING, //first connection succeeded and data transfer in progress
		STATE_SYNCING, //data transfer in progress (initial sync was already finished)
		STATE_INSYNC, //data in sync (end-of-queue mark received from peer)
	} state;
	uint64_t state_time; //time (uv_now) when current state was set
	uint64_t last_sync;//time of previous full sync (time of first STATE_INSYNC)
	
public:
	const iot_hostid_t hostid;

	iot_peer_link_t(iot_hostid_t hostid) : drivers_head(NULL), state(STATE_INIT), last_sync(0), hostid(hostid) {
		state_time=uv_now(main_loop);
	}

	iot_remote_driverinst_item_t* get_avail_drivers_list(void) {
		return drivers_head;
	}
	bool is_failed(void) { //check if peer can be treated as dead
		if(state<STATE_INITIALSYNCING && uv_now(main_loop)-state_time>10000) return true;
		return false;
	}
	bool is_data_trusted(void) { //check if data is fresh enough and can be trusted
		if(state==STATE_INSYNC) return true;
//TODO		if(state==STATE_SYNCING) {
//			if(uv_now(main_loop)-last_sync<5000) return true; //we had full sync not long ago
//			return false; //wait for end of sync
//		}
		return false;
	}
};

extern iot_peers_registry_t *peers_registry;

class iot_peers_registry_t {
	iot_peer_link_t *peers_head;
	
public:
	iot_peers_registry_t(void) : peers_head(NULL) {
		assert(peers_registry==NULL);
		peers_registry=this;
	}
	iot_peer_link_t* find_peer_link(iot_hostid_t hostid) {
		iot_peer_link_t* link=peers_head;
		while(link) {
			if(link->hostid==hostid) return link;
		}
		return NULL;
	}
};


#endif //IOT_PEERCONNECTION_H
