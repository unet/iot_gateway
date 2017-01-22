#include<stdint.h>
#include<assert.h>

#include<ecb.h>

#include <iot_module.h>
#include <kernel/iot_common.h>


#include<kernel/iot_deviceregistry.h>
#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_deviceconn.h>
#include<kernel/iot_kernel.h>


static iot_connid_t last_connid=0; //holds last assigned device connection id
static iot_device_connection_t iot_deviceconnections[IOT_MAX_DEVICECONNECTIONS]; //zero item is not used

iot_device_connection_t* iot_create_connection(iot_modinstance_item_t *client_inst, uint8_t idx) {
	assert(uv_thread_self()==main_thread);
	//find free index
	for(iot_connid_t i=0;i<IOT_MAX_DEVICECONNECTIONS;i++) {
		last_connid=(last_connid+1)%IOT_MAX_DEVICECONNECTIONS;
		if(!last_connid || iot_deviceconnections[last_connid].connid) continue;

		iot_deviceconnections[last_connid].init(last_connid, client_inst, idx);

		return &iot_deviceconnections[last_connid];
	}
	return NULL;
}

iot_device_connection_t* iot_find_device_conn(iot_connid_t connid) {
	if(connid==0 || connid>=IOT_MAX_DEVICECONNECTIONS || !iot_deviceconnections[connid].connid) return NULL;
	return &iot_deviceconnections[connid];
}

int kapi_connection_send_client_msg(iot_connid_t connid, void *drv_inst, iot_deviface_classid classid, void* data, uint32_t datasize) {
	iot_device_connection_t* conn=iot_find_device_conn(connid);
	if(!conn) return IOT_ERROR_NOT_FOUND;
	if(conn->driver_host!=iot_current_hostid || conn->driver.modinst->instance!=drv_inst || conn->deviface->cfg->classid!=classid)
		return IOT_ERROR_INVALID_ARGS;
	return conn->send_client_message(data, datasize);
}


void iot_device_connection_t::init(iot_connid_t id, iot_modinstance_item_t *client_inst, uint8_t idx) {
		assert(connid==0 && id!=0);
		memset(this, 0, sizeof(*this));
		client_host=iot_current_hostid;
		client.modinst=client_inst;
		client.dev_idx=idx;
		connid=id;
		uv_async_init(client_inst->thread->loop, &d2c.read_ready, [](uv_async_t* handle) -> void {
			iot_device_connection_t *conn=(iot_device_connection_t*) handle->data;
			conn->on_d2c_read_ready();
		});
		d2c.read_ready.data=this;

		uv_async_init(client_inst->thread->loop, &c2d.write_ready, [](uv_async_t* handle) -> void {
			iot_device_connection_t *conn=(iot_device_connection_t*) handle->data;
			conn->on_c2d_write_ready();
		});
		c2d.write_ready.data=this;
	}

void iot_device_connection_t::on_d2c_read_ready(void) {
		uint32_t sz, rval;
		int status;
		if(client_host==iot_current_hostid) {
			if(!client.modinst->started) return;
			if(client.modinst->type==IOT_MODINSTTYPE_EVSOURCE) {
				peek_msg<&iot_device_connection_t::d2c>(NULL, 0, sz, status);
				if(!status || status==-2) return; //no full request
				if(status<0) { //corrupted message
					uint32_t left;
					rval=read<&iot_device_connection_t::d2c>(NULL, sz, left, status);
					assert(rval==sz && left==0 && status==-1);
				} else {
					assert(status==1 && sz>0);
					uint32_t left;
					char buf[sz];
					rval=read<&iot_device_connection_t::d2c>(buf, sz, left, status);
					assert(rval==sz && left==0 && status==1);
					if(client.modinst->module->config->iface_event_source->device_action) {
						client.modinst->module->config->iface_event_source->device_action(client.modinst->instance, client.dev_idx, deviface->cfg->classid, IOT_DEVCONN_ACTION_MESSAGE, sz, buf);
					}
				}
				if(d2c.requests.load(std::memory_order_relaxed)>0) {
					//TODO use more optimal way (hack libuv?)
					uv_async_send(&d2c.read_ready);
				}
				return;
			}
			//TODO for executors
			assert(false);
		} else {
			//TODO
			assert(false);
		}
	}


//tries to write a message to driver's in-queue in full
//returns:
//0 - success
//IOT_ERROR_INVALID_ARGS - datasize is zero
//IOT_ERROR_TRY_AGAIN - not enough space in queue, but it can appear later (buffer size is enough)
//IOT_ERROR_NO_BUFSPACE - not enough space in queue, and it cannot appear later (TODO use another type of call)
int iot_device_connection_t::send_driver_message(const void* data, uint32_t datasize) { //can be called in client thread only
		assert(uv_thread_self()==client.modinst->thread->thread);
		int rval=send_message<&iot_device_connection_t::c2d>(data, datasize);
		if(!rval) {
			if(driver_host==iot_current_hostid && client.modinst->thread==driver.modinst->thread) {
				//TODO use more optimal way (hack libuv?)
				uv_async_send(&c2d.read_ready);
			} else {
				uv_async_send(&c2d.read_ready);
			}
		}
		return rval;
}

