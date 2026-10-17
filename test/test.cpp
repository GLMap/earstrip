// earstrip tests: exact covers, flags and strips over hand-made polygons, polygons that snap rounding produced from
// OpenStreetMap tiles, and seeded random polygons; then forced cuts, floating point coordinates, the flat interface,
// index and point types. Run: make test
#include "earstrip.hpp"
#include <array>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

struct Pt {
    int32_t x, y;
};

using Rings = std::vector<std::vector<Pt>>;

constexpr double Pi = 3.14159265358979323846;

// Ears only, forced cuts off: invalid input is refused, so these cases see exactly what the ears do.
struct Clipper {
    earstrip::Triangulator<uint32_t> t;
    Clipper()
    {
        t.setForcedCuts(false);
        t.setConstrained(false);
    }
    bool run(const Rings &rings) { return t.run(rings) != earstrip::Status::Refused; }
    bool droppedHoles() const { return t.holesLeftOut(); }
    const std::vector<uint32_t> &getStrips() const { return t.strips(); }
    const std::vector<uint32_t> &getStripEnds() const { return t.stripEnds(); }
};

static int failures = 0;
static Clipper clipper; // one instance for every case: buffers are reused

static __int128 ringArea2(const std::vector<Pt> &r)
{
    __int128 a = 0;
    for (size_t i = 0; i < r.size(); ++i) {
        const Pt p = r[i], q = r[(i + 1) % r.size()];
        a += (__int128)p.x * q.y - (__int128)q.x * p.y;
    }
    return a;
}

// Runs the clipper and checks the result flags, exact cover, strip orientation and that no triangle is flat.
// eitherResult accepts a refusal too, for degenerate input outside the guarantees.
static bool check(const char *name, const Rings &rings, bool expectOk, bool expectDropped = false, bool verbose = true, bool eitherResult = false)
{
    const bool ok = clipper.run(rings);
    bool good = (ok == expectOk || eitherResult) && clipper.droppedHoles() == expectDropped;
    std::string why;
    if (!good)
        why = "result " + std::to_string(ok) + " dropped " + std::to_string(clipper.droppedHoles());
    size_t triangles = 0;
    if (good && ok) {
        std::vector<Pt> all;
        __int128 expected = 0;
        for (size_t r = 0; r < rings.size(); ++r) {
            all.insert(all.end(), rings[r].begin(), rings[r].end());
            const __int128 a = ringArea2(rings[r]);
            const __int128 abs = a < 0 ? -a : a;
            expected += r == 0 ? abs : -abs;
        }
        const auto &idx = clipper.getStrips();
        const auto &ends = clipper.getStripEnds();
        __int128 sum = 0;
        uint32_t start = 0;
        for (uint32_t end : ends) {
            int sign = 0;
            if (end < start + 3) {
                good = false;
                why = "strip shorter than a triangle";
            }
            for (uint32_t k = start; k + 2 < end; ++k) {
                triangles++;
                if (idx[k] >= all.size() || idx[k + 1] >= all.size() || idx[k + 2] >= all.size()) {
                    good = false;
                    why = "index out of range";
                    continue;
                }
                const Pt a = all[idx[k]], b = all[idx[k + 1]], c = all[idx[k + 2]];
                __int128 cr = (__int128)(b.x - (int64_t)a.x) * (c.y - (int64_t)a.y) - (__int128)(b.y - (int64_t)a.y) * (c.x - (int64_t)a.x);
                if ((k - start) & 1)
                    cr = -cr;
                const int sg = cr > 0 ? 1 : cr < 0 ? -1 : 0;
                if (sg == 0) {
                    good = false;
                    why = "flat triangle";
                }
                if (sg && sign && sg != sign) {
                    good = false;
                    why = "strip orientation flips";
                }
                if (sg)
                    sign = sg;
                sum += cr < 0 ? -cr : cr;
            }
            start = end;
        }
        // with a dropped hole the triangles cover the outer ring minus the other holes, which this check does not know
        if (!expectDropped && sum != expected) {
            good = false;
            why = "area " + std::to_string((double)sum) + " expected " + std::to_string((double)expected);
        }
    }
    if (verbose || !good)
        printf(
            "%-38s %s  ok=%d dropped=%d triangles=%zu strips=%zu indices=%zu %s\n",
            name,
            good ? "PASS" : "FAIL",
            ok,
            clipper.droppedHoles(),
            triangles,
            clipper.getStripEnds().size(),
            clipper.getStrips().size(),
            why.c_str());
    failures += !good;
    return good;
}

static std::vector<Pt> reversed(std::vector<Pt> r)
{
    return {r.rbegin(), r.rend()};
}

static std::vector<Pt> scaled(const std::vector<Pt> &r, int32_t s, int32_t dx = 0, int32_t dy = 0)
{
    std::vector<Pt> out;
    for (Pt p : r)
        out.push_back({p.x * s + dx, p.y * s + dy});
    return out;
}

// Random star-shaped outer ring with small holes inside its kernel, some touching each other, with repeated and
// collinear points sprinkled in and random ring orientations.
static Rings randomPolygon(std::mt19937 &rng, int32_t scale)
{
    std::uniform_real_distribution<double> unit(0, 1);
    const double radius = 10000;
    const int points = 24 + (int)(unit(rng) * 300); // enough points to keep the inner disk inside
    Rings rings(1);
    for (int i = 0; i < points; ++i) {
        const double angle = 2 * Pi * (i + 0.4 * unit(rng)) / points;
        const double r = radius * (0.4 + 0.6 * unit(rng));
        rings[0].push_back({(int32_t)std::lround(r * std::cos(angle)), (int32_t)std::lround(r * std::sin(angle))});
    }
    // holes on a grid of cells inside the disk of radius 0.35 * radius, which the star always contains
    const int cells = (int)(unit(rng) * 7);
    const double cell = 0.7 * radius / std::max(cells, 1) / std::sqrt(2.0);
    for (int cx = 0; cx < cells; ++cx) {
        for (int cy = 0; cy < cells; ++cy) {
            if (unit(rng) < 0.3)
                continue;
            auto grid = [&](int k) { return (int32_t)std::floor(-0.35 * radius / std::sqrt(2.0) + k * cell); };
            const double x0 = grid(cx), y0 = grid(cy);
            std::vector<Pt> hole;
            if (unit(rng) < 0.3) {
                // full cell square: squares on the diagonals touch at corners in an X that encloses nothing
                if (cx != cy && (cx + cy != cells - 1 || cells % 2 == 0))
                    continue;
                hole = {{grid(cx), grid(cy)}, {grid(cx + 1), grid(cy)}, {grid(cx + 1), grid(cy + 1)}, {grid(cx), grid(cy + 1)}};
            } else {
                const int n = 3 + (int)(unit(rng) * 6);
                for (int i = 0; i < n; ++i) {
                    const double angle = 2 * Pi * (i + 0.5 * unit(rng)) / n;
                    const double r = cell * (0.15 + 0.3 * unit(rng));
                    hole.push_back({(int32_t)std::lround(x0 + cell / 2 + r * std::cos(angle)), (int32_t)std::lround(y0 + cell / 2 + r * std::sin(angle))});
                }
            }
            rings.push_back(hole);
        }
    }
    for (auto &ring : rings) {
        // repeated points and collinear midpoints
        std::vector<Pt> out;
        for (size_t i = 0; i < ring.size(); ++i) {
            out.push_back(ring[i]);
            const Pt q = ring[(i + 1) % ring.size()];
            if (unit(rng) < 0.05)
                out.push_back(ring[i]);
            if (unit(rng) < 0.05 && (q.x - ring[i].x) % 2 == 0 && (q.y - ring[i].y) % 2 == 0)
                out.push_back({(ring[i].x + q.x) / 2, (ring[i].y + q.y) / 2});
        }
        if (unit(rng) < 0.5)
            out = reversed(out);
        for (Pt &p : out)
            p = {p.x * scale, p.y * scale};
        ring = out;
    }
    return rings;
}

