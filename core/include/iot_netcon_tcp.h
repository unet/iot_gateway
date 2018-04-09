#ifndef IOT_NETCON_TCP_H
#define IOT_NETCON_TCP_H

#include<stdint.h>
#include<unistd.h>
#include<assert.h>
//#include<time.h>
//#include <atomic>
#include "uv.h"

//#include<time.h>

#include "iot_core.h"
#include "iot_netcon.h"
#include "iot_threadregistry.h"

class iot_netcon_tcp;

//metaclass for TCP network connection type
class iot_netcontype_metaclass_tcp: public iot_netcontype_metaclass {
	iot_netcontype_metaclass_tcp(void) : iot_netcontype_metaclass("tcp", NETCONTYPE_TCP, true, true, true) {
	}

public:
	static iot_netcontype_metaclass_tcp object;

	virtual void destroy_netcon(iot_netcon* obj) const { //must destroy netcon object in correct way (metaclass knows how its netcon objects are created)
		obj->~iot_netcon();
		iot_release_memblock(obj);
	}
	static iot_netcon_tcp* allocate_netcon(iot_netproto_config* protoconfig);

private:
	virtual int p_from_json(json_object* json, iot_netproto_config* config, iot_netcon*& obj, bool is_server, iot_netconregistryiface* registry, uint32_t metric) const override;
};


//represents TCP network connection type
class iot_netcon_tcp : public iot_netcon {
	union {
		struct {
			char *bindhost;
			char *bindiface;
			uint16_t bindport;
			sockaddr_storage cur_sockaddr; //keeps actual sockaddr this connection is bound to. one master can create one or several slaves with same bindhost/bindiface but different cur_sockaddr
		} server;
		struct {
			char *dsthost;
			uint16_t dstport;
			struct addrinfo* cur_addr; //is cycled through items in addr
		} client;
	};
	union {
		struct {
			uv_getaddrinfo_t addr_req;
		} resolving;
		struct {
			int num_sockets;
			int num_ready;
			uv_interface_address_t* interfaces;
			int num_interfaces;
		} preplisten;
		struct {
			uv_connect_t conn_req;
		} connecting;
	} phasedata;
	uint64_t retry_delay=0;
	struct addrinfo* addr=NULL; //result of resolving, for both client and server (unconnected) mode
	char* readbuf=NULL;
	size_t readbufsize=0;

	uv_timer_t retry_phase_timer={};
	uv_tcp_t h_tcp={}; //connected handle, for both client and server (both connected and unconnected) mode
	uv_write_t h_writereq={};

	uint32_t resolve_errors=0,
		connect_errors=0,
		listen_errors=0;
	uv_os_fd_t accepted_fd=INVALID_SOCKET;

	bool is_server; //flag if connection was inited as server (i.e. listening), otherwise client (connecting)
	bool h_writereq_valid=false;
	bool read_enabled=false;
	bool peer_closed_restart=false; //becomes true if established connection is closed by peer (meaningful for restarts of client connections)

	enum : uint8_t {
		PHASE_UNINITED,		//server or client
		PHASE_INITIAL,		//server or client
		PHASE_RESOLVING,	//server or client
		PHASE_PREPLISTEN,	//server
		PHASE_PREPLISTEN2,	//server
		PHASE_LISTENING,	//server (final phase for accepting connection)
		PHASE_CONNECTING,	//client
		PHASE_CONNECTED,	//common
		PHASE_RUNNING		//common
	} phase=PHASE_UNINITED; //phase is used inside worker thread
//	bool retry_phase_timer_active=false;
//	bool h_tcp_valid=false;
//	bool h_tcp_closing=false;
	h_state_t h_tcp_state=HS_UNINIT;
	h_state_t h_phase_timer_state=HS_UNINIT;
	h_state_t session_state=HS_UNINIT;
	h_state_t h_addr_req_state=HS_UNINIT; //resolving

//	uv_shutdown_t h_shutdownreq={};
//	bool h_shutdownreq_valid=false;

	//for creating slaves
//	iot_netcon_tcp(const iot_netcon_tcp *master): iot_netcon(master) {
//	}

	static int duplicate_server(const iot_netcon_tcp* srv, sockaddr* saddr, socklen_t addrlen) { //duplicates server connection but with specific sockaddr. used when, e.g. one host resolves to several IPs
		if(!srv->is_server || !saddr || (saddr->sa_family!=AF_INET && saddr->sa_family!=AF_INET6)) return IOT_ERROR_INVALID_ARGS;
		iot_netcon_tcp* rep=iot_netcontype_metaclass_tcp::allocate_netcon((iot_netproto_config*)srv->protoconfig);
		if(!rep) return IOT_ERROR_NO_MEMORY;

		int err=rep->init_server(srv->server.bindhost, srv->server.bindport, srv->server.bindiface, srv->registry, true);
		if(err) {
			rep->meta->destroy_netcon(rep);
			return err;
		}
		memcpy(&rep->server.cur_sockaddr, saddr, addrlen);
		rep->phase=PHASE_LISTENING;

		err=rep->start_uv(srv->thread, false, true); //listening socket creates too small load, so use the same thread as master
		if(err && err!=IOT_ERROR_NOT_READY) {
			if(!rep->unmark_initing(true)) {
				assert(false);
			}
		}
		return err;
	}
	static int accept_server(const iot_netcon_tcp* srv) { //creates server instance in connected state (accepts incoming connection)
		if(!srv || !srv->is_server || srv->phase!=PHASE_LISTENING) return IOT_ERROR_INVALID_ARGS;

		iot_netcon_tcp* rep=iot_netcontype_metaclass_tcp::allocate_netcon((iot_netproto_config*)srv->protoconfig);
		if(!rep) return IOT_ERROR_NO_MEMORY;

		int err=rep->init_server(NULL, 0, NULL, srv->registry, true);
		if(err) {
			rep->meta->destroy_netcon(rep);
			return err;
		}
		err=uv_tcp_init(srv->loop, &rep->h_tcp);
		assert(err==0);
		rep->h_tcp.data=rep;
		rep->h_tcp_state=HS_INIT;

		err=uv_accept((uv_stream_t*)&srv->h_tcp, (uv_stream_t*)&rep->h_tcp);
		if(err) { //must not happen
			assert(false);
			err=IOT_ERROR_TRY_AGAIN;
			goto onexit;
		}

		rep->phase=PHASE_CONNECTED;
		if(srv->thread->is_overloaded(rep->get_cpu_loading())) {
			uv_os_fd_t tmpfd=INVALID_SOCKET;
			err=uv_fileno((uv_handle_t*)&rep->h_tcp, &tmpfd);
			if(err!=0) { //must not happen
				assert(false);
				err=IOT_ERROR_TRY_AGAIN;
				goto onexit;
			}
#ifdef _WIN32
			WSAPROTOCOL_INFO info;
			err=WSADuplicateSocketW(tmpfd, GetCurrentProcessId, &info);
			if(err!=0) { //must not happen
				assert(false);
				err=IOT_ERROR_TRY_AGAIN;
				goto onexit;
			}
			rep->accepted_fd=WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0, WSA_FLAG_OVERLAPPED);
#else
			rep->accepted_fd=dup(tmpfd);
#endif
			if(rep->accepted_fd==INVALID_SOCKET) {err=IOT_ERROR_TRY_AGAIN;goto onexit;}
			rep->h_tcp_state=HS_CLOSING;
			uv_close((uv_handle_t*)&rep->h_tcp, [](uv_handle_t* handle) -> void {
				iot_netcon_tcp* obj=(iot_netcon_tcp*)(handle->data);
				assert(obj->h_tcp_state==HS_CLOSING);
				obj->h_tcp_state=HS_UNINIT;
				obj->start_uv(NULL, false, true);
			});
			err=0;
		} else {
			rep->h_tcp_state=HS_ACTIVE;

			err=rep->start_uv(srv->thread, false, true); //listening socket creates too small load, so use the same thread as master
		}
onexit:
		if(err && err!=IOT_ERROR_NOT_READY) {
			if(!rep->unmark_initing(true)) {
				assert(false);
			}
		}
		return err;
	}
