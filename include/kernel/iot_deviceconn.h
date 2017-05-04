#ifndef IOT_DEVICECONN_H
#define IOT_DEVICECONN_H
//Contains data structures and methods for device handles management

#include<stdint.h>
#include<assert.h>

#include<ecb.h>

#include <iot_module.h>
#include <iot_kapi.h>
#include <kernel/iot_common.h>


#include<kernel/iot_deviceregistry.h>
#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_kernel.h>

//space for IDs of device connections
#define IOT_MAX_DEVICECONNECTIONS 16384


/*Life cycle of connection

						   App startup: Static object construction with connident=={id:0, key:0}
			   client instance started: iot_create_connection() called to init client-side fields (can be delayed until proper driver found)  [state=IOT_DEVCONN_INIT]
				   proper driver found: connect_local or connect_remote called to try to bind driver, request to driver is queued  [state=IOT_DEVCONN_PENDING]
								/																			\
		driver's device_open() returned success and buffer memory allocated:							some error:
				[state=IOT_DEVCONN_READYDRV]														another driver will be tried [state=IOT_DEVCONN_INIT]
				notify client about ready connection												end.
								|
				client's device_attached() called

*/




struct iot_device_connection_t {
	iot_devifaceclass_data devclass;
	iot_hostid_t client_host;
	union client_data_t {
		struct { //remote client_host
			uint32_t module_id;
			iot_mi_inputid_t mi_inputid;
			iot_modinstance_type_t type;
		} remote;
		struct local_client_data_t {
			iot_modinstance_locker modinstlk; //local client_host
			uint8_t dev_idx; //index of this device connection for client modinst
			iot_driverclient_conndata_t *conndata;
			dbllist_node<iot_device_entry_t, iot_mi_inputid_t, uint32_t>* blistnode; //block list node. is assigned when entering pending state
		} local;

		client_data_t(void) {} //necessary to shut up compiler because of iot_modinstance_locker member
		~client_data_t(void) {}
	} client;
	const iot_deviceconn_filter_t* client_devifaceclassfilter; //set according to module config
	const iot_hwdev_ident_t* client_hwdevident; //set according to bound configuration item. can be NULL

	iot_hostid_t driver_host;
	union driver_data_t {
		struct { //remote driver_host
			uint32_t module_id;
			iot_miid_t miid;
//			uint8_t conn_idx; //index of this connection for driver modinst
		} remote;
		struct local_driver_data_t { //local driver_host
			iot_modinstance_locker modinstlk;
//			void *private_data;
			uint8_t conn_idx; //index of this connection for driver modinst
		} local;

		driver_data_t(void) {} //necessary to shut up compiler because of iot_modinstance_locker member
		~driver_data_t(void) {}
	} driver;

	iot_connid_t connident; //ID of this connection if it is used. zero for unused structure
	volatile enum iot_devconn_state_t : uint8_t {
		IOT_DEVCONN_INIT=0,    //connection not initiated yet (connect() wasn't called)
		IOT_DEVCONN_PENDING,   //connection in process of establishment (waiting for readiness from driver)
		IOT_DEVCONN_READYDRV,  //driver-side of connection and connection struct are ready (waiting for readiness from client)
//		IOT_DEVCONN_FULLREADY, //client-side of connection ready
//		IOT_DEVCONN_CLOSED     //connection cannot transmit more data, but already sent data can be read. none or one side of connection is closed (zero 
							   //value of client_host or driver_host show)
	} state;
	
	std::atomic_flag acclock; //lock to protect connection structure when it can be accessed/modified during processing connect in non-main threads.
							//this lock MUST be obtained by main thread before destroying connection which has state>=IOT_DEVCONN_PENDING as there can be 
							//messages in driver's or consumer's queue to work with same connection. Those async operations MUST use locking too.
							//Other threads must check connkey first and treat old connnection as closed on non-match without prior locking acclock. After
							//obtaining lock, connkey must be rechecked again
	iot_conn_drvview drvview;
	iot_conn_clientview clientview;

