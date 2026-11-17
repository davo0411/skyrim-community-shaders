#include "ComputeTessellation.h"
#include "Profiling.h"

#include "Utils/D3D.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <string>

namespace
{
	/// A leaf spans this many target-sized triangles on screen (its grid is about this many segments across). The CPU's
	/// cost goes with the leaves, the GPU's with the vertices: larger leaves cost the CPU a quarter as much per doubling,
	/// for a little more detail on each leaf's far side (it takes its nearest side's).
	constexpr float LeafSegments = 32.0f;
	/// Deepest quadtree level: a 16384-unit triangle down to one unit. With the grid's 2^9 it keeps lattice
	/// coordinates within float's exact range on the GPU (ComputeTessellation.hlsli LatticeWeights: 23 bits).
	constexpr int MaxDepth = 14;
	/// The edges' 1-D trees reach at most two levels below the leaves along them (Split1D).
	constexpr int MaxEdgeDepth = MaxDepth + 2;
	/// Most points on one segment (a power of two): a leaf grid is at most four times that across.
	constexpr float MaxSegmentFactor = 128.0f;
	/// Meshes with more triangles are drawn untessellated (they already carry their own detail).
	constexpr uint32_t MaxMeshTriangles = 4096;
	constexpr uint32_t MaxVertices = 2u << 20;
	/// The vertices a frame's mesh aims for: past it the target triangle size grows until the mesh fits (Build).
	/// The vertices a frame's mesh aims for, one per this many render pixels: past it the target triangle size grows until
	/// the mesh fits (Build). Every pass of the frame draws them, and past about one vertex per handful of pixels the
	/// triangles only cost the pixel shader quads (measured at 1440p: twice the vertices, 1.5 times the surface pass, no
	/// visible difference: the shading's detail is the pixel shader's own).
	constexpr float PixelsPerVertex = 7.0f;
	constexpr uint32_t MinVertexBudget = 128u << 10;
	/// About 4M: the most one Dispatch of 64-thread groups covers (D3D11 allows 65535 groups along x).
	constexpr uint32_t MaxTriangles = 65535u * 64u;
	/// Grid applied to absolute positions before anything two meshes must agree on (edge order, edge trees).
	constexpr double Quantum = 1.0 / 16.0;
	/// The steepest a wave's face typically is (its slope): seen at a grazing angle, the water's profile still spans this
	/// much of a segment's length on screen, however flat the angle (Factor).
	constexpr float GrazingFloor = 0.1f;  // mirrored by PBRWater::TessellationGrazingFloor
	/// The least share of the target size a triangle keeps across the line of sight (Factor): at grazing angles it is at
	/// most this many times wider than the target, and keeps the target's height on the steepest wave faces up to there.
	constexpr float GrazingShareMin = 0.25f;  // mirrored by PBRWater::TessellationGrazingMin
	/// Where the curl detail fades in (Factor): from the deepest water a wave curls in to this share of it, and from a lip
	/// spacing of CurlLipFar to CurlLipNear deep-water wavelengths (a lip needs a few dozen vertices across its wave).
	/// Mirrored by PBRWater::ShoreCurlSpacing.
	constexpr float CurlDepthFade = 0.75f;
	constexpr float CurlLipFar = 0.6f / 25.0f;
	constexpr float CurlLipNear = 0.45f / 25.0f;
	/// Meshes not drawn for this many frames are read back again if they return.
	constexpr uint32_t MeshLifetime = 600;

	struct Vec3
	{
		double x = 0.0, y = 0.0, z = 0.0;
	};
	Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	Vec3 operator*(Vec3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
	double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	double Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
	bool Less(Vec3 a, Vec3 b)
	{
		if (a.x != b.x)
			return a.x < b.x;
		if (a.y != b.y)
			return a.y < b.y;
		return a.z < b.z;
	}
	Vec3 Quantize(Vec3 a)
	{
		return { std::round(a.x / Quantum) * Quantum, std::round(a.y / Quantum) * Quantum, std::round(a.z / Quantum) * Quantum };
	}

	/// Distance from p to segment ab.
	double SegmentDistance(Vec3 p, Vec3 a, Vec3 b)
	{
		const Vec3 ab = b - a;
		const double len2 = Dot(ab, ab);
		const double t = len2 > 0.0 ? std::clamp(Dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
		return Length(p - (a + ab * t));
	}

	/// Distance from p to triangle abc (Ericson, Real-Time Collision Detection 5.1.5).
	double TriangleDistance(Vec3 p, Vec3 a, Vec3 b, Vec3 c)
	{
		const Vec3 ab = b - a, ac = c - a, ap = p - a;
		const double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
		if (d1 <= 0.0 && d2 <= 0.0)
			return Length(ap);
		const Vec3 bp = p - b;
		const double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
		if (d3 >= 0.0 && d4 <= d3)
			return Length(bp);
		const double vc = d1 * d4 - d3 * d2;
		if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
			return Length(p - (a + ab * (d1 / (d1 - d3))));
		const Vec3 cp = p - c;
		const double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
		if (d6 >= 0.0 && d5 <= d6)
			return Length(cp);
		const double vb = d5 * d2 - d1 * d6;
		if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
			return Length(p - (a + ac * (d2 / (d2 - d6))));
		const double va = d3 * d6 - d5 * d4;
		if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
			return Length(p - (b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))));
		const double denom = 1.0 / (va + vb + vc);
		return Length(p - (a + ab * (vb * denom) + ac * (vc * denom)));
	}

