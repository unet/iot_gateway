#ifndef IOT_ERROR_H
#define IOT_ERROR_H

#include<ecb.h>

//IOT ERROR codes
#define IOT_ERROR_MAP(XX) \
	XX(NO_ERROR, 0, "no error")												\
	XX(NO_MEMORY, -1, "memory allocation error")							\
	XX(NOT_INITED, -2, "object wasn't properly inited")						\
	XX(INITED_TWICE, -3, "object was already inited")						\
	XX(INVALID_THREAD, -4, "function called from unacceptable thread")		\
	XX(NO_BUFSPACE, -5, "provided buffer size if not enough")				\
	XX(NOT_FOUND, -6, "not found")											\
	XX(INVALID_ARGS, -7, "invalid args provided")							\
	XX(TEMPORARY_ERROR, -8, "temporary error")								\
	XX(DEVICE_NOT_SUPPORTED, -9, "device not supported")					\
	XX(INVALID_DEVICE_DATA, -10, "invalid device data")						\
	XX(CRITICAL_ERROR, -11, "critical error")								\
	XX(LIMIT_REACHED, -12, "limit reached")									\
	XX(NO_PEER, -13, "peer is not connected")								\
	XX(TRY_AGAIN, -14, "one more try should be made")						\
	XX(CRITICAL_BUG, -100, "bug in code")

typedef enum {
#define XX(nm, cc, _) IOT_ERROR_ ## nm = cc,
	IOT_ERROR_MAP(XX)
#undef XX
	IOT_ERROR_MAX = -101
} iot_error_t;


ECB_EXTERN_C_BEG

const char* kapi_strerror(int err);
const char* kapi_err_name(int err);

ECB_EXTERN_C_END



#endif // IOT_ERROR_H
