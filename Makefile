CXX ?= c++
CPPFLAGS += -Isrc
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread
LDLIBS += -lcurl -pthread

COMMON = src/benchmark.cpp src/system.cpp src/postgres.cpp src/analyze.cpp
HEADERS = src/server_opt.hpp src/json.hpp

.PHONY: all test sanitize clean
all: build/server-opt

build:
	mkdir -p build

build/server-opt: src/main.cpp $(COMMON) $(HEADERS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/main.cpp $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

build/tests: tests/test_server_opt.cpp $(COMMON) $(HEADERS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_server_opt.cpp $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

test: build/tests build/server-opt
	./build/tests
	./build/server-opt audit --output results/local-audit.json >/dev/null

sanitize:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="-O1 -g -std=c++17 -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer" test

clean:
	rm -rf build
