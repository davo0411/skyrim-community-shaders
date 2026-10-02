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
weather wind -> JONSWAP spectrum (WaveModel)       Prepass: ripple sim CS, fetch upload, frame constants
phase integration psi_i (double, at camera)  ----> b7 PBRWaterData (waves, phases, fetch, terrain, ripples)
WaterEnvironment: fetch bake + bathymetry           BSWaterShader::SetupGeometry: per-draw constants,
gather ripple sources, Havok buoyancy                 HS/DS/GS binding for that draw's descriptor
TESObjectCELL::GetWaterHeight += wave height        BSWaterShader::RestoreGeometry: unbind, restore topology
```

### One wave function everywhere

The surface is a sum of 16 Gerstner waves whose amplitudes follow a JONSWAP spectrum for the current wind
(`WaveModel.cpp`). The same evaluation runs in:

| Consumer               | Code                        | Purpose                                              |
| ---------------------- | --------------------------- | ---------------------------------------------------- |
| Vertex / domain shader | `PBRWater::DisplaceSurface` | displacement, current and previous frame             |
| Pixel shader           | `PBRWater::ShadeSurface`    | per-pixel normals, crest folding, filtered roughness |
| CPU                    | `WaveSnapshot::SampleAt`    | water height for swimming and buoyancy               |

so geometry, shading, motion vectors and gameplay agree without any GPU readback (this is why the waves
are analytic rather than FFT).

Phases are integrated per wave **at the camera** in double precision:
`psi_i += -omega_i * dt + k_i * dot(d_i, cameraDelta)`; the GPU only adds `k_i * dot(d_i, x - camera)`.
This keeps the surface continuous while weather changes the spectrum (no sliding or popping), freezes
the sea when the game is paused, and avoids float precision loss far from the world origin.

### Nothing is hard-coded per location

| Input                  | Source                                                                       | Effect                                                                                       |
| ---------------------- | ---------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------- |
| Wind speed / direction | `RE::Sky::windSpeed/windAngle` (blended across weather transitions)          | spectrum energy, peak period, direction                                                      |
| Fetch                  | baked from Unified Water's cell coverage, 16 directions (`WaterEnvironment`) | JONSWAP fetch-limited peak and height: ponds and rivers stay calm, the open sea builds swell |
| Depth                  | Terrain Shadows heightmap                                                    | `tanh(kh)` depth attenuation, shoreline waves (Green's law shoaling, breaking at H = 0.78 h) |
| Flow                   | Unified Water flowmap                                                        | calms waves on rivers                                                                        |
| Absorption             | the water form's vanilla shallow colour and visibility distance              | Beer-Lambert extinction per channel                                                          |

Per-weather overrides are registered with the weather variable registry (`RegisterWeatherVariables`):
wave height, choppiness, directional spread, storm wind speed, shore waves, foam, visibility, subsurface,
roughness.

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
with upscalers), never go finer than a quarter of the shortest displaced wavelength, drop to 1 where waves
have faded out or the sea is calm, and patches outside the (displacement-padded) frustum are culled.

## Shading

-   Analytic per-pixel wave normals; components smaller than the pixel footprint are faded out and their
    slope variance is added to the GGX roughness (LEAN-style), so distant water keeps its sparkle energy
    without aliasing.
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
    -   crests: surface folding (Gerstner Jacobian) scaled by the Monahan whitecap coverage for the wind,
        sampled at two earlier instants as well so foam trails behind the crest and thins out;
    -   breaking shoreline waves, and wakes from the ripple simulation.
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
-   **Swimming**: `TESObjectCELL::GetWaterHeight` returns the wave surface, so swimming actors ride waves.
-   **Buoyancy**: dynamic Havok bodies floating at the surface get a spring towards the displaced surface
    plus the orbital velocity of the waves.

## Debugging

-   Settings > PBR Water > Debug: wireframe overlay / wireframe only, and debug views (normals, foam,
    depth/shore, crest compression, roughness, wave height, fetch), plus live spectrum values.
-   Every resource is named for RenderDoc (`PBRWater::*`).

## Validation outside the game

The shaders are validated with the real `d3dcompiler_47` against every recorded Water permutation in
`.github/configs/shader-validation.yaml`, for all five stages (VS, HS, DS, GS, PS) with `PBR_WATER`
defined, plus `RippleSimCS.hlsl`. With `PBR_WATER` undefined the restructured `Water.hlsl` compiles to
bit-identical bytecode to `dev` for all permutations.

## In-game test checklist

-   [ ] Wireframe on the sea near Solitude / Dawnstar: tessellation density follows distance, no cracks.
-   [ ] Toggle DLSS / FSR and frame generation: waves stay sharp, no ghosting trails on crests.
-   [ ] Underwater (looking up): surface, Snell's window and the water mask line up with the displaced waves.
-   [ ] Weather change clear -> storm on the coast: sea builds smoothly, no popping.
-   [ ] Lake Ilinalta vs a small pond vs the White River: wave size follows fetch and flow.
-   [ ] Wade into the sea: ripples from each leg, wakes, foam; no vanilla wading mesh.
-   [ ] Swim in waves: player bobs with the visible surface. Drop a basket in the sea: it rides the waves.
-   [ ] Shader cache cold start (async compile): water renders untessellated until compiled, then switches.

## Known limitations

-   Shoreline waves and depth attenuation need a Terrain Shadows heightmap for the worldspace.
-   Gameplay water heights do not include the flowmap damping on rivers (fetch already keeps river waves small).
-   Seams between Unified Water tiles of different LOD sizes rely on the displacement distance fade.

## References and credits

-   Tessendorf, _Simulating Ocean Water_ (2001)
-   Hasselmann et al. (1973) JONSWAP; Pierson & Moskowitz (1964); Monahan & O'Muircheartaigh (1980) whitecaps
-   GPU Gems ch. 1, _Effective Water Simulation from Physical Models_
-   Olano & Baker, _LEAN Mapping_ (2010)
-   Ang, _The Technical Art of Sea of Thieves_ (SIGGRAPH 2018 Talks) - foam and scattering breakdown
-   Crest Ocean System (wave-harmonic/crest, MIT) - coverage-threshold foam with feathering and a bubble layer
-   Dave Hoskins, _Hash without Sine_ (MIT) - foam noise hashes
-   Pleasant & Ross, _Wakes, Explosions and Lighting: Interactive Water Simulation in Atlas_ (GDC 2019) - subsurface term
-   Earlier PBR Water / Gerstner branches by davo0411 - tessellation hook points and the wading-mesh replacement