// Polygons GEOS reports valid after snap rounding onto an integer grid, which a ray along a row of vertices used to
// bridge through; format: "# name", one line of "x,y" points per ring (outer first), blank line between polygons.
static const char *snapRounded = R"(# 14-bit after snap rounding, 219 rings
10169,2319 10222,2327 10248,2288 10288,2314 10334,2303 10351,2246 10374,2397 10333,2394 10316,2478 10441,2510 10446,2509 10494,2467 10562,2439 10640,2440 10660,2372 10716,2363 10743,2318 10736,2248 10711,2248 10708,2312 10657,2350 10658,2268 10568,2269 10533,2269 10642,2245 10656,2191 10418,2211 10363,2224 10324,2174 10338,2137 10316,2113 10287,2080 10285,2077 10255,2100 10167,1934 10140,1929 10148,1961 10162,2098 10143,2141
10568,2269 10568,2273 10565,2271
10316,2113 10314,2115 10314,2112
10287,2080 10288,2082 10283,2080
10706,2347 10702,2348 10702,2345
10656,2338 10656,2341 10652,2340
10655,2286 10650,2295 10650,2287
10654,2362 10650,2365 10649,2358
10650,2373 10643,2376 10643,2370
10649,2350 10645,2356 10645,2347
10639,2339 10634,2347 10631,2340
10634,2233 10632,2233 10632,2230
10630,2376 10633,2380 10629,2383
10630,2401 10633,2404 10627,2405
10630,2375 10625,2374 10629,2367
10626,2391 10622,2401 10616,2393
10624,2403 10625,2408 10618,2405
10623,2410 10624,2414 10619,2413
10622,2340 10621,2337 10623,2336
10612,2370 10621,2369 10617,2374
10622,2342 10623,2345 10620,2344
10619,2237 10623,2240 10618,2240
10619,2338 10620,2341 10617,2339
10614,2275 10618,2280 10614,2281
10609,2427 10612,2432 10605,2435
10608,2305 10611,2310 10604,2309
10607,2411 10611,2413 10607,2416
10608,2370 10602,2382 10596,2371
10609,2343 10602,2359 10595,2348
10606,2314 10608,2323 10600,2326
10599,2332 10602,2336 10598,2337
10594,2374 10593,2383 10588,2376
10579,2393 10592,2402 10575,2400
10590,2419 10585,2420 10584,2415
10587,2372 10583,2376 10581,2373
10575,2348 10574,2354 10571,2350
10569,2404 10571,2409 10566,2409
10569,2430 10570,2434 10560,2433
10566,2385 10567,2391 10560,2390
10566,2349 10563,2363 10555,2356
10562,2327 10562,2329 10560,2328
10562,2300 10559,2303 10558,2302
10551,2418 10554,2427 10547,2425
10553,2351 10551,2354 10551,2352
10552,2310 10547,2311 10546,2303
10551,2405 10546,2414 10542,2406
10549,2242 10540,2245 10539,2239
10546,2330 10544,2335 10539,2329
10543,2343 10540,2343 10541,2339
10543,2321 10542,2323 10541,2321
10542,2257 10540,2257 10541,2255
10530,2406 10533,2418 10525,2410
10532,2352 10530,2355 10529,2352
10527,2322 10531,2325 10524,2327
10529,2355 10527,2355 10528,2353
10524,2245 10526,2252 10518,2251
10524,2447 10525,2450 10522,2449
10523,2327 10520,2330 10516,2329
10516,2323 10514,2330 10508,2325
10512,2355 10513,2358 10510,2358
10508,2286 10512,2292 10505,2291
10502,2357 10509,2359 10503,2363
10503,2346 10502,2350 10500,2348
10502,2407 10503,2410 10500,2409
10461,2295 10498,2295 10470,2310
10497,2366 10491,2369 10486,2362
10493,2408 10490,2411 10489,2407
10491,2398 10483,2401 10485,2393
10490,2331 10488,2333 10487,2331
10488,2377 10488,2380 10486,2379
10484,2334 10487,2336 10484,2338
10483,2355 10480,2364 10478,2357
10481,2239 10483,2241 10479,2241
10475,2317 10477,2328 10470,2322
10475,2463 10477,2469 10473,2466
10474,2329 10473,2332 10471,2330
10470,2317 10467,2321 10465,2316
10404,2379 10460,2359 10448,2409 10418,2418
10467,2289 10463,2293 10462,2289
10461,2242 10456,2248 10455,2244
10452,2219 10459,2238 10448,2236
10444,2498 10445,2506 10439,2506
10441,2489 10443,2492 10441,2493
10442,2419 10441,2420 10439,2419
10422,2335 10410,2343 10406,2332
10423,2303 10415,2303 10412,2295
10423,2482 10419,2484 10420,2480
10406,2340 10407,2353 10399,2354
10405,2241 10404,2248 10398,2246
10405,2474 10401,2478 10401,2472
10404,2317 10401,2321 10399,2317
10400,2283 10398,2307 10385,2290
10392,2401 10399,2410 10391,2410
10397,2315 10389,2322 10389,2314
10396,2396 10393,2401 10389,2395
10396,2261 10392,2266 10389,2260
10385,2228 10389,2231 10384,2236
10389,2252 10381,2259 10377,2249
10387,2339 10384,2343 10381,2336
10385,2313 10376,2324 10376,2313
10384,2238 10385,2240 10382,2240
10383,2227 10379,2229 10380,2225
10368,2231 10377,2246 10362,2255
10377,2255 10374,2263 10372,2258
10370,2223 10370,2226 10366,2225
10370,2280 10367,2283 10365,2278
10361,2224 10360,2239 10354,2230
10354,2422 10357,2427 10352,2428
10334,2238 10348,2247 10333,2251
10329,2219 10343,2229 10320,2232
10345,2233 10344,2237 10339,2235
10332,2207 10344,2216 10332,2218
10336,2452 10335,2458 10333,2451
10329,2249 10329,2251 10326,2249
10329,2214 10327,2214 10328,2212
10328,2218 10327,2219 10326,2218
10328,2208 10320,2206 10322,2202
10328,2238 10325,2240 10325,2235
10325,2242 10327,2246 10324,2248
10323,2213 10319,2214 10319,2211
10319,2214 10306,2206 10314,2200
10319,2283 10321,2288 10316,2288
10317,2226 10315,2226 10315,2223
10316,2248 10312,2252 10311,2246
10315,2262 10306,2269 10305,2260
10308,2278 10313,2281 10309,2290
10291,2197 10307,2197 10303,2209
10306,2243 10295,2256 10292,2248
10306,2255 10304,2257 10304,2254
10305,2285 10303,2288 10303,2285
10302,2240 10298,2239 10299,2237
10301,2175 10301,2183 10294,2178
10301,2232 10296,2235 10295,2228
10289,2255 10300,2278 10285,2276
10297,2163 10300,2170 10294,2173
10297,2213 10299,2214 10297,2218
10298,2209 10298,2212 10295,2210
10298,2298 10294,2300 10294,2294
10298,2194 10295,2195 10295,2192
10295,2189 10292,2190 10293,2186
10291,2241 10294,2240 10295,2244
10294,2213 10291,2214 10293,2211
10292,2221 10290,2224 10287,2221
10291,2143 10287,2143 10289,2139
10291,2238 10289,2240 10287,2237
10284,2221 10282,2233 10268,2236
10287,2289 10283,2287 10287,2285
10284,2188 10287,2195 10284,2195
10286,2158 10272,2167 10273,2156
10280,2251 10281,2276 10262,2269
10284,2201 10283,2203 10280,2201
10282,2090 10280,2093 10279,2090
10213,2136 10259,2189 10202,2164
10278,2199 10280,2213 10271,2209
10279,2218 10278,2221 10276,2219
10276,2242 10277,2244 10275,2243
10276,2223 10273,2223 10274,2219
10274,2212 10276,2215 10274,2216
10273,2194 10274,2200 10270,2198
10268,2247 10271,2246 10271,2251
10260,2198 10272,2206 10261,2211
10270,2218 10271,2219 10269,2219
10268,2172 10267,2176 10264,2174
10263,2175 10267,2180 10261,2179
10264,2239 10266,2241 10263,2242
10258,2267 10260,2272 10254,2276
10218,2192 10264,2231 10212,2236
10261,2158 10256,2158 10257,2154
10260,2206 10259,2209 10258,2207
10257,2201 10255,2203 10255,2201
10248,2204 10253,2212 10245,2212
10247,2279 10248,2283 10241,2281
10245,2267 10243,2268 10244,2265
10245,2275 10240,2276 10240,2272
10245,2203 10244,2205 10242,2201
10236,2282 10240,2298 10226,2282
10243,2143 10236,2146 10233,2132
10241,2278 10238,2281 10237,2278
10240,2204 10236,2205 10236,2201
10236,2113 10239,2120 10234,2117
10234,2250 10228,2276 10222,2257
10236,2274 10236,2276 10233,2275
10234,2125 10235,2130 10230,2126
10225,2246 10222,2246 10224,2242
10219,2259 10211,2262 10209,2250
10204,2187 10216,2182 10213,2192
10214,2125 10214,2129 10209,2128
10212,2046 10208,2041 10213,2041
10208,2193 10210,2195 10206,2195 10207,2194
10207,2194 10204,2194 10206,2191
10194,2234 10206,2241 10191,2242
10204,2195 10204,2197 10202,2196
10203,2175 10201,2174 10202,2172
10196,2154 10202,2159 10198,2167
10196,2004 10199,2007 10195,2010
10190,2123 10194,2134 10188,2130
10194,2186 10190,2186 10192,2184
10185,2299 10183,2301 10183,2298
10184,2126 10180,2127 10180,2123
10181,2263 10169,2265 10173,2258
10175,2280 10171,2282 10172,2278
10173,1970 10171,1970 10172,1967
10172,1961 10172,1965 10169,1963
10171,1943 10170,1947 10167,1943
10170,2250 10165,2256 10164,2254
10163,2155 10168,2167 10154,2159
10167,2196 10165,2205 10156,2197
10165,2140 10165,2148 10155,2153
10163,2123 10161,2121 10162,2120
10162,2134 10160,2136 10158,2134
10161,2175 10161,2177 10159,2176
10160,2111 10160,2116 10157,2114
10161,1953 10157,1954 10158,1950
10157,2180 10155,2179 10157,2178
10155,2136 10157,2138 10155,2142
10157,2185 10155,2187 10155,2183
10147,1937 10155,1942 10146,1945
10153,2161 10152,2163 10151,2160
10152,2145 10149,2149 10148,2147

