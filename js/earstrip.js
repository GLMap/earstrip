// earstrip for JavaScript: the C++ triangulator compiled to WebAssembly, with an earcut-compatible call.
// https://github.com/GLMap/earstrip
//
// Copyright (c) 2026 Evgen Bodunov
// SPDX-License-Identifier: ISC

import wasmBase64 from './dist/wasm.js';

function decode(base64) {
    if (typeof Buffer !== 'undefined') return new Uint8Array(Buffer.from(base64, 'base64'));
    const text = atob(base64);
    const bytes = new Uint8Array(text.length);
    for (let i = 0; i < text.length; ++i) bytes[i] = text.charCodeAt(i);
    return bytes;
}

const imports = {
    env: { emscripten_notify_memory_growth() {} },
    wasi_snapshot_preview1: { fd_write: () => 0, fd_close: () => 0, fd_seek: () => 0 },
};

let ex = null;
let memory = null;
let f64 = null, u32 = null, u8 = null;
let layout = 0; // where the module publishes its buffers, in u32 units

function start(instance) {
    ex = instance.exports;
    memory = ex.memory;
    ex._initialize();
    layout = ex.layoutBuffer() >> 2;
    views();
}

// Typed views of the module memory, made again whenever it grows. Growing detaches the old buffer, which leaves its
// views empty: checking a view's length is much cheaper than reading memory.buffer.
function views() {
    if (f64 && f64.length) return;
    f64 = new Float64Array(memory.buffer);
    u32 = new Uint32Array(memory.buffer);
    u8 = new Uint8Array(memory.buffer);
}

const bytes = decode(wasmBase64);
// Compiling synchronously lets earstrip() be called right after the import; where that is not allowed (a browser
// main thread may refuse it), `ready` resolves once the module is compiled.
export let ready;
try {
    start(new WebAssembly.Instance(new WebAssembly.Module(bytes), imports));
    ready = Promise.resolve();
} catch (e) {
    ready = WebAssembly.instantiate(bytes, imports).then((r) => start(r.instance));
}

function load(data, holeIndices, dim) {
    if (!ex) throw new Error('earstrip: the WebAssembly module is not compiled yet, await ready first');
    const holes = holeIndices ? holeIndices.length : 0;
    const ptr = ex.reserve(data.length, holes);
    views();
    f64.set(data, ptr >> 3);
    if (holes) u32.set(holeIndices, ex.holeBuffer() >> 2);
    return holes;
}

// Triangulates a polygon given as flat coordinates (dim numbers per vertex, x and y first) and the vertex index where
// each hole starts, the same arguments earcut takes. Returns an array of vertex indices, three per triangle, each
// counter-clockwise (positive area with y up). Every index is an input vertex.
export default function earstrip(data, holeIndices, dim = 2) {
    if (!ex) throw new Error('earstrip: the WebAssembly module is not compiled yet, await ready first');
    const n = data.length, h = holeIndices ? holeIndices.length : 0;
    if (!u32.length) views();
    if (u32[layout + 1] < n || u32[layout + 3] < h) {
        ex.grow(n, h);
        views();
    }
    f64.set(data, u32[layout] >> 3);
    if (h) u32.set(holeIndices, u32[layout + 2] >> 2);
    ex.run(Math.floor(n / dim), dim, h);
    if (!u32.length) views();
    const out = u32[layout + 4] >> 2, count = u32[layout + 5];
    // a plain array, as earcut returns: cheaper to make than a typed one in V8 for the few indices of most polygons
    const triangles = new Array(count);
    for (let i = 0; i < count; ++i) triangles[i] = u32[out + i];
    return triangles;
}

// The same polygon as triangle strips: { indices, ends, clockwise, points } with the indices of all strips one after
// another, the end of each strip in them, per strip whether its first triangle runs clockwise, and the x, y pairs of
// points added where rings cross: index n + k, n being the input vertex count, is point k.
export function strips(data, holeIndices, dim = 2) {
    const holes = load(data, holeIndices, dim);
    const count = ex.strip(Math.floor(data.length / dim), dim, holes);
    views();
    const indexCount = ex.stripIndexCount(), added = ex.extraPointCount();
    const i = ex.stripIndices() >> 2, e = ex.stripEnds() >> 2, c = ex.stripClockwise(), p = ex.extraPoints() >> 3;
    return {
        indices: u32.slice(i, i + indexCount),
        ends: u32.slice(e, e + count),
        clockwise: u8.slice(c, c + count),
        points: f64.slice(p, p + 2 * added),
    };
}

