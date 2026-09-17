# Cross-compileren naar Windows vanaf Linux met MinGW-w64.
#
#   make            bouwt de 32-bit en 64-bit probe
#   make probe32    alleen 32-bit (voor een 32-bit doelproces)
#   make probe64    alleen 64-bit
#   make clean

CXX32 := i686-w64-mingw32-g++
CXX64 := x86_64-w64-mingw32-g++

# -static zodat er geen libstdc++/libgcc-DLL's meegeleverd hoeven te worden.
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
            -fno-exceptions -fno-rtti -static -static-libgcc -static-libstdc++
LDLIBS   := -lpsapi -lversion

BUILD  := build
COMMON := src/common/target.cpp src/common/rtti.cpp src/common/derive.cpp
PROBE  := src/probe/main.cpp src/probe/selftest.cpp $(COMMON)

.PHONY: all probe32 probe64 clean
all: probe32 probe64

probe32: $(BUILD)/zp-probe32.exe
probe64: $(BUILD)/zp-probe64.exe

$(BUILD)/zp-probe32.exe: $(PROBE) | $(BUILD)
	$(CXX32) $(CXXFLAGS) -o $@ $(PROBE) $(LDLIBS)

$(BUILD)/zp-probe64.exe: $(PROBE) | $(BUILD)
	$(CXX64) $(CXXFLAGS) -o $@ $(PROBE) $(LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

# --- testdoel -------------------------------------------------------------
# Een synthetisch proces met dezelfde structuurvorm als de game, zodat de
# afleidingslogica te controleren is zonder dat Generals hoeft te draaien.
.PHONY: test
$(BUILD)/fake_game64.exe: tests/fake_game.cpp | $(BUILD)
	$(CXX64) $(CXXFLAGS) -o $@ $<

test: $(BUILD)/fake_game64.exe $(BUILD)/zp-probe64.exe
	./tests/run_test.sh
