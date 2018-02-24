#include<stdint.h>
#include<stdlib.h>
#include<assert.h>
#include<string.h>

#ifndef NDEBUG
	#include <execinfo.h>
#endif

#include "iot_memalloc.h"
#include "iot_threadregistry.h"


iot_membuf_chain* iot_memallocator::allocate_chain(uint32_t size) { //size is length of useful data to store in chained buffer
		assert(uv_thread_self()==*thread); //only one thread can allocate
		if(!size) return NULL;

		uint32_t perblock=iot_membuf_chain::get_increment(IOT_MEMOBJECT_CHAINSIZE-offsetof(struct iot_memobject, data));
		uint32_t nblocks=(size + perblock - 1) / perblock;

		iot_membuf_chain* res=NULL;
		iot_memobject* obj;
		while(nblocks>0) {
			obj=freelist[14].pop();
			if(!obj) break;
			obj->parent=this;
			obj->refcount.store(1, std::memory_order_relaxed);
			obj->listindex=14;
			totalinfly.fetch_add(1, std::memory_order_release);

			if(!res) {
				res=(iot_membuf_chain*)(obj->data);
				res->init(IOT_MEMOBJECT_CHAINSIZE-offsetof(struct iot_memobject, data));
			} else {
				res->add_buf((char*)obj->data, IOT_MEMOBJECT_CHAINSIZE-offsetof(struct iot_memobject, data));
			}
			nblocks--;
		}
		if(nblocks>0) { //no free blocks left, need to allocate additional
			uint32_t n=nblocks;
			if(!do_allocate_freelist(n, objsizes[14], obj, objoptblock[14])) {
				if(res) release(res);
				return NULL;
			}
			iot_memobject* rest;
			do {
				rest=obj->next.load(std::memory_order_relaxed);

				obj->parent=this;
				obj->refcount.store(1, std::memory_order_relaxed);
				obj->listindex=14;
				totalinfly.fetch_add(1, std::memory_order_release);

				if(!res) {
					res=(iot_membuf_chain*)(obj->data);
					res->init(IOT_MEMOBJECT_CHAINSIZE-offsetof(struct iot_memobject, data));
				} else {
					res->add_buf((char*)obj->data, IOT_MEMOBJECT_CHAINSIZE-offsetof(struct iot_memobject, data));
				}
				nblocks--;
				obj=rest;
			} while(nblocks>0 && obj);
			assert(nblocks==0); //success from do_allocate_freelist must guarantee enough blocks
			if(rest) { //put excess items to freelist
				freelist[14].push_list(rest);
			}
		}
		return res;
	}

iot_threadmsg_t *iot_memallocator::allocate_threadmsg(void) { //allocates threadmsg structure as memblock and inits is properly
		iot_threadmsg_t *msg=(iot_threadmsg_t*)allocate(sizeof(iot_threadmsg_t));
		if(msg) {
			memset(msg, 0, sizeof(*msg));
			msg->is_msgmemblock=1;
		}
		return msg;
	}

