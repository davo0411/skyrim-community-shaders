#include "BisectionTessellator.h"
#include "Profiling.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace
{
	constexpr uint32_t None = 0xFFFFFFFFu;
	/// Grid applied to absolute positions before two roots' edges are matched (quantized to a sixteenth of a unit).
	constexpr double Quantum = 1.0 / 16.0;
	/// State buffer layout (BisectionCS.hlsl): count, cursor, end, listed, two dispatch arguments, the reach.
	constexpr uint32_t StateBytes = 96;
	constexpr uint32_t StateDispatch = 16;
	constexpr uint32_t StateVertexDispatch = 32;
	constexpr uint32_t StateIterate = 64;

	using Point = std::array<int64_t, 3>;

	Point QuantizePoint(const double (&p)[3])
	{
		return { static_cast<int64_t>(std::llround(p[0] / Quantum)), static_cast<int64_t>(std::llround(p[1] / Quantum)), static_cast<int64_t>(std::llround(p[2] / Quantum)) };
	}

	struct EdgeKey
	{
		Point a, b;  // a < b
		bool operator==(const EdgeKey&) const = default;
	};

	struct EdgeKeyHash
	{
		size_t operator()(const EdgeKey& k) const
		{
			size_t h = 1469598103934665603ull;
			for (const auto& p : { k.a, k.b }) {
				for (const int64_t v : p) {
					h ^= static_cast<size_t>(v);
					h *= 1099511628211ull;
				}
			}
			return h;
		}
	};

	std::unique_ptr<Buffer> CreateStructured(UINT stride, UINT count, bool uav, const char* name)
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = stride * count;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = stride;
		auto buffer = std::make_unique<Buffer>(desc, nullptr, name);
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srv.Buffer.NumElements = count;
		buffer->CreateSRV(srv);
		if (uav) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
			u.Format = DXGI_FORMAT_UNKNOWN;
			u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			u.Buffer.NumElements = count;
			buffer->CreateUAV(u);
		}
		return buffer;
	}

	bool CreateRaw(UINT bytes, UINT misc, const char* name, winrt::com_ptr<ID3D11Buffer>& buffer, winrt::com_ptr<ID3D11UnorderedAccessView>* uav,
		winrt::com_ptr<ID3D11ShaderResourceView>* srv = nullptr)
	{
		auto device = globals::d3d::device;
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = bytes;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u) | (srv ? D3D11_BIND_SHADER_RESOURCE : 0u);
		desc.MiscFlags = misc | ((uav || srv) ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : 0u);
		buffer = nullptr;
		if (FAILED(device->CreateBuffer(&desc, nullptr, buffer.put())))
			return false;
		Util::SetResourceName(buffer.get(), name);
		if (uav) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
			u.Format = DXGI_FORMAT_R32_TYPELESS;
			u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			u.Buffer.NumElements = bytes / 4;
			u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			*uav = nullptr;
			if (FAILED(device->CreateUnorderedAccessView(buffer.get(), &u, uav->put())))
				return false;
		}
		if (srv) {
			D3D11_SHADER_RESOURCE_VIEW_DESC v{};
			v.Format = DXGI_FORMAT_R32_TYPELESS;
			v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
			v.BufferEx.NumElements = bytes / 4;
			v.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
			*srv = nullptr;
			if (FAILED(device->CreateShaderResourceView(buffer.get(), &v, srv->put())))
				return false;
		}
		return true;
	}
}

