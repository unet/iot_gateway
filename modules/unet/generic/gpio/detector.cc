#include<sys/types.h>
#include<sys/stat.h>
#include<sys/file.h>
#include<fcntl.h>
#include<stdlib.h>
#include<unistd.h>
#include<assert.h>
#include<new>


#include "iot_module.h"
#include "iot_hwdevcontype_gpio.h"
#include "iot_devclass_activatable.h"
#include "iot_devclass_keyboard.h"

#include "modules/brgl/accessory/libgpiod/src/include/gpiod.h"


IOT_LIBVERSION_DEFINE; //creates global symbol with library full version spec according to IOT_LIBVERSION, IOT_LIBPATCHLEVEL and IOT_LIBREVISION defines


/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////gpio:detector Device Detector module
/////////////////////////////////////////////////////////////////////////////////
class detector;
#define DETECTOR_MAX_DEVS 16
#define DETECTOR_POLL_INTERVAL 5000
#define DETECTOR_MAX_GPIOCTRLS 4
class detector : public iot_device_detector_base {
	uv_timer_t timer_watcher={};
	struct devdata_t {
		uint64_t recheck_after=0; //time from uv_now() when port must be retried
		iot_hwdev_details_gpio details;
		volatile enum {
			STATE_INVALID=0, //this record is free to be used for new manual device
			STATE_VALID, //record is used but there was error registering device
			STATE_REGISTERED //device was added to registry
		} state=STATE_INVALID;

		devdata_t(void) {}
	} devlist[DETECTOR_MAX_DEVS]; //current list of monitored ports and detected devices

	struct ctrldata_t {
		uint8_t index; //index of controller in system
		//TODO
	} ctrllist[DETECTOR_MAX_GPIOCTRLS];

	void on_timer(void) {
		//check manual devices
		uint64_t now=uv_now(loop);
		int err;

		//TODO detect GPIO controllers!!!!


		for(int i=0;i<DETECTOR_MAX_DEVS;i++) {
			if(devlist[i].recheck_after>now) continue;
			switch(devlist[i].state) {
				case devdata_t::STATE_INVALID: break;
				case devdata_t::STATE_VALID:
					err=kapi_hwdev_registry_action(IOT_ACTION_ADD, NULL, &devlist[i].details);
					if(err) {
						kapi_outlog_error("Cannot update device in registry: %s", kapi_strerror(err));
						devlist[i].recheck_after=now+5*60*1000; //5 min
						break;
					}
					devlist[i].state=devdata_t::STATE_REGISTERED;
					break;
				case devdata_t::STATE_REGISTERED: //do periodic recheck
					//nothing to do
					break;
			}
		}
	}

//iot_module_instance_base methods


	//Called to start work of previously inited instance.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, iot_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(void) {
		uv_timer_init(loop, &timer_watcher);
		timer_watcher.data=this;
		ref(); //instance is now additionally reffered in uv handle, so increase refcount to have correct closing of handle

