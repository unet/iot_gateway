#ifndef IOT_DEVCLASS_TONEPLAYER_H
#define IOT_DEVCLASS_TONEPLAYER_H
//Contains interface to communicate using IOT_DEVCLASSID_TONEPLAYER class

#include<stdint.h>
//#include<time.h>
#include<assert.h>
#include "ecb.h"


struct iot_toneplayer_tone_t {
	uint16_t freq, //frequency in HZ, valid range on linux is (20 ; 32767)
		len; //duration in 1/32 of sec
};

enum class iot_toneplayer_playmode_t : uint8_t {
	PLAYONE, //current song will end and player stops
	REPEATONE, //current song will play endlessly
	REPEATALL, //all assigned songs will play endlessly
};

struct iot_toneplayer_status_t {
	uint32_t songs[8]; //bitmap of assigned songs. bit 0 is not used
	uint8_t num_songs;
	uint8_t current_song; //index of current song being played of stopped at (1-255 for valid song, 0 if no songs or song not selected yet)
	uint16_t current_tone; //current tone being played of stopped at (1-num_tones when current_song is valid, 0 otherwise)
	bool is_playing; //true if player is started
	iot_toneplayer_playmode_t mode; //current play mode
};


class iot_toneplayer_state {
	struct song_t {
		char* title;
		uint16_t num_tones;
		iot_toneplayer_tone_t* tones;
	};
	song_t* songs[255];
	iot_toneplayer_playmode_t playmode=iot_toneplayer_playmode_t::PLAYONE;
	uint8_t current_song=0;
	uint16_t current_tone=0; //index of tone being played
public:
	iot_toneplayer_state(void) {
		memset(songs, 0, sizeof(songs));
	}

	//on success returns index of song (either equal to argument index!=0 OR actual index), >0
	//on error returns error code <0:
	//	IOT_ERROR_NO_MEMORY - cannot allocate memory
	//	IOT_ERROR_INVALID_ARGS - num_tones cannot be zero
	int set_song(uint8_t index, const char* title, uint16_t num_tones, iot_toneplayer_tone_t* tones) {
		if(num_tones==0) return IOT_ERROR_INVALID_ARGS;
		size_t titlelen=strlen(title)+1;
		size_t sz=sizeof(song_t)+titlelen+num_tones*sizeof(iot_toneplayer_tone_t)+alignof(iot_toneplayer_tone_t);
		song_t* song=(song_t*)malloc(sz);
		if(!song) return IOT_ERROR_NO_MEMORY;

		song->title=((char*)song)+sizeof(song_t);
		song->tones=(iot_toneplayer_tone_t*)(uintptr_t(song->title+titlelen+alignof(iot_toneplayer_tone_t)) & ~(alignof(iot_toneplayer_tone_t)-1));
		memcpy(song->title, title, titlelen);
		song->num_tones=num_tones;
		memcpy(song->tones, tones, num_tones*sizeof(iot_toneplayer_tone_t));

		if(!index) { //find first free or overwrite first one (if all songs set)
			int i;
			for(i=0;i<255;i++) if(!songs[i]) break;
			index=i<255 ? uint8_t(i+1) : 1;
		}
		if(songs[index-1]) free(songs[index-1]);
		songs[index-1]=song;

		if(current_song==index) {
			current_tone=0;
		}

		return index;
	}
	void unset_song(uint8_t index) {
		if(index>0) {
			if(songs[index-1]) free(songs[index-1]);
			songs[index-1]=NULL;
			if(current_song==index) {
				current_tone=0;
			}
			return;
		}
		for(uint8_t i=0;i<255;i++) {
			if(songs[i]) {
				free(songs[i]);
				songs[i]=NULL;
			}
		}
		current_song=0;
		current_tone=0;
	}
	void set_playmode(iot_toneplayer_playmode_t mode) {
		playmode=mode;
	}
	void rewind(uint8_t song_index, uint16_t tone_index) {
		if(song_index>0) {
			current_song=song_index;
			current_tone=tone_index; //can be zero or outside num_tones of current song
		}
	}
	//returns NULL if playing must be stopped
	const iot_toneplayer_tone_t* get_nexttone(void) {
		if(!current_song || !songs[current_song-1]) {
			if(current_song && (playmode==iot_toneplayer_playmode_t::PLAYONE || playmode==iot_toneplayer_playmode_t::REPEATONE)) return NULL; //current song was removed in such modes, then stop
			//find next valid song
			int i;
			for(i=0;i<255;i++) if(songs[(i+current_song)%255]) break;
			if(i>=255) return NULL; //no songs
			current_song=(i+current_song)%255+1;
		}
		if(current_tone>=songs[current_song-1]->num_tones) { //current song ended
			if(playmode==iot_toneplayer_playmode_t::PLAYONE) return NULL; //stop
			current_tone=0;
			if(playmode==iot_toneplayer_playmode_t::REPEATALL) {
				current_song=current_song % 255+1;
				return get_nexttone();
			}
		}
		return &songs[current_song-1]->tones[current_tone++];
	}
	void get_status(iot_toneplayer_status_t *st) { //is_playing is set to false!!!
		memset(st, 0, sizeof(*st));
		for(int i=0;i<255;i++) if(songs[i]) {
			bitmap32_set_bit(st->songs, i+1);
			st->num_songs++;
		}
		st->current_song=current_song;
		st->current_tone=current_tone;
		st->mode=playmode;
	}
};


