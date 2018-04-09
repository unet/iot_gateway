#ifndef IOT_NETPROTO_IOTGW_H
#define IOT_NETPROTO_IOTGW_H
//Contains constants, methods and data structures for managing connections to other gateways

#include<stdint.h>
#include<assert.h>

#include "iot_core.h"
#include "iot_netcon.h"
#include "iot_threadregistry.h"



class iot_netproto_config_iotgw;
struct iot_netproto_session_iotgw;
class iot_gwprotoreq_introduce;
class iot_gwproto_sesregistry;


//#include "iot_configregistry.h"


enum class alloc_method_t : uint8_t {
	STATIC,
	NEW,
	MALLOC,
	MEMBLOCK
};

enum class iot_gwproto_cmd_t : uint8_t {
	ILLEGAL=0,
	INTRODUCE=1,   //sent by client side of connection to introduce itself
};

//bit flags describing type of packet
enum iot_gwproto_reqtype_t : uint8_t {
	IOTGW_IS_REPLY=1,		//packet is reply packet, so on receive must be matched with corresponding request with same cmd, cmd_ver and rqid. when absent, packet is a request
	IOTGW_IS_ASYNC=2,		//request packet which doesn't want any reply. otherwise reply is expected. should not be set for reply packets
};

enum class iot_gwproto_errcode_t : uint8_t {
	OK=0,
	BADCMD=1,			//unknown command or command version
	REPLYTM=2,			//reply timed out (generated on sending side)
	CANTREADREQCHUNKS=3,//p_req_inpacket_wantsfull returned true when buffer_exceeded was true (memory for chunks cannot be allocated or request processing bug)
	CANTREADREPCHUNKS=4,//p_reply_inpacket_wantsfull returned true when buffer_exceeded was true (memory for chunks cannot be allocated or request processing bug)
	NOINTRODUCTION=5,	//command arrived without prior intoduction request

	CUSTOMBASE=32	//custom error statuses processed by specific request classes start from this value
};

//base class for IOTGW protocol request types
struct iot_gwprotoreq {
	iot_gwprotoreq* next=NULL, *prev=NULL;
	uint8_t reqtype;
	const iot_gwproto_cmd_t cmd;
	const uint8_t cmd_ver;
	enum state_t : uint8_t {
		STATE_READ_READY,		//for any reqtype
		STATE_BEING_READ,		//for any reqtype
		STATE_READ,				//for any reqtype
		STATE_EXECUTING,		//for any reqtype
		STATE_SEND_READY,		//for any reqtype
		STATE_SEND_QUEUERED,	//for any reqtype
		STATE_BEING_SENT,		//for any reqtype
		STATE_SENT,				//for REPLY and NOTIFY
		STATE_WAITING_REPLY,	//for REQUEST
//		STATE_REPLIED,			//for REQUEST
		STATE_FINISHED			//for any reqtype
	} state;
	uint32_t rqid; //will be set to actual during send for outgoing request or must be provided in contructor for incoming request/reply or outgoing reply

	alloc_method_t alloc_method; //method of allocation of this struct to select proper deallocation method in release()

	iot_gwprotoreq(uint8_t reqtype, iot_gwproto_cmd_t cmd, uint8_t cmd_ver, alloc_method_t alloc_method, bool is_outgoing, uint32_t rqid=0) : 
		reqtype(reqtype), cmd(cmd), cmd_ver(cmd_ver), state(is_outgoing ? STATE_SEND_READY : STATE_READ_READY), rqid(rqid), alloc_method(alloc_method)
	{
	}

	virtual ~iot_gwprotoreq() {
		assert(next==NULL && prev==NULL);
	}


	void on_session_close(void) { //called for queuered requests when session is closed
		iot_gwprotoreq* p=this;
		release(p);
	}

	static void release(iot_gwprotoreq* &r);

	//request header details to start sending request/reply in STATE_BEING_SENT.
	virtual int p_req_outpacket_start(iot_netproto_session_iotgw* session, uint32_t &data_size, uint8_t &error_status) = 0; //returns error status (todo)
	virtual int p_reply_outpacket_start(iot_netproto_session_iotgw* session, uint32_t &data_size, uint8_t &error_status) = 0; //returns error status (todo)

