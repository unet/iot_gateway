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


#define IOT_THREAD_LOADING_MAX 1000
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
		if(thitem->init()) {delete thitem; goto onerr;}
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
					case IOT_MSG_NETCON_START: { //peercon must be started
						//peercon thread
						iot_netcon *con=(iot_netcon *)msg->data;
						assert(con!=NULL);
						iot_release_msg(msg); msg=NULL; //early release of message struct
						con->start_msg=NULL; //mark that no references to connection left in message struct
						con->do_start_uv();
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

