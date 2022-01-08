# on a mac need to set PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
# the code *requires* C++ 20 or later

LIBTINS = $(HOME)/src/libtins
CPPFLAGS += -I$(LIBTINS)/include
LDFLAGS += -L$(LIBTINS)/lib -ltins -lpcap
CXXFLAGS = -g -O0 -Wall -std=c++20 -I/opt/local/include
HDRS = ./movingmin.hpp ./flowDelay.hpp
DEPS = $(HDRS)
BINS = dlyloc
JUNK = 

CXX=clang++
JUNK += $(addsuffix .dSYM,$(BINS))

all: dlyloc 

.PHONY: clean distclean tags

dlyloc: dlyloc.cpp $(DEPS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -rf $(BINS) $(JUNK)

distclean: clean
