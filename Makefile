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

all: $(TARGET) $(TEST)

$(BUILD):
	mkdir -p $(BUILD)

$(TARGET): src/main.cpp $(CORE) $(RENDER) $(GAME) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST): tests/tests.cpp $(CORE) $(RENDER) $(GAME) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -I. -o $@ $^ $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

demo: $(TARGET)
	./$(TARGET) --headless --frames 1200

test: $(TEST)
	./$(TEST)

clean:
	rm -rf $(BUILD)

.PHONY: all run demo test clean
