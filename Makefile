CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-sign-compare

test: test/test
	./test/test

test/test: test/test.cpp include/earstrip.hpp
	$(CXX) $(CXXFLAGS) -Iinclude test/test.cpp -o $@

clean:
	rm -f test/test

.PHONY: test clean bench/bench data

# Native benchmark; EARCUT=<dir with earcut.hpp> adds earcut.hpp to the comparison.
bench/bench: bench/bench.cpp include/earstrip.hpp
	$(CXX) -std=c++17 -O3 -Iinclude $(if $(EARCUT),-DWITH_EARCUT -I$(EARCUT)) bench/bench.cpp -o $@

# The polygons the benchmarks and the README numbers use (OpenStreetMap data, ODbL: see DATA.md), from the
# bench-data-1 release, checked against data/SHA256SUMS.
DATA_URL ?= https://github.com/GLMap/earstrip/releases/download/bench-data-1
DATA := data/z13.bin data/z9.bin data/world.bin data/gom4old.bin
SHA256 := $(shell command -v sha256sum >/dev/null 2>&1 && echo sha256sum || echo shasum -a 256)

data: $(DATA)
	cd data && $(SHA256) -c SHA256SUMS

data/%.bin:
	curl -fL --retry 3 -o $@.tar.xz $(DATA_URL)/$*.bin.tar.xz
	tar -xJf $@.tar.xz -C data
	rm $@.tar.xz
