#include<sys/types.h>
#include<sys/stat.h>
#include<sys/file.h>
#include<fcntl.h>
#include<stdlib.h>
#include<unistd.h>
#include<assert.h>
#include<new>


#include "iot_module.h"
#include "iot_hwdevcontype_serial.h"
#include "iot_hwdevcontype_zwave.h"
#include "iot_devclass_meter.h"
#include "devclass_controller.h"

#include "modules/zwave_me/accessory/zway/ZWayLib.h"
#include "modules/zwave_me/accessory/zway/ZLogging.h"


IOT_LIBVERSION_DEFINE; //creates global symbol with library full version spec according to IOT_LIBVERSION, IOT_LIBPATCHLEVEL and IOT_LIBREVISION defines


static struct device_prop_rules {
	const char* name;
	uint8_t namelen;
	uint8_t size; //1 if byte_ptr is valid, 2 if word_ptr is valid
	ZWDataType type; //required type of dataholder
	uint8_t iot_hwdev_details_zwave::* byte_ptr;
	uint16_t iot_hwdev_details_zwave::* word_ptr;
} dev_props_need[]={
	{".manufacturerId", sizeof(".manufacturerId")-1, 2, Integer, NULL, &iot_hwdev_details_zwave::manufacturer_id},
	{".manufacturerProductId", sizeof(".manufacturerProductId")-1, 2, Integer, NULL, &iot_hwdev_details_zwave::product_id},
	{".manufacturerProductType", sizeof(".manufacturerProductType")-1, 2, Integer, NULL, &iot_hwdev_details_zwave::product_type},
	{".ZWProtocolMajor", sizeof(".ZWProtocolMajor")-1, 1, Integer, &iot_hwdev_details_zwave::proto_major, NULL},
	{".ZWProtocolMinor", sizeof(".ZWProtocolMinor")-1, 1, Integer, &iot_hwdev_details_zwave::proto_minor, NULL},
	{".applicationMajor", sizeof(".applicationMajor")-1, 1, Integer, &iot_hwdev_details_zwave::app_major, NULL},
	{".applicationMinor", sizeof(".applicationMinor")-1, 1, Integer, &iot_hwdev_details_zwave::app_minor, NULL},
	{".basicType", sizeof(".basicType")-1, 1, Integer, &iot_hwdev_details_zwave::basic_type, NULL}
};

static struct inst_prop_rules {
	const char* name;
	uint8_t namelen;
	uint8_t size; //1 if byte_ptr is valid, 2 if word_ptr is valid
	ZWDataType type; //required type of dataholder
	uint8_t iot_hwdev_details_zwave::inst_prop_t::* byte_ptr;
	uint16_t iot_hwdev_details_zwave::inst_prop_t::* word_ptr;
} inst_props_need[]={
	{".genericType", sizeof(".genericType")-1, 1, Integer, &iot_hwdev_details_zwave::inst_prop_t::generic_type, NULL},
	{".specificType", sizeof(".specificType")-1, 1, Integer, &iot_hwdev_details_zwave::inst_prop_t::specific_type, NULL}
};


template<class RuleClass, class DestClass> void process_zway_props(RuleClass* rules, unsigned num_rules, DestClass* dst, ZDataIterator child, const char *path, unsigned pathlen, const char *ctrl_name) {
	for(unsigned i=0;i<num_rules;i++) { //compare path with each name
		if(pathlen<=rules[i].namelen || memcmp(path + pathlen - rules[i].namelen, rules[i].name, rules[i].namelen)!=0) continue;
		//name matches
		ZWDataType type;
		zdata_get_type(child->data, &type);
		if(type!=rules[i].type) {
			kapi_outlog_notice("Type of data field '%s' is %d, not matching type %d, controller '%s'", path, int(type), int(rules[i].type), ctrl_name);
			continue;
		}
		int int_val;
		switch(rules[i].size) {
			case 1: //uint8_t
				zdata_get_integer(child->data, &int_val);
				if(int_val<0 || int_val>255) kapi_outlog_notice("Data field '%s' exceede uint8 range with value %d, controller '%s'", path, int_val, ctrl_name);
				dst->*(rules[i].byte_ptr)=uint8_t(int_val);
				break;
			case 2: //uint16_t
				zdata_get_integer(child->data, &int_val);
				if(int_val<0 || int_val>65535) kapi_outlog_notice("Data field '%s' exceede uint16 range with value %d, controller '%s'", path, int_val, ctrl_name);
				dst->*(rules[i].word_ptr)=uint16_t(int_val);
				break;
			default:
				assert(false);
		}
	}
}




class zwave_ctrl_details_serial_subclass : public iot_hwdev_details_serial_subclass {
public:
	static const zwave_ctrl_details_serial_subclass object;
};

const zwave_ctrl_details_serial_subclass zwave_ctrl_details_serial_subclass::object;

class zw_device_props;

class zwave_ctrl_handle : public iot_objectrefable {
	friend class zw_device_props;
	ZWay zway=NULL;
	ZWLog logger=NULL;
	mutable uv_mutex_t hlock;
	mutable uv_mutex_t CBhlock; //lock to control running of callbacks. every callback obtains this lock at start and releases at end
	zw_device_props *devices_head=NULL; //this list keeps one reference to each device using explicit ref()
	mutable uv_thread_t lockowner=0;
	mutable uv_thread_t CBlockowner=0;
	int portfd=-1;
	iot_objref_ptr<iot_device_driver_base> ctrldriver_inst; //set to driver instance if driver_attached is true
	void (*ctrldriverDevCallback)(iot_device_driver_base*)=NULL; //keeps driver's callback after device polling is started
	uint32_t homeid=0;
	bool devices_polling_on=false; //shows if callback for tracking changes in devices list or their instances/cclases is enabled
	volatile uint8_t cancel_callback=0;//bitfield showing if particular callback must exit immediately after obtaining callbacklock: bit 0 - termination callback, bit 1 - device updates callback
	volatile uint8_t fatalerror=0;
	volatile uint8_t deinit_pending=0;
	volatile uint8_t ctrldriver_attached=0; //flag that driver thread has control over handle
	volatile std::atomic<int32_t> device_access_inprogress=0; //this value when >0 prevents deinit() even when ctrldriver_attached becomes false
public:
	volatile uint8_t ctrl_data_valid=0; //ctrl_data valid
	iot_hwdev_details_serial ctrl_data;
	uint8_t ctrl_nodeid=0; //node (device) id of controller own representation in reported devices

	zwave_ctrl_handle(object_destroysub_t destroy_sub = object_destroysub_delete) : iot_objectrefable(destroy_sub, true), ctrl_data(NULL) {
		uv_mutex_init(&hlock);
		uv_mutex_init(&CBhlock);
	}
	~zwave_ctrl_handle(void) {
		assert(!ctrldriver_attached); //destructor must not run until driver holds reference
		deinit();
		assert(zway==NULL && portfd==-1);
		assert(devices_head==NULL);
		if(logger) {
			zlog_close(logger);
			logger=NULL;
		}
		uv_mutex_destroy(&hlock);
		uv_mutex_destroy(&CBhlock);
	}
	////////////////////////////////////////////////
	///////////iot_zwave_device_handle interface:
	////////////////////////////////////////////////

	void lock(void) const {
		if(lockowner==uv_thread_self()) { //must not have recursion?
			assert(false);
			return;
		}
		uv_mutex_lock(&hlock);
		lockowner=uv_thread_self();
	}
	void unlock(void) const {
		if(lockowner!=uv_thread_self()) {
			assert(false);
			return;
		}
		lockowner=0;
		uv_mutex_unlock(&hlock);
	}
	bool is_locked(void) const { //checks if current thread already owns the lock
		return lockowner==uv_thread_self();
	}


	////////////////////////////////////////////////
	///////////end of iot_zwave_device_handle interface:
	////////////////////////////////////////////////




	uint32_t get_homeid(void) const {
		return homeid;
	}
	bool is_ctrldriver_attached(void) const {
		return ctrldriver_attached!=0;
	}

	zw_device_props* start_device_enumerate(iot_device_driver_base* drvinst) {
		if(!drvinst) return NULL;
		lock();
		if(!is_ok_int() || !ctrldriver_attached || ctrldriver_inst!=drvinst) {
			unlock();
			return NULL;
		}
		return devices_head;
	}

	void stop_device_enumerate(void) {
		unlock();
	}

	void remove_device(zw_device_props* dev); //MUST BE LOCKED!!!


	int start_device_polling(iot_device_driver_base* drvinst, void (*ctrldriverDevCallback_)(iot_device_driver_base*)); //only attached driver can enable polling

	bool is_ok(void) const {
		lock();
		bool rval = zway!=NULL && !fatalerror;
		unlock();
		return rval;
	}
	bool try_ctrldriver_attach(iot_device_driver_base* drvinst) {
		if(!drvinst || !drvinst->miid) return false;
		bool rval;
		lock();
		if(is_ok_int() && !ctrldriver_attached && ctrl_data_valid) {
			assert(uv_thread_self()==drvinst->thread);
			rval=true;
			ctrldriver_attached=1;
			ctrldriver_inst=drvinst;
		} else rval=false;
		unlock();
		return rval;
	}
	void lock_for_device_access(void) { 
		device_access_inprogress.fetch_add(1, std::memory_order_relaxed); //prevent ctrl handler from deinit while lock here is free
	}
	void unlock_for_device_access(void) { //object must be unlocked
		if(device_access_inprogress.fetch_sub(1, std::memory_order_relaxed)==1) {
			if(deinit_pending) deinit();
		}
	}

	void ctrldriver_detach(iot_device_driver_base* drvinst) {
		lock();
		if(ctrldriver_attached && drvinst && ctrldriver_inst==drvinst) {
			assert(uv_thread_self()==drvinst->thread);
			ctrldriver_attached=0;
			ctrldriver_inst=NULL;
			devices_polling_on=false;
			ctrldriverDevCallback=NULL;
		} else {
			assert(false); //some bug?
		}
		unlock();
		if(!ctrldriver_attached && deinit_pending) deinit();
	}
	bool ping(bool skiplock) { //return false if connection is bad
		bool rval;
		if(!skiplock) lock();
		if(!is_ok_int()) rval=false;
		else {
			ZWError r=zway_device_send_nop(zway, 1, NULL, NULL, NULL);
			if(r!=NoError) {
				rval=false;
				fatalerror=1;
			} else rval=true;
		}
		if(!skiplock) unlock();
		return rval;
	}