		int err=uv_timer_start(&timer_watcher, [](uv_timer_t* handle) -> void {
			detector* obj=(detector*)(handle->data);
			obj->on_timer();
		}, 0, DETECTOR_POLL_INTERVAL);
		if(err<0) {
			uv_close((uv_handle_t*)&timer_watcher, kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'
			kapi_outlog_error("Cannot start timer: %s", uv_strerror(err));
			return IOT_ERROR_TEMPORARY_ERROR;
		}
		return 0;
	}
	virtual int stop(void) {
		assert(uv_thread_self()==thread);

		uv_close((uv_handle_t*)&timer_watcher, kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'

/*
		for(int i=0;i<DETECTOR_MAX_DEVS;i++) {
			switch(devlist[i].state) {
				case devdata_t::STATE_INVALID: break;
				case devdata_t::STATE_VALID:
					devlist[i].handle=NULL;
					break;
				case devdata_t::STATE_INITPENDING: 
				case devdata_t::STATE_REGISTERED: 
					assert(devlist[i].handle!=NULL);
					devlist[i].handle->deinit();
					devlist[i].handle=NULL;
					break;
			}
		}
*/
		return 0;
	}

public:
	detector(void) {
	}
	virtual ~detector(void) {
	}
	int init(json_object* devs) {
		int num=devs ? json_object_array_length(devs) : 0;
		int num_found=0;
		for(int i=0;i<num && num_found<DETECTOR_MAX_DEVS;i++) {
			json_object* cfg=json_object_array_get_idx(devs, i);
			if(!json_object_is_type(cfg, json_type_object)) {
				kapi_outlog_error("Manual device search params item must be an object, skipping '%s'", json_object_to_json_string(cfg));
				continue;
			}
			json_object* val=NULL;
			int method=-1; //-1 - invalid, 0 == gpio
			if(json_object_object_get_ex(cfg, "method", &val)) {
				if(json_object_is_type(val, json_type_string)) {
					int len=json_object_get_string_len(val);
					const char *m=json_object_get_string(val);
					if(len==4 && memcmp(m, "gpio", len)==0) method=0;
//					else if(len==4 && memcmp(m, "gpio", len)==0) method=0;
				}
			} else method=0;
			switch(method) {
				case 0: {//gpio
					int numpins=0;
					uint8_t port_mask[IOT_HWDEV_IDENT_GPIO_PORTMASKLEN];
					uint8_t ports[32];
					uint32_t drvmodule_id=0;
					uint16_t vendor=0, product=0, version=0;
					uint8_t ctrl_id=0;
					const char* name=NULL;
					
					if(json_object_object_get_ex(cfg, "driver_module", &val)) IOT_JSONPARSE_UINT(val, uint32_t, drvmodule_id);
					if(!drvmodule_id) {
						kapi_outlog_error("Invalid params for 'gpio' method of manual device search: non-zero 'driver_module' required");
						goto nextspec;
					}
					if(json_object_object_get_ex(cfg, "controller_id", &val)) IOT_JSONPARSE_UINT(val, uint8_t, ctrl_id);
					if(json_object_object_get_ex(cfg, "vendor", &val)) IOT_JSONPARSE_UINT(val, uint16_t, vendor);
					if(json_object_object_get_ex(cfg, "product", &val)) IOT_JSONPARSE_UINT(val, uint16_t, product);
					if(json_object_object_get_ex(cfg, "version", &val)) IOT_JSONPARSE_UINT(val, uint16_t, version);
					if(json_object_object_get_ex(cfg, "name", &val) && json_object_is_type(val, json_type_string)) name=json_object_get_string(val);

					memset(port_mask, 0, sizeof(port_mask));
					if(json_object_object_get_ex(cfg, "pins", &val) && json_object_is_type(val, json_type_array) && (numpins=json_object_array_length(val))>0) {
						if(numpins>32) {
							kapi_outlog_error("Invalid params for 'gpio' method of manual device search: no more than 32 pin indexes can be specified");
							goto nextspec;
						}
						for(int pi=0;pi<numpins;pi++) {
							json_object* pin=json_object_array_get_idx(val, pi);
							int32_t pinidx=-1;
							IOT_JSONPARSE_UINT(pin, uint8_t, pinidx);
							if(pinidx<0 || pinidx>=int32_t(sizeof(port_mask)*8)) {
								kapi_outlog_error("Invalid params for 'gpio' method of manual device search: pin index must be integer in range [0; %d]", int(sizeof(port_mask)*8)-1);
								goto nextspec;
							}
							uint32_t byteidx=uint32_t(pinidx) >> 3u;
							uint8_t bitidx=pinidx  - (byteidx<<3); //0 - 7
							uint8_t bitmask=(1<<bitidx);
							if(port_mask[byteidx] & bitmask) { //duplicate pin
								kapi_outlog_error("Invalid params for 'gpio' method of manual device search: duplicate pin index %d", int(pinidx));
								goto nextspec;
							}
							port_mask[byteidx] |= bitmask;
							ports[pi]=uint8_t(pinidx);
						}
					} else {
						kapi_outlog_error("Invalid params for 'gpio' method of manual device search: non-empty 'pins' array required");
						goto nextspec;
					}
					json_object_object_get_ex(cfg, "params", &val);

					devlist[num_found].details.set(name, val, uint8_t(numpins), ports, ctrl_id, drvmodule_id, vendor, product, version);
					devlist[num_found].state=devlist[num_found].STATE_VALID;
					devlist[num_found].recheck_after=0;
					num_found++;
					break;
				}
				default:
					kapi_outlog_error("Unknown method of manual device search '%s', skipping", json_object_get_string(val));
					break;
			}
nextspec: ;
		}
		return 0;
	}
	int deinit(void) {
		return 0;
	}

	//manual_devices is array of objects like {
	//	method: "default"  - optional if single method exists. determines which additional fields must be present
	//	serial_port: "PORTNAME"
	//}
	static int init_instance(iot_device_detector_base**instance, json_object *json_params, json_object *manual_devices) {
		if(manual_devices && (!json_object_is_type(manual_devices, json_type_array) || !json_object_array_length(manual_devices))) {
			manual_devices=NULL;
		}

		detector *inst=new detector();
		if(!inst) return IOT_ERROR_TEMPORARY_ERROR;

		int err=inst->init(manual_devices);
		if(err) { //error
			inst->unref();
			return err;
		}
		*instance=inst;
		return 0;
	}
	//called to deinit instance.
	//Return values:
	//0 - success
	//any other error leaves instance in hang state
	static int deinit_instance(iot_device_detector_base* instance) {
		detector *inst=static_cast<detector*>(instance);
		int err=inst->deinit();
		if(err) return err;
		inst->unref();
		return 0;
	}
	static int check_system(void) {
		gpiod_chip_iter *iterator=gpiod_chip_iter_new();
		if(!iterator) return IOT_ERROR_TEMPORARY_ERROR;
		gpiod_chip *chip=gpiod_chip_iter_next(iterator);
		gpiod_chip_iter_free(iterator);
		if(!chip) return IOT_ERROR_NOT_SUPPORTED; //may be retry to find USB controllers?
		return 0;
	}

};

//static iot_hwdevcontype_t detector_devcontypes[]={DEVCONTYPE_CUSTOM_LINUXINPUT};


iot_detector_moduleconfig_t IOT_DETECTOR_MODULE_CONF(det)={
	.version = IOT_VERSION_COMPOSE(0,0,1),
	.cpu_loading = 3,
	.manual_devices_required = 0,
//	.num_hwdevcontypes = sizeof(detector_devcontypes)/sizeof(detector_devcontypes[0]),

	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &detector::init_instance,
	.deinit_instance = &detector::deinit_instance,
	.check_system = &detector::check_system

//	.hwdevcontypes = detector_devcontypes
};


//driver for RaZberry controller which also implements detecting for connected devices
#define PINDRV_MAX_PORTS 32
struct pindrv_instance : public iot_device_driver_base {
	iot_hwdev_details_gpio dev_data;
	gpiod_chip *chip=NULL;
	gpiod_line_bulk bulk=GPIOD_LINE_BULK_INITIALIZER;
	uint32_t lines_mask; //has bulk.num_lines lower bits set
	uv_timer_t timer_watcher={}; //to count different retries
	uv_poll_t pollfd[PINDRV_MAX_PORTS];
	int num_polfds=0; //number of inited pollfds
	const iot_conn_drvview *iconn=NULL;
	enum product_t : uint8_t {
		PROD_PIN_INPUT,
		PROD_PIN_OUTPUT,
	} product;
	enum outputmode_t : uint8_t {
		OUTMODE_PUSH_PULL,
		OUTMODE_OPEN_DRAIN,
		OUTMODE_OPEN_SOURCE
	};
	bool timer_active=false;

