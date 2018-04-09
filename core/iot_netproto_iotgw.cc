#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include "iot_netproto_iotgw.h"
#include "iot_peerconnection.h"
#include "iot_threadregistry.h"


iot_netprototype_metaclass_iotgw iot_netprototype_metaclass_iotgw::object;

//static const char* reqtype_descr[uint8_t(iot_gwproto_reqtype_t::MAX)+1]={"reply", "request", "notification"};

void iot_gwprotoreq::release(iot_gwprotoreq* &r) {
		assert(r!=NULL);
		switch(r->alloc_method) {
			case alloc_method_t::STATIC:
				break;
			case alloc_method_t::NEW:
				delete r;
				break;
			case alloc_method_t::MALLOC:
				r->~iot_gwprotoreq();
				free(r);
				break;
			case alloc_method_t::MEMBLOCK:
				r->~iot_gwprotoreq();
				iot_release_memblock(r);
				break;
			default:
				assert(false);
		}
		r=NULL;
	}


int iot_netproto_config_iotgw::instantiate(iot_netconiface* coniface, iot_hostid_t meshtun_peer) {
		assert(uv_thread_self()==coniface->thread->thread);
		iot_objref_ptr<iot_peer> p;
		if(meshtun_peer) {
			p = gwinst->peers_registry->find_peer(meshtun_peer);
			if(!p) return IOT_ERROR_NO_PEER;
			assert(!peer || peer==p);
		}
		auto ses=new iot_netproto_session_iotgw(coniface, this, !p ? peer : p, object_destroysub_delete);
		if(!ses) return IOT_ERROR_NO_MEMORY;
		return 0;
	}



const uint8_t iot_netproto_session_iotgw::packet_signature[2]={'I', 'G'};

iot_netproto_session_iotgw::iot_netproto_session_iotgw(iot_netconiface* coniface, iot_netproto_config_iotgw* config, const iot_objref_ptr<iot_peer> &peer, object_destroysub_t destroysub) :
			iot_netproto_session(&iot_netprototype_metaclass_iotgw::object, coniface, destroysub), config(config), peer_host(peer), introduced(false)
	{
		assert(config!=NULL);
		if(peer_host) {
			introduced=true;
		}
	}
iot_netproto_session_iotgw::~iot_netproto_session_iotgw() {
		if(peer_host && peer_prev) {
			peer_host->on_dead_iotsession(this);
		}
		//todo do something with requests in queue
		if(current_outpacket) {
			current_outpacket->on_session_close();
			current_outpacket=NULL;
		}
		for(iot_gwprotoreq* nextreq, *curreq=outpackets_head; curreq; curreq=nextreq) {
			nextreq=curreq->next;
			BILINKLISTWT_REMOVE(curreq, next, prev);
			curreq->on_session_close();
		}
		for(iot_gwprotoreq* nextreq, *curreq=waitingpackets_head; curreq; curreq=nextreq) {
			nextreq=curreq->next;
			BILINKLISTWT_REMOVE(curreq, next, prev);
			curreq->on_session_close();
		}
		outlog_notice("IOTGW SESSION DESTROYED");
	}


int iot_netproto_session_iotgw::start(void) {
		assert(uv_thread_self()==thread->thread);

		if(!peer_host) { //this should be listening side of connection which yet doesn't know its peer, setup timer to get intoduction from other side
		} else { //if(!introduced) { //this is connecting side of connection which knows its peer (as it should be according to config) and introduction to another side is required
			iot_gwprotoreq_introduce* req=iot_gwprotoreq_introduce::create_out_request(coniface->allocator);
			if(!req) return IOT_ERROR_NO_MEMORY;
			add_req(req);
		}
		readbuf_offset=0;
		int err=coniface->read_data(readbuf, sizeof(readbuf));
		return err;
	}

