#include "PBRWater.h"

#include "Globals.h"
#include "State.h"
#include "Util.h"
#include "RE/C/Calendar.h"
#include "TerrainShadows.h"

#include "Water/WaterTessellation.h"
#include "Water/WaterWaves.h"
#include "Water/WaterRipples.h"
#include "Water/WaterSettings.h"

#include <d3d11.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PBRWater::Settings,
	general,
	tessellation,
	waves,
	ripples,
	foam)

void PBRWater::LoadSettings(json& o_json)
{
	settings = o_json;
}

void PBRWater::SaveSettings(json& o_json)
{
	o_json = settings;
}

void PBRWater::RestoreDefaultSettings()
{
	settings = {};
}

void PBRWater::DrawSettings()
{
	if (ImGui::BeginTabBar("PBRWaterTabs")) {
		if (ImGui::BeginTabItem("General")) {
			ImGui::Checkbox("Use Optimised Meshes", &settings.general.UseOptimisedMeshes);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Uses meshes with lower tri-count for improved performance.\nRequires location change or restart.");
			}

			ImGui::Checkbox("Enable Tessellation", &settings.tessellation.EnableTessellation);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Hardware tessellation for dynamic mesh density based on distance.");
			}
			
			if (UnifiedWaterTessellation::AreShadersCompiling()) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "(Compiling shaders...)");
			} else if (!UnifiedWaterTessellation::AreShadersReady() && settings.tessellation.EnableTessellation) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "(Shader compilation failed)");
			}

			if (settings.tessellation.EnableTessellation) {
				ImGui::Indent();
				ImGui::SliderFloat("Min Distance", &settings.tessellation.TessellationMinDistance, 64.0f, 1024.0f, "%.0f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Distance where maximum tessellation is applied.");
				ImGui::SliderFloat("Max Distance", &settings.tessellation.TessellationMaxDistance, 1024.0f, 16384.0f, "%.0f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Distance where minimum tessellation is applied.");
				ImGui::SliderFloat("Max Factor", &settings.tessellation.TessellationMaxFactor, 4.0f, 64.0f, "%.0f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Tessellation factor for nearby water.\nHigher = more polygons.");
				ImGui::Unindent();
			}

			ImGui::Spacing();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Waves")) {
			UnifiedWaterWaves::DrawWaveSettings(settings.waves);
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Wading Ripples")) {
			UnifiedWaterRipples::DrawRippleSettings(settings.ripples);
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Foam")) {
			UnifiedWaterSettings::DrawFoamSettings(settings.foam);
			ImGui::EndTabItem();
		}
		
		if (ImGui::BeginTabItem("Debug")) {
			ImGui::Checkbox("Show Tri Visualizer", &settings.general.ShowWireframe);
			if (settings.general.ShowWireframe) {
				ImGui::SameLine();
				ImGui::Checkbox("Raw Barycentrics", &settings.general.WireframeRawMode);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Shows raw barycentric coordinates as RGB.");
				}
			}
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}

void PBRWater::SetupResources()
{
	perFrame = new ConstantBuffer(ConstantBufferDesc<PerFrame>());
	perTile = new ConstantBuffer(ConstantBufferDesc<PerTile>());
	actorRippleBuffer = new ConstantBuffer(ConstantBufferDesc<ActorRippleBuffer>());

	auto tessParamsBuffer = new ConstantBuffer(ConstantBufferDesc<TessellationParams>());
	UnifiedWaterTessellation::SetTessellationParamsBuffer(tessParamsBuffer);

	UnifiedWaterTessellation::CompileShadersAsync();
}

void PBRWater::ClearShaderCache()
{
	UnifiedWaterTessellation::GetHullShader() = nullptr;
	UnifiedWaterTessellation::GetDomainShader() = nullptr;
	UnifiedWaterTessellation::GetGeometryShader() = nullptr;
	UnifiedWaterTessellation::CompileShadersAsync();
}

