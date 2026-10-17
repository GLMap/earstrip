// The WebAssembly side of the JavaScript package: one triangulator, input and output buffers in linear memory.
#include "earstrip.hpp"
#include <vector>

#define EXPORT(name) extern "C" __attribute__((export_name(#name)))

static earstrip::Triangulator<uint32_t> triangulator;
static std::vector<double> coords;
static std::vector<uint32_t> holes, starts, holeStarts, output, offsets;
static earstrip::Status status;

// Where the fast path finds its buffers: coordinates, their capacity, hole starts, their capacity, output, its size.
// JavaScript reads it after every call that may move them, so that a polygon costs a single call.
static uint32_t layout[6];

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
EXPORT(lastStatus) uint32_t lastStatus() { return (uint32_t)status; }

// Triangle list of the polygon in the buffers; returns the index count, output() the indices.
EXPORT(triangulate) uint32_t triangulate(uint32_t vertices, uint32_t dim, uint32_t holeCount)
{
    triangulator.setTriangleList(true);
    triangulator.setExtraPoints(false);
    status = triangulator.run(coords.data(), vertices, dim, holes.data(), holeCount);
    output = triangulator.triangleList();
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
EXPORT(extraPoints) const double *extraPoints() { return triangulator.extraPoints().data(); }

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
EXPORT(triangulateBatch) uint32_t triangulateBatch(uint32_t polygons, uint32_t dim)
{
    output.clear();
    offsets.resize(polygons + 1);
    triangulator.setTriangleList(true);
    triangulator.setExtraPoints(false);
    for (uint32_t k = 0; k < polygons; ++k) {
        offsets[k] = (uint32_t)output.size();
        const uint32_t first = starts[k];
        triangulator.run(coords.data() + (size_t)first * dim, starts[k + 1] - first, dim, holes.data() + holeStarts[k], holeStarts[k + 1] - holeStarts[k]);
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
