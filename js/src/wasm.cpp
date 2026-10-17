// The WebAssembly side of the JavaScript package: one triangulator, input and output buffers in linear memory.
#include "earstrip.hpp"
#include <vector>

#define EXPORT(name) extern "C" __attribute__((export_name(#name)))

static earstrip::Triangulator<uint32_t> triangulator;
static std::vector<double> coords;
// Leaves the elements a resize adds uninitialized: the output is written right after it grows.
template <class T> struct Uninitialized : std::allocator<T> {
    template <class U> struct rebind { using other = Uninitialized<U>; };
    Uninitialized() = default;
    template <class U> Uninitialized(const Uninitialized<U> &) {}
    template <class U> void construct(U *) {}
    template <class U, class... A> void construct(U *p, A &&...a) { ::new ((void *)p) U(std::forward<A>(a)...); }
};

static std::vector<uint32_t> holes, starts, holeStarts, offsets;
static std::vector<uint32_t, Uninitialized<uint32_t>> output;
static earstrip::Status status;

// Where the fast path finds its buffers: coordinates, their capacity, hole starts, their capacity, output, its size;
// for runDirect() also the strip ends, their count, the strips' windings, the added points and their count.
// JavaScript reads it after every call that may move them, so that a polygon costs a single call.
static uint32_t layout[11];

static void publish()
{
    layout[0] = (uint32_t)(uintptr_t)coords.data();
    layout[1] = (uint32_t)coords.capacity();
    layout[2] = (uint32_t)(uintptr_t)holes.data();
    layout[3] = (uint32_t)holes.capacity();
    layout[4] = (uint32_t)(uintptr_t)output.data();
    layout[5] = (uint32_t)output.size();
}

EXPORT(layoutBuffer) const uint32_t *layoutBuffer()
{
    publish();
    return layout;
}

// Makes room for count coordinates and holeCount hole starts, with slack so that growing is rare.
EXPORT(grow) void grow(uint32_t count, uint32_t holeCount)
{
    if (coords.capacity() < count)
        coords.reserve(count + count / 2 + 64);
    if (holes.capacity() < holeCount)
        holes.reserve(holeCount + holeCount / 2 + 16);
    coords.resize(coords.capacity());
    holes.resize(holes.capacity());
    publish();
}

// The fast path: triangulates what JavaScript wrote into the buffers and publishes where the triangle list is.
EXPORT(run) void run(uint32_t vertices, uint32_t dim, uint32_t holeCount)
{
    triangulator.setTriangleList(true);
    triangulator.setExtraPoints(false);
    status = triangulator.run(coords.data(), vertices, dim, holes.data(), holeCount);
    const auto &list = triangulator.triangleList();
    layout[4] = (uint32_t)(uintptr_t)list.data();
    layout[5] = (uint32_t)list.size();
}

// Room for count coordinates and holeCount hole starts; returns the coordinates, holeBuffer() the hole starts.
EXPORT(reserve) double *reserve(uint32_t count, uint32_t holeCount)
{
    coords.resize(count);
    holes.resize(holeCount);
    publish();
    return coords.data();
}
EXPORT(holeBuffer) uint32_t *holeBuffer() { return holes.data(); }

EXPORT(setForcedCuts) void setForcedCuts(uint32_t on) { triangulator.setForcedCuts(on != 0); }
EXPORT(setConstrained) void setConstrained(uint32_t on) { triangulator.setConstrained(on != 0); }

// Frees the triangulator's and the call buffers; WebAssembly memory itself never shrinks, but the allocator reuses it.
EXPORT(shrink) void shrink()
{
    triangulator.shrink();
    for (auto *v : {&holes, &starts, &holeStarts, &offsets})
        std::vector<uint32_t>().swap(*v);
    decltype(output)().swap(output);
    std::vector<double>().swap(coords);
    publish();
}
EXPORT(lastStatus) uint32_t lastStatus() { return (uint32_t)status; }

// Triangle list of the polygon in the buffers; returns the index count, output() the indices.
EXPORT(triangulate) uint32_t triangulate(uint32_t vertices, uint32_t dim, uint32_t holeCount)
{
    triangulator.setTriangleList(true);
    triangulator.setExtraPoints(false);
    status = triangulator.run(coords.data(), vertices, dim, holes.data(), holeCount);
    const auto &list = triangulator.triangleList();
    output.assign(list.begin(), list.end());
    return (uint32_t)output.size();
}
EXPORT(output) const uint32_t *outputBuffer() { return output.data(); }

// Strips of the polygon in the buffers; returns the strip count; stripIndices(), stripEnds() and stripClockwise() follow.
// The strips may index points added where rings cross: extraPoints() holds their x, y pairs.
EXPORT(strip) uint32_t strip(uint32_t vertices, uint32_t dim, uint32_t holeCount)
{
    triangulator.setTriangleList(false);
    triangulator.setExtraPoints(true);
    status = triangulator.run(coords.data(), vertices, dim, holes.data(), holeCount);
    return (uint32_t)triangulator.stripEnds().size();
}
EXPORT(stripIndexCount) uint32_t stripIndexCount() { return (uint32_t)triangulator.strips().size(); }
EXPORT(stripIndices) const uint32_t *stripIndices() { return triangulator.strips().data(); }
EXPORT(stripEnds) const uint32_t *stripEnds() { return triangulator.stripEnds().data(); }
EXPORT(stripClockwise) const uint8_t *stripClockwise() { return triangulator.stripClockwise().data(); }
EXPORT(extraPointCount) uint32_t extraPointCount() { return (uint32_t)triangulator.extraPoints().size() / 2; }

