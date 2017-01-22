#ifndef IOT_KERNEL_H
#define IOT_KERNEL_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching

//#include <stdint.h>
//#include <atomic>
#include <uv.h>

//#include<time.h>

//#include<ecb.h>

#include <iot_kapi.h>
#include <kernel/iot_common.h>


struct iot_threadmsg_t;
struct iot_thread_item_t;
class iot_thread_registry_t;

enum iot_msg_code_t : uint16_t {
	IOT_MSG_START_MODINSTANCE, //start instance in modinstance arg. data unused. instance can be of any type.
	IOT_MSG_OPEN_CONNECTION, //open connection to driver instance in modinstance arg by calling open() method. data unused. bytearg contains index of connection in instance data
	IOT_MSG_DRV_CONNECTION_READY, //notify driver consumer instance about driver connection. data unused. bytearg contains index of connected driver in instance data
};

extern iot_thread_item_t main_thread_item; //prealloc main thread item
extern iot_thread_registry_t* thread_registry;

extern uv_loop_t *main_loop;


#include <kernel/iot_memalloc.h>
//#include <kernel/iot_deviceregistry.h>
#include <kernel/iot_moduleregistry.h>

//void kern_notifydriver_removedhwdev(iot_hwdevregistry_item_t*);

#define IOT_THREAD_LOADING_MAX 1000
#define IOT_THREAD_LOADING_MAIN (IOT_THREAD_LOADING_MAX/10)

//selects memory model for message data argument to extended version of iot_thread_item_t::send_msg
enum iot_threadmsg_datamem_t : uint8_t {
	IOT_THREADMSG_DATAMEM_STATIC, //provided data buffer points to static buffer (or is NULL), so no releasing required
	IOT_THREADMSG_DATAMEM_TEMP_NOALLOC, //provided data buffer points to temporary buffer, so it MUST fit IOT_MSG_BUFSIZE bytes or error (assert in debug) will be returned
	IOT_THREADMSG_DATAMEM_TEMP, //provided data buffer points to temporary buffer, so it either must fit IOT_MSG_BUFSIZE bytes or memory will be allocated by provided allocator
	IOT_THREADMSG_DATAMEM_MEMBLOCK_NOOPT, //provided data buffer points to buffer allocated by iot_memallocator. release will be called for it when releasing message. refcount should be increased before sending message if buffer will be used later
	IOT_THREADMSG_DATAMEM_MEMBLOCK, //provided data buffer points to buffer allocated by iot_memallocator. if its size fits IOT_MSG_BUFSIZE, buffer will be copied and released immediately. refcount should be increased before sending message if buffer will be used later
	IOT_THREADMSG_DATAMEM_MALLOC_NOOPT, //provided data buffer points to buffer allocated by malloc(). free() will be called for it when releasing message
	IOT_THREADMSG_DATAMEM_MALLOC, //provided data buffer points to buffer allocated by malloc(). if its size fits IOT_MSG_BUFSIZE, buffer will be copied and freed immediately
};

#define IOT_THREADMSG_LOADING_MAX 1000

struct iot_threadmsg_t { //this struct MUST BE 64 bytes
	volatile std::atomic<iot_threadmsg_t*> next; //points to next message in message queue
	iot_modinstance_item_t *modinstance; //msg destination instance or can be NULL for kernel with is_kernel flag set
	iot_msg_code_t code;
	uint8_t bytearg; //arbitrary byte argument for command in code
	uint8_t is_memblock:1, //flag that data pointer must be released using iot_release_memblock()
		is_msgmemblock:1, //flag that this struct must be released using iot_release_memblock(). otherwise it is not released
		is_malloc:1, //flag that data pointer must be released using free()
		is_kernel:1; //message is for kernel and thus modinstance is message argument, not destination instance
	uint32_t datasize; //size of data pointed by data. must be checked for correctness for each command during its processing
	void* data; //data corresponding to code. can point to builtin buffer if IOT_MSG_BUFSIZE is enough or be allocated as memblock or malloc
	char buf[64-(sizeof(std::atomic<iot_threadmsg_t*>)+sizeof(void*)*2+sizeof(iot_msg_code_t)+2+4)]; //must complement struct to 64 bytes!
};
#define IOT_MSG_BUFSIZE (sizeof(iot_threadmsg_t::buf)-offsetof(struct iot_threadmsg_t, buf))

