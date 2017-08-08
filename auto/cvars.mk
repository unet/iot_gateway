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
KERNELDEPSFILE := .kdeps

INCLUDEDIRS := -I$(BASEDIR) -I$(BASEDIR)/$(INCLUDEDIR) -I$(BASEDIR)/libuv/include

#CFLAGS = -Wall -Wstrict-overflow=2 -g -O0 -fPIC -rdynamic -fno-strict-aliasing -I. -Iinclude -pthread -Ilibuv/include
#CFLAGS_NDEBUG = -Wall -Wstrict-overflow=2 -O2 -DNDEBUG -I. -Iinclude -pthread -Ilibuv/include
CFLAGS := -Wall -Wstrict-overflow=2 -g -O0 -pthread -fdata-sections -ffunction-sections $(INCLUDEDIRS)
CXXFLAGS = $(CFLAGS) -fno-rtti -std=c++11

DYNCFLAGS := -fPIC
DYNLDFLAGS := -g -shared


#custom function to reverse word list
reverse = $(if $(word 2,$1),$(call reverse,$(wordlist 2,$(words $1),$1)) $(firstword $1),$1)

#function to read file contents with compatibility code for old Make (<4.2)
ifeq ($(firstword $(sort 4.2 $(MAKE_VERSION))),4.2)
readfileval = $(read < $1)
else
readfileval = $(strip $(shell if [ -e $1 ]; then cat $1; fi))
endif

COMMACHAR=,
