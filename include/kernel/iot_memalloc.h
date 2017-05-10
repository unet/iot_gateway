#ifndef IOT_MEMALLOC_H
#define IOT_MEMALLOC_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching

#include<stdint.h>
#include <sys/time.h>
//#include<assert.h>
//#include<string.h>
#include<uv.h>

//#include<ecb.h>
#include<new>

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
#ifndef NDEBUG
	void* backtrace[3];
	timeval alloctimeval;
#endif
	volatile std::atomic<uint8_t> refcount; //reference count of this object. object is returned to free list when its refcount goes to zero
	uint8_t listindex:4;  //valid if refcount>0. special value 14 additionally means that data is really iot_membuf_chain object
								//15 means that memory object is temporary and has arbitrary size
	uint16_t	memchunk; //index of parent memchunk in parent->memchunks array
	uint32_t data[1]; //arbitrary data or iot_membuf_chain if listindex==14. ansure aslignment by 4 using uint32_t
};



//Manages memory allocations for one thread. Allows to release allocated blocks from any thread
class /*alignas(IOT_MEMOBJECT_PARENT_ALIGN)*/ iot_memallocator {
	static const uint32_t objsizes[15]; //size of allocated objects for corresponding freelist
	static const uint32_t objoptblock[15]; //optimal size of OS-allocated block for corresponding freelist
	mpsc_queue<iot_memobject, iot_memobject, &iot_memobject::next> freelist[15];

	volatile std::atomic<int32_t> totalinfly={0}; //incremented during block allocation and decremented during release
	uv_thread_t thread={}; //which thread can do allocations

	void **memchunks; //array of OS-allocated memory chunks
	int32_t *memchunks_refs; //for each OS-allocated chunk keeps total number of blocks in use and in freelist, so 0 means that chunk can be freed. for holes == -2
	uint32_t nummemchunks, maxmemchunks; //current quantity of chunk pointers in memchunks, number of allocated items in memchunks array
	volatile std::atomic<uint32_t> numholes; //current number of holes in memchunks array (they appear during freeing OS-allocated blocks)
public:
	iot_memallocator(void) :
//		totalinfly({0}),
		memchunks(NULL),  memchunks_refs(NULL), nummemchunks(0), maxmemchunks(0), numholes({0}) {
			if(this==&main_allocator) thread=uv_thread_self();
	}
	~iot_memallocator(void) {
		deinit();
	}
	void set_thread(uv_thread_t th) {
		thread=th;
	}
	iot_membuf_chain* allocate_chain(uint32_t size);
	void* allocate(uint32_t size, bool allow_direct=false); //true allow_direct says that block can be malloced directly without going to freelist on release. can be used for rarely realloced buffers
	bool incref(void* ptr); //increase object's reference count if possible (returns true). max number of refs is IOT_MEMOBJECT_MAXREF. can be called from any thread
	void release(void* ptr); //decrease object's reference count. can be called from any thread

