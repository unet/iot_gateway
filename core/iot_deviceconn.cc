#include<stdint.h>
#include<assert.h>

#include "iot_deviceconn.h"

#include "iot_deviceregistry.h"
#include "iot_moduleregistry.h"
#include "iot_configregistry.h"
#include "iot_configmodel.h"
#include "iot_threadregistry.h"
#include "iot_peerconnection.h"


static iot_connsid_t last_connsid=0; //holds last assigned device CONNection Struct ID
static iot_device_connection_t iot_deviceconnections[IOT_MAX_DEVICECONNECTIONS]; //zero item is not used

iot_device_connection_t* iot_create_connection(iot_modinstance_item_t *client_inst, uint8_t idx) {
	assert(uv_thread_self()==main_thread);
	//find free index
	for(iot_connsid_t i=0;i<IOT_MAX_DEVICECONNECTIONS;i++) {
		last_connsid=(last_connsid+1)%IOT_MAX_DEVICECONNECTIONS;
		if(!last_connsid || iot_deviceconnections[last_connsid].connident) continue;

		iot_deviceconnections[last_connsid].init_local(last_connsid, client_inst, idx);

		return &iot_deviceconnections[last_connsid];
	}
	return NULL;
}

iot_device_connection_t* iot_find_device_conn(const iot_connid_t &connid) {
	if(!connid || connid.id>=IOT_MAX_DEVICECONNECTIONS || iot_deviceconnections[connid.id].connident!=connid) return NULL;
	return &iot_deviceconnections[connid.id];
}

int iot_deviface__DRVBASE::send_client_msg(const void *msg, uint32_t msgsize) const {
	if(!drvconn) return IOT_ERROR_INVALID_ARGS;
	iot_device_connection_t* conn=iot_find_device_conn(drvconn->id);
	if(!conn) return IOT_ERROR_NOT_FOUND;
	if(conn->driver_host!=iot_current_hostid || &conn->drvview!=drvconn) return IOT_ERROR_INVALID_ARGS;
	return conn->send_client_message(msg, msgsize);
}

int iot_deviface__DRVBASE::read_client_req(void* buf, uint32_t bufsize, uint32_t &dataread, uint32_t &szleft) const {
	if(!drvconn) return IOT_ERROR_INVALID_ARGS;
	iot_device_connection_t* conn=iot_find_device_conn(drvconn->id);
	if(!conn) return IOT_ERROR_NOT_FOUND;
	if(conn->driver_host!=iot_current_hostid || &conn->drvview!=drvconn) return IOT_ERROR_INVALID_ARGS;
	return conn->read_client_request(buf, bufsize, dataread, szleft);
}


int iot_deviface__CLBASE::send_driver_msg(const void *msg, uint32_t msgsize) const {
	if(!clconn) return IOT_ERROR_INVALID_ARGS;
	iot_device_connection_t* conn=iot_find_device_conn(clconn->id);
	if(!conn) return IOT_ERROR_NOT_FOUND;
	if(conn->client_host!=iot_current_hostid || &conn->clientview!=clconn) return IOT_ERROR_INVALID_ARGS;
	return conn->send_driver_message(msg, msgsize);
}

int32_t iot_deviface__CLBASE::start_driver_req(const void *data, uint32_t datasize, uint32_t fulldatasize) const {
	if(!clconn) return IOT_ERROR_INVALID_ARGS;
	iot_device_connection_t* conn=iot_find_device_conn(clconn->id);
	if(!conn) return IOT_ERROR_NOT_FOUND;
	if(conn->client_host!=iot_current_hostid || &conn->clientview!=clconn) return IOT_ERROR_INVALID_ARGS;
	return conn->start_driver_request(data, datasize, fulldatasize);
}

int32_t iot_deviface__CLBASE::continue_driver_req(const void *data, uint32_t datasize) const {
	if(!clconn) return IOT_ERROR_INVALID_ARGS;
	iot_device_connection_t* conn=iot_find_device_conn(clconn->id);
	if(!conn) return IOT_ERROR_NOT_FOUND;
	if(conn->client_host!=iot_current_hostid || &conn->clientview!=clconn) return IOT_ERROR_INVALID_ARGS;
	return conn->continue_driver_request(data, datasize);
}

int iot_device_driver_base::kapi_notify_write_avail(const iot_conn_drvview* conn_, bool enable) {
	if(!conn_) return IOT_ERROR_INVALID_ARGS;
	iot_modinstance_locker modinstlk=modules_registry->get_modinstance(miid);
	if(!modinstlk || modinstlk.modinst->instance!=this) {
		assert(false);
		return 0;
	}
	iot_device_connection_t* conn=iot_find_device_conn(conn_->id);
	if(!conn) return IOT_ERROR_NOT_FOUND;
	if(conn->driver_host!=iot_current_hostid || conn->driver.local.modinstlk.modinst->instance!=this) return IOT_ERROR_INVALID_ARGS;
	conn->client_write_avail_notify(enable);
	return 0;
}
int iot_driver_client_base::kapi_notify_write_avail(const iot_conn_clientview* conn_, bool enable) {
	if(!conn_) return IOT_ERROR_INVALID_ARGS;
	iot_modinstance_locker modinstlk=modules_registry->get_modinstance(miid);
	if(!modinstlk || modinstlk.modinst->instance!=this) {
		assert(false);
		return 0;
	}
	iot_device_connection_t* conn=iot_find_device_conn(conn_->id);
	if(!conn) return IOT_ERROR_NOT_FOUND;
	if(conn->client_host!=iot_current_hostid || conn->client.local.modinstlk.modinst->instance!=this) return IOT_ERROR_INVALID_ARGS;
	conn->driver_write_avail_notify(enable);
	return 0;
}


void iot_device_connection_t::init_local(iot_connsid_t id, iot_modinstance_item_t *client_inst, uint8_t idx) { //local client instance init. TODO: realize for remote
		assert(uv_thread_self()==main_thread);
		assert(!connident && id!=0);

		uint32_t prevconnkey=connident.key; //connkey must be preserved

		memset(this, 0, sizeof(*this));

		connident.key=prevconnkey+1;
		if(expect_false(connident.key==0)) connident.key=1; //do not allow zero value

		client_host=iot_current_hostid;
		if(!client.local.modinstlk.lock(client_inst)) { //instance must not be pending for release
			assert(false);
			return;
		}
		client.local.dev_idx=idx;

		drvview.client = {
			.hostid = iot_current_hostid,
			.module_id = client.local.modinstlk.modinst->module->dbitem->module_id,
			.miid = client.local.modinstlk.modinst->get_miid()
		};

		switch(client_inst->type) {
			case IOT_MODTYPE_NODE: {
				auto iface=static_cast<iot_node_module_item_t*>(client_inst->module)->config;
				assert(idx<IOT_CONFIG_MAX_NODE_DEVICES);
				client.local.conndata=&client_inst->data.node.dev[idx];
				client_devifaceclassfilter=&iface->devcfg[idx];
				if(client_inst->data.node.model->cfgitem->dev) { //find correct user device preference for current device connection matching labels
					auto cur=client_inst->data.node.model->cfgitem->dev;
					while(cur) {
						if(strcmp(cur->label, client_devifaceclassfilter->label)==0) {
							client_numhwdevidents=cur->numidents;
							client_hwdevidents=cur->idents;
							break;
						}
					}
				}
				break;
			}
			case IOT_MODTYPE_DRIVER:
			case IOT_MODTYPE_DETECTOR:
				//list illegal values
				assert(false);
				return;
		}

		assert(client.local.conndata->conn==NULL);
		client.local.conndata->conn=this;

		connident.id=id;

		state=IOT_DEVCONN_INIT;
	}

