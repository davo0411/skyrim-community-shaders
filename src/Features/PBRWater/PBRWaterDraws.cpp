// PBR Water: The water draws: per-draw constants and bindings, the projected grids in place of the engine's meshes, and
// the batched far stencil tiles.

#include "Features/PBRWater.h"
#include "Features/PBRWater/EngineWater.h"
#include "Features/PBRWater/PBRWaterInternal.h"
#include "Features/PBRWater/Profiling.h"

#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/TerrainShadows.h"
#include "Features/UnifiedWater.h"
#include "Features/Upscaling.h"
#include "Features/VolumetricShadows.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/ActorUtils.h"
#include "Utils/UI.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace PBRWaterDetail;

namespace
{
	/// The longest a river lattice's edge stands on screen, in grid cells (Grid Cell Size). Mirrors Water.hlsl RiverLatticePixels.
	constexpr float RiverLatticeCells = 3.0f;

	/// Other views' water (CheapOffscreenWater): no shore waves, surf, foam, sediment, subsurface, point lights, ripples or
	/// wave shadows. They see it small or blurred.
	void CheapOffscreenGates(PBRWater::GpuData& d)
	{
		d.Shore0.x = 0.0f;
		d.Foam2.w = 0.0f;
		d.Swash0.w = 0.0f;
		d.Light1.y = 0.0f;
		d.Surface1.y = 0.0f;
		d.Light0.y = 0.0f;
		d.Light1.w = 0.0f;
		d.Ripple0.w = 0.0f;
		d.SunMap0.w = 0.0f;
		d.Draw0.z = 0.0f;
		if (d.Params0.x < 0.5f)
			d.Surface1.x = 0.0f;  // the open sea was their only waves
	}
}

