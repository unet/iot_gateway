
#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include<ecb.h>


#include <iot_kapi.h>
#include <kernel/iot_common.h>

#include<kernel/iot_moduleregistry.h>
#include<kernel/iot_kernel.h>
#include<kernel/iot_configregistry.h>
#include<kernel/iot_peerconnection.h>

iot_peers_registry_t *peers_registry;
static iot_peers_registry_t _peers_registry; //instantiate singleton