	union prodstate_t { //product dependent state and params
		struct {
			uint32_t state_mask; //current state last reported to clients
			uint32_t new_state_mask; //new state to sent to clients when reseting new_state_pending_mask after timeout
			uint32_t new_state_pending_mask; //shows which bits wait for timeout to apply new state
			uint32_t filter_updates_ms;
			bool pull_up; //enable pull up resistor
			bool pull_down; //enable pull down resistor
			bool active_low; //The active state of the line is low (high is the default)
		} pin_input;
		struct {
			uint32_t cur_state; //current state set on outputs
			uint32_t want_state_bitmap; //mask of bits which are pending to be applied from want_state to cur_state
			uint32_t want_state; //new values for bits. want_state_bitmap shows which bits were updated
			uint32_t filter_updates_ms;

			outputmode_t mode;
			bool pull_updown; //enable pull-up for OUTMODE_OPEN_DRAIN or pull-down for OUTMODE_OPEN_SOURCE
			bool active_low; //The active state of the line is low (high is the default)
		} pin_output;
	} prodstate;
	bool started=false;

	static int init_instance(iot_device_driver_base**instance, const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data, iot_devifaces_list* devifaces, json_object *json_params) {
		const iot_hwdev_details_gpio* details=iot_hwdev_details_gpio::cast(dev_data);
		if(!details) return IOT_ERROR_NOT_SUPPORTED;
		if(details->vendor!=1 || details->version!=0) return IOT_ERROR_NOT_SUPPORTED;
		if(!details->num_ports || details->num_ports>PINDRV_MAX_PORTS) return IOT_ERROR_INVALID_DEVICE_DATA;
		int err=0;
		product_t product;
		prodstate_t prodstate;
		gpiod_line *lines[details->num_ports];
		json_object* val=NULL;
		json_object* cfg=details->params;

		//try to open chip
		gpiod_chip *chip=gpiod_chip_open_by_number(details->ctrl_id);
		if(!chip) return IOT_ERROR_INVALID_DEVICE_DATA;

		//try to open all lines
		uint16_t i;
		for(i=0;i<details->num_ports;i++) {
			lines[i]=gpiod_chip_get_line(chip, details->ports[i]);
			if(!lines[i]) {err=IOT_ERROR_INVALID_DEVICE_DATA;goto onerror;}
		}

		if(details->product==1) { //Pin Input
			iot_deviface_params_keyboard iface(false, details->num_ports-1);
			devifaces->add(&iface);
			product=PROD_PIN_INPUT;
			prodstate.pin_input={.state_mask=0, .new_state_mask=0, .new_state_pending_mask=0, .filter_updates_ms=0, .pull_up=false, .pull_down=false, .active_low=false};
			//parse params
			if(cfg && json_object_is_type(cfg, json_type_object)) {
				if(json_object_object_get_ex(cfg, "active_low", &val)) {
					prodstate.pin_input.active_low=json_object_get_boolean(val);
				}
				if(json_object_object_get_ex(cfg, "pull_up", &val)) {
					prodstate.pin_input.pull_up=json_object_get_boolean(val);
				}
				if(json_object_object_get_ex(cfg, "pull_down", &val)) {
					prodstate.pin_input.pull_down=json_object_get_boolean(val);
				}
				if(json_object_object_get_ex(cfg, "filter_updates_ms", &val)) {
					IOT_JSONPARSE_UINT(val, uint32_t, prodstate.pin_input.filter_updates_ms);
					if(prodstate.pin_input.filter_updates_ms>3600000) prodstate.pin_input.filter_updates_ms=3600000;
				}
			}
		} else if(details->product==2) { //Pin Output
			iot_deviface_params_activatable iface(details->num_ports);
			devifaces->add(&iface);
			product=PROD_PIN_OUTPUT;
			prodstate.pin_output={.cur_state=0, .want_state_bitmap=0, .want_state=0, .filter_updates_ms=0, .mode=OUTMODE_PUSH_PULL, .pull_updown=false, .active_low=false};
			//parse params
			if(cfg && json_object_is_type(cfg, json_type_object)) {
				if(json_object_object_get_ex(cfg, "active_low", &val)) {
					prodstate.pin_output.active_low=json_object_get_boolean(val);
				}
				if(json_object_object_get_ex(cfg, "mode", &val)) {
					const char *mode=json_object_get_string(val);
					if(strcmp(mode, "open_drain")==0) prodstate.pin_output.mode=OUTMODE_OPEN_DRAIN;
						else if(strcmp(mode, "open_source")==0) prodstate.pin_output.mode=OUTMODE_OPEN_SOURCE;
				}
				if(json_object_object_get_ex(cfg, "filter_updates_ms", &val)) {
					IOT_JSONPARSE_UINT(val, uint32_t, prodstate.pin_output.filter_updates_ms);
					if(prodstate.pin_output.filter_updates_ms>3600000) prodstate.pin_output.filter_updates_ms=3600000;
				}
			}
		}
		if(!devifaces->num) {err=IOT_ERROR_NOT_SUPPORTED;goto onerror;}
		pindrv_instance *inst;
		inst=new pindrv_instance(product, &prodstate, details, chip, lines);
		if(!inst) {err=IOT_ERROR_TEMPORARY_ERROR;goto onerror;}

		*instance=inst;

		char buf[256];
		kapi_outlog_info("Driver inited for device with name='%s', contype=%s", details->name, dev_ident->local->sprint(buf,sizeof(buf)));
		return 0;
onerror:
		if(chip) gpiod_chip_close(chip);
		return err;
	}