void PBRWater::PostPostLoad()
{
	stl::write_vfunc<0x6, BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);
	stl::write_vfunc<0x7, BSWaterShader_RestoreGeometry>(RE::VTABLE_BSWaterShader[0]);
	logger::info("[PBR Water] Installed shader hooks");
}

void PBRWater::Reset()
{
	ResetTimingState();
}

void PBRWater::ResetTimingState()
{
	hasLastTimingSample = false;
	lastTimingFrameIndex = std::numeric_limits<std::uint32_t>::max();
	lastGameTimeHours = 0.0f;
	lastRealTimeSeconds = 0.0f;
	lastTimeScale = 1.0f;
	currentGameTimeHours = 0.0f;
	currentRealTimeSeconds = 0.0f;
	currentTimeScale = 1.0f;
	prevTileData.clear();
}

void PBRWater::UpdatePerFrameData(PerFrame& data, float waterSurfaceHeight)
{
	data.WaveIntensity = settings.waves.WaveIntensity;
	data.WaveAmplitude = settings.waves.WaveAmplitude;
	data.WaveSpeed = settings.waves.WaveSpeed;
	data.WaveSteepness = settings.waves.WaveSteepness;
	
	// WireframeEnabled: 0=off, 1=wireframe, 2=raw barycentrics debug
	data.WireframeEnabled = settings.general.ShowWireframe ? 
		(settings.general.WireframeRawMode ? 2.0f : 1.0f) : 0.0f;
	data.PerFramePad0 = 0.0f;
	data.PerFramePad1 = 0.0f;
	data.PerFramePad2 = 0.0f;
	
	// Wave parameters
	data.Wave1Amplitude = settings.waves.Wave1Amplitude;
	data.Wave1Wavelength = settings.waves.Wave1Wavelength;
	data.Wave1Steepness = settings.waves.Wave1Steepness;
	data.Wave1AngleOffset = settings.waves.Wave1AngleOffset;
	
	data.Wave2Amplitude = settings.waves.Wave2Amplitude;
	data.Wave2Wavelength = settings.waves.Wave2Wavelength;
	data.Wave2Steepness = settings.waves.Wave2Steepness;
	data.Wave2AngleOffset = settings.waves.Wave2AngleOffset;
	
	data.Wave3Amplitude = settings.waves.Wave3Amplitude;
	data.Wave3Wavelength = settings.waves.Wave3Wavelength;
	data.Wave3Steepness = settings.waves.Wave3Steepness;
	data.Wave3AngleOffset = settings.waves.Wave3AngleOffset;
	
	data.Wave4Amplitude = settings.waves.Wave4Amplitude;
	data.Wave4Wavelength = settings.waves.Wave4Wavelength;
	data.Wave4Steepness = settings.waves.Wave4Steepness;
	data.Wave4AngleOffset = settings.waves.Wave4AngleOffset;
	
	data.Wave5Amplitude = settings.waves.Wave5Amplitude;
	data.Wave5Wavelength = settings.waves.Wave5Wavelength;
	data.Wave5Steepness = settings.waves.Wave5Steepness;
	data.Wave5AngleOffset = settings.waves.Wave5AngleOffset;
	
	data.Wave6Amplitude = settings.waves.Wave6Amplitude;
	data.Wave6Wavelength = settings.waves.Wave6Wavelength;
	data.Wave6Steepness = settings.waves.Wave6Steepness;
	data.Wave6AngleOffset = settings.waves.Wave6AngleOffset;

	// Tessellation
	bool tessellationEnabled = settings.tessellation.EnableTessellation && 
	                           UnifiedWaterTessellation::AreShadersReady();
	data.TessellationEnabled = tessellationEnabled ? 1.0f : 0.0f;
	data.WaveFadeStart = settings.waves.WaveFadeStart;
	data.WaveFadeEnd = settings.waves.WaveFadeEnd;

	// Ripple settings
	data.RippleStrength = settings.ripples.EnableActorRipples ? settings.ripples.RippleStrength : 0.0f;
	data.RippleRadius = settings.ripples.RippleRadius;
	data.RippleWaveSpeed = settings.ripples.RippleWaveSpeed;
	data.RippleWaveFreq1 = settings.ripples.RippleWaveFreq1;
	data.RippleWaveFreq2 = settings.ripples.RippleWaveFreq2;
	data.RippleWaveFreq3 = settings.ripples.RippleWaveFreq3;
	data.RippleNormalStrength = settings.ripples.RippleNormalStrength;
	
	// Foam system
	data.FoamEnabled = settings.foam.EnableFoam ? 1.0f : 0.0f;
	data.FoamIntensity = settings.foam.FoamIntensity;
	data.FoamIntensityFlowmap = settings.foam.FoamIntensityFlowmap;
	data.FoamThreshold = settings.foam.FoamThreshold;
	data.FoamSharpness = settings.foam.FoamSharpness;
	data.FoamLargeWaveSlopeRequirement = settings.foam.LargeWaveSlopeRequirement;
	data.FoamSmallWaveSlopeMultiplier = settings.foam.SmallWaveSlopeMultiplier;
	data.FoamSmallWaveBaseOffset = settings.foam.SmallWaveBaseOffset;
	data.FoamSmallWaveHeightRange = settings.foam.SmallWaveHeightRange;
	
	// Depth-based wave control
	data.ShallowWaveDepthMin = settings.waves.ShallowWaveDepthMin;
	data.ShallowWaveDepthMax = settings.waves.ShallowWaveDepthMax;
	data.ShoreWaveDepthThreshold = settings.waves.ShoreWaveDepthThreshold;
	data.ShoreWaveStrength = settings.waves.ShoreWaveStrength;
	
	// Terrain heightmap parameters
	auto& terrainShadows = globals::features::terrainShadows;
	auto terrainData = terrainShadows.GetCommonBufferData();
	data.TerrainHeightmapEnabled = terrainData.EnableTerrainShadow ? 1.0f : 0.0f;
	data.TerrainScaleX = terrainData.Scale.x;
	data.TerrainScaleY = terrainData.Scale.y;
	data.TerrainOffsetX = terrainData.Offset.x;
	data.TerrainOffsetY = terrainData.Offset.y;
	data.TerrainZRangeMin = terrainData.ZRange.x;
	data.TerrainZRangeMax = terrainData.ZRange.y;
	data.TerrainRawZMin = terrainData.RawHeightmapZRange.x;
	data.TerrainRawZMax = terrainData.RawHeightmapZRange.y;
	data.TerrainPad0 = 0.0f;
	data.TerrainPad1 = 0.0f;
	data.TerrainPad2 = 0.0f;
	
	// Timing
	float gameTimeHours = 0.0f;
	float realTimeSeconds = 0.0f;
	float timeScale = 1.0f;

	if (const auto calendar = RE::Calendar::GetSingleton()) {
		gameTimeHours = calendar->GetHoursPassed();
		timeScale = calendar->GetTimescale();
	}

	if (globals::state) {
		realTimeSeconds = globals::state->timer;
	}

	const auto* state = globals::state;
	const std::uint32_t frameIndex = state ? state->frameCount : lastTimingFrameIndex;
	if (lastTimingFrameIndex != frameIndex) {
		if (hasLastTimingSample) {
			lastGameTimeHours = currentGameTimeHours;
			lastRealTimeSeconds = currentRealTimeSeconds;
			lastTimeScale = currentTimeScale;
		}
		lastTimingFrameIndex = frameIndex;
	}

	data.GameTimeHours = gameTimeHours;
	data.RealTimeSeconds = realTimeSeconds;
	data.TimeScale = timeScale;
	data.CellWorldSize = 4096.0f;
	data.PrevGameTimeHours = hasLastTimingSample ? lastGameTimeHours : gameTimeHours;
	data.PrevRealTimeSeconds = hasLastTimingSample ? lastRealTimeSeconds : realTimeSeconds;
	data.PrevTimeScale = hasLastTimingSample ? lastTimeScale : timeScale;

	currentGameTimeHours = gameTimeHours;
	currentRealTimeSeconds = realTimeSeconds;
	currentTimeScale = timeScale;
	hasLastTimingSample = true;

	// Player ripple data
	float currentRealTime = globals::state ? globals::state->timer : 0.0f;
	UnifiedWaterRipples::UpdatePlayerRippleData(
		data.PlayerPosX,
		data.PlayerPosY,
		data.PlayerPosZ,
		data.PlayerSpeed,
		data.PlayerInWater,
		data.PlayerVelocityX,
		data.PlayerVelocityY,
		data.PlayerWaterDepth,
		waterSurfaceHeight,
		currentRealTime);
}