	iot_threadmsg_t *allocate_threadmsg(void); //allocates threadmsg structure as memblock and inits is properly

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

template<class Key1, class Key2, class Value> struct dbllist_node { //represents node which is member of two bi-dir. linked lists with tail
	dbllist_node *next[2], *prev[2]; //index i - to organize list of nodes with equal Key[i]
	Key1 key1;
	Key2 key2;
	Value val;

	dbllist_node(const Key1 &key1, const Key2 &key2, const Value &val) : next{NULL, NULL}, prev{NULL,NULL}, key1(key1), key2(key2), val(val) {
	}
	~dbllist_node(void) {
		remove(true);
	}
	void remove(bool norelease=false) {
		remove_from(0);
		remove_from(1);
		if(!norelease) iot_release_memblock(this);
	}
	void remove_from(unsigned idx) { //idx is 0 or 1
		assert(idx<2);
		BILINKLISTWT_REMOVE(this, next[idx], prev[idx]);
	}
};


//Index is 1 or 2
template<class Key1, class Key2, class Value, unsigned Index> class dbllist_list { //list of nodes with equal Key{Index}. only find_key{3-Index}() can be called
	dbllist_node<Key1, Key2, Value> *head, *tail;

public:
	dbllist_list(void) : head(NULL), tail(NULL) {}
	bool is_empty(void) {
		if(head && tail) return false;
		assert(!head && !tail);
		return true;
	}
	//creates new node but does not insert into list
	dbllist_node<Key1, Key2, Value> *new_node(const Key1 &key1, const Key2 &key2, const Value &val, iot_memallocator* allocator) const { //only list with Index==2 (i.e. with items of equal key2) can search by key1
		dbllist_node<Key1, Key2, Value> *it=(dbllist_node<Key1, Key2, Value> *)allocator->allocate(sizeof(dbllist_node<Key1, Key2, Value>));
		if(it) {
			new(it) dbllist_node<Key1, Key2, Value>(key1, key2, val);
		}
		return it;
	}
	dbllist_node<Key1, Key2, Value> *find_key1(const Key1 &key1) const { //only list with Index==2 (i.e. with items of equal key2) can search by key1
		if(Index!=2) { //illegal call
			assert(false);
			return NULL;
		}
		dbllist_node<Key1, Key2, Value> *it=head;
		while(it) { //more than 1 item, so remove second to avoid reassigning of head on each step
			if(it->key1==key1) return it;
			if(it==tail) break;
			it=head->next[Index-1];
		}
		return NULL;
	}
	dbllist_node<Key1, Key2, Value> *find_key2(const Key2 &key2) const { //only list with Index==1 (i.e. with items of equal key1) can search by key2
		if(Index!=1) { //illegal call
			assert(false);
			return NULL;
		}
		dbllist_node<Key1, Key2, Value> *it=head;
		while(it) { //more than 1 item, so remove second to avoid reassigning of head on each step
			if(it->key2==key2) return it;
			if(it==tail) break;
			it=head->next[Index-1];
		}
		return NULL;
	}
	void insert_head(dbllist_node<Key1, Key2, Value>* item) {
		assert(!head || (Index==1 ? head->key1==item->key1 : head->key2==item->key2)); //if have at least one elem, check that keyX matches
		BILINKLISTWT_REMOVE_NOCL(item, next[Index-1], prev[Index-1]);
		BILINKLISTWT_INSERTHEAD(item, head, tail, next[Index-1], prev[Index-1]);
	}
	void insert_tail(dbllist_node<Key1, Key2, Value>* item) {
		assert(!head || (Index==1 ? head->key1==item->key1 : head->key2==item->key2)); //if have at least one elem, check that keyX matches
		BILINKLISTWT_REMOVE_NOCL(item, next[Index-1], prev[Index-1]);
		BILINKLISTWT_INSERTTAIL(item, head, tail, next[Index-1], prev[Index-1]);
	}
	dbllist_node<Key1, Key2, Value> *get_first(void) const {
		return head;
	}
	dbllist_node<Key1, Key2, Value> *get_last(void) const {
		return tail;
	}
	dbllist_node<Key1, Key2, Value> *get_next(dbllist_node<Key1, Key2, Value>* item) const {
		return item==tail ? NULL : item->next[Index-1];
	}
	dbllist_node<Key1, Key2, Value> *get_prev(dbllist_node<Key1, Key2, Value>* item) const {
		return item==head ? NULL : item->prev[Index-1];
	}
	void remove_all(void) { //cleans list and deallocates all nodes as memblocks
		if(!head) return;
		dbllist_node<Key1, Key2, Value> *it;
		while(head != tail) { //more than 1 item, so remove second to avoid reassigning of head on each step
			it=head->next[Index-1];
			BILINKLISTWT_REMOVE_NOCL(it, next[0], prev[0]);
			BILINKLISTWT_REMOVE_NOCL(it, next[1], prev[1]);
			iot_release_memblock(it);
		}
		//single item left
		assert(head && tail && head==tail);
		it=head;
		BILINKLISTWT_REMOVE_NOCL(it, next[0], prev[0]);
		BILINKLISTWT_REMOVE_NOCL(it, next[1], prev[1]);
		iot_release_memblock(it);
	}
	void removeby_value(const Value &val) {
		if(!head) return;
		dbllist_node<Key1, Key2, Value> *it=get_first(), *next;
		while(it) { //more than 1 item, so remove second to avoid reassigning of head on each step
			next = get_next(it);
			if(it->val==val) {
				BILINKLISTWT_REMOVE_NOCL(it, next[0], prev[0]);
				BILINKLISTWT_REMOVE_NOCL(it, next[1], prev[1]);
				iot_release_memblock(it);
			}
			it=next;
		}
	}
};


#endif //IOT_MEMALLOC_H
