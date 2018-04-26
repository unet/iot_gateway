#ifndef IOT_NETPROTO_MESH_H
#define IOT_NETPROTO_MESH_H

#include<stdint.h>
#include<assert.h>

#include "iot_core.h"
#include "iot_netcon.h"
//#include "iot_threadregistry.h"

//#include "iot_mesh_control.h"


class iot_netproto_config_mesh;
class iot_netproto_session_mesh;

//TODO recalculate it dynamically?
//value in microseconds!!!
#define ROUTING_DELAY 10

//max allowed value for link delay in microseconds
#define ROUTEENTRY_MAXDELAY 120000000

//theoretical max number of holes is 16 (4 bits are used)
#define IOT_MESHTUNSTREAM_MAXHOLES 2


//Routing table entry created by some mesh session. It describes route to particular host (hostid/hostpeer in struct) through particular mesh session ('session' in struct).
//Each iot_peer has routeslist_head linked list containing such entries (with hostid equal to peer's hostid) in order of increased 'realdelay'.
//Such entries are updated after getting new routing table from session's peer or recalculation of session->cur_delay_us or ROUTING_DELAY

//Before doing any concurrent (dangerouse for concurrent reading) action to this struct inside mesh session code (even during session
//destruction), write lock must be obtained inside corresponding iot_peer object
struct iot_meshroute_entry { 
	iot_meshroute_entry* next, *prev; //for position in iot_peer's routeslist_head list. prev==NULL means that entry is detached
	iot_meshroute_entry* orig_next, *orig_prev; //for position in iot_meshorigroute_entry's routeslist_head list. orig_prev==NULL means that entry is detached

	iot_netproto_session_mesh* session; //parent mesh proto session. must always be copied as iot_objref_ptr to prevent session destruction during pointer usage
	iot_hostid_t hostid; //destination host. cannot be equal to current host
	iot_objref_ptr<iot_peer> hostpeer; //iot_peer corresponding to host
	uint32_t delay; //path delay in microseconds (us) as was reported by direct peer of session. hard limited to ROUTEENTRY_MAXDELAY us
	uint32_t fulldelay; //delay + session->cur_delay_us + ROUTING_DELAY. Theoretical delay in communication and initial value for realdelay. hard limited to ROUTEENTRY_MAXDELAY
	uint32_t realdelay; //initially set to fulldelay. Is used when selecting most quick path to destination host. Can be actualized (corrected) after IOTGW session
						//creation. In such case, following updates to fulldelay (due to routing table update from peer or cur_session_delay update) preserve difference
						//with realdelay. E.g. if at some moment fulldelay=100 and realdelay=109, updating fulldelay to 110
						//will set realdelay as (109-100)+110=119   (so actually ROUTING_DELAY is corrected)
						//till next actualization. Change of realdelay results in resorting iot_peer's route search list (or setting flag in iot_peer, that
						//resorting is necessary)
	uint16_t pathlen; //pathlen as was reported by direct peer of session but incremented by 1 (current session is one more hop in path)
	enum : uint8_t {
		MOD_NONE=0,
		MOD_REMOVE,
		MOD_INSERT,
		MOD_INC, //realdelay was increased or unchanged
		MOD_DEC, //realdelay was decreased
	} pendmod; //pending modification
	uint8_t is_active; //true value shows that this session is selected as active route to hostpeer

	void init(iot_netproto_session_mesh* ses, const iot_objref_ptr<iot_peer> &peer);
	bool update(uint32_t delay_, uint16_t pathlen_, uint32_t ses_delay_, bool preserve_realfull_diff=true) { //ses_delay_ should not exceed 2 minutes
		//returns false if realdelay was assigned to ROUTEENTRY_MAXDELAY
		delay=delay_ >= ROUTEENTRY_MAXDELAY ? ROUTEENTRY_MAXDELAY : delay_;

		uint32_t newfulldelay=delay+ses_delay_+ROUTING_DELAY;
		uint32_t newrealdelay=realdelay;
		if(newfulldelay==0) newfulldelay=1;

		if(preserve_realfull_diff && fulldelay<ROUTEENTRY_MAXDELAY) {
			int32_t diff=realdelay-fulldelay;
			int32_t tmprealdelay=newfulldelay+diff;
			if(tmprealdelay>=ROUTEENTRY_MAXDELAY) {
				realdelay=ROUTEENTRY_MAXDELAY;
				fulldelay=newfulldelay>=ROUTEENTRY_MAXDELAY ? ROUTEENTRY_MAXDELAY : newfulldelay;
				pendmod=prev ? MOD_REMOVE : MOD_NONE; //remove routes with too exceeding delay
				return false;
			}
			newrealdelay = tmprealdelay<=0 ? 1 : uint32_t(tmprealdelay);
			fulldelay=newfulldelay>ROUTEENTRY_MAXDELAY ? ROUTEENTRY_MAXDELAY : newfulldelay;
			goto good;
		}
		if(newfulldelay>=ROUTEENTRY_MAXDELAY) {
			realdelay=fulldelay=ROUTEENTRY_MAXDELAY;
			pendmod=prev ? MOD_REMOVE : MOD_NONE; //remove routes with exceeding delay
			return false;
		}
		newrealdelay=fulldelay=newfulldelay;
good:
		//determine direction of change
		if(prev) { //entry is now in routing table
			if(newrealdelay==realdelay && pathlen==pathlen_) pendmod=MOD_NONE;
			else if(newrealdelay<realdelay) pendmod=MOD_DEC;
			else pendmod=MOD_INC; //will be selected if newrealdelay==realdelay but pathlen_ differs
		} else {
			pendmod=MOD_INSERT;
		}
		realdelay=newrealdelay;
		pathlen=pathlen_;
		return true;
	}
};