public:
	iot_netcon_tcp(iot_netproto_config* protoconfig)
			: iot_netcon(&iot_netcontype_metaclass_tcp::object, protoconfig) {
	}
	~iot_netcon_tcp(void) {
		assert(h_tcp_state==HS_UNINIT && h_phase_timer_state==HS_UNINIT && session_state==HS_UNINIT && !h_writereq_valid && h_addr_req_state==HS_UNINIT);
		if(is_server) {
			if(server.bindhost) {free(server.bindhost); server.bindhost=NULL;}
			if(server.bindiface) {free(server.bindiface); server.bindiface=NULL;}
		} else {
			if(client.dsthost) {free(client.dsthost); client.dsthost=NULL;}
		}
		if(addr) {
			uv_freeaddrinfo(addr); addr=NULL;
		}
		if(accepted_fd!=INVALID_SOCKET) {
#ifdef _WIN32
			closesocket(accepted_fd);
#else
			close(accepted_fd);
#endif
			accepted_fd=INVALID_SOCKET;
		}
	}
	int init_server(const char *host_, uint16_t port_, const char *iface_, iot_netconregistryiface* registryiface, bool noinit=false) { //true noinit to leave object in INITING state on success
//		assert(uv_thread_self()==control_thread_item->thread || uv_thread_self()==thread->thread);
		if(!registryiface) return IOT_ERROR_INVALID_ARGS; //required for server mode

		if(!trymark_initing()) {
			//must not fail if init is done in one thread just after construction
			assert(false);
			return IOT_ERROR_CRITICAL_BUG;
		}
		//now state is INITING and only current thread will do it
		assert(phase==PHASE_UNINITED);

		is_passive=true;
		is_server=true;
		server={};
		server.bindport=port_;
		int err=IOT_ERROR_NO_MEMORY;
		size_t ifacelen;

		size_t hostlen;
		hostlen=host_ ? strlen(host_) : 0;

		if(hostlen>1 || (hostlen==1 && host_[0]!='*')) { //host provided
			server.bindhost=(char*)malloc(hostlen+1);
			if(!server.bindhost) goto onerror;
			memcpy(server.bindhost, host_, hostlen+1);
		}
		ifacelen=iface_ ? strlen(iface_) : 0;

		if(ifacelen>1 || (ifacelen==1 && iface_[0]!='*')) { //iface provided
			server.bindiface=(char*)malloc(ifacelen+1);
			if(!server.bindiface) goto onerror;
			memcpy(server.bindiface, iface_, ifacelen+1);
		}

		phase=PHASE_INITIAL;
		err=assign_registryiface(registryiface);
		if(err) goto onerror;

		if(!noinit) mark_inited();
		return 0;
onerror:
		if(server.bindhost) {free(server.bindhost); server.bindhost=NULL;}
		if(server.bindiface) {free(server.bindiface); server.bindiface=NULL;}
		unmark_initing();
		return err;
	}

	int init_client(uint32_t metric_, const char *host_, uint16_t port_, iot_netconregistryiface* registryiface) {
//		assert(state==STATE_UNINITED && phase==PHASE_UNINITED);
//		assert(uv_thread_self()==control_thread_item->thread);
		size_t hostlen;
		if(host_) {
			hostlen=strlen(host_);
		} else {
			hostlen=0;
		}
		if(!hostlen || (hostlen==1 && host_[0]=='*')) { //no host
			outlog_error("'host' field is missing in %s connection spec", get_typename());
			return IOT_ERROR_BAD_DATA;
		}
		if(!port_) { //no port
			outlog_error("'port' field is missing or invalid in %s connection spec", get_typename());
			return IOT_ERROR_BAD_DATA;
		}

		if(!trymark_initing()) {
			//must not fail if init is done in one thread
			assert(false);
			return IOT_ERROR_CRITICAL_BUG;
		}
		//now state is INITED and only current thread will do it
		assert(phase==PHASE_UNINITED);

		is_passive=false;
		is_server=false;
		client={};
		client.dstport=port_;
		int err;

		metric=metric_;

		client.dsthost=(char*)malloc(hostlen+1);
		if(!client.dsthost) {
			err=IOT_ERROR_NO_MEMORY;
			goto onerror;
		}
		memcpy(client.dsthost, host_, hostlen+1);

		if(registryiface) {
			err=assign_registryiface(registryiface);
			if(err) goto onerror;
		}

		phase=PHASE_INITIAL;
		mark_inited();

		return 0;
onerror:
		if(client.dsthost) {free(client.dsthost); client.dsthost=NULL;}
		unmark_initing();
		return err;
	}

