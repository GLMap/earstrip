// Native benchmark over a file of polygons; optionally against earcut.hpp (build with -DWITH_EARCUT -I<earcut.hpp dir>).
// Input "PLY1": u32 magic, u32 polygons, u32 rings, u32 points, u32 polyRingStart[polygons + 1],
// u32 ringPointStart[rings + 1], i16 xy[2 * points], then optionally u32 resultHash[polygons] (FNV-1a of the
// expected strips with forced cuts off, see hashOf) that --check compares against.
// Usage: bench <file.bin> [-r repeats] [--check]
#include "earstrip.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#ifdef WITH_EARCUT
#    include "earcut.hpp"
#endif

using Clock = std::chrono::steady_clock;

struct Pt {
    int16_t x, y;
};

struct Ring {
    using value_type = Pt;
    const Pt *p;
    uint32_t n;
    size_t size() const { return n; }
    const Pt &operator[](size_t i) const { return p[i]; }
};

struct Polygon {
    const Pt *base;
    const uint32_t *starts; // ring starts in base, count + 1
    uint32_t n;
    size_t size() const { return n; }
    Ring operator[](size_t r) const { return {base + starts[r], starts[r + 1] - starts[r]}; }
    bool empty() const { return n == 0; }
};

#ifdef WITH_EARCUT
namespace mapbox::util {
template <> struct nth<0, Pt> { static int16_t get(const Pt &t) { return t.x; } };
template <> struct nth<1, Pt> { static int16_t get(const Pt &t) { return t.y; } };
}
#endif

static uint32_t fnv(uint32_t h, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        h ^= (v >> (8 * i)) & 0xff;
        h *= 16777619u;
    }
    return h;
}

template <class T> static uint32_t hashOf(const T &t, earstrip::Status s)
{
    const bool ok = s != earstrip::Status::Refused;
    uint32_t h = fnv(2166136261u, ok ? (t.holesLeftOut() ? 2 : 0) : 1);
    if (ok) {
        for (auto i : t.strips())
            h = fnv(h, i);
        for (auto e : t.stripEnds())
            h = fnv(h, e);
    }
    return h;
}