class iot_deviface_params_toneplayer : public iot_deviface_params {
	friend class iot_devifacetype_metaclass_toneplayer;
	friend class iot_deviface__toneplayer_BASE;
	//no actual params for this iface

	iot_deviface_params_toneplayer(void);
public:

	static const iot_deviface_params_toneplayer object; //statically created object to return from deserialization
	
	static const iot_deviface_params_toneplayer* cast(const iot_deviface_params* params);

	virtual bool is_tmpl(void) const override { //check if current objects represents template. otherwise it must be exact connection specification
		return false;
	}
	virtual size_t get_size(void) const override { //must return 0 if object is statically precreated and thus must not be copied by value, only by reference
		if(this==&object) return 0;
		return sizeof(*this);
	}
	virtual uint32_t get_d2c_maxmsgsize(void) const override;
	virtual uint32_t get_c2d_maxmsgsize(void) const override;
	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const override {
		if(!bufsize) return buf;

		int len=0;
		get_fullname(buf, bufsize, &len);
		if(doff) *doff+=len;
		return buf;
	}
private:
	virtual bool p_matches(const iot_deviface_params* opspec0) const override {
		const iot_deviface_params_toneplayer* opspec=cast(opspec0);
		if(!opspec) return false;
		return true;
	}
};

class iot_devifacetype_metaclass_toneplayer : public iot_devifacetype_metaclass {
	iot_devifacetype_metaclass_toneplayer(void) : iot_devifacetype_metaclass(0, "unet", "Toneplayer") {}

	PACKED(
		struct serialize_header_t {
			uint32_t format;
		}
	);

public:
	static iot_devifacetype_metaclass_toneplayer object; //the only instance of this class

private:
	virtual int p_serialized_size(const iot_deviface_params* obj0) const override {
		const iot_deviface_params_toneplayer* obj=iot_deviface_params_toneplayer::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		return sizeof(serialize_header_t);
	}
	virtual int p_serialize(const iot_deviface_params* obj0, char* buf, size_t bufsize) const override {
		const iot_deviface_params_toneplayer* obj=iot_deviface_params_toneplayer::cast(obj0);
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		if(bufsize<sizeof(serialize_header_t)) return IOT_ERROR_NO_BUFSPACE;

		serialize_header_t *h=(serialize_header_t*)buf;
		h->format=repack_uint32(uint32_t(1));
		return 0;
	}
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_deviface_params*& obj) const override {
		return 0;
	}
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_deviface_params*& obj) const override {
		return 0;
	}
};


inline iot_deviface_params_toneplayer::iot_deviface_params_toneplayer(void) : iot_deviface_params(&iot_devifacetype_metaclass_toneplayer::object) {
}
inline const iot_deviface_params_toneplayer* iot_deviface_params_toneplayer::cast(const iot_deviface_params* params) {
	if(!params) return NULL;
	return params->get_metaclass()==&iot_devifacetype_metaclass_toneplayer::object ? static_cast<const iot_deviface_params_toneplayer*>(params) : NULL;
}


class iot_deviface__toneplayer_BASE {
public:
	enum req_t : uint32_t { //commands (requests) which driver can execute. use uint32_t to have 4-byte alignment for request-specific structs
		REQ_SET_SONG, //request to assign song tones. if this song is being played, it will restart. uses req_set_song struct
		REQ_UNSET_SONG, //uses req_unset_song struct
		REQ_PLAY, //uses req_play struct
		REQ_STOP, //pauses player (current position is not lost)
		REQ_GET_STATUS //request to send EVENT_STATUS
	};

	enum event_t : uint32_t { //events (or replies) which driver can send to client. use uint32_t to have 4-byte alignment for request-specific structs
		EVENT_STATUS
	};


