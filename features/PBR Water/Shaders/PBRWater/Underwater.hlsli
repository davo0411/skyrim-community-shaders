#ifndef __PBR_WATER_UNDERWATER_HLSLI__
#define __PBR_WATER_UNDERWATER_HLSLI__

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "PBRWater/Optics.hlsli"

// ============================================================================
// PBR Water - underwater view.
//
// Ported from the Crest Ocean System underwater effect (wave-harmonic/crest, MIT License,
// Copyright (c) 2019 Wave Harmonic and contributors: UnderwaterEffect.hlsl,
// UnderwaterEffectShared.hlsl, UnderwaterMeniscus.shader, OceanEmission.hlsl). As in Crest, per pixel:
//   1. decide whether the view ray starts under the water (Crest renders an ocean mask; here the
//      analytic wave surface is tested at the pixel's near-plane point, so the waterline across a
//      half-submerged lens follows the waves);
//   2. merge the scene depth with the distance to the surface seen from below;
//   3. fog the scene towards the water's lit scatter colour with Beer-Lambert extinction;
//   4. darken a thin meniscus along the waterline.
//
// Extended with what the rest of PBR Water knows about the water (Optics.hlsli):
//   - extinction and body colour from the water form, plus the spatially varying sediment, integrated
//     with a short ray march so murky patches, the stirred-up bottom layer and wading silt are volumes;
//   - sunlight and sky light attenuated with depth (K_d): deep water darkens, and submerged objects
//     lose light on its way down from the surface;
//   - Henyey-Greenstein forward scattering (the glow around the sun) and light shafts: sun rays
//     entering through the focusing surface, extruded along the refracted sun direction.
//
// Runs inside ISSAOComposite, which is where Skyrim fogs the opaque scene, and replaces the vanilla
// underwater fog there. The water surface seen from below is shaded afterwards by Water.hlsl with the
// same functions.
// ============================================================================

namespace PBRWater
{
	bool UnderwaterActive() { return Underwater0.x > 0.5; }

	/// Flat water height at the camera, camera-relative.
	float UnderwaterPlaneZ() { return Underwater0.y - FrameBuffer::CameraPosAdjust.z; }

	struct UnderwaterLighting
	{
		float3 waterSun;  // body colour, sunlit share (linear)
		float3 waterSky;  // body colour, sky-lit share
		float3 sedimentSun;
		float3 sedimentSky;
		float3 toSun;  // refracted direction towards the sun
		float sunCosine;
		float sunShare;
	};