	iot_threadmsg_t* clientclose_msg; //preallocated msg struct to send message to client about connection close
	iot_threadmsg_t* driverclose_msg; //preallocated msg struct to send message to driver when establishing or closing connection
	iot_threadmsg_t* driverstatus_msg; //preallocated msg struct to send message to driver when establishing or closing connection
	iot_threadmsg_t* c2d_ready_msg; //preallocated msg struct to send message to driver side when it can read or write
	iot_threadmsg_t* d2c_ready_msg; //preallocated msg struct to send message to client side when it can read or write
//	iot_threadmsg_t* c2d_read_ready_msg; //preallocated msg struct to send message to second side when it can read full request or get continuation for streamed requests
//	iot_threadmsg_t* c2d_write_ready_msg; //preallocated msg struct to send message to first side when it can write new request or put continuation for streamed requests
//	iot_threadmsg_t* d2c_read_ready_msg; //preallocated msg struct to send message to second side when it can read full request or get continuation for streamed requests
//	iot_threadmsg_t* d2c_write_ready_msg; //preallocated msg struct to send message to first side when it can write new request or put continuation for streamed requests

	struct packet_hdr {
		uint32_t data_size; //size of pure data that follows
	};
	struct packet_tail {
		uint32_t committed_data_size; //size of actually written data. when less than data_size in header, request is dropped
	};

private:
	void* connbuf; //address of allocated connection buffer (which is ued for c2d.buf and d2c.buf)
	struct direction_state {
		byte_fifo_buf buf; //ring buffer for communication. first side writes (client for c2d), second reads
		bool want_write; //true if signal about free space in buf must be sent (can be configured by first side)
		volatile bool reader_closed; //true if reading side (driver for c2d) already processed close of connection (connection must be in IOT_DEVCONN_CLOSED state)
		packet_hdr read_head;
		packet_tail write_pendingtail, read_pendingtail;

		uint32_t write_size_left; //how many bytes of request must be additionally supplied to end write request. 
									//0 if no active half-written request
		uint32_t read_size_left; //how many bytes of request must be additionally read to end reading of request
									//0 if no active half-read request
		volatile std::atomic<uint32_t> requests; //number of complete (i.e. with tail) requests in corresponding queue available for reading.
	} c2d, d2c;

public:

	iot_device_connection_t(void) {}
	void init_local(iot_connsid_t id, iot_modinstance_item_t *client_inst, uint8_t idx);
	void deinit(void);
	int close(iot_threadmsg_t* asyncmsg=NULL); //any thread

	void lock(void) { //tries to lock structure from modifying
		uint8_t c=0;
		while(acclock.test_and_set(std::memory_order_acquire)) {
			//busy wait
			c++;
			if((c & 0x3F)==0x3F) sched_yield();
		}
	}
	void unlock(void) {
		acclock.clear(std::memory_order_release);
	}

	int connect_remote(iot_miid_t& driver_inst, const iot_devifaceclass_id_t* ifaceclassids, uint8_t num_ifaceclassids);
	int connect_local(iot_modinstance_item_t* driver_inst);
	int process_connect_local(bool); //called in working thread of driver instance
	int on_drvconnect_status(int err, bool isasync); //depending on state can run in different threads
	void process_driver_ready(void); //runs in client thread after driver finished connection
	void process_close_client(iot_threadmsg_t* msg); //client instance  thread
	void process_close_driver(iot_threadmsg_t* msg); //driver instance  thread

	//tries to write a message to driver's in-queue in full
	//returns:
	//0 - success
	//IOT_ERROR_INVALID_ARGS - datasize is zero
	//IOT_ERROR_TRY_AGAIN - not enough space in queue, but it can appear later (buffer size is enough)
	//IOT_ERROR_NO_BUFSPACE - not enough space in queue, and it cannot appear later (TODO use another type of call)
	int send_driver_message(const void* data, uint32_t datasize); //can be called in client thread only

