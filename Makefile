CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Iinclude
LDFLAGS  ?=

BIN   := tinweb
SRC   := $(wildcard src/*.cpp)
OBJ   := $(SRC:src/%.cpp=build/%.o)
DEPS  := $(OBJ:.o=.d)

.PHONY: all run clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

build:
	mkdir -p build

run: $(BIN)
	./$(BIN) --port 8080 --root www

clean:
	rm -rf build $(BIN)

-include $(DEPS)