struct iot_thread_item_t {
	iot_thread_item_t *next, *prev; //position in iot_thread_registry_t::threads_head list
	uv_thread_t thread;
	uv_loop_t* loop;
	iot_memallocator* allocator;
	uv_async_t msgq_watcher; //gets signal when new message arrives
	iot_modinstance_item_t *instances_head; //list of instances, which work (or will work after start) in this thread

	mpsc_queue<iot_threadmsg_t, iot_threadmsg_t, &iot_threadmsg_t::next> msgq;
	uint16_t cpu_loading; //current sum of declared cpu loading

	void send_msg(iot_threadmsg_t* msg) {
		if(msgq.push(msg)) {
			uv_async_send(&msgq_watcher);
		}
	}
};


class iot_thread_registry_t {
	iot_thread_item_t *threads_head;

public:	
	iot_thread_registry_t(void);
	void remove_modinstance(iot_modinstance_item_t* inst_item, iot_thread_item_t* thread_item);
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
};

//fills thread message struct
//'msg' arg can be NULL to request struct allocation from provided allocator (which can be NULL to request its auto selection).
//Otherwise (if struct is already allocated) it must be zeroed and 'is_msgmemblock' set correctly.
//Interprets data pointer according to datamem
//returns error if no memory or critical error (when datasize>0 but data is NULL or datamem is IOT_THREADMSG_DATAMEM_TEMP_NOALLOC
//and datasize exceedes IOT_MSG_BUFSIZE or illegal datamem)
inline int iot_prepare_msg(iot_threadmsg_t *&msg,iot_msg_code_t code, iot_modinstance_item_t* modinst, uint8_t bytearg, void* data, size_t datasize, 
		iot_threadmsg_datamem_t datamem, iot_memallocator* allocator, bool is_kernel=false) {
		
		if(!data) {
			assert(datasize==0);
			if(datasize!=0) return IOT_ERROR_CRITICAL_BUG;
		}
		bool msg_alloced=false;
		if(!msg) {
			if(!allocator) {
				allocator=thread_registry->find_allocator(uv_thread_self());
				assert(allocator!=NULL);
			}
			msg=(iot_threadmsg_t*)allocator->allocate(sizeof(iot_threadmsg_t));
			if(!msg) return IOT_ERROR_NO_MEMORY;

			memset(msg, 0, sizeof(*msg));
			msg->is_msgmemblock=1;
			msg_alloced=true;
		}
		msg->code=code;
		msg->bytearg=bytearg;
		if(is_kernel) msg->is_kernel=1;
		msg->modinstance=modinst;

		if(!data || !datasize) return 0;

		msg->datasize=datasize;

		switch(datamem) {
			case IOT_THREADMSG_DATAMEM_STATIC: //provided data buffer points to static buffer (or is NULL), so no releasing required
				msg->data=data;
				break;
			case IOT_THREADMSG_DATAMEM_TEMP_NOALLOC: //provided data buffer points to temporary buffer, so it MUST fit IOT_MSG_BUFSIZE bytes or error (assert in debug) will be returned
				if(datasize>IOT_MSG_BUFSIZE) {
					assert(false);
					if(msg_alloced) allocator->release(msg);
					return IOT_ERROR_CRITICAL_BUG;
				}
				//here datasize<=IOT_MSG_BUFSIZE
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
				msg->data=data;
				msg->is_malloc=1;
				break;
			default:
				assert(false);
				return IOT_ERROR_CRITICAL_BUG;
		}
		return 0;
	}



#endif //IOT_KERNEL_H