	//tries to write a message to clients's in-queue in full
	//returns:
	//0 - success
	//IOT_ERROR_INVALID_ARGS - datasize is zero
	//IOT_ERROR_TRY_AGAIN - not enough space in queue, but it can appear later (buffer size is enough)
	//IOT_ERROR_NO_BUFSPACE - not enough space in queue, and it cannot appear later (TODO use another type of call)
	int send_client_message(const void* data, uint32_t datasize); //can be called in driver thread only

	//checks if client buffer has incomming data and sends appropriate signal if so
//	void check_client_queue(void) {
//		if(d2c.buf.pending_read()>0) {
//			uv_async_send(&d2c.read_ready);
//		}
//	}

	void on_c2d_ready(void);
//	void on_c2d_write_ready(void) {} //TODO

	void on_d2c_ready(void);
//	void on_d2c_write_ready(void) {} //TODO

private:

	void d2c_ready(void); //called by driver after writing data to d2c stream buffer
//	void c2d_write_ready(void); //called by driver after reading data from c2d stream buffer if c2d.want_write is true

	void c2d_ready(void); //called by client after writing data to c2d stream buffer
//	void d2c_write_ready(void); //called by client after reading data from d2c stream buffer if d2c.want_write is true


	//tries to write a message to corresponding queue in full
	//returns:
	//0 - success
	//IOT_ERROR_INVALID_ARGS - datasize is zero
	//IOT_ERROR_TRY_AGAIN - not enough space in queue, but it can appear later (buffer size is enough)
	//IOT_ERROR_NO_BUFSPACE - not enough space in queue, and it cannot appear later (TODO use another type of call)
	template <direction_state iot_device_connection_t::*dir>
	int send_message(const void* data, uint32_t datasize) {
		if(datasize==0 || datasize>0x3fffffff) return IOT_ERROR_INVALID_ARGS;

		uint32_t space=(this->*dir).buf.avail_write();
		if(space<datasize+sizeof(packet_hdr)+sizeof(packet_tail)) {
			if((this->*dir).buf.getsize()<datasize+sizeof(packet_hdr)+sizeof(packet_tail)) return IOT_ERROR_NO_BUFSPACE;
			return IOT_ERROR_TRY_AGAIN;
		}
		uint32_t rval=write_start<dir>((const char*)data, datasize, datasize);
		assert(rval==datasize);
		return 0;
	}

