#include "WaterMeshes.h"

#include "Globals.h"
#include "Utils/D3D.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
	/// Meshes with more triangles are drawn untessellated (they already carry their own detail).
	constexpr uint32_t MaxMeshTriangles = 4096;
	/// Meshes not drawn for this many frames are read back again if they return.
	constexpr uint32_t MeshLifetime = 600;

	/**
	 * A mesh that is one flat rectangle cut into a grid (the cells' water: 32 x 32 quads, 2048 triangles) becomes its
	 * rectangle's two triangles, so the subdivision alone sets its detail. Cut along the grid, every one of its 2048 triangles
	 * was a root to refine every frame, near or far. Its UVs
	 * and colours must vary affinely over it (the rectangle's corners then carry them), and the triangles must tile the
	 * rectangle exactly. Neighbouring cells share whole rectangle sides.
	 * @return whether the mesh was merged
	 */
	template <class CornerT>
	bool MergeRectangle(std::vector<CornerT>& corners)
	{
		if (corners.size() < 6 || corners.size() % 3 != 0)
			return false;
		const double z = corners[0].position[2];
		double minX = corners[0].position[0], maxX = minX, minY = corners[0].position[1], maxY = minY;
		double area = 0.0;
		int winding = 0;
		for (size_t t = 0; t < corners.size(); t += 3) {
			for (size_t i = t; i < t + 3; ++i) {
				const auto& p = corners[i].position;
				if (std::abs(p[2] - z) > 1e-3)
					return false;
				minX = std::min(minX, static_cast<double>(p[0]));
				maxX = std::max(maxX, static_cast<double>(p[0]));
				minY = std::min(minY, static_cast<double>(p[1]));
				maxY = std::max(maxY, static_cast<double>(p[1]));
			}
			const auto &a = corners[t].position, &b = corners[t + 1].position, &c = corners[t + 2].position;
			const double cross = (static_cast<double>(b[0]) - a[0]) * (static_cast<double>(c[1]) - a[1]) - (static_cast<double>(b[1]) - a[1]) * (static_cast<double>(c[0]) - a[0]);
			if (cross == 0.0)
				continue;
			const int sign = cross > 0.0 ? 1 : -1;
			if (winding != 0 && sign != winding)
				return false;
			winding = sign;
			area += 0.5 * std::abs(cross);
		}
		const double rectangle = (maxX - minX) * (maxY - minY);
		if (winding == 0 || rectangle <= 0.0 || std::abs(area - rectangle) > 1e-5 * rectangle)
			return false;

		// The attributes at a point, interpolated from the mesh's own vertices at three of the rectangle's corners (affine
		// over the plane): spanning the whole rectangle, they carry the half-float UVs' rounding no further than it is.
		auto nearest = [&](double x, double y) {
			size_t best = 0;
			double bestDistance = std::numeric_limits<double>::max();
			for (size_t i = 0; i < corners.size(); ++i) {
				const double dx = corners[i].position[0] - x, dy = corners[i].position[1] - y;
				if (dx * dx + dy * dy < bestDistance) {
					bestDistance = dx * dx + dy * dy;
					best = i;
				}
			}
			return corners[best];
		};
		const CornerT c0 = nearest(minX, minY), c1 = nearest(maxX, minY), c2 = nearest(minX, maxY);
		const auto &r0 = c0.position, &r1 = c1.position, &r2 = c2.position;
		const double e1x = r1[0] - r0[0], e1y = r1[1] - r0[1], e2x = r2[0] - r0[0], e2y = r2[1] - r0[1];
		const double det = e1x * e2y - e1y * e2x;
		if (std::abs(det) < 0.5 * rectangle)
			return false;
		auto at = [&](double x, double y) {
			const double px = x - r0[0], py = y - r0[1];
			const double w1 = (px * e2y - py * e2x) / det, w2 = (e1x * py - e1y * px) / det, w0 = 1.0 - w1 - w2;
			CornerT out{};
			out.position[0] = static_cast<float>(x);
			out.position[1] = static_cast<float>(y);
			out.position[2] = static_cast<float>(z);
			for (int k = 0; k < 2; ++k)
				out.uv[k] = static_cast<float>(w0 * c0.uv[k] + w1 * c1.uv[k] + w2 * c2.uv[k]);
			for (int k = 0; k < 4; ++k)
				out.color[k] = static_cast<float>(w0 * c0.color[k] + w1 * c1.color[k] + w2 * c2.color[k]);
			return out;
		};
		// Every corner must lie on that plane of attributes (UVs to a texel of a 4096 texture, colours to 8 bits).
		for (const auto& c : corners) {
			const auto fit = at(c.position[0], c.position[1]);
			for (int k = 0; k < 2; ++k) {
				if (std::abs(fit.uv[k] - c.uv[k]) > 1e-3f + 1e-3f * std::abs(c.uv[k]))
					return false;
			}
			for (int k = 0; k < 4; ++k) {
				if (std::abs(fit.color[k] - c.color[k]) > 1.0f / 255.0f)
					return false;
			}
		}

		const auto c00 = at(minX, minY), c10 = at(maxX, minY), c11 = at(maxX, maxY), c01 = at(minX, maxY);
		if (winding > 0)
			corners = { c00, c10, c11, c00, c11, c01 };
		else
			corners = { c00, c11, c10, c00, c01, c11 };
		return true;
	}

	bool ReadBack(ID3D11Buffer* buffer, std::vector<uint8_t>& out)
	{
		if (!buffer)
			return false;
		D3D11_BUFFER_DESC desc{};
		buffer->GetDesc(&desc);
		if (desc.ByteWidth == 0)
			return false;
		D3D11_BUFFER_DESC staging{};
		staging.ByteWidth = desc.ByteWidth;
		staging.Usage = D3D11_USAGE_STAGING;
		staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		winrt::com_ptr<ID3D11Buffer> copy;
		if (FAILED(globals::d3d::device->CreateBuffer(&staging, nullptr, copy.put())))
			return false;
		Util::SetResourceName(copy.get(), "PBRWater::WaterMeshReadback");
		auto context = globals::d3d::context;
		context->CopyResource(copy.get(), buffer);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(copy.get(), 0, D3D11_MAP_READ, 0, &mapped)))
			return false;
		out.resize(desc.ByteWidth);
		std::memcpy(out.data(), mapped.pData, desc.ByteWidth);
		context->Unmap(copy.get(), 0);
		return true;
	}
}

