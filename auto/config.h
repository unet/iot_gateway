#ifndef CONFIG_H
#define CONFIG_H

//relative path is relative to binary location, not current dir
#define MODULES_DIR "modules"
#define RUN_DIR "run"
#define CONF_DIR ""

#define IOT_SOEXT ".so"

#define IOT_SIGLEN 0

//default port for TCP and UDP peer connectors
#define IOT_PEERCON_TCPUDP_PORT 12002

#endif //CONFIG_H