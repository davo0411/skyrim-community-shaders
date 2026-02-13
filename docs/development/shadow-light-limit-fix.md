# Shadow Casting Light Limit Fix — Design Document

## Problem Statement

Skyrim's Creation Engine has a hard limit of **4 simultaneous shadow-casting lights** in any
given scene. When more than 4 lights should cast shadows, the engine silently discards the
extras, causing visible shadow pop-in/pop-out as the player moves and lights compete for the
limited shadow slots.

This limit manifests as:
- Interior scenes with 5+ torches/fires where only the closest 4 cast shadows
- Shadow "popping" when crossing cell boundaries or moving between light sources
- Modded content (lanterns, candles, magical effects) losing shadows unpredictably

## Root Cause Analysis

The limit stems from **5 interacting engine constraints**:

### 1. Shadow Mask Render Target — RGBA = 4 Channels

The engine's shadow mask (`kSHADOW_MASK`) is a single **RGBA texture**. Each channel stores the
shadow occlusion factor for exactly one shadow-casting light:

| Channel | Typical Use                     |
|---------|--------------------------------|
| R (.x)  | Directional light (sun)        |
| G (.y)  | Point/spot shadow light #1     |
| B (.z)  | Point/spot shadow light #2     |
| A (.w)  | Point/spot shadow light #3     |

**File:** `RE::RENDER_TARGETS::kSHADOW_MASK` — Format: `DXGI_FORMAT_R8G8B8A8_UNORM`

### 2. BSShadowLight::maskIndex — Channel Assignment (0–3)

Each shadow-casting light has a `maskIndex` member (offset 0x520) that identifies which RGBA
channel of the shadow mask this light writes to:

```cpp
// CommonLibSSE-NG: include/RE/B/BSShadowLight.h
class BSShadowLight : public BSLight {
    std::uint32_t maskIndex;           // 520 — values 0, 1, 2, or 3
    std::uint32_t accumulatedIndex;    // 524
    // ...
};
```

### 3. DrawWorld — Accumulation Counter (cycles 0→3)

During the shadow accumulation phase, `DrawWorld` tracks global state:

```cpp
// CommonLibSSE-NG: include/RE/D/DrawWorld.h
struct DrawWorld {
    std::uint32_t activeShadowLightCount;   // 40
    std::uint32_t shadowLightCount;         // 44
    std::uint32_t shadowLightMaskIndex;     // 48 — increments 0,1,2,3 then stops
    std::uint32_t shadowLightMask;          // 4C — bitmask of used channels
    // ...
};
```

The engine iterates `ShadowSceneNode::shadowLightsAccum` calling
`BSShadowLight::Accumulate()` on each. The virtual:

```cpp
virtual void Accumulate(uint32_t& globalCount, uint32_t& maskChannel, NiPointer<NiAVObject> scene) = 0;
```

...assigns `this->maskIndex = maskChannel++`. The **calling loop** exits when
`maskChannel >= 4`.

### 4. BSUtilityShader — Shadow Mask Write Pass

The engine renders each shadow light's depth comparison into the shadow mask using
`BSUtilityShader` with technique flags:

| Flag                      | Value     | Purpose                        |
|---------------------------|-----------|--------------------------------|
| `RenderShadowmask`       | `1 << 21` | Directional (sun) shadow mask  |
| `RenderShadowmaskSpot`   | `1 << 22` | Frustum/spotlight shadow mask  |
| `RenderShadowmaskPb`     | `1 << 23` | Parabolic shadow mask          |
| `RenderShadowmaskDpb`    | `1 << 24` | Dual-parabolic shadow mask     |

The shader uses `maskIndex` to select which channel to write: output write mask maps
maskIndex 0→R, 1→G, 2→B, 3→A.

### 5. Lighting Shader — Shadow Mask Sampling

In the pixel shader (`Lighting.hlsl`), shadow data is consumed:

```hlsl
// Vanilla path:
uint numShadowLights = min(4, uint(NumLightNumShadowLight.y));  // Hard-capped
float4 shadowColor = TexShadowMaskSampler.Sample(...);          // RGBA = 4 channels

// Per light:
if (lightIndex < numShadowLights) {
    lightShadow *= shadowColor[ShadowLightMaskSelect[lightIndex]]; // Index 0-3
}

// LLF clustered path:
if (light.lightFlags & Shadow) {
    shadowComponent = shadowColor[light.shadowLightIndex];  // Still 0-3
}
```

