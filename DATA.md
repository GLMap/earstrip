# Benchmark data

The four files of polygons behind the numbers in the [README](README.md). They are in the
[bench-data-1](https://github.com/GLMap/earstrip/releases/tag/bench-data-1) release, 147 MB packed; `make data`
downloads them into `data/` and checks them against [data/SHA256SUMS](data/SHA256SUMS). A release's files never
change: other data would come as bench-data-2.

| File | Polygons | Rings | Points | Packed | What |
| --- | ---: | ---: | ---: | ---: | --- |
| `z13.bin` | 3,740,425 | 3,763,815 | 28,943,266 | 92 MB | every polygon of zoom level 13 of GLMap's offline map of the Masovian Voivodeship, Poland, with the expected results |
| `z9.bin` | 683,742 | 714,401 | 3,370,197 | 11 MB | the same map at zoom level 9, with the expected results |
| `world.bin` | 56,472 | 199,272 | 3,431,387 | 8 MB | the polygons of GLMap's world map tiles, up to 11,675 holes in one |
| `gom4old.bin` | 1,212,030 | 1,262,364 | 15,173,525 | 35 MB | an older build of the Masovian map, from before its rings were repaired on the tile grid: rings that cross or touch |

## Format

Little-endian, in tile coordinates:

```
u32 magic "PLY1", u32 polygons, u32 rings, u32 points
u32 polyRingStart[polygons + 1]   first ring of each polygon; the first ring is the outer one
u32 ringPointStart[rings + 1]     first point of each ring
i16 xy[2 * points]
```

`z13.bin` and `z9.bin` go on with two arrays of one `u32` per polygon. The first is the FNV-1a hash of the strips
the ears write with the constrained triangulation and forced cuts off (`hashOf` in `bench/bench.cpp`; `bench
--check` and `js/test/test.mjs <file>` compare against it). The second is the FNV-1a hash of earcut.hpp 3.2.4's
triangle indices, which `bench/wasm/bench.mjs` checks its WebAssembly build of earcut.hpp against.

## License

Map data © OpenStreetMap contributors. These files are extracts of OpenStreetMap data, including the water
polygons derived from its coastlines, and are available under the
[Open Database License 1.0](https://opendatacommons.org/licenses/odbl/1-0/). The ISC license of the code does not
apply to them.
