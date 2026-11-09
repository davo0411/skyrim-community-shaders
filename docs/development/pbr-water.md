# PBR Water

PBR Water (`features/PBR Water`, `src/Features/PBRWater*`) turns Skyrim's water into a physically based,
wind-driven surface: real wave geometry through GPU tessellation, PBR shading, bubbles, shoreline waves and
interactive ripples, with gameplay (swimming, floating objects) following the rendered waves.

It is a **Beta, Unreleased** feature (`PBRWater.ini`) and builds on Unified Water (water meshes, flowmaps
and the per-cell water cache). Its depth comes from the plugins' own LAND records.

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
    peak (the Wind Turbulence setting, `DirectionalSpread`).
-   **Synthesis** (`OceanSpectrumCS` -> `OceanFFTCS` rows -> columns -> `OceanAssembleCS`): each lattice
    point gets `h(k, t) = h0(k) e^(-i w t) + conj(h0(-k)) e^(+i w t)` with `h0 = xi * a(k)`, where `xi` is a
    standard complex Gaussian from a hash of the lattice index (no textures, and the CPU can reproduce it).
    Eight real fields (choppy displacement, height, shear, slopes, compression) are packed two per complex
    transform. The FFT is a radix-2 Stockham kernel in group shared memory (one group per line; 128, 256 or
    512 per the Wave Detail setting).
-   **Outputs** (texture arrays, one slice per cascade, full mip chains): displacement + shear, slopes +
    compression, and `slope^2` (RG16F). Last frame's displacement is kept for
    motion vectors. The open sea's foam is a persistent R32F array beside them, one slice per cascade, updated in place (see Foam).
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
| Depth                  | the worldspace's LAND records, read from the plugins (`PBRWaterLand`)        | shore field: refraction, shoaling (Green's law), depth-limited breaking (Hs <= 0.6 h), swash up the beach                   |
| Flow                   | Unified Water flowmap                                                        | calms waves on rivers                                                                                                       |
| Absorption             | the water form's vanilla shallow colour and visibility distance              | Beer-Lambert extinction per channel                                                                                         |

The fetch bake marches 16 upwind rays per cell, each over the open water it crosses (past the map's edge it runs on
as open sea) up to 160 cells (~9.6 km, 144 km at the default Fetch Scale, about fully developed), then stores an
Shore Protection Manual _effective fetch_ (USACE 1984): `sum(x cos² a) / sum(cos a)` over the radials within
±45° of upwind, so it grows steadily away from a lee shore and stays short beside coasts and among islands (a
log-space mean let the shores beside a channel hold the fetch down across most of the sea). Only the water outside
the baked field counts as unlimited (500 km). A ray that reached the open sea used to count as
unlimited too, which jumped the fetch from a bay's to the ocean's with nothing fetch-limited in between; and a single
upwind ray zeroed the whole coastal sea whenever the wind blew offshore. Fetch-limited seas
also get the JONSWAP Phillips constant `0.076 (gF/U²)^-0.22` (amplitude gain up to 2x over open-sea PM);
the gain only raises the surface, not the horizontal displacement, so folding stays bounded.

