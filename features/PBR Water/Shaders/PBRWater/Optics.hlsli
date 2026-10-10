#ifndef __PBR_WATER_OPTICS_HLSLI__
#define __PBR_WATER_OPTICS_HLSLI__

#include "Common/Color.hlsli"
#include "PBRWater/Noise.hlsli"
#include "PBRWater/PBRWater.hlsli"

// ============================================================================
// PBR Water - optical properties of the water body, shared by the surface (seen from above and
// below) and the underwater view.
//
//   Clarity    : water is rarely uniformly clear. Suspended sediment ("turbidity") varies in drifting
//                patches, is stirred up from the bottom by the waves' orbital motion in the shallows
//                and surf zone, is carried by river currents and storms, and is kicked up by anything
//                wading through shallow water. It adds extinction and tints the water towards the
//                sediment colour.
//   Extinction : split into absorption and scattering. The water form's shallow colour and visibility
//                give the total extinction; scattering is spectrally flat (particles), so the colour of
//                the water comes from absorption. Sediment adds its own scattering and absorption.
//   Downwelling: sunlight reaching depth z is attenuated by the diffuse attenuation coefficient
//                K_d ~ a + b_b (Gordon 1989), not by the full extinction: particles scatter mostly
//                forwards, so murky water still lets light down while blurring the view.
//   Scattering : Henyey-Greenstein phase function (forward peaked, like ocean particles; Petzold 1972).
//   Waves      : sunlight transmitted through thin wave crests, coloured by what survives the path
//                through the crest, replaces the purely artistic subsurface term.
// ============================================================================

namespace PBRWater
{
	static const float WaterIOR = 1.333;

	/// Exact unpolarised Fresnel reflectance for a dielectric. `eta` = n_incident / n_transmitted.
	/// Returns 1 under total internal reflection.
	float FresnelDielectric(float cosI, float eta)
	{
		cosI = saturate(cosI);
		float sinT2 = eta * eta * (1.0 - cosI * cosI);
		if (sinT2 >= 1.0)
			return 1.0;
		float cosT = sqrt(1.0 - sinT2);
		float rs = (eta * cosI - cosT) / (eta * cosI + cosT);
		float rp = (cosI - eta * cosT) / (cosI + eta * cosT);
		return 0.5 * (rs * rs + rp * rp);
	}

	/// Per-channel extinction (1/unit). The vanilla shallow colour is read as the tint light picks up
	/// over the water form's visibility distance; at that distance 5% of the light remains.
	float3 Extinction(float3 shallowColorLinear, float visibilityUnits)
	{
		float3 tint = saturate(shallowColorLinear / max(max(shallowColorLinear.r, max(shallowColorLinear.g, shallowColorLinear.b)), 1e-4));
		tint = max(tint, 0.02);
		return (log(20.0) - log(tint)) / max(visibilityUnits, 1.0);
	}

	/// Henyey-Greenstein phase function, normalised so that an isotropic medium gives 1.
	float PhaseHG(float cosTheta, float g)
	{
		float g2 = g * g;
		float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
		return (1.0 - g2) / (denom * sqrt(denom));
	}

	/// Direction towards the sun as seen from under a flat surface. Snell's law bends it towards the
	/// vertical: under water the sun is never more than ~49 degrees from the zenith.
	float3 RefractedSunDirection(float3 toSun)
	{
		float3 travel = refract(-normalize(float3(toSun.xy, max(toSun.z, 0.02))), float3(0, 0, 1), 1.0 / WaterIOR);
		return -travel;
	}

	// ------------------------------------------------------------------------
	// Clarity (suspended sediment)
	// ------------------------------------------------------------------------

	/// Turbidity split by where the sediment sits in the water column.
	struct Turbidity
	{
		float mixed;   // spread through the column: fine particles, plankton, river and storm runoff
		float bottom;  // concentrated near the bed: sand and silt stirred up by waves and wading
	};

	float ClarityTime() { return Optics0.z; }

