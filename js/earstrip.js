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

// Typed views of the module memory, made again whenever it grows.
function views() {
    if (f64 && f64.buffer === memory.buffer) return;
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
    if (u32.buffer !== memory.buffer) views();
    if (u32[layout + 1] < n || u32[layout + 3] < h) {
        ex.grow(n, h);
        views();
    }
    f64.set(data, u32[layout] >> 3);
    if (h) u32.set(holeIndices, u32[layout + 2] >> 2);
    ex.run(Math.floor(n / dim), dim, h);
    if (u32.buffer !== memory.buffer) views();
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

// The direct calls: the caller writes into the module's memory and reads the results there, with no copies. Every
// view stays valid until the next earstrip call; take a copy (slice) of what you keep.
//
//     const { coords, holes } = direct.input(vertexCount, holeCount); // write the polygon into these
//     const s = direct.strips(vertexCount, holeCount);                 // or restart() or triangles()
function inputViews(count, holeCount) {
    if (u32.buffer !== memory.buffer) views();
    if (u32[layout + 1] < count || u32[layout + 3] < holeCount) {
        ex.grow(count, holeCount);
        views();
    }
    const c = u32[layout] >> 3, h = u32[layout + 2] >> 2;
    return { coords: f64.subarray(c, c + count), holes: u32.subarray(h, h + holeCount) };
}

export const direct = {
    // Room for vertexCount vertices of dim numbers each and holeCount hole starts, as views to write into.
    input(vertexCount, holeCount = 0, dim = 2) {
        if (!ex) throw new Error('earstrip: the WebAssembly module is not compiled yet, await ready first');
        return inputViews(vertexCount * dim, holeCount);
    },
    // Strips of the polygon in the input: { indices, ends, clockwise, points }, as strips() returns, but views.
    strips(vertexCount, holeCount = 0, dim = 2) {
        const count = ex.strip(vertexCount, dim, holeCount);
        views();
        const indexCount = ex.stripIndexCount(), added = ex.extraPointCount();
        const i = ex.stripIndices() >> 2, e = ex.stripEnds() >> 2, c = ex.stripClockwise(), p = ex.extraPoints() >> 3;
        return {
            indices: u32.subarray(i, i + indexCount),
            ends: u32.subarray(e, e + count),
            clockwise: u8.subarray(c, c + count),
            points: f64.subarray(p, p + 2 * added),
        };
    },
    // One index buffer for TRIANGLE_STRIP with primitive restart (WebGL 2, WebGPU with stripIndexFormat 'uint32'):
    // 0xFFFFFFFF between strips, every triangle counter-clockwise. Added points: extraPoints().
    restart(vertexCount, holeCount = 0, dim = 2) {
        const count = ex.restart(vertexCount, dim, holeCount);
        views();
        const o = ex.output() >> 2;
        return u32.subarray(o, o + count);
    },
    // The triangle list, three counter-clockwise triangle's indices each; every index is an input vertex.
    triangles(vertexCount, holeCount = 0, dim = 2) {
        ex.run(vertexCount, dim, holeCount);
        if (u32.buffer !== memory.buffer) views();
        const o = u32[layout + 4] >> 2;
        return u32.subarray(o, o + u32[layout + 5]);
    },
    // Points the last strips() or restart() added, as x, y pairs.
    extraPoints() {
        views();
        const p = ex.extraPoints() >> 3;
        return f64.subarray(p, p + 2 * ex.extraPointCount());
    },
    // Room for a batch: { coords, polygonStarts, holes, holeStarts } to write into, as batch() takes them.
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
    // output 'triangles' (default) or 'restart': one restart index buffer for the whole batch; polygon k's part starts
    // at offsets[k], with a restart index unless it is the first.
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
