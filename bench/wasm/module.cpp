// earstrip and earcut.hpp 3.2.4 in one WebAssembly module for bench.mjs (build.sh).
// runAll: every polygon of a file already in the module's memory, looped inside it (a map engine in WebAssembly).
// runOne: one polygon copied in by JavaScript (a drop-in for a JavaScript triangulator).
#include "earstrip.hpp"
#include "earcut.hpp"
#include <emscripten/emscripten.h>
#include <cstdlib>

struct Pt {
    int16_t x, y;
};
struct RingV {
    const Pt *p;
    uint32_t n;
    uint32_t size() const { return n; }
    Pt operator[](uint32_t i) const { return p[i]; }
};
struct RingsV {
    const Pt *base;
    const uint32_t *starts; // count + 1 vertex offsets from base
    uint32_t cnt;
    uint32_t count() const { return cnt; }
    RingV getVector(uint32_t r) const { return {base + starts[r], starts[r + 1] - starts[r]}; }
};

// The rings as earstrip's nested input
struct Poly {
    RingsV r;
    size_t size() const { return r.cnt; }
    RingV operator[](size_t i) const { return r.getVector((uint32_t)i); }
};

namespace mapbox::util {
template <> struct nth<0, Pt> { static int16_t get(const Pt &t) { return t.x; } };
template <> struct nth<1, Pt> { static int16_t get(const Pt &t) { return t.y; } };
}
struct EarcutRing {
    using value_type = Pt;
    RingV r;
    size_t size() const { return r.n; }
    Pt operator[](size_t i) const { return r.p[i]; }
};
struct EarcutPoly {
    RingsV r;
    bool empty() const { return r.cnt == 0; }
    size_t size() const { return r.cnt; }
    EarcutRing operator[](size_t i) const { return {r.getVector((uint32_t)i)}; }
};

static uint32_t fnv(uint32_t h, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        h ^= (v >> (8 * i)) & 0xff;
        h *= 16777619u;
    }
    return h;
}

static earstrip::Triangulator<uint16_t> clipper;
static mapbox::detail::Earcut<uint16_t> earcut;

extern "C" {
EMSCRIPTEN_KEEPALIVE void *wasmAlloc(uint32_t size) { return malloc(size); }

// Returns the total index count; fills hashes (the result hash per polygon, as bench.cpp's hashOf) when given.
EMSCRIPTEN_KEEPALIVE uint32_t runAll(const int16_t *xy, const uint32_t *ringPointStart, const uint32_t *polyRingStart, uint32_t polys, uint32_t *hashes)
{
    uint32_t sink = 0;
    for (uint32_t p = 0; p < polys; ++p) {
        const uint32_t r0 = polyRingStart[p], r1 = polyRingStart[p + 1];
        // ring starts are global vertex offsets; strip indices still count from the polygon's first vertex
        RingsV rings{(const Pt *)xy, ringPointStart + r0, r1 - r0};
        // the hashes in the file are of the ears alone; timing runs use the defaults
        const bool full = !hashes;
        clipper.setConstrained(full);
        clipper.setForcedCuts(full);
        const bool ok = clipper.run(Poly{rings}) != earstrip::Status::Refused;
        sink += (uint32_t)clipper.strips().size();
        if (hashes) {
            uint32_t h = fnv(2166136261u, ok ? (clipper.holesLeftOut() ? 2 : 0) : 1);
            if (ok) {
                for (auto i : clipper.strips())
                    h = fnv(h, i);
                for (auto e : clipper.stripEnds())
                    h = fnv(h, e);
            }
            hashes[p] = h;
        }
    }
    return sink;
}

// starts: rings + 1 vertex offsets into xy. Returns 0 refused, 1 ok, 2 ok with holes left out.
EMSCRIPTEN_KEEPALIVE uint32_t runOne(const int16_t *xy, const uint32_t *starts, uint32_t rings)
{
    const auto s = clipper.run(Poly{RingsV{(const Pt *)xy, starts, rings}});
    return s == earstrip::Status::Refused ? 0 : clipper.holesLeftOut() ? 2 : 1;
}
// earcut.hpp counterparts; hashes are FNV-1a over the triangle indices, the file's second hash array.
EMSCRIPTEN_KEEPALIVE uint32_t runAllEarcut(const int16_t *xy, const uint32_t *ringPointStart, const uint32_t *polyRingStart, uint32_t polys, uint32_t *hashes)
{
    uint32_t sink = 0;
    for (uint32_t p = 0; p < polys; ++p) {
        const uint32_t r0 = polyRingStart[p], r1 = polyRingStart[p + 1];
        earcut(EarcutPoly{{(const Pt *)xy, ringPointStart + r0, r1 - r0}});
        sink += (uint32_t)earcut.indices.size();
        if (hashes) {
            uint32_t h = 2166136261u;
            for (auto i : earcut.indices)
                h = fnv(h, i);
            hashes[p] = h;
        }
    }
    return sink;
}
EMSCRIPTEN_KEEPALIVE uint32_t runOneEarcut(const int16_t *xy, const uint32_t *starts, uint32_t rings)
{
    earcut(EarcutPoly{{(const Pt *)xy, starts, rings}});
    return (uint32_t)earcut.indices.size();
}
EMSCRIPTEN_KEEPALIVE const uint16_t *earcutIndices() { return earcut.indices.data(); }

EMSCRIPTEN_KEEPALIVE const uint16_t *strips() { return clipper.strips().data(); }
EMSCRIPTEN_KEEPALIVE uint32_t stripsSize() { return (uint32_t)clipper.strips().size(); }
EMSCRIPTEN_KEEPALIVE const uint32_t *stripEnds() { return clipper.stripEnds().data(); }
EMSCRIPTEN_KEEPALIVE uint32_t stripEndsSize() { return (uint32_t)clipper.stripEnds().size(); }
}