	/// Drifting, plume-shaped patches in [0, 1]: 0 clearest, 1 murkiest.
	float TurbidityPatches(float2 absXY)
	{
		float scale = max(Clarity0.w, 100.0);
		float t = ClarityTime();
		// Plumes drift downwind at a few centimetres per second and slowly change shape.
		float2 q = absXY / scale - Params1.xy * (t * 0.04 * UnitsPerMetre / scale);
		float2 warp = float2(Fbm2(q * 0.5 + float2(3.1, t * 0.003)), Fbm2(q * 0.5 + float2(-7.7, -t * 0.0027))) - 0.5;
		return smoothstep(0.3, 0.72, Fbm(q + warp * 1.4));
	}

	/**
	 * Sediment stirred up by the waves' orbital motion at the bed. Linear wave theory gives the near-bed
	 * orbital velocity u_b = a w / sinh(k h); fine sand and silt start moving at ~0.1 m/s, so the surf
	 * zone and wave-swept shallows turn murky in a swell while deep or calm water stays clear.
	 */
	float ResuspensionTurbidity(float depthUnits, float fetchRatio, float damping, float shoreBreak)
	{
		if (Clarity1.w <= 0.0)
			return 0.0;
		float h = max(depthUnits * MetresPerUnit, 0.05);
		float omega = max(Shore0.y, 0.3);
		float amplitude = 0.5 * Optics0.w * MetresPerUnit * fetchRatio * damping;
		// Eckart's explicit approximation of the dispersion relation w^2 = g k tanh(k h).
		float k0h = omega * omega * h / 9.81;
		float kh = min(k0h / sqrt(max(SafeTanh(k0h), 1e-6)), 20.0);
		float sinhKh = 0.5 * (exp(kh) - exp(-kh));
		float bedVelocity = amplitude * omega / max(sinhKh, 1e-3);
		return Clarity1.w * saturate(saturate((bedVelocity - 0.08) / 0.5) + 0.5 * shoreBreak);
	}

	/**
	 * @param positionWS camera-relative position of the water column
	 * @param depthUnits water depth there (units), used for wave resuspension
	 * @param flow       0..1 river current strength
	 */
	/// Sediment spread through the whole column at absolute position `absXY`.
	float MixedTurbidity(float2 absXY, float flow)
	{
		return Clarity0.y * lerp(1.0 - Clarity0.z, 1.0 + 2.0 * Clarity0.z, TurbidityPatches(absXY)) + Clarity2.x * flow + Clarity2.y;
	}

	Turbidity GetTurbidity(float3 positionWS, float depthUnits, float fetchRatio, float damping, float shoreBreak, float flow)
	{
		Turbidity t;
		t.mixed = MixedTurbidity(positionWS.xy + FrameBuffer::CameraPosAdjust.xy, flow);
		t.bottom = ResuspensionTurbidity(depthUnits, fetchRatio, damping, shoreBreak) + Clarity2.z * RippleSilt(positionWS);
		return t;
	}

	/// Concentration of the bottom layer at `heightAboveBed` relative to the bed (Rouse-like decay).
	float BottomLayerProfile(float heightAboveBed)
	{
		return exp(-max(heightAboveBed, 0.0) / max(Clarity2.w, 1.0));
	}

	/// Mean bottom-layer concentration over a column `depth` deep.
	float BottomLayerColumnMean(float depth)
	{
		float H = max(Clarity2.w, 1.0);
		float d = max(depth, 1e-3);
		return H / d * (1.0 - exp(-d / H));
	}

	float TurbidityAt(Turbidity t, float heightAboveBed)
	{
		return t.mixed + t.bottom * BottomLayerProfile(heightAboveBed);
	}

	float ColumnTurbidity(Turbidity t, float depth)
	{
		return t.mixed + t.bottom * BottomLayerColumnMean(depth);
	}

	// ------------------------------------------------------------------------
	// Optical properties
	// ------------------------------------------------------------------------

	struct WaterOptics
	{
		float3 extinction;     // c = a + b (1/unit)
		float3 scattering;     // b (1/unit)
		float3 downwelling;    // K_d for light travelling straight down (1/unit); divide by the direction cosine
		float3 sedimentShare;  // fraction of the extinction caused by sediment
	};

	/// Single-scattering albedo of the sediment: mostly scattering, absorbing a little more blue,
	/// which is what makes muddy light yellow-brown.
	float3 SedimentAlbedo()
	{
		float3 c = max(Clarity1.xyz, 1e-3);
		return 0.6 + 0.35 * c / max(c.r, max(c.g, c.b));
	}