//tries to write a message to clients's in-queue in full
//returns:
//0 - success
//IOT_ERROR_INVALID_ARGS - datasize is zero
//IOT_ERROR_TRY_AGAIN - not enough space in queue, but it can appear later (buffer size is enough)
//IOT_ERROR_NO_BUFSPACE - not enough space in queue, and it cannot appear later (TODO use another type of call)
int iot_device_connection_t::send_client_message(const void* data, uint32_t datasize) { //can be called in driver thread only
		assert(uv_thread_self()==driver.modinst->thread->thread);
		int rval=send_message<&iot_device_connection_t::d2c>(data, datasize);
		if(!rval) {
			if(client_host==iot_current_hostid && client.modinst->thread==driver.modinst->thread) {
				//TODO use more optimal way (hack libuv?)
				uv_async_send(&d2c.read_ready);
			} else {
				uv_async_send(&d2c.read_ready);
			}
		}
		return rval;
}


//add request to client input buffer.
//returns actually written bytes.
//if returned value is less than sz (it cannot be greater) but > 0, request was not written entirely and additional calls 
//to write_client_end() must be made to write full request
//if 0 is returned, write_client_start() must be retried again (with another request if necessary)
uint32_t iot_device_connection_t::write_client_start(const char *data, uint32_t sz, uint32_t fullsz) {
	return write_start<&iot_device_connection_t::d2c>(data,sz,fullsz);
}
uint32_t iot_device_connection_t::write_driver_start(const char *data, uint32_t sz, uint32_t fullsz) {
	return write_start<&iot_device_connection_t::c2d>(data,sz,fullsz);
}

//read request FOR client (from driver)
//status is returned after every call:
// 0 - continue to call read_client (even if szleft is 0 status may be still unknown)
// 1 - request fully read and it is good
// -1 - request corrupted. return value can be any (new read_client call will read next request)
//return value shows how many bytes were written into buf
//szleft shows how many bytes of request left, so that necessary buffer could be provided in full. zero indicates that 
//	either nothing to read, or request tail is still on its way to say if request is OK or corrupted
uint32_t iot_device_connection_t::read_client(char *buf, uint32_t bufsz, uint32_t &szleft, int &status) {
	return read<&iot_device_connection_t::d2c>(buf, bufsz, szleft, status);
}

//Try to peek start of request for client. Returns zero and sets zero status if reading of some request has already begun
//status is returned after every call:
// 0 - request is not full
// 1 - request fully read and it is good
// -1 - request fully read and it is corrupted. return value can be any (new read_client call will read next request)
//return value shows how many bytes were written into buf
//szleft shows how many bytes of request left, so that necessary buffer could be provided in full. zero indicates that 
//	either nothing to read, or request tail is still on its way to say if request is OK or corrupted
//
uint32_t iot_device_connection_t::peek_client_msg(char *buf, uint32_t bufsz, uint32_t &szleft, int &status) {
	return peek_msg<&iot_device_connection_t::d2c>(buf, bufsz, szleft, status);
}


//read request FOR driver
uint32_t iot_device_connection_t::read_driver(char *buf, uint32_t bufsz, uint32_t &szleft, int &status) {
	return read<&iot_device_connection_t::c2d>(buf, bufsz, szleft, status);
}

uint32_t iot_device_connection_t::peek_driver_msg(char *buf, uint32_t bufsz, uint32_t &szleft, int &status) {
	return peek_msg<&iot_device_connection_t::c2d>(buf, bufsz, szleft, status);
}


//write additional bytes of unfinished request to client input buffer.
//returns actually written bytes.
//returns 0xffffffff on error (if provided sz is greater then it should be accorting to write_client_start call params or
//		just no active half-written request, new request should be started)
uint32_t iot_device_connection_t::write_client_end(const char *data, uint32_t sz) {
	return write_end<&iot_device_connection_t::d2c>(data,sz);
}
uint32_t iot_device_connection_t::write_driver_end(const char *data, uint32_t sz) {
	return write_end<&iot_device_connection_t::c2d>(data,sz);
}




int iot_device_connection_t::connect_remote(iot_miid_t* driver_inst, iot_deviface_classid* ifaceclassids, uint8_t num_ifaceclassids) {
		return 0; //TODO
}