//real function called insted of allocate() with or without debug data
void* iot_memallocator::allocate(uint32_t size, bool allow_direct) { //true allow_direct says that block can be malloced directly without going to freelist on release. can be used for rarely realloced buffers
		assert(uv_thread_self()==*thread); //only one thread can allocate

		iot_memobject* rval;
		size+=offsetof(struct iot_memobject, data);
		int listidx;
		if(size>IOT_MEMOBJECT_MAXPLAINSIZE+offsetof(struct iot_memobject, data)) {
			if(!allow_direct) return NULL; //objects larger than IOT_MEMOBJECT_MAXPLAINSIZE must be allocated with true allow_direct or by allocate_chain
			uint16_t chunkidx;
			rval=(iot_memobject*)do_allocate_direct(size, chunkidx);
			if(!rval) return NULL;
			rval->memchunk=chunkidx;
			listidx=15;
		} else {
			listidx=0;
			if(size<=160) {
				if(size<=64) {
					if(size>32) {
						if(size<=48) listidx=1; //>32 <=48
						else listidx=2; //>48  <=64
					}
					//else listidx=0; //<=32, inited as 0
				}
				else {
					if(size<=96) {
						if(size<=80) listidx=3; //>64 <=80
						else listidx=4; //>80 <=96
					}
					else {
						if(size<=128) listidx=5; //>96 <=128
						else listidx=6; //>128 <=160
					}
				}
			}
			else {
				if(size<=512) {
					if(size<=384) {
						if(size<=256) listidx=7; //>160 <=256
						else listidx=8; //>256 <=384
					}
					else listidx=9; //>384 <=512
				} else {
					if(size<=2048) {
						if(size<=1024) listidx=10; //>512 <=1024
						else listidx=11; //>1024 <=2048
					}
					else {
						if(size<=4096) listidx=12; //>2048 <=4096
						else listidx=13; //>4096 <=IOT_MEMOBJECT_MAXPLAINSIZE+offsetof(struct iot_memobject, data) (8192)
					}
				}
			}
			rval=freelist[listidx].pop();
			if(!rval) { //no free blocks left, need to allocate additional
				uint32_t n=1;
				if(!do_allocate_freelist(n, objsizes[listidx], rval, objoptblock[listidx])) return NULL;
				iot_memobject* rest=rval->next.load(std::memory_order_relaxed);
				if(rest) { //put excess items to freelist
					freelist[listidx].push_list(rest);
				}
			}
		}
		rval->parent=this;
		rval->refcount.store(1, std::memory_order_relaxed);
		rval->listindex=listidx;
		totalinfly.fetch_add(1, std::memory_order_release);
#ifndef NDEBUG
		void* tmp[4]; //we need to abandon first value. it is always inside this func
		int nback,i;
		nback=backtrace(tmp, 4);
		for(i=0;i<3;i++) rval->backtrace[i]=i<nback-1 ? tmp[i+1] : NULL;
		gettimeofday(&rval->alloctimeval, NULL);
#endif
		return rval->data;
	}

bool iot_memallocator::incref(void* ptr) { //increase object's reference count if possible (returns true). max number of refs is IOT_MEMOBJECT_MAXREF. can be called from any thread
		iot_memobject* obj=(iot_memobject*)container_of(ptr, struct iot_memobject, data);
		assert(obj->parent==this);
		uint32_t oldrefcount=obj->refcount.fetch_add(1, std::memory_order_acq_rel);
		assert(oldrefcount>0);
		if(oldrefcount>=IOT_MEMOBJECT_MAXREF) {
			obj->refcount.fetch_sub(1, std::memory_order_release);
			return false;
		}
		return true;
	}

void *iot_allocate_memblock(uint32_t size, bool allow_direct) {
	iot_memallocator* allocator=thread_registry->find_allocator(uv_thread_self());
	assert(allocator!=NULL);
	if(!allocator) return NULL;
	return allocator->allocate(size, allow_direct);
}

void iot_release_memblock(void *memblock) {
	assert(memblock!=NULL);
	iot_memobject* obj=(iot_memobject*)container_of(memblock, struct iot_memobject, data);
	assert(obj->parent->signature==IOT_MEMOBJECT_SIGNATURE);
	obj->parent->release(memblock);
}

bool iot_incref_memblock(void *memblock) {
	assert(memblock!=NULL);
	iot_memobject* obj=(iot_memobject*)container_of(memblock, struct iot_memobject, data);
	assert(obj->parent->signature==IOT_MEMOBJECT_SIGNATURE);
	return obj->parent->incref(memblock);
}


