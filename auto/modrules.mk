#determine base dir of source
BASEDIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../)

.DEFAULT_GOAL := all

$(BASEDIR)/auto/cvars.mk: ; #prevent make from attempt to remake this file
include $(BASEDIR)/auto/cvars.mk

$(BASEDIR)/auto/commonrules.mk: ; #prevent make from attempt to remake this file
include $(BASEDIR)/auto/commonrules.mk

#By default use all C and CPP files in directory as sources
ifeq ($(strip $(srcs)),)
srcs := $(filter %.cc %.C %.cpp %.c, $(wildcard *.*))
ifeq ($(strip $(srcs)),)
#still no sources
$(error No source files found)
endif
endif


objs := $(addsuffix .o, $(basename $(srcs)))



#regenerate deps file when any source changes
$(DEPSFILE) : $(srcs) $(BUILDTYPEFILE)
	@set -e; echo "Generating dependencies for $(FULLBUNDLENAME)..." ; $(RM) $@; $(CXX) $(CXXFLAGS) -MM -MP $(srcs) | sed -r 's,^([a-zA-Z0-9_]+)\.o[ :]+,\1.o $@ : ,g' > $@



##By default use all headers in directory as common requisites
#ifeq ($(strip $(headers)),)
#headers := $(wildcard *.h)
#endif


ifdef IOTCFG_LOADED

ifeq ($(BUILDTYPE),m)								#build as dynamic module

CFLAGS += $(DYNCFLAGS)
DEPBUNDLES = 

all: $(BUILDTYPEFILE) rmcoredeps $(library-name).$(SOSUFFIX)

$(library-name).$(SOSUFFIX): $(objs) $(BASEDIR)/auto/bundles.cmd
	@# $(foreach dep,$(dependency-libraries),$(if $(filter y m,$(call getbuildtype1,$(subst /, ,$(dep)))),good,$(error Dependency library $(dep) of $(FULLBUNDLENAME) is disabled from build, enable it in bundles.cfg)))
	@# $(eval DEPBUNDLES = $(foreach dep,$(dependency-libraries),$(if $(filter m,$(call getbuildtype1,$(subst /, ,$(dep)))),$(dep),)))
	@set -e; for i in $(DEPBUNDLES) ; do echo Making $$i as dependency ; $(MAKE) -C ../../../$$i; done
	$(CXX) $(DYNLDFLAGS) -Wl,-soname=$(MODULESDIR)/$(library-vendor)/$(library-dir)/$(library-name).$(SOSUFFIX) $(foreach dep,$(DEPBUNDLES),../../../$(dep).$(SOSUFFIX)) -o $@ $(objs)
	@if [ ! -e ../$(library-name).$(SOSUFFIX) ] ; then ln -s $(library-name)/$(library-name).$(SOSUFFIX) ../$(library-name).$(SOSUFFIX) ; fi

##Check that build type for current library wasn't changed
#ifneq ($(BUILDTYPE),$(call readfileval,$(BUILDTYPEFILE)))
#$(file >$(BUILDTYPEFILE),$(BUILDTYPE))
#$(objs): $(BUILDTYPEFILE)
#endif


manifest.part%json registry.part%json : manifest.json $(BASEDIR)/manifest_proc $(library-name).$(SOSUFFIX)
	@# $(foreach dep,$(dependency-libraries),$(if $(filter y m,$(call getbuildtype1,$(subst /, ,$(dep)))),good,$(error Dependency library $(dep) of $(FULLBUNDLENAME) is disabled from build, enable it in bundles.cfg)))
	@set -e; for i in $(dependency-libraries) ; do echo Making registry of $$i as dependency ; $(MAKE) -C ../../../$$i registry; done
	cd $(BASEDIR) && ./manifest_proc $(FULLBUNDLENAME) $(dependency-libraries)


else ifeq ($(BUILDTYPE),y)							#build for static inclusion

all: $(BUILDTYPEFILE) checkdepsstatic rmcoredeps $(objs) $(COREDEPSFILE)

$(COREDEPSFILE):
	@echo "mod-objs += $(addprefix $(MODULESDIR)/$(FULLBUNDLENAME)/,$(objs))" > $(COREDEPSFILE)


##Check that build type for current library wasn't changed
#ifneq ($(BUILDTYPE),$(call readfileval,$(BUILDTYPEFILE)))
#$(file >$(BUILDTYPEFILE),$(BUILDTYPE))
#$(objs): $(BUILDTYPEFILE)
#endif

manifest.part%json registry.part%json : manifest.json $(BASEDIR)/manifest_proc $(objs)
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

$(patsubst %.c,%.o,$(filter %.c, $(srcs))) : %.o : %.c
	$(CC) $(CFLAGS) -fvisibility=hidden -o $@ -c $<

$(patsubst %.C,%.o,$(filter %.C, $(srcs))) : %.o : %.C
	$(CXX) $(CXXFLAGS) -fvisibility=hidden -o $@ -c $<

$(patsubst %.cc,%.o,$(filter %.cc, $(srcs))) : %.o : %.cc
	$(CXX) $(CXXFLAGS) -fvisibility=hidden -o $@ -c $<

$(patsubst %.cpp,%.o,$(filter %.cpp, $(srcs))) : %.o : %.cpp
	$(CXX) $(CXXFLAGS) -fvisibility=hidden -o $@ -c $<




clean::
	$(RM) *.o *.so $(DEPSFILE)


