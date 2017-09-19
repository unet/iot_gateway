#ifndef IOT_DAEMONLIB_H
#define IOT_DAEMONLIB_H


#define outlog_error(format... ) do_outlog(__FILE__, __LINE__, __func__, LERROR, format)
#define outlog_notice(format... ) if(LMIN<=LNOTICE && min_loglevel <= LNOTICE) do_outlog(__FILE__, __LINE__, __func__, LNOTICE, format)
#define outlog_info(format... ) if(LMIN<=LINFO && min_loglevel <= LINFO) do_outlog(__FILE__, __LINE__, __func__, LINFO, format)
#define outlog_debug(format... ) if(LMIN<=LDEBUG && min_loglevel <= LDEBUG) do_outlog(__FILE__, __LINE__, __func__, LDEBUG, format)
#define outlog(level, format... ) if(LMIN<=level && min_loglevel <= level) do_outlog(__FILE__, __LINE__, __func__, level, format)

//use 'errbuf' where err description must be put
#define outlog_errno(err, level, format... ) if(min_loglevel <= level) {char errbuf2[128]; char* errbuf=strerror_r(err,errbuf2,sizeof(errbuf2)); do_outlog(__FILE__, __LINE__, __func__, level, format);}

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
