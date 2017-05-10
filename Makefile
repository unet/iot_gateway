#list of modules to compile
MODULEDIR = modules
KERNELDIR = kernel
INCLUDEDIR = include
MODULELIST = unet/generic/inputlinux unet/generic/kbd

common_hdr = $(wildcard $(INCLUDEDIR)/*.h)
kernel_ccsrc = $(wildcard $(KERNELDIR)/*.cc)
kernel_hdr = $(common_hdr) $(wildcard $(INCLUDEDIR)/$(KERNELDIR)/*.h) bundles-db.h modules-db.h
kernel_ccobjs = $(patsubst %.cc,%.o,$(kernel_ccsrc))


GCC=gcc
CPP=g++
LD=ld

#COMMONFLAGS = -Wall -Wstrict-overflow=2 -g -O0 -fPIC -rdynamic -fno-strict-aliasing -fdata-sections -ffunction-sections -I. -Iinclude -pthread -Ilibuv
COMMONFLAGS = -Wall -Wstrict-overflow=2 -g -O0 -I. -Iinclude -pthread -Ilibuv
#COMMONFLAGS_NDEBUG = -Wall -Wstrict-overflow=2 -O2 -DNDEBUG -I. -Iinclude -pthread -Ilibuv
CPPFLAGS = $(COMMONFLAGS) -std=c++11
CFLAGS = $(COMMONFLAGS)

LDFLAGS = -g -Wl,--gc-sections -pthread -Wl,--export-dynamic
LDDYNFLAGS = -shared


LIBS = -lpthread -ljson-c  -ldl -Llibuv/.libs -Wl,-static,-luv,-call_shared
APPEXT =

ifdef SystemRoot
	#windows
	LIBS := $(LIBS) -lws2_32
	LDFLAGS := $(LDFLAGS) -L/usr/x86_64-w64-mingw32/lib
	APPEXT := .exe
endif

APPNAME = iotdaemon$(APPEXT)

.PHONY: all kernel modules clean

all: kernel modules $(APPNAME)


modules_objs = $(foreach name, $(MODULELIST), $(MODULEDIR)/$(name)/$(notdir $(name)).o)

#which objs among modules_objs are statically linked to app
builtin_modules_objs = $(modules_objs)
#$(modules_objs)
dyn_modules_objs =
dyn_modules_sos = $(patsubst %.o,%.so,$(dyn_modules_objs))

$(dyn_modules_sos) : %.so : %.o
	$(LD) $(LDDYNFLAGS) -o $@ $(patsubst %.so,%.o,$@)

modules: $(builtin_modules_objs) $(dyn_modules_sos)


$(modules_objs): $(patsubst %.o,%.cc,$(modules_objs)) $(common_hdr)
	$(CPP) $(CPPFLAGS) -fPIC -o $@ -c $(patsubst %.o,%.cc,$@)

$(APPNAME): $(kernel_ccobjs) $(builtin_modules_objs)
	$(CPP) $(LDFLAGS) $(kernel_ccobjs) $(builtin_modules_objs) -o $@  $(LIBS)

kernel: $(kernel_ccobjs)

$(kernel_ccobjs): %.o : %.cc $(kernel_hdr)
	$(CPP) $(CPPFLAGS) -DDAEMON_KERNEL -o $@ -c $(patsubst %.o,%.cc,$@)

clean:
	rm $(KERNELDIR)/*.o $(APPNAME) $(modules_objs) $(dyn_modules_sos)

