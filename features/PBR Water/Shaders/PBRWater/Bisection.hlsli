#ifndef __PBR_WATER_BISECTION_HLSLI__
#define __PBR_WATER_BISECTION_HLSLI__

// ============================================================================
// PBR Water - the bisection tessellator's data and rules (BisectionCS.hlsl, BisectionTessellator).
// Design: docs/development/pbr-water-bisection-tessellator.md.
//
// Longest-edge bisection of the water meshes' base triangles (roots), conforming: a triangle splits only together with
// the triangle across its refinement edge (a diamond), so no edge ever carries a hanging vertex. Each triangle of the
// subdivision (a bisector) is drawn as a uniform grid of BisectionGridN segments a side; neighbours share whole edges,
// so their grids meet point for point.
//
// Corners are counter-clockwise, edge k runs from corner k to corner k + 1, and edge 2 (corner 2 to corner 0) is the
// refinement edge, opposite the apex, corner 1. Splitting (c0, c1, c2) at m, the midpoint of c2-c0:
//   child A (the parent's slot): (c1, m, c0)   n = (B, twin's child B, resolve(parent.n[0]))
//   child B (a new slot):        (c2, m, c1)   n = (twin's child A, A, resolve(parent.n[1]))
// The twin of a diamond is (d0, d1, d2) with d0 = c2 and d2 = c0: its child A covers m-c2, its child B c0-m.
// ============================================================================

#include "PBRWater/PBRWater.hlsli"

#define PBRW_BISECTION_GRID_N 4
#define PBRW_BISECTION_NONE 0xFFFFFFFFu

namespace PBRWaterBisection
{
	static const uint GridN = PBRW_BISECTION_GRID_N;
	static const uint GridVertices = (GridN + 1) * (GridN + 2) / 2;
	static const uint GridTriangles = GridN * GridN;
	static const uint None = PBRW_BISECTION_NONE;

	static const uint FlagSplit = 1u;   // splits this iteration (with its twin)
	static const uint FlagForced = 2u;  // a finer neighbour waits on it: split even if the spacing does not ask
	static const uint FlagCulled = 4u;  // outside the view: drawn by nobody, split only when forced

	/// A water mesh's base triangle: the bisection's root. 192 bytes. Mirrors BisectionTessellator::GpuRoot.
	struct Root
	{
		float4 world[3];  // xyz camera-relative corner positions, refinement edge on corner 2 to corner 0
		float4 model[3];  // xyz model-space corner positions
		float4 color[3];
		float4 uv01;  // xy corner 0's texture coordinate, zw corner 1's
		float2 uv2;
		uint mesh;         // the mesh slot (BisectionTessellator::Lookup), which draw lists it goes in
		uint order;        // the corners in canonical order (by absolute position), 2 bits each, as BaseTriangle::order
		uint4 neighbours;  // xyz the root across edges 0, 1 and 2 (None: the mesh's border), w unused
	};

	/// A triangle of the subdivision. 48 bytes.
	struct Bisector
	{
		float2 b0;  // the corners as weights of root corners 1 and 2 (dyadic: exact in float to about 23 levels)
		float2 b1;
		float2 b2;
		uint root;
		uint depth;
		uint3 n;  // the bisector across edges 0, 1 and 2 (None: the border)
		uint flags;
	};

	float3 Weights(float2 b) { return float3(1.0 - b.x - b.y, b.x, b.y); }

	/// Camera-relative world position of a point given by its root weights.
	float3 RootPoint(Root r, float2 b)
	{
		float3 w = Weights(b);
		return r.world[0].xyz * w.x + r.world[1].xyz * w.y + r.world[2].xyz * w.z;
	}

	/// The two children of `p` (A in the parent's slot, B in `slotB`), their corners only. Neighbours: LinkCS.
	void Children(Bisector p, out Bisector a, out Bisector b)
	{
		float2 m = 0.5 * (p.b2 + p.b0);
		a = p;
		b = p;
		a.b0 = p.b1;
		a.b1 = m;
		a.b2 = p.b0;
		b.b0 = p.b2;
		b.b1 = m;
		b.b2 = p.b1;
		a.depth = b.depth = p.depth + 1;
		a.flags = b.flags = 0;
	}

	// ------------------------------------------------------------------------
	// The spacing the mesh should have: the one rule the split test, the waves' vertex filter and the curl share.
	// ------------------------------------------------------------------------

	/**
	 * World spacing (units) the water mesh should have at camera-relative `positionWS`: the target size (Tess0.y, as the
	 * budget coarsened it: Draw0.w) on screen at this distance, twice it on open water and once in the shore band and the
	 * ripple grid, divided by the curl detail where breakers curl (ShoreCurlSpacing's weights), wider at grazing angles
	 * (TessellationGrazingScale), never finer than 128 / Maximum Tessellation (Tess0.z).
	 */
	float TessellationSpacing(float3 positionWS)
	{
		float distance = max(length(positionWS), 1.0);
		float size = max(PBRWater::Tess0.y, 1.0) * max(PBRWater::Draw0.w, 1.0) * distance / max(PBRWater::Tess0.w, 1e-3);
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;

		float detail = 0.0;
		float2 uv;
		if (PBRWater::RippleUV(absXY, PBRWater::Ripple0.xy, uv))
			detail = smoothstep(0.0, 0.1, min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y)));
		float curl = 0.0;
		[branch] if (PBRWater::ShoreFieldUV(absXY, uv))
		{
			PBRWater::ShoreRay ray = PBRWater::SampleShore(absXY);
			float shore = smoothstep(2.25 * PBRWater::Shore3.w, 1.75 * PBRWater::Shore3.w, ray.distance) * ray.weight;
			detail = max(detail, shore);
			[branch] if (PBRWater::Shore3.z > 0.0 && shore > 0.0)
			{
				float curlDepth = PBRWater::ShoreCurlDepth();
				float crestDepth = PBRWater::ShoreCrestDepth(absXY, 1e6);
				float curlDetail = max(PBRWater::Shore1.y, 1.0);
				curl = shore * smoothstep(curlDepth, PBRWater::TessellationCurlDepthFade * curlDepth, crestDepth) *
				       smoothstep(PBRWater::TessellationCurlLipFar * PBRWater::Shore3.w, PBRWater::TessellationCurlLipNear * PBRWater::Shore3.w, size / curlDetail);
			}
		}
		float curlDetail = max(PBRWater::Shore1.y, 1.0);
		float spacing = size * (2.0 - detail) / (1.0 + (curlDetail - 1.0) * curl) * PBRWater::TessellationGrazingScale(positionWS);
		return max(spacing, 128.0 / max(PBRWater::Tess0.z, 1.0));
	}
}

#endif  // __PBR_WATER_BISECTION_HLSLI__
