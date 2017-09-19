#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

//#include "iot_compat.h"
//#include "evwrap.h"
#include "uv.h"
#include "iot_module.h"
#include "iot_daemonlib.h"
#include "iot_core.h"

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

iot_thread_registry_t* thread_registry=NULL;
static iot_thread_registry_t _thread_registry; //instantiate singleton class
iot_thread_item_t* main_thread_item=NULL;

uv_thread_t main_thread=0;
uv_loop_t *main_loop=NULL;
volatile sig_atomic_t need_exit=0; //1 means graceful exit after getting SIGTERM or SIGUSR1, 2 means urgent exit after SIGINT or SIGQUIT
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
	allocator->set_thread(thread);
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
	assert(this==main_thread_item || !thread); //non-main thread must be already stopped and destroyed
	if(loop) {
		for(unsigned i=0;i<sizeof(atimer_pool)/sizeof(atimer_pool[0]);i++) {
			atimer_pool[i].timer.deinit();
		}
		uv_close((uv_handle_t*)&msgq_watcher, NULL);
	}
	if(this!=main_thread_item) {
		if(loop) {
			uv_walk(loop, [](uv_handle_t* handle, void* arg) -> void {if(!uv_is_closing(handle)) {uv_close(handle, NULL);}}, NULL);
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
			allocator->set_thread(main_thread);
//			delete allocator;
//			allocator=NULL;
		}
	}
}

iot_thread_registry_t::iot_thread_registry_t(void) {
		assert(thread_registry==NULL);
		assert(sizeof(iot_threadmsg_t)==64);
		thread_registry=this;

		main_thread=uv_thread_self();
		main_loop=uv_default_loop();

		//fill main thread item
		main_thread_item=&main_thread_obj;
		main_thread_item->init(true);
		main_thread_item->cpu_loading=IOT_THREAD_LOADING_MAIN;
		BILINKLIST_INSERTHEAD(main_thread_item, threads_head, next, prev);
	}

void iot_thread_registry_t::remove_modinstance(iot_modinstance_item_t* inst_item) { //hang instances are moved to hang list
		assert(uv_thread_self()==main_thread);
		assert(inst_item!=NULL);

		iot_thread_item_t* thread_item=inst_item->thread;
		inst_item->thread=NULL;
		assert(thread_item!=NULL);

		uint8_t cpu_loadtp=inst_item->cpu_loading;
		if(cpu_loadtp>=IOT_THREAD_LOADING_NUM) cpu_loadtp=0;
		assert(thread_item->cpu_loading >= iot_thread_loading[cpu_loadtp]);
		thread_item->cpu_loading-=iot_thread_loading[cpu_loadtp];
		BILINKLIST_REMOVE(inst_item, next_inthread, prev_inthread);

		if(inst_item->state==IOT_MODINSTSTATE_HUNG) BILINKLIST_INSERTHEAD(inst_item, thread_item->hung_instances_head, next_inthread, prev_inthread);

		if(is_shutdown && !thread_item->instances_head) on_thread_modinstances_ended(thread_item);
	}

void iot_thread_registry_t::add_modinstance(iot_modinstance_item_t* inst_item, iot_thread_item_t* thread_item) {
		assert(uv_thread_self()==main_thread);

		assert(inst_item!=NULL);
		assert(thread_item!=NULL);
		assert(inst_item->thread==NULL);
		inst_item->thread=thread_item;

		uint8_t cpu_loadtp=inst_item->cpu_loading;
		if(cpu_loadtp>=IOT_THREAD_LOADING_NUM) cpu_loadtp=0;
		thread_item->cpu_loading+=iot_thread_loading[cpu_loadtp];
		BILINKLIST_INSERTHEAD(inst_item, thread_item->instances_head, next_inthread, prev_inthread);
	}