	int init(const char* portname, const char* cfgdirpath, const char *sharedatadirpath) { //must be race free!!!
	//return value:
	//positive - delay in milliseconds to repeat operation
	//nevative error code among:
	//	IOT_ERROR_INVALID_STATE

		if(zway) return IOT_ERROR_INVALID_STATE;
		char devpath[32];
		char transpath[PATH_MAX];
		char zddxpath[PATH_MAX];
		int err;
		ZWError r;

		snprintf(devpath, sizeof(devpath), "/dev/%s", portname);
		int fd=open(devpath, O_RDWR);
		if(fd<0) {
			err=errno;
			kapi_outlog_notice("Cannot open device file '%s': %s", devpath, uv_strerror(uv_translate_sys_error(err)));
			return 5*60*1000; //5 min
		}
		if(!flock(fd, LOCK_EX | LOCK_UN)) { //file is locked by driver?
			err=30*1000; //30 sec
			goto onerror;
		}
		//here fd is locked
		if(!logger) {
			logger = zlog_create(stderr, Information);
			if(!logger) {
				kapi_outlog_notice("Cannot create zway logger");
				err=2*60*1000; //2 min
				goto onerror;
			}
		}
		assert(!ctrldriver_attached);
		fatalerror=0;
		cancel_callback=0;
		ctrl_data_valid=0;

		err=snprintf(transpath, sizeof(transpath), "%s/translations", sharedatadirpath);
		err=snprintf(zddxpath, sizeof(zddxpath), "%s/ZDDX", sharedatadirpath);

		r = zway_init(&zway, ZSTR(devpath), cfgdirpath, transpath, zddxpath, NULL, logger);
		if(r!=NoError) {
			kapi_outlog_notice("Failed to init RaZberry Z-Wave controller on serial port '%s': %s", devpath, zstrerror(r));
			err=5*60*1000; //5 min
			goto onerror;
		}

		ZDeviceCallback callback;

		r=zway_device_add_callback(zway, DeviceAdded, (callback=[](const ZWay zway, ZWDeviceChangeType type, ZWBYTE node_id, ZWBYTE instance_id, ZWBYTE command_id, void *arg)->void {
			zwave_ctrl_handle* h=(zwave_ctrl_handle*)arg;
			h->onDeviceEvent_initial(type, node_id, instance_id, command_id);
		}), this); //setup callback to be called when device is recognized
		assert(r==NoError);

		r=zway_start(zway, [](ZWay zway, void *arg) -> void {
			zwave_ctrl_handle* h=(zwave_ctrl_handle*)arg;
			h->onZWayTerminated();
		}, this); //this call starts new thread. all callbacks are called in it

		if(r!=NoError) {
			kapi_outlog_notice("Failed to start managing thread of RaZberry Z-Wave controller on serial port '%s': %s", devpath, zstrerror(r));
		} else {
			//managing thread started successfully
			r=zway_discover(zway);
			if(r!=NoError) {
				kapi_outlog_notice("Failed to start discovering for RaZberry Z-Wave controller on serial port '%s': %s", devpath, zstrerror(r));
				zway_stop(zway);
			}
		}
		if(r!=NoError) {
			err=1*60*1000;
			goto onerror;
		}

		CBlock(); //get in sync with callbacks
		if(fatalerror) { //termination occured just after discovering OR error getting controller data
			cancel_callback=255;
			CBunlock();
			zway_stop(zway);
			err=1*60*1000;
			goto onerror;
		}
		CBunlock();
		ctrl_data.set_serial_port(portname);

		portfd=fd;
		return 0;

onerror:
		if(zway) {
			zway_terminate(&zway);
			zway=NULL;
		}
		if(fd>=0) close(fd);
		return err;
	}

	bool deinit(void) { //can be called by ctrl detector only, if no driver still attached OR detector is terminated
	//returns false if driver attached or zway uninited
		CBlock(); //wait for callbacks to terminate
		cancel_callback=255; //signal to all callbacks to exit immediately
		CBunlock();
		//after this point no callback can get to lock() call, so no deadlocks can rise
		lock();
		if(!zway || ctrldriver_attached || device_access_inprogress.load(std::memory_order_relaxed)>0) {
kapi_outlog_error("DEINIT DELAYED: ctrldriver_attached=%d, device_access_inprogress=%d", int(ctrldriver_attached), int(device_access_inprogress.load(std::memory_order_relaxed)));
			deinit_pending=1;
			unlock();
			return false;
		}
kapi_outlog_error("DEINITING");
		free_devices();
//		unlock();
//		//after this point all methods which get lock() must exit immediately checking is_ok()

		zway_stop(zway);
		zway_terminate(&zway);
//TODO if deadlock appears in zway_stop or zway_terminate, uncomment unlock()/lock()
//		lock(); //necessary to sync reset of deinit_pending
		zway=NULL;
		if(portfd>=0) {
			close(portfd);
			portfd=-1;
		}
		deinit_pending=0;
		unlock();
		//handle is reusable for init()
		return true;
	}


private:
	static void onDeviceEvent_static(const ZWay zway, ZWDeviceChangeType type, ZWBYTE node_id, ZWBYTE instance_id, ZWBYTE command_id, void *arg) {
		zwave_ctrl_handle* h=(zwave_ctrl_handle*)arg;
		h->onDeviceEvent(type, node_id, instance_id, command_id);
	}
	bool is_ok_int(void) const { //MUST BE LOCKED!!!
		assert(is_locked());
		return zway!=NULL && !fatalerror;
	}
	void free_devices(void); //MUST BE LOCKED!!!

	void CBlock(void) const {
		assert(CBlockowner!=uv_thread_self()); //must not have recursion?
		uv_mutex_lock(&CBhlock);
		CBlockowner=uv_thread_self();
	}
	void CBunlock(void) const {
		assert(CBlockowner==uv_thread_self());
		CBlockowner=0;
		uv_mutex_unlock(&CBhlock);
	}

	void onDeviceEvent(ZWDeviceChangeType type, ZWBYTE node_id, ZWBYTE instance_id, ZWBYTE command_id) {
		assert(zway!=NULL);
		CBlock();
		if(cancel_callback & 2) {
			CBunlock();
			return;
		}
		lock();

kapi_outlog_notice("GOT DEVICE EVENT FROM RAZBERRY: %d", int(type));
onexit:
		unlock();
		CBunlock();
	}

	void onDeviceEvent_initial(ZWDeviceChangeType type, ZWBYTE node_id, ZWBYTE instance_id, ZWBYTE command_id) {
		assert(zway!=NULL);
		CBlock();
		if(cancel_callback & 2) {
			CBunlock();
			return;
		}

		ZWError r;
		//read controller props
		if(!ctrl_data_valid) {
			int vendor, productId, productType;
			int _homeid=0;
			int _nodeid=1;
			//vendor
			ZDataHolder h=zway_find_controller_data(zway, "manufacturerId");
			if(!h) {assert(false);goto onerr;}
			r=zdata_get_integer(h, &vendor);
			if(r!=NoError || vendor<=0 || vendor>65535) goto onerr;

			//product id
			h=zway_find_controller_data(zway, "manufacturerProductId");
			if(!h) {assert(false);goto onerr;}
			r=zdata_get_integer(h, &productId);
			if(r!=NoError || productId<=0 || productId>65535) goto onerr;

			//product type
			h=zway_find_controller_data(zway, "manufacturerProductType");
			if(!h) {assert(false);goto onerr;}
			r=zdata_get_integer(h, &productType);
			if(r!=NoError || productType<0 || productType>=65535) goto onerr;

			//home id
			if((h=zway_find_controller_data(zway, "homeId"))) {
				zdata_get_integer(h, &_homeid);
				if(_homeid!=0) homeid=uint32_t(_homeid);
			}
			//node id
			if((h=zway_find_controller_data(zway, "nodeId"))) {
				zdata_get_integer(h, &_nodeid);
				if(_nodeid>0) ctrl_nodeid=uint8_t(_nodeid);
			}

			char namebuf[256];
			ZWCSTR vendor_str=NULL;
			ZWCSTR chip_str=NULL;
			ZWCSTR homename_str=NULL;
			//vendor string
			if((h=zway_find_controller_data(zway, "vendor"))) {
				zdata_get_string(h, &vendor_str);
			}
			//chip name
			if((h=zway_find_controller_data(zway, "ZWaveChip"))) {
				zdata_get_string(h, &chip_str);
			}
			//home name
			if((h=zway_find_controller_data(zway, "homeName"))) {
				zdata_get_string(h, &homename_str);
				if(homename_str && !homename_str[0]) homename_str=NULL;
			}
			if(vendor_str) {
				if(chip_str) {
					if(homename_str) snprintf(namebuf, sizeof(namebuf), "%s Z-Wave Constroller (using %s) in '%s'", vendor_str, chip_str, homename_str);
					else snprintf(namebuf, sizeof(namebuf), "%s Z-Wave Constroller using %s", vendor_str, chip_str);
				} else {
					if(homename_str) snprintf(namebuf, sizeof(namebuf), "%s Z-Wave Constroller in '%s'", vendor_str, homename_str);
					else snprintf(namebuf, sizeof(namebuf), "%s Z-Wave Constroller", vendor_str);
				}
			} else {
				if(chip_str) {
					if(homename_str) snprintf(namebuf, sizeof(namebuf), "Z-Wave Constroller (using %s) in '%s'", chip_str, homename_str);
					else snprintf(namebuf, sizeof(namebuf), "Z-Wave Constroller using %s", chip_str);
				} else {
					if(homename_str) snprintf(namebuf, sizeof(namebuf), "Z-Wave Constroller in '%s'", homename_str);
					else snprintf(namebuf, sizeof(namebuf), "Z-Wave Constroller");
				}
			}


			ctrl_data.set_serial_data(iot_hwdev_localident_serial::IDSPACE_ZWAVE, uint16_t(vendor), uint16_t(productId), uint16_t(productType), namebuf);
			ctrl_data_valid=true;
		}

		CBunlock();
		return;
onerr:
		fatalerror=1;
		CBunlock();
	}
	void onZWayTerminated(void) {
		assert(zway!=NULL);
		CBlock();
		if(cancel_callback & 1) {
			CBunlock();
			return;
		}
		fatalerror=1; //TODO some notification
		CBunlock();
	}

};



