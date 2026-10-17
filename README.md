# earstrip

Polygon triangulation that is fast on clean polygons and right on broken ones: ear clipping that writes **triangle
strips**, and a **constrained Delaunay triangulation** for everything the ears cannot cut. A header-only C++17
library, and a WebAssembly build for JavaScript with two ways in: the same call as
[earcut](https://github.com/mapbox/earcut), and direct calls that write into the module's memory and hand back an
index buffer for WebGL 2 or WebGPU.

- **Faster than earcut.** On 3.7 million map polygons: 508 ms natively against 545 ms. In a map engine compiled to
  WebAssembly, with the polygons already in its memory, 600 ms in V8 and 592 ms in JavaScriptCore against 1,092 and
  1,261 ms for earcut.js. Through the earcut-compatible JavaScript call, 1.4 and 1.5 times as fast per polygon; with
  all polygons written straight into the module and returned as one restart index buffer, 1.7 and 1.9 times. On
  polygons with thousands of holes, 6.2 times natively and 7.4 times in V8.
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
import earstrip, { direct } from 'earstrip';

const triangles = earstrip([0, 0, 10, 0, 10, 10, 0, 10, 3, 3, 7, 3, 7, 7, 3, 7], [4]);
// [vertex indices, three per triangle], the same arguments as earcut(data, holeIndices, dim)

const { coords, holes } = direct.input(8, 1); // or write into the module's memory
coords.set([0, 0, 10, 0, 10, 10, 0, 10, 3, 3, 7, 3, 7, 7, 3, 7]);
holes.set([4]);
const r = direct.run(8, 1); // strips with 0xFFFFFFFF restarts, every triangle counter-clockwise
gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, r.u32, gl.STATIC_DRAW, r.indexStart, r.indexCount);
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
| zoom 13, 3,740,425 polygons, 28.9 million vertices: C++ (earcut.hpp 3.2.4) | 508 ms | 545 ms |
| the same in WebAssembly, loop inside the module, V8 / JavaScriptCore (earcut.hpp) | 600 / 592 ms | 922 / 839 ms |
| the same in WebAssembly, one call from JavaScript per polygon, V8 / JavaScriptCore | 678 / 710 ms | 999 / 946 ms |
| the same through the JavaScript package, one call per polygon (earcut.js 3.2.4) | 777 / 843 ms | 1,092 / 1,261 ms |
| the same through the package, all polygons in one call | 690 / 694 ms | |
| the same through the package's direct calls, one call per polygon, strips | 697 / 692 ms | |
| the same through the direct calls, all polygons in one call, restart index buffer | 644 / 651 ms | |
| zoom 9, 683,742 polygons: C++ | 69 ms | 83 ms |
| zoom 9 through the package, one call per polygon, V8 / JavaScriptCore | 112 / 117 ms | 161 / 160 ms |
| world map tiles, 56,472 polygons, 3.4 million vertices: C++ | 463 ms | 2,861 ms |
| world map tiles through the package, one call per polygon, V8 / JavaScriptCore | 663 / 641 ms | 4,895 / 4,040 ms |
| indices per triangle | 1.66 as strips (1.95 at zoom 9), 3 as a list | 3 |

The two WebAssembly rows are the case of an engine that keeps its geometry in WebAssembly memory: 16-bit tile
coordinates, strips left in the module, the JavaScript call copying only the input. The package's earcut-compatible
call takes coordinates as JavaScript numbers and returns a new array of triangle indices on every call; its direct
calls write the coordinates into the module and leave the result there, which is most of the way to the engine.

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
WebAssembly module is 68 KB, 27 KB with gzip.

## Building

```sh
make test                          # C++ tests
make bench/bench EARCUT=<dir>      # native benchmark, optionally against earcut.hpp
cd js && EMSDK=<emsdk> ./build.sh  # WebAssembly module, then: node test/test.mjs
```

The benchmarks read polygons from a simple binary file (format at the top of `bench/bench.cpp`).

## License

ISC, see [LICENSE](LICENSE).
