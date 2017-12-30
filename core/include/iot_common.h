#ifndef IOT_COMMON_H
#define IOT_COMMON_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <sched.h>
#include <assert.h>

#include "uv.h"
//#include "ecb.h"
//#include "iot_daemonlib.h"


#define IOT_JSONPARSE_UINT(jsonval, typename, varname) { \
	if(typename ## _MAX == UINT32_MAX) {		\
		errno=0;	\
		int64_t i64=json_object_get_int64(jsonval);		\
		if(!errno && i64>0 && i64<=UINT32_MAX) varname=(typename)i64;	\
	} else if(typename ## _MAX == UINT64_MAX) {							\
		uint64_t u64=iot_strtou64(json_object_get_string(jsonval), NULL, 10);	\
		if(!errno && u64>0 && (u64<INT64_MAX || json_object_is_type(jsonval, json_type_string))) varname=(typename)(u64);	\
	} else if(typename ## _MAX == UINT16_MAX || typename ## _MAX == UINT8_MAX) {							\
		errno=0;	\
		int i=json_object_get_int(jsonval);		\
		if(!errno && i>0 && (uint32_t)i <= typename ## _MAX) varname=(typename)i;	\
	} else {	\
		assert(false);	\
	}	\
}

#define IOT_STRPARSE_UINT(str, typename, varname) { \
	if(typename ## _MAX == UINT32_MAX) {		\
		uint32_t u32=iot_strtou32(str, NULL, 10);	\
		if(!errno && u32>0) varname=(typename)(u32);	\
	} else if(typename ## _MAX == UINT64_MAX) {							\
		uint64_t u64=iot_strtou64(str, NULL, 10);	\
		if(!errno && u64>0) varname=(typename)(u64);	\
	} else if(typename ## _MAX == UINT16_MAX || typename ## _MAX == UINT8_MAX) {							\
		uint32_t u32=iot_strtou32(str, NULL, 10);	\
		if(!errno && u32>0 && u32 <= typename ## _MAX) varname=(typename)(u32);	\
	} else {	\
		assert(false);	\
	}	\
}



