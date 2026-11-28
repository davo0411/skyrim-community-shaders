#include "RiverLattice.h"
#include "WaterMeshes.h"

#include "Globals.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

namespace
{
	/// Water.hlsl RiverCorner.
	struct GpuCorner
	{
		float position[3];
		float uv[2];
		uint32_t color;  ///< RGBA8
	};
	static_assert(sizeof(GpuCorner) == 24);

	constexpr UINT TrianglesSlot = 106;   ///< Water.hlsl RiverTriangles, vertex stage
	constexpr UINT CornersSlot = 108;     ///< Water.hlsl RiverCorners, vertex stage
	constexpr uint32_t EvictAfter = 600;  ///< frames a mesh read back stays unused before it is dropped

	uint32_t LevelOf(uint32_t index) { return 1u << index; }

	/// Water.hlsl RiverEdgeCuts before its clamp, for camera-relative corners: the edge's length on screen in target edges.
	float EdgeCuts(const float a[3], const float b[3], float pixelsPerUnit, float targetPixels)
	{
		const float m[3] = { 0.5f * (a[0] + b[0]), 0.5f * (a[1] + b[1]), 0.5f * (a[2] + b[2]) };
		const float e[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		const float d2 = std::max(m[0] * m[0] + m[1] * m[1] + m[2] * m[2], 1.0f);
		const float along = e[0] * m[0] + e[1] * m[1] + e[2] * m[2];
		const float across = std::sqrt(std::max(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] - along * along / d2, 0.0f));
		return across * pixelsPerUnit / (std::sqrt(d2) * targetPixels);
	}
}

void RiverLattice::Setup()
{
	auto device = globals::d3d::device;
	for (uint32_t k = 0; k < Levels; ++k) {
		// Lattice vertex (i, j), i + j <= n, rows j of n + 1 - j (Water.hlsl RiverLatticeVertex decodes it); the triangles keep
		// the source triangle's winding (corner 0 at (0, 0), 1 at (n, 0), 2 at (0, n)).
		const uint32_t n = LevelOf(k);
		auto index = [n](uint32_t i, uint32_t j) { return static_cast<uint16_t>(j * (n + 1) - j * (j - 1) / 2 + i); };
		std::vector<uint16_t> list;
		for (uint32_t j = 0; j < n; ++j) {
			for (uint32_t i = 0; i + j < n; ++i) {
				list.insert(list.end(), { index(i, j), index(i + 1, j), index(i, j + 1) });
				if (i + j + 1 < n)
					list.insert(list.end(), { index(i + 1, j), index(i + 1, j + 1), index(i, j + 1) });
			}
		}
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = static_cast<UINT>(list.size() * sizeof(uint16_t));
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		D3D11_SUBRESOURCE_DATA data{ list.data(), 0, 0 };
		if (SUCCEEDED(device->CreateBuffer(&desc, &data, indices[k].put())))
			Util::SetResourceName(indices[k].get(), "PBRWater::RiverLatticeIndices");
		indexCounts[k] = static_cast<uint32_t>(list.size());
	}
	for (uint32_t river = 0; river < 2; ++river) {
		for (uint32_t culled = 0; culled < 2; ++culled) {
			engineConstants[river][culled] = std::make_unique<ConstantBuffer>(ConstantBufferDesc<float4>(), "PBRWater::RiverDrawConstants");
			engineConstants[river][culled]->Update(float4{ static_cast<float>(river), culled ? -1.0f : 0.0f, 0.0f, 0.0f });
		}
	}
	drawConstants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<float4>(), "PBRWater::RiverLatticeConstants");
}