## Current State: How Light Limit Fix Handles This

The existing **Light Limit Fix (LLF)** feature in Community Shaders removes the vanilla
**4-total-light limit** (up to 7 lights per draw call in vanilla) by implementing a GPU-side
**clustered lighting** system that supports 1024+ lights per scene.

However, **LLF does NOT increase the shadow-casting light limit**. It still reads from the
same 4-channel shadow mask:

```cpp
// LightLimitFix.cpp — UpdateLights()
if (bsLight->IsShadowLight()) {
    auto* shadowLight = static_cast<RE::BSShadowLight*>(bsLight);
    GET_INSTANCE_MEMBER(maskIndex, shadowLight);
    light.shadowMaskIndex = maskIndex;        // Still 0-3
    light.lightFlags.set(LightFlags::Shadow);
}
```

```hlsl
// LightLimitFix.hlsli — IsLightIgnored()
if (light.lightFlags & Shadow) {
    return !(ShadowBitMask & (1 << light.shadowLightIndex));  // 4-bit mask
}
```

## Proposed Solution: Extended Shadow Mask System

### Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                    Engine Hooks (C++)                     │
│                                                          │
│  ┌─────────────────────┐  ┌───────────────────────────┐ │
│  │ Accumulation Hook   │  │ Shadow Mask Render Hook    │ │
│  │                     │  │                            │ │
│  │ • Raise loop limit  │  │ • maskIndex 0–3: vanilla   │ │
│  │   from 4 → N        │  │ • maskIndex 4+: redirect   │ │
│  │ • Let Accumulate()  │  │   to Texture2DArray slice  │ │
│  │   assign maskIndex  │  │                            │ │
│  │   0 to N-1          │  │                            │ │
│  └─────────────────────┘  └───────────────────────────┘ │
│                                                          │
│  ┌─────────────────────────────────────────────────────┐ │
│  │ Resource Management                                  │ │
│  │                                                      │ │
│  │ • Texture2DArray: screenW × screenH × N slices      │ │
│  │ • Per-slice RTVs for rendering                       │ │
│  │ • SRV for sampling in lighting shaders               │ │
│  │ • Sync slices 0–3 with vanilla RGBA each frame      │ │
│  └─────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│                   Shader Changes (HLSL)                   │
│                                                          │
│  ┌─────────────────────────────────────────────────────┐ │
│  │ ExtendedShadowMask.hlsli                            │ │
│  │                                                      │ │
│  │ GetShadow(screenPos, shadowColor, maskIndex):       │ │
│  │   if maskIndex < 4: return shadowColor[maskIndex]   │ │
│  │   else: return ExtendedArray.Load(pos, maskIndex)   │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                          │
│  ┌─────────────────────────────────────────────────────┐ │
│  │ LightLimitFix.hlsli updates                         │ │
│  │                                                      │ │
│  │ • ShadowBitMask: uint → supports N bits             │ │
│  │ • shadowLightIndex: values 0 to N-1                 │ │
│  │ • Replace shadowColor[idx] with GetShadow()         │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                          │
│  ┌─────────────────────────────────────────────────────┐ │
│  │ Lighting.hlsl updates                                │ │
│  │                                                      │ │
│  │ • LLF path: use ExtendedShadowMask::GetShadow()    │ │
│  │ • Vanilla path: unchanged (still 4-channel RGBA)    │ │
│  └─────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

### Phase 1: Engine Hooks (C++ — `ShadowLightLimitFix`)

**File:** `src/EngineFixes/ShadowLightLimitFix.h/.cpp`

#### Hook 1: Shadow Light Accumulation Loop

**Target:** The loop in `Main::DrawWorld` (or its sub-function) that iterates
`ShadowSceneNode::shadowLightsAccum` and calls `BSShadowLight::Accumulate()`.

**What to change:**
- The vanilla loop exits when `shadowMaskChannel >= 4`
- We change this to exit when `shadowMaskChannel >= kMaxShadowLights` (default 8)