	//request additional bytes of request/reply in STATE_BEING_SENT state to send out.
	//buf0/buf0size show address and size of preallocated buffer which can be used by request class if necessary.
	//	if used, buf0used must be assigned to show how many bytes have been actually written. 
	//ownbufsvec[maxownbufs] can be used if request class made own memory buffer(s) allocation for sending out. These
	//	buffers must remain valid during STATE_BEING_SENT state or till next call of this func.
	//Must return number of filled items in ownbufsvec in any (can be 0 if buf0 was enough), or error status
	virtual int p_req_outpacket_cont(iot_netproto_session_iotgw* session, size_t offset, char* buf0, size_t buf0size, size_t &buf0used, iovec *ownbufsvec, int maxownbufs) = 0;
	virtual int p_reply_outpacket_cont(iot_netproto_session_iotgw* session, size_t offset, char* buf0, size_t buf0size, size_t &buf0used, iovec *ownbufsvec, int maxownbufs) = 0;

	//called for request/reply when outpacket has been outputted to lower (or lowest?) level
	virtual int p_req_outpacket_sent(iot_netproto_session_iotgw* session) {
		if(reqtype & IOTGW_IS_ASYNC) state=STATE_FINISHED;
		return 0;
	}
	virtual int p_reply_outpacket_sent(iot_netproto_session_iotgw* session) {
		state=STATE_FINISHED;
		return 0;
	}

	//give additional bytes of incoming request/reply to request object in STATE_BEING_READ state
	//data_size contains full size of data (obtained from packet header)
	//offset contains offset of provided chunk in full data
	//chunk/chunksize provide additional chunk of data (offset + chunksize cannot exceed data_size)
	//Returns:
	//0 - chunk successfully processed, but more data required (offset + chunksize must be < data_size)
	//1 - chunk successfully processed and all data received (offset + chunksize must be == data_size and state set to STATE_READ or higher)
	// negative error status
	//IOT_ERROR_OBJECT_INVALIDATED - request was released or mutated to reply, so reference to it must be dropped immediately
	virtual int p_req_inpacket_cont(iot_netproto_session_iotgw* session, size_t data_size, size_t offset, const char* chunk, size_t chunksize) = 0;
	virtual int p_reply_inpacket_cont(iot_netproto_session_iotgw* session, size_t data_size, size_t offset, const char* chunk, size_t chunksize) = 0;

	//for just created incoming request must tell if full packet must be presented to p_[req|reply]_inpacket_cont function.
	//if readbuf size is not enough, buffer_exceeded will be false. return value of true in such case aborts request processing with error
	virtual bool p_req_inpacket_wantsfull(iot_netproto_session_iotgw* session, size_t data_size, bool buffer_exceeded) {
		assert(!buffer_exceeded);
		return true;
	}
	virtual bool p_reply_inpacket_wantsfull(iot_netproto_session_iotgw* session, size_t data_size, bool buffer_exceeded) {
		assert(!buffer_exceeded);
		return true;
	}

	//is called after reading full request/reply packet if state was left as STATE_READ
	//returns error code:
	//IOT_ERROR_OBJECT_INVALIDATED - request was released or mutated to reply, so reference to it must be dropped immediately
	virtual int p_req_inpacket_execute(iot_netproto_session_iotgw* session) = 0;
	virtual int p_reply_inpacket_execute(iot_netproto_session_iotgw* session) = 0;

	//called for request object when corresponding reply is received.
	virtual int p_req_got_reply(iot_netproto_session_iotgw* session, uint32_t data_size, uint8_t errcode) = 0;

private:
};






//metaclass for IOTGW protocol type
class iot_netprototype_metaclass_iotgw : public iot_netprototype_metaclass {
	iot_netprototype_metaclass_iotgw(void) : iot_netprototype_metaclass("iotgw", MESHTUN_PROTO_IOTGW, true, true, 0) {
	}

public:
	static iot_netprototype_metaclass_iotgw object;
};


//keeps configuration for IOTGW protocol sessions
class iot_netproto_config_iotgw : public iot_netproto_config {
public:
	iot_gwinstance *gwinst;
	iot_objref_ptr<iot_peer> const peer;
	iot_netproto_config_iotgw(iot_gwinstance *gwinst, iot_peer* peer, object_destroysub_t destroysub, bool is_dynamic, void* customarg=NULL) : 
		iot_netproto_config(&iot_netprototype_metaclass_iotgw::object, destroysub, is_dynamic, customarg), gwinst(gwinst), peer(peer)
	{
		assert(gwinst!=NULL);
	}

	~iot_netproto_config_iotgw(void) {}

	virtual int instantiate(iot_netconiface* coniface, iot_hostid_t meshtun_peer=0) override; //called in coniface->thread
};




#define IOT_GWPROTO_SENDBUFSIZE 65536
#define IOT_GWPROTO_READBUFSIZE 65536

#define IOT_GWPROTO_MINBUFSIZE (sizeof(iot_netproto_session_iotgw::packet_hdr_prolog)+sizeof(iot_netproto_session_iotgw::packet_hdr)+128)

