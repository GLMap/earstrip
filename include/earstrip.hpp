// earstrip: ear clipping triangulation of polygons with holes into triangle strips.
// https://github.com/GLMap/earstrip
//
// Copyright (c) 2026 Evgen Bodunov
// SPDX-License-Identifier: ISC

#pragma once

#include <algorithm>
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
#else
#    define EARSTRIP_INLINE inline __attribute__((always_inline))
#endif

namespace earstrip {

// How a polygon was covered.
enum class Status {
    Exact,        // the triangles cover the polygon exactly
    HolesLeftOut, // a hole could not be joined to the boundary (it lies outside or crosses it) and is left out
    Approximate,  // the boundary ran out of ears (the polygon is not simple); the rest was cut by forced cuts
    Refused,      // as Approximate, with forced cuts switched off: the strips cover only what the ears did
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
    void setForcedCuts(bool on) { _forcedCuts = on; }

private:
    static constexpr uint32_t None = UINT32_MAX;
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

    static double cross(double ax, double ay, double bx, double by, double cx, double cy) { return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax); }
    static double cross(const Node &a, const Node &b, const Node &c) { return cross(a.x, a.y, b.x, b.y, c.x, c.y); }
    static bool samePos(const Node &a, const Node &b) { return a.x == b.x && a.y == b.y; }

    // The ears are only right on a simple boundary. When they run out, the polygon is cut again from the start with
    // its crossings untangled first, so that no ear cut before the stall spans the wrong area.
    template <class Rings> Status runRings(const Rings &rings)
    {
        _careful = false;
        const Status s = runOnce(rings);
        if (s != Status::Refused || !_forcedCuts)
            return s;
        _careful = true;
        runOnce(rings);
        return Status::Approximate;
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