void iot_memallocator::release(void* ptr) { //decrease object's reference count. can be called from any thread
		iot_memobject* obj=(iot_memobject*)container_of(ptr, struct iot_memobject, data);
		assert(obj->parent==this);
		uint32_t refcount=obj->refcount.fetch_sub(1, std::memory_order_acq_rel);
		assert(refcount>0);
		if(refcount>1) return; //there are other refs
		//refcount was 1, so became 0
		int32_t infly=totalinfly.fetch_sub(1, std::memory_order_release);
		assert(infly>0);
		infly--;
		if(obj->listindex<14) {
			freelist[obj->listindex].push(obj);
			goto onexit;
		}
		if(obj->listindex==15) {
			memchunks_refs[obj->memchunk]--;
			do_free_direct(obj->memchunk);
			goto onexit;
		}
		//obj->listindex==14
		//ptr must point to iot_membuf_chain object
		int n;
		n=0;
		do {
			ptr=((iot_membuf_chain*)ptr)->next;
			freelist[14].push(obj);
			if(!ptr) break;
			n++;
			obj=(iot_memobject*)container_of(ptr, struct iot_memobject, data);
			refcount=obj->refcount.fetch_sub(1, std::memory_order_acq_rel);
			assert(refcount==1);
		} while(1);
		if(n>0) {
			infly=totalinfly.fetch_sub(n, std::memory_order_release);
			assert(infly>=n);
			infly-=n;
		}
onexit:
		if(!infly && prev_slave && thread==&main_thread) { //non-main allocator was previously uninited and now became empty
			BILINKLIST_REMOVE(this, next_slave, prev_slave);
			delete this;
		}
	}


void iot_memallocator::do_free_direct(uint16_t &chunkindex) {
		assert(chunkindex<nummemchunks);
		assert(memchunks[chunkindex]!=NULL);
		assert(memchunks_refs[chunkindex]==0);
		free(memchunks[chunkindex]);
		memchunks[chunkindex]=NULL;
		memchunks_refs[chunkindex]=-2;
		numholes.fetch_add(std::memory_order_release);
	}

void *iot_memallocator::do_allocate_direct(uint32_t size, uint16_t &chunkindex) { //allocate next chunk of memory
	//returns NULL on allocation error
		void* res;
		if(nummemchunks>=maxmemchunks) { //reallocate memchunks array to make it bigger
			if(numholes.load(std::memory_order_relaxed)>0) {
				uint32_t i;
				for(i=0;i<nummemchunks;i++) { //look for a hole
					if(memchunks_refs[i]==-2) break;
				}
				numholes.fetch_sub(1, std::memory_order_relaxed); //decrease even if no real hole was found in release mode
				assert(i<nummemchunks); //something wrong if numholes>0 but no holes found
				if(i<nummemchunks) {
					res=malloc(size);
					if(!res) {
						numholes.fetch_add(1, std::memory_order_relaxed); //reverse sub
						return NULL;
					}
					chunkindex=uint16_t(i);
					memchunks[chunkindex]=res;
					memchunks_refs[chunkindex]=1;
					return res;
				}
			}
			assert(nummemchunks<65536);
			if(nummemchunks>=65536) return NULL; //for release mode do such test anyway

			uint32_t newmax=nummemchunks+1+50;
			if(newmax>65536) newmax=65536;
			void **t=(void**)malloc(sizeof(void*)*newmax);
			if(!t) return NULL;
			int32_t *r=(int32_t*)malloc(sizeof(int32_t)*newmax);
			if(!r) {
				free(t);
				return NULL;
			}
			if(nummemchunks>0) {
				memcpy(t, memchunks, sizeof(void*)*nummemchunks);
				memcpy(r, memchunks_refs, sizeof(int32_t)*nummemchunks);
			}
			if(memchunks) {
				free(memchunks);
				free(memchunks_refs);
			}
			memchunks=t;
			memchunks_refs=r;
			maxmemchunks=newmax;
		}
		res=malloc(size);
		if(!res) return NULL;
		chunkindex=uint16_t(nummemchunks);
		nummemchunks++;
		memchunks[chunkindex]=res;
		memchunks_refs[chunkindex]=1;
		return res;
	}