//Piece of routing information from peer. Array of such entries is stored in iot_peer object when corresponding peer sends its routing table update (through any of
//mesh sessions to it)
struct iot_meshorigroute_entry {
	iot_meshroute_entry *routeslist_head; //list of route entries which was generated after this orig route
	iot_hostid_t hostid; //destination host. cannot be equal to current host OR source peer host (i.e. that host in whose iot_peer object entry is stored)
	iot_objref_ptr<iot_peer> hostpeer; //iot_peer corresponding to host (evaluated on this side for purpose of speedup)
	uint32_t delay; //path delay in microseconds (us) as was reported by direct peer of session. hard limited to ROUTEENTRY_MAXDELAY us
	uint16_t pathlen; //pathlen as was reported by direct peer of session (this value is incremented by 1 when route entry is copied to mesh session's iot_meshroute_entry)

	static void array_extend(iot_meshorigroute_entry *array, uint32_t cur_len, uint32_t start_idx, uint32_t num) { //extends array of cur_len iot_meshorigroute_entry items by inserting num empty items at start_idx and moving existing items
		if(start_idx<cur_len) {
			memmove(array+start_idx+num, array+start_idx, sizeof(*array)*(cur_len-start_idx));
			memset(array+start_idx, 0, sizeof(*array)*num); //IMPORTANT to invalidate hostpeer as it was copied and helds unaccounted reference
		} else {
			assert(start_idx==cur_len);
		}
	}
	static void array_shrink(iot_meshorigroute_entry *array, uint32_t cur_len, uint32_t start_idx, uint32_t remove_num) { //extends array of cur_len iot_meshorigroute_entry items by inserting num empty items at start_idx and moving existing items
		assert(start_idx+remove_num<=cur_len);
		if(!remove_num) return;
		for(uint32_t i=start_idx; i<start_idx+remove_num; i++) array[i].hostpeer=NULL; //free reference to host in items which will be overwritten
		if(start_idx+remove_num<cur_len) {
			memmove(array+start_idx, array+start_idx+remove_num, sizeof(*array)*(cur_len-start_idx-remove_num));
			memset(array+cur_len-remove_num, 0, sizeof(*array)*remove_num); //IMPORTANT to invalidate hostpeer as it was copied and helds unaccounted reference
		}
	}
};
/*
struct iot_meshses_command {
	volatile std::atomic<iot_meshses_command*> next=NULL; //points to next command in command queue
	enum code_t : uint8_t {
		COM_ILLEGAL=0,
		COM_CHECK_STREAMQ,   //check meshtunq of particular peer
	} code=COM_ILLEGAL;
	uint8_t in_queue=0;

	iot_meshses_command(code_t code) : code(code) {
	}
	void clear_in_queue(void) {
		in_queue=0;
	}
	void set_in_queue(void) {
		in_queue=1;
	}
	bool check_in_queue(void) const {
		return in_queue!=0;
	}
};

struct iot_meshses_command_check_streamq : public iot_meshses_command {
	iot_objref_ptr<iot_peer> peer;

	iot_meshses_command_check_streamq(const iot_objref_ptr<iot_peer> &peer) : iot_meshses_command(COM_CHECK_STREAMQ), peer(peer) {
		assert(peer!=NULL);
	}
};

*/


//metaclass for MESH protocol type
class iot_netprototype_metaclass_mesh : public iot_netprototype_metaclass {
	iot_netprototype_metaclass_mesh(void) : iot_netprototype_metaclass("mesh", 0, false) {
	}

public:
	static iot_netprototype_metaclass_mesh object;
};


//keeps configuration for MESH protocol sessions
class iot_netproto_config_mesh : public iot_netproto_config {
public:
	iot_objref_ptr<iot_peer> const peer; //is NULL for listening connections
	iot_netproto_config_mesh(iot_gwinstance *gwinst, iot_peer* peer, object_destroysub_t destroysub, bool is_dynamic, void* customarg=NULL) : 
		iot_netproto_config(&iot_netprototype_metaclass_mesh::object, gwinst, destroysub, is_dynamic, customarg), peer(peer)
	{
		assert(gwinst!=NULL);
	}

	~iot_netproto_config_mesh(void) {}

	virtual int instantiate(iot_netconiface* coniface, iot_hostid_t meshtun_peer=0) override; //called in coniface->thread
};



#define IOT_MESHPROTO_SENDBUFSIZE 65536
#define IOT_MESHPROTO_READBUFSIZE 65536

#define IOT_MESHPROTO_TUNSTREAM_IDLEN 8

//represents MESH protocol session
class iot_netproto_session_mesh : public iot_netproto_session {
	friend class iot_meshnet_controller;
	friend struct iot_meshtun_stream_listen_state;
	friend struct iot_meshtun_stream_state;