	//add request to client input buffer.
	//returns actually written bytes.
	//if returned value is less than sz (it cannot be greater) but > 0, request was not written entirely and additional calls 
	//to write_client_end() must be made to write full request
	//if 0 is returned, write_client_start() must be retried again (with another request if necessary)
	template <direction_state iot_device_connection_t::*dir>
	uint32_t write_start(const char *data, uint32_t sz, uint32_t fullsz=0) {
		if(!fullsz) fullsz=sz;
		assert(fullsz>1 && fullsz<=0x3fffffff && sz<=fullsz);
		if(sz==0) return 0;
		uint32_t rval;

		if((this->*dir).write_size_left>sizeof(packet_tail)) { //pending request bytes and tail
			//here prev request was not written completely, so user requests to abort it
			rval=(this->*dir).buf.write_zero((this->*dir).write_size_left-sizeof(packet_tail)); //try to fill left request space by zeros
			(this->*dir).write_size_left-=rval;
			if((this->*dir).write_size_left>sizeof(packet_tail)) return 0; //still cannot finish request space. request to retry
			//request space filled
			//go on with next condition
		}

		if((this->*dir).write_size_left>0 && (this->*dir).write_size_left<=sizeof(packet_tail)) { //pending tail
			rval=(this->*dir).buf.write(((char*)&(this->*dir).write_pendingtail)+(sizeof(packet_tail)-(this->*dir).write_size_left), (this->*dir).write_size_left);
			(this->*dir).write_size_left-=rval;
			if((this->*dir).write_size_left>0) return 0; //still cannot finish tail. request to retry
			//tail written
			(this->*dir).requests.fetch_add(1, std::memory_order_acq_rel); //count complete requests, even corrupted
			//go on with next condition
		}

		//no active half-written request
		assert((this->*dir).write_size_left==0);
		packet_hdr hdr;
		if((this->*dir).buf.avail_write()<sizeof(hdr)) return 0; //no space for header, request to repeat current call
		hdr.data_size=fullsz;
		rval=(this->*dir).buf.write(&hdr, sizeof(hdr));
		assert(rval==sizeof(hdr)); //must always succeed

		//mark that there is half-written request. add size of tail
		(this->*dir).write_size_left=fullsz+sizeof(packet_tail);

		rval=(this->*dir).buf.write(data, sz);
		(this->*dir).write_size_left-=rval;
		(this->*dir).write_pendingtail.committed_data_size=rval;
		if((this->*dir).write_size_left==sizeof(packet_tail)) { //full request written, try to write tail
			assert(rval==fullsz);
			rval=(this->*dir).buf.write(&(this->*dir).write_pendingtail, sizeof(packet_tail));
			(this->*dir).write_size_left-=rval;
			if((this->*dir).write_size_left==0) {
				assert(rval==sizeof(packet_tail));
				(this->*dir).requests.fetch_add(1, std::memory_order_acq_rel); //count complete requests
				return sz; //return status of full write
			}
			//here request written completely but no space for tail
			assert((this->*dir).write_size_left<=sizeof(packet_tail)); //such condition indicates that only tail with full commit is pending
			return sz==fullsz ? sz-1 : rval; //force user to call (this->*dir).write_end() when there is no space for tail. actually (this->*dir).write_start will also try to finish writing tail
		}
		//not full request written. (this->*dir).write_end must be called until it indicates full write
		assert((this->*dir).write_size_left>sizeof(packet_tail));
		return rval;
	}