// Triangulates many polygons in one call. data holds the coordinates of all of them; polygon k has the vertices
// [polygonStarts[k], polygonStarts[k + 1]) and the holes holeIndices[holeStarts[k] .. holeStarts[k + 1]), counted
// from its own first vertex. Returns { indices, offsets }: indices count from the first vertex of data, and polygon
// k's triangles are indices[offsets[k] .. offsets[k + 1]).
export function batch(data, polygonStarts, holeIndices, holeStarts, dim = 2) {
    const polygons = polygonStarts.length - 1;
    load(data, holeIndices, dim);
    const s = ex.reserveBatch(polygons);
    views();
    u32.set(polygonStarts, s >> 2);
    if (holeStarts) u32.set(holeStarts, ex.batchHoleStarts() >> 2);
    else u32.fill(0, ex.batchHoleStarts() >> 2, (ex.batchHoleStarts() >> 2) + polygons + 1);
    const count = ex.triangulateBatch(polygons, dim, 0);
    views();
    const out = ex.output() >> 2, off = ex.offsets() >> 2;
    return { indices: u32.slice(out, out + count), offsets: u32.slice(off, off + polygons + 1) };
}

// How the last polygon was covered: 'exact', 'holes-left-out', 'approximate' (it is not simple), 'refused' or
// 'constrained' (by the constrained triangulation: its rings cross or touch, or it has more than 128 rings).
export function lastStatus() {
    return ['exact', 'holes-left-out', 'approximate', 'refused', 'constrained'][ex.lastStatus()];
}

// When a polygon runs out of ears and the constrained triangulation cannot take it, cut it anyway (the default) or
// give up and leave it partly covered.
export function setForcedCuts(on) {
    ex.setForcedCuts(on ? 1 : 0);
}

// Polygons the ears cannot finish go to the constrained triangulation (the default); off, to forced cuts.
export function setConstrained(on) {
    ex.setConstrained(on ? 1 : 0);
}

// Frees the module's buffers, which otherwise keep the size of the largest polygon seen. WebAssembly memory never
// shrinks, but what is freed is used again.
export function shrink() {
    ex.shrink();
    views();
}

// The direct calls: the caller writes into the module's memory and reads the results there, with no copies and,
// polygon by polygon, no allocation:
//
//     const { coords, holes } = direct.input(vertexCount, holeCount); // write the polygon into these
//     const r = direct.run(vertexCount, holeCount, 'restart');          // or 'strips', 'triangles'
//     gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, 0, r.u32, r.indexStart, r.indexCount);
//
// The input views and the result stay valid until the next earstrip call.
// The input views of direct.input() and where the buffers were when they were made: the buffers move when the module
// grows, and also when earstrip(), strips(), batch() or shrink() reallocate them.
let inputCache = null, inputAt = [0, 0, 0, 0];
const MODES = { triangles: 0, strips: 1, restart: 2 };

// The result of direct.run(): offsets into views of the whole module memory, and views of the parts made on demand.
// One object, reused by every call; a class, because V8 stores into an object literal with getters far more slowly.
class DirectResult {
    constructor() {
        this.u32 = null;
        this.u8 = null;
        this.f64 = null;
        this.indexStart = 0; // the indices: u32[indexStart .. indexStart + indexCount)
        this.indexCount = 0;
        this.endsStart = 0; // strips: the end of each strip, u32[endsStart .. endsStart + stripCount)
        this.stripCount = 0;
        this.clockwiseStart = 0; // strips: whether each strip's first triangle runs clockwise, u8[clockwiseStart ..]
        this.pointsStart = 0; // points added where rings cross, x, y pairs: f64[pointsStart .. + 2 * pointCount)
        this.pointCount = 0;
    }
    get indices() {
        return this.u32.subarray(this.indexStart, this.indexStart + this.indexCount);
    }
    get ends() {
        return this.u32.subarray(this.endsStart, this.endsStart + this.stripCount);
    }
    get clockwise() {
        return this.u8.subarray(this.clockwiseStart, this.clockwiseStart + this.stripCount);
    }
    get points() {
        return this.f64.subarray(this.pointsStart, this.pointsStart + 2 * this.pointCount);
    }
}
const result = new DirectResult();