	enum cmd_t : uint8_t {
		CMD_ILLEGAL=0,
		CMD_SERVICE, //service message or error code
		CMD_AUTH_REQ,
		CMD_AUTH_REPLY,
		CMD_AUTH_FINISH,
		CMD_RTABLE_REQ, //request to update routing table (sender updated its routing table subpart on receiver)
		CMD_MESHTUN, //incapsulated mesh stream packet to some host which can be behind direct peer. if not destined locally then is proxied to another peer using routing table
	};
	enum headflags_t : uint16_t {
	};
	enum srvcode_t : uint8_t {
//		SRVCODE_SEQUENCE_ACK, //acknowledge of seq_id of received packets (it will be in qword_param). FOR DATAGRAM connections only
//		SRVCODE_SEQUENCE_BAD, //seq_id of received packet (it will be in qword_param2) is wrong-ordered (too large). next awaited seq_id is in qword_param. FOR DATAGRAM connections only
		SRVCODE_INVALID_AUTHTYPE, //provided authtype (for CMD_AUTH_REQ, CMD_AUTH_REPLY or CMD_AUTH_FINISH) is unallowed. TODO: put allowed list somewhere
		SRVCODE_RTABLE_UPDATED, //sent in reply to CMD_RTABLE_REQ (or may be sometime without it) to notify about routing table update to specific version in qword_param. byte_param (when true) show about some error during parsing or processing of CMD_RTABLE_REQ packet
	};
	enum authtype_t : uint8_t {
		AUTHTYPE_UNSET=0,		//illegal value meaning authtype unset
		AUTHTYPE_PSK_SHA256,	//client's (i.e. active side) password is used to sign AUTH_REQ and AUTH_FINISH, server's password is used to sign AUTH_REPLY. host's serial is used as password or guid (in binary form) when no serial
		AUTHTYPE_RSA_SHA256,	//client's (i.e. active side) RSA private key is used to sign AUTH_REQ and AUTH_FINISH, server's RSA private key is used to sign AUTH_REPLY. data is hashed with SHA256 prior to signing
	};
	enum hevent_t : uint8_t { //possible reasons to call state_handler
		HEVENT_OTHER, //no specific reason or state change
		HEVENT_PHASE_TIMER, //phase timer elapsed
		HEVENT_CAN_WRITE, //new packet can be written
		HEVENT_WRITE_READY, //write request ready
		HEVENT_NEW_PACKET_HEADER, //new packet header was read. current_inpackethdr contains packet header address
		HEVENT_NEW_PACKET_READY, //new packet was fully read
	};

