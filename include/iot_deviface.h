#ifndef IOT_DEVIFACE_H
#define IOT_DEVIFACE_H


//////////////////////////////////////////////////////////////////////
//////////////////////////device interface API type
//////////////////////////////////////////////////////////////////////

#define IOT_DEVIFACE_PARAMS_MAXSIZE 32

class iot_deviface_params;

class iot_devifacetype_metaclass { //base abstract class for specifc device interface metaclass singleton objects
	iot_type_id_t ifacetype_id;
	uint32_t ver; //version of realization of metaclass and all its child classes
//	const char *vendor_name; //is NULL for built-in types
	const char *type_name;
	const char *parentlib;

	PACKED(
		struct serialize_base_t {
			iot_type_id_t ifacetype_id;
		}
	);
public:
	iot_devifacetype_metaclass* next, *prev; //non-NULL prev means that class is registered and both next and prev are used. otherwise only next is used
													//for position in pending registration list

protected:
	iot_devifacetype_metaclass(iot_type_id_t id, /*const char* vendor, */const char* type, uint32_t ver, const char* parentlib=IOT_CURLIBRARY); //id must be zero for non-builtin types. vendor is NULL for builtin types. type cannot be NULL

public:
	iot_devifacetype_metaclass(const iot_devifacetype_metaclass&) = delete; //block copy-construtors and default assignments

	iot_type_id_t get_id(void) const {
		return ifacetype_id;
	}
	uint32_t get_version(void) const {
		return ver;
	}
	const char* get_name(void) const {
		return type_name;
	}
	const char* get_library(void) const {
		return parentlib;
	}
//	const char* get_vendor(void) const {
//		return vendor_name;
//	}
	void set_id(iot_type_id_t id) {
		if(ifacetype_id>0 || !id) {
			assert(false);
			return;
		}
		ifacetype_id=id;
	}
	char* get_fullname(char *buf, size_t bufsize, int *doff=NULL) const { //doff - delta offset. will be incremented on number of written chars
	//returns buf value
		if(!bufsize) return buf;
//		int len=snprintf(buf, bufsize, "IFACETYPE:%s:%s", vendor_name ? vendor_name : "BUILTIN", type_name);
		int len=snprintf(buf, bufsize, "IFACETYPE:%s", type_name);
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	static const iot_devifacetype_metaclass* findby_id(iot_type_id_t ifacetype_id, bool try_load=true);

	int serialized_size(const iot_deviface_params* obj) const {
		int res=p_serialized_size(obj);
		if(res<0) return res;
		return sizeof(serialize_base_t)+res;
	}
	int serialize(const iot_deviface_params* obj, char* buf, size_t bufsize) const; //returns error code or 0 on success
	static int deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_deviface_params*& obj) {
		//returns negative error code OR number of bytes written to provided buffer OR required buffer size when buf is NULL
		//returned value can be zero (regardless buf was NULL or not) to indicate that buffer was not used (or is not necessary) and that obj was assigned to
		//correct precreated object (may be statically allocated)
		if(datasize<sizeof(serialize_base_t)) return IOT_ERROR_BAD_DATA;
		serialize_base_t *p=(serialize_base_t*)data;
		iot_type_id_t ifacetype_id=repack_type_id(p->ifacetype_id);
		if(!ifacetype_id) return IOT_ERROR_BAD_DATA;
		const iot_devifacetype_metaclass* metaclass=iot_devifacetype_metaclass::findby_id(ifacetype_id);
		if(!metaclass) return IOT_ERROR_NOT_FOUND;
		obj=NULL;
		return metaclass->p_deserialize(data+sizeof(serialize_base_t), datasize-sizeof(serialize_base_t), buf, bufsize, obj);
	}
	//returns negative error code OR number of bytes written to provided buffer OR required buffer size when buf is NULL
	//returned value can be zero (regardless buf was NULL or not) to indicate that buffer was not used (or is not necessary) and that obj was assigned to
	//correct precreated object (may be statically allocated)
	//default_ifacetype is used when no "ifacetype_id" property in provided json or it is incorrect. Thus if it is 0, then required.
	static int from_json(json_object* json, char* buf, size_t bufsize, const iot_deviface_params*& obj, iot_type_id_t default_ifacetype=0);