bool iot_memallocator::do_allocate_freelist(uint32_t &n, uint32_t sz, iot_memobject * &ret, uint32_t OPTIMAL_BLOCK, uint32_t MAX_BLOCK,unsigned maxn) {
	//'n' - minimal amount to be allocated for success. on exit it is updated to show quantity of allocated items
	//'sz' - size of each item
	//'ret' - head of allocated unidirectional list of items will be put here. if 'ret' already has pointer to unidirectional list, this list will be prepended
	//'maxn' - optional maximum quantity of allocated items
	//returns false if 'n' was not satisfied (but less structs can be allocated and returned with 'n' updated to show quantity of allocated)
		uint32_t perchunk;
		uint32_t nchunks;
		uint32_t chunksize;
		
		if(n>maxn) n=maxn;

		perchunk=OPTIMAL_BLOCK/sz;
		if(perchunk < n) {
			//optimal block is not enough for n items, try to take bigger chunk up to MAX_BLOCK
			chunksize=sz*n;
			if(chunksize>MAX_BLOCK && sz<MAX_BLOCK) { //avoid allocation chunks larger than MAX_BLOCK
				perchunk=MAX_BLOCK/sz;
				chunksize=perchunk*sz; //will be >OPTIMAL_BLOCK and <=MAX_BLOCK
				nchunks=(n+perchunk-1)/perchunk; //emulate integer ceil()
				//chunks must be recalculated after first allocation
			} else {
				nchunks=1;
				perchunk=n;
			}
		} else { //optimal block is enough for n and more
			nchunks=1;
			if(perchunk>maxn) perchunk=maxn;
			chunksize=perchunk*sz;
		}

		uint32_t n_good=0;
		uint16_t chunkidx;
		while(nchunks>0) {
			char *t=(char*)do_allocate_direct(chunksize, chunkidx);
			if(!t) { //malloc failure
				if(n_good>=n || perchunk<=1) break; //stop if minimum quantity reached or chunksize cannot be decreased
				perchunk>>=1; //decrease chunksize to have the half of items
				chunksize=perchunk*sz;
				nchunks=((n-n_good)+perchunk-1)/perchunk;
				continue;
			}
			iot_memobject *first=(iot_memobject *)t;
			for(unsigned i=0;i<perchunk-1;i++) {
				((iot_memobject *)t)->next.store((iot_memobject *)(t+sz), std::memory_order_relaxed);
				((iot_memobject *)t)->memchunk=chunkidx;
#ifndef NDEBUG
				((iot_memobject *)t)->refcount.store(0, std::memory_order_relaxed);
#endif
				t+=sz;
			}
			((iot_memobject *)t)->next.store(ret, std::memory_order_relaxed);
			((iot_memobject *)t)->memchunk=chunkidx;
#ifndef NDEBUG
			((iot_memobject *)t)->refcount.store(0, std::memory_order_relaxed);
#endif

			memchunks_refs[chunkidx]=(int32_t)perchunk;
			ret=first;
			nchunks--;
			n_good+=perchunk;
			if(nchunks>0 && n_good+perchunk>maxn) { //another samesized chunk will be allocated which overflows maxn
				assert(n_good<=maxn); //problem with some math above
				perchunk=maxn-n_good;
				if(!perchunk) break; //maxn reached
				nchunks=1;
				chunksize=perchunk*sz;
			}
		}
		if(n_good>=n) {
			n=n_good;
			return true;
		}
		n=n_good;
		return false;
	}

