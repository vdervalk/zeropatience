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

# De game is 32-bit, dus de DLL en de injector moeten dat ook zijn. De probe
# bouwen we als 64-bit, want die kan een 32-bit proces gewoon uitlezen.
.PHONY: all probe32 probe64 trainer clean
all: probe32 probe64 trainer

trainer: $(BUILD)/zp-freeze.dll $(BUILD)/zp-trainer.exe $(BUILD)/zp-inject.exe

probe32: $(BUILD)/zp-probe32.exe
probe64: $(BUILD)/zp-probe64.exe

$(BUILD)/zp-probe32.exe: $(PROBE) | $(BUILD)
	$(CXX32) $(CXXFLAGS) -o $@ $(PROBE) $(LDLIBS)

$(BUILD)/zp-probe64.exe: $(PROBE) | $(BUILD)
	$(CXX64) $(CXXFLAGS) -o $@ $(PROBE) $(LDLIBS)

FREEZE := src/freeze/dll.cpp src/freeze/resolve.cpp $(COMMON)
INJECT := src/inject/main.cpp src/common/target.cpp src/common/inject.cpp
GUI    := src/gui/main.cpp src/common/target.cpp src/common/inject.cpp

# -static-libgcc en -static-libstdc++ zodat er geen runtime-DLL's naast
# hoeven te staan; de DLL wordt in een vreemd proces geladen en kan niet op
# zoekpaden vertrouwen.
$(BUILD)/zp-freeze.dll: $(FREEZE) | $(BUILD)
	$(CXX32) $(CXXFLAGS) -shared -o $@ $(FREEZE) $(LDLIBS) 		-Wl,--enable-stdcall-fixup

$(BUILD)/zp-inject.exe: $(INJECT) | $(BUILD)
	$(CXX32) $(CXXFLAGS) -o $@ $(INJECT) $(LDLIBS)

# De manifest-resource vraagt om verhoogde rechten en om de moderne
# besturingselementen. -mwindows onderdrukt het consolevenster.
$(BUILD)/app.res: src/gui/app.rc src/gui/app.manifest | $(BUILD)
	i686-w64-mingw32-windres -I src/gui -O coff -o $@ src/gui/app.rc

$(BUILD)/zp-trainer.exe: $(GUI) $(BUILD)/app.res | $(BUILD)
	$(CXX32) $(CXXFLAGS) -mwindows -o $@ $(GUI) $(BUILD)/app.res 		$(LDLIBS) -lcomctl32

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
