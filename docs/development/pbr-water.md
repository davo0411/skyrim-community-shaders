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

| Input                  | Source                                                                       | Effect                                                                                        |
| ---------------------- | ---------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------- |
| Wind speed / direction | `RE::Sky::windSpeed/windAngle` (blended across weather transitions)          | spectrum energy, peak period, direction                                                       |
| Fetch                  | baked from Unified Water's cell coverage, 16 directions (`WaterEnvironment`) | JONSWAP fetch-limited peak and height: ponds and rivers stay calm, the open sea builds swell  |
| Depth                  | Terrain Shadows heightmap                                                    | `tanh(kh)` depth attenuation, shoreline waves (Green's law shoaling, breaking at H = 0.78 h)  |
| Flow                   | Unified Water flowmap                                                        | calms waves on rivers                                                                         |
| Absorption             | the water form's vanilla shallow colour and visibility distance              | Beer-Lambert extinction per channel                                                           |
| Clarity                | drifting sediment field, wave orbital velocity at the bed, flow, weather     | extra extinction and sediment colour; murky shallows in a swell, muddy rivers, clear calm sea |

Per-weather overrides are registered with the weather variable registry (`RegisterWeatherVariables`):
wave height, choppiness, directional spread, storm wind speed, shore waves, foam, visibility, subsurface,
roughness, turbidity, storm turbidity, sediment colour, underwater visibility and light shafts.

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
-   Water column (`Optics.hlsli`), blended in linear light:
    `refraction * T * Tbottom + body * (1 - T)` with `T = exp(-c * L)`, where `L` is the path along the
    _refracted_ ray (vertical depth from the depth buffer divided by the refracted direction cosine; Snell
    bends the ray down, so at grazing angles light crosses far less water than the straight view ray
    suggests) and `Tbottom = exp(-K_d * depth / mu)` is the light lost on its way down to what is seen
    (Gordon 1989: `K_d ~ a + b_b`). Extinction `c` is the water form's (shallow colour + visibility) plus
    sediment; scattering is spectrally flat, so the colour of clear water comes from absorption.
-   Body colour: the water form's lit body colour, shifted towards the sediment colour by the share of the
    extinction the sediment causes. The sediment gives the hue at about twice the body's brightness
    (particles backscatter more light than clear water), so muddy water turns olive-brown while every
    water form keeps its own brightness through the day.
-   Wave translucency: sunlight refracts into the far face of a crest, scatters once (Henyey-Greenstein,
    forward peaked as for ocean particles) towards the viewer and refracts out. Its colour is what survives
    the path through the crest, `(b / c) (1 - exp(-c t)) exp(-c s)`: clear water glows green-blue, murky
    water dimly in its sediment colour. Replaces the artistic Atlas term.
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

## Wind on the surface

The ripples too small for the wave model and the normal maps follow the local wind (`Shading.hlsli`):

-   **Micro-roughness**: the capillary share of the Cox & Munk (1954) clean-surface slope statistics,
    `mss = 0.003 + 5.08e-3 U` (up-wind `3.16e-3 U`, cross-wind `0.003 + 1.92e-3 U`), is added to the GGX
    roughness. Calm water is glassy, a breeze spreads the sun into a glitter path and blurs reflections.
-   **Gusts (cat's paws)**: the wind over the water varies in patches stretched along the wind and carried
    downwind at the wind speed; gusts show as darker, rougher patches sweeping across calm water. The vanilla
    normal maps (the wind's ripples) are scaled by the local wind, so calm patches lose them.
-   **Lee calm**: close to the upwind shore (short fetch) the wind has not reached the surface yet, so the
    water there stays smooth.
-   **Rain** roughens the surface (rings and crowns) and keeps the ripples whatever the wind.
-   **Glossy reflections**: the reflection cubemap and Dynamic Cubemaps are sampled at a mip chosen by the
    roughness, and rough water only shows the blurred screen-space reflection, so wind, rain and distance
    (where the wave detail is filtered into roughness) blur the reflections.
-   **Windrows**: Langmuir circulation sweeps whatever floats into lines along the wind, spaced at about
    twice the dominant wavelength (Craik & Leibovich 1976); the lines meander, break up along their
    length and drift slowly downwind. From a fresh breeze on (> 5 m/s) they carry streaks of old foam. In
    light winds (~2-7 m/s) they collect natural surface films that damp the capillary ripples (Marangoni
    damping), so they show as smooth, glassy slicks instead.

## Bioluminescence

Opt-in (off by default): plankton flash blue-green where the water is sheared, so breaking waves, wakes,
splashes and the swash glow, as sparks that light up and fade. The glow is emitted under the fog and only
shows in the dark (scaled by the scene light).

## Water clarity

Suspended sediment ("turbidity", `Optics.hlsli`) is split by where it sits in the column:

-   **Mixed** through the column: drifting, domain-warped fBm patches (`Patchiness`, `Patch Size`) that
    move downwind at a few cm/s and change shape over hours; river silt from the flowmap current; and a
    weather term from strong wind and rain that builds up and clears over minutes (`Storm Turbidity`).
-   **Bottom layer**, decaying with height above the bed (`exp(-z / H)`, a Rouse-like profile with
    `Sediment Layer Height` H):
    -   resuspension by the waves' orbital motion: linear wave theory gives the near-bed velocity
        `u_b = a w / sinh(k h)` (dispersion from Eckart's approximation); sand and silt start moving at
        ~0.1 m/s, so the surf zone and shallows turn murky in a swell while deep or calm water stays clear;
    -   **wading silt**: the ripple simulation's fourth channel. Feet moving over the bed (not swimming)
        inject silt that diffuses slowly and settles with a 20 s half-life, leaving clouds behind anyone
        walking through a pond or the shallows.

The surface uses the column mean of the bottom layer; the underwater view samples it per step of its ray
march, so clouds hang near the bottom.

## Underwater

Ported from the Crest Ocean System underwater effect (wave-harmonic/crest, MIT). Skyrim fogs the opaque
scene in the SAO composite image-space pass (`ISSAOComposite.hlsl`, `APPLY_FOG`), which runs before the
water is drawn; PBR Water hooks its three variants to bind its constants and, when the camera is in the
water, replaces the vanilla underwater fog there (`Underwater.hlsli`):

1. **Mask**: Crest renders an ocean mask; here the analytic wave surface (with the same Lagrangian
   inversion the CPU uses) is tested at each pixel's near-plane point, so the waterline across a
   half-submerged lens follows the waves.
2. **Depth merge**: the fog distance is the nearer of the scene and the displaced surface seen from below.
3. **Water volume**: a jittered, quadratically spaced ray march (`Underwater Quality` samples, resolved by
   TAA) through the extinction of the water form plus the sediment field. Each sample scatters the water's
   body colour (and the sediment colour, by share) split into sun and sky light by the current lights,
   attenuated by `K_d` with depth, with a Henyey-Greenstein sun glow and **light shafts**: the brightness
   of the sun ray through the sample comes from a caustic pattern where that ray entered the surface, so
   it is extruded along the refracted sun direction and integrated into shafts; the pattern coarsens and
   softens with depth. Submerged objects lose the light that was absorbed on its way down to them.
4. **Meniscus**: Crest's thin blue-grey line where the surface crosses the lens, a couple of pixels wide
   at any resolution (measured with the surface's screen-space gradient).

The surface seen from below (`UNDERWATER` water permutation) then uses the same functions: Snell's window
shows the world above, total internal reflection outside it mirrors the water body (instead of the sky
cubemap), and both are seen through the water in front of the camera. The composite marks the frame when
it has fogged the scene, so the window is not fogged twice.

The camera's water (flat height, form colours scaled by the weather, underwater fog distance as the
visibility) is looked up on the main thread each frame; the view is active while the camera is within the
reach of the waves above the flat plane or below it.

## Interaction

-   **Ripples** (`RippleSimulation`, `RippleSimCS.hlsl`): a 512^2 camera-centred heightfield integrated with the
    2D wave equation at a fixed 60 Hz, interpolated between steps to the render time. A copy of the
    previous frame's state provides correct motion vectors for the ripples. Every actor collision shape (each leg, the torso, ...), and loose Havok object
    crossing the surface acts as a moving constraint, which produces bow waves, wakes and rings. A fourth
    channel carries silt kicked up by wading feet (see Water clarity).
    It replaces the vanilla wading displacement mesh, which is disabled because it is a second surface
    that cannot follow the waves.
-   **Swimming**: `TESObjectCELL::GetWaterHeight` returns the wave surface, so swimming actors ride waves.
-   **Buoyancy**: dynamic Havok bodies floating at the surface get a spring towards the displaced surface
    plus the orbital velocity of the waves.

## Debugging

-   Settings > PBR Water > Debug: wireframe overlay / wireframe only, and debug views (normals, foam,
    depth/shore, crest compression, roughness, wave height, fetch, water clarity, local wind), plus live
    spectrum values.
-   Every resource is named for RenderDoc (`PBRWater::*`).

## Validation outside the game

The shaders are validated with the real `d3dcompiler_47` against every recorded Water permutation in
`.github/configs/shader-validation.yaml`, for all five stages (VS, HS, DS, GS, PS) with `PBR_WATER`
defined, plus `RippleSimCS.hlsl` and the `ISSAOComposite.hlsl` variants (fog, SAO, both) with and without
the other image-space feature defines. With `PBR_WATER` undefined the restructured `Water.hlsl` compiles to
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
-   [ ] Dive in the sea at noon: deep water darkens with depth, glow towards the sun, shafts from the
        surface; looking up, Snell's window shows the sky and total internal reflection the water body.
-   [ ] Half-submerged camera in waves: the waterline follows the waves with a thin meniscus.
-   [ ] Water Clarity debug view on a beach in a storm: brown surf zone, clear deeper water; walk through a
        pond and watch silt clouds settle behind you.
-   [ ] White River vs Lake Ilinalta: river silt vs patchy lake clarity.
-   [ ] Calm morning on a lake: glassy water near the upwind shore, gusts sweeping across as dark patches
        (Local Wind debug view); the sun's reflection spreads into a glitter path as the wind picks up.
-   [ ] Storm at sea: foam streaks along the wind; rain roughens the surface and blurs the reflections.
-   [ ] Bioluminescence on at night: wakes, splashes and breaking waves glow; nothing in daylight.

## Known limitations

-   Shoreline waves and depth attenuation need a Terrain Shadows heightmap for the worldspace.
-   Gameplay water heights do not include the flowmap damping on rivers (fetch already keeps river waves small).
-   Seams between Unified Water tiles of different LOD sizes rely on the displacement distance fade.
-   The underwater view replaces vanilla fog for the opaque scene only; forward-rendered transparent
    objects and particles keep the vanilla underwater fog.
-   When a wave trough exposes a camera that is below the flat plane, the game still draws the water with
    its from-below technique for that frame.

## References and credits

-   Tessendorf, _Simulating Ocean Water_ (2001)
-   Hasselmann et al. (1973) JONSWAP; Pierson & Moskowitz (1964); Monahan & O'Muircheartaigh (1980) whitecaps
-   GPU Gems ch. 1, _Effective Water Simulation from Physical Models_
-   Olano & Baker, _LEAN Mapping_ (2010)
-   Ang, _The Technical Art of Sea of Thieves_ (SIGGRAPH 2018 Talks) - foam and scattering breakdown
-   Crest Ocean System (wave-harmonic/crest, MIT) - coverage-threshold foam with feathering and a bubble layer;
    the underwater effect (mask, depth merge, scatter colour fog, meniscus) is ported from its
    `UnderwaterEffect.hlsl`, `UnderwaterEffectShared.hlsl` and `UnderwaterMeniscus.shader`
-   Gordon (1989), _Dependence of the diffuse reflectance of natural waters on the sun angle_ - `K_d ~ a + b_b`
-   Petzold (1972) particle phase functions; Henyey & Greenstein (1941)
-   Eckart (1952) explicit dispersion relation; Rouse (1937) suspended sediment profile
-   Cox & Munk (1954), _Measurement of the roughness of the sea surface from photographs of the sun's glitter_
-   Craik & Leibovich (1976), _A rational model for Langmuir circulations_ - windrow spacing
-   Dave Hoskins, _Hash without Sine_ (MIT) - foam noise hashes
-   Pleasant & Ross, _Wakes, Explosions and Lighting: Interactive Water Simulation in Atlas_ (GDC 2019) - subsurface term
-   Earlier PBR Water / Gerstner branches by davo0411 - tessellation hook points and the wading-mesh replacement
