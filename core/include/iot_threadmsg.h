#ifndef IOT_THREADMSG_H
#define IOT_THREADMSG_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching

//#include <stdint.h>
#include <atomic>
#include "uv.h"

//#include<time.h>

#include "iot_core.h"


//list of possible codes of thread messages.
//fields from iot_threadmsg_t struct which are interpreted differently depending on message code: miid, bytearg, data
enum iot_msg_code_t : uint16_t {
	IOT_MSG_INVALID=0,				//marks invalidated content of msg structure
//core destined messages (is_core must be true)
	IOT_MSG_START_MODINSTANCE,		//start instance in [miid] arg. [data] unused. [bytearg] unused. instance can be of any type.
	IOT_MSG_MODINSTANCE_STARTSTATUS,//notification to core about status of modinstance start (status will be in data)
	IOT_MSG_STOP_MODINSTANCE,		//stop instance in [miid] arg. [data] unused. [bytearg] unused. instance can be of any type.
	IOT_MSG_MODINSTANCE_STOPSTATUS,	//notification to core about status of modinstance stop (status will be in data)
	IOT_MSG_FREE_MODINSTANCE,		//notification to core about unlocking last reference to modinstance structure in pending free state

	IOT_MSG_DRVOPEN_CONNECTION,		//open connection to driver instance by calling process_local_connect method. [data] contains address of connection structure
	IOT_MSG_CONNECTION_DRVOPENSTATUS,//notification to core about status of opening connection to driver (status will be in intarg). [data] contains address of connection structure
	IOT_MSG_CONNECTION_DRVREADY,	//notify driver consumer instance about driver connection. [data] contains address of connection structure.
	IOT_MSG_CONNECTION_D2C_READY,	//client side of connection can read data. [data] contains address of connection structure.
	IOT_MSG_CONNECTION_C2D_READY,	//driver side of connection can read data. [data] contains address of connection structure.
	IOT_MSG_CLOSE_CONNECTION,		//async call for driver connection close. [data] contains address of connection structure.
	IOT_MSG_CONNECTION_CLOSECL,		//notify driver consumer instance about driver connection close. [data] contains address of connection structure.
	IOT_MSG_CONNECTION_CLOSEDRV,	//notify driver instance about driver connection close. [data] contains address of connection structure.

	IOT_MSG_THREAD_SHUTDOWN,		//process shutdown of thread (break event loop and exit thread)
	IOT_MSG_THREAD_SHUTDOWNREADY,	//notification to main thread about child thread stop. [data] contains thread item address

	IOT_MSG_EVENTSIG_OUT,			//notification to config modeller about change of output value or new output msg. [data] contains iot_modelsignal pointer.
	IOT_MSG_EVENTSIG_NOUPDATE,		//notification to config modeller that sync execution of node changed NO outputs. [data] contains iot_modelnegsignal pointer.

	IOT_MSG_NETCON_STARTSTOP,			//request to working thread of peer connection to start work. [data] contains iot_peercon pointer.
//	IOT_MSG_NETCON_STOP,			//request to working thread of peer connection to start work. [data] contains iot_peercon pointer.
//	IOT_MSG_NETCON_STOPPED,			//request to working thread of peer connection to start work. [data] contains iot_peercon pointer.

//node instance destined messages
	IOT_MSG_NOTIFY_INPUTSUPDATED,	//notification to modinstance about change of input value(s) and/or new input msg(s). [data] contains iot_notify_inputsupdate
									//pointer. [bytearg] contains sync mode at the time of message generation in main thread
};

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
	iot_miid_t miid; //msg destination instance or can be 0 for core with is_core flag set
	iot_msg_code_t code;
	uint8_t bytearg; //arbitrary byte argument for command in code
	uint8_t is_memblock:1,	//flag that DATA pointer must be released using iot_release_memblock(), conflicts with 'is_malloc'
		is_malloc:1,		//flag that DATA pointer must be released using free(), conflicts with 'is_memblock'
		is_releasable:1,	//flag that DATA points to iot_releasable-derived class and its releasedata() method must be called when releasing msg struct.
							//(iot_releasable*) POINTER MUST BE PROVIDED for DATA pointer!!!

		is_msgmemblock:1, //flag that THIS struct must be released using iot_release_memblock(), conflicts with 'is_msginstreserv'