**Implementation options:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| **A: Patch comparison** | Find `cmp reg, 4` / `jge` and change immediate to 8 | Minimal code change | Fragile across versions; still need RT redirect |
| **B: Replace function** | Hook the entire accumulation function | Full control | Must reimplement loop; needs relocation research |
| **C: Detour Accumulate** | Let vanilla run 4 lights, then extend post-hoc | Compat-safe; uses existing hooks | Must call `Accumulate()`+`Render()` ourselves |
| **D: Screen-space shadows** | No engine hooks; SS ray-march for overflow lights | Zero engine interaction | Contact-only shadows, no self-shadowing |
| **E: Temporal cycling** | Rotate lights through 4 slots across frames | No shadow mask changes needed | Temporal artifacts, complex slot management |
#### Approach C: Detour Accumulate — Detailed Analysis (Recommended)

The key insight is that the engine's shadow pipeline runs in **strict sequential batch phases**:

```
Phase 1: ACCUMULATE ALL    → iterates shadowLightsAccum, assigns maskIndex 0–3, exits at ≥ 4
Phase 2: RENDER ALL SHADOW MAPS  → calls BSShadowLight::Render() for each accumulated light
Phase 3: RENDER ALL SHADOW MASKS → BSUtilityShader writes to kSHADOW_MASK RGBA per light
Phase 4: LIGHTING          → pixel shaders read shadow mask
```

This is **batched, NOT interleaved** per-light. Evidence:
- `Main_RenderShadowMaps` fires once (already hooked in `Deferred.cpp`) wrapping ALL shadow maps
- `Main_RenderShadowmasks` fires once wrapping ALL mask passes
- Shadow map virtuals (`BSShadowLight::Render()`) fire INSIDE phase 2, not interleaved with phase 3

Because phases are sequential, we can **inject between Phase 1 and Phase 2** to handle extra lights.

##### C3: Hybrid Detour (Safest, Recommended)

Let vanilla handle its 4 lights end-to-end, then render extras ourselves:

1. Vanilla runs phases 1–3 normally (4 lights accumulated, shadow maps rendered, masks written)
2. After `Main_RenderShadowMaps` completes (hook already exists in `Deferred.cpp`),
   scan `activeShadowLights` for lights where `maskIndex == 255` (engine sentinel for "not accumulated")
3. For each extra light:
   a. Call `Accumulate()` virtual to set up internal state (camera, culling, descriptors)
   b. Call `Render()` virtual to generate its shadow depth map
4. During `Main_RenderShadowmasks`, intercept the utility shader to render
   each extra light's shadow mask to our `Texture2DArray` slices
5. Bind the extended array SRV for the lighting shader

**Implementation using existing hooks:**

```cpp
// In Deferred::Hooks::Main_RenderShadowMaps::thunk():
void thunk() {
    func();  // Vanilla: accumulate + render shadow maps for 4 lights

    // --- OUR EXTENSION ---
    auto* ssn = globals::game::smState->shadowSceneNode[0];
    auto& shadowLights = ssn->GetRuntimeData().activeShadowLights;

    uint32_t extendedChannel = 4;  // Start after vanilla's 4 slots

    for (auto& lightPtr : shadowLights) {
        if (extendedChannel >= kMaxShadowLights) break;

        auto* light = static_cast<RE::BSShadowLight*>(lightPtr.get());
        GET_INSTANCE_MEMBER(maskIndex, light);

        if (maskIndex == 255) {  // Engine sentinel: "not accumulated"
            // This light was skipped by vanilla — handle it ourselves
            uint32_t dummyGlobalCount = extendedChannel;
            NiPointer<NiAVObject> scene = ssn; // world root

            light->Accumulate(dummyGlobalCount, extendedChannel, scene);
            light->Render();  // Generate shadow depth map
        }
    }

    globals::deferred->EarlyPrepasses();  // Existing CS code
}
```

**Why this is safe:**
- We call `Accumulate()` AFTER vanilla's loop is done — no interference
- `Accumulate()` is a well-defined virtual: it assigns `this->maskIndex = maskChannel++`
  and sets up shadowmap descriptors, culling cameras, etc.
- `Render()` uses the state set up by `Accumulate()` — it renders depth from the light's POV
- Values `maskIndex = 4, 5, 6, 7` are unused by vanilla — no conflicts
- Existing hooks (`Main_RenderShadowMaps`, `BSUtilityShader`) give us all needed injection points