	WaterOptics GetWaterOptics(float3 baseExtinction, float turbidity)
	{
		WaterOptics o;
		float sediment = max(turbidity, 0.0) * Clarity0.x;
		float3 sedimentAlbedo = SedimentAlbedo();
		o.extinction = baseExtinction + sediment;
		// Particle scattering is spectrally flat: give the clearest channel an albedo of 0.6 and let
		// absorption make up the rest of every channel's extinction.
		o.scattering = 0.6 * min(baseExtinction.r, min(baseExtinction.g, baseExtinction.b)) + sediment * sedimentAlbedo;
		// K_d ~ a + b_b with a backscatter fraction of ~10% for the forward-peaked particle phase function.
		o.downwelling = max(o.extinction - 0.9 * o.scattering, 0.0) * Optics0.y;
		o.sedimentShare = sediment / max(o.extinction, 1e-8);
		return o;
	}

	/// Lit colour of muddy water for a water body colour (linear): the sediment colour's hue at about twice
	/// the brightness of the clear body, since suspended particles backscatter more light than clear water.
	/// Anchoring it to the body keeps every water form's own brightness through the day.
	float3 SedimentBodyColour(float3 waterBodyLinear)
	{
		float3 sediment = max(Color::IrradianceToLinear(Color::Water(Clarity1.xyz)), 1e-4);
		return sediment * (2.0 * Color::RGBToLuminance(max(waterBodyLinear, 0.0)) / max(Color::RGBToLuminance(sediment), 1e-4));
	}

	/// Colour of a deep water column: the water form's body colour, shifted towards the sediment colour by
	/// the share of the extinction the sediment is responsible for.
	float3 BodyColour(float3 waterBody, float3 sedimentBody, WaterOptics o)
	{
		return lerp(waterBody, sedimentBody, o.sedimentShare);
	}

	/// Fraction of the light at the surface that reaches `depth` (direct sun at `sunCosine`, plus sky light).
	float3 DownwellingTransmittance(WaterOptics o, float depth, float sunCosine, float sunShare)
	{
		float3 sun = exp(-o.downwelling * depth / max(sunCosine, 0.3));
		float3 sky = exp(-o.downwelling * depth / 0.75);
		return lerp(sky, sun, sunShare);
	}

	// ------------------------------------------------------------------------
	// Light transmitted through the water
	// ------------------------------------------------------------------------

	/**
	 * Sunlight transmitted through thin wave crests. Light refracts into the far face of the crest,
	 * scatters once towards the viewer and refracts out of the near face. Its colour is what survives
	 * the path through the crest: clear sea water glows green-blue, murky water dimly in its sediment
	 * colour. Multiply by the sun irradiance.
	 * @param crest     0..1 height of the point within the wave amplitude
	 * @param thickness crest thickness scale (units)
	 */
	float3 WaveTranslucency(float3 N, float3 toEye, float3 toLight, float crest, WaterOptics o, float thickness)
	{
		float3 viewIn = refract(-toEye, N, 1.0 / WaterIOR);
		if (dot(viewIn, viewIn) < 1e-6)
			return 0.0;
		// The far face of the crest leans the other way.
		float3 backNormal = normalize(float3(-N.xy, max(N.z, 0.05)));
		float3 lightIn = refract(-toLight, backNormal, 1.0 / WaterIOR);
		lightIn = dot(lightIn, lightIn) > 1e-6 ? lightIn : -toLight;

		float phase = PhaseHG(dot(lightIn, -viewIn), Optics0.x);
		float transmission = (1.0 - FresnelDielectric(dot(backNormal, toLight), 1.0 / WaterIOR)) *
		                     (1.0 - FresnelDielectric(dot(N, toEye), 1.0 / WaterIOR));
		float tip = saturate(crest);
		float sunPath = thickness * (1.1 - tip);
		float3 scattered = o.scattering / max(o.extinction, 1e-8) * (1.0 - exp(-o.extinction * thickness)) * exp(-o.extinction * sunPath);
		return scattered * phase * transmission * tip;
	}
}

#endif  // __PBR_WATER_OPTICS_HLSLI__
