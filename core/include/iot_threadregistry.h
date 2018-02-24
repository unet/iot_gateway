#ifndef IOT_THREADREGISTRY_H
#define IOT_THREADREGISTRY_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching

//#include <stdint.h>
#include <atomic>
#include "uv.h"

//#include<time.h>

#include "iot_core.h"

#include "iot_threadmsg.h"


extern int max_threads; //maximum threads to run (specified from command line). limited by IOT_THREADS_MAXNUM

//void kern_notifydriver_removedhwdev(iot_hwdevregistry_item_t*);


//upper limit on threads number for max_threads
#define IOT_THREADS_MAXNUM 100

#define IOT_THREAD_LOADING_MAX 1000


class iot_thread_item_t {
	friend class iot_thread_registry_t;

	static uint32_t last_thread_id;

	iot_threadmsg_t termmsg={}; //preallocated msg structire to send
	iot_thread_item_t *next=NULL, *prev=NULL; //position in iot_thread_registry_t::threads_head list
	uint32_t thread_id=0;
	uv_async_t msgq_watcher; //gets signal when new message arrives
	iot_modinstance_item_t *instances_head=NULL; //list of instances, which work (or will work after start) in this thread
	iot_modinstance_item_t *hung_instances_head=NULL; //list of instances in HUNG state
	iot_netcon *netcons_head=NULL; //list of netcons, which work (or will work after start) in this thread

	mpsc_queue<iot_threadmsg_t, iot_threadmsg_t, &iot_threadmsg_t::next> msgq;
	volatile std::atomic<uint32_t> cpu_loading={0}; //current sum of declared cpu loading. atomic is used to provide atomicity only, so memory_order_relaxed is everywhere
	bool is_shutdown=false;
	volatile std::atomic_flag shutdown_signaled=ATOMIC_FLAG_INIT;
	iot_spinlock datalock; //used to protect is_shutdown, cpu_loading counter and instances_head and netcons_head lists in [add|remove]_[modinstance|netcon] calls

	struct {
		iot_atimer timer;
		uint32_t interval;
	} atimer_pool[8]={};

	void thread_func(void);

public:
	uv_thread_t thread=0;
	uv_loop_t* loop=NULL;
	iot_memallocator* allocator=NULL;

	iot_thread_item_t(void) {
		thread_id=last_thread_id++;
	}

	int init(bool ismain=false);

	void deinit(void);
	bool is_overloaded(uint8_t cpu_loadtp);

	void send_msg(iot_threadmsg_t* msg) {
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

	void remove_modinstance(iot_modinstance_item_t* inst_item);
	bool add_modinstance(iot_modinstance_item_t* inst_item);

	void remove_netcon(iot_netcon* netcon);
	bool add_netcon(iot_netcon* netcon);

	void check_thread_load_ended(void);

private:
	void on_shutdown_msg(void);
	void shutdown_self(bool sync=false);
};


class iot_thread_registry_t {
	uv_rwlock_t threads_lock; //to access threads_head list

	iot_thread_item_t *threads_head=NULL;
	iot_thread_item_t *delthreads_head=NULL;
	int num_threads=0; //number of started additional threads (not counting main)
	iot_thread_item_t main_thread_obj;

	volatile bool is_shutdown=false;
public:	

	iot_thread_registry_t(void);
	~iot_thread_registry_t(void) {
		uv_rwlock_destroy(&threads_lock);
	}

	bool is_shutting_down(void) const {
		return is_shutdown;
	}


	static void on_thread_msg(uv_async_t* handle);

	iot_thread_item_t* find_thread(uv_thread_t th_id) {
		uv_rwlock_rdlock(&threads_lock);

		iot_thread_item_t* th=threads_head;
		while(th) {
			if(th->thread==th_id) break;
			th=th->next;
		}

		uv_rwlock_rdunlock(&threads_lock);
		return th;
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
	void shutdown(void); //do hard shutdown
	void on_thread_shutdown(iot_thread_item_t* thread);
//	void shutdown_thread(iot_thread_item_t* thread, bool sync=false);

	//finds and assigns execution thread to modinstance
	bool settle_modinstance(iot_modinstance_item_t* inst_item); //called from any thread

	//finds and assigns execution thread to netcon
	bool settle_netcon(iot_netcon* netcon); //called from any thread

private:
	iot_thread_item_t* assign_thread(uint8_t cpu_loadtp); //must be called under write lock of threads_lock!
//	void on_thread_load_ended(iot_thread_item_t* thread); //called by remove_modinstance() or remove_netcon() after removing last modinstance in shutdown mode
};



#endif //IOT_THREADREGISTRY_H
