#ifndef __WATER_WAVE_SCATTERING_HLSLI__
#define __WATER_WAVE_SCATTERING_HLSLI__

// ============================================================================
// PHYSICALLY-BASED WATER VOLUME SCATTERING + WAVE EDGE HIGHLIGHT (SSS)
// ============================================================================
// Volume scattering uses ShallowColor-tinted coefficients (PerMaterial).
// Wave edge term uses UnifiedWaterPerFrame (GerstnerWaves.hlsli) + Color::Water.
// Reference (absorption): https://s.campbellsci.com/documents/es/technical-papers/obs_light_absorption.pdf

// ============================================================================
// SSS PARAMETERS — declare these in your PerMaterial cbuffer and define
// SSS_PARAMS_DECLARED before including this file, e.g.:
//
//   float SSSDistortion;   // 0.2  — normal deflection of back-light
//   float SSSPower;        // 8.0  — back-scatter lobe sharpness
//   float SSSScale;        // 1.5  — back-scatter brightness
//   float SSSRimPower;     // 4.0  — rim glow sharpness
//   float SSSRimScale;     // 0.5  — rim glow brightness
//   #define SSS_PARAMS_DECLARED
//
// If SSS_PARAMS_DECLARED is not defined the constants below are used as
// compile-time defaults so the shader still compiles without cbuffer changes.
// ============================================================================
#ifndef SSS_PARAMS_DECLARED
static const float SSSDistortion = 0.2f;
static const float SSSPower      = 8.0f;
static const float SSSScale      = 1.5f;
static const float SSSRimPower   = 4.0f;
static const float SSSRimScale   = 0.5f;
#endif

// Henyey-Greenstein phase function for anisotropic scattering
float PhaseHenyeyGreenstein(float cosTheta, float g)
{
	const float scale = 0.25f / Math::PI;
	float g2 = g * g;
	float num = 1.0f - g2;
	float denom = pow(abs(1.0f + g2 - 2.0f * g * cosTheta), 1.5f);
	return scale * num / max(denom, 0.0001f);
}

struct WaterScatteringResult
{
	float3 scatter;
	float3 transmittance;
};

