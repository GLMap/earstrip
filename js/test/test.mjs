// JavaScript tests: small cases, then (given a PLY1 file with expected hashes) the WebAssembly build against the
// native results on every polygon of it.
// Usage: node test/test.mjs [polygons.bin]
import { readFileSync } from 'node:fs';
import earstrip, { batch, direct, flatten, lastStatus, setConstrained, setForcedCuts, shrink, strips } from '../earstrip.js';

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
setForcedCuts(true);
// strips() lets the constrained triangulation add the crossing point; the earcut-compatible call does not
const bs = strips(bowTie);
check('bow tie strips: constrained, crossing added', lastStatus() === 'constrained' && bs.points.length === 2 && bs.points[0] === 5 && bs.points[1] === 5);
t = earstrip(bowTie);
check('bow tie triangles: input vertices only', lastStatus() !== 'constrained' && t.every((i) => i < 4));
// past 128 rings the ears are skipped
const grid = [0, 0, 410, 0, 410, 210, 0, 210], gridHoles = [];
for (let i = 0; i < 20; ++i)
    for (let j = 0; j < 10; ++j) {
        const x = 10 + 20 * i, y = 10 + 20 * j;
        gridHoles.push(grid.length / 2);
        grid.push(x, y, x, y + 10, x + 10, y + 10, x + 10, y);
    }
t = earstrip(grid, gridHoles);
check('200 holes: constrained', lastStatus() === 'constrained' && area(grid, t) === 410 * 210 - 200 * 100);
shrink();
const again = earstrip(grid, gridHoles);
check('shrink, then the same polygon', again.length === t.length && again.every((v, i) => v === t[i]));
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

// The direct calls give what the copying calls give; restart buffers decode to the same counter-clockwise triangles.
function decodeRestart(buffer) {
    const tris = [];
    let start = 0;
    for (let i = 0; i <= buffer.length; ++i) {
        if (i < buffer.length && buffer[i] !== 0xffffffff) continue;
        for (let k = start; k + 2 < i; ++k) {
            const a = buffer[k], b = buffer[k + 1], c = buffer[k + 2];
            if (a === b || b === c || a === c) continue;
            tris.push(((k - start) & 1) === 0 ? [a, b, c] : [b, a, c]);
        }
        start = i + 1;
    }
    return tris;
}
const key = (t) => [...t].sort((x, y) => x - y).join(',');
function sameDirect(name, data, holeList) {
    const n = data.length / 2, h = holeList.length;
    const easyTriangles = earstrip(data, holeList), easyStrips = strips(data, holeList);
    const write = () => {
        const input = direct.input(n, h);
        input.coords.set(data);
        input.holes.set(holeList);
    };
    const same = (a, b) => a.length === b.length && Array.prototype.every.call(a, (v, i) => v === b[i]);
    write();
    const tri = Array.from(direct.run(n, h, 'triangles').indices);
    write();
    const st = direct.run(n, h, 'strips');
    let good = same(tri, easyTriangles) && same(st.indices, easyStrips.indices) && same(st.ends, easyStrips.ends) &&
        same(st.clockwise, easyStrips.clockwise) && same(st.points, easyStrips.points);
    const all = [...data, ...easyStrips.points];
    write();
    const r = direct.run(n, h);
    const decoded = decodeRestart(Array.from(r.u32.subarray(r.indexStart, r.indexStart + r.indexCount)));
    good = good && same(r.points, easyStrips.points);
    const fromStrips = [];
    for (let s = 0, begin = 0; s < easyStrips.ends.length; begin = easyStrips.ends[s++])
        for (let k = begin; k + 2 < easyStrips.ends[s]; ++k) fromStrips.push([easyStrips.indices[k], easyStrips.indices[k + 1], easyStrips.indices[k + 2]]);
    good = good && decoded.length === fromStrips.length && decoded.map(key).sort().join(';') === fromStrips.map(key).sort().join(';');
    for (const [a, b, c] of decoded)
        good = good && (all[2 * b] - all[2 * a]) * (all[2 * c + 1] - all[2 * a + 1]) - (all[2 * b + 1] - all[2 * a + 1]) * (all[2 * c] - all[2 * a]) > 0;
    check(`direct: ${name}`, good);
}
sameDirect('square with a hole', withHole, [4]);
sameDirect('bow tie', bowTie, []);
sameDirect('200 holes', grid, gridHoles);
sameDirect('lon/lat ring', lonlat, []);
{
    const polys = [square, withHole, bowTie, grid];
    const holesOf = [[], [4], [], gridHoles];
    const total = polys.reduce((a, p) => a + p.length / 2, 0), totalHoles = holesOf.reduce((a, h) => a + h.length, 0);
    const flat = new Float64Array(2 * total), starts = new Uint32Array(polys.length + 1), hs = new Uint32Array(polys.length + 1), hl = [];
    let v = 0;
    polys.forEach((p, k) => {
        flat.set(p, 2 * v);
        starts[k] = v;
        hs[k] = hl.length;
        hl.push(...holesOf[k]);
        v += p.length / 2;
    });
    starts[polys.length] = v;
    hs[polys.length] = hl.length;
    const easy = batch(flat, starts, new Uint32Array(hl), hs);
    let input = direct.batchInput(polys.length, total, totalHoles);
    input.coords.set(flat);
    input.polygonStarts.set(starts);
    input.holes.set(hl);
    input.holeStarts.set(hs);
    const fast = direct.batch(polys.length);
    let good = fast.indices.length === easy.indices.length && fast.indices.every((x, i) => x === easy.indices[i]) && fast.offsets.every((x, i) => x === easy.offsets[i]);
    input = direct.batchInput(polys.length, total, totalHoles);
    input.coords.set(flat);
    input.polygonStarts.set(starts);
    input.holes.set(hl);
    input.holeStarts.set(hs);
    const restart = decodeRestart(Array.from(direct.batch(polys.length, 2, 'restart').indices));
    const listed = [];
    for (let k = 0; k < easy.indices.length; k += 3) listed.push([easy.indices[k], easy.indices[k + 1], easy.indices[k + 2]]);
    good = good && restart.map(key).sort().join(';') === listed.map(key).sort().join(';');
    check('direct: batch, triangles and restart', good);
}

