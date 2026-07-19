# Запасной вариант сборки без CMake: make -> build/graphrank
CXX      ?= g++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -pthread

SRC := src/main.cpp src/csv_import.cpp src/grid_build.cpp src/leaderrank.cpp

build/graphrank: $(SRC) src/*.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) -o $@

.PHONY: clean test
clean:
	rm -rf build

test: build/graphrank
	./tests/run_tests.sh build/graphrank