iot_thread_item_t* iot_thread_registry_t::assign_thread(uint8_t cpu_loadtp){
		assert(uv_thread_self()==main_thread);

		if(cpu_loadtp>=IOT_THREAD_LOADING_NUM) cpu_loadtp=0;

		//find thread with minimum loading
		iot_thread_item_t* minthread=NULL;
		uint16_t minload=IOT_THREAD_LOADING_MAX;
		iot_thread_item_t* it=threads_head;
		while(it) {
			if(!minthread || it->cpu_loading<minload) {minload=it->cpu_loading;minthread=it;}
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
		//TODO
		iot_thread_item_t* thitem=new iot_thread_item_t;
		if(!thitem) goto onerr;
		if(thitem->init()) goto onerr;
		BILINKLIST_INSERTHEAD(thitem, threads_head, next, prev);
		num_threads++;
		outlog_debug("New thread created with ID %u", thitem->thread_id);
		return thitem;

onerr:
		outlog_notice("Cannot create new thread, using existing. Per-thread load exceeded.");
		return minthread;
	}

void iot_thread_registry_t::graceful_shutdown(void) { //initiate graceful shutdown, stop all module instances in all threads
		assert(!is_shutdown);
		is_shutdown=true;
		iot_thread_item_t* th=threads_head;
		while(th) {
			iot_modinstance_item_t* modinst, *nextmodinst=th->instances_head;
			if(nextmodinst) {
				//send stop request to all module instances
				while((modinst=nextmodinst)) {
					nextmodinst=modinst->next_inthread;
	
					modinst->stop(false);
				}
			} else { //no started modinstances
				on_thread_modinstances_ended(th);
			}
			th=th->next;
		}
	}
void iot_thread_registry_t::on_thread_modinstances_ended(iot_thread_item_t* thread) { //called by remove_modinstance() after removing last modinstance in shutdown mode
		assert(uv_thread_self()==main_thread);
		assert(is_shutdown);
		if(thread!=main_thread_item) {
			assert(!thread->is_shutdown);
			//send msg to exit
			iot_threadmsg_t* msg=&thread->termmsg;
			int err=iot_prepare_msg(msg, IOT_MSG_THREAD_SHUTDOWN, NULL, 0, NULL, 0, IOT_THREADMSG_DATAMEM_STATIC, true);
			if(err) {
				assert(false);
			} else {
				thread->send_msg(msg);
				thread->is_shutdown=true;
			}
		} else {
			on_thread_shutdown(main_thread_item);
		}
	}

//notification from non-main thread about termination
void iot_thread_registry_t::on_thread_shutdown(iot_thread_item_t* thread) {
	assert(uv_thread_self()==main_thread);

	outlog_debug("Thread with ID %u shut down", thread->thread_id);

	if(thread!=main_thread_item) {
		int err=uv_thread_join(&thread->thread);
		assert(err==0);
		thread->thread=0;

		thread->deinit();

		assert(num_threads>0);
		num_threads--;
	}
	BILINKLIST_REMOVE_NOCL(thread, next, prev);
	BILINKLIST_INSERTHEAD(thread, delthreads_head, next, prev); //TODO dealloc closed threads later if necesary

	if(!threads_head) { //main thread is the last one
		//process all messages currently in msg queue
		main_thread_item->is_shutdown=true;
		on_thread_msg(&main_thread_item->msgq_watcher);

		main_thread_item->deinit();
		modules_registry->graceful_shutdown();
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
						assert(uv_thread_self()!=main_thread);

						iot_release_msg(msg); msg=NULL;

						thread_item->is_shutdown=true;
						uv_stop (thread_item->loop);
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

						config_registry->inject_signals(sig);
						had_modelsignals=true;
						break;
					}
					case IOT_MSG_EVENTSIG_NOUPDATE: { //new signal from node about change of value output
						//main thread
						assert(uv_thread_self()==main_thread);
						iot_modelnegsignal neg=*(static_cast<iot_modelnegsignal*>(msg->data));
						iot_release_msg(msg); msg=NULL; //early release of message struct

						config_registry->inject_negative_signal(&neg);
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
		if(had_modelsignals) config_registry->commit_event();
	}

//Return values:
//0 - success
//IOT_ERROR_CRITICAL_BUG - in release mode assertion failed
//IOT_ERROR_NO_MEMORY
int iot_prepare_msg(iot_threadmsg_t *&msg,iot_msg_code_t code, iot_modinstance_item_t* modinst, uint8_t bytearg, void* data, size_t datasize, 
		iot_threadmsg_datamem_t datamem, bool is_core, iot_memallocator* allocator) {
		
		if(!data) {
			assert(datasize==0);
			if(datasize!=0) return IOT_ERROR_CRITICAL_BUG;
			datamem=IOT_THREADMSG_DATAMEM_STATIC;
		} else if(datasize==0) {
			//check that datamem is allowed with zero datasize
			if(datamem!=IOT_THREADMSG_DATAMEM_STATIC && datamem!=IOT_THREADMSG_DATAMEM_MEMBLOCK_NOOPT && datamem!=IOT_THREADMSG_DATAMEM_MALLOC_NOOPT) {
				assert(false);
				return IOT_ERROR_CRITICAL_BUG;
			}
		}
		assert(code != IOT_MSG_INVALID);
		assert(!modinst || modinst->get_miid());

		bool msg_alloced=false;
		if(!msg) {
			if(!allocator) {
				allocator=thread_registry->find_allocator(uv_thread_self());
				assert(allocator!=NULL);
			}

			msg=allocator->allocate_threadmsg(); //sets is_msgmemblock. other fields zeroed
			if(!msg) return IOT_ERROR_NO_MEMORY;

			msg_alloced=true;

		} else {
			//structure must be zeroed or with released content
			assert(msg->code==IOT_MSG_INVALID);
			if(msg->code!=IOT_MSG_INVALID) return IOT_ERROR_CRITICAL_BUG;

/*			if(msg->is_msginstreserv) {
				assert(modinst->get_miid()==msg->miid);
				if(modinst->get_miid()!=msg->miid) return IOT_ERROR_CRITICAL_BUG;
			}*/
		}

		switch(datamem) {
			case IOT_THREADMSG_DATAMEM_STATIC: //provided data buffer points to static buffer or is arbitrary integer, so no releasing required
				//datasize can be zero here
				msg->data=data;
				break;
			case IOT_THREADMSG_DATAMEM_TEMP_NOALLOC: //provided data buffer points to temporary buffer, so it MUST fit IOT_MSG_BUFSIZE bytes or error (assert in debug) will be returned
				if(datasize>IOT_MSG_BUFSIZE) {
					assert(false);
					goto errexit;
				}
				//here 0 < datasize <= IOT_MSG_BUFSIZE
				msg->data=msg->buf;
				memcpy(msg->buf, data, datasize);
				break;
			case IOT_THREADMSG_DATAMEM_TEMP: //provided data buffer points to temporary buffer, so it either must fit IOT_MSG_BUFSIZE bytes or memory will be allocated by provided allocator
				if(datasize<=IOT_MSG_BUFSIZE) {
					msg->data=msg->buf;
					memcpy(msg->buf, data, datasize);
					break;
				}
				if(!allocator) {
					allocator=thread_registry->find_allocator(uv_thread_self());
					assert(allocator!=NULL);
				}
				msg->data=allocator->allocate(datasize, true);
				if(!msg->data) {
					if(msg_alloced) allocator->release(msg);
					return IOT_ERROR_NO_MEMORY;
				}
				memcpy(msg->data, data, datasize);
				msg->is_memblock=1;
				break;
			case IOT_THREADMSG_DATAMEM_MEMBLOCK: //provided data buffer points to buffer allocated by iot_memallocator. if its size fits IOT_MSG_BUFSIZE, buffer will be copied and released immediately. refcount should be increased before sending message if buffer will be used later
				if(datasize<=IOT_MSG_BUFSIZE) {
					msg->data=msg->buf;
					memcpy(msg->buf, data, datasize);
					iot_release_memblock(data);
					break;
				}
				//go on with IOT_THREADMSG_DATAMEM_MEMBLOCK_NOOPT case
			case IOT_THREADMSG_DATAMEM_MEMBLOCK_NOOPT: //provided data buffer points to buffer allocated by iot_memallocator. release will be called for it when releasing message. refcount should be increased before sending message if buffer will be used later
				//datasize can be zero here
				msg->data=data;
				msg->is_memblock=1;
				break;
			case IOT_THREADMSG_DATAMEM_MALLOC: //provided data buffer points to buffer allocated by malloc(). if its size fits IOT_MSG_BUFSIZE, buffer will be copied and freed immediately
				if(datasize<=IOT_MSG_BUFSIZE) {
					msg->data=msg->buf;
					memcpy(msg->buf, data, datasize);
					free(data);
					break;
				}
				//go on with IOT_THREADMSG_DATAMEM_MALLOC_NOOPT case
			case IOT_THREADMSG_DATAMEM_MALLOC_NOOPT: //provided data buffer points to buffer allocated by malloc(). free() will be called for it when releasing message
				//datasize can be zero here
				msg->data=data;
				msg->is_malloc=1;
				break;
			default:
				assert(false);
				goto errexit;
		}
		msg->code=code;
		msg->bytearg=bytearg;
		if(modinst) msg->miid=modinst->get_miid();
			else msg->miid.clear();
		msg->datasize=datasize;
		if(is_core) msg->is_core=1;

		return 0;
errexit:
		if(msg_alloced) allocator->release(msg);
		return IOT_ERROR_CRITICAL_BUG;
	}


void iot_release_msg(iot_threadmsg_t *&msg, bool nofree_msgmemblock) { //nofree_msgmemblock if true, then msg struct with is_msgmemblock set is not released (only cleared)
	if(msg->code!=IOT_MSG_INVALID) { //content is valid, so must be released
		if(msg->data) { //data pointer can and must be cleared before calling this method to preserve data
			if(msg->is_releasable) { //object in data was saved as iot_releasable derivative, so its internal data must be released
				iot_releasable *rel=(iot_releasable *)msg->data;
				rel->releasedata();
				msg->is_releasable=0;
			}
			if(msg->is_memblock) {
				assert(msg->is_malloc==0);
				iot_release_memblock(msg->data);
				msg->is_memblock=0;
			} else if(msg->is_malloc) {
				free(msg->data);
				msg->is_malloc=0;
			}
			msg->data=NULL;
		} else {
			msg->is_releasable=msg->is_memblock=msg->is_malloc=0;
		}
		msg->bytearg=0;
		msg->intarg=0;
//		if(!msg->is_msginstreserv) msg->miid.clear();
		msg->datasize=0;
		msg->is_core=0;
		msg->code=IOT_MSG_INVALID;
	}
	if(msg->is_msgmemblock) {
//		assert(msg->is_msginstreserv==0);
		if(!nofree_msgmemblock) {iot_release_memblock(msg);msg=NULL;}
		return;
	}
	assert(!nofree_msgmemblock); //do not allow this options to be passed for non-msgmemblock allocated structs. this can show on mistake
//	if(msg->is_msginstreserv) {
//		iot_modinstance_locker l=modules_registry->get_modinstance(msg->miid);
//		if(l) l.modinst->release_msgreserv(msg);
//		return;
//	}
	//here msg struct must be statically allocated
}

//marks module (its current version) as buggy across restarts. Schedules restart of program
void iot_process_module_bug(iot_any_module_item_t *module_item) {
	//TODO
}