//represents IOTGW protocol session
struct iot_netproto_session_iotgw : public iot_netproto_session {
//	enum headflags_t : uint8_t {
//	};
	PACKED (
		struct packet_hdr_prolog { //packet prolog. tells which optional parts of header are present.
			uint8_t iotsign[2]; //'I','G' - protocol signature
			uint8_t headflags; //bit field with flags from headflags_t. for future extensions
			uint8_t headlen; //size of header, including all optional and generic part according to headflags
		}
	);
	PACKED (
		struct packet_hdr { //data packet header, always immediately precedes data
			uint8_t reqtype;//bit mask constructed from values from iot_gwproto_reqtype_t
			uint8_t cmd;	//value from iot_gwproto_cmd_t
			uint8_t cmd_ver;//version of command request structure. reply structure must use the same numbering
			uint8_t error;	//error status for reply
			uint32_t sz;	//size of additional data (format depends on cmd)
			uint32_t rqid;	//request ID to be matched between request and reply. zero is unused
		}
	);
	static_assert(sizeof(packet_hdr)==12);

	alignas(16) char sendbuf[IOT_GWPROTO_SENDBUFSIZE >= IOT_GWPROTO_MINBUFSIZE ? IOT_GWPROTO_SENDBUFSIZE : IOT_GWPROTO_MINBUFSIZE];
	alignas(16) char readbuf[IOT_GWPROTO_READBUFSIZE >= IOT_GWPROTO_MINBUFSIZE ? IOT_GWPROTO_READBUFSIZE : IOT_GWPROTO_MINBUFSIZE];

	static const uint8_t packet_signature[2];
	static_assert(sizeof(packet_hdr_prolog::iotsign)==sizeof(packet_signature));

	iot_netproto_session_iotgw *peer_next=NULL, *peer_prev=NULL; //used to keep links for peer->sesreg iot_gwproto_sesregistry object
	iot_objref_ptr<iot_netproto_config_iotgw> const config;
	iot_objref_ptr<iot_peer> peer_host;

	uint32_t next_rqid=1; //incremented for each outgoing request
	iot_gwprotoreq* outpackets_head=NULL, *outpackets_tail=NULL; //outgoing queue. packets are added to tail, taken from head
	iot_gwprotoreq* current_outpacket=NULL; //request being currently sent
	size_t current_outpacket_offset=0, current_outpacket_size=0; //related to current_outpacket

	iot_gwprotoreq* waitingpackets_head=NULL, *waitingpackets_tail=NULL; //reply awaiting queue. waiting requests are added to tail, checked from head
	iot_gwprotoreq* current_inpacket=NULL; //request being read (if header already got)
	size_t current_inpacket_offset=0, current_inpacket_size=0;
	uint32_t readbuf_offset=0;
	bool current_inpacket_wantsfull=false; //when true, means that current_inpacket wants all data in one chunk (so current_inpacket_size must fit readbuf)

	bool introduced;
	uint8_t closed=0; //1 - graceful close: reading stops immediately, writing stops accepting new requests but finishes to send existing and then makes shutdown of coniface
					//2 - immediate close: reading stops immediately, write is stopped immediately by closing coniface
	uint8_t pending_close=0; //1 - graceful close: reading stops immediately, writing stops accepting new requests but finishes to send existing and then makes shutdown of coniface
					//2 - immediate close: reading stops immediately, write is stopped immediately by closing coniface
	bool in_processing=false; //when true, stop() call will just set 'pending_close' var. otherwise do actual stop
	uint8_t inside_write_next=0; //tracks recursive calls of write_next(). 0 - not in write_next(), 1 - in write_next() and not recursive call noticed, 2 - in write_next() and was recursive call

	iot_netproto_session_iotgw(iot_netconiface* coniface, iot_netproto_config_iotgw* config, const iot_objref_ptr<iot_peer> &peer, object_destroysub_t destroysub);
	~iot_netproto_session_iotgw();

	virtual int start(void) override;

	int on_introduce(iot_gwprotoreq_introduce* req, iot_hostid_t host_id, uint32_t core_vers);