void iot_device_connection_t::deinit(void) {
	assert(uv_thread_self()==main_thread);
	assert(connident && state==IOT_DEVCONN_INIT);

	if(client_host==iot_current_hostid) {
		client.local.conndata->conn=NULL;
		client.local.modinstlk.unlock();
	} else {
		assert(client_host!=0);
		//TODO for remote client
		assert(false);
	}
	client_devifaceclassfilter=NULL;
	client_numhwdevidents=0;
	client_hwdevidents=NULL;
	memset(&client, 0, sizeof(client));
	client_host=0;

	if(clientclose_msg)  {iot_release_msg(clientclose_msg); clientclose_msg=NULL;}
	if(driverclose_msg)  {
		iot_release_msg(driverclose_msg);
		driverclose_msg=NULL;
	}
	if(driverstatus_msg)  {iot_release_msg(driverstatus_msg); driverstatus_msg=NULL;}
	if(c2d_ready_msg)  {iot_release_msg(c2d_ready_msg); c2d_ready_msg=NULL;}
	if(d2c_ready_msg)  {iot_release_msg(d2c_ready_msg); d2c_ready_msg=NULL;}
//	if(c2d_read_ready_msg)  {iot_release_msg(c2d_read_ready_msg); c2d_read_ready_msg=NULL;}
//	if(c2d_write_ready_msg) {iot_release_msg(c2d_write_ready_msg);c2d_write_ready_msg=NULL;}
//	if(d2c_read_ready_msg)  {iot_release_msg(d2c_read_ready_msg); d2c_read_ready_msg=NULL;}
//	if(d2c_write_ready_msg) {iot_release_msg(d2c_write_ready_msg);d2c_write_ready_msg=NULL;}
	if(connbuf) {iot_release_memblock(connbuf);connbuf=NULL;}

	connident.id=0;
}

//IOT_ERROR_NOT_READY - close is in progress
//IOT_ERROR_NO_MEMORY if asyncmsg was NULL and not main thread
int iot_device_connection_t::close(iot_threadmsg_t* asyncmsg) { //can be called from any thread
	//asyncmsg can be NULL in main thread or to request allocation
	iot_threadmsg_t *msg;
	if(uv_thread_self() != main_thread) {
		msg=asyncmsg;
		int err=iot_prepare_msg(msg, IOT_MSG_CLOSE_CONNECTION, NULL, 0, &connident, sizeof(connident), IOT_THREADMSG_DATAMEM_TEMP, true);
		if(err) {
			assert(!asyncmsg && err==IOT_ERROR_NO_MEMORY);
			if(asyncmsg) iot_release_msg(asyncmsg);
			return err;
		}
		main_thread_item->send_msg(msg);
		return IOT_ERROR_NOT_READY; //successful status for case with different threads
	} else if(asyncmsg) {
		iot_release_msg(asyncmsg);
		asyncmsg=NULL;
	}
	//main thread
	assert(connident);
	if(state==IOT_DEVCONN_PENDING) { //setup is in progress, so need lock
		lock();
		if(state==IOT_DEVCONN_PENDING) { //recheck that setup hasn't finished on driver
			connident.key++; //this will actually stop connection establishment in driver thread, so state can now be modified in main thread only
			if(expect_false(connident.key==0)) connident.key=1; //do not allow zero value
		}
		unlock();
	}
	iot_modinstance_item_t* clinst, * drvinst;
	int err;
	bool detach_pending=false;
	if(state==IOT_DEVCONN_READYDRV) {
		if(d2c.reader_closed && clientclose_msg!=NULL) { //unstable situation during connection setup, so need to lock and upgrade connident.key
			lock();
			assert(state==IOT_DEVCONN_READYDRV);
			if(d2c.reader_closed) { //recheck that setup hasn't finished on client
				connident.key++; //this will actually stop connection establishment in client thread, so d2c.reader_closed can now be modified in main thread only
				if(expect_false(connident.key==0)) connident.key=1; //do not allow zero value
			}
			unlock();
		}

		//check that client side is closed
		if(!d2c.reader_closed) { //client side not closed (client has attached connection)
			if(client_host==iot_current_hostid) {
				clinst=client.local.modinstlk.modinst;
				assert(clinst!=NULL);
				if(clinst->is_working()) { //client still active
					detach_pending=true; //mark that closing is delayed
					if(clientclose_msg) { //client not notified about close
						msg=clientclose_msg;
						err=iot_prepare_msg(msg, IOT_MSG_CONNECTION_CLOSECL, clinst, 0, &connident, sizeof(connident), IOT_THREADMSG_DATAMEM_TEMP, true);
						assert(!err);
						clientclose_msg=NULL;
						clinst->thread->send_msg(msg);
						//send msg to detach. it must set d2c.reader_closed=true
					}
				} else {
					d2c.reader_closed=true;
				}
			} else { //remote
				//TODO
				assert(false);
			}
		}

		//check that driver side is closed
		if(!c2d.reader_closed) { //driver side not closed (or not notified about close)
			if(driver_host==iot_current_hostid) {
				drvinst=driver.local.modinstlk.modinst;
				assert(drvinst!=NULL);
				if(drvinst->is_working()) { //driver still active, so connection must be detached
					detach_pending=true; //mark that closing is delayed
					if(driverclose_msg) { //driver not notified about close
						msg=driverclose_msg;
						err=iot_prepare_msg(msg, IOT_MSG_CONNECTION_CLOSEDRV, drvinst, 0, &connident, sizeof(connident), IOT_THREADMSG_DATAMEM_TEMP, true);
						assert(!err);
						driverclose_msg=NULL;
						drvinst->thread->send_msg(msg);
						//send msg to detach. it must set c2d.reader_closed=true
					}
				} else {
					c2d.reader_closed=true;
				}
			} else { //remote
				//TODO
				assert(false);
			}
		}
		if(detach_pending) return IOT_ERROR_NOT_READY; //both client and driver can be notified about connection close
	}


	if(state>IOT_DEVCONN_INIT) { //clean driver data
		if(driver_host==iot_current_hostid) {
			drvinst=driver.local.modinstlk.modinst;
			assert(drvinst!=NULL);

			drvinst->data.driver.conn[driver.local.conn_idx]=NULL;

			if(drvinst->is_working()) {
				if(state==IOT_DEVCONN_READYDRV && drvinst->data.driver.announce_connclose) { //connection was in established state
					drvinst->data.driver.retry_clients.removeby_value(0xFFFFFFFE); //unblock all clients waiting for free connection slots
					drvinst->data.driver.retry_clients.removeby_value(0xFFFFFFFD); //unblock all clients waiting for closed connection
					//check if it was the last one
					int i;
					for(i=0;i<IOT_MAX_DRIVER_CLIENTS;i++) if(drvinst->data.driver.conn[i]!=NULL) break;
					if(i>=IOT_MAX_DRIVER_CLIENTS) drvinst->data.driver.announce_connclose=0; //the last one was closed, reset flag
					//TODO: announce to all marked other hosts about closed connections and free slots
				} else if(drvinst->data.driver.announce_connfree_once) {
					drvinst->data.driver.announce_connfree_once=0;
					drvinst->data.driver.retry_clients.removeby_value(0xFFFFFFFE); //unblock all clients waiting for free connection slots
					//TODO: announce to all marked other hosts about free connection slots
				}
			}

			driver.local.modinstlk.unlock();

		} else {
			//TODO for remote
			assert(false);
		}
		memset(&driver, 0, sizeof(driver));
		memset(&deviface, 0, sizeof(deviface));
		driver_host=0;

		state=IOT_DEVCONN_INIT;
	}

	deinit();
	return 0;
}