private:
	virtual uint8_t p_get_cpu_loading(void) override { //must return pure cpu loading of netcon layer implementation
		return 3;
	}
	virtual int do_stop(void) override {
		assert(!is_started() || uv_thread_self()==thread->thread);

		int num_closing=0;

		if(session_state>HS_UNINIT) {
			assert(protosession!=NULL);
			assert(phase==PHASE_RUNNING);
			if(session_state>=HS_INIT) {
				session_state=HS_CLOSING;
				num_closing++;
				if(!protosession->stop(true)) { //session won't call on_sesion_closed()
					num_closing--;
					session_state=HS_UNINIT;
				}
			} else num_closing++;
		}

		if(h_tcp_state>=HS_INIT) {
			h_tcp_state=HS_CLOSING;
			num_closing++;
			uv_close((uv_handle_t*)&h_tcp, [](uv_handle_t* handle) -> void {
				iot_netcon_tcp* obj=(iot_netcon_tcp*)(handle->data);
				assert(obj->h_tcp_state==HS_CLOSING);
				obj->h_tcp_state=HS_UNINIT;
				obj->do_stop();
			});
		} else if(h_tcp_state==HS_CLOSING) num_closing++;

		if(h_phase_timer_state>=HS_INIT) {
			h_phase_timer_state=HS_CLOSING;
			num_closing++;
			uv_close((uv_handle_t*)&retry_phase_timer, [](uv_handle_t* handle) -> void {
				iot_netcon_tcp* obj=(iot_netcon_tcp*)(handle->data);
				assert(obj->h_phase_timer_state==HS_CLOSING);
				obj->h_phase_timer_state=HS_UNINIT;
				obj->do_stop();
			});
		} else if(h_phase_timer_state==HS_CLOSING) num_closing++;

		int err;

		if(h_addr_req_state>HS_UNINIT) {
			assert(phase==PHASE_RESOLVING);
			if(h_addr_req_state>=HS_INIT) {
				h_addr_req_state=HS_CLOSING;
				num_closing++;
				err=uv_cancel((uv_req_t*)&phasedata.resolving.addr_req);
				if(err) {
					assert(false);
					h_addr_req_state=HS_UNINIT;
					num_closing--;
				}
			} else num_closing++;
		}

		if(num_closing>0) return IOT_ERROR_NOT_READY;

/*		switch(phase) {
			case PHASE_INITIAL:		//server or client
			case PHASE_PREPLISTEN:	//server
			case PHASE_PREPLISTEN2:	//server
			case PHASE_LISTENING:	//server (final phase for accepting connection)
			case PHASE_CONNECTED:	//common
			case PHASE_CONNECTING:	//client
				break;
			case PHASE_RESOLVING:	//server or client
				if(!phasedata.resolving.active) break;
				err=uv_cancel((uv_req_t*)&phasedata.resolving.addr_req);
				if(err) {
					assert(false);
					break;
				}
				//wait for cancel reported to callback
				return;
			case PHASE_RUNNING:		//common
				if(protosession && protosession->stop(false)) return; //works only if do_stop called again and connection still waits for on_session_closed() call. then stop session ungracefully
				break;
			default:
				outlog_error("%s() called for illegal phase %d", __func__, int(phase));
				break;
		}
*/
		phase=PHASE_INITIAL;
		return on_stopped();
	}
	virtual void do_start_uv(void) override {
		assert(uv_thread_self()==thread->thread);

		assert(h_phase_timer_state==HS_UNINIT);
		int err=uv_timer_init(loop, &retry_phase_timer);
		assert(err==0);
		retry_phase_timer.data=this;
		h_phase_timer_state=HS_INIT;

		if(h_tcp_state==HS_UNINIT) {
			err=uv_tcp_init(loop, &h_tcp);
			assert(err==0);
			h_tcp.data=this;
			h_tcp_state=HS_INIT;
		} else {
			assert(h_tcp.loop==loop);
			assert(h_tcp_state==HS_ACTIVE);
		}

		resolve_errors=0;
		if(is_server) {
			listen_errors=0;
			if(phase==PHASE_CONNECTED) {
				if(accepted_fd!=INVALID_SOCKET) { //this is duplicated raw socket
					err=uv_tcp_open(&h_tcp, accepted_fd);
					assert(err==0);
					accepted_fd=INVALID_SOCKET;
					h_tcp_state=HS_ACTIVE;
				}
				process_common_phase();
			}
			else if(phase==PHASE_LISTENING || phase==PHASE_INITIAL) process_server_phase();
			else {
				assert(false);
				do_stop();
				return;
			}
		} else {
			assert(phase==PHASE_INITIAL);
			if(peer_closed_restart) {
				connect_errors=1;
				peer_closed_restart=false;
			} else {
				connect_errors=0;
			}
			process_client_phase();
		}
	}
	void retry_phase(void) { //schedules call of process_[client|server] phase() after delay ms in retry_delay var
		assert(uv_thread_self()==thread->thread);
		h_phase_timer_state=HS_ACTIVE;
		uv_timer_start(&retry_phase_timer, [](uv_timer_t* h) -> void {
			iot_netcon_tcp* obj=(iot_netcon_tcp*)(h->data);

			obj->h_phase_timer_state=HS_INIT;
			if(!obj->is_server) obj->process_client_phase();
				else obj->process_server_phase();
		}, retry_delay, 0);
	}
	void resolve_host(const char *host, uint16_t port) {
		assert(phase==PHASE_RESOLVING && uv_thread_self()==thread->thread);
		assert(host!=NULL);
		char portbuf[8];
		snprintf(portbuf, sizeof(portbuf), "%u", unsigned(port));

		phasedata.resolving.addr_req.data=this;

		addrinfo hints={};
		hints.ai_family=AF_UNSPEC;
		hints.ai_socktype=SOCK_STREAM;
		hints.ai_flags=AI_NUMERICSERV | AI_ADDRCONFIG;

		h_addr_req_state=HS_ACTIVE;
		int err=uv_getaddrinfo(loop, &phasedata.resolving.addr_req, [](uv_getaddrinfo_t* req, int status, struct addrinfo* res) -> void {
			iot_netcon_tcp* obj=(iot_netcon_tcp*)req->data;
			obj->h_addr_req_state=HS_UNINIT;
			obj->on_resolve_status(status, res);
		}, host, portbuf, &hints);
		if(!err) return;
		h_addr_req_state=HS_UNINIT;

		assert(err!=UV_EINVAL);
		//must be UV_ENOMEM
		uint32_t delay=(2*60+(resolve_errors>10 ? 10 : resolve_errors)*3*60);
		outlog_notice("Error calling uv_getaddrinfo() for host '%s': %s, retrying in %u secs", host, uv_strerror(err), delay);
		resolve_errors++;
		retry_delay=delay*1000;
		retry_phase();
	}
	void on_resolve_status(int status, struct addrinfo* res) {
		if(!status) {
			resolve_errors=0;
			addr=res;
			if(!is_server) {
				phase=PHASE_CONNECTING;
				client.cur_addr=addr;
				if(connect_errors>0) { //initial delay after restart of client connection required
					retry_delay=30*1000;
					retry_phase();
					return;
				}
				process_client_phase();
			} else {
				phase=PHASE_PREPLISTEN;
				process_server_phase();
			}
			return;
		}
		if(status==UV_ECANCELED) {
			do_stop();
			return;
		}
		uint32_t delay=(2*60+(resolve_errors>10 ? 10 : resolve_errors)*3*60);
		outlog_notice("Error from uv_getaddrinfo() for host '%s': %s, retrying in %u secs", is_server ? server.bindhost : client.dsthost, uv_strerror(status), delay);
		resolve_errors++;
		retry_delay=delay*1000;
		retry_phase();
	}
	void process_common_phase(void) {
		assert(uv_thread_self()==thread->thread);
		int err;
//		uint32_t delay;
//again:
		switch(phase) {
			case PHASE_CONNECTED:
outlog_notice("in common phase %u", unsigned(phase));
				assert(h_tcp_state==HS_ACTIVE);
				assert(protosession==NULL);
				err=protoconfig->instantiate(this);
				if(err) {
					outlog_error("Cannot initialize protocol '%s': %s", protoconfig->get_typename(), kapi_strerror(err));
					do_stop();
					return;
				}
				assert(protosession!=NULL);

				phase=PHASE_RUNNING;
				session_state=HS_ACTIVE;
				err=protosession->start();
				if(err) {
					outlog_error("Cannot start session of protocol '%s': %s", protoconfig->get_typename(), kapi_strerror(err));
					do_stop();
					return;
				}
			case PHASE_RUNNING:
				return;
			default:
				outlog_error("%s() called for illegal phase %d, aborting", __func__, int(phase));
				do_stop();
				return;
		}
	}
	void process_server_phase(void) {
		assert(uv_thread_self()==thread->thread);
		int err;
		uint32_t delay;
again:
		switch(phase) {
			case PHASE_INITIAL:
				if(server.bindhost) {
					phase=PHASE_RESOLVING;
					goto again;
				}
				phase=PHASE_PREPLISTEN;
				goto again;
			case PHASE_RESOLVING:
outlog_notice("in phase %u", unsigned(phase));
				resolve_host(server.bindhost, server.bindport);
				return;
			case PHASE_PREPLISTEN: { //determine number of listen sockets, allocate memory for interfaces list
outlog_notice("in phase %u", unsigned(phase));
				int num_sockets=0;
				phasedata.preplisten={};

				if(!server.bindiface) {
					int limit=10;
					if(!server.bindhost && !addr) { //case 1: all IPs of all interfaces
						uv_getaddrinfo_t addr_req;
						char portbuf[8];
						addrinfo hints={};
						snprintf(portbuf, sizeof(portbuf), "%u", unsigned(server.bindport));
						hints.ai_family=AF_UNSPEC;
						hints.ai_socktype=SOCK_STREAM;
						hints.ai_flags=AI_NUMERICSERV | AI_ADDRCONFIG | AI_PASSIVE;

						err=uv_getaddrinfo(loop, &addr_req, NULL, NULL, portbuf, &hints);
						assert(err!=UV_EINVAL);
						if(err) {
							if(err==UV_EAI_AGAIN || err==UV_EAI_MEMORY || err==UV_ENOMEM) { //temporary errors
								delay=(2*60+(listen_errors>10 ? 10 : listen_errors)*3*60);
								outlog_notice("Error calling uv_getaddrinfo() for empty host and port %s: %s, retrying in %u secs", portbuf, uv_strerror(err), delay);
								listen_errors++;
								retry_delay=delay*1000;
								retry_phase();
								return;
							}
							outlog_notice("Error calling uv_getaddrinfo() for empty host and port %s: %s", portbuf, uv_strerror(err));
							//go on
						} else {
							addr=addr_req.addrinfo;
							limit=1; //only one inaddr_any can be listened on
						}
					}// else { //case 2: specific IPs obtained after resolving host
					//both case 1 and 2
					if(addr) {
						addrinfo* cur=addr;
						while(cur && num_sockets<limit) {
							if(cur->ai_addr->sa_family==AF_INET || cur->ai_addr->sa_family==AF_INET6) num_sockets++;
							cur=cur->ai_next;
						}
					}
				} else { //specific interface provided
					err=uv_interface_addresses(&phasedata.preplisten.interfaces, &phasedata.preplisten.num_interfaces);
					if(err) {
						if(err==UV_ENOBUFS || err==UV_ENOMEM || err==UV_EAGAIN || err==-EWOULDBLOCK || err==UV_EINTR || err==UV_ECONNRESET) { //temporary errors
							delay=(2*60+(listen_errors>10 ? 10 : listen_errors)*3*60);
							outlog_notice("Error calling uv_interface_addresses(): %s, retrying in %u secs", uv_strerror(err), delay);
							listen_errors++;
							retry_delay=delay*1000;
							retry_phase();
							return;
						}
						outlog_notice("Error calling uv_interface_addresses(): %s", uv_strerror(err));
						//go on
					}
					if(phasedata.preplisten.num_interfaces>0) { //use is_internal prop in phasedata.preplisten.interfaces[i] to mark address as selected (set bit 1)
						if(!addr) { //case 3: all ips of specific interface with AF_INET or AF_INET6
							for(int i=0;i<phasedata.preplisten.num_interfaces;i++) {
								if(strcmp(phasedata.preplisten.interfaces[i].name, server.bindiface)!=0) continue;
								if(phasedata.preplisten.interfaces[i].address.address4.sin_family!=AF_INET && phasedata.preplisten.interfaces[i].address.address6.sin6_family!=AF_INET6) continue;
								phasedata.preplisten.interfaces[i].is_internal|=2;
								num_sockets++;
							}
						} else { //case 4: specific IPs but only when on specific interface
							addrinfo* cur=addr;
							int i;
							while(cur) {
								if(cur->ai_addr->sa_family==AF_INET) {
									for(i=0;i<phasedata.preplisten.num_interfaces;i++) {
										if(strcmp(phasedata.preplisten.interfaces[i].name, server.bindiface)!=0) continue;
										if(phasedata.preplisten.interfaces[i].address.address4.sin_family!=AF_INET) continue;
										if(memcmp(&phasedata.preplisten.interfaces[i].address.address4.sin_addr, &((sockaddr_in*)(cur->ai_addr))->sin_addr, sizeof(struct in_addr))!=0) continue;
										phasedata.preplisten.interfaces[i].is_internal|=2;
										num_sockets++;
										break;
									}
								} else if(cur->ai_addr->sa_family==AF_INET6) {
									for(i=0;i<phasedata.preplisten.num_interfaces;i++) {
										if(strcmp(phasedata.preplisten.interfaces[i].name, server.bindiface)!=0) continue;
										if(phasedata.preplisten.interfaces[i].address.address6.sin6_family!=AF_INET6) continue;
										if(memcmp(&phasedata.preplisten.interfaces[i].address.address6.sin6_addr, &((sockaddr_in6*)(cur->ai_addr))->sin6_addr, sizeof(struct in6_addr))!=0) continue;
										phasedata.preplisten.interfaces[i].is_internal|=2;
										num_sockets++;
										break;
									}
								}
								cur=cur->ai_next;
							}
						}
					}
				}
				if(!num_sockets) {
					outlog_notice("Cannot find any address to listen for host '%s' and interface '%s', aborting", server.bindhost ? server.bindhost : "ANY", server.bindiface ? server.bindiface : "ANY");
					do_stop();
					return;
				}
				phasedata.preplisten.num_sockets=num_sockets;
				phase=PHASE_PREPLISTEN2;
			}
			case PHASE_PREPLISTEN2: { //run num_sockets-1 duplicates for each sockaddr
outlog_notice("in phase %u, numsocks=%d", unsigned(phase), phasedata.preplisten.num_sockets);
				assert(phasedata.preplisten.num_ready<phasedata.preplisten.num_sockets);
				int cur_idx=0;
				if(!server.bindiface) {
					assert(addr!=NULL);
					for(addrinfo* cur=addr;cur && phasedata.preplisten.num_ready<phasedata.preplisten.num_sockets;cur=cur->ai_next) {
						if(cur->ai_addr->sa_family!=AF_INET && cur->ai_addr->sa_family!=AF_INET6) continue;
						if(cur_idx<phasedata.preplisten.num_ready) {cur_idx++; continue;} //already done during previous try
						if(cur_idx==phasedata.preplisten.num_sockets-1) memcpy(&server.cur_sockaddr, cur->ai_addr, cur->ai_addrlen);
						else {
							err=duplicate_server(this, cur->ai_addr, cur->ai_addrlen);
							if(err) {
								delay=(2*60+(listen_errors>10 ? 10 : listen_errors)*3*60);
								outlog_notice("Error duplicating server connection on host '%s' and interface '%s': %s, retrying in %u secs", server.bindhost ? server.bindhost : "ANY", server.bindiface ? server.bindiface : "ANY", kapi_strerror(err), delay);
								listen_errors++;
								retry_delay=delay*1000;
								retry_phase();
								return;
							}
						}
						//success
						phasedata.preplisten.num_ready++;
						cur_idx++;
					}
				} else {
					for(int i=0;i<phasedata.preplisten.num_interfaces && phasedata.preplisten.num_ready<phasedata.preplisten.num_sockets;i++) {
						auto *iface=&phasedata.preplisten.interfaces[i];
						if(!(iface->is_internal & 2)) continue;
						if(cur_idx<phasedata.preplisten.num_ready) {cur_idx++; continue;} //already done during previous try
						sockaddr* saddr;
						int slen;
						if(iface->address.address4.sin_family==AF_INET) {
							saddr=(sockaddr*)&iface->address.address4;
							slen=sizeof(sockaddr_in);
							iface->address.address4.sin_port=htons(server.bindport);
						} else {
							saddr=(sockaddr*)&iface->address.address6;
							slen=sizeof(sockaddr_in6);
							iface->address.address6.sin6_port=htons(server.bindport);
						}

						if(cur_idx==phasedata.preplisten.num_sockets-1) {
							memcpy(&server.cur_sockaddr, saddr, slen);
						}
						else {
							err=duplicate_server(this, saddr, slen);
							if(err) {
								delay=(2*60+(listen_errors>10 ? 10 : listen_errors)*3*60);
								outlog_notice("Error duplicating server connection on host '%s' and interface '%s': %s, retrying in %u secs", server.bindhost ? server.bindhost : "ANY", server.bindiface ? server.bindiface : "ANY", kapi_strerror(err), delay);
								listen_errors++;
								retry_delay=delay*1000;
								retry_phase();
								return;
							}
						}
						//success
						phasedata.preplisten.num_ready++;
						cur_idx++;
					}
				}
				assert(cur_idx==phasedata.preplisten.num_sockets);
				assert(phasedata.preplisten.num_ready==phasedata.preplisten.num_sockets);

				//free data in phasedata.preplisten
				if(phasedata.preplisten.interfaces) {
					uv_free_interface_addresses(phasedata.preplisten.interfaces, phasedata.preplisten.num_sockets);
					phasedata.preplisten.interfaces=NULL;
				}
				phase=PHASE_LISTENING;
			}
			case PHASE_LISTENING: {
outlog_notice("in phase %u, obj=%p", unsigned(phase), this);
				assert(server.cur_sockaddr.ss_family!=0);
				if(h_tcp_state<HS_INIT) {
					if(h_tcp_state==HS_CLOSING) return;
					err=uv_tcp_init(loop, &h_tcp);
					assert(err==0);
					h_tcp.data=this;
					h_tcp_state=HS_INIT;
				}

				char ipstr[INET6_ADDRSTRLEN+1]="";
				if(server.cur_sockaddr.ss_family==AF_INET)
					err=uv_inet_ntop(server.cur_sockaddr.ss_family, &((sockaddr_in*)&server.cur_sockaddr)->sin_addr, ipstr, sizeof(ipstr));
				else
					err=uv_inet_ntop(server.cur_sockaddr.ss_family, &((sockaddr_in6*)&server.cur_sockaddr)->sin6_addr, ipstr, sizeof(ipstr));
				assert(err==0);

				err=uv_tcp_bind(&h_tcp, (sockaddr*)&server.cur_sockaddr, 0);
				if(err) {
					delay=(2*60+(listen_errors>10 ? 10 : listen_errors)*3*60);
					listen_errors++;
					outlog_notice("Error calling uv_tcp_bind() for host '%s' and interface '%s' [%s:%u]: %s, retrying in %u secs", server.bindhost ? server.bindhost : "ANY", server.bindiface ? server.bindiface : "ANY", ipstr, unsigned(server.bindport), uv_strerror(err), delay);
					close_handle_retry(delay*1000);
					return;
				}

				err=uv_listen((uv_stream_t*)&h_tcp, 16, [](uv_stream_t* server, int status) -> void {
					iot_netcon_tcp* obj=(iot_netcon_tcp*)server->data;
					obj->on_incomming_connection(status);
				});
				if(err) {
					delay=(2*60+(listen_errors>10 ? 10 : listen_errors)*3*60);
					listen_errors++;
					outlog_notice("Error calling uv_listen() for host '%s' and interface '%s' [%s:%u]: %s, retrying in %u secs", server.bindhost ? server.bindhost : "ANY", server.bindiface ? server.bindiface : "ANY", ipstr, unsigned(server.bindport), uv_strerror(err), delay);
					close_handle_retry(delay*1000);
					return;
				}
				h_tcp_state=HS_ACTIVE;
				outlog_notice("Now listening for incoming connections on host '%s' and interface '%s' [%s:%u]", server.bindhost ? server.bindhost : "ANY", server.bindiface ? server.bindiface : "ANY", ipstr, unsigned(server.bindport));
				listen_errors=0;
				return;
			}
			default:
				outlog_error("%s() called for illegal phase %d for host '%s', aborting", __func__, int(phase), client.dsthost);
				do_stop();
				return;
		}
	}
	void close_handle_retry(uint32_t delay_ms) { //close handle and then retry phase after delay
		assert(h_tcp_state>=HS_INIT);
		assert(h_phase_timer_state==HS_INIT);
		h_tcp_state=HS_CLOSING;
		retry_delay=delay_ms;
		uv_close((uv_handle_t*)&h_tcp, [](uv_handle_t* handle) -> void {
			iot_netcon_tcp* obj=(iot_netcon_tcp*)(handle->data);
			obj->h_tcp_state=HS_UNINIT;
			obj->retry_phase();
		});
	}
	void on_incomming_connection(int status) {
		int err;
		if(status) {
			char ipstr[INET6_ADDRSTRLEN+1]="";
			err=uv_inet_ntop(server.cur_sockaddr.ss_family, &server.cur_sockaddr, ipstr, sizeof(ipstr));
			assert(err==0);
			outlog_info("Error accepting new connection on host '%s' and interface '%s' [%s:%u]: %s", server.bindhost ? server.bindhost : "ANY", server.bindiface ? server.bindiface : "ANY", ipstr, unsigned(server.bindport), uv_strerror(status));
			return;
		}
		err=accept_server(this);
		assert(err!=IOT_ERROR_INVALID_ARGS);
	}
	void process_client_phase(void) {
		assert(uv_thread_self()==thread->thread);
		int err;
		uint32_t delay;
//again:
		switch(phase) {
			case PHASE_INITIAL:
				phase=PHASE_RESOLVING;
			case PHASE_RESOLVING:
outlog_notice("in client phase %u", unsigned(phase));
				resolve_host(client.dsthost, client.dstport);
				return;
			case PHASE_CONNECTING:
outlog_notice("in client phase %u", unsigned(phase));
				assert(client.cur_addr!=NULL);
				if(h_tcp_state<HS_INIT) {
					if(h_tcp_state==HS_CLOSING) return;
					err=uv_tcp_init(loop, &h_tcp);
					assert(err==0);
					h_tcp.data=this;
					h_tcp_state=HS_INIT;
				}
				phasedata.connecting.conn_req.data=this;

				err=uv_tcp_connect(&phasedata.connecting.conn_req, &h_tcp, client.cur_addr->ai_addr, [](uv_connect_t* req, int status) -> void {
					iot_netcon_tcp* obj=(iot_netcon_tcp*)req->data;
					obj->on_connect_status(status);
				});
				if(!err) return;
				assert(err!=UV_EINVAL);
				//must be UV_ENOMEM
				delay=30+(connect_errors>10 ? 10 : connect_errors)*3*60;
				outlog_notice("Error calling uv_tcp_connect() for host '%s': %s, retrying in %u secs", client.dsthost, uv_strerror(err), delay);
				connect_errors++;
				close_handle_retry(delay*1000);
				return;
			default:
				outlog_error("%s() called for illegal phase %d for host '%s', aborting", __func__, int(phase), client.dsthost);
				do_stop();
				return;
		}
	}
	void on_connect_status(int status) {
		int err;
		if(!status) {
			//check for self-connected socket
			sockaddr_storage addr;
			socklen_t slen2=sizeof(addr);
			err=uv_tcp_getsockname(&h_tcp, (sockaddr*)&addr, (int*)&slen2);
			if(err) {
				assert(false);
			} else {
				if(slen2==client.cur_addr->ai_addrlen && memcmp(client.cur_addr->ai_addr, &addr, slen2)==0) { //addrs are the same
					outlog_notice("Looped to myself while connecting to host '%s', retrying", client.dsthost);
					close_handle_retry(200);
					return;
				}
			}

			connect_errors=0;
			phase=PHASE_CONNECTED;
			h_tcp_state=HS_ACTIVE;
			process_common_phase();
			return;
		}
		if(status==UV_ECANCELED) return; //handle was closed and connection request automatically cancelled

		uv_getnameinfo_t ip_req;
		const char* ipstr="UNKNOWN";
		err=uv_getnameinfo(loop, &ip_req, NULL, client.cur_addr->ai_addr, NI_NUMERICHOST | NI_NUMERICSERV);
		if(err) outlog_notice("Error calling uv_getnameinfo() for IP of host '%s': %s", client.dsthost, uv_strerror(err));
			else ipstr=ip_req.host;
		uint32_t delay;
		if(client.cur_addr->ai_next) {
			outlog_info("Error connecting to host '%s' (by IP '%s'): %s, trying next IP", client.dsthost, ipstr, uv_strerror(status));
			client.cur_addr=client.cur_addr->ai_next;
			delay=200;
		} else {
			delay=30+(connect_errors>10 ? 10 : connect_errors)*3*60;
			outlog_notice("Error connecting to host '%s' (by IP '%s'): %s, retrying in %u secs", client.dsthost, ipstr, uv_strerror(status), delay);
			connect_errors++;
			client.cur_addr=addr;
			delay*=1000;
		}
		close_handle_retry(delay);
	}

	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const override {
		if(!bufsize) return buf;

		int len;
		if(is_server)
			len=snprintf(buf, bufsize, "{%s server:port=%u,host=%s,interface=%s}",meta->type_name, unsigned(server.bindport), server.bindhost ? server.bindhost : "ANY", server.bindiface ? server.bindiface : "ANY");
			else
			len=snprintf(buf, bufsize, "{%s client:metric=%u,port=%u,host=%s}",meta->type_name, unsigned(metric), unsigned(client.dstport), client.dsthost ? client.dsthost : "ANY");
		if(len>=int(bufsize)) len=int(bufsize)-1;
		if(doff) *doff+=len;
		return buf;
	}


	void stop_data_read(void) {
		if(phase!=PHASE_RUNNING) return;
		if(h_tcp_state==HS_ACTIVE) {
			int err=uv_read_stop((uv_stream_t*)&h_tcp);
			assert(err==0);
		}
		read_enabled=false;
		readbuf=NULL;
		readbufsize=0;
	}