bool iot_memallocator::deinit(void) { //free all OS-allocated chunks
		if(!memchunks) return true;
		//clear all freelists
		for(int i=0;i<15;i++) {
			iot_memobject* lst=freelist[i].pop_all();
			while(lst) {
				assert(lst->refcount==0); //freelist must contain items without refs
				assert(memchunks_refs[lst->memchunk]>0);
				memchunks_refs[lst->memchunk]--;
				lst=lst->next.load(std::memory_order_relaxed);
			}
		}
		bool wasslaveerror=false;
		if(this!=&main_allocator) {
			if(totalinfly.load(std::memory_order_acquire)>0 && !prev_slave) { //deinit of non-main allocator. just become slave of main
				main_allocator.add_slave(this);
				return false;
			}
		} else {
			iot_memallocator *sl, *slnext=slaves_head;
			while((sl=slnext)) {
				slnext=slnext->next_slave;
				if(sl->totalinfly.load(std::memory_order_relaxed)>0) wasslaveerror=true;
				BILINKLIST_REMOVE(sl, next_slave, prev_slave);
				delete sl;
			}
		}
#ifndef NDEBUG
		if(totalinfly.load(std::memory_order_acquire)>0) { //some block was not deallocated. find and print backtrace for all such blocks
			outlog_debug("Leak of memory blocks detected");
			printf("Leak of memory blocks detected\n");
			for(uint32_t i=0;i<nummemchunks;i++) {
				if(memchunks_refs[i]<=0) continue;
				int listindex=((iot_memobject*)memchunks[i])->listindex;
				if(listindex>15) {
					outlog_debug("Cannot determine block size for memory chunk %d", i);
					printf("Cannot determine block size for memory chunk %d\n", i);
					continue;
				}
//				memchunks[i]=NULL;
				int nfound=0, offset=0;
				char timebuf[128];
				for(int j=0; j<(listindex==15 ? 1 : 1024) && nfound<memchunks_refs[i]; j++, offset+=objsizes[listindex]) { //analyze up to 1024 subblocks in memory chunk
					iot_memobject* obj=(iot_memobject*)((char*)memchunks[i]+offset);
					if(obj->refcount==0) continue;
					nfound++;
					struct tm tm1;
					localtime_r(&obj->alloctimeval.tv_sec,&tm1);
					strftime(timebuf, sizeof(timebuf),"%d.%m.%Y %H:%M:%S", &tm1);
					char **symb=backtrace_symbols(obj->backtrace,3);
					outlog_debug("Found unreleased memblock (data 0x%lx) of %d bytes (-1 for arbitrary block), allocated at %s.%06u, backtrace: %s\n\t%s\n\t%s", long(uintptr_t(&obj->data)), listindex==15 ? -1 : int(objsizes[listindex]), timebuf,
						unsigned(obj->alloctimeval.tv_usec), symb[0], symb[1], symb[2]);
					printf("Found unreleased memblock (data 0x%lx) of %d bytes (-1 for arbitrary block), allocated at %s.%06u, backtrace: %s\n\t%s\n\t%s\n", long(uintptr_t(&obj->data)), listindex==15 ? -1 : int(objsizes[listindex]), timebuf,
						unsigned(obj->alloctimeval.tv_usec), symb[0], symb[1], symb[2]);
					free(symb);
				}
			}
			if(prev_slave) return false; //main allocator will assert
		}
#endif
		if(totalinfly.load(std::memory_order_acquire)!=0 || wasslaveerror) {
			assert(false);
			return false;
		}
		uint32_t realholes=0;
		for(uint32_t i=0;i<nummemchunks;i++) {
			if(memchunks_refs[i]==-2) {realholes++;continue;} //a hole
			assert(memchunks_refs[i]==0);
			free(memchunks[i]);
			memchunks[i]=NULL;
		}
		free(memchunks);
		free(memchunks_refs);
		memchunks=NULL;
		memchunks_refs=NULL;
		nummemchunks=0;
		maxmemchunks=0;
		assert(realholes==numholes.load(std::memory_order_relaxed));
		numholes.store(0,std::memory_order_relaxed);
printf("Allocator deinited\n");
		return true;
	}

const uint32_t iot_memallocator::objsizes[15]={
		32,  //index 0
		48,
		64,
		80,
		96,
		128,
		160,
		256,
		384, //index 8
		512,
		1024,
		2048,
		4096,
		IOT_MEMOBJECT_MAXPLAINSIZE+offsetof(struct iot_memobject, data), //index 13
		IOT_MEMOBJECT_CHAINSIZE //index 14 , underlying data for objects must be iot_membuf_chain
	};
const uint32_t iot_memallocator::objoptblock[15]={
		16384,  //index 0
		16384,
		16384,
		32768,
		32768,
		32768,
		32768,
		32768,
		65536, //index 8
		65536,
		65536,
		128*1024,
		256*1024,
		256*1024, //index 13
		256*1024 //index 14 , underlying data for objects must be iot_membuf_chain
	};

iot_memallocator main_allocator;

