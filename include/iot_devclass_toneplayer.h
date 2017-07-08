#ifndef IOT_DEVCLASS_TONEPLAYER_H
#define IOT_DEVCLASS_TONEPLAYER_H
//Contains interface to communicate using IOT_DEVCLASSID_TONEPLAYER class

#include<stdint.h>
//#include<time.h>
#include<assert.h>
#include<ecb.h>


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


struct iot_devifacetype_toneplayer : public iot_devifacetype_iface {
	iot_devifacetype_toneplayer(void) : iot_devifacetype_iface(IOT_DEVIFACETYPEID_TONEPLAYER, "Toneplayer") {
	}
	static void init_classdata(iot_devifacetype* devclass) {
		devclass->classid=IOT_DEVIFACETYPEID_TONEPLAYER;
		*((uint32_t*)devclass->data)=0;
	}

private:
	virtual bool check_data(const char* cls_data) const override { //actual check that data is good by format
		return true;
	}
	virtual bool check_istmpl(const char* cls_data) const override { //actual check that data corresponds to template (so not all data components are specified)
		return false;
	}
	virtual size_t print_data(const char* cls_data, char* buf, size_t bufsize) const override { //actual class data printing function. it must return number of written bytes (without NUL)
		int len=snprintf(buf, bufsize, "%s",name);
		return len>=int(bufsize) ? bufsize-1 : len;
	}
	virtual uint32_t get_d2c_maxmsgsize(const char* cls_data) const override;
	virtual uint32_t get_c2d_maxmsgsize(const char* cls_data) const override;
	virtual bool compare(const char* cls_data, const char* tmpl_data) const override { //actual comparison function
		return true;
	}
};

class iot_devifaceclass__toneplayer_BASE {
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


//	const msg* parse_event(const void *data, uint32_t data_size) {
//		uint32_t statesize=(attr->max_keycode / 32)+1;
//		if(data_size != sizeof(msg)+statesize*sizeof(uint32_t)) return NULL;
//		return static_cast<const msg*>(data);
//	}
	static uint32_t get_maxmsgsize(void) {
		return sizeof(req_t)+sizeof(req_set_song);
	}
protected:
	const iot_devifacetype_toneplayer *iface;

	iot_devifaceclass__toneplayer_BASE(const iot_devifacetype *devclass) {
		const iot_devifacetype_iface* iface_=devclass->find_iface();
		if(iface_ && iface_->classid==IOT_DEVIFACETYPEID_TONEPLAYER) iface=static_cast<const iot_devifacetype_toneplayer *>(iface_);
	}
};


class iot_devifaceclass__toneplayer_DRV : public iot_devifaceclass__DRVBASE, public iot_devifaceclass__toneplayer_BASE {

public:
	iot_devifaceclass__toneplayer_DRV(const iot_devifacetype *devclass) : iot_devifaceclass__DRVBASE(devclass),
																				iot_devifaceclass__toneplayer_BASE(devclass) {
	}

	//on success sets req to request type and returns non-NULL pointer which can be cast to appropriate struct type (if request has connected struct)
	const void* parse_req(const void *data, uint32_t data_size, req_t& req) {
		if(data_size<sizeof(req_t)) return NULL;
		req=*((req_t*)data);
		data_size-=sizeof(req_t);
		data=(const void *)(uintptr_t(data)+sizeof(req_t));
		switch(req) {
			case REQ_SET_SONG: {
				if(data_size<sizeof(req_set_song)) break;
				req_set_song *song=(req_set_song*)data;
				if(data_size==sizeof(req_set_song)+song->num_tones*sizeof(iot_toneplayer_tone_t)) return data;
				break;
			}
			case REQ_UNSET_SONG:
				if(data_size!=sizeof(req_unset_song)) break;
				return data;
			case REQ_PLAY:
				if(data_size!=sizeof(req_play)) break;
			case REQ_STOP:
			case REQ_GET_STATUS:
				return data;
		}
		return NULL;
	}

