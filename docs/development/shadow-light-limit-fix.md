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
| **A: Patch comparison** | Find `cmp reg, 4` / `jge` and change immediate to 8 | Minimal code change | Fragile across versions |
| **B: Replace function** | Hook the entire accumulation function | Full control | Must reimplement loop |
| **C: Detour Accumulate** | Let vanilla run, then extend | Compat-safe | Complex state management |

**Recommended: Approach B** — Hook the accumulation function and re-implement the loop with
the higher limit. This gives full control and is the most maintainable approach.

**Relocation research needed:**
```
SE:  The accumulation loop is in Main::DrawWorld — find the sub-function that
     iterates shadowLightsAccum. Look for references to DrawWorld::shadowLightMaskIndex.
     Candidates: REL::ID(35560), offsets around the shadowmap rendering section.

AE:  Corresponding AE relocation.

VR:  Corresponding VR relocation (may differ in structure due to stereo rendering).
```

#### Hook 2: Shadow Mask Render Target Redirect

**Target:** The point where `BSUtilityShader` renders shadow masks.

**What to change:**
- For `maskIndex` 0–3: vanilla render to RGBA channels (unchanged)
- For `maskIndex` 4–7: swap render target to our `Texture2DArray` slice

**Implementation:**
```cpp
// During the shadow mask render pass for each light:
void BeforeShadowmaskRender(uint32_t maskIndex) {
    if (maskIndex >= 4) {
        // Save current RT
        context->OMGetRenderTargets(1, &savedRT, &savedDSV);
        // Bind our extended slice
        context->OMSetRenderTargets(1, &sliceRTVs[maskIndex], savedDSV);
    }
}

void AfterShadowmaskRender(uint32_t maskIndex) {
    if (maskIndex >= 4) {
        // Restore vanilla RT
        context->OMSetRenderTargets(1, &savedRT, savedDSV);
    }
}
```

**Note:** We also need to handle the utility shader's output write mask. The vanilla shader
uses `maskIndex` to select which RGBA channel to write. For extended slots, we need the
shader to always write to `.r` (since each array slice is R-only). This may require:
- A shader define/permutation for extended shadow mask mode
- Or: patching the output write mask in the blend state

#### Hook 3: Shadow Mask Clear

**Target:** Where the engine clears `kSHADOW_MASK` before the shadow pass.

**What to change:** Also clear our extended array slices to 1.0 (fully lit = no shadow).

#### Hook 4: Bind Extended Shadow Mask SRV

**Target:** After shadow mask rendering, before the lighting pass.

**What to change:** Bind `ExtendedShadowMaskArray` SRV to shader register `t48` so the
lighting shaders can sample from it.

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

### Step 1: Relocation Research (Critical Path)
- [ ] Identify the exact function containing the shadow light accumulation loop
- [ ] Find the `cmp reg, 4` instruction and surrounding context
- [ ] Map relocations for SE, AE, and VR
- [ ] Verify `BSShadowLight::Accumulate()` behavior when maskIndex > 3

### Step 2: Engine Fix — Accumulation Hook
- [ ] Implement `ShadowLightLimitFix::Hooks::DrawWorld_AccumulateShadowLights`
- [ ] Test that maskIndex values 4+ are correctly assigned
- [ ] Verify no engine crashes from extended maskIndex values

### Step 3: Extended Shadow Mask Resources
- [ ] Create Texture2DArray and per-slice RTVs
- [ ] Hook shadow mask clear to also clear extended slices
- [ ] Bind extended SRV to lighting shader

### Step 4: Shadow Mask Render Redirect
- [ ] Hook BSUtilityShader shadow mask render pass
- [ ] Redirect maskIndex 4+ to array slices
- [ ] Handle utility shader output write mask (R-only for extended)

### Step 5: LLF Integration
- [ ] Update ShadowBitMask population for extended range
- [ ] Update shader to use ExtendedShadowMask::GetShadow()
- [ ] Update clustered light buffer shadow data

### Step 6: Deferred Pipeline
- [ ] Extend CopyShadowData for additional shadow lights
- [ ] Update PerGeometry struct and shader cbuffer layout
- [ ] Test with deferred rendering pipeline

### Step 7: Testing & Optimization
- [ ] Test in shadow-heavy interiors (Blue Palace, Dragonsreach)
- [ ] Performance profiling with 4/6/8 shadow lights
- [ ] VR compatibility testing
- [ ] Shadow transition smoothing

## Alternative Approaches Considered

### A: Virtual Shadow Maps (VSM)
Replace the entire shadow system with a virtual shadow map (like UE5). This would remove the
per-light limit entirely but is a massive undertaking requiring a complete shadow system rewrite.

### B: Shadow Map Atlas
Instead of separate shadow maps per light, pack all shadow maps into a single large atlas
texture. The shadow mask would then be replaced by direct atlas lookups in the lighting shader.
This is cleaner but requires rewriting the shadow map rendering pipeline.

### C: Tiled Shadow Mask
Instead of RGBA channels, use a tiled approach where different screen regions can have different
shadow light assignments. This is used in some deferred renderers but doesn't fit Skyrim's
forward+ architecture well.

### D: Temporal Shadow Cycling
Cycle shadow lights across frames — render 4 lights in frame N, 4 different lights in frame N+1,
and blend the results. This doubles effective capacity with minimal GPU cost but introduces
temporal artifacts and requires careful state management.

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
