#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>
#include<stdlib.h>
#include<unistd.h>
#include<assert.h>
#include<new>


#include <linux/input.h>

#include "uv.h"
#include "iot_utils.h"
#include "iot_error.h"


//#define IOT_VENDOR unet
//#define IOT_BUNDLE generic__toneplayer

#include "iot_module.h"

IOT_LIBVERSION_DEFINE;

#include "iot_devclass_keyboard.h"
#include "iot_devclass_activatable.h"
#include "modules/unet/types/di_toneplayer/iot_devclass_toneplayer.h"


/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////toneplayer:basic node module
/////////////////////////////////////////////////////////////////////////////////


struct basic_instance : public iot_node_base {
	uint32_t node_id;
	bool play=false;
	struct song_t {
		song_t* next;
		char title[128];
		uint16_t num_tones;
		iot_toneplayer_tone_t *tones;
	} *songs_head=NULL;

	struct {
		const iot_conn_clientview *conn;
	} device={}; //per device connection state

/////////////static fields/methods for module instances management
	static int init_instance(iot_node_base** instance, uv_thread_t thread, uint32_t node_id, json_object *json_cfg) {

		basic_instance *inst=new basic_instance(thread, node_id, json_cfg);
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_node_base* instance) {
		basic_instance *inst=static_cast<basic_instance*>(instance);
		delete inst;
		return 0;
	}
private:
	basic_instance(uv_thread_t thread, uint32_t node_id, json_object *json_cfg) : iot_node_base(thread), node_id(node_id)
	{
		int numsongs=0;
		if(json_cfg) {
			json_object *songs=NULL;
			if(json_object_object_get_ex(json_cfg, "songs", &songs) && json_object_is_type(songs,  json_type_object)) {
				json_object_object_foreach(songs, songtitle, songtones) {
					if(!json_object_is_type(songtones, json_type_array)) continue;
					int len=json_object_array_length(songtones);
					if(len>65535) len=65535;

					song_t* newsong=(song_t*)malloc(sizeof(song_t)+len*sizeof(iot_toneplayer_tone_t));
					if(!newsong) continue;
					snprintf(newsong->title, sizeof(newsong->title), "%s", songtitle);
					newsong->num_tones=0;
					newsong->tones=(iot_toneplayer_tone_t*)(uintptr_t(newsong)+sizeof(song_t));

					for(int i=0;i<len;i++) {
						json_object* tone=json_object_array_get_idx(songtones, i);
						if(!json_object_is_type(tone, json_type_array) || json_object_array_length(tone)!=2) continue;

						errno=0;
						int freq=json_object_get_int(json_object_array_get_idx(tone, 0));
						if(errno || freq<0) continue;
						freq=freq<21 ? 0 : freq > 32766 ? 32766 : freq;

						errno=0;
						int len=json_object_get_int(json_object_array_get_idx(tone, 1));
						if(errno || len<=0) continue;

						newsong->tones[newsong->num_tones++]={uint16_t(freq), uint16_t(len)};
					}
					if(!newsong->num_tones) {
						free(newsong);
					} else {
						newsong->next=songs_head;
						songs_head=newsong;
						numsongs++;
					}
				}
			}
		}

		kapi_outlog_info("NODE toneplayer:basic INITED id=%u, numsongs=%d", node_id, numsongs);
	}

	virtual ~basic_instance(void) {
	}

	virtual int start(void) override {
		assert(uv_thread_self()==thread);

		return 0;
	}

	virtual int stop(void) override {
		assert(uv_thread_self()==thread);
		return 0;
	}

