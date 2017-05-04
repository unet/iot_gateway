#ifndef IOT_HWDEVREG_H
#define IOT_HWDEVREG_H
//Contains constants, methods and data structures for ///


class iot_devifaceclass__DRVBASE {
	const iot_devifaceclass_data* devclass;
protected:
	iot_devifaceclass__DRVBASE(const iot_devifaceclass_data* devclass) : devclass(devclass) {
	}
	int send_client_msg(const iot_connid_t &connid, iot_device_driver_base *drv_inst, const void *msg, uint32_t msgsize) {
		return kapi_connection_send_client_msg(connid, drv_inst, devclass->classid, msg, msgsize);
	}
};

class iot_devifaceclass__CLBASE {
	const iot_devifaceclass_data* devclass;
protected:
	iot_devifaceclass__CLBASE(const iot_devifaceclass_data* devclass) : devclass(devclass) {
	}
	int send_driver_msg(const iot_connid_t &connid, iot_driver_client_base *client_inst, const void *msg, uint32_t msgsize) {
		return kapi_connection_send_driver_msg(connid, client_inst, devclass->classid, msg, msgsize);
	}
};



//type of device connection identification
//USB device identified by its USB ids.
#define IOT_DEVCONTYPE_USB			1
#define IOT_DEVCONTYPE_VIRTUAL			2 //TODO

typedef struct { //represents custom data for devices with IOT_DEVCONTYPE_USB connection type
	uint8_t busid;  //optional bus id to help find device or 0 to find by vendor:product
	uint8_t connid; //optional bus connection id to help find device or 0 to find by vendor:product
	uint16_t vendor;//vendor code of device
	uint16_t product;//product code of device
} iot_hwdevcontype_usb_t;


//IOT DEVCLASSID codes with type iot_devifaceclass_id_t
#define IOT_DEVIFACECLASS_IDMAP(XX) \
	XX(KEYBOARD, 1) /*sizeof(iot_devifaceclass__keyboard_ATTR) set of keys or standard keyboard (with SHIFT, CTRL and ALT keys)*/	\
	XX(LEDS, 2) /*set of lamps (or single lamp) which can be turned on and off*/							\
	XX(HW_SWITCHES, 3) /*set of hardware switchers like notebook lid opening sensor*/			\
	XX(BASIC_SPEAKER, 4) /*basic sound source which can generate tone of given frequency during given time*/

//	XX(LEDS, 2, "LEDs", 32, 32, 0)
//	XX(HW_SWITCHES, 3, "Hardware switch", 32, 32, 0
//	XX(BASIC_SPEAKER, 4, "Speaker", 32, 32, 0)

enum iot_devifaceclass_basic_ids : uint8_t {
#define XX(nm, cc) IOT_DEVIFACECLASSID_ ## nm = cc,
	IOT_DEVIFACECLASS_IDMAP(XX)
#undef XX
	IOT_DEVIFACECLASS_IDMAX = 255
};


//typedef struct { //represents ID of device with IOT_DEVCONTYPE_SOCKET connection type
//	char conn[64]; //connection string like '[unix|tcp|upd]:[path|hostname:port|IPV4:port|IPV6:port]
//} iot_hwdevcontype_socket_t;


#endif //IOT_HWDEVREG_H