# 12-bit after snap rounding, 5 rings
3199,3645 3206,3651 3236,3658 3279,3600 3271,3596 3276,3587 3270,3576 3247,3575 3255,3559 3255,3551 3233,3540 3223,3560 3233,3548 3251,3559 3239,3567 3225,3562 3218,3571 3202,3608 3210,3612 3205,3626 3191,3633 3186,3642
3250,3605 3259,3600 3252,3610
3263,3614 3263,3619 3259,3616
3261,3600 3258,3598 3260,3595
3249,3599 3246,3602 3247,3598

# 12-bit after snap rounding, 17 rings
2366,3082 2388,3134 2397,3150 2463,3163 2470,3138 2478,3071 2480,3062 2493,3073 2501,3068 2499,3061 2527,3043 2542,3042 2548,3053 2551,3064 2553,3070 2555,3060 2544,3037 2529,3021 2492,2997 2483,3004 2479,2994 2463,2981 2435,3009 2409,3028 2390,3045 2389,3054 2369,3072
2551,3064 2552,3062 2553,3065
2504,3010 2507,3011 2505,3011
2464,3077 2470,3077 2466,3081
2460,3070 2465,3068 2467,3071
2464,3082 2465,3086 2459,3085
2462,3114 2463,3117 2457,3116
2464,3128 2462,3129 2461,3128
2451,3134 2464,3132 2452,3152 2445,3145
2461,3075 2457,3080 2456,3077
2460,3128 2458,3129 2457,3128
2458,3067 2459,3071 2453,3070
2406,3046 2411,3054 2405,3060 2397,3051
2401,3063 2396,3066 2398,3059
2393,3074 2399,3074 2392,3080
2398,3055 2393,3058 2391,3054
2390,3082 2389,3094 2383,3089

# 12-bit after snap rounding, 22 rings
2541,3104 2590,3114 2643,3109 2669,3116 2744,3134 2779,3136 2777,3135 2779,3134 2780,3136 2822,3137 2857,3129 2877,3127 2937,3105 2963,3084 2990,3066 3008,3040 3009,3029 3034,3008 3056,2976 3058,2958 3054,2941 3064,2893 3073,2865 3080,2856 3089,2827 3104,2798 3107,2786 3123,2763 3130,2736 3094,2724 3080,2766 3077,2783 3067,2809 3058,2824 3031,2852 3013,2893 2992,2954 2978,2981 2959,3001 2957,3010 2945,3020 2937,3039 2917,3057 2881,3076 2863,3081 2849,3094 2821,3093 2786,3077 2743,3073 2721,3068 2698,3073 2642,3072 2592,3068 2554,3078 2519,3074 2535,3086
2691,3086 2694,3088 2683,3089
2702,3095 2747,3094 2757,3100 2748,3105 2713,3106 2678,3098
2710,3107 2708,3108 2699,3105
2764,3108 2800,3109 2822,3115 2815,3120 2798,3124 2773,3122 2741,3113
2739,3116 2776,3125 2740,3122 2727,3125 2719,3122
2733,3126 2733,3127 2730,3126
2766,3127 2762,3130 2748,3127
2746,3127 2754,3132 2736,3128
2761,3131 2762,3132 2761,3132
2780,3131 2780,3133 2779,3131
2768,3132 2767,3134 2766,3133
2775,3133 2772,3134 2769,3133
3085,2774 3095,2747 3093,2764 3084,2793
3025,2911 3029,2894 3043,2871 3057,2859 3045,2902 3038,2921 3034,2930 3025,2941
3056,2928 3053,2941 3049,2942
3014,2955 3010,2969 3008,2959
3023,2985 3028,2958 3043,2941 3052,2956 3052,2971 3029,3005 3010,3021
2984,2988 2994,2980 2995,3016 2966,3030 2971,3010
2946,3022 2955,3016 2965,2999 2974,2995 2963,3020 2947,3032 2966,3022 2955,3039 2936,3057 2923,3076 2901,3089 2866,3097 2857,3095 2871,3082 2894,3073 2927,3053 2944,3034 2946,3033
2939,3063 2959,3039 2965,3038 2979,3027 2958,3058 2984,3024 2997,3016 2988,3040 2961,3068 2929,3082
2847,3118 2852,3122 2828,3123

