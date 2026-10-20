// earstrip and earcut.hpp 3.2.4 in one WebAssembly module (build.sh), driven from JavaScript, on data/z13.bin or
// data/z9.bin (the files with the expected results); checks both against the native results first.
// Usage: node --expose-gc --max-old-space-size=12000 bench.mjs <polygons.bin> [-r repeats], or bun bench.mjs ...
//   all-in-wasm: one call loops over every polygon already in the module's memory
//   per call:    JavaScript copies each polygon into the module's memory and calls runOne, as a drop-in for a
//                JavaScript triangulator
import { readFileSync } from 'node:fs';
const args = process.argv.slice(2);
let repeats = 7, modulePath = new URL('./module.mjs', import.meta.url).href;
for (let i = 1; i < args.length; ++i) {
    if (args[i] === '-r') repeats = +args[++i];
    else if (args[i] === '--module') modulePath = args[++i];
}
const gc = globalThis.gc ?? (typeof Bun !== 'undefined' ? () => Bun.gc(true) : () => {});
const M = await (await import(modulePath)).default();
const buf = readFileSync(args[0]);
const ab = buf.buffer, base = buf.byteOffset;
const head = new Uint32Array(ab, base, 4);
const P = head[1], R = head[2], N = head[3];
let off = base + 16;
const polyRingStart = new Uint32Array(ab, off, P + 1); off += 4 * (P + 1);
const ringPointStart = new Uint32Array(ab, off, R + 1); off += 4 * (R + 1);
const xy = new Int16Array(ab, off, 2 * N); off += 4 * N;
const hashes = new Uint32Array(ab, off, P); off += 4 * P;
const earcutHashes = new Uint32Array(ab, off, P);

let maxPoints = 0, maxRings = 0;
for (let p = 0; p < P; ++p) {
    const r0 = polyRingStart[p], r1 = polyRingStart[p + 1];
    maxPoints = Math.max(maxPoints, ringPointStart[r1] - ringPointStart[r0]);
    maxRings = Math.max(maxRings, r1 - r0);
}
// every allocation first: views stay valid once memory stops growing
const xyPtr = M._wasmAlloc(4 * N), rpsPtr = M._wasmAlloc(4 * (R + 1)), prsPtr = M._wasmAlloc(4 * (P + 1)), hashPtr = M._wasmAlloc(4 * P);
const oneXy = M._wasmAlloc(4 * maxPoints), oneStarts = M._wasmAlloc(4 * (maxRings + 1));
M._runOne(oneXy, oneStarts, 0);
const H16 = new Int16Array(M.HEAPU32.buffer), HU16 = new Uint16Array(M.HEAPU32.buffer), HU32 = new Uint32Array(M.HEAPU32.buffer);
H16.set(xy, xyPtr >> 1);
HU32.set(ringPointStart, rpsPtr >> 2);
HU32.set(polyRingStart, prsPtr >> 2);

M._runAll(xyPtr, rpsPtr, prsPtr, P, hashPtr);
let mismatch = 0;
for (let p = 0; p < P; ++p) mismatch += HU32[(hashPtr >> 2) + p] !== hashes[p];
M._runAllEarcut(xyPtr, rpsPtr, prsPtr, P, hashPtr);
let earcutMismatch = 0;
for (let p = 0; p < P; ++p) earcutMismatch += HU32[(hashPtr >> 2) + p] !== earcutHashes[p];
console.log(`${P} polygons, ${N} vertices; WebAssembly and native results differ for earstrip ${mismatch}, earcut.hpp ${earcutMismatch}`);

// per-call inputs prepared outside the timing, the same views the JS benchmark passes
const views = new Array(P), starts = new Array(P);
for (let p = 0; p < P; ++p) {
    const r0 = polyRingStart[p], r1 = polyRingStart[p + 1], v0 = ringPointStart[r0];
    views[p] = xy.subarray(2 * v0, 2 * ringPointStart[r1]);
    const s = new Uint32Array(r1 - r0 + 1);
    for (let r = r0; r <= r1; ++r) s[r - r0] = ringPointStart[r] - v0;
    starts[p] = s;
}
// results stay in wasm memory, as the JS benchmark leaves them where each triangulator puts them
const runOne = M._runOne, stripsSize = M._stripsSize, runOneEarcut = M._runOneEarcut;
function perCall() {
    let sink = 0;
    const t = performance.now();
    for (let p = 0; p < P; ++p) {
        H16.set(views[p], oneXy >> 1);
        HU32.set(starts[p], oneStarts >> 2);
        runOne(oneXy, oneStarts, starts[p].length - 1);
        sink += stripsSize();
    }
    return performance.now() - t;
}
function perCallEarcut() {
    let sink = 0;
    const t = performance.now();
    for (let p = 0; p < P; ++p) {
        H16.set(views[p], oneXy >> 1);
        HU32.set(starts[p], oneStarts >> 2);
        sink += runOneEarcut(oneXy, oneStarts, starts[p].length - 1);
    }
    return performance.now() - t;
}
function allIn(fn) {
    const t = performance.now();
    fn(xyPtr, rpsPtr, prsPtr, P, 0);
    return performance.now() - t;
}
const runs = [
    ['earstrip    all-in-wasm', () => allIn(M._runAll)],
    ['earcut.hpp  all-in-wasm', () => allIn(M._runAllEarcut)],
    ['earstrip    per call   ', perCall],
    ['earcut.hpp  per call   ', perCallEarcut],
];
const res = runs.map(() => []);
for (let r = 0; r < repeats; ++r) {
    // rotate the order so no variant always runs first
    for (let k = 0; k < runs.length; ++k) {
        const i = (k + r) % runs.length;
        gc();
        res[i].push(runs[i][1]());
    }
}
for (let i = 0; i < runs.length; ++i)
    console.log(`${runs[i][0]} best of ${repeats} ${Math.min(...res[i]).toFixed(1).padStart(7)} ms  [${res[i].map((v) => v.toFixed(0)).join(' ')}]`);
