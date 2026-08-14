# Host-only build for the chip module's AY-3-8910/YM2149 reference rig
# (module_chip.md §12.1, AY-P0). Plain g++, no cmake -- mirrors tools/sid_ref/Makefile.
#
#   make    # -> ./ayumi_render ./ayumi_dump
#
# Unlike tools/sid_ref/, there is no fetch step: ayumi.c/ayumi.h are vendored
# directly at ayumi/ (MIT license, see ayumi/LICENSE) rather than pulled by a
# gitignored fetch script the way reSID's GPL-2 source has to be.

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall
INCLUDES  = -I. -I../host_render

SHARED = ayreg.h ../host_render/wav32.h ayumi/ayumi.h

all: ayumi_render ayumi_dump

ayumi_render: ayumi_render.cpp ayumi/ayumi.c $(SHARED)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ ayumi_render.cpp ayumi/ayumi.c -lm

# ayumi_dump #includes ayumi/ayumi.c directly (not linked) to reach its
# file-local static functions -- see ayumi_dump.cpp's own header comment.
ayumi_dump: ayumi_dump.cpp ayumi/ayumi.c $(SHARED)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ ayumi_dump.cpp -lm

clean:
	rm -f ayumi_render ayumi_dump

.PHONY: all clean
