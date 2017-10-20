#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include "iot_netproto_iotgw.h"


iot_netprototype_metaclass_iotgw iot_netprototype_metaclass_iotgw::object;

static const char* reqtype_descr[uint8_t(iot_gwproto_reqtype_t::MAX)+1]={"reply", "request", "notification"};

int iot_netproto_config_iotgw::instantiate(iot_netconiface* coniface) { //must assign con->protostate
		assert(uv_thread_self()==coniface->worker_thread);
outlog_debug("protocol '%s' established", get_typename());
		iot_netproto_session_iotgw* ses=new iot_netproto_session_iotgw(coniface, this);
		coniface->protosession=ses;
		return 0;
	}

void iot_netproto_config_iotgw::deinstantiate(iot_netconiface* coniface) { //must free con->protosession and clean the pointer
	if(!coniface->protosession) return;
outlog_debug("protocol '%s' released", get_typename());
	delete coniface->protosession;
	coniface->protosession=NULL;
}


int iot_netproto_session_iotgw::start(void) {
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

bool iot_netproto_session_iotgw::start_new_inpacket(packet_hdr *hdr) {
		iot_gwprotoreq* req;
		iot_gwproto_reqtype_t reqtype=iot_gwproto_reqtype_t(hdr->reqtype);
		iot_gwproto_cmd_t cmd=iot_gwproto_cmd_t(hdr->cmd);
		uint32_t data_size=repack_uint32(hdr->sz);
		uint32_t rqid=repack_uint32(hdr->rqid);

		if(!introduced && cmd!=iot_gwproto_cmd_t::INTRODUCE && reqtype!=iot_gwproto_reqtype_t::REQUEST) {
			assert(false);
			//todo, return reply with iot_gwproto_errcode_t::NOINTRODUCTION
			return false;
		}

		if(reqtype==iot_gwproto_reqtype_t::REPLY) { //find request or discard
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
		current_inpacket_wantsfull=req->reqtype!=iot_gwproto_reqtype_t::REPLY ?
				req->p_req_inpacket_wantsfull(this, data_size, data_size>sizeof(readbuf)) :
				req->p_reply_inpacket_wantsfull(this, data_size, data_size>sizeof(readbuf));
		req->state = iot_gwprotoreq::STATE_BEING_READ;

		return true;
	}

int iot_netproto_session_iotgw::on_introduce(iot_gwprotoreq_introduce* req, iot_hostid_t host_id, uint32_t core_vers) {
		if(introduced) {
			if(peer_host->host_id==host_id) return 0;
			return iot_gwprotoreq_introduce::ERRCODE_INVALID_AUTH; //repeated introduce with different host
		}
		assert(peer_host==NULL);
		if(host_id!=0) peer_host=config_registry->host_find(host_id, false); //will increase refcount!!!
		if(!peer_host) return iot_gwprotoreq_introduce::ERRCODE_UNKNOWN_HOST;
		introduced=true;
		return 0;
	}
