# PBR Water

PBR Water (`features/PBR Water`, `src/Features/PBRWater*`) turns Skyrim's water into a physically based,
wind-driven surface: real wave geometry through GPU tessellation, PBR shading, bubbles, shoreline waves and
interactive ripples, with gameplay (swimming, floating objects) following the rendered waves.

It is a **Beta, Unreleased** feature (`PBRWater.ini`) and builds on Unified Water (water meshes, flowmaps
and the per-cell water cache) and, optionally, the Terrain Shadows heightmap (bathymetry).

## Architecture

```
main thread (Main::Update)                         render thread
--------------------------                         -------------
weather wind + swell -> OceanSpectrum (WaveModel)  Prepass: FFT ocean CS (spectrum, FFT, assemble, mips),
wave clock (double), CPU mirror frames (worker) -> ripple sim CS, fetch / shore field upload, frame
WaterEnvironment: fetch bake + bathymetry,           constants, swash CS (ShoreSimCS), t123 for Lighting
  shore field (worker)                              b7 PBRWaterData (cascades, fetch, terrain, shore, swash)
gather ripple sources, Havok buoyancy               BSWaterShader::SetupGeometry: per-draw constants,
TESObjectCELL::GetWaterHeight += wave height          HS/DS/GS binding for that draw's descriptor
TESWaterSystem underwater height = wave surface     BSWaterShader::RestoreGeometry: unbind, restore topology
```

### FFT ocean

The open water is an FFT ocean (Tessendorf 2001): four world-anchored, periodic cascades of 1000, 162, 26.3
and 4.27 m. Each carries one band of the spectrum, from 6 of its own lattice steps up to where the next
cascade starts, so every wavenumber belongs to exactly one cascade and the tile ratios (~6.17, not an
integer) never line up into a visible repeat. A 256^2 cascade holds tens of thousands of waves, against the
16 Gerstner waves this replaced, of which the largest carried 40-50 % of the energy and read as one regular
wave train.

-   **Spectrum** (`WaveModel.cpp`, mirrored in `OceanSpectrum.hlsli`): JONSWAP for the wind sea (fully
    developed Pierson-Moskowitz peak for the weather's wind) plus a narrow JONSWAP swell from distant storms
    (height, period and direction relative to the wind are settings and weather variables). Directional
    spreading is `cos^2s(theta / 2)` (Longuet-Higgins 1963) with the Mitsuyasu exponent, narrowest at the
    peak; the Directional Spread setting maps to the exponent at the peak.
-   **Synthesis** (`OceanSpectrumCS` -> `OceanFFTCS` rows -> columns -> `OceanAssembleCS`): each lattice
    point gets `h(k, t) = h0(k) e^(-i w t) + conj(h0(-k)) e^(+i w t)` with `h0 = xi * a(k)`, where `xi` is a
    standard complex Gaussian from a hash of the lattice index (no textures, and the CPU can reproduce it).
    Eight real fields (choppy displacement, height, shear, slopes, compression) are packed two per complex
    transform. The FFT is a radix-2 Stockham kernel in group shared memory (one group per line; 128, 256 or
    512 per the Wave Detail setting).
-   **Outputs** (texture arrays, one slice per cascade, full mip chains): displacement + shear, slopes +
    compression, and `(slope^2, trace^2 of the compression, |laplacian|)`. Last frame's displacement is kept for
    motion vectors. Foam is not made per cascade (see Foam).
-   **Time**: frequencies are quantised to multiples of `2 pi / 1024 s`, so the sea repeats exactly every
    1024 s and the GPU only ever sees time as a fraction of that loop. Wind changes rescale amplitudes on
    fixed random coefficients, so the sea changes continuously; pausing freezes it.
-   **Choppiness** is 1 for linear theory and is held back automatically so the surface folds over itself
    (Jacobian < 0) no more often than a 3-sigma event: the divergence of the horizontal displacement has a
    standard deviation of `lambda * sqrt(mss)`.

| Consumer               | Code                                            | Purpose                                                         |
| ---------------------- | ----------------------------------------------- | --------------------------------------------------------------- |
| Vertex / domain shader | `PBRWater::DisplaceSurface`                     | displacement, current and previous frame, mip by vertex spacing |
| Pixel shader           | `PBRWater::ShadeSurface`                        | filtered normals, LEAN roughness, breaking, aeration            |
| Hull shader            | `PBRWater::SurfaceCurvature`                    | curvature at the target triangle size                           |
| CPU                    | `WaveSnapshot::SampleAt`, `ParcelDisplacements` | water height for swimming, buoyancy and floating objects        |

### Gameplay without readback

Swimming, Havok buoyancy and the floating objects need the surface on the CPU. GPU readback would arrive a
couple of frames late, so instead `OceanMirror` re-synthesises the three largest cascades on the CPU from
the same coefficients (same hash, same spectrum code, same band boundaries): a 128^2 grid per cascade with
3.5+ samples per wavelength, three packed transforms (displacement and its velocity), computed on a worker
thread at 20 Hz one frame ahead and interpolated in time. Only the smallest cascade (waves under 0.71 m,
about 1 cm rms whatever the wind) is left out. Validated against a brute-force sum of every component (max
error 2.7 cm on 2.5 m waves, from the cubic interpolation) and against an emulation of the GPU shader
(identical to float precision). The coefficients are cached while the sea state holds still, so a frame
costs ~3 ms of the worker every 50 ms; the main thread only picks frames up.

The worker is never joined or destroyed: a thread killed by process exit cannot be joined from a DLL's
static destructors without risking a hang at game exit.

### Nothing is hard-coded per location

| Input                  | Source                                                                       | Effect                                                                                                                      |
| ---------------------- | ---------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| Wind speed / direction | `RE::Sky::windSpeed/windAngle` (blended across weather transitions)          | spectrum energy, peak period, direction                                                                                     |
| Fetch                  | baked from Unified Water's cell coverage, 16 directions (`WaterEnvironment`) | JONSWAP fetch-limited peak, energy and height per cascade: ponds and rivers keep only short chop, the open sea builds swell |
| Depth                  | Terrain Shadows heightmap and the loaded cells' terrain                      | shore field: refraction, shoaling (Green's law), depth-limited breaking (Hs <= 0.6 h), swash up the beach                   |
| Flow                   | Unified Water flowmap                                                        | calms waves on rivers                                                                                                       |
| Absorption             | the water form's vanilla shallow colour and visibility distance              | Beer-Lambert extinction per channel                                                                                         |

The fetch bake marches 16 upwind rays per cell, then stores the Shore Protection Manual _effective fetch_
(cos²-weighted over ±45° of upwind) with a floor of 40% of the cell's longest radial for swell from the open
sea. A single upwind ray zeroed the whole coastal sea whenever the wind blew offshore. Fetch-limited seas
also get the JONSWAP Phillips constant `0.076 (gF/U²)^-0.22` (amplitude gain up to 2x over open-sea PM);
the gain only raises the surface, not the horizontal displacement, so folding stays bounded.

