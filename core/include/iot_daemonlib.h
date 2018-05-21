#ifndef IOT_DAEMONLIB_H
#define IOT_DAEMONLIB_H

#include "iot_module.h"

//bitmask for log levels
#define LMASKVAL 7

//addition debug sublevels
#define LDEBUG_MESHTUN (1*(LMASKVAL+1))
#define LDEBUG_MESH (2*(LMASKVAL+1))
#define LDEBUG_MODELLING (3*(LMASKVAL+1))
#define LDEBUG_IOTGW (4*(LMASKVAL+1))
#define LDEBUG_DEVREG (5*(LMASKVAL+1))


#ifndef NDEBUG

	#ifdef IOTDEBUG_MESHTUN
	#define outlog_debug_meshtun_vars(VARDECL, format... ) do {if(min_loglevel <= LNOTICE) {VARDECL; do_outlog(__FILE__, __LINE__, __func__, LDEBUG+LDEBUG_MESHTUN, format);}} while(0)
	#define outlog_debug_meshtun(format... ) outlog_debug_meshtun_vars( , format)
	#endif

	#ifdef IOTDEBUG_MODELLING
	#define outlog_debug_modelling_vars(VARDECL, format... ) do {if(min_loglevel <= LNOTICE) {VARDECL; do_outlog(__FILE__, __LINE__, __func__, LDEBUG+LDEBUG_MODELLING, format);}} while(0)
	#define outlog_debug_modelling(format... ) outlog_debug_modelling_vars( , format)
	#endif

	#ifdef IOTDEBUG_MESH
	#define outlog_debug_mesh_vars(VARDECL, format... ) do {if(min_loglevel <= LNOTICE) {VARDECL; do_outlog(__FILE__, __LINE__, __func__, LDEBUG+LDEBUG_MESH, format);}} while(0)
	#define outlog_debug_mesh(format... ) outlog_debug_mesh_vars( , format)
	#endif

	#ifdef IOTDEBUG_IOTGW
	#define outlog_debug_iotgw_vars(VARDECL, format... ) do {if(min_loglevel <= LNOTICE) {VARDECL; do_outlog(__FILE__, __LINE__, __func__, LDEBUG+LDEBUG_IOTGW, format);}} while(0)
	#define outlog_debug_iotgw(format... ) outlog_debug_iotgw_vars( , format)
	#endif

	#ifdef IOTDEBUG_DEVREG
	#define outlog_debug_devreg_vars(VARDECL, format... ) do {if(min_loglevel <= LNOTICE) {VARDECL; do_outlog(__FILE__, __LINE__, __func__, LDEBUG+LDEBUG_DEVREG, format);}} while(0)
	#define outlog_debug_devreg(format... ) outlog_debug_devreg_vars( , format)
	#endif


#endif

#ifndef outlog_debug_meshtun
#define outlog_debug_meshtun_vars(VARDECL, format... )
#define outlog_debug_meshtun(format... )
#endif

#ifndef outlog_debug_modelling
#define outlog_debug_modelling_vars(VARDECL, format... )
#define outlog_debug_modelling(format... )
#endif


#ifndef outlog_debug_mesh
#define outlog_debug_mesh_vars(VARDECL, format... )
#define outlog_debug_mesh(format... )
#endif

#ifndef outlog_debug_iotgw
#define outlog_debug_iotgw_vars(VARDECL, format... )
#define outlog_debug_iotgw(format... )
#endif

#ifndef outlog_debug_devreg
#define outlog_debug_devreg_vars(VARDECL, format... )
#define outlog_debug_devreg(format... )
#endif


//use 'errbuf' where err description must be put
#define outlog_errno(err, level, format... ) do {if(min_loglevel <= level) {char errbuf2[128]; char* errbuf=strerror_r(err,errbuf2,sizeof(errbuf2)); do_outlog(__FILE__, __LINE__, __func__, level, format);}} while(0)

extern char bin_dir[]; //parent dir for daemon binary
extern char run_dir[];
extern char conf_dir[];
extern char modules_dir[];
extern size_t bin_dir_len;
extern char bin_basename[]; //parent dir for daemon binary


bool init_log(const char* dir,const char* logfile);
void close_log(void);
void remove_pidfile(const char* dir, const char* pidfile);
int create_pidfile(const char* dir, const char* pidfile);
bool parse_args(int argc, char **arg);

#endif // IOT_DAEMONLIB_H
