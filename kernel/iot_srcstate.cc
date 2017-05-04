#include<string.h>
#include<assert.h>

#include<iot_module.h>

#include<iot_srcstate_buttons.h>

/*
//calculates required storage in bytes by start address of data or returns negative error code (non-parsable data provided)
ssize_t iot_srcstate_data_buttons_t::get_size(void *buf) {
	iot_srcstate_data_buttons_t* p=(iot_srcstate_data_buttons_t*)buf;
	return sizeof(iot_srcstate_data_buttons_t)+p->size;
}

//extracts data from buf into provided struct. dstsize must be at least size returned by get_size(). Returns actual size of data
ssize_t iot_srcstate_data_buttons_t::extract(void *buf, iot_srcstate_data_buttons_t* dst, ssize_t dstsize) {
	ssize_t need=get_size(buf);
	if(need<=0) return need; //return error as is. 0 means 'no action' here
	if(dstsize<need) return IOT_ERROR_NO_BUFSPACE;
	memcpy(dst,buf,need); //storage format is direct
	return need;
}
*/

//struct iot_srcstate_custom_header_t { //custom field of iot_srcstate_t begins with such header
//	iot_state_classid classid[IOT_SOURCE_STATE_MAX_CLASSES]; //first zero terminates list
//	uint32_t offset[IOT_SOURCE_STATE_MAX_CLASSES]; //offset of data from start of this header (i.e. from start of custom data block)
//};

//tries to find specified class of data inside state custom block
//Returns 0 on success and fills startoffset to point to start of data. This offset can then be passed to CLASS::get_size and/or CLASS::extract methods to get actual data
//Possible errors:
//IOT_ERROR_NOT_FOUND - class of data not present in state
//int iot_find_srcstate_dataclass(iot_srcstate_t* state,iot_state_classid clsid, void** startoffset) {
//	assert(startoffset!=NULL);

//	if(state->custom_len <= sizeof(iot_srcstate_custom_header_t)) return IOT_ERROR_NOT_FOUND;
//	iot_srcstate_custom_header_t* h=(iot_srcstate_custom_header_t*)(state->custom);
//	for(int i=0;i<IOT_SOURCE_STATE_MAX_CLASSES;i++) {
//		if(!h->classid[i]) return IOT_ERROR_NOT_FOUND; //list terminated
//		if(h->classid[i]!=clsid) continue;
//		*startoffset=state->custom + h->offset[i];
//		return 0;
//	}
//	return IOT_ERROR_NOT_FOUND;
//}

