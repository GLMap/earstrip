export type Coordinates = ArrayLike<number>;

/** Triangulates a polygon from flat coordinates (dim numbers per vertex) and the vertex index where each hole starts.
 * Returns vertex indices, three per triangle, each counter-clockwise. Same arguments as earcut. */
declare function earstrip(data: Coordinates, holeIndices?: ArrayLike<number> | null, dim?: number): number[];

declare namespace earstrip {
    /** The polygon as triangle strips: all strip indices, the end of each strip, and whether its first triangle is clockwise. */
    /** points: x, y pairs added where rings cross; index n + k, n being the input vertex count, is point k. */
    function strips(data: Coordinates, holeIndices?: ArrayLike<number> | null, dim?: number): { indices: Uint32Array; ends: Uint32Array; clockwise: Uint8Array; points: Float64Array };
    /** Many polygons in one call; indices count from the first vertex of data, polygon k owns indices[offsets[k], offsets[k + 1]). */
    function batch(data: Coordinates, polygonStarts: ArrayLike<number>, holeIndices?: ArrayLike<number> | null, holeStarts?: ArrayLike<number> | null, dim?: number): { indices: Uint32Array; offsets: Uint32Array };
    /** How the last polygon was covered. */
    function lastStatus(): 'exact' | 'holes-left-out' | 'approximate' | 'refused' | 'constrained';
    /** Cut polygons without ears anyway (default) or give up on them, when the constrained triangulation cannot take them. */
    function setForcedCuts(on: boolean): void;
    /** Send polygons the ears cannot finish to the constrained triangulation (default) or to forced cuts. */
    function setConstrained(on: boolean): void;
    /** Frees the module's buffers, which otherwise keep the size of the largest polygon seen. */
    function shrink(): void;
    /** GeoJSON-style rings as earstrip's arguments. */
    function flatten(rings: ArrayLike<ArrayLike<ArrayLike<number>>>): { vertices: number[]; holes: number[]; dimensions: number };
    /** The direct calls: no copies, results as views into the module's memory. */
    const direct: typeof import('./earstrip').direct;
    /** Resolves once the WebAssembly module is compiled; already resolved where it compiles synchronously. */
    const ready: Promise<void>;
}

/** The result of direct.run(): offsets into views of the whole module memory; the getters make views of the parts. */
interface DirectResult {
    u32: Uint32Array;
    u8: Uint8Array;
    f64: Float64Array;
    indexStart: number;
    indexCount: number;
    endsStart: number;
    stripCount: number;
    clockwiseStart: number;
    pointsStart: number;
    pointCount: number;
    readonly indices: Uint32Array;
    readonly ends: Uint32Array;
    readonly clockwise: Uint8Array;
    readonly points: Float64Array;
}

/** The direct calls: write into the module's memory, read results there; no copies, no allocation per polygon.
 * Views and the result stay valid until the next earstrip call. */
declare const direct: {
    /** Views spanning the input buffers; write coordinates (dim per vertex) and hole starts at their start. */
    input(vertexCount: number, holeCount?: number, dim?: number): { coords: Float64Array; holes: Uint32Array };
    /** Triangulates the input: 'restart' (default) one TRIANGLE_STRIP buffer with 0xFFFFFFFF restarts, 'strips', or
     * 'triangles' (input vertices only). */
    run(vertexCount: number, holeCount?: number, mode?: 'restart' | 'strips' | 'triangles', dim?: number): DirectResult;
    /** Views to write a batch into, as batch() takes it. */
    batchInput(polygons: number, vertexCount: number, holeCount?: number, dim?: number): { coords: Float64Array; holes: Uint32Array; polygonStarts: Uint32Array; holeStarts: Uint32Array };
    /** Every polygon of the batch input as a triangle list or one restart index buffer. */
    batch(polygons: number, dim?: number, output?: 'triangles' | 'restart'): { indices: Uint32Array; offsets: Uint32Array };
};

export { direct };
export default earstrip;
export declare const strips: typeof earstrip.strips;
export declare const batch: typeof earstrip.batch;
export declare const lastStatus: typeof earstrip.lastStatus;
export declare const setForcedCuts: typeof earstrip.setForcedCuts;
export declare const setConstrained: typeof earstrip.setConstrained;
export declare const shrink: typeof earstrip.shrink;
export declare const flatten: typeof earstrip.flatten;
export declare const ready: Promise<void>;
