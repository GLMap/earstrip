# earstrip

Polygon triangulation by ear clipping, compiled to WebAssembly. The default export takes the same arguments as
earcut and returns the same kind of result, so it can replace it in place:

```js
import earstrip from 'earstrip';

// a square with a square hole: flat coordinates, then the vertex index where each hole starts
const triangles = earstrip([0, 0, 10, 0, 10, 10, 0, 10, 2, 2, 8, 2, 8, 8, 2, 8], [4]);
// [i0, i1, i2, ...], three indices per counter-clockwise triangle
```

The module is inlined and compiles synchronously where the platform allows it; elsewhere `await ready` before the
first call.

## Triangle strips

The triangulator cuts ears along zig-zag strips, so the native result is a set of triangle strips, about 1.7 indices
per triangle on map polygons instead of 3:

```js
import { strips } from 'earstrip';

const { indices, ends, clockwise } = strips(vertices, holes);
// strip k is indices[ends[k - 1] ?? 0 .. ends[k]); clockwise[k] tells the winding of its first triangle
```

## Many polygons at once

```js
import { batch } from 'earstrip';

const { indices, offsets } = batch(vertices, polygonStarts, holeIndices, holeStarts);
// indices count from the first vertex of `vertices`; polygon k owns indices[offsets[k] .. offsets[k + 1])
```

One call for a whole tile avoids the per-call cost of crossing into WebAssembly.

## Polygons that are not simple

`lastStatus()` says how the last polygon was covered: `exact`; `holes-left-out` when a hole could not be joined;
`approximate` when the boundary crosses itself and the polygon was cut again with its crossings split into loops,
each filled only if it lies inside the rings by the even-odd rule; `refused` when that was turned off with
`setForcedCuts(false)`.

## Speed

3,740,425 polygons of GLMap's zoom 13 tiles, 28.9 million vertices, Apple M4 Pro, one thread:

| | V8 (Node) | JavaScriptCore |
|---|---:|---:|
| earstrip, one call per polygon | 875 ms | 934 ms |
| earstrip, one batch call | 737 ms | 732 ms |
| earcut.js 3.2.4 | 1,109 ms | 1,288 ms |

## Other helpers

- `flatten(rings)` turns GeoJSON-style rings into `{ vertices, holes, dimensions }`.
- `earstrip.d.ts` has the types.

The C++ header the module is built from works natively as well. ISC licence.