// Appends the strips as one index buffer for TRIANGLE_STRIP with primitive restart: 0xFFFFFFFF between strips, and
// a strip whose first triangle runs clockwise starts with its first index twice, so every triangle is counter-clockwise.
static void appendRestart(uint32_t first)
{
    const auto &strips = triangulator.strips();
    const auto &ends = triangulator.stripEnds();
    const auto &cw = triangulator.stripClockwise();
    // at most a restart and a doubled first index per strip
    size_t at = output.size();
    output.resize(at + strips.size() + 2 * ends.size());
    uint32_t *out = output.data();
    const uint32_t *in = strips.data();
    uint32_t begin = 0;
    for (size_t k = 0; k < ends.size(); ++k) {
        if (at)
            out[at++] = UINT32_MAX;
        if (cw[k])
            out[at++] = first + in[begin];
        for (uint32_t i = begin; i < ends[k]; ++i)
            out[at++] = first + in[i];
        begin = ends[k];
    }
    output.resize(at);
}

// The polygon in the buffers as one restart index buffer; returns its length, output() the indices. Added points
// are allowed: extraPoints() holds them.
EXPORT(restart) uint32_t restart(uint32_t vertices, uint32_t dim, uint32_t holeCount)
{
    triangulator.setTriangleList(false);
    triangulator.setExtraPoints(true);
    status = triangulator.run(coords.data(), vertices, dim, holes.data(), holeCount);
    output.clear();
    appendRestart(0);
    return (uint32_t)output.size();
}
EXPORT(extraPoints) const double *extraPoints() { return triangulator.extraPoints().data(); }

// One polygon of the direct calls in a single call: mode 0 a triangle list (input vertices only), 1 strips, 2 one
// restart index buffer (both may add points). Publishes the results in the layout.
EXPORT(runDirect) void runDirect(uint32_t vertices, uint32_t dim, uint32_t holeCount, uint32_t mode)
{
    triangulator.setTriangleList(mode == 0);
    triangulator.setExtraPoints(mode != 0);
    status = triangulator.run(coords.data(), vertices, dim, holes.data(), holeCount);
    const uint32_t *indices = triangulator.triangleList().data();
    size_t count = triangulator.triangleList().size();
    if (mode == 1) {
        indices = triangulator.strips().data();
        count = triangulator.strips().size();
    } else if (mode == 2) {
        output.clear();
        appendRestart(0);
        indices = output.data();
        count = output.size();
    }
    layout[4] = (uint32_t)(uintptr_t)indices;
    layout[5] = (uint32_t)count;
    layout[6] = (uint32_t)(uintptr_t)triangulator.stripEnds().data();
    layout[7] = (uint32_t)triangulator.stripEnds().size();
    layout[8] = (uint32_t)(uintptr_t)triangulator.stripClockwise().data();
    layout[9] = (uint32_t)(uintptr_t)triangulator.extraPoints().data();
    layout[10] = (uint32_t)triangulator.extraPoints().size() / 2;
}

// Room for a batch of the given sizes in one go: count coordinates, holeCount hole starts, polygons + 1 vertex starts
// and hole start offsets. JavaScript writes into them through the layout.
EXPORT(reserveAll) void reserveAll(uint32_t count, uint32_t holeCount, uint32_t polygons)
{
    coords.resize(count);
    holes.resize(holeCount);
    starts.resize(polygons + 1);
    holeStarts.resize(polygons + 1);
    publish();
}
EXPORT(batchStarts) uint32_t *batchStarts() { return starts.data(); }

// Room for a batch: polygons + 1 vertex starts and hole start offsets; the hole starts themselves go to holeBuffer().
EXPORT(reserveBatch) uint32_t *reserveBatch(uint32_t polygons)
{
    starts.resize(polygons + 1);
    holeStarts.resize(polygons + 1);
    return starts.data();
}
EXPORT(batchHoleStarts) uint32_t *batchHoleStarts() { return holeStarts.data(); }

// Triangle lists of many polygons at once: polygon k has the vertices [starts[k], starts[k + 1]) of the coordinates
// and the holes holeBuffer()[holeStarts[k] .. holeStarts[k + 1]), counted from its first vertex. Indices in the
// output count from the first vertex of the batch; offsets() gives where each polygon's indices begin.
// mode 1 writes one restart index buffer instead of triangles (see appendRestart).
EXPORT(triangulateBatch) uint32_t triangulateBatch(uint32_t polygons, uint32_t dim, uint32_t mode)
{
    output.clear();
    offsets.resize(polygons + 1);
    triangulator.setTriangleList(mode == 0);
    triangulator.setExtraPoints(false);
    for (uint32_t k = 0; k < polygons; ++k) {
        offsets[k] = (uint32_t)output.size();
        const uint32_t first = starts[k];
        triangulator.run(coords.data() + (size_t)first * dim, starts[k + 1] - first, dim, holes.data() + holeStarts[k], holeStarts[k + 1] - holeStarts[k]);
        if (mode == 1) {
            appendRestart(first);
            continue;
        }
        const auto &list = triangulator.triangleList();
        const size_t begin = output.size();
        output.resize(begin + list.size());
        for (size_t i = 0; i < list.size(); ++i)
            output[begin + i] = first + list[i];
    }
    offsets[polygons] = (uint32_t)output.size();
    return (uint32_t)output.size();
}
EXPORT(offsets) const uint32_t *offsetsBuffer() { return offsets.data(); }