template<class Node, class NodeBase, volatile std::atomic<NodeBase*> NodeBase::* NextPtr> class mpsc_queue { //Multiple Producer (push, push_list) Single Consumer (pop, pop_all) unlimited FIFO queue. Deletions not possible
	NodeBase *const stub;
	alignas(NodeBase) char stubbuf[sizeof(NodeBase)];
	volatile std::atomic<NodeBase*> tail;
	NodeBase* head;
public:
	mpsc_queue(void): stub((NodeBase*)stubbuf),tail({stub}), head(stub) {
		(stub->*NextPtr).store(NULL, std::memory_order_release);
	}


	bool push(Node* newnode) { //returns true if first item is added (e.g. to send signal that queue is not empty)
		(newnode->*NextPtr).store(NULL, std::memory_order_relaxed);
		NodeBase* prevtail=tail.exchange(newnode, std::memory_order_acq_rel);
		//(critial point) - last element of list (when traversing from head) does not coinside with tail pointer
		(prevtail->*NextPtr).store(newnode, std::memory_order_release);
		return (prevtail==stub);
	}

	//adds several items connected by NextPtr field, with NULL for the last item
	bool push_list(Node* listhead) { //returns true if first item is added (e.g. to send signal that queue is not empty)
		Node* newtail=listhead;
		//find last item of supplied list
		do {
			Node* next=(newtail->*NextPtr).load(std::memory_order_relaxed);
			if(!next) break;
			newtail=next;
		} while(1);
		NodeBase* prevtail=tail.exchange(newtail, std::memory_order_acq_rel);
		//(critial point)
		(prevtail->*NextPtr).store(listhead, std::memory_order_release);
		return (prevtail==stub);
	}

	Node* pop(void) {
		NodeBase* oldhead=head;
		NodeBase* next=(oldhead->*NextPtr).load(std::memory_order_acquire);
		if(oldhead==stub) { //head points to stub, so queue is empty or stub should just be removed
			if(!next) return NULL; //queue is empty (push of first element can be in progress)
			//remove stub from queue and continue
			head=oldhead=next;
			next=(next->*NextPtr).load(std::memory_order_acquire);
		}
		if(next) { //not last item
			head=next;
		} else { //queue contains one item, so head must be equal to tail
			(stub->*NextPtr).store(NULL, std::memory_order_release); //init stub
			if(tail.compare_exchange_strong(head, stub, std::memory_order_acq_rel, std::memory_order_relaxed)) { //tail is still equal to head, so queue becomes empty
										/*  head will be modified if FALSE!!! */
				head=stub;
			} else { //push is right in progress. it can be at critial point so we must wait when it will be passed and 'next' gets non-NULL value
				uint16_t c=1;
				while(!(oldhead->*NextPtr).load(std::memory_order_acquire)) {
					if(!(c++ & 1023)) sched_yield();
				}
				head=(oldhead->*NextPtr).load(std::memory_order_relaxed); //non-NULL next becomes new head
			}
		}
		return static_cast<Node*>(oldhead);
	}
	Node* pop_all(void) {
		NodeBase* oldhead=head;
		NodeBase* next=(oldhead->*NextPtr).load(std::memory_order_acquire);
		if(oldhead==stub) { //head points to stub, so queue is empty or stub should just be removed
			if(!next) return NULL; //queue is empty
			//remove stub from queue and continue
			head=oldhead=next;
			next=(next->*NextPtr).load(std::memory_order_acquire);
		}
		(stub->*NextPtr).store(NULL, std::memory_order_release); //init stub
		do {
			while(next) { //not last item in head
				head=next;
				next=(next->*NextPtr).load(std::memory_order_acquire);
			}
			//here head must be equal to tail, item pointed by head has next==NULL
			NodeBase* tmp=head;
			if(tail.compare_exchange_strong(tmp, stub, std::memory_order_acq_rel, std::memory_order_relaxed)) { //tail is still equal to head, so queue becomes empty
										/*  tmp will be modified if FALSE!!! */
				head=stub;
				break;
			}
			//push is right in progress. it can be at critial point so we must wait when it will be passed and 'next' gets non-NULL value
			uint16_t c=1;
			while(!(head->*NextPtr).load(std::memory_order_acquire)) {
				if(!(c++ & 1023)) sched_yield();
			}
			//here push was finished so we can continue to grab connected items (more than one could become ready at this point)
			next=(head->*NextPtr).load(std::memory_order_relaxed);
		} while(1);
		return static_cast<Node*>(oldhead);
	}
};