**Risk:** `Accumulate()` might have internal assumptions about `maskChannel < 4` (e.g., indexing
into `focusShadowmapDescriptors[4]`). However, that array is for **cascade splits** on the
directional light, not for mask channel selection — point lights use `shadowmapDescriptors`
(a `BSTArray`, dynamically sized). Point light `Accumulate()` implementations should be safe.

**Hook surface for Approach C3:**

| Hook | Location | Already In CS? | Used For |
|------|----------|----------------|----------|
| `Main_RenderShadowMaps` | `Deferred.cpp` | ✅ Yes | Inject after accumulation, before lighting |
| `Main_RenderShadowmasks` | `FrameAnnotations.cpp` | ✅ Yes (perf event) | Intercept shadow mask writes for extra lights |
| `BSUtilityShader` dispatch | `State.cpp` | ✅ Yes (`CopyShadowData`) | Redirect RT for maskIndex ≥ 4 |
| `BSLightingShader_SetupGeometry` | `LightLimitFix.h` | ✅ Yes | Read extended shadow data |

**New hooks needed:**

| Hook | Purpose | Difficulty |
|------|---------|------------|
| Shadow mask clear | Clear extended array slices to 1.0 | Easy — extend `Main_RenderShadowmasks` |
| Bind extended SRV | Attach `t48` for extended shadow array | Easy — in LLF `Prepass()` |

##### Shadow Mask Render Redirect (for maskIndex ≥ 4)

For extra lights, the utility shader's shadow mask pass needs output redirected:

| Method | Description | Complexity |
|--------|-------------|------------|
| **Blend state swap** | Set `RenderTargetWriteMask = R` and swap to array slice RTV | Low |
| **Shader permutation** | Add `EXTENDED_SHADOW_SLOT` define writing to `.r` only | Medium |
| **Our own compute pass** | Skip BSUtilityShader; do shadow comparison in compute | Medium |

The blend state swap is simplest: before rendering a shadow mask for maskIndex ≥ 4,
save the current RT, bind our array slice RTV, and force write mask to R-only.

```cpp
// Pseudocode in the BSUtilityShader shadowmask hook:
uint32_t currentMaskIndex = GetCurrentShadowLightMaskIndex();
if (currentMaskIndex >= 4) {
    context->OMGetRenderTargets(1, &savedRT, &savedDSV);
    context->OMSetRenderTargets(1, &sliceRTVs[currentMaskIndex], savedDSV);
    func(shader, technique);  // Render shadow mask to our slice
    context->OMSetRenderTargets(1, &savedRT, savedDSV);
} else {
    func(shader, technique);  // Vanilla path
}
```

---

#### Approach D: No Engine Hooks — Screen-Space Shadows for Overflow Lights

The most decoupled approach: completely bypass the engine's shadow pipeline for extra lights
and use **screen-space shadow ray-marching** instead. Community Shaders already implements
Screen-Space Shadows for the directional light (`ScreenSpaceShadows.cpp`, `t45`).

**How it works:**

1. During LLF's `UpdateLights()`, identify shadow-capable lights where `maskIndex == 255`
2. For each, dispatch a compute shader that ray-marches from each pixel toward the light
   position through the depth buffer
3. Store results in a `Texture2DArray` (one slice per extra shadow light)
4. In the lighting shader, for lights with `shadowLightIndex >= 4`, sample from the
   screen-space shadow buffer instead of the shadow mask

**Pros:**
- **Zero engine hooks** — no interaction with the accumulation/rendering pipeline at all
- Simpler implementation — compute shader + depth buffer (already available infrastructure)
- No shadow map memory overhead for extra lights
- Reuses existing SSS patterns in Community Shaders

**Cons:**
- **Contact shadows only** — can't shadow objects behind the camera or off-screen
- No self-shadowing for geometry in the light's shadow volume
- Ray marching quality depends on depth buffer resolution
- Each extra light adds a full-screen compute dispatch (~0.3ms each at 1080p)
- Not as visually accurate as real shadow maps

**Best suited for:** A pragmatic first implementation that provides "good enough" shadows
for overflow lights without any engine-side risk. Can be upgraded to Approach C later.