int iot_device_connection_t::connect_remote(iot_miid_t& driver_inst) {//, const iot_devifacetype_id_t* ifaceclassids, uint8_t num_ifaceclassids) {
		return 0; //TODO
}

//tries to setup connection to specified local driver
//returns:
//	0 - success. connection
//	IOT_ERROR_NOT_READY - possible success. connection is tried to be set asynchronously. nothing should be for remote clients, on_drvconnect_status() must get real result to return to client
//	IOT_ERROR_TEMPORARY_ERROR - some temporary error (like no memory). for local client means that another try IS SCHEDULED later (so nothing to do).
//								for remote such status is just returned and must be transfered to client host AND RETRY BE SCHEDULED THERE
//	IOT_ERROR_NO_MEMORY
//	IOT_ERROR_NOT_SUPPORTED
//	IOT_ERROR_HARD_LIMIT_REACHED - for remote clients tells that retry must be blocked until driver frees connection slot (retry time is special value 0xFFFFFFFE on client side)
//								for local clients block will be already set (so can be used to break early loop through consumers)
//	IOT_ERROR_LIMIT_REACHED - for remote clients tells that retry must be blocked until driver closes any of its established connections (retry time is special value 0xFFFFFFFD on client side)
//								for local clients such error is not used
//////	IOT_ERROR_CRITICAL_ERROR - client instance cannot setup connection due to bad or absent client_hwdevident, so current device connection should be blocked until update
//								of client_hwdevident. connection struct should be released!!!

int iot_device_connection_t::connect_local(iot_modinstance_item_t* driver_inst) {
		assert(uv_thread_self()==main_thread);
		assert(state==IOT_DEVCONN_INIT);
		assert(driver_inst->type==IOT_MODTYPE_DRIVER);
		if(!driver_inst->is_working()) {
			assert(false);
			return IOT_ERROR_NOT_SUPPORTED;
		}
		uint32_t now32=uint32_t((uv_now(main_loop)+500)/1000);

//		if(!client_hwdevident && !client_devifaceclassfilter->flag_canauto)
//			return IOT_ERROR_CRITICAL_ERROR; //no hwdevice assigned by customer and autoassigned disabled by module
		assert(client_numhwdevidents || client_devifaceclassfilter->flag_canauto);


		//check if device matches user preference
		iot_hwdevregistry_item_t* hwdev=driver_inst->data.driver.hwdev;
		assert(hwdev!=NULL);
		if(client_numhwdevidents) { //there is hwdevice filter bound to client. it can be exact or a template.
			//DEVICE IN hwdev IS LOCAL, so no addtional check for client_devifaceclassfilter->flag_localonly
			int i;
			for(i=0;i<client_numhwdevidents;i++) if(client_hwdevidents[i].matches(&hwdev->dev_ident)) break;
			if(i>=client_numhwdevidents) return IOT_ERROR_NOT_SUPPORTED;
		}

		//check if driver provides any of interfaces requested by node module
		uint8_t i, j;
		int selected_iface=-1;
		int err;
		for(j=0; j<client_devifaceclassfilter->num_devifaces; j++) { //search requested device iface class among those supported by driver
			for(i=0;i<driver_inst->data.driver.num_devifaces;i++) {
				if(client_devifaceclassfilter->devifaces[j]->matches(driver_inst->data.driver.devifaces[i].data)) {
					selected_iface=i;
					break;
				}
			}
			if(selected_iface>=0) break; //found
		}
		if(selected_iface<0) {
			if(client_devifaceclassfilter->num_devifaces==0 && driver_inst->data.driver.num_devifaces>0) { //client accepts any iface, so take first iface of driver
				selected_iface=0;
				assert(driver_inst->data.driver.devifaces[0].is_valid());
			}
			else return IOT_ERROR_NOT_SUPPORTED;
		}

		if(!clientclose_msg) { //preallocate msg struct if necessary
			clientclose_msg=main_allocator.allocate_threadmsg(); //sets is_msgmemblock. other fields zeroed
			if(!clientclose_msg) goto on_no_mem;
		}
		if(!driverclose_msg) { //preallocate msg struct if necessary
			driverclose_msg=main_allocator.allocate_threadmsg(); //sets is_msgmemblock. other fields zeroed
			if(!driverclose_msg) goto on_no_mem;
		}
		if(!driverstatus_msg) { //preallocate msg struct if necessary
			driverstatus_msg=main_allocator.allocate_threadmsg(); //sets is_msgmemblock. other fields zeroed
			if(!driverstatus_msg) goto on_no_mem;
		}
		if(!c2d_ready_msg) { //preallocate msg struct if necessary
			c2d_ready_msg=main_allocator.allocate_threadmsg(); //sets is_msgmemblock. other fields zeroed
			if(!c2d_ready_msg) goto on_no_mem;
		}
		if(!d2c_ready_msg) { //preallocate msg struct if necessary
			d2c_ready_msg=main_allocator.allocate_threadmsg(); //sets is_msgmemblock. other fields zeroed
			if(!d2c_ready_msg) goto on_no_mem;
		}
/*		if(!c2d_read_ready_msg) { //preallocate msg struct if necessary
			c2d_read_ready_msg=main_allocator.allocate_threadmsg(); //sets is_msgmemblock. other fields zeroed
			if(!c2d_read_ready_msg) goto on_no_mem;
		}
		if(!d2c_read_ready_msg) { //preallocate msg struct if necessary
			d2c_read_ready_msg=main_allocator.allocate_threadmsg(); //sets is_msgmemblock. other fields zeroed
			if(!d2c_read_ready_msg) goto on_no_mem;
		}
		if(!c2d_write_ready_msg) { //preallocate msg struct if necessary
			c2d_write_ready_msg=main_allocator.allocate_threadmsg(); //sets is_msgmemblock. other fields zeroed
			if(!c2d_write_ready_msg) goto on_no_mem;
		}
		if(!d2c_write_ready_msg) { //preallocate msg struct if necessary
			d2c_write_ready_msg=main_allocator.allocate_threadmsg(); //sets is_msgmemblock. other fields zeroed
			if(!d2c_write_ready_msg) goto on_no_mem;
		}*/

		if(client_host==iot_current_hostid) { //always do blockage on local CLIENT SIDE (as no need to have it on both sides, connection is initiated on client side)
			client.local.blistnode=client.local.conndata->find_local_driver_retry_node(driver_inst);
			if(client.local.blistnode) {
				if(client.local.conndata->is_driver_blocked(client.local.blistnode, now32)) return IOT_ERROR_TEMPORARY_ERROR; //client-driver connection is still blocked
			} else {
				client.local.blistnode=client.local.conndata->create_local_driver_retry_node(driver_inst, iot_mi_inputid_t(client.local.modinstlk.modinst->get_miid(), client.local.dev_idx),
						now32+2*60, &main_allocator);
				if(!client.local.blistnode) goto on_no_mem;
			}
		}

		//check if driver can take more connections
		uint8_t conn_idx;
		for(conn_idx=0;conn_idx<IOT_MAX_DRIVER_CLIENTS;conn_idx++) if(!driver_inst->data.driver.conn[conn_idx]) break;
		if(conn_idx>=IOT_MAX_DRIVER_CLIENTS) {
			driver_inst->data.driver.announce_connfree_once=1; //set flag this driver should rescan consumers after closing any its connection
			if(client_host==iot_current_hostid) {
				client.local.conndata->block_driver(client.local.blistnode, 0xFFFFFFFE); //block until connection slot gets free
			} //for remote requests exact error core must be returned to make correct blocking and client't host marked that there are blocked clients on it
			return IOT_ERROR_HARD_LIMIT_REACHED; //for local clients such code can be used to break loop through clients
		}

		deviface=driver_inst->data.driver.devifaces[selected_iface];

		driver_host=iot_current_hostid;
		driver.local.modinstlk.lock(driver_inst);
		driver.local.conn_idx=conn_idx;
		driver_inst->data.driver.conn[conn_idx]=this;

		connident.key++;
		if(expect_false(connident.key==0)) connident.key=1; //do not allow zero value

		d2c.reader_closed=true;

		client_numhwdevidents=0;
		client_hwdevidents=NULL; //clear reference to iot_config_item_node_t

		state=IOT_DEVCONN_PENDING;

		if(client_host==iot_current_hostid) {
			client.local.conndata->block_driver(client.local.blistnode, now32+2*60); //block current client-driver pair until result
			client.local.modinstlk.modinst->recheck_job(true);
		}

		err=process_connect_local(false);
//		if(!err || err==IOT_ERROR_NOT_READY) return err;

		return on_drvconnect_status(err, false);
on_no_mem:

		if(client_host==iot_current_hostid) {
			//just set common retry period for client's connection line
			if(client.local.conndata->retry_drivers_timeout<now32+30) {
				client.local.conndata->retry_drivers_timeout=now32+30;
				client.local.modinstlk.modinst->recheck_job(true);
			} //else recheck already set on later time, so do nothing here
		}
		return IOT_ERROR_NO_MEMORY;
}

