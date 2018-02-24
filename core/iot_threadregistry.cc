#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

//#include "iot_compat.h"
//#include "evwrap.h"
#include "iot_threadregistry.h"

#include "iot_netcon.h"
#include "iot_moduleregistry.h"
#include "iot_deviceconn.h"
#include "iot_configmodel.h"
#include "iot_configregistry.h"
#include "iot_peerconnection.h"


#define IOT_THREAD_LOADING_MAIN (IOT_THREAD_LOADING_MAX/10)

//correspondence between cpu_loading indexes and points which every modinstance adds to thread total loading estimation
static const uint16_t iot_thread_loading[4] = {
	1,
	(IOT_THREAD_LOADING_MAX/20),
	(IOT_THREAD_LOADING_MAX/10),
	IOT_THREAD_LOADING_MAX
};

#define IOT_THREAD_LOADING_NUM (sizeof(iot_thread_loading)/sizeof(iot_thread_loading[0]))


//void kern_notifydriver_removedhwdev(iot_hwdevregistry_item_t* devitem) {
//	printf("busy HWDev removed: contype=%d, unique=%lu\n", devitem->devdata.dev_ident.contype, devitem->devdata.dev_ident.hwid);
//}

int max_threads=10;

uint32_t iot_thread_item_t::last_thread_id=0;

int iot_thread_item_t::init(bool ismain) {//, uv_loop_t* loop_, iot_memallocator* allocator_) {
	int err;
	uint32_t interval;
	if(ismain) {
		thread=main_thread;
		allocator=&main_allocator;

		loop=main_loop;
	} else {
		allocator=new iot_memallocator;
		if(!allocator) goto onerr;

		loop=new uv_loop_t;
		if(!loop) goto onerr;
		uv_loop_init(loop);

	}

	uv_async_init(loop, &msgq_watcher, iot_thread_registry_t::on_thread_msg);
	msgq_watcher.data=this;

	interval=1*1000; //interval of first timer in milliseconds
	for(unsigned i=0;i<sizeof(atimer_pool)/sizeof(atimer_pool[0]);i++) {
		atimer_pool[i].timer.init(interval, loop);
		interval*=2;
	}

	if(!ismain) {
#ifndef _WIN32
		sigset_t myset, oldset;
		sigfillset( &myset );
		pthread_sigmask( SIG_SETMASK, &myset, &oldset ); //block all signals by default
#endif
		err=uv_thread_create(&thread, [](void * arg) -> void {
			iot_thread_item_t* thitem=(iot_thread_item_t*)arg;
			thitem->thread_func();

		}, this);

#ifndef _WIN32
		pthread_sigmask( SIG_SETMASK, &oldset, NULL ); //restore previous signal mask
#endif
		if(err) goto onerr;
	}

	return 0;
onerr:
	deinit();
	return IOT_ERROR_NO_MEMORY;
}

