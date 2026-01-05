# Mesh-Specific Shader Targeting System

## Problem Statement

Currently, there is no clear way to implement specific shaders on specific meshes. For example:
- Terrain variation on the top of dirt cliff meshes
- Subsurface scattering on candles
- Custom material profiles on modded content with unconventional naming schemes

## Current State

The codebase currently has **no unified system** for applying specific shaders to specific meshes. Different features use different approaches:

| Feature | Targeting Method | Limitations |
|---------|-----------------|-------------|
| **TruePBR** | Editor ID matching via JSON configs in `Data\PBRTextureSets\` and `Data\PBRMaterialObjects\` | Only works for TextureSets/MaterialObjects, not meshes directly |
| **ExtendedTranslucency** | `NiExtraData` attached to geometry (`NiIntegerExtraData` with name `AnisotropicAlphaMaterial`) | Requires modifying NIFs |
| **SubsurfaceScattering** | "Human profile" vs "Base profile" (hardcoded shader distinction) | No per-mesh control |
| **Terrain Variation** | Shader type detection (landscape materials) | Only terrain, no mesh targeting |

## Available Data at Render Time

From `BSRenderPass` during `SetupGeometry`, we have access to:

### 1. Geometry Data (`pass->geometry` - `BSGeometry*`)
- `geometry->name` - NIF node name (e.g., `"Candle01"`)
- `geometry->GetUserData()` - Returns `TESObjectREFR*` for the reference
- `geometry->GetExtraData(name)` - Custom NIF extra data

### 2. Material Data (`pass->shaderProperty` - `BSShaderProperty*`)
- `shaderProperty->material` → `BSLightingShaderMaterialBase*`
- Material's texture paths via `GetTexturePath()`
- TextureSet via `GetTextureSet()`

### 3. Reference/Form Chain (when `geometry->GetUserData()` succeeds)
- `userData->GetObjectReference()` → Base object
- `baseObject->GetFormEditorID()` - Editor ID
- `baseObject->As<TESModel>()->GetModel()` - Model path

---

## Proposed Solution: Unified Mesh Shader Targeting System

### Architecture Overview

A **three-tier targeting system** that supports multiple identification methods with fallbacks:

```
┌─────────────────────────────────────────────────────────────────┐
│                    MeshShaderRuleManager                        │
├─────────────────────────────────────────────────────────────────┤
│  JSON Config Files (Data\ShaderRules\*.json)                    │
│  ├── Per-feature rules                                          │
│  ├── Global rules                                                │
│  └── Mod-provided rules                                         │
├─────────────────────────────────────────────────────────────────┤
│  Runtime Resolution (during SetupGeometry)                      │
│  ├── 1. NiExtraData (explicit per-mesh, highest priority)      │
│  ├── 2. Editor ID match                                         │
│  ├── 3. Texture path patterns                                   │
│  ├── 4. Model path patterns                                     │
│  └── 5. Node name patterns (fallback for non-vanilla)          │
└─────────────────────────────────────────────────────────────────┘
```

---

## Tier 1: Explicit NiExtraData (Modder-Controlled)

For modders who want explicit control, attach `NiStringExtraData` or `NiIntegerExtraData` to mesh nodes in NIFSkope:

```
ExtraData Name: "CS_ShaderProfile"
Value: "SubsurfaceScattering:Candle" or "TerrainVariation:DirtCliff"
```

**Pros**: Most reliable, modder-controlled, works with any mesh  
**Cons**: Requires NIF editing

---

## Tier 2: Configuration-Based Pattern Matching

JSON configuration files that support multiple matching strategies.

### File Structure

Location: `Data\ShaderRules\{FeatureName}.json` or `Data\ShaderRules\{ModName}_{FeatureName}.json`

### Example Configuration

```json
{
  "version": "1.0",
  "feature": "SubsurfaceScattering",
  "rules": [
    {
      "name": "Vanilla Candles",
      "profile": "Candle",
      "priority": 100,
      "match": {
        "editorId": ["Candle01", "Candle02", "CandleHorn*"],
        "texturePath": ["*candle*_d.dds", "*wax*_d.dds"],
        "modelPath": ["meshes/clutter/*candle*.nif"]
      },
      "settings": {
        "subsurfaceOpacity": 0.8,
        "subsurfaceColor": [1.0, 0.9, 0.7]
      }
    },
    {
      "name": "Skin Materials",
      "profile": "Human",
      "priority": 50,
      "match": {
        "texturePath": ["*skin*_d.dds", "*body*_d.dds", "*hand*_d.dds"],
        "shaderFlags": ["kSkinned"]
      }
    },
    {
      "name": "Dirt Cliff Meshes",
      "profile": "TerrainVariation",
      "priority": 75,
      "match": {
        "modelPath": ["meshes/landscape/rocks/*dirt*cliff*.nif"],
        "nodeName": ["*Cliff*", "*DirtRock*"],
        "textureSet": ["LDirtCliffs01*", "LRoadDirt*"]
      }
    }
  ]
}
```

### Match Criteria Options

| Field | Description | Example |
|-------|-------------|---------|
| `editorId` | Form Editor ID patterns | `["Candle*", "CandleHorn01"]` |
| `modelPath` | NIF file path patterns | `["meshes/clutter/*candle*.nif"]` |
| `texturePath` | Diffuse texture path patterns | `["*wax*_d.dds"]` |
| `nodeName` | NIF node name patterns | `["*Candle*"]` |
| `textureSet` | TextureSet Editor ID patterns | `["CandleTextures*"]` |
| `shaderFlags` | Required BSShaderProperty flags | `["kSkinned", "kTreeAnim"]` |

---

## Tier 3: Runtime API for Dynamic Rules

Allow features and other mods to register rules at runtime.

### Core Classes

```cpp
class MeshShaderRuleManager {
public:
    struct MatchCriteria {
        std::vector<std::string> editorIdPatterns;    // Glob patterns for form editor IDs
        std::vector<std::string> modelPathPatterns;   // Glob patterns for .nif paths
        std::vector<std::string> texturePathPatterns; // Glob patterns for texture paths
        std::vector<std::string> nodeNamePatterns;    // Glob patterns for NIF node names
        std::vector<std::string> textureSetPatterns;  // Glob patterns for texture set editor IDs
        uint64_t requiredShaderFlags = 0;
        uint64_t forbiddenShaderFlags = 0;
    };
    