void PBRWater::SetupDraw(RE::BSShader* waterShader, RE::BSRenderPass* pass)
{
	if (!gpuBuffer || !waterShader || !pass || !pass->geometry)
		return;

	auto context = globals::d3d::context;
	auto state = globals::state;
	auto shaderCache = globals::shaderCache;

	const uint32_t vertexDescriptor = state->modifiedVertexDescriptor;
	const uint32_t technique = WaterTechnique(vertexDescriptor);
	const bool waves = WavesReach(*pass->geometry);

	const auto snap = snapshot.load(std::memory_order_acquire);
	const bool hasWaves = waves && snap && ((snap->ocean && !snap->ocean->calm && ocean.IsReady()) || snap->shore.enabled || settings.EnableRipples);

	// The grid is drawn by our own vertex shader (its modes, Tess0.x): while that is still compiling the vanilla shaders
	// are bound, and the draw stays the engine's.
	auto customShadersLive = [&] {
		return shaderCache->IsEnabled() && shaderCache->GetVertexShader(*waterShader, vertexDescriptor) &&
		       shaderCache->GetPixelShader(*waterShader, state->modifiedPixelDescriptor);
	};

	// The grids are built for the main camera: every pass of the main view draws them (depth and motion, surface, point
	// lights, underwater), each the same vertices. The other views (reflections, cubemaps, the map) see the water small or
	// not at all, and draw the engine's meshes, as do rivers, interiors and a level's first frame.
	const bool mainView = state->inWorld && !(state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections));
	// A level's projected grid draws every mesh at that level: the engine's draw rasterises nothing (Tess0.x = 2), and the
	// first draw at the level in each pass draws the grid in its state instead (RestoreDraw, DrawGrid).
	// The levels the grids take: a lake gets one once its meshes are drawn; a river keeps its meshes and its cells leave the
	// grid at its level, but at the sea's level (its mouth) the sea's grid draws it.
	const bool riverMouth = IsRiverMouth(*pass->geometry);
	if (settings.EnableTessellation && mainView && IsTessellatedTechnique(technique)) {
		const auto& bound = pass->geometry->worldBound;
		if (WaterMeshes::IsRiver(*pass->geometry) && !riverMouth)
			projectedGrid.ExcludeRiver(bound.center.x, bound.center.y, bound.radius, bound.center.z);
		else
			projectedGrid.Want(bound.center.z, state->frameCount);
	}
	const int gridSlot = mainView && IsTessellatedTechnique(technique) && customShadersLive() ? GridSlotOf(*pass->geometry) : -1;
	const bool gridSea = gridSlot >= 0;
	bool gridFirst = false;
	// A river's mouth never draws the grid itself: the grid would take its flowmap permutation (the technique is the same).
	if (gridSea && !riverMouth) {
		gridFirst = std::exchange(gridPassFrame[technique * 8 + static_cast<uint32_t>(gridSlot)], state->frameCount) != state->frameCount;
		if (!projectedGrid.HasTexCoords(pass->geometry->worldBound.center.z)) {
			// The level's meshes' texture coordinates as a plane over absolute world xy, from the first one seen in the
			// worldspace (the vanilla normal maps; the grid evaluates them per vertex, ProjectedGridCS).
			float4 mapU{}, mapV{};
			if (WaterMeshes::AffineTexCoords(*pass->geometry, mapU, mapV)) {
				const auto& world = pass->geometry->world;
				const float invScale = 1.0f / std::max(world.scale, 1e-6f);
				auto toWorld = [&](const float4& map) {
					const float ax = (map.x * world.rotate.entry[0][0] + map.y * world.rotate.entry[0][1]) * invScale;
					const float ay = (map.x * world.rotate.entry[1][0] + map.y * world.rotate.entry[1][1]) * invScale;
					return float4(ax, ay, map.z - ax * world.translate.x - ay * world.translate.y, 0.0f);
				};
				mapU = toWorld(mapU);
				mapV = toWorld(mapV);
			}
			projectedGrid.SetTexCoords(pass->geometry->worldBound.center.z, mapU, mapV);
		}
	}
	ID3D11GeometryShader* geometryShader = nullptr;
	if (settings.WireframeMode > 0 && !IsStencilTechnique(technique) && customShadersLive())
		geometryShader = shaderCache->GetGeometryShader(*waterShader, vertexDescriptor);

	// Vertex spacing of the engine's mesh, for filtering waves its vertices cannot carry.
	float spacing = 0.0f;
	uint32_t triangleCount = 0;
	if (const auto triShape = pass->geometry->AsTriShape()) {
		triangleCount = triShape->GetTrishapeRuntimeData().triangleCount;
		if (hasWaves && !gridSea && triangleCount)
			spacing = pass->geometry->worldBound.radius * std::numbers::sqrt2_v<float> / std::max(std::sqrt(triangleCount * 0.5f), 1.0f);
	}

	// The water draws (Debug section; DrawGrid counts the grids').
	if (state->frameCount != drawStatsFrame) {
		lastDrawStats = drawStats;
		drawStats = {};
		drawStatsFrame = state->frameCount;
	}
	++drawStats.draws;
	++drawStats.byTechnique[DrawPassIndex(technique)];

	// Most water is flat distant tiles, hundreds of them: anything done per draw is paid hundreds of times per pass. The
	// constants, views and samplers are uploaded and bound once per batch (SetupBatch, BindDrawResources); a draw only binds
	// another constant buffer when its mode (Tess0.x), its spacing or its view's gates differ from the last draw's.
	GpuData& d = frameData;
	if (drawResourcesFrame != state->frameCount)
		BindDrawResources();
	// The engine's own draw of a mesh the grid draws runs the vertex shader in its culling mode (Tess0.x = 2): it
	// rasterises nothing, and RestoreDraw draws the grid with its own constants, which the pixel stage gets now.
	const bool cheap = !mainView && settings.CheapOffscreenWater;
	ID3D11Buffer* cb = gridSea ? gridDrawBuffers[gridSlot]->CB() : (cheap ? offscreenDrawBuffer->CB() : flatDrawBuffer->CB());
	if (spacing > 0.0f) {
		// Neighbouring tiles share a spacing: upload only when it (or the view's gates) changes.
		if (spacing != uploadedSpacing || cheap != uploadedSpacingCheap) {
			GpuData drawData = d;
			drawData.Tess0.x = 0.0f;
			drawData.Draw0.x = spacing;
			if (cheap)
				CheapOffscreenGates(drawData);
			spacingDrawBuffer->Update(drawData);
			uploadedSpacing = spacing;
			uploadedSpacingCheap = cheap;
		}
		cb = spacingDrawBuffer->CB();
	}
	ID3D11Buffer* vertexCb = gridSea ? swallowDrawBuffer->CB() : cb;
	if (vertexCb != boundVertexCb) {
		context->VSSetConstantBuffers(7, 1, &vertexCb);
		boundVertexCb = vertexCb;
	}
	if (cb != boundPixelCb) {
		context->PSSetConstantBuffers(7, 1, &cb);
		boundPixelCb = cb;
	}

	// The renderer binds textures and constant buffers lazily, right before the draw call, so the
	// device still holds the previous draw's state here: read this draw's from the shadow state.
	auto& shadow = globals::game::shadowState->GetRuntimeData();
	// That flush also re-binds every sampler slot the renderer has marked modified (Lighting uses 14-15),
	// which would replace ours with a point or wrap sampler depending on what was drawn before: drop those
	// pending updates for this draw. RestoreDraw() marks them again so the next draw gets the game's.
	shadow.PSSamplerModifiedBits &= ~(3u << WaterSamplerBase);

	// A river's mesh says so to the vertex program (Water.hlsl PBRWRiver). Its mouth is the sea's, also where the engine
	// draws it (the other views).
	const bool riverMesh = !gridSea && WaterMeshes::IsRiver(*pass->geometry);
	const bool river = hasWaves && riverMesh && !riverMouth;
	// Near the camera a river's mesh is drawn as its lattice (RestoreDraw), the engine's draw culled.
	bool lattice = false;
	if (hasWaves && riverMesh && settings.EnableTessellation && mainView && IsTessellatedTechnique(technique) && customShadersLive())
		lattice = riverLattice.Choose(*pass->geometry, d.Tess0.w, RiverLatticeCells * d.Tess0.y, spacing, state->frameCount);
	ID3D11Buffer* riverCb = riverLattice.EngineCB(river, lattice);
	if (riverCb != boundRiverCb) {
		context->VSSetConstantBuffers(8, 1, &riverCb);
		boundRiverCb = riverCb;
	}
	riverLatticePending = lattice;
	riverLatticeRiver = river;
	cachedDrawPending = gridFirst;
	cachedDrawStart = gridSea ? GridMeshStart - static_cast<uint32_t>(gridSlot) : 0;
	cachedDrawTechnique = technique;
	cachedDrawGeometry = pass->geometry;
	cachedDrawShader = waterShader;
	cachedDrawPixelDescriptor = state->modifiedPixelDescriptor;

	if (geometryShader) {
		context->GSSetConstantBuffers(7, 1, &cb);
		context->GSSetShader(geometryShader, nullptr, 0);
		geometryShaderBound = true;
	}
}