A cascade's content is the same everywhere, so location changes weight whole cascades, at each cascade's
energy-weighted frequency and wavenumber: the fetch-limited Pierson-Moskowitz cut-off removes the long-wave
cascades from sheltered water (whose spectrum is the open sea's high-frequency tail, which is universal),
`tanh(k h)` calms them in the shallows. The CPU applies the same weights.

Per-weather overrides are registered with the weather variable registry (`RegisterWeatherVariables`):
wave height, choppiness, directional spread, swell height / period / direction, storm wind speed, foam,
visibility, subsurface, roughness.

## Shoreline

Near the shore the long FFT cascades hand over (`h < L0 / 2`) to Fluid Flux 2-style coastline waves (Imaginary Blend;
DaiZiLing's HLSL port of its `MF_SurfaceFlux_Coastline`, the Flux2-Shoreline-Wave-HLSL repository): one train of the
sea's dominant waves running up a coastline map that is traced over the real depths instead of baked. No per-shore
setup: everything follows from the sea state and the terrain. Four parts: a coastline map (CPU, how long the waves take
to reach the waterline from where), the train (GPU, what they look like), the swash (GPU, what they do on the beach)
and a per-pixel waterline (where the water ends).

-   **Coastline map** (`WaveModel::BuildShoreField`, rebuilt on a `WaterEnvironment` worker around the player:
    768^2 texels of 128 units, ~1.4 km across, from the loaded cells' exact terrain, else the Terrain Shadows
    heightmap, and each cell's water level from Unified Water's cache). Fluid Flux's coastline distance, as a travel
    time: `tau`, the time the waves take from a point to the nearest waterline, solves the eikonal `|grad tau| = 1 / c(h)`
    by fast sweeping (Zhao 2005) from the waterline outwards. So the crests run parallel to the shore, slow down and
    close up over the shoals, turn towards the depth contours and wrap into bays, and surf runs into every beach nearly
    head-on whatever the sea offshore does (traced from the open sea in its one direction instead, the fronts ran along
    the banks of an inlet and lee shores got none). The map also holds `grad tau` (the waves travel down it, at
    `1 / |grad tau|`), the signed distance to the waterline (to find the nearest point of the coast), the beach slope at
    the waterline, the exposure to the swell (open water upwind along its direction: lee shores see little of it) and
    the room for the train. Up the beach the direction, exposure and room of the waterline the inland ray leaves from
    are carried. The phase lag `omega tau` uses the frequency the map was traced for (`Shore4`): with the eased one,
    `tau` turned every change of the sea into `tau d(omega)/dt` of extra phase speed. A new map (the player moved ~175 m,
    the sea changed period, the swell turned) takes over by blending the two maps' lags over 4 s: both are traced
    from the waterline on the same texel grid, so for the same waves they agree and nothing moves; traced for a new
    period, the crests slide onto their new spacing over the blend.
-   **Exact terrain everywhere it was loaded** (`LandCache`, `WaterEnvironment::UpdateLoadedLand`): Skyrim
    streams only the 5 x 5 cells around the player, and beyond them the coarse Terrain Shadows heightmap is
    metres off. With only the loaded block exact, the depth had a straight cliff along its border (the waves'
    height, breaking and arrival times jumped along the line), and the shore field was rebuilt every time the
    camera crossed a cell. Every loaded cell's heights are now kept for the worldspace; the shaders and gameplay
    read an 11 x 11-cell window of them, the shore field reads all of them, and where exact data ends it is
    carried half a cell further and faded into the coarse heights (`FeatherBed`; `ExactTerrainShare` at the
    window's border). New cells refine the shore field at most every 10 s.
-   **Coherent fronts only**: where fronts from different stretches of shore meet (mid-channel, behind headlands and
    islands, bays reached from both sides) `tau` kinks, so crests met at sharp angles and the direction of travel
    flipped across a line. Across a kink the averaged slowness is shorter than `1 / c`, so `|<grad tau>| c` (averaged
    over ~3.5 m) measures how coherent the fronts are; the room fades the train where it drops below ~0.9 (Fluid Flux's
    coastline mask), and where no front arrived at all.
-   **Train** (`PBRWater::ShoreWaves`, mirrored by `WaveModel.cpp`'s `ShoreTrain` for gameplay), as Fluid Flux:
    -   the phase runs with time and the travel time to the shore, `theta = clock + omega tau`, offset along the coast
        (value noise at the nearest point of the waterline over ~4 and ~1.7 deep-water wavelengths, drifting slowly:
        Variation Along the Coast) and, from a few wavelengths offshore, across it (`MF_CoastlineOffsets`): each
        stretch of shore sees its crests at its own time, and the crests bend offshore;
    -   the wave's shape follows its phase and its life stage, the stage being the shoaled height against the
        breaking limit. Fluid Flux's canned profile animation is analytic here: linear (Airy) particle orbits in
        finite depth, horizontal semi-axis `a coth(kh)` (Dean & Dalrymple 1991), so the crests sharpen and the troughs
        flatten as the wave feels the bottom; the orbits tilt with the bed slope (Biesel 1952) and, nearing breaking,
        pitch forward, so the front steepens and the crest leans over it. The orbits are held to `k ax <= 0.7`, so
        the surface compresses on the crests but never folds over itself (Jacobian >= 0.3). Shoaling follows
        Green's law up to depth-limited breaking at `H = 0.78 h` (McCowan 1894); broken waves become the bore, a
        saw-tooth with a steep front (a fifth of the cycle) and a long back, held to the depth (Svendsen 2006);
    -   the horizontal motion follows the direction of travel, and offshore the waves hand over to the open sea.
    -   A random sea's energy is carried by regular waves of `Hrms = Hs / sqrt(2)`. The Iribarren number of the beach
        (Battjes 1974) gates breaking, surf and swash: waves spill and plunge on gentle beaches (`xi < 2`) and surge
        against rock faces and cliffs (`xi > 3.3`).
-   **Room for the train** (`ShoreField::room`, `t124`): a train of shoaling, breaking waves needs open water
    a few wavelengths across. Each water texel gets the width of the largest disc of water around it (twice
    the distance to the waterline at that disc's centre, propagated by raster sweeps) against the shore waves'
    wavelength in the local depth: `room = smoothstep(0.1, 0.6, width / L)`. It scales the train's share
    (`ctx.train`) and the beach factor (surf, swash, breaking foam), so streams, ditches and ponds narrower than
    a few waves get none: before, the train entered a stream from both banks (every up-wave bank is an entry
    seed), its fronts met at odd angles and the whole stream "broke". There the FFT sea, fetch- and
    depth-limited, carries what waves there are. Mirrored on the CPU (`WaveSnapshot` train weight and beach).
-   **Smooth fields**: the beach slope, the exposure and the room are copied along rays from the waterline and
    propagated texel by texel, so they jumped where rays from different stretches of shore met (and the room's
    disc sweep drew octagons); the travel times and directions carried up the beach jumped the same way. They
    gate the surf, swash and foam, which showed the seams as hard corners. All are smoothed after the build
    (`Blur` over ~4 m; `SmoothReached` on the carried times and directions).
-   **Wave groups** (`ShoreGroup`, mirrored on the CPU): the shore waves come in sets. Their height is
    scaled by 0.45-1.55 from a value noise over about three deep-water wavelengths of coast (at the waterline the
    waves run to) and over the time they reach it (now plus their travel time, so a set travels in with them),
    re-drawn every ~7 periods and cross-faded (groupiness: Longuet-Higgins 1984). Bigger waves shoal and break further out,
    run up further and leave their foam higher on their own, so no two stretches of beach keep the same
    rhythm. Shore Wave Height scales the sea arriving at the shore (the swell and the wind sea).
-   **Swash** (`PBRWater::SwashTip`): from the crests that reached the waterline of the ray through a point
    over the last six periods (a crest at every whole cycle of the train's phase there, each with its wave set's
    height), the tip runs up 1.6 times as
    far as Stockdon et al. (2006) give (`ShoreRunupScale`: their 2 % value is fitted to the mean water line,
    the visible sheet runs on past it) and moves ballistically, decelerating at `g sin^2 beta` (Shen & Meyer 1963; Peregrine &
    Williams 2001). The water over the beach is a thin sheet (`SwashTop`: 15 % of the height below the tip,
    at most 30 cm), never a raised mesh. Once a crest has drained the tip keeps falling below the still water
    rather than resting at it, so the vertices interpolate it linearly (a tip clamped at 0 put the swash front
    on the triangle edges). The swash mark (the highest a drained crest reached) goes to the pixels with the
    tip; up to it the mesh stays above the bed and both passes keep the pixels, and where the ground shows the
    colour pass draws the foam the swash left there (Foam).
-   **Waterline per pixel**: near the waterline the vertex shader holds the mesh just above the bed
    (`WaterlineLift`), so where the water ends is decided per pixel. Both passes first drop the pixels whose
    bed (terrain heights at the pixel) lies more than 5 cm above the interpolated waves and the swash sheet
    rebuilt over that bed (`AboveWaterline`), so the stencil pass (water mask, motion vectors) and the colour
    pass cover the same pixels; the vertices carry the waves without the sheet, as the sheet ends abruptly at its
    tip and cannot be interpolated. The colour pass then sets the water's surface at the pixel (the waves, or
    the swash sheet) against the ground seen through it (depth buffer), so the edge follows the rendered terrain
    and whatever stands in the water. The thinnest water fades into the ground over 6 cm, and dry pixels inside
    the mask output the ground (`RefractionTex`) instead of discarding: `ISWaterBlend` composites `kRAW_WATER`
    wherever the mask is set, and that target is not cleared, so a discard showed an older frame's water.
-   **Water mask and blend**: the stencil pass's mask (`VdotN` in the UNORM `kNORMAL_TAAMASK_SSRMASK.z`, also
    read by the upscaler) is kept at least 2/255, as wave faces seen edge-on rounded to 0 and punched 2x2
    holes in the water. `ISWaterBlend` skips its history blend under `PBR_WATER` (the engine's TAA water
    blending, `ToggleTAAWaterBlending`): unclamped and 60-80% beyond ~20 m, it dragged the travelling crests,
    foam and swash behind them.
-   **No foam on terrain or meshes**: the foam the backwash used to leave on the sand was drawn by Lighting on
    every upward-facing lit surface near the shoreline, a procedural splotch on the landscape and on objects;
    it was removed (`ShoreWetness.hlsli` is wetness and water height only, `ShoreSimCS` keeps only the wet
    level and the water surface).
-   **Whitewater needs real breakers** (`BreakerFoam`): breaking is decided by the height against the depth,
    so even centimetre ripples "break" in the last few centimetres of water, but only real breakers entrain air.
    The whitewater and the swash foam scale with the arriving significant height (a trace from 2 cm, full from
    40 cm), so a stream's wavelets make only a little.
-   **Wet shores** (`ShoreSimulation`, `ShoreSimCS.hlsl`, a camera-centred grid scrolled in whole texels):
    the highest water each point saw lately (the swash on beaches, the waves against rocks, piers and hulls),
    sinking slowly as it drains, and the water surface height. Lighting reads them through
    `t123` and `SharedData::pbrWaterSettings` (`PBRWater/ShoreWetness.hlsli`): Wetness Effects' shore wetness
    follows what the waves reached, and caustics and underwater wetness follow the moving surface.
-   Needs Depth-Aware Waves (bathymetry).

## Tessellation

Hull, domain and geometry shaders are first-class `ShaderCache` classes (`ShaderClass::Hull/Domain/Geometry`,
`GetHullShader()` etc.): compiled per descriptor, async, disk cached (`.hso/.dso/.gso`), hot-reloadable.

While a draw is tessellated the vertex shader only forwards the raw vertex (`PackControlPoint`); the domain
shader re-runs the complete vertex program (`WaterVertex`) on every generated vertex. All per-vertex
attributes (texcoords, fog, refraction coordinates, motion-vector positions) therefore come from the exact
same code path for every permutation.

### Why the old branches broke on STENCIL and with DLSS / frame generation

1. **One hull/domain shader for every permutation.** The old code compiled HS/DS once with a fixed
   descriptor and bound them to all water passes. The STENCIL pass (water mask + motion vectors) has a
   different `VS_OUTPUT` than the colour passes, so the stage signatures did not match. Now HS/DS/GS are
   compiled with the draw's own vertex descriptor (`state->modifiedVertexDescriptor`).
2. **Motion vectors described the flat plane.** The old domain shader interpolated `WorldPosition` /
   `PreviousWorldPosition` without displacing them, so DLSS / FSR / frame generation reprojected waves as if
   they were flat. Now both are displaced, the previous position with the previous frame's phases.
3. **Vanilla shaders during async compilation.** Until our vertex/pixel shaders for a descriptor are
   compiled the game binds its own, whose signatures do not match our HS/DS. Tessellation is only enabled
   for a draw once our VS and PS for that descriptor are live.
4. **Depth must match across passes.** The STENCIL and colour passes tessellate with identical inputs,
   edge factors are computed from canonically ordered endpoints, the domain shader sums corners in a
   canonical order, and the position chain is `precise`, so both passes rasterise the same depth and
   shared edges never crack.
5. **Topology.** The renderer only re-applies its cached topology when it changes, so the shadow state is
   kept at "triangle list, not dirty" while the device is switched to a 3-control-point patch list;
   `RestoreGeometry` switches it back before anything else draws.

Edge factors target an on-screen triangle size in pixels using the _internal_ render height (so they scale
with upscalers). Crests get more: the mean |laplacian| of the height (stored per cascade, mip-filtered so short
waves are not averaged away) at the scale of the target triangles, sharpened by the horizontal compression,
sets how fine an edge must be for a sub-pixel chord error. Edges drop to 1 where waves have faded out or the
sea is calm, and patches outside the (displacement-padded) frustum are culled. The domain shader reads each
cascade from the mip whose texels match the vertex spacing the tessellation aims for at that distance
(`TessellatedSpacing`, a function of position only, so neighbouring patches agree and shared edges never
crack), so waves the mesh cannot carry are averaged out instead of aliasing. The curvature term never asks for
triangles smaller than the target size: below about 8 x 8 pixels a triangle wastes the rasterizer's tiles and the
pixel shader's 2 x 2 quads (Fatahalian et al. 2010), more than the extra silhouette detail gains.

### Tessellation cost

The domain shader runs the whole vertex program on every generated vertex, in both the stencil and the colour pass,
so its cost is per generated vertex; the hull shader runs once per patch, and the patches are the water meshes' own
triangles. Measured with `d3dcompiler_47` (static instruction slots of the domain shader, colour / stencil pass, and
its texture operations):

| Change                                                                                                                            | Colour    | Stencil   |
| --------------------------------------------------------------------------------------------------------------------------------- | --------- | --------- |
| Before                                                                                                                            | 2815 (46) | 3001 (51) |
| The swash's wave groups: 3 value noises per evaluation instead of 12 (`ShoreGroupSlices`; the stencil pass evaluates it twice)    | -441      | -753      |
| The exact terrain and its gradient from one 4 x 4 block of texels (`BilinearLoadGrad`: 12 loads and 1 `GetDimensions`, not 20, 5) | -45 (-8)  | -46 (-8)  |
| Surface noise baked once per frame (`SurfaceNoiseCS`, t126): one bilinear lookup instead of ~30 value noises (433 slots)          | (433)     | -         |
| After                                                                                                                             | 2343 (39) | 2202 (43) |

The noise's direct evaluation stays as the fallback beyond the bake, so it still counts statically; near the camera,
where the tessellator puts its vertices, the colour pass runs 433 slots fewer than the table shows. The bake is a
128^2 grid around the camera with four texels across a cell of the noise's finest octave (the gusts' second octave,
half Gust Size; the sediment patches' third, a quarter of Patch Size): 5 m texels at the defaults, so it covers 630 m,
the tessellated water. Its bilinear lookup is within 0.7 % rms of the direct evaluation for the gusts and 0.2 % for
the sediment patches (at most 4 % of their range). It costs what 16k vertices did, so it pays for itself whenever
the tessellator generates more than that per frame. The terrain and swash changes give the same values as before (to
rounding).

What the research says, and what it means here:

-   **Small triangles are the expensive part.** Rasterizers walk 8 x 8 pixel tiles and shade 2 x 2 quads, so
    triangles much below 8 pixels a side waste both (Fatahalian et al., _Reducing Shading on GPUs using
    Quad-Fragment Merging_, SIGGRAPH 2010). Hence the target-size floor on the curvature term above.
-   **Moderate factors.** AMD's GCN guidance capped tessellation factors at 15 (Unity HDRP still applies it on GCN
    consoles, `MAX_TESSELLATION_FACTORS 15`); NVIDIA and AMD's GDC 2010 "Tessellation Performance" talk advises
    tessellating only where it improves the image, drawing untessellated where the factor would be ~1, and culling
    patches outside the frustum by setting their factors to 0 (~30% in their example). PBR Water does all of these
    except the cap.
-   **Per-vertex work belongs in the vertex shader.** Unity HDRP's tessellation computes every per-vertex attribute
    in the vertex shader and only interpolates them in the domain shader, which re-evaluates displacement alone; the
    vertex shader's results are shared through the post-transform cache, the hull shader's per-edge work is not.
-   **Shipped ocean renderers do not use hardware tessellation.** Crest renders a camera-centred LOD mesh
    (CDClipmaps: clipmaps with CDLOD's continuous morphing, Strugar 2010; Losasso & Hoppe 2004), GodotOceanWaves a
    camera-snapped clipmap mesh (329k vertices at high quality) whose vertex shader only samples the displacement
    cascades, the projected grid spaces vertices evenly on screen instead of in the world (Johanson 2004), and Unreal's
    water plugin draws instanced tiles from a quadtree with concentric LODs (`FWaterQuadTree`); Unreal 5 removed
    hardware tessellation altogether.

**The base meshes limit the tessellator.** The tessellator subdivides each mesh triangle uniformly at the density
of its densest edge, and caps that at 64 segments. Unified Water's distant tiles are a single quad (`WaterMesh.nif`: 4
vertices, scaled to 1-n cells); if the loaded cells' water is as coarse (Debug shows the tessellated draws' patch
count and vertex spacing each frame), every patch is half a cell, 29 x 58 m: the half under the camera is subdivided
as finely everywhere as its nearest edge needs, so most of its vertices land where they are not needed, and next to the
camera 64 segments are still 0.9 m apart. The bigger saving from here is structural, not in the shaders:

1. Give the tessellator small patches: a 16 x 16 grid per cell (3.6 m patches; the untessellated path gets waves too)
   with the edge factors computed per vertex in the vertex shader (once per vertex instead of twice per edge) and
   culled per patch, including patches under the terrain.
2. Or follow Crest and GodotOceanWaves: replace the cells' water with one camera-centred CDLOD mesh with a cheap
   vertex program, and no hardware tessellation.

## Shading

-   FFT normals sampled with trilinear / 8x anisotropic filtering over the pixel footprint. The slope
    variance the filter averages away is recovered from the mip-filtered second moments (LEAN mapping,
    Olano & Baker 2010; Bruneton et al. 2010) and added to the GGX roughness, so distant water keeps its
    sparkle energy without aliasing.
-   Exact dielectric Fresnel (IOR 1.333), with total internal reflection when seen from below.
-   GGX sun and point-light specular (including Light Limit Fix lights).
-   Water column: `refraction * T + bodyColour * (1 - T)`, `T = exp(-sigma * pathLength)` with the path
    length measured from the depth buffer to the _displaced_ surface. Reflections no longer vanish in
    shallow water.
-   Wave subsurface scattering (Atlas, GDC 2019).
-   Drifting sediment patches and wind gusts are tens of metres across: their noise (`TurbidityPatches`, `WindGust`)
    is baked once per frame around the camera (`SurfaceNoiseCS`), the vertices read it (`PBRWater::SurfaceNoise`,
    evaluating it themselves beyond the bake) and the pixels interpolate it.
-   **Foam** is made only where waves break, kept where the water carries it, and lit as a diffuse scatterer.
    One setting, Foam Amount (1 = as measured).
    -   _Whitecaps_ (`PBRWater::Whitecap`): Tessendorf (2001, sec. 4.6) shows that the choppy displacement
        folds the surface where the smallest eigenvalue of the Jacobian of `x -> x + D(x)` falls to zero; whitecaps
        are taken where it falls below a threshold (Tessendorf, Reinhard & Gao 2023, "Whitecap Phenomenology for
        Ocean Surface Simulation"). The Jacobian is that of the _summed_ random sea that whitens (`WaveSums::foam`:
        the cascades longer than 0.7 m - the smallest only breaks on the microscale, without entraining air), so the
        foam lands on the sharpest crests of the surface as displaced, and only high on a crest (`foamCrest`: the
        height over its local standard deviation, from 0.8 to 1.8, the shore train's height included): short waves
        break on the crests of the long ones (Banner & Phillips 1974), so whitecaps sit on the tops of the waves
        instead of on every crest of the chop. Each cascade used to whiten on its own crests
        against its own threshold, which scattered flecks of the short bands over the whole sea (troughs included)
        and missed the visible crests. The wind sets the threshold as in their Algorithm 1 (`ComputeWhitecaps`):
        Monahan & O'Muircheartaigh's (1980) whitecap coverage `W = 3.84e-6 U^3.41` (times Foam Amount) is the crests
        breaking now plus the foam they leave, so `W (1 - g) / (1 - g W)` (`g = e^-0.15`) breaks at any instant, and
        the threshold is the eigenvalue below which the sea breaks that much on its crests. The Jacobian's entries
        and the height are jointly Gaussian with a covariance from the directional spectrum (`AddDeformation`: a wave
        `a cos(psi)` deforms by `-k (cos^2, sin^2, cos sin) a cos(psi)` of its direction - crests compress, so height
        and trace correlate at about -0.55 - spread by the analytic Fourier moments of the `cos^2s` spreading); the
        eigenvalue is not Gaussian, so the threshold is the crest-weighted quantile of 8192 fixed draws
        (deterministic, smooth in the sea state, recomputed only when it moves; tail extrapolated in normal scores
        below 8 weighted samples). Monahan's `W` is a function
        of the local wind, not of how big the waves have grown, so the shaders apply the threshold as a deficit
        `1 - lambda_min` in units of the _local_ spread of the random sea's Jacobian trace (the cascades' open-sea
        spreads `Cascade1.w` times their local weights): a fetch-limited bay, or the shoals where the shore train
        carries the long waves, whitecap like the open sea under the same wind; the chop breaks on the train's
        crests through the crest weight. The train's compression is not in the whitecap Jacobian: its regular crests
        are no random sea, and against the random sea's small spread over the shoals they whitened every crest of the
        surf zone (its own breaking is its whitewater). One global eigenvalue threshold left the coastal sea almost
        without whitecaps. Whitecaps need
        waves of a few tens of centimetres (Beaufort 3), so a stream's or pond's wavelets never whiten
        (`foamGate`).
    -   _Prefiltered_ (`PBRWater::Whitecap`; Dupuy & Bruneton 2012): `OceanAssembleCS` stores the squared trace of
        each cascade's compression (`OceanSurface.z`), so its mips keep the variance a pixel or a simulation texel
        averages away (minus the filtered trace squared, as LEAN does for the slopes). The source is the share of
        the footprint past the threshold: the eigenvalue against the threshold feathered by a quarter of its
        spread where the crests are resolved, and the Gaussian share from the averaged variance where they are not,
        with the threshold moving to the one that gives a fully averaged footprint exactly the breaking share
        (`averagedDeficit`). Without it the distant sea and the simulation's coarse level (which filter the short
        cascades away) whitecapped a fraction of the measured coverage.
    -   _Whitewater_ (`ShoreWaves`): the rollers of the breaking shore waves at the same criterion. At the Miche
        (1944) limit `H / L = 0.142 tanh(kh)` the horizontal semi-axis of a wave's Airy orbits reaches
        `k ax = 0.142 pi` at any depth, i.e. the orbit's Jacobian falls to `1 - 0.142 pi = 0.55` at the crest. The
        free orbit of the shoaled wave (beta times the limiting one) falls below that wherever beta times its
        compression profile exceeds 1, on the top of the wave only (above about half its amplitude: the roller rides
        the crest and the upper front; the face below and the trough stay water), a little thicker the further over
        the limit. The rollers once spread down the whole front and lasted, which washed the shore waves white.
    -   _Swash_ (`PBRWater::SwashFoam`): the uprush carries the bores' foam in a band a few metres behind its tip.
    -   _Kept_ (`FoamSimulation`, `FoamSimCS.hlsl`, t121): the whitecap opacity map of Tessendorf et al. (2023),
        stepped like Crest's foam simulation (`UpdateFoam.compute`): sources are written by maximum and everything
        decays with Callaghan, Deane & Stokes' (2012) measured whitecap foam decay (e-folding 2 s, within their
        1.4-4.8 s), so a crest's foam trails it for a short way and does not litter the sea; the coverage is the
        thickness clamped at 1, so the thicker foam of the bores lasts a little longer.
        Freshness (y) decays within a second (a crest breaks actively for about that long). The grid is indexed by
        the undisplaced (Lagrangian) position the shaders sample at, so the foam stays with the water it lies on
        while the crests run on: a breaking crest leaves a trail, and foam lingers at the water's edge after a wave
        and on the sand the swash drained from. Two 512^2 levels (fine: a quarter of the coarse texel; coarse:
        twice the displacement fade end across), scrolled with the camera in whole texels (never resampled); what
        scrolls into the fine level comes from the coarse one. Sources are only written where the water's surface
        or the swash sheet covers the bed. The water shader adds what breaks at its pixel, so crest lines are as
        sharp as the pixels.
    -   _Pattern_: the coverage is drawn with the "black point fade" of Crest (wave-harmonic/crest, MIT; also Sea of
        Thieves): the foam texture is thresholded at `1 - coverage`, so as coverage drops the bubbles in the
        foam grow and merge, and dense foam thins into lace and then specks instead of fading uniformly
        (`PBRWater/Foam.hlsli`). The texture is baked at start-up on a worker (`FoamPattern.cpp`, t125, 512^2 RG8
        with mips): round bubbles of random size on a jittered grid (20 cm, with 2.7 times smaller ones in the walls),
        merged with a smooth minimum, walls bent by a domain warp, density varied by clumps, every value
        histogram-equalised so a coverage `C` whitens exactly a share `C` and the mips converge to it far away. It is
        sampled at the Lagrangian position the foam is kept at and churned on the water (not on the sand) by Fluid
        Flux's three-phase flow advection (`MF_AdvectionData`: three copies slide 30 cm downwind and back every five
        seconds, each fading out before it wraps). Its G channel holds the bubble specks under the surface (Crest's
        bubble layer), seen with parallax 30 cm down, from the fresh foam and the wakes' churn.
    -   _Lighting_: foam is a Lambertian scatterer (Dupuy & Bruneton 2012 after Koepke 1984), lit by the sun (with
        its shadow) and the sky (with Skylighting) like any surface, on the wave normal bumped by its own relief.
        Reflectance (`PBRWaterFoam::Albedo`): fresh dense foam ~55 % in the visible (Whitlock, Bartlett & Gurganus
        1982), a little less towards the red (Frouin, Schwindling & Deschamps 1996), 60 % of that once aged; with
        the thinning coverage that averages near Koepke's 22 % over a whitecap's life. Foam scatters the sun glint
        with the rest of the light.
    -   Nothing is drawn from what the depth buffer shows through the water (that put foam in the shape of whatever
        stood in it), and there is no foam on calm water, along calm shores or on terrain and meshes.