	struct req_set_song {
		uint8_t index; //index of song (1-255 for specific slot, 0 for next free slot)
		char title[129]; //song title (NUL-terminated)
		uint16_t num_tones; //[1; 65535], 0 is illegal
		iot_toneplayer_tone_t tones[];
	};

	struct req_unset_song {
		uint8_t index; //index of song (1-255 for specific, 0 to remove all)
	};

	struct req_play {
		uint8_t song_index; //index of song (1-255 for specific, 0 for latest)
		iot_toneplayer_playmode_t mode;
		uint16_t tone_index; //[1; num_tones] if song_index>0, 0 to continue (if song_index==0) or equivalent to 1
		uint32_t stop_after; //when !=0 specifies number of seconds for auto stop
	};

	struct reqmsg {
		req_t req_code;
		union {
			req_set_song set_song;
			req_unset_song unset_song;
			req_play play;
		} req_data[];
	};

	struct eventmsg {
		event_t event_code;
		union {
			iot_toneplayer_status_t status;
		} event_data[];
	};

//	const msg* parse_event(const void *data, uint32_t data_size) {
//		uint32_t statesize=(attr->max_keycode / 32)+1;
//		if(data_size != sizeof(msg)+statesize*sizeof(uint32_t)) return NULL;
//		return static_cast<const msg*>(data);
//	}
	constexpr static uint32_t get_maxmsgsize(void) {
		return sizeof(req_t)+sizeof(req_set_song);
	}
protected:

	iot_deviface__toneplayer_BASE(void) {}
	bool init(const iot_deviface_params *deviface) {
		const iot_deviface_params_toneplayer*params=iot_deviface_params_toneplayer::cast(deviface);
		if(!params) return false; //illegal interface type
		return true;
	}
};


