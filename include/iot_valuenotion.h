#ifndef IOT_VALUENOTION_H
#define IOT_VALUENOTION_H
//declarations for built-in value types used for node inputs and outputs


//#include <stdlib.h>
//#include <stdint.h>

//#ifndef DAEMON_CORE
////////////////////////////////////////////////////////////////////
///////////////////////////Specific declarations for external modules
////////////////////////////////////////////////////////////////////
//macro for generating ID of custom module specific class of state data
//#define IOT_VALUENOTION_CUSTOM(module_id, index) (((module_id)<<8)+((index) & 0xff))

//#endif


//notions for value classes
//#define IOT_VALUENOTION_KEYCODE					1
//#define IOT_VALUENOTION_DEGCELCIUS				2
//#define IOT_VALUENOTION_DEGFAHRENHEIT			3



class iot_valuenotion { //indeed this is metaclass, but only metaclass is necessary for notions for now
	iot_type_id_t notion_id;
public:
	const uint32_t version; //version of realization of metaclass and all its child classes
	const char *const type_name;
	const char *const parentlib;

	iot_valuenotion* next, *prev; //non-NULL prev means that class is registered and both next and prev are used. otherwise only next is used
													//for position in pending registration list

protected:
	iot_valuenotion(iot_type_id_t id, const char* type, uint32_t ver, const char* parentlib=IOT_CURLIBRARY); //id must be zero for non-builtin types. type cannot be NULL

public:
	iot_valuenotion(const iot_valuenotion&) = delete; //block copy-construtors and default assignments

	iot_type_id_t get_id(void) const {
		return notion_id;
	}
	void set_id(iot_type_id_t id) {
		if(notion_id>0 || !id) {
			assert(false);
			return;
		}
		notion_id=id;
	}

	char* get_fullname(char *buf, size_t bufsize, int *doff=NULL) const { //doff - delta offset. will be incremented on number of written chars
	//returns buf value
		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, "VALUENOTION:%s", type_name);
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
//	static const iot_valuenotion* findby_id(iot_type_id_t notion_id, bool try_load=true);
};


class iot_valuenotion_keycode : public iot_valuenotion {
	iot_valuenotion_keycode(void) : iot_valuenotion(0, "keycode", IOT_VERSION_COMPOSE(0,0,1)) {
	}
public:
	static const iot_valuenotion_keycode object;
};

class iot_valuenotion_degcelcius : public iot_valuenotion {
	iot_valuenotion_degcelcius(void) : iot_valuenotion(0, "degcelcius", IOT_VERSION_COMPOSE(0,0,1)) {
	}
public:
	static const iot_valuenotion_degcelcius object;
};

class iot_valuenotion_degfahrenheit : public iot_valuenotion {
	iot_valuenotion_degfahrenheit(void) : iot_valuenotion(0, "degfahrenheit", IOT_VERSION_COMPOSE(0,0,1)) {
	}
public:
	static const iot_valuenotion_degfahrenheit object;
};




#endif //IOT_VALUENOTION_H