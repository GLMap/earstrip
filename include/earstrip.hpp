// earstrip: ear clipping triangulation of polygons with holes into triangle strips.
// https://github.com/GLMap/earstrip
//
// Copyright (c) 2026 Evgen Bodunov
// SPDX-License-Identifier: ISC

#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#    include <intrin.h>
#    define EARSTRIP_INLINE __forceinline
#    define EARSTRIP_RARE
#else
#    define EARSTRIP_INLINE inline __attribute__((always_inline))
// Code few polygons reach: with EARSTRIP_SMALL_CONSTRAINED kept out of line and optimized for size
#    if defined(EARSTRIP_SMALL_CONSTRAINED)
#        define EARSTRIP_RARE __attribute__((noinline, minsize))
#    else
#        define EARSTRIP_RARE
#    endif
#endif

namespace earstrip {

// How a polygon was covered.
enum class Status {
    Exact,        // the triangles cover the polygon exactly
    HolesLeftOut, // a hole could not be joined to the boundary (it lies outside or crosses it) and is left out
    Approximate,  // the boundary ran out of ears (the polygon is not simple); the rest was cut by forced cuts
    Refused,      // as Approximate, with forced cuts switched off: the strips cover only what the ears did
    Constrained,  // covered by the constrained triangulation: the rings cross, a hole could not be joined or there are
                  // more than 128 rings; what lies inside ring 0 by non-zero winding and outside every hole
};

// Coordinates of a point: members x and y, or std::get<0> and std::get<1> (std::array, std::pair, std::tuple).
// Specialize for other point types.
template <class P, class = void> struct point_traits {
    static double x(const P &p)
    {
        using std::get;
        return (double)get<0>(p);
    }
    static double y(const P &p)
    {
        using std::get;
        return (double)get<1>(p);
    }
};

template <class P> struct point_traits<P, std::void_t<decltype(std::declval<const P &>().x), decltype(std::declval<const P &>().y)>> {
    static double x(const P &p) { return (double)p.x; }
    static double y(const P &p) { return (double)p.y; }
};

namespace detail {

inline uint32_t bitWidth(uint32_t v)
{
    uint32_t w = 0;
    while (v) {
        v >>= 1;
        w++;
    }
    return w;
}

inline uint32_t lowestBit(uint64_t v)
{
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long i;
    _BitScanForward64(&i, v);
    return (uint32_t)i;
#else
    return (uint32_t)__builtin_ctzll(v);
#endif
}

inline uint64_t bits(double v)
{
    v += 0.0; // -0 and 0 are the same position
    uint64_t b;
    std::memcpy(&b, &v, sizeof b);
    return b;
}

// Sorts n items by a strict total order (or one under which equal items are interchangeable). With
// EARSTRIP_SMALL_CONSTRAINED a heap sort, much smaller code than std::sort; either gives the same result.
template <class T, class Less> void sortItems(T *a, size_t n, Less less)
{
#if defined(EARSTRIP_SMALL_CONSTRAINED)
    auto sift = [&](size_t root, size_t end) {
        while (2 * root + 1 < end) {
            size_t child = 2 * root + 1;
            if (child + 1 < end && less(a[child], a[child + 1]))
                child++;
            if (!less(a[root], a[child]))
                return;
            std::swap(a[root], a[child]);
            root = child;
        }
    };
    for (size_t i = n / 2; i > 0; --i)
        sift(i - 1, n);
    for (size_t end = n; end > 1; --end) {
        std::swap(a[0], a[end - 1]);
        sift(0, end - 1);
    }
#else
    std::sort(a, a + n, less);
#endif
}

// Rings of a polygon given as a container of containers of points.
template <class Polygon> struct NestedRings {
    const Polygon &polygon;
    size_t count() const { return polygon.size(); }
    size_t size(size_t r) const { return polygon[r].size(); }
    double x(size_t r, size_t i) const
    {
        using P = std::decay_t<decltype(polygon[r][i])>;
        return point_traits<P>::x(polygon[r][i]);
    }
    double y(size_t r, size_t i) const
    {
        using P = std::decay_t<decltype(polygon[r][i])>;
        return point_traits<P>::y(polygon[r][i]);
    }
};

// Rings of a polygon given as flat coordinates and the vertex index where each hole starts.
template <class T, class H> struct FlatRings {
    const T *coords;
    size_t vertices, dim;
    const H *holes;
    size_t holeCount;
    size_t start(size_t r) const { return r == 0 ? 0 : (size_t)holes[r - 1]; }
    size_t end(size_t r) const { return r < holeCount ? (size_t)holes[r] : vertices; }
    size_t count() const { return vertices ? holeCount + 1 : 0; }
    size_t size(size_t r) const { return end(r) > start(r) ? end(r) - start(r) : 0; }
    double x(size_t r, size_t i) const { return (double)coords[(start(r) + i) * dim]; }
    double y(size_t r, size_t i) const { return (double)coords[(start(r) + i) * dim + 1]; }
};

// EARSTRIP_SMALL_CONSTRAINED optimizes the constrained triangulation for size: it covers few polygons, so its speed
// matters less than the size of a WebAssembly module.
#if defined(EARSTRIP_SMALL_CONSTRAINED) && defined(__clang__)
#    pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#elif defined(EARSTRIP_SMALL_CONSTRAINED) && defined(__GNUC__)
#    pragma GCC push_options
#    pragma GCC optimize("Os")
#endif

// Constrained Delaunay triangulation of the ring edges into strips. The rings' crossings are rerouted through a grid
// point next to them, which becomes an added point. Kept: what lies inside ring 0 (non-zero winding) and outside every
// hole. Integer input of 32 bits is used as it is; other input is rounded to a 2^30 grid over its bounding box.
template <class Index> class Constrained {
    static constexpr uint32_t None = UINT32_MAX;
    // Input and added points the strips can index
    static constexpr uint64_t IndexCount = (uint64_t)std::numeric_limits<Index>::max() + 1;
    static constexpr uint32_t MaxPoints = (uint32_t)std::min<uint64_t>(IndexCount - 2, UINT32_MAX - 2);
    // Floating point input is rounded to a grid of this many units over its bounding box
    static constexpr double GridSize = 1 << 30;
    // Up to this coordinate span the in-circle test is exact in 64 bits
    static constexpr int64_t SmallSpan = 1 << 14;

    struct Vertex {
        int64_t x, y;
    };

    // Signed 128-bit integer for the exact predicates; portable to targets without a native one.
    struct Int128 {
        uint64_t lo = 0;
        int64_t hi = 0;

        static void mulUnsigned(uint64_t x, uint64_t y, uint64_t &hi, uint64_t &lo)
        {
            const uint64_t x0 = x & 0xFFFFFFFFu, x1 = x >> 32, y0 = y & 0xFFFFFFFFu, y1 = y >> 32;
            const uint64_t p00 = x0 * y0, p01 = x0 * y1, p10 = x1 * y0, p11 = x1 * y1;
            const uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
            lo = (mid << 32) | (p00 & 0xFFFFFFFFu);
            hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
        }

        static Int128 mul(int64_t a, int64_t b)
        {
            Int128 r;
            uint64_t hi;
            mulUnsigned(a < 0 ? 0 - (uint64_t)a : (uint64_t)a, b < 0 ? 0 - (uint64_t)b : (uint64_t)b, hi, r.lo);
            r.hi = (int64_t)hi;
            return (a < 0) != (b < 0) ? -r : r;
        }

        // Product with b; it must fit 127 bits.
        Int128 times(int64_t b) const
        {
            const Int128 m = hi < 0 ? -*this : *this;
            Int128 r;
            uint64_t carry;
            const uint64_t y = b < 0 ? 0 - (uint64_t)b : (uint64_t)b;
            mulUnsigned(m.lo, y, carry, r.lo);
            r.hi = (int64_t)(carry + (uint64_t)m.hi * y);
            return (hi < 0) != (b < 0) ? -r : r;
        }

        Int128 operator-() const
        {
            Int128 r;
            r.lo = 0 - lo;
            r.hi = (int64_t)(~(uint64_t)hi + (lo == 0));
            return r;
        }
        Int128 operator+(Int128 o) const
        {
            Int128 r;
            r.lo = lo + o.lo;
            r.hi = (int64_t)((uint64_t)hi + (uint64_t)o.hi + (r.lo < lo));
            return r;
        }
        Int128 operator-(Int128 o) const { return *this + -o; }
        bool operator<(Int128 o) const { return hi != o.hi ? hi < o.hi : lo < o.lo; }
        bool operator<=(Int128 o) const { return !(o < *this); }
        bool operator==(Int128 o) const { return hi == o.hi && lo == o.lo; }
        int sign() const { return hi < 0 ? -1 : hi > 0 || lo != 0 ? 1 : 0; }
        Int128 abs() const { return hi < 0 ? -*this : *this; }
        double toDouble() const { return hi < 0 ? -(-*this).toDouble() : (double)hi * 18446744073709551616.0 + (double)lo; }

        // Floor of n / d for d > 0 whose quotient fits 62 bits; r gets the remainder in [0, d).
        static int64_t floorDiv(Int128 n, Int128 d, Int128 &r)
        {
            int64_t q = (int64_t)std::floor(n.toDouble() / d.toDouble());
            r = n - d.times(q);
            while (r.sign() < 0) {
                q--;
                r = r + d;
            }
            while (d <= r) {
                q++;
                r = r - d;
            }
            return q;
        }
    };

    // Net number of times ring 0 and the holes run along a half-edge, in its direction
    struct Wind {
        int32_t outer, holes;
        Wind operator+(Wind o) const { return {outer + o.outer, holes + o.holes}; }
        Wind operator-() const { return {-outer, -holes}; }
    };

    struct Segment {
        uint64_t key; // low vertex << 32 | high vertex
        Wind wind;    // from low to high
    };

    struct Pending {
        uint32_t a, b;
        Wind wind;
    };

    struct SortItem {
        int64_t x, y;
        uint32_t index;
    };

    enum class WalkEnd : uint8_t { Target, Vertex, Crossing, Failed };
    enum class Location : uint8_t { Vertex, Edge, Triangle, Outside };
    enum : uint8_t { Unseen = 0, Seen, Dropped, Free, Used };

    struct Point {
        double x, y;
    };

    std::vector<Point> _input; // ring points as given
    std::vector<uint32_t> _ringEnds;
    std::vector<SortItem> _sorted;
    std::vector<uint32_t> _vertexOf;   // vertex of each input point
    std::vector<Vertex> _vertices;     // distinct input points in sweep order, then added points
    std::vector<Index> _outIndex;      // index of each vertex in the result
    std::vector<uint32_t> _vertexEdge; // a half-edge leaving each vertex
    std::vector<uint32_t> _hullNext, _hullPrev, _hullEdge;
    // Half-edge e of triangle e / 3 runs from _org[e] to _org[next(e)] with the triangle on its left
    std::vector<uint32_t> _org, _twin;
    std::vector<Wind> _wind;
    std::vector<Segment> _segments;
    std::vector<Pending> _pending;
    std::vector<std::pair<uint32_t, uint32_t>> _crossed;
    std::vector<uint32_t> _stack;
    std::vector<uint32_t> _chain;
    std::vector<Wind> _triangleWind;
    std::vector<uint8_t> _state;
    std::vector<uint8_t> _degree;
    std::vector<uint32_t> _candidates;
    std::vector<uint32_t> _run;   // vertices of a strip run
    std::vector<uint32_t> _taken;         // triangles of the last strip
    std::vector<uint32_t> _stripVertices; // vertices of the last strip
    std::vector<Index> _strips;
    std::vector<uint32_t> _stripEnds;
    std::vector<uint8_t> _stripCW; // first triangle of the strip runs clockwise
    uint32_t _pointCount = 0; // input points
    uint32_t _inputVertices = 0;
    uint32_t _vertexCount = 0;
    uint32_t _triangleCount = 0;
    uint32_t _extraCount = 0;
    uint64_t _work = 0, _workLimit = 0;
    double _originX = 0, _originY = 0, _scale = 1;
    bool _wide = false;        // coordinate differences may exceed 32 bits
    bool _exactCircle = false; // in-circle fits 64 bits
    bool _smallArea = false;   // ring areas fit 64 bits
    bool _integral = false;    // every coordinate is an integer of 32 bits: no rounding to the grid
    bool _failed = false;      // a limit was hit; the strips are empty

    // ---------- fill rule ----------

    // Inside ring 0 and outside the holes (inside a hole its turned winding is negative)
    static bool keep(Wind w) { return w.outer != 0 && w.holes >= 0; }

    // An edge the rule can see: it is kept, split at crossings and never flipped.
    static bool significant(Wind w) { return w.outer != 0 || w.holes != 0; }

    // Equal as far as the rule can tell.
    static bool sameForRule(Wind a, Wind b) { return a.outer == b.outer && a.holes == b.holes; }

    // ---------- the steps ----------

public:
    // Copies the rings; nothing reads them afterwards. False when they have more points than Index can count.
    template <class Rings> bool load(const Rings &rings)
    {
        const uint32_t ringCount = (uint32_t)rings.count();
        uint64_t total = 0;
        for (uint32_t r = 0; r < ringCount; ++r)
            total += rings.size(r);
        _pointCount = 0;
        _ringEnds.clear();
        if (total > MaxPoints)
            return false;
        if (_input.size() < total)
            _input.resize(total);
        _ringEnds.resize(ringCount);
        uint32_t used = 0;
        _integral = true;
        for (uint32_t r = 0; r < ringCount; ++r) {
            const uint32_t size = (uint32_t)rings.size(r);
            for (uint32_t i = 0; i < size; ++i) {
                const double x = rings.x(r, i), y = rings.y(r, i);
                _integral = _integral && integer(x) && integer(y);
                _input[used++] = {x, y};
            }
            _ringEnds[r] = used;
        }
        _pointCount = used;
        return true;
    }

    // Triangulates the loaded rings; false when a limit was hit.
    EARSTRIP_RARE bool build()
    {
        _strips.clear();
        _stripEnds.clear();
        _stripCW.clear();
        _extraCount = 0;
        _vertexCount = _inputVertices = 0;
        _triangleCount = 0;
        _failed = false;
        if (_pointCount < 3)
            return true;
        toGrid();
        sortVertices();
        if (_vertexCount < 3 || !collectSegments() || !sweep())
            return true;
        _work = 0;
        _workLimit = 16 * ((uint64_t)_segments.size() + _vertexCount) + (1 << 16);
        for (const Segment &s : _segments) {
            insertSegment((uint32_t)(s.key >> 32), (uint32_t)s.key, s.wind);
            if (_failed) {
                _extraCount = 0;
                return false;
            }
        }
        fill();
        buildStrips();
        return true;
    }

    std::vector<Index> &strips() { return _strips; }
    std::vector<uint32_t> &stripEnds() { return _stripEnds; }
    std::vector<uint8_t> &stripClockwise() { return _stripCW; }

    // Added points as x, y pairs; strip index pointCount + k is added point k.
    void extraPoints(std::vector<double> &out) const
    {
        out.clear();
        for (uint32_t k = 0; k < _extraCount; ++k) {
            const Point p = extraPoint(k);
            out.push_back(p.x);
            out.push_back(p.y);
        }
    }
    uint32_t extraCount() const { return _extraCount; }

    // Doubled area of ring 0 minus the holes, as the rings are given.
    double inputRingsArea() const
    {
        double rv = 0;
        uint32_t begin = 0;
        for (uint32_t r = 0; r < _ringEnds.size(); ++r) {
            const uint32_t end = _ringEnds[r];
            double a = 0;
            for (uint32_t i = begin; i < end; ++i) {
                const Point p = _input[i], q = _input[i + 1 < end ? i + 1 : begin];
                a += p.x * q.y - q.x * p.y;
            }
            rv += r == 0 ? std::abs(a) : -std::abs(a);
            begin = end;
        }
        return rv;
    }

    // True when the given strips (over the input points) cover an area strictly closer to the ring area than the
    // constrained ones.
    bool closer(const std::vector<Index> &strips, const std::vector<uint32_t> &ends, double ringsArea) const
    {
        return std::abs(covered(strips, ends) - ringsArea) < std::abs(covered(_strips, _stripEnds) - ringsArea);
    }

    // The same area for a triangle list.
    bool closerList(const std::vector<Index> &list, double ringsArea) const
    {
        double sum = 0;
        for (size_t k = 0; k + 2 < list.size(); k += 3)
            sum += std::abs(cross(point(list[k]), point(list[k + 1]), point(list[k + 2])));
        return std::abs(sum - ringsArea) < std::abs(covered(_strips, _stripEnds) - ringsArea);
    }

private:
    static bool integer(double v) { return v == std::floor(v) && v >= -2147483648.0 && v <= 2147483647.0; }

    static double cross(Point a, Point b, Point c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

    double covered(const std::vector<Index> &strips, const std::vector<uint32_t> &ends) const
    {
        double sum = 0;
        uint32_t begin = 0;
        for (uint32_t end : ends) {
            for (uint32_t k = begin; k + 2 < end; ++k)
                sum += std::abs(cross(point(strips[k]), point(strips[k + 1]), point(strips[k + 2])));
            begin = end;
        }
        return sum;
    }

    Point point(uint32_t index) const { return index < _pointCount ? _input[index] : extraPoint(index - _pointCount); }

    Point extraPoint(uint32_t k) const
    {
        const Vertex v = _vertices[_inputVertices + k];
        if (_integral)
            return {(double)v.x, (double)v.y};
        return {_originX + (double)v.x / _scale, _originY + (double)v.y / _scale};
    }

    // ---------- input ----------

    // Integer coordinates of the input points and the exactness modes of the predicates.
    void toGrid()
    {
        const uint32_t n = _pointCount;
        if (_sorted.size() < n)
            _sorted.resize(n);
        SortItem *items = _sorted.data();
        if (!_integral) {
            double minX = _input[0].x, minY = _input[0].y, maxX = minX, maxY = minY;
            for (uint32_t i = 1; i < n; ++i) {
                minX = std::min(minX, _input[i].x);
                maxX = std::max(maxX, _input[i].x);
                minY = std::min(minY, _input[i].y);
                maxY = std::max(maxY, _input[i].y);
            }
            const double span = std::max(maxX - minX, maxY - minY);
            _scale = span > 0 && std::isfinite(span) ? GridSize / span : 1;
            _originX = minX;
            _originY = minY;
            auto grid = [](double v) { return std::isfinite(v) ? (int64_t)std::llround(std::clamp(v, 0.0, GridSize)) : 0; };
            for (uint32_t i = 0; i < n; ++i)
                items[i] = {grid((_input[i].x - minX) * _scale), grid((_input[i].y - minY) * _scale), i};
            _wide = false;
            _exactCircle = false;
            _smallArea = false;
        } else {
            int64_t minX = (int64_t)_input[0].x, minY = (int64_t)_input[0].y, maxX = minX, maxY = minY;
            for (uint32_t i = 0; i < n; ++i) {
                const int64_t x = (int64_t)_input[i].x, y = (int64_t)_input[i].y;
                items[i] = {x, y, i};
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
            const int64_t span = std::max(maxX - minX, maxY - minY);
            _wide = span > INT32_MAX;
            _exactCircle = span <= SmallSpan;
            _smallArea = span < (1 << 23);
        }
    }

    // Sorts the points by x, then y, and merges equal ones into vertices; a vertex keeps the last index of its point.
    void sortVertices()
    {
        const uint32_t n = _pointCount;
        SortItem *items = _sorted.data();
        auto less = [](const SortItem &a, const SortItem &b) { return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.index < b.index; };
        if (n <= 32) {
            for (uint32_t i = 1; i < n; ++i) {
                const SortItem item = items[i];
                uint32_t j = i;
                for (; j > 0 && less(item, items[j - 1]); --j)
                    items[j] = items[j - 1];
                items[j] = item;
            }
        } else {
            sortItems(items, n, less);
        }
        reserveVertices(n);
        if (_vertexOf.size() < n)
            _vertexOf.resize(n);
        uint32_t count = 0;
        for (uint32_t i = 0; i < n;) {
            uint32_t j = i;
            while (j + 1 < n && items[j + 1].x == items[i].x && items[j + 1].y == items[i].y)
                j++;
            _vertices[count] = {items[i].x, items[i].y};
            _outIndex[count] = (Index)items[j].index;
            for (uint32_t k = i; k <= j; ++k)
                _vertexOf[items[k].index] = count;
            count++;
            i = j + 1;
        }
        _vertexCount = _inputVertices = count;
    }

    // Grows the vertex arrays to count vertices and the half-edges to 2 * count triangles.
    void reserveVertices(uint32_t count)
    {
        if (_vertices.size() < count) {
            const size_t size = std::max<size_t>({count, 2 * _vertices.size(), 16});
            _vertices.resize(size);
            _outIndex.resize(size);
            _vertexEdge.resize(size);
            _hullNext.resize(size);
            _hullPrev.resize(size);
            _hullEdge.resize(size);
        }
        if (_org.size() < 6 * (size_t)count) {
            const size_t size = std::max<size_t>(6 * (size_t)count, 2 * _org.size());
            _org.resize(size);
            _twin.resize(size);
            _wind.resize(size);
        }
    }

    // Ring edges as segments between vertices with their net winding: ring 0 turned counterclockwise, holes clockwise,
    // a flat ring as given. Equal segments are merged; false when none is left.
    bool collectSegments()
    {
        _segments.clear();
        uint32_t begin = 0;
        for (uint32_t r = 0; r < _ringEnds.size(); ++r) {
            const uint32_t end = _ringEnds[r];
            if (end - begin >= 2) {
                const int areaSign = ringAreaSign(begin, end);
                const int32_t sign = areaSign == 0 || (areaSign > 0) == (r == 0) ? 1 : -1;
                for (uint32_t i = begin; i < end; ++i) {
                    const uint32_t a = _vertexOf[i], b = _vertexOf[i + 1 < end ? i + 1 : begin];
                    if (a == b)
                        continue;
                    const int32_t s = a < b ? sign : -sign;
                    const uint64_t key = a < b ? (uint64_t)a << 32 | b : (uint64_t)b << 32 | a;
                    _segments.push_back({key, r == 0 ? Wind{s, 0} : Wind{0, s}});
                }
            }
            begin = end;
        }
        Segment *s = _segments.data();
        const uint32_t n = (uint32_t)_segments.size();
        if (n <= 32) {
            for (uint32_t i = 1; i < n; ++i) {
                const Segment item = s[i];
                uint32_t j = i;
                for (; j > 0 && item.key < s[j - 1].key; --j)
                    s[j] = s[j - 1];
                s[j] = item;
            }
        } else {
            sortItems(s, n, [](const Segment &a, const Segment &b) { return a.key < b.key; });
        }
        uint32_t kept = 0;
        for (uint32_t i = 0; i < n;) {
            Segment merged = s[i];
            uint32_t j = i + 1;
            for (; j < n && s[j].key == merged.key; ++j)
                merged.wind = merged.wind + s[j].wind;
            if (significant(merged.wind))
                s[kept++] = merged;
            i = j;
        }
        _segments.resize(kept);
        return kept > 0;
    }

    // Sign of the area of the ring of input points [begin, end) on the vertices. Exact.
    int ringAreaSign(uint32_t begin, uint32_t end) const
    {
        const Vertex o = _vertices[_vertexOf[begin]];
        if (_smallArea) {
            int64_t area = 0;
            for (uint32_t i = begin; i < end; ++i) {
                const Vertex p = _vertices[_vertexOf[i]], q = _vertices[_vertexOf[i + 1 < end ? i + 1 : begin]];
                area += (p.x - o.x) * (q.y - o.y) - (q.x - o.x) * (p.y - o.y);
            }
            return (area > 0) - (area < 0);
        }
        Int128 area;
        for (uint32_t i = begin; i < end; ++i) {
            const Vertex p = _vertices[_vertexOf[i]], q = _vertices[_vertexOf[i + 1 < end ? i + 1 : begin]];
            area = area + Int128::mul(p.x - o.x, q.y - o.y) - Int128::mul(q.x - o.x, p.y - o.y);
        }
        return area.sign();
    }

    // ---------- predicates ----------

    // Sign of the turn a -> b -> c: 1 counterclockwise, -1 clockwise, 0 collinear. Exact.
    int orient(const Vertex &a, const Vertex &b, const Vertex &c) const
    {
        const int64_t abx = b.x - a.x, aby = b.y - a.y, acx = c.x - a.x, acy = c.y - a.y;
        if (!_wide) {
            const int64_t d = abx * acy - aby * acx;
            return (d > 0) - (d < 0);
        }
        return (Int128::mul(abx, acy) - Int128::mul(aby, acx)).sign();
    }

    int orient(uint32_t a, uint32_t b, uint32_t c) const { return orient(_vertices[a], _vertices[b], _vertices[c]); }

    // True when d lies inside the circle through the counterclockwise triangle a, b, c; inexact results count as outside.
    bool inCircle(uint32_t ia, uint32_t ib, uint32_t ic, uint32_t id) const
    {
        const Vertex a = _vertices[ia], b = _vertices[ib], c = _vertices[ic], d = _vertices[id];
        if (_exactCircle) {
            const int64_t adx = a.x - d.x, ady = a.y - d.y, bdx = b.x - d.x, bdy = b.y - d.y, cdx = c.x - d.x, cdy = c.y - d.y;
            const int64_t det = (adx * adx + ady * ady) * (bdx * cdy - bdy * cdx) + (bdx * bdx + bdy * bdy) * (cdx * ady - cdy * adx) +
                                (cdx * cdx + cdy * cdy) * (adx * bdy - ady * bdx);
            return det > 0;
        }
        const double adx = (double)(a.x - d.x), ady = (double)(a.y - d.y), bdx = (double)(b.x - d.x), bdy = (double)(b.y - d.y);
        const double cdx = (double)(c.x - d.x), cdy = (double)(c.y - d.y);
        const double al = adx * adx + ady * ady, bl = bdx * bdx + bdy * bdy, cl = cdx * cdx + cdy * cdy;
        const double det = al * (bdx * cdy - bdy * cdx) + bl * (cdx * ady - cdy * adx) + cl * (adx * bdy - ady * bdx);
        const double bound = al * (std::abs(bdx * cdy) + std::abs(bdy * cdx)) + bl * (std::abs(cdx * ady) + std::abs(cdy * adx)) +
                             cl * (std::abs(adx * bdy) + std::abs(ady * bdx));
        return det > bound * 1e-14;
    }

    // For collinear a, p, q other than a: p and q lie on the same side of a.
    static bool sameDirection(const Vertex &a, const Vertex &p, const Vertex &q)
    {
        return p.x != a.x ? (p.x > a.x) == (q.x > a.x) : (p.y > a.y) == (q.y > a.y);
    }

    // For collinear a, p, q with p and q on the same side of a: p is nearer to a than q.
    static bool nearer(const Vertex &a, const Vertex &p, const Vertex &q)
    {
        return q.x != a.x ? std::abs(p.x - a.x) < std::abs(q.x - a.x) : std::abs(p.y - a.y) < std::abs(q.y - a.y);
    }

    // Grid point for the crossing of the properly crossing segments ab and cd: of the corners of the grid cell holding the
    // crossing and the segment ends, the one that moves both segments over the least area, a corner on a tie. A corner lies
    // in the bounding box of both segments.
    Vertex crossing(uint32_t ia, uint32_t ib, uint32_t ic, uint32_t id) const
    {
        const Vertex a = _vertices[ia], b = _vertices[ib], c = _vertices[ic], d = _vertices[id];
        const int64_t ux = b.x - a.x, uy = b.y - a.y, vx = d.x - c.x, vy = d.y - c.y;
        Int128 den = Int128::mul(ux, vy) - Int128::mul(uy, vx);
        Int128 num = Int128::mul(c.x - a.x, vy) - Int128::mul(c.y - a.y, vx);
        if (den.sign() < 0) {
            den = -den;
            num = -num;
        }
        // crossing = a + (num * u) / den, num / den in (0, 1)
        Int128 rx, ry; // remainders in [0, den)
        const int64_t fx = a.x + Int128::floorDiv(num.times(ux), den, rx), fy = a.y + Int128::floorDiv(num.times(uy), den, ry);
        const double px = (double)fx + rx.toDouble() / den.toDouble(), py = (double)fy + ry.toDouble() / den.toDouble();
        auto cost = [&](const Vertex &v) {
            return (Int128::mul(ux, v.y - a.y) - Int128::mul(uy, v.x - a.x)).abs() + (Int128::mul(vx, v.y - c.y) - Int128::mul(vy, v.x - c.x)).abs();
        };
        auto distance = [&](const Vertex &v) { return ((double)v.x - px) * ((double)v.x - px) + ((double)v.y - py) * ((double)v.y - py); };
        // the cell corners, a tie going to the nearer one
        Vertex corner = {fx + (den <= rx + rx), fy + (den <= ry + ry)};
        Int128 cornerCost = cost(corner);
        double cornerDistance = distance(corner);
        for (int k = 0; k < 4; ++k) {
            const Vertex v = {fx + (k & 1), fy + (k >> 1)};
            if ((v.x != fx && rx.sign() == 0) || (v.y != fy && ry.sign() == 0))
                continue;
            const Int128 vc = cost(v);
            const double vd = distance(v);
            if (vc < cornerCost || (vc == cornerCost && vd < cornerDistance)) {
                corner = v;
                cornerCost = vc;
                cornerDistance = vd;
            }
        }
        // a segment end adds no vertex, but must move less area than the corner
        Vertex end = a;
        Int128 endCost = cost(a);
        double endDistance = distance(a);
        for (const Vertex &v : {b, c, d}) {
            const Int128 vc = cost(v);
            const double vd = distance(v);
            if (vc < endCost || (vc == endCost && vd < endDistance)) {
                end = v;
                endCost = vc;
                endDistance = vd;
            }
        }
        return endCost < cornerCost ? end : corner;
    }

    // ---------- mesh ----------

    static uint32_t nextEdge(uint32_t e) { return e % 3 == 2 ? e - 2 : e + 1; }
    static uint32_t prevEdge(uint32_t e) { return e % 3 == 0 ? e + 2 : e - 1; }

    uint32_t newTriangle(uint32_t a, uint32_t b, uint32_t c)
    {
        const uint32_t e = 3 * _triangleCount++;
        _org[e] = a;
        _org[e + 1] = b;
        _org[e + 2] = c;
        _twin[e] = _twin[e + 1] = _twin[e + 2] = None;
        _wind[e] = _wind[e + 1] = _wind[e + 2] = Wind{0, 0};
        return e;
    }

    void link(uint32_t e, uint32_t f)
    {
        _twin[e] = f;
        if (f != None)
            _twin[f] = e;
    }

    void addWind(uint32_t e, uint32_t from, Wind w)
    {
        if (_org[e] != from)
            w = -w;
        _wind[e] = _wind[e] + w;
        if (_twin[e] != None)
            _wind[_twin[e]] = -_wind[e];
    }

    // Calls f with the half-edges leaving v until it returns true; returns that half-edge or None.
    template <class F> uint32_t findOut(uint32_t v, F &&f) const
    {
        const uint32_t e0 = _vertexEdge[v];
        uint32_t e = e0;
        while (true) {
            if (f(e))
                return e;
            const uint32_t t = _twin[prevEdge(e)];
            if (t == None)
                break;
            if (t == e0)
                return None;
            e = t;
        }
        // on the boundary: the other way round
        e = e0;
        while (_twin[e] != None) {
            e = nextEdge(_twin[e]);
            if (f(e))
                return e;
        }
        return None;
    }

    // Half-edge a -> b, or b -> a when that one has no twin, or None.
    uint32_t findEdge(uint32_t a, uint32_t b) const
    {
        uint32_t rv = None;
        findOut(a, [&](uint32_t e) {
            if (_org[nextEdge(e)] == b) {
                rv = e;
                return true;
            }
            if (_org[prevEdge(e)] == b) {
                rv = _twin[prevEdge(e)] != None ? _twin[prevEdge(e)] : prevEdge(e);
                return true;
            }
            return false;
        });
        return rv;
    }

    // Boundary half-edge leaving v, or None inside.
    uint32_t boundaryOut(uint32_t v) const
    {
        const uint32_t e0 = _vertexEdge[v];
        uint32_t e = e0;
        while (_twin[e] != None) {
            e = nextEdge(_twin[e]);
            if (e == e0)
                return None;
        }
        return e;
    }

    // Boundary half-edges after and before boundary half-edge e.
    uint32_t nextBoundary(uint32_t e) const
    {
        uint32_t g = nextEdge(e);
        while (_twin[g] != None)
            g = nextEdge(_twin[g]);
        return g;
    }

    uint32_t prevBoundary(uint32_t e) const
    {
        uint32_t g = prevEdge(e);
        while (_twin[g] != None)
            g = prevEdge(_twin[g]);
        return g;
    }

    // Replaces the diagonal e of a convex quad by the other one. With e = a -> b, c its apex and d the apex of its twin,
    // e's triangle becomes a, d, c and the twin's becomes b, c, d; the edges c -> a and d -> b stay in place.
    void flip(uint32_t e)
    {
        const uint32_t f = _twin[e];
        const uint32_t e1 = nextEdge(e), e2 = prevEdge(e), f1 = nextEdge(f), f2 = prevEdge(f);
        const uint32_t a = _org[e], b = _org[f], c = _org[e2], d = _org[f2];
        const uint32_t te = _twin[e1], tf = _twin[f1];
        const Wind we = _wind[e1], wf = _wind[f1];
        _org[e1] = d;
        _org[f1] = c;
        link(e, tf);
        _wind[e] = wf;
        link(f, te);
        _wind[f] = we;
        link(e1, f1);
        _wind[e1] = _wind[f1] = Wind{0, 0};
        if (tf == None)
            _hullEdge[a] = e;
        if (te == None)
            _hullEdge[b] = f;
        _vertexEdge[a] = e;
        _vertexEdge[b] = f;
        _vertexEdge[c] = e2;
        _vertexEdge[d] = f2;
    }

    // Flips the edges on the stack until the triangles beside them are locally Delaunay; each edge on the stack has the
    // new point as the apex of its triangle. Edges the fill rule sees stay.
    void legalize()
    {
        // flips only improve, so this ends; the guard keeps a bad float predicate from looping
        for (uint32_t flips = 0; !_stack.empty() && flips <= 64 * _triangleCount + 1024;) {
            const uint32_t e = _stack.back();
            _stack.pop_back();
            const uint32_t f = _twin[e];
            if (f == None || significant(_wind[e]) || !inCircle(_org[e], _org[nextEdge(e)], _org[prevEdge(e)], _org[prevEdge(f)]))
                continue;
            flip(e);
            flips++;
            _stack.push_back(e);
            _stack.push_back(prevEdge(f));
        }
        _stack.clear();
    }

    // ---------- Delaunay triangulation of the vertices ----------

    // Adds the vertices in sorted order, each outside the hull of the previous ones; false when all are collinear.
    bool sweep()
    {
        const uint32_t n = _vertexCount;
        uint32_t k = 2;
        while (k < n && orient(0, 1, k) == 0)
            k++;
        if (k == n)
            return false;
        // fan from vertex k over the collinear run 0 .. k - 1, the run on the hull
        const bool left = orient(0, 1, k) > 0;
        for (uint32_t i = 0; i + 1 < k; ++i) {
            const uint32_t e = left ? newTriangle(i, i + 1, k) : newTriangle(i + 1, i, k);
            if (i > 0)
                link(left ? e + 2 : e + 1, left ? e - 2 : e - 1);
            _vertexEdge[i] = left ? e : e + 1;
        }
        const uint32_t last = 3 * (k - 2);
        if (left) {
            for (uint32_t i = 0; i + 1 < k; ++i)
                setHull(i, i + 1, 3 * i);
            setHull(k - 1, k, last + 1);
            setHull(k, 0, 2);
            _vertexEdge[k - 1] = last + 1;
            _vertexEdge[k] = 2;
        } else {
            for (uint32_t i = 0; i + 1 < k; ++i)
                setHull(i + 1, i, 3 * i);
            setHull(0, k, 1);
            setHull(k, k - 1, last + 2);
            _vertexEdge[k - 1] = last;
            _vertexEdge[k] = last + 2;
        }
        _stack.clear();
        for (uint32_t q = k + 1; q < n; ++q)
            addRight(q, q - 1);
        return true;
    }

    void setHull(uint32_t from, uint32_t to, uint32_t e)
    {
        _hullNext[from] = to;
        _hullPrev[to] = from;
        _hullEdge[from] = e;
    }

    // Joins vertex q to the hull edges it sees, starting next to the previous vertex p.
    void addRight(uint32_t q, uint32_t p)
    {
        uint32_t lo = p, hi = p;
        while (orient(_hullPrev[lo], lo, q) < 0)
            lo = _hullPrev[lo];
        while (orient(hi, _hullNext[hi], q) < 0)
            hi = _hullNext[hi];
        uint32_t prev = None;
        for (uint32_t u = lo; u != hi;) {
            const uint32_t w = _hullNext[u];
            const uint32_t e = newTriangle(w, u, q);
            link(e, _hullEdge[u]);
            link(e + 1, prev);
            if (prev == None)
                setHull(lo, q, e + 1);
            prev = e + 2;
            _stack.push_back(e);
            u = w;
        }
        setHull(q, hi, prev);
        _vertexEdge[q] = prev;
        legalize();
    }

    // ---------- constraints ----------

    // Makes the segment a -> b a chain of edges and adds its winding; a crossing with an edge the rule sees adds a
    // vertex at the rounded crossing point and reroutes both through it.
    void insertSegment(uint32_t a, uint32_t b, Wind wind)
    {
        _pending.clear();
        _pending.push_back({a, b, wind});
        while (!_pending.empty()) {
            const Pending s = _pending.back();
            _pending.pop_back();
            if (s.a == s.b)
                continue;
            uint32_t h = findEdge(s.a, s.b);
            if (h != None) {
                addWind(h, s.a, s.wind);
                continue;
            }
            uint32_t hit = None;
            switch (walk(s.a, s.b, hit)) {
            case WalkEnd::Target:
                makeEdge(s.a, s.b);
                break;
            case WalkEnd::Vertex:
                makeEdge(s.a, hit);
                _pending.push_back({hit, s.b, s.wind});
                break;
            case WalkEnd::Crossing:
                resolve(s, hit);
                continue;
            case WalkEnd::Failed:
                assert(false);
                continue;
            }
            h = findEdge(s.a, hit == None ? s.b : hit);
            assert(h != None);
            if (h != None)
                addWind(h, s.a, s.wind);
        }
    }

    // Walks from vertex a towards vertex b listing the crossed edges; stops at b, at a vertex on the way (hit) or at a
    // crossed edge the rule sees (hit = that half-edge).
    WalkEnd walk(uint32_t a, uint32_t b, uint32_t &hit)
    {
        _crossed.clear();
        const Vertex va = _vertices[a], vb = _vertices[b];
        uint32_t h = None;
        findOut(a, [&](uint32_t e) {
            const uint32_t x = _org[nextEdge(e)], y = _org[prevEdge(e)];
            const Vertex &vx = _vertices[x], &vy = _vertices[y];
            const int ox = orient(va, vx, vb);
            if (ox == 0 && sameDirection(va, vx, vb)) {
                hit = x;
                return true;
            }
            const int oy = orient(va, vy, vb);
            if (oy == 0 && sameDirection(va, vy, vb)) {
                hit = y;
                return true;
            }
            if (ox > 0 && oy < 0) {
                h = nextEdge(e);
                return true;
            }
            return false;
        });
        if (hit != None)
            return WalkEnd::Vertex;
        if (h == None)
            return WalkEnd::Failed;
        // h runs from the right of a -> b to its left; a straight walk crosses each triangle once
        for (uint32_t step = 0; step <= _triangleCount; ++step) {
            if (significant(_wind[h])) {
                hit = h;
                return WalkEnd::Crossing;
            }
            _crossed.emplace_back(_org[h], _org[nextEdge(h)]);
            const uint32_t t = _twin[h];
            if (t == None)
                return WalkEnd::Failed;
            const uint32_t z = _org[prevEdge(t)];
            if (z == b)
                return WalkEnd::Target;
            const int o = orient(va, vb, _vertices[z]);
            if (o == 0) {
                hit = z;
                return WalkEnd::Vertex;
            }
            h = o > 0 ? nextEdge(t) : prevEdge(t);
        }
        assert(false);
        return WalkEnd::Failed;
    }

    // Flips the crossed edges until a -> b is an edge: a convex quad gets the other diagonal, which is queued again while
    // it still crosses; a concave one waits.
    void makeEdge(uint32_t a, uint32_t b)
    {
        const Vertex va = _vertices[a], vb = _vertices[b];
        size_t head = 0;
        const size_t limit = 64 + 8 * _crossed.size() * _crossed.size();
        for (size_t step = 0; head < _crossed.size() && step < limit; ++step) {
            const auto [u, v] = _crossed[head++];
            const uint32_t g = findEdge(u, v);
            if (g == None || _twin[g] == None) {
                assert(false);
                continue;
            }
            const uint32_t c = _org[prevEdge(g)], d = _org[prevEdge(_twin[g])];
            if (orient(c, d, u) * orient(c, d, v) < 0) {
                flip(g);
                if (orient(va, vb, _vertices[c]) * orient(va, vb, _vertices[d]) < 0)
                    _crossed.emplace_back(c, d);
            } else {
                _crossed.emplace_back(u, v);
            }
        }
        assert(head == _crossed.size());
    }

    // Segment s crosses edge h: both go through a vertex at the rounded crossing point.
    void resolve(const Pending &s, uint32_t h)
    {
        if (++_work > _workLimit) {
            fail();
            return;
        }
        const uint32_t r = _org[h], l = _org[nextEdge(h)];
        const Wind w = _wind[h];
        _wind[h] = Wind{0, 0};
        if (_twin[h] != None)
            _wind[_twin[h]] = Wind{0, 0};
        const uint32_t x = insertPoint(crossing(s.a, s.b, r, l), r);
        if (x == None) {
            fail();
            return;
        }
        _pending.push_back({x, l, w});
        _pending.push_back({r, x, w});
        _pending.push_back({x, s.b, s.wind});
        _pending.push_back({s.a, x, s.wind});
    }

    void fail()
    {
        _failed = true;
        _pending.clear();
    }

    // ---------- added points ----------

    uint32_t insertPoint(Vertex p, uint32_t from)
    {
        uint32_t where = None;
        const Location location = locate(p, from, where);
        if (location == Location::Vertex)
            return where;
        if (_pointCount + _extraCount >= MaxPoints)
            return None;
        const uint32_t x = _vertexCount++;
        reserveVertices(_vertexCount);
        _vertices[x] = p;
        _outIndex[x] = (Index)(_pointCount + _extraCount++);
        _stack.clear();
        if (location == Location::Triangle)
            splitTriangle(where, x);
        else if (location == Location::Edge)
            splitEdge(where, x);
        else
            extendHull(where, x);
        legalize();
        return x;
    }

    // Finds point p walking straight from vertex s: a vertex, an edge or a triangle holding it (where), or outside
    // beyond a boundary edge it sees.
    Location locate(Vertex p, uint32_t s, uint32_t &where) const
    {
        for (uint32_t guard = 0; guard <= _vertexCount; ++guard) {
            const Vertex vs = _vertices[s];
            if (vs.x == p.x && vs.y == p.y) {
                where = s;
                return Location::Vertex;
            }
            uint32_t h = None, next = None;
            bool done = false;
            Location found = Location::Vertex;
            auto onRay = [&](uint32_t v, uint32_t edge) {
                const Vertex &vv = _vertices[v];
                if (vv.x == p.x && vv.y == p.y) {
                    where = v;
                    found = Location::Vertex;
                    done = true;
                } else if (nearer(vs, p, vv)) {
                    where = edge;
                    found = Location::Edge;
                    done = true;
                } else {
                    next = v;
                }
            };
            findOut(s, [&](uint32_t e) {
                const uint32_t x = _org[nextEdge(e)], y = _org[prevEdge(e)];
                const Vertex &vx = _vertices[x], &vy = _vertices[y];
                const int ox = orient(vs, vx, p);
                if (ox == 0 && sameDirection(vs, vx, p)) {
                    onRay(x, e);
                    return true;
                }
                const int oy = orient(vs, vy, p);
                if (oy == 0 && sameDirection(vs, vy, p)) {
                    onRay(y, prevEdge(e));
                    return true;
                }
                if (ox > 0 && oy < 0) {
                    const int o = orient(vx, vy, p);
                    if (o > 0) {
                        where = e / 3;
                        found = Location::Triangle;
                        done = true;
                    } else if (o == 0) {
                        where = nextEdge(e);
                        found = Location::Edge;
                        done = true;
                    } else {
                        h = nextEdge(e);
                    }
                    return true;
                }
                return false;
            });
            if (done)
                return found;
            if (next != None) {
                s = next;
                continue;
            }
            if (h == None) {
                // outside, in the outer angle of hull vertex s
                const uint32_t out = boundaryOut(s);
                if (out != None) {
                    if (orient(vs, _vertices[_org[nextEdge(out)]], p) < 0) {
                        where = out;
                        return Location::Outside;
                    }
                    const uint32_t in = prevBoundary(out);
                    if (orient(_vertices[_org[in]], vs, p) < 0) {
                        where = in;
                        return Location::Outside;
                    }
                }
                assert(false);
                where = s;
                return Location::Vertex;
            }
            // h runs from the right of s -> p to its left
            for (uint32_t step = 0;; ++step) {
                if (step > _triangleCount) {
                    assert(false);
                    where = s;
                    return Location::Vertex;
                }
                const uint32_t t = _twin[h];
                if (t == None) {
                    where = h;
                    return Location::Outside;
                }
                const uint32_t l = _org[t], r = _org[nextEdge(t)], z = _org[prevEdge(t)];
                const Vertex &vz = _vertices[z];
                const int o1 = orient(_vertices[r], vz, p), o2 = orient(vz, _vertices[l], p);
                if (o1 >= 0 && o2 >= 0) {
                    if (o1 > 0 && o2 > 0) {
                        where = t / 3;
                        return Location::Triangle;
                    }
                    if (o1 == 0 && o2 == 0) {
                        where = z;
                        return Location::Vertex;
                    }
                    where = o1 == 0 ? nextEdge(t) : prevEdge(t);
                    return Location::Edge;
                }
                const int oz = orient(vs, p, vz);
                if (oz == 0) {
                    s = z;
                    break;
                }
                h = oz > 0 ? nextEdge(t) : prevEdge(t);
            }
        }
        assert(false);
        where = s;
        return Location::Vertex;
    }

    // Splits triangle t into three around the new vertex x.
    void splitTriangle(uint32_t t, uint32_t x)
    {
        const uint32_t e0 = 3 * t, e1 = e0 + 1, e2 = e0 + 2;
        const uint32_t a = _org[e0], b = _org[e1], c = _org[e2];
        const uint32_t t1 = _twin[e1], t2 = _twin[e2];
        const Wind w1 = _wind[e1], w2 = _wind[e2];
        const uint32_t g = newTriangle(b, c, x), k = newTriangle(c, a, x);
        _org[e2] = x;
        _twin[e1] = _twin[e2] = None;
        _wind[e1] = _wind[e2] = Wind{0, 0};
        // t: a -> b -> x, g: b -> c -> x, k: c -> a -> x
        link(g, t1);
        _wind[g] = w1;
        link(k, t2);
        _wind[k] = w2;
        link(e1, g + 2);
        link(g + 1, k + 2);
        link(k + 1, e2);
        _vertexEdge[a] = e0;
        _vertexEdge[b] = g;
        _vertexEdge[c] = k;
        _vertexEdge[x] = e2;
        _stack.push_back(e0);
        _stack.push_back(g);
        _stack.push_back(k);
    }

    // Splits the edge of half-edge h and the triangles beside it at the new vertex x; both halves keep its winding.
    void splitEdge(uint32_t h, uint32_t x)
    {
        const uint32_t h1 = nextEdge(h), h2 = prevEdge(h), f = _twin[h];
        const uint32_t a = _org[h], b = _org[h1], c = _org[h2];
        const uint32_t th1 = _twin[h1];
        const Wind wh1 = _wind[h1], w = _wind[h];
        // h's triangle becomes a -> x -> c, a new one x -> b -> c
        const uint32_t g = newTriangle(x, b, c);
        _org[h1] = x;
        link(g + 1, th1);
        _wind[g + 1] = wh1;
        link(h1, g + 2);
        _wind[h1] = Wind{0, 0};
        _wind[g] = w;
        _vertexEdge[a] = h;
        _vertexEdge[b] = g + 1;
        _vertexEdge[c] = h2;
        _vertexEdge[x] = h1;
        _stack.push_back(h2);
        _stack.push_back(g + 1);
        if (f == None) {
            _twin[h] = None;
            _twin[g] = None;
            return;
        }
        // f's triangle b -> a -> d becomes b -> x -> d, a new one x -> a -> d
        const uint32_t f1 = nextEdge(f), f2 = prevEdge(f);
        const uint32_t d = _org[f2];
        const uint32_t tf1 = _twin[f1];
        const Wind wf1 = _wind[f1];
        const uint32_t k = newTriangle(x, a, d);
        _org[f1] = x;
        link(k + 1, tf1);
        _wind[k + 1] = wf1;
        link(f1, k + 2);
        _wind[f1] = Wind{0, 0};
        _wind[k] = -w;
        link(h, k);
        link(g, f);
        _vertexEdge[d] = f2;
        _stack.push_back(f2);
        _stack.push_back(k + 1);
    }

    // Joins the new vertex x, outside the hull, to the boundary edges it sees around boundary half-edge h.
    void extendHull(uint32_t h, uint32_t x)
    {
        const Vertex vx = _vertices[x];
        auto sees = [&](uint32_t e) { return orient(_vertices[_org[e]], _vertices[_org[nextEdge(e)]], vx) < 0; };
        uint32_t first = h;
        for (uint32_t e = prevBoundary(first); e != h && sees(e); e = prevBoundary(e))
            first = e;
        _chain.clear();
        uint32_t e = first;
        do {
            _chain.push_back(e);
            e = nextBoundary(e);
        } while (e != first && sees(e));
        uint32_t prev = None;
        for (uint32_t edge : _chain) {
            const uint32_t u = _org[edge], w = _org[nextEdge(edge)];
            const uint32_t g = newTriangle(w, u, x);
            link(g, edge);
            _wind[g] = -_wind[edge];
            link(g + 1, prev);
            prev = g + 2;
            _vertexEdge[u] = g + 1;
            _stack.push_back(g);
        }
        _vertexEdge[x] = prev;
    }

    // ---------- fill ----------

    // Winding numbers of the triangles, from the outside in, and the kept ones.
    void fill()
    {
        const uint32_t triangles = _triangleCount;
        if (_triangleWind.size() < triangles)
            _triangleWind.resize(std::max<size_t>(triangles, 2 * _triangleWind.size()));
        _state.assign(triangles, Unseen);
        uint32_t start = None;
        for (uint32_t e = 0; e < 3 * triangles; ++e) {
            if (_twin[e] == None) {
                start = e;
                break;
            }
        }
        if (start == None)
            return;
        _triangleWind[start / 3] = _wind[start];
        _state[start / 3] = Seen;
        _stack.clear();
        _stack.push_back(start / 3);
        while (!_stack.empty()) {
            const uint32_t t = _stack.back();
            _stack.pop_back();
            const Wind w = _triangleWind[t];
            for (uint32_t e = 3 * t; e < 3 * t + 3; ++e) {
                const uint32_t f = _twin[e];
                if (f == None) {
                    assert(sameForRule(w, _wind[e]));
                    continue;
                }
                const uint32_t n = f / 3;
                const Wind nw = w + _wind[f];
                if (_state[n] == Unseen) {
                    _state[n] = Seen;
                    _triangleWind[n] = nw;
                    _stack.push_back(n);
                } else {
                    assert(sameForRule(_triangleWind[n], nw));
                }
            }
        }
        for (uint32_t t = 0; t < triangles; ++t)
            _state[t] = keep(_triangleWind[t]) ? Free : Dropped;
    }

    // ---------- strips ----------

    // Covers the kept triangles with strips. A strip starts at a triangle with the fewest free neighbors, takes the
    // longest of its zig-zag runs and grows both ways.
    void buildStrips()
    {
        const uint32_t triangles = _triangleCount;
        if (_degree.size() < triangles)
            _degree.resize(std::max<size_t>(triangles, 2 * _degree.size()));
        _candidates.clear();
        for (uint32_t t = 0; t < triangles; ++t) {
            if (_state[t] != Free)
                continue;
            uint8_t d = 0;
            for (uint32_t e = 3 * t; e < 3 * t + 3; ++e)
                d += _twin[e] != None && _state[_twin[e] / 3] == Free;
            _degree[t] = d;
            if (d <= 1)
                _candidates.push_back(t);
        }
        std::reverse(_candidates.begin(), _candidates.end());
        uint32_t scan = 0;
        while (true) {
            uint32_t t = None;
            while (!_candidates.empty()) {
                const uint32_t c = _candidates.back();
                _candidates.pop_back();
                if (_state[c] == Free) {
                    t = c;
                    break;
                }
            }
            if (t == None) {
                while (scan < triangles && _state[scan] != Free)
                    scan++;
                if (scan == triangles)
                    break;
                t = scan;
            }
            emitStrip(t);
        }
    }

    // Free triangles in a row from the triangle of exit half-edge g; the newest strip vertex is g's origin when atStart.
    uint32_t runLength(uint32_t g, bool atStart) const
    {
        uint32_t count = 0;
        while (true) {
            const uint32_t u = _twin[g];
            if (u == None || _state[u / 3] != Free)
                return count;
            count++;
            g = atStart ? nextEdge(u) : prevEdge(u);
            atStart = !atStart;
        }
    }

    // Takes the run into _run (new vertices) and marks its triangles used.
    void takeRun(uint32_t g, bool atStart)
    {
        while (true) {
            const uint32_t u = _twin[g];
            if (u == None || _state[u / 3] != Free)
                return;
            _state[u / 3] = Used;
            _taken.push_back(u / 3);
            _run.push_back(_org[prevEdge(u)]);
            g = atStart ? nextEdge(u) : prevEdge(u);
            atStart = !atStart;
        }
    }

    void emitStrip(uint32_t t)
    {
        _state[t] = Used;
        const uint32_t e = 3 * t;
        // option o: rotation o / 2, counterclockwise order when o is even
        uint32_t best = 0, bestLength = 0;
        for (uint32_t o = 0; o < 6; ++o) {
            const uint32_t r = o / 2, h0 = e + r, h1 = e + (r + 1) % 3, h2 = e + (r + 2) % 3;
            const uint32_t length = (o & 1) == 0 ? runLength(h1, false) + runLength(h0, true) : runLength(h1, true) + runLength(h2, false);
            if (length > bestLength || o == 0) {
                best = o;
                bestLength = length;
            }
        }
        const uint32_t r = best / 2, h0 = e + r, h1 = e + (r + 1) % 3, h2 = e + (r + 2) % 3;
        const bool ccw = (best & 1) == 0;
        _taken.clear();
        _taken.push_back(t);
        _run.clear();
        if (ccw)
            takeRun(h0, true);
        else
            takeRun(h2, false);
        _stripVertices.clear();
        for (size_t i = _run.size(); i > 0; --i)
            _stripVertices.push_back(_run[i - 1]);
        const uint32_t a = _org[h0], b = _org[h1], c = _org[h2];
        _stripVertices.push_back(a);
        _stripVertices.push_back(ccw ? b : c);
        _stripVertices.push_back(ccw ? c : b);
        _run.clear();
        takeRun(h1, !ccw);
        for (uint32_t v : _run)
            _stripVertices.push_back(v);
        for (uint32_t v : _stripVertices)
            _strips.push_back(_outIndex[v]);
        _stripEnds.push_back((uint32_t)_strips.size());
        _stripCW.push_back(orient(_stripVertices[0], _stripVertices[1], _stripVertices[2]) < 0);
        // neighbors of the used triangles lose a free neighbor
        for (uint32_t used : _taken) {
            for (uint32_t g = 3 * used; g < 3 * used + 3; ++g) {
                const uint32_t n = _twin[g] == None ? None : _twin[g] / 3;
                if (n != None && _state[n] == Free && --_degree[n] <= 1)
                    _candidates.push_back(n);
            }
        }
    }
};

#if defined(EARSTRIP_SMALL_CONSTRAINED) && defined(__clang__)
#    pragma clang attribute pop
#elif defined(EARSTRIP_SMALL_CONSTRAINED) && defined(__GNUC__)
#    pragma GCC pop_options
#endif

} // namespace detail

// Triangulates polygons into triangle strips. An instance is meant to be reused: its buffers only grow.
// Index is the type of the vertex indices in the strips; a polygon may have at most as many vertices as it can count.
template <class Index = uint32_t> class Triangulator {
    static_assert(std::is_unsigned<Index>::value, "Index must be an unsigned integer type");

public:
    // A polygon as a container of rings, each a container of points; ring 0 is the outer ring, the others are holes.
    // Indices count the vertices of all rings in order. Either orientation is fine, a repeated closing point too.
    template <class Polygon> Status run(const Polygon &polygon) { return runRings(detail::NestedRings<Polygon>{polygon}); }

    // A polygon as flat coordinates, dim numbers per vertex (x and y first), and the vertex index where each hole starts.
    template <class T, class H = uint32_t>
    Status run(const T *coords, size_t vertexCount, size_t dim = 2, const H *holeStarts = nullptr, size_t holeCount = 0)
    {
        return runRings(detail::FlatRings<T, H>{coords, vertexCount, dim, holeStarts, holeCount});
    }

    // Vertex indices of all strips, one after another, and the end of each strip in them. Within a strip the triangles
    // alternate as a GPU expects; stripClockwise() tells, per strip, whether its first triangle runs clockwise.
    const std::vector<Index> &strips() const { return _strips; }
    const std::vector<uint32_t> &stripEnds() const { return _stripEnds; }
    const std::vector<uint8_t> &stripClockwise() const { return _stripCW; }

    // The same triangles as a list, three indices each, every one counter-clockwise (positive area with y up).
    void triangles(std::vector<Index> &out) const
    {
        out.clear();
        uint32_t begin = 0;
        for (size_t s = 0; s < _stripEnds.size(); ++s) {
            const uint32_t end = _stripEnds[s];
            for (uint32_t k = begin; k + 2 < end; ++k) {
                const bool flip = (((k - begin) & 1) != 0) != (_stripCW[s] != 0);
                out.push_back(_strips[k + (flip ? 1 : 0)]);
                out.push_back(_strips[k + (flip ? 0 : 1)]);
                out.push_back(_strips[k + 2]);
            }
            begin = end;
        }
    }

    // Writes a triangle list instead of strips: run() then fills triangleList() (three indices per triangle, each
    // counter-clockwise) and leaves strips() empty. Cheaper than strips plus triangles() when a list is the goal.
    void setTriangleList(bool on) { _listMode = on; }
    const std::vector<Index> &triangleList() const { return _list; }

    // True when the last run left out a hole that could not be joined to the boundary.
    bool holesLeftOut() const { return _droppedHoles; }

    // When the boundary runs out of ears, cut the corners that do the least damage instead of giving up (default on).
    // Only used when the constrained triangulation is off or fails.
    void setForcedCuts(bool on) { _forcedCuts = on; }

    // Polygons the ears cannot finish, those with a hole the ears leave out (when that covers the rings' area better)
    // and those with more than 128 rings go to the constrained triangulation (default on): Status::Constrained.
    void setConstrained(bool on) { _constrained = on; }

    // Lets the constrained triangulation add points where rings cross (default on); off, such polygons take the ears'
    // way instead, so every index is an input vertex.
    void setExtraPoints(bool on) { _extraAllowed = on; }

    // Points added by the last run as x, y pairs: index n + k in the strips is point k, n being the number of input
    // vertices of all rings.
    const std::vector<double> &extraPoints() const { return _extra; }

private:
    static constexpr uint32_t None = UINT32_MAX;
    // Joining a hole to the boundary scans the boundary: with more rings the constrained triangulation is faster
    static constexpr uint32_t MaxEarRings = 128;
    // Up to this many blockers an ear test scans them all; more are sorted into buckets of about BlockersPerBucket.
    static constexpr uint32_t SmallBlockers = 32;
    static constexpr double BlockersPerBucket = 2.0;
    // Largest boundary the second run splits at its crossings; larger ones go straight to forced cuts.
    static constexpr uint32_t SplitLimit = 4096;

    struct Node {
        double x, y;
        uint32_t prev, next;
        Index index;
        uint8_t reflex; // corner is not strictly convex
        // Joining rings uses shared; classification then initializes indexed for clipping.
        union {
            uint8_t shared;  // another ring has a vertex at this position
            uint8_t indexed; // listed in the blocker index
        };
        uint16_t ring; // 0 outer, holes from 1
    };

    struct Item {
        double x, y;
        uint32_t node;
    };

    struct Hole {
        double x, y;
        uint32_t node; // rightmost vertex
        uint32_t first, count;
        uint16_t ring;
    };

    std::vector<Node> _nodes;
    std::vector<Index> _strips;
    std::vector<uint32_t> _stripEnds;
    std::vector<uint8_t> _stripCW; // first triangle of the strip runs clockwise
    std::vector<Index> _list;      // triangle list mode output
    bool _listMode = false;
    uint32_t _stripLen = 0; // indices in the strip being written
    bool _cw = false;       // its first triangle runs clockwise
    Index _p0 = 0, _p1 = 0; // its last two indices
    std::vector<uint32_t> _blockers;
    std::vector<Hole> _holes;
    std::vector<uint8_t> _merged;   // ring is part of the boundary
    std::vector<uint32_t> _order;   // holes by decreasing rightmost x
    std::vector<uint32_t> _queue;   // merged node ranges to look for touching holes
    std::vector<uint32_t> _samePos; // cyclic lists of nodes at one position
    std::vector<uint32_t> _cycle;   // nodes of a boundary in walking order
    std::vector<uint32_t> _loops;   // start node and length of loops still to clip
    std::vector<uint32_t> _hash;
    std::vector<Item> _items;           // blockers by bucket; a single bucket stays compact, otherwise gone ones get x = inf
    std::vector<uint32_t> _bucketStart; // first item of each bucket
    std::vector<uint32_t> _slot;        // item of each indexed node
    double _bucketOrigin = 0, _bucketScale = 0;
    uint32_t _buckets = 1;
    bool _axisY = false;
    uint32_t _live = 0;         // blockers still reflex
    uint32_t _built = 0;        // items in the index when it was built
    uint32_t _blockerCount = 0; // blockers found by the last classification
    bool _droppedHoles = false;
    uint32_t _outerCount = 0;
    bool _forced = false;  // a forced cut was made or the boundary untangled
    bool _careful = false; // second run of a polygon the ears could not finish
    bool _forcedCuts = true;
    bool _constrained = true;
    bool _extraAllowed = true;
    detail::Constrained<Index> _cdt;
    std::vector<double> _extra; // points the constrained triangulation added

    static double cross(double ax, double ay, double bx, double by, double cx, double cy) { return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax); }
    static double cross(const Node &a, const Node &b, const Node &c) { return cross(a.x, a.y, b.x, b.y, c.x, c.y); }
    static bool samePos(const Node &a, const Node &b) { return a.x == b.x && a.y == b.y; }

    // The ears are only right on a simple boundary. When they run out, the polygon is cut again from the start with
    // its crossings untangled first, so that no ear cut before the stall spans the wrong area.
    template <class Rings> Status runRings(const Rings &rings)
    {
        _extra.clear();
        _careful = false;
        if (_constrained && rings.count() > MaxEarRings && cover(rings, false))
            return Status::Constrained;
        const Status s = runOnce(rings);
        if (_constrained && (s == Status::Refused || s == Status::HolesLeftOut) && cover(rings, s == Status::HolesLeftOut))
            return Status::Constrained;
        if (s != Status::Refused || !_forcedCuts)
            return s;
        _careful = true;
        runOnce(rings);
        return Status::Approximate;
    }

    // Replaces the result by the constrained triangulation; false keeps what the ears did: when it hits a limit, adds
    // points that are not allowed or, with compare, covers an area no closer to the rings' than the ears.
    template <class Rings> EARSTRIP_RARE bool cover(const Rings &rings, bool compare)
    {
        if (!_cdt.load(rings) || !_cdt.build() || (!_extraAllowed && _cdt.extraCount() > 0))
            return false;
        if (compare) {
            const double area = _cdt.inputRingsArea();
            if (_listMode ? _cdt.closerList(_list, area) : _cdt.closer(_strips, _stripEnds, area))
                return false;
        }
        std::swap(_strips, _cdt.strips());
        std::swap(_stripEnds, _cdt.stripEnds());
        std::swap(_stripCW, _cdt.stripClockwise());
        _cdt.extraPoints(_extra);
        _droppedHoles = false;
        if (_listMode) {
            triangles(_list);
            _strips.clear();
            _stripEnds.clear();
            _stripCW.clear();
        }
        return true;
    }

    template <class Rings> Status runOnce(const Rings &rings)
    {
        _strips.clear();
        _stripEnds.clear();
        _stripCW.clear();
        _list.clear();
        _droppedHoles = false;
        _forced = false;
        const uint32_t ringCount = (uint32_t)rings.count();
        if (ringCount == 0)
            return Status::Exact;
        uint32_t total = 0;
        for (uint32_t r = 0; r < ringCount; ++r)
            total += (uint32_t)rings.size(r);
        if (_nodes.size() < total + 2 * ringCount) {
            const size_t capacity = total + 2 * ringCount;
            _nodes.resize(capacity);
            _samePos.resize(capacity);
            _slot.resize(capacity);
            _blockers.resize(capacity);
            _items.resize(capacity);
        }

        double area;
        uint32_t rightmost;
        uint32_t count = loadRing(rings, 0, 0, 0, area, rightmost);
        if (count < 3)
            return Status::Exact;
        const uint32_t outerCount = count;
        _outerCount = count;
        _holes.clear();
        if (ringCount == 1) {
            if (count == 3) {
                // a triangle needs no tests, a flat one has nothing to cover
                if (area != 0)
                    emitConvex(3, area < 0);
                return Status::Exact;
            }
            if (count == 4 && convexQuad(area)) {
                emitConvex(4, area < 0);
                return Status::Exact;
            }
            if (classifyRing(count, area < 0)) {
                emitConvex(count, area < 0);
                return Status::Exact;
            }
            return finish(_careful ? clipLoops(0, count) : clip(0, count));
        }

        linkRing(0, count, area < 0);
        setRing(0, count, 0);
        uint32_t used = count;
        uint32_t base = (uint32_t)rings.size(0);
        _holes.clear();
        for (uint32_t r = 1; r < ringCount; ++r) {
            double holeArea;
            const uint32_t holeCount = loadRing(rings, r, used, base, holeArea, rightmost);
            base += (uint32_t)rings.size(r);
            if (holeCount < 3 || holeArea == 0)
                continue;
            linkRing(used, holeCount, holeArea > 0);
            const uint16_t id = (uint16_t)(_holes.size() + 1);
            setRing(used, holeCount, id);
            _holes.push_back({_nodes[rightmost].x, _nodes[rightmost].y, rightmost, used, holeCount, id});
            used += holeCount;
        }
        findTouches(used);
        _merged.assign(_holes.size() + 1, 0);
        _merged[0] = 1;
        _order.clear();
        for (uint32_t h = 0; h < _holes.size(); ++h)
            _order.push_back(h);
        std::sort(_order.begin(), _order.end(), [this](uint32_t a, uint32_t b) {
            const Hole &ha = _holes[a], &hb = _holes[b];
            return ha.x != hb.x ? ha.x > hb.x : ha.y < hb.y;
        });
        // Rings touching the boundary join at the shared vertex; the rest get a bridge, rightmost first.
        joinTouchingRings(0, outerCount);
        for (uint32_t h : _order) {
            const Hole &hole = _holes[h];
            if (_merged[hole.ring])
                continue;
            if (!bridge(hole.node, used)) {
                _droppedHoles = true;
                continue;
            }
            _merged[hole.ring] = 1;
            joinTouchingRings(hole.first, hole.count);
        }
        const uint32_t boundary = classifyCycle(0);
        return finish(_careful ? clipLoops(0, boundary) : clip(0, boundary));
    }

    Status finish(bool clipped) const
    {
        if (!clipped)
            return Status::Refused;
        if (_forced)
            return Status::Approximate;
        return _droppedHoles ? Status::HolesLeftOut : Status::Exact;
    }

    // Copies ring r into nodes from first on, skipping repeated points; returns the kept count, twice the signed area
    // and the rightmost node.
    template <class Rings> uint32_t loadRing(const Rings &rings, uint32_t r, uint32_t first, uint32_t indexBase, double &area, uint32_t &rightmost)
    {
        Node *n = _nodes.data() + first;
        const uint32_t size = (uint32_t)rings.size(r);
        if (size < 3)
            return 0;
        double lastX = rings.x(r, 0), lastY = rings.y(r, 0), sum = 0, rightX = lastX;
        n[0].x = lastX;
        n[0].y = lastY;
        n[0].index = (Index)indexBase;
        uint32_t count = 1, right = 0;
        for (uint32_t i = 1; i < size; ++i) {
            const double x = rings.x(r, i), y = rings.y(r, i);
            Node &node = n[count];
            node.x = x;
            node.y = y;
            node.index = (Index)(indexBase + i);
            // a repeated point adds nothing to the area and is overwritten by the next one
            sum += (lastX - x) * (lastY + y);
            right = x > rightX ? count : right;
            rightX = x > rightX ? x : rightX;
            count += x != lastX || y != lastY;
            lastX = x;
            lastY = y;
        }
        sum += (lastX - n[0].x) * (lastY + n[0].y);
        while (count > 1 && samePos(n[count - 1], n[0]))
            count--;
        area = sum;
        rightmost = first + (right < count ? right : 0);
        return count;
    }

    // Links nodes [first, first + count) into a cycle, backwards when reversed.
    void linkRing(uint32_t first, uint32_t count, bool reversed)
    {
        Node *n = _nodes.data();
        const uint32_t last = first + count - 1;
        if (!reversed) {
            for (uint32_t i = first; i < last; ++i) {
                n[i].next = i + 1;
                n[i + 1].prev = i;
            }
            n[last].next = first;
            n[first].prev = last;
        } else {
            for (uint32_t i = first; i < last; ++i) {
                n[i].prev = i + 1;
                n[i + 1].next = i;
            }
            n[last].prev = first;
            n[first].next = last;
        }
    }

    void setRing(uint32_t first, uint32_t count, uint16_t ring)
    {
        Node *n = _nodes.data();
        for (uint32_t i = first; i < first + count; ++i)
            n[i].ring = ring;
    }

    // Chains nodes at equal positions and flags positions that several rings share.
    void findTouches(uint32_t used)
    {
        Node *n = _nodes.data();
        const uint32_t bitCount = detail::bitWidth(used * 2 - 1);
        const uint32_t mask = (1u << bitCount) - 1;
        _hash.assign(mask + 1, None);
        uint32_t *same = _samePos.data();
        for (uint32_t i = 0; i < used; ++i) {
            Node &a = n[i];
            a.shared = 0;
            same[i] = i;
            uint64_t key = detail::bits(a.x) * 0x9E3779B97F4A7C15ull ^ detail::bits(a.y) * 0xC2B2AE3D27D4EB4Full;
            key ^= key >> 31;
            key *= 0xBF58476D1CE4E5B9ull;
            uint32_t h = (uint32_t)(key >> 40) & mask;
            while (true) {
                const uint32_t j = _hash[h];
                if (j == None) {
                    _hash[h] = i;
                    break;
                }
                if (samePos(n[j], a)) {
                    same[i] = same[j];
                    same[j] = i;
                    if (n[j].ring != a.ring)
                        a.shared = n[j].shared = 1;
                    break;
                }
                h = (h + 1) & mask;
            }
        }
    }

    // Joins every unmerged hole that shares a vertex with the merged nodes [first, first + count), transitively.
    void joinTouchingRings(uint32_t first, uint32_t count)
    {
        Node *n = _nodes.data();
        const uint32_t *same = _samePos.data();
        _queue.clear();
        _queue.push_back(first);
        _queue.push_back(count);
        while (!_queue.empty()) {
            const uint32_t end = _queue.back() + _queue[_queue.size() - 2];
            const uint32_t begin = _queue[_queue.size() - 2];
            _queue.resize(_queue.size() - 2);
            for (uint32_t s = begin; s < end; ++s) {
                if (!n[s].shared)
                    continue;
                for (uint32_t v = same[s]; v != s; v = same[v]) {
                    const uint16_t ring = n[v].ring;
                    if (_merged[ring] || !joinTouching(_holes[ring - 1]))
                        continue;
                    _merged[ring] = 1;
                    _queue.push_back(_holes[ring - 1].first);
                    _queue.push_back(_holes[ring - 1].count);
                }
            }
        }
    }

    // Splices a hole into the boundary at a vertex they share; false when it touches nothing merged.
    bool joinTouching(const Hole &hole)
    {
        Node *n = _nodes.data();
        const uint32_t *same = _samePos.data();
        for (uint32_t s = hole.first; s < hole.first + hole.count; ++s) {
            if (!n[s].shared)
                continue;
            const Node &hs = n[s], &hp = n[hs.prev], &hn = n[hs.next];
            for (uint32_t v = same[s]; v != s; v = same[v]) {
                const Node &bv = n[v];
                if (!_merged[bv.ring])
                    continue;
                const Node &bp = n[bv.prev], &bn = n[bv.next];
                if (!inSector(v, hp.x, hp.y, false) || !inSector(v, hn.x, hn.y, false) || !inSector(s, bp.x, bp.y, false) || !inSector(s, bn.x, bn.y, false))
                    continue;
                // v -> hole after s ... s -> boundary after v
                const uint32_t vn = bv.next, sn = hs.next;
                n[v].next = sn;
                n[sn].prev = v;
                n[s].next = vn;
                n[vn].prev = s;
                return true;
            }
        }
        return false;
    }

    // Links a single ring stored in array order, flags its corners and lists the reflex ones as blockers;
    // true when the ring is convex: every corner strictly convex and the edges turn around once.
    bool classifyRing(uint32_t count, bool reversed)
    {
        Node *n = _nodes.data();
        uint32_t *blockers = _blockers.data();
        Item *items = _items.data();
        uint32_t *slot = _slot.data();
        const double sign = reversed ? -1.0 : 1.0;
        uint32_t found = 0, turns = 0;
        int firstDir = 0, lastDir = 0;
        double ex = n[0].x - n[count - 1].x, ey = n[0].y - n[count - 1].y;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t prev = i == 0 ? count - 1 : i - 1, next = i + 1 == count ? 0 : i + 1;
            Node &node = n[i];
            const double fx = n[next].x - node.x, fy = n[next].y - node.y;
            const bool reflex = (ex * fy - ey * fx) * sign <= 0;
            node.reflex = node.indexed = reflex;
            node.prev = reversed ? next : prev;
            node.next = reversed ? prev : next;
            // written for every node, kept only for reflex ones
            blockers[found] = i;
            items[found] = {node.x, node.y, i};
            slot[i] = found;
            found += reflex;
            // a ring that turns around once switches the x direction of its edges twice
            const int dir = (fx > 0) - (fx < 0);
            turns += dir != 0 && lastDir != 0 && dir != lastDir;
            firstDir = firstDir ? firstDir : dir;
            lastDir = dir ? dir : lastDir;
            ex = fx;
            ey = fy;
        }
        turns += firstDir != lastDir;
        _blockerCount = found;
        return found == 0 && turns == 2;
    }

