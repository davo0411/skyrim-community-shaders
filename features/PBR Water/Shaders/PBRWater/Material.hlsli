#ifndef __PBR_WATER_MATERIAL_HLSLI__
#define __PBR_WATER_MATERIAL_HLSLI__

#include "PBRWater/PBRWater.hlsli"
#include "PBRWater/Shading.hlsli"

// ============================================================================
// PBR Water - the deferred surface's material (SurfaceMaterialCS.hlsl writes it, Water.hlsl's lighting reads it).
//
// The surface program's waves, shore train and foam coverage cost the pixel shader most of its registers, which capped the
// surface pass's occupancy. SurfaceMaterialCS evaluates them once per recorded water pixel (PBRWater::EvaluateSurface) and
// keeps what the lighting reads of them: the waves' own normal, the ripples, the foam coverages and the few depths and
// heights the column and the waterline are worked out from. The lighting pixel shader (Water.hlsl, PBRW_MATERIAL) no
// longer holds any of that code: it adds the vanilla detail normal maps (PBRWater::ApplyDetail) and lights the surface.
//
// 24 bytes a pixel in two textures, every field its own bits (a 64-bit word is two of the words below):
//
//   A.x  normal.x 16 | normal.y 16            the waves' own normal, octahedral (unorm 16 each)
//   A.y  variance 8 | detail 8 | fetch 8 | break 8
//          variance  slope variance of the waves and the wind's ripples, 0 or log2 over 2^-24..2^2 (254 steps)
//          detail    strength of the vanilla detail normal maps (0..2), unorm
//          fetch     FetchHeightRatio of the fetch here, unorm
//          break     how far the shore waves here have got with breaking, unorm
//   A.z  height 16 | elevation 16            water surface and bed above the still water (units), half floats
//   A.w  swash 16 | share 8 | swashFoam 8    the surf simulation's surface (half float, below -60000 where it is not the
//                                            water), its share of the surface and its foam, unorm
//   B.x  foam 8 | flat 8 | rough 8 | crest 8
//          foam      0..1 the open sea's and the breakers' foam coverage, unorm
//          flat      0..1 the shore foam on flat water (bore, what the surf left), unorm
//          rough     0..1 the foam that roughens the surface (distance faded), unorm
//          crest     units above the still water the shore wave's crest stands, 0 or log2 over 0.002..16 m
//   B.y  ripple.x 12 | ripple.y 12 | depth 8    the ripples' slope (signed, +-1), the water's depth, 0 or log2 over 0.01..1000 m
//
// What is approximated against the inline evaluation (Water.hlsl without PBRW_MATERIAL): the fields above are quantised as
// described; the pixel's footprint and the cascades' screen derivatives come from the rays through the neighbouring pixels
// met with the plane of the undisplaced point (as DeferredShoreCS does) instead of from the derivatives of its quad; and the
// foam coverages are summed to the three terms the waterline and the foam pattern are worked out from.
// ============================================================================

namespace PBRWaterMaterial
{
	static const float HalfLimit = 60000.0;
	static const float VarianceMin = -24.0;  // log2
	static const float VarianceMax = 2.0;
	static const float CrestMinMetres = 0.002;
	static const float CrestMaxMetres = 16.0;
	static const float DepthMinMetres = 0.01;
	static const float DepthMaxMetres = 1000.0;

	uint PackUnorm8(float v) { return (uint)round(saturate(v) * 255.0); }
	float UnpackUnorm8(uint v) { return (v & 0xFF) / 255.0; }

	/// 0 for zero, else 1..255 along log2 from log2(lo) to log2(hi).
	uint LogCode(float v, float lo, float hi)
	{
		if (v <= lo)
			return 0u;
		return 1u + (uint)round(saturate((log2(v) - log2(lo)) / (log2(hi) - log2(lo))) * 254.0);
	}

	float LogValue(uint code, float lo, float hi)
	{
		return (code & 0xFF) == 0 ? 0.0 : exp2(log2(lo) + ((code & 0xFF) - 1) / 254.0 * (log2(hi) - log2(lo)));
	}

	uint Half(float v) { return f32tof16(clamp(v, -HalfLimit, HalfLimit)); }

	/// Octahedral encoding of a unit vector in [-1, 1]^2.
	float2 OctEncode(float3 n)
	{
		n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
		float2 e = n.xy;
		if (n.z < 0.0)
			e = (1.0 - abs(e.yx)) * float2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
		return e;
	}

	float3 OctDecode(float2 e)
	{
		float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
		float t = saturate(-n.z);
		n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
		return PBRWater::SafeNormalize(n, float3(0.0, 0.0, 1.0));
	}

	uint PackSigned12(float v) { return (uint)((int)round(clamp(v, -1.0, 1.0) * 2047.0) & 0xFFF); }
	float UnpackSigned12(uint v)
	{
		int x = (int)(v & 0xFFF);
		return (x >= 2048 ? x - 4096 : x) / 2047.0;
	}