void PBRWater::UpdatePerTileData(const RE::BSRenderPass* pass, PerTile& data)
{
	int32_t x, y;
	Util::WorldToCell(pass->geometry->world.translate, x, y);

	int32_t lodLevel = 1;
	if (pass->geometry->name.c_str()) {
		const char* name = pass->geometry->name.c_str();
		if (strncmp(name, "WaterLOD_", 9) == 0) {
			lodLevel = atoi(name + 9);
			if (lodLevel != 1 && lodLevel != 4 && lodLevel != 8 && lodLevel != 16 && lodLevel != 32) {
				lodLevel = 1;
			}
		}
	}

	RE::TESWorldSpace* activeWorldSpace = nullptr;
	std::uint32_t worldSpaceId = 0;
	if (const auto tes = RE::TES::GetSingleton()) {
		activeWorldSpace = tes->GetRuntimeData2().worldSpace;
		if (activeWorldSpace) {
			worldSpaceId = activeWorldSpace->GetFormID();
		}
	}

	auto mixKey = [](std::uint64_t seed, std::uint64_t value) noexcept {
		seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
		return seed;
	};

	std::uint64_t tileKeySeed = 0;
	tileKeySeed = mixKey(tileKeySeed, static_cast<std::uint64_t>(worldSpaceId));
	tileKeySeed = mixKey(tileKeySeed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(lodLevel)));
	tileKeySeed = mixKey(tileKeySeed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)));
	tileKeySeed = mixKey(tileKeySeed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)));
	const std::uint64_t tileKey = tileKeySeed;

	float prevNormalX = 0.0f;
	float prevNormalY = 0.0f;
	float prevDistance = 10000.0f;
	float prevSegments = 32.0f;
	const auto prevTileIt = prevTileData.find(tileKey);
	if (prevTileIt != prevTileData.end()) {
		prevNormalX = prevTileIt->second.normalX;
		prevNormalY = prevTileIt->second.normalY;
		prevDistance = prevTileIt->second.distance;
		prevSegments = prevTileIt->second.segmentsPerAxis;
	}

	data.PrevData[0] = prevNormalX;
	data.PrevData[1] = prevNormalY;
	data.PrevData[2] = prevDistance;
	data.PrevData[3] = prevSegments;

	data.TileData[0] = static_cast<float>(x);
	data.TileData[1] = static_cast<float>(y);
	data.TileData[2] = static_cast<float>(lodLevel);
	data.TileData[3] = 1.0f;

	float currentSegmentsPerAxis = prevSegments;
	if (const auto triShape = pass->geometry->AsTriShape()) {
		auto& runtimeData = triShape->GetTrishapeRuntimeData();
		const float triangleCount = static_cast<float>(runtimeData.triangleCount);
		if (triangleCount > 0.0f) {
			currentSegmentsPerAxis = std::max(1.0f, std::sqrt(triangleCount * 0.5f));
		}
	}
	data.PrevData[3] = currentSegmentsPerAxis;

	prevTileData[tileKey] = PBRWater::PrevTileData{ prevNormalX, prevNormalY, prevDistance, currentSegmentsPerAxis };
}

