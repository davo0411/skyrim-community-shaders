# PBR Water

PBR Water (`features/PBR Water`, `src/Features/PBRWater*`) turns Skyrim's water into a physically based,
wind-driven surface: real wave geometry through GPU tessellation, PBR shading, foam, shoreline waves and
interactive ripples, with gameplay (swimming, floating objects) following the rendered waves.

It is a **Beta, Unreleased** feature (`PBRWater.ini`) and builds on Unified Water (water meshes, flowmaps
and the per-cell water cache) and, optionally, the Terrain Shadows heightmap (bathymetry).

## Architecture

```
main thread (Main::Update)                         render thread
--------------------------                         -------------
weather wind + swell -> OceanSpectrum (WaveModel)  Prepass: FFT ocean CS (spectrum, FFT, assemble, mips),
wave clock (double), CPU mirror frames (worker) -> ripple sim CS, fetch upload, frame constants
WaterEnvironment: fetch bake + bathymetry           b7 PBRWaterData (cascades, fetch, terrain, ripples)
gather ripple sources, Havok buoyancy               BSWaterShader::SetupGeometry: per-draw constants,
TESObjectCELL::GetWaterHeight += wave height          HS/DS/GS binding for that draw's descriptor
                                                    BSWaterShader::RestoreGeometry: unbind, restore topology
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
    compression, and `(slope^2, crest foam trail, |laplacian|)`. Last frame's displacement is kept for
    motion vectors.
-   **Time**: frequencies are quantised to multiples of `2 pi / 1024 s`, so the sea repeats exactly every
    1024 s and the GPU only ever sees time as a fraction of that loop. Wind changes rescale amplitudes on
    fixed random coefficients, so the sea changes continuously; pausing freezes it.
-   **Choppiness** is 1 for linear theory and is held back automatically so the surface folds over itself
    (Jacobian < 0) no more often than a 3-sigma event: the divergence of the horizontal displacement has a
    standard deviation of `lambda * sqrt(mss)`.

| Consumer               | Code                                            | Purpose                                                         |
| ---------------------- | ----------------------------------------------- | --------------------------------------------------------------- |
| Vertex / domain shader | `PBRWater::DisplaceSurface`                     | displacement, current and previous frame, mip by vertex spacing |
| Pixel shader           | `PBRWater::ShadeSurface`                        | filtered normals, LEAN roughness, crest foam and trails         |
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
| Depth                  | Terrain Shadows heightmap and the loaded cells' terrain                      | `tanh(kh)` depth attenuation per cascade, shoreline waves (Green's law shoaling, breaking at H = 0.78 h)                    |
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
wave height, choppiness, directional spread, swell height / period / direction, storm wind speed, shore
waves, foam, visibility, subsurface, roughness.

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
crack), so waves the mesh cannot carry are averaged out instead of aliasing.

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
-   Foam is a _coverage_ field turned into a pattern by a soft threshold (the approach used by Crest and
    Sea of Thieves): as coverage drops, the bubbles in the foam grow and merge, so dense foam thins into
    lace and then specks instead of fading uniformly. Coverage sources:
    -   contact: an exponential band where the water meets anything (shore, rocks, piers, legs), measured
        from the depth buffer and surging as each shore wave runs up;
    -   crests: surface folding (the Jacobian of the summed cascades) scaled by the Monahan whitecap
        coverage for the wind, plus a trail: each cascade accumulates foam in its own tile (so it rides the
        waves it came from), thinning over the Crest Foam Trail time and drifting with the surface current.
        A cascade only knows its own band, so it stores the probability that the whole surface folds there
        given its compression and the spread of the other bands: sharp where it dominates, a uniform low
        level where it is minor. Trails survive into the distance as mean coverage when the waves are sub-pixel;
    -   breaking shoreline waves, and wakes and splashes from the ripple simulation.
-   The pattern is procedural (no texture assets): round bubbles of random size on a jittered grid,
    merged with a smooth minimum, with walls bent by an fBm domain warp and density varied by large-scale
    clumping noise. A blurred "bubbles" layer lightens the water under the foam. The pattern lives in
    Lagrangian coordinates so it rides the wave orbits, and fades to its average when sub-pixel.

## Interaction

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
-   **Wake and splash churn**: the simulation's foam channel is a tracer with its own lifetime (Wake Foam
    Lifetime), carried by the surface drift (semi-Lagrangian) and spreading slowly. Bodies moving through the
    water churn it along their wake; a body hitting the water fast from above (jumping or falling in) digs a
    crater and churns a burst of it (Splash Foam); ripples that get steep enough break. The shading shows it
    as aerated water (bubble specks and a milky layer) with only a trace of surface foam, and no longer gates
    it to ripple crests, which made it flicker in and out as rings passed.
-   **Swimming**: `TESObjectCELL::GetWaterHeight` returns the wave surface, so swimming actors ride waves.
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
    foam and fog are effect meshes), without dynamic Havok bodies (those already float through buoyancy), and
    not resting on the bottom: the landscape (`TES::GetLandHeight` at five points) lies more than
    `max(0.3 draft, 0.25 m)` below its keel and a Havok ray straight down from the keel hits nothing within
    that distance (piers, posts, rocks and wrecks stand on something). Markers and water are skipped.
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

-   Settings > PBR Water > Debug: wireframe overlay / wireframe only, and debug views (normals, foam,
    depth/shore, crest compression, roughness, wave height, fetch), plus live sea-state values and whether
    the GPU ocean and the gameplay mirror are running.
-   Every resource is named for RenderDoc (`PBRWater::*`).

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
-   [ ] Lake Ilinalta vs a small pond vs the White River: wave size follows fetch and flow.
-   [ ] Wade into the sea: ripples from each leg, wakes, foam; no vanilla wading mesh.
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
-   [ ] Storm: whitecaps leave trails that thin out and drift downwind; far water keeps a faint foam cover.
-   [ ] Swim in a choppy sea: rings and foam stay on the swimmer, no ripples while floating still.
-   [ ] Jump off a dock: crater, ring and a burst of whitewater that drifts and fades over Wake Foam Lifetime.

## Known limitations

-   Shoreline waves and depth attenuation need a Terrain Shadows heightmap for the worldspace.
-   Fetch and depth weight whole cascades, so the transition from a sheltered bay to the open sea happens
    in four steps of band, not per wave. Shallow water calms the waves but cannot slow them (one dispersion
    relation per tile).
-   Gameplay leaves out the smallest cascade (waves under 0.71 m, ~1 cm rms, up to twice that in sheltered
    water where it carries more of the energy).
-   Gameplay water heights do not include the flowmap damping on rivers (fetch already keeps river waves small).
-   Seams between Unified Water tiles of different LOD sizes rely on the displacement distance fade.
-   Floating objects are found by heuristics: an unusual structure standing in deep water without touching
    the bottom can float (lower the Largest Hull setting), and a ship whose mesh reaches into a shallow bed
    stays still. Only loaded references move; distant LOD models do not. NPC navmesh on deck does not move,
    so NPCs keep their path while the deck carries them. Open hulls are not masked out of the water
    surface, so in rough seas a wave higher than a low gunwale can show inside the boat.

## References and credits

-   Tessendorf, _Simulating Ocean Water_ (2001) - FFT synthesis, choppy waves, frequency quantisation
-   Horvath, _Empirical Directional Wave Spectra for Computer Graphics_ (DigiPro 2015) - spectra and spreading
-   Mitsuyasu et al. (1975), Longuet-Higgins et al. (1963) - directional spreading
-   Bruneton, Neyret & Holzschuch, _Real-time Realistic Ocean Lighting using Seamless Transitions from
    Geometry to BRDF_ (2010) - filtered slope variance
-   Mihelich & Tcheblokov, _Wakes, Explosions and Lighting: Interactive Water Simulation in Atlas_ (GDC 2019) - FFT cascades, foam accumulation
-   Unity HDRP water system - CPU re-simulation of the FFT for script queries instead of readback
-   Hasselmann et al. (1973) JONSWAP; Pierson & Moskowitz (1964); Monahan & O'Muircheartaigh (1980) whitecaps
-   Olano & Baker, _LEAN Mapping_ (2010)
-   Ang, _The Technical Art of Sea of Thieves_ (SIGGRAPH 2018 Talks) - foam and scattering breakdown
-   Crest Ocean System (wave-harmonic/crest, MIT) - coverage-threshold foam with feathering and a bubble layer
-   Dave Hoskins, _Hash without Sine_ (MIT) - foam noise hashes
-   Atlas (GDC 2019, above) - subsurface term
-   Earlier PBR Water / Gerstner branches by davo0411 - tessellation hook points and the wading-mesh replacement
-   GPU Gems ch. 1 Gerstner waves - still the shape of the shoreline wave train
-   Bobbing Framework (RavenKZP, GPL-3.0-or-later WITH Modding Exception) - moving placed references with
    keyframed collision, their children and the actors standing on them; the floating objects build on it
-   Faltinsen, _Sea Loads on Ships and Offshore Structures_ (1990) - heave and roll natural periods of a hull
