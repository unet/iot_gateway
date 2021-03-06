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
int iot_netcon::start_uv(iot_thread_item_t* use_thread, bool always_async, bool finish_init) { //use_thread can be NULL to auto select thread. can be used from any thread
																				//true always_async means that async start must be used even from the same thread
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

		if(!always_async && uv_thread_self() == thread->thread) {
			on_startstop_msg();
			return 0;
		}

		if(commsg_pending.test_and_set(std::memory_order_acq_rel)) { //here this flag must be always unset
			assert(false);
			return IOT_ERROR_NOT_READY;
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
	commsg_pending.clear(std::memory_order_release); //this is end of critical section which could be initiated in restart()

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
	if(graceful_sesstop && protosession) { //graceful stop of session requested
		protosession->stop(true);
	} else {
		do_stop();
	}
}


//errors
//IOT_ERROR_NOT_READY - destroying will began in async way
int iot_netcon::destroy(bool always_async, bool graceful_stop) {
	destroy_pending=true;

	char outbuf[256];
	sprint(outbuf, sizeof(outbuf));
outlog_debug("Netcon %s is destroying", outbuf);

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
	return restart(always_async, graceful_stop);
}

int iot_netcon::restart(bool always_async, bool graceful_stop) {
	state_t curstate=state.load(std::memory_order_acquire);
	if(curstate!=STATE_STARTED) return IOT_ERROR_NO_ACTION;

	if(gracefulstop_pending.test_and_set(std::memory_order_acq_rel)) { //set gracefulstop_pending on any type of restart
		//here either graceful or hard restart was already initiated
		if(graceful_stop) return IOT_ERROR_NOT_READY; //
		//else try to initiate hard restart
		if(stop_pending.test_and_set(std::memory_order_acq_rel)) return IOT_ERROR_NOT_READY; //hard stop already in progress
	} else { //neither type of restart was initiated, so both types can be initiated now
		//only one thread can arrive here for started netcon which was not requested to restart
		if(!graceful_stop)
			if(stop_pending.test_and_set(std::memory_order_acq_rel)) return IOT_ERROR_NOT_READY;
	}
	//only one thread can arrive here for started netcon
	graceful_sesstop=graceful_stop;

	if(!always_async && uv_thread_self() == thread->thread) {
		return do_stop();
	}

	//try to start critical section till execution of on_startstop_msg() or end of on_stopped()
	if(commsg_pending.test_and_set(std::memory_order_acq_rel)) { //msg can be busy when we had previous graceful stop request and now we have hard OR if on_stopped is in progress
		return IOT_ERROR_NOT_READY;
	}
	assert(com_msg.is_free());

	iot_threadmsg_t *msg=&com_msg;
	int err=iot_prepare_msg(msg,IOT_MSG_NETCON_STARTSTOP, NULL, 0, this, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
	if(err) {
		assert(false);
		commsg_pending.clear(std::memory_order_release);
		stop_pending.clear(std::memory_order_release);
		gracefulstop_pending.clear(std::memory_order_release);
		return err;
	}
	thread->send_msg(msg);
	return IOT_ERROR_NOT_READY;
}

int iot_netcon::on_stopped(void) { //finishes hard STOP operation for base class
	state_t curstate=state.load(std::memory_order_acquire);
	assert(curstate!=STATE_STARTED || uv_thread_self() == thread->thread);

	if(commsg_pending.test_and_set(std::memory_order_acq_rel)) { //msg is busy till execution of on_startstop_msg()
		assert(curstate==STATE_STARTED);
		graceful_sesstop=false; //switch to hard stop in case graceful stop was scheduled
		return IOT_ERROR_NOT_READY;
	}
	assert(com_msg.is_free());

	char outbuf[256];
	sprint(outbuf, sizeof(outbuf));
	if(destroy_pending || is_passive || curstate!=STATE_STARTED || thread_registry->is_shutting_down() || (protoconfig->gwinst && protoconfig->gwinst->is_shutdown)) {
outlog_debug("Netcon %s (%lu) stopped, destroying...", outbuf, uintptr_t(this));
		if(registry && registry_prev) {
			registry->on_destroyed_connection(this);
			registry=NULL;
		}
		if(thread) thread->remove_netcon(this); //will nullify thread pointer
		meta->destroy_netcon(this);
		return IOT_ERROR_OBJECT_INVALIDATED;
	}
	//restart
outlog_debug("Netcon %s stopped, restarting...", outbuf);

	commsg_pending.clear(std::memory_order_release);
	stop_pending.clear(std::memory_order_release);
	gracefulstop_pending.clear(std::memory_order_release);

	if(meta->is_uv) {
		do_start_uv();
	}
	else { //TODO
		assert(false);
	}
	return 0;
}



iot_netcontype_metaclass_tcp iot_netcontype_metaclass_tcp::object;

int iot_netcontype_metaclass::meshproxy_from_json(iot_netcontype_metaclass* meta, iot_hostid_t proxyhost, json_object* json, iot_netproto_config* protoconfig, iot_netcon*& obj, bool is_server, iot_netconregistryiface* registry, uint32_t metric) {
	//TODO
	//call some size_t bufsize=meta->p_proxyparams_from_json(json, is_server, NULL, 0) to get buffer size for connection params
	//char buf[bufsize]
	//meta->p_proxyparams_from_json(json, is_server, buf, bufsize) to get packed binary connection type specific params
	//create iot_netproto_config_slave instance (proxyconf) using meta->type_id and buf[bufsize] as meta data, protoconfig to know final protocol to set up when proxyhost returns success
	//create iot_netcon_mesh to proxyhost with proxyconf as protocol. metric can be assigned after init_client() call
	//IMPORTANT! all hosts, which can act as proxy, must create listening netcon_mesh sockets for proxy protocol on some dedicated port (zero)!
	return IOT_ERROR_NOT_SUPPORTED;
}