	PACKED (
		struct packet_hdr { //packet header
			uint8_t reserved; //must be zero (version?)
			uint8_t cmd;	//value from cmd_t
			uint16_t headflags; //bit field with flags from headflags_t. for future extensions
			uint32_t sz;	//size of additional data (format depends on cmd)
			//8 bytes
			uint64_t seq_id;//sequence id of this packet. unused when parent connection is streamed (i.e. TCP). for use over UDP or raw sockets. initially zero. //on overflow (when value is zero again) HEADFLAG_SEQID_OVERFLOW must be set
			//16 bytes
		}
	);
	static_assert(sizeof(packet_hdr) % 8==0);
	PACKED (
		struct packet_auth_req { //authentication request. is sent from client to server just after connection was made
			uint32_t guid;
			uint16_t flags; //for future extensions, must be zero
			uint8_t authtype; //value from authtype_t which shows which authtype was used (and is in authdata[])
			uint8_t reserved; //zero
			//8 bytes
			iot_hostid_t srchost; //determines whose public key must be used to check signature
			//16 bytes
			iot_hostid_t dsthost;
			//24 bytes
			uint64_t timestamp_ns; //current time on host
			//32 bytes
			uint8_t random[32];
			//64 bytes
			uint32_t time_synced; //timestamp of last time sync with time source host, in seconds
			uint16_t time_accuracy_ms; //calculated current time accuracy (sum of all synchromization errors plus optional increasing time error calculated between last two sync sessions)
			uint8_t reserved2[2]; //zero bytes
			//72 bytes
			/////above data is signed
			uint64_t reltime_ns; //current relative time on host after computing signature, as close to moment of transmition as possible
			//80 bytes
//ASSUMED because of auth_cache_t
//			uint8_t authdata[]; //auth-type dependent structure (authtype_rsa_sign for AUTHTYPE_RSA), signature includes packet_hdr+packet_auth_req without reltime_ns and authdata
		}
	);
	static_assert(sizeof(packet_auth_req) % 8==0);
	PACKED (
		struct packet_auth_reply { //authentication reply. is sent from server to client in reply to request. when correctly signed it proves that server side owns correct private key, so is authenticated
			uint16_t flags; //for future extensions, must be zero
			uint8_t authtype; //value from authtype_t which shows which authtype was used (and is in authdata[])
			uint8_t reserved; //zero
			uint32_t time_synced; //timestamp of last time sync with time source host, in seconds
			//8 bytes
			uint16_t time_accuracy_ms; //calculated current time accuracy (sum of all synchromization errors plus optional increasing time error calculated between last two sync sessions)
			uint8_t reserved2[6]; //zero bytes
			//16 bytes
			uint64_t timestamp_ns; //current time on host just after getting request, as close to moment of receiving as possible
			//24 bytes
			uint8_t random[32];
			//56 bytes
			uint64_t routing_version; //version of original routing table from srchost on dsthost (for srchost to known that it must resend its routing table)
			//64 bytes
			/////above data is signed
			uint64_t reltime_ns; //current relative time on host after doing all long checks and computing signature, as close to moment of transmition as possible
			//72 bytes
//ASSUMED	uint8_t authdata[]; //auth-type dependent structure (authtype_rsa_sign for AUTHTYPE_RSA), signature includes request.packet_hdr+packet_auth_req
								//without authdata (but with reltime_ns)+packet_hdr+packet_auth_reply without reltime_ns and authdata
		}
	);
	static_assert(sizeof(packet_auth_reply) % 8==0);
	PACKED (
		struct packet_auth_finish { //authentication confirmation. is sent from client to server. when correctly signed it proves that client side owns correct
									//private key, so is authenticated (first request can be duplicated by third party in the middle, so it not enoght to authenticate client)
			uint16_t flags; //for future extensions, must be zero
			uint8_t authtype; //value from authtype_t which shows which authtype was used (and is in authdata[]). MUST MATCH authtype in previous packet_auth_req
			uint8_t reserved[5]; //zero
			//8 bytes
			uint64_t routing_version; //version of original routing table from dsthost on srchost (for dsthost to known that it must resend its routing table)
			//16 bytes
//ASSUMED	uint8_t authdata[]; //auth-type dependent structure (authtype_rsa_sign for AUTHTYPE_RSA), signature includes request.packet_hdr+packet_auth_req
								//without authdata (but with reltime_ns)+reply.packet_hdr+packet_auth_reply without authdata (but with reltime_ns)+
								//packet_hdr+packet_auth_finish without authdata
		}
	);
	static_assert(sizeof(packet_auth_finish) % 8==0);
	PACKED (
		struct packet_rtable_req { //CMD_RTABLE_REQ
			uint64_t version; //version of routing table according to sender's numeration
			uint16_t num_items; //number of items to follow
			uint8_t reserved[6]; //zero
			struct item_t {
				iot_hostid_t hostid; //destination host. cannot be equal to current host OR peer host
				uint32_t delay; //path delay in microseconds (us)
				uint16_t pathlen;
				uint16_t reserved; //zero
			} items[];
		}
	);
	static_assert(sizeof(packet_rtable_req) % 8==0);
	PACKED (
		struct packet_service { //service message or error notification
			uint8_t code; //code from srvcode_t
			uint8_t byte_param;
			uint16_t word_param;
			uint32_t dword_param;
			uint64_t qword_param;
			uint64_t qword_param2;
		}
	);
	static_assert(sizeof(packet_service) % 8==0);
public:
	enum meshtunflags_t : uint16_t {
		MTFLAG_STREAM=1<<0,		//this is tunneled stream and packet_meshtun_stream follows (relative order 1) 
		MTFLAG_STREAM_SYN=1<<1,	//this packet is initiation of stream from src to dst
		MTFLAG_STREAM_FIN=1<<2,	//this packet FINALIZES stream from src to dst (i.e. src will not send more data, but can receive)
		MTFLAG_STREAM_ACK=1<<3,	//flag that ack_sequence field of packet_meshtun_stream is valid
		MTFLAG_STREAM_WANTACK=1<<4,	//can be set to ask peer to send ACK of received data as soon as possible (e.g. if output buffer is filled more than half, ACK will allow to clean it)
		MTFLAG_STREAM_RESET=1<<5,	//cannot be set together with other stream flags. means that connection must be reset, if exists. no reply is assumed
		MTFLAG_STATUS=1<<6,	//shows that packet returns status to mirrored packet. status code is in packet_meshtun_stream::statuscode for MTFLAG_STREAM, TODO for datagrams
	};

	PACKED (
		struct packet_meshtun_hdr { //tunnel over mesh net (incapsulation of connection) to another host. fixed common part for stream and datagram tunnels
			iot_hostid_t srchost;
			iot_hostid_t dsthost;
			//16 bytes
			uint16_t srcport;
			uint16_t dstport;
			uint16_t protoid;
			uint16_t flags; //bit field with flags from meshtunflags_t. determines optional header parts
			//24 bytes
			uint32_t datalen; //length of meshtun data present in this packet after this header and all optional parts
			uint16_t hdrlen; //total length of this and optional header parts and authdata. hdrlen+datalen gives total packet length without packet_hdr
			uint8_t metalen64; //length of meta data in 'meta[]' in 8-byte qwords
			uint8_t ttl; //Time To Live. decremented after every forwarding. packet is dropped if ttl gets to zero
			//32 bytes
			char meta[]; //bytes of meta data if metalen64>0

//ASSUMED after all header parts
//			uint8_t authdata[]; //auth-type dependent structure
		}
	);
	static_assert(sizeof(packet_meshtun_hdr) % 8==0);
	PACKED (
		struct packet_meshtun_stream { //fixed part for tunnelled streams
			uint8_t streamrandom[IOT_MESHPROTO_TUNSTREAM_IDLEN];
			//8 bytes
			uint64_t ack_sequence; //acknowledgement of dsthost's data sequence
			uint64_t data_sequence; //sequence number of mesh stream data present in this packet after this header and all optional parts. must be zero if MSFLAG_INITIATE is set
			//24 bytes
			uint32_t rwndsize; //size of receive window on sending side (i.e. how many bytes after ack_sequence+sum(sack_ranges[i].offset+sack_ranges[i].len) can be received)
			int16_t statuscode; //when MTFLAG_STATUS flag is set keeps status code
			uint8_t num_sack_ranges;
			uint8_t reserved;
			//32 bytes
			PACKED (
				struct sack_range_t { //
					uint32_t offset; //offset from end of previous acknowledged range (or ack_sequence for first range)
					uint32_t len; //length of range
				}
			) sack_ranges[];
		} //size is sizeof(packet_meshtun_stream)+sizeof(packet_meshtun_stream::sack_range_t)*num_sack_ranges
	);
	static_assert(sizeof(packet_meshtun_stream) % 8==0);
private:


