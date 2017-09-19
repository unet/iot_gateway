
.DEFAULT_GOAL := all

auto/cvars.mk: ; #prevent make from attempt to remake this file
include auto/cvars.mk

APPEXT =
ifdef SystemRoot
	#windows
	APPEXT := .exe
endif

APPNAME := iotdaemon$(APPEXT)

COREDIR := core
COREINCLUDEDIR := core/include

#BUNDLELIST := unet/generic/inputlinux unet/generic/kbd unet/generic/toneplayer
BUNDLELIST := $(shell sed -n -r 's,^\s*([a-zA-Z0-9_]*[a-zA-Z0-9]/[a-zA-Z0-9_]*[a-zA-Z0-9]/[a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*y\s*$$,\1,p' bundles.cfg | tr '\n' ' ')
#DYNBUNDLELIST := unet/generic/inputlinux unet/generic/kbd unet/generic/toneplayer
DYNBUNDLELIST := $(shell sed -n -r 's,^\s*([a-zA-Z0-9_]*[a-zA-Z0-9]/[a-zA-Z0-9_]*[a-zA-Z0-9]/[a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*m\s*$$,\1,p' bundles.cfg | tr '\n' ' ')

common_hdr := $(wildcard $(INCLUDEDIR)/*.h)
core_hdr := $(common_hdr) $(wildcard $(COREINCLUDEDIR)/*.h)
core_autohdr := 
core_ccsrc := $(wildcard $(COREDIR)/*.cc)
core_ccobjs := $(patsubst %.cc,%.o,$(core_ccsrc))


CORECFLAGS := -I$(COREINCLUDEDIR) -Iauto -DDAEMON_CORE


mod-objs :=

PHONY := all core static_modules modules clean registry coreregistry

all: $(APPNAME) modules registry

clean:
	$(RM) $(COREDIR)/*.o *.o $(APPNAME) manifest_proc auto/iot_linkedlibs.cc auto/iot_dynlibs.cc auto/iot_modulesdb.h auto/bundles.cmd registry.part.json manifest.part.json dev_ids.json
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


$(APPNAME): $(core_ccobjs) main.o static_modules
	@# $(eval mod-objs :=)
	@# $(foreach mod,$(BUNDLELIST),$(eval $(call readfileval,$(MODULESDIR)/$(mod)/$(COREDEPSFILE))))
	$(CXX) $(LDFLAGS) $(core_ccobjs) main.o $(mod-objs) -o $@  $(LDLIBS)

manifest_proc: $(core_ccobjs) manifest_proc.o static_modules modules
	@# $(eval mod-objs :=)
	@# $(foreach mod,$(BUNDLELIST),$(eval $(call readfileval,$(MODULESDIR)/$(mod)/$(COREDEPSFILE))))
	$(CXX) $(LDFLAGS) $(core_ccobjs) manifest_proc.o $(mod-objs) -o $@  $(LDLIBS)

coreregistry: manifest.part.json registry.part.json

manifest.part%json registry.part%json : manifest.json $(core_ccobjs) manifest_proc.o reg_ids.json dev_ids.json
	./manifest_proc CORE

dev_ids.json:;

#manifest.part%json : manifest.json
#	./manifest_proc CORE


registry: manifest_proc coreregistry
	@echo Generating manifests and registry...
	@set -e; for i in $(BUNDLELIST) $(DYNBUNDLELIST); do $(MAKE) -C $(MODULESDIR)/$$i registry; done
	./manifest_proc REGISTRY registry.part.json $(foreach lib,$(BUNDLELIST) $(DYNBUNDLELIST),$(MODULESDIR)/$(lib)/registry.part.json)
	@echo SUCCESS!!!

core: $(core_ccobjs)

$(COREDIR)/iot_libregistry.o: auto/iot_linkedlibs.cc


$(core_ccobjs) main.o manifest_proc.o: $(core_autohdr) $(core_hdr)

$(core_ccobjs) main.o manifest_proc.o: %.o : %.cc
	$(CXX) $(CORECFLAGS) $(CXXFLAGS) -o $@ -c $<

manifest_proc.o: auto/iot_dynlibs.cc


static_modules:
	@echo Making static modules...
	@set -e; for i in $(BUNDLELIST) ; do $(MAKE) -C $(MODULESDIR)/$$i; done

modules:
	@echo Making dynamic modules...
	@set -e; for i in $(DYNBUNDLELIST) ; do $(MAKE) -C $(MODULESDIR)/$$i; done


.PHONY : $(PHONY)