	float Smoothstep(float e0, float e1, float x)
	{
		const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	/// Smallest e with 2^e >= f (f >= 1).
	/**
	 * The points a segment of `factor` gets: the power of two nearest to it (log2 rounded). Rounded up, every segment
	 * carried up to twice the points the target size asked for, and the triangles came out at half to all of it, with the
	 * newest level's half-morphed vertices cutting slivers between them: the pixel shader paid whole quads for those. The
	 * newest level morphs in over the half level below its own (TessellationMorph), so nothing pops.
	 */
	uint32_t PointsLog2(float factor)
	{
		uint32_t e = 0;
		while (static_cast<float>(1u << e) * 1.41421356f < factor && e < 15)
			++e;
		return e;
	}

	uint32_t VertexCount(uint32_t n) { return (n + 1) * (n + 2) / 2; }

	/// A point of a base triangle's lattice at some depth: the weights of its corners 1 and 2 are a / 2^d and b / 2^d.
	struct Lat
	{
		int64_t a = 0, b = 0;
	};
	Lat operator+(Lat p, Lat q) { return { p.a + q.a, p.b + q.b }; }
	Lat operator-(Lat p, Lat q) { return { p.a - q.a, p.b - q.b }; }
	Lat Twice(Lat p) { return { 2 * p.a, 2 * p.b }; }

	/// A cell of the lattice: a node of the quadtree. Corner order keeps the base triangle's winding.
	struct Cell
	{
		Lat p[3];
		int depth = 0;
	};

	/// One segment of a leaf's side, as encoded for the GPU.
	struct SideSegment
	{
		uint32_t log2Quarters;  // 0, 1 or 2: a quarter, a half or the whole side
		float factor;
	};

	struct Subdivider
	{
		const WaterTessellator::FrameParams& params;
		Vec3 camera;
		float targetSize = 10.0f;  // on-screen triangle size (px): the setting, coarsened to the vertex budget
		double k = 0.1;            // a cell splits while its longest side exceeds k times its distance
		double splitFloor = 0.0;   // and never below this (units): its grid already holds the finest spacing there is
		double splitFloor1 = 0.0;  // the edges' trees' floor, in the same proportion as k1 to k
		double k1 = 0.05;          // the edges' 1-D trees, finer by enough that a cell never waits on its edge (see Split1D)

		// The base triangle being cut.
		Vec3 corner[3];
		Vec3 edgeLo[3], edgeHi[3];  // each base edge's quantized ends in canonical order
		bool edgeReversed[3] = {};  // the edge runs from its higher end in the triangle's winding

		Vec3 Position(Lat l, int depth) const
		{
			const double s = std::ldexp(1.0, -depth);
			return corner[0] + (corner[1] - corner[0]) * (static_cast<double>(l.a) * s) + (corner[2] - corner[0]) * (static_cast<double>(l.b) * s);
		}

		/// The base edge a side lies on (0: corners 0-1, 1: 1-2, 2: 2-0) or -1, and its index along the edge in
		/// canonical direction at the cell's depth.
		int BaseEdge(Lat p, Lat q, int depth, int64_t& index) const
		{
			const int64_t size = int64_t(1) << depth;
			int edge = -1;
			int64_t ip = 0, iq = 0;
			if (p.b == 0 && q.b == 0) {
				edge = 0;
				ip = p.a;
				iq = q.a;
			} else if (p.a + p.b == size && q.a + q.b == size) {
				edge = 1;
				ip = p.b;
				iq = q.b;
			} else if (p.a == 0 && q.a == 0) {
				edge = 2;
				ip = size - p.b;
				iq = size - q.b;
			}
			if (edge < 0)
				return -1;
			if (edgeReversed[edge]) {
				ip = size - ip;
				iq = size - iq;
			}
			index = std::min(ip, iq);
			return edge;
		}

		void EdgeSegment(int edge, int depth, int64_t index, Vec3& a, Vec3& b) const
		{
			const double s = std::ldexp(1.0, -depth);
			const Vec3 d = edgeHi[edge] - edgeLo[edge];
			a = edgeLo[edge] + d * (static_cast<double>(index) * s);
			b = edgeLo[edge] + d * (static_cast<double>(index + 1) * s);
		}

		/**
		 * The 1-D tree of a base edge: a function of the edge's two (quantized) ends alone, so every triangle and mesh on
		 * it agrees. Its k1 = k / (2 (1 + k)) makes it split wherever a cell on the edge does (for triangles whose longest
		 * side is at most twice their shortest), and at most two levels further.
		 */
		bool Split1D(int edge, int depth, int64_t index) const
		{
			if (depth >= MaxEdgeDepth)
				return false;
			Vec3 a, b;
			EdgeSegment(edge, depth, index, a, b);
			return Length(b - a) > std::max(k1 * std::max(SegmentDistance(camera, a, b), 1.0), splitFloor1);
		}

		/// A cell splits while its longest side is more than k times its distance: with k < 1 neighbours differ by a level at most.
		bool Split(const Cell& c) const
		{
			if (c.depth >= MaxDepth)
				return false;
			const Vec3 p0 = Position(c.p[0], c.depth), p1 = Position(c.p[1], c.depth), p2 = Position(c.p[2], c.depth);
			const double longest = std::max({ Length(p1 - p0), Length(p2 - p1), Length(p0 - p2) });
			if (longest <= std::max(k * std::max(TriangleDistance(camera, p0, p1, p2), 1.0), splitFloor))
				return false;
			// Never finer than the edge's own tree along the base edges: the edge's segments are what both sides share.
			for (int s = 0; s < 3; ++s) {
				int64_t index = 0;
				const int edge = BaseEdge(c.p[s], c.p[(s + 1) % 3], c.depth, index);
				if (edge >= 0 && !Split1D(edge, c.depth, index))
					return false;
			}
			return true;
		}

		/// The lattice cell one level up that holds cell c.
		static Cell Parent(const Cell& c)
		{
			const Lat sum = c.p[0] + c.p[1] + c.p[2];
			// The centroid at the parent's depth is sum / 6.
			const int64_t a0 = sum.a / 6, b0 = sum.b / 6;
			const int64_t fa = sum.a - 6 * a0, fb = sum.b - 6 * b0;
			Cell parent;
			parent.depth = c.depth - 1;
			if (fa + fb < 6) {
				parent.p[0] = { a0, b0 };
				parent.p[1] = { a0 + 1, b0 };
				parent.p[2] = { a0, b0 + 1 };
			} else {
				parent.p[0] = { a0 + 1, b0 };
				parent.p[1] = { a0, b0 + 1 };
				parent.p[2] = { a0 + 1, b0 + 1 };
			}
			return parent;
		}

		/// How many points a segment needs: its length on screen over the target size there.
		float Factor(Vec3 a, Vec3 b) const
		{
			if (Less(b, a))
				std::swap(a, b);
			const Vec3 mid = (a + b) * 0.5;
			const Vec3 rel = mid - camera;
			const double distance = Length(rel);
			const float d = static_cast<float>(distance);
			float fade = 1.0f - Smoothstep(params.fadeStart, std::max(params.fadeEnd, params.fadeStart + 1.0f), d);
			if (params.grid.z > params.grid.x) {
				const float inside = std::min({ static_cast<float>(mid.x) - params.grid.x, static_cast<float>(mid.y) - params.grid.y,
					params.grid.z - static_cast<float>(mid.x), params.grid.w - static_cast<float>(mid.y) });
				fade *= Smoothstep(256.0f, 2048.0f, inside);
			}
			if (fade <= 0.0f)
				return 1.0f;

			const float x = static_cast<float>(mid.x), y = static_cast<float>(mid.y);
			// The detail the waves need here, as weights that fade in and out over a distance instead of switching: each was a
			// step in the triangles' size (twice the points at the ripple grid's and the shore band's edge, and the curl detail's
			// factor on top in the surf zone), and the leaves straddling a step took the finer side across their whole grid and
			// fanned slivers along their coarser sides.
			float detail = 0.0f;  // 0..1 the ripple grid or the shore band: the setting's size instead of twice it
			if (params.ripples) {
				const float u = (x - params.rippleX) * params.rippleInvExtent, v = (y - params.rippleY) * params.rippleInvExtent;
				detail = Smoothstep(0.0f, 0.1f, std::min({ u, v, 1.0f - u, 1.0f - v }));
			}
			float curlWeight = 0.0f;  // 0..1 the breakers' lips need the curl detail here
			const float curlDetail = std::max(params.curlDetail, 1.0f);
			const float viewDist = std::max(d, 1.0f);
			if (const auto* field = params.shore) {
				const float u = (x - params.shoreMinX) * params.shoreInvX, v = (y - params.shoreMinY) * params.shoreInvY;
				if (u > 0.0f && v > 0.0f && u < 1.0f && v < 1.0f && field->Valid()) {
					const float fx = std::clamp((x - field->minX) / field->step, 0.0f, static_cast<float>(field->width - 1));
					const float fy = std::clamp((y - field->minY) / field->step, 0.0f, static_cast<float>(field->height - 1));
					const uint32_t x0 = std::min(static_cast<uint32_t>(fx), field->width - 2);
					const uint32_t y0 = std::min(static_cast<uint32_t>(fy), field->height - 2);
					const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
					const size_t i = static_cast<size_t>(y0) * field->width + x0;
					auto bilinear = [&](const std::vector<float>& f) {
						const float top = f[i] + (f[i + 1] - f[i]) * tx;
						const float bottom = f[i + field->width] + (f[i + field->width + 1] - f[i + field->width]) * tx;
						return top + (bottom - top) * ty;
					};
					// The shore band: within two deep-water wavelengths of the waterline, fading out over the next half.
					const float shoreWeight = Smoothstep(2.25f * params.deepLength, 1.75f * params.deepLength, bilinear(field->distance));
					detail = std::max(detail, shoreWeight);
					// A breaker curls only on the shore train (PBRWater::ShoreWaves runs where ctx.train > 0): within its reach of
					// the waterline, where it has room. Past that the curl's detail bought nothing but quads.
					const bool train = bilinear(field->travelTime) * field->omega / static_cast<float>(PBRWaterModel::TwoPi) < params.trainCycles &&
					                   (field->room.size() != field->level.size() || bilinear(field->room) > 0.0f);
					if (train && params.curl > 0.0f && shoreWeight > 0.0f) {
						// In the surf zone, where the waves can curl at all (PBRWaterModel::ShoreCurlDepth), and as far as the mesh
						// can hold a lip, measured by the deep-water wavelength (it covered the whole sea in a storm). Mirrored by
						// PBRWater::ShoreCurlSpacing, which the lip's own detail follows.
						const float crestDepth = field->CrestDepthAt(x, y, std::numeric_limits<float>::max());
						const float lipSpacing = std::max(targetSize, 1.0f) * viewDist / std::max(params.pixelsPerUnit, 1e-3f) / curlDetail;
						curlWeight = shoreWeight * Smoothstep(params.curlDepth, CurlDepthFade * params.curlDepth, crestDepth) *
						             Smoothstep(CurlLipFar * params.deepLength, CurlLipNear * params.deepLength, lipSpacing);
					}
				}
			}
			const float length = static_cast<float>(Length(b - a));
			const float edgePixels = length * params.pixelsPerUnit / viewDist;
			float target = std::max(targetSize, 1.0f) * (2.0f - detail) / (1.0f + (curlDetail - 1.0f) * curlWeight);
			// Seen at a grazing angle the water's triangles cover the angle's sine of the screen they would face on, a wave's
			// face (GrazingFloor) a little more. Points by the square root of that share kept their area: 3000 units out with
			// the camera 400 up they were 21 pixels wide and 3 high, rows of slivers that made the middle distance a solid
			// mesh and cost the stencil pass quads for a few pixels each. Points by the share itself keep the triangles about
			// the target's height on the wave faces, wider across the view instead (GrazingShareMin times at most): the far
			// waves' shapes on screen are the faces' height, and the shading's detail is the pixel shader's own. From the
			// camera's height over the water alone, so turning the camera changes nothing and both sides of an edge still
			// agree (the waves' vertex filter and the curl's spacing match it: TessellationCS, PBRWater::ShoreCurlSpacing).
			const float height = std::abs(static_cast<float>(camera.z - mid.z));
			const float grazing = std::min(height / viewDist + GrazingFloor, 1.0f);
			float factor = edgePixels / target * std::max(grazing, GrazingShareMin);
			// No finer than the finest spacing (Maximum Tessellation).
			if (params.minSpacing > 0.0f)
				factor = std::min(factor, length / params.minSpacing);
			return std::clamp(factor, 1.0f, MaxSegmentFactor);
		}

		/// The segments of a side on a base edge: the edge tree's leaves below the side's own segment.
		void CollectEdge(int edge, int depth, int64_t index, int sideDepth, std::vector<SideSegment>& out) const
		{
			if (depth < sideDepth + 2 && Split1D(edge, depth, index)) {
				CollectEdge(edge, depth + 1, 2 * index, sideDepth, out);
				CollectEdge(edge, depth + 1, 2 * index + 1, sideDepth, out);
				return;
			}
			Vec3 a, b;
			EdgeSegment(edge, depth, index, a, b);
			out.push_back({ static_cast<uint32_t>(2 - (depth - sideDepth)), Factor(a, b) });
		}

		/// The segments of side s of a leaf, from its corner s to corner s + 1.
		void SideSegments(const Cell& c, int s, std::vector<SideSegment>& out) const
		{
			out.clear();
			const Lat p = c.p[s], q = c.p[(s + 1) % 3];
			int64_t index = 0;
			const int edge = BaseEdge(p, q, c.depth, index);
			if (edge >= 0) {
				CollectEdge(edge, c.depth, index, c.depth, out);
				// The edge tree runs in canonical order: flip it when the side runs the other way.
				int64_t ip = 0, iq = 0;
				const int64_t size = int64_t(1) << c.depth;
				if (edge == 0) {
					ip = p.a;
					iq = q.a;
				} else if (edge == 1) {
					ip = p.b;
					iq = q.b;
				} else {
					ip = size - p.b;
					iq = size - q.b;
				}
				if (edgeReversed[edge])
					std::swap(ip, iq);
				if (ip > iq)
					std::reverse(out.begin(), out.end());
				return;
			}
			// Inside the base triangle: the cell across the side, and whether it is split (it is then the finer side, and
			// owns the points: our side is its two halves).
			Cell neighbour;
			neighbour.depth = c.depth;
			neighbour.p[0] = q;
			neighbour.p[1] = p;
			neighbour.p[2] = p + q - c.p[(s + 2) % 3];
			const bool finer = c.depth > 0 && Split(Parent(neighbour)) && Split(neighbour);
			if (finer) {
				const Lat mid = p + q;
				out.push_back({ 1u, Factor(Position(Twice(p), c.depth + 1), Position(mid, c.depth + 1)) });
				out.push_back({ 1u, Factor(Position(mid, c.depth + 1), Position(Twice(q), c.depth + 1)) });
			} else {
				out.push_back({ 2u, Factor(Position(p, c.depth), Position(q, c.depth)) });
			}
		}
	};

	/// Conservative frustum test of camera-relative points grown by `marginXY` sideways and `marginZ` up and down.
	bool OutsideFrustum(const float (&m)[4][4], const Vec3* points, size_t count, double marginXY, double marginZ)
	{
		Vec3 lo = points[0], hi = points[0];
		for (size_t i = 1; i < count; ++i) {
			lo = { std::min(lo.x, points[i].x), std::min(lo.y, points[i].y), std::min(lo.z, points[i].z) };
			hi = { std::max(hi.x, points[i].x), std::max(hi.y, points[i].y), std::max(hi.z, points[i].z) };
		}
		lo = lo - Vec3{ marginXY, marginXY, marginZ };
		hi = hi + Vec3{ marginXY, marginXY, marginZ };
		uint32_t outside = 0x1F;
		for (uint32_t i = 0; i < 8; ++i) {
			const double x = (i & 1) ? hi.x : lo.x, y = (i & 2) ? hi.y : lo.y, z = (i & 4) ? hi.z : lo.z;
			double clip[4];
			for (int r = 0; r < 4; ++r)
				clip[r] = m[r][0] * x + m[r][1] * y + m[r][2] * z + m[r][3];
			uint32_t mask = 0;
			mask |= clip[0] < -clip[3] ? 0x01u : 0u;
			mask |= clip[0] > clip[3] ? 0x02u : 0u;
			mask |= clip[1] < -clip[3] ? 0x04u : 0u;
			mask |= clip[1] > clip[3] ? 0x08u : 0u;
			mask |= clip[3] <= 0.0 ? 0x10u : 0u;
			outside &= mask;
		}
		return outside != 0;
	}

	/**
	 * A mesh that is one flat rectangle cut into a grid (the cells' water: 32 x 32 quads, 2048 triangles) becomes its
	 * rectangle's two triangles, so the quadtree alone sets its detail. Cut along the grid, every one of its triangles was
	 * a base triangle the CPU visited and a leaf it encoded every frame, near or far: almost all of a frame's work. Its UVs
	 * and colours must vary affinely over it (the rectangle's corners then carry them), and the triangles must tile the
	 * rectangle exactly. Neighbouring cells share whole rectangle sides, whose edge trees then agree as before.
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
		Util::SetResourceName(copy.get(), "PBRWater::TessellationMeshReadback");
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

	std::unique_ptr<Buffer> CreateStructured(UINT stride, UINT count, bool dynamic, const char* name)
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = stride * count;
		desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (dynamic ? 0u : D3D11_BIND_UNORDERED_ACCESS);
		desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0u;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = stride;
		auto buffer = std::make_unique<Buffer>(desc, nullptr, name);
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srv.Buffer.NumElements = count;
		buffer->CreateSRV(srv);
		if (!dynamic) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_UNKNOWN;
			uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uav.Buffer.NumElements = count;
			buffer->CreateUAV(uav);
		}
		return buffer;
	}

	struct TessellationCB
	{
		uint32_t counts[4];
		float spacing[4];  // x target size (px, coarsened to the budget), y unused, z the finest spacing (units)
	};

	template <class T>
	bool Upload(Buffer& buffer, const std::vector<T>& data)
	{
		auto context = globals::d3d::context;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(buffer.resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return false;
		std::memcpy(mapped.pData, data.data(), data.size() * sizeof(T));
		context->Unmap(buffer.resource.get(), 0);
		return true;
	}
}

void WaterTessellator::SetupResources()
{
	constants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<TessellationCB>(), "PBRWater::TessellationCB");
	EnsureCapacity(1024, 256, 65536, 131072);

	auto device = globals::d3d::device;
	D3D11_BUFFER_DESC reachDesc{};
	reachDesc.ByteWidth = 16;
	reachDesc.Usage = D3D11_USAGE_DEFAULT;
	reachDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	reachDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	if (SUCCEEDED(device->CreateBuffer(&reachDesc, nullptr, reachBuffer.put()))) {
		Util::SetResourceName(reachBuffer.get(), "PBRWater::TessellationReach");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.NumElements = 4;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (SUCCEEDED(device->CreateUnorderedAccessView(reachBuffer.get(), &uav, reachUAV.put())))
			Util::SetResourceName(reachUAV.get(), "PBRWater::TessellationReach UAV");
	}
	D3D11_BUFFER_DESC stagingDesc{};
	stagingDesc.ByteWidth = 16;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (auto& staging : reachStaging) {
		if (SUCCEEDED(device->CreateBuffer(&stagingDesc, nullptr, staging.put())))
			Util::SetResourceName(staging.get(), "PBRWater::TessellationReachReadback");
	}
	CompileShaders();
}

void WaterTessellator::CompileShaders()
{
	auto compile = [](const char* entry) {
		winrt::com_ptr<ID3D11ComputeShader> cs;
		cs.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\TessellationCS.hlsl", {}, "cs_5_0", entry)));
		return cs;
	};
	vertexCS = compile("VertexCS");
	morphCS = compile("MorphCS");
	indexCS = compile("IndexCS");
	ready = vertexCS && morphCS && indexCS && constants && leafBuffer && baseBuffer && vertexBuffer && rawBuffer && indexBuffer;
	if (!ready)
		logger::warn("[PBR Water] Tessellation shaders failed to compile; the water is drawn untessellated");
	Clear();
}

void WaterTessellator::EnsureCapacity(uint32_t leafCount, uint32_t baseCount, uint32_t vertexCount, uint32_t triangleCount)
{
	auto grow = [](uint32_t need, uint32_t have) { return need <= have ? have : std::max(need + need / 2, have * 2); };
	if (leafCount > leafCapacity || !leafBuffer) {
		leafCapacity = grow(leafCount, leafCapacity);
		leafBuffer = CreateStructured(sizeof(GpuLeaf), leafCapacity, true, "PBRWater::TessellationLeaves");
	}
	if (baseCount > baseCapacity || !baseBuffer) {
		baseCapacity = grow(baseCount, baseCapacity);
		baseBuffer = CreateStructured(sizeof(GpuBaseTriangle), baseCapacity, true, "PBRWater::TessellationBaseTriangles");
	}
	if (vertexCount > vertexCapacity || !vertexBuffer) {
		vertexCapacity = std::min(grow(vertexCount, vertexCapacity), MaxVertices);
		vertexBuffer = CreateStructured(sizeof(GpuVertex), vertexCapacity, false, "PBRWater::TessellationVertices");
		rawBuffer = CreateStructured(sizeof(GpuRawDisplacement), vertexCapacity, false, "PBRWater::TessellationRawDisplacement");
	}
	if (triangleCount > triangleCapacity || !indexBuffer) {
		triangleCapacity = std::min(grow(triangleCount, triangleCapacity), MaxTriangles);
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = triangleCapacity * 12;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_INDEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		indexBuffer = std::make_unique<Buffer>(desc, nullptr, "PBRWater::TessellationIndices");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.NumElements = triangleCapacity * 3;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		indexBuffer->CreateUAV(uav);
	}
}

void WaterTessellator::ReadReach()
{
	auto context = globals::d3d::context;
	auto& staging = reachStaging[reachSlot];
	if (!reachPending[reachSlot] || !staging)
		return;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (context->Map(staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK)
		return;
	float values[2];
	std::memcpy(values, mapped.pData, sizeof(values));
	context->Unmap(staging.get(), 0);
	reachPending[reachSlot] = false;
	if (!std::isfinite(values[0]) || !std::isfinite(values[1]))
		return;
	// The frame's largest at once, letting go slowly: the camera turns towards water it has not measured.
	reachXY = std::max(values[0], reachXY * 0.98f);
	reachZ = std::max(values[1], reachZ * 0.98f);
}

void WaterTessellator::Clear()
{
	ranges.clear();
	built = false;
	stats = {};
}

void WaterTessellator::Reset()
{
	Clear();
	registered.clear();
}

bool WaterTessellator::SameSource(const Source& a, const Source& b)
{
	return a.rendererData == b.rendererData && a.triangleCount == b.triangleCount && a.vertexDesc == b.vertexDesc &&
	       std::memcmp(&a.world, &b.world, sizeof(RE::NiTransform)) == 0;
}

bool WaterTessellator::MakeSource(RE::BSGeometry& geometry, Source& s)
{
	auto* triShape = geometry.AsTriShape();
	auto& data = geometry.GetGeometryRuntimeData();
	if (!triShape || !data.rendererData)
		return false;
	s.world = geometry.world;
	s.rendererData = data.rendererData;
	s.triangleCount = triShape->GetTrishapeRuntimeData().triangleCount;
	s.vertexDesc = std::bit_cast<uint64_t>(data.vertexDesc);
	return s.triangleCount > 0 && s.triangleCount <= MaxMeshTriangles;
}

void WaterTessellator::Register(RE::BSGeometry& geometry)
{
	Source s;
	if (MakeSource(geometry, s))
		registered[&geometry] = s;
}

bool WaterTessellator::Lookup(RE::BSGeometry& geometry, uint32_t& startIndex, uint32_t& indexCount) const
{
	if (!built)
		return false;
	const auto it = ranges.find(&geometry);
	Source s;
	if (it == ranges.end() || !MakeSource(geometry, s) || !SameSource(s, it->second.source))
		return false;
	startIndex = it->second.startIndex;
	indexCount = it->second.indexCount;
	return true;
}

void WaterTessellator::TakeBaseTriangles(std::vector<WorldMesh>& out)
{
	out.clear();
	++frame;
	Clear();
	auto sources = std::move(registered);
	registered.clear();
	if (!ready)
		return;
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

bool WaterTessellator::StillSource(RE::BSGeometry& geometry, const Source& source)
{
	Source s;
	return MakeSource(geometry, s) && SameSource(s, source);
}

bool WaterTessellator::AffineTexCoords(RE::BSGeometry& geometry, float4& mapU, float4& mapV) const
{
	const auto range = ranges.find(&geometry);
	if (range == ranges.end())
		return false;
	const auto mesh = meshes.find(range->second.source.rendererData);
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

bool WaterTessellator::BuiltFor(uint32_t frameCount, const float4& cameraPosAdjust) const
{
	return built && builtFrame == frameCount && builtCamera.x == cameraPosAdjust.x && builtCamera.y == cameraPosAdjust.y && builtCamera.z == cameraPosAdjust.z;
}

const WaterTessellator::Mesh* WaterTessellator::GetMesh(const Source& source)
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

void WaterTessellator::Build(const FrameParams& params)
{
	const auto cpuStart = std::chrono::steady_clock::now();
	++frame;
	Clear();
	leaves.clear();
	baseTriangles.clear();
	auto sources = std::move(registered);
	registered.clear();
	if (!ready)
		return;

	Subdivider sub{ params };
	sub.camera = { params.cameraX, params.cameraY, params.cameraZ };

	// The frustum culling's margin: how far the waves can carry a culled leaf's vertices into view. The analytic bound
	// (the caller's) is a storm's worst crest everywhere, some 100 m in a gale, which kept everything near the camera,
	// behind it too; once measured, what this frame's waves actually reach, with room to spare.
	ReadReach();
	float marginXY = params.cullMarginXY, marginZ = params.cullMarginZ;
	if (reachXY >= 0.0f) {
		marginXY = std::min(marginXY, 1.25f * reachXY + 48.0f);
		marginZ = std::min(marginZ, 1.25f * reachZ + 48.0f);
	}

	uint64_t totalVertices = 0;
	uint64_t totalTriangles = 0;
	bool overflow = false;
	std::vector<SideSegment> segments[3];
	std::vector<Cell> stack;

	// Water wholly under the land: the terrain stands above the highest the surface can reach (the waves, or the swash
	// running up the beach and the mesh held on the bed under it: DisplaceSurface) everywhere the waves can carry the
	// cell's points. Coastal cells' water planes run under the whole cell, the land included: built, evaluated and drawn,
	// it only lost the depth test. The terrain's block minima bound its triangles (Bathymetry::LowestOver); cells too large
	// for a cheap answer are left to their children.
	const float ceilingAbove = std::max(marginZ, params.runup + 0.5f * static_cast<float>(PBRWaterModel::UnitsPerMetre)) + 64.0f;
	auto underTerrain = [&](const Cell& cell) {
		if (!params.terrain)
			return false;
		const Vec3 p0 = sub.Position(cell.p[0], cell.depth), p1 = sub.Position(cell.p[1], cell.depth), p2 = sub.Position(cell.p[2], cell.depth);
		const double x0 = std::min({ p0.x, p1.x, p2.x }) - marginXY, x1 = std::max({ p0.x, p1.x, p2.x }) + marginXY;
		const double y0 = std::min({ p0.y, p1.y, p2.y }) - marginXY, y1 = std::max({ p0.y, p1.y, p2.y }) + marginXY;
		const double top = std::max({ p0.z, p1.z, p2.z }) + ceilingAbove;
		const float lowest = params.terrain->LowestOver(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1), 256);
		return lowest > top;
	};

	// One subdivision at `scale` times the target triangle size; overflow when it needs more than `vertexLimit` vertices.
	auto subdivide = [&](float scale, uint64_t vertexLimit) {
		sub.targetSize = std::max(params.targetSize, 1.0f) * scale;
		sub.k = std::clamp(static_cast<double>(LeafSegments * sub.targetSize / std::max(params.pixelsPerUnit, 1e-3f)), 0.01, 0.5);
		sub.k1 = sub.k / (2.0 * (1.0 + sub.k));
		sub.splitFloor = LeafSegments * std::max(params.minSpacing, 0.0f);
		sub.splitFloor1 = sub.splitFloor * sub.k1 / sub.k;
		totalVertices = 0;
		totalTriangles = 0;
		overflow = false;
		leaves.clear();
		baseTriangles.clear();
		ranges.clear();
		stats.meshes = 0;
		stats.underTerrain = 0;

		for (const auto& [geometry, source] : sources) {
			const Mesh* mesh = GetMesh(source);
			if (!mesh)
				continue;
			++stats.meshes;
			Range range;
			range.source = source;
			range.startIndex = static_cast<uint32_t>(totalTriangles * 3);

			const auto& w = source.world;
			auto toWorld = [&](const float* p) {
				const double s = w.scale;
				const double x = p[0] * s, y = p[1] * s, z = p[2] * s;
				return Vec3{ w.translate.x + w.rotate.entry[0][0] * x + w.rotate.entry[0][1] * y + w.rotate.entry[0][2] * z,
					w.translate.y + w.rotate.entry[1][0] * x + w.rotate.entry[1][1] * y + w.rotate.entry[1][2] * z,
					w.translate.z + w.rotate.entry[2][0] * x + w.rotate.entry[2][1] * y + w.rotate.entry[2][2] * z };
			};

			const uint32_t baseCount = static_cast<uint32_t>(mesh->corners.size() / 3);
			for (uint32_t t = 0; t < baseCount && !overflow; ++t) {
				const Corner* c = &mesh->corners[static_cast<size_t>(t) * 3];
				Vec3 quantized[3];
				Vec3 relative[3];
				for (int i = 0; i < 3; ++i) {
					sub.corner[i] = toWorld(c[i].position);
					quantized[i] = Quantize(sub.corner[i]);
					relative[i] = sub.corner[i] - sub.camera;
				}
				if (OutsideFrustum(params.viewProj, relative, 3, marginXY, marginZ))
					continue;
				for (int e = 0; e < 3; ++e) {
					const Vec3 a = quantized[e], b = quantized[(e + 1) % 3];
					sub.edgeReversed[e] = Less(b, a);
					sub.edgeLo[e] = sub.edgeReversed[e] ? b : a;
					sub.edgeHi[e] = sub.edgeReversed[e] ? a : b;
				}

				GpuBaseTriangle base{};
				for (int i = 0; i < 3; ++i) {
					base.world[i] = { static_cast<float>(relative[i].x), static_cast<float>(relative[i].y), static_cast<float>(relative[i].z), 0.0f };
					base.model[i] = { c[i].position[0], c[i].position[1], c[i].position[2], 0.0f };
					base.color[i] = { c[i].color[0], c[i].color[1], c[i].color[2], c[i].color[3] };
				}
				base.uv01 = { c[0].uv[0], c[0].uv[1], c[1].uv[0], c[1].uv[1] };
				base.uv2[0] = c[2].uv[0];
				base.uv2[1] = c[2].uv[1];
				// The corners in canonical order, so a point two triangles share sums the same terms in the same order.
				int order[3] = { 0, 1, 2 };
				std::sort(order, order + 3, [&](int x, int y) { return Less(quantized[x], quantized[y]) || (!Less(quantized[y], quantized[x]) && x < y); });
				base.order = static_cast<uint32_t>(order[0] | (order[1] << 2) | (order[2] << 4));
				const uint32_t baseIndex = static_cast<uint32_t>(baseTriangles.size());
				const size_t leavesBefore = leaves.size();

				stack.clear();
				Cell root;
				root.p[0] = { 0, 0 };
				root.p[1] = { 1, 0 };
				root.p[2] = { 0, 1 };
				stack.push_back(root);
				while (!stack.empty() && !overflow) {
					const Cell cell = stack.back();
					stack.pop_back();
					Vec3 rel[3];
					for (int i = 0; i < 3; ++i)
						rel[i] = sub.Position(cell.p[i], cell.depth) - sub.camera;
					if (cell.depth > 0 && OutsideFrustum(params.viewProj, rel, 3, marginXY, marginZ))
						continue;
					if (underTerrain(cell)) {
						++stats.underTerrain;
						continue;
					}
					if (sub.Split(cell)) {
						Cell child;
						child.depth = cell.depth + 1;
						const Lat m01 = cell.p[0] + cell.p[1], m12 = cell.p[1] + cell.p[2], m20 = cell.p[2] + cell.p[0];
						const Lat c0 = Twice(cell.p[0]), c1 = Twice(cell.p[1]), c2 = Twice(cell.p[2]);
						child.p[0] = c0, child.p[1] = m01, child.p[2] = m20;
						stack.push_back(child);
						child.p[0] = m01, child.p[1] = c1, child.p[2] = m12;
						stack.push_back(child);
						child.p[0] = m20, child.p[1] = m12, child.p[2] = c2;
						stack.push_back(child);
						// The middle cell, turned half a turn: the same winding.
						child.p[0] = m12, child.p[1] = m20, child.p[2] = m01;
						stack.push_back(child);
						continue;
					}

					// A leaf: its sides' segments, and the grid that carries them all.
					GpuLeaf leaf{};
					float leafFactor = 1.0f;
					uint32_t leafLog2N = 0;
					for (int s = 0; s < 3; ++s) {
						sub.SideSegments(cell, s, segments[s]);
						uint32_t code = static_cast<uint32_t>(segments[s].size());
						for (size_t i = 0; i < segments[s].size() && i < 4; ++i) {
							const auto& seg = segments[s][i];
							const uint32_t log2Points = PointsLog2(seg.factor);
							code |= ((seg.log2Quarters & 3u) | (log2Points << 2)) << (3 + 6 * i);
							leaf.sideLog2[s][i] = std::log2(seg.factor);
							// Across the whole side (the leaf's grid) a segment of q quarters needs 4 / q times its points.
							const uint32_t quarters = 1u << seg.log2Quarters;
							leafFactor = std::max(leafFactor, seg.factor * 4.0f / static_cast<float>(quarters));
							leafLog2N = std::max(leafLog2N, log2Points + 2 - seg.log2Quarters);
						}
						leaf.sides[s] = code;
					}
					const uint32_t n = 1u << leafLog2N;
					leaf.vertexOffset = static_cast<uint32_t>(totalVertices);
					leaf.triangleOffset = static_cast<uint32_t>(totalTriangles);
					leaf.baseIndex = baseIndex;
					leaf.shape = leafLog2N | (static_cast<uint32_t>(cell.depth) << 8);
					for (int i = 0; i < 3; ++i) {
						leaf.corners[2 * i] = static_cast<int32_t>(cell.p[i].a);
						leaf.corners[2 * i + 1] = static_cast<int32_t>(cell.p[i].b);
					}
					leaf.interiorLog2 = std::log2(leafFactor);
					totalVertices += VertexCount(n);
					totalTriangles += static_cast<uint64_t>(n) * n;
					if (totalVertices > vertexLimit || totalTriangles > MaxTriangles) {
						overflow = true;
						break;
					}
					leaves.push_back(leaf);
				}
				if (leaves.size() > leavesBefore)
					baseTriangles.push_back(base);
			}
			range.indexCount = static_cast<uint32_t>(totalTriangles * 3) - range.startIndex;
			ranges[geometry] = range;
			if (overflow)
				break;
		}
	};

	// What a subdivision that ran past its limit had made by then (the log when the budget coarsens it).
	auto describe = [&](float atScale) {
		uint32_t byN[9] = {};
		uint32_t byDepth[16] = {};
		for (const auto& l : leaves) {
			++byN[std::min<uint32_t>(l.shape & 0xFF, 8)];
			++byDepth[std::min<uint32_t>((l.shape >> 8) & 0xFF, 15)];
		}
		std::string n, depth;
		for (const uint32_t h : byN)
			n += std::to_string(h) + " ";
		for (const uint32_t h : byDepth)
			depth += std::to_string(h) + " ";
		return std::format("target {:.1f} px (x{:g}), k {:.4f}, px/unit {:.1f}, camera ({:.0f}, {:.0f}, {:.0f}); before stopping: {} meshes, {} base triangles, {} leaves; leaves by log2 N: {}; by depth: {}",
			sub.targetSize, atScale, sub.k, params.pixelsPerUnit, params.cameraX, params.cameraY, params.cameraZ, stats.meshes, baseTriangles.size(), leaves.size(), n, depth);
	};
	static auto lastLog = std::chrono::steady_clock::time_point{};
	const bool logNow = cpuStart - lastLog > std::chrono::seconds(10);

	// Constant on-screen triangles cost the same count per doubling of distance, and the sea is seen at grazing angles,
	// so the count follows the view more than the setting. Past the budget the target size grows (the count falls with
	// its square) until it fits, starting from last frame's, and relaxes back as the view allows.
	const uint32_t budget = std::clamp(static_cast<uint32_t>(std::max(params.renderPixels, 0.0f) / PixelsPerVertex), MinVertexBudget, MaxVertices);
	float scale = std::max(detailScale, 1.0f);
	subdivide(scale, budget);
	for (int attempt = 0; overflow && attempt < 6; ++attempt) {
		if (logNow && attempt == 0) {
			lastLog = cpuStart;
			logger::info("[PBR Water] Compute tessellation over its budget of {} vertices, coarsening: {}", budget, describe(scale));
		}
		scale *= 2.0f;
		subdivide(scale, budget);
	}
	// Room to spare: the next frame tries finer (the count goes with 1 / scale^2, and 1 / 0.88^2 ~ 1.3).
	detailScale = !overflow && scale > 1.0f && totalVertices * 1.3 < budget * 0.7 ? std::max(scale * 0.88f, 1.0f) : scale;

	std::erase_if(meshes, [&](const auto& entry) { return frame - entry.second.lastUsed > MeshLifetime; });

	if (overflow) {
		// Even 64 times coarser does not fit: something is wrong with the inputs. Draw untessellated this frame.
		if (logNow) {
			lastLog = cpuStart;
			logger::warn("[PBR Water] Tessellation does not fit {} vertices; drawing the water untessellated: {}", budget, describe(scale));
		}
		detailScale = 1.0f;
		Clear();
		return;
	}
	stats.detailScale = scale;

	stats.baseTriangles = static_cast<uint32_t>(baseTriangles.size());
	stats.leaves = static_cast<uint32_t>(leaves.size());
	stats.vertices = static_cast<uint32_t>(totalVertices);
	stats.triangles = static_cast<uint32_t>(totalTriangles);
	builtFrame = params.frame;
	builtCamera = { static_cast<float>(params.cameraX), static_cast<float>(params.cameraY), static_cast<float>(params.cameraZ), 0.0f };

	if (!leaves.empty()) {
		EnsureCapacity(static_cast<uint32_t>(leaves.size()), static_cast<uint32_t>(baseTriangles.size()), stats.vertices, stats.triangles);
		if (stats.vertices > vertexCapacity || stats.triangles > triangleCapacity || !Upload(*leafBuffer, leaves) || !Upload(*baseBuffer, baseTriangles)) {
			Clear();
			return;
		}

		PBRWaterProfiling::Scope pass("PBRWater::Tessellate");
		auto context = globals::d3d::context;
		TessellationCB cb{ { stats.leaves, stats.vertices, stats.triangles, 0 }, { std::max(params.targetSize, 1.0f) * scale, 0.0f, std::max(params.minSpacing, 0.0f), 0.0f } };
		constants->Update(cb);
		ID3D11Buffer* cbs[1] = { constants->CB() };
		context->CSSetConstantBuffers(0, 1, cbs);

		ID3D11ShaderResourceView* srvs[3] = { leafBuffer->srv.get(), baseBuffer->srv.get(), nullptr };
		ID3D11UnorderedAccessView* uavs[4] = { vertexBuffer->uav.get(), rawBuffer->uav.get(), nullptr, reachUAV.get() };
		if (reachUAV) {
			const UINT zero[4] = {};
			context->ClearUnorderedAccessViewUint(reachUAV.get(), zero);
		}
		context->CSSetShaderResources(0, 3, srvs);
		context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
		context->CSSetShader(vertexCS.get(), nullptr, 0);
		context->Dispatch((stats.vertices + 63) / 64, 1, 1);
		uavs[3] = nullptr;
		context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
		if (reachBuffer && reachStaging[reachSlot]) {
			context->CopyResource(reachStaging[reachSlot].get(), reachBuffer.get());
			reachPending[reachSlot] = true;
			reachSlot = (reachSlot + 1) % ReachLatency;
		}

		// The morph reads every vertex's own displacement from the raw copy.
		uavs[1] = nullptr;
		context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
		srvs[2] = rawBuffer->srv.get();
		context->CSSetShaderResources(0, 3, srvs);
		context->CSSetShader(morphCS.get(), nullptr, 0);
		context->Dispatch((stats.vertices + 63) / 64, 1, 1);

		uavs[0] = nullptr;
		uavs[2] = indexBuffer->uav.get();
		context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
		context->CSSetShader(indexCS.get(), nullptr, 0);
		context->Dispatch((stats.triangles + 63) / 64, 1, 1);

		ID3D11ShaderResourceView* nullSrvs[3] = {};
		ID3D11UnorderedAccessView* nullUavs[3] = {};
		ID3D11Buffer* nullCb = nullptr;
		context->CSSetShaderResources(0, 3, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		context->CSSetConstantBuffers(0, 1, &nullCb);
		context->CSSetShader(nullptr, nullptr, 0);
	} else {
		PBRWaterProfiling::Skipped("PBRWater::Tessellate");
	}

	built = true;
	stats.cpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
}

void WaterTessellator::Draw(uint32_t startIndex, uint32_t indexCount) const
{
	if (!built || indexCount == 0 || !indexBuffer || !vertexBuffer)
		return;
	auto context = globals::d3d::context;
	ID3D11Buffer* savedIndices = nullptr;
	DXGI_FORMAT savedFormat = DXGI_FORMAT_UNKNOWN;
	UINT savedOffset = 0;
	context->IAGetIndexBuffer(&savedIndices, &savedFormat, &savedOffset);

	ID3D11ShaderResourceView* srv = vertexBuffer->srv.get();
	context->IASetIndexBuffer(indexBuffer->resource.get(), DXGI_FORMAT_R32_UINT, 0);
	context->VSSetShaderResources(109, 1, &srv);
	context->DrawIndexed(indexCount, startIndex, 0);

	ID3D11ShaderResourceView* nullSrv = nullptr;
	context->VSSetShaderResources(109, 1, &nullSrv);
	context->IASetIndexBuffer(savedIndices, savedFormat, savedOffset);
	if (savedIndices)
		savedIndices->Release();
}
