ifeq ($(CXX),g++)
    EXTRA_FLAGS = --no-gnu-unique
else
    EXTRA_FLAGS =
endif

CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++2b -Wall -Wno-c++11-narrowing
INCLUDES = `pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon`

SRC    = main.cpp deco.cpp shape.cpp
TARGET = brasswindows.so

all: $(TARGET)

$(TARGET): $(SRC) globals.hpp deco.hpp shape.hpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(EXTRA_FLAGS) $(INCLUDES) $(SRC) -o $@

clean:
	rm -f ./$(TARGET)

.PHONY: all clean
