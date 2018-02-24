#include<stdint.h>
#include<assert.h>
//#include<unistd.h>
//#include<time.h>

#include "iot_netcon.h"
#include "iot_netcon_tcp.h"

/*

iot_netconiface::~iot_netconiface(void) {
		assert(protosession==NULL);
//		if(protosession) { //should be on hard process shutdown only, so thread must be main
//			assert(uv_thread_self()==main_thread);
//			protosession->coniface_detach(); //for case when there are other refs to session and it won't be destroyed right now
//		}
		//destructor of session will be called if single reference left to it
	}

iot_netcon::iot_netcon(	const iot_netcontype_metaclass* meta,
				iot_netproto_config* protoconfig
//				iot_thread_item_t* ctrl_thread
			) : iot_netconiface(meta->is_stream), protoconfig(protoconfig), meta(meta)
//				, control_thread_item(ctrl_thread ? ctrl_thread : thread_registry->find_thread(uv_thread_self())) 
	{ //only derived classes can create instances , is_slave(false)
		assert(meta!=NULL);
		assert(protoconfig!=NULL);
//		assert(control_thread_item!=NULL);

		cpu_loading=get_cpu_loading();
	}

*/

//errors:
//IOT_ERROR_UNKNOWN_ACTION - this function cannot be used for this type of netcon (it is not UV)
//IOT_ERROR_NO_ACTION - netcon already started or being destroyed
//IOT_ERROR_NOT_INITED - init not done or not finished
//IOT_ERROR_NOT_READY - async start
int iot_netcon::start_uv(iot_thread_item_t* use_thread, bool finish_init) { //use_thread can be NULL to auto select thread. can be used from any thread
																				//true finish_init means that current state of object must be INITING to prevent
																				//possibility of its destruction after setting state to INITED if creation is not 
																				//within registry thread
//		assert(uv_thread_self()==control_thread_item->thread);
		if(!meta->is_uv) return IOT_ERROR_UNKNOWN_ACTION;

		state_t oldstate=finish_init ? STATE_INITING : STATE_INITED;
		//try to start. this is ONE_TIME action for each netcon, once been started it can be destroyed only
		if(!state.compare_exchange_strong(oldstate, STATE_STARTING, std::memory_order_acq_rel, std::memory_order_relaxed)) {
			if(oldstate>=STATE_STARTING || oldstate==STATE_DESTROYING) return IOT_ERROR_NO_ACTION;
			assert(false);
			return IOT_ERROR_NOT_INITED;
		}
		cpu_loading=get_cpu_loading();
		if(use_thread ? !use_thread->add_netcon(this) : !thread_registry->settle_netcon(this)) {
			state.store(STATE_INITED, std::memory_order_release);
			destroy();
			return 0;
		}
		loop=thread->loop;
		allocator=thread->allocator;

		if(uv_thread_self() == thread->thread) {
			on_startstop_msg();
			return 0;
		}

		assert(com_msg.is_free());
		iot_threadmsg_t *msg=&com_msg;
		int err=iot_prepare_msg(msg,IOT_MSG_NETCON_STARTSTOP, NULL, 0, this, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
		if(err) {
			assert(false);
			state.store(STATE_INITED, std::memory_order_release);
			std::atomic_thread_fence(std::memory_order_acq_rel);
			if(destroy_pending) destroy();
			return err;
		}
		thread->send_msg(msg);
		return IOT_ERROR_NOT_READY;
	}

void iot_netcon::on_startstop_msg(void) {
	assert(uv_thread_self() == thread->thread);

	com_msg.clear();

	state_t curstate=state.load(std::memory_order_acquire);
	if(curstate==STATE_STARTING) { //this is initial start which could be cancelled
		if(!state.compare_exchange_strong(curstate, STATE_STARTED, std::memory_order_acq_rel, std::memory_order_relaxed)) {
			assert(false);
			state=STATE_STARTED;
		}
		std::atomic_thread_fence(std::memory_order_acq_rel);
		if(destroy_pending) {
			do_stop();
			return;
		}
		do_start_uv();
		return;
	}
	//must be in STATE_STARTED
	assert(curstate==STATE_STARTED);
	do_stop();
}


//errors
//IOT_ERROR_NOT_READY - destroying will began in async way
int iot_netcon::destroy(void) {
	destroy_pending=true;

	std::atomic_thread_fence(std::memory_order_acq_rel);

	state_t curstate=state.load(std::memory_order_acquire);
	do {
		switch(curstate) {
			case STATE_UNINITED:
			case STATE_INITED:
				if(state.compare_exchange_strong(curstate, STATE_DESTROYING, std::memory_order_acq_rel, std::memory_order_acquire)) {
					//can be destroyed right now
					return do_stop();
				}
				continue; //retry do-loop
			case STATE_INITING:
			case STATE_STARTING:
			case STATE_DESTROYING:
				return IOT_ERROR_NOT_READY;
			case STATE_STARTED:
				break;
		}
		break;
	} while(1);
	//here state is STARTED
	return restart();
}

int iot_netcon::restart(void) {
	state_t curstate=state.load(std::memory_order_acquire);
	if(curstate!=STATE_STARTED) return IOT_ERROR_NO_ACTION;

	if(stop_pending.test_and_set(std::memory_order_acq_rel)) return IOT_ERROR_NOT_READY;
	//only one thread can arrive here for started netcon

	if(uv_thread_self() == thread->thread) {
		return do_stop();
	}

	assert(com_msg.is_free()); //state is STARTED, so msg must be free already
	iot_threadmsg_t *msg=&com_msg;
	int err=iot_prepare_msg(msg,IOT_MSG_NETCON_STARTSTOP, NULL, 0, this, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
	if(err) {
		assert(false);
		stop_pending.clear(std::memory_order_release);
		return err;
	}
	thread->send_msg(msg);
	return IOT_ERROR_NOT_READY;
}

int iot_netcon::on_stopped(void) { //finishes STOP operation for base class
	state_t curstate=state.load(std::memory_order_acquire);
	assert(curstate!=STATE_STARTED || uv_thread_self() == thread->thread);

	if(curstate==STATE_STARTED && !com_msg.is_free()) return IOT_ERROR_NOT_READY; //msg is in fly. wait for it to arrive
	char outbuf[256];
	sprint(outbuf, sizeof(outbuf));
	if(destroy_pending || is_passive || curstate!=STATE_STARTED || thread_registry->is_shutting_down()) {
outlog_notice("Netcon %s (%lu) stopped, destroying...", outbuf, uintptr_t(this));
		if(registry && registry_prev) {
			registry->on_destroyed_connection(this);
			registry=NULL;
		}
		if(thread) thread->remove_netcon(this); //will nullify thread pointer
		meta->destroy_netcon(this);
		return IOT_ERROR_OBJECT_INVALIDATED;
	}
	//restart
outlog_notice("Netcon %s stopped, restarting...", outbuf);
	stop_pending.clear(std::memory_order_release);
	if(meta->is_uv) {
		do_start_uv();
	}
	else { //TODO
		assert(false);
	}
	return 0;
}



/*
int iot_netcon::on_stopped(bool ismsgproc) { //finishes STOP operation in control thread OR sends message to control thread. ismsgproc is used internally and must be
											//false if not in processing of IOT_MSG_NETCON_STOPPED
outlog_error("on_stopped for %lu", uintptr_t(this));
		if(uv_thread_self()==control_thread_item->thread) {
			iot_thread_item_t* use_thread=NULL;
			bool needrestart=false;
			if(state==STATE_STARTED) {
				assert(stop_pending);
				state=STATE_INITED;
				stop_pending=false;
				needrestart=true;
				if(meta->is_uv) {
					assert(thread!=NULL);
					use_thread=thread;
					thread->remove_netcon(this); //will nullify thread pointer
				}
			}
			char outbuf[256];
			sprint(outbuf, sizeof(outbuf));
			if(destroy_pending || is_passive || !needrestart || thread_registry->is_shutting_down()) {
outlog_notice("Netcon %s (%lu) stopped, destroying...", outbuf, uintptr_t(this));
				if(registry && registry_prev) {
					registry->on_destroyed_connection(this);
					registry=NULL;
				}
				state=STATE_UNINITED;
				meta->destroy_netcon(this);
				return IOT_ERROR_OBJECT_INVALIDATED;
			} else { //restart
outlog_notice("Netcon %s stopped, restarting...", outbuf);
				if(meta->is_uv) {
					int err=start_uv(use_thread);
					assert(err==0); //restart must succeed
				} //else //TODO
			}
			return 0;
		}
		if(ismsgproc) { //should not happen
			assert(false);
			return IOT_ERROR_CRITICAL_BUG;
		}
		iot_threadmsg_t* msg=start_msg && start_msg->is_free() ? start_msg : NULL; //use start_msg if it was allocated and is already free, otherwise new msg will be allocated
		int err=iot_prepare_msg(msg,IOT_MSG_NETCON_STOPPED, NULL, 0, this, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
		if(err) return err;
		control_thread_item->send_msg(msg); //processing will call this func again with ismsgproc=true
		return IOT_ERROR_OBJECT_INVALIDATED; //always return such error when async signal is sent because we cannot be sure destroy_pending isn't ot will not be set at any point in time
	}
*/

iot_netcontype_metaclass_tcp iot_netcontype_metaclass_tcp::object;

