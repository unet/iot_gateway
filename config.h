#ifndef CONFIG_H
#define CONFIG_H


#ifndef _WIN32
	//default home dir when no startup argument
	#define PREFIX "/usr/local/iotgateway/"
#else
	//windows must take actual path from registry
#endif



#endif //CONFIG_H