export const direct = {
    // Views to write a polygon into: coords for vertexCount vertices of dim numbers each, holes for holeCount hole
    // starts. They span the whole buffers and are the same objects from call to call until the buffers move.
    input(vertexCount, holeCount = 0, dim = 2) {
        if (!ex) throw new Error('earstrip: the WebAssembly module is not compiled yet, await ready first');
        const n = vertexCount * dim;
        if (!u32.length) views();
        if (u32[layout + 1] < n || u32[layout + 3] < holeCount) {
            ex.grow(n, holeCount);
            views();
        }
        const at = inputAt;
        if (!inputCache || !inputCache.coords.length || at[0] !== u32[layout] || at[1] !== u32[layout + 1] ||
            at[2] !== u32[layout + 2] || at[3] !== u32[layout + 3]) {
            for (let i = 0; i < 4; ++i) at[i] = u32[layout + i];
            inputCache = { coords: f64.subarray(at[0] >> 3, (at[0] >> 3) + at[1]), holes: u32.subarray(at[2] >> 2, (at[2] >> 2) + at[3]) };
        }
        return inputCache;
    },
    // Triangulates the polygon in the input. mode 'restart' (default): one index buffer for TRIANGLE_STRIP with
    // primitive restart (WebGL 2, WebGPU with a uint32 strip index format), 0xFFFFFFFF between strips, every triangle
    // counter-clockwise; 'strips': the strips with their ends and windings; 'triangles': a list, three indices per
    // counter-clockwise triangle, every index an input vertex. 'restart' and 'strips' may add points where rings cross.
    run(vertexCount, holeCount = 0, mode = 'restart', dim = 2) {
        ex.runDirect(vertexCount, dim, holeCount, MODES[mode] ?? 2);
        if (!u32.length) views();
        result.u32 = u32;
        result.u8 = u8;
        result.f64 = f64;
        result.indexStart = u32[layout + 4] >> 2;
        result.indexCount = u32[layout + 5];
        result.endsStart = u32[layout + 6] >> 2;
        result.stripCount = u32[layout + 7];
        result.clockwiseStart = u32[layout + 8];
        result.pointsStart = u32[layout + 9] >> 3;
        result.pointCount = u32[layout + 10];
        return result;
    },
    // Views to write a batch into: { coords, polygonStarts, holes, holeStarts }, as batch() takes them.
    batchInput(polygons, vertexCount, holeCount = 0, dim = 2) {
        if (!ex) throw new Error('earstrip: the WebAssembly module is not compiled yet, await ready first');
        ex.reserveAll(vertexCount * dim, holeCount, polygons);
        views();
        const c = u32[layout] >> 3, h = u32[layout + 2] >> 2, s = ex.batchStarts() >> 2, hs = ex.batchHoleStarts() >> 2;
        return {
            coords: f64.subarray(c, c + vertexCount * dim),
            holes: u32.subarray(h, h + holeCount),
            polygonStarts: u32.subarray(s, s + polygons + 1),
            holeStarts: u32.subarray(hs, hs + polygons + 1),
        };
    },
    // Every polygon of the batch input: { indices, offsets } as views, indices counting from the batch's first vertex.
    // output 'triangles' (default) or 'restart': one restart index buffer for the whole batch, polygon k's part
    // starting at offsets[k] (with a restart index unless it is the first). Every index is an input vertex.
    batch(polygons, dim = 2, output = 'triangles') {
        const count = ex.triangulateBatch(polygons, dim, output === 'restart' ? 1 : 0);
        views();
        const o = ex.output() >> 2, off = ex.offsets() >> 2;
        return { indices: u32.subarray(o, o + count), offsets: u32.subarray(off, off + polygons + 1) };
    },
};

// GeoJSON-style rings (arrays of [x, y, ...] points, outer ring first) as earstrip's arguments.
export function flatten(rings) {
    const vertices = [], holes = [];
    const dimensions = rings.length && rings[0].length ? rings[0][0].length : 2;
    let count = 0;
    for (let r = 0; r < rings.length; ++r) {
        if (r > 0) holes.push(count);
        for (const point of rings[r]) {
            for (let d = 0; d < dimensions; ++d) vertices.push(point[d]);
            count++;
        }
    }
    return { vertices, holes, dimensions };
}

earstrip.strips = strips;
earstrip.batch = batch;
earstrip.lastStatus = lastStatus;
earstrip.setForcedCuts = setForcedCuts;
earstrip.setConstrained = setConstrained;
earstrip.shrink = shrink;
earstrip.direct = direct;
earstrip.flatten = flatten;
earstrip.ready = ready;
