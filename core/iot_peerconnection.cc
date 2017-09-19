
#include<stdint.h>
#include<assert.h>
//#include<time.h>

#include "iot_kapi.h"
#include "iot_common.h"

#include "iot_moduleregistry.h"
#include "iot_core.h"
#include "iot_configregistry.h"
#include "iot_peerconnection.h"

iot_peers_registry_t *peers_registry;
static iot_peers_registry_t _peers_registry; //instantiate singleton