//called just after successful updating of connection state or in case of error
//returns:
//	0 - success. connection
//	IOT_ERROR_NOT_READY - possible success. connection is tried to be set asynchronously
//	IOT_ERROR_NO_MEMORY
//	IOT_ERROR_LIMIT_REACHED - only for remote client (local get IOT_ERROR_TEMPORARY_ERROR)
//	IOT_ERROR_TEMPORARY_ERROR - some temporary error. another try can be attempted later
//	IOT_ERROR_NOT_SUPPORTED
int iot_device_connection_t::on_drvconnect_status(int err, bool isasync) {
	assert(uv_thread_self()==main_thread);

	if(!err) {
		assert(state==IOT_DEVCONN_READYDRV);
		//reset retry period for local client
		if(client_host==iot_current_hostid) {
			if(client.local.blistnode) { //NOTE. retry period won't be reset if connection is closed before executing on_drvconnect_status(0, true) as call will be cancelled
				client.local.blistnode->remove();
			}
		}
		return 0;
	}
	if(err==IOT_ERROR_NOT_READY) {
		assert(state==IOT_DEVCONN_PENDING || state==IOT_DEVCONN_READYDRV); //driver could manage to finish connect and upgrade state, so allow state to be READYDRV
		return err;
	}
	assert(state==IOT_DEVCONN_PENDING);

	//downgrade to INIT state

	//no lock necessary as this is main thread and only it can move state downwards

	iot_modinstance_item_t *drvinst=driver.local.modinstlk.modinst;

	if(err==IOT_ERROR_LIMIT_REACHED) {
		drvinst->data.driver.announce_connclose=1; //set flag that close of established connection must restart search of clients
		if(client_host==iot_current_hostid) {
			if(client.local.blistnode) {
				client.local.conndata->block_driver(client.local.blistnode, 0xFFFFFFFD); //block until any established connection is closed
			}
			err=IOT_ERROR_TEMPORARY_ERROR; //in either sync local search (looping through drivers or through clients) cannot current IOT_ERROR_LIMIT_REACHED be used to stop loop, so do not return it
		} //for remote requests exact error core must be returned to make correct blocking and client't host marked that there are blocked clients on it
	}
	if(drvinst->data.driver.announce_connfree_once) {
		drvinst->data.driver.announce_connfree_once=0;
		drvinst->data.driver.retry_clients.removeby_value(0xFFFFFFFE); //unblock all clients waiting for free connection slots
		//TODO: announce to all marked other hosts about free connection slots
	}

	connident.key++;
	if(expect_false(connident.key==0)) connident.key=1; //do not allow zero value

	state=IOT_DEVCONN_INIT;
	drvinst->data.driver.conn[driver.local.conn_idx]=NULL;
	driver.local.modinstlk.unlock();
	client.local.blistnode=NULL;

	memset(&driver, 0, sizeof(driver));
	memset(&deviface, 0, sizeof(deviface));

	driver_host=0;

	if(err==IOT_ERROR_CRITICAL_BUG) { //must stop all module's driver instance
		if(drvinst->module->state!=IOT_MODULESTATE_DISABLED) {
			drvinst->module->state=IOT_MODULESTATE_DISABLED;
			static_cast<iot_driver_module_item_t*>(drvinst->module)->stop_all_drivers(true);
		}
		err=IOT_ERROR_NOT_SUPPORTED;
	}

	if(!isasync) return err;

	//restart connection search for local client
	if(client_host==iot_current_hostid) {
		modules_registry->try_connect_consumer_to_driver(client.local.modinstlk.modinst, client.local.dev_idx);
		return IOT_ERROR_NOT_READY;
	}
	//TODO notify remote client about error
	assert(false);
	return IOT_ERROR_NOT_READY;
}

