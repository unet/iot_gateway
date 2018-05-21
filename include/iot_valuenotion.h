#ifndef IOT_VALUENOTION_H
#define IOT_VALUENOTION_H
//declarations for built-in notion goups and value notions
#include"iot_valueclasses.h"


class iot_valuenotion;

class iot_notiongroup { //indeed this is base metaclass, but only metaclass is necessary for notion groups for now
	iot_type_id_t notiongroup_id;
	const iot_valuenotion *basic_notion; //notion group CAN HAVE one basic value notion for which other value notions of group must define conversion functions
public:
	const uint32_t version; //version of realization of metaclass and all its child classes
	const char *const type_name;
	const char *const parentlib;

	iot_notiongroup* next, *prev; //non-NULL prev means that class is registered and both next and prev are used. otherwise only next is used
													//for position in pending registration list

protected:
	iot_notiongroup(iot_type_id_t id, const char* type, const iot_valuenotion *basic_notion, uint32_t ver, const char* parentlib=IOT_CURLIBRARY); //id must be zero for non-builtin types. type cannot be NULL

public:
	iot_notiongroup(const iot_notiongroup&) = delete; //block copy-construtors and default assignments

	iot_type_id_t get_id(void) const {
		return notiongroup_id;
	}
	void set_id(iot_type_id_t id) {
		if(notiongroup_id>0 || !id) {
			assert(false);
			return;
		}
		notiongroup_id=id;
	}

	char* get_fullname(char *buf, size_t bufsize, int *doff=NULL) const { //doff - delta offset. will be incremented on number of written chars
	//returns buf value
		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, "NOTIONGROUP:%s", type_name);
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
//	static const iot_notiongroup* findby_id(iot_type_id_t notiongroup_id, bool try_load=true);

	int convert_datavalue(const iot_datavalue* srcval, const iot_valuenotion* srcnotion, const iot_valuenotion* resultnotion, char* resultbuf, size_t resultbufsize, const iot_datavalue*& resultval) const;
private:
	//must undeerstand srcval and find way to convert notions. srcnotion and resultnotion are guaranted to be different
	virtual int p_convert_datavalue(const iot_datavalue* srcval, const iot_valuenotion* srcnotion, const iot_valuenotion* resultnotion, char* resultbuf, size_t resultbufsize, const iot_datavalue*& resultval) const = 0;
};


//Common functionality for notion groups which work with numeric quantities
class iot_notiongroup_numeric : public iot_notiongroup {
public:
	iot_notiongroup_numeric(iot_type_id_t id, const char* type, const iot_valuenotion *basic_notion, uint32_t ver, const char* parentlib=IOT_CURLIBRARY) : 
		iot_notiongroup(id, type, basic_notion, ver, parentlib) {
	}

private:
	virtual int p_convert_datavalue(const iot_datavalue* srcval, const iot_valuenotion* srcnotion, const iot_valuenotion* resultnotion, char* resultbuf, size_t resultbufsize, const iot_datavalue*& resultval) const {
/*		//just example for future
		const iot_datavalue_numeric *srcnum=iot_datavalue_numeric::cast(srcval);
		if(!srcnum) return IOT_ERROR_NOT_SUPPORTED;
		if(srcnotion==basic_notion) {
		}*/
		return IOT_ERROR_NOT_SUPPORTED;
	}
};


class iot_valuenotion { //indeed this is base metaclass, but only metaclass is necessary for notions for now
	iot_type_id_t notion_id;
	const iot_notiongroup *notion_group; //notion CAN HAVE generalized group describing common preperty (or physical quantity) of one or several notions. and to define conversions between different notions
public:
	const uint32_t version; //version of realization of metaclass and all its child classes
	const char *const type_name;
	const char *const parentlib;

	iot_valuenotion* next, *prev; //non-NULL prev means that class is registered and both next and prev are used. otherwise only next is used
													//for position in pending registration list

protected:
	iot_valuenotion(iot_type_id_t id, const char* type, const iot_notiongroup *notion_group, uint32_t ver, const char* parentlib=IOT_CURLIBRARY); //id must be zero for non-builtin types. type cannot be NULL

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

	const iot_notiongroup *get_group(void) const {
		return notion_group;
	}

	//source notion is current
	int convert_datavalue(const iot_datavalue* srcval, const iot_valuenotion* resultnotion, char* resultbuf, size_t resultbufsize, const iot_datavalue*& resultval) const {
		if(!srcval || !resultnotion) return IOT_ERROR_INVALID_ARGS;
		if(resultnotion==this) {
			return IOT_ERROR_NO_ACTION;
		}
		if(resultnotion->notion_group==notion_group) { //notiongroup driven conversion can be used
			return notion_group->convert_datavalue(srcval, this, resultnotion, resultbuf, resultbufsize, resultval);
		}
		//TODO call some virtual function in this class which can try to make specific conversion within different notion groups
		return IOT_ERROR_NOT_SUPPORTED;
	}
};


class iot_notiongroup_temperature : public iot_notiongroup_numeric {
	iot_notiongroup_temperature(void);
public:
	static const iot_notiongroup_temperature object;
};

class iot_valuenotion_degcelcius : public iot_valuenotion {
	iot_valuenotion_degcelcius(void) : iot_valuenotion(0, "degcelcius", &iot_notiongroup_temperature::object, IOT_VERSION_COMPOSE(1,0,0)) {
	}
public:
	static const iot_valuenotion_degcelcius object;
	int convert_to_basic(double src, double &dst) {
		dst=src;
		return 0;
	}
	int convert_from_basic(double src, double &dst) {
		dst=src;
		return 0;
	}
};

class iot_valuenotion_degfahrenheit : public iot_valuenotion {
	iot_valuenotion_degfahrenheit(void) : iot_valuenotion(0, "degfahrenheit", &iot_notiongroup_temperature::object, IOT_VERSION_COMPOSE(1,0,0)) {
	}
public:
	static const iot_valuenotion_degfahrenheit object;

	int convert_to_basic(double src, double &dst) {
		dst=(src - 32)*5/9;
		return 0;
	}
	int convert_from_basic(double src, double &dst) {
		dst=9*src/5+32;
		return 0;
	}

};



class iot_valuenotion_keycode : public iot_valuenotion {
	iot_valuenotion_keycode(void) : iot_valuenotion(0, "keycode", NULL, IOT_VERSION_COMPOSE(0,0,1)) {
	}
public:
	static const iot_valuenotion_keycode object;
};


class iot_notiongroup_luminance : public iot_notiongroup_numeric {
	iot_notiongroup_luminance(void);
public:
	static const iot_notiongroup_luminance object;
};

class iot_valuenotion_percentlum : public iot_valuenotion {
	iot_valuenotion_percentlum(void) : iot_valuenotion(0, "percentlum", &iot_notiongroup_luminance::object, IOT_VERSION_COMPOSE(1,0,0)) {
	}
public:
	static const iot_valuenotion_percentlum object;
	int convert_to_basic(double src, double &dst) {
		dst=src;
		return 0;
	}
	int convert_from_basic(double src, double &dst) {
		dst=src;
		return 0;
	}
};


#endif //IOT_VALUENOTION_H