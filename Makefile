# Against the Wall — C++20 prototype build.
# Zero external dependencies: the engine links only libc, libm and libdl
# (X11/GLX/OpenGL are resolved at runtime via dlopen).

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O3 -ffast-math -Wall -Wextra -Wshadow -Wno-unused-parameter
LDFLAGS  ?=
LDLIBS   := -ldl

BUILD := build
TARGET := $(BUILD)/atw
TEST   := $(BUILD)/atw_tests

CORE := src/core/window_x11.cpp src/core/window_headless.cpp
RENDER := src/render/gl.cpp src/render/renderer.cpp
GAME := src/game/game.cpp
AUDIO := src/audio/audio.cpp src/audio/audio_linux.cpp
AUDIO_WIN := src/audio/audio.cpp src/audio/audio_win32.cpp

all: $(TARGET) $(TEST)

$(BUILD):
	mkdir -p $(BUILD)

# Compiled per translation unit with header dependency tracking (-MMD -MP):
# the engine is header-heavy, so a touched .hpp must rebuild its users.
DEPFLAGS = -MMD -MP
OBJDIR := $(BUILD)/obj
TESTOBJDIR := $(BUILD)/testobj
OBJS := $(patsubst %.cpp,$(OBJDIR)/%.o,src/main.cpp $(CORE) $(RENDER) $(GAME) $(AUDIO))
TESTOBJS := $(patsubst %.cpp,$(TESTOBJDIR)/%.o,tests/tests.cpp $(CORE) $(RENDER) $(GAME) $(AUDIO))
DEPS := $(OBJS:.o=.d) $(TESTOBJS:.o=.d)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST): $(TESTOBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# The tests include headers as "src/...", hence the extra -I.
$(TESTOBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -I. -c $< -o $@

-include $(DEPS)

# Cross-compile a native Windows .exe (requires mingw-w64; see README).
WINDOWS_CXX ?= x86_64-w64-mingw32-g++
windows: | $(BUILD)
	mkdir -p $(BUILD)
	$(WINDOWS_CXX) $(CXXFLAGS) -mwindows -static -o $(BUILD)/atw.exe \
	  src/main.cpp src/core/window_win32.cpp src/core/window_headless.cpp \
	  src/render/gl.cpp src/render/renderer.cpp src/game/game.cpp \
	  src/audio/audio.cpp src/audio/audio_win32.cpp \
	  -lopengl32 -lgdi32 -luser32 -lwinmm -static-libgcc -static-libstdc++

run: $(TARGET)
	./$(TARGET)

demo: $(TARGET)
	./$(TARGET) --headless --frames 1200

test: $(TEST)
	./$(TEST)

clean:
	rm -rf $(BUILD)

.PHONY: all run demo test clean windows
