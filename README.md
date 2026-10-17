# earstrip

Polygon triangulation by ear clipping that writes **triangle strips**, with a constrained Delaunay triangulation for
the polygons ears cannot cut: a header-only C++17 library and a WebAssembly build for JavaScript with the same call
as [earcut](https://github.com/mapbox/earcut).

It is the triangulator of the [GLMap](https://globus.software) map engine, where it cuts every building, park and
lake of an offline map on the phone; on the 5.8 million polygons of our test sets the library writes exactly the
strips the engine does, ears and constrained triangulation alike. Strips take 1.7 to 2 indices per triangle instead
of 3, and earstrip is faster than earcut natively and in the browser.

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

At zoom 9 (683,742 polygons) the native times are 67 ms against 83 ms. The constrained triangulation, added later,
covers 82 of the zoom-13 polygons and changes these times within the noise.

Polygons with many holes, the 56,472 polygons of GLMap's world map tiles, some with up to 11,675 holes (measured on
a busy machine, so slower than the table above): earstrip 518 ms, earstrip with ears alone 2,100 ms, earcut.hpp
3,194 ms.

On the polygons the ears cannot cut exactly, the share of pixels where the triangles differ from an even-odd fill of
the rings on a 128 × 128 grid, on average and at worst:

| | earstrip | earcut.hpp |
| --- | ---: | ---: |
| zoom 13, 82 polygons | 0.15%, 2.6% | 31%, 483% |
| zoom 9, 183 polygons | 0.7%, 24% | 12%, 236% |
| 1,468 polygons of a map with unrepaired rings | 0.11%, 11% | 4.7%, 848% |

Part of what remains is where non-zero winding fills more than even-odd does, a ring walked twice for example. The WebAssembly module is 70 KB, 27 KB with gzip.

## Building

```sh
make test                          # C++ tests
make bench/bench EARCUT=<dir>      # native benchmark, optionally against earcut.hpp
cd js && EMSDK=<emsdk> ./build.sh  # WebAssembly module, then: node test/test.mjs
```

The benchmarks read polygons from a simple binary file (format at the top of `bench/bench.cpp`).

## License

ISC, see [LICENSE](LICENSE).