    // Flags the corners of the linked boundary and collects blockers; returns the vertex count.
    uint32_t classifyCycle(uint32_t start)
    {
        Node *n = _nodes.data();
        uint32_t *blockers = _blockers.data();
        Item *items = _items.data();
        uint32_t *slot = _slot.data();
        uint32_t count = 0, found = 0, v = start;
        do {
            Node &node = n[v];
            const bool reflex = cross(n[node.prev], node, n[node.next]) <= 0;
            node.reflex = node.indexed = reflex;
            blockers[found] = v;
            items[found] = {node.x, node.y, v};
            slot[v] = found;
            found += reflex;
            count++;
            v = node.next;
        } while (v != start);
        _blockerCount = found;
        return count;
    }

    // True when all four corners of a single quad turn the way of its area.
    bool convexQuad(double area) const
    {
        const Node *n = _nodes.data();
        const double sign = area < 0 ? -1.0 : 1.0;
        return std::min(std::min(cross(n[3], n[0], n[1]) * sign, cross(n[0], n[1], n[2]) * sign),
                   std::min(cross(n[1], n[2], n[3]) * sign, cross(n[2], n[3], n[0]) * sign)) > 0;
    }

    // A convex ring is one zig-zag strip, wound the way the ring runs.
    void emitConvex(uint32_t count, bool clockwise)
    {
        const Node *n = _nodes.data();
        uint32_t lo = 1, hi = count - 1;
        beginStrip(clockwise);
        put(n[0].index);
        while (lo <= hi) {
            put(n[lo++].index);
            if (lo <= hi)
                put(n[hi--].index);
        }
        endStrip(clockwise);
    }