	template <direction_state iot_device_connection_t::*dir>
	uint32_t write_end(const char *data, uint32_t sz) {
		if(sz==0) return 0;
		uint32_t rval;
		if((this->*dir).write_size_left>sizeof(packet_tail)) { //pending request bytes and tail
			//here prev request was not written completely, user tries to finish it
			assert((this->*dir).write_size_left-sizeof(packet_tail) >= sz);
			if((this->*dir).write_size_left-sizeof(packet_tail) < sz) return 0xffffffff; //for release mode

			rval=(this->*dir).buf.write(data, sz);
			(this->*dir).write_size_left-=rval;
			(this->*dir).write_pendingtail.committed_data_size+=rval; //increase commited bytes counter
			if((this->*dir).write_size_left>sizeof(packet_tail)) {
				//request bytes still not finished
				return rval;
			}
			//only tail left
			assert(rval==sz);
			assert((this->*dir).write_size_left==sizeof(packet_tail));

			rval=(this->*dir).buf.write(&(this->*dir).write_pendingtail, sizeof(packet_tail));
			(this->*dir).write_size_left-=rval;
			if((this->*dir).write_size_left==0) {
				assert(rval==sizeof(packet_tail));
				(this->*dir).requests.fetch_add(1, std::memory_order_acq_rel); //count complete requests
				return sz; //return status of full write
			}
			//here request written completely but no space for tail
			assert((this->*dir).write_size_left<=sizeof(packet_tail)); //such condition indicates that only tail with full commit is pending
			return 0; //force user to call (this->*dir).write_end() again
		}
		if((this->*dir).write_size_left==0) return 0xffffffff;
		//tail must be retried. arguments ignored

		rval=(this->*dir).buf.write(((char*)&(this->*dir).write_pendingtail)+(sizeof(packet_tail)-(this->*dir).write_size_left), (this->*dir).write_size_left);
		(this->*dir).write_size_left-=rval;
		if((this->*dir).write_size_left>0) return 0; //still cannot finish tail. request to retry
		(this->*dir).requests.fetch_add(1, std::memory_order_acq_rel); //count complete requests
		return sz; //indicate full requested write
	}
//read (or discard when buf==NULL) request
//status is returned after every call:
// 0 - continue to call read_client (even if szleft is 0 status may be still unknown)
// 1 - request fully read and it is good
// -1 - request corrupted. return value can be any (new read_client call will read next request)
//return value shows how many bytes were written into buf (or discarded if buf is NULL)
//szleft shows how many bytes of request left, so that necessary buffer could be provided in full. zero indicates that 
//	either nothing to read, or request tail is still on its way to say if request is OK or corrupted
	template <direction_state iot_device_connection_t::*dir>
	uint32_t read(char *buf, uint32_t bufsz, uint32_t &szleft, int &status) {
		uint32_t rval;

		if((this->*dir).read_size_left==0) {
			//no active half-read request
			//new request must be read
			if((this->*dir).buf.pending_read()<sizeof(packet_hdr)) {//no bytes for header, request to repeat
				szleft=0;
				status=0;
				return 0;
			}
			rval=(this->*dir).buf.read(&(this->*dir).read_head, sizeof(packet_hdr));
			assert(rval==sizeof(packet_hdr)); //must always succeed

			//mark that there is half-read request. add size of tail
			(this->*dir).read_size_left=(this->*dir).read_head.data_size+sizeof(packet_tail);
			//go on
		}

		uint32_t wasread=0;

		if((this->*dir).read_size_left>sizeof(packet_tail)) { //pending request bytes and tail
			if(bufsz>0) {
				wasread=(this->*dir).buf.read(buf, bufsz < (this->*dir).read_size_left-sizeof(packet_tail) ?
											bufsz :
											(this->*dir).read_size_left-sizeof(packet_tail));
				(this->*dir).read_size_left-=wasread;
			}

			if((this->*dir).read_size_left>sizeof(packet_tail)) { //still not finished. request to retry
				szleft=(this->*dir).read_size_left-sizeof(packet_tail);
				status=0;
				return wasread;
			}
			//request finished
			assert((this->*dir).read_size_left==sizeof(packet_tail));
			//go on and read tail
		}

		//pending tail
		assert((this->*dir).read_size_left>0);
		szleft=0;

		//read tail
		rval=(this->*dir).buf.read(((char*)&(this->*dir).read_pendingtail)+(sizeof(packet_tail)-(this->*dir).read_size_left), (this->*dir).read_size_left);
		(this->*dir).read_size_left-=rval;
		if((this->*dir).read_size_left>0) { //still cannot read tail. request to retry
			status=0;
			return wasread;
		}

		//tail read compeletely
		if((this->*dir).read_pendingtail.committed_data_size==(this->*dir).read_head.data_size) status=1;
			else status=-1;
		(this->*dir).requests.fetch_sub(1, std::memory_order_acq_rel); //count complete requests
		return wasread;
	}

//Try to peek start of request for client. Buffer is not modified in any way.
//Arguments:
//buf - buffer to write to. if NULL then maximum possible size is examined to find end of message (bufsize is ignored and assumed 0)
//bufsize - available space in buf. Can be zero if only szleft and status required according to current state of buffer
//Returns number of actually written bytes to buf (so cannot exceed bufsize or be positive with NULL buf).
//Returned in args:
//szleft - show how many bytes left to read to finish reading of current request. can be zero if no requests or packet head is not full
//status:
//	0 - there is nothing to read (returned value will be zero) or current request was not read comletely (no data or provided space in buffer is not enough)
//	1 - request if fully read (or can be fully read with non-NULL buf) and it is good (not corrupted)
//	-1 - request is fully read (or can be fully read with non-NULL buf) and it is corrupted (so returned data must be discarded)
//	-2 - there is half-written request. Only szleft is meaningful and shows how many bytes must be read to read the rest (it can be zero if only packet tail is pending).
	template <direction_state iot_device_connection_t::*dir>
	uint32_t peek_msg(char *buf, uint32_t bufsz, uint32_t &szleft, int &status) {
		uint32_t rval;

		if((this->*dir).read_size_left>0) {
			if((this->*dir).read_size_left>sizeof(packet_tail)) { //still not finished. request to retry
				szleft=(this->*dir).read_size_left-sizeof(packet_tail);
			} else {
				szleft=0;
			}
			status=-2;
			return 0;
		}

		//no active half-read request
		//new request must be read
		if((this->*dir).buf.pending_read()<sizeof(packet_hdr)) {//no bytes for header, request to repeat
			szleft=0;
			status=0;
			return 0;
		}
		packet_hdr head;
		rval=(this->*dir).buf.peek(&head, sizeof(packet_hdr));
		assert(rval==sizeof(packet_hdr)); //must always succeed

		uint32_t left=head.data_size+sizeof(packet_tail);

		uint32_t wasread=0;

		if(buf) {
			if(bufsz>0) {
				wasread=(this->*dir).buf.peek(buf, bufsz < head.data_size ?
										bufsz :
										head.data_size, sizeof(packet_hdr));
				left-=wasread;
			}
			if(left>sizeof(packet_tail)) { //still not finished. request to retry
				szleft=left-sizeof(packet_tail);
				status=0;
				return wasread;
			}
			szleft=0;
		} else { //skip size of whole packet if possible
			rval=(this->*dir).buf.peek(NULL, head.data_size, sizeof(packet_hdr));
			left-=rval;
			if(left>sizeof(packet_tail)) { //still not finished. request to retry
				szleft=0;
				status=0;
				return wasread;
			}
			szleft=head.data_size;
		}

		//request finished
		assert(left==sizeof(packet_tail));
		//go on and read tail

		//read tail
		packet_tail tail;
		rval=(this->*dir).buf.peek(&tail, left, head.data_size+sizeof(packet_hdr));
		left-=rval;
		if(left>0) { //still cannot read tail. request to retry
			status=0;
			return wasread;
		}

		//tail read compeletely
		if(tail.committed_data_size==head.data_size) status=1;
			else status=-1;
		return wasread;
	}

