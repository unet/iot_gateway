#cvars must be already included

ifeq ($(strip $(library-platform)),)
library-platform := any
endif

ifeq ($(call is_platform_supported,$(library-platform)),)
$(error Current platform $(PLATFORMNAME) is not supported for this library)
endif

REVDIRNAMES := $(call reverse,$(subst /, ,$(realpath $(CURDIR))))
BUILDTYPEFILE := .buildtype
DEPSFILE := .deps

library-name := $(word 1,$(REVDIRNAMES))
library-dir := $(word 2,$(REVDIRNAMES))
library-vendor := $(word 3,$(REVDIRNAMES))

$(if $(and $(library-name),$(library-dir),$(library-vendor)),,$(error Cannot determine full library name. Makefile run from incorrect directory?))

FULLBUNDLENAME := $(library-vendor)/$(library-dir)/$(library-name)

ifneq ($(MAKECMDGOALS),clean)
-include $(BASEDIR)/auto/bundles.cmd  #contains values for IOTCFG_ vars. include will trigger file rebuild if necessary
-include $(DEPSFILE) #include will trigger file rebuild if necessary
endif

#rule for rebuilding auto/bundles.cmd
$(BASEDIR)/auto/bundles.cmd: $(BASEDIR)/bundles.cfg
	@echo "Rebuilding auto/bundles.cmd"
	@sed -r 's,^\s*([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])/([a-zA-Z0-9_]*[a-zA-Z0-9])\s*=\s*([ymn])\s*$$,IOTCFG_\1__\2__\3 = \4,' $< >$@
	@echo "\nIOTCFG_LOADED := 1" >>$@

#BUILDTYPEFILE must be updated
$(BUILDTYPEFILE) : $(BASEDIR)/auto/bundles.cmd
	@# HIDE FROM SHELL $(eval $(call readfilecont,$(BASEDIR)/auto/bundles.cmd))
	@if [ -f "$(BUILDTYPEFILE)" ]; then read -r oldtype < $(BUILDTYPEFILE) ; if [ "$$oldtype" != "$(BUILDTYPE)" ] ; then echo Cleaning after change of build type; $(RM) *.o *.so ; fi ; fi
	@echo $(BUILDTYPE) > $(BUILDTYPEFILE)

getbuildtype = $(IOTCFG_$1__$2__$3)

#takes single argument as wordlist
getbuildtype1 = $(IOTCFG_$(word 1,$1)__$(word 2,$1)__$(word 3,$1))

BUILDTYPE = $(call getbuildtype,$(library-vendor),$(library-dir),$(library-name))



rmcoredeps:
	@$(RM) $(COREDEPSFILE)

#do nothing. necessary for modtime comparison only in 'registry'-derived rule
$(BASEDIR)/manifest_proc: ;

#when built as module check that dependency bundles are also built as either module or statically. add those built as modules to DEPBUNDLES
checkdeps:
	@# $(foreach dep,$(dependency-libraries),$(if $(filter y m,$(call getbuildtype1,$(subst /, ,$(dep)))),good,$(error Dependency library $(dep) of $(FULLBUNDLENAME) is disabled from build, enable it in bundles.cfg)))
	@# $(eval DEPBUNDLES = $(foreach dep,$(dependency-libraries),$(if $(filter m,$(call getbuildtype1,$(subst /, ,$(dep)))),$(dep),)))
	@set -e; for i in $(DEPBUNDLES) ; do echo Making $$i as dependency ; $(MAKE) -C ../../../$$i; done

#when built statically check that dependency bundles are also built statically
checkdepsstatic:
	@# $(foreach dep,$(dependency-libraries),$(if $(filter y,$(call getbuildtype1,$(subst /, ,$(dep)))),good,$(error Dependency library $(dep) of $(FULLBUNDLENAME) is disabled from build or built as module, enable it to be statically built in bundles.cfg)))

registry: manifest.part.json registry.part.json

CFLAGS += -DIOT_LIBVENDOR=$(library-vendor) -DIOT_LIBDIR=$(library-dir) -DIOT_LIBNAME=$(library-name) -DIOT_LIBVERSION=$(library-version) -DIOT_LIBPATCHLEVEL=$(library-patchlevel) -DIOT_LIBREVISION=$(library-revision)


clean::
	$(RM) $(BUILDTYPEFILE) $(COREDEPSFILE) registry.part.json manifest.part.json ../$(library-name).$(SOSUFFIX)


.PHONY : all clean rmcoredeps checkdeps checkdepsstatic registry