-   The vanilla normal maps keep most of their strength over the FFT waves (0.75-1.3 by local wind), so
    the flowmap to open-water transition and the capillary texture stay visible.
-   **Wave shadows through Skylighting**: the displaced water surface is drawn into Skylighting's sky
    occlusion map each frame (`DrawSkylightingOccluder`, `SkylightingOccluderVS.hlsl`: one depth-only draw of
    a 177^2-vertex grid over the cells' still water, displaced like the mesh, just before Skylighting updates
    its probes), so the waves occlude the sky like any other geometry. The probes average it over many frames
    and directions: a point low in a trough sees less sky past the crests around it than a crest does. The
    water reads Skylighting one probe cell above its own displaced surface, for its ambient (with the wave
    facet's normal) and its sky reflection. Anything under the still water reads Skylighting just above the
    surface instead of from under the occluder (`Skylighting::LiftOutOfWater`, switched on by
    `SharedData::pbrWaterSettings`), since its sky light comes in through the water and the water shaders
    attenuate it. Nothing is drawn while the water is calm.
-   **Ripples** (`RippleSimulation`, `RippleSimCS.hlsl`): a 512^2 camera-centred heightfield integrated with the
    2D wave equation at a fixed 60 Hz, interpolated between steps to the render time. A copy of the
    previous frame's state provides correct motion vectors for the ripples. Every actor collision shape (each leg, the torso, ...), and loose Havok object
    crossing the surface acts as a moving constraint, which produces bow waves, wakes and rings.
    It replaces the vanilla wading displacement mesh, which is disabled because it is a second surface
    that cannot follow the waves.
-   **Ripples ride the waves**: the simulation lives in the water's rest (Lagrangian) coordinates. A body is
    placed where the water under it rests (`WaveSnapshot::Sample::originX/Y`, the inverse of the horizontal
    displacement) and the shaders sample at undisplaced positions, so rings, wakes and foam stay attached to
    the body and move with the waves. Previously they were injected at world positions but sampled at rest
    positions, so in a choppy sea they sat up to a metre off the swimmer and sloshed back and forth. Motion
    is measured relative to the water too: a body bobbing with the waves makes no ripples.
-   **Wake and splash churn**: the simulation's foam channel is a tracer (half-life 3 s), carried by the
    surface drift (semi-Lagrangian) and spreading slowly. Bodies moving through the water churn it along their
    wake; a body hitting the water fast from above (jumping or falling in) digs a crater and churns a burst of
    it; ripples that get steep enough break. Both scale with Foam Amount. The shading shows it as aerated water
    (bubble specks), and no longer gates it to ripple crests, which made it flicker in and out as rings passed.
-   **Swimming**: `TESObjectCELL::GetWaterHeight` returns the wave surface, so swimming actors ride waves.
-   **Underwater view** (Underwater Follows Waves): the game's camera-underwater state (`TESWaterSystem`
    underwater height and flag) is set from the displaced surface above the camera each frame, on the main
    thread and again before the world is drawn, so the underwater view and the waterline across the lens
    start where the camera dips under the rendered waves. Only within the waves' reach of the surface.