	PACKED (
		struct authtype_rsa_sign { //RSA signature optinally hashed with some hashing method
			uint8_t pubkey_id[2]; //first 2 bytes of public key corresponding to private key which was used for signature to select correct one if several are actual
			uint8_t keylen32; //length of private key in 32-bit words
			uint8_t reserved; //must be 0
			uint8_t hashedsign[]; //hashing method dependent bytes of signed hash (e.g. hash_sha256 for SHA256)
		}
	);
	PACKED (
		struct hash_sha256 {
			uint8_t hash[32]; //hashing method dependent bytes of signed hash (e.g. 32 for SHA256)
		}
	);
	static constexpr uint32_t max_authdata_size=sizeof(authtype_rsa_sign)+sizeof(hash_sha256); //max possible size of authdata in auth_req and auth_reply and auth_finish


	PACKED (
		struct auth_cache_t {
			packet_hdr request_hdr;
			packet_auth_req request;
			packet_hdr reply_hdr;
			packet_auth_reply reply;
			packet_hdr fin_hdr;
			packet_auth_finish fin;
		}
	);


	union {
		struct {
			auth_cache_t auth_cache;
			uint64_t finish_reltime_ns; //time when AUTH_FINISH request was first seen by server-side or AUTH_REPLY by client side
		} before_auth;
	} statedata; //state-dependent data
	iot_meshroute_entry *routes=NULL; //iot_memblock of buffer (sized as maxroutes*sizeof(iot_meshroute_entry)) with routing entries obtained from peer.
										//never contains data for current host. data for peer host is always at index 0

	alignas(16) char sendbuf[IOT_MESHPROTO_SENDBUFSIZE];
	alignas(16) char readbuf[IOT_MESHPROTO_READBUFSIZE];

	static_assert(sizeof(sendbuf) >= 4096);
	static_assert(sizeof(readbuf) >= 4096);
public:
	iot_netproto_session_mesh *peer_next=NULL, *peer_prev=NULL; //used to keep links for peer->meshsessions_head list
	iot_objref_ptr<iot_peer> peer_host; //is NULL for passive connections before authentication
private:
	iot_objref_ptr<iot_netproto_config_mesh> const config;
//	mpsc_queue<iot_meshses_command, iot_meshses_command, &iot_meshses_command::next> comq;

	uint64_t next_seq_id=0; //this connection side next sequence id to use. incremented for each outgoing packet
	uint64_t next_peer_seq_id=0; //peer's connection side next assumed sequence id for incoming packet

	void (iot_netproto_session_mesh::* state_handler)(hevent_t)=NULL;

	uv_timer_t phase_timer={};
	uv_async_t comq_watcher={}; //gets signal when new command arrives

	packet_hdr *current_inpacket_hdr=NULL;
	cmd_t current_outpacket_cmd=CMD_ILLEGAL; //request being currently sent
	iot_objref_ptr<iot_meshtun_packet> current_outmeshtun; //keeps reference to meshtun state being currently written (after sending but before
																//getting WRITE_READY event) if current_outpacket_cmd==CMD_MESHTUN
/*	struct { //describes what part of output from current_outmeshtun is being sent (to mark as sent on success and retry in another session on error)
		uint64_t ack_sequence; //acknowledgement of contiguous peer's sequence number (not offseted by initial_sequenced_in)
		uint64_t data_sequence; //start sequence number of data
		uint64_t sack_sequence_start[IOT_MESHTUNSTREAM_MAXHOLES]; //ACK for disordered sequence ranges
		uint32_t sack_sequence_len[IOT_MESHTUNSTREAM_MAXHOLES];
		uint32_t datalen;
		uint16_t flags; //bit field with flags from meshtunflags_t
		uint8_t num_sack_ranges; //number of items in sack_sequence_start[] and sack_sequence_len[]
		uint8_t had_state; //what state had current_outmeshtun
	} current_outmeshtun_segment;
*/
	size_t current_inpacket_offset=0, current_inpacket_size=0;
//	size_t current_outpacket_offset=0, current_outpacket_size=0; //related to current_outpacket_hdr
	uint32_t readbuf_offset=0;
	uint32_t sendbuf_offset=0;

	uint32_t cur_delay_us=0; //current communication delay in microseconds (time from request sending to reply receiving). TODO: add some averaging math which uses several
						//latest value and update this value and all routing entries when difference becomes meaningful (e.g. more than 10%)

	uint32_t numroutes=0; //number of entries in routes table (at least 1 in working state, for peer host)
	uint32_t maxroutes=0; //for how many route entries routes memory buffer was allocated (space for config_registry->num_hosts + 4 entries during allocation)
	uint32_t lastroute=0; //used to cycle through all active routes in round-robin way

	h_state_t phase_timer_state=HS_UNINIT;
	h_state_t comq_watcher_state=HS_UNINIT;