// Track tessellation state for RestoreGeometry
static D3D11_PRIMITIVE_TOPOLOGY originalTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
static bool tessellationActiveForPass = false;

void PBRWater::BSWaterShader_SetupGeometry::thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass)
{
	auto& pbrWater = globals::features::pbrWater;

	// Update and bind the per-frame constant buffer
	if (pbrWater.GetPerFrameBuffer()) {
		PBRWater::PerFrame perFrameData{};
		
		float waterSurfaceHeight = 0.0f;
		// Update actor ripple buffer
		PBRWater::ActorRippleBuffer actorRippleData{};
		UnifiedWaterRipples::UpdateActorRipples(actorRippleData, waterSurfaceHeight, false);
		pbrWater.GetActorRippleBuffer()->Update(actorRippleData);

		pbrWater.UpdatePerFrameData(perFrameData, waterSurfaceHeight);
		pbrWater.GetPerFrameBuffer()->Update(perFrameData);
		
		auto context = globals::d3d::context;
		ID3D11Buffer* buffers[1] = { pbrWater.GetPerFrameBuffer()->CB() };
		context->VSSetConstantBuffers(7, 1, buffers);
		context->PSSetConstantBuffers(7, 1, buffers);
		
		ID3D11Buffer* actorBuffers[1] = { pbrWater.GetActorRippleBuffer()->CB() };
		context->PSSetConstantBuffers(10, 1, actorBuffers);
	}

	// Update per-tile data for temporal blending
	if (pbrWater.GetPerTileBuffer()) {
		PBRWater::PerTile perTileData{};
		pbrWater.UpdatePerTileData(pass, perTileData);
		pbrWater.GetPerTileBuffer()->Update(perTileData);

		auto context = globals::d3d::context;
		ID3D11Buffer* buffers[1] = { pbrWater.GetPerTileBuffer()->CB() };
		context->VSSetConstantBuffers(8, 1, buffers);
		context->PSSetConstantBuffers(8, 1, buffers);
	}

	// Extract technique from passEnum
	uint32_t technique = (pass->passEnum >> 11) & 0xF;
	bool techniqueSupportsTessel = UnifiedWaterTessellation::IsTechniqueCompatible(technique);

	// Tessellation setup
	bool tessellationEnabled = pbrWater.GetSettings().tessellation.EnableTessellation && 
	                           UnifiedWaterTessellation::AreShadersReady() &&
	                           techniqueSupportsTessel;

	auto context = globals::d3d::context;

	// Clean up any lingering tessellation state
	UnifiedWaterTessellation::UnbindTessellationShaders(context);

	// Call original SetupGeometry
	func(waterShader, pass);

	// Bind terrain elevation heightmap (t61) to VS/DS for water depth estimation
	// This uses the raw heightmap data, not the shadow-processed data (t60)
	ID3D11ShaderResourceView* terrainElevationSRV[1] = { nullptr };
	context->PSGetShaderResources(61, 1, terrainElevationSRV);
	if (terrainElevationSRV[0]) {
		context->VSSetShaderResources(61, 1, terrainElevationSRV);
		context->DSSetShaderResources(61, 1, terrainElevationSRV);
		terrainElevationSRV[0]->Release();
	}
	
	ID3D11SamplerState* terrainSampler[1] = { nullptr };
	context->PSGetSamplers(4, 1, terrainSampler);
	if (terrainSampler[0]) {
		context->VSSetSamplers(12, 1, terrainSampler);
		context->DSSetSamplers(12, 1, terrainSampler);
		terrainSampler[0]->Release();
	}

	bool geometryShaderOnlyForVisualizer = !tessellationEnabled && 
	                                        pbrWater.GetSettings().general.ShowWireframe && 
	                                        UnifiedWaterTessellation::AreShadersReady() &&
	                                        techniqueSupportsTessel;

	if (tessellationEnabled) {
		auto* tessParamsBuffer = UnifiedWaterTessellation::GetTessellationParamsBuffer();
		if (tessParamsBuffer) {
			UnifiedWaterTessellation::UpdateTessellationParams(pbrWater.GetSettings().tessellation, context);
		}

		context->IAGetPrimitiveTopology(&originalTopology);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

		context->HSSetShader(UnifiedWaterTessellation::GetHullShader().get(), nullptr, 0);
		context->DSSetShader(UnifiedWaterTessellation::GetDomainShader().get(), nullptr, 0);
		context->GSSetShader(UnifiedWaterTessellation::GetGeometryShader().get(), nullptr, 0);

		// Bind VS constant buffers to DS
		ID3D11Buffer* vsBuffers[3] = { nullptr, nullptr, nullptr };
		context->VSGetConstantBuffers(0, 3, vsBuffers);
		context->DSSetConstantBuffers(0, 3, vsBuffers);
		
		ID3D11Buffer* frameBuffer[1] = { nullptr };
		context->PSGetConstantBuffers(12, 1, frameBuffer);
		if (frameBuffer[0]) {
			context->HSSetConstantBuffers(12, 1, frameBuffer);
			context->DSSetConstantBuffers(12, 1, frameBuffer);
		}

		if (pbrWater.GetPerFrameBuffer()) {
			ID3D11Buffer* perFrameBuffers[1] = { pbrWater.GetPerFrameBuffer()->CB() };
			context->HSSetConstantBuffers(7, 1, perFrameBuffers);
			context->DSSetConstantBuffers(7, 1, perFrameBuffers);
		}

		// Bind normal textures to DS
		ID3D11ShaderResourceView* normalSRVs[3] = { nullptr, nullptr, nullptr };
		ID3D11SamplerState* normalSamplers[3] = { nullptr, nullptr, nullptr };
		context->PSGetShaderResources(4, 3, normalSRVs);
		context->PSGetSamplers(4, 3, normalSamplers);
		context->DSSetShaderResources(4, 3, normalSRVs);
		context->DSSetSamplers(4, 3, normalSamplers);
		
		for (int i = 0; i < 3; i++) {
			if (normalSRVs[i]) normalSRVs[i]->Release();
			if (normalSamplers[i]) normalSamplers[i]->Release();
		}
		
		ID3D11ShaderResourceView* flowmapSRVs[2] = { nullptr, nullptr };
		ID3D11SamplerState* flowmapSamplers[2] = { nullptr, nullptr };
		context->PSGetShaderResources(8, 2, flowmapSRVs);
		context->PSGetSamplers(8, 2, flowmapSamplers);
		context->DSSetShaderResources(8, 2, flowmapSRVs);
		context->DSSetSamplers(8, 2, flowmapSamplers);
		for (int i = 0; i < 2; i++) {
			if (flowmapSRVs[i]) flowmapSRVs[i]->Release();
			if (flowmapSamplers[i]) flowmapSamplers[i]->Release();
		}

		tessellationActiveForPass = true;
	} else if (geometryShaderOnlyForVisualizer) {
		context->GSSetShader(UnifiedWaterTessellation::GetGeometryShader().get(), nullptr, 0);
		tessellationActiveForPass = true;
	}
}

void PBRWater::BSWaterShader_RestoreGeometry::thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass, uint32_t renderFlags)
{
	if (tessellationActiveForPass) {
		auto context = globals::d3d::context;

		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);

		if (originalTopology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
			context->IASetPrimitiveTopology(originalTopology);
		}

		tessellationActiveForPass = false;
	}

	func(waterShader, pass, renderFlags);
}
