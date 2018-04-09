#ifndef IOT_ERROR_H
#define IOT_ERROR_H

//IOT ERROR codes
#define IOT_ERROR_MAP(XX) \
	XX(NO_ERROR, 0, "no error")												\
	XX(NO_MEMORY, -1, "memory allocation error")							\
	XX(NOT_INITED, -2, "object wasn't properly inited")						\
	XX(INITED_TWICE, -3, "object was already inited")						\
	XX(INVALID_THREAD, -4, "function called from unacceptable thread")		\
	XX(NO_BUFSPACE, -5, "provided buffer size is not enough")				\
	XX(NOT_FOUND, -6, "not found")											\
	XX(INVALID_ARGS, -7, "invalid args provided")							\
	XX(TEMPORARY_ERROR, -8, "temporary error")								\
	XX(NOT_SUPPORTED, -9, "not supported")									\
	XX(INVALID_DEVICE_DATA, -10, "invalid device data")						\
	XX(CRITICAL_ERROR, -11, "critical error")								\
	XX(LIMIT_REACHED, -12, "limit reached")									\
	XX(NO_PEER, -13, "peer is not connected")								\
	XX(TRY_AGAIN, -14, "one more try should be made")						\
	XX(MESSAGE_IGNORED, -15, "unknown or invalid message")					\
	XX(UNKNOWN_ACTION, -16, "unknown action")								\
	XX(NOT_READY, -17, "object or task not ready")							\
	XX(ACTION_CANCELLED, -18, "action cancelled or shurdown in progress")	\
	XX(MODULE_BLOCKED, -19, "module blocked")								\
	XX(NO_ACTION, -20, "no action performed")								\
	XX(HARD_LIMIT_REACHED, -21, "hard limit reached")						\
	XX(BAD_REQUEST, -22, "request is broken")								\
	XX(BAD_DATA, -23, "request is broken")									\
	XX(OBJECT_INVALIDATED, -24, "target object was freed (form of success)") \
	XX(ADDRESS_INUSE, -25, "address is already busy or cannot be assigned")	\
	XX(INVALID_STATE, -26, "object is in inappropriate state")				\
	XX(NO_ROUTE, -27, "no route to destination")							\
	XX(CONN_RESET, -28, "connection reset by peer")							\
	XX(ACTION_TIMEOUT, -29, "action timeout")								\
	XX(STREAM_CLOSED, -30, "stream is shut down or closed by peer")			\
	XX(CRITICAL_BUG, -100, "bug in code")

enum iot_error_t {
#define XX(nm, cc, _) IOT_ERROR_ ## nm = cc,
	IOT_ERROR_MAP(XX)
#undef XX
	IOT_ERROR_MAX = -101
};


const char* kapi_strerror(int err);
const char* kapi_err_name(int err);


#endif // IOT_ERROR_H
