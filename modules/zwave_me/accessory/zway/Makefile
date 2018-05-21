#version is integer from 0 to 255
library-version := 2
#patchlevel is integer from 0 (from 1 if version is 0) to 255
library-patchlevel := 3
#revision is integer from 0 to 65535 and can be incremented everytime when version or patchlevel is changed OR can be used as sublevel of patchlevel
library-revision := 7

#'any' or set of items from list: linux, mswin, darwin, unix (any unix clone including linux and darwin), android?
library-platform := linux

library-shared-data := ZDDX translations config

dependency-libraries :=

#if lib can be built as shared library, provide list of source libraries (.so), archives (.a) and/or object files for such build
library-dynamic-sublibs = $(wildcard $(PLATFORMNAME)/$(PLATFORMARCH)/*.so) $(wildcard $(PLATFORMNAME)/$(PLATFORMARCH)/*.so.*)
library-dynamic-sublib-dirs = $(PLATFORMNAME)/$(PLATFORMARCH)
library-dynamic-ldflags := -larchive -lcrypto

#if lib can be built for static inclusion, provide list of archives (.a) and/or object files for such build
#library-static-sublibs :=


include ../../../../auto/librules.mk





