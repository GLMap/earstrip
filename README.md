# earstrip

Polygon triangulation that is fast on clean polygons and right on broken ones: ear clipping that writes **triangle
strips**, and a **constrained Delaunay triangulation** for everything the ears cannot cut. A header-only C++17
library, and a WebAssembly build for JavaScript with the same call as [earcut](https://github.com/mapbox/earcut).

- **Faster than earcut.** On 3.7 million map polygons: 521 ms natively against 553 ms; in JavaScript 1.29 times as
  fast in V8 and 1.40 in JavaScriptCore, one call per polygon, and 1.62 and 1.80 times with all polygons in one call.
  On polygons with thousands of holes, 6.2 times natively and 7.4 times in V8.
- **Strips, not lists:** 1.66 indices per triangle on map polygons instead of 3.
- **Right on broken input.** Rings that cross themselves or each other, touch, or hold a hole outside the outer ring
  get a fill by a fixed rule instead of whatever the ears happened to cut. On such polygons from real map tiles, the
  triangles miss or overpaint 0.15% of the pixels on average, earcut 31%; at worst 2.6% against 483%.
- **Proven on real data.** It is the triangulator of the [GLMap](https://globus.software) map engine, which cuts every
  building, park and lake of an offline map on the phone with it; on the 5.8 million polygons of our test sets the
  library writes exactly the engine's strips, natively and in WebAssembly.
- **No allocation once warm:** the buffers grow to the largest polygon and are reused; `shrink()` frees them.

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

## How it works

- **Ear clipping that only tests dents.** Only a reflex vertex can block an ear, so a test reads the reflex
  vertices alone; above 32 of them they go into buckets along the longer axis, and the test is a branch-free count
  before any exact check.
- **Zig-zag strips.** After an ear is cut its neighbours are tried first, and every further ear adds one index to
  the strip. Convex rings, triangles and quads skip the ear tests.
- **Holes** are joined to the boundary by Eberly's bridges, rightmost first, with the cases tile grids produce: a
  vertex lying on the ray, a hole touching the outer ring at a vertex or inside an edge.
- **A constrained triangulation for what ears cannot do.** When rings cross or touch, the ears run out; a hole that
  crosses the boundary cannot be bridged; and past 128 rings bridging gets quadratic, because each bridge scans the
  boundary. Those polygons go to a constrained Delaunay triangulation of the ring edges (`Status::Constrained`). It
  fills what lies inside the outer ring by non-zero winding and outside every hole, and reroutes crossing edges
  through a grid point next to the crossing, which becomes an added point: `extraPoints()` has their coordinates,
  index `n + k` in the strips is point `k`. `setExtraPoints(false)` keeps every index an input vertex.
- **And without it** (`setConstrained(false)`, or when added points are not allowed), a polygon whose ears run out
  is cut again from the start, split at every crossing into loops; earstrip fills each loop that an even-odd fill
  would and cuts corners as a last resort (`Status::Approximate`). `setForcedCuts(false)` makes it give up instead
  (`Status::Refused`).
- **Exact predicates for integer coordinates within ±2²⁵** in the ears (tile grids) and for integers of 32 bits in
  the constrained triangulation; other input is rounded there to a 2³⁰ grid over the polygon's bounding box. Points
  can be anything with `.x`/`.y` members, `std::array`, `std::pair`, flat arrays with a stride, or your own
  `earstrip::point_traits`.
- **Memory.** A triangulator keeps its buffers between runs: 80 to 110 bytes per vertex of the largest polygon for
  the ears and about 230 for the constrained triangulation. After the first polygons a run allocates nothing.

## Numbers

OpenStreetMap polygons in tile coordinates: every polygon of zoom levels 13 and 9 of the Masovian Voivodeship, and
the polygons of GLMap's world map tiles, some with up to 11,675 holes. One thread of an Apple M4 Pro, best of 7
runs.

| | earstrip | earcut |
| --- | ---: | ---: |
| zoom 13, 3,740,425 polygons, 28.9 million vertices: C++ (earcut.hpp 3.2.4) | 521 ms | 553 ms |
| the same in JavaScript, one call per polygon, V8 / JavaScriptCore (earcut.js 3.2.4) | 850 / 914 ms | 1,095 / 1,276 ms |
| the same, all polygons in one call, V8 / JavaScriptCore | 675 / 708 ms | |
| zoom 9, 683,742 polygons: C++ | 70 ms | 84 ms |
| zoom 9 in JavaScript, one call per polygon, V8 / JavaScriptCore | 124 / 125 ms | 159 / 160 ms |
| world map tiles, 56,472 polygons, 3.4 million vertices: C++ | 467 ms | 2,892 ms |
| world map tiles in JavaScript, one call per polygon, V8 / JavaScriptCore | 663 / 646 ms | 4,873 / 4,004 ms |
| indices per triangle | 1.66 as strips (1.95 at zoom 9), 3 as a list | 3 |

On the world map tiles the ears alone take 1,851 ms: past 128 rings the constrained triangulation is what makes the
difference.

On the polygons the ears cannot cut exactly, the share of pixels where the triangles differ from an even-odd fill of
the rings on a 128 × 128 grid, on average and at worst:

| | earstrip | earcut.hpp |
| --- | ---: | ---: |
| zoom 13, 82 polygons | 0.15%, 2.6% | 31%, 483% |
| zoom 9, 183 polygons | 0.7%, 24% | 12%, 236% |
| 1,468 polygons of a map with unrepaired rings | 0.11%, 11% | 4.7%, 848% |

Part of what remains is where non-zero winding fills more than even-odd does, a ring walked twice for example. The
WebAssembly module is 73 KB, 28 KB with gzip.

## Building

```sh
make test                          # C++ tests
make bench/bench EARCUT=<dir>      # native benchmark, optionally against earcut.hpp
cd js && EMSDK=<emsdk> ./build.sh  # WebAssembly module, then: node test/test.mjs
```

The benchmarks read polygons from a simple binary file (format at the top of `bench/bench.cpp`).

## License

ISC, see [LICENSE](LICENSE).