	//returns false if request was released
	bool add_req(iot_gwprotoreq* req) { //request should not be used in any way after calling this func!!! it can be released in it (false is returned in such case)
		assert(uv_thread_self()==thread->thread);
		assert(req && !req->prev && !req->next);
		assert(req->state==iot_gwprotoreq::STATE_SEND_READY);

		if(closed) { //new request cannot be added
			req->release(req);
			return false;
		}

		BILINKLISTWT_INSERTTAIL(req, outpackets_head, outpackets_tail, next, prev);
		req->state=iot_gwprotoreq::STATE_SEND_QUEUERED;
		if(coniface->can_write_data()>0) write_next();
		return true;
	}

private:
	virtual void on_stop(bool graceful) override;
//	virtual void on_coniface_detached(void) override {
//		closed=2;
//		//anything to do? for now just wait for destruction
//	}
	virtual void on_write_data_status(int status) override {
		assert(uv_thread_self()==thread->thread);
		assert(status==0);

		assert(current_outpacket);
		assert(current_outpacket->state==iot_gwprotoreq::STATE_BEING_SENT);

		if(closed>1) return;

		if(current_outpacket_offset==current_outpacket_size) {
			in_processing=true;

			int err;
			if((current_outpacket->reqtype & (IOTGW_IS_REPLY | IOTGW_IS_ASYNC)) == 0) { //sync request
				current_outpacket->state=iot_gwprotoreq::STATE_WAITING_REPLY;
				BILINKLISTWT_INSERTTAIL(current_outpacket, waitingpackets_head, waitingpackets_tail, next, prev);
				err=current_outpacket->p_req_outpacket_sent(this);
			} else { //reply or async request
				current_outpacket->state=iot_gwprotoreq::STATE_SENT;
				err=current_outpacket->reqtype & IOTGW_IS_REPLY ?
						current_outpacket->p_reply_outpacket_sent(this) :
						current_outpacket->p_req_outpacket_sent(this);
			}
			if(err) {
				assert(false);
				//todo  (remember about in_processing)
			}
			if(current_outpacket->state==iot_gwprotoreq::STATE_FINISHED) current_outpacket->release(current_outpacket);
			current_outpacket=NULL;

			in_processing=false;
			if(pending_close) {
				stop();
				if(closed>1) return;
			}
		} else {
			assert(current_outpacket_offset<current_outpacket_size);
		}
		write_next();
	}
	void write_next(void) {
		//returns true if next request/part was processed in sync and ones more call to write_next must be done
		assert(uv_thread_self()==thread->thread);
		if(inside_write_next) {inside_write_next=2;return;}
		int err;
again:
		//check if there is any request to send
		in_processing=true;
		size_t sendbuf_startoff;
		if(!current_outpacket) { //no current request. take first from main queue
			if(!outpackets_head) { //nothing to send
				if(closed==1) pending_close=1; //this will initiate rerun of stop()
				goto onexit;
			}
			current_outpacket=outpackets_head;
			BILINKLISTWT_REMOVE(current_outpacket, next, prev);
			assert(current_outpacket->state==iot_gwprotoreq::STATE_SEND_QUEUERED);
			current_outpacket->state=iot_gwprotoreq::STATE_BEING_SENT;

			//prepare packet header
			packet_hdr_prolog *hdr_prolog=(packet_hdr_prolog *)sendbuf;
			memcpy(hdr_prolog->iotsign, packet_signature, sizeof(hdr_prolog->iotsign));
			uint8_t headflags=0;
			uint8_t headlen=sizeof(packet_hdr_prolog);

			packet_hdr *hdr=(packet_hdr *)(sendbuf+headlen);
			headlen+=sizeof(packet_hdr);
			hdr_prolog->headlen=headlen;
			hdr_prolog->headflags=headflags;

			uint32_t data_size=0;
			uint8_t errcode=0;
			if(current_outpacket->reqtype & IOTGW_IS_REPLY) {
				int rval=current_outpacket->p_reply_outpacket_start(this, data_size, errcode);
				if(rval) { //todo
					assert(false);
				}
				hdr->rqid=repack_uint32(current_outpacket->rqid);
			} else {  //request or notification, generate new rqid
				int rval=current_outpacket->p_req_outpacket_start(this, data_size, errcode);
				if(rval) { //todo
					assert(false);
				}
				current_outpacket->rqid=next_rqid;
				hdr->rqid=repack_uint32(next_rqid);
				if(++next_rqid==0) next_rqid=1;
			}
			hdr->reqtype=uint8_t(current_outpacket->reqtype);
			hdr->cmd=uint8_t(current_outpacket->cmd);
			hdr->cmd_ver=current_outpacket->cmd_ver;
			hdr->error=errcode;
			hdr->sz=repack_uint32(data_size);

			current_outpacket_offset=0;
			current_outpacket_size=data_size;
			sendbuf_startoff=hdr_prolog->headlen;
		} else { //continue sending current packet
			assert(current_outpacket->state==iot_gwprotoreq::STATE_BEING_SENT);
			assert(current_outpacket_size>current_outpacket_offset); //packet must have more bytes to send
			sendbuf_startoff=0;
		}

		iovec ownbufs[64]; //index 0 is used for sendbuf[]
		int ownbufsused;
		ownbufsused=1;
		ownbufs[0].iov_base=sendbuf;
		ownbufs[0].iov_len=sendbuf_startoff;
		if(current_outpacket_size>current_outpacket_offset) {
			size_t bufused=0;
			int rval;
			if(current_outpacket->reqtype & IOTGW_IS_REPLY)
				rval=current_outpacket->p_reply_outpacket_cont(this, current_outpacket_offset, sendbuf+sendbuf_startoff, sizeof(sendbuf)-sendbuf_startoff, bufused, ownbufs, 64-ownbufsused);
			else
				rval=current_outpacket->p_req_outpacket_cont(this, current_outpacket_offset, sendbuf+sendbuf_startoff, sizeof(sendbuf)-sendbuf_startoff, bufused, ownbufs, 64-ownbufsused);
			if(rval<0) {
				//todo
				assert(false);
			}
			ownbufsused+=rval;
			ownbufs[0].iov_len+=bufused;

			//update offset
			current_outpacket_offset+=bufused;
			for(int i=1; i<ownbufsused; i++) current_outpacket_offset+=ownbufs[i].iov_len;

			assert(current_outpacket_size>=current_outpacket_offset); //todo
		}

		in_processing=false;
		if(pending_close) {
			stop();
			if(closed>1) return;
		}

		err=coniface->write_data(ownbufs, ownbufsused);
		if(err==1) { //was immediate success, on_write_data_status was not called
			inside_write_next=1;
			on_write_data_status(0);
			if(inside_write_next>1) {
				inside_write_next=0;
				goto again;
			}
			inside_write_next=0;
		} else if(err<0) {
			//some error
			assert(false);
		} //else on_write_data_status will be called asynchronously
		return;
onexit:
		in_processing=false;
		if(pending_close) stop();
		inside_write_next=0;
	}