-   **Buoyancy**: dynamic Havok bodies floating at the surface get a spring towards the displaced surface
    plus the orbital velocity of the waves.

## Floating objects

Boats, ships, rafts and ice floes ride the waves (`FloatingObjects`). It builds on Bobbing Framework
(RavenKZP, GPL-3.0-or-later WITH Modding Exception), which moves a configured reference's 3D on a sine
with keyframed collision, carries its child references and the actors standing on it. Here nothing is
configured: no JSON, no patched meshes, no plugin records.

-   **Detection** (a scan every second, or after moving 2048 units, within the range): a static, movable
    static, activator or furniture reference, placed upright through the flat water plane with at least a
    couple of units below and above it, mostly above water (draft at most 80 % of its height), not taller
    than three times its length, between 0.3 m and the size limit long, drawn with lit materials (waterfalls,
    foam and fog are effect meshes), without dynamic Havok bodies (those already float through buoyancy),
    not a rock, boulder or cliff model, and not resting on the bottom. The bed comes from the exact terrain
    of the loaded cells (else the heightmap, else `TES::GetLandHeight` queried from above the hull, never
    from its keel, which is under the ground for a sunk rock). A hull may graze the bed of a shallow
    harbour: it stays put only if the terrain rises into it, if it is mostly under water and touching the
    bed, or if most of its footprint lies in the last 15 cm of water; a Havok ray straight down from the
    keel that hits a structure (pier, post, wreck) holds it too. The pier test (a pile or grounded section
    inside the footprint) only applies to low platforms, so a moored ship whose box takes in the dock's
    piles still floats. Markers and water are skipped. Select a reference in the console to see in the
    Debug section why it floats or stays put.
