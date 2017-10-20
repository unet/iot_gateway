#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include "iot_netcon.h"
#include "iot_netcon_tcp.h"


int iot_netcon::start_uv(iot_thread_item_t* use_thread) {
		assert(uv_thread_self()==control_thread_item->thread);
		if(!meta->is_uv) return IOT_ERROR_UNKNOWN_ACTION;

		if(state==STATE_STARTED) return IOT_ERROR_NO_ACTION;
		assert(state==STATE_INITED);

		if(!use_thread) return IOT_ERROR_INVALID_ARGS;
//		worker_thread_item=use_thread;
		worker_thread=use_thread->thread;
		loop=use_thread->loop;
		allocator=use_thread->allocator;

		state=STATE_STARTED;
		//AFTER this state change connection object cannot be freed until start_msg is NULL and (worker thread is dead or STOPPED notification arrived to control thread)
		if(uv_thread_self() != worker_thread) {
			int err=iot_prepare_msg(start_msg,IOT_MSG_PEERCON_STARTWORK, NULL, 0, this, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
			if(err) return err;
			use_thread->send_msg(start_msg);
		} else {
			do_start_uv();
		}
		return 0;
	}

iot_netcontype_metaclass_tcp iot_netcontype_metaclass_tcp::object;