    struct Rule {
        std::string name;
        std::string featureName;
        std::string profileName;
        MatchCriteria match;
        int priority = 0;
        json settings;  // Feature-specific settings
    };
    
    // Registration
    void RegisterRule(const Rule& rule);
    void LoadRulesFromFile(const std::filesystem::path& path);
    void DiscoverAllRules();  // Scan Data\ShaderRules\
    
    // Resolution (called during SetupGeometry)
    struct ResolvedRule {
        std::string profileName;
        json settings;
        bool matched = false;
    };
    ResolvedRule ResolveForGeometry(
        const std::string& featureName,
        RE::BSGeometry* geometry,
        RE::BSShaderProperty* property
    );
    
    // Caching for performance
    void InvalidateCache();
    
private:
    // Pattern matching utilities
    bool MatchGlob(std::string_view pattern, std::string_view text);
    std::optional<std::string> GetEditorIdForGeometry(RE::BSGeometry* geometry);
    std::optional<std::string> GetModelPathForGeometry(RE::BSGeometry* geometry);
    std::vector<std::string> GetTexturePathsForProperty(RE::BSShaderProperty* property);
    
    // Cached lookups (cleared on cell change or reload)
    std::unordered_map<RE::BSGeometry*, ResolvedRule> geometryCache;
};
```

---

## Integration with Features

Each feature hooks into the system via a standard interface:

```cpp
// In SubsurfaceScattering.cpp
void SubsurfaceScattering::BSLightingShader_SetupGeometry(RE::BSRenderPass* pass)
{
    auto& ruleManager = MeshShaderRuleManager::GetSingleton();
    auto resolved = ruleManager.ResolveForGeometry(
        "SubsurfaceScattering",
        pass->geometry,
        static_cast<RE::BSShaderProperty*>(pass->shaderProperty)
    );
    
    if (resolved.matched) {
        // Apply profile-specific settings
        if (resolved.profileName == "Candle") {
            ApplyCandleProfile(resolved.settings);
        } else if (resolved.profileName == "Human") {
            ApplyHumanProfile(resolved.settings);
        }
        // Set shader descriptor flag
        SetShaderProfile(resolved.profileName);
    }
}
```

---

## Performance Considerations

1. **Geometry Caching**: Cache resolved rules per `BSGeometry*` pointer, invalidate on cell load
2. **Compiled Patterns**: Pre-compile glob patterns at load time using optimized matching
3. **Early Exit**: Check NiExtraData first (O(1) lookup), fall through to pattern matching only if not found
4. **Hierarchical Matching**: Match by texture path (most common) before attempting model path (requires form lookup)

---

## Supporting Non-Vanilla Meshes

The multi-tier approach handles unconventional naming:

1. **NiExtraData**: Modders add explicit data - works for anything
2. **Texture Patterns**: `*wax*`, `*candle*` catches most candle textures regardless of mesh name
3. **Node Name Patterns**: Fallback for meshes where only the NIF internal node names are consistent
4. **Mod-Specific Rule Files**: Modders can ship `{ModName}_SubsurfaceScattering.json` with their mod

---

## Example: Terrain Variation on Dirt Cliffs

```json
{
  "feature": "TerrainVariation",
  "rules": [
    {
      "name": "Vanilla Dirt Cliffs",
      "profile": "DirtCliff",
      "match": {
        "editorId": ["LDirtCliffs*", "MountainCliff*Dirt*"],
        "modelPath": ["meshes/landscape/rocks/*dirt*cliff*.nif", "meshes/landscape/mountains/*dirt*.nif"]
      },
      "settings": {
        "enableTilingFix": true,
        "blendStrength": 0.7
      }
    },
    {
      "name": "All Cliff Textures (Fallback)",
      "profile": "GenericCliff",
      "priority": -10,
      "match": {
        "texturePath": ["*cliff*_d.dds", "*rock*dirt*_d.dds"]
      },
      "settings": {
        "enableTilingFix": true,
        "blendStrength": 0.5
      }
    }
  ]
}
```

---

## Implementation Roadmap

| Phase | Description | Status |
|-------|-------------|--------|
| **Phase 1** | Core `MeshShaderRuleManager` class with JSON loading and pattern matching | 🔄 In Progress |
| **Phase 2** | Integrate with one feature (Subsurface Scattering as proof of concept) | ⏳ Planned |
| **Phase 3** | Add geometry caching and performance optimizations | ⏳ Planned |
| **Phase 4** | Roll out to other features (Terrain Variation, Extended Materials, etc.) | ⏳ Planned |
| **Phase 5** | Documentation and modder tools (rule validation, debugging overlays) | ⏳ Planned |

---

## Files to Create/Modify

| File | Purpose |
|------|---------|
| `src/MeshShaderRuleManager.cpp` | Core system implementation |
| `src/MeshShaderRuleManager.h` | Core system header |
| `src/Utils/GlobMatcher.cpp` | Pattern matching utilities |
| `src/Utils/GlobMatcher.h` | Pattern matching header |
| `package/ShaderRules/SubsurfaceScattering.json` | Default SSS rules |
| `package/ShaderRules/TerrainVariation.json` | Default terrain rules |
| Feature files | Hook integration |

---

## Benefits

- ✅ Works out-of-box for vanilla content via shipped JSON configs
- ✅ Supports modded content through texture/model pattern matching
- ✅ Allows explicit per-mesh control via NiExtraData for edge cases
- ✅ Enables modders to ship their own rule files
- ✅ Clear upgrade path from current hardcoded approaches
- ✅ Performance-optimized with caching
- ✅ Extensible for future features
