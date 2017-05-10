#ifndef IOT_KERNEL_H
#define IOT_KERNEL_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching

//#include <stdint.h>
#include <atomic>
#include <uv.h>

//#include<time.h>

//#include<ecb.h>

#include <iot_module.h>
#include <kernel/iot_common.h>


struct iot_threadmsg_t;
struct iot_thread_item_t;
class iot_thread_registry_t;
struct iot_modinstance_item_t;

//list of possible codes of thread messages.
//fields from iot_threadmsg_t struct which are interpreted differently depending on message code: miid, bytearg, data
enum iot_msg_code_t : uint16_t {
	IOT_MSG_INVALID=0,				//marks invalidated content of msg structure
//kernel destined messages (is_kernel must be true)
	IOT_MSG_START_MODINSTANCE,		//start instance in [miid] arg. [data] unused. [bytearg] unused. instance can be of any type.
	IOT_MSG_MODINSTANCE_STARTSTATUS,//notification to kernel about status of modinstance start (status will be in data)
	IOT_MSG_STOP_MODINSTANCE,		//stop instance in [miid] arg. [data] unused. [bytearg] unused. instance can be of any type.
	IOT_MSG_MODINSTANCE_STOPSTATUS,	//notification to kernel about status of modinstance stop (status will be in data)
	IOT_MSG_FREE_MODINSTANCE,		//notification to kernel about unlocking last reference to modinstance structure in pending free state

	IOT_MSG_DRVOPEN_CONNECTION,		//open connection to driver instance by calling process_local_connect method. [data] contains address of connection structure
	IOT_MSG_CONNECTION_DRVOPENSTATUS,//notification to kernel about status of opening connection to driver (status will be in intarg). [data] contains address of connection structure
	IOT_MSG_CONNECTION_DRVREADY,	//notify driver consumer instance about driver connection. [data] contains address of connection structure.
	IOT_MSG_CONNECTION_D2C_READY,	//client side of connection can read data. [data] contains address of connection structure.
	IOT_MSG_CONNECTION_C2D_READY,	//driver side of connection can read data. [data] contains address of connection structure.
	IOT_MSG_CLOSE_CONNECTION,		//async call for driver connection close. [data] contains address of connection structure.
	IOT_MSG_CONNECTION_CLOSECL,		//notify driver consumer instance about driver connection close. [data] contains address of connection structure.
	IOT_MSG_CONNECTION_CLOSEDRV,	//notify driver instance about driver connection close. [data] contains address of connection structure.

	IOT_MSG_THREAD_SHUTDOWN,		//process shutdown of thread (break event loop and exit thread)
	IOT_MSG_THREAD_SHUTDOWNREADY,	//notification to main thread about child thread stop. [data] contains thread item address
};

extern iot_thread_item_t* main_thread_item; //prealloc main thread item
extern iot_thread_registry_t* thread_registry;

extern uv_loop_t *main_loop;
extern volatile sig_atomic_t need_exit;
extern int max_threads; //maximum threads to run (specified from command line). limited by IOT_THREADS_MAXNUM

//void kern_notifydriver_removedhwdev(iot_hwdevregistry_item_t*);

#define IOT_THREAD_LOADING_MAX 1000
#define IOT_THREAD_LOADING_MAIN (IOT_THREAD_LOADING_MAX/10)

//upper limit on threads number for max_threads
#define IOT_THREADS_MAXNUM 100

//selects memory model for message data argument to extended version of iot_thread_item_t::send_msg
enum iot_threadmsg_datamem_t : uint8_t {
	IOT_THREADMSG_DATAMEM_STATIC, //provided data buffer points to static buffer (or datasize is zero and data is arbitraty integer), so no releasing required
	IOT_THREADMSG_DATAMEM_TEMP_NOALLOC, //provided data buffer points to temporary buffer, so it MUST fit IOT_MSG_BUFSIZE bytes or error (assert in debug) will be returned
	IOT_THREADMSG_DATAMEM_TEMP, //provided data buffer points to temporary buffer, so it either must fit IOT_MSG_BUFSIZE bytes or memory will be allocated by provided allocator
	IOT_THREADMSG_DATAMEM_MEMBLOCK_NOOPT, //provided data buffer points to buffer allocated by iot_memallocator. release will be called for it when releasing message. refcount should be increased before sending message if buffer will be used later. datasize is not engaged anywhere, so can be arbitrary
	IOT_THREADMSG_DATAMEM_MEMBLOCK, //provided data buffer points to buffer allocated by iot_memallocator. if its size fits IOT_MSG_BUFSIZE, buffer will be copied and released immediately. refcount should be increased before sending message if buffer will be used later
	IOT_THREADMSG_DATAMEM_MALLOC_NOOPT, //provided data buffer points to buffer allocated by malloc(). free() will be called for it when releasing message. datasize is not engaged anywhere, so can be arbitrary
	IOT_THREADMSG_DATAMEM_MALLOC, //provided data buffer points to buffer allocated by malloc(). if its size fits IOT_MSG_BUFSIZE, buffer will be copied and freed immediately
};

