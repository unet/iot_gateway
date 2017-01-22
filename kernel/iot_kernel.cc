#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

//#include <iot_compat.h>
//#include "evwrap.h"
#include <uv.h>
#include <ecb.h>
#include <iot_kapi.h>
#include <iot_error.h>
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
//	printf("busy HWDev removed: contype=%d, unique=%lu\n", devitem->devdata.dev_ident.contype, devitem->devdata.dev_ident.unique_refid);
//}

iot_thread_registry_t* thread_registry=NULL;
static iot_thread_registry_t _thread_registry; //instantiate singleton class
iot_thread_item_t main_thread_item;

uv_thread_t main_thread=0;
uv_loop_t *main_loop=NULL;


iot_thread_registry_t::iot_thread_registry_t(void) : threads_head(NULL) {
		assert(thread_registry==NULL);
		assert(sizeof(iot_threadmsg_t)==64);
		thread_registry=this;

		main_thread=uv_thread_self();
		main_loop=uv_default_loop();

		//fill main thread item
		memset(&main_thread_item, 0, sizeof(main_thread_item));
		main_thread_item.thread=main_thread;
		main_thread_item.loop=main_loop;
		main_thread_item.allocator=&main_allocator;
		main_thread_item.cpu_loading=IOT_THREAD_LOADING_MAIN;
		BILINKLIST_INSERTHEAD(&main_thread_item, threads_head, next, prev);

		uv_async_init(main_thread_item.loop, &main_thread_item.msgq_watcher, iot_thread_registry_t::on_thread_msg);
		main_thread_item.msgq_watcher.data=&main_thread_item;
	}

void iot_thread_registry_t::remove_modinstance(iot_modinstance_item_t* inst_item, iot_thread_item_t* thread_item) {
		assert(uv_thread_self()==main_thread);
		assert(thread_item!=NULL);
		assert(inst_item!=NULL);

		uint8_t cpu_loadtp=inst_item->cpu_loading;
		if(cpu_loadtp>=IOT_THREAD_LOADING_NUM) cpu_loadtp=0;
		assert(thread_item->cpu_loading >= iot_thread_loading[cpu_loadtp]);
		thread_item->cpu_loading-=iot_thread_loading[cpu_loadtp];
		BILINKLIST_REMOVE(inst_item, next_inthread, prev_inthread);
	}

void iot_thread_registry_t::add_modinstance(iot_modinstance_item_t* inst_item, iot_thread_item_t* thread_item) {
		assert(uv_thread_self()==main_thread);

		assert(thread_item!=NULL);
		assert(inst_item!=NULL);
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

void iot_thread_registry_t::on_thread_msg(uv_async_t* handle) { //static
		assert(uv_thread_self()==main_thread);

		iot_thread_item_t* thread_item=(iot_thread_item_t*)(handle->data);
		iot_threadmsg_t* msg, *nextmsg=thread_item->msgq.pop_all();
		while(nextmsg) {
			msg=nextmsg;
			nextmsg=(nextmsg->next).load(std::memory_order_relaxed);

			if(msg->is_kernel) { //msg for kernel
				switch(msg->code) {
					case IOT_MSG_START_MODINSTANCE: //try to start provided instance (for any type of instance)
						assert(msg->modinstance!=NULL);
						modules_registry->start_modinstance(msg->modinstance);
						break;
					case IOT_MSG_OPEN_CONNECTION: {//try to open connection to driver instance
						//bytearg contains index of connection for driver instance
						//data contains address of iot_device_connection_t structure
						assert(msg->modinstance!=NULL);
						assert(msg->modinstance->type==IOT_MODINSTTYPE_DRIVER);
						assert(msg->bytearg < sizeof(msg->modinstance->driver_conn)/sizeof(msg->modinstance->driver_conn[0]));

						iot_device_connection_t *conn=msg->modinstance->driver_conn[msg->bytearg];
						assert(conn!=NULL);
						conn->process_connect_local();
						break;
						}
					case IOT_MSG_DRV_CONNECTION_READY: //notify consumer instance about ready driver connection
						//bytearg contains index of driver connection
						assert(msg->modinstance!=NULL);
						iot_device_connection_t *conn;
						switch(msg->modinstance->type) {
							case IOT_MODINSTTYPE_EVSOURCE:
								assert(msg->bytearg < sizeof(msg->modinstance->evsrc_devconn)/sizeof(msg->modinstance->evsrc_devconn[0]));
								conn=msg->modinstance->evsrc_devconn[msg->bytearg];
								break;
							case IOT_MODINSTTYPE_DRIVER: //list all illegal types
								assert(false);
								conn=NULL;
								break;
						}
						if(conn) modules_registry->process_device_attached(conn);
						break;
				}
			} else {
				assert(msg->modinstance!=NULL);
			}
			if(msg->is_msgmemblock) iot_release_memblock(msg);
		}
	}