void iot_netproto_session_iotgw::on_stop(bool graceful) {
		assert(uv_thread_self()==thread->thread);
		if(in_processing) { //stop() must be repeated after processing
			assert(closed<3);
			if(!pending_close || (!graceful && pending_close==1)) pending_close=graceful ? 1 : 2;
			return;
		}
		uint8_t pclose=pending_close;
		pending_close=0;

		if(closed==3) return; //stop already finished

		if(!closed) {
			//common steps to initiate both types of stop

			if(!graceful || pclose>=2) {
				closed=2;
				//initiate hard stop
				//for now just continue
			}
			else { //can initiate graceful or hard stop
				if(current_outpacket || outpackets_head) { //there are requests to send, initiate graceful stop
					closed=1;
					return;
				} 
				//no requests, use hard stop
				closed=2;
			}
		} else if(closed==1) { //graceful stop in progress. allow upgrade to hard stop
			if(graceful && pclose<2) {
				//repeated graceful request or changing of some state involved by graceful stop. return if need to wait more or upgrade to hard stop
				if(current_outpacket || outpackets_head) return; //there are requests to send, initiate graceful stop
				closed=2; //no requests, use hard stop
			} else { //upgrade to hard stop
				closed=2;
				//initiate hard stop after promoting graceful to hard
			}
		}
		//initiate/continue hard stop
		if(peer_host && peer_prev) { //unregister from peer
			peer_host->on_dead_iotsession(this);
		}

		int8_t num_closing=0;

/*		if(phase_timer_state>=HS_INIT) {
			phase_timer_state=HS_CLOSING;
			num_closing++;
			uv_close((uv_handle_t*)&phase_timer, [](uv_handle_t* handle) -> void {
				iot_netproto_session_mesh* obj=(iot_netproto_session_mesh*)(handle->data);
				assert(obj->phase_timer_state==HS_CLOSING);
				obj->phase_timer_state=HS_UNINIT;
				obj->on_stop(true);
			});
		} else if(phase_timer_state==HS_CLOSING) num_closing++;

		if(comq_watcher_state>=HS_INIT) {
			comq_watcher_state=HS_CLOSING;
			num_closing++;
			uv_close((uv_handle_t*)&comq_watcher, [](uv_handle_t* handle) -> void {
				iot_netproto_session_mesh* obj=(iot_netproto_session_mesh*)(handle->data);
				assert(obj->comq_watcher_state==HS_CLOSING);
				obj->comq_watcher_state=HS_UNINIT;
				obj->on_stop(true);
			});
		} else if(comq_watcher_state==HS_CLOSING) num_closing++;
*/
		if(num_closing>0) return;

		//can be destroyed from this point
		closed=3;
		self_closed();
	}


bool iot_netproto_session_iotgw::start_new_inpacket(packet_hdr *hdr) {
		iot_gwprotoreq* req;
		iot_gwproto_reqtype_t reqtype=iot_gwproto_reqtype_t(hdr->reqtype);
		iot_gwproto_cmd_t cmd=iot_gwproto_cmd_t(hdr->cmd);
		uint32_t data_size=repack_uint32(hdr->sz);
		uint32_t rqid=repack_uint32(hdr->rqid);

		if(!introduced && (cmd!=iot_gwproto_cmd_t::INTRODUCE || (reqtype & (IOTGW_IS_REPLY | IOTGW_IS_ASYNC))!=0)) {
			assert(false);
			//todo, return reply with iot_gwproto_errcode_t::NOINTRODUCTION
			return false;
		}

		if(reqtype & IOTGW_IS_REPLY) { //find request or discard
			assert(!(reqtype & IOTGW_IS_ASYNC)); //todo - abort connection
			if(waitingpackets_head) {
				iot_gwprotoreq* cur=waitingpackets_head;
				do {
					if(cur->rqid==rqid) {
						if(cmd==cur->cmd && hdr->cmd_ver==cur->cmd_ver) req=cur;
						//else cmd mismatch and packet must be discarded
						break;
					}
					if(cur==waitingpackets_tail) break;
					cur=cur->next;
				} while(1);
				if(req) {
					BILINKLISTWT_REMOVE(req, next, prev);
					if(!hdr->error || hdr->error>=uint8_t(iot_gwproto_errcode_t::CUSTOMBASE))
						req->p_req_got_reply(this, data_size, hdr->error);
					else {
						assert(false);
						//todo system error processing
					}
				}
			}
		} else {
			switch(cmd) {
				case iot_gwproto_cmd_t::INTRODUCE:
					req=iot_gwprotoreq_introduce::create_incoming(reqtype, cmd, hdr->cmd_ver, data_size, rqid, coniface->allocator);
					break;
				case iot_gwproto_cmd_t::ILLEGAL:
					req=NULL;
					break;
			}
		}
		if(!req) return false;
		assert(req->state == iot_gwprotoreq::STATE_READ_READY);
		current_inpacket=req;
		current_inpacket_offset=0;
		current_inpacket_size=data_size;
		current_inpacket_wantsfull=req->reqtype & IOTGW_IS_REPLY ?
				req->p_reply_inpacket_wantsfull(this, data_size, data_size>sizeof(readbuf)) :
				req->p_req_inpacket_wantsfull(this, data_size, data_size>sizeof(readbuf));
		req->state = iot_gwprotoreq::STATE_BEING_READ;

		return true;
	}

int iot_netproto_session_iotgw::on_introduce(iot_gwprotoreq_introduce* req, iot_hostid_t host_id, uint32_t core_vers) {
		if(introduced) { //for client side of connection. make session approved
			if(peer_host->host_id!=host_id) return iot_gwprotoreq_introduce::ERRCODE_INVALID_AUTH; //repeated introduce with different host
		} else {
			assert(peer_host==NULL);
			if(host_id!=0) peer_host=config->gwinst->peers_registry->find_peer(host_id);
			if(!peer_host) return iot_gwprotoreq_introduce::ERRCODE_UNKNOWN_HOST;
			introduced=true;
		}
		if(!peer_prev) peer_host->on_new_iotsession(this);
		return 0;
	}

