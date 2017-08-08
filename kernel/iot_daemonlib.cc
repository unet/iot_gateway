#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#include "config.h"

#include "iot_module.h"
#include "iot_daemonlib.h"

static const char *loglevel_str[]={
	"Dbg_",
	"Info",
	"NOTE",
	"!ERR"
};

//allows to copy all logging to stdout
#define LOGGER_STDOUT 1


static int logfd=-1;
char rootpath[128]; //root dir for daemon
size_t rootlen; //length of root dir

int min_loglevel=-1; //means 'unset'

//#ifdef NDEBUG
//int min_loglevel=LNOTICE;
//#else
////__attribute__ ((visibility ("default"))) 
//int min_loglevel=LDEBUG;
//#endif


#ifndef _WIN32
static long logclock_cost=0, prevlogcost=0;
static timespec prevlogts={0,0};
#endif


//opens or reopens logfile with provided relative path inside daemon's root dir
//returns false on error
bool init_log(const char* logfile)
{
	char buf[256];
	snprintf(buf,sizeof(buf),"%s%s",rootpath,logfile);
	int newlogfd=open(buf,O_WRONLY|O_APPEND|O_CREAT,0666);
	if(newlogfd<0) {
		char errbuf[256];
		if(logfd>=0) {
			outlog_error("Error reopening log file %s: %s\n",buf,strerror_r(errno, errbuf, sizeof(errbuf)));
		} else { //should be non-daemonized. TODO for windows service
			fprintf(stderr,"Error opening log file %s: %s\n",buf,strerror_r(errno, errbuf, sizeof(errbuf)));
		}
		return false;
	}
	if(logfd>=0 && newlogfd!=logfd) { //log was opened, so this call can be reinit
		dup2(newlogfd, logfd);
		close(newlogfd);
	}
	logfd=newlogfd;
#ifndef _WIN32
	//calculate cost of clock_gettime call
	struct timespec ts, ts2;
	clock_gettime(CLOCK_REALTIME, &ts);
	clock_gettime(CLOCK_REALTIME, &ts2);
	clock_gettime(CLOCK_REALTIME, &ts2);
	clock_gettime(CLOCK_REALTIME, &ts2);
	clock_gettime(CLOCK_REALTIME, &ts2);
	clock_gettime(CLOCK_REALTIME, &ts2);
	clock_gettime(CLOCK_REALTIME, &ts2);
	clock_gettime(CLOCK_REALTIME, &ts2);
	clock_gettime(CLOCK_REALTIME, &ts2);
	
	logclock_cost=(ts2.tv_nsec-ts.tv_nsec+1000000000*(ts2.tv_sec-ts.tv_sec))/8;
	prevlogts=ts2;
printf("loglevel=%d\n",min_loglevel);
	if(min_loglevel==LDEBUG) {
		long mindif=100000;
		for(int i=8;i>=0;i--) {
			outlog_debug("Measuring log delay (cost %ld)...%d",logclock_cost, i);
			long d=prevlogts.tv_nsec-ts2.tv_nsec+1000000000*(prevlogts.tv_sec-ts2.tv_sec)-prevlogcost;
			ts2=prevlogts;
			if(d<mindif) {
				mindif=d;
				logclock_cost=mindif;
			}
		}
		outlog_debug("cost=%ldns",logclock_cost);
	}
#endif
	return true;
}


void close_log(void)
{
	if(logfd>=0) close(logfd);
	logfd=-1;
}

static void do_voutlog(const char*file, int line, const char* func,int level, const char *fmt, va_list ap)
{
	if(logfd<0) return;
	char buf[2048];
	size_t len;

	long nsec;
#ifndef _WIN32
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	nsec=ts.tv_nsec;//unsigned((tv.tv_usec+500)/1000);
	struct tm tm1, *tm;
	localtime_r(&ts.tv_sec,&tm1);
	tm=&tm1;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	nsec=tv.tv_usec*1000;
	time_t tt=tv.tv_sec;
	tm* tm=localtime(&tt);
#endif

	size_t buflen=sizeof(buf)-256; //leave reserve for "{truncated}" suffics and file/func information
	if(level>LERROR) level=LERROR;

	len=strftime(buf,buflen,"%d.%m.%Y %H:%M:%S",tm);
#ifndef _WIN32
	len+=snprintf(buf+len,buflen-len,".%09ld {%09ldns}: [%d %s] ",nsec,(ts.tv_nsec-prevlogts.tv_nsec+1000000000*(ts.tv_sec-prevlogts.tv_sec))-logclock_cost,int(getpid()), loglevel_str[level]);
#else
	len+=snprintf(buf+len,buflen-len,".%09ld: [%d %s] ",nsec,int(getpid()), loglevel_str[level]);
#endif
	len+=vsnprintf(buf+len,buflen-len,fmt, ap);
	if(len>=buflen) { //output buf overfilled
		len=buflen-1;
		len+=snprintf(buf+len,sizeof(buf)-len,"{truncated}");
	}
	if(func && len<sizeof(buf)) len+=snprintf(buf+len,sizeof(buf)-len,"    at %s()", func);
	if(file && len<sizeof(buf)) len+=snprintf(buf+len,sizeof(buf)-len,"%s {\"%s\" line %d}", func ? "" : "   ", file, line);
	if(len>=sizeof(buf)-1) buf[sizeof(buf)-1]='\n';
		else {buf[len++]='\n';buf[len]='\0';}
	write(logfd,buf,len);
#ifdef _WIN32
//TODO check if console connected
	//copy to console
	buf[sizeof(buf)-1]='\0';
	puts(buf);
#else
	clock_gettime(CLOCK_REALTIME, &prevlogts);
	prevlogcost=prevlogts.tv_nsec-ts.tv_nsec+1000000000*(prevlogts.tv_sec-ts.tv_sec);
#if LOGGER_STDOUT
	buf[sizeof(buf)-1]='\0';
	printf("%s",buf);
#endif
#endif
}