WaterScatteringResult CalculateWaterScattering(float3 startPosWS, float3 endPosWS, float3 sunDir, float3 sunColor, float occlusion, float cameraDistance, float depthBlend)
{
	WaterScatteringResult result;
	result.scatter = 0.0f;
	result.transmittance = 1.0f;

	float3 worldDir = endPosWS - startPosWS;
	float dist = length(worldDir);
	if (dist < 0.01f) {
		return result;
	}

	worldDir = worldDir / dist;

	// Optical coefficients follow the game's shallow water tint (ESP ShallowColor).
	// Do NOT use normalize(rcp(shallow)) — that inverts channels and picks a complementary hue
	// (strong green/purple fringes) instead of the authored WaterColor.
	float3 waterTint = Color::Water(max(ShallowColor.xyz, 0.001f));
	float lum = max(dot(waterTint, float3(0.2126f, 0.7152f, 0.0722f)), 0.04f);
	const float baseScatterSigma = 0.002f * 2.0f * 1.428f;
	const float baseAbsorpSigma = 0.0002f * 2.0f * 1.428f;
	float3 scatterCoeff = waterTint * (baseScatterSigma / lum);
	float3 absorpCoeff = waterTint * (baseAbsorpSigma / lum);
	float3 extinction = scatterCoeff + absorpCoeff;

	float cosTheta = dot(sunDir, worldDir);
	float phase = PhaseHenyeyGreenstein(cosTheta, 0.5f);

	float distRatio = abs(sunDir.z / max(abs(worldDir.z), 0.001f));

	const float cutoffTransmittance = 0.01f;
	float maxExtinction = max(extinction.x, max(extinction.y, extinction.z));
	float cutoffDist = -log(cutoffTransmittance) / ((1.0f + distRatio) * maxExtinction);

	float marchDist = min(dist, cutoffDist);
	float sunMarchDist = marchDist * distRatio;

	// Distance-based LOD: reduce steps at distance for performance
	// Smooth transition zones ensure no popping
	uint nSteps = 8;
	float lodFade = 1.0f;

	const float lod1Distance = 4096.0f;   // Medium distance threshold
	const float lod2Distance = 8192.0f;   // Far distance threshold

	if (cameraDistance > lod2Distance) {
		nSteps = 4;  // Minimal steps at far distance
		float fadeStart = lod2Distance;
		float fadeEnd = lod2Distance + 2048.0f;
		lodFade = 1.0f - saturate((cameraDistance - fadeStart) / (fadeEnd - fadeStart));
	} else if (cameraDistance > lod1Distance) {
		// BUG FIX 1: nSteps was set to 6 then immediately overwritten by the lerp below.
		// The lerp result is a float in [6,8] — cast to uint gives either 6 or 8, never 7,
		// and nSteps=6 above was a dead write. Just use the lerp assignment directly.
		float fadeStart = lod1Distance;
		float fadeEnd = lod1Distance + 2048.0f;
		float blend = saturate((cameraDistance - fadeStart) / (fadeEnd - fadeStart));
		nSteps = (uint)lerp(8, 6, blend);
		lodFade = 1.0f - blend * 0.3f;  // Slight intensity reduction
	}

	// BUG FIX 2: step must scale with marchDist so each segment covers the correct
	// physical distance.  The original (1.0/nSteps) was a dimensionless fraction;
	// multiplying by marchDist inside the loop gave the right per-step distance, but
	// sampleTransmittance used (step * marchDist) which double-counted marchDist when
	// step was already marchDist/nSteps.  Unify: store the actual per-step world
	// distance here and remove the extra marchDist factor inside the loop.
	float stepDist = marchDist / (float)nSteps;

	float3 scatter = 0.0f;
	float3 transmittance = 1.0f;

	[unroll]
	for (uint i = 0; i < nSteps; ++i) {
		float t = (i + 0.5f) / (float)nSteps;  // BUG FIX 3: t must be a fraction [0,1]
		                                         // of the total march, independent of
		                                         // the step size.  Original used
		                                         // (i+0.5)*step where step=1/nSteps, so
		                                         // t was correct numerically — kept as-is
		                                         // but rewritten for clarity now that step
		                                         // is a world-space distance.

		// BUG FIX 2 (cont.): use stepDist directly — no marchDist multiplier needed.
		float3 sampleTransmittance = exp(-stepDist * extinction);
		transmittance *= sampleTransmittance;

		float3 sunTransmittance = exp(-sunMarchDist * t * extinction);
		float3 inScatter = scatterCoeff * phase * sunTransmittance;
		scatter += inScatter * (1.0f - sampleTransmittance) / max(extinction, 0.0001f) * transmittance;
	}

	// Apply LOD fade to maintain scattering presence at distance without popping
	float3 scatterLit = scatter * sunColor * occlusion * 3.0f * lodFade;
	// Re-inject ESP water colour (same lerp as refraction diffuse) so scattering matches vanilla hue
	float3 gameWater = lerp(Color::Water(ShallowColor.xyz), Color::Water(DeepColor.xyz), saturate(depthBlend));
	result.scatter = scatterLit * lerp(1.0.xxx, gameWater, 0.66f);
	result.transmittance = exp(-dist * (1.0f + distRatio) * extinction);

	return result;
}