    EARSTRIP_INLINE void beginStrip(bool clockwise)
    {
        _stripLen = 0;
        _cw = clockwise;
    }

    // Adds an index to the strip being written; in list mode every index from the third on writes its triangle.
    EARSTRIP_INLINE void put(Index i)
    {
        if (_listMode) {
            if (_stripLen >= 2) {
                const bool flip = (((_stripLen - 2) & 1) != 0) != _cw;
                _list.push_back(flip ? _p1 : _p0);
                _list.push_back(flip ? _p0 : _p1);
                _list.push_back(i);
            }
            _p0 = _p1;
            _p1 = i;
        } else {
            _strips.push_back(i);
        }
        _stripLen++;
    }

    EARSTRIP_INLINE void endStrip(bool clockwise)
    {
        if (_listMode)
            return;
        _stripEnds.push_back((uint32_t)_strips.size());
        _stripCW.push_back(clockwise);
    }

    // True when point p lies in the sector of node t's corner.
    bool inSector(uint32_t t, double px, double py, bool strict) const
    {
        const Node *n = _nodes.data();
        const Node &v = n[t], &a = n[v.prev], &c = n[v.next];
        const double c1 = cross(a.x, a.y, v.x, v.y, px, py), c2 = cross(v.x, v.y, c.x, c.y, px, py);
        if (cross(a, v, c) > 0)
            return strict ? c1 > 0 && c2 > 0 : c1 >= 0 && c2 >= 0;
        return strict ? c1 > 0 || c2 > 0 : c1 >= 0 || c2 >= 0;
    }