//processes thread message to finish connect to started local driver instance (also to connect local driver end of inter-host connection)
//returns:
//	0 - success. connection
//	IOT_ERROR_NOT_READY - possible success. connection is tried to be set asynchronously
//	IOT_ERROR_NO_MEMORY
/////	IOT_ERROR_ACTION_CANCELLED
//	IOT_ERROR_TEMPORARY_ERROR - some temporary error. another try can be attempted later
//	IOT_ERROR_NOT_SUPPORTED
int iot_device_connection_t::process_connect_local(bool isasync) { //called in working thread of driver instance with acquired acclock or in main thead
	//isasync show if call was made while processing IOT_MSG_OPEN_CONNECTION message
	assert(state==IOT_DEVCONN_PENDING && driver_host==iot_current_hostid);

	iot_modinstance_item_t *drvinst=driver.local.modinstlk.modinst;
	assert(drvinst!=NULL && drvinst->type==IOT_MODTYPE_DRIVER);

	int err;
	iot_threadmsg_t *msg;

	if(!isasync) { //initial call from main thread, from connect_local()
		assert(uv_thread_self()==main_thread);

		if(main_thread != drvinst->thread->thread) { //not working thread of driver instance, so must be main thread. async start
			msg=driverstatus_msg;
			assert(msg!=NULL);
			err=iot_prepare_msg(msg, IOT_MSG_DRVOPEN_CONNECTION, NULL, 0, &connident, sizeof(connident), IOT_THREADMSG_DATAMEM_TEMP, true);
			if(err) {
				assert(false);
				return IOT_ERROR_TEMPORARY_ERROR;
			}

			//send signal to start
			driverstatus_msg=NULL;
			drvinst->thread->send_msg(msg);
			return IOT_ERROR_NOT_READY; //successful status for case with different threads
		}
	} else {
		assert(uv_thread_self()==drvinst->thread->thread);
	}
	//this is working thread of driver modinstance

	char *buf=NULL;

	if(!drvinst->is_working()) { //driver instance is not started or being stopped
		err=IOT_ERROR_TEMPORARY_ERROR;
		goto onexit;
	}

	drvview.id=connident;
	drvview.index=driver.local.conn_idx;
	drvview.deviface = deviface.data;

	//allocate buffers
	uint32_t c2d_bufsize, d2c_bufsize;
	uint8_t c2d_p, d2c_p; //power of 2 for buffer size

	c2d_bufsize=deviface.data->get_c2d_maxmsgsize()*2;
	if(c2d_bufsize<IOT_MEMOBJECT_MAXPLAINSIZE/2) c2d_bufsize=IOT_MEMOBJECT_MAXPLAINSIZE/2;

	//find smallest power of 2 which is >= c2d_bufsize but no more than 1MB
	for(c2d_p=12;c2d_p<21;c2d_p++) {
		if((uint32_t(1)<<c2d_p)>=c2d_bufsize) break;
	}
//	if(c2d_p>=21) {
//		err=IOT_ERROR_NO_MEMORY;
//		goto onexit;
//	}
	c2d_bufsize=1<<c2d_p;


	d2c_bufsize=deviface.data->get_d2c_maxmsgsize()*2;
	if(d2c_bufsize<IOT_MEMOBJECT_MAXPLAINSIZE/2) d2c_bufsize=IOT_MEMOBJECT_MAXPLAINSIZE/2;

	//find smallest power of 2 which is >= d2c_bufsize but no more than 1MB
	for(d2c_p=12;d2c_p<21;d2c_p++) {
		if((uint32_t(1)<<d2c_p)>=d2c_bufsize) break;
	}
//	if(d2c_p>=21) {
//		err=IOT_ERROR_NO_MEMORY;
//		goto onexit;
//	}
	d2c_bufsize=1<<d2c_p;

	buf=(char*)drvinst->thread->allocator->allocate(c2d_bufsize+d2c_bufsize, true);
	if(!buf) {
		err=IOT_ERROR_NO_MEMORY;
		goto onexit;
	}
	if(!c2d.buf.setbuf(c2d_p, buf)) {
		assert(false);
		err=IOT_ERROR_TEMPORARY_ERROR;
		goto onexit;
	}
	if(!d2c.buf.setbuf(d2c_p, buf+c2d_bufsize)) {
		assert(false);
		err=IOT_ERROR_TEMPORARY_ERROR;
		goto onexit;
	}
	c2d.got_writespace=true;
	d2c.got_writespace=true;

	err=static_cast<iot_device_driver_base*>(drvinst->instance)->device_open(&drvview);
	if(err) {
		auto module=drvinst->module;
		if(err==IOT_ERROR_CRITICAL_BUG) {
			outlog_error("Critical bug opening connection to driver module '%s::%s' with ID %u. Module will be blocked.", module->dbitem->bundle->name, module->dbitem->module_name, module->dbitem->module_id);
		}
		else if(err!=IOT_ERROR_NOT_SUPPORTED && err!=IOT_ERROR_TEMPORARY_ERROR && err!=IOT_ERROR_LIMIT_REACHED) {
			outlog_error("Illegal error opening connection to driver module '%s::%s' with ID %u: %s. This is buf. Module will be blocked.", module->dbitem->bundle->name, module->dbitem->module_name, module->dbitem->module_id, kapi_strerror(err));
			err=IOT_ERROR_CRITICAL_BUG;
		}
		goto onexit;
	}
	//no failure after this point, or device_close() should be called

	state=IOT_DEVCONN_READYDRV;

	//success always async
//	msg=c2d_write_ready_msg;
	msg=c2d_ready_msg; //use this msg struct as it should not be used until client receives next msg (client uses this struct to notify driver about new msg or free space)
	assert(msg!=NULL);
	if(client_host==iot_current_hostid) {
		err=iot_prepare_msg(msg, IOT_MSG_CONNECTION_DRVREADY, NULL, 0, &connident, sizeof(connident), IOT_THREADMSG_DATAMEM_TEMP, true);
		assert(err==0);
//		c2d_write_ready_msg=NULL;
		c2d_ready_msg=NULL;
		client.local.modinstlk.modinst->thread->send_msg(msg);
	} else {
		//TODO prepare msg over host connection
		assert(false);
//		c2d_write_ready_msg=NULL;
//		c2d_ready_msg=NULL;
		//TODO send msg over host connection
		assert(false);
	}


	err=0;
	connbuf=buf;
onexit:

	//roll back update of connection data
	if(err && buf) {
		c2d.buf.init();
		d2c.buf.init();
		iot_release_memblock(buf);
	}

	if(isasync) {
		msg=driverstatus_msg;
		assert(msg!=NULL);
		int err2=iot_prepare_msg(msg, IOT_MSG_CONNECTION_DRVOPENSTATUS, NULL, 0, &connident, sizeof(connident), IOT_THREADMSG_DATAMEM_TEMP, true);
		assert(err2==0);
		assert(msg->datasize<=IOT_MSG_INTARG_SAFEDATASIZE);
		msg->intarg=err;

		driverstatus_msg=NULL;
		main_thread_item->send_msg(msg);
		return IOT_ERROR_NOT_READY;
	}
	return err;
}


void iot_device_connection_t::process_driver_ready(void) { //runs in client thread after driver finished connection
	assert(state==IOT_DEVCONN_READYDRV && client_host==iot_current_hostid);

	iot_modinstance_item_t *modinst=client.local.modinstlk.modinst;
	assert(modinst!=NULL && modinst->type!=IOT_MODTYPE_DRIVER);

	assert(uv_thread_self()==modinst->thread->thread);

	if(driver_host==iot_current_hostid) {
		clientview={
			.id = connident,
			.index = client.local.dev_idx,
			.deviface = deviface.data,
			.driver = {
				.hostid = iot_current_hostid,
				.module_id = driver.local.modinstlk.modinst->module->dbitem->module_id,
				.miid = driver.local.modinstlk.modinst->get_miid()
			}
		};
	} else {
		clientview={
			.id = connident,
			.index = client.local.dev_idx,
			.deviface = deviface.data,
			.driver = {
				.hostid = driver_host,
				.module_id = driver.remote.module_id,
				.miid = driver.remote.miid
			}
		};
	}
	switch(modinst->type) {
		case IOT_MODTYPE_NODE: {
			outlog_debug("Device input %d of node id=%" IOT_PRIiotid " attached to driver instance %u", 
				int(clientview.index), modinst->data.node.model->node_id, unsigned(clientview.driver.miid.iid));
			static_cast<iot_node_base*>(modinst->instance)->device_attached(&clientview);
			break;
		}
		case IOT_MODTYPE_DRIVER: //list all illegal types
		case IOT_MODTYPE_DETECTOR:
			assert(false);
			return;
	}

//	state=IOT_DEVCONN_FULLREADY;
	d2c.reader_closed=false;

	if((c2d.want_write && c2d.buf.avail_write()>0) || d2c.buf.pending_read()>0 || d2c.want_write) { //this check will always see if message from driver was sent in its device_open because IOT_MSG_CONNECTION_DRVREADY
		//is sent after device_open(). driver writes after changing state to IOT_DEVCONN_READYDRV will always put IOT_MSG_CONNECTION_D2C_READREADY to client
		//queue after current IOT_MSG_CONNECTION_DRVREADY
		on_d2c_ready(); //this will process all currently visible reads, so some IOT_MSG_CONNECTION_D2C_READREADY msg which can be now in fly
							//will just get empty processing, and this is OK
	}
}