void BisectionTessellator::SetupResources()
{
	constants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<BisectionCB>(), "PBRWater::BisectionCB");
	bisectors[0] = CreateStructured(BisectorStride, Capacity, true, "PBRWater::Bisectors0");
	bisectors[1] = CreateStructured(BisectorStride, Capacity, true, "PBRWater::Bisectors1");
	childB = CreateStructured(sizeof(uint32_t), Capacity, true, "PBRWater::BisectionChildB");
	list = CreateStructured(sizeof(uint32_t), Capacity, true, "PBRWater::BisectionList");
	vertices = CreateStructured(sizeof(GpuVertex), Capacity * GridVertices, true, "PBRWater::BisectionVertices");
	bool ok = CreateRaw(StateBytes, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, "PBRWater::BisectionState", state, &stateUAV);
	ok = ok && CreateRaw(MaxMeshes * 4, 0, "PBRWater::BisectionMeshCounts", meshCounts, &meshCountsUAV);
	// Each mesh's DrawInstancedIndirect arguments, then each mesh's offset in the draw lists (BisectionCS OffsetsCS).
	ok = ok && CreateRaw(MaxMeshes * 20, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, "PBRWater::BisectionDrawArgs", drawArgs, &drawArgsUAV, &drawArgsSRV);
	drawCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<DrawCB>(), "PBRWater::BisectionDrawCB");
	for (auto& staging : stateStaging) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = StateBytes;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		staging = nullptr;
		if (SUCCEEDED(globals::d3d::device->CreateBuffer(&desc, nullptr, staging.put())))
			Util::SetResourceName(staging.get(), "PBRWater::BisectionStateStaging");
	}
	// DispatchIndirect reads its arguments from here, copied out of the state buffer, which is bound for writing.
	ok = ok && CreateRaw(32, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, "PBRWater::BisectionDispatchArgs", dispatchArgs, nullptr);
	if (!ok)
		logger::warn("[PBR Water] The bisection tessellator's buffers could not be created");
	CompileShaders();
}

void BisectionTessellator::CompileShaders()
{
	auto compile = [](const char* entry) {
		winrt::com_ptr<ID3D11ComputeShader> cs;
		cs.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\BisectionCS.hlsl", {}, "cs_5_0", entry)));
		return cs;
	};
	initCS = compile("InitCS");
	frameStartCS = compile("FrameStartCS");
	mergeDecideCS = compile("MergeDecideCS");
	mergeLinkCS = compile("MergeLinkCS");
	compactCountCS = compile("CompactCountCS");
	compactWriteCS = compile("CompactWriteCS");
	compactArgsCS = compile("CompactArgsCS");
	classifyCS = compile("ClassifyCS");
	linkCS = compile("LinkCS");
	argsCS = compile("ArgsCS");
	fixupCS = compile("FixupCS");
	countCS = compile("CountCS");
	offsetsCS = compile("OffsetsCS");
	scatterCS = compile("ScatterCS");
	vertexCS = compile("VertexCS");
	ready = initCS && frameStartCS && mergeDecideCS && mergeLinkCS && compactCountCS && compactWriteCS && compactArgsCS && classifyCS && linkCS && argsCS && fixupCS && countCS && offsetsCS && scatterCS && vertexCS && constants && bisectors[0] &&
	        bisectors[1] && childB && list && vertices && stateUAV && meshCountsUAV && drawArgsUAV && drawArgsSRV && drawCB && dispatchArgs;
	if (!ready)
		logger::warn("[PBR Water] Bisection tessellation shaders failed to compile; the water is drawn untessellated");
	Clear();
}

void BisectionTessellator::Clear()
{
	built = false;
	slotOf.clear();
	slots.clear();
	retained.clear();
	persistent = false;
}

bool BisectionTessellator::BuiltFor(uint32_t frameCount, const float4& cameraPosAdjust) const
{
	return built && builtFrame == frameCount && builtCamera.x == cameraPosAdjust.x && builtCamera.y == cameraPosAdjust.y && builtCamera.z == cameraPosAdjust.z;
}

bool BisectionTessellator::Lookup(RE::BSGeometry& geometry, uint32_t& slot) const
{
	if (!built)
		return false;
	const auto it = slotOf.find(&geometry);
	if (it == slotOf.end() || !WaterMeshes::StillSource(geometry, slots[it->second].source))
		return false;
	slot = it->second;
	return true;
}

void BisectionTessellator::Draw(uint32_t slot) const
{
	if (!built || slot >= slots.size())
		return;
	auto context = globals::d3d::context;
	// The vertex shader finds the mesh's bisectors from its slot (b11) and the offsets after the draw arguments (t108):
	// SV_VertexID starts at 0 for every draw.
	DrawCB data{ { slot, 0, 0, 0 } };
	drawCB->Update(data);
	winrt::com_ptr<ID3D11Buffer> savedCb;
	context->VSGetConstantBuffers(11, 1, savedCb.put());
	ID3D11Buffer* cb = drawCB->CB();
	context->VSSetConstantBuffers(11, 1, &cb);
	ID3D11ShaderResourceView* srvs[2] = { drawArgsSRV.get(), vertices->srv.get() };
	context->VSSetShaderResources(108, 2, srvs);
	context->DrawInstancedIndirect(drawArgs.get(), slot * 16);
	ID3D11ShaderResourceView* nullSrvs[2] = {};
	context->VSSetShaderResources(108, 2, nullSrvs);
	ID3D11Buffer* restored = savedCb.get();
	context->VSSetConstantBuffers(11, 1, &restored);
}