//used to pass data from handle to drivers
class zw_device_props : public iot_zwave_device_handle {
	friend class zwave_ctrl_handle;
	zw_device_props *next=NULL, *prev=NULL; //one reference to this object is hold my linked list until parent becomes NULL
	iot_objref_ptr<zwave_ctrl_handle> parent;  //holds reference to ctrl handle until is removed from it. removed device (with zero parent) rejects all requests
	void (*driverStateCallback)(iot_device_driver_base*, uint8_t instance, uint8_t cc, void *custom_arg)=NULL; //callback set by driver, custom_arg is inst-cc dependent
	iot_objref_ptr<iot_device_driver_base> driver_inst; //set to driver instance if driver_attached is true

//	uint8_t device_locked=0;
	volatile uint8_t driver_attached=0; //flag that driver thread has control over handle
public:
	iot_hwdev_localident_zwave devident; //valid if in_registry is true
	iot_hwdev_details_zwave *details=NULL; //points to memory after this structure
	enum updateflag_t: uint8_t { //flags telling which update event occured
		UPDATEFLAG_DEV_ADDED = (1<<0), //new device added
		UPDATEFLAG_DEV_REMOVED = (1<<1), //device removed (cannot be set together with UPDATEFLAG_DEVADDED)
		UPDATEFLAG_INST_UPDATED = (1<<2), //instances were updated
		UPDATEFLAG_CC_UPDATED = (1<<3), //instances were updated
	};
	uint8_t ctrldriver_pending_updates=0; //bitmask of updateflag_t flags telling which updated controller driver must process. driver resets this field by itself
	bool in_registry=false; //used by ctrldriver. true value means that device was added to dev registry with devident

	zw_device_props(object_destroysub_t destroy_sub = object_destroysub_delete) : iot_zwave_device_handle(destroy_sub) {}

	zw_device_props *get_next(void) const {
		return next;
	}
private:
	virtual int lock(void) const override {
		//use reflock to protect 'parent' pointer from races when controller driver tries to remove device (and set parent to NULL)	
		reflock.lock();
		iot_objref_ptr<zwave_ctrl_handle> p_parent=parent; //preserve parent pointer (and stop parent from dying if its still alive)
		reflock.unlock();
		if(!p_parent) { //parent already detached, so device is dead
			return IOT_ERROR_INVALID_STATE; //any holder must release its reference after such error
		}
		//here parent pointer is valid and will stay valid until clearing p_parent
		if(p_parent->is_locked()) {
			assert(false);
			return IOT_ERROR_NO_ACTION;
		}
		p_parent->lock();
		if(!parent || !p_parent->is_ok_int() || !p_parent->ctrldriver_attached) {
			p_parent->unlock();
			return IOT_ERROR_INVALID_STATE; //any holder must release its reference after such error
		}
		assert(details!=NULL);
		return 0;
	}
	virtual void unlock(void) const override {
		reflock.lock();
		iot_objref_ptr<zwave_ctrl_handle> p_parent=parent; //preserve parent pointer (and stop parent from dying if its still alive)
		reflock.unlock();
		if(!p_parent) { //parent already detached, so device is dead
			assert(false);
			return;
		}
		p_parent->unlock();
	}

	virtual bool try_device_driver_attach(iot_device_driver_base *drvinst, void (*driverStateCallback_)(iot_device_driver_base *drvinst, uint8_t instance, uint8_t cc, void *custom_arg)) override {
		if(!drvinst || !drvinst->miid) return false;
		bool rval;
		if(lock()!=0) return false;
		if(!driver_attached) {
			assert(uv_thread_self()==drvinst->thread);
			rval=true;
			driver_attached=1;
			driver_inst=drvinst;
			driverStateCallback=driverStateCallback_;
		} else rval=false;
		unlock();
		return rval;
	}
	virtual void device_driver_detach(iot_device_driver_base *drvinst) override {
		int lockerr=lock();
		if(driver_attached && drvinst && driver_inst==drvinst) {
			assert(uv_thread_self()==drvinst->thread);
			driver_attached=0;
			driver_inst=NULL;
			driverStateCallback=NULL;
		} else {
			assert(false); //some bug?
		}
		if(!lockerr) unlock();
	}

	//Multilevel Sensor Command Class
	//GET
	//Params:
	//Returns:
	//	0 - success, dataptr
	//	IOT_ERROR_NOT_FOUND - instance-CC pait is not valid for this device
	virtual int cc_multilevel_sensor_get(uint8_t instance, iot_zwave_cc_multilevel_sensor_data_t **dataptr, void* custom_arg=NULL) override {
		int err=lock();
		if(err) return err;
		bool drv_attached=driver_attached;


/*		if(device_locked) {
			unlock();
			return IOT_ERROR_NOT_READY;
		}
*/
		//check that cc is valid for instance
		for(uint16_t i=0;i<details->num_inst;i++) {
			if(details->inst_prop[i].inst!=instance) continue;
			auto *pairs=details->get_inst_cc_pairs_address();
			for(uint32_t cc=details->inst_prop[i].cc_pair_start; cc<details->num_inst_cc_pairs && pairs[cc].inst==instance; cc++) {
				if(pairs[cc].cc==49) goto cont;
			}
		}
		unlock();
		return IOT_ERROR_NOT_FOUND;
cont:
		iot_zwave_cc_data_common* cc_data=NULL;

		bool track=false;
		if(drv_attached) {
			if(uv_thread_self()!=driver_inst->thread) {
				assert(false);
				return IOT_ERROR_INVALID_THREAD;
			}
			if(driverStateCallback) {
				cc_data=find_cc_data(instance, 49);
				if(!cc_data) {
					track=true;
					cc_data=(iot_zwave_cc_data_common*)iot_allocate_memblock(sizeof(iot_zwave_cc_data_common));
					if(!cc_data) {
						unlock();
						return IOT_ERROR_NO_MEMORY;
					}
					new(cc_data) iot_zwave_cc_data_common(this, instance, 49, custom_arg);
				}
				else {
					assert(false);
					cc_data=NULL;
				}
			}
		}

		parent->lock_for_device_access(); //prevent ctrl handler from deinit while lock here is free
//		device_locked=1;
		unlock();

		ZDataHolder topdh,dh;
		err=IOT_ERROR_NOT_FOUND;

		zdata_acquire_lock(ZDataRoot(parent->zway));

		topdh=zway_find_device_instance_cc_data(parent->zway, details->node_id, instance, 49, ".");
		if(!topdh) {assert(false);goto onerror_dh;}

		dh=zdata_find(topdh, "typemask");
		if(!dh) {assert(false);goto onerror_dh;}

		ZWDataType type;
		ZWError r;
		r=zdata_get_type(dh, &type);
		if(r!=NoError) {assert(false);goto onerror_dh;}
		if(type!=Binary) {/*can still be Empty*/ assert(false);goto onerror_dh;}
		size_t len;
		const ZWBYTE *binary;
		r=zdata_get_binary(dh, &binary, &len);
		if(r!=NoError) {assert(false);goto onerror_dh;}

		len&=0x0F;
		uint8_t num_types;
		num_types=0;
		uint8_t i;
		//count set bits
		for(i=0;i<len;i++) {
			uint8_t b=binary[i];
			for(; b!=0; num_types++) { //Brian Kernighan algo to count set bits
				b&=b-1; // this clears the LSB-most set bit
			}
		}

		if(num_types==0) {
//			if(cc_data) {
//				remove_cc_data(cc_data);
//				if(cc_data->is_tracked) {
//					zdata_remove_callback_ex(topdh, on_cc_data_change_static, cc_data);
//					cc_data->is_tracked=0;
//				}
//			}
			assert(false);
			goto onerror_dh;
		}

		size_t sz;
		sz=iot_zwave_cc_multilevel_sensor_data_t::calc_size(num_types);
		iot_zwave_cc_multilevel_sensor_data_t *data;
		data=(iot_zwave_cc_multilevel_sensor_data_t*)iot_allocate_memblock(sz, true);
		if(!data) {
			err=IOT_ERROR_NO_MEMORY;
			goto onerror_dh;
		}
		memset(data, 0, sz);

		uint8_t type_id;
		type_id=1;
		for(i=0;i<len;i++) {
			uint8_t b=binary[i];
			for(uint8_t j=0;j<8 && data->num_types<num_types;j++, type_id++) {
				if(b&1) {
					data->type_data[data->num_types++].type_id=type_id;
				}
				b>>=1;
			}
		}
		assert(data->num_types==num_types);

		char typebuf[8];

		for(i=0;i<data->num_types;i++) {
			zway_cc_sensor_multilevel_get(parent->zway, details->node_id, instance, data->type_data[i].type_id, NULL, NULL, NULL); //temporary try to speed up getting actual data

			snprintf(typebuf, sizeof(typebuf), "%d", int(data->type_data[i].type_id));
			dh=zdata_find(topdh, typebuf);
			if(!dh) {
				assert(false);
				data->type_data[i].is_empty=true;
				continue;
			}
			ZDataIterator child = zdata_first_child(dh);
			while (child != NULL) {
				char *path = zdata_get_path(child->data);
				if(path) {
					size_t pathlen=strlen(path);
					int int_val;
					float float_val;

					if(pathlen>sizeof(".val")-1 && memcmp(path+pathlen-(sizeof(".val")-1), ".val", sizeof(".val")-1)==0) {
						zdata_get_type(child->data, &type);
						if(type==Integer) {
							zdata_get_integer(child->data, &int_val);
							data->type_data[i].value=int_val;
						} else if(type==Float) {
							zdata_get_float(child->data, &float_val);
							data->type_data[i].value=float_val;
						} else if(type==Empty) {
							data->type_data[i].is_empty=true;
						} else {
							assert(false);
							data->type_data[i].value=0;
						}
					}
					else if(pathlen>sizeof(".scale")-1 && memcmp(path+pathlen-(sizeof(".scale")-1), ".scale", sizeof(".scale")-1)==0) {
						zdata_get_type(child->data, &type);
						if(type==Integer) {
							zdata_get_integer(child->data, &int_val);
							if(int_val<0 || int_val>3) {
								assert(false);
								int_val=0;
							}
							data->type_data[i].scale_id=uint8_t(int_val);
						} else if(type==Empty) {
							data->type_data[i].is_empty=true;
						} else {
							assert(false);
							data->type_data[i].scale_id=0;
						}
					}
					free(path);
				}
				child = zdata_next_child(child);
			}
		}
		if(dataptr) *dataptr=data;
		else {
			iot_release_memblock(data); //???
		}
		if(track) {
			zdata_add_callback_ex(topdh, on_cc_data_change_static, TRUE, cc_data);
			add_cc_data(cc_data);
			cc_data=NULL;
		}

		zdata_release_lock(ZDataRoot(parent->zway));
		parent->unlock_for_device_access();
		return 0;

onerror_dh:
//		device_locked=0;
		zdata_release_lock(ZDataRoot(parent->zway));
		parent->unlock_for_device_access();
		if(cc_data) {
			cc_data->~iot_zwave_cc_data_common();
			iot_release_memblock(cc_data);
		}
		return err;
	}


