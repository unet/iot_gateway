#ifndef IOT_COMMON_H
#define IOT_COMMON_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching

#include <stdint.h>
#include <atomic>
#include <sched.h>
#include <assert.h>

//#include<time.h>

#include "ecb.h"
#include "iot_utils.h"


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



#endif //IOT_COMMON_H