void BisectionTessellator::ReadBackState()
{
	// The oldest copy (written three frames ago): never waits on the GPU.
	auto context = globals::d3d::context;
	auto& staging = stateStaging[stagingSlot];
	if (!staging)
		return;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (context->Map(staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK)
		return;
	uint32_t words[StateBytes / 4];
	std::memcpy(words, mapped.pData, StateBytes);
	context->Unmap(staging.get(), 0);
	gpuStats.bisectors = words[0];
	gpuStats.listed = words[3];
	gpuStats.iterations = words[19];
	// About as many as the subdivision took, and two for the camera moving; more while it still hits the cap.
	if (gpuStats.iterations > 0)
		iterationCap = gpuStats.iterations >= iterationCap ? std::min(iterationCap + 8, MaxIterations) : std::clamp(gpuStats.iterations + 2, 4u, MaxIterations);
}

void BisectionTessellator::Build(const FrameParams& params, WaterMeshes& meshes)
{
	const auto cpuStart = std::chrono::steady_clock::now();
	built = false;
	slotOf.clear();
	slots.clear();
	++frameCounter;
	std::vector<WaterMeshes::WorldMesh> drawn;
	meshes.TakeBaseTriangles(drawn);
	stats = {};
	if (!ready) {
		Clear();
		return;
	}
	// This frame's meshes and those drawn within RetainFrames, in a fixed order (by geometry): the roots stay the same while
	// the camera turns away from a mesh and back, and the subdivision with them.
	for (auto& mesh : drawn) {
		auto& entry = retained[mesh.geometry];
		entry.mesh = std::move(mesh);
		entry.lastFrame = frameCounter;
	}
	std::erase_if(retained, [&](const auto& entry) { return frameCounter - entry.second.lastFrame > RetainFrames; });
	std::vector<const WaterMeshes::WorldMesh*> source;
	source.reserve(retained.size());
	for (const auto& [geometry, entry] : retained)
		source.push_back(&entry.mesh);
	std::sort(source.begin(), source.end(), [](const auto* a, const auto* b) { return std::less<const void*>()(a->geometry, b->geometry); });
	if (source.empty()) {
		persistent = false;
		return;
	}

	// The roots: every base triangle, counter-clockwise, its longest edge as edge 2 (corner 2 to corner 0).
	rootData.clear();
	std::vector<std::array<Point, 3>> quantized;
	const double camera[3] = { params.cameraX, params.cameraY, params.cameraZ };
	// What the roots are, for keeping the subdivision: their meshes, corners and order.
	uint64_t signature = 1469598103934665603ull;
	auto mix = [&](uint64_t v) {
		signature ^= v;
		signature *= 1099511628211ull;
	};
	for (const auto* meshPtr : source) {
		const auto& mesh = *meshPtr;
		if (slots.size() >= MaxMeshes)
			break;
		const uint32_t slot = static_cast<uint32_t>(slots.size());
		slots.push_back({ mesh.source });
		slotOf[mesh.geometry] = slot;
		mix(reinterpret_cast<uintptr_t>(mesh.geometry));
		for (const auto& t : mesh.triangles) {
			if (rootData.size() >= Capacity / 4)
				break;
			int c[3] = { 0, 1, 2 };
			const double cross = (t.world[1][0] - t.world[0][0]) * (t.world[2][1] - t.world[0][1]) - (t.world[1][1] - t.world[0][1]) * (t.world[2][0] - t.world[0][0]);
			if (cross < 0.0)
				std::swap(c[1], c[2]);
			auto length2 = [&](int i, int j) {
				double l = 0.0;
				for (int k = 0; k < 3; ++k)
					l += (t.world[c[i]][k] - t.world[c[j]][k]) * (t.world[c[i]][k] - t.world[c[j]][k]);
				return l;
			};
			const double e[3] = { length2(0, 1), length2(1, 2), length2(2, 0) };
			const int longest = static_cast<int>(std::max_element(e, e + 3) - e);
			const int shift = (longest + 1) % 3;
			const int r[3] = { c[shift], c[(shift + 1) % 3], c[(shift + 2) % 3] };

			GpuRoot root{};
			std::array<Point, 3> q;
			for (int i = 0; i < 3; ++i) {
				const auto& w = t.world[r[i]];
				root.world[i] = { static_cast<float>(w[0] - camera[0]), static_cast<float>(w[1] - camera[1]), static_cast<float>(w[2] - camera[2]), 0.0f };
				root.model[i] = { t.model[r[i]][0], t.model[r[i]][1], t.model[r[i]][2], 0.0f };
				root.color[i] = { t.color[r[i]][0], t.color[r[i]][1], t.color[r[i]][2], t.color[r[i]][3] };
				q[i] = QuantizePoint(w);
				for (const int64_t v : q[i])
					mix(static_cast<uint64_t>(v));
			}
			root.uv01 = { t.uv[r[0]][0], t.uv[r[0]][1], t.uv[r[1]][0], t.uv[r[1]][1] };
			root.uv2[0] = t.uv[r[2]][0];
			root.uv2[1] = t.uv[r[2]][1];
			root.mesh = slot;
			// The corners in canonical order (by absolute position), so a point two roots share sums the same terms in
			// the same order (VertexCS).
			int order[3] = { 0, 1, 2 };
			std::sort(order, order + 3, [&](int a, int b) { return q[a] < q[b] || (!(q[b] < q[a]) && a < b); });
			root.order = static_cast<uint32_t>(order[0] | (order[1] << 2) | (order[2] << 4));
			root.neighbours[0] = root.neighbours[1] = root.neighbours[2] = None;
			root.neighbours[3] = mesh.source.river ? 1u : 0u;
			rootData.push_back(root);
			quantized.push_back(q);
		}
	}
	stats.meshes = static_cast<uint32_t>(slots.size());
	stats.roots = static_cast<uint32_t>(rootData.size());
	if (rootData.empty()) {
		persistent = false;
		return;
	}
	// Other roots (a cell loaded or dropped, a mesh changed): the subdivision starts over from them.
	const bool reset = !persistent || signature != rootSignature || stats.roots != rootCount;
	rootSignature = signature;
	rootCount = stats.roots;

	// Each edge's other root, across meshes as well: edge k runs from corner k to corner k + 1.
	std::unordered_map<EdgeKey, uint32_t, EdgeKeyHash> open;
	open.reserve(rootData.size() * 3);
	for (uint32_t i = 0; i < rootData.size(); ++i) {
		for (uint32_t k = 0; k < 3; ++k) {
			EdgeKey key{ quantized[i][k], quantized[i][(k + 1) % 3] };
			if (key.b < key.a)
				std::swap(key.a, key.b);
			const auto [it, inserted] = open.try_emplace(key, i * 3 + k);
			if (inserted)
				continue;
			const uint32_t j = it->second / 3, f = it->second % 3;
			if (j != i && rootData[j].neighbours[f] == None) {
				rootData[i].neighbours[k] = j;
				rootData[j].neighbours[f] = i;
				stats.twins += 2;
			}
		}
	}

	auto context = globals::d3d::context;
	if (rootData.size() > rootCapacity || !roots) {
		rootCapacity = std::max<uint32_t>(static_cast<uint32_t>(rootData.size()) * 2, 1024);
		roots = CreateStructured(sizeof(GpuRoot), rootCapacity, false, "PBRWater::BisectionRoots");
	}
	const D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(rootData.size() * sizeof(GpuRoot)), 1, 1 };
	context->UpdateSubresource(roots->resource.get(), 0, &box, rootData.data(), 0, 0);

	BisectionCB cb{};
	cb.counts[0] = stats.roots;
	cb.counts[1] = Capacity;
	cb.counts[2] = stats.meshes;
	cb.cull[0] = params.cullMarginXY;
	cb.cull[1] = params.cullMarginZ;
	cb.cull[2] = static_cast<float>(MaxDepth);
	std::memcpy(cb.viewProj, params.viewProj, sizeof(cb.viewProj));
	constants->Update(cb);

	PBRWaterProfiling::Scope pass("PBRWater::Tessellate");
	ID3D11Buffer* cbs[1] = { constants->CB() };
	context->CSSetConstantBuffers(0, 1, cbs);
	ID3D11ShaderResourceView* srv = roots->srv.get();
	context->CSSetShaderResources(0, 1, &srv);
	const UINT zero[4] = {};
	context->ClearUnorderedAccessViewUint(meshCountsUAV.get(), zero);
	ReadBackState();
	// Starting over takes a level an iteration (and the forced chains); following the view, an iteration or two.
	const uint32_t iterations = reset ? MaxIterations : iterationCap;

	auto bind = [&] {
		ID3D11UnorderedAccessView* uavs[8] = { bisectors[current]->uav.get(), bisectors[1 - current]->uav.get(), childB->uav.get(), stateUAV.get(),
			meshCountsUAV.get(), drawArgsUAV.get(), list->uav.get(), vertices->uav.get() };
		context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
	};
	auto copyArgs = [&](uint32_t from, uint32_t to) {
		const D3D11_BOX args{ from, 0, 0, from + 12, 1, 1 };
		context->CopySubresourceRegion(dispatchArgs.get(), 0, to, 0, 0, state.get(), 0, &args);
	};
	auto run = [&](ID3D11ComputeShader* cs) {
		context->CSSetShader(cs, nullptr, 0);
		context->DispatchIndirect(dispatchArgs.get(), 0);
	};

	auto swap = [&] {
		current = 1 - current;
		bind();
	};
	auto single = [&](ID3D11ComputeShader* cs) {
		context->CSSetShader(cs, nullptr, 0);
		context->Dispatch(1, 1, 1);
	};

	bind();
	{
		PBRWaterProfiling::Scope refine("PBRWater::BisectionRefine");
		if (reset) {
			context->CSSetShader(initCS.get(), nullptr, 0);
			context->Dispatch((stats.roots + 63) / 64, 1, 1);
		} else {
			// Last frame's subdivision: a level of merges where the view no longer needs it, then packed to the front.
			single(frameStartCS.get());
			copyArgs(StateDispatch, 0);
			run(mergeDecideCS.get());
			run(mergeLinkCS.get());
			swap();
			run(compactCountCS.get());
			run(compactWriteCS.get());
			swap();
			single(compactArgsCS.get());
		}
		for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
			// Zero groups once an iteration changed nothing (ArgsCS).
			copyArgs(StateIterate, 0);
			run(classifyCS.get());
			run(linkCS.get());
			single(argsCS.get());
			swap();
		}
		// The iterations that dispatched nothing still swapped the two buffers: the subdivision back into the bound one.
		copyArgs(StateDispatch, 0);
		run(fixupCS.get());
		// The last iteration's children have no culling flags yet: classify once more (its split marks go unused).
		run(classifyCS.get());
		run(countCS.get());
		context->CSSetShader(offsetsCS.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);
		run(scatterCS.get());
	}
	{
		PBRWaterProfiling::Scope vertexPass("PBRWater::BisectionVertices");
		copyArgs(StateVertexDispatch, 16);
		context->CSSetShader(vertexCS.get(), nullptr, 0);
		context->DispatchIndirect(dispatchArgs.get(), 16);
	}
	if (stateStaging[stagingSlot]) {
		context->CopyResource(stateStaging[stagingSlot].get(), state.get());
		stagingSlot = (stagingSlot + 1) % 3;
	}

	ID3D11UnorderedAccessView* nullUavs[8] = {};
	context->CSSetUnorderedAccessViews(0, 8, nullUavs, nullptr);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	context->CSSetShaderResources(0, 1, &nullSrv);
	context->CSSetShader(nullptr, nullptr, 0);

	persistent = true;
	builtFrame = params.frame;
	builtCamera = { static_cast<float>(params.cameraX), static_cast<float>(params.cameraY), static_cast<float>(params.cameraZ), 0.0f };
	built = true;
	stats.cpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
	stats.bisectors = gpuStats.bisectors;
	stats.listed = gpuStats.listed;
	stats.iterations = gpuStats.iterations;
	stats.iterationCap = iterations;
}
