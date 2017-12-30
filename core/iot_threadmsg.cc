#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

//#include "iot_compat.h"
//#include "evwrap.h"
#include "iot_threadmsg.h"

#include "iot_netcon.h"
#include "iot_moduleregistry.h"
#include "iot_threadregistry.h"

//Return values:
//0 - success
//IOT_ERROR_CRITICAL_BUG - in release mode assertion failed
//IOT_ERROR_NO_MEMORY
int iot_prepare_msg(iot_threadmsg_t *&msg,iot_msg_code_t code, iot_modinstance_item_t* modinst, uint8_t bytearg, void* data, size_t datasize, 
		iot_threadmsg_datamem_t datamem, bool is_core, iot_memallocator* allocator) {
		
		if(!data) {
			assert(datasize==0);
			if(datasize!=0) return IOT_ERROR_CRITICAL_BUG;
			datamem=IOT_THREADMSG_DATAMEM_STATIC;
		} else if(datasize==0) {
			//check that datamem is allowed with zero datasize
			if(datamem!=IOT_THREADMSG_DATAMEM_STATIC && datamem!=IOT_THREADMSG_DATAMEM_MEMBLOCK_NOOPT && datamem!=IOT_THREADMSG_DATAMEM_MALLOC_NOOPT) {
				assert(false);
				return IOT_ERROR_CRITICAL_BUG;
			}
		}
		assert(code != IOT_MSG_INVALID);
		assert(!modinst || modinst->get_miid());

		bool msg_alloced=false;
		if(!msg) {
			if(!allocator) {
				allocator=thread_registry->find_allocator(uv_thread_self());
				assert(allocator!=NULL);
			}

			msg=allocator->allocate_threadmsg(); //sets is_msgmemblock. other fields zeroed
			if(!msg) return IOT_ERROR_NO_MEMORY;

			msg_alloced=true;

		} else {
			//structure must be zeroed or with released content
			assert(msg->code==IOT_MSG_INVALID);
			if(msg->code!=IOT_MSG_INVALID) return IOT_ERROR_CRITICAL_BUG;

/*			if(msg->is_msginstreserv) {
				assert(modinst->get_miid()==msg->miid);
				if(modinst->get_miid()!=msg->miid) return IOT_ERROR_CRITICAL_BUG;
			}*/
		}

		switch(datamem) {
			case IOT_THREADMSG_DATAMEM_STATIC: //provided data buffer points to static buffer or is arbitrary integer, so no releasing required
				//datasize can be zero here
				msg->data=data;
				break;
			case IOT_THREADMSG_DATAMEM_TEMP_NOALLOC: //provided data buffer points to temporary buffer, so it MUST fit IOT_MSG_BUFSIZE bytes or error (assert in debug) will be returned
				if(datasize>IOT_MSG_BUFSIZE) {
					assert(false);
					goto errexit;
				}
				//here 0 < datasize <= IOT_MSG_BUFSIZE
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
				msg->is_memblock=1;
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
				//datasize can be zero here
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
				//datasize can be zero here
				msg->data=data;
				msg->is_malloc=1;
				break;
			default:
				assert(false);
				goto errexit;
		}
		msg->code=code;
		msg->bytearg=bytearg;
		if(modinst) msg->miid=modinst->get_miid();
			else msg->miid.clear();
		msg->datasize=datasize;
		if(is_core) msg->is_core=1;

		return 0;
errexit:
		if(msg_alloced) allocator->release(msg);
		return IOT_ERROR_CRITICAL_BUG;
	}


void iot_release_msg(iot_threadmsg_t *&msg, bool nofree_msgmemblock) { //nofree_msgmemblock if true, then msg struct with is_msgmemblock set is not released (only cleared)
	if(msg->code!=IOT_MSG_INVALID) { //content is valid, so must be released
		if(msg->data) { //data pointer can and must be cleared before calling this method to preserve data
			if(msg->is_releasable) { //object in data was saved as iot_releasable derivative, so its internal data must be released
				iot_releasable *rel=(iot_releasable *)msg->data;
				rel->releasedata();
				msg->is_releasable=0;
			}
			if(msg->is_memblock) {
				assert(msg->is_malloc==0);
				iot_release_memblock(msg->data);
				msg->is_memblock=0;
			} else if(msg->is_malloc) {
				free(msg->data);
				msg->is_malloc=0;
			}
			msg->data=NULL;
		} else {
			msg->is_releasable=msg->is_memblock=msg->is_malloc=0;
		}
		msg->bytearg=0;
		msg->intarg=0;
//		if(!msg->is_msginstreserv) msg->miid.clear();
		msg->datasize=0;
		msg->is_core=0;
		msg->code=IOT_MSG_INVALID;
	}
	if(msg->is_msgmemblock) {
//		assert(msg->is_msginstreserv==0);
		if(!nofree_msgmemblock) {iot_release_memblock(msg);msg=NULL;}
		return;
	}
	assert(!nofree_msgmemblock); //do not allow this options to be passed for non-msgmemblock allocated structs. this can show on mistake
//	if(msg->is_msginstreserv) {
//		iot_modinstance_locker l=modules_registry->get_modinstance(msg->miid);
//		if(l) l.modinst->release_msgreserv(msg);
//		return;
//	}
	//here msg struct must be statically allocated
}