	static void on_cc_data_change_static(const ZDataRootObject root, ZWDataChangeType type, ZDataHolder data, void *arg) {
		assert(false);
	}
};


void zwave_ctrl_handle::remove_device(zw_device_props* dev) { //MUST BE LOCKED!!!
		assert(is_locked());
		if(dev->parent!=this || !dev->prev) { //device must still be in list
			assert(false);
			return;
		}
		BILINKLIST_REMOVE(dev, next, prev);
		assert(get_refcount()>1); //current ctrl handle should not be freed here.
		dev->reflock.lock();
		dev->parent=NULL;
		dev->reflock.unlock();
		dev->unref();
	}
void zwave_ctrl_handle::free_devices(void) { //MUST BE LOCKED!!!
		assert(is_locked());
		for(zw_device_props *cur, *next=devices_head; (cur=next);) {
			next=cur->next;
			remove_device(cur);
		}
		assert(devices_head==NULL);
	}

int zwave_ctrl_handle::start_device_polling(iot_device_driver_base* drvinst, void (*ctrldriverDevCallback_)(iot_device_driver_base*)) { //only attached driver can enable polling
		if(!drvinst || !ctrldriverDevCallback_) return IOT_ERROR_INVALID_ARGS;
		iot_hwdev_details_zwave::inst_cc_pair_t *workbuf=NULL;

		CBlock(); //wait for device callback to terminate and block them
		cancel_callback|=2;
		CBunlock();
		//after this point onDeviceEvent callback can not get to lock() call, so no deadlocks can rise
		int err=0;
		lock();
		if(!is_ok_int() || !ctrldriver_attached || ctrldriver_inst!=drvinst || devices_polling_on) {
			unlock();
			return IOT_ERROR_INVALID_STATE; //driver must detach and terminate
		}
		assert(devices_head==NULL);
		unlock();

		//here no concurreny with deinit possible because ctrldriver_attached is true

		assert(uv_thread_self()==drvinst->thread);
		workbuf=(iot_hwdev_details_zwave::inst_cc_pair_t *)malloc(sizeof(iot_hwdev_details_zwave::inst_cc_pair_t)*65536); //allocate space for maximum number of instance-cc pairs per device
		if(!workbuf) return IOT_ERROR_NO_MEMORY;


		zdata_acquire_lock(ZDataRoot(zway));

		ZWDevicesList list;
		list = zway_devices_list(zway);
		if(!list) {
			err=IOT_ERROR_CRITICAL_ERROR; //driver must detach and terminate
			goto onexit;
		}
		int d;
		zw_device_props *new_devices_head;
		//loop over devices
		for(d=0, new_devices_head=NULL; list[d]; d++) {
			if(list[d]==ctrl_nodeid) continue; //skip node of controller for now

			//find device data holder
			ZDataHolder dh=zway_find_device_data(zway, list[d], ".");
			if(!dh) { //something wrong?
				assert(false);
				continue;
			}

			int num_inst=0, num_inst_cc=0;

			ZWInstancesList ilist = zway_instances_list(zway, list[d]);
			if(!ilist) {
				kapi_outlog_notice("Error requesting instances list for node %d on '%s', may be lack of memory, stopping device polling", int(list[d]), ctrl_data.name);
				break;
			}
			iot_hwdev_details_zwave::inst_prop_t inst[256];
			int i=-1, iidx=0; //0 instance always present
			//loop over instances
			do {

				//find device data holder
				ZDataHolder idh=zway_find_device_instance_data(zway, list[d], iidx, ".");
				if(!idh) { //something wrong?
					assert(false);
					if(iidx==0) break; //instance 0 cannot be absent
					continue;
				}

				inst[num_inst]={.inst=uint8_t(iidx), .generic_type=0, .specific_type=0, .cc_pair_start=uint16_t(num_inst_cc), .num_cc_pair=0};


				num_inst++;

				ZWCommandClassesList clist = zway_command_classes_list(zway, list[d], iidx);
				if(!clist) {
					kapi_outlog_notice("Error requesting command classes list for node %d:%d on '%s', may be lack of memory, stopping device polling", int(list[d]), iidx, ctrl_data.name);
					break; //one more iteration in outer loop is possible but this is ok
				}
				int num_cc=0;
				//loop over Command classes
				for(int c=0; clist[c] && num_cc<256; c++) {
					workbuf[num_inst_cc++]={uint8_t(iidx), uint8_t(clist[c])};
					num_cc++;
				}
				//
				zway_command_classes_list_free(clist);
				if(!num_cc) { //skip instances without command classes
					num_inst--;
				} else {
					inst[num_inst-1].num_cc_pair=num_cc;

					//fill instance props
					ZDataIterator child = zdata_first_child(idh);
					while (child != NULL) {
						char *path = zdata_get_path(child->data);
						if(path) {
							size_t pathlen=strlen(path);

							process_zway_props<inst_prop_rules, iot_hwdev_details_zwave::inst_prop_t>(inst_props_need, sizeof(inst_props_need)/sizeof(inst_props_need[0]), &inst[num_inst-1], child, path, pathlen, ctrl_data.name);

							free(path);
						}
						child = zdata_next_child(child);
					}

				}


				i++;
				iidx=ilist[i];
			} while(iidx>0 && iidx<=255);
			
			zway_instances_list_free(ilist);

			if(!num_inst) continue; //skip devices without instances

			size_t sz=sizeof(zw_device_props) + iot_hwdev_details_zwave::calc_size(num_inst, num_inst_cc);
			zw_device_props *dev=(zw_device_props*)iot_allocate_memblock(sz, true);
			if(!dev) {
				kapi_outlog_notice("Error allocating memory for data about node %d on '%s', stopping device polling", int(list[d]), ctrl_data.name);
				break;
			}
			new(dev) zw_device_props(object_destroysub_memblock); //will have refcount==1 matching its being in linked list
			BILINKLIST_INSERTHEAD(dev, new_devices_head, next, prev);
			dev->parent=this;

			dev->details=(iot_hwdev_details_zwave*)(dev+1);
			new(dev->details) iot_hwdev_details_zwave(homeid, list[d]);
			dev->details->set_instances_data(num_inst, inst, num_inst_cc, workbuf);

			//fill device props
			ZDataIterator child = zdata_first_child(dh);
			while (child != NULL) {
				char *path = zdata_get_path(child->data);
				if(path) {
					size_t pathlen=strlen(path);

					process_zway_props<device_prop_rules, iot_hwdev_details_zwave>(dev_props_need, sizeof(dev_props_need)/sizeof(dev_props_need[0]), dev->details, child, path, pathlen, ctrl_data.name);

					free(path);
				}
				child = zdata_next_child(child);
			}

			dev->ctrldriver_pending_updates=dev->UPDATEFLAG_DEV_ADDED | dev->UPDATEFLAG_INST_UPDATED | dev->UPDATEFLAG_CC_UPDATED;
		}
		zway_devices_list_free(list);
		free(workbuf);
		workbuf=NULL;
		if(new_devices_head) {
			devices_head=new_devices_head;
			BILINKLIST_FIXHEAD(devices_head, prev);
		}

		devices_polling_on=true;
		ctrldriverDevCallback=ctrldriverDevCallback_;
		zway_device_add_callback(zway, DeviceAdded | DeviceRemoved | InstanceAdded | InstanceRemoved | CommandAdded | CommandRemoved, onDeviceEvent_static, this);

		err=0;
onexit:
		zdata_release_lock(ZDataRoot(zway));
		if(workbuf) free(workbuf);
		return err;
	}


class zwave_ctrl_details : public iot_hwdev_details_serial {
public:
	iot_objref_ptr<zwave_ctrl_handle> handle;

	zwave_ctrl_details(void) : iot_hwdev_details_serial(&zwave_ctrl_details_serial_subclass::object) {
	}
	zwave_ctrl_details(const iot_hwdev_details_serial& op, zwave_ctrl_handle *handle) : iot_hwdev_details_serial(op, &zwave_ctrl_details_serial_subclass::object), handle(handle) {
	}
	zwave_ctrl_details(const zwave_ctrl_details& op) : iot_hwdev_details_serial(op), handle(op.handle) {
	}

	static const zwave_ctrl_details* cast(const iot_hwdev_details* data) {
		if(!data || !data->is_valid()) return NULL;
		const iot_hwdev_details_serial* base=iot_hwdev_details_serial::cast(data);
		if(!base || base->subclass!=&zwave_ctrl_details_serial_subclass::object) return NULL;
		return static_cast<const zwave_ctrl_details*>(base);
	}

	virtual size_t get_size(void) const override {
		return sizeof(*this);
	}
	virtual bool copy_to(void* buffer, size_t buffer_size) const override {
		if(buffer_size<sizeof(*this)) return false;
		if((char*)this == buffer) {
			assert(false);
			return false;
		}
		new(buffer) zwave_ctrl_details(*this);
		return true;
	}
	virtual bool operator==(const iot_hwdev_details& _op) const override {
		if(&_op==this) return true;
		const zwave_ctrl_details* op=cast(&_op); //check type
		if(!op) return false;

		if(handle!=op->handle) return false;

		return iot_hwdev_details_serial::operator==(*op);
	}

};

EXPORTSYM devifacetype_metaclass_controller devifacetype_metaclass_controller::object;
EXPORTSYM const deviface_params_controller deviface_params_controller::object;




/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////inputlinux:detector Device Detector module
/////////////////////////////////////////////////////////////////////////////////


//period of checking for available keyboards, in milliseconds
#define DETECTOR_POLL_INTERVAL 5000
//maximum  devices for detection
#define DETECTOR_MAX_DEVS 8

class detector;

