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
    /** GeoJSON-style rings as earstrip's arguments. */
    function flatten(rings: ArrayLike<ArrayLike<ArrayLike<number>>>): { vertices: number[]; holes: number[]; dimensions: number };
    /** Resolves once the WebAssembly module is compiled; already resolved where it compiles synchronously. */
    const ready: Promise<void>;
}

export default earstrip;
export declare const strips: typeof earstrip.strips;
export declare const batch: typeof earstrip.batch;
export declare const lastStatus: typeof earstrip.lastStatus;
export declare const setForcedCuts: typeof earstrip.setForcedCuts;
export declare const setConstrained: typeof earstrip.setConstrained;
export declare const flatten: typeof earstrip.flatten;
export declare const ready: Promise<void>;