    // Among the boundary nodes at target's position, the one whose sector contains p, or None.
    uint32_t pickCopy(uint32_t target, double px, double py) const
    {
        if (inSector(target, px, py, true))
            return target;
        const Node *n = _nodes.data();
        const uint32_t *same = _samePos.data();
        uint32_t loose = inSector(target, px, py, false) ? target : None;
        for (uint32_t v = same[target]; v != target; v = same[v]) {
            if (!_merged[n[v].ring])
                continue;
            if (inSector(v, px, py, true))
                return v;
            if (loose == None && inSector(v, px, py, false))
                loose = v;
        }
        return loose;
    }

    // Joins the hole through its rightmost node m to the boundary (Eberly); false when no boundary vertex is visible.
    bool bridge(uint32_t m, uint32_t &used)
    {
        Node *n = _nodes.data();
        const uint8_t *merged = _merged.data();
        const double mx = n[m].x, my = n[m].y;
        const Node &hp = n[n[m].prev], &hn = n[n[m].next];
        double bestX = std::numeric_limits<double>::infinity(), rayX = bestX;
        uint32_t edge = None, onEdge = None, onRay = None;
        bool up = false;
        // the boundary is every node of a merged ring, each with its outgoing edge
        for (uint32_t v = 0; v < used; ++v) {
            const Node &a = n[v];
            if (!merged[a.ring])
                continue;
            const Node &b = n[a.next];
            // nearest vertex lying on the ray itself, which the crossing test passes over
            if (a.y == my && a.x > mx && a.x < rayX) {
                rayX = a.x;
                onRay = v;
            }
            bool through = false;
            if ((a.y <= my) != (b.y <= my)) {
                // computed from the lower end so that coincident bridge edges give the same x
                const Node &lo = a.y < b.y ? a : b, &hi = a.y < b.y ? b : a;
                const double x = lo.y == my ? lo.x : lo.x + (my - lo.y) * (hi.x - lo.x) / (hi.y - lo.y);
                if (x >= mx) {
                    const bool u = b.y > a.y;
                    if (x < bestX || (x == bestX && u && !up)) {
                        bestX = x;
                        edge = v;
                        up = u;
                    }
                    through = x == mx && lo.y != my;
                }
            } else if (a.y == my && b.y == my) {
                through = (a.x < mx && mx < b.x) || (b.x < mx && mx < a.x);
            }
            // m inside this edge with the hole on its inner side
            if (through && cross(a, b, hp) >= 0 && cross(a, b, hn) >= 0)
                onEdge = v;
        }

        if (onEdge != None) {
            // m lies on the edge: insert the hole into it.
            const uint32_t m2 = used++, mp = n[m].prev, edgeEnd = n[onEdge].next;
            n[m2] = n[m];
            n[onEdge].next = m;
            n[m].prev = onEdge;
            n[mp].next = m2;
            n[m2].prev = mp;
            n[m2].next = edgeEnd;
            n[edgeEnd].prev = m2;
            _samePos[m2] = _samePos[m];
            _samePos[m] = m2;
            return true;
        }
        if (onRay != None && rayX < bestX) {
            // The ray touches a vertex before any crossing: nothing lies between it and m.
            const uint32_t t = pickCopy(onRay, mx, my);
            if (t != None) {
                splice(t, m, used);
                return true;
            }
        }
        if (edge == None)
            return false;
        const uint32_t edgeEnd = n[edge].next;
        const Node &ea = n[edge], &eb = n[edgeEnd];
        uint32_t hit = ea.y == my && ea.x == bestX ? edge : eb.y == my && eb.x == bestX ? edgeEnd : None;
        if (hit != None) {
            // The ray hits a vertex: visible when m lies in the sector of one of its copies.
            const uint32_t t = pickCopy(hit, mx, my);
            if (t == None)
                return false;
            splice(t, m, used);
            return true;
        }
        if (!up)
            return false;
        // Visible candidate: the edge end with larger x unless a blocker in triangle m, hit point, p is closer in angle.
        const uint32_t pi = ea.x >= eb.x ? edge : edgeEnd;
        const Node &p = n[pi];
        const double ix = bestX, px = p.x, py = p.y;
        const bool above = py > my;
        const double y0 = above ? my : py, y1 = above ? py : my;
        uint32_t best = None;
        double bdx = 0, bdy = 0;
        for (uint32_t v = 0; v < used; ++v) {
            const Node &r = n[v];
            const bool box = (r.x >= mx) & (r.x <= px) & (r.y >= y0) & (r.y <= y1);
            if (box && !(r.x == px && r.y == py) && merged[r.ring]) {
                const bool inside = above
                                        ? cross(mx, my, ix, my, r.x, r.y) >= 0 && cross(ix, my, px, py, r.x, r.y) >= 0 && cross(px, py, mx, my, r.x, r.y) >= 0
                                        : cross(mx, my, px, py, r.x, r.y) >= 0 && cross(px, py, ix, my, r.x, r.y) >= 0 && cross(ix, my, mx, my, r.x, r.y) >= 0;
                if (inside && cross(n[r.prev], r, n[r.next]) <= 0) {
                    const double dx = r.x - mx, dy = std::abs(r.y - my);
                    const double lhs = dy * bdx, rhs = bdy * dx;
                    if (best == None || lhs < rhs || (lhs == rhs && dx < bdx)) {
                        best = v;
                        bdx = dx;
                        bdy = dy;
                    }
                }
            }
        }
        const uint32_t target = best != None ? best : pi;
        const uint32_t t = pickCopy(target, mx, my);
        splice(t != None ? t : target, m, used);
        return true;
    }

