#ifndef CONFIG_H
#define CONFIG_H

//relative path is relative to binary location, not current dir
#define MODULES_DIR "modules"
#define RUN_DIR "run"
#define CONF_DIR ""

#define IOT_SOEXT ".so"

#define IOT_SIGLEN 0

//#define IOTDEBUG_MESHTUN
//#define IOTDEBUG_MESH
#define IOTDEBUG_MODELLING
//#define IOTDEBUG_IOTGW
#define IOTDEBUG_DEVREG


//default port for TCP and UDP peer connectors
#define IOT_PEERCON_TCPUDP_PORT 12002

#endif //CONFIG_H