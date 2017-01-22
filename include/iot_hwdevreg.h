#ifndef IOT_HWDEVREG_H
#define IOT_HWDEVREG_H
//Contains constants, methods and data structures for LOCAL hardware devices storing and searching


#include<stdint.h>
//#include<time.h>

#include<ecb.h>

typedef uint32_t iot_hwdevcontype_t;
typedef uint64_t iot_hwdevuniqueid_t;


//struct which locally identifies specific detected hw device
typedef struct {
	iot_hwdevcontype_t contype; //type of connection identification among predefined IOT_DEVCONTYPE_ constants or DeviceDetector module-specific built by IOT_DEVCONTYPE_CUSTOM macro
	uint32_t detector_module_id; //id of Device Detector module which added this device
	iot_hwdevuniqueid_t unique_refid; //in pair with devcontype gives unique id of device record among those made by detector_module_id for ability to reference (remove etc).assigned by detector module
} iot_hwdev_localident_t;

//struct which globally identifies specific detected hw device
typedef struct {
	iot_hostid_t hostid; //where device is physically connected
	iot_hwdev_localident_t dev;
} iot_hwdev_ident_t;


#define IOT_MAX_CLASSES_PER_DEVICE 4

//struct with data about hardware device
typedef struct {
	iot_hwdev_ident_t dev_ident; //identification of device
	uint32_t custom_len:24; //actual length or custom data
	char* custom_data; //dev_ident.contype dependent additional data about device, must point to at least 4-byte aligned buffer
} iot_hwdev_data_t;


//macro for generating ID of custom module specific hw device connection type
#define IOT_DEVCONTYPE_CUSTOM(module_id, index) (((module_id)<<8)+(index))

//type of device connection identification
//USB device identified by its USB ids.
#define IOT_DEVCONTYPE_USB			1

typedef struct { //represents custom data for devices with IOT_DEVCONTYPE_USB connection type
	uint8_t busid;  //optional bus id to help find device or 0 to find by vendor:product
	uint8_t connid; //optional bus connection id to help find device or 0 to find by vendor:product
	uint16_t vendor;//vendor code of device
	uint16_t product;//product code of device
} iot_hwdevcontype_usb_t;



ECB_EXTERN_C_BEG

//Used by device detector modules for managing registry of available hardware devices
//action is one of {IOT_ACTION_ADD, IOT_ACTION_REMOVE,IOT_ACTION_REPLACE}
//contype must be among those listed in .devcontypes field of detector module interface
//hostid field of ident is ignored by kernel (it always assigns current host)
//returns:
int kapi_hwdev_registry_action(enum iot_action_t action, iot_hwdev_localident_t* ident, size_t custom_len, void* custom_data);


ECB_EXTERN_C_END


//macro for generating ID of custom module specific device interface class
#define IOT_DEVIFACECLASS_CUSTOM(module_id, index) (((module_id)<<8)+(index))
//gets module ID from value obtained by IOT_DEVIFACECLASS_CUSTOM macro
#define IOT_DEVIFACECLASS_CUSTOM_MODULEID(clsid) ((clsid)>>8)


//IOT DEVCLASSID codes with type iot_deviface_classid
#define IOT_DEVCLASSID_MAP(XX) \
	XX(KEYBOARD, 1, "PC keyboard") /*set of keys representing standard keyboard (with SHIFT, CTRL and ALT keys)*/	\
	XX(KEYS, 2, "Keys") /*set of keys (or single key)*/																\
	XX(LEDS, 3, "LEDs") /*set of lamps (or single lamp) which can be turned on and off*/							\
	XX(HW_SWITCHES, 4, "Hardware switch") /*set of hardware switchers like notebook lid opening sensor*/			\
	XX(BASIC_SPEAKER, 5, "Speaker") /*basic sound source which can generate tone of given frequency during given time*/

typedef enum {
#define XX(nm, cc, _) IOT_DEVCLASSID_ ## nm = cc,
	IOT_DEVCLASSID_MAP(XX)
#undef XX
	IOT_DEVCLASSID_MAX = 255
} iot_deviface_classid_basic;


typedef struct {
	iot_deviface_classid classid;
	uint32_t d2c_maxmsgsize;
	uint32_t c2d_maxmsgsize;
} iot_devifacecls_config_t;


//enum iot_hwdevice_classid { //functional class of device which tells about its purpose, not ways of connection or protocols. One device can serve several purposes
//	IOT_HWDEVICE_CLASSID_KEYBOARD,		//device has button(s) which pressing and/or releasing can be catched (like PC keyboard, or PC's Sleep or Power button)
//	IOT_HWDEVICE_CLASSID_LAMP,			//device has lamp(s) which can be turned on/off
//	IOT_HWDEVICE_CLASSID_SOUNDSOURCE	//device can generate sounds or music
//};

//#define IOT_HWDEVICE_CLASSIDS_MAX 4

//typedef struct {
//	uint32_t num_classids:4;	//how many items are in classids. At least one must be
//		
//	enum iot_hwdevice_classid classids[IOT_HWDEVICE_CLASSIDS_MAX];
//} iot_hwdevice_t;



//typedef struct { //represents ID of device with IOT_DEVCONTYPE_SOCKET connection type
//	char conn[64]; //connection string like '[unix|tcp|upd]:[path|hostname:port|IPV4:port|IPV6:port]
//} iot_hwdevcontype_socket_t;




#endif //IOT_HWDEVREG_H
