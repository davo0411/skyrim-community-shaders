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
	/// Grid applied to absolute positions before two roots' edges are matched (as WaterTessellator's Quantum).
	constexpr double Quantum = 1.0 / 16.0;
	/// State buffer layout (BisectionCS.hlsl): count, cursor, end, listed, two dispatch arguments, the reach.
	constexpr uint32_t StateBytes = 64;
	constexpr uint32_t StateDispatch = 16;
	constexpr uint32_t StateVertexDispatch = 32;

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

	bool CreateRaw(UINT bytes, UINT misc, const char* name, winrt::com_ptr<ID3D11Buffer>& buffer, winrt::com_ptr<ID3D11UnorderedAccessView>* uav)
	{
		auto device = globals::d3d::device;
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = bytes;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = uav ? D3D11_BIND_UNORDERED_ACCESS : 0u;
		desc.MiscFlags = misc | (uav ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : 0u);
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
	vertices = CreateStructured(sizeof(WaterTessellator::GpuVertex), Capacity * GridVertices, true, "PBRWater::BisectionVertices");
	bool ok = CreateRaw(StateBytes, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, "PBRWater::BisectionState", state, &stateUAV);
	ok = ok && CreateRaw(MaxMeshes * 4, 0, "PBRWater::BisectionMeshCounts", meshCounts, &meshCountsUAV);
	ok = ok && CreateRaw(MaxMeshes * 16, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, "PBRWater::BisectionDrawArgs", drawArgs, &drawArgsUAV);
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
	classifyCS = compile("ClassifyCS");
	allocateCS = compile("AllocateCS");
	linkCS = compile("LinkCS");
	argsCS = compile("ArgsCS");
	countCS = compile("CountCS");
	offsetsCS = compile("OffsetsCS");
	scatterCS = compile("ScatterCS");
	vertexCS = compile("VertexCS");
	ready = initCS && classifyCS && allocateCS && linkCS && argsCS && countCS && offsetsCS && scatterCS && vertexCS && constants && bisectors[0] &&
	        bisectors[1] && childB && list && vertices && stateUAV && meshCountsUAV && drawArgsUAV && dispatchArgs;
	if (!ready)
		logger::warn("[PBR Water] Bisection tessellation shaders failed to compile; the patch tessellator is used instead");
	Clear();
}

void BisectionTessellator::Clear()
{
	built = false;
	slotOf.clear();
	slots.clear();
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
	if (it == slotOf.end() || !WaterTessellator::StillSource(geometry, slots[it->second].source))
		return false;
	slot = it->second;
	return true;
}

void BisectionTessellator::Draw(uint32_t slot) const
{
	if (!built || slot >= slots.size())
		return;
	auto context = globals::d3d::context;
	ID3D11ShaderResourceView* srv = vertices->srv.get();
	context->VSSetShaderResources(109, 1, &srv);
	context->DrawInstancedIndirect(drawArgs.get(), slot * 16);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	context->VSSetShaderResources(109, 1, &nullSrv);
}

void BisectionTessellator::Build(const WaterTessellator::FrameParams& params, WaterTessellator& meshes)
{
	const auto cpuStart = std::chrono::steady_clock::now();
	Clear();
	std::vector<WaterTessellator::WorldMesh> source;
	meshes.TakeBaseTriangles(source);
	stats = {};
	if (!ready || source.empty())
		return;

	// The roots: every base triangle, counter-clockwise, its longest edge as edge 2 (corner 2 to corner 0).
	rootData.clear();
	std::vector<std::array<Point, 3>> quantized;
	const double camera[3] = { params.cameraX, params.cameraY, params.cameraZ };
	for (const auto& mesh : source) {
		if (slots.size() >= MaxMeshes)
			break;
		const uint32_t slot = static_cast<uint32_t>(slots.size());
		slots.push_back({ mesh.source });
		slotOf[mesh.geometry] = slot;
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
			root.neighbours[0] = root.neighbours[1] = root.neighbours[2] = root.neighbours[3] = None;
			rootData.push_back(root);
			quantized.push_back(q);
		}
	}
	stats.meshes = static_cast<uint32_t>(slots.size());
	stats.roots = static_cast<uint32_t>(rootData.size());
	if (rootData.empty()) {
		Clear();
		return;
	}

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

	uint32_t current = 0;
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

	bind();
	context->CSSetShader(initCS.get(), nullptr, 0);
	context->Dispatch((stats.roots + 63) / 64, 1, 1);
	for (uint32_t iteration = 0; iteration < MaxIterations; ++iteration) {
		copyArgs(StateDispatch, 0);
		run(classifyCS.get());
		run(allocateCS.get());
		run(linkCS.get());
		context->CSSetShader(argsCS.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);
		current = 1 - current;
		bind();
	}
	// The last iteration's children have no culling flags yet: classify once more (its split marks go unused).
	copyArgs(StateDispatch, 0);
	run(classifyCS.get());
	run(countCS.get());
	context->CSSetShader(offsetsCS.get(), nullptr, 0);
	context->Dispatch(1, 1, 1);
	run(scatterCS.get());
	copyArgs(StateVertexDispatch, 16);
	context->CSSetShader(vertexCS.get(), nullptr, 0);
	context->DispatchIndirect(dispatchArgs.get(), 16);

	ID3D11UnorderedAccessView* nullUavs[8] = {};
	context->CSSetUnorderedAccessViews(0, 8, nullUavs, nullptr);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	context->CSSetShaderResources(0, 1, &nullSrv);
	context->CSSetShader(nullptr, nullptr, 0);

	builtFrame = params.frame;
	builtCamera = { static_cast<float>(params.cameraX), static_cast<float>(params.cameraY), static_cast<float>(params.cameraZ), 0.0f };
	built = true;
	stats.cpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
}
