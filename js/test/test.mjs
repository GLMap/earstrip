// JavaScript tests: small cases, then (given a PLY1 file with expected hashes) the WebAssembly build against the
// native results on every polygon of it.
// Usage: node test/test.mjs [polygons.bin]
import { readFileSync } from 'node:fs';
import earstrip, { batch, flatten, lastStatus, setForcedCuts, strips } from '../earstrip.js';

let failures = 0;
function check(name, ok, detail = '') {
    console.log(`${name.padEnd(44)} ${ok ? 'PASS' : 'FAIL'} ${detail}`);
    failures += !ok;
}

function area(data, tri, dim = 2) {
    let sum = 0;
    for (let k = 0; k < tri.length; k += 3) {
        const a = tri[k] * dim, b = tri[k + 1] * dim, c = tri[k + 2] * dim;
        sum += ((data[b] - data[a]) * (data[c + 1] - data[a + 1]) - (data[b + 1] - data[a + 1]) * (data[c] - data[a])) / 2;
    }
    return sum;
}

const square = [0, 0, 10, 0, 10, 10, 0, 10];
let t = earstrip(square);
check('square', t.length === 6 && area(square, t) === 100, `${t}`);
const withHole = [...square, 3, 3, 7, 3, 7, 7, 3, 7];
t = earstrip(withHole, [4]);
check('square with a hole', t.length === 24 && area(withHole, t) === 84 && lastStatus() === 'exact');
const cw = [0, 0, 0, 10, 10, 10, 10, 0];
t = earstrip(cw);
check('clockwise ring, counter-clockwise triangles', area(cw, t) === 100);
const xyz = [0, 0, 5, 10, 0, 5, 10, 10, 5, 0, 10, 5];
t = earstrip(xyz, null, 3);
check('three dimensions', t.length === 6 && area(xyz, t, 3) === 100);
const bowTie = [0, 0, 10, 10, 10, 0, 0, 10];
t = earstrip(bowTie);
check('bow tie: approximate, not refused', lastStatus() === 'approximate' && t.length > 0);
setForcedCuts(false);
earstrip(bowTie);
check('bow tie without forced cuts: refused', lastStatus() === 'refused');
setForcedCuts(true);
const s = strips(withHole, [4]);
check('strips', s.ends.length > 0 && s.ends[s.ends.length - 1] === s.indices.length);
const f = flatten([[[0, 0], [10, 0], [10, 10], [0, 10]], [[3, 3], [7, 3], [7, 7], [3, 7]]]);
check('flatten', earstrip(f.vertices, f.holes, f.dimensions).length === 24);
const b = batch(new Float64Array([...square, ...withHole]), new Uint32Array([0, 4, 12]), new Uint32Array([4]), new Uint32Array([0, 0, 1]));
check('batch', b.offsets[1] === 6 && b.offsets[2] === 30 && b.indices[6] >= 4);
const lonlat = [];
for (let i = 0; i < 20000; ++i) {
    const a = (2 * Math.PI * i) / 20000;
    lonlat.push(21.0122 + 0.01 * Math.cos(a), 52.2297 + 0.006 * Math.sin(a));
}
const s0 = performance.now();
t = earstrip(lonlat);
check('20,000 lon/lat vertices', t.length === 3 * 19998, `${(performance.now() - s0).toFixed(1)} ms`);

// Every polygon of a PLY1 file against the native hashes, forced cuts off as in the file.
if (process.argv[2]) {
    const buf = readFileSync(process.argv[2]);
    const ab = buf.buffer, base = buf.byteOffset;
    const [, P, R, N] = new Uint32Array(ab, base, 4);
    let off = base + 16;
    const prs = new Uint32Array(ab, off, P + 1); off += 4 * (P + 1);
    const rps = new Uint32Array(ab, off, R + 1); off += 4 * (R + 1);
    const xy = new Int16Array(ab, off, 2 * N); off += 4 * N;
    const hashes = new Uint32Array(ab, off, P);
    const fnv = (h, v) => {
        for (let i = 0; i < 4; ++i) h = Math.imul(h ^ ((v >>> (8 * i)) & 0xff), 16777619) >>> 0;
        return h;
    };
    setForcedCuts(false);
    let mismatches = 0;
    for (let p = 0; p < P; ++p) {
        const r0 = prs[p], r1 = prs[p + 1], v0 = rps[r0];
        const data = xy.subarray(2 * v0, 2 * rps[r1]);
        const holes = [];
        for (let r = r0 + 1; r < r1; ++r) holes.push(rps[r] - v0);
        const st = strips(data, holes);
        const status = lastStatus();
        let h = fnv(2166136261, status === 'refused' ? 1 : status === 'holes-left-out' ? 2 : 0);
        if (status !== 'refused') {
            for (const i of st.indices) h = fnv(h, i);
            for (const e of st.ends) h = fnv(h, e);
        }
        mismatches += h !== hashes[p];
    }
    setForcedCuts(true);
    check(`${P} polygons against the native results`, mismatches === 0, `${mismatches} differ`);
}

console.log(failures ? `FAILED: ${failures}` : 'OK');
process.exit(failures ? 1 : 0);