	//returns negative error code (IOT_ERROR_NO_MEMORY) or zero on success
	int to_json(const iot_deviface_params* obj, json_object* &dst) const;

private:
	virtual int p_serialized_size(const iot_deviface_params* obj) const = 0;
	virtual int p_serialize(const iot_deviface_params* obj, char* buf, size_t bufsize) const = 0; //returns error code or 0 on success
	virtual int p_deserialize(const char* data, size_t datasize, char* buf, size_t bufsize, const iot_deviface_params*& obj) const = 0;
	virtual int p_from_json(json_object* json, char* buf, size_t bufsize, const iot_deviface_params*& obj) const = 0;
	virtual int p_to_json(const iot_deviface_params* obj, json_object* &dst) const = 0;
};


class iot_deviface_params { //base class for representing device interface params
	const iot_devifacetype_metaclass* meta; //keep reference to metaclass here to optimize (exclude virtual function call) requests which are redirected to metaclass

	iot_deviface_params(void) = delete;
protected:
	constexpr iot_deviface_params(const iot_devifacetype_metaclass* meta): meta(meta) { //only derived classes can create instances
	}
public:
	const iot_devifacetype_metaclass* get_metaclass(void) const {
		return meta;
	}
	iot_type_id_t get_id(void) const { //returns either valid ifacetype id OR zero
		return meta->get_id();
	}
	bool is_valid(void) const {
		if(!meta) return false;
		return get_id()!=0;
	}
	void invalidate(void) { //can be used on allocated objects of derived classes or on internal buffer of iot_hwdev_ident_buffered
		meta=NULL;
	}
	char* get_fullname(char *buf, size_t bufsize, int* doff=NULL) const {
		if(meta) return meta->get_fullname(buf, bufsize, doff);

		if(!bufsize) return buf;
		int len=snprintf(buf, bufsize, "%s", "IFACETYPE:INVALID");
		if(doff) *doff += len>=int(bufsize-1) ? int(bufsize-1) : len;
		return buf;
	}
	const char* get_name(void) const {
		if(meta) return meta->get_name();
		return "INVALID";
	}


	bool matches(const iot_deviface_params* spec) const {
		if(!spec->is_valid()) {
			assert(false);
			return false;
		}
		if(spec->is_tmpl()) { //right operand must be exact spec
			assert(false);
			return false;
		}
		if(spec==this) return true; //match self
		return p_matches(spec);
	}

	int serialized_size(void) const {
		return meta->serialized_size(this);
	}
	int serialize(char* buf, size_t bufsize) const { //returns error code or 0 on success
		return meta->serialize(this, buf, bufsize);
	}
	int to_json(json_object* &dst) const { //returns error code or 0 on success
		return meta->to_json(this, dst);
	}

	virtual bool is_tmpl(void) const = 0; //check if current objects represents template. otherwise it must be exact connection specification
	virtual size_t get_size(void) const = 0; //must return 0 if object is statically precreated and thus must not be copied by value, only by reference
	virtual uint32_t get_d2c_maxmsgsize(void) const = 0;
	virtual uint32_t get_c2d_maxmsgsize(void) const = 0;
	virtual char* sprint(char* buf, size_t bufsize, int* doff=NULL) const = 0;
private:
	virtual bool p_matches(const iot_deviface_params* spec) const = 0;
};


struct iot_deviface_params_buffered { //same as iot_deviface_params but has builtin buffer for storing any type of iot_deviface_params
	const iot_deviface_params* data;
	alignas(iot_deviface_params) char databuf[IOT_DEVIFACE_PARAMS_MAXSIZE]; //preallocated buffer 

