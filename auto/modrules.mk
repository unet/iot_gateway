#determine base dir of source
BASEDIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../)

$(BASEDIR)/auto/cvars.mk: ; #prevent make from attempt to remake this file
include $(BASEDIR)/auto/cvars.mk

ifeq ($(strip $(library-platform)),)
library-platform := any
endif

#By default use all C and CPP files in directory as sources
ifeq ($(strip $(srcs)),)
srcs := $(filter %.cc %.C %.cpp %.c, $(wildcard *.*))
ifeq ($(strip $(srcs)),)
#still no sources
$(error No source files found)
endif
endif

objs := $(addsuffix .o, $(basename $(srcs)))


REVDIRNAMES := $(call reverse,$(subst /, ,$(realpath $(CURDIR))))
BUILDTYPEFILE := .buildtype
DEPSFILE := .deps

library-name := $(word 1,$(REVDIRNAMES))
library-dir := $(word 2,$(REVDIRNAMES))
library-vendor := $(word 3,$(REVDIRNAMES))

$(if $(and $(library-name),$(library-dir),$(library-vendor)),,$(error Cannot determine full library name. Makefile run from incorrect directory?))

.DEFAULT_GOAL := all

FULLBUNDLENAME := $(library-vendor)/$(library-dir)/$(library-name)


ifneq ($(MAKECMDGOALS),clean)
-include $(BASEDIR)/auto/bundles.cmd  #contains values for IOTCFG_ vars. include will trigger file rebuild if necessary
-include $(DEPSFILE) #include will trigger file rebuild if necessary
endif


#rule for rebuilding auto/bundles.cmd
$(BASEDIR)/auto/bundles.cmd: $(BASEDIR)/bundles.cfg
	@echo "Rebuilding auto/bundles.cmd"
	@sed -r 's,^\s*([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*([ymn])\s*$$,IOTCFG_\1__\2__\3 = \4,' $< >$@
	@echo "IOTCFG_LOADED := 1" >>$@

#regenerate deps file when any source changes
$(DEPSFILE) : $(srcs) $(BUILDTYPEFILE)
	@set -e; echo "Generating dependencies for $(FULLBUNDLENAME)..." ; $(RM) $@; $(CXX) $(CXXFLAGS) -MM -MP $(srcs) | sed -r 's,^([a-zA-Z0-9_]+)\.o[ :]+,\1.o $@ : ,g' > $@

#BUILDTYPEFILE must be updated
$(BUILDTYPEFILE) : $(BASEDIR)/auto/bundles.cmd
	@# HIDE FROM SHELL $(eval $(call readfilecont,$(BASEDIR)/auto/bundles.cmd))
	@if [ -f "$(BUILDTYPEFILE)" ]; then read -r oldtype < $(BUILDTYPEFILE) ; if [ "$$oldtype" != "$(BUILDTYPE)" ] ; then echo Cleaning after change of build type; $(RM) *.o *.so ; fi ; fi
	@echo $(BUILDTYPE) > $(BUILDTYPEFILE)

getbuildtype = $(IOTCFG_$1__$2__$3)

#takes single argument as wordlist
getbuildtype1 = $(IOTCFG_$(word 1,$1)__$(word 2,$1)__$(word 3,$1))

BUILDTYPE = $(call getbuildtype,$(library-vendor),$(library-dir),$(library-name))



##By default use all headers in directory as common requisites
#ifeq ($(strip $(headers)),)
#headers := $(wildcard *.h)
#endif


ifdef IOTCFG_LOADED

ifeq ($(BUILDTYPE),m)								#build as dynamic module

CFLAGS += $(DYNCFLAGS)
DEPBUNDLES = 

all: $(BUILDTYPEFILE) rmkerneldeps $(library-name).$(SOSUFFIX)

$(library-name).$(SOSUFFIX): $(objs) $(BASEDIR)/auto/bundles.cmd
	@# $(foreach dep,$(dependency-bundles),$(if $(filter y m,$(call getbuildtype1,$(subst /, ,$(dep)))),good,$(error Dependency library $(dep) of $(FULLBUNDLENAME) is disabled from build, enable it in bundles.cfg)))
	@# $(eval DEPBUNDLES = $(foreach dep,$(dependency-bundles),$(if $(filter m,$(call getbuildtype1,$(subst /, ,$(dep)))),$(dep),)))
	@set -e; for i in $(DEPBUNDLES) ; do echo Making $$i as dependency ; $(MAKE) -C ../../../$$i; done
	$(CXX) $(DYNLDFLAGS) -Wl,-soname=$(MODULESDIR)/$(library-vendor)/$(library-dir)/$(library-name).$(SOSUFFIX) $(foreach dep,$(DEPBUNDLES),../../../$(dep).$(SOSUFFIX)) -o $@ $(objs)
	@if [ ! -e ../$(library-name).$(SOSUFFIX) ] ; then ln -s $(library-name)/$(library-name).$(SOSUFFIX) ../$(library-name).$(SOSUFFIX) ; fi

