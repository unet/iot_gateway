#ifndef IOT_COMMON_H
#define IOT_COMMON_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching

#include <stdint.h>
#include <atomic>
#include <sched.h>
#include <assert.h>

//#include<time.h>

//#include<ecb.h>


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
		//(critial point)
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
			if(!next) return NULL; //queue is empty
			//remove stub from queue and continue
			head=oldhead=next;
			next=(next->*NextPtr).load(std::memory_order_acquire);
		}
		if(next) { //not last item
			head=next;
		} else { //queue contains one item, so head must be equal to tail
			(stub->*NextPtr).store(NULL, std::memory_order_release); //init stub
			if(tail.compare_exchange_strong(oldhead, stub, std::memory_order_acq_rel, std::memory_order_relaxed)) { //tail is still equal to head, so queue becomes empty
				head=stub;
			} else { //push is right in progress. it can be at critial point so we must wait when it will be passed and 'next' gets non-NULL value
				unsigned char c=0;
				while(!(oldhead->*NextPtr).load(std::memory_order_acquire)) {
					c++;
					if((c & 0x3F) == 0x3F) sched_yield();
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
		do {
			while(next) { //not last item in head
				head=next;
				next=(next->*NextPtr).load(std::memory_order_acquire);
			}
			//here head must be equal to tail, item pointed by head has next==NULL
			(stub->*NextPtr).store(NULL, std::memory_order_release); //init stub
			if(tail.compare_exchange_strong(head, stub, std::memory_order_acq_rel, std::memory_order_relaxed)) { //tail is still equal to head, so queue becomes empty
				head=stub;
				break;
			}
			//push is right in progress. it can be at critial point so we must wait when it will be passed and 'next' gets non-NULL value
			unsigned char c=0;
			while(!(head->*NextPtr).load(std::memory_order_acquire)) {
				c++;
				if((c & 0x3F) == 0x3F) sched_yield();
			}
			//here push was finished so we can continue to grab connected items (more than one could become ready at this point)
			next=(head->*NextPtr).load(std::memory_order_relaxed);
		} while(1);
		return static_cast<Node*>(oldhead);
	}
};


//Thread-safe Single Producer Single Consumer circular byte buffer.
class byte_fifo_buf {
	uint32_t readpos, writepos, bufsize, mask;
	char* buf;

public:
	byte_fifo_buf(void) {
		buf=NULL;
		bufsize=readpos=writepos=mask=0;
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
	void clear(void) {
		readpos=writepos=0;
	}
	uint32_t pending_read(void) { //returns unread bytes
		assert(writepos>=readpos && writepos-readpos<=bufsize);
		return writepos-readpos;
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
		writepos+=ws;
		std::atomic_thread_fence(std::memory_order_release);
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
		writepos+=ws;
		std::atomic_thread_fence(std::memory_order_release);
		return ws;
	}
};



#endif //IOT_COMMON_H
