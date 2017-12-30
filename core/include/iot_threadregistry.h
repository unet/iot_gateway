#ifndef IOT_THREADREGISTRY_H
#define IOT_THREADREGISTRY_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching

//#include <stdint.h>
#include <atomic>
#include "uv.h"

//#include<time.h>

#include "iot_core.h"



extern int max_threads; //maximum threads to run (specified from command line). limited by IOT_THREADS_MAXNUM

//void kern_notifydriver_removedhwdev(iot_hwdevregistry_item_t*);


//upper limit on threads number for max_threads
#define IOT_THREADS_MAXNUM 100

//#define IOT_THREADMSG_LOADING_MAX 1000


struct iot_thread_item_t {
	iot_threadmsg_t termmsg={}; //preallocated msg structire to send
	iot_thread_item_t *next=NULL, *prev=NULL; //position in iot_thread_registry_t::threads_head list
	uint32_t thread_id=0;
	uv_thread_t thread=0;
	uv_loop_t* loop=NULL;
	iot_memallocator* allocator=NULL;
	uv_async_t msgq_watcher; //gets signal when new message arrives
	iot_modinstance_item_t *instances_head=NULL; //list of instances, which work (or will work after start) in this thread
	iot_modinstance_item_t *hung_instances_head=NULL; //list of instances in HUNG state

	mpsc_queue<iot_threadmsg_t, iot_threadmsg_t, &iot_threadmsg_t::next> msgq;
	uint16_t cpu_loading=0; //current sum of declared cpu loading
	bool is_shutdown=false;


	struct {
		iot_atimer timer;
		uint32_t interval;
	} atimer_pool[8]={};

	iot_thread_item_t(void) {
		thread_id=last_thread_id++;
	}

	int init(bool ismain=false);

	void deinit(void);

	void send_msg(iot_threadmsg_t* msg) {
		if(is_shutdown) return;
		assert(msg->code!=0 && loop!=NULL);
		if(msgq.push(msg)) {
			uv_async_send(&msgq_watcher);
		}
	}
	void schedule_atimer(iot_atimer_item& it, uint64_t delay) { //schedules atimer_item to signal after delay or earlier
		assert(uv_thread_self()==main_thread);
		for(int i=sizeof(atimer_pool)/sizeof(atimer_pool[0])-1;i>=1;i--) {
			if(delay<atimer_pool[i].interval) continue;
			atimer_pool[i].timer.schedule(it);
			return;
		}
		atimer_pool[0].timer.schedule(it);
	}
private:
	static uint32_t last_thread_id;
	void thread_func(void);
};


class iot_thread_registry_t {
	iot_thread_item_t *threads_head=NULL;
	iot_thread_item_t *delthreads_head=NULL;
	int num_threads=0; //number of started additional threads (not counting main)
	iot_thread_item_t main_thread_obj;

public:	
	bool is_shutdown=false;

	iot_thread_registry_t(void);
	void remove_modinstance(iot_modinstance_item_t* inst_item);
	void add_modinstance(iot_modinstance_item_t* inst_item, iot_thread_item_t* thread_item);
	iot_thread_item_t* assign_thread(uint8_t cpu_loadtp);
	static void on_thread_msg(uv_async_t* handle);

	iot_thread_item_t* find_thread(uv_thread_t th_id) {
		assert(uv_thread_self()==main_thread);

		iot_thread_item_t* th=threads_head;
		while(th) {
			if(th->thread==th_id) return th;
			th=th->next;
		}
		return NULL;
	}
	iot_memallocator* find_allocator(uv_thread_t th_id) {
		iot_thread_item_t* th=find_thread(th_id);
		if(th) return th->allocator;
		return NULL;
	}
	uv_loop_t* find_loop(uv_thread_t th_id) {
		iot_thread_item_t* th=find_thread(th_id);
		if(th) return th->loop;
		return NULL;
	}

	void graceful_shutdown(void); //initiate graceful shutdown, stop all module instances in all threads
	void on_thread_modinstances_ended(iot_thread_item_t* thread); //called by remove_modinstance() after removing last modinstance in shutdown mode
	void on_thread_shutdown(iot_thread_item_t* thread);
};



#endif //IOT_THREADREGISTRY_H