void iot_device_connection_t::process_close_client(iot_threadmsg_t* msg) { //client instance  thread. instance must be working!!!
	assert(client_host==iot_current_hostid);
	assert(state==IOT_DEVCONN_READYDRV);

	iot_modinstance_item_t *modinst=client.local.modinstlk.modinst;
	assert(modinst!=NULL && modinst->is_working());
	assert(uv_thread_self()==modinst->thread->thread);

	assert(!d2c.reader_closed);

	switch(modinst->type) {
		case IOT_MODTYPE_NODE: {
			outlog_debug("Device input %d of node module %u detached", int(clientview.index), modinst->module->dbitem->module_id);
			static_cast<iot_node_base*>(modinst->instance)->device_detached(&clientview);
			break;
		}
		case IOT_MODTYPE_DRIVER: //list all illegal types
		case IOT_MODTYPE_DETECTOR:
			assert(false);
			iot_release_msg(msg);
			return;
	}

	d2c.reader_closed=true;
	close(msg);
}
void iot_device_connection_t::process_close_driver(iot_threadmsg_t* msg) { //driver instance  thread. instance must be working!!!
	assert(driver_host==iot_current_hostid);
	assert(state==IOT_DEVCONN_READYDRV);

	iot_modinstance_item_t *modinst=driver.local.modinstlk.modinst;
	assert(modinst!=NULL && modinst->is_working());
	assert(uv_thread_self()==modinst->thread->thread);

	assert(!c2d.reader_closed);
	static_cast<iot_device_driver_base*>(modinst->instance)->device_close(&drvview);

	c2d.reader_closed=true;
	close(msg);
}



//void iot_device_connection_t::c2d_write_ready(void) { //called by driver after reading data from c2d stream buffer if c2d.want_write is true
//}

//tries to write a message to driver's in-queue in full
//returns:
//0 - success
//IOT_ERROR_INVALID_ARGS - datasize is zero
//IOT_ERROR_TRY_AGAIN - not enough space in queue, but it can appear later (buffer size is enough). driver_write_space_notify(true) can be used to enable notification about free space
//IOT_ERROR_NO_BUFSPACE - not enough space in queue, and it cannot appear later (TODO use another type of call)
//IOT_ERROR_NO_PEER - driver side of connection is closed
int iot_device_connection_t::send_driver_message(const void* data, uint32_t datasize) { //can be called in client thread only
	assert(client_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV);
	if(c2d.reader_closed) return IOT_ERROR_NO_PEER; //driver already closed
//		if(state!=IOT_DEVCONN_FULLREADY && state!=IOT_DEVCONN_READYDRV) return IOT_ERROR_NOT_READY;
	assert(uv_thread_self()==client.local.modinstlk.modinst->thread->thread);

	int err=send_message<&iot_device_connection_t::c2d>(data, datasize);
	if(err) return err;
	c2d_ready();
//			if(driver_host==iot_current_hostid && clinst->thread==driver.local.modinstlk.modinst->thread) {
//				//TODO use more optimal way (hack libuv?)
//				uv_async_send(&c2d.read_ready);
//			} else {
//				uv_async_send(&c2d.read_ready);
//			}
	return 0;
}

//can be called by CLIENT to enable/disable notifications about free write space in driver's buffer
void iot_device_connection_t::driver_write_avail_notify(bool want_write) {
	assert(client_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV);
	assert(uv_thread_self()==client.local.modinstlk.modinst->thread->thread);
	if(c2d.want_write==want_write) return;
	c2d.want_write=want_write;
	if(!want_write) return;
	c2d_ready();
}

//starts writing new request to driver. if there is unfinished previous request, is will be marked as bad.
//fulldatasize (defaults to datasize if zero) must specify full request data size, while datasize shows how many bytes are ready to be written.
//So fulldatasize cannot be less than datasize, datasize cannot be 0 and fulldatasize cannot exceed or be equal to 1GB.
//On success returns positive number of actually written bytes. If this number is less than fulldatasize, additional calls to continue_driver_request() must be done
//On error result is negative error code:
//	IOT_ERROR_NO_PEER - peer already closed connection
//	IOT_ERROR_INVALID_ARGS - invalid arguments
//	IOT_ERROR_TRY_AGAIN - no space in buffer available. call can be retried (the same or a new one) after getting CANWRITE notification.
int32_t iot_device_connection_t::start_driver_request(const void* data, uint32_t datasize, uint32_t fulldatasize) { //can be called in client thread only
	assert(client_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV);
//		if(state!=IOT_DEVCONN_FULLREADY && state!=IOT_DEVCONN_READYDRV) return IOT_ERROR_NOT_READY;
	assert(uv_thread_self()==client.local.modinstlk.modinst->thread->thread);

	if(c2d.reader_closed) return IOT_ERROR_NO_PEER; //driver already closed

	if(!fulldatasize) fulldatasize=datasize;
	if(!data || !datasize || fulldatasize<datasize || fulldatasize>0x3fffffff) return IOT_ERROR_INVALID_ARGS;

	uint32_t rval=write_driver_start(data, datasize, fulldatasize);
	if(rval==0) return IOT_ERROR_TRY_AGAIN;

	c2d_ready();

	return int32_t(rval);
}

//continues writing request to driver.
//fulldatasize (defaults to datasize if zero) must specify full request data size, while datasize shows how many bytes are ready to be written.
//On success returns positive number of actually written bytes (can be < datasize)
//On error result is negative error code:
//	IOT_ERROR_NO_PEER - peer already closed connection
//	IOT_ERROR_INVALID_ARGS - invalid arguments OR there is NO unfinished request to continue OR excess data provided (written datasize + provided datasize exceed fulldatasize)
//	IOT_ERROR_TRY_AGAIN - no space in buffer available. call must be retried after getting CANWRITE notification.
int32_t iot_device_connection_t::continue_driver_request(const void* data, uint32_t datasize) { //can be called in client thread only
	assert(client_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV);
//		if(state!=IOT_DEVCONN_FULLREADY && state!=IOT_DEVCONN_READYDRV) return IOT_ERROR_NOT_READY;
	assert(uv_thread_self()==client.local.modinstlk.modinst->thread->thread);

	if(c2d.reader_closed) return IOT_ERROR_NO_PEER; //driver already closed

	if(!data || !datasize) return IOT_ERROR_INVALID_ARGS;

	uint32_t rval=write_driver_end(data, datasize);
	if(rval==0) return IOT_ERROR_TRY_AGAIN;
	if(rval==0xffffffffu) return IOT_ERROR_INVALID_ARGS;

	c2d_ready();

	return int32_t(rval);
}