//Thread-safe Single Producer Single Consumer circular byte buffer.
class byte_fifo_buf {
	volatile uint32_t readpos, writepos;
	uint32_t bufsize, mask;
	char* buf;

public:
	byte_fifo_buf(void) {
		init();
	}
	void init(void) {
		buf=NULL;
		bufsize=mask=0;
		clear();
	}
	void clear(void) {
		std::atomic_thread_fence(std::memory_order_release);
		readpos=writepos=0;
	}
	uint32_t getsize(void) {
		return bufsize;
	}
	bool setbuf(uint8_t size_power, char* newbuf) { //size_power - power of 2 for size of new buffer
		//returns false on invalid args
		//can be called WHEN NO reader or writer is operating. RELEASE memory order must be guaranteed if either is resumed immediately after this function
		if(size_power==0 || size_power>30 || !newbuf) return false;
		uint32_t newsize = uint32_t(1)<<size_power;
		uint32_t newmask = newsize-1;
		uint32_t ws;
		if(buf && (ws=pending_read())>0) {
			if(ws>newsize) return false; //do not allow to loose data
			writepos=read(newbuf, newsize);
		} else {
			writepos=0;
		}
		buf=newbuf;
		bufsize=newsize;
		readpos=0;
		mask=newmask;
		return true;
	}
	uint32_t pending_read(void) { //returns unread bytes
		assert(writepos>=readpos && writepos-readpos<=bufsize);
		uint32_t res=writepos-readpos;
		std::atomic_thread_fence(std::memory_order_acquire); //to be in sync with update of writepos in write() and write_zero()
		return res;
	}
	uint32_t avail_write(void) { //returns available space
		return bufsize-pending_read();
	}
	uint32_t read(void* dstbuf, size_t dstsize) { //read at most dstsize bytes. returns number of copied bytes
		//NULL dstbuf means that data must be discarded
		uint32_t avail=pending_read();
		uint32_t ws=avail < dstsize ? avail : dstsize; //working size
		if(!ws) return 0;
		if(dstbuf) {
			uint32_t readpos1=readpos & mask;
			if(readpos1+ws <= bufsize) { //no buffer border crossed, move single block
				memcpy(dstbuf, &buf[readpos1], ws);
			} else { //buffer border crossed, move two blocks
				uint32_t ws1 = bufsize - readpos1;
				memcpy(dstbuf, &buf[readpos1], ws1);
				memcpy((char*)dstbuf + ws1, buf, ws - ws1);
			}
		}
		std::atomic_thread_fence(std::memory_order_release);
		readpos+=ws;
		return ws;
	}
	uint32_t peek(void* dstbuf, size_t dstsize, uint32_t offset=0) { //read at most dstsize bytes. returns number of copied bytes
		//NULL dstbuf means that data will not be actually copied, but return value will be the same as if dstbuf was non-NULL
		//offset cannot exceed available data size (ot zero will be returned)
		uint32_t avail=pending_read();
		if(offset>avail) return 0;
		avail-=offset;
		uint32_t ws=avail < dstsize ? avail : dstsize; //working size
		if(!ws) return 0;
		if(dstbuf) {
			uint32_t readpos1=(readpos+offset) & mask;
			if(readpos1+ws <= bufsize) { //no buffer border crossed, move single block
				memcpy(dstbuf, &buf[readpos1], ws);
			} else { //buffer border crossed, move two blocks
				uint32_t ws1 = bufsize - readpos1;
				memcpy(dstbuf, &buf[readpos1], ws1);
				memcpy((char*)dstbuf + ws1, buf, ws - ws1);
			}
		}
		return ws;
	}
	uint32_t write(const void* srcbuf, size_t srcsize) { //write at most srcsize bytes. returns number of written bytes
		uint32_t avail=avail_write();
		uint32_t ws=avail < srcsize ? avail : srcsize; //working size
		if(!ws) return 0;
		if((writepos & mask)+ws <= bufsize) { //no buffer border crossed, move single block
			memcpy(&buf[writepos & mask], srcbuf, ws);
		} else { //buffer border crossed, move two blocks
			uint32_t ws1 = bufsize - (writepos & mask);
			memcpy(&buf[writepos & mask], srcbuf, ws1);
			memcpy(buf, (char*)srcbuf + ws1, ws - ws1);
		}
		std::atomic_thread_fence(std::memory_order_release);
		writepos+=ws;
		return ws;
	}
	uint32_t write_zero(size_t srcsize) { //write at most srcsize zero bytes. returns number of written bytes
		uint32_t avail=avail_write();
		uint32_t ws=avail < srcsize ? avail : srcsize; //working size
		if(!ws) return 0;
		if((writepos & mask)+ws <= bufsize) { //no buffer border crossed, fill single block
			memset(&buf[writepos & mask], 0, ws);
		} else { //buffer border crossed, move two blocks
			uint32_t ws1 = bufsize - (writepos & mask);
			memset(&buf[readpos & mask], 0, ws1);
			memset(buf, 0, ws - ws1);
		}
		std::atomic_thread_fence(std::memory_order_release);
		writepos+=ws;
		return ws;
	}
};

struct iot_releasable {
	virtual void releasedata(void) = 0; //called to free/release dynamic data connected with object
};


class iot_objectrefable;
typedef void (*object_destroysub_t)(iot_objectrefable*);

void object_destroysub_memblock(iot_objectrefable*);
void object_destroysub_delete(iot_objectrefable*);


//object which can be referenced by iot_objid_t - derived struct, has reference count and delayed destruction
class iot_objectrefable {
	mutable volatile std::atomic_flag reflock=ATOMIC_FLAG_INIT; //lock to protect critical sections
//reflock protected:
	mutable volatile bool pend_destroy; //flag that this struct in waiting for zero in refcount to be freed. can be accessed under acclock only
	mutable volatile uint8_t reflock_recurs=0;
	mutable volatile int32_t refcount; //how many times address of this object is referenced. value -1 together with true pend_destroy means that destroy_sub was already called (or is right to be called)
	mutable volatile uv_thread_t reflocked_by={};
///////////////////

