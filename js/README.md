# earstrip

Polygon triangulation compiled to WebAssembly: ear clipping into triangle strips, and a constrained Delaunay
triangulation for the polygons ears cannot cut. 1.4 to 2.1 times as fast as earcut on map polygons, 6 to 7 times on
polygons with thousands of holes, and right where earcut paints the wrong area on broken rings.

There are two ways to call it:

- **In place of earcut.** `earstrip(data, holes, dim)` takes earcut's arguments and returns the same kind of array.
  Nothing else changes.
- **Direct calls.** The polygon is written straight into the module's memory and the result is read from there:
  the library copies nothing and allocates nothing per polygon, and the result can be an index buffer ready for
  WebGL 2 or WebGPU.

The module is inlined and compiles synchronously where the platform allows it; elsewhere `await ready` before the
first call. It is 68 KB, 27 KB with gzip.

## In place of earcut

```js
import earstrip from 'earstrip';

// a square with a square hole: flat coordinates, then the vertex index where each hole starts
const triangles = earstrip([0, 0, 10, 0, 10, 10, 0, 10, 2, 2, 8, 2, 8, 8, 2, 8], [4]);
// [i0, i1, i2, ...], three indices per counter-clockwise triangle
```

The triangulator cuts ears along zig-zag strips, so its own result is a set of triangle strips, about 1.7 indices per
triangle on map polygons instead of 3:

```js
import { strips } from 'earstrip';

const { indices, ends, clockwise } = strips(vertices, holes);
// strip k is indices[ends[k - 1] ?? 0 .. ends[k]); clockwise[k] tells the winding of its first triangle
```

Many polygons in one call, which saves crossing into WebAssembly for each of them:

```js
import { batch } from 'earstrip';

const { indices, offsets } = batch(vertices, polygonStarts, holeIndices, holeStarts);
// indices count from the first vertex of `vertices`; polygon k owns indices[offsets[k] .. offsets[k + 1])
```

## Direct calls

```js
import { direct } from 'earstrip';

const { coords, holes } = direct.input(vertexCount, holeCount);
coords.set(vertices);  // x, y of each vertex
holes.set(holeStarts); // the vertex index where each hole starts
const r = direct.run(vertexCount, holeCount); // 'restart' by default, or 'strips', 'triangles'
// indices from vertexCount on are points added where rings cross: r.points, x, y pairs after the vertices
gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, r.u32, gl.STATIC_DRAW, r.indexStart, r.indexCount);
gl.drawElements(gl.TRIANGLE_STRIP, r.indexCount, gl.UNSIGNED_INT, 0);
```

`direct.input()` returns views of the module's input buffers; write the polygon at their start. `direct.run()`
returns the same object every time, with offsets into `r.u32`, `r.u8` and `r.f64`, views of the whole module memory:

- `'restart'`: one index buffer for `TRIANGLE_STRIP`, `0xFFFFFFFF` between strips. WebGL 2 always restarts the strip
  there; in WebGPU set `stripIndexFormat: 'uint32'` (`device.queue.writeBuffer(buffer, 0, r.u32, r.indexStart,
  r.indexCount)`). Every triangle is counter-clockwise: a strip that would start clockwise begins with its first
  index twice.
- `'strips'`: the strips themselves, `r.indices`, `r.ends`, `r.clockwise`, as `strips()` returns them.
- `'triangles'`: the list `earstrip()` returns, every index an input vertex.

`r.indices`, `r.ends`, `r.clockwise` and `r.points` make views on demand. The input views and the result stay valid
until the next earstrip call: upload or copy what you need before the next polygon.

A whole tile in one call: `direct.batchInput(polygons, vertexCount, holeCount)` gives views to write
`{ coords, polygonStarts, holes, holeStarts }` into, as `batch()` takes them, and `direct.batch(polygons, 2,
'restart')` returns `{ indices, offsets }` as views: one restart index buffer for the whole tile, indices counting from
its first vertex, polygon k from `offsets[k]` on. Here every index is an input vertex.

## Polygons that are not simple

Polygons whose rings cross or touch, with a hole that crosses the boundary or with more than 128 rings go to a
constrained Delaunay triangulation of the ring edges. It fills what lies inside the outer ring by non-zero winding
and outside every hole. Where rings cross it adds a point: `strips()` and the direct `'restart'` and `'strips'`
return those as `points`, x, y pairs, and index `n + k` (n input vertices) is point `k`. `earstrip()`, `batch()`,
`direct.batch()` and the direct `'triangles'` never add points; such a polygon is cut again from the start with its
crossings split into loops, each filled only if it lies inside the rings by the even-odd rule.

`lastStatus()` says how the last polygon was covered: `exact`; `constrained`; `holes-left-out` when a hole could not
be joined and the constrained triangulation did no better; `approximate` for the loops; `refused` when those were
turned off with `setForcedCuts(false)`. `setConstrained(false)` sends everything the ears cannot finish to the loops.

## Speed

OpenStreetMap polygons in tile coordinates, Apple M4 Pro, one thread, best of 7 runs, against earcut.js 3.2.4:

| | V8 (Node 26) | JavaScriptCore (Bun 1.3) |
|---|---:|---:|
| 3,740,425 polygons of zoom 13: `earstrip()`, one call per polygon | 766 ms | 807 ms |
| the same, all in one `batch()` call | 671 ms | 661 ms |
| the same, `direct.run()` per polygon, `'strips'` | 682 ms | 662 ms |
| the same, `direct.run()` per polygon, `'restart'` | 747 ms | 721 ms |
| the same, all in one `direct.batch()` call, `'restart'` | 635 ms | 619 ms |
| the same, earcut.js | 1,103 ms | 1,290 ms |
| 56,472 polygons of world map tiles, up to 11,675 holes: `earstrip()` | 662 ms | 633 ms |
| the same, earcut.js | 4,927 ms | 4,016 ms |

Beyond the triangulation itself, the time goes mostly into turning the strips into another layout: a list for
`earstrip()`, a restart buffer for `'restart'`. Copying the coordinates in and making the JavaScript array cost little
in V8, more in JavaScriptCore. `'strips'` hands the strips over as they are, and a batch also saves crossing into
WebAssembly for every polygon. With the polygons as 16-bit coordinates already in WebAssembly memory and the loop
inside the module, the same zoom 13 takes 601 ms in V8 and 566 ms in JavaScriptCore.

On the polygons the ears cannot cut exactly, 82 at zoom 13, the triangles differ from an even-odd fill of the rings
on 0.15% of the pixels on average and 2.6% at worst; earcut on 31% and 483%.

## Other helpers

- `flatten(rings)` turns GeoJSON-style rings into `{ vertices, holes, dimensions }`.
- `shrink()` frees the module's buffers, which otherwise keep the size of the largest polygon seen.
- `earstrip.d.ts` has the types.

The C++ header the module is built from works natively as well: [GLMap/earstrip](https://github.com/GLMap/earstrip)
on GitHub. How the ears are cut and what each idea saves, step by step:
[Cutting map polygons into triangles](https://globus.software/blog/2026/10/16/cutting-map-polygons/). ISC licence.
