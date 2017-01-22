#ifndef IOT_DAEMONLIB_H
#define IOT_DAEMONLIB_H


#define outlog_error(format... ) do_outlog(__FILE__, __LINE__, __func__, LERROR, format)
#define outlog_notice(format... ) if(min_loglevel <= 2) do_outlog(__FILE__, __LINE__, __func__, LNOTICE, format)
#define outlog_info(format... ) if(min_loglevel <= 1) do_outlog(__FILE__, __LINE__, __func__, LINFO, format)
#define outlog_debug(format... ) if(min_loglevel == 0) do_outlog(__FILE__, __LINE__, __func__, LDEBUG, format)
#define outlog(level, format... ) if(min_loglevel <= level) do_outlog(__FILE__, __LINE__, __func__, level, format)

//use 'errbuf' where err description must be put
#define outlog_errno(err, level, format... ) if(min_loglevel <= level) {char errbuf2[128]; char* errbuf=strerror_r(err,errbuf2,sizeof(errbuf2)); do_outlog(__FILE__, __LINE__, __func__, level, format);}

extern char rootpath[128]; //root dir for daemon


bool init_log(const char* logfile);
void close_log(void);
void remove_pidfile(const char* pidfile);
int create_pidfile(const char* pidfile);


#endif // IOT_DAEMONLIB_H