```hlsl
// Compute shader: ScreenSpacePointShadow.hlsl
[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    float depth = DepthBuffer.Load(int3(DTid.xy, 0)).x;
    float3 worldPos = ReconstructWorldPos(DTid.xy, depth);

    float shadow = 1.0;
    float3 toLight = LightPosition - worldPos;
    float lightDist = length(toLight);
    float3 rayDir = toLight / lightDist;

    // March from surface toward light through depth buffer
    float stepSize = lightDist / NUM_STEPS;
    for (uint i = 1; i <= NUM_STEPS; i++) {
        float3 samplePos = worldPos + rayDir * stepSize * i;
        float2 sampleUV = ProjectToScreen(samplePos);
        float sampleDepth = DepthBuffer.SampleLevel(LinearClamp, sampleUV, 0).x;
        float expectedDepth = LinearizeDepth(samplePos);

        if (sampleDepth < expectedDepth - bias) {
            shadow = 0.0;
            break;
        }
    }
    OutputShadow[uint3(DTid.xy, lightSlice)] = shadow;
}
```

---

#### Approach E: Temporal Shadow Cycling

Rotate which point lights occupy the 4 shadow slots across frames:

- Frame N: Lights A, B, C, D have shadow slots
- Frame N+1: Lights A, B, E, F have shadow slots
- Cached shadow from frame N is used for C and D in frame N+1
- Exponential moving average blends new and cached shadows

**Pros:** No shadow mask format changes, minimal GPU cost increase, doubles effective capacity.
**Cons:** Temporal ghosting, moving lights/player break the cache, complex slot management,
needs motion vectors for reprojection.

---

#### Recommended Implementation Strategy

| Phase | Approach | Risk | Effort | Visual Quality |
|-------|----------|------|--------|---------------|
| **Phase 1** (Quick win) | **D: Screen-space shadows** for overflow lights | Very low | Low | Good (contact only) |
| **Phase 2** (Proper fix) | **C3: Hybrid detour** using existing hooks | Medium | Medium | Excellent (real shadow maps) |
| **Phase 3** (Polish) | **E: Temporal cycling** for slot competition smoothing | Low | Medium | Excellent + smooth transitions |

### Phase 2: LLF C++ Changes

**File:** `src/Features/LightLimitFix.h/.cpp`

#### Update StrictLightDataCB

```cpp
// Current:
struct StrictLightDataCB {
    uint NumStrictLights;
    int RoomIndex;
    uint ShadowBitMask;     // Only 4 bits used (0-3)
    uint pad0;
    LightData StrictLights[15];
};

// Updated:
struct StrictLightDataCB {
    uint NumStrictLights;
    int RoomIndex;
    uint ShadowBitMask;     // Now 8 bits used (0-7)
    uint pad0;
    LightData StrictLights[15];
};
// ShadowBitMask already supports 32 bits, just needs to be populated correctly.
```

#### Update BSLightingShader_SetupGeometry_GeometrySetupConstantPointLights

```cpp
// Current: only processes numShadowLights (engine-capped at 4)
for (uint32_t i = 0; i < a_pass->numShadowLights; i++) {
    auto bsLight = a_pass->sceneLights[i + 1];
    auto* shadowLight = static_cast<RE::BSShadowLight*>(bsLight);
    GET_INSTANCE_MEMBER(maskIndex, shadowLight);
    strictLightDataTemp.ShadowBitMask |= (1 << maskIndex);
}

// Updated: also check for extended shadow lights
// The BSRenderPass::numShadowLights is still engine-limited to 4,
// but we can iterate ALL shadow lights from the scene and build the
// ShadowBitMask from the clustered light list.
```

#### Update UpdateLights

```cpp
// Current:
if (bsLight->IsShadowLight()) {
    GET_INSTANCE_MEMBER(maskIndex, shadowLight);
    light.shadowMaskIndex = maskIndex;  // Values 0-3
}

// Updated:
// maskIndex can now be 0-7. The LightData struct already stores it as uint32.
// No structural change needed, just ensure we don't filter on maskIndex < 4.
```

### Phase 3: Shader Changes

**File:** `features/Light Limit Fix/Shaders/LightLimitFix/ExtendedShadowMask.hlsli`

