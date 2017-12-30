#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include "iot_netcon.h"
#include "iot_netcon_tcp.h"


iot_netconiface::~iot_netconiface() {
		if(protosession) {
			protosession->stop(true);
			protosession=NULL;
		}
	}


iot_netcon::iot_netcon(	const iot_netcontype_metaclass* meta,
				iot_netproto_config* protoconfig,
				iot_thread_item_t* ctrl_thread
			) : protoconfig(protoconfig), meta(meta), control_thread_item(ctrl_thread ? ctrl_thread : thread_registry->find_thread(uv_thread_self())) { //only derived classes can create instances , is_slave(false)
		assert(meta!=NULL);
		assert(protoconfig!=NULL);
		assert(control_thread_item!=NULL);
	}


int iot_netcon::start_uv(iot_thread_item_t* use_thread) {
		assert(uv_thread_self()==control_thread_item->thread);
		if(!meta->is_uv) return IOT_ERROR_UNKNOWN_ACTION;
		if(!use_thread) return IOT_ERROR_INVALID_ARGS;

		if(state!=STATE_INITED) {
			if(state==STATE_STARTED) return IOT_ERROR_NO_ACTION;
			assert(false);
			return IOT_ERROR_NOT_READY;
		}

		worker_thread_item=use_thread;
		worker_thread=use_thread->thread;
		loop=use_thread->loop;
		allocator=use_thread->allocator;

		state=STATE_STARTED;
		//AFTER this state change connection object cannot be freed until start_msg->is_free() and (worker thread is dead or STOPPED notification arrived to control thread)
		if(uv_thread_self() != worker_thread) {
			if(!start_msg) {
				start_msg=(iot_threadmsg_t*)allocator->allocate(sizeof(iot_threadmsg_t));
				if(!start_msg) {state=STATE_INITED; return IOT_ERROR_NO_MEMORY;}
				start_msg->clear();
			}
			int err=iot_prepare_msg(start_msg,IOT_MSG_NETCON_START, NULL, 0, this, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
			if(err) {state=STATE_INITED; return err;}
			use_thread->send_msg(start_msg);
		} else {
			do_start_uv();
		}
		return 0;
	}

int iot_netcon::stop(bool destroy) {
		assert(uv_thread_self()==control_thread_item->thread);

		if(destroy) destroy_pending=true;
		if(state==STATE_STARTED) {
			if(stop_pending) return IOT_ERROR_NO_ACTION;
			stop_pending=true;

			if(uv_thread_self() != worker_thread) {
				iot_threadmsg_t* msg=start_msg && start_msg->is_free() ? start_msg : NULL; //use start_msg if it was allocated and is already free, otherwise new msg will be allocated
				int err=iot_prepare_msg(msg,IOT_MSG_NETCON_STOP, NULL, 0, this, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
				if(err) {stop_pending=false;return err;}
				worker_thread_item->send_msg(msg); //processing will call do_stop()
			} else {
				do_stop(); //processing must call on_stopped()
			}
			return 0;
		}
		return on_stopped();
	}

int iot_netcon::on_stopped(bool wasasync) { //finishes STOP operation in control thread OR sends message to control thread. wasasync is used internally and must be
											//false if not in processing of IOT_MSG_NETCON_STOPPED
		if(uv_thread_self()==control_thread_item->thread) {
			if(state==STATE_STARTED) {
				assert(stop_pending);
				state=STATE_INITED;
				stop_pending=false;
			}
			if(destroy_pending) {
				state=STATE_UNINITED;
				meta->destroy_netcon(this);
				return IOT_ERROR_OBJECT_INVALIDATED;
			}
			return 0;
		}
		if(wasasync) { //should not happen
			assert(false);
			return IOT_ERROR_CRITICAL_BUG;
		}
		iot_threadmsg_t* msg=start_msg && start_msg->is_free() ? start_msg : NULL; //use start_msg if it was allocated and is already free, otherwise new msg will be allocated
		int err=iot_prepare_msg(msg,IOT_MSG_NETCON_STOPPED, NULL, 0, this, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
		if(err) return err;
		control_thread_item->send_msg(msg); //processing will call this func again with wasasync=true
		return IOT_ERROR_OBJECT_INVALIDATED; //always return such error when async signal is sent because we cannot be sure destroy_pending isn't ot will not be set at any point in time
	}


iot_netcontype_metaclass_tcp iot_netcontype_metaclass_tcp::object;