	//outgoing events (from driver to client)
	int send_status(const iot_conn_drvview *conn, const iot_toneplayer_status_t* status) {
		if(!iface) return IOT_ERROR_NOT_INITED;
		uint8_t buf[sizeof(event_t)+sizeof(iot_toneplayer_status_t)];
		*((event_t*)buf)=EVENT_STATUS;
		memcpy(buf+sizeof(event_t), status, sizeof(iot_toneplayer_status_t));
		return send_client_msg(conn, buf, sizeof(buf));
	}
};

class iot_devifaceclass__toneplayer_CL : public iot_devifaceclass__CLBASE, public iot_devifaceclass__toneplayer_BASE {

public:
	iot_devifaceclass__toneplayer_CL(const iot_devifacetype *devclass) : iot_devifaceclass__CLBASE(devclass),
																				iot_devifaceclass__toneplayer_BASE(devclass) {
	}

	int set_song(const iot_conn_clientview *conn, uint32_t &continue_offset, uint8_t song_index, const char* title, uint16_t num_tones, iot_toneplayer_tone_t *tones) {
		if(!iface) return IOT_ERROR_NOT_INITED;
		if(!num_tones) return IOT_ERROR_INVALID_ARGS;
		uint8_t buf[sizeof(req_t)+sizeof(req_set_song)+num_tones*sizeof(iot_toneplayer_tone_t)];

		if(continue_offset!=0 && continue_offset>sizeof(buf)) return IOT_ERROR_INVALID_ARGS;
		*((req_t*)buf)=REQ_SET_SONG;
		req_set_song* song=(req_set_song*)(buf+sizeof(req_t));
		song->index=song_index;
		snprintf(song->title, sizeof(song->title), "%s", title);
		song->num_tones=num_tones;
		memcpy(song->tones, tones, num_tones*sizeof(iot_toneplayer_tone_t));

		int32_t rval;
		if(!continue_offset) {
			rval=start_driver_req(conn, buf, sizeof(buf));
			if(rval==int32_t(sizeof(buf))) return 0;
			if(rval<0) return rval;
			assert(rval>0);
			continue_offset=uint32_t(rval);
			return IOT_ERROR_TRY_AGAIN;
		}
		rval=continue_driver_req(conn, buf+continue_offset, sizeof(buf)-continue_offset);
		if(rval==int32_t(sizeof(buf)-continue_offset)) return 0;
		if(rval<0) return rval;
		assert(rval>0);
		continue_offset+=uint32_t(rval);
		return IOT_ERROR_TRY_AGAIN;
	}

	int unset_song(const iot_conn_clientview *conn, uint8_t song_index) {
		if(!iface) return IOT_ERROR_NOT_INITED;
		uint8_t buf[sizeof(req_t)+sizeof(req_unset_song)];
		*((req_t*)buf)=REQ_UNSET_SONG;
		req_unset_song* song=(req_unset_song*)(buf+sizeof(req_t));
		song->index=song_index;

		return send_driver_msg(conn, buf, sizeof(buf));
	}

	int play(const iot_conn_clientview *conn, uint8_t song_index, iot_toneplayer_playmode_t mode, uint16_t tone_index, uint32_t stop_after=0) {
		if(!iface) return IOT_ERROR_NOT_INITED;
		uint8_t buf[sizeof(req_t)+sizeof(req_play)];
		*((req_t*)buf)=REQ_PLAY;
		req_play* data=(req_play*)(buf+sizeof(req_t));
		data->song_index=song_index;
		data->mode=mode;
		data->tone_index=tone_index;
		data->stop_after=stop_after;

		return send_driver_msg(conn, buf, sizeof(buf));
	}

	int stop(const iot_conn_clientview *conn) {
		if(!iface) return IOT_ERROR_NOT_INITED;
		req_t req=REQ_STOP;

		return send_driver_msg(conn, &req, sizeof(req));
	}

	int get_status(const iot_conn_clientview *conn) {
		if(!iface) return IOT_ERROR_NOT_INITED;
		req_t req=REQ_GET_STATUS;

		return send_driver_msg(conn, &req, sizeof(req));
	}

};


#endif //IOT_DEVCLASS_TONEPLAYER_H
