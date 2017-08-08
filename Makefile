
.DEFAULT_GOAL := all

auto/cvars.mk: ; #prevent make from attempt to remake this file
include auto/cvars.mk

APPEXT =
ifdef SystemRoot
	#windows
	APPEXT := .exe
endif

APPNAME := iotdaemon$(APPEXT)

KERNELDIR := kernel
KERNELINCLUDEDIR := kernel/include

#BUNDLELIST := unet/generic/inputlinux unet/generic/kbd unet/generic/toneplayer
BUNDLELIST := $(shell sed -n -r 's,^\s*([a-zA-Z0-9_]*[a-zA-Z0-9]/[a-zA-Z0-9_]*[a-zA-Z0-9]/[a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*y\s*$$,\1,p' bundles.cfg | tr '\n' ' ')
#DYNBUNDLELIST := unet/generic/inputlinux unet/generic/kbd unet/generic/toneplayer
DYNBUNDLELIST := $(shell sed -n -r 's,^\s*([a-zA-Z0-9_]*[a-zA-Z0-9]/[a-zA-Z0-9_]*[a-zA-Z0-9]/[a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*m\s*$$,\1,p' bundles.cfg | tr '\n' ' ')

common_hdr := $(wildcard $(INCLUDEDIR)/*.h)
kernel_hdr := $(common_hdr) $(wildcard $(KERNELINCLUDEDIR)/*.h)
kernel_autohdr := 
kernel_ccsrc := $(wildcard $(KERNELDIR)/*.cc)
kernel_ccobjs := $(patsubst %.cc,%.o,$(kernel_ccsrc))


KERNELCFLAGS := -I$(KERNELINCLUDEDIR) -Iauto -DDAEMON_KERNEL

LDFLAGS := -g -pthread -Wl,--export-dynamic,--gc-sections -Llibuv/.libs

#LDLIBS = -lpthread -ljson-c -ldl -Wl,-static,-l:libuv/.libs/libuv.a,-call_shared
LDLIBS = -lpthread -ljson-c -ldl -l:libuv.a

ifdef SystemRoot
#windows
LDFLAGS += -L/usr/x86_64-w64-mingw32/lib
LDLIBS += -lws2_32
endif

mod-objs =

PHONY := all kernel static_modules modules clean

all: $(APPNAME) modules

clean:
	$(RM) $(KERNELDIR)/*.o $(APPNAME) auto/iot_bundlesdb.h auto/iot_modulesdb.h auto/bundles.cmd
	@echo Cleaning modules...
	@for i in $(BUNDLELIST) $(DYNBUNDLELIST) ; do $(MAKE) -C $(MODULESDIR)/$$i clean; done


auto/iot_bundlesdb.h: bundles.cfg
	@echo "Rebuilding auto/iot_bundlesdb.h"
	@sed -n -r 's~^\s*([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*y\s*$$~static iot_modulesdb_bundle_t iot_moddb_bundle_\1__\2__\3("\1/\2/\3",true);\n#define IOT_MODULESDB_BUNDLE_\1__\2__\3~p' $< >$@
	@sed -n -r 's~^\s*([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*m\s*$$~static iot_modulesdb_bundle_t iot_moddb_bundle_\1__\2__\3("\1/\2/\3",false);\n#define IOT_MODULESDB_BUNDLE_\1__\2__\3~p' $< >>$@

auto/iot_modulesdb.h: modulesdb.cfg
	@echo "Rebuilding auto/iot_modulesdb.h"
	@sed -n -r 's~^\s*([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9]):([a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*(.*)~#ifdef IOT_MODULESDB_BUNDLE_\1__\2__\3\niot_modulesdb_item_t("\4", \&iot_moddb_bundle_\1__\2__\3, \5),\n#else\niot_modulesdb_item_t("\1/\2/\3:\4", NULL, \5),\n#endif~p' $< >$@


$(APPNAME): $(kernel_ccobjs) static_modules
	@# $(foreach mod,$(BUNDLELIST),$(eval $(call readfileval,$(MODULESDIR)/$(mod)/$(KERNELDEPSFILE))))
	$(CXX) $(LDFLAGS) $(kernel_ccobjs) $(mod-objs) -o $@  $(LDLIBS)

kernel: $(kernel_ccobjs)

$(KERNELDIR)/iot_moduleregistry.o: auto/iot_bundlesdb.h auto/iot_modulesdb.h

$(kernel_ccobjs): $(kernel_autohdr) $(kernel_hdr)

$(kernel_ccobjs): %.o : %.cc
	$(CXX) $(CXXFLAGS) $(KERNELCFLAGS) -o $@ -c $<


static_modules:
	@echo Making static modules...
	@for i in $(BUNDLELIST) ; do $(MAKE) -C $(MODULESDIR)/$$i; done

modules:
	@echo Making dynamic modules...
	@for i in $(DYNBUNDLELIST) ; do $(MAKE) -C $(MODULESDIR)/$$i; done


.PHONY : $(PHONY)