void iot_thread_item_t::thread_func(void) {
	thread=uv_thread_self();
	allocator->set_thread(&thread);
	for(unsigned i=0;i<sizeof(atimer_pool)/sizeof(atimer_pool[0]);i++) atimer_pool[i].timer.set_thread(thread);

	uv_run(loop, UV_RUN_DEFAULT);

	//TERMINATION

	//process all messages currently in msg queue
	iot_thread_registry_t::on_thread_msg(&msgq_watcher);

	//send msg to main thread
	iot_threadmsg_t* msg=&termmsg;
	int err=iot_prepare_msg(msg, IOT_MSG_THREAD_SHUTDOWNREADY, NULL, 0, this, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
	if(err) {
		assert(false);
	} else {
		main_thread_item->send_msg(msg);
	}
}


void iot_thread_item_t::deinit(void) {
	assert(uv_thread_self()==main_thread);
	assert(this==main_thread_item || thread==main_thread); //non-main thread must be already stopped and destroyed (thread is reassigned to main_thread after system thread stop)
	if(loop) {
		for(unsigned i=0;i<sizeof(atimer_pool)/sizeof(atimer_pool[0]);i++) {
			atimer_pool[i].timer.deinit();
		}
		uv_close((uv_handle_t*)&msgq_watcher, NULL);
	}
	if(this==main_thread_item) return;
	if(loop) {
		uv_walk(loop, [](uv_handle_t* handle, void* arg) -> void {
			if(!uv_is_closing(handle)) uv_close(handle, NULL);
		}, NULL);
		uv_run(loop, UV_RUN_NOWAIT);
		int err=uv_loop_close(loop);
		if(!err) {
			delete loop;
		} else {
			outlog_debug("Thread %u left event loop in busy state", thread_id);
		}
		loop=NULL;
	}
	if(allocator) {
		allocator->set_thread(&main_thread); //allocator has reference to thread of current thread item which is updated to address of main_thread after stopping thread
		if(allocator->deinit()) {
			delete allocator;
		}
		allocator=NULL;
	}
	//TODO: LEAVES item in memory!!! possibly leaves allocator in slavelist of main allocator!!!  so must be used on termination only or be upgraded
}

bool iot_thread_item_t::is_overloaded(uint8_t cpu_loadtp) {
	if(cpu_loadtp>=IOT_THREAD_LOADING_NUM) cpu_loadtp=0;
	return cpu_loading.load(std::memory_order_relaxed)+iot_thread_loading[cpu_loadtp] > IOT_THREAD_LOADING_MAX;
}


bool iot_thread_item_t::add_modinstance(iot_modinstance_item_t* inst_item) {
	assert(inst_item && !inst_item->thread);

	inst_item->thread=this;

	uint8_t cpu_loadtp=inst_item->cpu_loading;
	if(cpu_loadtp>=IOT_THREAD_LOADING_NUM) cpu_loadtp=0;

	datalock.lock();
		if(is_shutdown || thread_registry->is_shutting_down()) {
			datalock.unlock();
			return false;
		}
		cpu_loading.fetch_add(iot_thread_loading[cpu_loadtp], std::memory_order_relaxed);
		BILINKLIST_INSERTHEAD(inst_item, instances_head, next_inthread, prev_inthread);
	datalock.unlock();
	return true;
}

void iot_thread_item_t::remove_modinstance(iot_modinstance_item_t* inst_item) { //hang instances are moved to hang list
	assert(inst_item && inst_item->thread==this);

	inst_item->thread=NULL;

	uint8_t cpu_loadtp=inst_item->cpu_loading;
	if(cpu_loadtp>=IOT_THREAD_LOADING_NUM) cpu_loadtp=0;

	bool became_empty=false;
	datalock.lock();
		assert(cpu_loading.load(std::memory_order_relaxed) >= iot_thread_loading[cpu_loadtp]);
		assert(instances_head!=NULL);

		cpu_loading.fetch_sub(iot_thread_loading[cpu_loadtp], std::memory_order_relaxed);
		BILINKLIST_REMOVE(inst_item, next_inthread, prev_inthread);
		if(inst_item->state==IOT_MODINSTSTATE_HUNG) BILINKLIST_INSERTHEAD(inst_item, hung_instances_head, next_inthread, prev_inthread);
		if(!instances_head) became_empty=true;
	datalock.unlock();

	if(became_empty && thread_registry->is_shutting_down()) check_thread_load_ended();
}

bool iot_thread_item_t::add_netcon(iot_netcon* netcon) {
	assert(netcon && !netcon->thread);

	netcon->thread=this;

	uint8_t cpu_loadtp=netcon->cpu_loading;
	if(cpu_loadtp>=IOT_THREAD_LOADING_NUM) cpu_loadtp=0;

	datalock.lock();
		if(is_shutdown || thread_registry->is_shutting_down()) {
			datalock.unlock();
			return false;
		}
		cpu_loading.fetch_add(iot_thread_loading[cpu_loadtp], std::memory_order_relaxed);
		BILINKLIST_INSERTHEAD(netcon, netcons_head, next_inthread, prev_inthread);
	datalock.unlock();
	return true;
}

void iot_thread_item_t::remove_netcon(iot_netcon* netcon) {
	assert(netcon && netcon->thread==this);

	netcon->thread=NULL;

	uint8_t cpu_loadtp=netcon->cpu_loading;
	if(cpu_loadtp>=IOT_THREAD_LOADING_NUM) cpu_loadtp=0;

	bool became_empty=false;
	datalock.lock();
		assert(cpu_loading.load(std::memory_order_relaxed) >= iot_thread_loading[cpu_loadtp]);
		assert(netcons_head!=NULL);

		cpu_loading.fetch_sub(iot_thread_loading[cpu_loadtp], std::memory_order_relaxed);
		BILINKLIST_REMOVE(netcon, next_inthread, prev_inthread);
		if(!netcons_head) became_empty=true;
	datalock.unlock();

	if(became_empty && thread_registry->is_shutting_down()) check_thread_load_ended();
}

void iot_thread_item_t::check_thread_load_ended(void) { //called when thread registry is in shutdown mode to check if this thread can terminate itself
	//called by remove_modinstance() or remove_netcon() after removing last instance in shutdown mode
	assert(thread_registry->is_shutting_down());
	bool can_stop=false;
	datalock.lock();
	if(!instances_head && !netcons_head) can_stop=true;
	is_shutdown=true;
	datalock.unlock();
	if(can_stop) shutdown_self();
}

void iot_thread_item_t::on_shutdown_msg(void) {
	assert(uv_thread_self()==thread);

	if(this==main_thread_item) {
		//this is main thread of execution
		thread_registry->on_thread_shutdown(main_thread_item);
		return;
	}
	uv_stop (loop);
	//thread_func will send notification to main thread on terminate
}

void iot_thread_item_t::shutdown_self(bool sync) {
	if(!shutdown_signaled.test_and_set(std::memory_order_acquire)) { //not already signalled to stop
		is_shutdown=true;
		//this point can be reached by single thread only
		if(uv_thread_self()==thread) { //execution thread is my, no need to send msg to me
			on_shutdown_msg();
		} else {
			//send msg to request event loop stop
			iot_threadmsg_t* msg=&termmsg;
			int err=iot_prepare_msg(msg, IOT_MSG_THREAD_SHUTDOWN, NULL, 0, NULL, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
			if(err) {
				assert(false);
			} else {
				send_msg(msg);
			}
		}
	}
	if(sync && uv_thread_self()==main_thread && thread!=main_thread) {
		int err=uv_thread_join(&thread); //wait for thread shutdown message to go to main queue
		assert(err==0);
		thread=main_thread; //reassign system thread address so that objects running in destroyed thread could be happy when stopping in main thread
	}
}

iot_thread_registry_t::iot_thread_registry_t(void) {
		assert(thread_registry==NULL);
		assert(sizeof(iot_threadmsg_t)==64);
		thread_registry=this;

		int err=uv_rwlock_init(&threads_lock);
		assert(!err);

		main_thread=uv_thread_self();
		main_loop=uv_default_loop();

		//fill main thread item
		main_thread_item=&main_thread_obj;
		main_thread_item->init(true);
		main_thread_item->cpu_loading.store(IOT_THREAD_LOADING_MAIN, std::memory_order_relaxed);
		BILINKLIST_INSERTHEAD(main_thread_item, threads_head, next, prev);
	}


	//finds and assigns execution thread to modinstance
bool iot_thread_registry_t::settle_modinstance(iot_modinstance_item_t* inst_item) { //called from any thread
		bool rval;
		uv_rwlock_wrlock(&threads_lock);
		if(!is_shutdown) {
			iot_thread_item_t *th=assign_thread(inst_item->cpu_loading);
			assert(th!=NULL);

			rval=th->add_modinstance(inst_item);
		} else {
			rval=false;
		}
		uv_rwlock_wrunlock(&threads_lock);
		return rval;
	}

	//finds and assigns execution thread to netcon
bool iot_thread_registry_t::settle_netcon(iot_netcon* netcon) { //called from any thread
		bool rval;
		uv_rwlock_wrlock(&threads_lock);
		if(!is_shutdown) {
			iot_thread_item_t *th=assign_thread(netcon->cpu_loading);
			assert(th!=NULL);

			rval=th->add_netcon(netcon);
		} else {
			rval=false;
		}
		uv_rwlock_wrunlock(&threads_lock);
		return rval;
	}

iot_thread_item_t* iot_thread_registry_t::assign_thread(uint8_t cpu_loadtp){ //finds suitable thread or creates new one
		//called under write lock!

		if(cpu_loadtp>=IOT_THREAD_LOADING_NUM) cpu_loadtp=0;

		//find thread with minimum loading
		iot_thread_item_t* minthread=NULL;
		uint16_t minload=IOT_THREAD_LOADING_MAX;
		iot_thread_item_t* it=threads_head;
		while(it) {
			uint32_t cpuload=it->cpu_loading.load(std::memory_order_relaxed);
			if(!minthread || cpuload<minload) {minload=cpuload;minthread=it;}
			it=it->next;
		}
		assert(minthread!=NULL); //at least main thread must be here

		if(cpu_loadtp<3) {
			if(minload+iot_thread_loading[cpu_loadtp]<=IOT_THREAD_LOADING_MAX) return minthread;
		}
		if(num_threads>=max_threads) {
			outlog_notice("Limit on number of threads should be raised! Per-thread load exceeded.");
			return minthread;
		}
		//here new thread must and can be started
		iot_thread_item_t* thitem=new iot_thread_item_t;
		if(!thitem) goto onerr;
		if(thitem->init()) {delete thitem; goto onerr;}
		BILINKLIST_INSERTHEAD(thitem, threads_head, next, prev);
		num_threads++;
		outlog_notice("New thread created with ID %u", thitem->thread_id);
		return thitem;

onerr:
		outlog_notice("Cannot create new thread, using existing. Per-thread load exceeded.");
		return minthread;
	}

void iot_thread_registry_t::graceful_shutdown(void) { //initiate graceful shutdown, so that thread gets destroyed when all module instances and netcons get stopped
		assert(uv_thread_self()==main_thread);
		assert(!is_shutdown);
		is_shutdown=true;

		uv_rwlock_rdlock(&threads_lock);

		iot_thread_item_t* th=threads_head;
		while(th) {
			if(th!=main_thread_item) th->check_thread_load_ended(); //avoid threads_lock recursion for main thread
			th=th->next;
		}

		uv_rwlock_rdunlock(&threads_lock);

		main_thread_item->check_thread_load_ended(); //call outside lock to avoid lock recursion
	}

//notification from thread to main thread about termination
void iot_thread_registry_t::on_thread_shutdown(iot_thread_item_t* thread) {
	assert(uv_thread_self()==main_thread);

	bool lastthread=false;
	uv_rwlock_wrlock(&threads_lock);
	BILINKLIST_REMOVE_NOCL(thread, next, prev);
	if(!threads_head) lastthread=true;
	uv_rwlock_wrunlock(&threads_lock);

	outlog_debug("Thread with ID %u shut down", thread->thread_id);

	if(thread!=main_thread_item && thread->thread!=main_thread) {
		int err=uv_thread_join(&thread->thread);
		assert(err==0);
		thread->thread=main_thread;
	}

	while(auto t=thread->instances_head) {
		BILINKLIST_REMOVE(t, next_inthread, prev_inthread);
	}
	while(auto t=thread->netcons_head) {
		BILINKLIST_REMOVE(t, next_inthread, prev_inthread);
	}

	if(thread!=main_thread_item) {
		thread->deinit();

		assert(num_threads>0);
		num_threads--;
	}
	BILINKLIST_INSERTHEAD(thread, delthreads_head, next, prev); //TODO dealloc closed threads later if necesary

	if(lastthread) { //it was the last thread, deinit main thread
		//process all messages currently in msg queue
//		main_thread_item->is_shutdown=true;
		on_thread_msg(&main_thread_item->msgq_watcher);

		main_thread_item->deinit();
		uv_stop (main_loop);
	}
}

void iot_thread_registry_t::shutdown(void) {
	assert(uv_thread_self()==main_thread);
	if(threads_head) {

		outlog_debug("Forcing threads down");

		is_shutdown=true;
		uv_rwlock_rdlock(&threads_lock);

		iot_thread_item_t* th=threads_head;
		while(th) {
			if(th!=main_thread_item) {
				th->shutdown_self(true);
			}
			th=th->next;
		}
		uv_rwlock_rdunlock(&threads_lock);

		on_thread_msg(&main_thread_item->msgq_watcher); //process messages in main queue
		main_thread_item->shutdown_self();
	}
}

void iot_thread_registry_t::on_thread_msg(uv_async_t* handle) { //static
		iot_thread_item_t* thread_item=(iot_thread_item_t*)(handle->data);
		iot_threadmsg_t* msg, *nextmsg=thread_item->msgq.pop_all();
		bool had_modelsignals=false; //flag that some modelling signals were sent to config registry and thus commit_signals() must be called
		while(nextmsg) {
			msg=nextmsg;
			nextmsg=(nextmsg->next).load(std::memory_order_relaxed);
			iot_modinstance_locker modinstlk=modules_registry->get_modinstance(msg->miid);
			iot_modinstance_item_t* modinst=modinstlk.modinst;

			if(msg->is_core) { //msg for core
				switch(msg->code) {
					case IOT_MSG_THREAD_SHUTDOWN: //request to current thread to stop
						iot_release_msg(msg); msg=NULL;

						thread_item->on_shutdown_msg();
						break;
					case IOT_MSG_THREAD_SHUTDOWNREADY: {//notification from child thread about shutdown
						assert(uv_thread_self()==main_thread);

						iot_thread_item_t* th=(iot_thread_item_t*)msg->data;
						iot_release_msg(msg); msg=NULL;

						thread_registry->on_thread_shutdown(th);
						break;
					}
					case IOT_MSG_START_MODINSTANCE: //try to start provided instance (for any type of instance)
						//instance thread
						assert(modinst!=NULL);

						if(!modinst->msgp.start) {iot_release_msg(msg, true); modinst->msgp.start=msg;} //return msg for reuse for start status
							else {assert(false);iot_release_msg(msg);}
						msg=NULL;
						modinst->start(true);
						break;
					case IOT_MSG_MODINSTANCE_STARTSTATUS: {//start attempt of provided instance made
						//main thread
						assert(uv_thread_self()==main_thread);
						assert(modinst!=NULL);
						int err=msg->intarg;
						iot_release_msg(msg); msg=NULL;

						modinst->on_start_status(err, true);
						break;
					}
					case IOT_MSG_STOP_MODINSTANCE: //stop provided instance (for any type of instance)
						//instance thread
						if(!modinst) break; //can be already free

						if(!modinst->msgp.stop) {iot_release_msg(msg, true); modinst->msgp.stop=msg; modinst->stopmsglock.clear(std::memory_order_release);} //return msg for reuse in delayed or repeated stop
							else {assert(false);iot_release_msg(msg);}
						msg=NULL;
						modinst->stop(true);
						break;
					case IOT_MSG_MODINSTANCE_STOPSTATUS: {//stop attempt of provided instance made
						//main thread
						assert(uv_thread_self()==main_thread);
						assert(modinst!=NULL);
						int err=msg->intarg;

						if(!modinst->msgp.stopstatus) {iot_release_msg(msg, true); modinst->msgp.stopstatus=msg;} //return msg for reuse in modinstance delayed free
							else {assert(false);iot_release_msg(msg);}
						msg=NULL;

						modinst->on_stop_status(err, true);
						break;
					}
					case IOT_MSG_FREE_MODINSTANCE: {
						//main thread
						assert(uv_thread_self()==main_thread);

						if(modinstlk) modinstlk.unlock();
						iot_miid_t miid=msg->miid;

						iot_release_msg(msg); msg=NULL;
						modules_registry->free_modinstance(miid);
						break;
					}
					case IOT_MSG_DRVOPEN_CONNECTION: {//try to open connection to driver instance
						//data contains address of iot_connid_t structure
						iot_device_connection_t *conn=iot_find_device_conn(*(iot_connid_t*)msg->data); //also does connident.key check before getting the lock
						if(!conn) break; //connection was closed, no action required
						conn->lock();
						if(conn->connident==*(iot_connid_t*)msg->data) { //repeat check with lock
							if(!conn->driverstatus_msg) {iot_release_msg(msg, true); conn->driverstatus_msg=msg;}
								else {assert(false);iot_release_msg(msg);}
							msg=NULL;
							conn->process_connect_local(true);
						} //else connection was JUST closed, no action required
						conn->unlock();
						break;
					}
					case IOT_MSG_CONNECTION_DRVOPENSTATUS: { //send status of connection open
						//data contains address of iot_connid_t structure
						iot_device_connection_t *conn=iot_find_device_conn(*(iot_connid_t*)msg->data); //also does connident.key check before getting the lock
						if(!conn) break; //connection was closed, no action required

						//conn->lock(); //no lock necessary for main thread

						int err=msg->intarg;
						if(!conn->driverstatus_msg) {iot_release_msg(msg, true); conn->driverstatus_msg=msg;}
								else {assert(false);iot_release_msg(msg);}
						msg=NULL;
						conn->on_drvconnect_status(err, true);

						break;
					}
					case IOT_MSG_CONNECTION_DRVREADY: { //notify LOCAL consumer instance about ready driver connection
						//data contains address of iot_connid_t structure
						iot_device_connection_t *conn=iot_find_device_conn(*(iot_connid_t*)msg->data); //also does connident.key check before getting the lock
						if(!conn) break; //connection was closed, no action required
						conn->lock();
						if(conn->connident==*(iot_connid_t*)msg->data) { //repeat check with lock
							if(!conn->c2d_ready_msg) {iot_release_msg(msg, true); conn->c2d_ready_msg=msg;}
								else {assert(false);iot_release_msg(msg);}
							msg=NULL;
							conn->process_driver_ready();
						} //else connection was JUST closed, no action required
						conn->unlock();
						break;
					}
					case IOT_MSG_CONNECTION_D2C_READY: { //notify LOCAL consumer instance about available data on connection
						//data contains address of iot_connid_t structure
						iot_device_connection_t *conn=iot_find_device_conn(*(iot_connid_t*)msg->data); //also does connident.key check before getting the lock
						if(!conn) break; //connection was closed, no action required
						//state must be >= IOT_DEVCONN_READYDRV, so no lock is necessary as main thread cannot just close such connections
//						conn->lock();

						if(!conn->d2c_ready_msg) {iot_release_msg(msg, true); conn->d2c_ready_msg=msg; std::atomic_thread_fence(std::memory_order_release);}
							else {assert(false);iot_release_msg(msg);}
						msg=NULL;
						conn->on_d2c_ready();

						break;
					}
					case IOT_MSG_CONNECTION_C2D_READY: { //notify LOCAL driver instance about available data on connection
						//data contains address of iot_connid_t structure
						iot_device_connection_t *conn=iot_find_device_conn(*(iot_connid_t*)msg->data); //also does connident.key check before getting the lock
						if(!conn) break; //connection was closed, no action required
						//state must be >= IOT_DEVCONN_READYDRV, so no lock is necessary as main thread cannot just close such connections
//						conn->lock();

						if(!conn->c2d_ready_msg) {iot_release_msg(msg, true); conn->c2d_ready_msg=msg; std::atomic_thread_fence(std::memory_order_release);}
							else {assert(false);iot_release_msg(msg);}
						msg=NULL;
						conn->on_c2d_ready();

						break;
					}
					case IOT_MSG_CLOSE_CONNECTION: {
						//main thread
						assert(uv_thread_self()==main_thread);
						//data contains address of iot_connid_t structure
						iot_device_connection_t *conn=iot_find_device_conn(*(iot_connid_t*)msg->data); //also does connident.key check before getting the lock
						if(!conn) break; //connection was closed, no action required

//						if(msg==conn->clientclose_msg || msg==conn->driverclose_msg) iot_release_msg(msg, true); //just clean msg struct if one of these was used without clearing pointer
//						else
						iot_release_msg(msg);
						msg=NULL;

						conn->close();
						break;
					}
					case IOT_MSG_CONNECTION_CLOSECL: { //notify client about closed connection
						if(!modinst) break; //client can be already stopped and/or freed and thus its connection side already closed or being closed right now
						assert(uv_thread_self()==modinst->thread->thread);
						if(!modinst->is_working()) break;
						//data contains address of iot_connid_t structure
						iot_device_connection_t *conn=iot_find_device_conn(*(iot_connid_t*)msg->data); //also does connident.key check before getting the lock
						if(!conn) break; //connection was closed, no action required
						iot_release_msg(msg, true);
						conn->process_close_client(msg); //reuse msg structure. it should have been conn->closeclient_msg (now nullified)
						msg=NULL;
						break;
					}
					case IOT_MSG_CONNECTION_CLOSEDRV: { //notify driver about closed connection
						if(!modinst) break; //client can be already stopped and/or freed and thus its connection side already closed or being closed right now
						assert(uv_thread_self()==modinst->thread->thread);
						if(!modinst->is_working()) break;
						//data contains address of iot_connid_t structure
						iot_device_connection_t *conn=iot_find_device_conn(*(iot_connid_t*)msg->data); //also does connident.key check before getting the lock
						if(!conn) break; //connection was closed, no action required
						iot_release_msg(msg, true);
						conn->process_close_driver(msg); //reuse msg structure. it should have been conn->closedriver_msg (now nullified)
						msg=NULL;
						break;
					}
					case IOT_MSG_EVENTSIG_OUT: { //new signal from node about change of value output
						//main thread
						assert(uv_thread_self()==main_thread);
						assert(msg->data!=NULL && msg->is_releasable);
						iot_modelsignal* sig=static_cast<iot_modelsignal*>((iot_releasable*)msg->data);
						msg->data=NULL;

						iot_release_msg(msg); msg=NULL; //early release of message struct

#ifdef IOT_SERVER
						assert(false); //TODO
#else
						gwinstance->config_registry->inject_signals(sig);
#endif
						had_modelsignals=true;
						break;
					}
					case IOT_MSG_EVENTSIG_NOUPDATE: { //new signal from node about change of value output
						//main thread
						assert(uv_thread_self()==main_thread);
						iot_modelnegsignal neg=*(static_cast<iot_modelnegsignal*>(msg->data));
						iot_release_msg(msg); msg=NULL; //early release of message struct

#ifdef IOT_SERVER
						assert(false); //TODO
#else
						gwinstance->config_registry->inject_negative_signal(&neg);
#endif
						break;
					}
					case IOT_MSG_NETCON_STARTSTOP: { //netcon must be started/stopped
						//netcon thread
						iot_netcon *con=(iot_netcon *)msg->data;
						assert(con!=NULL);
						assert(msg==&con->com_msg);
						msg=NULL; //is static
						con->on_startstop_msg();
						break;
					}

					default:
outlog_debug("got unprocessed message %u", unsigned(msg->code));
						assert(false);
						break;
				}
			} else {
				if(!modinst) {
					iot_release_msg(msg);
					continue;
				}
				switch(msg->code) {
					case IOT_MSG_NOTIFY_INPUTSUPDATED: {
						assert(uv_thread_self()==modinst->thread->thread);
						if(!modinst->is_working()) break;
						auto model=modinst->data.node.model;
						if(!model->cfgitem) break; //model is being stopped and is already detached from config item. ignore signal.

						assert(msg->data!=NULL && msg->is_releasable);
						iot_notify_inputsupdate* notifyupdate=static_cast<iot_notify_inputsupdate*>((iot_releasable*)msg->data);

						iot_modelnegsignal neg={event_id : notifyupdate->reason_event, node_id : model->node_id};

						iot_modelsignal* result_signals=NULL;

						if(model->do_execute(msg->bytearg==1, msg, result_signals)) {
							//sync reply is ready in outsignals
							assert(msg!=NULL);
							iot_release_msg(msg, true); //leave only msg struct
							int err;
							if(result_signals) {
printf("Got new signals from node %" IOT_PRIiotid " for event %" PRIu64 "\n", model->node_id, neg.event_id.numerator);
								err=iot_prepare_msg_releasable(msg, IOT_MSG_EVENTSIG_OUT, NULL, 0, static_cast<iot_releasable*>(result_signals), 0, IOT_THREADMSG_DATAMEM_MEMBLOCK_NOOPT, true);
							} else { //empty updates
printf("Got empty update from node %" IOT_PRIiotid " for event %" PRIu64 "\n", model->node_id, neg.event_id.numerator);
								err=iot_prepare_msg(msg, IOT_MSG_EVENTSIG_NOUPDATE, NULL, 0, &neg, sizeof(neg), IOT_THREADMSG_DATAMEM_TEMP_NOALLOC, true);
							}
							assert(err==0);
							main_thread_item->send_msg(msg);
							msg=NULL;
						}

						break;
					}
					default:
outlog_debug("got unprocessed non-core message %u", unsigned(msg->code));
						break;
				}
			}
			if(msg) iot_release_msg(msg);
		}
#ifdef IOT_SERVER
		assert(false); //TODO
#else
		if(had_modelsignals) gwinstance->config_registry->commit_event();
#endif
	}