# 14-bit after snap rounding, 88 rings
10231,13024 10227,12997 10222,12965 10170,12967 10200,12938 10094,12887 10044,12820 10064,12804 10131,12825 10209,12826 10186,12618 10110,11880 10104,11854 10047,11836 10100,11815 10047,11799 10053,11738 10094,11747 10096,11685 10060,11623 10019,11626 10038,11568 9979,11567 9969,11627 9953,11577 9882,11566 9767,11604 9787,11730 9754,11736 9731,11627 9743,11738 9689,11596 9657,11598 9610,11607 9587,11611 9212,11843 9004,11841 8889,11927 8731,11996 8511,12077 8260,12172 8081,12260 7988,12322 7866,12753 7724,13266 7785,13271 7873,13245 7862,13154 7991,13099 7998,13039 8068,13027 8093,13052 8468,12902 8547,13089 8624,13057 8668,13013 8728,12988 8841,13240 8820,13275 8776,13288 8817,13348 8832,13305 8892,13275 8943,13325 8988,13302 9065,13318 9066,13261 9044,13213 9115,13227 9111,13198 9347,13188 9360,13073 9409,13110 9454,13082 9451,12997 9496,13000 9510,13060 9546,13064 9583,13014 9553,13101 9572,13157 9540,13177 9562,13237 9650,13220 9665,13244 9641,13283 9601,13278 9545,13309 9632,13302 9636,13325 9817,13276 10039,13278 10028,13256 9919,13256 9868,13234 9871,13157 9838,13155 9857,13070 10056,13076 10140,13152 10095,13074 10067,12947 10112,12946 10148,12982 10176,13089 10205,13049 10239,13096
9610,11607 9627,11621 9603,11622
10149,12696 10174,12780 10149,12788
10055,12390 10033,12389 10053,12378
10041,12421 10028,12420 10029,12405
10027,12440 10011,12448 10007,12439
10017,12383 10017,12397 10005,12397
10012,12247 9995,12246 10010,12235
10007,11712 10007,11732 9992,11735
9933,12714 9996,12756 9983,12776
9986,11724 9952,11746 9948,11722
9919,12581 9928,12612 9912,12610
9825,12373 9821,12362 9834,12367
9822,11759 9810,11784 9796,11766
9813,11740 9800,11742 9806,11701
9812,11878 9798,11891 9794,11877
9776,12071 9761,12063 9773,12058
9772,11779 9766,11802 9755,11778
9769,11922 9760,11927 9757,11920
9748,11905 9731,11905 9741,11895
9737,11787 9726,11798 9710,11790
9737,11815 9733,11830 9717,11815
9735,11918 9708,11922 9722,11905
9713,13169 9706,13146 9726,13146
9698,11793 9715,11803 9679,11823
9714,11839 9693,11832 9701,11825
9711,11942 9696,11948 9704,11927
9697,11844 9687,11862 9680,11845
9696,11638 9679,11637 9688,11625
9654,11886 9671,11913 9632,11900
9668,11831 9632,11826 9636,11812
9670,11931 9665,11963 9645,11949
9659,11846 9642,11867 9630,11848
9656,11922 9639,11934 9628,11913
9651,11728 9630,11734 9644,11717
9653,11690 9637,11694 9627,11671
9653,11639 9636,11657 9637,11627
9638,11978 9617,11970 9639,11953
9644,11772 9637,11786 9639,11771
9637,12325 9639,12335 9634,12329
9620,11683 9628,11705 9600,11683
9624,11858 9610,11840 9617,11832
9608,12891 9626,12914 9573,12952 9546,12940
9619,11748 9592,11735 9605,11722
9613,11974 9582,11973 9620,11945
9613,11704 9595,11717 9596,11699
9595,11794 9607,11813 9579,11814
9607,11995 9591,12023 9579,12003
9587,11782 9586,11794 9566,11783
9549,12950 9537,12956 9539,12945
9539,11995 9500,11989 9519,11973
9481,12006 9470,12013 9474,12001
9453,11981 9461,11995 9444,11991
9446,12462 9437,12463 9447,12452
9441,11961 9428,11972 9424,11956
9367,12882 9408,12890 9389,12938 9360,12933
9378,12541 9364,12545 9377,12530
9369,12313 9361,12320 9358,12306
9352,11777 9331,11783 9344,11764
9331,12062 9303,12075 9312,12053
9329,11798 9312,11805 9305,11792
9326,12926 9270,12921 9300,12893
9324,12793 9324,12801 9311,12795
9266,13086 9282,13110 9204,13124
9287,12059 9268,12049 9284,12045
9286,12037 9250,12039 9262,12026
9269,12015 9248,11998 9258,11985
9248,12020 9222,12026 9232,12011
9240,11853 9227,11837 9235,11833
9185,12648 9158,12651 9182,12635
9097,13096 9136,13134 9045,13136
9050,13158 9050,13194 9041,13170
8997,13049 8990,13069 8978,13067
8956,13285 8955,13303 8929,13288
8959,12026 8943,12020 8945,12014
8948,12819 8935,12823 8930,12796
8742,12943 8837,13011 8784,13007
8825,12943 8831,12989 8774,12942
8780,12572 8762,12555 8776,12545
8723,12838 8690,12864 8681,12851
8630,12987 8628,13006 8620,12987
8621,12972 8611,12966 8617,12958
8599,12493 8576,12513 8564,12497
8564,12296 8574,12322 8546,12308
8227,12505 8317,12492 8274,12523 8186,12552
8258,12640 8225,12636 8234,12624
7934,12533 8051,12510 8153,12507 8162,12543 8122,12539 8103,12569 8059,12576 7968,12554
8038,12587 8100,12577 8076,12627 8011,12627

# 12-bit after snap rounding, 88 rings
2557,3255 2556,3249 2555,3241 2542,3241 2550,3234 2523,3221 2511,3204 2516,3200 2532,3206 2552,3206 2546,3154 2527,2969 2526,2963 2511,2958 2525,2953 2511,2949 2513,2934 2523,2936 2524,2921 2515,2905 2504,2906 2509,2891 2494,2891 2492,2906 2488,2894 2470,2891 2441,2900 2446,2932 2438,2933 2432,2906 2435,2934 2422,2898 2414,2899 2402,2901 2396,2902 2336,2940 2303,2960 2251,2960 2222,2981 2182,2998 2127,3019 2065,3042 2020,3064 1997,3080 1966,3188 1931,3316 1946,3317 1968,3311 1965,3288 1997,3274 1999,3259 2017,3256 2023,3262 2117,3225 2136,3272 2156,3264 2167,3253 2182,3246 2210,3309 2205,3318 2194,3321 2204,3336 2208,3326 2223,3318 2235,3331 2247,3325 2266,3329 2266,3315 2261,3303 2278,3306 2277,3299 2336,3296 2340,3268 2352,3277 2363,3270 2362,3249 2374,3249 2377,3264 2386,3265 2395,3253 2388,3275 2393,3289 2385,3294 2390,3309 2412,3304 2416,3310 2410,3320 2400,3319 2386,3327 2408,3325 2409,3331 2454,3318 2509,3319 2507,3313 2479,3313 2467,3308 2467,3289 2459,3288 2464,3267 2514,3268 2535,3287 2523,3268 2516,3236 2528,3236 2537,3245 2544,3272 2551,3262 2559,3273
2402,2901 2406,2905 2400,2905
2336,2940 2338,2944 2332,2945
2537,3173 2543,3194 2537,3196
2513,3097 2508,3097 2513,3094
2510,3105 2507,3104 2507,3101
2506,3109 2502,3111 2501,3109
2504,3095 2504,3099 2501,3099
2503,3061 2498,3061 2502,3058
2501,2927 2501,2932 2498,2933
2483,3178 2499,3188 2495,3193
2496,2930 2488,2936 2487,2930
2479,3145 2482,3152 2478,3152
2456,3093 2455,3090 2458,3091
2455,2939 2452,2945 2449,2941
2453,2934 2450,2935 2451,2925
2453,2969 2449,2972 2448,2969
2444,3017 2440,3015 2443,3014
2443,2944 2441,2950 2438,2944
2442,2980 2440,2981 2439,2979
2437,2976 2432,2976 2435,2973
2434,2946 2431,2949 2427,2947
2434,2953 2433,2957 2429,2953
2433,2979 2427,2980 2430,2976
2428,3292 2426,3286 2431,3286
2424,2948 2428,2950 2419,2955
2428,2959 2423,2957 2425,2956
2427,2985 2424,2986 2426,2981
2424,2960 2421,2965 2420,2961
2424,2909 2419,2909 2422,2906
2413,2971 2417,2978 2408,2974
2417,2957 2408,2956 2409,2952
2417,2982 2416,2990 2411,2987
2414,2961 2410,2966 2407,2961
2414,2980 2409,2983 2407,2978
2412,2931 2407,2933 2411,2929
2413,2922 2409,2923 2406,2917
2413,2909 2409,2914 2409,2906
2409,2994 2404,2992 2409,2988
2411,2942 2409,2946 2409,2942
2409,3081 2409,3083 2408,3082
2405,2920 2407,2926 2400,2920
2406,2964 2402,2959 2404,2957
2402,3222 2406,3228 2393,3237 2386,3234
2404,2936 2398,2933 2401,2930
2403,2993 2395,2993 2405,2986
2403,2925 2398,2929 2399,2924
2398,2948 2401,2953 2394,2953
2401,2998 2397,3005 2394,3000
2396,2945 2396,2948 2391,2945
2387,3237 2384,3238 2384,3236
2384,2998 2375,2997 2379,2993
2370,3001 2367,3003 2368,3000
2363,2995 2365,2998 2361,2997
2361,3115 2359,3115 2361,3112
2360,2990 2357,2992 2356,2988
2341,3220 2352,3222 2347,3234 2340,3233
2344,3135 2341,3136 2344,3132
2342,3078 2340,3079 2339,3076
2332,3015 2325,3018 2328,3013
2332,2949 2328,2951 2326,2947
2331,3231 2317,3230 2325,3223
2331,3198 2331,3200 2327,3198
2316,3271 2320,3277 2301,3280
2321,3014 2317,3012 2321,3011
2321,3009 2312,3009 2315,3006
2317,3003 2312,2999 2314,2996
2312,3004 2305,3006 2308,3002
2310,2963 2306,2959 2308,2958
2296,3161 2289,3162 2295,3158
2274,3273 2284,3283 2261,3283
2262,3289 2262,3298 2260,3292
2249,3262 2247,3267 2244,3266
2239,3321 2238,3325 2232,3321
2239,3006 2235,3004 2236,3003
2237,3204 2233,3205 2232,3198
2185,3235 2209,3252 2196,3251
2206,3235 2207,3247 2193,3235
2195,3142 2190,3138 2194,3136
2180,3209 2172,3215 2170,3212
2157,3246 2157,3251 2155,3246
2155,3242 2152,3241 2154,3239
2149,3123 2144,3128 2141,3124
2141,3073 2143,3080 2136,3076
2056,3126 2079,3122 2068,3130 2046,3137
2064,3159 2056,3158 2058,3155
1983,3133 2012,3127 2038,3126 2040,3135 2030,3134 2025,3142 2014,3143 1992,3138
2009,3146 2025,3144 2019,3156 2002,3156
)";