//read (or discard when buf==NULL) request for client
//buf can be NULL to discard read bytes of request.
//bufsize can be 0 if only obtaining szleft is necessary
//On exit dataread shows many bytes were written into buf (or discarded if buf is NULL), 
//szleft shows how many bytes of request left (if IOT_ERROR_TRY_AGAIN is returned), so that necessary buffer could be provided in full. zero indicates that 
//	either nothing to read, or request tail is still on its way to say if request is OK or corrupted.
//Returns:
//	0 - dataread bytes of request were read into buf (or discarded), request is complete and good
//	IOT_ERROR_TRY_AGAIN - dataread bytes of request were read into buf (or discarded), request is incomplete (szleft bytes left to read) or no requests.
//	IOT_ERROR_BAD_REQUEST - dataread bytes of request were read into buf (or discarded), request is complete but corrupted (writer didn't supplied enough bytes,
//							so read data will contain zeros from some point and till the end)
int iot_device_connection_t::read_client_request(void* buf, uint32_t bufsize, uint32_t &dataread, uint32_t &szleft) { //can be called in client thread only
	assert(driver_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV);
	assert(uv_thread_self()==client.local.modinstlk.modinst->thread->thread);

	int status;
	uint32_t rval=read_client(buf, bufsize, szleft, status);
	dataread=rval;
	if(status==0) return IOT_ERROR_TRY_AGAIN;  //request is not full yet or not confirmed
	if(status==1) return 0;
	return IOT_ERROR_BAD_REQUEST;
}


void iot_device_connection_t::c2d_ready(void) { //called by client after writing data to c2d stream buffer
	assert(client_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV);
	assert(uv_thread_self()==client.local.modinstlk.modinst->thread->thread);

	iot_threadmsg_t *msg=c2d_ready_msg;
	if(!msg) return; //message is already in fly

	if(driver_host==iot_current_hostid) {
		int err=iot_prepare_msg(msg, IOT_MSG_CONNECTION_C2D_READY, NULL, 0, &connident, sizeof(connident), IOT_THREADMSG_DATAMEM_TEMP, true);
		assert(err==0);
		c2d_ready_msg=NULL;

		driver.local.modinstlk.modinst->thread->send_msg(msg);
	} else if(driver_host) {
		//todo for remote
		assert(false);
	}
}
//void iot_device_connection_t::d2c_write_ready(void) { //called by client after reading data from d2c stream buffer if d2c.want_write is true
//}

void iot_device_connection_t::on_c2d_ready(void) { //processes IOT_MSG_CONNECTION_C2D_READY msg
	assert(driver_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV);

	if(c2d.reader_closed) return; //driver already closed

	iot_modinstance_item_t *drvinst=driver.local.modinstlk.modinst;
	assert(uv_thread_self()==drvinst->thread->thread);

	uint32_t sz, rval;
	int status;

	if(!drvinst->is_working()) return;

	if(d2c.want_write && d2c.buf.avail_write()>0) {
		static_cast<iot_device_driver_base*>(drvinst->instance)->device_action(&drvview, IOT_DEVCONN_ACTION_CANWRITE, 0, NULL);
	}

	peek_msg<&iot_device_connection_t::c2d>(NULL, 0, sz, status);

	uint32_t wasspace=c2d.buf.avail_write();
	if(status!=0) {
		if(status==-2) { //there is half-read request, notify instance
			static_cast<iot_device_driver_base*>(drvinst->instance)->device_action(&drvview, IOT_DEVCONN_ACTION_CANREADCONT, sz, NULL);
			peek_msg<&iot_device_connection_t::c2d>(NULL, 0, sz, status); //repeat call on first iteration if we sent CANREAD notification, instance could read tail of request
		}

		//read full requests
		do {
			if(!status || status==-2) break; //no full request
			if(status<0) { //corrupted message, must be cleared
				uint32_t left;
				rval=read<&iot_device_connection_t::c2d>(NULL, sz, left, status);
				assert(rval==sz && left==0 && status==-1);
			} else {
				assert(status==1 && sz>0);
				uint32_t left;
				alignas(8) char buf[sz];
				rval=read<&iot_device_connection_t::c2d>(buf, sz, left, status);
				assert(rval==sz && left==0 && status==1);
				static_cast<iot_device_driver_base*>(drvinst->instance)->device_action(&drvview, IOT_DEVCONN_ACTION_FULLREQUEST, sz, buf);
			}
			peek_msg<&iot_device_connection_t::c2d>(NULL, 0, sz, status); //repeat call on first iteration if we sent CANREAD notification, instance could read tail of request
		} while(c2d.requests.load(std::memory_order_relaxed)>0); //have more incoming data
	}
	if(status==0 && sz>0) { //there is half-written request, notify instance
		static_cast<iot_device_driver_base*>(drvinst->instance)->device_action(&drvview, IOT_DEVCONN_ACTION_CANREADNEW, sz, NULL);
	}
	if(c2d.buf.avail_write()>wasspace) c2d.got_writespace=true; //got free space

	if(c2d.want_write && c2d.got_writespace) { //client wants to know about free space for writing and driver freed some space
		c2d.got_writespace=false; //reset to avoid endless ping-pong effect
		d2c_ready();
	}
}






//tries to write a message to clients's in-queue in full
//returns:
//0 - success
//IOT_ERROR_INVALID_ARGS - datasize is zero
//IOT_ERROR_TRY_AGAIN - not enough space in queue, but it can appear later (buffer size is enough)
//IOT_ERROR_NO_BUFSPACE - not enough space in queue, and it cannot appear later (TODO use another type of call)
////IOT_ERROR_NOT_READY - driver side of connection is closed
int iot_device_connection_t::send_client_message(const void* data, uint32_t datasize) { //can be called in driver thread only
	assert(driver_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV || (state==IOT_DEVCONN_PENDING && d2c.buf.getsize()>0));

//	if(state!=IOT_DEVCONN_FULLREADY && state!=IOT_DEVCONN_READYDRV) return IOT_ERROR_NOT_READY;
	assert(uv_thread_self()==driver.local.modinstlk.modinst->thread->thread);

	int err=send_message<&iot_device_connection_t::d2c>(data, datasize);
	if(err) return err;
	if(state==IOT_DEVCONN_READYDRV) d2c_ready();
//			if(client_host==iot_current_hostid && client.local.modinstlk.modinst->thread==drvinst->thread) {
//				//TODO use more optimal way (hack libuv?)
//				uv_async_send(&d2c.read_ready);
//			} else {
//				if(state==IOT_DEVCONN_FULLREADY) uv_async_send(&d2c.read_ready);
//			}
	return 0;
}

//can be called by DRIVER to enable/disable notifications about free write space in clients's buffer
void iot_device_connection_t::client_write_avail_notify(bool want_write) {
	assert(driver_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV || (state==IOT_DEVCONN_PENDING && d2c.buf.getsize()>0));

	assert(uv_thread_self()==driver.local.modinstlk.modinst->thread->thread);
	if(d2c.want_write==want_write) return;
	d2c.want_write=want_write;
	if(!want_write) return;
	if(state==IOT_DEVCONN_READYDRV) d2c_ready();
}