Already created — provides `ExtendedShadowMask::GetShadow()` that transparently samples from
either the vanilla RGBA texture (maskIndex 0–3) or the extended array (maskIndex 4+).

**File:** `package/Shaders/Lighting.hlsl`

The LLF path in the lighting shader needs to be updated:

```hlsl
// Current (LLF path):
if (light.lightFlags & LightLimitFix::LightFlags::Shadow) {
    shadowComponent = shadowColor[light.shadowLightIndex];  // Only 0-3 valid
    lightShadow *= shadowComponent;
}

// Updated:
#if defined(EXTENDED_SHADOW_MASK)
if (light.lightFlags & LightLimitFix::LightFlags::Shadow) {
    shadowComponent = ExtendedShadowMask::GetShadow(
        input.Position.xy, shadowColor, light.shadowLightIndex);
    lightShadow *= shadowComponent;
}
#else
if (light.lightFlags & LightLimitFix::LightFlags::Shadow) {
    shadowComponent = shadowColor[light.shadowLightIndex];
    lightShadow *= shadowComponent;
}
#endif
```

### Phase 4: Deferred Pipeline Updates

**File:** `src/Deferred.cpp`

The `CopyShadowData()` function copies shadow projection matrices during the utility shader
shadowmask pass. It currently captures data for 4 shadow lights. It may need to be extended
to capture data for up to 8 shadow lights.

The `PerGeometry` struct in Deferred.h has:
```cpp
DirectX::XMFLOAT4X3 FocusShadowMapProj[4];       // 4 cascade splits (directional)
DirectX::XMFLOAT4X3 ShadowMapProj[2][3];          // 2 eyes × 3 shadow maps
DirectX::XMFLOAT4X3 CameraViewProjInverse[2];     // 2 eyes
```

The `ShadowMapProj[2][3]` stores projection matrices for the point light shadow maps. If we
increase to 7 point light shadows, this needs to grow to `ShadowMapProj[2][7]`. However, this
is part of a constant buffer and increasing it requires careful layout matching with the shader.

## Performance Considerations

### GPU Cost Per Additional Shadow Light

Each shadow-casting light requires:
1. **Shadow map render pass:** Geometry culling + depth rendering from the light's POV
   - Point lights: 6 faces (cubemap) or 2 passes (dual-paraboloid)
   - Spot lights: 1 pass (frustum)
2. **Shadow mask render pass:** Full-screen comparison against the shadow map
3. **Memory:** One array slice for the extended shadow mask

| Lights | Shadow Passes (point, dual-parab.) | Shadow Mask Memory (1080p) |
|--------|-----------------------------------|---------------------------|
| 4 (vanilla) | 1 dir + 6 point = 7 passes   | 8 MB (RGBA8)             |
| 6           | 1 dir + 10 point = 11 passes  | 12 MB (6 × R8 slices)    |
| 8           | 1 dir + 14 point = 15 passes  | 16 MB (8 × R8 slices)    |

### Recommendations

1. **Default to 6** (1 sun + 5 point) for a good balance
2. **Make configurable** via `iMaxShadowLights` INI setting
3. **Quality presets:** Low=4 (vanilla), Medium=6, High=8
4. **Distance-based priority:** Sort shadow lights by distance to camera, assign slots to closest
5. **Fade transitions:** When a light gains/loses its shadow slot, fade the shadow in/out over
   a few frames to avoid popping

## Implementation Roadmap

### Step 1: Screen-Space Point Light Shadows (Phase 1 — No Engine Hooks)
- [ ] Create `ScreenSpacePointShadow.hlsl` compute shader (ray-march depth buffer toward each light)
- [ ] In LLF `UpdateLights()`, identify lights where `maskIndex == 255` (overflow lights)
- [ ] Dispatch compute shader per overflow light, write to `Texture2DArray`
- [ ] Bind result as SRV `t48` in LLF `Prepass()`
- [ ] Update `ExtendedShadowMask::GetShadow()` to sample from it
- [ ] Test in shadow-heavy interiors

### Step 2: Engine Detour — Accumulate + Render Extra Lights (Phase 2)
- [ ] In `Main_RenderShadowMaps` thunk (existing hook), after `func()`:
      scan `activeShadowLights` for `maskIndex == 255` lights