// Holes touching other rings at points inside their edges.
static const char *edgeTouching = R"(# hole vertex on the left outer edge (not the rightmost hole vertex)
0,0 10,0 10,10 0,10
0,5 5,3 5,7

# hole vertex on the bottom outer edge
0,0 10,0 10,10 0,10
5,0 7,4 3,4

# hole vertex on the top outer edge, rightmost vertex elsewhere
0,0 10,0 10,10 0,10
4,10 2,7 8,6

# hole vertex on another hole's edge
0,0 20,0 20,20 0,20
2,2 10,2 10,10 2,10
10,6 15,4 15,8

# hole vertex on another hole's edge, hole on the left
0,0 20,0 20,20 0,20
12,2 18,2 18,10 12,10
12,6 6,4 6,8

# two holes touching the outer ring and each other at edge points
0,0 20,0 20,20 0,20
0,10 6,6 6,14
6,10 12,6 12,14

# hole vertices on two outer edges (splits interior) - invalid, either result
0,0 10,0 10,10 0,10
0,5 5,4 10,5 5,6
)";

static std::vector<std::pair<std::string, Rings>> parsePolygons(const char *text)
{
    std::vector<std::pair<std::string, Rings>> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        if (line[0] == '#') {
            out.push_back({line.substr(2), {}});
            continue;
        }
        std::istringstream points(line);
        std::string token;
        out.back().second.emplace_back();
        while (points >> token) {
            const size_t comma = token.find(',');
            out.back().second.back().push_back({std::stoi(token.substr(0, comma)), std::stoi(token.substr(comma + 1))});
        }
    }
    return out;
}