bool WaterMeshes::SameSource(const Source& a, const Source& b)
{
	return a.rendererData == b.rendererData && a.triangleCount == b.triangleCount && a.vertexDesc == b.vertexDesc &&
	       std::memcmp(&a.world, &b.world, sizeof(RE::NiTransform)) == 0;
}

bool WaterMeshes::IsRiver(RE::BSGeometry& geometry)
{
	const auto prop = geometry.GetGeometryRuntimeData().shaderProperty.get();
	if (!prop || prop->GetRTTI() != globals::rtti::BSWaterShaderPropertyRTTI.get())
		return false;
	return static_cast<RE::BSWaterShaderProperty*>(prop)->waterFlags.any(RE::BSWaterShaderProperty::WaterFlag::kEnableFlowmap);
}

bool WaterMeshes::MakeSource(RE::BSGeometry& geometry, Source& s)
{
	auto* triShape = geometry.AsTriShape();
	auto& data = geometry.GetGeometryRuntimeData();
	if (!triShape || !data.rendererData)
		return false;
	s.world = geometry.world;
	s.rendererData = data.rendererData;
	s.triangleCount = triShape->GetTrishapeRuntimeData().triangleCount;
	s.vertexDesc = std::bit_cast<uint64_t>(data.vertexDesc);
	s.river = IsRiver(geometry);
	return s.triangleCount > 0 && s.triangleCount <= MaxMeshTriangles;
}

void WaterMeshes::Register(RE::BSGeometry& geometry)
{
	Source s;
	if (MakeSource(geometry, s))
		registered[&geometry] = s;
}