- [ ] Call `Accumulate()` on each with counter starting at 4
- [ ] Call `Render()` on each to generate shadow depth maps
- [ ] Verify no engine crash from `maskIndex > 3` in `Accumulate()`
- [ ] Test that shadow maps are actually generated

### Step 3: Extended Shadow Mask Resources
- [ ] Create `Texture2DArray` (screenW × screenH × kMaxShadowLights slices, `R8_UNORM`)
- [ ] Create per-slice RTVs for rendering
- [ ] Hook shadow mask clear to also clear extended slices to 1.0
- [ ] Bind extended SRV to `t48` for lighting shaders

### Step 4: Shadow Mask Render Redirect
- [ ] In `BSUtilityShader` dispatch hook (State.cpp, already exists for `CopyShadowData`):
      detect when rendering shadow mask for `maskIndex >= 4`
- [ ] Save current RT, swap to our array slice RTV, render, restore RT
- [ ] Handle output write mask (blend state or shader permutation)

### Step 5: LLF Integration
- [ ] `ShadowBitMask` population: also set bits 4–7 for extended shadow lights
- [ ] Update shader: `ExtendedShadowMask::GetShadow()` for all shadow light lookups
- [ ] Remove `shadowMaskIndex != 255` exclusion for lights with extended slots
- [ ] Update `StrictLightDataCB` population for per-geometry shadow data

### Step 6: Deferred Pipeline
- [ ] Extend `CopyShadowData` for additional shadow lights (projection matrices)
- [ ] Update `PerGeometry::ShadowMapProj` array size and shader cbuffer layout
- [ ] Test with deferred rendering pipeline active

### Step 7: Temporal Cycling (Phase 3 — Polish)
- [ ] Implement priority queue for shadow light slot assignment
- [ ] Cache previous frame's shadow data per light
- [ ] Smooth shadow fade-in/out when lights gain/lose slots
- [ ] Reprojection for cached shadows when camera moves

### Step 8: Testing & Optimization
- [ ] Test in shadow-heavy interiors (Blue Palace, Dragonsreach, modded interiors)
- [ ] Performance profiling with 4/6/8 shadow lights
- [ ] VR compatibility testing (stereo shadow maps, doubled viewports)
- [ ] INI setting: `iMaxShadowLights` (user-configurable, default 6)
- [ ] Quality presets: Low=4 (vanilla), Medium=6, High=8

## Alternative Approaches Considered (Not Recommended)

### Virtual Shadow Maps (VSM)
Replace the entire shadow system with a virtual shadow map (like UE5). Would remove the
per-light limit entirely but is a massive undertaking requiring a complete shadow system rewrite.
Not practical as a Community Shaders feature — would essentially be writing a new renderer.

### Shadow Map Atlas
Pack all shadow maps into a single large atlas texture. The shadow mask would then be replaced
by direct atlas lookups in the lighting shader. Cleaner architecturally but requires rewriting
the shadow map rendering pipeline — too invasive for an engine fix.

### Tiled Shadow Mask
Use a tiled approach where different screen regions can have different shadow light assignments.
Used in some deferred renderers but doesn't fit Skyrim's forward+ architecture well.
The per-tile bookkeeping overhead likely exceeds the benefit for the typical 4→8 light increase.

## Files Created/Modified

### New Files
- `src/EngineFixes/ShadowLightLimitFix.h` — Engine fix header with hook declarations
- `src/EngineFixes/ShadowLightLimitFix.cpp` — Engine fix implementation
- `features/Light Limit Fix/Shaders/LightLimitFix/ExtendedShadowMask.hlsli` — Shader sampling header
- `docs/development/shadow-light-limit-fix.md` — This design document

### Files to Modify
- `src/EngineFix.cpp` — Register new engine fix in fix list
- `src/Features/LightLimitFix.cpp` — Update shadow bit mask and light processing
- `src/Features/LightLimitFix.h` — Update StrictLightDataCB if needed
- `features/Light Limit Fix/Shaders/LightLimitFix/LightLimitFix.hlsli` — Include ExtendedShadowMask
- `package/Shaders/Lighting.hlsl` — Update LLF shadow sampling path
- `src/Deferred.cpp` — Extend CopyShadowData for additional lights
- `src/Deferred.h` — Update PerGeometry struct