	bool start_new_inpacket(packet_hdr *hdr);

	virtual bool on_read_data_status(ssize_t nread, char* &databuf, size_t &databufsize) override {
outlog_notice("IOT session got %d bytes", int(nread));
		if(!nread) {  //reading end of connection closed
			stop(false);
			return false;
		}
		if(closed) return false;
		assert(nread>0);
		assert(databuf==readbuf+readbuf_offset);
		assert(readbuf_offset<sizeof(readbuf));
		int err;

		if(!current_inpacket) { //on start of this function header must be not ready and begin at start of readbuf
			assert(readbuf_offset<sizeof(packet_hdr_prolog)); //partial header must begin at start of readbuf
			//simulate single reading at beginning of readbuf
			nread+=readbuf_offset;
			readbuf_offset=0;
		}

		in_processing=true;

		while(nread>0 && !closed) {
			if(!current_inpacket) {
				if(size_t(nread)<sizeof(packet_hdr_prolog)) { //remaining data is not enough for full header prolog, wait for more
					if(readbuf_offset!=0) memcpy(readbuf, readbuf+readbuf_offset, nread); //move partial header to start of readbuf if not at start
					readbuf_offset=nread;
					databuf=readbuf+readbuf_offset; databufsize=sizeof(readbuf)-readbuf_offset;
					goto onexit; //continue reading
				}
				packet_hdr_prolog *hdr_prolog=(packet_hdr_prolog *)(readbuf+readbuf_offset);

				assert(hdr_prolog->headlen==sizeof(packet_hdr_prolog)+sizeof(packet_hdr)); //todo. for now there are no optional header parts

				if(size_t(nread)<hdr_prolog->headlen) { //remaining data is not enough for full header, wait for more
					if(readbuf_offset!=0) memcpy(readbuf, readbuf+readbuf_offset, nread); //move partial header to start of readbuf if not at start
					readbuf_offset=nread;
					databuf=readbuf+readbuf_offset; databufsize=sizeof(readbuf)-readbuf_offset;
					goto onexit; //continue reading
				}

				packet_hdr *hdr=(packet_hdr *)(readbuf+readbuf_offset+sizeof(packet_hdr_prolog));
				if(!start_new_inpacket(hdr)) { //returns false on memory error. may be retry memory allocation after timeout
					if(readbuf_offset!=0) memcpy(readbuf, readbuf+readbuf_offset, nread); //move current unprocessed data to start of readbuf for later retry
					readbuf_offset=nread; //remember obtained data
					assert(false); //todo retrying

					if(pending_close) goto onexit;
					in_processing=false;

					return false;
				}
				readbuf_offset+=hdr_prolog->headlen;
				nread-=hdr_prolog->headlen;
				//else current_inpacket, current_inpacket_wantsfull, current_inpacket_offset, current_inpacket_size assigned correctly
				assert(current_inpacket!=NULL && current_inpacket_offset==0);
				//if current_inpacket_size==0, packet must have state >= STATE_READ
			}
			if(current_inpacket_offset<current_inpacket_size) { //unfinished packet
				assert(current_inpacket->state == iot_gwprotoreq::STATE_BEING_READ);
				//try to feed current_inpacket_size-current_inpacket_offset bytes to request class
				size_t left=current_inpacket_size-current_inpacket_offset;
				size_t chunksize;
				bool last_chunk;
				if(size_t(nread)<left) { //request will not be finished by rest of data
					if(current_inpacket_wantsfull) { //full data requested but data is not full yet.
						assert(current_inpacket_size<=sizeof(readbuf));
						//current_inpacket_offset shows how many bytes of body is already read BEFORE readbuf_offset position
						assert(readbuf_offset>=current_inpacket_offset);
						//virtually consume read bytes
						readbuf_offset+=nread;
						current_inpacket_offset+=nread;
						left-=nread;

						//ensure rest of data fits remaining space and/or optimize readbuf usage
						if(!current_inpacket_offset) { //possible when nread==0 and no body data has been already read
							break; //reset readbuf to full space and continue reading
						}

						if(readbuf_offset>current_inpacket_offset && (sizeof(readbuf)-readbuf_offset<left || readbuf_offset>sizeof(readbuf)/2)) {
							memcpy(readbuf, readbuf+(readbuf_offset-current_inpacket_offset), current_inpacket_offset);
							readbuf_offset=current_inpacket_offset;
						}

						databuf=readbuf+readbuf_offset; databufsize=sizeof(readbuf)-readbuf_offset;
						goto onexit; //continue reading
					}
					chunksize=nread;
					last_chunk=false;
				} else {
					if(current_inpacket_wantsfull) { //full data requested, so current_inpacket_offset shows previously read bytes count
						assert(readbuf_offset>=current_inpacket_offset);
						chunksize=left+current_inpacket_offset;
						readbuf_offset-=current_inpacket_offset;
					} else {
						chunksize=left;
					}
					last_chunk=true;
					current_inpacket->state=iot_gwprotoreq::STATE_READ;
				}
				if(current_inpacket->reqtype & IOTGW_IS_REPLY) //request or notification
					err=current_inpacket->p_reply_inpacket_cont(this, current_inpacket_size, current_inpacket_offset, readbuf+readbuf_offset, chunksize);
					else
					err=current_inpacket->p_req_inpacket_cont(this, current_inpacket_size, current_inpacket_offset, readbuf+readbuf_offset, chunksize);
				if(err==IOT_ERROR_OBJECT_INVALIDATED) {
					assert(last_chunk);
					current_inpacket=NULL;
				} else if(err) {
					assert(false);
					//todo
				}
				if(!last_chunk) { //all read data was fed, use all space of readbuf
					assert(!current_inpacket_wantsfull);
					current_inpacket_offset+=chunksize;
					break; //nread will always become 0 for non-last chunk, readbuf can be reset to full size
				}
				//last chunk
				readbuf_offset+=chunksize;
				nread-=chunksize;
			} else { //for empty body only
				assert(current_inpacket_size==0 && current_inpacket_offset==0);
				current_inpacket->state=iot_gwprotoreq::STATE_READ;
			}
			if(!current_inpacket) continue; //request could mutate into reply in p_req_inpacket_cont
			//here request is full and must wait for execution, be executing, sending reply or finished
			if(current_inpacket->state == iot_gwprotoreq::STATE_READ) {
				//run execution manually
				if(current_inpacket->reqtype & IOTGW_IS_REPLY) //request or notification
					err=current_inpacket->p_reply_inpacket_execute(this);
					else
					err=current_inpacket->p_req_inpacket_execute(this);
				if(err==IOT_ERROR_OBJECT_INVALIDATED) {
					current_inpacket=NULL;
					continue;
				}
				if(err) {
					assert(false);
					//todo
				}
			}
			assert(current_inpacket->state > iot_gwprotoreq::STATE_READ);
			if(current_inpacket->state == iot_gwprotoreq::STATE_FINISHED) {
				current_inpacket->release(current_inpacket);
			} else {
				current_inpacket=NULL;
			}
		}
		readbuf_offset=0;
		databuf=readbuf; databufsize=sizeof(readbuf);
onexit:
		in_processing=false;
		if(pending_close) {stop(); return false;}
		return true;
	}
};