void WaterMeshes::TakeBaseTriangles(std::vector<WorldMesh>& out)
{
	out.clear();
	++frame;
	auto sources = std::move(registered);
	registered.clear();
	for (const auto& [geometry, source] : sources) {
		const Mesh* mesh = GetMesh(source);
		if (!mesh)
			continue;
		WorldMesh& world = out.emplace_back();
		world.geometry = geometry;
		world.source = source;
		const auto& w = source.world;
		world.triangles.resize(mesh->corners.size() / 3);
		for (size_t t = 0; t < world.triangles.size(); ++t) {
			WorldTriangle& tri = world.triangles[t];
			for (int i = 0; i < 3; ++i) {
				const Corner& c = mesh->corners[t * 3 + i];
				const double s = w.scale;
				const double x = c.position[0] * s, y = c.position[1] * s, z = c.position[2] * s;
				tri.world[i][0] = w.translate.x + w.rotate.entry[0][0] * x + w.rotate.entry[0][1] * y + w.rotate.entry[0][2] * z;
				tri.world[i][1] = w.translate.y + w.rotate.entry[1][0] * x + w.rotate.entry[1][1] * y + w.rotate.entry[1][2] * z;
				tri.world[i][2] = w.translate.z + w.rotate.entry[2][0] * x + w.rotate.entry[2][1] * y + w.rotate.entry[2][2] * z;
				std::copy_n(c.position, 3, tri.model[i]);
				std::copy_n(c.uv, 2, tri.uv[i]);
				std::copy_n(c.color, 4, tri.color[i]);
			}
		}
	}
	std::erase_if(meshes, [&](const auto& entry) { return frame - entry.second.lastUsed > MeshLifetime; });
}

bool WaterMeshes::StillSource(RE::BSGeometry& geometry, const Source& source)
{
	Source s;
	return MakeSource(geometry, s) && SameSource(s, source);
}

const WaterMeshes::Mesh* WaterMeshes::GetMesh(const Source& source)
{
	const auto* shape = static_cast<const RE::BSGraphics::TriShape*>(source.rendererData);
	auto* vb = reinterpret_cast<ID3D11Buffer*>(shape->vertexBuffer);
	auto* ib = reinterpret_cast<ID3D11Buffer*>(shape->indexBuffer);
	auto& mesh = meshes[source.rendererData];
	mesh.lastUsed = frame;
	// Read once, including when that failed: a mesh that cannot be read is drawn untessellated, not read every frame.
	if (mesh.read && mesh.vertexBuffer.get() == vb && mesh.triangleCount == source.triangleCount && mesh.vertexDesc == source.vertexDesc)
		return mesh.valid ? &mesh : nullptr;

	// New (or rebuilt) mesh: read its buffers back once.
	mesh = {};
	mesh.lastUsed = frame;
	mesh.read = true;
	mesh.triangleCount = source.triangleCount;
	mesh.vertexDesc = source.vertexDesc;
	mesh.vertexBuffer.copy_from(vb);
	std::vector<uint8_t> vertices, indices;
	if (!vb || !ib || !ReadBack(vb, vertices) || !ReadBack(ib, indices))
		return nullptr;

	const uint64_t desc = source.vertexDesc;
	const uint32_t stride = static_cast<uint32_t>((4 * desc) & 0x3C);
	const uint32_t flags = static_cast<uint32_t>(desc >> 44);
	if (stride < 8 || vertices.size() < stride)
		return nullptr;
	const uint32_t vertexCount = static_cast<uint32_t>(vertices.size() / stride);
	auto offsetOf = [&](RE::BSGraphics::Vertex::Attribute a) { return static_cast<uint32_t>((desc >> (4 * a + 2)) & 0x3C); };
	const bool hasUV = (flags & RE::BSGraphics::Vertex::VF_UV) != 0;
	const bool hasColor = (flags & RE::BSGraphics::Vertex::VF_COLORS) != 0;
	const uint32_t uvOffset = offsetOf(RE::BSGraphics::Vertex::VA_TEXCOORD0);
	const uint32_t colorOffset = offsetOf(RE::BSGraphics::Vertex::VA_COLOR);
	// The position is 16 bytes (float3 + bitangent x) at full precision, 8 (half4) otherwise. The water meshes are full
	// precision without VF_FULLPREC in their descriptor, so the layout decides: the position spans up to the first
	// attribute after it (the whole vertex when it is alone).
	uint32_t positionSize = stride;
	for (uint32_t a = RE::BSGraphics::Vertex::VA_TEXCOORD0; a < RE::BSGraphics::Vertex::VA_COUNT; ++a) {
		const auto attribute = static_cast<RE::BSGraphics::Vertex::Attribute>(a);
		const uint32_t offset = offsetOf(attribute);
		if ((flags & (1u << a)) && offset > 0)
			positionSize = std::min(positionSize, offset);
	}
	const bool fullPrecision = (flags & RE::BSGraphics::Vertex::VF_FULLPREC) != 0 || positionSize >= 12;
	if ((fullPrecision && stride < 12) || (hasUV && uvOffset + 4 > stride) || (hasColor && colorOffset + 4 > stride))
		return nullptr;
	const size_t indexCount = static_cast<size_t>(source.triangleCount) * 3;
	if (indices.size() < indexCount * sizeof(uint16_t))
		return nullptr;

	auto half = [](const uint8_t* p) {
		uint16_t h;
		std::memcpy(&h, p, sizeof(h));
		return DirectX::PackedVector::XMConvertHalfToFloat(h);
	};
	mesh.corners.resize(indexCount);
	for (size_t i = 0; i < indexCount; ++i) {
		uint16_t index;
		std::memcpy(&index, indices.data() + i * sizeof(uint16_t), sizeof(index));
		if (index >= vertexCount)
			return nullptr;
		const uint8_t* v = vertices.data() + static_cast<size_t>(index) * stride;
		Corner& c = mesh.corners[i];
		for (int k = 0; k < 3; ++k) {
			if (fullPrecision)
				std::memcpy(&c.position[k], v + 4 * k, sizeof(float));
			else
				c.position[k] = half(v + 2 * k);
			if (!std::isfinite(c.position[k]))
				return nullptr;
		}
		c.uv[0] = hasUV ? half(v + uvOffset) : 0.0f;
		c.uv[1] = hasUV ? half(v + uvOffset + 2) : 0.0f;
		for (int k = 0; k < 4; ++k)
			c.color[k] = hasColor ? static_cast<float>(v[colorOffset + k]) / 255.0f : 1.0f;
	}
	MergeRectangle(mesh.corners);
	mesh.valid = true;
	return &mesh;
}