// Tiny convex holes with corners on a coarse integer grid in separate cells, so that many vertices of different
// holes share a row with the rightmost vertex of another hole.
static Rings gridHoles(std::mt19937 &rng)
{
    std::uniform_int_distribution<int> coord(0, 6), count(3, 7);
    const int cells = 3 + (int)(rng() % 10);
    const int32_t cell = 8;
    Rings rings = {{{-cell, -cell}, {cells * cell + cell, -cell}, {cells * cell + cell, cells * cell + cell}, {-cell, cells * cell + cell}}};
    for (int cx = 0; cx < cells; ++cx) {
        for (int cy = 0; cy < cells; ++cy) {
            if (rng() % 4 == 0)
                continue;
            // convex hull of a few random grid points
            std::vector<Pt> pts;
            for (int i = count(rng); i > 0; --i)
                pts.push_back({cx * cell + coord(rng), cy * cell + coord(rng)});
            std::sort(pts.begin(), pts.end(), [](Pt a, Pt b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
            std::vector<Pt> hull;
            for (int pass = 0; pass < 2; ++pass) {
                const size_t base = hull.size();
                for (Pt p : pts) {
                    while (hull.size() >= base + 2) {
                        const Pt a = hull[hull.size() - 2], b = hull.back();
                        if ((int64_t)(b.x - a.x) * (p.y - a.y) - (int64_t)(b.y - a.y) * (p.x - a.x) > 0)
                            break;
                        hull.pop_back();
                    }
                    hull.push_back(p);
                }
                hull.pop_back();
                std::reverse(pts.begin(), pts.end());
            }
            if (hull.size() >= 3)
                rings.push_back(rng() % 2 ? hull : reversed(hull));
        }
    }
    return rings;
}


// Doubled area the triangles cover, over the input points and the added ones; false on a bad index, a flat triangle or a
// strip whose triangles turn different ways.
template <class Index> static bool cover(const earstrip::Triangulator<Index> &t, const std::vector<std::array<double, 2>> &points, double &area, std::string &why)
{
    std::vector<std::array<double, 2>> all(points);
    for (size_t k = 0; k + 1 < t.extraPoints().size(); k += 2)
        all.push_back({t.extraPoints()[k], t.extraPoints()[k + 1]});
    area = 0;
    uint32_t start = 0;
    for (size_t s = 0; s < t.stripEnds().size(); ++s) {
        const uint32_t end = t.stripEnds()[s];
        if (end < start + 3) {
            why = "strip shorter than a triangle";
            return false;
        }
        for (uint32_t k = start; k + 2 < end; ++k) {
            const auto &idx = t.strips();
            if (idx[k] >= all.size() || idx[k + 1] >= all.size() || idx[k + 2] >= all.size()) {
                why = "index out of range";
                return false;
            }
            const auto a = all[idx[k]], b = all[idx[k + 1]], c = all[idx[k + 2]];
            double cr = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            if ((((k - start) & 1) != 0) != (t.stripClockwise()[s] != 0))
                cr = -cr;
            if (cr <= 0) {
                why = cr == 0 ? "flat triangle" : "triangle against the strip's winding";
                return false;
            }
            area += cr;
        }
        start = end;
    }
    return true;
}

// The constrained triangulation: what it covers (inside ring 0 by non-zero winding, outside every hole), the points it
// adds at crossings, and when it is used.
static void constrainedTests()
{
    using P = std::array<double, 2>;
    using Poly = std::vector<std::vector<P>>;
    int bad = 0;
    auto expect = [&](const char *name, const Poly &poly, earstrip::Status status, double area2, size_t extra, double tolerance = 0) {
        earstrip::Triangulator<uint32_t> t;
        const auto s = t.run(poly);
        std::vector<P> points;
        for (const auto &r : poly)
            points.insert(points.end(), r.begin(), r.end());
        double area = 0;
        std::string why;
        bool good = cover(t, points, area, why);
        if (good && s != status)
            why = "status " + std::to_string((int)s);
        else if (good && std::abs(area - area2) > tolerance)
            why = "area " + std::to_string(area / 2) + " expected " + std::to_string(area2 / 2);
        else if (good && t.extraPoints().size() != 2 * extra)
            why = std::to_string(t.extraPoints().size() / 2) + " added points, expected " + std::to_string(extra);
        good = why.empty();
        printf("%-38s %s  %s\n", name, good ? "PASS" : "FAIL", why.c_str());
        bad += !good;
        return t.extraPoints();
    };
    const auto bowTie = expect("constrained: bow tie", Poly{{{0, 0}, {10, 10}, {10, 0}, {0, 10}}}, earstrip::Status::Constrained, 100, 1);
    if (bowTie.size() != 2 || bowTie[0] != 5 || bowTie[1] != 5) {
        printf("%-38s FAIL  crossing not at (5, 5)\n", "constrained: bow tie crossing");
        bad++;
    }
    expect("constrained: square walked twice", Poly{{{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}, {10, 0}, {10, 10}, {0, 10}}}, earstrip::Status::Constrained, 200, 0);
    expect("constrained: hole outside", Poly{{{0, 0}, {10, 0}, {10, 10}, {0, 10}}, {{20, 0}, {24, 0}, {24, 4}, {20, 4}}}, earstrip::Status::Constrained, 200, 0);
    // a pentagram: its centre pentagon has winding 2 and stays filled, so the cover is the star's outline through the five
    // added crossings
    {
        Poly star(1);
        for (int k = 0; k < 5; ++k) {
            const double a = Pi / 2 + 4 * Pi * k / 5;
            star[0].push_back({std::round(1000 * std::cos(a)), std::round(1000 * std::sin(a))});
        }
        earstrip::Triangulator<uint32_t> t;
        const auto s = t.run(star);
        double area = 0, outline = 0;
        std::string why;
        bool good = cover(t, star[0], area, why) && s == earstrip::Status::Constrained && t.extraPoints().size() == 10;
        if (good) {
            std::vector<P> ring(star[0]);
            for (size_t k = 0; k < 10; k += 2)
                ring.push_back({t.extraPoints()[k], t.extraPoints()[k + 1]});
            std::sort(ring.begin(), ring.end(), [](const P &a, const P &b) { return std::atan2(a[1], a[0]) < std::atan2(b[1], b[0]); });
            for (size_t i = 0; i < ring.size(); ++i) {
                const P p = ring[i], q = ring[(i + 1) % ring.size()];
                outline += p[0] * q[1] - q[0] * p[1];
            }
            good = std::abs(area - outline) < 1e-6;
        }
        printf("%-38s %s  %s area %.0f, outline %.0f\n", "constrained: pentagram", good ? "PASS" : "FAIL", why.c_str(), area / 2, outline / 2);
        bad += !good;
    }
    // 200 holes: past 128 rings the ears are skipped
    {
        Poly grid(1);
        grid[0] = {{0, 0}, {410, 0}, {410, 210}, {0, 210}};
        double area2 = 2.0 * 410 * 210;
        for (int i = 0; i < 20; ++i)
            for (int j = 0; j < 10; ++j) {
                const double x = 10 + 20 * i, y = 10 + 20 * j;
                grid.push_back({{x, y}, {x, y + 10}, {x + 10, y + 10}, {x + 10, y}});
                area2 -= 200;
            }
        expect("constrained: 200 holes", grid, earstrip::Status::Constrained, area2, 0);
        earstrip::Triangulator<uint32_t> ears;
        ears.setConstrained(false);
        const bool good = ears.run(grid) == earstrip::Status::Exact;
        printf("%-38s %s\n", "ears alone: 200 holes", good ? "PASS" : "FAIL");
        bad += !good;
    }
    // lon/lat: rounded to the grid, the crossing comes back within a grid step
    {
        const Poly poly{{{21.0, 52.2}, {21.01, 52.21}, {21.01, 52.2}, {21.0, 52.21}}};
        const auto extra = expect("constrained: lon/lat bow tie", poly, earstrip::Status::Constrained, 1e-4, 1, 1e-12);
        const bool good = extra.size() == 2 && std::abs(extra[0] - 21.005) < 1e-9 && std::abs(extra[1] - 52.205) < 1e-9;
        printf("%-38s %s\n", "constrained: lon/lat crossing", good ? "PASS" : "FAIL");
        bad += !good;
    }
    // added points refused: the bow tie goes the ears' way and every index is an input vertex
    {
        earstrip::Triangulator<uint32_t> t;
        t.setExtraPoints(false);
        const auto s = t.run(Poly{{{0, 0}, {10, 10}, {10, 0}, {0, 10}}});
        bool good = s == earstrip::Status::Approximate && t.extraPoints().empty();
        for (uint32_t i : t.strips())
            good &= i < 4;
        printf("%-38s %s\n", "constrained: no added points allowed", good ? "PASS" : "FAIL");
        bad += !good;
    }
    // list mode gives what triangles() makes of the strips, for constrained polygons too
    {
        const Poly polys[] = {{{{0, 0}, {10, 10}, {10, 0}, {0, 10}}}, {{{0, 0}, {20, 20}, {20, 0}, {0, 10}}}};
        earstrip::Triangulator<uint32_t> strips, list;
        list.setTriangleList(true);
        std::vector<uint32_t> tris;
        bool good = true;
        for (const Poly &poly : polys) {
            strips.run(poly);
            strips.triangles(tris);
            good &= list.run(poly) == earstrip::Status::Constrained && tris == list.triangleList();
        }
        printf("%-38s %s\n", "constrained: triangle list mode", good ? "PASS" : "FAIL");
        bad += !good;
    }
    // more points than uint16_t can index: the constrained triangulation stays out
    {
        Poly big(1);
        for (int k = 0; k < 70000; ++k) {
            const double a = 2 * Pi * k / 70000;
            big[0].push_back({std::round(1e6 * std::cos(a)), std::round(1e6 * std::sin(a))});
        }
        big[0].push_back({0, 0});
        big[0].push_back({2e6, 1});
        earstrip::Triangulator<uint16_t> t;
        const auto s = t.run(big);
        const bool good = s != earstrip::Status::Constrained && t.extraPoints().empty();
        printf("%-38s %s  status %d\n", "constrained: past the index range", good ? "PASS" : "FAIL", (int)s);
        bad += !good;
    }
    failures += bad;
}

int main()
{
    const std::vector<Pt> square = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    check("triangle", {{{0, 0}, {5, 0}, {0, 7}}}, true);
    check("triangle cw", {{{0, 0}, {0, 7}, {5, 0}}}, true);
    check("convex quad", {square}, true);
    check("convex quad cw closed", {{{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}}}, true);
    check("concave quad", {{{0, 0}, {10, 0}, {3, 3}, {0, 10}}}, true);
    check("concave quad cw", {reversed({{0, 0}, {10, 0}, {3, 3}, {0, 10}})}, true);
    check("convex hexagon", {{{0, 0}, {4, -2}, {8, 0}, {8, 4}, {4, 6}, {0, 4}}}, true);
    check("L shape", {{{0, 0}, {10, 0}, {10, 4}, {4, 4}, {4, 10}, {0, 10}}}, true);
    check("square with hole", {square, {{3, 3}, {7, 3}, {7, 7}, {3, 7}}}, true);
    check("square with cw hole", {square, reversed({{3, 3}, {7, 3}, {7, 7}, {3, 7}})}, true);
    check("two holes", {scaled(square, 2), {{2, 2}, {8, 2}, {8, 8}, {2, 8}}, {{12, 12}, {18, 12}, {15, 18}}}, true);
    check("two holes same rightmost x", {scaled(square, 2), {{2, 2}, {8, 2}, {8, 8}, {2, 8}}, {{4, 12}, {8, 12}, {8, 18}}}, true);
    check("hole touching outer vertex", {square, {{10, 10}, {4, 7}, {7, 4}}}, true);
    check("hole touching outer, not rightmost", {{{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 5}}, {{0, 5}, {5, 3}, {5, 7}}}, true);
    check("hole vertex on vertical outer edge", {square, {{10, 5}, {4, 8}, {4, 2}}}, true);
    check("hole vertex on horizontal outer edge", {square, {{6, 10}, {3, 8}, {5, 6}}}, true);
    check("hole touching other hole", {scaled(square, 2), {{2, 2}, {8, 2}, {8, 8}, {2, 8}}, {{8, 8}, {14, 9}, {9, 14}}}, true);
    check("holes touching outer and each other", {{{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 5}}, {{0, 5}, {5, 3}, {5, 7}}, {{5, 7}, {8, 9}, {9, 4}}}, true);
    check("duplicates and collinear", {{{0, 0}, {0, 0}, {5, 0}, {10, 0}, {10, 0}, {10, 5}, {10, 10}, {5, 10}, {5, 10}, {0, 10}, {0, 5}, {0, 0}}}, true);
    check("collinear concave", {{{0, 0}, {4, 0}, {8, 0}, {8, 4}, {6, 4}, {4, 4}, {4, 8}, {2, 8}, {0, 8}, {0, 4}}}, true);
    // a spike makes the ring non-simple: its ears may be cut before the cleanup sees it, so refusing is allowed
    check("zero width spike", {{{0, 0}, {10, 0}, {10, 10}, {5, 10}, {5, 15}, {5, 10}, {0, 10}}}, true, false, true, true);
    check("zero width notch", {{{0, 0}, {10, 0}, {10, 10}, {5, 10}, {5, 5}, {5, 10}, {0, 10}}}, true, false, true, true);
    check("all collinear", {{{0, 0}, {5, 0}, {10, 0}}}, true);
    check("two distinct points", {{{0, 0}, {5, 0}, {0, 0}, {5, 0}}}, true);
    check("empty", {{}}, true);

    // Rings whose corners all turn one way but that wind around more than once are not convex: no fast path for them.
    // Refusing them is allowed; an accepted one must still be covered exactly by triangles that all turn one way.
    const std::vector<Pt> nearSpike =
        {{68, 1684}, {129, 1657}, {176, 1626}, {196, 1608}, {212, 1585}, {223, 1559}, {228, 1531}, {227, 1248}, {227, 1249}, {230, 526}, {0, 527}, {0, 1698}};
    check("near spike crossing itself (gom4)", {nearSpike}, true, false, true, true);
    int variantFailures = 0;
    for (int sym = 0; sym < 8; ++sym) {
        for (size_t start = 0; start < nearSpike.size(); ++start) {
            for (int32_t scale : {1, 100000}) {
                // rotations by quarter turns, mirrors, every start point, tile and world scale
                std::vector<Pt> ring;
                for (size_t i = 0; i < nearSpike.size(); ++i) {
                    Pt p = nearSpike[(start + i) % nearSpike.size()];
                    if (sym & 1)
                        p = {-p.x, p.y};
                    for (int r = 0; r < sym / 2; ++r)
                        p = {-p.y, p.x};
                    ring.push_back({p.x * scale, p.y * scale});
                }
                variantFailures += !check("near spike variant", {ring}, true, false, false, true);
            }
        }
    }
    printf("%-38s %s  %d failed\n", "near spike, 192 variants", variantFailures ? "FAIL" : "PASS", variantFailures);
    int starFailures = 0;
    for (int points = 5; points <= 13; ++points) {
        for (int step = 2; 2 * step < points; ++step) {
            if (std::gcd(points, step) != 1)
                continue;
            // star polygon {points/step}: every corner turns the same way, the ring winds step times
            std::vector<Pt> star;
            for (int i = 0; i < points; ++i) {
                const double angle = 2 * Pi * step * i / points;
                star.push_back({(int32_t)std::lround(1000 * std::cos(angle)), (int32_t)std::lround(1000 * std::sin(angle))});
            }
            starFailures += !check("star", {star}, true, false, false, true);
            starFailures += !check("star reversed", {reversed(star)}, true, false, false, true);
        }
    }
    // a square walked twice
    starFailures += !check("square twice", {{{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}, {10, 0}, {10, 10}, {0, 10}}}, true, false, false, true);
    printf("%-38s %s  %d failed\n", "star polygons, square walked twice", starFailures ? "FAIL" : "PASS", starFailures);

    // Combs: hundreds of reflex corners go through the bucket index.
    const int teeth = 120;
    std::vector<Pt> spikes;
    for (int i = 0; i < teeth; ++i) {
        spikes.push_back({i * 10, 0});
        spikes.push_back({i * 10 + 4, 0});
        spikes.push_back({i * 10 + 5, 60 + (i * 37) % 30});
        spikes.push_back({i * 10 + 6, 0});
    }
    spikes.push_back({teeth * 10, 0});
    spikes.push_back({teeth * 10, 100});
    spikes.push_back({0, 100});
    check("comb, 483 points, notches", {spikes}, true);
    std::vector<Pt> slots;
    for (int i = 0; i < teeth; ++i) {
        slots.push_back({i * 10, 0});
        slots.push_back({i * 10 + 5, 0});
        slots.push_back({i * 10 + 5, 90});
        slots.push_back({i * 10 + 8, 90});
        slots.push_back({i * 10 + 8, 0});
    }
    slots.push_back({teeth * 10, 0});
    slots.push_back({teeth * 10, 100});
    slots.push_back({0, 100});
    check("comb, 603 points, slots", {slots}, true);
    Rings holes = {scaled(square, 100)};
    for (int i = 0; i < 9; ++i)
        for (int j = 0; j < 9; ++j)
            holes.push_back({{i * 100 + 20, j * 100 + 20}, {i * 100 + 80, j * 100 + 30}, {i * 100 + 70, j * 100 + 80}, {i * 100 + 50, j * 100 + 50}});
    check("81 holes", holes, true);

    // World scale coordinates (~600 million).
    const int32_t big = 600000000;
    check(
        "world-scale square with hole",
        {scaled(square, big / 10, -big / 2, -big / 2), scaled({{3, 3}, {7, 3}, {7, 7}, {3, 7}}, big / 10, -big / 2, -big / 2)},
        true);
    check("world-scale L", {scaled({{0, 0}, {10, 0}, {10, 4}, {4, 4}, {4, 10}, {0, 10}}, big / 10)}, true);

    // Invalid input.
    check("bow tie", {{{0, 0}, {10, 10}, {10, 0}, {0, 10}}}, false);
    check("bow tie asymmetric", {{{0, 0}, {20, 20}, {20, 0}, {0, 10}}}, false);
    check("hole outside", {square, {{20, 20}, {30, 20}, {30, 30}}}, true, true);
    check("hole left of outer", {square, {{-30, 2}, {-20, 2}, {-25, 8}}}, true, true);
    check("hole outside, other hole kept", {scaled(square, 2), {{2, 2}, {8, 2}, {5, 8}}, {{30, 30}, {40, 30}, {35, 40}}}, true, true);

    // Degenerate and invalid inputs: a refusal hands them to the constrained triangulation, an acceptance must still cover
    // exactly
    check("single-point holes", {square, {{3, 3}}, {{6, 6}}, {{5, 2}}}, true, false, true, true);
    check("ring touching itself at a vertex", {{{0, 0}, {5, 0}, {5, 5}, {10, 5}, {10, 10}, {5, 10}, {5, 5}, {0, 5}}}, true, false, true, true);
    check("hole equal to the outer ring", {square, square}, true, false, true, true);
    check("hole along an outer edge", {square, {{0, 2}, {5, 2}, {5, 8}, {0, 8}}}, true, false, true, true);
    check("touching holes closing a loop",
        {scaled(square, 3),
            {{6, 3}, {12, 6}, {6, 9}, {3, 6}},
            {{12, 6}, {18, 3}, {24, 6}, {18, 9}},
            {{12, 6}, {18, 9}, {12, 12}, {9, 9}},
            {{12, 6}, {9, 9}, {6, 9}, {9, 3}}},
        true, false, true, true);
    // Valid inputs that tempt a wrong diagonal
    check("collinear tips", {{{0, 0}, {10, 0}, {10, 4}, {8, 2}, {6, 4}, {4, 2}, {2, 4}, {0, 2}}}, true);
    check("collinear valleys", {{{0, 0}, {12, 0}, {12, 6}, {10, 2}, {8, 6}, {6, 2}, {4, 6}, {2, 2}, {0, 6}}}, true);
    check("holes on one row", {scaled(square, 4), {{4, 10}, {8, 10}, {6, 14}}, {{14, 10}, {18, 10}, {16, 14}}, {{24, 10}, {28, 10}, {26, 14}}}, true);
    check("hole right of a reflex vertex",
        {{{0, 0}, {20, 0}, {20, 20}, {0, 20}, {0, 12}, {10, 10}, {0, 8}}, {{12, 9}, {16, 9}, {14, 11}}}, true);

    for (const auto &[name, rings] : parsePolygons(snapRounded))
        check(("snap rounded, " + name).c_str(), rings, true);
    for (const auto &[name, rings] : parsePolygons(edgeTouching))
        check(name.c_str(), rings, true, false, true, name.find("invalid") != std::string::npos);
    std::mt19937 gridRng(777);
    int gridFailures = 0;
    for (int i = 0; i < 2000; ++i)
        gridFailures += !check(("grid holes " + std::to_string(i)).c_str(), gridHoles(gridRng), true, false, false);
    printf("%-38s %s  2000 polygons, %d failed\n", "tiny grid-aligned holes", gridFailures ? "FAIL" : "PASS", gridFailures);

    // Random valid polygons at tile and at world scale.
    std::mt19937 rng(12345);
    int randomFailures = 0;
    const int randomCount = 3000;
    for (int i = 0; i < randomCount; ++i) {
        const Rings rings = randomPolygon(rng, i % 3 == 2 ? 40000 : 1);
        randomFailures += !check(("random " + std::to_string(i)).c_str(), rings, true, false, false);
    }
    printf("%-38s %s  %d polygons, %d failed\n", "random star polygons with holes", randomFailures ? "FAIL" : "PASS", randomCount, randomFailures);


    // Forced cuts (the default without the constrained triangulation): invalid rings still give a triangle per vertex
    // beyond two, never a refusal.
    {
        earstrip::Triangulator<uint32_t> t;
        t.setConstrained(false);
        int forcedFailures = 0;
        const Rings invalid[] = {
            {{{0, 0}, {10, 10}, {10, 0}, {0, 10}}},
            {{{0, 0}, {20, 20}, {20, 0}, {0, 10}}},
            {{{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}, {10, 0}, {10, 10}, {0, 10}}},
            {nearSpike},
        };
        for (const Rings &rings : invalid) {
            const auto s = t.run(rings);
            size_t triangles = 0;
            uint32_t begin = 0;
            for (uint32_t end : t.stripEnds()) {
                triangles += end - begin - 2;
                begin = end;
            }
            bool inRange = true;
            for (uint32_t i : t.strips())
                inRange &= i < rings[0].size();
            // a square walked twice is empty under the even-odd rule; the figure eights have lobes to fill
            const bool figureEight = &rings - invalid < 2;
            forcedFailures += s != earstrip::Status::Approximate && s != earstrip::Status::Exact;
            forcedFailures += !inRange || (figureEight && triangles == 0);
        }
        printf("%-38s %s  %d failed\n", "forced cuts on invalid rings", forcedFailures ? "FAIL" : "PASS", forcedFailures);
        failures += forcedFailures;
    }

    // Longitude and latitude: close doubles must not share hash buckets, or joining holes turns quadratic.
    {
        using P = std::array<double, 2>;
        std::vector<std::vector<P>> poly(2);
        const int n = 20000;
        for (int i = 0; i < n; ++i) {
            const double a = 2 * Pi * i / n;
            poly[0].push_back({21.0122 + 0.01 * std::cos(a), 52.2297 + 0.006 * std::sin(a)});
            poly[1].push_back({21.0122 + 0.004 * std::cos(-a), 52.2297 + 0.002 * std::sin(-a)});
        }
        earstrip::Triangulator<uint32_t> t;
        const auto start = std::chrono::steady_clock::now();
        const auto s = t.run(poly);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::vector<uint32_t> tris;
        t.triangles(tris);
        double area = 0, expected = 0;
        std::vector<P> all(poly[0]);
        all.insert(all.end(), poly[1].begin(), poly[1].end());
        for (size_t k = 0; k < tris.size(); k += 3) {
            const P a = all[tris[k]], b = all[tris[k + 1]], c = all[tris[k + 2]];
            area += ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) / 2;
        }
        for (int r = 0; r < 2; ++r) {
            double a = 0;
            for (size_t i = 0; i < poly[r].size(); ++i) {
                const P p = poly[r][i], q = poly[r][(i + 1) % poly[r].size()];
                a += p[0] * q[1] - q[0] * p[1];
            }
            expected += (r ? -1 : 1) * std::abs(a) / 2;
        }
        const bool good = s == earstrip::Status::Exact && std::abs(area - expected) <= 1e-6 * expected && tris.size() == 3 * (2 * n) && ms < 500;
        printf("%-38s %s  %.1f ms, area error %.2e\n", "40,000 lon/lat vertices with a hole", good ? "PASS" : "FAIL", ms, std::abs(area - expected) / expected);
        failures += !good;
    }

    // The flat interface gives the same strips as the nested one; uint16_t indices; pairs as points.
    {
        const double flat[] = {0, 0, 10, 0, 10, 10, 0, 10, 3, 3, 7, 3, 7, 7, 3, 7};
        const uint32_t holeStarts[] = {4};
        earstrip::Triangulator<uint32_t> a;
        a.run(flat, 8, 2, holeStarts, 1);
        const std::vector<uint32_t> fromFlat = a.strips();
        a.run(Rings{square, {{3, 3}, {7, 3}, {7, 7}, {3, 7}}});
        earstrip::Triangulator<uint16_t> b;
        b.run(std::vector<std::vector<std::pair<int, int>>>{{{0, 0}, {10, 0}, {10, 10}, {0, 10}}, {{3, 3}, {7, 3}, {7, 7}, {3, 7}}});
        const bool good = fromFlat == a.strips() && std::equal(b.strips().begin(), b.strips().end(), a.strips().begin(), a.strips().end());
        printf("%-38s %s\n", "flat input, uint16_t, pairs", good ? "PASS" : "FAIL");
        failures += !good;
    }

    // triangles() winds every triangle counter-clockwise, whichever way the input rings run.
    {
        std::mt19937 rng(99);
        int windingFailures = 0;
        earstrip::Triangulator<uint32_t> t;
        std::vector<uint32_t> tris;
        for (int i = 0; i < 200; ++i) {
            const Rings rings = randomPolygon(rng, 1);
            t.run(rings);
            t.triangles(tris);
            std::vector<Pt> all;
            for (const auto &r : rings)
                all.insert(all.end(), r.begin(), r.end());
            for (size_t k = 0; k < tris.size(); k += 3) {
                const Pt a = all[tris[k]], b = all[tris[k + 1]], c = all[tris[k + 2]];
                windingFailures += (int64_t)(b.x - a.x) * (c.y - a.y) - (int64_t)(b.y - a.y) * (c.x - a.x) <= 0;
            }
        }
        printf("%-38s %s  %d triangles wound the wrong way\n", "triangle list winding", windingFailures ? "FAIL" : "PASS", windingFailures);
        // list mode writes exactly what triangles() makes of the strips
        std::mt19937 rng2(99);
        earstrip::Triangulator<uint32_t> list;
        list.setTriangleList(true);
        int listFailures = 0;
        for (int i = 0; i < 500; ++i) {
            const Rings rings = randomPolygon(rng2, 1);
            t.run(rings);
            t.triangles(tris);
            list.run(rings);
            listFailures += tris != list.triangleList();
        }
        printf("%-38s %s  %d differ\n", "triangle list mode", listFailures ? "FAIL" : "PASS", listFailures);
        windingFailures += listFailures;
        failures += windingFailures != 0;
    }

    constrainedTests();

    printf("%s: %d failures\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