struct iot_gwprotoreq_introduce : public iot_gwprotoreq {
	PACKED (
		struct req_t {
			iot_hostid_t host_id;
			uint32_t core_version;
		}
	);
	static_assert(sizeof(req_t)==sizeof(iot_hostid_t)+sizeof(uint32_t));
	PACKED (
		struct reply_t {
			iot_hostid_t host_id;
			uint32_t core_version;
		}
	);

	enum : uint8_t { //reply error statuses
		ERRCODE_UNKNOWN_HOST=uint8_t(iot_gwproto_errcode_t::CUSTOMBASE),
		ERRCODE_INVALID_AUTH=uint8_t(iot_gwproto_errcode_t::CUSTOMBASE)+1, //repeated introduce with different host
	};
	uint8_t reply_errcode=0;


	iot_gwprotoreq_introduce(uint8_t tp, bool is_outgoing, uint32_t rqid=0, alloc_method_t alloc_method=alloc_method_t::NEW) :
			iot_gwprotoreq(tp, iot_gwproto_cmd_t::INTRODUCE, 0, alloc_method, is_outgoing, rqid) {
	}

	~iot_gwprotoreq_introduce() {
	}
	static int32_t object_size(void) { //no additional args
		return sizeof(iot_gwprotoreq_introduce);
	}
	static iot_gwprotoreq_introduce* create_out_request(iot_memallocator* allocator) { //no additional args
		iot_gwprotoreq_introduce* obj=(iot_gwprotoreq_introduce*)allocator->allocate(object_size());
		if(!obj) return NULL;
		new(obj) iot_gwprotoreq_introduce(0, true, 0, alloc_method_t::MEMBLOCK);
		return obj;
	}
	static iot_gwprotoreq_introduce* create_incoming(uint8_t reqtype, iot_gwproto_cmd_t cmd, uint8_t cmd_ver, uint32_t data_size, uint32_t rqid, iot_memallocator* allocator) {
		assert(cmd==iot_gwproto_cmd_t::INTRODUCE && cmd_ver==0); //todo. return mark about unknown command
		if(reqtype && IOTGW_IS_REPLY) {
			assert(data_size==sizeof(reply_t));
		} else {
			assert(data_size==sizeof(req_t));
		}
		iot_gwprotoreq_introduce* obj=(iot_gwprotoreq_introduce*)allocator->allocate(object_size());
		if(!obj) return NULL;
		new(obj) iot_gwprotoreq_introduce(reqtype, false, rqid, alloc_method_t::MEMBLOCK);
		return obj;
	}
private:
	void mutate_2reply(void) {
		assert((reqtype & (IOTGW_IS_REPLY | IOTGW_IS_ASYNC))==0);
		reqtype=IOTGW_IS_REPLY;
		state=STATE_SEND_READY;
		//do any deallocation/reallocation if necessary
	}

