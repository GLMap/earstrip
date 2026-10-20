# earstrip

Polygon triangulation for map data. earstrip cuts ears along zig-zag strips, so what it writes is triangle strips,
about 1.7 indices per triangle instead of 3. When the rings of a polygon cross or touch, it covers the polygon with a
constrained Delaunay triangulation instead of keeping whatever triangles the ears left. It is a header-only C++17
library, and a WebAssembly build for JavaScript that takes the same arguments as
[earcut](https://github.com/mapbox/earcut).

On 3.7 million polygons of a real map it is 13% faster than earcut.hpp in C++ and 1.4 to 2.1 times faster than
earcut.js in the browser, and 6 to 7 times faster on polygons with thousands of holes. It is the triangulator of the
[GLMap](https://globus.software) map engine, which cuts every building, park and lake of an offline map on the phone
with it.

## C++

Copy [`include/earstrip.hpp`](include/earstrip.hpp) into your project. It needs C++17 or later and nothing but the
standard library, and compiles without warnings under `-Wall -Wextra -pedantic` in Clang and GCC.

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

A polygon is a container of rings, the outer ring first, each a container of points. A point can be anything with
`x` and `y` members, a `std::array` or `std::pair`, or your own type through `earstrip::point_traits`; flat
coordinates with the vertex index where each hole starts work too, as in earcut. `Index` is the type of the indices,
and a polygon can have as many vertices as it can count. A `Triangulator` keeps its buffers between runs, so once it
has seen the largest polygon it allocates nothing more; `shrink()` gives the memory back.

## JavaScript

```sh
npm install earstrip
```

```js
import earstrip from 'earstrip';

const triangles = earstrip([0, 0, 10, 0, 10, 10, 0, 10, 3, 3, 7, 3, 7, 7, 3, 7], [4]);
// earcut's arguments and the same kind of array back: three vertex indices per triangle
```

The [package's README](js/README.md) has the rest: the strips themselves, many polygons in one call, and direct calls
that write a polygon into the module's memory and hand back an index buffer for WebGL 2 or WebGPU.

## Polygons that are not simple

Ear clipping assumes a simple polygon, and map data often is not one. Rounding to a tile grid makes rings cross
themselves or each other, touch at a vertex, or push part of a hole outside the outer ring. On such a polygon the
ears run out, or the triangles they leave cover the wrong area.

earstrip notices that and covers the polygon again with a constrained Delaunay triangulation of the ring edges. It
fills what lies inside the outer ring by the non-zero winding rule and outside every hole. Where two edges cross it
adds a point next to the crossing: `extraPoints()` has their coordinates, and index `n + k` in the strips is point
`k`. Polygons with more than 128 rings go there directly, because joining that many holes to the boundary takes the
ears quadratic time. On the map polygons the ears cannot cut exactly, the triangles differ from an even-odd fill of
the rings on 0.15% of the pixels on average; earcut's on 31%.

`run()` says how a polygon was covered: `Exact`, `Constrained`, or `HolesLeftOut` when a hole could not be joined and
the constrained triangulation did no better. `setExtraPoints(false)` keeps every index an input vertex, and
`setConstrained(false)` turns the constrained triangulation off; polygons that would need it are then cut again with
their crossings split into loops, each filled by the even-odd rule, and the last corners are cut by force
(`Approximate`). With `setForcedCuts(false)` earstrip stops there instead (`Refused`).

## How it works

The ideas behind earstrip and what each of them saves are told step by step in
[Cutting map polygons into triangles](https://globus.software/blog/2026/10/16/cutting-map-polygons/).

Only a reflex vertex can stop a corner from being an ear, so the ear test reads the dents of the polygon alone. A
branch-free pass marks the dents inside the ear's triangle, and only those get an exact check; past 32 dents they are
sorted into buckets along the longer axis, and a test reads only the buckets its triangle spans. After an ear is cut,
its neighbours are tried first, and every ear cut there adds one index to the strip. Convex rings, triangles and
quads need no tests at all.

Holes are joined to the outer ring by Eberly's bridges, rightmost first, with the cases tile grids produce: a vertex
lying on the ray, and a hole that touches the outer ring at a vertex or inside an edge. The predicates are exact for
integer coordinates within ±2²⁵ in the ears and for 32-bit integers in the constrained triangulation, which rounds
other input to a 2³⁰ grid over the polygon's bounding box.

## Numbers

OpenStreetMap polygons in tile coordinates, one thread of an Apple M4 Pro, best of 7 runs:

| | earstrip | earcut |
| --- | ---: | ---: |
| 3,740,425 polygons of the Masovian Voivodeship at zoom 13, C++ | 490 ms | 552 ms |
| the same in WebAssembly, inside an engine, V8 / JavaScriptCore | 601 / 566 ms | 930 / 843 ms |
| the same through the npm package, one call per polygon | 766 / 807 ms | 1,103 / 1,290 ms |
| the same through the package, all polygons in one direct call | 635 / 619 ms | |
| 56,472 polygons of world map tiles, up to 11,675 holes in one, C++ | 459 ms | 2,892 ms |

earcut here is earcut.hpp 3.2.4 in C++ and WebAssembly and earcut.js 3.2.4 through JavaScript.
[BENCHMARKS.md](BENCHMARKS.md) has every number of the same run, with zoom 9 and the wrong pixels; below is how to
get them yourself.

## Building

```sh
make test                          # C++ tests
cd js && EMSDK=<emsdk> ./build.sh  # WebAssembly module, then: node test/test.mjs
```

## Checking the numbers

The polygons are in the [bench-data-1](https://github.com/GLMap/earstrip/releases/tag/bench-data-1) release, 147 MB
packed; [DATA.md](DATA.md) describes them. They are OpenStreetMap data under the ODbL. earcut.hpp and earcut.js
3.2.4 come from [earcut](https://github.com/mapbox/earcut).

```sh
make data                                     # downloads them into data/, checks data/SHA256SUMS
make bench/bench EARCUT=<dir with earcut.hpp>
bench/bench data/z13.bin -r 7                 # C++ rows and wrong pixels; also z9, world, gom4old
bench/bench data/world.bin -r 7 --no-constrained          # the ears alone
node js/test/test.mjs data/z13.bin            # the WebAssembly build against the native results, every polygon
node js/bench/bench.mjs data/z13.bin --earcut <earcut>/src/earcut.js     # package rows; bun for JavaScriptCore
EMSDK=<emsdk> EARCUT=<dir with earcut.hpp> bench/wasm/build.sh
node --expose-gc --max-old-space-size=12000 bench/wasm/bench.mjs data/z13.bin   # the two WebAssembly rows
```

## License

ISC, see [LICENSE](LICENSE). The benchmark polygons are OpenStreetMap data under the ODbL, see [DATA.md](DATA.md).
