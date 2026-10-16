# earstrip

Polygon triangulation by ear clipping that writes **triangle strips**: a header-only C++17 library and a
WebAssembly build for JavaScript with the same call as [earcut](https://github.com/mapbox/earcut).

It is the triangulator of the [GLMap](https://globus.software) map engine, where it cuts every building, park and
lake of an offline map on the phone; on the 4.4 million polygons of our test set the library writes exactly the
strips the engine does. Strips take 1.7 to 2 indices per triangle instead of 3, and earstrip is faster than earcut
natively and in the browser.

```cpp
#include "earstrip.hpp"

std::vector<std::vector<std::array<double, 2>>> polygon = {
    {{0, 0}, {10, 0}, {10, 10}, {0, 10}}, // outer ring
    {{3, 3}, {7, 3}, {7, 7}, {3, 7}},     // a hole
};
earstrip::Triangulator<uint32_t> t;      // reuse it: buffers only grow
t.run(polygon);
t.strips();     // indices of all strips, one after another
t.stripEnds();  // where each strip ends
std::vector<uint32_t> list;
t.triangles(list); // or as a list, three indices per triangle
```

```js
import earstrip from 'earstrip';

const triangles = earstrip([0, 0, 10, 0, 10, 10, 0, 10, 3, 3, 7, 3, 7, 7, 3, 7], [4]);
// [vertex indices, three per triangle], the same arguments as earcut(data, holeIndices, dim)
```

## What it does

- **Ear clipping that only tests dents.** Only a reflex vertex can block an ear, so a test reads the reflex
  vertices alone; above 32 of them they go into buckets along the longer axis, and the test is a branch-free count
  before any exact check.
- **Zig-zag strips.** After an ear is cut its neighbours are tried first, and every further ear adds one index to
  the strip. Convex rings, triangles and quads skip the ear tests.
- **Holes** are joined to the boundary by Eberly's bridges, rightmost first, with the cases tile grids produce: a
  vertex lying on the ray, a hole touching the outer ring at a vertex or inside an edge.
- **Broken polygons still get a fill.** When a ring crosses itself the ears run out. earstrip then cuts the
  polygon again from the start, split at every crossing into loops; it fills each loop that an even-odd fill would,
  and cuts corners as a last resort. `Status::Approximate` says when that happened, and `setForcedCuts(false)`
  makes it give up instead (`Status::Refused`), for engines with a fallback of their own.
- **Exact predicates for integer coordinates within ±2²⁵** (tile grids); other input is handled in double precision
  like any triangulator. Points can be anything with `.x`/`.y` members, `std::array`, `std::pair`, flat arrays with a
  stride, or your own `earstrip::point_traits`.

## Numbers

Every polygon of zoom levels 13 and 9 of the Masovian Voivodeship (OpenStreetMap data, tile coordinates), one
thread of an Apple M4 Pro, best of 5 to 7 runs.

| 3,740,425 polygons, 28.9 million vertices | earstrip | earcut |
| --- | ---: | ---: |
| C++, native (earcut.hpp 3.2.4) | 515 ms | 545 ms |
| WebAssembly, one call per polygon from JS, V8 / JSC | 875 / 934 ms | |
| WebAssembly, one batch call, V8 / JSC | 737 / 732 ms | |
| earcut.js 3.2.4, V8 / JSC | | 1,109 / 1,288 ms |
| Indices per triangle | 1.66 as strips (1.95 at zoom 9), 3 as a list | 3 |

At zoom 9 (683,742 polygons) the native times are 67 ms against 83 ms.

On the polygons that are not simple (76 at zoom 13, 152 at zoom 9), measured as the share of pixels where the
triangles differ from an even-odd fill of the rings on a 128 × 128 grid: earstrip 11% on average and 61% at
worst at zoom 13, earcut 33% and 483%; at zoom 9, 11% and 100% against 13% and 236%.

## Building

```sh
make test                          # C++ tests
make bench/bench EARCUT=<dir>      # native benchmark, optionally against earcut.hpp
cd js && EMSDK=<emsdk> ./build.sh  # WebAssembly module, then: node test/test.mjs
```

The benchmarks read polygons from a simple binary file (format at the top of `bench/bench.cpp`).

## License

ISC, see [LICENSE](LICENSE).