RiverLattice::Mesh* RiverLattice::Find(RE::BSGeometry& geometry, uint32_t frame)
{
	auto* triShape = geometry.AsTriShape();
	auto& data = geometry.GetGeometryRuntimeData();
	if (!triShape || !data.rendererData)
		return nullptr;
	const auto* shape = static_cast<const RE::BSGraphics::TriShape*>(data.rendererData);
	const uint32_t triangles = triShape->GetTrishapeRuntimeData().triangleCount;
	const uint64_t vertexDesc = std::bit_cast<uint64_t>(data.vertexDesc);
	Evict(frame);
	auto& mesh = meshes[data.rendererData];
	// A renderer's data freed and reused for another mesh: read again.
	if (mesh.triangles != triangles || mesh.vertexDesc != vertexDesc || mesh.vertexBuffer != shape->vertexBuffer) {
		mesh = Mesh{};
		mesh.triangles = triangles;
		mesh.vertexDesc = vertexDesc;
		mesh.vertexBuffer = shape->vertexBuffer;
		std::vector<WaterMeshes::Corner> corners;
		mesh.failed = !WaterMeshes::Triangles(geometry, corners) || corners.size() != static_cast<size_t>(triangles) * 3;
		if (!mesh.failed) {
			std::vector<GpuCorner> gpu;
			gpu.reserve(corners.size());
			mesh.positions.reserve(corners.size() * 3);
			for (size_t t = 0; t < corners.size(); t += 3) {
				for (int e = 0; e < 3; ++e) {
					const auto& a = corners[t + e].position;
					const auto& b = corners[t + (e + 1) % 3].position;
					const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
					mesh.longestEdge = std::max(mesh.longestEdge, std::sqrt(dx * dx + dy * dy + dz * dz));
				}
			}
			for (const auto& c : corners) {
				GpuCorner g{ { c.position[0], c.position[1], c.position[2] }, { c.uv[0], c.uv[1] }, 0 };
				for (int k = 0; k < 4; ++k)
					g.color |= static_cast<uint32_t>(std::lround(std::clamp(c.color[k], 0.0f, 1.0f) * 255.0f)) << (8 * k);
				gpu.push_back(g);
				mesh.positions.insert(mesh.positions.end(), { c.position[0], c.position[1], c.position[2] });
			}
			auto device = globals::d3d::device;
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = static_cast<UINT>(gpu.size() * sizeof(GpuCorner));
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(GpuCorner);
			D3D11_SUBRESOURCE_DATA init{ gpu.data(), 0, 0 };
			winrt::com_ptr<ID3D11Buffer> buffer;
			mesh.failed = FAILED(device->CreateBuffer(&desc, &init, buffer.put())) || FAILED(device->CreateShaderResourceView(buffer.get(), nullptr, mesh.corners.put()));
			if (!mesh.failed) {
				Util::SetResourceName(buffer.get(), "PBRWater::RiverLatticeCorners");
				Util::SetResourceName(mesh.corners.get(), "PBRWater::RiverLatticeCorners SRV");
				D3D11_BUFFER_DESC orderDesc{};
				orderDesc.ByteWidth = triangles * sizeof(uint32_t);
				orderDesc.Usage = D3D11_USAGE_DEFAULT;
				orderDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				orderDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				orderDesc.StructureByteStride = sizeof(uint32_t);
				mesh.failed = FAILED(device->CreateBuffer(&orderDesc, nullptr, mesh.order.put())) ||
				              FAILED(device->CreateShaderResourceView(mesh.order.get(), nullptr, mesh.orderSRV.put()));
				if (!mesh.failed) {
					Util::SetResourceName(mesh.order.get(), "PBRWater::RiverLatticeTriangles");
					Util::SetResourceName(mesh.orderSRV.get(), "PBRWater::RiverLatticeTriangles SRV");
				}
			}
		}
	}
	mesh.usedFrame = frame;
	return mesh.failed ? nullptr : &mesh;
}

void RiverLattice::Evict(uint32_t frame)
{
	if (frame - evictedFrame < 60)
		return;
	evictedFrame = frame;
	std::erase_if(meshes, [frame](const auto& entry) { return frame - entry.second.usedFrame > EvictAfter; });
}

bool RiverLattice::Sort(Mesh& mesh, RE::BSGeometry& geometry, float pixelsPerUnit, float targetPixels)
{
	// The corners camera-relative, as the shader's World takes them.
	const auto& world = geometry.world;
	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	auto place = [&](const float* p, float out[3]) {
		for (int r = 0; r < 3; ++r)
			out[r] = (world.rotate.entry[r][0] * p[0] + world.rotate.entry[r][1] * p[1] + world.rotate.entry[r][2] * p[2]) * world.scale;
		out[0] += world.translate.x - eye.x;
		out[1] += world.translate.y - eye.y;
		out[2] += world.translate.z - eye.z;
	};
	std::vector<uint8_t> levels(mesh.triangles);
	std::fill(std::begin(mesh.counts), std::end(mesh.counts), 0u);
	bool any = false;
	for (uint32_t t = 0; t < mesh.triangles; ++t) {
		float c[3][3];
		for (int v = 0; v < 3; ++v)
			place(&mesh.positions[(static_cast<size_t>(t) * 3 + v) * 3], c[v]);
		const float cuts = std::max({ EdgeCuts(c[0], c[1], pixelsPerUnit, targetPixels), EdgeCuts(c[0], c[2], pixelsPerUnit, targetPixels),
			EdgeCuts(c[1], c[2], pixelsPerUnit, targetPixels) });
		// 2% over the shader's count: its float rounding never cuts an edge more than its triangles' levels.
		const float needed = std::ceil(cuts * 1.02f);
		uint8_t level = 0;
		while (level + 1 < Levels && static_cast<float>(LevelOf(level)) < needed)
			++level;
		levels[t] = level;
		++mesh.counts[level];
		any = any || level > 0;
	}
	if (!any)
		return false;
	uint32_t offsets[Levels]{};
	for (uint32_t k = 1; k < Levels; ++k)
		offsets[k] = offsets[k - 1] + mesh.counts[k - 1];
	mesh.sorted.resize(mesh.triangles);
	for (uint32_t t = 0; t < mesh.triangles; ++t)
		mesh.sorted[offsets[levels[t]]++] = t;
	globals::d3d::context->UpdateSubresource(mesh.order.get(), 0, nullptr, mesh.sorted.data(), 0, 0);
	return true;
}