// ============================================================================
// WAVE EDGE SSS — Barré-Brisebois / Zucconi fast subsurface scattering
// ============================================================================
// Based on: "Approximating Translucency for a Fast, Cheap and Convincing
//            Subsurface Scattering Look" (GDC 2011, Colin Barré-Brisebois &
//            Marc Bouchard), as explained by Alan Zucconi:
//   https://www.alanzucconi.com/2017/08/30/fast-subsurface-scattering-1/
//
// Core idea: add a back-lit translucency term computed from a virtual light
// at -sunDir (light shining through the wave from behind).  The surface
// normal is blended in via the distortion parameter to control how strongly
// the normal deflects that back-light — tighter distortion = sharper crest
// highlight; looser = broader glow.
//
// Three additive terms keep colour impact minimal:
//   1. BACK-SCATTER  — The canonical Barré-Brisebois term.  View-dependent,
//                      sharpened by SSSPower, scaled by SSSScale.
//   2. THICKNESS PROXY — Wave height drives a cheap "how much water is
//                      between the sun and this point" estimate.  Thin crests
//                      let more light through; deep troughs attenuate it.
//   3. RIM          — Grazing-angle edge glow (N·V near 0) so crests stay
//                      bright when viewed nearly flat.
//
// Colour is a shallow/deep lerp tinted to the sun — identical to the old
// code — so the hue stays authored.  The terms add *on top* of the existing
// surface colour rather than replacing it, which is why there is no saturate
// at the outer level; let the caller or tonemapper handle HDR.

float3 CalculateWaveEdgeSSS(
	float3 viewDirection,       // camera → surface (our convention)
	float3 waveNormal,
	float3 sunDir,
	float3 sunColor,
	float waveHeight,
	float horizontalDisplacement,
	float3 shallowColor,
	float3 deepColor,
	float depthBlend)
{
	// ------------------------------------------------------------------ //
	// Safe-normalise — zero normals at destructive-interference crests    //
	// (Gerstner horizontal displacement) must not produce NaN.            //
	// ------------------------------------------------------------------ //
	float waveNormalLen = length(waveNormal);
	float3 N = (waveNormalLen > 0.0001f) ? (waveNormal / waveNormalLen) : float3(0.0f, 0.0f, 1.0f);

	// viewDirection is camera→surface; the lighting equations want surface→camera (V).
	float3 V = -viewDirection;
	float3 L = sunDir;

	// ------------------------------------------------------------------ //
	// Sun-side gate — SSS only makes physical sense when the sun is on   //
	// the far side of the wave from the viewer (light shining through).  //
	// dot(-L, V) > 0 means viewer and sun are on opposite sides.         //
	// Without this gate the back-scatter term fires on sun-facing waves  //
	// and produces dark artefacts where waveTint * nearZeroScatter        //
	// undercuts the base surface colour.                                  //
	// ------------------------------------------------------------------ //
	float sunBehindWave = saturate(dot(-L, V));

	// ------------------------------------------------------------------ //
	// TERM 1 — Barré-Brisebois back-scatter                               //
	// H_back = normalize(-L + N * SSSDistortion)                          //
	// I_back = saturate(dot(V, -H_back))^SSSPower * SSSScale              //
	//                                                                      //
	// SSSDistortion (δ) blends the virtual back-light between purely      //
	// opposite-sun (0) and normal-deflected (1).  ~0.2 suits water.       //
	// ------------------------------------------------------------------ //
	float3 H_back = normalize(-L + N * SSSDistortion);
	float VdotH_back = saturate(dot(V, -H_back));
	float backScatter = pow(VdotH_back, SSSPower) * SSSScale * sunBehindWave;

	// ------------------------------------------------------------------ //
	// TERM 2 — Thickness proxy via wave height                            //
	// Thin crests (high normHeight) transmit more; deep troughs are       //
	// opaque. Floor is 0 so flat calm water contributes nothing.          //
	// ------------------------------------------------------------------ //
	float maxAmp = max(Wave1Amplitude * WaveAmplitude * M_TO_GAME_UNIT, 1.0f);
	float normHeight = saturate(max(0.0f, waveHeight) / maxAmp);
	float thicknessMask = normHeight;   // 0 on flat water, 1 at full crest
	backScatter *= thicknessMask;

	// ------------------------------------------------------------------ //
	// TERM 3 — Rim / edge glow                                            //
	// Must only fire on normals facing *away* from the camera (true       //
	// silhouette edges), not on camera-facing normals. The original       //
	// (1-NdotV)^p peaked at NdotV=0, i.e. exactly the normals facing     //
	// sideways toward the viewer, producing black patches on prominent    //
	// wave faces.                                                          //
	//                                                                      //
	// Fix: multiply by (1-NdotV) to push the peak toward back-facing     //
	// normals, then gate the whole term with sunBehindWave so it only     //
	// appears when the geometry is backlit.                                //
	// ------------------------------------------------------------------ //
	float NdotV = saturate(dot(N, V));
	float rimAngle = pow(1.0f - NdotV, SSSRimPower) * (1.0f - NdotV);
	float rim = rimAngle * SSSRimScale * thicknessMask * sunBehindWave;

	// ------------------------------------------------------------------ //
	// Combine — purely additive over the base surface colour.             //
	// No outer saturate so HDR bloom can pick up bright crest highlights. //
	// Tint matches refraction, with shallow bias (dbWave) vs full depthBlend.
	// ------------------------------------------------------------------ //
	// Slight bias toward shallow (surface) water vs deep — reads closer to classic water colour
	float dbWave = saturate(depthBlend * 0.9f);
	float3 waveTint = lerp(Color::Water(shallowColor), Color::Water(deepColor), dbWave) * sunColor;
	float3 waveColor = waveTint * (backScatter + rim);
	return waveColor;
}