int iot_device_connection_t::connect_local(iot_modinstance_item_t* driver_inst, iot_deviface_classid* ifaceclassids, uint8_t num_ifaceclassids) {
//ifaceclassids is list of num_ifaceclassids items with decreasing priority of wanted interface
		assert(uv_thread_self()==main_thread);
		assert(state==IOT_DEVCONN_INIT);
		
		assert(driver_inst->type==IOT_MODINSTTYPE_DRIVER);

		//check if driver provides any of requested interface
		iot_hwdevregistry_item_t* devitem=driver_inst->driver_hwdev;
		assert(devitem->num_devifaces <= sizeof(devitem->devifaces)/sizeof(devitem->devifaces[0]));

		uint8_t i, j;
		iot_devifacecls_item_t* deviface1=NULL;
		for(j=0; j<num_ifaceclassids; j++) {
			for(i=0;i<devitem->num_devifaces;i++) {
				if(devitem->devifaces[i]->cfg->classid==ifaceclassids[j]) {deviface1=devitem->devifaces[i];break;} //found
			}
			if(i < devitem->num_devifaces) break; //found
		}
		if(!deviface1) {
			if(j==0) {
				deviface1=devitem->devifaces[0]; //evsrc accepts any iface, so take first iface of driver
				assert(deviface1!=NULL);
			}
			else return IOT_ERROR_DEVICE_NOT_SUPPORTED;
		}

		//check if driver can take more connections
		for(i=0;i<sizeof(driver_inst->driver_conn)/sizeof(driver_inst->driver_conn[0]);i++) if(!driver_inst->driver_conn[i]) break;
		if(i>=sizeof(driver_inst->driver_conn)/sizeof(driver_inst->driver_conn[0])) return IOT_ERROR_LIMIT_REACHED;
		uint8_t conn_idx=i;

		iot_threadmsg_t *msg=NULL;
		int err=iot_prepare_msg(msg,IOT_MSG_OPEN_CONNECTION, driver_inst, conn_idx, NULL, 0, IOT_THREADMSG_DATAMEM_STATIC, &main_allocator, true);
		if(err) return err;

		state=IOT_DEVCONN_PENDING;
		deviface=deviface1;
		driver_host=iot_current_hostid;
		driver.modinst=driver_inst;
		driver.conn_idx=conn_idx;
		driver_inst->driver_conn[conn_idx]=this;

		driver_inst->thread->send_msg(msg);
		return 0;
}

//processes thread message to finish connect to started driver instance
void iot_device_connection_t::process_connect_local(void) { //called in working thread of driver instance
	assert(state==IOT_DEVCONN_PENDING);
	assert(uv_thread_self()==driver.modinst->thread->thread);

	void *priv=NULL;
	char *buf=NULL;
	int err;
	if(driver.modinst->module->config->iface_device_driver->open) { //leave this check just in case. but it should be true
		err=driver.modinst->module->config->iface_device_driver->open(driver.modinst->instance, connid, deviface->cfg->classid, &priv);
		if(err) goto errexit;
	} //assume success if no open() method provided

	//allocate buffers
	uint32_t c2d_bufsize, d2c_bufsize;
	uint8_t c2d_p, d2c_p;
	c2d_bufsize=deviface->cfg->c2d_maxmsgsize*2;
	if(c2d_bufsize<IOT_MEMOBJECT_MAXPLAINSIZE/2) c2d_bufsize=IOT_MEMOBJECT_MAXPLAINSIZE/2;

	//find smallest power of 2 which is >= c2d_bufsize
	for(c2d_p=12;c2d_p<30;c2d_p++) {
		if((uint32_t(1)<<c2d_p)>=c2d_bufsize) break;
	}
	if(c2d_p>=30) {
		err=IOT_ERROR_NO_MEMORY;
		goto errexit;
	}
	c2d_bufsize=1<<c2d_p;


	d2c_bufsize=deviface->cfg->d2c_maxmsgsize*2;
	if(d2c_bufsize<IOT_MEMOBJECT_MAXPLAINSIZE/2) d2c_bufsize=IOT_MEMOBJECT_MAXPLAINSIZE/2;

	//find smallest power of 2 which is >= d2c_bufsize
	for(d2c_p=12;d2c_p<30;d2c_p++) {
		if((uint32_t(1)<<d2c_p)>=d2c_bufsize) break;
	}
	if(d2c_p>=30) {
		err=IOT_ERROR_NO_MEMORY;
		goto errexit;
	}
	d2c_bufsize=1<<d2c_p;

	buf=(char*)driver.modinst->thread->allocator->allocate(c2d_bufsize+d2c_bufsize, true);
	if(!buf) {
		err=IOT_ERROR_NO_MEMORY;
		goto errexit;
	}
	if(!c2d.buf.setbuf(c2d_p, buf)) {
		err=IOT_ERROR_NO_MEMORY;
		goto errexit;
	}
	if(!d2c.buf.setbuf(d2c_p, buf)) {
		err=IOT_ERROR_NO_MEMORY;
		goto errexit;
	}

	uv_async_init(driver.modinst->thread->loop, &c2d.read_ready, [](uv_async_t* handle) -> void {
		iot_device_connection_t *conn=(iot_device_connection_t*) handle->data;
		conn->on_c2d_read_ready();
	});
	c2d.read_ready.data=this;
	uv_async_init(driver.modinst->thread->loop, &d2c.write_ready, [](uv_async_t* handle) -> void {
		iot_device_connection_t *conn=(iot_device_connection_t*) handle->data;
		conn->on_d2c_write_ready();
	});
	d2c.write_ready.data=this;


	driver.private_data=priv;
	state=IOT_DEVCONN_READY;

	modules_registry->notify_device_attached(this);
	return;
errexit:
	driver.modinst->driver_conn[driver.conn_idx]=NULL;
	state=IOT_DEVCONN_INIT;
	if(buf) iot_release_memblock(buf);

	assert(false);
	//TODO try another driver?
}