class detector : public iot_device_detector_base {
	uv_timer_t timer_watcher={};
	int num_devidents=0; //number of filled items in devidents[]
	struct devdata_t {
		uint64_t recheck_after; //time from uv_now() when port must be retried
//		iot_hwdev_localident_serial ident; //valid only if dev_present is true
		iot_objref_ptr<zwave_ctrl_handle> handle;
		char serial_port[IOT_HWDEV_IDENT_SERIAL_PORTLEN+1];
		volatile enum {
			STATE_INVALID=0, //this record is free to be used for new manual device
			STATE_VALID, //record is used but serial port not found or got error accessing it OR periodic recheck initiated, so no further actions until recheck_after time
			STATE_INITPENDING, //callback about device data or termination is pending, serial port is locked
			STATE_REGISTERED //device was added to registry and thus can be taken by driver at any moment
		} state;
	} devlist[DETECTOR_MAX_DEVS]={}; //current list of monitored ports and detected devices
	char cfgdirpath[PATH_MAX];
	char sharedatadirpath[PATH_MAX];

	void on_timer(void) {
		//check manual devices
		uint64_t now=uv_now(loop);
		int err;

		for(int i=0;i<DETECTOR_MAX_DEVS;i++) {
			if(devlist[i].recheck_after>now) continue;
			switch(devlist[i].state) {
				case devdata_t::STATE_INVALID: break;
				case devdata_t::STATE_VALID:
					if(!devlist[i].handle) { //create handle
						zwave_ctrl_handle *h=(zwave_ctrl_handle*)iot_allocate_memblock(sizeof(zwave_ctrl_handle), true);
						if(!h) { //no memory?
							devlist[i].recheck_after=now+2*60*1000; //2 min
							break;
						}
						new(h) zwave_ctrl_handle(object_destroysub_memblock);
						devlist[i].handle=iot_objref_ptr<zwave_ctrl_handle>(true, h);
					}
					err=devlist[i].handle->init(devlist[i].serial_port, cfgdirpath, sharedatadirpath);
					if(err>0) {
						devlist[i].recheck_after=now+err;
						break;
					} else if(err<0) {
						kapi_outlog_error("Error initing zway handle: %s", kapi_strerror(err));
						devlist[i].handle=NULL;
						devlist[i].recheck_after=now+5*60*1000;
						break;
					}
					now=uv_now(loop);

					devlist[i].state=devdata_t::STATE_INITPENDING;
					if(!devlist[i].handle->ctrl_data_valid) { //no controller data available, wait a little
						devlist[i].recheck_after=now+15*1000; //15 sec, timeout for operation
						break;
					}
					//pass through
				case devdata_t::STATE_INITPENDING: {//timeout elapsed
					assert(devlist[i].handle!=NULL);
					if(!devlist[i].handle->is_ok() || !devlist[i].handle->ctrl_data_valid) { //still no controller data available or lost connection
						devlist[i].handle->deinit(); //just deinit to allow to use again as driver could not yet get the reference
						devlist[i].state=devdata_t::STATE_VALID;
						devlist[i].recheck_after=now+5*60*1000; //5 min
						break;
					}
					//device must be registered
//					const iot_hwdev_details_serial &data=devlist[i].handle->ctrl_data;
//					iot_hwdev_localident_serial newident(devlist[i].serial_port, data.idspace, data.vendor, data.product, data.version);
					{ //closure for temporary object
						zwave_ctrl_details details(devlist[i].handle->ctrl_data, (zwave_ctrl_handle*)devlist[i].handle);
						err=kapi_hwdev_registry_action(IOT_ACTION_ADD, NULL, &details);
					}
					if(err) {
						kapi_outlog_error("Cannot update device in registry: %s", kapi_strerror(err));
						devlist[i].handle->deinit(); //just deinit to allow to use again as driver could not yet get the reference
						devlist[i].state=devdata_t::STATE_VALID;
						devlist[i].recheck_after=now+5*60*1000; //5 min
						break;
					}
//					devlist[i].ident=newident;
					devlist[i].state=devdata_t::STATE_REGISTERED;
					devlist[i].recheck_after=now+2*60*1000; //2 min

//					if(devlist[i].device_added && !devlist[i].newident.matches_hwid(&devlist[i].ident)) { //device must be removed
//						assert(false); //TODO
//						devlist[i].device_added=false;
//					}

//					if(!devlist[i].device_added) { //new device must be added
//						err=kapi_hwdev_registry_action(IOT_ACTION_ADD, &devlist[i].newident, NULL);
//						if(err) kapi_outlog_error("Cannot update device in registry: %s", kapi_strerror(err));
//						else {
//							devlist[i].ident=devlist[i].newident;
//							devlist[i].device_added=true;
//						}
//					}
					break;
				}
				case devdata_t::STATE_REGISTERED: //do periodic recheck
					assert(devlist[i].handle!=NULL);
					if(!devlist[i].handle->is_ok()) {
						{ //closure for temporary object
							zwave_ctrl_details details(devlist[i].handle->ctrl_data, (zwave_ctrl_handle*)devlist[i].handle);
							err=kapi_hwdev_registry_action(IOT_ACTION_REMOVE, NULL, &details);
						}
						if(!devlist[i].handle->deinit()) {
							devlist[i].handle=NULL; //try to deinit to allow to use again. will fail if driver already attached
						}
						if(err) {
							kapi_outlog_error("Cannot remove device from registry: %s", kapi_strerror(err));
							//continue after error
						}
						devlist[i].state=devdata_t::STATE_VALID;
						devlist[i].recheck_after=now+1*60*1000; //1 min
						break;
					}
					devlist[i].recheck_after=now+2*60*1000; //2 min
					if(!devlist[i].handle->is_ctrldriver_attached() && !devlist[i].handle->ping(false)) { //no driver yet, so need recheck here
						devlist[i].recheck_after=0; //will fail next check for is_ok
					}
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
		assert(uv_thread_self()==thread);

		char statedirpath[PATH_MAX];
		int err=kapi_lib_rundir(statedirpath, sizeof(statedirpath), true);
		if(err<0 || err>int(sizeof(statedirpath))) {
			kapi_outlog_error("Error getting/creating lib state directory: %s", err<0 ? kapi_strerror(err) : "path buffer overrun, use shorter base directory");
			return IOT_ERROR_CRITICAL_ERROR;
		}
		err=kapi_lib_datadir(sharedatadirpath, sizeof(sharedatadirpath), "zwave_me/accessory/zway");
		if(err<0 || err>int(sizeof(sharedatadirpath))) {
			kapi_outlog_error("Error getting lib shared data directory: %s", err<0 ? kapi_strerror(err) : "path buffer overrun, use shorter base directory");
			return IOT_ERROR_CRITICAL_ERROR;
		}

		//ensure state dir has 'config' subdirectory with Defaults.xml and 'zddx' subdir
		struct stat st;
		char dstpath[PATH_MAX];
		char srcpath[PATH_MAX];
		snprintf(cfgdirpath, sizeof(cfgdirpath), "%s/config", statedirpath);
		if(mkdir(cfgdirpath, 0755)<0 && errno!=EEXIST) { //cannot create /config subdir
			kapi_outlog_error("Cannot create '%s' directory: %s", cfgdirpath, uv_strerror(uv_translate_sys_error(err)));
			return IOT_ERROR_TEMPORARY_ERROR;
		}
		//here /config subdir exists or was created
		err=snprintf(dstpath, sizeof(dstpath), "%s/Defaults.xml", cfgdirpath);
		if(stat(dstpath, &st)<0) {
			//try to create symbolic link to shared file
			err=snprintf(srcpath, sizeof(srcpath), "%s/config/Defaults.xml", sharedatadirpath);
			if(symlink(srcpath, dstpath)<0) {
				kapi_outlog_error("Cannot create '%s' as symlink to '%s': %s", dstpath, srcpath, uv_strerror(uv_translate_sys_error(err)));
				return IOT_ERROR_TEMPORARY_ERROR;
			}
		}
		//here /config/Defaults.xml exists or was created
		err=snprintf(dstpath, sizeof(dstpath), "%s/config/zddx", statedirpath);
		if(mkdir(dstpath, 0755)<0 && errno!=EEXIST) { //cannot create /config/zddx subdir
			kapi_outlog_error("Cannot create '%s' directory: %s", dstpath, uv_strerror(uv_translate_sys_error(err)));
			return IOT_ERROR_TEMPORARY_ERROR;
		}

		uv_timer_init(loop, &timer_watcher);
		timer_watcher.data=this;
		ref(); //instance is now additionally reffered in uv handle, so increase refcount to have correct closing of handle

		err=uv_timer_start(&timer_watcher, [](uv_timer_t* handle) -> void {
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

		return 0;
	}

public:
	detector(void) {
	}
	virtual ~detector(void) {
	}
	int init(json_object* devs) {
		int num=json_object_array_length(devs);
		int num_found=0; //number of filled items in serial_port[]
		for(int i=0;i<num;i++) {
			json_object* cfg=json_object_array_get_idx(devs, i);
			if(!json_object_is_type(cfg, json_type_object)) {
				kapi_outlog_error("Manual device search params item must be an object, skipping '%s'", json_object_to_json_string(cfg));
				continue;
			}
			json_object* val;
			//method of device specification will be under 'method' key. Here we have single method, so no check for it
			if(json_object_object_get_ex(cfg, "serial_port", &val) && json_object_is_type(val, json_type_string)) {
				int len=json_object_get_string_len(val);
				if(len<=0 || len>IOT_HWDEV_IDENT_SERIAL_PORTLEN) continue;
				devlist[num_found]={};
				memcpy(devlist[num_found].serial_port, json_object_get_string(val), len+1);
				devlist[num_found].state=devlist[num_found].STATE_VALID;
				num_found++;
				if(num_found>=DETECTOR_MAX_DEVS) break;
			}
		}
		if(!num_found) return IOT_ERROR_CRITICAL_ERROR;
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
		if(!manual_devices || !json_object_is_type(manual_devices, json_type_array) || !json_object_array_length(manual_devices)) {
			kapi_outlog_error("No explicit device search params provided");
			return IOT_ERROR_CRITICAL_ERROR;
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
		struct stat statbuf;
		int err=stat("/dev", &statbuf);
		if(!err) {
			if(S_ISDIR(statbuf.st_mode)) return 0; //dir
			return IOT_ERROR_NOT_SUPPORTED;
		}
		if(err==ENOMEM) return IOT_ERROR_TEMPORARY_ERROR;
		return IOT_ERROR_NOT_SUPPORTED;
	}

};

//static iot_hwdevcontype_t detector_devcontypes[]={DEVCONTYPE_CUSTOM_LINUXINPUT};


iot_detector_moduleconfig_t IOT_DETECTOR_MODULE_CONF(det)={
	.version = IOT_VERSION_COMPOSE(0,0,1),
	.cpu_loading = 3,
	.manual_devices_required = 1,
//	.num_hwdevcontypes = sizeof(detector_devcontypes)/sizeof(detector_devcontypes[0]),

	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &detector::init_instance,
	.deinit_instance = &detector::deinit_instance,
	.check_system = &detector::check_system

//	.hwdevcontypes = detector_devcontypes
};

//driver for RaZberry controller which also implements detecting for connected devices
struct ctrl_drv_instance : public iot_device_driver_base {
	zwave_ctrl_details ctrl_data;
	iot_objref_ptr<zwave_ctrl_handle> handle;
	uv_timer_t timer_watcher={}; //to count different retries
	bool started=false;

	static int init_instance(iot_device_driver_base**instance, const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data, iot_devifaces_list* devifaces, json_object *json_params) {
		const zwave_ctrl_details* details=zwave_ctrl_details::cast(dev_data);
		if(!details) return IOT_ERROR_NOT_SUPPORTED;
		if(!details->handle || !details->handle->is_ok()) return IOT_ERROR_INVALID_DEVICE_DATA;
		
		devifaces->add(&deviface_params_controller::object);
		if(!devifaces->num) return IOT_ERROR_NOT_SUPPORTED;

		ctrl_drv_instance *inst=new ctrl_drv_instance(details);
		if(!inst) return IOT_ERROR_TEMPORARY_ERROR;

		*instance=inst;

		char buf[256];
		kapi_outlog_info("Driver inited for device with name='%s', contype=%s", details->name, dev_ident->local->sprint(buf,sizeof(buf)));
		return 0;
	}

	static int deinit_instance(iot_device_driver_base* instance) {
		ctrl_drv_instance *inst=static_cast<ctrl_drv_instance*>(instance);
		inst->unref();
		return 0;
	}

	static int check_device(const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data) {
		//SUPPORTED DEVICE IS SPECIFIED IN DEVICE FILTER!!
		//HERE JUST CHECK THAT HANDLE IS VALID
		const zwave_ctrl_details* details=zwave_ctrl_details::cast(dev_data);
		if(!details) return IOT_ERROR_NOT_SUPPORTED;
		if(!details->handle || !details->handle->is_ok()) return IOT_ERROR_INVALID_DEVICE_DATA;
		return 0;
	}
/////////////public methods

private:
	ctrl_drv_instance(const zwave_ctrl_details* details) : ctrl_data(*details), handle(details->handle) {
		ctrl_data.handle=NULL; //release unnecessary reference
	}


	//Called to start work of previously inited instance.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, iot_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(void) {
		if(!handle->try_ctrldriver_attach(this)) return IOT_ERROR_CRITICAL_ERROR; //device is unusable

		kapi_outlog_info("Driver attached to '%s'",ctrl_data.name);

//		uv_timer_init(loop, &timer_watcher);
//		timer_watcher.data=this;
//		ref(); //instance is now additionally reffered in uv handle, so increase refcount to have correct closing of handle

		int err=handle->start_device_polling(this, [](iot_device_driver_base* obj)->void {
			ctrl_drv_instance* inst=static_cast<ctrl_drv_instance*>(obj);
			inst->onDevicesUpdated();
		});
		if(err) {
			assert(false);
			handle->ctrldriver_detach(this);
			return IOT_ERROR_CRITICAL_ERROR;
		}
		//TODO
		err=check_device_list();
		if(err) {
			assert(false);
			handle->ctrldriver_detach(this);
			return err;
		}

		started=true;
		return 0;
	}
	void onDevicesUpdated(void) {
		//TODO send async signal to call check_device_list
	}

	int check_device_list(void) { //checks devices for pending updates
		zw_device_props* dev=handle->start_device_enumerate(this), *nextdev;
		if(!dev) return 0;
		int err;

//		iot_hwdev_localident_zwave ident;

		for(; dev; dev=nextdev) {
			nextdev=dev->get_next();
			if(!dev->ctrldriver_pending_updates) continue;
			if(dev->ctrldriver_pending_updates & dev->UPDATEFLAG_DEV_REMOVED) {
				if(dev->in_registry) {
					err=kapi_hwdev_registry_action(IOT_ACTION_REMOVE, &dev->devident, NULL);
					if(err) {
						assert(false);
						kapi_outlog_error("Cannot remove device from registry: %s", kapi_strerror(err));
					}
				}
				handle->remove_device(dev);
				continue;
			}
			if(dev->ctrldriver_pending_updates & dev->UPDATEFLAG_DEV_ADDED) {
				assert(!dev->in_registry);

				if(!dev->details->fill_localident(&dev->devident, sizeof(dev->devident), NULL)) { //can fail if not enough data, and need to wait for instance update
					continue;
				}
				dev->ctrldriver_pending_updates=0;
				dev->details->handle=dev;
				err=kapi_hwdev_registry_action(IOT_ACTION_ADD, &dev->devident, dev->details);
				dev->details->handle=NULL;
				if(err) {
					kapi_outlog_error("Cannot update device in registry: %s", kapi_strerror(err));
				} else {
					dev->in_registry=true;
				}
				continue;
			}
			if(!dev->in_registry) continue;
			if(dev->ctrldriver_pending_updates & dev->UPDATEFLAG_INST_UPDATED) { //instance add/remove can change device ident, so must check for this
				iot_hwdev_localident_zwave newident;
				dev->ctrldriver_pending_updates &= ~dev->UPDATEFLAG_INST_UPDATED;
				if(!dev->details->fill_localident(&newident, sizeof(newident), NULL) || !newident.matches(&dev->devident)) {
					//ident became invalid or just changed. re-add device
					err=kapi_hwdev_registry_action(IOT_ACTION_REMOVE, &dev->devident, NULL);
					if(err) {
						assert(false);
						kapi_outlog_error("Cannot remove device from registry: %s", kapi_strerror(err));
						continue;
					}
					dev->in_registry=false;
					dev->devident=newident;
					err=kapi_hwdev_registry_action(IOT_ACTION_ADD, &dev->devident, dev->details);
					if(err) {
						kapi_outlog_error("Cannot update device in registry: %s", kapi_strerror(err));
					} else {
						dev->in_registry=true;
					}
				}
				continue;
			}
			if(dev->ctrldriver_pending_updates & dev->UPDATEFLAG_CC_UPDATED) { //instance add/remove can change device ident, so must check for this
				//TODO this change can influence decision of drivers whether they can use the device. May be always re-add the device
				iot_hwdev_localident_zwave newident;
				dev->ctrldriver_pending_updates &= ~dev->UPDATEFLAG_CC_UPDATED;
				auto action = !dev->details->fill_localident(&newident, sizeof(newident), NULL) || !newident.matches(&dev->devident) ? IOT_ACTION_REMOVE : IOT_ACTION_TRYREMOVE;
				//do hard remove if ident became invalid or just changed. use soft remove if not
				err=kapi_hwdev_registry_action(action, &dev->devident, NULL);
				if(err) {
					if(err==IOT_ERROR_ACTION_CANCELLED) continue; //soft remove failed because device is already used by some driver
					assert(false);
					kapi_outlog_error("Cannot remove device from registry: %s", kapi_strerror(err));
					continue;
				}
				//remove succeeded
				dev->in_registry=false;
				dev->devident=newident;
				err=kapi_hwdev_registry_action(IOT_ACTION_ADD, &dev->devident, dev->details);
				if(err) {
					kapi_outlog_error("Cannot update device in registry: %s", kapi_strerror(err));
				} else {
					dev->in_registry=true;
				}
				continue;
			}
		}

onexit:
		handle->stop_device_enumerate();
		return err;
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

		handle->ctrldriver_detach(this);

//		uv_close((uv_handle_t*)&timer_watcher, kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'
		return 0;
	}

//iot_device_driver_base methods
	virtual int device_open(const iot_conn_drvview* conn) {
//		kapi_notify_write_avail(conn, true);
//		const iot_devifacetype_metaclass* ifacetype=conn->deviface->get_metaclass();
//		if(ifacetype==&iot_devifacetype_metaclass_keyboard::object) {
//			if(!have_kbd) return IOT_ERROR_NOT_SUPPORTED;
//			if(conn_kbd) return IOT_ERROR_LIMIT_REACHED;
//			conn_kbd=conn;

//			iot_deviface__keyboard_DRV iface(conn);
//			int err=iface.send_set_state(keys_state);
//			assert(err==0);
//		} else {
			return IOT_ERROR_NOT_SUPPORTED;
//		}
//		return 0;
	}
	virtual int device_close(const iot_conn_drvview* conn) {
//		const iot_devifacetype_metaclass* ifacetype=conn->deviface->get_metaclass();
//		if(ifacetype==&iot_devifacetype_metaclass_keyboard::object) {
//			if(conn==conn_kbd) conn_kbd=NULL;
		return 0;
	}
	virtual int device_action(const iot_conn_drvview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) {
//		int err;
//		if(action_code==IOT_DEVCONN_ACTION_CANWRITE) {
//		} else 
		if(action_code==IOT_DEVCONN_ACTION_FULLREQUEST) {
//			if(conn==conn_kbd) {
//				iot_deviface__keyboard_DRV iface(conn);
//				const iot_deviface__keyboard_DRV::msg* msg=iface.parse_req(data, data_size);
//				if(!msg) return IOT_ERROR_MESSAGE_IGNORED;
//
//				if(msg->req_code==iface.REQ_GET_STATE) {
//					err=iface.send_set_state(keys_state);
//					assert(err==0);
//					return 0;
//				}
//				return IOT_ERROR_MESSAGE_IGNORED;
		}
		kapi_outlog_info("Device action in driver inst %u, act code %u, datasize %u from device index %d", miid.iid, unsigned(action_code), data_size, int(conn->index));
		return 0;
	}

	static void on_timer_static(uv_timer_t* handle) {
		ctrl_drv_instance* obj=static_cast<ctrl_drv_instance*>(handle->data);
		obj->on_timer();
	}
	void on_timer(void) {
	}
};

static const iot_hwdev_localident_serial devfilter1(NULL, 1,{{1|2|4, iot_hwdev_localident_serial::IDSPACE_ZWAVE,0x0147,0x0001,0x0400}});
static const iot_hwdev_localident* driver_devidents[]={
	&devfilter1
};

static const iot_devifacetype_metaclass* driver_ifaces[]={
//	&iot_devifacetype_metaclass_keyboard::object,
};


iot_driver_moduleconfig_t IOT_DRIVER_MODULE_CONF(ctrldrv)={
	.version = IOT_VERSION_COMPOSE(0,0,1),

	.cpu_loading = 0,
	.num_hwdev_idents = sizeof(driver_devidents)/sizeof(driver_devidents[0]),
	.num_dev_ifaces = sizeof(driver_ifaces)/sizeof(driver_ifaces[0]),
	.is_detector = 1,

	.hwdev_idents = driver_devidents,
	.dev_ifaces = driver_ifaces,

	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &ctrl_drv_instance::init_instance,
	.deinit_instance = &ctrl_drv_instance::deinit_instance,
	.check_device = &ctrl_drv_instance::check_device
};



//driver for Z-Wave device
struct dev_drv_instance : public iot_device_driver_base {
	iot_hwdev_details_zwave *dev_data=NULL;
	iot_objref_ptr<iot_zwave_device_handle> handle;
	uv_timer_t timer_watcher={}; //to count different retries
	bool started=false;
	uint8_t num_devifaces;
	struct deviface_cfg_t {
		iot_deviface_params_buffered devifacebuf;
		iot_type_id_t ifacetype_id;
		const iot_conn_drvview *conn;
		union {
			struct {
				iot_type_id_t notion_id;
				double values[8];
				struct {
					uint8_t inst; //instance
					uint8_t cc; //command class
					uint8_t subtype; //type_id is applicable to CC
				} value_src[8];
				uint8_t num_values;
				uint8_t pending_update;
			} meter;
		} u;
		deviface_cfg_t(void):conn(NULL) {}
	} deviface_cfg[]; //array of num_devifaces items
	

	static int init_instance(iot_device_driver_base**instance, const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data, iot_devifaces_list* devifaces, json_object *json_params) {
		const iot_hwdev_details_zwave* details=iot_hwdev_details_zwave::cast(dev_data);
		if(!details) return IOT_ERROR_NOT_SUPPORTED;
		if(!details->handle || !details->handle->is_ok()) return IOT_ERROR_INVALID_DEVICE_DATA;
		int err;

		uint8_t num_devifaces=0;
		deviface_cfg_t deviface_cfg[IOT_CONFIG_MAX_IFACES_PER_DEVICE];

		auto *inst_cc_pairs=details->get_inst_cc_pairs_address();
		for(uint32_t icc=0;icc<details->num_inst_cc_pairs;icc++) {
			if(inst_cc_pairs[icc].cc==49) {
				iot_zwave_cc_multilevel_sensor_data_t *dataptr=NULL;
				err=details->handle->cc_multilevel_sensor_get(inst_cc_pairs[icc].inst, &dataptr);
				if(err) return IOT_ERROR_TEMPORARY_ERROR;

				for(uint16_t i=0;i<dataptr->num_types;i++) {
					if(dataptr->type_data[i].is_empty) {
						iot_release_memblock(dataptr);
						return IOT_ERROR_TEMPORARY_ERROR;
					}
					uint8_t type_id=dataptr->type_data[i].type_id;
					iot_type_id_t notion_id=0;
					if(type_id<iot_zwave_cc_multilevel_sensor_data_t::scale2notion_size) {
						auto notion=iot_zwave_cc_multilevel_sensor_data_t::scale2notion[type_id][dataptr->type_data[i].scale_id];
						if(notion) notion_id=notion->get_id();
					}

					//find suitable deviface_cfg
					uint8_t j;
					for(j=0;j<num_devifaces;j++)
						if(deviface_cfg[j].ifacetype_id==iot_devifacetype_metaclass_meter::object.get_id() && deviface_cfg[j].u.meter.notion_id==notion_id) break;
					if(j==num_devifaces) {
						if(j==IOT_CONFIG_MAX_IFACES_PER_DEVICE) continue; //cannot add more devifaces for device
						deviface_cfg[j].ifacetype_id=iot_devifacetype_metaclass_meter::object.get_id();
						deviface_cfg[j].u.meter.notion_id=notion_id;
						deviface_cfg[j].u.meter.num_values=0;

						num_devifaces++;
					}
					if(j<num_devifaces) {
						if(deviface_cfg[j].u.meter.num_values>=8) continue; //cannot add more subvalues to deviface params
						deviface_cfg[j].u.meter.values[deviface_cfg[j].u.meter.num_values]=dataptr->type_data[i].value;
						deviface_cfg[j].u.meter.value_src[deviface_cfg[j].u.meter.num_values]={.inst=inst_cc_pairs[icc].inst, .cc=inst_cc_pairs[icc].cc, .subtype=type_id};
						deviface_cfg[j].u.meter.pending_update=0;
						deviface_cfg[j].u.meter.num_values++;

						new(deviface_cfg[j].devifacebuf.databuf) iot_deviface_params_meter(notion_id, deviface_cfg[j].u.meter.num_values);
						deviface_cfg[j].devifacebuf.data=(iot_deviface_params*)(&deviface_cfg[j].devifacebuf.databuf);
					}
				}
				iot_release_memblock(dataptr);
			}
		}
		for(uint8_t j=0;j<num_devifaces;j++) {
			devifaces->add(deviface_cfg[j].devifacebuf.data);
		}

		if(!devifaces->num) return IOT_ERROR_NOT_SUPPORTED;

		iot_hwdev_details_zwave* detailsbuf=(iot_hwdev_details_zwave*)iot_allocate_memblock(details->get_size(), true);
		if(!detailsbuf) return IOT_ERROR_TEMPORARY_ERROR;

		bool res=details->copy_to(detailsbuf, details->get_size());
		if(!res) {
			iot_release_memblock(detailsbuf);
			return IOT_ERROR_CRITICAL_BUG;
		}

		uint32_t sz=sizeof(dev_drv_instance)+sizeof(deviface_cfg_t)*num_devifaces;
		dev_drv_instance *inst=(dev_drv_instance *)iot_allocate_memblock(sz, true);
		if(!inst) {
			iot_release_memblock(detailsbuf);
			return IOT_ERROR_TEMPORARY_ERROR;
		}
		new(inst) dev_drv_instance(detailsbuf, num_devifaces, deviface_cfg);

		*instance=inst;

		char descr[256]="";
		char buf[128];
		int off=0;
		for(unsigned i=0;i<devifaces->num;i++) {
			const iot_deviface_params *iface=devifaces->items[i].data;
			if(!iface || !iface->is_valid()) {
//				assert(false);
				continue;
			}
			off+=snprintf(descr+off, sizeof(descr)-off, "%s%s", i==0 ? "" : ", ", iface->sprint(buf,sizeof(buf)));
			if(off>=int(sizeof(descr))) break;
		}
		kapi_outlog_info("Driver inited for device with name='%s', contype=%s, devifaces='%s'", details->name, dev_ident->local->sprint(buf,sizeof(buf)), descr);
		return 0;
	}

	static int deinit_instance(iot_device_driver_base* instance) {
		dev_drv_instance *inst=static_cast<dev_drv_instance*>(instance);
		inst->unref();
		return 0;
	}

	static int check_device(const iot_hwdev_ident* dev_ident, const iot_hwdev_details* dev_data) {
		const iot_hwdev_details_zwave* details=iot_hwdev_details_zwave::cast(dev_data);
		if(!details) return IOT_ERROR_NOT_SUPPORTED;
		if(!details->handle || !details->handle->is_ok()) {
			assert(false);
			return IOT_ERROR_INVALID_DEVICE_DATA;
		}
		return 0;
	}
/////////////public methods

private:
	dev_drv_instance(iot_hwdev_details_zwave* details, uint8_t num_devifaces, deviface_cfg_t *deviface_cfg_)
			: iot_device_driver_base(object_destroysub_memblock), dev_data(details), handle(details->handle), num_devifaces(num_devifaces) {
		assert(dev_data!=NULL);
		dev_data->handle=NULL; //release unnecessary reference
		for(uint8_t i=0;i<num_devifaces;i++)
			deviface_cfg[i]=deviface_cfg_[i];
	}
	~dev_drv_instance(void) {
		if(dev_data) iot_release_memblock(dev_data);
	}

	//Called to start work of previously inited instance.
	//Return values:
	//0 - driver successfully started. It could start with temporary error state and have own retry strategy.
	//IOT_ERROR_CRITICAL_BUG - critical bug in module, so it must be blocked till program restart. this instance will be deinited.
	//IOT_ERROR_CRITICAL_ERROR - non-recoverable error. may be error in configuration. instanciation for specific entity (device for driver, iot_id for others) will be blocked
	//IOT_ERROR_TEMPORARY_ERROR - module should be retried later
	//other errors equivalent to IOT_ERROR_CRITICAL_BUG!!!
	virtual int start(void) {
		if(!handle->try_device_driver_attach(this, NULL/*onStateUpdated_static*/)) return IOT_ERROR_CRITICAL_ERROR; //device is unusable

		uv_timer_init(loop, &timer_watcher);
		timer_watcher.data=this;
		ref(); //instance is now additionally reffered in uv handle, so increase refcount to have correct closing of handle

		int err=uv_timer_start(&timer_watcher, [](uv_timer_t* handle) -> void {
			dev_drv_instance* obj=(dev_drv_instance*)(handle->data);
			obj->on_timer();
		}, 0, 5000);
		if(err<0) {
			uv_close((uv_handle_t*)&timer_watcher, kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'
			kapi_outlog_error("Cannot start timer: %s", uv_strerror(err));
			handle->device_driver_detach(this);
			return IOT_ERROR_TEMPORARY_ERROR;
		}


		//TODO
		err=check_props();
		if(err) {
			assert(false);
			handle->device_driver_detach(this);
			return IOT_ERROR_CRITICAL_ERROR;
		}

		started=true;
		return 0;
	}
	static void onStateUpdated_static(iot_device_driver_base *drvinst, uint8_t instance, uint8_t cc, void *custom_arg) {
		dev_drv_instance* obj=static_cast<dev_drv_instance*>(drvinst);
		if(!obj->miid) return;
		obj->onStateUpdated(instance, cc, custom_arg);
	}
	void onStateUpdated(uint8_t instance, uint8_t cc, void *custom_arg) {
		//TODO send async signal to call check_device_list
	}
	int check_props(void) {
		int err;

		auto *inst_cc_pairs=dev_data->get_inst_cc_pairs_address();
		for(uint32_t icc=0;icc<dev_data->num_inst_cc_pairs;icc++) {
			if(inst_cc_pairs[icc].cc==49) {
				iot_zwave_cc_multilevel_sensor_data_t *dataptr=NULL;
				err=handle->cc_multilevel_sensor_get(inst_cc_pairs[icc].inst, &dataptr);
				if(err) return err;

				for(uint16_t i=0;i<dataptr->num_types;i++) {
					if(dataptr->type_data[i].is_empty) {
						iot_release_memblock(dataptr);
						return IOT_ERROR_CRITICAL_ERROR;
					}
					uint8_t type_id=dataptr->type_data[i].type_id;
					iot_type_id_t notion_id=0;
					if(type_id<iot_zwave_cc_multilevel_sensor_data_t::scale2notion_size) {
						auto notion=iot_zwave_cc_multilevel_sensor_data_t::scale2notion[type_id][dataptr->type_data[i].scale_id];
						if(notion) notion_id=notion->get_id();
					}

					//find suitable deviface_cfg
					uint8_t j;
					for(j=0;j<num_devifaces;j++)
						if(deviface_cfg[j].ifacetype_id==iot_devifacetype_metaclass_meter::object.get_id() && deviface_cfg[j].u.meter.notion_id==notion_id) break;
					if(j==num_devifaces) continue;
					for(uint8_t v=0;v<deviface_cfg[j].u.meter.num_values; v++) {
						auto vdata=&deviface_cfg[j].u.meter.value_src[v];
						if(vdata->inst==inst_cc_pairs[icc].inst && vdata->cc==inst_cc_pairs[icc].cc && vdata->subtype==type_id) {
kapi_outlog_error("GOT NEW VALUE %g, prev=%g", dataptr->type_data[i].value, deviface_cfg[j].u.meter.values[v]);
							if(deviface_cfg[j].u.meter.values[v]!=dataptr->type_data[i].value) {
								deviface_cfg[j].u.meter.values[v]=dataptr->type_data[i].value;
								deviface_cfg[j].u.meter.pending_update=1;
							}
							break;
						}
					}
				}
				iot_release_memblock(dataptr);
			}
		}
		//send pending events
		uint8_t j;
		for(j=0;j<num_devifaces;j++) {
			if(deviface_cfg[j].ifacetype_id==iot_devifacetype_metaclass_meter::object.get_id() && deviface_cfg[j].u.meter.pending_update) {
				if(deviface_cfg[j].conn) {
					iot_deviface__meter_DRV iface(deviface_cfg[j].conn);
					err=iface.send_set_state(deviface_cfg[j].u.meter.values);
					assert(err==0);
				}
			}
		}
		return 0;
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

		handle->device_driver_detach(this);

		uv_close((uv_handle_t*)&timer_watcher, kapi_process_uv_close); //kapi_process_uv_close requires 'data' field of handle assigned to 'this'
		return 0;
	}

//iot_device_driver_base methods
	virtual int device_open(const iot_conn_drvview* conn) {
		uint8_t j;
		for(j=0;j<num_devifaces;j++)
			if(deviface_cfg[j].devifacebuf.data->matches(conn->deviface)) break;
		if(j>=num_devifaces) return IOT_ERROR_NOT_SUPPORTED;
		if(deviface_cfg[j].conn) return IOT_ERROR_LIMIT_REACHED;

		deviface_cfg[j].conn=conn;
//		kapi_notify_write_avail(conn, true);

		if(deviface_cfg[j].ifacetype_id==iot_devifacetype_metaclass_meter::object.get_id()) {
			iot_deviface__meter_DRV iface(conn);
			int err=iface.send_set_state(deviface_cfg[j].u.meter.values);
			assert(err==0);
			deviface_cfg[j].u.meter.pending_update=0;
		}
		return 0;
	}
	virtual int device_close(const iot_conn_drvview* conn) {
		uint8_t j;
		for(j=0;j<num_devifaces;j++)
			if(deviface_cfg[j].devifacebuf.data->matches(conn->deviface)) break;
		if(j>=num_devifaces) return 0;
		if(conn==deviface_cfg[j].conn) deviface_cfg[j].conn=NULL;
		return 0;
	}
	virtual int device_action(const iot_conn_drvview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) {
//		int err;
//		if(action_code==IOT_DEVCONN_ACTION_CANWRITE) {
//		} else 
		if(action_code==IOT_DEVCONN_ACTION_FULLREQUEST) {
//			if(conn==conn_kbd) {
//				iot_deviface__keyboard_DRV iface(conn);
//				const iot_deviface__keyboard_DRV::msg* msg=iface.parse_req(data, data_size);
//				if(!msg) return IOT_ERROR_MESSAGE_IGNORED;
//
//				if(msg->req_code==iface.REQ_GET_STATE) {
//					err=iface.send_set_state(keys_state);
//					assert(err==0);
//					return 0;
//				}
//				return IOT_ERROR_MESSAGE_IGNORED;
		}
		kapi_outlog_info("Device action in driver inst %u, act code %u, datasize %u from device index %d", miid.iid, unsigned(action_code), data_size, int(conn->index));
		return 0;
	}

	static void on_timer_static(uv_timer_t* handle) {
		dev_drv_instance* obj=static_cast<dev_drv_instance*>(handle->data);
		obj->on_timer();
	}
	void on_timer(void) {
		check_props();
	}
};

static const iot_hwdev_localident_zwave zdevfilter(0, {});
static const iot_hwdev_localident* devdriver_devidents[]={
	&zdevfilter
};

static const iot_devifacetype_metaclass* devdriver_ifaces[]={
//	&iot_devifacetype_metaclass_keyboard::object,
};


iot_driver_moduleconfig_t IOT_DRIVER_MODULE_CONF(devdrv)={
	.version = IOT_VERSION_COMPOSE(0,0,1),

	.cpu_loading = 0,
	.num_hwdev_idents = sizeof(devdriver_devidents)/sizeof(devdriver_devidents[0]),
	.num_dev_ifaces = sizeof(devdriver_ifaces)/sizeof(devdriver_ifaces[0]),
	.is_detector = 1,

	.hwdev_idents = devdriver_devidents,
	.dev_ifaces = devdriver_ifaces,

	.init_module = NULL,
	.deinit_module = NULL,
	.init_instance = &dev_drv_instance::init_instance,
	.deinit_instance = &dev_drv_instance::deinit_instance,
	.check_device = &dev_drv_instance::check_device
};



class evsrc_meter : public iot_node_base {
	uint32_t node_id;
	const iot_conn_clientview *devconn=NULL;
	uint8_t num_values=0;
	double values[32];

public:
	static int init_instance(iot_node_base**instance, uint32_t node_id, json_object *json_cfg) {
		evsrc_meter *inst=new evsrc_meter(node_id);
		*instance=inst;

		return 0;
	}
	static int deinit_instance(iot_node_base* instance) {
		evsrc_meter *inst=static_cast<evsrc_meter*>(instance);
		inst->unref();
		return 0;
	}
private:
	evsrc_meter(uint32_t node_id) : node_id(node_id) {}
	virtual ~evsrc_meter(void) {}

	virtual int start(void) override {
		return 0;
	}

	virtual int stop(void) override {
		return 0;
	}

	virtual int device_attached(const iot_conn_clientview* conn) override {
		assert(devconn==NULL);

		devconn=conn;
		memset(values, 0, sizeof(values));
		iot_deviface__meter_CL iface(conn);
		num_values=iface.get_statesize();
		return 0;
	}
	virtual int device_detached(const iot_conn_clientview* conn) override {
		assert(devconn!=NULL);

		devconn=NULL;
		update_outputs();
		return 0;
	}
	virtual int device_action(const iot_conn_clientview* conn, iot_devconn_action_t action_code, uint32_t data_size, const void* data) override {
		assert(devconn==conn);

//		int err;
		if(action_code==IOT_DEVCONN_ACTION_FULLREQUEST) {//new message arrived
			iot_deviface__meter_CL iface(conn);
			const iot_deviface__meter_CL::msg* msg=iface.parse_event(data, data_size);
			if(!msg) return 0;

			switch(msg->event_code) {
				case iface.EVENT_SET_STATE:
					kapi_outlog_info("GOT NEW VALUES, datasize=%u, statesize=%u", data_size, unsigned(msg->statesize));
					break;
				default:
					kapi_outlog_info("Got unknown event %d from device, node_id=%u", int(msg->event_code), node_id);
					return 0;
			}
			//update key state of device
			memcpy(values, msg->state, msg->statesize*sizeof(double));
			update_outputs();
			return 0;
		}
		kapi_outlog_info("Device action, node_id=%u, act code %u, datasize %u", node_id, unsigned(action_code), data_size);
		return 0;
	}

//own methods
	int update_outputs(void) { //set current common key state on output
		iot_datavalue_numeric v(values[0], false);
		uint8_t outn=0;
		const iot_datavalue* outv=&v;
		int err=kapi_update_outputs(NULL, 1, &outn, &outv);
		if(err) {
			kapi_outlog_error("Cannot update output value for node_id=%" IOT_PRIiotid ": %s, event lost", node_id, kapi_strerror(err));
		}
		return err;
	}
};

//keys_instance* keys_instance::instances_head=NULL;


//static const iot_deviface_params_meter meter_filter(1000 /*notion type for degcelcius*/, 1, 0);
static const iot_deviface_params_meter meter_filter(1 /*notion type for percentlum*/, 1, 0);

static const iot_deviface_params* meter_devifaces[]={
	&meter_filter
};


iot_node_moduleconfig_t IOT_NODE_MODULE_CONF(meter)={
	.version = IOT_VERSION_COMPOSE(0,0,1),
	.cpu_loading = 0,
	.num_devices = 1,
	.num_valueoutputs = 1,
	.num_valueinputs = 0,
	.num_msgoutputs = 0,
	.num_msginputs = 0,
	.is_persistent = 1,
	.is_sync = 0,

	.devcfg={
		{
			.label = "dev",
			.num_devifaces = sizeof(meter_devifaces)/sizeof(meter_devifaces[0]),
			.flag_canauto = 1,
			.flag_localonly = 1,
			.devifaces = meter_devifaces
		}
	},
	.valueoutput={
		{
			.label = "value",
			.notion = NULL,
			.dataclass = &iot_datatype_metaclass_numeric::object
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
	.init_instance = &evsrc_meter::init_instance,
	.deinit_instance = &evsrc_meter::deinit_instance

//	.get_state = [](void* instance, iot_srcstate_t* statebuf, size_t bufsize)->int{return ((keys_instance*)instance)->get_state(statebuf, bufsize);},
};

//end of kbdlinux:keys event source  module