	virtual int p_req_got_reply(iot_netproto_session_iotgw* session, uint32_t data_size, uint8_t errcode) override {
		assert((reqtype & (IOTGW_IS_REPLY | IOTGW_IS_ASYNC))==0);
		assert(state==STATE_WAITING_REPLY);
		reqtype=IOTGW_IS_REPLY;
		reply_errcode=errcode;
		state=STATE_READ_READY;
		//do any deallocation/reallocation if necessary
		return 0;
	}

	virtual int p_req_outpacket_start(iot_netproto_session_iotgw* session, uint32_t &data_size, uint8_t &errcode) override {
		data_size=sizeof(req_t);
		return 0;
	}
	virtual int p_reply_outpacket_start(iot_netproto_session_iotgw* session, uint32_t &data_size, uint8_t &errcode) override {
		data_size=sizeof(reply_t);
		errcode=reply_errcode;
		return 0;
	}
	virtual int p_req_outpacket_cont(iot_netproto_session_iotgw* session, size_t offset, char* buf0, size_t buf0size, size_t &buf0used, iovec *ownbufsvec, int maxownbufs) override {
		req_t req={host_id: repack_hostid(iot_current_hostid), core_version: repack_uint32(iot_core_version)};
		if(offset>=sizeof(req)) {
			assert(false);
			return IOT_ERROR_CRITICAL_BUG;
		}
		size_t left=sizeof(req)-offset;
		if(buf0size>=left) {
			memcpy(buf0, ((char*)&req)+offset, left);
			buf0used=left;
		} else if(buf0size>0) {
			memcpy(buf0, ((char*)&req)+offset, buf0size);
			buf0used=buf0size;
		} else {
			assert(false); //buf0size is zero
			return IOT_ERROR_TRY_AGAIN;
		}
		return 0;
	}
	virtual int p_reply_outpacket_cont(iot_netproto_session_iotgw* session, size_t offset, char* buf0, size_t buf0size, size_t &buf0used, iovec *ownbufsvec, int maxownbufs) override {
		reply_t reply={host_id: repack_hostid(iot_current_hostid), core_version: repack_uint32(iot_core_version)};
		if(offset>=sizeof(reply)) {
			assert(false);
			return IOT_ERROR_CRITICAL_BUG;
		}
		size_t left=sizeof(reply)-offset;
		if(buf0size>=left) {
			memcpy(buf0, ((char*)&reply)+offset, left);
			buf0used=left;
		} else if(buf0size>0) {
			memcpy(buf0, ((char*)&reply)+offset, buf0size);
			buf0used=buf0size;
		} else {
			assert(false); //buf0size is zero
			return IOT_ERROR_TRY_AGAIN;
		}
		return 0;
	}
	virtual int p_req_inpacket_cont(iot_netproto_session_iotgw* session, size_t data_size, size_t offset, const char* chunk, size_t chunksize) override {
		assert(chunksize==data_size);
		req_t* req=(req_t*)chunk;

		//do sync execution
		int rval=session->on_introduce(this, repack_hostid(req->host_id), repack_uint32(req->core_version));
		if(rval<0) { //session will be closed immediately
			state=STATE_FINISHED;
			return 0;
		}
		//end of execution

		if(reqtype & IOTGW_IS_ASYNC) state=STATE_FINISHED;
		else {
			mutate_2reply();
			reply_errcode=uint8_t(rval); //0 or ERRCODE_UNKNOWN_HOST or ERRCODE_INVALID_AUTH

			session->add_req(this);

			if(rval>0) { //was some error
				session->stop(false);
			}
			else outlog_notice("Host %" IOT_PRIhostid " introduced successfully", repack_hostid(req->host_id));
			return IOT_ERROR_OBJECT_INVALIDATED;
		}
		return 0;
	}
	virtual int p_reply_inpacket_cont(iot_netproto_session_iotgw* session, size_t data_size, size_t offset, const char* chunk, size_t chunksize) override {
		assert(chunksize==data_size);
		reply_t* reply=(reply_t*)chunk;

		state=STATE_FINISHED;

		//do sync execution
		int rval=session->on_introduce(this, repack_hostid(reply->host_id), repack_uint32(reply->core_version));
		//end of execution

		if(rval>0) { //was some error
			session->stop(false);
		}
		else if(!rval) outlog_notice("Host %" IOT_PRIhostid " confirmed introduction", repack_hostid(reply->host_id));
		return 0;
	}
	virtual int p_req_inpacket_execute(iot_netproto_session_iotgw* session) override {
		assert(false);
		return IOT_ERROR_CRITICAL_BUG;
	}
	virtual int p_reply_inpacket_execute(iot_netproto_session_iotgw* session) override {
		assert(false);
		return IOT_ERROR_CRITICAL_BUG;
	}

};

