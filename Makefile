CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Iinclude
LDFLAGS  ?=

BIN      := tinweb
SRC      := $(wildcard src/*.cpp)
OBJ      := $(SRC:src/%.cpp=build/%.o)
DEPS     := $(OBJ:.o=.d)
TEST_BIN := build/http_tests

.PHONY: all run test clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

build:
	mkdir -p build

$(TEST_BIN): tests/http_test.cpp src/http.cpp | build
	$(CXX) $(CXXFLAGS) -o $@ $^

test: $(TEST_BIN)
	./$(TEST_BIN)

run: $(BIN)
	./$(BIN) --port 8080 --root www

clean:
	rm -rf build $(BIN)

-include $(DEPS)