	object_destroysub_t destroy_sub; //can be NULL if no specific destruction required (i.e. object is statically allocated). this function MUST call iot_objectrefable desctuctor (explicitely or doing delete)

protected:
	iot_objectrefable(object_destroysub_t destroy_sub, bool is_dynamic) : destroy_sub(destroy_sub) {
		if(is_dynamic) { //creates object with 1 reference count and pending release, so first unref() without prior ref() will destroy the object
			refcount=1;
			pend_destroy=true;
		} else { //such mode can be used for statically allocated objects (so destroy_sub must be NULL)
			refcount=0;
			pend_destroy=false;
		}
	}
	void lock_refdata(void) const { //wait for reflock mutex
		if(reflocked_by==uv_thread_self()) { //this thread already owns lock, just increase recursion level
			reflock_recurs++;
			assert(reflock_recurs<5); //limit recursion to protect against endless loops
			return;
		}
		uint16_t c=1;
		while(reflock.test_and_set(std::memory_order_acquire)) {
			//busy wait
			if(!(c++ & 1023)) sched_yield();
		}
		reflocked_by=uv_thread_self();
		assert(reflock_recurs==0);
	}
	void unlock_refdata(void) const { //free reflock mutex
		if(reflocked_by!=uv_thread_self()) {
			assert(false);
			return;
		}
		if(reflock_recurs>0) { //in recursion
			reflock_recurs--;
			return;
		}
		reflocked_by={};
		reflock.clear(std::memory_order_release);
	}
public:
	virtual ~iot_objectrefable(void) {
		if(!destroy_sub) { //statically allocated global object, so no explicit try_destroy call required to be done and refcount must be 0 on destruction in such case
			if(refcount==0) return;
			//else try_destroy call could have been done and refcount must be -1
		}
		assert(refcount==-1 && pend_destroy); //ensure destroy was called (and thus refcount checked)
	}
	int32_t get_refcount(void) const {
		return refcount;
	}
	void ref(void) const { //increment refcount if destruction is not pending. returns false in last case
//outlog_error("%x ref increased", unsigned(uintptr_t(this)));
		lock_refdata();
		assert(refcount>=0);
//		if(refcount<0) {
//			assert(false);
//			pend_destroy=true; //set to make guaranted exit by next condition
//		}
//		if(pend_destroy) { //cannot be referenced ones more
//			unlock_refdata();
//			return false;
//		}
		refcount++;
		unlock_refdata();
		return;// true;
	}
	void unref(void) {
//outlog_error("%x unref", unsigned(uintptr_t(this)));
		lock_refdata();
		if(refcount<=0) {
			assert(false);
			unlock_refdata();
			return;
		}
		refcount--;
		if(refcount>0 || !pend_destroy) {
			unlock_refdata();
			return;
		}
		//here refcount==0 and pend_destroy==true
		//mark as destroyed by setting refcount to -1
		refcount=-1;
		unlock_refdata();
		if(destroy_sub) destroy_sub(this);
	}
	bool try_destroy(void) { //destroys object by calling destroy_sub and returns true if refcount is zero. otherwise marks object to be destroyed on last reference free and returns false
		lock_refdata();
		pend_destroy=true;
		if(refcount==0) {
			//mark as destroyed by setting refcount to -1
			refcount=-1;
			unlock_refdata();
			if(destroy_sub) destroy_sub(this);
			return true;
		}
		assert(refcount>0);
		unlock_refdata();
		return false;
	}
};