void PBRWater::SetupBatch()
{
	if (!gpuBuffer)
		return;
	UpdateFrameConstants();
	// Before the batch's pass and before the draws upload the constants, which say where the heightfield is.
	const auto snap = snapshot.load(std::memory_order_acquire);
	UpdateSunMap(settings.EnableWaveShadows && snap && ((snap->ocean && !snap->ocean->calm && ocean.IsReady()) || snap->shore.enabled) && globals::shaderCache->IsEnabled());
	UploadDrawConstants();
	stencilBatch.ready = false;
	// Bound with the batch's first draw: the lit geometry drawn between batches (True PBR, Terrain Helper) uses t80-t97.
	drawResourcesFrame = UINT32_MAX;
	// A batch the engine never closed (none should be) ends here rather than swallowing the next.
	EndDrawBatch();
	BeginDrawBatch(WaterTechnique(globals::state->modifiedVertexDescriptor));
}

void PBRWater::UploadDrawConstants()
{
	const GpuData& d = frameData;
	if (std::memcmp(&uploadedDrawData, &d, sizeof(GpuData)) == 0)
		return;
	uploadedDrawData = d;
	GpuData drawData = d;
	drawData.Tess0.x = 2.0f;
	swallowDrawBuffer->Update(drawData);
	drawData.Tess0.x = 3.0f;
	deferredDrawBuffer->Update(drawData);
	drawData.Tess0.x = 5.0f;
	instancedDrawBuffer->Update(drawData);
	// The projected grids built this frame, each with its layout (ProjectedGrid::BuiltLevel).
	const uint32_t frame = globals::state->frameCount;
	for (uint32_t slot = 0; slot < projectedGrid.BuiltCount(frame); ++slot) {
		const auto& grid = projectedGrid.Built(slot);
		GpuData gridData = drawData;
		gridData.Proj0 = { static_cast<float>(grid.columns), grid.z, 1.0f, static_cast<float>(grid.firstVertex) };
		gridData.Proj1 = projectedGrid.CellsMapping();
		gridData.Proj2 = { static_cast<float>(grid.firstCell), static_cast<float>(grid.firstPatch), static_cast<float>(grid.subdivision), 0.0f };
		gridData.Tess0.x = 6.0f;
		gridDrawBuffers[slot]->Update(gridData);
		gridData.Tess0.x = 7.0f;
		patchDrawBuffers[slot]->Update(gridData);
	}
	drawData.Tess0.x = 0.0f;
	flatDrawBuffer->Update(drawData);
	// Costly gates off for other views (CheapOffscreenWater); always uploaded so toggling the setting never binds stale data.
	CheapOffscreenGates(drawData);
	offscreenDrawBuffer->Update(drawData);
	uploadedSpacing = -1.0f;
	GatherWaterSRVs(d, drawSRVs);
	drawResourcesFrame = UINT32_MAX;
}