	iot_deviface_params_buffered(void) {
		iot_deviface_params* p=(iot_deviface_params*)databuf;
		data=p;
		p->invalidate();
	}
	iot_deviface_params_buffered(const iot_deviface_params_buffered &op) {
		size_t sz=op.data->get_size();
		if(op.data==(iot_deviface_params*)op.databuf) {
			assert(sz>0);
			if(sz>sizeof(databuf)) {
				assert(false);
				data=NULL;
				return;
			}
			memcpy(databuf, op.data, sz);
			data=(iot_deviface_params*)databuf;
		} else {
			assert(sz==0);
			data=op.data;
		}
	}

	bool is_valid(void) const {
		if(!data) return false;
		return data->is_valid();
	}
	iot_deviface_params_buffered& operator=(const iot_deviface_params* pars) {
		assert(pars!=NULL);
		size_t sz=pars->get_size();
		if(!sz) {
			data=pars;
		} else {
			if(sz>sizeof(databuf)) {
				assert(false);
				data=NULL;
				return *this;
			}
			memcpy(databuf, pars, sz);
			data=(iot_deviface_params*)databuf;
		}
		return *this;
	}
	iot_deviface_params_buffered& operator=(const iot_deviface_params_buffered &op) {
		size_t sz=op.data->get_size();
		if(op.data==(iot_deviface_params*)op.databuf) {
			assert(sz>0);
			if(sz>sizeof(databuf)) {
				assert(false);
				data=NULL;
				return *this;
			}
			memcpy(databuf, op.data, sz);
			data=(iot_deviface_params*)databuf;
		} else {
			assert(sz==0);
			data=op.data;
		}
		return *this;
	}

	static int deserialize(const char* data, size_t datasize, iot_deviface_params_buffered* obj) { //obj must point to somehow allocated iot_deviface_params_buffered struct
		if(!obj) return IOT_ERROR_INVALID_ARGS;
		const iot_deviface_params* pdata=(iot_deviface_params*)obj->databuf;
		int rval=iot_devifacetype_metaclass::deserialize(data, datasize, obj->databuf, sizeof(obj->databuf), pdata);

		if(rval<0) return rval;
		//all is good
		obj->data=pdata;
		return rval;
	}
	static int from_json(json_object* json, iot_deviface_params_buffered *obj, iot_type_id_t default_ifacetype=0) { //obj must point to somehow (from stack etc.) allocated iot_hwdev_ident struct
		if(!obj) return IOT_ERROR_INVALID_ARGS;

		const iot_deviface_params* pdata=(iot_deviface_params*)obj->databuf;
		int rval=iot_devifacetype_metaclass::from_json(json, obj->databuf, sizeof(obj->databuf), pdata, default_ifacetype);

		if(rval<0) return rval;
		//all is good
		obj->data=pdata;
		return rval;
	}
};



//base classes for actual programmatic API of device interfaces

class iot_deviface__DRVBASE {
	const iot_conn_drvview* drvconn;
protected:
	iot_deviface__DRVBASE(void) : drvconn(NULL) {}
//	iot_deviface__DRVBASE(const iot_conn_drvview* conn_=NULL) {
//		init(conn_);
//	}
	bool init(const iot_conn_drvview* conn_=NULL) {
		drvconn=conn_;
		return !!conn_;
	}
	int send_client_msg(const void *msg, uint32_t msgsize) const;
	int read_client_req(void* buf, uint32_t bufsize, uint32_t &dataread, uint32_t &szleft) const;
public:
	bool is_inited(void) const {
		return drvconn!=NULL;
	}
};

class iot_deviface__CLBASE {
	const iot_conn_clientview* clconn;
protected:
	iot_deviface__CLBASE(void): clconn(NULL) {}
//	iot_deviface__CLBASE(const iot_conn_clientview* conn_=NULL) {
//		init(conn_);
//	}
	bool init(const iot_conn_clientview* conn_=NULL) {
		clconn=conn_;
		return !!conn_;
	}
	int send_driver_msg(const void *msg, uint32_t msgsize) const;
	int32_t start_driver_req(const void *data, uint32_t datasize, uint32_t fulldatasize=0) const;
	int32_t continue_driver_req(const void *data, uint32_t datasize) const;
public:
	bool is_inited(void) const {
		return clconn!=NULL;
	}
};



#endif //IOT_HWDEVREG_H