	//add request to client input buffer.
	//returns actually written bytes.
	//if returned value is less than sz (it cannot be greater) but > 0, request was not written entirely and additional calls 
	//to write_client_end() must be made to write full request
	//if 0 is returned, write_client_start() must be retried again (with another request if necessary)
	uint32_t write_client_start(const char *data, uint32_t sz, uint32_t fullsz=0);
	//add request to driver input buffer.
	uint32_t write_driver_start(const char *data, uint32_t sz, uint32_t fullsz=0);

	//write additional bytes of unfinished request to client input buffer.
	//returns actually written bytes.
	//returns 0xffffffff on error (if provided sz is greater then it should be accorting to write_client_start call params or
	//		just no active half-written request, new request should be started)
	uint32_t write_client_end(const char *data, uint32_t sz);
	//write additional bytes of unfinished request to driver input buffer.
	uint32_t write_driver_end(const char *data, uint32_t sz);

	uint32_t read_client(char *buf, uint32_t bufsz, uint32_t &szleft, int &status);
	uint32_t peek_client_msg(char *buf, uint32_t bufsz, uint32_t &szleft, int &status);

	uint32_t read_driver(char *buf, uint32_t bufsz, uint32_t &szleft, int &status);
	uint32_t peek_driver_msg(char *buf, uint32_t bufsz, uint32_t &szleft, int &status);


};

iot_device_connection_t* iot_create_connection(iot_modinstance_item_t *client_inst, uint8_t idx);
iot_device_connection_t* iot_find_device_conn(const iot_connid_t &connid);


#endif //IOT_DEVICECONN_H