//iot_netconiface methods:
	//enable reading into specified data buffer or reconfigure previous buffer. NULL databuf and zero datalen disable reading.
	//0 - reading successfully set. iot_netproto_session::on_read_data_status() will be called when data is available.
	//1 - reading successfully set and ready, iot_netproto_session::on_read_data_status() was already called before return from read_data()
	//IOT_ERROR_INVALID_ARGS - invalid arguments (databuf is NULL or datalen==0 but not simultaneously)
	virtual int read_data(void *databuf, size_t datalen) override {
		if(phase!=PHASE_RUNNING || h_tcp_state!=HS_ACTIVE) {
			assert(false);
			return IOT_ERROR_INVALID_ARGS;
		}
		int err;
		if(!databuf) {
			if(!datalen) { //disable reading
				if(read_enabled) stop_data_read();
				return 0;
			}
			return IOT_ERROR_INVALID_ARGS;
		}
		if(!datalen) return IOT_ERROR_INVALID_ARGS;

		readbuf=(char*)databuf;
		readbufsize=datalen;

		if(!read_enabled) {
			err=uv_read_start((uv_stream_t*)&h_tcp, [](uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) -> void {
				iot_netcon_tcp* obj=(iot_netcon_tcp*)handle->data;
				buf->base=obj->readbuf;
				buf->len=obj->readbufsize;
			}, [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) -> void {
				iot_netcon_tcp* obj=(iot_netcon_tcp*)stream->data;
				obj->on_read(nread, buf);
			});
			assert(err==0);
			read_enabled=true;
		}
		return 0;
	}
	void on_read(ssize_t nread, const uv_buf_t* buf) {
		assert(protosession!=NULL);
		if(nread==0) return;

		if(nread==UV_EOF || nread<0) { //connection closed by other side or some error
			h_tcp_state=HS_INIT;
			stop_data_read();
			peer_closed_restart=true;
//			detach_session();
//			session_state=HS_UNINIT;
			do_stop();
//			protosession->on_read_data_status(0, readbuf, readbufsize);
			return;
		}
		if(!protosession->on_read_data_status(nread, readbuf, readbufsize)) stop_data_read();
	}


	//check if new write request can be added. must return:
	//1 - write_data() can be called with request data
	//0 - request writing is in progress, so iot_netproto_session::on_write_data_status() must be waited (no need to call can_write_data() again)