	float3 SceneDirectionalLight()
	{
		float llDirLightMult = (SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear) ? SharedData::linearLightingSettings.dirLightMult : 1.0f;
		return Color::DirectionalLight(SharedData::DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * llDirLightMult;
	}

	/// The water form colours are display colours of lit water (the weather scales them through the day),
	/// so they are split into sun and sky shares by the luminance of the current lights, as the surface does.
	UnderwaterLighting GetUnderwaterLighting()
	{
		UnderwaterLighting l;
		float3 toSun = SharedData::DirLightDirection.xyz;
		float sunLuma = (SharedData::InInterior || toSun.z <= 0.0) ? 0.0 : Color::RGBToLuminance(max(SceneDirectionalLight(), 0.0));
		float skyLuma = Color::RGBToLuminance(max(SharedData::GetAmbient(float3(0, 0, 1)), 0.0));
		l.sunShare = sunLuma / max(sunLuma + skyLuma, 1e-5);
		float3 body = Color::IrradianceToLinear(Color::Water(Underwater2.xyz));
		float3 sediment = SedimentBodyColour(body);
		l.waterSun = body * l.sunShare;
		l.waterSky = body * (1.0 - l.sunShare);
		l.sedimentSun = sediment * l.sunShare;
		l.sedimentSky = sediment * (1.0 - l.sunShare);
		l.toSun = RefractedSunDirection(toSun);
		l.sunCosine = l.toSun.z;
		return l;
	}

	float3 UnderwaterBaseExtinction()
	{
		return Extinction(Color::Water(Underwater1.xyz), Underwater0.w);
	}

	/**
	 * Radiance scattered towards the camera per unit extinction at a point `depth` below the surface
	 * (linear). The sunlit share is weighted by the phase function and the light shafts, both shares by
	 * how much light still reaches that depth.
	 */
	float3 UnderwaterSource(UnderwaterLighting l, WaterOptics o, float3 viewDir, float depth, float shafts)
	{
		float3 sunDown = exp(-o.downwelling * depth / max(l.sunCosine, 0.3));
		float3 skyDown = exp(-o.downwelling * depth / 0.75);
		float phase = lerp(1.0, PhaseHG(dot(viewDir, l.toSun), Optics0.x), saturate(Underwater3.y));
		// Sky light under water arrives mostly from above (Crest blends a grazing and a vertical colour).
		float skyDistribution = 0.75 + 0.25 * viewDir.z;
		float3 sun = lerp(l.waterSun, l.sedimentSun, o.sedimentShare) * (phase * shafts) * sunDown;
		float3 sky = lerp(l.waterSky, l.sedimentSky, o.sedimentShare) * skyDistribution * skyDown;
		return sun + sky;
	}

	/// Light shafts: brightness of the sun ray through camera-relative `p`, `depth` below the surface,
	/// from the caustic pattern where that ray entered the water. Mean 1, so shafts redistribute light.
	float LightShafts(UnderwaterLighting l, float3 p, float depth)
	{
		float strength = Underwater1.w * exp(-depth / max(Underwater2.w, 1.0));
		if (strength <= 1e-3 || l.sunShare <= 1e-3)
			return 1.0;
		float2 entry = p.xy + FrameBuffer::CameraPosAdjust.xy + l.toSun.xy / max(l.toSun.z, 0.3) * depth;
		float depthMetres = depth * MetresPerUnit;
		// Focus is lost with depth: the network grows coarser and softer.
		float2 surface = entry * MetresPerUnit / (1.0 + depthMetres * 0.05);
		float blur = saturate(depth / max(Underwater2.w, 1.0));
		return max(lerp(1.0, CausticPattern(surface, ClarityTime(), blur), strength), 0.0);
	}

	struct UnderwaterSegment
	{
		float3 inscatter;      // linear radiance scattered towards the camera
		float3 transmittance;  // of the light from the end of the segment
		float endTurbidity;
	};

	/**
	 * Integrates the water between the camera and `distance` along `viewDir` (camera-relative, unit).
	 * Steps are spaced quadratically (dense near the camera, where shafts and silt are resolved) and
	 * jittered per pixel and frame; the temporal AA averages the jitter out. Each segment adds its
	 * source radiance times the light it removes, which conserves energy for any step size.
	 */
	UnderwaterSegment MarchUnderwater(float3 viewDir, float distance, float planeZ, UnderwaterLighting l, float3 baseExtinction, float jitter, uint samples)
	{
		UnderwaterSegment s;
		s.inscatter = 0.0;
		s.transmittance = 1.0;
		s.endTurbidity = 0.0;

		// Beyond ~4.6 optical depths in the clearest channel (1% left) the rest is closed analytically.
		float clearest = min(baseExtinction.r, min(baseExtinction.g, baseExtinction.b));
		float range = min(min(distance, 4.6 / max(clearest, 1e-6)), 60000.0);

		// The drifting patches vary over tens of metres: evaluate them at both ends and interpolate.
		float2 cameraXY = FrameBuffer::CameraPosAdjust.xy;
		float mixedNear = MixedTurbidity(cameraXY, 0.0);
		float mixedFar = MixedTurbidity(cameraXY + viewDir.xy * range, 0.0);
		float fetchRatio = FetchHeightRatio(SampleFetchMetres(cameraXY));

		float3 lastSource = 0.0;
		float3 lastExtinction = baseExtinction;
		[loop] for (uint i = 0; i < samples; i++)
		{
			float u0 = (float)i / samples;
			float u1 = (float)(i + 1) / samples;
			float um = ((float)i + jitter) / samples;
			float t0 = range * u0 * u0;
			float t1 = range * u1 * u1;
			float3 p = viewDir * (range * um * um);
			float depth = max(planeZ - p.z, 0.0);

			// Bottom layer: wave-stirred sediment and wading silt, thickest at the bed.
			float2 absXY = p.xy + cameraXY;
			float profile = 1.0;
			float waterDepth = 1e6;
			if (Terrain1.z > 0.5 && TerrainInside(absXY)) {
				float bed = SampleTerrainZ(absXY) - FrameBuffer::CameraPosAdjust.z;
				profile = BottomLayerProfile(p.z - bed);
				waterDepth = max(planeZ - bed, 0.0);
			}
			float bottom = (ResuspensionTurbidity(waterDepth, fetchRatio, 1.0, 0.0) + Clarity2.z * RippleSilt(p)) * profile;
			float turbidity = lerp(mixedNear, mixedFar, um) + bottom;

			WaterOptics o = GetWaterOptics(baseExtinction, turbidity);
			float3 source = UnderwaterSource(l, o, viewDir, depth, LightShafts(l, p, depth));
			float3 segment = exp(-o.extinction * (t1 - t0));
			s.inscatter += s.transmittance * (1.0 - segment) * source;
			s.transmittance *= segment;
			lastSource = source;
			lastExtinction = o.extinction;
			s.endTurbidity = turbidity;
		}

		if (distance > range) {
			float3 tail = exp(-lastExtinction * min(distance - range, 1e6));
			s.inscatter += s.transmittance * (1.0 - tail) * lastSource;
			s.transmittance *= tail;
		}
		return s;
	}

	/**
	 * Radiance of the water body seen along `viewDir` from `depth` below the surface when nothing
	 * stops the ray (linear): the source integrated over an infinite path whose depth changes by
	 * -viewDir.z per unit. Used for total internal reflection at the surface seen from below.
	 */
	float3 UnderwaterBodyRadiance(UnderwaterLighting l, WaterOptics o, float3 viewDir, float depth)
	{
		float descent = max(-viewDir.z, 0.0);
		float3 sunK = o.downwelling / max(l.sunCosine, 0.3);
		float3 skyK = o.downwelling / 0.75;
		float3 sunScale = o.extinction / max(o.extinction + sunK * descent, 1e-8);
		float3 skyScale = o.extinction / max(o.extinction + skyK * descent, 1e-8);
		float phase = lerp(1.0, PhaseHG(dot(viewDir, l.toSun), Optics0.x), saturate(Underwater3.y));
		float3 sun = lerp(l.waterSun, l.sedimentSun, o.sedimentShare) * phase * exp(-sunK * depth) * sunScale;
		float3 sky = lerp(l.waterSky, l.sedimentSky, o.sedimentShare) * (0.75 + 0.25 * viewDir.z) * exp(-skyK * depth) * skyScale;
		return sun + sky;
	}

	/**
	 * Fogs one pixel of the opaque scene when the camera is under water (ISSAOComposite).
	 * @param sceneColour frame colour in the shading space, replaced by the fogged colour
	 * @param meniscus    colour multiplier for the waterline, applied by the caller to every pixel
	 * @return false when the pixel's view ray starts above the water (vanilla fog applies)
	 */
	bool ApplyUnderwater(inout float3 sceneColour, float2 uv, float rawDepth, bool isGeometry, float2 pixel, out float3 meniscus)
	{
		meniscus = 1.0;

		// Camera-relative view ray and the point where it leaves the lens (near plane).
		float2 ndc = float2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y);
		float4 sceneH = mul(FrameBuffer::CameraViewProjInverse, float4(ndc, rawDepth, 1.0));
		float4 nearH = mul(FrameBuffer::CameraViewProjInverse, float4(ndc, 0.0, 1.0));
		float3 scenePos = sceneH.xyz / sceneH.w;
		float3 nearPos = nearH.xyz / nearH.w;
		float3 viewDir = SafeNormalize(scenePos, float3(0, 1, 0));

		float planeZ = UnderwaterPlaneZ();
		float waterline = planeZ;
		[branch] if (Underwater0.z > 0.0)
		{
			// The camera is within reach of the waves: test the displaced surface at the lens.
			waterline = SurfaceHeightAt(nearPos.xy, planeZ, 2);
			// Meniscus (Crest): a thin, slightly blue-grey line where the surface crosses the lens, a
			// couple of pixels wide whatever the resolution, measured with the surface's screen gradient.
			float signedHeight = waterline - nearPos.z;
			float pixels = abs(signedHeight) / max(fwidth(signedHeight), 1e-5);
			float width = 2.5 * max(SharedData::BufferDim.y / 1080.0, 0.5);
			float alpha = sqrt(smoothstep(width, 0.0, pixels)) * saturate(Underwater3.x);
			meniscus = lerp(1.0, 1.3 * float3(0.37, 0.4, 0.5), alpha);
		}
		if (nearPos.z > waterline)
			return false;

		// Merge the scene depth with the surface seen from below (Crest's GetOceanSurfaceAndUnderwaterData).
		float sceneDistance = isGeometry ? length(scenePos) : 1e7;
		float surfaceDistance = 1e7;
		if (viewDir.z > 1e-3) {
			float3 hit = viewDir * (max(planeZ, 1.0) / viewDir.z);
			surfaceDistance = max(SurfaceHeightAt(hit.xy, planeZ, 1), 0.0) / viewDir.z;
		}
		float distance = min(sceneDistance, surfaceDistance);

		UnderwaterLighting light = GetUnderwaterLighting();
		float3 baseExtinction = UnderwaterBaseExtinction();
		float jitter = Random::InterleavedGradientNoise(pixel, SharedData::FrameCount);
		UnderwaterSegment s = MarchUnderwater(viewDir, distance, planeZ, light, baseExtinction, jitter, clamp((uint)Underwater3.w, 4u, 32u));

		float3 scene = Color::IrradianceToLinear(sceneColour);
		if (sceneDistance < surfaceDistance) {
			// Submerged objects were lit through the water above them.
			WaterOptics o = GetWaterOptics(baseExtinction, s.endTurbidity);
			scene *= DownwellingTransmittance(o, max(planeZ - scenePos.z, 0.0), light.sunCosine, light.sunShare);
		}
		sceneColour = Color::IrradianceToGamma(scene * s.transmittance + s.inscatter);
		return true;
	}
}

#endif  // __PBR_WATER_UNDERWATER_HLSLI__
