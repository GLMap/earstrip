CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-sign-compare

test: test/test
	./test/test

test/test: test/test.cpp include/earstrip.hpp
	$(CXX) $(CXXFLAGS) -Iinclude test/test.cpp -o $@

clean:
	rm -f test/test

.PHONY: test clean bench/bench

# Native benchmark; EARCUT=<dir with earcut.hpp> adds earcut.hpp to the comparison.
bench/bench: bench/bench.cpp include/earstrip.hpp
	$(CXX) -std=c++17 -O3 -Iinclude $(if $(EARCUT),-DWITH_EARCUT -I$(EARCUT)) bench/bench.cpp -o $@
