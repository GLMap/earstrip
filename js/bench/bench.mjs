// earstrip (WebAssembly, through the public calls) against earcut.js on every polygon of a PLY1 file.
// Both get the same Float64Array per polygon and return a new array of triangle indices each call.
// Usage: node bench/bench.mjs <polygons.bin> --earcut <path to earcut's ES module> [-r repeats]
import { readFileSync } from 'node:fs';
import earstrip, { batch, direct } from '../earstrip.js';

const args = process.argv.slice(2);
let repeats = 7, earcutPath = null;
for (let i = 1; i < args.length; ++i) {
    if (args[i] === '-r') repeats = +args[++i];
    else if (args[i] === '--earcut') earcutPath = args[++i];
}
const earcut = earcutPath ? (await import(earcutPath)).default : null;
const gc = globalThis.gc ?? (typeof Bun !== 'undefined' ? () => Bun.gc(true) : () => {});

const buf = readFileSync(args[0]);
const ab = buf.buffer, base = buf.byteOffset;
const [, P, R, N] = new Uint32Array(ab, base, 4);
let off = base + 16;
const prs = new Uint32Array(ab, off, P + 1); off += 4 * (P + 1);
const rps = new Uint32Array(ab, off, R + 1); off += 4 * (R + 1);
const xy = new Int16Array(ab, off, 2 * N);

// inputs prepared outside the timing
const all = new Float64Array(xy);
const data = new Array(P), holes = new Array(P);
const polygonStarts = new Uint32Array(P + 1), holeStarts = new Uint32Array(P + 1), holeList = [];
for (let p = 0; p < P; ++p) {
    const r0 = prs[p], r1 = prs[p + 1], v0 = rps[r0];
    data[p] = all.subarray(2 * v0, 2 * rps[r1]);
    const h = [];
    for (let r = r0 + 1; r < r1; ++r) h.push(rps[r] - v0);
    holes[p] = h.length ? h : undefined;
    polygonStarts[p] = v0;
    holeStarts[p] = holeList.length;
    holeList.push(...h);
}
polygonStarts[P] = rps[prs[P]];
holeStarts[P] = holeList.length;
const holeArray = new Uint32Array(holeList);

function time(fn) {
    gc();
    const t = performance.now();
    const sink = fn();
    return [performance.now() - t, sink];
}
const runs = {
    'earstrip, one call per polygon': () => {
        let sink = 0;
        for (let p = 0; p < P; ++p) sink += earstrip(data[p], holes[p]).length;
        return sink;
    },
    'earstrip.batch, one call for all': () => batch(all, polygonStarts, holeArray, holeStarts).indices.length,
    'direct.run, triangles': () => perPolygon('triangles'),
    'direct.run, strips': () => perPolygon('strips'),
    'direct.run, restart': () => perPolygon('restart'),
    'direct.batch, triangles': () => directBatch('triangles'),
    'direct.batch, restart': () => directBatch('restart'),
};
// the direct calls: each polygon written straight into the module's memory, results read as views
function perPolygon(mode) {
    let sink = 0;
    for (let p = 0; p < P; ++p) {
        const n = data[p].length >> 1, h = holes[p] ? holes[p].length : 0;
        const input = direct.input(n, h);
        input.coords.set(data[p]);
        if (h) input.holes.set(holes[p]);
        sink += direct.run(n, h, mode).indexCount;
    }
    return sink;
}
function directBatch(output) {
    const input = direct.batchInput(P, all.length >> 1, holeArray.length);
    input.coords.set(all);
    input.polygonStarts.set(polygonStarts);
    input.holes.set(holeArray);
    input.holeStarts.set(holeStarts);
    return direct.batch(P, 2, output).indices.length;
}
if (earcut)
    runs['earcut.js, one call per polygon'] = () => {
        let sink = 0;
        for (let p = 0; p < P; ++p) sink += earcut(data[p], holes[p]).length;
        return sink;
    };
const best = {};
const order = Object.entries(runs);
for (let r = 0; r < repeats; ++r)
    for (const [name, fn] of r & 1 ? [...order].reverse() : order) best[name] = Math.min(best[name] ?? Infinity, time(fn)[0]);
const engine = typeof Bun !== 'undefined' ? `Bun ${Bun.version} (JavaScriptCore)` : `Node ${process.version} (V8)`;
console.log(`${P} polygons, ${N} vertices, ${engine}, best of ${repeats}:`);
for (const [name, ms] of Object.entries(best)) console.log(`  ${name.padEnd(40)} ${ms.toFixed(1)} ms`);
if (earcut) {
    const e = best['earcut.js, one call per polygon'];
    console.log(`  earcut.js / earstrip: ${(e / best['earstrip, one call per polygon']).toFixed(2)}x per call, ${(e / best['earstrip.batch, one call for all']).toFixed(2)}x batch`);
}