void PBRWater::BindDrawResources()
{
	auto context = globals::d3d::context;
	drawResourcesFrame = globals::state->frameCount;
	const GpuData& d = frameData;
	// These slots are free in every stage of the water shaders (Physical Sky, View Atmosphere and Landscape Seams use
	// the t110-t127 range, Lighting uses s12-s13).
	ID3D11SamplerState* linearClamp = linearClampSampler.get();
	ID3D11SamplerState* samplers[2] = { linearClamp, oceanSampler.get() };
	context->VSSetShaderResources(WaterSRVBase, WaterSRVCount, drawSRVs);
	context->PSSetShaderResources(WaterSRVBase, WaterSRVCount, drawSRVs);
	context->VSSetSamplers(WaterSamplerBase, 2, samplers);
	context->PSSetSamplers(WaterSamplerBase, 2, samplers);
	// The waves' heightfield is the pixel stage's alone, in a slot nothing else uses.
	ID3D11ShaderResourceView* heightfield = d.SunMap0.w > 0.5f && sunMapTexture ? sunMapTexture->srv.get() : nullptr;
	context->PSSetShaderResources(WaveHeightfieldSlot, 1, &heightfield);
	boundVertexCb = nullptr;
	boundPixelCb = nullptr;
	boundRiverCb = nullptr;
}

void PBRWater::BeginDrawBatch(uint32_t technique)
{
	drawBatchOpen = true;
	PBRWaterProfiling::Begin(DrawPassNames[globals::state->inWorld ? 0 : 1][DrawPassIndex(technique)]);
	// The water around the camera under the surface: over the scene before the first colour pass of the water (the surface or
	// the surface from below), so the surface draws over its own pixels with its own path through the water.
	const auto* state = globals::state;
	const uint32_t pass = DrawPassIndex(technique);
	const bool mainView = state->inWorld && !(state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections));
	if (mainView && (pass == 0 || pass == 2) && underwaterFrame != state->frameCount) {
		underwaterFrame = state->frameCount;
		DrawUnderwaterVolume();
	}
}

void PBRWater::EndDrawBatch()
{
	if (!std::exchange(drawBatchOpen, false))
		return;
	EndDeferredBatch();
	PBRWaterProfiling::End();
}