void do_outlog(const char*file, int line, const char* func, int level, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	do_voutlog(file, line, func, level, fmt, ap);
	va_end(ap);
}


#ifndef _WIN32

void remove_pidfile(const char* pidfile)
{
	char namebuf[256];
	snprintf(namebuf,sizeof(namebuf),"%s%s",rootpath,pidfile);
	unlink(namebuf);
}


int create_pidfile(const char* pidfile)
{
	char namebuf[256];
	int pidf;
	size_t l;
	snprintf(namebuf,sizeof(namebuf),"%s%s",rootpath,pidfile);
	pidf=open(namebuf,O_RDWR|O_CREAT,0644);
	if(pidf<0) {
		outlog_errno(errno,LERROR,"cannot create pid file %s: %s",namebuf,errbuf);
		return 0;
	}
	while(flock(pidf,LOCK_EX)==-1) {
		if(errno==EINTR) continue;
		outlog_errno(errno,LERROR,"cannot lock pid file %s: %s",namebuf,errbuf);
		close(pidf);
		return 0;
	}
	if(lseek(pidf,0,SEEK_END)>0) //pid file exists, check that process with written pid still exists
		{
		pid_t p;
		char buf[10];
		l=pread(pidf,buf,10,0);
		if(l>0) {
			buf[l]='\0';
			p=strtoul(buf,NULL,10);
			if(p>1 && kill(p,0)==0)	{
				outlog_error("cannot run: pid file %s exists and process with pid %u is running",namebuf,(unsigned)p);
				close(pidf);
				return 0;
			}
		}
		ftruncate(pidf,0);
	}
	l=snprintf(namebuf,sizeof(namebuf),"%u",(unsigned)getpid());
	if(pwrite(pidf,namebuf,l,0)!=int(l)) {
		outlog_errno(errno,LERROR,"cannot write to pid file %u bytes: %s",l,errbuf);
		return 0;
	}
	close(pidf);
	return 1;
}


bool parse_args(int argc, char **arg, const char* rundir, const char* addhelparg, const char* addhelpmsg)
{
	int i;
	if(argc<2 || !arg[1]) {
		i=snprintf(rootpath,sizeof(rootpath)-1,"%s",".");
//		fprintf(stderr,"Missing mandatory parameter.\n"
//"Syntax: PROGNAME workdir [loglevel]%s\n"
//"\tworkdir\t\tdaemon home directory, where config files are searched and subdir '/%s' is used for DB, pid-file and logs\n"
//"\tloglevel\tlevel of logging: 0-debug,1-info,2-notice,3-error (default is %d)\n"
//"%s\n",addhelparg ? addhelparg : "", rundir, min_loglevel, addhelpmsg ? addhelpmsg : "");
//		return false;
	} else {
		i=snprintf(rootpath,sizeof(rootpath)-1,"%s",arg[1]); //reserve one char for trailing '/'
		if(i>=(int)sizeof(rootpath)-1 || i<=0) {
			fprintf(stderr,"Error: length of workdir must not exceed %d chars\n\n",int(sizeof(rootpath))-2);
			return false;
		}
	}
	rootlen=i;
	if(rootpath[rootlen-1]!='/') { //append '/' is necessary
		rootpath[rootlen++]='/';
		rootpath[rootlen]='\0';
	}
	if(argc>=3 && arg[2]) {
		min_loglevel=atoi(arg[2]);
		if(min_loglevel<LDEBUG) min_loglevel=LDEBUG;
			else if(min_loglevel>LERROR) min_loglevel=LERROR;
	}

	char namebuf[256];
	snprintf(namebuf,sizeof(namebuf),"%s%s",rootpath,rundir);
	if(mkdir(namebuf, 0755) && errno!=EEXIST) {
		fprintf(stderr,"Error creating '%s': %s\n", namebuf, strerror(errno));
		return false;
	}
	return true;
}


#endif