bool WaterMeshes::AffineTexCoords(RE::BSGeometry& geometry, float4& mapU, float4& mapV) const
{
	Source source;
	if (!MakeSource(geometry, source))
		return false;
	const auto mesh = meshes.find(source.rendererData);
	if (mesh == meshes.end() || !mesh->second.valid || mesh->second.corners.size() != 6)
		return false;
	const auto& c = mesh->second.corners;
	const double e1x = c[1].position[0] - c[0].position[0], e1y = c[1].position[1] - c[0].position[1];
	const double e2x = c[2].position[0] - c[0].position[0], e2y = c[2].position[1] - c[0].position[1];
	const double det = e1x * e2y - e1y * e2x;
	if (std::abs(det) < 1e-6)
		return false;
	double map[2][3];
	for (int k = 0; k < 2; ++k) {
		const double d1 = c[1].uv[k] - c[0].uv[k], d2 = c[2].uv[k] - c[0].uv[k];
		const double ddx = (d1 * e2y - d2 * e1y) / det, ddy = (e1x * d2 - e2x * d1) / det;
		map[k][0] = ddx;
		map[k][1] = ddy;
		map[k][2] = c[0].uv[k] - ddx * c[0].position[0] - ddy * c[0].position[1];
	}
	// Both triangles on that plane (a merged rectangle always is; two triangles of their own need not be).
	for (const auto& corner : c) {
		for (int k = 0; k < 2; ++k) {
			const double fit = map[k][0] * corner.position[0] + map[k][1] * corner.position[1] + map[k][2];
			if (std::abs(fit - corner.uv[k]) > 1e-3 + 1e-3 * std::abs(corner.uv[k]))
				return false;
		}
	}
	mapU = { static_cast<float>(map[0][0]), static_cast<float>(map[0][1]), static_cast<float>(map[0][2]), 0.0f };
	mapV = { static_cast<float>(map[1][0]), static_cast<float>(map[1][1]), static_cast<float>(map[1][2]), 0.0f };
	return true;
}