/*
struct iot_gwprotoreq_introduce : public iot_gwprotoreq {
	PACKED (
		struct req_t {
			iot_hostid_t host_id;
			uint32_t core_version;
		}
	);

	req_t* reqpacket;
	alloc_method_t reqpacket_alloced;

	iot_gwprotoreq_introduce(alloc_method_t alloc_method) : iot_gwprotoreq(alloc_method), reqpacket(NULL), reqpacket_alloced(alloc_method_t::STATIC) {
	}

	~iot_gwprotoreq_introduce() {
		switch(reqpacket_alloced) {
		}
	}
	int set_reqpacket(alloc_method_t alloced, char* reqdata, int datasize) {
		if(!reqdata || datasize!=calc_reqpacket_size()) return IOT_ERROR_INVALID_ARGS;
		reqpacket_alloced=alloced;
		reqpacket=(req_t*)reqdata;
		return 0;
	}

	static int calc_object_size(void) { //no additional args
		return sizeof(iot_gwprotoreq_introduce);
	}
	static constexpr int calc_reqpacket_size(void) { //no additional args
		return sizeof(req_t);
	}

	static int build_reqpacket(char* buf, int &bufsize) { //no additional args
		if(int(sizeof(req_t))>bufsize) {
			bufsize=int(sizeof(req_t));
			return IOT_ERROR_NO_BUFSPACE;
		}
		assert(buf!=NULL);
		req_t* req=(req_t*)buf;
		req->host_id=repack_hostid(iot_current_hostid);
		req->core_version=repack_uint32(iot_core_version);
		return 0;
	}
};
*/

#endif // IOT_NETPROTO_IOTGW_H