-   **Riders**: fish and other critters, plants and debris at or just under the surface move with the
    water's orbital motion, which fades as `e^(-k z)` below the surface for the local dominant waves, and
    are pushed down when a trough would leave them in the air (never into the bed). The motion is added to
    the reference's own position each frame, so critters keep their paths.
-   **Hull parts**: candidates are sorted by waterplane area; a smaller one whose centre lies inside a larger
    one's footprint becomes part of it (hull halves, decks placed as separate references). Ice floes that
    only touch stay separate.
-   **Attachments**: any other reference (statics, containers, lights, doors, flora, activators) inside the
    hull's footprint and height range rides it, unless what lies directly below it, down to the keel, is not
    this hull: so masts, sails and lanterns go with the ship, a dock reaching over it does not.
-   **Motion**: five water parcels (the centre and both ends of each axis, at 70 % of the half extents) are
    evaluated with the same wave function as the renderer (`WaveSnapshot::ParcelDisplacements`). The mean
    vertical displacement drives heave, a plane through them pitch and roll, and their mean horizontal
    displacement surge and sway, so a long hull averages out short waves and a raft follows every one.
    Heave, pitch and roll respond as damped oscillators with the hull's own natural frequency,
    `omega = sqrt(g / draft)` (a box hull), tilt at 0.75 of that, so small boats bob quickly and ships
    roll slowly. Tilt is limited to 0.35 rad. The motion fades out with the wave geometry's distance fade
    and towards the edge of the range.