class iot_deviface__toneplayer_DRV : public iot_deviface__DRVBASE, public iot_deviface__toneplayer_BASE {

public:
	iot_deviface__toneplayer_DRV(const iot_conn_drvview *conn=NULL) {
		init(conn);
	}
	bool init(const iot_conn_drvview *conn=NULL) {
		if(!conn) { //uninit request
			//do uninit
			iot_deviface__DRVBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		if(!iot_deviface__toneplayer_BASE::init(conn->deviface)) {
			iot_deviface__DRVBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		return iot_deviface__DRVBASE::init(conn);
	}
//	bool is_inited(void) inherited from iot_deviface__DRVBASE

	//on success sets req to request type and returns non-NULL pointer which can be cast to appropriate struct type (if request has connected struct)
	const void* parse_req(const void *data, uint32_t data_size, req_t& req) const {
		if(!is_inited()) return NULL;
		if(data_size<sizeof(reqmsg)) return NULL;
//		req=*((req_t*)data);
		const reqmsg* msg=(const reqmsg*)data;
		req=msg->req_code;
		data_size-=sizeof(reqmsg);
//		data=(const void *)(uintptr_t(data)+sizeof(req_t));
		switch(req) {
			case REQ_SET_SONG: {
				if(data_size<sizeof(req_set_song)) break;
//				req_set_song *song=(req_set_song*)data;
				const req_set_song *song=&msg->req_data[0].set_song;
				if(data_size==sizeof(req_set_song)+song->num_tones*sizeof(iot_toneplayer_tone_t)) return song;
				break;
			}
			case REQ_UNSET_SONG:
				if(data_size!=sizeof(req_unset_song)) break;
				return &msg->req_data[0].unset_song;
			case REQ_PLAY:
				if(data_size!=sizeof(req_play)) break;
				return &msg->req_data[0].play;
			case REQ_STOP:
			case REQ_GET_STATUS:
				return msg;
		}
		return NULL;
	}

	//outgoing events (from driver to client)
	int send_status(const iot_toneplayer_status_t* status) const {
		if(!is_inited()) return IOT_ERROR_NOT_INITED;
//		char buf[sizeof(event_t)+sizeof(iot_toneplayer_status_t)];
		alignas(eventmsg) char buf[sizeof(eventmsg)+sizeof(iot_toneplayer_status_t)];
		eventmsg* msg=(eventmsg*)buf;

//		*((event_t*)buf)=EVENT_STATUS;
		msg->event_code=EVENT_STATUS;
		iot_toneplayer_status_t* data=&msg->event_data[0].status;
		memcpy(data, status, sizeof(iot_toneplayer_status_t));
		return send_client_msg(buf, sizeof(buf));
	}
};

class iot_deviface__toneplayer_CL : public iot_deviface__CLBASE, public iot_deviface__toneplayer_BASE {

public:
	iot_deviface__toneplayer_CL(const iot_conn_clientview *conn=NULL) {
		init(conn);
	}
	bool init(const iot_conn_clientview *conn=NULL) {
		if(!conn) { //uninit request
			//do uninit
			iot_deviface__CLBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		if(!iot_deviface__toneplayer_BASE::init(conn->deviface)) {
			iot_deviface__CLBASE::init(NULL); //do uninit for DRVBASE
			return false;
		}
		return iot_deviface__CLBASE::init(conn);
	}
//	bool is_inited(void) inherited from iot_deviface__CLBASE

	int set_song(uint32_t &continue_offset, uint8_t song_index, const char* title, uint16_t num_tones, iot_toneplayer_tone_t *tones) const {
		if(!is_inited()) return IOT_ERROR_NOT_INITED;
		if(!num_tones) return IOT_ERROR_INVALID_ARGS;
//		uint8_t buf[sizeof(req_t)+sizeof(req_set_song)+num_tones*sizeof(iot_toneplayer_tone_t)];
		alignas(reqmsg) char buf[sizeof(reqmsg)+sizeof(req_set_song)+num_tones*sizeof(iot_toneplayer_tone_t)];
		reqmsg* msg=(reqmsg*)buf;

		if(continue_offset!=0 && continue_offset>sizeof(buf)) return IOT_ERROR_INVALID_ARGS;
//		*((req_t*)buf)=REQ_SET_SONG;
		msg->req_code=REQ_SET_SONG;
//		req_set_song* song=(req_set_song*)(buf+sizeof(req_t));
		req_set_song* song=&msg->req_data[0].set_song;
		song->index=song_index;
		snprintf(song->title, sizeof(song->title), "%s", title);
		song->num_tones=num_tones;
		memcpy(song->tones, tones, num_tones*sizeof(iot_toneplayer_tone_t));

		int32_t rval;
		if(!continue_offset) {
			rval=start_driver_req(buf, sizeof(buf));
			if(rval==int32_t(sizeof(buf))) return 0;
			if(rval<0) return rval;
			assert(rval>0);
			continue_offset=uint32_t(rval);
			return IOT_ERROR_TRY_AGAIN;
		}
		rval=continue_driver_req(buf+continue_offset, sizeof(buf)-continue_offset);
		if(rval==int32_t(sizeof(buf)-continue_offset)) return 0;
		if(rval<0) return rval;
		assert(rval>0);
		continue_offset+=uint32_t(rval);
		return IOT_ERROR_TRY_AGAIN;
	}

	int unset_song(uint8_t song_index) const {
		if(!is_inited()) return IOT_ERROR_NOT_INITED;
//		uint8_t buf[sizeof(req_t)+sizeof(req_unset_song)];
		alignas(reqmsg) char buf[sizeof(reqmsg)+sizeof(req_unset_song)];
		reqmsg* msg=(reqmsg*)buf;

//		*((req_t*)buf)=REQ_UNSET_SONG;
		msg->req_code=REQ_UNSET_SONG;
//		req_unset_song* song=(req_unset_song*)(buf+sizeof(req_t));
		req_unset_song* song=&msg->req_data[0].unset_song;
		song->index=song_index;

		return send_driver_msg(buf, sizeof(buf));
	}

	int play(uint8_t song_index, iot_toneplayer_playmode_t mode, uint16_t tone_index, uint32_t stop_after=0) const {
		if(!is_inited()) return IOT_ERROR_NOT_INITED;
//		uint8_t buf[sizeof(req_t)+sizeof(req_play)];
		alignas(reqmsg) char buf[sizeof(reqmsg)+sizeof(req_play)];
		reqmsg* msg=(reqmsg*)buf;
//		*((req_t*)buf)=REQ_PLAY;
		msg->req_code=REQ_PLAY;
//		req_play* data=(req_play*)(buf+sizeof(req_t));
		req_play* data=&msg->req_data[0].play;
		data->song_index=song_index;
		data->mode=mode;
		data->tone_index=tone_index;
		data->stop_after=stop_after;

		return send_driver_msg(buf, sizeof(buf));
	}

	int stop(void) const {
		if(!is_inited()) return IOT_ERROR_NOT_INITED;
		reqmsg msg;
		msg.req_code=REQ_STOP;

		return send_driver_msg(&msg, sizeof(msg));
	}

	int get_status(void) const {
		if(!is_inited()) return IOT_ERROR_NOT_INITED;
		reqmsg msg;
		msg.req_code=REQ_GET_STATUS;

		return send_driver_msg(&msg, sizeof(msg));
	}

};



#endif //IOT_DEVCLASS_TONEPLAYER_H