A cascade's content is the same everywhere, so location changes weight whole cascades, at each cascade's
energy-weighted frequency and wavenumber: the fetch-limited Pierson-Moskowitz cut-off removes the long-wave
cascades from sheltered water (whose spectrum is the open sea's high-frequency tail, which is universal),
`tanh(k h)` calms them in the shallows. The CPU applies the same weights.

Per-weather overrides are registered with the weather variable registry (`RegisterWeatherVariables`):
wave height, choppiness, directional spread, swell height / period / direction, storm wind speed, foam,
visibility, subsurface, roughness.

### Rivers

Where Unified Water's worldspace flowmap shows a current, the waves are damped (`WaveParam.w`) and carried along it.
The vertex/domain shader reads the flowmap once by position (`GetWaveFlowDamping`) and gets the current
(`PBRWater::FlowVelocity`: RG direction, B x A strength, the River Current Speed setting at full strength); the pixel shader gets it as
a vertex interpolant (`FlowVelocity`). After Crest's `ShapeCombine` flow, `FlowPhases` gives each cascade two reads
pushed back along the current by two phases of a sawtooth clock half a period apart (period from the cascade's texel, a
power of two so the clock's 86400 s wrap never jumps), cross-faded with triangle weights. `EvaluateWaves` (with last
frame's phases for motion vectors) and `EvaluateWavesFiltered` (displacement, derivatives and LEAN moments) use the same
blend, and only where the current is non-zero (`[branch]`), so still water costs nothing. Anything else sampled per
cascade with the cascade UV (a foam slice) should take its reads from `FlowPhases` too. The flow clock is the game
timer (pauses with the game), independent of the FFT clock.

## Shoreline

Within a few waves of the shore the random sea hands over to Fluid Flux 2-style coastline waves (Imaginary Blend;
DaiZiLing's HLSL port of its `MF_SurfaceFlux_Coastline`, the Flux2-Shoreline-Wave-HLSL repository): a train of waves
running up a coastline map that is traced over the real depths instead of baked. No per-shore setup: everything follows
from the sea state and the terrain. Four parts: a coastline map (CPU, how long the waves take to reach the waterline
from where), the train (GPU, what they look like), the swash (GPU, what they do on the beach) and a per-pixel waterline
(where the water ends).

-   **Coastline map** (`WaveModel::BuildShoreField`, rebuilt on a `WaterEnvironment` worker around the player:
    768^2 texels of 128 units, ~1.4 km across, from the terrain and each cell's water level from Unified Water's
    cache). Fluid Flux's coastline distance, as a travel time: `tau`, the time the waves take from a point to the nearest
    waterline, integrated along the straight ray from the shore at the phase speed over the real depths. So the crests
    run parallel to the coast, slow down and close up as the water shoals, and surf runs into every beach nearly head-on
    whatever the sea offshore does. A true eikonal (`|grad tau| = 1 / c(h)`, solved by fast sweeping) let the first
    arrival race through deep water: the fronts came around every deep hole from all sides and met in its middle, so on
    Skyrim's steep, uneven beds most of the water had no coherent front and the train covered a strip one or two waves
    wide. Traced from the open sea in its one direction, the fronts ran along the banks of an inlet and lee shores got
    none. The map also holds the direction of travel (`grad tau`, of length `1 / c`), the signed distance to the
    waterline (to find the nearest point of the coast), the crests' offset along the coast, the beach slope at the
    waterline, the exposure to the swell (open water upwind along its direction: lee shores see little of it) and the
    room for the train. Up the beach the direction, exposure and room of the waterline the inland ray leaves from are
    carried. The phase lag `omega tau` uses the frequency the map was traced for (`Shore4`). A new map (the player moved
    ~175 m, the sea changed period, the swell turned) takes over by blending the two maps over 4 s.
-   **The whole worldspace's terrain** (`PBRWaterLand::Load`, `src/Features/PBRWater/LandRecords.cpp`): the game keeps
    LAND heights only for the 5 x 5 cells around the player, and the Terrain Shadows heightmap cannot stand in for them.
    It is BC4, 8 bits over +-262136 units, and Skyrim's land uses 38 of those values. Measured against every LAND in
    Skyrim.esm: all water 0-10 m deep read 20.3 m with no variation, 35% of the land within 10 m of the sea read as
    under water, with a bias of -18.6 m there. So a marsh 0.6 m deep shaded as deep sea, and wherever exact data ended
    the depth had a cliff that broke the waves, the breaking and the shore field's slopes. A worker reads every
    active plugin in load order instead, keeping the last LAND of each cell, into one grid of the terrain's vertices (a
    texel per vertex, 128 units apart): `t112`, R16_UNORM, about 2 cm per step. Records are found by walking the
    file's groups, and compressed records are inflated only as far as the heights (`VHGT`), so a LAND costs a fraction
    of its size. Nothing depends on what has loaded: depth, shore field and gameplay read the same exact heights
    everywhere, and they only change with the land itself. They are kept while the player stays on the same land,
    across interiors and the cities that use their parent worldspace's land.
-   **Coherent fronts only**: where rays from different stretches of shore meet (mid-channel, off a headland) their
    directions disagree. Averaged over ~7 m of water, the length of the mean direction measures how coherent the fronts
    are; the room fades the train where it drops below ~0.9 (Fluid Flux's coastline mask), and where no ray arrived.
-   **Train** (`PBRWater::ShoreWaveAt` and `ShoreWaves`, mirrored by `WaveModel.cpp`'s `ShoreTrain` for gameplay):
    -   _Whole waves_: the phase, `clock + omega tau` plus the offset along the coast, is cut into cycles, one per wave
        (as Fluid Flux's `frac` of its wave distance). Each wave is decided once, at its own crest half a wave away at
        most, so all of it agrees: its height (the sea's `Hrms = Hs / sqrt(2)`, its set, Green's law shoaling over the
        depth under the crest, up to the breaking limit `H = gamma h` and no steeper than Miche's `H / L = 0.142`) and its
        life stage. The breaker index `gamma` is Weggel's (1972) `b(m) - a(m) Hb / (g T^2)`, `a = 43.75 (1 - e^-19m)`,
        `b = 1.56 / (1 + e^-19.5m)` on the beach slope `m` (`ShoreBreakerIndex`): McCowan's 0.78 on a flat bed, higher on
        steep beaches. Up to the limit the stage is `r / (1 + r)` with `r` the shoaled height over the limit (the wave
        peaks as it shoals). Past it the stage runs on the distance travelled since the break point, in breaker heights,
        `(1 - r^-4/5) / (gamma m)` (Green's law puts the depth at `h_b r^-4/5`; `ShoreBreakerStage`): the crest overturns
        until it reaches the trough ahead at Galvin's (1969) plunge distance `(4.0 - 9.25 m) Hb` (at least 2), the tube
        collapses into a bore over as far again, and the bore runs to the waterline. Driven by `r` alone the curl lasted
        while the depth fell to a third of the breaking depth, tens of metres on a gentle beach. So a wave peaks where it
        shoals, breaks where it reaches its limit, on a sandbar as on the beach, and sooner if it is a big one; several
        waves at different stages stand along a ray at once.
    -   _Shape_ (`ShoreProfile.cpp`, t114): Fluid Flux draws its waves from a canned profile animation, a texture by
        position across the wave and life. This one is computed at start-up (256 x 64, two RGBA16F slices): forward and
        upward displacement, their derivatives, and the white water. A sinusoid narrows into a peaked crest over long
        flat troughs as it shoals (cnoidal), its water is thrown ahead of the front into a lip (each stage's forward motion is
        scaled so the surface never stretches below 0.35 of itself: a fold drew as a flat top over a right-angled wall at
        the mesh's vertex spacing)
        (plunging), which falls into a bore: a short front and a long back (Svendsen 2006), lower than the wave that
        broke. Displacement and surface vanish at both ends of a wave, so consecutive waves meet without a step. The sea's
        level comes from the depth at the point (continuous), not from the wave.
    -   _Hand-over_ (`ctx.train`): the train takes over within `ShoreTrainCycles` (setting, default 7) waves of the shore, faded over the
        outer half, counted in waves rather than depth: the beds fall away within a wave or two of the waterline, and a
        depth limit left room for one wave. Offshore it rides on the FFT sea. Only in the surf, as its shoaled height
        nears the breaker limit (`ShoreSurfShare`), does it take the random sea's energy up to twice its frequency
        (`ShoreHandOver`), so the breakers are the train's with the short chop on top. Handing over wherever the train
        reached replaced a stormy FFT sea with the smooth train hundreds of metres out.
    -   _Period_: the train follows the sea's peak period held to 2.5-3.5 s by default (settings `ShoreShortestPeriod`,
        `ShoreLongestPeriod`): Skyrim's coast is compressed as its distances are (Fetch Scale), and the open sea's
        period gave one wave as long as the beach is deep.
        The clock runs at that frequency, so the crests travel at the waves' own celerity (a slower clock, the former
        `ShoreTimeScale`, held a broken wave's rolled lump on the water for seconds).
    -   _Surf foam_ (`ShoreFoamCS`, `ShoreSimulation::UpdateFoam`, t127 `ShoreFoamAt`): the bores' white water rides the
        wave (the profile); what the breakers leave behind is simulated on a camera-centred grid (1024 x 1024, 17 cm, in
        the water's rest coordinates). Foam is a tracer on the surface: the profile's white water is its source (a share
        `ShoreResidualFoam`, "Foam Left Behind", of it), the water carries it semi-Lagrangian (as Crest 4 moves its foam)
        and it decays with the e-folding time `ShoreFoamLifetime` (default 12 s; surf foam outlasts the bubble plume under
        it where coastal surfactants stabilise it) on the water and within a second stranded on dry sand. Under the waves
        the water moves at `u = c eta / (h + eta)` less the drawn displacement's own rate; at the waterline with the
        simulated swash's own velocity. Up the beach the profile makes none: the swash carries its own (below). The advection
        reads Catmull-Rom, clamped to the four texels around (a bilinear read every frame blurred the foam into smears over
        its lifetime), and the water shader reads the grid as a cubic B-spline (bilinear drew its 35 cm texels as
        diamonds). The breaking wave's own white water (`BreakerFoam`) is drawn as the open sea's is, a raw coverage with no
        texture: from the folding of its surface (the Jacobian of the shore train's horizontal displacement below 0.6,
        while it breaks) and the profile's foam slice. The foam texture is kept for the flat white water: the bore once it
        has formed and lies near horizontal, what the surf left on the water and the swash sheet's; the two cross-fade by
        the surface's flatness and the wave's stage. A texture on the face, projected from the side and climbing with the
        water (the former `ShoreFaceRise` / `ShoreFaceFoamPhases`), streaked. The pattern holds still where the surface is
        and the foam moves over it as its coverage. Every other way of carrying the pattern tore it: at the rest
        position the waves' horizontal displacement drew it out across every breaker's face, from above alone it ran down a
        rising face, and carried along (with the water sloshing under the waves, with the breakers, with the swash, where
        neighbouring water belonged to different uprushes, or as a texture offset per texel) it was torn into streaks. The
        stencil pass writes no sideways motion for the pixels the foam covers, so temporal AA and upscalers do not
        reproject it from where the mesh was. The
        pattern is a microfoam (`FoamBubbleSize`, default 3 cm, lace at 3 and 9 times that, `Foam.hlsli` SurfaceFoam);
        each scale drops out as it shrinks below a few pixels and distant foam is drawn as its coverage, not as coarser
        blotches. The swash sheet's edge is antialiased over the pixel. This replaced a fixed lace wherever the surf was depth-limited and a
        coverage window sliding over a still pattern, which smeared as the waves moved.
    -   _Breaking or surging_: the Iribarren number of the beach (Battjes 1974): waves spill and plunge on gentle beaches
        (`xi < 2`) and surge against rock faces and cliffs (`xi > 3.3`), where their stage stops short of breaking.
    -   _Crest wobble_ (`ShoreWobble`, setting `ShoreCrestWobble`, default 0.35 waves): the phase of every train is
        offset by three slow sines of the waterline point and the clock, so crest lines bend and drift and stretches of
        beach break at different times. Each sine turns over a whole number of times per wrap of the clock (1024 waves).
    -   _Bore whelps_ (`ShoreWhelps`, `ShoreWhelpAmplitude`, setting `ShoreBoreWhelps`, default 0.6): a weak bore
        (Froude number below about 1.3: its height under 0.4 of the depth) trails a short train of smaller crests, a few
        depths apart, dying away behind its front (Favre 1935; Chanson 2009), and water that deepens inshore of a bar frees
        a wave's harmonics in the same way (Beji and Battjes 1993). They live in the main wave's own phase, so they never
        run ahead of it. They replace two independent shorter trains (the former `ShoreSplitWaves`), whose crests were tied
        to nothing and popped in ahead of and behind the main breaker. Mirrored on the CPU.
    -   _Wind_ (`ShoreOnshoreWind`): an onshore wind lowers the breaker index (`ShoreWindBreaking`, 12 % at full strength)
        and moves the breaker type towards spilling (`ShoreBreakerIribarren`, `ShoreWindPlunge`, 30 % of the surf
        similarity); an offshore wind holds the waves up into plungers that break later (Douglass 1990; King and Baker
        1996; Feddersen and Fincham 2023). Mirrored on the CPU.
    -   _Curl_ (`ShoreCurlShape`, setting `ShoreCurl`, default 1): only plunging breakers curl, by the beach's surf
        similarity `xi0 = tan(beta) / sqrt(H0 / L0)` with the sea's real peak period (Battjes 1974: spilling below 0.5,
        surging above 3.3; `ctx.iribarren`). Where the mesh is fine enough (`ShoreCurlWeight`:
        vertex spacing below 1/25 of the wavelength, full at 1/50; tessellated draws only) the plunging stages
        (0.40-0.80) turn the water above a pivot on the front face (45 % of the crest's height) forward about it, by up
        to 2.3 rad at the crest and less behind it, and throw it 1.2 wave heights ahead, so the crest becomes the lip's
        tip and falls into the trough in front. The curl grows over stages 0.44-0.64 (to Galvin's plunge point), then
        the tube is crushed into the plunge point (0.62-0.70) while the bore, its front pushed up ahead of it, takes over
        (to 0.78; `ShoreBreakerStage` gives the transition half a plunge length, Svendsen 1984, and shortens a spiller's
        plunge to 0.35 of Galvin's). The bore then settles to the inner surf's saturated height, 0.4-0.6 of the depth with
        the slope (`ShoreBoreHeight`; Thornton and Guza 1983, Raubenheimer et al. 1996), and keeps a front of 0.15 of the
        breaker's height to the waterline for the swash to carry on; unrolling the lip instead raised a wall of glassy water. The bore's front is a roller of about 30
        degrees (`frontWidth` 0.035 of a wave in ShoreProfile.cpp), not a step. It depends on the wave's aspect, so the shader works it out (three evaluations for the phase
        derivative) instead of the table, blended in from it at the ends of the stage window and where the mesh first
        resolves the face (spacing 1/12 to 1/25 of the wavelength). A weaker plunger (surf similarity 0.35-0.6) or a
        mesh between 1/25 and 1/50 curls less (a smaller turn and throw: the curl's `amount`), rather than blending a
        share of the full curl into the table's wave: that moved the vertices little while the full curl's turn about
        the lip took over the normal, so a plain standing face was lit as an overhang. The surface stays one
        sheet, so `FinishWaves` keeps the downward normals of the lip's underside where a curl is shown, and the pixel
        shader lays the ripples' and detail maps' slopes onto the face wherever the surface is near vertical or past it
        (along the crest, and up and down the face for what runs along the way the face looks: projected as a horizontal
        tilt, a near-vertical face lost all but the along-crest part and shaded flat) (blended in by `waves.normal.z`, not by the curl's weight, which drew a line of sideways normals
        along the crest). A shading normal facing away from the camera (a lip's far side, seen from behind or where it
        passes through the water behind it) is mirrored across the view direction (Water.hlsl). On a steep face the refracted
        path is held to the crest's thickness at that height (about three times the height still above it,
        `ShoreTrain::crest`): the view ray runs into the crest nearly level and leaves through its back, where the slab
        model took it down to the bed far behind and every face shaded as the same deep body colour. Where a wave stands
        above the still water, anything behind it that is also above the still water (icebergs, cliffs, the far shore)
        is not its background: the refracted ray bends down into the sea and never reaches it, so the path is taken as
        endless there and only the bed shows through. The water is one depth-tested layer, so the nearest wave hides
        those behind it without any order-independent transparency. Foam is lit about the normal bent halfway up with the sun wrapped around it (whitewater scatters
        light through itself), so the foam on a lip's underside does not go dark.
        The hull shader
        divides the target triangle size by `ShoreCurlDetail` (default 2) on the shore wherever a curl can show.
        Geometry only: the CPU mirror rides the table's uncurled wave.
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
-   **Wave heights** (`ShoreSet`, mirrored on the CPU, setting `ShoreWaveVariation`, default 1): random waves have no
    single height or break point; the biggest break furthest out (Thornton and Guza 1983). Each wave's height is a slow
    group envelope (0.6-1.4, some eight waves a group: Longuet-Higgins 1984) times a Rayleigh draw of its own (no memory
    from one wave to the next), at most 2.5 times the sea's. The draw varies along the coast over a crest length that
    shortens with the directional spread (`ShoreCrestLength`: 20 deep wavelengths in a unidirectional swell to 2 in a
    confused sea; Baker et al. 2023), so stretches of one crest break at different distances from the shore. A wave keeps
    its height all the way in.
-   **Swash, simulated** (`SwashStepCS`, near the camera): every bore's uprush is water of its own. The shallow water
    equations with wetting and drying run on the foam's grid (1024 x 1024, 17 cm, scrolled in whole texels): Rusanov
    fluxes on the hydrostatic reconstruction of Audusse et al. (2004), which keeps water at rest over any bed, keeps
    depths positive and lets the edge run over dry sand; explicit steps of at most 3.5 ms (up to ten a frame), the bed
    held no deeper than 2 m; dry sand with dry neighbours and no foam is skipped. The surf hands its water over:
    `ShoreFoamCS` lays out the bed and, from deeper than about 0.7 of the waves' height (while the bore is still a bore),
    the analytic waves' level, their water's velocity (under a crest the bore's, Rankine-Hugoniot: `C = sqrt(g h2 (h1 +
    h2) / (2 h1))`, `u2 = C (1 - h1 / h2)`, so collapsing bores launch uprushes of a few metres a second; Shen and Meyer
    1963, Hughes 1992) and their foam, and the state is relaxed to them there (50 ms), so each bore enters
    as itself. The hand-over (`SwashHandOver`) runs from about 0.7 of the waves' height deep and is finished by
    about 0.4 m of water, before the drawn bore's front collapses, so the simulation carries the bore's water through
    the collapse; the drawn shore waves give way to it in the same measure (`DrawnShoreWaves`), so only one crest runs in. Each wave comes in turned by up to 10 degrees of its own (a hash of its number and stretch of coast), so
    neighbouring uprushes cross. Inshore of that the water is free: it runs up the slope, slows, drains back, meets the
    next uprush (a hydraulic jump) and the ones beside it. Bed friction is implicit, its drag coefficient
    `0.006 * SwashLinger` (setting, default 2): more drag, a shorter uprush that drains back more slowly. The foam is a
    tracer per area carried upwind with the water: the bores' foam rides in with them, thins as the sheet spreads, decays
    over `ShoreFoamLifetime` on the water and within a few seconds stranded on the sand (the swash mark), and white water
    forms where the water converges hard (uprush into backwash or into another uprush) and at the uprush's front, where
    the depth falls away ahead of fast water. `SwashOutCS` packs the swash's surface, depth
    and foam with the surf's foam into `ShoreFoamTexture` (t127); the draws (`SwashSheet`, `SwashSurface`), the wet
    sand (`ShoreSimCS`) and the foam read it. Where the surf holds the water the analytic waves are drawn and the swash
    has no depth. Past the grid (beyond about 80 m, faded over its outer tenth) the analytic swash below takes over.
-   **Swash, analytic** (`PBRWater::SwashTip`, past the simulation's grid): from the crests that reached the waterline of the ray through a point
    over the last four periods (an uprush each time a bore's front, `ShoreBoreFront` into the cycle, reaches the
    waterline, each with its own height), the tip runs up as far as Stockdon et al. (2006) give and moves ballistically, decelerating at `g sin^2 beta` (Shen & Meyer 1963; Peregrine &
    Williams 2001). The backwash takes `SwashLinger` (setting, default 2) times as long as the uprush (deceleration
    divided by its square, and the sheet thins on the stretched time): the water soaks in and drags on the sand
    on the way down, so the sheet and its foam stay on the beach longer. Stockdon's `L0` is the real sea's deep-water
    wavelength (`ShoreParams::seaLength`, Shore4.z), not the shore train's held-short period: run-up grows with
    `sqrt(H0 L0)`, and the train's 3 s wavelength cut it to about a third. The bore keeps its white water to the waterline
    (the profile no longer fades it over the last stages); a fresh uprush is all foam and drains as lace, by its age
    from the lens coefficient (`swashFresh`), its leading 15 cm white. The water over the beach is a thin sheet (`SwashTop`: 15 % of the height below the tip,
    at most 30 cm), never a raised mesh. Once a crest has drained the tip keeps falling below the still water
    rather than resting at it, so the vertices interpolate it linearly (a tip clamped at 0 put the swash front
    on the triangle edges). The swash mark (the highest a drained crest reached) goes to the pixels with the
    tip; up to it the mesh stays above the bed and both passes keep the pixels.
-   **Waterline per pixel**: near the waterline the vertex shader holds the mesh just above the bed
    (`WaterlineLift`), so where the water ends is decided per pixel. The bed is read where the waves have carried the
    vertex, not under its rest position, and as high as the game's terrain triangles can stand there
    (`TerrainCeiling`: the bilinear height plus a quarter of the LAND quad's twist): held above the bed at its rest
    position, a vertex carried up the slope sank its triangles into the sand and left pockets of ground in the water. Both passes first drop the pixels whose
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
-   **Foam needs real breakers** (`BreakerStrength`): breaking is decided by the height against the depth,
    so even centimetre ripples "break" in the last few centimetres of water, but only real breakers entrain air.
    The shore waves' break stage scales with the arriving significant height (a trace from 2 cm, full from
    40 cm), so a stream's wavelets make only a little.
-   **Wet shores** (`ShoreSimulation`, `ShoreSimCS.hlsl`, a camera-centred grid scrolled in whole texels):
    the highest water each point saw lately (the swash on beaches, the waves against rocks, piers and hulls),
    sinking slowly as it drains, and the water surface height. Lighting reads them through
    `t123` and `SharedData::pbrWaterSettings` (`PBRWater/ShoreWetness.hlsli`): Wetness Effects' shore wetness
    follows what the waves reached, and caustics and underwater wetness follow the moving surface.

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
with upscalers): the target size near the shore and in the ripple simulation (`EdgeTessFactor`: one shore-field lookup
per edge), twice it on the open water. The hull shader reads no waves: it runs for every edge of every patch, and its
old curvature term (the wave context and three cascades per edge) made it one of the most expensive stages. Edges
drop to 1 where waves have faded out, and patches outside the (displacement-padded) frustum are culled. The domain
shader reads each cascade from the mip whose texels match the vertex spacing the tessellation aims for at that
distance (`TessellatedSpacing`, a function of position only, so neighbouring patches agree and shared edges never
crack), so waves the mesh cannot carry are averaged out instead of aliasing. Only the stencil pass works out last
frame's surface (motion vectors); the colour pass skips it.

### Draw cost

Unified Water draws its distant water as hundreds of small tiles, each a draw call in both the stencil and the colour
pass (836 at the Solitude docks), and nearly all of them flat. Setting each one up as a full PBR draw (the constants
uploaded, 17 views bound on four stages, samplers saved and restored, tessellation bound and unbound) cost 8.6 ms of
the water's 16.4 ms there while the GPU executed almost nothing. Now a tile wholly outside the waves' reach
(`WavesReach`: outside the loaded cells or past the displacement fade) is drawn untessellated, the constants are
uploaded once per frame into two buffers (with and without tessellation) that the draws swap between, and the views
stay bound for the frame (only the pixel stage's t110-t111, which Landscape Seams shares, is checked per draw). The
water's draws went from 16.4 to 10.9 ms there.

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
    Quad-Fragment Merging_, SIGGRAPH 2010). Hence the target triangle size.
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
-   **Caustics on the bed** come from Water Effects (`WaterEffects::ComputeCaustics`, for every shader that draws them, PBR
    Water or not): after Crest's `ApplyCaustics`, the pattern is read where the sun's light crossed the surface
    (`depth * sun.xy / (4 sun.z)` away) and blurs with `|depth - focal depth| / depth of field` mip levels on top of the
    hardware's (focal depth 2 m, field 0.33 m). Its projection, low-sun limit, focal depth, depth of field and maximum
    blur are Water Effects settings (`SharedData::waterEffectsSettings`).
-   Drifting sediment patches and wind gusts are tens of metres across: their noise (`TurbidityPatches`, `WindGust`)
    is baked once per frame around the camera (`SurfaceNoiseCS`), the vertices read it (`PBRWater::SurfaceNoise`,
    evaluating it themselves beyond the bake) and the pixels interpolate it.
-   **Foam** is made only where waves break, and lit as a diffuse scatterer. One setting, Foam Amount (1 = as measured).
    -   _Open sea_ (`OceanAssembleCS`, `PBRWater::FftFoam`): a direct port of the foam of GarrettGunnell/Water (Acerola, MIT).
        Every texel of every cascade keeps a foam value (`OceanFoam`, t121, mip-mapped). Each frame, from the Jacobian
        `J = (1 + dDx/dx)(1 + dDy/dy) - (dDx/dy)^2` of that cascade's own displacement (choppiness is already in the
        spectrum): `foam *= exp(-decay)`, `foam = saturate(foam)`, `biased = max(0, bias - J)`, and
        `if (biased > threshold) foam += add * biased`. Acerola steps once per frame; here the decay exponent and the add
        scale by the frame time in 1/60 s, so 60 fps is identical and a paused game neither grows nor decays the foam.
        The foam is cleared when the resources are made and when the sea goes calm. Defaults are the sample scene's
        (bias 0.85, threshold 0, add 0.1, decay 0.0175; the script's defaults differ: -0.5, 0, 0.5, 0.05); Foam Amount
        multiplies `add`.
    -   _Drawing it_ (`EvaluateWavesFiltered`, Water.hlsl): the pixel sums the four cascades' foam, each plus its offset
        (the sample scene's `_FoamSubtract`: 0.04, -0.04, -0.46, -0.38, longest waves first), clamps it, fades it with
        the camera distance (`pow(1 - distance / FoamFadeDistance, FoamDepthFalloff)`, Acerola's `1 - Linear01Depth`)
        and blends `lerp(colour, foamColour, foam)` with the foam read at the same cascade UVs (and river flow reads) as
        the displacement. `foamColour` is `PBRWaterFoam::Albedo` lit as the shore foam is. The only change from Acerola:
        each cascade counts by its local share (`ctx.cascade`, `FoamCascadeWeightFull`), so ponds, sheltered water and
        the surf, where the shore train has taken the FFT's waves over, get none. The foam also adds
        `FoamRoughnessModifier * foam` to the base roughness (0 in the sample scene). It is the raw value: not run
        through the black-point pattern below, which the shore foam alone uses. Where both foam, the larger coverage wins.
    -   _Shore waves_ carry their white water in their profile (lip and bore front), moving with the wave, scaled by
        how real the breakers are (`BreakerStrength`); the water's edge is a thin line of lace where the swash sheet is
        about to end (Water.hlsl).
    -   _Pattern_ (`PBRWater/Foam.hlsli`, shore foam only): the "black point fade" of Crest: the foam texture is thresholded so dense
        foam thins into lace and then specks; thin foam is also translucent. The texture (`FoamPattern.cpp`, t125, 512^2
        R8 with mips, baked at start-up) holds round bubbles of random size, histogram-equalised so a coverage `C`
        whitens a share `C`. It is sampled at three scales four times apart, which gives the edge irregular lace at every
        size (one scale drew smooth spots), and the scales follow the pixel: as the finest would turn sub-pixel it fades
        out and a coarser one fades in, so foam stays a crisp pattern at any distance. Nothing fades it into a flat mean.
        It is sampled at the Lagrangian position, so it moves and compresses with the water.
    -   _Lighting_: foam is a Lambertian scatterer (Dupuy & Bruneton 2012 after Koepke 1984), lit by the sun (with
        its shadow) and the sky (with Skylighting) like any surface. Reflectance (`PBRWaterFoam::Albedo`): dense foam
        ~55 % in the visible (Whitlock, Bartlett & Gurganus 1982), a little less towards the red (Frouin, Schwindling
        & Deschamps 1996). Foam scatters the sun glint with the rest of the light.
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
    displacement) and the shaders sample at undisplaced positions, so rings and wakes stay attached to
    the body and move with the waves. Previously they were injected at world positions but sampled at rest
    positions, so in a choppy sea they sat up to a metre off the swimmer and sloshed back and forth. Motion
    is measured relative to the water too: a body bobbing with the waves makes no ripples.
-   **Ripple physics** (after Crest 4's `UpdateDynWaves.compute` and `ShapeCombine.compute`):
    -   _Wave speed_ is the gravity-capillary phase speed `c^2 = g / k + sigma k / rho` (sigma 0.074 N/m) at the shortest
        wavelength the grid holds, two texels (Crest: deep-water `g / k` at the same wavelength), clamped so the leapfrog
        scheme stays stable (Courant `C^2 = (c dt / dx)^2 <= 0.45`; the limit is 0.5). The viscous term is kept. At the
        default 64 m grid (12.5 cm texels) that is about 0.6 m/s.
    -   _Terrain awareness_: the depth per texel is the still water level minus the terrain height (`TerrainTexture`, t112,
        mapped by `Terrain0` / `Terrain1`). The level comes from the exterior cells around the eye
        (`PBRWater::GatherWaterLevels`: `GetExteriorWaterHeight` of a 5 x 5 cell table, the eye's own cell from
        `GetWaterHeight` at the eye so a lake or river at another height than the cell default is followed), the same
        source as the Skylighting occluder. The grid is for one water level per cell, not per water body: two bodies at
        different heights in one cell are measured against the same level. Dry texels (depth <= 0) hold 0, so ripples stop at
        the shore instead of running under the beach; shallower than half a wavelength the amplitude is damped by
        `1 - (1 - saturate(2 depth / wavelength)) * dt * 2` per step. Interiors, and exteriors without the terrain texture,
        have no depth (deep water). The terrain and flowmap mappings are those uploaded for the previous frame.
    -   _River flow_: the previous state (both leapfrog levels, and the silt) is read semi-Lagrangian at `p - dt v`, bilinearly,
        with `v` from Unified Water's worldspace flowmap (`Flow0`), decoded exactly like `PBRWater::FlowVelocity` so the waves
        and the ripples are carried by the same current. Without a flowmap there is no advection.
    -   _Horizontal displacement_ (`RippleDisplacement`): the ripple height displaces the surface vertically and, like a
        Gerstner wave, horizontally along its gradient, `HorizDisplace * grad(h) / k` with `k = 2 pi / (3 texels)`, clamped to
        +-0.3 texel. `DisplaceSurface` applies it for this frame and last frame's copy (motion vectors), as a function of the
        position only, so the stencil and colour passes agree. The pixel shader's ripple normal still comes from the height.
    -   Results that are not finite are zeroed (checked by bit pattern: the compiler assumes no infinities and would fold
        `isfinite` away).
-   **Splashes**: a body hitting the water fast from above (jumping or falling in) digs a crater.
-   **Swimming**: `TESObjectCELL::GetWaterHeight` returns the wave surface, so swimming actors ride waves.
-   **Underwater view**: the game's camera-underwater state (`TESWaterSystem`
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
    not a rock, boulder or cliff model, and not resting on the bottom. The bed comes from the terrain (else
    `TES::GetLandHeight` queried from above the hull, never from its keel, which is under the ground for a
    sunk rock). A hull may graze the bed of a shallow
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

## Settings

Physics stays in the code (gravity, the index of refraction, JONSWAP's alpha and gamma, Miche's and McCowan's limits,
Hrms = Hs / sqrt 2, the sun's angular radius, the foam's measured albedo, surface tension, stability limits, dispersion
relations); every guess, artistic scale and quality knob is a setting. At the defaults the water renders as it always
did. The menu has one tab per concern; rarely touched expert knobs sit under an Advanced header in their tab.

| Tab               | Holds                                                                                                                                             |
| ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| Waves             | wave height, wind speeds, swell (height, period, direction, peakedness, spread), choppiness, wind turbulence, fetch scale, gusts, wind streaks  |
| Shore             | shore waves, wet shores, train reach and pace, shore periods, wave sets                                                                           |
| Surface & Light   | roughness, normal map strength, refraction, sun and point light specular, subsurface, Fresnel blend                                               |
| Water Clarity     | visibility, turbidity and sediment (colour, density, patchiness, river, storm and wading silt, settling), scattering and light attenuation       |
| Foam              | open-sea foam (coverage, growth, fade, distance, roughness, per-cascade offsets), pattern (bubble size, feather), surf lace                       |
| Ripples & Rivers  | ripple range, height, strength, lifetime, horizontal displacement; river current speed, wave damping and flow period                              |
| Floating Objects  | swimming follows waves, floating objects, carrying actors, hull motion, range, size, small object buoyancy                                        |
| Performance       | wave detail, tessellation (triangle size, maximum factor), wave fade distances                                                                    |
| Debug             | wireframe, debug views, statistics                                                                                                                |

River Current Speed is one setting that drives both the ripple simulation and the wave shaders. Values the shaders
read arrive in `GpuData::Tune0..Tune5` (`PBRWater.hlsli`); the
shore train's periods, pace, reach and wave sets go through `PBRWaterModel::ShoreTuning`, so the CPU mirror
(swimming, floating objects) follows the same values as the shaders. Ripple Range applies live (the simulation
re-centres its grid when the texel size changes); all settings are clamped by `SanitizeSettings`.

## Debugging

-   Settings > PBR Water > Debug: wireframe overlay / wireframe only, and debug views (shore field: train share / room / exposure, wave direction, normals, foam,
    depth/shore, cascade foam (red the open sea's foam, green the shore waves' foam, blue the shore waves' break stage),
    roughness, wave height, fetch), plus
    live sea-state values (mean square slope) and whether
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
-   [ ] Wade into the sea: ripples from each leg, wakes; no white foam on the legs; no vanilla wading mesh.
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
-   [ ] Swim in a choppy sea: rings stay on the swimmer, no ripples while floating still.
-   [ ] Jump off a dock: crater and ring.
-   [ ] Open sea and a bay in a storm vs a fresh breeze: foam where the crests sharpen (Debug: Cascade Foam red),
        fading over a second or two, more of it the stronger the wind; none in calm weather or on ponds; none while the game
        is paused with the foam frozen as it was.
-   [ ] Beach near Dawnstar / the Solitude coast in a fresh breeze: waves shorten, turn to the shore and
        break with foam on the top of each breaking crest, irregular along it, trailing behind the crest and thinning
        out before the next; underwater, no white line along the shore; the sand is wet up
        to where the water reached and dries slowly. Rock outcrops and cliffs get no surf or swash, only the open-sea
        waves and wet rock up to the crest height.
-   [ ] Choppy sea with Skylighting: troughs are darker than crests (sky light and reflection); shallow
        seabeds and wading legs are not darkened by the water above them.
-   [ ] Sunset over a swell: the troughs behind the crests fall into shadow and lose the sun glint; at midday
        nothing is shadowed.
-   [ ] Swim at the surface in waves: the underwater view starts when the camera dips under a crest.

## Known limitations

-   Shoreline waves and depth attenuation start once the plugins' terrain has been read, about a second after a
    worldspace with new land loads.
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
-   Gameplay water heights do not include the waves travelling with river currents either (the CPU mirror ignores the
    flowmap); Skylighting's occluder grid does not carry them along the current.
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
-   GarrettGunnell/Water (Acerola, MIT, Copyright (c) 2023 Garrett Gunnell) - the open sea's foam: per-cascade Jacobian accumulation, decay, per-cascade offsets, distance attenuation
-   GodotOceanWaves (2Retr0, MIT) - Jacobian whitecap foam: growth below a threshold, exponential decay, summed over cascades
-   Ocean-URP (gasgiant, MIT) - per-cascade foam weights
-   Fluid Flux 2 (Imaginary Blend) and DaiZiLing's Flux2-Shoreline-Wave-HLSL port of its coastline waves - coastline
    map, coastline offsets, wave profile by phase and life stage, three-phase foam advection
-   Unity HDRP water system - CPU re-simulation of the FFT for script queries instead of readback
-   Hasselmann et al. (1973) JONSWAP; Pierson & Moskowitz (1964)
-   Olano & Baker, _LEAN Mapping_ (2010)
-   Ang, _The Technical Art of Sea of Thieves_ (SIGGRAPH 2018 Talks) - foam and scattering breakdown
-   Fatahalian et al., _Reducing Shading on GPUs using Quad-Fragment Merging_ (SIGGRAPH 2010) - small-triangle cost
-   NVIDIA & AMD, _Tessellation Performance_ (GDC 2010); Cantlay, _DirectX 11 Terrain Tessellation_ (NVIDIA 2011) -
    factor budgets, patch culling
-   Unity HDRP `TessellationShare.hlsl` - the GCN factor cap, per-vertex attributes interpolated in the domain shader
-   Johanson, _Real-time Water Rendering: Introducing the Projected Grid Concept_ (Lund 2004); Strugar, _CDLOD_
    (2010); Losasso & Hoppe, _Geometry Clipmaps_ (SIGGRAPH 2004) - ocean mesh LOD without hardware tessellation
-   Crest Ocean System (wave-harmonic/crest, MIT) - waves carried along river flow (ShapeCombine.compute `Flow()`, two time-offset reads cross-faded with triangle weights)
-   Crest Ocean System (wave-harmonic/crest, MIT) - foam texture black-point fade, saturated foam growth, bubble layer, caustics projected along the light with a focal depth (OceanEmission.hlsl `ApplyCaustics`)
-   Crest Ocean System (wave-harmonic/crest, MIT) - interactive ripples: terrain-aware shallow damping, flow advection, Courant-limited wave speed, non-finite guard (UpdateDynWaves.compute) and the horizontal displacement from the ripple gradient (ShapeCombine.compute, `_DYNAMIC_WAVE_SIM_ON`)
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
-   Audusse, Bouchut, Bristeau, Klein & Perthame, _A Fast and Stable Well-Balanced Scheme with Hydrostatic
    Reconstruction for Shallow Water Flows_ (SIAM J. Sci. Comput. 2004) - the simulated swash's wetting and drying
-   Whitlock, Bartlett & Gurganus, _Sea Foam Reflectance_ (GRL 1982) - foam albedo
-   Bobbing Framework (RavenKZP, GPL-3.0-or-later WITH Modding Exception) - moving placed references with
    keyframed collision, their children and the actors standing on them; the floating objects build on it
-   Faltinsen, _Sea Loads on Ships and Offshore Structures_ (1990) - heave and roll natural periods of a hull
