#ifndef IOT_KAPI_H
#define IOT_KAPI_H

#include<stdint.h>
#include<stddef.h>

#include<uv.h>
#include<ecb.h>

#include<iot_compat.h>
#include<iot_error.h>


//Log levels
#define LDEBUG  0
#define LINFO   1
#define LNOTICE 2
#define LERROR  3

extern int min_loglevel;
extern uv_thread_t main_thread;

#define IOT_MEMOBJECT_MAXPLAINSIZE 8192
#define IOT_MEMOBJECT_MAXREF 15

typedef uint16_t iot_iid_t;
typedef uint16_t iot_connid_t;
typedef uint32_t iot_deviface_classid; //id of driver interface type (abstraction over hardware devices provided by device drivers)
typedef uint32_t iot_hostid_t; //0 is illegal value for host id


static inline uint32_t get_time32(time_t tm) { //gets modified 32-bit timestamp. it has some offset to allow 32-bit var to work many year
	return uint32_t(tm-1000000000);
}

static inline time_t fix_time32(uint32_t tm) { //restores normal timestamp value
	return time_t(tm)+1000000000;
}



ECB_EXTERN_C_BEG

void do_outlog(const char*file, int line, const char* func, int level, const char *fmt, ...);

//release memory block allocated by iot_allocate_memblock
void iot_release_memblock(void *memblock);

//Find correct event loop for specified thread
//Returns 0 on success and fills loop with correct pointer
//On error returns negative error code:
//IOT_ERROR_INVALID_ARGS - thread is unknown of loop is NULL
uv_loop_t* kapi_get_event_loop(uv_thread_t thread);

void kapi_devdriver_self_abort(iot_iid_t iid, void *instance, int errcode);

const char* kapi_devclassid_str(uint32_t clsid);

int kapi_connection_send_client_msg(iot_connid_t connid, void *drv_inst, iot_deviface_classid classid, void* data, uint32_t datasize);

ECB_EXTERN_C_END




struct iot_membuf_chain;
struct iot_membuf_chain {
	iot_membuf_chain *next;
	uint32_t len, total_len; //len - size of useful space in this struct, total_len - sum of all 'len's in this and all following structs (connected with 'next')
	char buf[2]; //determines minimal allowed size of added buffer

	uint32_t init(uint32_t sz) { //sz - size of buffer on which this struct is put over
		if(sz<sizeof(iot_membuf_chain)) return 0;
		next=NULL;
		len=total_len=get_increment(sz);
		return len;
	}
	static unsigned int get_increment(uint32_t sz) { //for provided size of buffer returns increment of useful space
		if(sz<sizeof(iot_membuf_chain)) return 0;
		return sz-offsetof(iot_membuf_chain, buf);
	}
	uint32_t add_buf(char *newbuf, uint32_t sz) { //adds one more buffer to the end of chain. Operation is non-destructive for data in buf
		//returns 0 if provided buffer is too small or new total len otherwise
		iot_membuf_chain* n=(iot_membuf_chain *)newbuf;
		if(!n->init(sz)) return 0;
		uint32_t d=n->len;
		iot_membuf_chain* t=this;
		total_len+=d;
		while(t->next) {
			t=t->next;
			t->total_len+=d;
		}
		t->next=n;
		return total_len;
	}
	char *drop_nextbuf(void) {//removes next buf reference (thus decreasing chain length by 1), connecting its child to current struct as next. 
							//Destroys!!! data in buf if data length is larger than 'len'
		//returns removed buffer address or NULL if no next buffer and nothing was done
		if(!next) return NULL;
		iot_membuf_chain* t=next;
		next=t->next;
		total_len-=t->len;
		return (char*)t;
	}
	bool has_children(void) { //checks if there are any children
		return next!=NULL;
	}
	unsigned count_children(void) { //counts number of connected bufs
		unsigned c=0;
		iot_membuf_chain *p=next;
		while(p) {
			c++;
			p=p->next;
		}
		return c;
	}
};

#ifndef DAEMON_KERNEL
////////////////////////////////////////////////////////////////////
///////////////////////////Specific declarations for external modules
////////////////////////////////////////////////////////////////////

#define kapi_outlog_error(format... ) do_outlog(__FILE__, __LINE__, __func__, LERROR, format)
#define kapi_outlog_notice(format... ) if(min_loglevel <= 2) do_outlog(__FILE__, __LINE__, __func__, LNOTICE, format)
#define kapi_outlog_info(format... ) if(min_loglevel <= 1) do_outlog(__FILE__, __LINE__, __func__, LINFO, format)
#define kapi_outlog_debug(format... ) if(min_loglevel == 0) do_outlog(__FILE__, __LINE__, __func__, LDEBUG, format)
#define kapi_outlog(level, format... ) if(min_loglevel <= level) do_outlog(__FILE__, __LINE__, __func__, level, format)

#endif //DAEMON_KERNEL



//Event subscription


//FD watcher functions
/*
struct kapi_fd_watcher;

typedef void (*kapi_fd_watcher_cb)(kapi_fd_watcher* w, SOCKET fd, bool have_read, bool have_write);

struct kapi_fd_watcher {
	void* _private; //hides real realization from modules

	kapi_fd_watcher_cb cb;
	void *data; //custom user data

	kapi_fd_watcher(void) : _private(NULL), cb(NULL), data(NULL) {} //does nothing exclusive
	~kapi_fd_watcher(void);

	//must be called and return success before calling other methods
	//returns 0 on success or negative error code
	int init(kapi_fd_watcher_cb cb, SOCKET fd, bool want_read, bool want_write, void* customdata=NULL);

	//start watching for events on provided fd
	//returns 0 on success or negative error code
	int start(void);

	//stop watching for events on provided fd
	//returns 0 on success or negative error code
	int stop(void);

	//test if watcher is started
	//returns:
	// 1 if started
	// 0 if NOT started
	// negative error code on error
	int is_active(void);

};
*/
#endif // IOT_KAPI_H