	static int deinit_instance(iot_device_driver_base* instance) {
		pindrv_instance *inst=static_cast<pindrv_instance*>(instance);
		inst->unref();
		return 0;
	}

	static int check_device(const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data) {
		//SUPPORTED DEVICE IS SPECIFIED IN DEVICE FILTER!!
		//HERE JUST CHECK THAT product id is correct
		const iot_hwdev_details_gpio* details=iot_hwdev_details_gpio::cast(dev_data);
		if(!details) return IOT_ERROR_NOT_SUPPORTED;
		if(details->vendor!=1 || details->version!=0) return IOT_ERROR_NOT_SUPPORTED;
		if(details->product!=1 && details->product!=2) return IOT_ERROR_NOT_SUPPORTED;
		return 0;
	}
/////////////public methods

private:
	pindrv_instance(product_t product, prodstate_t *prodstate_, const iot_hwdev_details_gpio* details, gpiod_chip *chip, gpiod_line **ports) : dev_data(*details), chip(chip), product(product) {
		assert(dev_data.num_ports>0 && dev_data.num_ports<=32);
		for(uint16_t i=0;i<dev_data.num_ports;i++) gpiod_line_bulk_add(&bulk, ports[i]);
		lines_mask=0xFFFFFFFFu >> (32 - bulk.num_lines);
		prodstate=*prodstate_;
	}
	~pindrv_instance(void) {
		if(chip) gpiod_chip_close(chip);
		chip=NULL;
	}


	//Called to start work of previously inited instance.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, iot_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(void) {
//		if(!handle->try_ctrldriver_attach(this)) return IOT_ERROR_CRITICAL_ERROR; //device is unusable
		int err;
		if(dev_data.product==1) { //input
			err=gpiod_line_request_bulk_both_edges_events_flags(&bulk, "iotgw:unet/generic/gpio/pindrv", prodstate.pin_input.active_low ? GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW : 0);
//			err=gpiod_line_request_bulk_input_flags(&bulk, "iotgw:unet/generic/gpio/pindrv", prodstate.pin_input.active_low ? GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW : 0);
		} else { //output
			err=gpiod_line_request_bulk_output_flags(&bulk,
				"iotgw:unet/generic/gpio/pindrv",
				(prodstate.pin_output.active_low ? GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW : 0) | (prodstate.pin_output.mode==OUTMODE_OPEN_DRAIN ? GPIOD_LINE_REQUEST_FLAG_OPEN_DRAIN : prodstate.pin_output.mode==OUTMODE_OPEN_SOURCE ? GPIOD_LINE_REQUEST_FLAG_OPEN_SOURCE : 0),
				NULL
			);
		}
		if(err<0) {
			kapi_outlog_error("Cannot lock GPIO line(s): %s", uv_strerror(uv_translate_sys_error(errno)));
			return IOT_ERROR_TEMPORARY_ERROR;
		}

		kapi_outlog_info("Driver attached to '%s'",dev_data.name);


		if(dev_data.product==1) { //input
			for(uint32_t i=0;i<bulk.num_lines;i++) {
				int fd=gpiod_line_event_get_fd(bulk.lines[i]);
				if(fd<0) {
					kapi_outlog_error("Cannot get FD for GPIO line %u: %s", gpiod_line_offset(bulk.lines[i]), uv_strerror(uv_translate_sys_error(errno)));
					goto onerror;
				}
				err=uv_poll_init(loop, &pollfd[i], fd);
				if(err<0) {
					kapi_outlog_error("Cannot init polling on FD for GPIO line %u: %s", gpiod_line_offset(bulk.lines[i]), uv_strerror(err));
					goto onerror;
				}
				pollfd[i].data=this;
				ref(); //instance is now additionally reffered in uv handle, so increase refcount to have correct closing of handle
				num_polfds++;
				uv_poll_start(&pollfd[i], UV_READABLE, [](uv_poll_t* handle, int status, int events)->void {
					pindrv_instance* inst=static_cast<pindrv_instance*>(handle->data);
					inst->on_port_event(handle, status);
				});
			}
			//read initial values
			int ivals[bulk.num_lines];
			err=gpiod_line_get_value_bulk(&bulk, ivals);
			assert(!err);
			for(uint32_t i=0;i<bulk.num_lines;i++) {
				if(ivals[i]) {
					prodstate.pin_input.state_mask |= (1u<<i);
				} else {
					prodstate.pin_input.state_mask &= ~(1u<<i);
				}
			}
		} else { //output
		}
		uv_timer_init(loop, &timer_watcher);
		timer_watcher.data=this;
		ref(); //instance is now additionally reffered in uv handle, so increase refcount to have correct closing of handle

//		int err=handle->start_device_polling(this, [](iot_device_driver_base* obj)->void {
//			pindrv_instance* inst=static_cast<pindrv_instance*>(obj);
//			inst->onDevicesUpdated();
//		});
//		if(err) {
//			assert(false);
//			handle->ctrldriver_detach(this);
//			return IOT_ERROR_CRITICAL_ERROR;
//		}
//		//TODO
//		err=check_device_list();
//		if(err) {
//			assert(false);
//			handle->ctrldriver_detach(this);
//			return err;
//		}

