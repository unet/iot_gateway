#determine base dir of source
BASEDIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../)

.DEFAULT_GOAL := all

$(BASEDIR)/auto/cvars.mk: ; #prevent make from attempt to remake this file
include $(BASEDIR)/auto/cvars.mk

$(BASEDIR)/auto/commonrules.mk: ; #prevent make from attempt to remake this file
include $(BASEDIR)/auto/commonrules.mk

VERSIONFILE := version.o

define VERSIONCODE
#include"iot_module.h" \\
IOT_LIBVERSION_DEFINE;
endef

#for external libs version is specified in Makefile
$(VERSIONFILE): Makefile
	echo '#include"iot_module.h"\nIOT_LIBVERSION_DEFINE;\n' | $(CXX) $(CXXFLAGS) -x c++ -o $@ -c -


ifdef IOTCFG_LOADED

ifeq ($(BUILDTYPE),m)								#build as dynamic module

ifeq ($(library-dynamic-sublibs),)
$(error No sublibs for provided for building $(dep) of $(FULLBUNDLENAME) as a module)
endif

DEPBUNDLES = 

all: $(BUILDTYPEFILE) rmcoredeps $(VERSIONFILE) $(library-name).$(SOSUFFIX)

$(library-name).$(SOSUFFIX): $(BASEDIR)/auto/bundles.cmd $(library-dynamic-sublibs)
	@# $(foreach dep,$(dependency-libraries),$(if $(filter y m,$(call getbuildtype1,$(subst /, ,$(dep)))),good,$(error Dependency library $(dep) of $(FULLBUNDLENAME) is disabled from build, enable it in bundles.cfg)))
	@# $(eval DEPBUNDLES = $(foreach dep,$(dependency-libraries),$(if $(filter m,$(call getbuildtype1,$(subst /, ,$(dep)))),$(dep),)))
	@set -e; for i in $(DEPBUNDLES) ; do echo Making $$i as dependency ; $(MAKE) -C ../../../$$i; done
	$(CXX) $(DYNLDFLAGS) $(library-dynamic-ldflags) $(foreach p,$(library-dynamic-sublib-dirs),-L$(p) -Wl,-rpath=$(MODULESDIR)/$(library-vendor)/$(library-dir)/$(library-name)/$(p)) $(foreach l,$(library-dynamic-sublibs),-l:$(notdir $(l))) -Wl,-soname=$(MODULESDIR)/$(library-vendor)/$(library-dir)/$(library-name).$(SOSUFFIX) $(foreach dep,$(DEPBUNDLES),../../../$(dep).$(SOSUFFIX)) -o $@ $(VERSIONFILE)
	@if [ ! -e ../$(library-name).$(SOSUFFIX) ] ; then ln -s $(library-name)/$(library-name).$(SOSUFFIX) ../$(library-name).$(SOSUFFIX) ; fi

manifest.part%json registry.part%json : manifest.json $(BASEDIR)/manifest_proc $(library-name).$(SOSUFFIX)
	@# $(foreach dep,$(dependency-libraries),$(if $(filter y m,$(call getbuildtype1,$(subst /, ,$(dep)))),good,$(error Dependency library $(dep) of $(FULLBUNDLENAME) is disabled from build, enable it in bundles.cfg)))
	@set -e; for i in $(dependency-libraries) ; do echo Making registry of $$i as dependency ; $(MAKE) -C ../../../$$i registry; done
	cd $(BASEDIR) && ./manifest_proc $(FULLBUNDLENAME) $(dependency-libraries)

else ifeq ($(BUILDTYPE),y)							#build for static inclusion

all: $(BUILDTYPEFILE) checkdepsstatic rmcoredeps

manifest.part%json registry.part%json : manifest.json $(BASEDIR)/manifest_proc
	@# $(foreach dep,$(dependency-libraries),$(if $(filter y m,$(call getbuildtype1,$(subst /, ,$(dep)))),good,$(error Dependency library $(dep) of $(FULLBUNDLENAME) is disabled from build, enable it in bundles.cfg)))
	@set -e; for i in $(dependency-libraries) ; do echo Making registry of $$i as dependency ; $(MAKE) -C ../../../$$i registry; done
	cd $(BASEDIR) && ./manifest_proc $(FULLBUNDLENAME) $(dependency-libraries)

else

all: rmcoredeps

ifeq ($(MAKELEVEL),0)
$(warning Library $(library-vendor)/$(library-dir)/$(library-name) is excluded from build in bundles.cfg file)
endif

endif  #BUILDTYPE
endif  #IOTCFG_LOADED

clean::
	$(RM) $(VERSIONFILE) $(library-name).$(SOSUFFIX)

