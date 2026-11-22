# Numbers

OpenStreetMap polygons in tile coordinates: every polygon of zoom levels 13 and 9 of the Masovian Voivodeship, and
the polygons of GLMap's world map tiles, some with up to 11,675 holes. One thread of an Apple M4 Pro, best of 7
runs, Node 26.12 and Bun 1.3.10. The polygons and the commands for every row are under
[Checking the numbers](README.md#checking-the-numbers) in the README.

| | earstrip | earcut |
| --- | ---: | ---: |
| zoom 13, 3,740,425 polygons, 28.9 million vertices: C++ (earcut.hpp 3.2.4) | 431 ms | 565 ms |
| the same in WebAssembly, loop inside the module, V8 / JavaScriptCore (earcut.hpp) | 523 / 490 ms | 923 / 837 ms |
| the same in WebAssembly, one call from JavaScript per polygon, V8 / JavaScriptCore | 610 / 612 ms | 996 / 946 ms |
| the same through the JavaScript package, one call per polygon (earcut.js 3.2.4) | 691 / 737 ms | 1,107 / 1,292 ms |
| the same through the package, all polygons in one call | 599 / 585 ms | |
| the same through the package's direct calls, one call per polygon, strips | 605 / 585 ms | |
| the same through the direct calls, all polygons in one call, restart index buffer | 575 / 551 ms | |
| zoom 9, 683,742 polygons: C++ | 59 ms | 83 ms |
| zoom 9 through the package, one call per polygon, V8 / JavaScriptCore | 98 / 103 ms | 159 / 159 ms |
| world map tiles, 56,472 polygons, 3.4 million vertices: C++ | 397 ms | 2,815 ms |
| world map tiles through the package, one call per polygon, V8 / JavaScriptCore | 560 / 549 ms | 4,873 / 4,042 ms |
| indices per triangle | 1.66 as strips (1.95 at zoom 9), 3 as a list | 3 |

The two WebAssembly rows are the case of an engine that keeps its geometry in WebAssembly memory: 16-bit tile
coordinates, strips left in the module, the JavaScript call copying only the input. The package's earcut-compatible
call takes coordinates as JavaScript numbers and returns a new array of triangle indices on every call; its direct
calls write the coordinates into the module and leave the result there, which is most of the way to the engine.

On the world map tiles the ears alone take 690 ms: past 128 rings the constrained triangulation is faster than
bridging every hole.

On the polygons the ears cannot cut exactly, the share of pixels where the triangles differ from an even-odd fill of
the rings on a 128 × 128 grid, on average and at worst:

| | earstrip | earcut.hpp |
| --- | ---: | ---: |
| zoom 13, 82 polygons | 0.15%, 2.6% | 31%, 483% |
| zoom 9, 183 polygons | 0.7%, 24% | 12%, 236% |
| 1,468 polygons of a map with unrepaired rings | 0.11%, 11% | 4.7%, 848% |

Part of what remains is where non-zero winding fills more than even-odd does, a ring walked twice for example. The
WebAssembly module is 77 KB, 28 KB with gzip; the one without SIMD for older engines 74 KB, 28 KB.
