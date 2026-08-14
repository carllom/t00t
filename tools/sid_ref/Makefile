# Host-only build for the chip module's reference rig (module_chip.md §11.1, F0).
# Plain g++, no cmake -- the rig has no reason to need one, and this mirrors
# tools/fm_ref/Makefile.
#
#   ./fetch_resid.sh     # once: pull reSID's core into resid/ (gitignored)
#   make                 # -> ./resid_render ./resid_dump ./resid_probe
#
# C++20 is required: fetch_resid.sh generates siddefs.h with reSID 1.0's
# consteval/constinit spellings (see configure.ac's own defaults).

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wall
# reSID 1.0-pre1 warnings that are upstream's business, not ours, and would
# otherwise bury a real warning in our own code:
#   template-id-cdtor            dac.h's `DAC<bits>()` constructor spelling
#   deprecated-enum-float-conversion  sid.cc's FIR_N*clock_freq/sample_freq
CXXFLAGS += -Wno-template-id-cdtor -Wno-deprecated-enum-float-conversion
INCLUDES  = -I. -Iresid

RESID_SRC = resid/envelope.cc resid/extfilt.cc resid/filter.cc resid/pot.cc \
            resid/sid.cc resid/version.cc resid/voice.cc resid/wave.cc
RESID_DEP = $(RESID_SRC) $(wildcard resid/*.h)

# version.cc wants the autotools VERSION define; fetch_resid.sh pins 1.0-pre1.
RESID_DEFS = -DVERSION='"1.0-pre1"'

SHARED = sidreg.h ../host_render/wav32.h

all: resid_render resid_dump resid_probe

resid_render: resid_render.cpp $(RESID_DEP) $(SHARED)
	@test -f resid/sid.h || { echo "resid/ missing -- run ./fetch_resid.sh first"; exit 1; }
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(RESID_DEFS) -o $@ resid_render.cpp $(RESID_SRC)

resid_dump: resid_dump.cpp $(RESID_DEP) sidreg.h
	@test -f resid/sid.h || { echo "resid/ missing -- run ./fetch_resid.sh first"; exit 1; }
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(RESID_DEFS) -o $@ resid_dump.cpp $(RESID_SRC)

resid_probe: resid_probe.cpp $(RESID_DEP) sidreg.h
	@test -f resid/sid.h || { echo "resid/ missing -- run ./fetch_resid.sh first"; exit 1; }
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(RESID_DEFS) -o $@ resid_probe.cpp $(RESID_SRC)

clean:
	rm -f resid_render resid_dump resid_probe

# Removes fetched third-party sources too. `make clean` deliberately does not,
# so a rebuild doesn't need the network.
distclean: clean
	rm -rf resid

.PHONY: all clean distclean