//smart pointer which holds additional reference to iot_objectrefable-derived objects and frees it on destruction
template<typename T> class iot_objref_ptr {
	T *ptr;
public:
	iot_objref_ptr(void) : ptr(NULL) {} //default constructor for arrays
	iot_objref_ptr(const iot_objref_ptr& src) { //copy constructor
		ptr=src.ptr;
		if(ptr) ptr->ref(); //increase refcount
	}
	iot_objref_ptr(iot_objref_ptr&& src) noexcept { //move constructor
		ptr=src.ptr;
		src.ptr=NULL;
	}
	iot_objref_ptr(T *obj) {
		if(!obj) {
			ptr=NULL;
		} else {
			ptr=obj;
			ptr->ref(); //increase refcount
		}
	}
	iot_objref_ptr(bool noref, T *obj) { //true 'noref' tells to not increment refcount
		if(!obj) {
			ptr=NULL;
		} else {
			ptr=obj;
			if(!noref) ptr->ref(); //increase refcount
		}
	}
	iot_objref_ptr(T && srcobj) = delete; //move constructor for temporary object. NO TEMPORARY iot_objectrefable-derived OBJECTS SHOULD BE CREATED

	~iot_objref_ptr() {
		clear();
	}
	void clear(void) { //free reference
		if(!ptr) return;
		ptr->unref();
		ptr=NULL;
	}
	iot_objref_ptr& operator=(T *obj) {
		if(ptr==obj) return *this; //no action
		if(ptr) ptr->unref();
		if(!obj) {
			ptr=NULL;
		} else {
			ptr=obj;
			ptr->ref(); //increase refcount
		}
		return *this;
	}
	iot_objref_ptr& operator=(const iot_objref_ptr& src) { //copy assignment
		if(this==&src || ptr==src.ptr) return *this; //no action
		if(ptr) ptr->unref();
		ptr=src.ptr;
		if(ptr) ptr->ref(); //increase refcount
		return *this;
	}
	iot_objref_ptr& operator=(iot_objref_ptr&& src) {  //move assignment
		if(this==&src || ptr==src.ptr) return *this; //no action
		if(ptr) ptr->unref();
		ptr=src.ptr;
		src.ptr=NULL;
		return *this;
	}
	bool operator==(const T& rhs) const{
		return ptr==rhs.ptr;
	}
	bool operator!=(const T& rhs) const{
		return ptr!=rhs.ptr;
	}
	bool operator==(const T *rhs) const{
		return ptr==rhs;
	}
	bool operator!=(const T *rhs) const{
		return ptr!=rhs;
	}
	explicit operator bool() const {
		return ptr!=NULL;
	}
	bool operator !(void) const {
		return ptr==NULL;
	}
	operator T*() const {
		return ptr;
	}
	T* operator->() const {
		assert(ptr!=NULL);
		return ptr;
	}
};




//can be used with single writer only! no recursion allowed!
class iot_spinrwlock {
	volatile std::atomic<uint32_t> lockobj={0};
public:
	void rdlock(void) {
		uint32_t lockval=lockobj.load(std::memory_order_relaxed); //initial read
		uint16_t c=1;
		do {
			if(lockval<0x8000000u) { //no exclusive lock right now, try to increment readers count
				lockval=lockobj.fetch_add(1, std::memory_order_acq_rel);
				if(lockval<0x80000000u) break; //still no exclusive lock, so we got shared lock
				//exclusive lock was initiated just after "initial read", so cancel initiated shared lock because writer can be waiting for 0x80000000u value
				lockobj.fetch_sub(1, std::memory_order_release);
			}
			if(!((c++) & 1023)) sched_yield();
			lockval=lockobj.load(std::memory_order_acquire); //repeat read again
		} while(1);
		//here we got shared lock
		assert(lockval<0x20000); //limit maximum number of reader to 65536*2
	}
	void rdunlock(void) {
		lockobj.fetch_sub(1, std::memory_order_release);
	}
	void wrlock(void) {
		uint32_t lockval=lockobj.fetch_add(0x80000000u, std::memory_order_acq_rel);
		assert(lockval<0x80000000u);
		lockval+=0x80000000u;
		uint16_t c=1;
		while(lockval!=0x80000000u) {
			assert(lockval>0x80000000u);
			if(!((c++) & 1023)) sched_yield();
			lockval=lockobj.load(std::memory_order_acquire);
		}
		//here lockval==0x80000000u so no readers active and cannot become active
	}
	void wrunlock(void) {
		lockobj.fetch_sub(0x80000000u, std::memory_order_release);
	}
};





#endif //IOT_COMMON_H
