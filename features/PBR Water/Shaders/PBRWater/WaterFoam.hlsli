#ifndef __WATER_FOAM_HLSLI__
#define __WATER_FOAM_HLSLI__

// ============================================================================
// PROCEDURAL WATER FOAM
// ============================================================================
//
// Foam noise is sampled at the REST position (before Gerstner horizontal
// displacement) so the pattern stays anchored to the water grid rather than
// sliding as wave crests pass through.
//
// Foam is lit by the sun via a wrapped diffuse term derived from the wave
// normal, and fades on steep wave slopes where real foam would slide off.

// ── Noise primitives (pure arithmetic, no transcendentals) ──────────────────

float _foamHash1(float2 p)
{
	float3 p3 = frac(float3(p.xyx) * 0.1031f);
	p3 += dot(p3, p3.yzx + 33.33f);
	return frac((p3.x + p3.y) * p3.z);
}

float2 _foamHash2(float2 p)
{
	float3 p3 = frac(float3(p.xyx) * float3(0.1031f, 0.1030f, 0.0973f));
	p3 += dot(p3, p3.yzx + 33.33f);
	return frac((p3.xx + p3.yz) * p3.zy);
}

float _foamValueNoise(float2 p)
{
	float2 i = floor(p);
	float2 f = frac(p);
	float2 u = f * f * (3.0f - 2.0f * f);

	float a = _foamHash1(i);
	float b = _foamHash1(i + float2(1.0f, 0.0f));
	float c = _foamHash1(i + float2(0.0f, 1.0f));
	float d = _foamHash1(i + float2(1.0f, 1.0f));

	return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float _foamFBM(float2 p)
{
	float v = 0.0f;
	float a = 0.5f;
	float2 shift = float2(100.0f, 100.0f);

	[unroll]
	for (int i = 0; i < 3; i++) {
		v += a * _foamValueNoise(p);
		p = p * 2.2f + shift;
		a *= 0.5f;
	}
	return v;
}

float _foamVoronoi(float2 p)
{
	float2 n = floor(p);
	float2 f = frac(p);
	float md = 8.0f;

	[unroll]
	for (int j = -1; j <= 1; j++) {
		[unroll]
		for (int i = -1; i <= 1; i++) {
			float2 g = float2(float(i), float(j));
			float2 o = _foamHash2(n + g);
			float2 r = g + o - f;
			md = min(md, dot(r, r));
		}
	}
	return sqrt(md);
}

// ── Result structure ────────────────────────────────────────────────────────

struct FoamResult
{
	float3 color;
	float alpha;
};

// ── Main foam calculation ───────────────────────────────────────────────────

FoamResult CalculateFoam(
	float2 worldPos,
	float3 waveNormal,
	float waveHeight,
	float horizontalDisp,
	float2 wavePrimaryDir,
	float shoreInfluence,
	float timeSeconds,
	float cameraDist,
	float intensityMult,
	float3 sunDirection,
	float3 sunLightColor,
	float intersectionProximity)
{
	FoamResult result;
	result.color = float3(0.0f, 0.0f, 0.0f);
	result.alpha = 0.0f;

	// ── REST POSITION ───────────────────────────────────────────────────
	float2 restPos = worldPos - wavePrimaryDir * horizontalDisp;

	// ── COVERAGE: where foam appears ────────────────────────────────────
	// FoamThreshold controls how far down the wave foam extends:
	// 0 = everywhere with any positive height, 1 = only the peak.

	float maxAmp = max(Wave1Amplitude * WaveAmplitude * 70.0f, 1.0f);
	float normHeight = saturate(waveHeight / maxAmp);

	float heightMask = smoothstep(FoamThreshold * 0.5f, FoamThreshold + 0.3f, normHeight);

	float normalSteep = 1.0f - waveNormal.z;
	float normalContrib = smoothstep(0.15f, 0.6f, normalSteep) * normHeight;

	float crestCoverage = saturate(heightMask + normalContrib * 0.5f);

	// Shore foam
	float shoreCoverage = shoreInfluence * shoreInfluence;
	shoreCoverage = max(shoreCoverage, saturate(normHeight + 0.3f) * shoreInfluence);

	// Intersection foam: depth-buffer proximity to submerged geometry
	// intersectionProximity is 0 in open water, 1 right at a rock/shore contact
	float contactCoverage = intersectionProximity * intersectionProximity;

	float coverage = saturate(crestCoverage + shoreCoverage + contactCoverage) * intensityMult;
	if (coverage < 0.005f)
		return result;

	// ── FOAM TEXTURE (sampled at rest position) ─────────────────────────

	static const float CELL_SCALE = 0.075f;
	static const float FBM_SCALE = 0.022f;

	float2 flow = wavePrimaryDir * timeSeconds * 6.0f;

	// All noise uses restPos — foam pattern is pinned to the water grid
	float2 cellUV = (restPos + flow * 0.35f) * CELL_SCALE;
	float cellDist = _foamVoronoi(cellUV);

	float cellDist2 = _foamVoronoi(cellUV * 2.1f + float2(17.3f, 31.7f));
	float cells = cellDist * 0.55f + cellDist2 * 0.45f;

	float2 fbmUV = (restPos + flow * 0.2f) * FBM_SCALE;
	float organic = _foamFBM(fbmUV);

	float2 wavePerp = float2(-wavePrimaryDir.y, wavePrimaryDir.x);
	float2 streakSamplePos = restPos + flow * 0.15f;
	float streakCoord = dot(streakSamplePos, wavePrimaryDir) * FBM_SCALE * 0.7f
	                  + dot(streakSamplePos, wavePerp) * FBM_SCALE * 2.0f;
	float streak = _foamValueNoise(float2(streakCoord, streakCoord * 0.7f + 50.0f));

	// ── COMPOSITE FOAM MASK ─────────────────────────────────────────────

	float baseThreshold = lerp(0.55f, 0.08f, saturate(coverage * 1.3f));
	float threshold = baseThreshold + (organic - 0.5f) * 0.18f;
	threshold += (streak - 0.5f) * 0.08f;

	float bubbles = 1.0f - smoothstep(max(threshold * 0.4f, 0.02f), threshold, cells);

	float detail = _foamValueNoise((restPos + flow * 0.5f) * CELL_SCALE * 3.5f);
	bubbles *= smoothstep(0.15f, 0.45f, detail);

	float foamAlpha = bubbles * coverage;

	float sharpness = clamp(FoamSharpness, 0.5f, 4.0f);
	foamAlpha = pow(max(foamAlpha, 0.001f), sharpness);
	foamAlpha = saturate(foamAlpha);

	// ── SLOPE FADE ──────────────────────────────────────────────────────
	// Foam thins on near-vertical wave faces but stays on moderate slopes.
	// smoothstep gives a gentle rolloff so foam doesn't vanish on tilted crests.
	float slopeFade = smoothstep(0.15f, 0.7f, waveNormal.z);
	foamAlpha *= slopeFade;

	// ── NORMAL-BASED LIGHTING ───────────────────────────────────────────
	// Foam sits ON the wave surface, so it must be lit by the same normal.
	// Wrapped diffuse keeps shadow-side foam from going completely black.
	float foamNdotL = dot(waveNormal, sunDirection);
	float foamDiffuse = saturate(foamNdotL * 0.5f + 0.5f);

	// Shadow-side ambient: cool indirect tint when facing away from sun
	float3 shadowAmbient = float3(0.15f, 0.18f, 0.22f);
	float3 foamLighting = lerp(shadowAmbient, sunLightColor * 0.9f + 0.1f, foamDiffuse);

	// ── COLOR ───────────────────────────────────────────────────────────

	float3 foamWhite = float3(0.93f, 0.96f, 0.99f);
	float3 foamThin = float3(0.55f, 0.68f, 0.76f);
	float3 foamAlbedo = lerp(foamThin, foamWhite, saturate(foamAlpha * 2.0f));

	// Subtle warm edge tint (subsurface through bubble films)
	float edgeMask = smoothstep(0.02f, 0.15f, foamAlpha) * (1.0f - smoothstep(0.3f, 0.7f, foamAlpha));
	foamAlbedo += float3(0.08f, 0.04f, 0.0f) * edgeMask;

	result.color = foamAlbedo * foamLighting;

	// ── DISTANCE FADE ───────────────────────────────────────────────────

	float distFade = 1.0f - smoothstep(2500.0f, 6000.0f, cameraDist);
	result.alpha = foamAlpha * distFade;

	return result;
}

#endif // __WATER_FOAM_HLSLI__