    // Bridge between boundary node t and hole node m: t -> m ... hole ... m' -> t'.
    void splice(uint32_t t, uint32_t m, uint32_t &used)
    {
        Node *n = _nodes.data();
        const uint32_t t2 = used++, m2 = used++;
        const uint32_t tn = n[t].next, mp = n[m].prev;
        n[t2] = n[t];
        n[m2] = n[m];
        n[t].next = m;
        n[m].prev = t;
        n[mp].next = m2;
        n[m2].prev = mp;
        n[m2].next = t2;
        n[t2].prev = m2;
        n[t2].next = tn;
        n[tn].prev = t2;
        _samePos[t2] = _samePos[t];
        _samePos[t] = t2;
        _samePos[m2] = _samePos[m];
        _samePos[m] = m2;
    }

    // Sorts the blockers into buckets along the longer axis of their bounds, so a query is one contiguous range;
    // few blockers stay in one bucket.
    void buildIndex()
    {
        const Node *n = _nodes.data();
        const uint32_t count = _blockerCount;
        const uint32_t *blockers = _blockers.data();
        Item *items = _items.data();
        uint32_t *slot = _slot.data();
        _live = _built = count;
        _buckets = 1;
        // a short list stays as the classification wrote it
        if (count <= SmallBlockers)
            return;
        double minX = n[blockers[0]].x, minY = n[blockers[0]].y, maxX = minX, maxY = minY;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t b = blockers[i];
            minX = std::min(minX, n[b].x);
            maxX = std::max(maxX, n[b].x);
            minY = std::min(minY, n[b].y);
            maxY = std::max(maxY, n[b].y);
        }
        _axisY = maxY - minY > maxX - minX;
        const double extent = _axisY ? maxY - minY : maxX - minX;
        const uint32_t buckets = (uint32_t)std::max(1.0, count / BlockersPerBucket);
        _buckets = buckets;
        _bucketOrigin = _axisY ? minY : minX;
        _bucketScale = extent > 0 ? buckets / extent : 0;
        _bucketStart.assign(buckets + 1, 0);
        uint32_t *start = _bucketStart.data();
        for (uint32_t i = 0; i < count; ++i)
            start[bucketOf(n[blockers[i]]) + 1]++;
        for (uint32_t c = 0; c < buckets; ++c)
            start[c + 1] += start[c];
        // fill from each bucket's start, then shift the starts back
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t b = blockers[k];
            const uint32_t i = start[bucketOf(n[b])]++;
            items[i] = {n[b].x, n[b].y, b};
            slot[b] = i;
        }
        for (uint32_t c = buckets; c > 0; --c)
            start[c] = start[c - 1];
        start[0] = 0;
    }

    uint32_t bucket(double u) const { return (uint32_t)std::clamp((int64_t)((u - _bucketOrigin) * _bucketScale), (int64_t)0, (int64_t)_buckets - 1); }
    uint32_t bucketOf(const Node &v) const { return bucket(_axisY ? v.y : v.x); }

    // True when no blocker in items [begin, end) lies in the closed triangle a, b, c (counter-clockwise) away from its corners;
    // the neighbors ia and ic are corners themselves.
    EARSTRIP_INLINE static bool
    emptyTriangle(const Item *items, uint32_t begin, uint32_t end, const Node &a, const Node &b, const Node &c, uint32_t ia, uint32_t ic)
    {
        const double abx = b.x - a.x, aby = b.y - a.y, bcx = c.x - b.x, bcy = c.y - b.y, cax = a.x - c.x, cay = a.y - c.y;
        // branch-free pass counting blockers in the closed triangle; the rare hits are checked against the corners after
        uint32_t hits = 0;
        for (uint32_t i = begin; i < end; ++i) {
            const Item &p = items[i];
            const double side =
                std::min(std::min(abx * (p.y - a.y) - aby * (p.x - a.x), bcx * (p.y - b.y) - bcy * (p.x - b.x)), cax * (p.y - c.y) - cay * (p.x - c.x));
            hits += (uint32_t)(side >= 0) & (uint32_t)(p.node != ia) & (uint32_t)(p.node != ic);
        }
        if (hits == 0)
            return true;
        for (uint32_t i = begin; i < end; ++i) {
            const Item &p = items[i];
            if (p.node == ia || p.node == ic || cross(a.x, a.y, b.x, b.y, p.x, p.y) < 0 || cross(b.x, b.y, c.x, c.y, p.x, p.y) < 0 ||
                cross(c.x, c.y, a.x, a.y, p.x, p.y) < 0)
                continue;
            if ((p.x != a.x || p.y != a.y) && (p.x != b.x || p.y != b.y) && (p.x != c.x || p.y != c.y))
                return false;
        }
        return true;
    }

    // Bucket variant: a branch-free bounding box pass, then exact tests on the few hits.
    static bool emptyTriangleBox(const Item *items, uint32_t begin, uint32_t end, const Node &a, const Node &b, const Node &c, uint32_t ia, uint32_t ic)
    {
        const double minX = std::min(a.x, std::min(b.x, c.x)), maxX = std::max(a.x, std::max(b.x, c.x));
        const double minY = std::min(a.y, std::min(b.y, c.y)), maxY = std::max(a.y, std::max(b.y, c.y));
        for (uint32_t base = begin; base < end; base += 64) {
            const uint32_t stop = std::min(end, base + 64);
            uint64_t mask = 0;
            for (uint32_t i = base; i < stop; ++i) {
                const Item &p = items[i];
                const uint64_t in = (uint64_t)(p.x >= minX) & (uint64_t)(p.x <= maxX) & (uint64_t)(p.y >= minY) & (uint64_t)(p.y <= maxY) &
                                    (uint64_t)(p.node != ia) & (uint64_t)(p.node != ic);
                mask |= in << (i - base);
            }
            while (mask) {
                const Item &p = items[base + detail::lowestBit(mask)];
                mask &= mask - 1;
                if (cross(a.x, a.y, b.x, b.y, p.x, p.y) < 0 || cross(b.x, b.y, c.x, c.y, p.x, p.y) < 0 || cross(c.x, c.y, a.x, a.y, p.x, p.y) < 0)
                    continue;
                if ((p.x != a.x || p.y != a.y) && (p.x != b.x || p.y != b.y) && (p.x != c.x || p.y != c.y))
                    return false;
            }
        }
        return true;
    }

    EARSTRIP_INLINE bool isEar(uint32_t v) const
    {
        const Node *n = _nodes.data();
        const Node &b = n[v];
        if (b.reflex)
            return false;
        if (_live == 0)
            return true;
        const uint32_t ia = b.prev, ic = b.next;
        const Node &a = n[ia], &c = n[ic];
        const Item *items = _items.data();
        if (_buckets == 1)
            return emptyTriangle(items, 0, _live, a, b, c, ia, ic);
        const uint32_t *start = _bucketStart.data();
        const double lo = _axisY ? std::min(a.y, std::min(b.y, c.y)) : std::min(a.x, std::min(b.x, c.x));
        const double hi = _axisY ? std::max(a.y, std::max(b.y, c.y)) : std::max(a.x, std::max(b.x, c.x));
        return emptyTriangleBox(items, start[bucket(lo)], start[bucket(hi) + 1], a, b, c, ia, ic);
    }

    // Updates the corner flag of node v, dropping it from the blockers once it turns convex.
    EARSTRIP_INLINE void refreshCorner(Node &v, const Node &prev, const Node &next, uint32_t id)
    {
        const bool reflex = cross(prev, v, next) <= 0;
        if (v.indexed && !reflex)
            dropBlocker(v, id);
        v.reflex = reflex;
    }

    // Removes node id from the blocker index: swapped out of a single bucket, marked gone in several.
    void dropBlocker(Node &v, uint32_t id)
    {
        v.indexed = 0;
        Item *items = _items.data();
        const uint32_t i = _slot[id];
        _live--;
        if (_buckets == 1) {
            items[i] = items[_live];
            _slot[items[i].node] = i;
        } else {
            items[i].x = std::numeric_limits<double>::infinity();
            if (_live * 2 < _built)
                rebuildIndex();
        }
    }

    // Rebuilds the buckets from the live items once half of them are gone.
    void rebuildIndex()
    {
        uint32_t *blockers = _blockers.data();
        uint32_t found = 0;
        Item *items = _items.data();
        uint32_t *slot = _slot.data();
        for (uint32_t i = 0; i < _built; ++i) {
            if (items[i].x != std::numeric_limits<double>::infinity()) {
                blockers[found] = items[i].node;
                items[found] = items[i];
                slot[items[i].node] = found;
                found++;
            }
        }
        _blockerCount = found;
        buildIndex();
    }

    // Unlinks node v and refreshes the corners of its neighbors.
    EARSTRIP_INLINE void unlink(uint32_t v)
    {
        Node *n = _nodes.data();
        const uint32_t a = n[v].prev, c = n[v].next;
        Node &na = n[a], &nc = n[c];
        na.next = c;
        nc.prev = a;
        refreshCorner(na, n[na.prev], nc, a);
        refreshCorner(nc, na, n[nc.next], c);
    }

    // Drops repeated points and zero-area corners from the boundary through v; returns the vertex count.
    uint32_t cleanup(uint32_t &v, uint32_t count)
    {
        Node *n = _nodes.data();
        uint32_t since = 0;
        while (count >= 3 && since < count) {
            const Node &node = n[v];
            if (cross(n[node.prev], node, n[node.next]) == 0) {
                const uint32_t prev = node.prev;
                unlink(v);
                count--;
                v = prev;
                since = 0;
            } else {
                v = node.next;
                since++;
            }
        }
        if (count >= 3) {
            classifyCycle(v);
            buildIndex();
        }
        return count;
    }

    // Blockers in the closed triangle a, b, c away from its corners, counted over the whole index.
    uint32_t blockersInside(const Node &a, const Node &b, const Node &c, uint32_t ia, uint32_t ic) const
    {
        const Item *items = _items.data();
        const uint32_t end = _buckets == 1 ? _live : _built;
        uint32_t hits = 0;
        for (uint32_t i = 0; i < end; ++i) {
            const Item &p = items[i];
            if (p.node == ia || p.node == ic || cross(a.x, a.y, b.x, b.y, p.x, p.y) < 0 || cross(b.x, b.y, c.x, c.y, p.x, p.y) < 0 ||
                cross(c.x, c.y, a.x, a.y, p.x, p.y) < 0)
                continue;
            hits += (p.x != a.x || p.y != a.y) && (p.x != b.x || p.y != b.y) && (p.x != c.x || p.y != c.y);
        }
        return hits;
    }

    // Finds two boundary edges through v that cross properly, a -> b and c -> d in walking order. Quadratic in the
    // boundary, which is why only small boundaries are split.
    bool findCrossing(uint32_t v, uint32_t count, uint32_t &ia, uint32_t &ib, uint32_t &ic, uint32_t &id)
    {
        if (count > SplitLimit)
            return false;
        const Node *n = _nodes.data();
        _cycle.clear();
        uint32_t w = v;
        for (uint32_t i = 0; i < count; ++i, w = n[w].next)
            _cycle.push_back(w);
        for (uint32_t i = 0; i + 2 < count; ++i) {
            const Node &a = n[_cycle[i]], &b = n[_cycle[i + 1]];
            const double minX = std::min(a.x, b.x), maxX = std::max(a.x, b.x), minY = std::min(a.y, b.y), maxY = std::max(a.y, b.y);
            for (uint32_t j = i + 2; j < count; ++j) {
                if (i == 0 && j + 1 == count)
                    continue; // the edge into a
                const Node &c = n[_cycle[j]], &d = n[_cycle[j + 1 == count ? 0 : j + 1]];
                if (std::max(c.x, d.x) < minX || std::min(c.x, d.x) > maxX || std::max(c.y, d.y) < minY || std::min(c.y, d.y) > maxY)
                    continue;
                if (cross(a, b, c) * cross(a, b, d) < 0 && cross(c, d, a) * cross(c, d, b) < 0) {
                    ia = _cycle[i];
                    ib = _cycle[i + 1];
                    ic = _cycle[j];
                    id = _cycle[j + 1 == count ? 0 : j + 1];
                    return true;
                }
            }
        }
        return false;
    }

    uint32_t cycleLength(uint32_t v) const
    {
        const Node *n = _nodes.data();
        uint32_t count = 0, w = v;
        do {
            count++;
            w = n[w].next;
        } while (w != v);
        return count;
    }

    // Turns the cycle through v counter-clockwise; false when it has no area.
    bool orient(uint32_t v)
    {
        Node *n = _nodes.data();
        double area = 0;
        uint32_t w = v;
        do {
            const Node &p = n[w], &q = n[p.next];
            area += p.x * q.y - q.x * p.y;
            w = p.next;
        } while (w != v);
        if (area < 0) {
            w = v;
            do {
                Node &p = n[w];
                std::swap(p.prev, p.next);
                w = p.prev;
            } while (w != v);
        }
        return area != 0;
    }

    // Even-odd test of point p against the rings as loaded, before any bridge or cut.
    bool insideRings(double px, double py) const
    {
        const Node *n = _nodes.data();
        bool inside = false;
        auto ring = [&](uint32_t first, uint32_t count) {
            for (uint32_t i = first, j = first + count - 1; i < first + count; j = i++) {
                const Node &a = n[j], &b = n[i];
                if ((a.y > py) != (b.y > py) && px < (b.x - a.x) * (py - a.y) / (b.y - a.y) + a.x)
                    inside = !inside;
            }
        };
        ring(0, _outerCount);
        for (const Hole &hole : _holes)
            ring(hole.first, hole.count);
        return inside;
    }

    // A point just inside the counter-clockwise loop through v, left of the middle of its longest edge.
    void pointInside(uint32_t v, double &px, double &py) const
    {
        const Node *n = _nodes.data();
        uint32_t best = v, w = v;
        double bestLen = -1;
        do {
            const Node &p = n[w], &q = n[p.next];
            const double len = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y);
            if (len > bestLen) {
                bestLen = len;
                best = w;
            }
            w = p.next;
        } while (w != v);
        const Node &p = n[best], &q = n[p.next];
        const double e = 1e-4;
        px = (p.x + q.x) / 2 - (q.y - p.y) * e;
        py = (p.y + q.y) / 2 + (q.x - p.x) * e;
    }

    // Covers the lobe a loop of two vertices u -> w stands for: the lobe runs from u to w through the crossing x, which
    // lies on the edge s0 -> s1 at ts and on t0 -> t1 at tt; of the two triangles of real vertices around it, the one with
    // the smaller extra wedge is cut.
    void lobe(uint32_t u, uint32_t w, uint32_t extraA, double wedgeA, uint32_t extraB, double wedgeB)
    {
        const Node *n = _nodes.data();
        const uint32_t third = wedgeA <= wedgeB ? extraA : extraB;
        const bool ccw = cross(n[u], n[w], n[third]) > 0;
        beginStrip(false);
        put(n[u].index);
        put(n[ccw ? w : third].index);
        put(n[ccw ? third : w].index);
        endStrip(false);
    }

    // Second run of a polygon the ears could not finish: the boundary is cut at every crossing into loops, a -> b
    // crossing c -> d giving a -> d ... and b ... c -> b. Each loop the rings fill (even-odd, so both lobes of a figure
    // eight and not a hole) is turned counter-clockwise and clipped on its own; a loop of two vertices becomes the
    // triangle that covers its lobe best.
    bool clipLoops(uint32_t v, uint32_t count)
    {
        Node *n = _nodes.data();
        _loops.clear();
        _loops.push_back(v);
        _loops.push_back(count);
        while (!_loops.empty()) {
            const uint32_t c = _loops.back(), s = _loops[_loops.size() - 2];
            _loops.resize(_loops.size() - 2);
            if (c < 3)
                continue;
            uint32_t ia, ib, ic, id;
            if (findCrossing(s, c, ia, ib, ic, id)) {
                // the crossing lies on a -> b at t and on c -> d at u
                const double ca = cross(n[ic], n[id], n[ia]), cb = cross(n[ic], n[id], n[ib]);
                const double cc = cross(n[ia], n[ib], n[ic]), cd = cross(n[ia], n[ib], n[id]);
                const double t = ca / (ca - cb), u = cc / (cc - cd);
                const double xx = n[ia].x + t * (n[ib].x - n[ia].x), xy = n[ia].y + t * (n[ib].y - n[ia].y);
                n[ia].next = id;
                n[id].prev = ia;
                n[ic].next = ib;
                n[ib].prev = ic;
                _forced = true;
                const uint32_t la = cycleLength(ia), lb = cycleLength(ib);
                if (la == 2 && insideLobe(ia, id, xx, xy))
                    lobe(id, ia, ic, u, ib, 1 - t);
                if (lb == 2 && insideLobe(ib, ic, xx, xy))
                    lobe(ib, ic, ia, t, id, 1 - u);
                _loops.push_back(ia);
                _loops.push_back(la);
                _loops.push_back(ib);
                _loops.push_back(lb);
                continue;
            }
            if (!orient(s))
                continue;
            double px, py;
            pointInside(s, px, py);
            if (!insideRings(px, py))
                continue;
            clip(s, classifyCycle(s));
        }
        return true;
    }

    // True when the lobe u, w and the crossing x is filled by the rings, tested at its centroid.
    bool insideLobe(uint32_t u, uint32_t w, double xx, double xy) const
    {
        const Node *n = _nodes.data();
        return insideRings((n[u].x + n[w].x + xx) / 3, (n[u].y + n[w].y + xy) / 3);
    }

    // No ear is left: cuts the convex corner with the fewest blockers in its triangle (the smallest one on a tie),
    // or the corner at v when none is convex, and continues after it.
    void forcedCut(uint32_t &v, uint32_t count)
    {
        Node *n = _nodes.data();
        uint32_t best = v, bestHits = None, w = v;
        double bestArea = 0;
        for (uint32_t i = 0; i < count; ++i, w = n[w].next) {
            const Node &b = n[w], &a = n[b.prev], &c = n[b.next];
            const double area = cross(a, b, c);
            if (area <= 0)
                continue;
            const uint32_t hits = blockersInside(a, b, c, b.prev, b.next);
            if (hits < bestHits || (hits == bestHits && area < bestArea)) {
                best = w;
                bestHits = hits;
                bestArea = area;
            }
        }
        const uint32_t a = n[best].prev, b = n[best].next;
        unlink(best);
        beginStrip(false);
        put(n[best].index);
        put(n[b].index);
        put(n[a].index);
        endStrip(false);
        _forced = true;
        v = b;
    }

    // Cuts ears from the boundary through v into zig-zag strips; false when no ear is left even after the cleanup and
    // forced cuts are off.
    bool clip(uint32_t v, uint32_t count)
    {
        if (count < 3)
            return true;
        buildIndex();
        Node *n = _nodes.data();
        uint32_t stall = 0;
        bool cleaned = false;
        while (count >= 3) {
            if (!isEar(v)) {
                v = n[v].next;
                if (++stall < count)
                    continue;
                if (cleaned) {
                    if (!_careful)
                        return false;
                    forcedCut(v, count);
                    count--;
                    stall = 0;
                    cleaned = false;
                    continue;
                }
                count = cleanup(v, count);
                cleaned = true;
                stall = 0;
                continue;
            }
            stall = 0;
            cleaned = false;
            uint32_t a = n[v].prev, b = n[v].next;
            unlink(v);
            count--;
            beginStrip(false);
            put(n[v].index);
            bool sideA, firstA;
            if (count >= 3 && isEar(a)) {
                _cw = true;
                put(n[a].index);
                put(n[b].index);
                sideA = firstA = true;
            } else if (count >= 3 && isEar(b)) {
                put(n[b].index);
                put(n[a].index);
                sideA = firstA = false;
            } else {
                put(n[b].index);
                put(n[a].index);
                endStrip(false);
                v = n[b].next;
                continue;
            }
            // Zig-zag: the next ear alternates between the two ends of the last cut.
            while (true) {
                if (sideA) {
                    const uint32_t z = n[a].prev;
                    put(n[z].index);
                    unlink(a);
                    a = z;
                } else {
                    const uint32_t z = n[b].next;
                    put(n[z].index);
                    unlink(b);
                    b = z;
                }
                count--;
                sideA = !sideA;
                if (count < 3 || !isEar(sideA ? a : b))
                    break;
            }
            // v, a, b runs clockwise when the strip went on at a first
            endStrip(firstA);
            v = sideA ? b : a;
        }
        return true;
    }
};

// Triangulates a polygon (a container of rings of points, outer ring first) into a triangle list.
template <class Index = uint32_t, class Polygon> std::vector<Index> triangulate(const Polygon &polygon)
{
    Triangulator<Index> t;
    t.run(polygon);
    std::vector<Index> out;
    t.triangles(out);
    return out;
}

} // namespace earstrip
