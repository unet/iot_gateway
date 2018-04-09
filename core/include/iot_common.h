#ifndef IOT_COMMON_H
#define IOT_COMMON_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <sched.h>
#include <assert.h>

#ifndef NDEBUG
	#include <execinfo.h>
#endif

#include "uv.h"
//#include "ecb.h"
#include "iot_daemonlib.h"

enum h_state_t : uint8_t { //libuv handle state
	HS_UNINIT=0,
	HS_CLOSING,
	HS_INIT,
	HS_ACTIVE
}; //activation way: UNINIT -> INIT -> ACTIVE  , stop: ACTIVE -> CLOSING -> UNINIT   , reactivation: (stop) -> (activate)


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



template<class Node, class NodeBase, volatile std::atomic<NodeBase*> NodeBase::* NextPtr> class mpsc_queue { //Multiple Producer (push, push_list) Single Consumer (pop, pop_all, unpop, unpop_list, remove) unlimited FIFO queue.
	NodeBase *const stub;
	alignas(NodeBase) char stubbuf[sizeof(NodeBase)];
	volatile std::atomic<NodeBase*> tail;
	NodeBase* head; //accessed from consumer thread only
public:
	mpsc_queue(void): stub((NodeBase*)stubbuf),tail({stub}), head(stub) {
		(stub->*NextPtr).store(NULL, std::memory_order_release);
	}


	bool push(Node* newnode) { //returns true if first item is added (e.g. to send signal that queue is not empty)
		assert(!newnode->check_in_queue()); //check_in_queue MUST return constant FALSE if flag 'in_queue' is not implemented for NodeBase!!!
		(newnode->*NextPtr).store(NULL, std::memory_order_relaxed);
		newnode->set_in_queue();
		NodeBase* prevtail=tail.exchange(newnode, std::memory_order_acq_rel);
		//(critial point) - last element of list (when traversing from head) does not coinside with tail pointer
		(prevtail->*NextPtr).store(newnode, std::memory_order_release);
		return (prevtail==stub);
	}

	//adds several items connected by NextPtr field, with NULL for the last item
	bool push_list(Node* listhead) { //returns true if first item is added (e.g. to send signal that queue is not empty)
		NodeBase* newtail=listhead;
		NodeBase* next;
		assert(!newtail->check_in_queue());
		newtail->set_in_queue();
		//find last item of supplied list
		while((next=(newtail->*NextPtr).load(std::memory_order_relaxed))) {
			newtail=next;
			assert(!newtail->check_in_queue());
			newtail->set_in_queue();
		}
		//here newtail->*NextPtr is NULL, so no need to assign it
		NodeBase* prevtail=tail.exchange(newtail, std::memory_order_acq_rel);
		//(critial point)
		(prevtail->*NextPtr).store(listhead, std::memory_order_release);
		return (prevtail==stub);
	}

	void unpop(Node* newnode) { //operation for Consumer to put item at head of queue (reverse of pop)
		assert(!newnode->check_in_queue());
		newnode->set_in_queue();

		if(head==stub) { //head points to stub, so queue can be empty (and push, which will modify .next in stub, can start at any moment, so race is possible) or not (no race possible)
			NodeBase* next=(stub->*NextPtr).load(std::memory_order_acquire);
			if(!next) { //stub was the only item, so head must be equal to tail. try to make newnode new tail
				(newnode->*NextPtr).store(NULL, std::memory_order_release); //init newnode as new tail
				if(tail.compare_exchange_strong(head, newnode, std::memory_order_acq_rel, std::memory_order_relaxed)) { //tail is still equal to head, so queue remains with only newnode
											/*  head will be modified if FALSE!!! */
					head=newnode;
					return;
				} else { //push is right in progress. it can be at critial point so we must wait when it will be passed and 'next' gets non-NULL value
					uint16_t c=1;
					while(!(stub->*NextPtr).load(std::memory_order_acquire)) {
						if(!(c++ & 1023)) sched_yield();
					}
					next=(stub->*NextPtr).load(std::memory_order_relaxed);
				}
			} //else stub is not the only item, so no race possible, just remove stub and put newnode at head
			(newnode->*NextPtr).store(next, std::memory_order_relaxed);
		} else { //stub is absent, so there is at least one item at head
			(newnode->*NextPtr).store(head, std::memory_order_relaxed);
		}
		head=newnode;
	}

	void unpop_list(Node* listhead) { //operation for Consumer to put items at head of queue (reverse of pop_all)
		NodeBase* last=listhead;
		NodeBase* next;

		assert(!last->check_in_queue());
		last->set_in_queue();
		//find last item of supplied list
		while((next=(last->*NextPtr).load(std::memory_order_relaxed))) {
			last=next;

			assert(!last->check_in_queue());
			last->set_in_queue();
		}

		if(head==stub) { //head points to stub, so queue can be empty (and push, which will modify .next in stub, can start at any moment, so race is possible) or not (no race possible)
			next=(stub->*NextPtr).load(std::memory_order_acquire);
			if(!next) { //stub was the only item, so head must be equal to tail. try to make newnode new tail
				(last->*NextPtr).store(NULL, std::memory_order_release); //init newnode as new tail
				if(tail.compare_exchange_strong(head, last, std::memory_order_acq_rel, std::memory_order_relaxed)) { //tail is still equal to head, so queue remains with only listhead
											/*  head will be modified if FALSE!!! */
					head=listhead;
					return;
				} else { //push is right in progress. it can be at critial point so we must wait when it will be passed and 'next' gets non-NULL value
					uint16_t c=1;
					while(!(stub->*NextPtr).load(std::memory_order_acquire)) {
						if(!(c++ & 1023)) sched_yield();
					}
					next=(stub->*NextPtr).load(std::memory_order_relaxed);
				}
			} //else stub is not the only item, so no race possible, just remove stub and put newnode at head
			(last->*NextPtr).store(next, std::memory_order_relaxed);
		} else { //stub is absent, so there is at least one item at head
			(last->*NextPtr).store(head, std::memory_order_relaxed);
		}
		head=listhead;
	}

	bool remove(Node* oldnode) { //operation for Consumer to search for node in list and remove it. returns true if node was found and removed from queue
		NodeBase* cur=head, *prev=NULL; //head cannot be NULL
		NodeBase* next=(cur->*NextPtr).load(std::memory_order_acquire);
		do {
			if(cur==oldnode) break;
			prev=cur;
			cur=next;
			if(!cur) return false;
			next=(cur->*NextPtr).load(std::memory_order_acquire);
		} while(1);
		if(cur==head) { //item is at head, so just pop it
			pop();
			return true;
		}

		if(next) { //not last item, so prev item can be just updated
			(prev->*NextPtr).store(next, std::memory_order_relaxed);
			(cur->*NextPtr).store(NULL, std::memory_order_relaxed);
			cur->clear_in_queue();
			return true;
		}
		//is last item. push can be in progress which will update NextPtr of cur, so must wait for push operation to finish
		(prev->*NextPtr).store(NULL, std::memory_order_release); //prepare prev item to becoming new tail
		if(tail.compare_exchange_strong(cur, prev, std::memory_order_acq_rel, std::memory_order_relaxed)) { //tail is still equal to cur, so prev becomes tail
										/*  cur will be modified if FALSE!!! */
			//do nothing
		} else { //push is right in progress. it can be at critial point so we must wait when it will be passed and 'next' gets non-NULL value
			uint16_t c=1;
			while(!(oldnode->*NextPtr).load(std::memory_order_acquire)) {
				if(!(c++ & 1023)) sched_yield();
			}
			(prev->*NextPtr).store( (oldnode->*NextPtr).load(std::memory_order_relaxed), std::memory_order_relaxed);
		}
		(cur->*NextPtr).store(NULL, std::memory_order_relaxed);
		cur->clear_in_queue();
		return true;
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
		oldhead->clear_in_queue();
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
				head->clear_in_queue();
				head=next;
				next=(next->*NextPtr).load(std::memory_order_acquire);
			}
			//here head must be equal to tail, item pointed by head has next==NULL
			NodeBase* tmp=head;
			if(tail.compare_exchange_strong(tmp, stub, std::memory_order_acq_rel, std::memory_order_relaxed)) { //tail is still equal to head, so queue becomes empty
										/*  tmp will be modified if FALSE!!! */
				head->clear_in_queue();
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
	volatile std::atomic<uint32_t> readpos, writepos;
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
		readpos.store(0, std::memory_order_relaxed);
		writepos.store(0, std::memory_order_relaxed);
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
			ws=read(newbuf, newsize);
		} else {
			ws=0;
		}
		buf=newbuf;
		bufsize=newsize;
		mask=newmask;
		std::atomic_thread_fence(std::memory_order_release);
		readpos.store(0, std::memory_order_relaxed);
		writepos.store(ws, std::memory_order_relaxed);
		return true;
	}
	uint32_t pending_read(void) { //returns unread bytes
//		assert(/*writepos>=readpos && */writepos-readpos<=bufsize); //?? writepos can become < readpos after 32 bit overflow, but this must be OK while difference <= bufsize
		uint32_t res=writepos.load(std::memory_order_relaxed)-readpos.load(std::memory_order_relaxed); //32 bit overflow here is normal
		std::atomic_thread_fence(std::memory_order_acquire); //to be in sync with update of writepos in write() and write_zero()
		assert(res<=bufsize);
		return res;
	}
	uint32_t avail_write(void) { //returns available space
		return bufsize-pending_read();
	}
	uint32_t get_readbuf_iovec(iovec *iovecbuf, int &veclen) { //can be used to obtain direct addresses of data to read. after such direct read. a call to read() with NULL
															//dstbuf and dstsize==[return value of get_readbuf_iovec] must be done to update read position
		//iovecbuf must point to array of veclen structures. on exit veclen will be updated to actual number of filled
		//structures. maximum 2 structures can be filled!!! so no need to provide more than 2.
		//return value shows number of available bytes. it will be the sum of lengths of returned iovec structures
		if(veclen<=0 || !iovecbuf) {veclen=0;return 0;}
		uint32_t ws=pending_read();
		if(!ws) {veclen=0;return 0;}
		uint32_t readpos1=readpos.load(std::memory_order_relaxed) & mask;
		if(readpos1+ws <= bufsize) { //no buffer border crossed, move single block
			iovecbuf[0]={.iov_base=&buf[readpos1], .iov_len=ws};
			veclen=1;
		} else { //buffer border crossed, move two blocks
			uint32_t ws1 = bufsize - readpos1;
			iovecbuf[0]={.iov_base=&buf[readpos1], .iov_len=ws1};
			if(veclen==1) return ws1; //only one iovec supplied, so return only partial size of available read
			//here veclen>1
			iovecbuf[1]={.iov_base=buf, .iov_len=ws - ws1};
			veclen=2;
		}
		return ws;
	}

	uint32_t read(void* dstbuf, size_t dstsize, uint32_t offset=0) { //read at most dstsize bytes. returns number of copied bytes
		//NULL dstbuf means that data must be discarded, so only readpos is advanced
		//offset (which is added to readpos) cannot exceed available data size (ot zero will be returned)
		uint32_t avail=pending_read();
		if(offset>=avail) return 0;
		avail-=offset;
		uint32_t ws=avail < dstsize ? avail : dstsize; //working size
		if(!ws) return 0;
		if(dstbuf) {
			uint32_t readpos1=(readpos.load(std::memory_order_relaxed)+offset) & mask;
			if(readpos1+ws <= bufsize) { //no buffer border crossed, move single block
				memcpy(dstbuf, &buf[readpos1], ws);
			} else { //buffer border crossed, move two blocks
				uint32_t ws1 = bufsize - readpos1;
				memcpy(dstbuf, &buf[readpos1], ws1);
				memcpy((char*)dstbuf + ws1, buf, ws - ws1);
			}
		}
		std::atomic_thread_fence(std::memory_order_release);
		readpos.fetch_add(ws+offset, std::memory_order_relaxed);
		return ws;
	}
	uint32_t peek(void* dstbuf, size_t dstsize, uint32_t offset=0) { //read at most dstsize bytes WITHOUT MOVING readpos. returns number of copied bytes
		//NULL dstbuf means that data will not be actually copied, but return value will be the same as if dstbuf was non-NULL
		//offset (which is added to readpos) cannot exceed available data size (ot zero will be returned)
		uint32_t avail=pending_read();
		if(offset>=avail) return 0;
		avail-=offset;
		uint32_t ws=avail < dstsize ? avail : dstsize; //working size
		if(!ws) return 0;
		if(dstbuf) {
			uint32_t readpos1=(readpos.load(std::memory_order_relaxed)+offset) & mask;
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
	uint32_t poke(const void* srcbuf, size_t srcsize, uint32_t offset=0) { //write at most srcsize bytes WITHOUT MOVING writepos. returns number of written bytes
		//NULL srcbuf means that no data will be written, but return value will be the same as if dstbuf was non-NULL
		//offset (which is added to writepos) cannot exceed size of available space (ot zero will be returned)
		uint32_t avail=avail_write();
		if(offset>=avail) return 0;
		avail-=offset;
		uint32_t ws=avail < srcsize ? avail : srcsize; //working size
		if(!ws) return 0;
		if(srcbuf) {
			uint32_t writepos1=(writepos.load(std::memory_order_relaxed)+offset) & mask;
			if(writepos1+ws <= bufsize) { //no buffer border crossed, move single block
				memcpy(&buf[writepos1], srcbuf, ws);
			} else { //buffer border crossed, move two blocks
				uint32_t ws1 = bufsize - writepos1;
				memcpy(&buf[writepos1], srcbuf, ws1);
				memcpy(buf, (char*)srcbuf + ws1, ws - ws1);
			}
		}
		return ws;
	}
	uint32_t write(const void* srcbuf, size_t srcsize) { //write at most srcsize bytes. returns number of written bytes
		//NULL srcbuf means that no data will be written, but writepos is advanced as if write was made (this feature is meaningful together with using poke())
		uint32_t avail=avail_write();
		uint32_t ws=avail < srcsize ? avail : srcsize; //working size
		if(!ws) return 0;
		if(srcbuf) {
			uint32_t writepos1=writepos.load(std::memory_order_relaxed) & mask;
			if(writepos1+ws <= bufsize) { //no buffer border crossed, move single block
				memcpy(&buf[writepos1], srcbuf, ws);
			} else { //buffer border crossed, move two blocks
				uint32_t ws1 = bufsize - writepos1;
				memcpy(&buf[writepos1], srcbuf, ws1);
				memcpy(buf, (char*)srcbuf + ws1, ws - ws1);
			}
		}
		std::atomic_thread_fence(std::memory_order_release);
		writepos.fetch_add(ws, std::memory_order_relaxed);
		return ws;
	}
	uint32_t write_zero(size_t srcsize) { //write at most srcsize zero bytes. returns number of written bytes
		uint32_t avail=avail_write();
		uint32_t ws=avail < srcsize ? avail : srcsize; //working size
		if(!ws) return 0;
		uint32_t writepos1=writepos.load(std::memory_order_relaxed) & mask;
		if(writepos1+ws <= bufsize) { //no buffer border crossed, fill single block
			memset(&buf[writepos1], 0, ws);
		} else { //buffer border crossed, move two blocks
			uint32_t ws1 = bufsize - writepos1;
			memset(&buf[writepos1], 0, ws1);
			memset(buf, 0, ws - ws1);
		}
		std::atomic_thread_fence(std::memory_order_release);
		writepos.fetch_add(ws, std::memory_order_relaxed);
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
protected:
	iot_spinlock reflock;
private:
//reflock protected:
	mutable volatile bool pend_destroy; //flag that this struct in waiting for zero in refcount to be freed. can be accessed under acclock only
	mutable volatile int32_t refcount; //how many times address of this object is referenced. value -1 together with true pend_destroy means that destroy_sub was already called (or is right to be called)
#ifndef NDEBUG
public:
	mutable bool debug=false;
#endif
///////////////////

	object_destroysub_t destroy_sub; //can be NULL if no specific destruction required (i.e. object is statically allocated). this function MUST call
		//iot_objectrefable desctuctor (explicitely or doing delete)

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
		reflock.lock();
		assert(refcount>=0);
//		if(refcount<0) {
//			assert(false);
//			pend_destroy=true; //set to make guaranted exit by next condition
//		}
//		if(pend_destroy) { //cannot be referenced ones more
//			reflock.unlock();
//			return false;
//		}
		refcount++;

#ifndef NDEBUG
		if(debug) {
			void* tmp[4]; //we need to abandon first value. it is always inside this func
			int nback;
			nback=backtrace(tmp, 4);
			if(nback>1) {
				char **symb=backtrace_symbols(tmp+1,3);
				outlog_notice("Object 0x%lx reffered (now %d times) from %s <- %s <- %s", long(uintptr_t(this)), refcount, symb[0] ? symb[0] : "NULL", symb[1] ? symb[1] : "NULL", symb[2] ? symb[2] : "NULL");
				free(symb);
			}
		}
#endif
		reflock.unlock();
		return;// true;
	}
	void unref(void) {
//outlog_error("%x unref", unsigned(uintptr_t(this)));
		reflock.lock();
		if(refcount<=0) {
			assert(false);
			reflock.unlock();
			return;
		}
		refcount--;
#ifndef NDEBUG
		if(debug) {
			void* tmp[4]; //we need to abandon first value. it is always inside this func
			int nback;
			nback=backtrace(tmp, 4);
			if(nback>1) {
				char **symb=backtrace_symbols(tmp+1,3);
				outlog_notice("Object 0x%lx UNreffered (now %d times) from %s <- %s <- %s", long(uintptr_t(this)), refcount, symb[0] ? symb[0] : "NULL", symb[1] ? symb[1] : "NULL", symb[2] ? symb[2] : "NULL");
				free(symb);
			}
		}
#endif
		if(refcount>0 || !pend_destroy) {
			reflock.unlock();
			return;
		}
		//here refcount==0 and pend_destroy==true
		//mark as destroyed by setting refcount to -1
		refcount=-1;

		reflock.unlock();
		if(destroy_sub) destroy_sub(this);
	}
	bool try_destroy(void) { //destroys object by calling destroy_sub and returns true if refcount is zero. otherwise marks object to be destroyed on last reference free and returns false
		reflock.lock();
		pend_destroy=true;
		if(refcount==0) {
			//mark as destroyed by setting refcount to -1
			refcount=-1;
			reflock.unlock();
			if(destroy_sub) destroy_sub(this);
			return true;
		}
		assert(refcount>0);
		reflock.unlock();
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
	bool operator==(const iot_objref_ptr& rhs) const{
		return ptr==rhs.ptr;
	}
	bool operator!=(const iot_objref_ptr& rhs) const{
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
	explicit operator T*() const {
		return ptr;
	}
	T* operator->() const {
		assert(ptr!=NULL);
		return ptr;
	}
};




//can be used with single writer only! no recursion allowed!
//writer has preference (no new reader can gain lock if writer is waiting)
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
		uint32_t lockval=lockobj.fetch_sub(1, std::memory_order_release);
		assert(lockval!=0 && lockval!=0x80000000u);
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
		uint32_t lockval=lockobj.fetch_sub(0x80000000u, std::memory_order_release);
		assert(lockval>=0x80000000u); //previous state must be 'write lock'
	}
};





#endif //IOT_COMMON_H