	int16_t current_outmeshtun_segment=-1; //index of iot_meshtun_stream_state::output_inprogress[] item if current_outmeshtun contains iot_meshtun_stream_state ref. value -1 means that binding was cancelled
	uint8_t closed=0; //1 - graceful close in progress: reading stops immediately, writing stops accepting new requests but finishes to send existing and then makes shutdown of coniface
					//2 - immediate close in progress: reading stops immediately, write is stopped immediately
					//3 - close finished
	uint8_t pending_close=0; //1 - graceful close: reading stops immediately, writing stops accepting new requests but finishes to send existing and then makes shutdown of coniface
					//2 - immediate close: reading stops immediately, write is stopped immediately
	bool in_processing=false; //when true, stop() call will just set 'pending_close' var. otherwise do actual stop
//	bool routing_pending=false; //true value means that routing table must be sent to peer after current request is sent
	bool routing_syncing=false; //true value means that routing table sync to peer is waiting for configrmation from peer
	uint8_t routing_needconfirm=0; //>0 when routing table was received from peer and confirmation must be sent. 1 means 'no error', >1 means error
	volatile std::atomic<bool> has_activeroutes={false}; //is set when any route in routes list becomes active. signal must also be sent when activation occurs. clearing must occur inside session code with obtained R-routing lock

	enum state_t : uint8_t {
		STATE_INITIAL,
		STATE_BEFORE_AUTH, //authentication is in progress
		STATE_WORKING, //authentication done
	} state=STATE_INITIAL;



	enum phase_t : uint8_t { //interpretation of phase is state-dependent
		PHASE_INITIAL,
		PHASE_WAITING_PEER,
		PHASE_REQ_PREPARE,
		PHASE_REQ_BEING_SENT,
		PHASE_REPLY_BEING_SENT,
		PHASE_WAITING_PEER_2,
		PHASE_NORMAL,
	} phase=PHASE_INITIAL;

//	static const uint8_t packet_signature[4];
//	static_assert(sizeof(packet_hdr::sign)==sizeof(packet_signature));

public:
	iot_netproto_session_mesh(iot_netconiface* coniface, iot_netproto_config_mesh* config, object_destroysub_t destroysub);
	~iot_netproto_session_mesh();

	virtual int start(void) override;
	iot_hostid_t get_hostid(void) const;


	void send_signal(void) { //now is necessary to notify session about change or routing table for peer
		//any thread
		uv_async_send(&comq_watcher);
	}
/*	void send_command(iot_meshses_command* com) { //any thread
		assert(com!=NULL);
		if(comq.push(com)) {
			uv_async_send(&comq_watcher);
		}
	}
*/

private:
	bool check_privileged_output(void);
	void on_commandq(void);
	bool process_meshtun_output(iot_meshtun_stream_state *st, uint32_t delay, uint16_t pathlen);
	bool process_meshtun_output(iot_meshtun_forwarding *st, uint16_t pathlen);
	void process_meshtun_outputready(iot_meshtun_stream_state *st);
	void process_meshtun_outputaborted(iot_meshtun_stream_state *st);
	bool process_meshtun_proxy_input(packet_meshtun_hdr* req);
	bool process_meshtun_local_input(packet_meshtun_hdr* req);
	bool process_meshtun_input(iot_meshtun_stream_listen_state *st, packet_meshtun_hdr* req);
	bool process_meshtun_input(iot_meshtun_stream_state *st, packet_meshtun_hdr* req);
	bool process_meshtun_status(iot_meshtun_stream_state *st, packet_meshtun_hdr* req);
	bool reset_meshtun_stream(packet_meshtun_hdr* req);

	void send_service_packet(srvcode_t code, uint8_t byte_param=0, uint16_t word_param=0, uint32_t dword_param=0, uint64_t qword_param=0, uint64_t qword_param2=0) {
		assert(uv_thread_self()==thread->thread);
		assert(code);

		packet_hdr* hdr=prepare_packet_header(CMD_SERVICE, sizeof(packet_service));
		packet_service *req=(packet_service*)(hdr+1);

		req->code=code;
		req->byte_param=byte_param;
		req->word_param=repack_uint16(word_param);
		req->dword_param=repack_uint32(dword_param);
		req->qword_param=repack_uint64(qword_param);
		req->qword_param2=repack_uint64(qword_param2);

		int err;
		err=coniface->write_data(hdr, sizeof(*hdr)+sizeof(packet_service));
		assert(err==0);
	}
	void process_service_packet(packet_service* req);

	packet_hdr* prepare_packet_header(cmd_t cmd, uint64_t size64) {
		assert(sendbuf_offset==0);
		if(current_outpacket_cmd!=CMD_ILLEGAL || cmd==CMD_ILLEGAL) {
			assert(false);
			return NULL;
		}
		assert(size64<=0xFFFFFFFF);
		uint32_t size=uint32_t(size64);
		assert(sizeof(packet_hdr)+size+sendbuf_offset < sizeof(sendbuf));

		current_outpacket_cmd=cmd;
		//prepare packet header

		packet_hdr *hdr=(packet_hdr *)(sendbuf+sendbuf_offset);

		memset(hdr, 0, sizeof(*hdr));
		hdr->cmd=cmd;
		hdr->headflags=repack_uint16(0);
		hdr->sz=repack_uint32(size);
		hdr->seq_id=repack_uint64(next_seq_id);
		next_seq_id++;

		sendbuf_offset+=sizeof(*hdr)+size;
		return hdr;
	}
	bool choose_authtype(authtype_t &authtype, uint32_t &authdata_size) { //chooses best available auth type for current host. returns true on success
		//TODO!!!
		if(authtype==AUTHTYPE_UNSET) {
			//select optimal authtype among supported
			authtype=AUTHTYPE_PSK_SHA256;
		}
		switch(authtype) {
			case AUTHTYPE_PSK_SHA256: authdata_size=sizeof(hash_sha256); break;
			default: return false;
		}
		return true;
	}