bool PBRWater::WavesReach(const RE::NiAVObject& geometry) const
{
	// The waves fade out inside the loaded cells (LoadedGridFade) and with the distance (DisplacementDistanceFade).
	const auto& bound = geometry.worldBound;
	const float4& grid = frameData.Grid0;
	if (grid.z > grid.x && (bound.center.x + bound.radius < grid.x || bound.center.x - bound.radius > grid.z || bound.center.y + bound.radius < grid.y || bound.center.y - bound.radius > grid.w))
		return false;
	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float dx = bound.center.x - eye.x;
	const float dy = bound.center.y - eye.y;
	const float reach = frameData.Params1.w + bound.radius;
	return dx * dx + dy * dy < reach * reach;
}

void PBRWater::RestoreDraw()
{
	auto context = globals::d3d::context;
	// Rebound by the renderer's next flush: Lighting uses these slots.
	globals::game::shadowState->GetRuntimeData().PSSamplerModifiedBits |= (3u << WaterSamplerBase);
	if (std::exchange(cachedDrawPending, false)) {
		// The engine's draw rasterised nothing (the vertex shader's culling mode): the grid, with all of the draw's state,
		// the vertex shader in its grid modes. With the deferred surface the stencil pass also records its pixels, and the
		// surface and point light passes shade those instead of drawing the triangles again.
		const bool deferred = settings.DeferredSurface && settings.WireframeMode == 0;
		if (deferred && IsStencilTechnique(cachedDrawTechnique))
			DrawRecordedStencil();
		else if (!(deferred && cachedDrawTechnique < 8 && DrawDeferredSurface()))
			DrawGrid(GridMeshStart - cachedDrawStart);
		drawResourcesFrame = UINT32_MAX;
	}
	if (std::exchange(riverLatticePending, false) && cachedDrawGeometry) {
		riverLattice.Draw(*cachedDrawGeometry, riverLatticeRiver);
		boundRiverCb = nullptr;
	}
	if (geometryShaderBound) {
		context->GSSetShader(nullptr, nullptr, 0);
		geometryShaderBound = false;
	}
}

int PBRWater::GridSlotOf(RE::BSGeometry& geometry) const
{
	// A water mesh is flat: its bound's centre is at its level.
	if (!settings.EnableTessellation || geometry.worldBound.radius <= 0.0f || (WaterMeshes::IsRiver(geometry) && !IsRiverMouth(geometry)))
		return -1;
	return projectedGrid.SlotOf(geometry.worldBound.center.z, globals::state->frameCount);
}

bool PBRWater::IsRiverMouth(RE::BSGeometry& geometry) const
{
	return projectedGrid.HasCells() && WaterMeshes::IsRiver(geometry) && std::abs(geometry.worldBound.center.z - projectedGrid.SeaLevel()) < 4.0f;
}

void PBRWater::DrawGrid(uint32_t slot)
{
	auto context = globals::d3d::context;
	// The cells the grid's level is in: its pixels elsewhere are discarded (PBRWater.hlsli SeaCovers).
	ID3D11ShaderResourceView* cells = projectedGrid.CellsSRV();
	context->PSSetShaderResources(SeaLevelsSlot, 1, &cells);
	ID3D11Buffer* cb = gridDrawBuffers[slot]->CB();
	context->VSSetConstantBuffers(7, 1, &cb);
	context->PSSetConstantBuffers(7, 1, &cb);
	projectedGrid.DrawCells(slot);
	cb = patchDrawBuffers[slot]->CB();
	context->VSSetConstantBuffers(7, 1, &cb);
	context->PSSetConstantBuffers(7, 1, &cb);
	projectedGrid.DrawPatches(slot);
	ID3D11ShaderResourceView* none = nullptr;
	context->PSSetShaderResources(SeaLevelsSlot, 1, &none);
	++drawStats.computeDraws;
}