//		is_msginstreserv:1, //flag that THIS struct is from module instance reserv ('msgstructs' array) and must be just marked as free in 'msgstructs_usage',
//							//conflicts with 'is_msgmemblock'

		is_core:1; //message is for core and thus miid is message argument, not destination instance

	uint32_t datasize; //size of data pointed by data. must be checked for correctness for each command during its processing. Should be zero if
					//'data' is used to store arbitraty integer.
	void* data; //data corresponding to code. can point to builtin buffer if IOT_MSG_BUFSIZE is enough or be allocated as memblock (is_memblock==1) or
				//malloc (is_malloc==1). Can be used to store arbitrary integer value (up to 32 bit for compatibility) when both is_memblock and is_malloc are 0.

	char buf[64-(sizeof(std::atomic<iot_threadmsg_t*>)+sizeof(void*)+sizeof(iot_miid_t)+sizeof(iot_msg_code_t)+2+4+sizeof(int))]; //must complement struct to 64 bytes!
	int intarg; //arbitrary int argument for command in code when datasize is 0 or <= IOT_MSG_INTARG_SAFEDATASIZE.

	void clear(void) { //zero's structure so that it looks like free
		memset(this, 0, sizeof(*this));
		//IOT_MSG_INVALID has zero value, so no need to assign
	}
	bool is_free(void) volatile { //for use with static msg structs to determine is struct is not in use just now
		return code==IOT_MSG_INVALID;
	}
	void set_next(iot_threadmsg_t* n) { //shortcut for relaxed access of next pointer outside message queue position
		next.store(n, std::memory_order_relaxed);
	}
	iot_threadmsg_t* get_next(void) const { //shortcut for relaxed access of next pointer outside message queue position
		return next.load(std::memory_order_relaxed);
	}

	void clear_in_queue(void) {}
	void set_in_queue(void) {}
	constexpr bool check_in_queue(void) {
		return false;
	}

};
#define IOT_MSG_BUFSIZE (sizeof(iot_threadmsg_t)-offsetof(struct iot_threadmsg_t, buf))
#define IOT_MSG_INTARG_SAFEDATASIZE (offsetof(struct iot_threadmsg_t, intarg) - offsetof(struct iot_threadmsg_t, buf))



#include "iot_memalloc.h"

//fills thread message struct
//'msg' arg can be NULL to request struct allocation from provided allocator (which can be NULL to request its auto selection).
//Otherwise (if struct is already allocated) it must be zeroed and 'is_msgmemblock' set correctly.
//Interprets data pointer according to datamem
//returns error if no memory or critical error (when datasize>0 but data is NULL or datamem is IOT_THREADMSG_DATAMEM_TEMP_NOALLOC
//and datasize exceedes IOT_MSG_BUFSIZE or illegal datamem)
int iot_prepare_msg(iot_threadmsg_t *&msg,iot_msg_code_t code, iot_modinstance_item_t* modinst, uint8_t bytearg, void* data, size_t datasize, 
		iot_threadmsg_datamem_t datamem, bool is_core=false, iot_memallocator* allocator=NULL);

inline int iot_prepare_msg_releasable(iot_threadmsg_t *&msg,iot_msg_code_t code, iot_modinstance_item_t* modinst, uint8_t bytearg, iot_releasable* data, size_t datasize, 
		iot_threadmsg_datamem_t datamem, bool is_core=false, iot_memallocator* allocator=NULL) {
	int err=iot_prepare_msg(msg, code, modinst, bytearg, (void*)data, datasize, datamem, is_core, allocator);
	if(!err) msg->is_releasable=1;
	return err;
}


void iot_release_msg(iot_threadmsg_t *&msg, bool = false);


#endif //IOT_THREADMSG_H