	void state_change(state_t newstate, phase_t newphase=PHASE_INITIAL, hevent_t ev=HEVENT_OTHER) {
//		state_t prevstate=state;
		state=newstate;
		phase=newphase;
//		phase_timeout=false;
		switch(newstate) {
			case STATE_BEFORE_AUTH:
				memset(&statedata.before_auth, 0, sizeof(statedata.before_auth));
				state_handler=config->peer ? &iot_netproto_session_mesh::state_handler_before_auth_cl : &iot_netproto_session_mesh::state_handler_before_auth_srv;
				break;
			case STATE_WORKING:
				assert(peer_host!=NULL);
				state_handler=&iot_netproto_session_mesh::state_handler_working;
				break;
			default:
				assert(false);
		}
		(this->*state_handler)(ev);
	}
	void stop_phase_timer(void) {
		assert(uv_thread_self()==thread->thread);
		if(phase_timer_state==HS_ACTIVE) {
			phase_timer_state=HS_INIT;
//			phase_timeout=false;
			uv_timer_stop(&phase_timer);
		}
	}
	void run_phase_timer(uint64_t delay_ms) { //schedules call of state_handler
		assert(uv_thread_self()==thread->thread);
		phase_timer_state=HS_ACTIVE;
//		phase_timeout=false;
		uv_timer_start(&phase_timer, [](uv_timer_t* h) -> void {
			iot_netproto_session_mesh* obj=(iot_netproto_session_mesh*)(h->data);

			obj->phase_timer_state=HS_INIT;
//			obj->phase_timeout=true;

			(obj->*(obj->state_handler))(obj->HEVENT_PHASE_TIMER);
		}, delay_ms, 0);
	}

	void state_handler_working(hevent_t);
	void state_handler_before_auth_cl(hevent_t);
	void state_handler_before_auth_srv(hevent_t);

	bool check_authdata(iot_peer* peer, authtype_t atype, const char* authdata, uint32_t authdatasize, const char* data, uint32_t datasize);
	bool fill_authdata(authtype_t atype, char* buf, uint32_t bufsize, const char* data, uint32_t datasize);

	virtual void on_stop(bool graceful) override;
//	virtual void on_coniface_detached(void) override {
//outlog_notice("coniface detached");
//		stop(false);
//		//anything to do? for now just wait for destruction
//	}
	virtual void on_write_data_status(int status) override {
		assert(uv_thread_self()==thread->thread);
		assert(status==0);

		assert(current_outpacket_cmd!=CMD_ILLEGAL);
		sendbuf_offset=0;
		(this->*state_handler)(HEVENT_WRITE_READY);

		//on_can_write_data will be called automatically
	}
/*	virtual void on_can_write_data(void) override {
		assert(uv_thread_self()==thread->thread);

		(this->*state_handler)(HEVENT_CAN_WRITE);
	}
*/
	bool start_new_inpacket(packet_hdr *hdr) {
		uint32_t seq_id=repack_uint64(hdr->seq_id);

		if(expect_false(seq_id!=next_peer_seq_id)) {
			assert(false);
			if(seq_id<next_peer_seq_id) {
				//TODO skip packet, resend SRVCODE_SEQUENCE_ACK
			} else {
				//send SRVCODE_SEQUENCE_BAD asking to resend old packet
			}
			//set something to invalidate packet body
			return true;
		}
		next_peer_seq_id++;

//		cmd_t cmd=cmd_t(hdr->cmd);
		current_inpacket_hdr=hdr;
		current_inpacket_offset=0;
		current_inpacket_size=repack_uint32(hdr->sz);
		switch(state) {
			case STATE_BEFORE_AUTH: //for this state just call handler
			case STATE_WORKING: //for this state just call handler
				(this->*state_handler)(HEVENT_NEW_PACKET_HEADER);
				break;
			default:
				assert(false);
		}
		return true;
	}