	/// The material of a surface EvaluateSurface evaluated, in the two textures' words.
	void Encode(PBRWater::SurfaceShading s, out uint4 a, out uint2 b)
	{
		float2 oct = OctEncode(s.waveNormal) * 0.5 + 0.5;
		uint2 octBits = (uint2)round(saturate(oct) * 65535.0);
		float flatness = smoothstep(0.80, 0.95, s.waveNormal.z);
		// The shore foam gated by how flat the surface is: the bore's, and what the surf left where the simulation is not the water.
		float flatFoam = s.foam * s.bore * flatness;  // the surf's leftover foam is in breakerFoam (EvaluateWavesFiltered)
		float swash = s.swashDepth > PBRWater::SwashMinDepth ? s.swashSurface : -2.0 * HalfLimit;
		float elevation = max(s.elevation, -2.0 * HalfLimit);

		a.x = octBits.x | (octBits.y << 16);
		a.y = LogCode(s.baseVariance, exp2(VarianceMin), exp2(VarianceMax)) | (PackUnorm8(s.detailScale * 0.5) << 8) | (PackUnorm8(s.fetchRatio) << 16) | (PackUnorm8(s.breakStage) << 24);
		a.z = Half(s.height) | (Half(elevation) << 16);
		a.w = Half(swash) | (PackUnorm8(s.simShare) << 16) | (PackUnorm8(s.swashFoam) << 24);
		b.x = PackUnorm8(max(saturate(s.fftFoam), s.breakerFoam)) | (PackUnorm8(flatFoam) << 8) | (PackUnorm8(s.foamCover) << 16) |
		      (LogCode(s.crest * PBRWater::MetresPerUnit, CrestMinMetres, CrestMaxMetres) << 24);
		b.y = PackSigned12(s.ripple.x) | (PackSigned12(s.ripple.y) << 12) | (LogCode(s.depth * PBRWater::MetresPerUnit, DepthMinMetres, DepthMaxMetres) << 24);
	}

	/// A pixel's material as stored. The lighting program keeps these six words across its length and decodes each field
	/// where it is used: decoded early, the fields would sit in registers through the whole lighting.
	struct Words
	{
		uint4 a;
		uint2 b;
	};

	/// The waves' normal, the ripples and what roughens the surface: all PBRWater::ApplyDetail reads.
	PBRWater::SurfaceShading DecodeWaves(Words w)
	{
		PBRWater::SurfaceShading s = (PBRWater::SurfaceShading)0;
		s.waveNormal = OctDecode(float2(w.a.x & 0xFFFF, w.a.x >> 16) / 65535.0 * 2.0 - 1.0);
		s.baseVariance = LogValue(w.a.y, exp2(VarianceMin), exp2(VarianceMax));
		s.detailScale = UnpackUnorm8(w.a.y >> 8) * 2.0;
		s.foamCover = UnpackUnorm8(w.b.x >> 16);
		s.ripple = float2(UnpackSigned12(w.b.y), UnpackSigned12(w.b.y >> 12));
		return s;
	}

	/// The shore wave's crest and the water's height at the pixel.
	void DecodeCrest(Words w, inout PBRWater::SurfaceShading s)
	{
		s.crest = LogValue(w.b.x >> 24, CrestMinMetres, CrestMaxMetres) * PBRWater::UnitsPerMetre;
		s.height = f16tof32(w.a.z & 0xFFFF);
	}

	/// What the water column's clarity is worked out from.
	void DecodeColumn(Words w, inout PBRWater::SurfaceShading s)
	{
		s.depth = LogValue(w.b.y >> 24, DepthMinMetres, DepthMaxMetres) * PBRWater::UnitsPerMetre;
		s.fetchRatio = UnpackUnorm8(w.a.y >> 16);
		s.breakStage = UnpackUnorm8(w.a.y >> 24);
	}

	/**
	 * The bed and the foam coverages. The shore foam and the sea's foam are left where Water.hlsl reads them: fftFoam is the
	 * sea's and the breakers' coverage (breakerFoam 0), foam the shore's on flat water (already gated by flatness: bore 1, no
	 * surfFoam).
	 */
	void DecodeFoam(Words w, inout PBRWater::SurfaceShading s)
	{
		s.elevation = f16tof32(w.a.z >> 16);
		s.elevation = s.elevation < -HalfLimit + 1.0 ? -1e6 : s.elevation;
		float swash = f16tof32(w.a.w & 0xFFFF);
		// A surface that is not the water has no depth: Water.hlsl's tip gap is then the (very negative) surface itself.
		s.swashSurface = swash;
		s.swashDepth = swash > -HalfLimit + 1.0 ? 1e6 : 0.0;
		s.simShare = UnpackUnorm8(w.a.w >> 16);
		s.swashFoam = UnpackUnorm8(w.a.w >> 24);
		s.fftFoam = UnpackUnorm8(w.b.x);
		s.breakerFoam = 0.0;
		s.foam = UnpackUnorm8(w.b.x >> 8);
		s.bore = 1.0;
		s.surfFoam = 0.0;
	}
}

#endif  // __PBR_WATER_MATERIAL_HLSLI__
