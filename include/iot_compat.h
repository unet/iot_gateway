#ifndef IOT_COMPAT_H
#define IOT_COMPAT_H

#ifdef _WIN32
//	#define strerror_r(e,buf,bufsize) snprintf(buf,bufsize,"%s",strerror(e))
	#define strerror_r(errno,buf,len) (strerror_s(buf,len,errno), buf)
	#define __always_inline inline
#else
	#define INVALID_SOCKET (-1)
//	typedef int SOCKET;
//	#define SOCKET_ERROR (-1)	
//	#define closesocket close
#endif



#endif //IOT_COMPAT_H