	virtual bool on_read_data_status(ssize_t nread, char* &databuf, size_t &databufsize) override {
		assert(uv_thread_self()==thread->thread);

		if(closed) return false;
		assert(nread>0);
		assert(databuf==readbuf+readbuf_offset);
		assert(readbuf_offset<sizeof(readbuf));

//		int err;

		if(!current_inpacket_hdr) { //if no current header then on start of this function header must be not ready and begin at start of readbuf
			assert(readbuf_offset<sizeof(packet_hdr)); //partial header must begin at start of readbuf
			//simulate single reading at beginning of readbuf
			nread+=readbuf_offset;
			readbuf_offset=0;
		}

		in_processing=true;

		while(nread>0 && !closed && !pending_close) {
			if(!current_inpacket_hdr) {
				if(size_t(nread)<sizeof(packet_hdr)) { //remaining data is not enough for full header, wait for more
					if(readbuf_offset!=0) memcpy(readbuf, readbuf+readbuf_offset, nread); //move partial header to start of readbuf if not at start
					readbuf_offset=nread;
					databuf=readbuf+readbuf_offset; databufsize=sizeof(readbuf)-readbuf_offset;
					goto onexit; //continue reading
				}
				packet_hdr *hdr=(packet_hdr *)(readbuf+readbuf_offset);

				if(!start_new_inpacket(hdr)) { //returns false on memory error. may be retry memory allocation after timeout
					if(readbuf_offset!=0) memcpy(readbuf, readbuf+readbuf_offset, nread); //move current unprocessed data to start of readbuf for later retry
					readbuf_offset=nread; //remember obtained data
					assert(false); //todo retrying

					if(pending_close) goto onexit;
					in_processing=false;
					return false;
				}
				if(pending_close) goto onexit; //start_new_inpacket could initiate closing

				readbuf_offset+=sizeof(*hdr);
				nread-=sizeof(*hdr);
				//else current_inpacket_hdr, current_inpacket_offset, current_inpacket_size assigned correctly
				assert(current_inpacket_hdr!=NULL && current_inpacket_offset==0);
			}
			if(current_inpacket_offset<current_inpacket_size) { //unfinished packet
				//try to feed current_inpacket_size-current_inpacket_offset bytes to request class
				size_t left=current_inpacket_size-current_inpacket_offset;
//				size_t chunksize;
//				bool last_chunk;
				if(size_t(nread)<left) { //request will not be finished by rest of data
//					if(current_inpacket_wantsfull) { //full data requested but data is not full yet.
						assert(current_inpacket_size<=sizeof(readbuf)-sizeof(packet_hdr));
						//current_inpacket_offset shows how many bytes of body is already read BEFORE readbuf_offset position
						assert(readbuf_offset>=current_inpacket_offset+sizeof(packet_hdr));
						//virtually consume read bytes
						readbuf_offset+=nread;
						current_inpacket_offset+=nread;
						left-=nread;

						if(readbuf_offset>current_inpacket_offset+sizeof(packet_hdr) && (sizeof(readbuf)-readbuf_offset<left || readbuf_offset>sizeof(readbuf)/2)) {
							memcpy(readbuf, readbuf+(readbuf_offset-current_inpacket_offset-sizeof(packet_hdr)), current_inpacket_offset+sizeof(packet_hdr));
							readbuf_offset=current_inpacket_offset+sizeof(packet_hdr);
							current_inpacket_hdr=(packet_hdr *)(readbuf);
						}

						databuf=readbuf+readbuf_offset; databufsize=sizeof(readbuf)-readbuf_offset;
						goto onexit; //continue reading
//					}
//					chunksize=nread;
//					last_chunk=false;
				}// else {
//					if(current_inpacket_wantsfull) { //full data requested, so current_inpacket_offset shows previously read bytes count
						assert(readbuf_offset>=current_inpacket_offset+sizeof(packet_hdr));
//						chunksize=left+current_inpacket_offset;
//						readbuf_offset-=current_inpacket_offset;
//					} else {
//						chunksize=left;
//					}
//					last_chunk=true;
//					current_inpacket->state=iot_gwprotoreq::STATE_READ;
//				}
//				if(current_inpacket->reqtype & IOTGW_IS_REPLY) //request or notification
//					err=current_inpacket->p_reply_inpacket_cont(this, current_inpacket_size, current_inpacket_offset, readbuf+readbuf_offset, chunksize);
//					else
//					err=current_inpacket->p_req_inpacket_cont(this, current_inpacket_size, current_inpacket_offset, readbuf+readbuf_offset, chunksize);
//				if(err) {
//					assert(false);
//					//todo
//				}
//				if(!last_chunk) { //all read data was fed, use all space of readbuf
//					assert(!current_inpacket_wantsfull);
//					current_inpacket_offset+=chunksize;
//					break; //nread will always become 0 for non-last chunk, readbuf can be reset to full size
//				}
				//last chunk
				readbuf_offset+=left;//chunksize;
				nread-=left;//chunksize;
			} else { //for empty body only
				assert(current_inpacket_size==0 && current_inpacket_offset==0);
//				current_inpacket->state=iot_gwprotoreq::STATE_READ;
			}
			//here request is full and must wait for execution, be executing, sending reply or finished
//			if(current_inpacket->state == iot_gwprotoreq::STATE_READ) {
//				//run execution manually
//				if(current_inpacket->reqtype & IOTGW_IS_REPLY) //request or notification
//					err=current_inpacket->p_reply_inpacket_execute(this);
//					else
//					err=current_inpacket->p_req_inpacket_execute(this);
//				if(err) {
//					assert(false);
//					//todo
//				}
//			}
//			assert(current_inpacket->state > iot_gwprotoreq::STATE_READ);
//			if(current_inpacket->state == iot_gwprotoreq::STATE_FINISHED) {
//				current_inpacket->release(current_inpacket);
//			} else {
//				current_inpacket=NULL;
//			}
			

			//process fully read packet
			switch(state) {
				case STATE_BEFORE_AUTH: //for this state just call handler
				case STATE_WORKING: //for this state just call handler
					(this->*state_handler)(HEVENT_NEW_PACKET_READY);
					break;
				default:
					assert(false);
			}
			current_inpacket_hdr=NULL;
		}
		readbuf_offset=0;
		databuf=readbuf; databufsize=sizeof(readbuf);
onexit:
		in_processing=false;
		if(pending_close) {stop(); return false;}
		return true;
	}
	int resize_routes_buffer(uint32_t n, iot_memallocator* allocator); //must be called with routing_lock in write mode!!!
};

#endif // IOT_NETPROTO_MESH_H
