# Host-only build for the FM reference rig (fm2.md §3.1). Plain g++, no cmake --
# this machine has no cmake, and the rig has no reason to need one.
#
#   ./fetch_dexed.sh     # once: pull Dexed's msfa core into msfa/ (gitignored)
#   ./fetch_banks.sh     # once: pull the DX7 factory ROM banks into banks/
#   make                 # build ./dexed_render

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wno-unused-variable -Wno-unused-but-set-variable

# shim/ must come before msfa/ on the include path so shim/tuning.h wins; msfa/
# deliberately has no tuning.h (fetch_dexed.sh skips it -- it needs JUCE).
INCLUDES = -Ishim -Imsfa

MSFA_SRC = msfa/dx7note.cc msfa/env.cc msfa/exp2.cc msfa/fm_core.cc \
           msfa/fm_op_kernel.cc msfa/freqlut.cc msfa/lfo.cc msfa/pitchenv.cc \
           msfa/porta.cpp msfa/sin.cc

all: dexed_render

dexed_render: dexed_render.cpp $(MSFA_SRC) shim/tuning.h shim/libMTSClient.h wav32.h
	@test -f msfa/synth.h || { echo "msfa/ missing -- run ./fetch_dexed.sh first"; exit 1; }
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ dexed_render.cpp $(MSFA_SRC)

clean:
	rm -f dexed_render

# Removes fetched third-party sources too. `make clean` deliberately does not,
# so a rebuild doesn't need the network.
distclean: clean
	rm -rf msfa banks

.PHONY: all clean distclean