-   **Collision**: near the player (45 m plus the hull's half length) the hull's and its parts' collision
    becomes keyframed through `NiAVObject::SetMotionType`, so it follows the moving 3D; loose props on board
    are woken to react to the moving deck.
-   **Cargo**: each scan lists the loose things lying on the hulls near the player: havok clutter (settled
    or not), dropped items and dead bodies. Every frame each one on a deck is moved with it - all of its
    non-fixed rigid bodies (a ragdoll has many) and, where havok does not drive it, the reference - since the
    deck moves by setting its 3D, which never pushes what lies on it.
-   **Actors on board**: the player and nearby high-process actors standing on a moving hull are carried
    with the deck (Bobbing Framework's method: `Actor::SetPosition` without the controller, then
    `bhkCharacterController::SetPositionImpl`, restoring the controller's forward vector and collector
    early-out distance, and an in-air state from the deck dropping away reset to on-ground). Jumping and
    swimming actors are left alone. Their weight sinks and tilts the hull from its waterplane area and
    inertia (80 kg per actor, scaled), which is noticeable on a rowing boat and nothing on a ship.
-   **Nothing is saved**: references keep their placed position and form data; collision changes go through
    the 3D only (not `TESObjectREFR::SetMotionType`, which marks the reference Havok-moved for the save);
    every object is put back in place when the game saves (`Feature::SavingGame`, from SKSE's save message)
    and moves on the next frame. A reference that unloads or gets new 3D is dropped and found again.
-   **Bobbing Framework installed**: forms it animates (its JSON configs) are left to it.

## Debugging

-   Settings > PBR Water > Debug: wireframe overlay / wireframe only, and debug views (shore field: train share / room / exposure, wave direction, normals, foam,
    depth/shore, foam Jacobian (red its compression, green the crests breaking now, blue the shore waves' whitewater),
    foam simulation (red coverage, green freshness, blue thicker than covering), roughness, wave height, fetch), plus
    live sea-state values (whitecap coverage, the share breaking now and its eigenvalue threshold) and whether
    the GPU ocean and the gameplay mirror are running, and last frame's water draws: how many were tessellated, with
    how many patches (the meshes' triangles) and what vertex spacing.
-   Every resource is named for RenderDoc (`PBRWater::*`).
-   The GPU work is wrapped in perf events (`PBR Water - Simulation`, `- Ocean FFT`, `- Ripples`, `- Surface Noise`, `- Swash`, `- Foam`,
    `- Skylighting Occluder`, `- Draw` per water draw), so RenderDoc, Nsight and the devbench
    `communityshaders.nvperf` ranges time each pass on its own.
-   Shader changes can be A/B timed on one captured frame: swap the water VS/PS in a RenderDoc replay and read
    `gpu__time_duration` and `smsp__inst_executed` for the draw. RenderDoc exposes the NVIDIA counters once
    `nvperf_grfx_host.dll` (shipped in `SKSE/Plugins/CommunityShaders/NvPerf`) sits in its `plugins/nv` folder.
    The water pixel shader is issue-bound (about two thirds of it on the FMA pipe), so the instruction count is
    the figure to cut; it is also latency-sensitive, and branching arithmetic away from between texture fetches
    can make it slower despite fewer instructions.

## Validation outside the game

The shaders are validated with the real `d3dcompiler_47` against every recorded Water permutation in
`.github/configs/shader-validation.yaml`, for all five stages (VS, HS, DS, GS, PS) with `PBR_WATER`
defined, plus `RippleSimCS.hlsl` and the ocean compute shaders (`OceanSpectrumCS`, `OceanAssembleCS`,
`OceanFFTCS` for 128, 256 and 512, rows and columns). With `PBR_WATER` undefined the restructured
`Water.hlsl` compiles to bit-identical bytecode to `dev` for all permutations.

The ocean model is checked natively (it has no game dependencies): the CPU FFT against a brute-force sum of
every component, height variance per cascade against the spectrum's, Hs against JONSWAP, velocities against
finite differences, the horizontal inversion, and a numpy emulation of `OceanSpectrumCS` + inverse FFT
against the CPU mirror, including slopes and compression against finite differences of the heights.

## In-game test checklist

-   [ ] Wireframe on the sea near Solitude / Dawnstar: tessellation density follows distance, no cracks.
-   [ ] Toggle DLSS / FSR and frame generation: waves stay sharp, no ghosting trails on crests.
-   [ ] Underwater (looking up): surface, Snell's window and the water mask line up with the displaced waves.
-   [ ] Weather change clear -> storm on the coast: sea builds smoothly, no popping.
-   [ ] Lake Ilinalta vs a small pond vs the White River: wave size follows fetch and flow. The stream below
        Whiterun: no shore waves, no breaking or white lines along its banks.
-   [ ] Wade into the sea: ripples from each leg, wakes, bubbles; no white foam on the legs; no vanilla wading mesh.
-   [ ] Swim in waves: player bobs with the visible surface. Drop a basket in the sea: it rides the waves.
-   [ ] Shader cache cold start (async compile): water renders untessellated until compiled, then switches.
-   [ ] Sea of Ghosts in a storm: ice floes heave and tilt with the waves; big floes move slowly, small ones
        follow every wave. Docks, piers and rocks in the water stay still.
-   [ ] Solitude and Windhelm harbours: docked ships roll slowly with masts and rigging attached; walk on
        deck and stay on it; save, reload: everything is where it was placed.
-   [ ] Rowing boats on Lake Ilinalta / Riverwood: boats bob, stepping in tilts them; Debug shows the
        hull, attachment and on-board counts.
-   [ ] Open sea at each Wave Detail: no visible tile repetition from a cliff top; close-up ripples sharpen
        with the higher settings; Debug shows the FFT and gameplay mirror running.
-   [ ] Swell Height 2 m in calm weather: long, regular swell crossing at the Swell Direction; none in ponds.
-   [ ] Swim in a choppy sea: rings and bubbles stay on the swimmer, no ripples while floating still.
-   [ ] Jump off a dock: crater, ring and a burst of bubbles that drifts and fades over a few seconds.
-   [ ] Open sea in a storm vs a fresh breeze: whitecaps on the sharpest displaced crests (Debug: Foam Jacobian green
        on the crests, never in the troughs), thinning into trails behind them over a few seconds, more of them the
        stronger the wind; none in calm weather or on ponds; Debug shows the whitecap share and threshold.
-   [ ] Beach near Dawnstar / the Solitude coast in a fresh breeze: waves shorten, turn to the shore and
        break with foam on the crest and front face of each breaking wave, dense on the bores of the inner surf zone,
        trailing behind them and riding the front of the swash; lace lingers on the water's edge and the drained sand
        after each wave and thins out before the next; underwater, no white line along the shore; the sand is wet up
        to where the water reached and dries slowly. Rock outcrops and cliffs get no surf or swash, only the open-sea
        waves and wet rock up to the crest height.
-   [ ] Choppy sea with Skylighting: troughs are darker than crests (sky light and reflection); shallow
        seabeds and wading legs are not darkened by the water above them.
-   [ ] Sunset over a swell: the troughs behind the crests fall into shadow and lose the sun glint; at midday
        nothing is shadowed.
-   [ ] Swim at the surface in waves: the underwater view starts when the camera dips under a crest.

## Known limitations

-   Shoreline waves and depth attenuation need the bathymetry (loaded cells or a Terrain Shadows heightmap).
-   Only the dominant sea is refracted: the train is traced for its period, the shorter FFT bands near the shore are
    only shoaled and depth-limited. The map follows the player every ~175 m or a change of the dominant period or the
    swell's direction (blended over 4 s). The shore waves are one regular train in sets, not a random sea: Fluid
    Flux's offsets vary their timing along the coast, but every crest of a set has the same height. The wet level is resolved
    at the swash grid's ~0.9 m; the swash itself is evaluated per vertex and per pixel.
-   Gameplay water heights include the shore wave train but not the swash: swimming and buoyancy see the
    shoaled waves, not the run-up's thin sheet.
-   The waves occlude Skylighting where the cells' own water is (sea, lakes); separate river meshes are not
    drawn into its occlusion map.
-   The underwater view switches on the surface height above the camera; the lens line across the screen is
    still the game's flat line at that height, not the wave profile.
-   Fetch and depth weight whole cascades, so the transition from a sheltered bay to the open sea happens
    in four steps of band, not per wave. Shallow water calms the waves but cannot slow them (one dispersion
    relation per tile).
-   Gameplay leaves out the smallest cascade (waves under 0.71 m, ~1 cm rms, up to twice that in sheltered
    water where it carries more of the energy).
-   Gameplay water heights do not include the flowmap damping on rivers (fetch already keeps river waves small).
-   Seams between Unified Water tiles of different LOD sizes rely on the displacement distance fade. The
    displacement also fades out over the last 2048 units inside the loaded cells (`LoadedGridFade`): the
    flat LOD water beyond them meets the cells' water in T-junctions, and any wave left on that border
    opened holes.
-   Floating objects are found by heuristics: an unusual structure standing in deep water without touching
    the bottom can float (lower the Largest Hull setting), and a ship whose mesh reaches into a shallow bed
    stays still. Only loaded references move; distant LOD models do not. NPC navmesh on deck does not move,
    so NPCs keep their path while the deck carries them. Only the eight hulls nearest the camera are masked
    out of the water surface.

## References and credits

-   Tessendorf, _Simulating Ocean Water_ (2001) - FFT synthesis, choppy waves, frequency quantisation
-   Horvath, _Empirical Directional Wave Spectra for Computer Graphics_ (DigiPro 2015) - spectra and spreading
-   Mitsuyasu et al. (1975), Longuet-Higgins et al. (1963) - directional spreading
-   Bruneton, Neyret & Holzschuch, _Real-time Realistic Ocean Lighting using Seamless Transitions from
    Geometry to BRDF_ (2010) - filtered slope variance
-   Mihelich & Tcheblokov, _Wakes, Explosions and Lighting: Interactive Water Simulation in Atlas_ (GDC 2019) - FFT cascades, foam accumulation
-   GodotOceanWaves (2Retr0, MIT) - Jacobian whitecap foam: growth below a threshold, exponential decay, summed over cascades
-   Ocean-URP (gasgiant, MIT) - per-cascade foam weights
-   Fluid Flux 2 (Imaginary Blend) and DaiZiLing's Flux2-Shoreline-Wave-HLSL port of its coastline waves - coastline
    map, coastline offsets, wave profile by phase and life stage, three-phase foam advection
-   Unity HDRP water system - CPU re-simulation of the FFT for script queries instead of readback
-   Hasselmann et al. (1973) JONSWAP; Pierson & Moskowitz (1964); Monahan & O'Muircheartaigh (1980) whitecaps
-   Olano & Baker, _LEAN Mapping_ (2010)
-   Ang, _The Technical Art of Sea of Thieves_ (SIGGRAPH 2018 Talks) - foam and scattering breakdown
-   Fatahalian et al., _Reducing Shading on GPUs using Quad-Fragment Merging_ (SIGGRAPH 2010) - small-triangle cost
-   NVIDIA & AMD, _Tessellation Performance_ (GDC 2010); Cantlay, _DirectX 11 Terrain Tessellation_ (NVIDIA 2011) -
    factor budgets, patch culling
-   Unity HDRP `TessellationShare.hlsl` - the GCN factor cap, per-vertex attributes interpolated in the domain shader
-   Johanson, _Real-time Water Rendering: Introducing the Projected Grid Concept_ (Lund 2004); Strugar, _CDLOD_
    (2010); Losasso & Hoppe, _Geometry Clipmaps_ (SIGGRAPH 2004) - ocean mesh LOD without hardware tessellation
-   Crest Ocean System (wave-harmonic/crest, MIT) - foam texture black-point fade, saturated foam growth, bubble layer
-   Dave Hoskins, _Hash without Sine_ (MIT) - foam noise hashes
-   Atlas (GDC 2019, above) - subsurface term
-   Earlier PBR Water / Gerstner branches by davo0411 - tessellation hook points and the wading-mesh replacement
-   Zhao, _A Fast Sweeping Method for Eikonal Equations_ (Math. Comp. 2005) - shore wave rays
-   Eckart (1952) - shallow-water dispersion approximation; Green's law shoaling
-   Dean & Dalrymple, _Water Wave Mechanics for Engineers and Scientists_ (1991) - finite-depth particle orbits
-   Biesel (1952) - wave orbits over a sloping bed; McCowan (1894) - breaking index; Svendsen, _Introduction to
    Nearshore Hydrodynamics_ (2006) - surf-zone bores
-   Thornton & Guza, _Transformation of Wave Height Distribution_ (JGR 1983) - depth-limited breaking
-   Battjes, _Surf Similarity_ (ICCE 1974) - Iribarren number, breaking vs surging shores
-   Stockdon, Holman, Howd & Sallenger, _Empirical Parameterization of Setup, Swash, and Runup_ (Coastal
    Engineering 2006) - run-up
-   Shen & Meyer (1963); Peregrine & Williams, _Swash Overtopping a Truncated Plane Beach_ (JFM 2001) -
    ballistic swash and the swash lens
-   Whitlock, Bartlett & Gurganus, _Sea Foam Reflectance_ (GRL 1982) - foam albedo
-   Bobbing Framework (RavenKZP, GPL-3.0-or-later WITH Modding Exception) - moving placed references with
    keyframed collision, their children and the actors standing on them; the floating objects build on it
-   Faltinsen, _Sea Loads on Ships and Offshore Structures_ (1990) - heave and roll natural periods of a hull