##Check that build type for current library wasn't changed
#ifneq ($(BUILDTYPE),$(call readfileval,$(BUILDTYPEFILE)))
#$(file >$(BUILDTYPEFILE),$(BUILDTYPE))
#$(objs): $(BUILDTYPEFILE)
#endif

registry: manifest.part.json registry.part.json

manifest.part%json registry.part%json : manifest.json $(BASEDIR)/manifest_proc $(library-name).$(SOSUFFIX)
	@# $(foreach dep,$(dependency-bundles),$(if $(filter y m,$(call getbuildtype1,$(subst /, ,$(dep)))),good,$(error Dependency library $(dep) of $(FULLBUNDLENAME) is disabled from build, enable it in bundles.cfg)))
	@set -e; for i in $(dependency-bundles) ; do echo Making registry of $$i as dependency ; $(MAKE) -C ../../../$$i registry; done
	cd $(BASEDIR) && ./manifest_proc $(FULLBUNDLENAME) $(dependency-bundles)

else ifeq ($(BUILDTYPE),y)							#build for static inclusion

all: $(BUILDTYPEFILE) checkdepsstatic rmkerneldeps $(objs) $(KERNELDEPSFILE)

$(KERNELDEPSFILE):
	@echo "mod-objs += $(addprefix $(MODULESDIR)/$(FULLBUNDLENAME)/,$(objs))" > $(KERNELDEPSFILE)


##Check that build type for current library wasn't changed
#ifneq ($(BUILDTYPE),$(call readfileval,$(BUILDTYPEFILE)))
#$(file >$(BUILDTYPEFILE),$(BUILDTYPE))
#$(objs): $(BUILDTYPEFILE)
#endif

registry: manifest.part.json registry.part.json

manifest.part%json registry.part%json : manifest.json $(BASEDIR)/manifest_proc $(objs)
	@# $(foreach dep,$(dependency-bundles),$(if $(filter y m,$(call getbuildtype1,$(subst /, ,$(dep)))),good,$(error Dependency library $(dep) of $(FULLBUNDLENAME) is disabled from build, enable it in bundles.cfg)))
	@set -e; for i in $(dependency-bundles) ; do echo Making registry of $$i as dependency ; $(MAKE) -C ../../../$$i registry; done
	cd $(BASEDIR) && ./manifest_proc $(FULLBUNDLENAME) $(dependency-bundles)

else

all: rmkerneldeps

ifeq ($(MAKELEVEL),0)
$(warning Bundle $(library-vendor)/$(library-dir)/$(library-name) is excluded from build in bundles.cfg file)
endif

endif  #BUILDTYPE
endif  #IOTCFG_LOADED

CFLAGS += -DIOT_VENDOR=$(library-vendor) -DIOT_BUNDLEDIR=$(library-dir) -DIOT_BUNDLENAME=$(library-name) -DIOT_LIBVERSION=$(library-version) -DIOT_LIBPATCHLEVEL=$(library-patchlevel) -DIOT_LIBREVISION=$(library-revision)


rmkerneldeps:
	@$(RM) $(KERNELDEPSFILE)

#do nothing. necessary for modtime comparison only in 'registry'-derived rule
$(BASEDIR)/manifest_proc: ;

#$(objs): $(headers)

$(patsubst %.c,%.o,$(filter %.c, $(srcs))) : %.o : %.c
	$(CC) $(CFLAGS) -o $@ -c $<

$(patsubst %.C,%.o,$(filter %.C, $(srcs))) : %.o : %.C
	$(CXX) $(CXXFLAGS) -o $@ -c $<

$(patsubst %.cc,%.o,$(filter %.cc, $(srcs))) : %.o : %.cc
	$(CXX) $(CXXFLAGS) -o $@ -c $<

$(patsubst %.cpp,%.o,$(filter %.cpp, $(srcs))) : %.o : %.cpp
	$(CXX) $(CXXFLAGS) -o $@ -c $<


#when built as module check that dependency bundles are also built as either module or statically. add those built as modules to DEPBUNDLES
checkdeps:
	@# $(foreach dep,$(dependency-bundles),$(if $(filter y m,$(call getbuildtype1,$(subst /, ,$(dep)))),good,$(error Dependency library $(dep) of $(FULLBUNDLENAME) is disabled from build, enable it in bundles.cfg)))
	@# $(eval DEPBUNDLES = $(foreach dep,$(dependency-bundles),$(if $(filter m,$(call getbuildtype1,$(subst /, ,$(dep)))),$(dep),)))
	@set -e; for i in $(DEPBUNDLES) ; do echo Making $$i as dependency ; $(MAKE) -C ../../../$$i; done

#when built statically check that dependency bundles are also built statically
checkdepsstatic:
	@# $(foreach dep,$(dependency-bundles),$(if $(filter y,$(call getbuildtype1,$(subst /, ,$(dep)))),good,$(error Dependency library $(dep) of $(FULLBUNDLENAME) is disabled from build or built as module, enable it to be statically built in bundles.cfg)))

clean:
	$(RM) *.o *.so $(BUILDTYPEFILE) $(DEPSFILE) $(KERNELDEPSFILE) registry.part.json manifest.part.json


.PHONY : all clean rmkerneldeps checkdeps checkdepsstatic registry