// What a renderer would show wrong: on a 128 x 128 grid over the polygon, the pixels where the union of the triangles
// differs from the even-odd fill of the rings, relative to the pixels the rings fill.
static double pixelError(const Polygon &poly, const std::vector<uint32_t> &tris)
{
    std::vector<Pt> all;
    double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
    for (uint32_t r = 0; r < poly.n; ++r)
        for (uint32_t i = 0; i < poly[r].n; ++i) {
            const Pt q = poly[r][i];
            all.push_back(q);
            minX = std::min(minX, (double)q.x);
            maxX = std::max(maxX, (double)q.x);
            minY = std::min(minY, (double)q.y);
            maxY = std::max(maxY, (double)q.y);
        }
    const int grid = 128;
    const double sx = (maxX - minX) / grid, sy = (maxY - minY) / grid;
    size_t filled = 0, wrong = 0;
    for (int gy = 0; gy < grid; ++gy) {
        const double y = minY + (gy + 0.5) * sy;
        for (int gx = 0; gx < grid; ++gx) {
            const double x = minX + (gx + 0.5) * sx;
            bool inRings = false;
            for (uint32_t r = 0; r < poly.n; ++r) {
                const Ring ring = poly[r];
                for (uint32_t i = 0, j = ring.n - 1; i < ring.n; j = i++) {
                    const Pt a = ring[j], b = ring[i];
                    if ((a.y > y) != (b.y > y) && x < (double)(b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
                        inRings = !inRings;
                }
            }
            bool inTriangles = false;
            for (size_t k = 0; k + 2 < tris.size() && !inTriangles; k += 3) {
                const Pt a = all[tris[k]], b = all[tris[k + 1]], c = all[tris[k + 2]];
                const double d1 = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x), d2 = (c.x - b.x) * (y - b.y) - (c.y - b.y) * (x - b.x),
                             d3 = (a.x - c.x) * (y - c.y) - (a.y - c.y) * (x - c.x);
                inTriangles = (d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0);
            }
            filled += inRings;
            wrong += inRings != inTriangles;
        }
    }
    return filled ? (double)wrong / filled : (wrong ? 1.0 : 0.0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: bench <file.bin> [-r repeats] [--check]\n");
        return 2;
    }
    int repeats = 5;
    bool check = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc)
            repeats = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--check"))
            check = true;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    uint32_t head[4];
    if (fread(head, 4, 4, f) != 4 || head[0] != 0x31594c50u) {
        fprintf(stderr, "%s: not a PLY1 file\n", argv[1]);
        return 2;
    }
    const uint32_t polys = head[1], rings = head[2], points = head[3];
    std::vector<uint32_t> polyRingStart(polys + 1), ringPointStart(rings + 1), hashes(polys);
    std::vector<Pt> xy(points);
    fread(polyRingStart.data(), 4, polyRingStart.size(), f);
    fread(ringPointStart.data(), 4, ringPointStart.size(), f);
    fread(xy.data(), 4, xy.size(), f);
    const bool haveHashes = fread(hashes.data(), 4, hashes.size(), f) == hashes.size();
    fclose(f);
    // ring starts are offsets into the whole file; indices still count from each polygon's first point
    std::vector<Polygon> polygons(polys);
    for (uint32_t p = 0; p < polys; ++p)
        polygons[p] = {xy.data(), ringPointStart.data() + polyRingStart[p], polyRingStart[p + 1] - polyRingStart[p]};
    printf("%u polygons, %u rings, %u points\n", polys, rings, points);

    earstrip::Triangulator<uint16_t> strip;
    if (check) {
        if (!haveHashes) {
            fprintf(stderr, "--check: the file has no result hashes\n");
            return 2;
        }
        strip.setForcedCuts(false);
        uint32_t mismatches = 0;
        for (uint32_t p = 0; p < polys; ++p)
            mismatches += hashOf(strip, strip.run(polygons[p])) != hashes[p];
        printf("check: %u of %u polygons differ from the expected strips\n", mismatches, polys);
        strip.setForcedCuts(true);
        if (mismatches)
            return 1;
    }

    size_t sink = 0;
    auto runStrip = [&] {
        const auto s = Clock::now();
        for (const Polygon &p : polygons) {
            strip.run(p);
            sink += strip.strips().size();
        }
        return std::chrono::duration<double, std::milli>(Clock::now() - s).count();
    };
#ifdef WITH_EARCUT
    mapbox::detail::Earcut<uint16_t> earcut;
    auto runEarcut = [&] {
        const auto s = Clock::now();
        for (const Polygon &p : polygons) {
            earcut(p);
            sink += earcut.indices.size();
        }
        return std::chrono::duration<double, std::milli>(Clock::now() - s).count();
    };
#endif
    double bestStrip = 1e30, bestEarcut = 1e30;
    for (int r = 0; r < repeats; ++r) {
        bestStrip = std::min(bestStrip, runStrip());
#ifdef WITH_EARCUT
        bestEarcut = std::min(bestEarcut, runEarcut());
#endif
    }

    // what each one makes of the polygons the ears cannot cut exactly
    size_t exact = 0, holes = 0, approximate = 0, triangles = 0, indices = 0, strips = 0;
    double devStrip = 0, devEarcut = 0, worstStrip = 0, worstEarcut = 0;
    size_t stripBetter = 0, earcutBetter = 0;
    std::vector<uint32_t> tris;
    for (const Polygon &p : polygons) {
        const auto s = strip.run(p);
        exact += s == earstrip::Status::Exact;
        holes += s == earstrip::Status::HolesLeftOut;
        indices += strip.strips().size();
        strips += strip.stripEnds().size();
        std::vector<uint16_t> t16;
        strip.triangles(t16);
        triangles += t16.size() / 3;
        if (s != earstrip::Status::Approximate)
            continue;
        approximate++;
        tris.assign(t16.begin(), t16.end());
        const double ds = pixelError(p, tris);
        devStrip += ds;
        worstStrip = std::max(worstStrip, ds);
#ifdef WITH_EARCUT
        earcut(p);
        tris.assign(earcut.indices.begin(), earcut.indices.end());
        const double de = pixelError(p, tris);
        devEarcut += de;
        worstEarcut = std::max(worstEarcut, de);
        stripBetter += ds < de;
        earcutBetter += de < ds;
#endif
    }
    printf("earstrip:   best of %d %.1f ms; %zu triangles, %.3f indices per triangle in %zu strips\n", repeats, bestStrip, triangles,
        (double)indices / std::max<size_t>(triangles, 1), strips);
    printf("            exact %zu, holes left out %zu, approximate %zu (wrong pixels: mean %.4f, worst %.4f)\n", exact, holes, approximate,
        approximate ? devStrip / approximate : 0.0, worstStrip);
#ifdef WITH_EARCUT
    printf("earcut.hpp: best of %d %.1f ms; on the approximate ones wrong pixels mean %.4f, worst %.4f; closer: earstrip %zu, earcut %zu\n", repeats,
        bestEarcut, approximate ? devEarcut / approximate : 0.0, worstEarcut, stripBetter, earcutBetter);
#endif
    return sink == 42 ? 3 : 0;
}