//starts writing new request to client. if there is unfinished previous request, is will be marked as bad.
//fulldatasize (defaults to datasize if zero) must specify full request data size, while datasize shows how many bytes are ready to be written.
//So fulldatasize cannot be less than datasize, datasize cannot be 0 and fulldatasize cannot exceed or be equal to 1GB.
//On success returns positive number of actually written bytes. If this number is less than fulldatasize, additional calls to continue_driver_request() must be done
//On error result is negative error code:
//	IOT_ERROR_NO_PEER - peer already closed connection
//	IOT_ERROR_INVALID_ARGS
//	IOT_ERROR_TRY_AGAIN - no space in buffer available. call can be retried (the same or a new one) after getting CANWRITE notification.
int32_t iot_device_connection_t::start_client_request(const void* data, uint32_t datasize, uint32_t fulldatasize) { //can be called in driver thread only
	assert(client_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV || (state==IOT_DEVCONN_PENDING && d2c.buf.getsize()>0));
	assert(uv_thread_self()==driver.local.modinstlk.modinst->thread->thread);

	if(d2c.reader_closed) return IOT_ERROR_NO_PEER; //driver already closed

	if(!fulldatasize) fulldatasize=datasize;
	if(!data || !datasize || fulldatasize<datasize || fulldatasize>0x3fffffff) return IOT_ERROR_INVALID_ARGS;

	uint32_t rval=write_client_start(data, datasize, fulldatasize);
	if(rval==0) return IOT_ERROR_TRY_AGAIN;

	d2c_ready();

	return int32_t(rval);
}

//continues writing request to client.
//fulldatasize (defaults to datasize if zero) must specify full request data size, while datasize shows how many bytes are ready to be written.
//On success returns positive number of actually written bytes (can be < datasize)
//On error result is negative error code:
//	IOT_ERROR_NO_PEER - peer already closed connection
//	IOT_ERROR_INVALID_ARGS - invalid arguments OR there is NO unfinished request to continue OR excess data provided (written datasize + provided datasize exceed fulldatasize)
//	IOT_ERROR_TRY_AGAIN - no space in buffer available. call must be retried after getting CANWRITE notification.
int32_t iot_device_connection_t::continue_client_request(const void* data, uint32_t datasize) { //can be called in driver thread only
	assert(client_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV || (state==IOT_DEVCONN_PENDING && d2c.buf.getsize()>0));
	assert(uv_thread_self()==driver.local.modinstlk.modinst->thread->thread);

	if(d2c.reader_closed) return IOT_ERROR_NO_PEER; //driver already closed

	if(!data || !datasize) return IOT_ERROR_INVALID_ARGS;

	uint32_t rval=write_client_end(data, datasize);
	if(rval==0) return IOT_ERROR_TRY_AGAIN;
	if(rval==0xffffffffu) return IOT_ERROR_INVALID_ARGS;

	d2c_ready();

	return int32_t(rval);
}

//read (or discard when buf==NULL) request for driver
//buf can be NULL to discard read bytes of request.
//bufsize can be 0 if only obtaining szleft is necessary
//On exit dataread shows many bytes were written into buf (or discarded if buf is NULL), 
//szleft shows how many bytes of request left (if IOT_ERROR_TRY_AGAIN is returned), so that necessary buffer could be provided in full. zero indicates that 
//	either nothing to read, or request tail is still on its way to say if request is OK or corrupted.
//Returns:
//	0 - dataread bytes of request were read into buf (or discarded), request is complete and good
//	IOT_ERROR_TRY_AGAIN - dataread bytes of request were read into buf (or discarded), request is incomplete (szleft bytes left to read) or no requests.
//	IOT_ERROR_BAD_REQUEST - dataread bytes of request were read into buf (or discarded), request is complete but corrupted (writer didn't supplied enough bytes,
//							so read data will contain zeros from some point and till the end)
int iot_device_connection_t::read_driver_request(void* buf, uint32_t bufsize, uint32_t &dataread, uint32_t &szleft) { //can be called in driver thread only
	assert(driver_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV || (state==IOT_DEVCONN_PENDING && d2c.buf.getsize()>0));
	assert(uv_thread_self()==driver.local.modinstlk.modinst->thread->thread);

	if(state<IOT_DEVCONN_READYDRV) { //client side not attached yet, so cannot have anything to read
		dataread=0;
		szleft=0;
		return IOT_ERROR_TRY_AGAIN;
	}
	int status;
	uint32_t rval=read_driver(buf, bufsize, szleft, status);
	dataread=rval;
	if(status==0) return IOT_ERROR_TRY_AGAIN;  //request is not full yet or not confirmed
	if(status==1) return 0;
	return IOT_ERROR_BAD_REQUEST;
}


void iot_device_connection_t::d2c_ready(void) { //called by driver after writing data to d2c stream buffer
	assert(driver_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV);
	assert(uv_thread_self()==driver.local.modinstlk.modinst->thread->thread);

	iot_threadmsg_t *msg=d2c_ready_msg;
	if(!msg) return; //message is already in fly

	if(client_host==iot_current_hostid) {
		int err=iot_prepare_msg(msg, IOT_MSG_CONNECTION_D2C_READY, NULL, 0, &connident, sizeof(connident), IOT_THREADMSG_DATAMEM_TEMP, true);
		assert(err==0);
		d2c_ready_msg=NULL;

		client.local.modinstlk.modinst->thread->send_msg(msg);
	} else if(client_host) {
		//todo for remote
		assert(false);
	}
}


void iot_device_connection_t::on_d2c_ready(void) { //processes IOT_MSG_CONNECTION_D2C_READY msg
	assert(client_host==iot_current_hostid);
	assert(state>=IOT_DEVCONN_READYDRV);

	if(d2c.reader_closed) return; //client hasn't yet attached the connection or already closed

	iot_modinstance_item_t *clinst=client.local.modinstlk.modinst;
	assert(uv_thread_self()==clinst->thread->thread);

	uint32_t sz, rval;
	int status;

	if(!clinst->is_working()) return;

	if(c2d.want_write && c2d.buf.avail_write()>0) {
		static_cast<iot_node_base*>(clinst->instance)->device_action(&clientview, IOT_DEVCONN_ACTION_CANWRITE, 0, NULL);
	}

	uint32_t wasspace=d2c.buf.avail_write();
	do {
		peek_msg<&iot_device_connection_t::d2c>(NULL, 0, sz, status);
		if(!status || status==-2) break; //no full request
		if(status<0) { //corrupted message, must be cleared
			uint32_t left;
			rval=read<&iot_device_connection_t::d2c>(NULL, sz, left, status);
			assert(rval==sz && left==0 && status==-1);
		} else {
			assert(status==1 && sz>0);
			uint32_t left;
			alignas(8) char buf[sz];
			rval=read<&iot_device_connection_t::d2c>(buf, sz, left, status);
			assert(rval==sz && left==0 && status==1);
			static_cast<iot_node_base*>(clinst->instance)->device_action(&clientview, IOT_DEVCONN_ACTION_FULLREQUEST, sz, buf);
		}
	} while(d2c.requests.load(std::memory_order_relaxed)>0); //have more incoming data
	if(d2c.buf.avail_write()>wasspace) d2c.got_writespace=true; //got free space

	if(d2c.want_write && d2c.got_writespace) { //driver wants to know about free space for writing and client freed some space
		d2c.got_writespace=false; //reset to avoid endless ping-pong effect
		c2d_ready();
	}
}