////	//IOT_ERROR_NOT_READY - no request being written but netcon object is not ready to write request. iot_netproto_session::on_can_write_data() will be called when netcon is ready
	//IOT_ERROR_INVALID_STATE - netcon is not in RUNNING state
	virtual int can_write_data(void) override {
		if(phase!=PHASE_RUNNING || h_tcp_state!=HS_ACTIVE) {
			return IOT_ERROR_INVALID_STATE;
		}
		if(h_writereq_valid) return 0;
		return 1;
	}

	//try to add new write request. must return:
	//0 - request successfully added. iot_netproto_session::on_write_data_status() will be called later after completion
	//1 - request successfully finished. iot_netproto_session::on_write_data_status() was NOT CALLED
	//IOT_ERROR_TRY_AGAIN - another request writing is in progress, so iot_netproto_session::on_write_data_status() must be waited
	//IOT_ERROR_INVALID_STATE - (must not occur in debug mode) netcon is not in RUNNING state
////	//IOT_ERROR_NOT_READY - no request being written but netcon object is not ready to write request. iot_netproto_session::on_can_write_data() will be called when netcon is ready
	//IOT_ERROR_INVALID_ARGS - invalid arguments (databuf is NULL or datalen==0)
	virtual int write_data(void *databuf, size_t datalen) override {
		if(!databuf || !datalen) return IOT_ERROR_INVALID_ARGS;
		iovec vec;
		vec.iov_base=databuf;
		vec.iov_len=datalen;

		return iot_netcon_tcp::write_data(&vec, 1);
	}
	virtual int write_data(iovec *databufvec, int veclen) override {
		if(!databufvec || veclen<=0) return IOT_ERROR_INVALID_ARGS;
		if(phase!=PHASE_RUNNING || h_tcp_state!=HS_ACTIVE) {
			assert(false);
			return IOT_ERROR_INVALID_STATE;
//			return IOT_ERROR_NOT_READY;
		}
		if(h_writereq_valid) return IOT_ERROR_TRY_AGAIN;

		h_writereq_valid=true;

		uv_buf_t buf[veclen];
		for(int i=0;i<veclen;i++) {
			buf[i].base=(char*)databufvec[i].iov_base;
			buf[i].len=databufvec[i].iov_len;
		}
		h_writereq.data=this;
		int err=uv_write(&h_writereq, (uv_stream_t*)&h_tcp, buf, veclen, [](uv_write_t* req, int status)->void {
			iot_netcon_tcp* obj=(iot_netcon_tcp*)req->data;
			obj->on_write_status(status);
		});
		assert(err==0);
		return 0;
	}

	void on_write_status(int status) {
		h_writereq_valid=false;
		if(session_state!=HS_ACTIVE) return;
		assert(protosession!=NULL);
		if(!status) {
			protosession->on_write_data_status(0);
//			if(!h_writereq_valid) { //no additional write_data() called in on_write_data_status()
//				protosession->on_can_write_data();
//			}
			return;
		}
		do_stop();
		//todo
	}

	//stop reading side, wait for current write request to finish and then close connection
