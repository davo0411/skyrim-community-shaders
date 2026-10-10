#ifndef __PBR_WATER_TESSELLATION_HLSLI__
#define __PBR_WATER_TESSELLATION_HLSLI__

// ============================================================================
// PBR Water tessellation. Included by Water.hlsl for the hull and domain stages only.
//
// Crack prevention: a shared edge must get the same tessellation factor and the same generated
// vertices from both triangles that use it. Every per-edge quantity is therefore computed from
// the two endpoints in a canonical (sorted) order, and the domain shader accumulates corner
// contributions in a canonical order too, so floating point rounding is identical on both sides.
// Nothing per patch may reach the displacement either: the wave filter spacing is a function of
// the vertex position alone, or two patches would move their shared edge apart and open a crack.
// ============================================================================

namespace PBRWater
{
	/// Wave filter spacing for a tessellated vertex at camera-relative `positionWS`: the on-screen
	/// target triangle size (doubled, as most edges end up between the curvature and the baseline
	/// density) projected to that distance. Depends on the position only, so it is crack-free.
	float TessellatedSpacing(float3 positionWS)
	{
		return 2.0 * max(Tess0.y, 1.0) * max(length(positionWS), 1.0) / max(Tess0.w, 1e-3);
	}

	/// Strict weak order on positions, used to canonicalise shared edges.
	bool PositionLess(float3 a, float3 b)
	{
		if (a.x != b.x)
			return a.x < b.x;
		if (a.y != b.y)
			return a.y < b.y;
		return a.z < b.z;
	}

#if defined(DSHADER)
	/// Barycentric interpolation of the raw vertex, summed in canonical corner order so that points
	/// on a shared edge (one weight exactly zero) are bit-identical in both adjacent patches.
	VS_INPUT InterpolateVertex(VS_INPUT corners[3], float3 barycentric)
	{
		uint order[3] = { 0, 1, 2 };
		[unroll] for (uint sweep = 0; sweep < 2; sweep++)
		{
			[unroll] for (uint j = 0; j < 2 - sweep; j++)
			{
				if (PositionLess(corners[order[j + 1]].Position.xyz, corners[order[j]].Position.xyz)) {
					uint t = order[j];
					order[j] = order[j + 1];
					order[j + 1] = t;
				}
			}
		}

		float weights[3] = { barycentric.x, barycentric.y, barycentric.z };
		VS_INPUT v = (VS_INPUT)0;
		precise float3 position = 0.0;
		[unroll] for (uint k = 0; k < 3; k++)
		{
			uint c = order[k];
			position += corners[c].Position.xyz * weights[c];
#	if defined(NORMAL_TEXCOORD)
			v.TexCoord0 += corners[c].TexCoord0 * weights[c];
#	endif
#	if defined(VC)
			v.Color += corners[c].Color * weights[c];
#	endif
		}
		v.Position = float4(position, 1.0);
		return v;
	}
#endif

#if defined(HSHADER)
	float3 ControlPointWorld(VS_OUTPUT cp)
	{
		return mul(World, float4(cp.HPosition.xyz, 1.0)).xyz;
	}

	/// Tessellation factor of one edge, from the endpoints in canonical order so both patches sharing
	/// it agree. Two terms:
	///  - curvature: enough segments that the chord error of the local wave curvature stays below a
	///    fraction of a pixel (sagitta = curvature * segment^2 / 8), so sharp crests are dense and
	///    troughs / calm water stay coarse;
	///  - a coarse on-screen baseline (finer inside the ripple simulation, which has no analytic
	///    curvature to measure), so nothing is ever more than a few target triangles across.
	float EdgeTessFactor(float3 a, float3 b)
	{
		float3 lo = PositionLess(a, b) ? a : b;
		float3 hi = PositionLess(a, b) ? b : a;
		float len = length(hi - lo);
		float3 mid = (lo + hi) * 0.5;
		float dist = max(length(mid), 1.0);

		float fade = DisplacementDistanceFade(dist);
		if (fade <= 0.0)
			return 1.0;

		float pixelsPerUnit = Tess0.w / dist;
		float targetPixels = max(Tess0.y, 1.0);

		WaveContext ctx = BuildWaveContext(mid, fade, 0.0);
		float2 e = (hi.xy - lo.xy) / max(len, 1e-3);
		float curvature = SurfaceCurvature(mid, ctx, e);
		float errorPixels = targetPixels * 0.05;  // 0.5 px at the default 10 px target
		float curvatureFactor = len * sqrt(curvature * pixelsPerUnit / (8.0 * errorPixels));

		float2 rippleUV = (mid.xy + FrameBuffer::CameraPosAdjust.xy - Ripple0.xy) * Ripple0.z;
		bool inRipples = Ripple0.w > 0.5 && all(rippleUV > 0.0) && all(rippleUV < 1.0);
		float baselinePixels = targetPixels * (inRipples ? 1.0 : 4.0);
		float screenFactor = len * pixelsPerUnit / baselinePixels;

		return clamp(max(curvatureFactor, screenFactor), 1.0, Tess0.z);
	}

	/// Conservative frustum test of the patch bounds grown by the largest possible displacement.
	bool PatchOutsideFrustum(float3 p0, float3 p1, float3 p2)
	{
		// Amplitude sum doubled for the fetch energy gain, shore waves doubled for shoaling.
		float margin = 2.0 * Params2.z + 2.0 * Shore0.x + Ripple1.x + 64.0;
		float3 lo = min(p0, min(p1, p2)) - margin;
		float3 hi = max(p0, max(p1, p2)) + margin;

		uint outside = 0x1F;
		[unroll] for (uint i = 0; i < 8; i++)
		{
			float3 corner = float3((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
			float4 clip = mul(FrameBuffer::CameraViewProj, float4(corner, 1.0));
			uint mask = 0;
			mask |= (clip.x < -clip.w) ? 0x01 : 0;
			mask |= (clip.x > clip.w) ? 0x02 : 0;
			mask |= (clip.y < -clip.w) ? 0x04 : 0;
			mask |= (clip.y > clip.w) ? 0x08 : 0;
			mask |= (clip.w <= 0.0) ? 0x10 : 0;
			outside &= mask;
		}
		return outside != 0;
	}

	HS_CONSTANT_OUTPUT PatchConstants(InputPatch<VS_OUTPUT, 3> patch)
	{
		HS_CONSTANT_OUTPUT o;
		float3 p0 = ControlPointWorld(patch[0]);
		float3 p1 = ControlPointWorld(patch[1]);
		float3 p2 = ControlPointWorld(patch[2]);

		if (PatchOutsideFrustum(p0, p1, p2)) {
			o.Edge[0] = o.Edge[1] = o.Edge[2] = 0.0;
			o.Inside = 0.0;
			return o;
		}

		// Edge i is opposite control point i.
		o.Edge[0] = EdgeTessFactor(p1, p2);
		o.Edge[1] = EdgeTessFactor(p2, p0);
		o.Edge[2] = EdgeTessFactor(p0, p1);
		o.Inside = max(o.Edge[0], max(o.Edge[1], o.Edge[2]));
		return o;
	}
#endif
}

#if defined(HSHADER)
[domain("tri")]
	[partitioning("fractional_odd")]
	[outputtopology("triangle_cw")]
	[outputcontrolpoints(3)]
	[patchconstantfunc("PBRWater::PatchConstants")]
	[maxtessfactor(64.0)] VS_OUTPUT main(InputPatch<VS_OUTPUT, 3> patch, uint controlPoint : SV_OutputControlPointID) {
		return patch[controlPoint];
	}
#endif

#endif  // __PBR_WATER_TESSELLATION_HLSLI__