bool RiverLattice::Choose(RE::BSGeometry& geometry, float pixelsPerUnit, float targetPixels, float estimate, uint32_t frame)
{
	if (!indices[Levels - 1] || !drawConstants || pixelsPerUnit <= 0.0f || targetPixels <= 0.0f)
		return false;
	// Its nearest point: no edge of it stands longer on screen than its longest edge there.
	const auto& bound = geometry.worldBound;
	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float dx = bound.center.x - eye.x, dy = bound.center.y - eye.y, dz = bound.center.z - eye.z;
	const float nearest = std::max(std::sqrt(dx * dx + dy * dy + dz * dz) - bound.radius, 1.0f);
	// The bound's spacing understates a diagonal: twice it before reading the mesh back.
	if (2.0f * estimate * pixelsPerUnit / (nearest * targetPixels) <= 1.0f)
		return false;
	Mesh* mesh = Find(geometry, frame);
	if (!mesh || mesh->longestEdge * geometry.world.scale * pixelsPerUnit / (nearest * targetPixels) <= 1.0f)
		return false;
	// Every pass of the frame draws the same lattice (the stencil's depth and the surface's must match).
	if (mesh->sortedFrame != frame) {
		mesh->sortedFrame = frame;
		if (!Sort(*mesh, geometry, pixelsPerUnit, targetPixels))
			mesh->sorted.clear();
	}
	return !mesh->sorted.empty();
}

ID3D11Buffer* RiverLattice::EngineCB(bool river, bool lattice) const
{
	const auto& buffer = engineConstants[river ? 1 : 0][lattice ? 1 : 0];
	return buffer ? buffer->CB() : nullptr;
}

void RiverLattice::Draw(RE::BSGeometry& geometry, bool river)
{
	const auto found = meshes.find(geometry.GetGeometryRuntimeData().rendererData);
	if (found == meshes.end() || found->second.failed || found->second.sorted.empty())
		return;
	const Mesh& mesh = found->second;
	auto context = globals::d3d::context;

	winrt::com_ptr<ID3D11Buffer> engineIndices;
	DXGI_FORMAT engineFormat = DXGI_FORMAT_UNKNOWN;
	UINT engineOffset = 0;
	context->IAGetIndexBuffer(engineIndices.put(), &engineFormat, &engineOffset);
	D3D11_PRIMITIVE_TOPOLOGY topology{};
	context->IAGetPrimitiveTopology(&topology);

	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11ShaderResourceView* views[3] = { mesh.orderSRV.get(), nullptr, mesh.corners.get() };
	context->VSSetShaderResources(TrianglesSlot, 3, views);
	ID3D11Buffer* cb = drawConstants->CB();
	context->VSSetConstantBuffers(8, 1, &cb);
	// One instanced draw a level, one instance a triangle. The engine's input layout and vertex buffers stay bound: the
	// shader ignores what they fetch.
	uint32_t first = 0;
	for (uint32_t k = 0; k < Levels; ++k) {
		if (mesh.counts[k] == 0)
			continue;
		drawConstants->Update(float4{ river ? 1.0f : 0.0f, static_cast<float>(LevelOf(k)), static_cast<float>(first), 0.0f });
		context->IASetIndexBuffer(indices[k].get(), DXGI_FORMAT_R16_UINT, 0);
		context->DrawIndexedInstanced(indexCounts[k], mesh.counts[k], 0, 0, 0);
		first += mesh.counts[k];
	}

	ID3D11ShaderResourceView* none[3] = {};
	context->VSSetShaderResources(TrianglesSlot, 3, none);
	context->IASetPrimitiveTopology(topology);
	context->IASetIndexBuffer(engineIndices.get(), engineFormat, engineOffset);
}