// ============================================================================
// DIRECTIONAL SUNLIGHT IN WAVELETS — forward / sun-facing subsurface diffusion
// ============================================================================
// Approximates light scattering through a thin, sun-lit water sheet when the viewer
// looks toward the sun: V·L is high, wave crests (N·L) are lit, grazing views (1-N·V)
// pick up transmission. Uses the same directional light as the rest of the water
// (SunDir toward sun); tint is shallow-biased and scaled by ScatteringCoeff.
// Tuning is fixed in-shader (no extra UI).

static const float kDirSunSSS_FacingPow = 1.65f;
static const float kDirSunSSS_Wrap = 0.5f;
static const float kDirSunSSS_GrazingPow = 1.85f;
static const float kDirSunSSS_Intensity = 0.48f;

float3 CalculateDirectionalSunlightWaveSSS(
	float3 viewDirection,
	float3 waveNormalWS,
	float3 sunDirWS,
	float3 sunColor,
	float waveHeight,
	float3 shallowColor,
	float3 deepColor,
	float depthBlend)
{
	float sunLenSq = dot(sunDirWS, sunDirWS);
	if (sunLenSq < 1e-8f) {
		return 0.0.xxx;
	}
	float3 L = sunDirWS * rsqrt(sunLenSq);
	float3 V = -viewDirection;

	float wLenSq = dot(waveNormalWS, waveNormalWS);
	float3 N = (wLenSq > 1e-8f) ? (waveNormalWS * rsqrt(wLenSq)) : float3(0.0f, 0.0f, 1.0f);

	float NdotL = saturate(dot(N, L));
	float NdotV = saturate(dot(N, V));
	float VdotL = saturate(dot(V, L));

	// Viewer looks toward the sun — same-side as forward scattering through wavelets
	float facingSun = pow(VdotL, kDirSunSSS_FacingPow);

	// Wrapped diffuse on sun-lit faces (diffusion inside the volume)
	float wrapLight = saturate(NdotL * (1.0f - kDirSunSSS_Wrap) + kDirSunSSS_Wrap);

	// Grazing view — thin film / crest transmission
	float grazing = pow(1.0f - NdotV, kDirSunSSS_GrazingPow);

	float maxAmp = max(Wave1Amplitude * WaveAmplitude * M_TO_GAME_UNIT, 1.0f);
	float h = saturate(max(0.0f, waveHeight) / maxAmp);

	float dbDir = saturate(depthBlend * 0.9f);
	float3 tint = lerp(Color::Water(shallowColor), Color::Water(deepColor), dbDir);
	float strength = facingSun * wrapLight * grazing * (0.2f + 0.8f * h) * ScatteringCoeff * kDirSunSSS_Intensity;
	return tint * sunColor * strength;
}

#endif  // __WATER_WAVE_SCATTERING_HLSLI__
