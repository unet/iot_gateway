#ifndef IOT_VALUENOTION_H
#define IOT_VALUENOTION_H
//declarations for built-in value types used for node inputs and outputs


//#include <stdlib.h>
//#include <stdint.h>

#ifndef DAEMON_KERNEL
////////////////////////////////////////////////////////////////////
///////////////////////////Specific declarations for external modules
////////////////////////////////////////////////////////////////////
//macro for generating ID of custom module specific class of state data
#define IOT_VALUENOTION_CUSTOM(module_id, index) (((module_id)<<8)+((index) & 0xff))

#endif


//notions for value classes
#define IOT_VALUENOTION_KEYCODE					1
#define IOT_VALUENOTION_DEGCELCIUS				2
#define IOT_VALUENOTION_DEGFAHRENHEIT			3



class iot_valuenotion_BASE {
public:
	const iot_valuenotion_id_t notion_id;
protected:
	constexpr iot_valuenotion_BASE(iot_valuenotion_id_t id) : notion_id(id) {
	}
public:
	iot_valuenotion_BASE(const iot_valuenotion_BASE&) = delete; //forbid implicit copy/move constructors for all derived classes

	bool operator==(const iot_valuenotion_BASE &op) const {
//		if(&op==this) return true; //same object
		return notion_id==op.notion_id;
	}
	bool operator!=(const iot_valuenotion_BASE &op) const {
		return !(*this==op);
	}
	virtual const char* type_name(void) const = 0; //must return short abbreviation of type name
};


class iot_valuenotion_keycode : public iot_valuenotion_BASE {
	constexpr iot_valuenotion_keycode(void) : iot_valuenotion_BASE(IOT_VALUENOTION_KEYCODE) {
	}
public:
	static const iot_valuenotion_keycode iface;

	virtual const char* type_name(void) const override {
		return "Key code";
	}
};

class iot_valuenotion_degcelcius : public iot_valuenotion_BASE {
	constexpr iot_valuenotion_degcelcius(void) : iot_valuenotion_BASE(IOT_VALUENOTION_DEGCELCIUS) {
	}
public:
	static const iot_valuenotion_degcelcius iface;

	virtual const char* type_name(void) const override {
		return "Celcius degrees";
	}
};

class iot_valuenotion_degfahrenheit : public iot_valuenotion_BASE {
	constexpr iot_valuenotion_degfahrenheit(void) : iot_valuenotion_BASE(IOT_VALUENOTION_DEGFAHRENHEIT) {
	}
public:
	static const iot_valuenotion_degfahrenheit iface;

	virtual const char* type_name(void) const override {
		return "Fahrenheit degrees";
	}
};




#endif //IOT_VALUENOTION_H