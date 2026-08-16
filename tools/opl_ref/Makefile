# Host-only build for the OPL reference rig. Plain g++, no cmake -- matches
# tools/fm_ref/ and tools/sid_ref/'s own build shape.
#
#   ./fetch_nuked_opl3.sh   # once: pull Nuked-OPL3 into nuked/ (gitignored)
#   make                    # build ./nuked_render ./nuked_dump

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall
INCLUDES  = -Inuked -I../host_render

all: nuked_render nuked_dump

nuked_render: nuked_render.cpp nuked/opl3.c nuked/opl3.h ../host_render/wav_writer.h
	@test -f nuked/opl3.h || { echo "nuked/ missing -- run ./fetch_nuked_opl3.sh first"; exit 1; }
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ nuked_render.cpp nuked/opl3.c

# nuked_dump #includes nuked/opl3.c directly (not linked) to reach its
# file-local static tables (mt[]/kslrom[]/kslshift[]) -- see its own header
# comment. Only ever included from this one translation unit.
nuked_dump: nuked_dump.cpp nuked/opl3.c nuked/opl3.h
	@test -f nuked/opl3.h || { echo "nuked/ missing -- run ./fetch_nuked_opl3.sh first"; exit 1; }
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ nuked_dump.cpp

clean:
	rm -f nuked_render nuked_dump

# Removes the fetched third-party source too. `make clean` deliberately does
# not, so a rebuild doesn't need the network.
distclean: clean
	rm -rf nuked

.PHONY: all clean distclean