/*	virtual void graceful_close(void) override {
		if(h_shutdownreq_valid || !h_tcp_valid) return;
		if(read_enabled) stop_data_read();

		h_shutdownreq.data=this;
		h_shutdownreq_valid=true;
		int err=uv_shutdown(&h_shutdownreq, (uv_stream_t*)&h_tcp, [](uv_shutdown_t* req, int status) -> void {
			iot_netcon_tcp* obj=(iot_netcon_tcp*)req->data;
			obj->on_shutdown_status(status);
		});
		assert(err==0);
	}
	void on_shutdown_status(int status) {
		//todo
		do_stop();
	}*/
	virtual void on_session_closed(void) override {
		session_state=HS_UNINIT;
		do_stop();
	}

};

inline int iot_netcontype_metaclass_tcp::p_from_json(json_object* json, iot_netproto_config* protoconfig, iot_netcon*& obj, bool is_server, iot_netconregistryiface* registry, uint32_t metric) const {
		json_object* val=NULL;
		int err;
		//host
		const char *host=NULL;
		if(json_object_object_get_ex(json, "host", &val)) {
			if(!json_object_is_type(val, json_type_string)) {
				outlog_error("'host' field must be a string in %s connection spec", type_name);
				return IOT_ERROR_BAD_DATA;
			}
			host=json_object_get_string(val);
			//check for non-ascii chars (IDN name)
			int hostlen=json_object_get_string_len(val);
			int i;
			for(i=0;i<hostlen;i++) if((unsigned char)(host[i])>127) break;
			if(i<hostlen) { //has non-ascii
#ifdef CONFIG_LIBIDN
				char* idnhost=NULL;
				err=idna_to_ascii_8z(host, &idnhost, IDNA_ALLOW_UNASSIGNED);
				if(err) {
					outlog_error("Cannot parse 'host' value '%s' as IDN in %s connection spec:", idna_strerror(err), type_name);
					return IOT_ERROR_BAD_DATA;
				}
#else
				outlog_error("Non-ascii values not supported for 'host' field in %s connection spec, libidn is required", type_name);
				return IOT_ERROR_BAD_DATA;
#endif
			}
		}
		//port
		uint16_t port=0;
		if(json_object_object_get_ex(json, "port", &val)) {
			if(!json_object_is_type(val, json_type_string) && !json_object_is_type(val, json_type_int)) {
				outlog_error("'port' field must be a string or an integer in %s connection spec", type_name);
				return IOT_ERROR_BAD_DATA;
			}
			IOT_JSONPARSE_UINT(val, uint16_t, port);
			if(!port) {
				outlog_error("invalid port value '%s' for %s connection spec", json_object_get_string(val), type_name);
				return IOT_ERROR_BAD_DATA;
			}
		} else {
			port=IOT_PEERCON_TCPUDP_PORT;
		}
		//interface
		const char *iface=NULL;
		if(is_server && json_object_object_get_ex(json, "interface", &val)) {
			if(!json_object_is_type(val, json_type_string)) {
				outlog_error("'interface' field must be a string in %s connection spec", type_name);
				return IOT_ERROR_BAD_DATA;
			}
			iface=json_object_get_string(val);
		}

		iot_netcon_tcp* con=iot_netcontype_metaclass_tcp::allocate_netcon(protoconfig);
		if(!con) return IOT_ERROR_NO_MEMORY;
		if(is_server) err=con->init_server(host, port, iface, registry);
			else err=con->init_client(metric, host, port, registry);
		if(err) {
			con->meta->destroy_netcon(con);
			return err;
		}
		obj=con;
		return 0;
	}

inline iot_netcon_tcp* iot_netcontype_metaclass_tcp::allocate_netcon(iot_netproto_config* protoconfig) {
	assert(protoconfig!=NULL);
	iot_netcon_tcp *con=(iot_netcon_tcp *)iot_allocate_memblock(sizeof(iot_netcon_tcp), true);
	if(!con) return NULL;
	return new(con) iot_netcon_tcp(protoconfig);
}

#endif //IOT_NETCON_TCP_H