	virtual int device_attached(const iot_conn_clientview* conn) override {
		assert(uv_thread_self()==thread);
		assert(device.conn==NULL);

		device.conn=conn;
		iot_deviface__toneplayer_CL iface(conn);
		uint32_t contoff;
		int err;
		uint8_t songidx=1;
		song_t* cursong=songs_head;
		while(cursong) {
			contoff=0;
			err=iface.set_song(contoff, songidx, cursong->title, cursong->num_tones, cursong->tones );
			assert(err==0);
			cursong=cursong->next;
			if(songidx==255) break;
			songidx++;
		}

		if(play) {
			err=iface.play(0, iot_toneplayer_playmode_t::REPEATALL, 0);
			assert(err==0);
		}
		return 0;
	}
	virtual int device_detached(const iot_conn_clientview* conn) override {
		assert(uv_thread_self()==thread);
		assert(device.conn!=NULL);

		device.conn=NULL;
		return 0;
	}
	virtual int device_action(const iot_conn_clientview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) override {
		assert(uv_thread_self()==thread);
		assert(device.conn==conn);
/*
//		int err;
		if(action_code==IOT_DEVCONN_ACTION_FULLREQUEST) {//new message arrived
			iot_deviface__keyboard_CL iface(&conn->deviface);
			const iot_deviface__keyboard_CL::msg* msg=iface.parse_event(data, data_size);
			if(!msg) return 0;

			switch(msg->event_code) {
				case iface.EVENT_KEYDOWN:
					kapi_outlog_info("GOT keyboard DOWN for key %d from device index %d", (int)msg->key, int(conn->index));
//					if(msg->key==KEY_ESC) {
//						kapi_outlog_info("Requesting state");
//						err=iface.request_state(conn->id, this);
//						assert(err==0);
//					}
					break;
				case iface.EVENT_KEYUP:
					kapi_outlog_info("GOT keyboard UP for key %d from device index %d", (int)msg->key, int(conn->index));
					break;
				case iface.EVENT_KEYREPEAT:
					kapi_outlog_info("GOT keyboard REPEAT for key %d from device index %d", (int)msg->key, int(conn->index));
					break;
				case iface.EVENT_SET_STATE:
					kapi_outlog_info("GOT NEW STATE, datasize=%u, statesize=%u from device index %d", data_size, (unsigned)(msg->statesize), int(conn->index));
					break;
				default:
					kapi_outlog_info("Got unknown event %d from device index %d, node_id=%u", int(msg->event_code), int(conn->index), node_id);
					return 0;
			}
			//update key state of device
			memcpy(device[conn->index].keystate, msg->state, msg->statesize*sizeof(uint32_t));
			update_outputs();
			return 0;
		}*/
		kapi_outlog_info("Device action, node_id=%u, act code %u, datasize %u from device index %d", node_id, unsigned(action_code), data_size, int(conn->index));
		return 0;
	}

//methods from iot_node_base
	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) override {
		play=false;
		if(valueinputs[0].new_value) {
			const iot_datavalue_boolean* v=iot_datavalue_boolean::cast(valueinputs[0].new_value);
			if(*v) play=true;
		}
		if(!device.conn) return 0;

		iot_deviface__toneplayer_CL iface(device.conn);
		int err;
		if(play) {
			err=iface.play(0, iot_toneplayer_playmode_t::REPEATALL, 0, 0);
		} else {
			err=iface.stop();
		}
		assert(err==0);

		return 0;
	}

};

static const iot_deviface_params* basic_devifaces[]={
	&iot_deviface_params_toneplayer::object
};



iot_node_moduleconfig_t IOT_NODE_MODULE_CONF(basic)={
	.version = IOT_VERSION_COMPOSE(0,0,1),
	.init_module = NULL,
	.deinit_module = NULL,
	.cpu_loading = 0,
	.num_devices = 1,
	.num_valueoutputs = 0,
	.num_valueinputs = 1,
	.num_msgoutputs = 0,
	.num_msginputs = 0,
	.is_persistent = 1,
	.is_sync = 0,

	.devcfg={
		{
			.label = "dev",
			.num_devifaces = sizeof(basic_devifaces)/sizeof(basic_devifaces[0]),
			.flag_canauto = 1,
			.flag_localonly = 1,
			.devifaces = basic_devifaces
		}
	},
	.valueoutput={},
	.valueinput={
		{
			.label = "enable",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		}
	},
	.msgoutput={},
	.msginput={},

	//methods
	.init_instance = &basic_instance::init_instance,
	.deinit_instance = &basic_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};






