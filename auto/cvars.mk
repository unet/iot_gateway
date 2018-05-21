ifeq ($(strip $(BASEDIR)),)
BASEDIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../)
endif

CC := gcc
CXX := g++
#LD := ld

#list of modules to compile
MODULESDIR := modules
INCLUDEDIR := include
SOSUFFIX := so
COREDEPSFILE := .kdeps

INCLUDEDIRS := -I$(BASEDIR) -I$(BASEDIR)/$(INCLUDEDIR) -I$(BASEDIR)/libuv/include -I$(BASEDIR)/json-c

#CFLAGS = -Wall -Wstrict-overflow=2 -g -O0 -fPIC -rdynamic -fno-strict-aliasing -I. -Iinclude -pthread -Ilibuv/include
#CFLAGS_NDEBUG = -Wall -Wstrict-overflow=2 -O2 -DNDEBUG -I. -Iinclude -pthread -Ilibuv/include
CFLAGS := -Wall -Wstrict-overflow=2 -g -O0 -pthread -fdata-sections -ffunction-sections $(INCLUDEDIRS)
CXXFLAGS = $(CFLAGS) -fno-rtti -std=c++17

DYNCFLAGS := -fPIC
DYNLDFLAGS := -g -shared

LDFLAGS := -g -pthread -Wl,--export-dynamic,--gc-sections -Llibuv/.libs -Ljson-c/.libs
#LDLIBS = -lpthread -ldl -Wl,-static,-l:libuv/.libs/libuv.a,-call_shared
LDLIBS := -lpthread -ldl -l:libuv.a -l:libjson-c.a

ifeq ($(shell uname -m),armv6l)
LDLIBS += -latomic
endif


ifdef SystemRoot
#windows
LDFLAGS += -L/usr/x86_64-w64-mingw32/lib
LDLIBS += -lws2_32
endif

define newlinechar :=


endef

#determine current platform
PLATFORMNAME := linux
#linux is in this group
PLATFORMGROUP := unix

#PLATFORMARCH will be selected here. It must have values from list (using linux notation): x86_64 i386 armv6l
ifeq ($(OS),Windows_NT)
	PLATFORMNAME := mswin
	PLATFORMGROUP := mswin
	PLATFORMARCH := i686
	ifeq ($(PROCESSOR_ARCHITEW6432),AMD64)
		PLATFORMARCH := x86_64
	else
		ifeq ($(PROCESSOR_ARCHITECTURE),AMD64)
			PLATFORMARCH := x86_64
		endif
	endif
else
	TMP_UNAME_S := $(shell uname -s)
	ifeq ($(TMP_UNAME_S),Darwin)
		PLATFORMNAME := darwin
	endif
	undefine TMP_UNAME_S
	PLATFORMARCH := $(shell uname -m)
endif

#custom function to check current platform against list of supported
is_platform_supported = $(if $(filter any,$1),$(PLATFORMNAME),$(if $(filter $(PLATFORMNAME) $(PLATFORMGROUP),$1),$(PLATFORMNAME),))

#custom function to reverse word list
reverse = $(if $(word 2,$1),$(call reverse,$(wordlist 2,$(words $1),$1)) $(firstword $1),$1)

#function to read file contents with compatibility code for old Make (<4.2)
ifeq ($(firstword $(sort 4.2 $(MAKE_VERSION))),4.2)
readfileval = $(strip $(file < $1))
readfilecont = $(file < $1)
else
readfileval = $(strip $(shell if [ -e $1 ]; then cat $1; fi))
readfilecont = $(subst __NEWLINE_STUB__,$(newlinechar),$(shell if [ -e $1 ]; then cat $1 | sed 's,$$,__NEWLINE_STUB__,'; fi))
endif

COMMACHAR=,
EMPTYVAR=
SPACECHAR=$(EMPTYVAR) $(EMPTYVAR)
