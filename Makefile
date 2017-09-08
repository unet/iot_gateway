
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


mod-objs :=

PHONY := all kernel static_modules modules clean registry coreregistry

all: $(APPNAME) modules registry

clean:
	$(RM) $(KERNELDIR)/*.o *.o $(APPNAME) manifest_proc auto/iot_linkedlibs.cc auto/iot_dynlibs.cc auto/iot_modulesdb.h auto/bundles.cmd registry.part.json manifest.part.json dev_ids.json
	@echo Cleaning modules...
	@for i in $(BUNDLELIST) $(DYNBUNDLELIST) ; do $(MAKE) -C $(MODULESDIR)/$$i clean; done


auto/iot_linkedlibs.cc: bundles.cfg
	@echo "Rebuilding auto/iot_linkedlibs.cc"
#	@sed -n -r 's~^\s*([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*y\s*$$~static iot_regitem_lib_t iot_moddb_bundle_\1__\2__\3("\1/\2/\3",true);\n#define IOT_MODULESDB_BUNDLE_\1__\2__\3~p' $< >$@
#	@sed -n -r 's~^\s*([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*m\s*$$~static iot_regitem_lib_t iot_moddb_bundle_\1__\2__\3("\1/\2/\3",false);\n#define IOT_MODULESDB_BUNDLE_\1__\2__\3~p' $< >>$@
	@sed -n -r 's~^\s*([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*y\s*$$~static iot_regitem_lib_t iot_moddb_bundle_\1__\2__\3("\1/\2/\3",true);~p' $< >$@

auto/iot_dynlibs.cc: bundles.cfg
	@echo "Rebuilding auto/iot_dynlibs.cc"
	@sed -n -r 's~^\s*([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*m\s*$$~static iot_regitem_lib_t iot_moddb_bundle_\1__\2__\3("\1/\2/\3",false);~p' $< >$@


#auto/iot_modulesdb.h: modulesdb.cfg
#	@echo "Rebuilding auto/iot_modulesdb.h"
#	@sed -n -r 's~^\s*([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9]):([a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*(.*)~#ifdef IOT_MODULESDB_BUNDLE_\1__\2__\3\niot_regitem_module_t("\4", \&iot_moddb_bundle_\1__\2__\3, \5),\n#else\niot_regitem_module_t("\1/\2/\3:\4", NULL, \5),\n#endif~p' $< >$@


$(APPNAME): $(kernel_ccobjs) main.o static_modules
	@# $(eval mod-objs :=)
	@# $(foreach mod,$(BUNDLELIST),$(eval $(call readfileval,$(MODULESDIR)/$(mod)/$(KERNELDEPSFILE))))
	$(CXX) $(LDFLAGS) $(kernel_ccobjs) main.o $(mod-objs) -o $@  $(LDLIBS)

manifest_proc: $(kernel_ccobjs) manifest_proc.o static_modules modules
	@# $(eval mod-objs :=)
	@# $(foreach mod,$(BUNDLELIST),$(eval $(call readfileval,$(MODULESDIR)/$(mod)/$(KERNELDEPSFILE))))
	$(CXX) $(LDFLAGS) $(kernel_ccobjs) manifest_proc.o $(mod-objs) -o $@  $(LDLIBS)

coreregistry: manifest.part.json registry.part.json

manifest.part%json registry.part%json : manifest.json $(kernel_ccobjs) manifest_proc.o
	./manifest_proc CORE

registry: manifest_proc coreregistry
	@echo Generating manifests and registry...
	@set -e; for i in $(BUNDLELIST) $(DYNBUNDLELIST); do $(MAKE) -C $(MODULESDIR)/$$i registry; done
	./manifest_proc REGISTRY registry.part.json $(foreach lib,$(BUNDLELIST) $(DYNBUNDLELIST),$(MODULESDIR)/$(lib)/registry.part.json)

kernel: $(kernel_ccobjs)

$(KERNELDIR)/iot_libregistry.o: auto/iot_linkedlibs.cc


$(kernel_ccobjs) main.o manifest_proc.o: $(kernel_autohdr) $(kernel_hdr)

$(kernel_ccobjs) main.o manifest_proc.o: %.o : %.cc
	$(CXX) $(KERNELCFLAGS) $(CXXFLAGS) -o $@ -c $<

manifest_proc.o: auto/iot_dynlibs.cc


static_modules:
	@echo Making static modules...
	@set -e; for i in $(BUNDLELIST) ; do $(MAKE) -C $(MODULESDIR)/$$i; done

modules:
	@echo Making dynamic modules...
	@set -e; for i in $(DYNBUNDLELIST) ; do $(MAKE) -C $(MODULESDIR)/$$i; done


.PHONY : $(PHONY)