//#define IOT_THREADMSG_LOADING_MAX 1000

struct iot_threadmsg_t { //this struct MUST BE 64 bytes
	volatile std::atomic<iot_threadmsg_t*> next; //points to next message in message queue
	iot_miid_t miid; //msg destination instance or can be 0 for kernel with is_kernel flag set
	iot_msg_code_t code;
	uint8_t bytearg; //arbitrary byte argument for command in code
	uint8_t is_memblock:1, //flag that DATA pointer must be released using iot_release_memblock(), conflicts with 'is_malloc'
		is_malloc:1, //flag that DATA pointer must be released using free(), conflicts with 'is_memblock'

		is_msgmemblock:1, //flag that THIS struct must be released using iot_release_memblock(), conflicts with 'is_msginstreserv'
//		is_msginstreserv:1, //flag that THIS struct is from module instance reserv ('msgstructs' array) and must be just marked as free in 'msgstructs_usage',
//							//conflicts with 'is_msgmemblock'

		is_kernel:1; //message is for kernel and thus miid is message argument, not destination instance
	uint32_t datasize; //size of data pointed by data. must be checked for correctness for each command during its processing. Should be zero if
					//'data' is used to store arbitraty integer.
	void* data; //data corresponding to code. can point to builtin buffer if IOT_MSG_BUFSIZE is enough or be allocated as memblock (is_memblock==1) or
				//malloc (is_malloc==1). Can be used to store arbitrary integer value (up to 32 bit for compatibility) when both is_memblock and is_malloc are 0.

	char buf[64-(sizeof(std::atomic<iot_threadmsg_t*>)+sizeof(void*)+sizeof(iot_miid_t)+sizeof(iot_msg_code_t)+2+4+sizeof(int))]; //must complement struct to 64 bytes!
	int intarg; //arbitrary int argument for command in code when datasize is 0 or <= IOT_MSG_INTARG_SAFEDATASIZE.

	bool is_free(void) volatile { //for use with static msg structs to determine is struct is not in use just now
		return code==IOT_MSG_INVALID;
	}
};
#define IOT_MSG_BUFSIZE (sizeof(iot_threadmsg_t)-offsetof(struct iot_threadmsg_t, buf))
#define IOT_MSG_INTARG_SAFEDATASIZE (offsetof(struct iot_threadmsg_t, intarg) - offsetof(struct iot_threadmsg_t, buf))



#include <kernel/iot_memalloc.h>
//#include <kernel/iot_deviceregistry.h>
#include <kernel/iot_moduleregistry.h>

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

//fills thread message struct
//'msg' arg can be NULL to request struct allocation from provided allocator (which can be NULL to request its auto selection).
//Otherwise (if struct is already allocated) it must be zeroed and 'is_msgmemblock' set correctly.
//Interprets data pointer according to datamem
//returns error if no memory or critical error (when datasize>0 but data is NULL or datamem is IOT_THREADMSG_DATAMEM_TEMP_NOALLOC
//and datasize exceedes IOT_MSG_BUFSIZE or illegal datamem)
int iot_prepare_msg(iot_threadmsg_t *&msg,iot_msg_code_t code, iot_modinstance_item_t* modinst, uint8_t bytearg, void* data, size_t datasize, 
		iot_threadmsg_datamem_t datamem, bool is_kernel=false, iot_memallocator* allocator=NULL);

void iot_release_msg(iot_threadmsg_t *msg, bool = false);

void iot_process_module_bug(iot_module_item_t *module);

#endif //IOT_KERNEL_H
