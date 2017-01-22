#ifndef IOT_MEMALLOC_H
#define IOT_MEMALLOC_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching

#include<stdint.h>
//#include<assert.h>
//#include<string.h>
#include<uv.h>

//#include<ecb.h>

#include <iot_utils.h>
#include <iot_kapi.h>


struct iot_memobject;
class iot_memallocator;
extern iot_memallocator main_allocator;

#include <kernel/iot_common.h>


//mask for iot_memobject::parent field to get index of freelist inside allocator
//#define IOT_MEMOBJECT_PARENT_ALIGN 32
//#define IOT_MEMOBJECT_PARENT_MASK (~uintptr_t(IOT_MEMOBJECT_PARENT_ALIGN-1))
#define IOT_MEMOBJECT_CHAINSIZE 16384



struct iot_memobject {
	union {
		volatile std::atomic<iot_memobject *>next; //pointer to next object in freelist when object is free
		iot_memallocator *parent; //pointer to parent allocator object
	};
	volatile std::atomic<uint8_t> refcount; //reference count of this object. object is returned to free list when its refcount goes to zero
	uint8_t listindex:4;  //valid if refcount>0. special value 14 additionally means that data is really iot_membuf_chain object
								//15 means that memory object is temporary and has arbitrary size
	uint16_t	memchunk; //index of parent memchunk in parent->memchunks array
	char data[4]; //arbitrary data or iot_membuf_chain if listindex==14
};



//Manages memory allocations for one thread. Allows to release allocated blocks from any thread
class /*alignas(IOT_MEMOBJECT_PARENT_ALIGN)*/ iot_memallocator {
	static const uint32_t objsizes[15]; //size of allocated objects for corresponding freelist
	static const uint32_t objoptblock[15]; //optimal size of OS-allocated block for corresponding freelist
	mpsc_queue<iot_memobject, iot_memobject, &iot_memobject::next> freelist[15];

	volatile std::atomic<int32_t> totalinfly; //incremented during block allocation and decremented during release
	uv_thread_t thread; //which thread can do allocations

	void **memchunks; //array of OS-allocated memory chunks
	int32_t *memchunks_refs; //for each OS-allocated chunk keeps number of blocks in use or in freelist, so 0 means that chunk can be freed. for holes == -2
	uint32_t nummemchunks, maxmemchunks; //current quantity of chunk pointers in memchunks, number of allocated items in memchunks array
	volatile std::atomic<uint32_t> numholes; //current number of holes in memchunks array (they appear during freeing OS-allocated blocks)
public:
	iot_memallocator(uv_thread_t th) :
		totalinfly({0}), thread(th),
		memchunks(NULL),  memchunks_refs(NULL), nummemchunks(0), maxmemchunks(0), numholes({0}) {
			if(this==&main_allocator) thread=uv_thread_self();
	}
	~iot_memallocator(void) {
		deinit();
	}
	iot_membuf_chain* allocate_chain(uint32_t size);
	void* allocate(uint32_t size, bool allow_direct=false); //true allow_direct says that block can be malloced directly without going to freelist on release. can be used for rarely realloced buffers
	bool incref(void* ptr); //increase object's reference count if possible (returns true). max number of refs is IOT_MEMOBJECT_MAXREF. can be called from any thread
	void release(void* ptr); //decrease object's reference count. can be called from any thread

private:
	void deinit(void); //free all OS-allocated chunks
	void do_free_direct(uint16_t &chunkindex);
	void *do_allocate_direct(uint32_t size, uint16_t &chunkindex); //returns NULL on allocation error
	bool do_allocate_freelist(uint32_t &n, uint32_t sz, iot_memobject * &ret, uint32_t OPTIMAL_BLOCK=64*1024, uint32_t MAX_BLOCK=2*1024*1024,unsigned maxn=0xFFFFFFFF);
		//'n' - minimal amount to be allocated for success. on exit it is updated to show quantity of allocated items
		//'sz' - size of each item
		//'ret' - head of allocated unidirectional list of items will be put here. if 'ret' already has pointer to unidirectional list, this list will be prepended
		//'maxn' - optional maximum quantity of allocated items
		//returns false if 'n' was not satisfied (but less structs can be allocated and returned with 'n' updated to show quantity of allocated)
};


#endif //IOT_MEMALLOC_H
