#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

//#include <iot_compat.h>
//#include "evwrap.h"
#include <uv.h>
#include <ecb.h>
#include <iot_module.h>
#include <kernel/iot_daemonlib.h>
#include <kernel/iot_kernel.h>

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
iot_thread_item_t main_thread_item;

uv_thread_t main_thread=0;
uv_loop_t *main_loop=NULL;
volatile sig_atomic_t need_exit=0; //1 means graceful exit after getting SIGTERM or SIGUSR1, 2 means urgent exit after SIGINT or SIGQUIT

void iot_thread_item_t::init(uv_thread_t thread_, uv_loop_t* loop_, iot_memallocator* allocator_) {
		memset(this, 0, sizeof(*this));
		thread=thread_;
		loop=loop_;
		allocator=allocator_;

		uv_async_init(loop, &msgq_watcher, iot_thread_registry_t::on_thread_msg);
		msgq_watcher.data=this;

		uint32_t interval=1*1000; //interval of first timer in milliseconds
		for(unsigned i=0;i<sizeof(atimer_pool)/sizeof(atimer_pool[0]);i++) {
			atimer_pool[i].timer.init(interval, loop);
			interval*=2;
		}
	}


iot_thread_registry_t::iot_thread_registry_t(void) : threads_head(NULL) {
		assert(thread_registry==NULL);
		assert(sizeof(iot_threadmsg_t)==64);
		thread_registry=this;

		main_thread=uv_thread_self();
		main_loop=uv_default_loop();

		//fill main thread item
		main_thread_item.init(main_thread, main_loop, &main_allocator);
		main_thread_item.cpu_loading=IOT_THREAD_LOADING_MAIN;
		BILINKLIST_INSERTHEAD(&main_thread_item, threads_head, next, prev);
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

		if(cpu_loadtp<3) {
			//find thread with minimum loading
			iot_thread_item_t* minthread=NULL;
			uint16_t minload=IOT_THREAD_LOADING_MAX;
			iot_thread_item_t* it=threads_head;
			while(it) {
				if(it->cpu_loading<minload)	{minload=it->cpu_loading;minthread=it;}
				it=it->next;
			}
			if(minthread && minload+iot_thread_loading[cpu_loadtp]<=IOT_THREAD_LOADING_MAX) return minthread;
		}
		//here new thread must be started
		//TODO
		return &main_thread_item;
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
		assert(is_shutdown);
		if(thread!=&main_thread_item) {
			thread->deinit();
			BILINKLIST_REMOVE(thread, next, prev);
		}
		if(threads_head==&main_thread_item && main_thread_item.next==NULL && !main_thread_item.instances_head) { //only main thread left
			//process all messages currently in msg queue
			on_thread_msg(&main_thread_item.msgq_watcher);
			modules_registry->graceful_shutdown();
		}
	}


void iot_thread_registry_t::on_thread_msg(uv_async_t* handle) { //static
		iot_thread_item_t* thread_item=(iot_thread_item_t*)(handle->data);
		iot_threadmsg_t* msg, *nextmsg=thread_item->msgq.pop_all();
		while(nextmsg) {
			msg=nextmsg;
			nextmsg=(nextmsg->next).load(std::memory_order_relaxed);
			iot_modinstance_locker modinstlk=modules_registry->get_modinstance(msg->miid);
			iot_modinstance_item_t* modinst=modinstlk.modinst;

			if(msg->is_kernel) { //msg for kernel
				switch(msg->code) {
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
						assert(modinst!=NULL);

						if(!modinst->msgp.stop) {iot_release_msg(msg, true); modinst->msgp.stop=msg;} //return msg for reuse in delayed or repeated stop
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
						if(modinstlk) modinstlk.unlock();
						iot_miid_t miid=msg->miid;

						iot_release_msg(msg); msg=NULL;
						modinst->stop(true);

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

						if(!conn->d2c_ready_msg) {iot_release_msg(msg, true); conn->d2c_ready_msg=msg;}
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

						if(!conn->c2d_ready_msg) {iot_release_msg(msg, true); conn->c2d_ready_msg=msg;}
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
						//data contains address of iot_connid_t structure
						iot_device_connection_t *conn=iot_find_device_conn(*(iot_connid_t*)msg->data); //also does connident.key check before getting the lock
						if(!conn) break; //connection was closed, no action required
						iot_release_msg(msg, true);
						conn->process_close_client(msg); //reuse msg structure. it should have been conn->closeclient_msg (now nullified)
						break;
					}
					case IOT_MSG_CONNECTION_CLOSEDRV: { //notify driver about closed connection
						//data contains address of iot_connid_t structure
						iot_device_connection_t *conn=iot_find_device_conn(*(iot_connid_t*)msg->data); //also does connident.key check before getting the lock
						if(!conn) break; //connection was closed, no action required
						iot_release_msg(msg, true);
						conn->process_close_driver(msg); //reuse msg structure. it should have been conn->closedriver_msg (now nullified)
						break;
					}

					default:
outlog_debug("got unprocessed message %u", unsigned(msg->code));
						assert(false);
						break;
				}
			} else {
outlog_debug("got non-kernel message %u", unsigned(msg->code));
				assert(modinst!=NULL);
			}
			if(msg) iot_release_msg(msg);
		}
	}

//Return values:
//0 - success
//IOT_ERROR_CRITICAL_BUG - in release mode assertion failed
//IOT_ERROR_NO_MEMORY
int iot_prepare_msg(iot_threadmsg_t *&msg,iot_msg_code_t code, iot_modinstance_item_t* modinst, uint8_t bytearg, void* data, size_t datasize, 
		iot_threadmsg_datamem_t datamem, bool is_kernel, iot_memallocator* allocator) {
		
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
		if(is_kernel) msg->is_kernel=1;

		return 0;
errexit:
		if(msg_alloced) allocator->release(msg);
		return IOT_ERROR_CRITICAL_BUG;
	}


void iot_release_msg(iot_threadmsg_t *msg, bool nofree_msgmemblock) { //nofree_msgmemblock if true, then msg struct with is_msgmemblock set is not released (only cleared)
	if(msg->code!=IOT_MSG_INVALID) { //content is valid, so must be released
		if(msg->is_memblock) {
			assert(msg->is_malloc==0);
			iot_release_memblock(msg->data);
			msg->is_memblock=0;
		} else if(msg->is_malloc) {
			free(msg->data);
			msg->is_malloc=0;
		}
		msg->data=NULL;
		msg->bytearg=0;
		msg->intarg=0;
//		if(!msg->is_msginstreserv) msg->miid.clear();
		msg->datasize=0;
		msg->is_kernel=0;
		msg->code=IOT_MSG_INVALID;
	}
	if(msg->is_msgmemblock) {
//		assert(msg->is_msginstreserv==0);
		if(!nofree_msgmemblock) iot_release_memblock(msg);
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
void iot_process_module_bug(iot_module_item_t *module) {
	//TODO
}
