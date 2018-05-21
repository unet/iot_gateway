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
#include "iot_daemonlib.h"





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