// The input views follow the buffers when other calls move them.
{
    const square = [0, 0, 10, 0, 10, 10, 0, 10];
    const big = [];
    for (let i = 0; i < 200000; ++i) big.push(Math.cos(i / 200000 * 2 * Math.PI) * 1e4, Math.sin(i / 200000 * 2 * Math.PI) * 1e4);
    const triangulate = () => {
        direct.input(4).coords.set(square);
        return Array.from(direct.run(4, 0, 'triangles').indices).join();
    };
    const expected = earstrip(square).join();
    let good = triangulate() === expected;
    earstrip(big);
    good = good && triangulate() === expected;
    strips(big);
    good = good && triangulate() === expected;
    shrink();
    good = good && triangulate() === expected;
    check('direct: input after other calls move the buffers', good);
}

// Every polygon of a PLY1 file against the native hashes of the ears alone, forced cuts off as in the file.
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
    setConstrained(false);
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
    setConstrained(true);
    check(`${P} polygons against the native results`, mismatches === 0, `${mismatches} differ`);

    // the direct calls give what the copying calls give, polygon by polygon
    const hash = (h, a, from = 0, to = a.length) => {
        for (let i = from; i < to; ++i) h = fnv(h, a[i]);
        return h;
    };
    let differ = 0;
    for (let p = 0; p < P; ++p) {
        const r0 = prs[p], r1 = prs[p + 1], v0 = rps[r0], n = rps[r1] - v0, h = r1 - r0 - 1;
        const data = xy.subarray(2 * v0, 2 * rps[r1]);
        const holes = [];
        for (let r = r0 + 1; r < r1; ++r) holes.push(rps[r] - v0);
        const write = () => {
            const input = direct.input(n, h);
            input.coords.set(data);
            input.holes.set(holes);
        };
        const list = earstrip(data, holes), st = strips(data, holes);
        write();
        let r = direct.run(n, h, 'triangles');
        let good = hash(0, r.u32, r.indexStart, r.indexStart + r.indexCount) === hash(0, list);
        write();
        r = direct.run(n, h, 'strips');
        good = good && hash(0, r.u32, r.indexStart, r.indexStart + r.indexCount) === hash(0, st.indices) &&
            hash(0, r.u32, r.endsStart, r.endsStart + r.stripCount) === hash(0, st.ends) &&
            hash(0, r.u8, r.clockwiseStart, r.clockwiseStart + r.stripCount) === hash(0, st.clockwise) &&
            hash(0, r.f64.subarray(r.pointsStart, r.pointsStart + 2 * r.pointCount)) === hash(0, st.points);
        // the restart buffer, made here from the strips
        let expected = 0;
        for (let s = 0, begin = 0; s < st.ends.length; begin = st.ends[s++]) {
            if (s) expected = fnv(expected, 0xffffffff);
            if (st.clockwise[s]) expected = fnv(expected, st.indices[begin]);
            expected = hash(expected, st.indices, begin, st.ends[s]);
        }
        write();
        r = direct.run(n, h, 'restart');
        good = good && hash(0, r.u32, r.indexStart, r.indexStart + r.indexCount) === expected;
        differ += !good;
    }
    check(`${P} polygons, direct calls against the copying calls`, differ === 0, `${differ} differ`);
}

console.log(failures ? `FAILED: ${failures}` : 'OK');
process.exit(failures ? 1 : 0);