		started=true;
		return 0;
onerror:
		for(int i=0;i<num_polfds; i++) {
			uv_close((uv_handle_t*)&pollfd[i], kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'
		}
		gpiod_line_release_bulk(&bulk);
		return IOT_ERROR_TEMPORARY_ERROR;
	}

	//called to stop work of started instance. call can be followed by deinit or started again (if stop was manual, by user)
	//Return values:
	//0 - driver successfully stopped and can be deinited or restarted
	//IOT_ERROR_TRY_AGAIN - driver requires some time (async operation) to stop gracefully. kapi_self_abort() will be called to notify core when stop is finished.
	//						anyway second stop() call must free all resources correctly, may be in a hard way. otherwise module will be blocked and left in hang state (deinit
	//						cannot be called until stop reports OK)
	//any other error is treated as critical bug and driver is blocked for further starts. deinit won't be called for such instance. instance is put into hang state
	virtual int stop(void) {
		started=false;

		for(int i=0;i<num_polfds; i++) {
			uv_close((uv_handle_t*)&pollfd[i], kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'
		}

		gpiod_line_release_bulk(&bulk);

		uv_close((uv_handle_t*)&timer_watcher, kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'
		return 0;
	}

//iot_device_driver_base methods
	virtual int device_open(const iot_conn_drvview* conn) {
//		kapi_notify_write_avail(conn, true);
//		const iot_devifacetype_metaclass* ifacetype=conn->deviface->get_metaclass();
		if(product==PROD_PIN_INPUT) {
			assert(conn->deviface->get_metaclass()==&iot_devifacetype_metaclass_keyboard::object);
			if(iconn) return IOT_ERROR_LIMIT_REACHED;
			iconn=conn;

			iot_deviface__keyboard_DRV iface(conn);
			int err=iface.send_set_state(&prodstate.pin_input.state_mask);
			assert(err==0);
		} else {
			assert(conn->deviface->get_metaclass()==&iot_devifacetype_metaclass_activatable::object);
			if(iconn) return IOT_ERROR_LIMIT_REACHED;
			iconn=conn;
			prodstate.pin_output.want_state_bitmap=prodstate.pin_output.want_state=0;

			iot_deviface__activatable_DRV iface(conn);
			int err=iface.send_current_state(prodstate.pin_output.cur_state, lines_mask);
			assert(err==0);
		}
		return 0;
	}
	virtual int device_close(const iot_conn_drvview* conn) {
//		const iot_devifacetype_metaclass* ifacetype=conn->deviface->get_metaclass();
		if(product==PROD_PIN_INPUT) {
			assert(conn->deviface->get_metaclass()==&iot_devifacetype_metaclass_keyboard::object);
			if(conn==iconn) iconn=NULL;
		} else {
			assert(conn->deviface->get_metaclass()==&iot_devifacetype_metaclass_activatable::object);
			if(conn==iconn) iconn=NULL;
			prodstate.pin_output.want_state_bitmap=prodstate.pin_output.want_state=0;
		}
		return 0;
	}
	virtual int device_action(const iot_conn_drvview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) {
		int err;
//		if(action_code==IOT_DEVCONN_ACTION_CANWRITE) {
//		} else 
		if(action_code==IOT_DEVCONN_ACTION_FULLREQUEST && conn==iconn) {
			if(product==PROD_PIN_INPUT) {
				iot_deviface__keyboard_DRV iface(conn);
				const iot_deviface__keyboard_DRV::msg* msg=iface.parse_req(data, data_size);
				if(!msg) return IOT_ERROR_MESSAGE_IGNORED;

				if(msg->req_code==iface.REQ_GET_STATE) {
					err=iface.send_set_state(&prodstate.pin_input.state_mask);
					assert(err==0);
					return 0;
				}
				return IOT_ERROR_MESSAGE_IGNORED;
			} else { //output
				iot_deviface__activatable_DRV iface(conn);
				const iot_deviface__activatable_DRV::reqmsg* msg=iface.parse_req(data, data_size);
				if(!msg) return IOT_ERROR_MESSAGE_IGNORED;

				if(msg->req_code==iface.REQ_GET_STATE) {
					kapi_outlog_info("Driver got GET STATE");
					err=iface.send_current_state(prodstate.pin_output.cur_state, lines_mask);
					assert(err==0);
					return 0;
				}
				else if(msg->req_code==iface.REQ_SET_STATE) {
					kapi_outlog_info("Driver got SET STATE activate=%04x, deactivate=%04x", msg->activate_mask, msg->deactivate_mask);
					uint32_t activate_mask=msg->activate_mask & lines_mask, deactivate_mask=msg->deactivate_mask & lines_mask;

//					prodstate.pin_output.want_state_bitmap = activate_mask | deactivate_mask;
					uint32_t clr=activate_mask & deactivate_mask; //find bits set in both masks
					if(clr) { //reset common bits
						activate_mask&=~clr;
						deactivate_mask&=~clr;
					}
					prodstate.pin_output.want_state=uint32_t((prodstate.pin_output.want_state | activate_mask) & ~deactivate_mask); //set activated bits, reset deactivated, reset invalid

					uint32_t dif=(prodstate.pin_output.cur_state ^ prodstate.pin_output.want_state);// & prodstate.pin_output.want_state_bitmap; //difference
					if(dif && !timer_active) {
						for(uint32_t i=0;i<bulk.num_lines;i++) {
							if(dif & (1<<i)) {
								err=gpiod_line_set_value(bulk.lines[i], prodstate.pin_output.want_state & (1<<i) ? 1 : 0);
								assert(err==0);
							}
						}
						prodstate.pin_output.cur_state=prodstate.pin_output.want_state;
//						prodstate.pin_output.want_state_bitmap=0;

						if(prodstate.pin_input.filter_updates_ms>0) {
							err=uv_timer_start(&timer_watcher, on_timer_static, prodstate.pin_input.filter_updates_ms, 0);
							assert(err==0);
							timer_active=true;
						}
					}

					return 0;
				}
				return IOT_ERROR_MESSAGE_IGNORED;
			}
		}
		kapi_outlog_info("Device action in driver inst %u, act code %u, datasize %u from device index %d", miid.iid, unsigned(action_code), data_size, int(conn->index));
		return 0;
	}

	static void on_timer_static(uv_timer_t* handle) {
		pindrv_instance* obj=static_cast<pindrv_instance*>(handle->data);
		obj->on_timer();
	}
	void on_timer(void) {
		timer_active=false;
		int err;
		if(product==PROD_PIN_INPUT) {
			prodstate.pin_input.new_state_pending_mask=0;
			if(prodstate.pin_input.state_mask != prodstate.pin_input.new_state_mask) { //different
//				uint32_t dif=prodstate.pin_input.state_mask ^ prodstate.pin_input.new_state_mask;
				prodstate.pin_input.state_mask=prodstate.pin_input.new_state_mask;
				if(iconn) {
					iot_deviface__keyboard_DRV iface(iconn);
					iface.send_set_state(&prodstate.pin_input.state_mask);
				}
				//protect new value from change during filter time
				err=uv_timer_start(&timer_watcher, on_timer_static, prodstate.pin_input.filter_updates_ms, 0);
				assert(err==0);
				timer_active=true;
			}
		} else {
			uint32_t dif=(prodstate.pin_output.cur_state ^ prodstate.pin_output.want_state);// & prodstate.pin_output.want_state_bitmap; //difference
			if(dif) {
				for(uint32_t i=0;i<bulk.num_lines;i++) {
					if(dif & (1<<i)) {
						err=gpiod_line_set_value(bulk.lines[i], prodstate.pin_output.want_state & (1<<i) ? 1 : 0);
						assert(err==0);
					}
				}
				prodstate.pin_output.cur_state=prodstate.pin_output.want_state;
//				prodstate.pin_output.want_state_bitmap=0;

				err=uv_timer_start(&timer_watcher, on_timer_static, prodstate.pin_input.filter_updates_ms, 0);
				assert(err==0);
				timer_active=true;
			}
		}
	}
	void on_port_event(uv_poll_t* handle, int status) {
		assert(product==PROD_PIN_INPUT);
		long line_idx=handle-pollfd;
		assert(line_idx>=0 && unsigned(line_idx)<bulk.num_lines);
		if(status<0) {
			kapi_outlog_error("Got error polling on FD for GPIO line %u: %s", gpiod_line_offset(bulk.lines[line_idx]), uv_strerror(status));
			kapi_self_abort(IOT_ERROR_CRITICAL_ERROR);
			return;
		}
		uv_os_fd_t fd;
		int err=uv_fileno((uv_handle_t*)handle, &fd);
		if(err) {
			assert(false);
			kapi_self_abort(IOT_ERROR_CRITICAL_ERROR);
			return;
		}
		gpiod_line_event ev;
		uint32_t bit=(1u<<uint32_t(line_idx)), nbit=~bit;
		do {
			err=gpiod_line_event_read_fd(fd, &ev);
			if(!err) {
				if(ev.event_type==GPIOD_LINE_EVENT_RISING_EDGE) {
					prodstate.pin_input.new_state_mask |= bit;
				} else {
					prodstate.pin_input.new_state_mask &= nbit;
				}
				if(timer_active) continue;
				if(prodstate.pin_input.new_state_pending_mask & bit) { //we're waiting for state stabilization, so just update new state mask
					continue;
				}
				//this is first event after stabilization, apply it immediately
				if(ev.event_type==GPIOD_LINE_EVENT_RISING_EDGE) {
					if(prodstate.pin_input.state_mask & bit) continue; //check if really changed
					prodstate.pin_input.state_mask |= bit;
				} else {
					if(!(prodstate.pin_input.state_mask & bit)) continue; //check if really changed
					prodstate.pin_input.state_mask &= nbit;
				}
				if(iconn) {
					iot_deviface__keyboard_DRV iface(iconn);
					if(prodstate.pin_input.state_mask & bit) {
						iface.send_keydown(line_idx, &prodstate.pin_input.state_mask);
					} else {
						iface.send_keyup(line_idx, &prodstate.pin_input.state_mask);
					}
				}
				if(prodstate.pin_input.filter_updates_ms>0) { //setup filtering timer
					err=uv_timer_start(&timer_watcher, on_timer_static, prodstate.pin_input.filter_updates_ms, 0);
					assert(err==0);
					timer_active=true;
				} else {
					prodstate.pin_input.new_state_pending_mask|=bit;
				}
			}
			else if(errno!=EINTR) break;
		} while(1);

		if(timer_active) return;
		if(prodstate.pin_input.new_state_pending_mask) { //apply new state
			if((prodstate.pin_input.state_mask ^ prodstate.pin_input.new_state_mask) & bit) { //different
				prodstate.pin_input.state_mask ^= bit;
				if(iconn) {
					iot_deviface__keyboard_DRV iface(iconn);
					if(prodstate.pin_input.state_mask & bit) {
						iface.send_keydown(line_idx, &prodstate.pin_input.state_mask);
					} else {
						iface.send_keyup(line_idx, &prodstate.pin_input.state_mask);
					}
				}
			}
			prodstate.pin_input.new_state_pending_mask=0;
		}
//printf("STATE MASK=%08x\n", prodstate.pin_input.state_mask);
	}
};

static const iot_hwdev_localident_gpio devfilter1(1,{{3, 0, 0, 0, 0, 0}}); //3 is driver module's ID
static const iot_hwdev_localident* driver_devidents[]={
	&devfilter1
};

static const iot_devifacetype_metaclass* driver_ifaces[]={
//	&iot_devifacetype_metaclass_keyboard::object,
};


iot_driver_moduleconfig_t IOT_DRIVER_MODULE_CONF(pindrv)={
	.version = IOT_VERSION_COMPOSE(0,0,1),

	.cpu_loading = 0,
	.num_hwdev_idents = sizeof(driver_devidents)/sizeof(driver_devidents[0]),
	.num_dev_ifaces = sizeof(driver_ifaces)/sizeof(driver_ifaces[0]),
	.is_detector = 0,

	.hwdev_idents = driver_devidents,
	.dev_ifaces = driver_ifaces,

	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &pindrv_instance::init_instance,
	.deinit_instance = &pindrv_instance::deinit_instance,
	.check_device = &pindrv_instance::check_device
};



//up to 32
#define EVENTSRV_NUM_OUTPUTS 4

struct eventsrc_instance : public iot_node_base {
	uint32_t node_id=0;

	const iot_conn_clientview *cconn=NULL;
	uint32_t state_mask=0;
	uint16_t num_lines=0;

/////////////static fields/methods for module instances management
	static int init_instance(iot_node_base**instance, uint32_t node_id, json_object *json_cfg) {
		eventsrc_instance *inst=new eventsrc_instance(node_id);
		*instance=inst;

		return 0;
	}
	static int deinit_instance(iot_node_base* instance) {
		eventsrc_instance *inst=static_cast<eventsrc_instance*>(instance);
		inst->unref();
		return 0;
	}
private:
	eventsrc_instance(uint32_t node_id) : node_id(node_id) {}
	virtual ~eventsrc_instance(void) {}

	virtual int start(void) override {
		return 0;
	}

	virtual int stop(void) override {
		return 0;
	}

//methods from iot_node_base
	virtual int device_attached(const iot_conn_clientview* conn) override {
		assert(conn->index==0);
		assert(cconn==NULL);

		cconn=conn;
		state_mask=0;
		iot_deviface__keyboard_CL iface(conn);
		num_lines=iface.get_max_keycode()+1;
		if(num_lines>EVENTSRV_NUM_OUTPUTS) num_lines=EVENTSRV_NUM_OUTPUTS;
		return 0;
	}
	virtual int device_detached(const iot_conn_clientview* conn) override {
		assert(conn->index==0);
		if(cconn) {
			cconn=NULL;
			update_outputs();
		}
		return 0;
	}
	virtual int device_action(const iot_conn_clientview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) override {
		assert(conn->index==0);
		assert(cconn==conn);

//		int err;
		if(action_code==IOT_DEVCONN_ACTION_FULLREQUEST) {//new message arrived
			iot_deviface__keyboard_CL iface(conn);
			const iot_deviface__keyboard_CL::msg* msg=iface.parse_event(data, data_size);
			if(!msg) return 0;

			switch(msg->event_code) {
				case iface.EVENT_KEYDOWN:
					kapi_outlog_info("GOT keyboard DOWN for key %d from device index %d", (int)msg->key, int(conn->index));
					break;
				case iface.EVENT_KEYUP:
					kapi_outlog_info("GOT keyboard UP for key %d from device index %d", (int)msg->key, int(conn->index));
					break;
				case iface.EVENT_SET_STATE:
					kapi_outlog_info("GOT NEW STATE, datasize=%u, statesize=%u from device index %d", data_size, (unsigned)(msg->statesize), int(conn->index));
					break;
				default:
					kapi_outlog_info("Got unknown event %d from device index %d, node_id=%u", int(msg->event_code), int(conn->index), node_id);
					return 0;
			}
			//update key state of device
			//TODO check that change is withing num_lines bits
			state_mask=msg->state[0];
			update_outputs();
			return 0;
		}
		kapi_outlog_info("Device action, node_id=%u, act code %u, datasize %u from device index %d", node_id, unsigned(action_code), data_size, int(conn->index));
		return 0;
	}

//own methods
	int update_outputs(void) { //set current common key state on output
		uint8_t outn[EVENTSRV_NUM_OUTPUTS];
		const iot_datavalue* outv[EVENTSRV_NUM_OUTPUTS];
		for(uint16_t i=0;i<EVENTSRV_NUM_OUTPUTS;i++) {
			outn[i]=i;
			if(!cconn) outv[i]=NULL;
			else outv[i]=(state_mask & (1<<i)) ? &iot_datavalue_boolean::const_true : &iot_datavalue_boolean::const_false;
		}
		int err=kapi_update_outputs(NULL, EVENTSRV_NUM_OUTPUTS, outn, outv);
		if(err) {
			kapi_outlog_error("Cannot update output value for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
		}
		return err;
	}
};

//keys_instance* keys_instance::instances_head=NULL;


static const iot_deviface_params_keyboard kbd_filter_any(2);

static const iot_deviface_params* eventsrc_devifaces[]={
	&kbd_filter_any
};


iot_node_moduleconfig_t IOT_NODE_MODULE_CONF(eventsrc)={
	.version = IOT_VERSION_COMPOSE(0,0,1),
	.cpu_loading = 0,
	.num_devices = 1,
	.num_valueoutputs = EVENTSRV_NUM_OUTPUTS,
	.num_valueinputs = 0,
	.num_msgoutputs = 0,
	.num_msginputs = 0,
	.is_persistent = 1,
	.is_sync = 0,

	.devcfg={
		{
			.label = "dev",
			.num_devifaces = sizeof(eventsrc_devifaces)/sizeof(eventsrc_devifaces[0]),
			.flag_canauto = 1,
			.flag_localonly = 0,
			.devifaces = eventsrc_devifaces
		}
	},
	.valueoutput={
		{
			.label = "out0",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		},
		{
			.label = "out1",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		},
		{
			.label = "out2",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		},
		{
			.label = "out3",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		}
	},
	.valueinput={
	},
	.msgoutput={
	},
	.msginput={
	},

	//methods
	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &eventsrc_instance::init_instance,
	.deinit_instance = &eventsrc_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};


#define ACT_NUM_INPUTS 4

struct act_instance : public iot_node_base {
	uint32_t node_id;
	uint32_t activate_bitmap=0, deactivate_bitmap=0;
	const iot_conn_clientview *cconn=NULL;

/////////////static fields/methods for module instances management
	static int init_module(void) {
		return 0;
	}
	static int deinit_module(void) {
		return 0;
	}

	static int init_instance(iot_node_base** instance, uint32_t node_id, json_object *json_cfg) {
		act_instance *inst=new act_instance(node_id);
		*instance=inst;
		return 0;
	}

	static int deinit_instance(iot_node_base* instance) {
		act_instance *inst=static_cast<act_instance*>(instance);
		inst->unref();
		return 0;
	}
private:
	act_instance(uint32_t node_id) : node_id(node_id)
	{
	}
	virtual int start(void) override {
		return 0;
	}
	virtual int stop(void) override {
		return 0;
	}

	virtual int device_attached(const iot_conn_clientview* conn) override {
		assert(cconn==NULL);

		cconn=conn;
		iot_deviface__activatable_CL iface(conn);
		int err=iface.set_state(activate_bitmap, deactivate_bitmap );
		assert(err==0);
		return 0;
	}
	virtual int device_detached(const iot_conn_clientview* conn) override {
		assert(cconn!=NULL);

		cconn=NULL;
		return 0;
	}
	virtual int device_action(const iot_conn_clientview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) override {
		assert(cconn==conn);
		kapi_outlog_info("Device action, node_id=%u, act code %u, datasize %u from device index %d", node_id, unsigned(action_code), data_size, int(conn->index));
		return 0;
	}

//methods from iot_node_base
	virtual int process_input_signals(iot_event_id_t eventid, uint8_t num_valueinputs, const iot_value_signal *valueinputs, uint8_t num_msginputs, const iot_msg_signal *msginputs) override {
		activate_bitmap=deactivate_bitmap=0;
		for(int i=0;i<num_valueinputs;i++) {
			const iot_datavalue_boolean* v=iot_datavalue_boolean::cast(valueinputs[i].new_value);
			if(v && *v) activate_bitmap|=1<<i;
				else deactivate_bitmap|=1<<i; //false or undef disable output
		}
		if(!cconn) return 0;

		iot_deviface__activatable_CL iface(cconn);
		int err=iface.set_state(activate_bitmap, deactivate_bitmap );
		assert(err==0);

		return 0;
	}

};

//keys_instance* keys_instance::instances_head=NULL;


static const iot_deviface_params_activatable act_filter_any(1,0);

static const iot_deviface_params* act_devifaces[]={
	&act_filter_any
};


iot_node_moduleconfig_t IOT_NODE_MODULE_CONF(act)={
	.version = IOT_VERSION_COMPOSE(0,0,1),

	.cpu_loading = 0,
	.num_devices = 1,
	.num_valueoutputs = 0,
	.num_valueinputs = ACT_NUM_INPUTS,
	.num_msgoutputs = 0,
	.num_msginputs = 0,
	.is_persistent = 1,
	.is_sync = 0,

	.devcfg={
		{
			.label = "dev",
			.num_devifaces = sizeof(act_devifaces)/sizeof(act_devifaces[0]),
			.flag_canauto = 1,
			.flag_localonly = 0,
			.devifaces = act_devifaces
		}
	},
	.valueoutput={},
	.valueinput={
		{
			.label = "in0",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		},
		{
			.label = "in1",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		},
		{
			.label = "in2",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		},
		{
			.label = "in3",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_boolean::object
		}
	},
	.msgoutput={},
	.msginput={},

	//methods
	.init_module = act_instance::init_module,
	.deinit_module = act_instance::deinit_module,
	.init_instance = &act_instance::init_instance,
	.deinit_instance = &act_instance::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};