bool PBRWater::SkipGridPass(const RE::BSRenderPass* pass) const
{
	if (!loaded || !settings.EnableTessellation || !pass || !pass->geometry || !pass->shader || pass->shader->shaderType.get() != RE::BSShader::Type::Water)
		return false;
	const auto* state = globals::state;
	const bool mainView = state->inWorld && !(state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections));
	// The batch's technique (BeginTechnique set the descriptor): the grid's first draw in it marked gridPassFrame (SetupDraw).
	const uint32_t technique = WaterTechnique(state->modifiedVertexDescriptor);
	if (!mainView || !IsTessellatedTechnique(technique))
		return false;
	const int slot = GridSlotOf(*pass->geometry);
	if (slot < 0)
		return false;
	const auto drawn = gridPassFrame.find(technique * 8 + static_cast<uint32_t>(slot));
	return drawn != gridPassFrame.end() && drawn->second == state->frameCount;
}

bool PBRWater::CollectStencilTile(RE::BSRenderPass* pass, uint32_t technique)
{
	if (!stencilBatch.ready || technique != stencilBatch.technique || !settings.BatchedStencil || !pass || !pass->geometry)
		return false;
	if (pass->geometry->GetGeometryRuntimeData().rendererData != stencilBatch.rendererData || WavesReach(*pass->geometry) || GridSlotOf(*pass->geometry) >= 0)
		return false;
	if (stencilInstances.size() >= MaxStencilInstances)
		FlushStencilInstances();
	const auto& world = pass->geometry->world;
	stencilInstances.push_back({ world.translate.x, world.translate.y, world.translate.z, world.scale });
	return true;
}

void PBRWater::AfterEnginePass(RE::BSRenderPass* pass, uint32_t technique)
{
	stencilBatch.ready = false;
	if (!loaded || !settings.BatchedStencil || settings.WireframeMode > 0 || !gpuBuffer || !pass || !pass->geometry || !pass->shader ||
		pass->shader->shaderType.get() != RE::BSShader::Type::Water)
		return;
	const auto* state = globals::state;
	auto* shaderCache = globals::shaderCache;
	const bool mainView = state->inWorld && !(state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections));
	if (!mainView || !IsStencilTechnique(WaterTechnique(state->modifiedVertexDescriptor)) || !shaderCache->IsEnabled() ||
		!shaderCache->GetVertexShader(*pass->shader, state->modifiedVertexDescriptor))
		return;
	// The tiles the waves reach are drawn as they are (displaced, tessellated, recorded): only the flat far water is batched.
	const auto* triShape = pass->geometry->AsTriShape();
	const auto& data = pass->geometry->GetGeometryRuntimeData();
	if (!triShape || !data.rendererData || WavesReach(*pass->geometry) || GridSlotOf(*pass->geometry) >= 0)
		return;
	// Only Unified Water's tiles share a mesh (clones of one): its tiles are told apart by it.
	auto* unifiedTile = globals::features::unifiedWater.GetTileRendererData();
	if (!unifiedTile || data.rendererData != unifiedTile)
		return;
	stencilBatch.ready = true;
	stencilBatch.technique = technique;
	stencilBatch.rendererData = data.rendererData;
	stencilBatch.indexCount = triShape->GetTrishapeRuntimeData().triangleCount * 3;
}

void PBRWater::FlushStencilInstances()
{
	if (stencilInstances.empty())
		return;
	auto context = globals::d3d::context;
	const auto count = static_cast<UINT>(stencilInstances.size());
	std::copy(stencilInstances.begin(), stencilInstances.end(), stencilUpload.begin());
	stencilInstanceBuffer->Update(stencilUpload.data(), sizeof(float4) * MaxStencilInstances);
	ID3D11ShaderResourceView* view = stencilInstanceBuffer->SRV();
	context->VSSetShaderResources(StencilInstanceSlot, 1, &view);
	ID3D11Buffer* cb = instancedDrawBuffer->CB();
	context->VSSetConstantBuffers(7, 1, &cb);
	context->PSSetConstantBuffers(7, 1, &cb);
	boundVertexCb = nullptr;
	boundPixelCb = nullptr;
	// The last tile the engine drew left its mesh, shaders and targets bound: every Unified Water tile is that mesh.
	context->DrawIndexedInstanced(stencilBatch.indexCount, count, 0, 0, 0);
	ID3D11ShaderResourceView* none = nullptr;
	context->VSSetShaderResources(StencilInstanceSlot, 1, &none);
	stencilInstances.clear();
}
