NATIVE_CC ?= i686-w64-mingw32-g++

ifeq (, $(shell which $(NATIVE_CC) 2>/dev/null))
$(error "Native GCC is missing. Please install it first. If it's path is custom, set it with export NATIVE_CC=<path to native gcc toolchain>")
endif

.PHONY: all clean

all: png2rgba

clean:
	@rm -f png2rgba

png2rgba: png2rgba.cpp
	@$(NATIVE_CC) -o2 -s -static -o $@ png2rgba.cpp -lpng -lz
