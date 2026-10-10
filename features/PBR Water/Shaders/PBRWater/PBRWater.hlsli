#ifndef __PBR_WATER_HLSLI__
#define __PBR_WATER_HLSLI__

#include "Common/FrameBuffer.hlsli"
#include "Common/Game.hlsli"
#include "Common/Math.hlsli"

// ============================================================================
// PBR Water - shared constants, resources and the wave model.
//
// The open water is an FFT ocean (Tessendorf 2001): four world-anchored cascades of 1000, 162, 26.3 and
// 4.27 m, each carrying one band of a JONSWAP wind sea plus swell, synthesised every frame on the GPU
// (OceanSpectrumCS / OceanFFTCS / OceanAssembleCS) into mip-mapped texture arrays. Every consumer samples
// the same textures with the same per-location weights:
//   - vertex / domain shader : displacement (current + previous frame for motion vectors), mip by vertex spacing
//   - pixel shader           : normals, crest foam trails and LEAN-filtered roughness, mip by pixel footprint
//   - hull shader            : curvature for adaptive tessellation
// The CPU re-synthesises the long-wave cascades from the same coefficients (WaveModel.cpp), so swimming,
// buoyancy and floating objects follow the rendered surface without any GPU readback.
//
// What varies over the map (fetch, depth, river flow) cannot change a cascade's content, so it weights
// each cascade as a whole: the fetch-limited spectral peak removes long-wave cascades from sheltered water,
// young seas get the JONSWAP energy gain, and tanh(kh) calms waves in the shallows.
//
// References:
//   Tessendorf, "Simulating Ocean Water" (2001)
//   Horvath, "Empirical Directional Wave Spectra for Computer Graphics" (DigiPro 2015)
//   Bruneton, Neyret & Holzschuch, "Real-time Realistic Ocean Lighting using Seamless Transitions from
//   Geometry to BRDF" (2010) - filtered slope variance
//   Olano & Baker, "LEAN Mapping" (2010)
//   Mihelich & Tcheblokov, "Wakes, Explosions and Lighting: Interactive Water Simulation in Atlas" (GDC 2019)
//   Ang et al., "The Technical Art of Sea of Thieves" (SIGGRAPH 2018 Talks) - foam/scatter breakdown
// ============================================================================

#define PBRW_CASCADES 4

namespace PBRWater
{
	cbuffer PBRWaterData : register(b7)
	{
		float4 Cascade0[PBRW_CASCADES];  // x 1 / tile size (1/unit), yz tile coordinate of the reference camera (incl. half texel), w texel size (units)
		float4 Cascade1[PBRW_CASCADES];  // x energy-weighted angular frequency (rad/s), y 1 / its Pierson-Moskowitz weight at unlimited fetch, z energy-weighted wavenumber (rad/unit), w std of its compression (open sea)
		float4 Params0;                  // x FFT ocean active, y wind speed (m/s), z peak omega at unlimited fetch, w gravity (units/s^2)
		float4 Params1;                  // xy wind direction, z displacement fade start, w displacement fade end (units)
		float4 Params2;                  // x choppiness, y whitecap coverage, z displacement bound (units), w highest mip level
		float4 RefCamPos;                // xyz camera position the tile coordinates are relative to, w unused
		float4 Tess0;                    // x tessellation active for this draw, y target triangle size (px), z max factor, w pixels per unit at distance 1
		float4 Draw0;                    // x vertex spacing of this draw (units), y wireframe mode, z debug view, w unused
		float4 Fetch0;                   // xy grid origin (absolute world units), zw 1 / grid extent (units)
		float4 Fetch1;                   // x enabled, y direction count, z wind slice coordinate, w fetch outside the grid (m)
		float4 Terrain0;                 // xy heightmap uv scale, zw heightmap uv offset
		float4 Terrain1;                 // x heightmap z min, y heightmap z max, z enabled, w heightmap texel size (units)
		float4 Shore0;                   // x amplitude (units), y angular frequency (rad/s), z nominal beach slope, w onset depth (units)
		float4 Shore1;                   // x steepness, y foam strength, z phase (now), w phase (previous frame)
		float4 Ripple0;                  // xy simulation origin (absolute world units), z 1 / simulation extent (units), w enabled
		float4 Ripple1;                  // x displacement scale (units), y normal strength, z unused, w texel size (uv)
		float4 Ripple2;                  // xy previous frame's simulation origin, z step interpolation (now), w step interpolation (previous frame)
		float4 Light0;                   // x base roughness, y subsurface strength, z vanilla fresnel blend, w sun specular intensity
		float4 Light1;                   // x visibility scale, y foam albedo, z refraction distortion scale, w point light specular intensity
		float4 Foam0;                    // x shore foam width (units), y crest foam threshold, z foam amount, w foam pattern scale (units)
		float4 Foam1;                    // x crest foam persistence (s), y foam animation time (s, wrapped), z wake foam strength, w flow wave damping
		float4 Clarity0;                 // x sediment extinction (1/unit per unit turbidity), y base turbidity, z patchiness (0..1), w patch size (units)
		float4 Clarity1;                 // xyz sediment colour (gamma, like the water form colours), w shore resuspension strength
		float4 Clarity2;                 // x river turbidity, y weather turbidity (storm and rain, already scaled), z wading silt strength, w sediment layer height (units)
		float4 Optics0;                  // x scattering anisotropy g, y downwelling attenuation scale, z clarity animation time (s, wrapped), w significant wave height (units)
		float4 Surface0;                 // x wind roughness scale, y gust strength, z gust size (units), w rain intensity (0..1)
		float4 Surface1;                 // x wind streak strength, yzw unused
		float4 Land0;                    // xy loaded terrain grid corner (absolute units), zw 1 / grid extent (units)
		float4 Land1;                    // x enabled, y vertex spacing (units)
		float4 Foam2;                    // x whitecap amount, y whitecap pattern scale (units), z whitecap streak stretch, w bubble amount
		float4 Foam3;                    // xy foam drift offset (units, wrapped), zw extra bubble drift (units, wrapped)
		float4 Flow0;                    // worldspace flowmap UV = absXY * xz + yw (x = 0: no flowmap)
	}

	Texture2D<float4> RippleTexture : register(t110);     // x height, y height one step earlier, z foam, w silt
	Texture2DArray<float> FetchTexture : register(t111);  // fetch in km, one slice per upwind direction
	Texture2D<float> TerrainHeightTexture : register(t112);
	Texture2D<float4> RipplePreviousTexture : register(t113);           // the ripple state as displayed last frame (motion vectors)
	Texture2D<float> LoadedTerrainTexture : register(t114);             // exact terrain z of the loaded cells (LAND vertex heights)
	Texture2DArray<float4> OceanDisplacement : register(t115);          // xyz displacement (units), w dDx/dy, one slice per cascade
	Texture2DArray<float4> OceanDerivatives : register(t116);           // xy height slopes, zw compression dDx/dx, dDy/dy
	Texture2DArray<float4> OceanSurface : register(t117);               // xy squared slopes, z crest foam trail, w |laplacian| (1/unit)
	Texture2DArray<float4> OceanPreviousDisplacement : register(t118);  // last frame's displacement (motion vectors)
	SamplerState LinearClampSampler : register(s12);
	SamplerState OceanSampler : register(s13);  // anisotropic, wrap

	static const float UnitsPerMetre = METRES_TO_UNITS;
	static const float MetresPerUnit = 1.0 / METRES_TO_UNITS;

	/// Normalises `v`, or returns `fallback` for a zero-length vector (normalize() would return NaN).
	float3 SafeNormalize(float3 v, float3 fallback)
	{
		float lengthSq = dot(v, v);
		return lengthSq > 1e-20 ? v * rsqrt(lengthSq) : fallback;
	}

	bool OceanActive() { return Params0.x > 0.5; }
	float Gravity() { return Params0.w; }

	// ------------------------------------------------------------------------
	// Spatial context: everything that makes the spectrum vary over the map.
	// ------------------------------------------------------------------------
	struct WaveContext
	{
		float fetchMetres;   // open water upwind (m)
		float peakOmega;     // local spectral peak (rad/s); larger than the open-sea peak where fetch is short
		float energyGain;    // amplitude gain of the fetch-limited spectrum (FetchEnergyGain)
		float fetchRatio;    // local significant wave height relative to the open sea (fetch-limited growth)
		float depth;         // water depth below the undisplaced surface (units)
		float damping;       // overall amplitude multiplier (distance fade, flow, ...)
		float2 terrainGrad;  // terrain height gradient (units/unit), points uphill i.e. towards the shore
		float hasTerrain;    // 1 when depth and gradient come from the heightmap
	};

	float SampleTerrainZ(float2 absXY)
	{
		float2 uv = absXY * Terrain0.xy + Terrain0.zw;
		float h = TerrainHeightTexture.SampleLevel(LinearClampSampler, uv, 0);
		return lerp(Terrain1.x, Terrain1.y, h);
	}

	bool TerrainInside(float2 absXY)
	{
		float2 uv = absXY * Terrain0.xy + Terrain0.zw;
		return all(uv > 0.0) && all(uv < 1.0);
	}

	/// Fetch (open water distance upwind) in metres, interpolated between the two nearest
	/// precomputed directions. Short fetch means a young, short-wavelength sea.
	float SampleFetchMetres(float2 absXY)
	{
		if (Fetch1.x < 0.5)
			return Fetch1.w;
		float2 uv = (absXY - Fetch0.xy) * Fetch0.zw;
		if (any(uv < 0.0) || any(uv > 1.0))
			return Fetch1.w;
		float slices = Fetch1.y;
		float s = Fetch1.z;
		float s0 = floor(s);
		float t = s - s0;
		float s1 = fmod(s0 + 1.0, slices);
		float f0 = FetchTexture.SampleLevel(LinearClampSampler, float3(uv, s0), 0);
		float f1 = FetchTexture.SampleLevel(LinearClampSampler, float3(uv, s1), 0);
		return lerp(f0, f1, t) * 1000.0;
	}

	/// JONSWAP fetch-limited peak angular frequency (Hasselmann et al. 1973), clamped to the
	/// fully developed Pierson-Moskowitz peak.
	float FetchPeakOmega(float fetchMetres)
	{
		const float g = 9.81;
		float U = max(Params0.y, 0.5);
		float F = max(fetchMetres, 10.0);
		float omega = 22.0 * pow(abs(g * g / (U * F)), 1.0 / 3.0);
		return max(omega, Params0.z);
	}

	/// Amplitude gain of a young, fetch-limited sea: its Phillips constant is larger than the open sea's
	/// (JONSWAP alpha = 0.076 (gF/U^2)^-0.22 against Pierson-Moskowitz 0.0081), so the short waves that
	/// survive the fetch carry more energy. Amplitude scales with sqrt(alpha).
	float FetchEnergyGain(float fetchMetres)
	{
		float U = max(Params0.y, 0.5);
		float chi = 9.81 * max(fetchMetres, 10.0) / (U * U);
		float alpha = 0.076 * pow(chi, -0.22);
		return sqrt(clamp(alpha / 0.0081, 1.0, 4.0));
	}

	/// JONSWAP fetch-limited growth: Hs(F) = 0.0016 sqrt(gF) U / g relative to Hs(open sea) = 0.21 U^2 / g.
	float FetchHeightRatio(float fetchMetres)
	{
		return saturate(0.0016 / 0.21 * sqrt(9.81 * max(fetchMetres, 0.0)) / max(Params0.y, 0.5));
	}

	WaveContext BuildWaveContext(float3 positionWS, float damping)
	{
		WaveContext ctx;
		float3 absPos = positionWS + FrameBuffer::CameraPosAdjust.xyz;
		ctx.fetchMetres = SampleFetchMetres(absPos.xy);
		ctx.peakOmega = FetchPeakOmega(ctx.fetchMetres);
		ctx.energyGain = FetchEnergyGain(ctx.fetchMetres);
		ctx.fetchRatio = FetchHeightRatio(ctx.fetchMetres);
		ctx.depth = 1e6;
		ctx.terrainGrad = 0.0;
		ctx.hasTerrain = 0.0;
		ctx.damping = damping;

		// The undisplaced surface point is the water plane: its own z is the water height.
		float2 landUV = (absPos.xy - Land0.xy) * Land0.zw;
		if (Land1.x > 0.5 && all(landUV > 0.0) && all(landUV < 1.0)) {
			// Exact terrain of the loaded cells: resolves beaches, rocks and shelves.
			float2 t = Land1.y * Land0.zw;
			float z0 = LoadedTerrainTexture.SampleLevel(LinearClampSampler, landUV, 0);
			float zx = LoadedTerrainTexture.SampleLevel(LinearClampSampler, landUV + float2(t.x, 0), 0) -
			           LoadedTerrainTexture.SampleLevel(LinearClampSampler, landUV - float2(t.x, 0), 0);
			float zy = LoadedTerrainTexture.SampleLevel(LinearClampSampler, landUV + float2(0, t.y), 0) -
			           LoadedTerrainTexture.SampleLevel(LinearClampSampler, landUV - float2(0, t.y), 0);
			ctx.depth = max(absPos.z - z0, 0.0);
			ctx.terrainGrad = float2(zx, zy) / (2.0 * Land1.y);
			ctx.hasTerrain = 1.0;
		} else if (Terrain1.z > 0.5 && TerrainInside(absPos.xy)) {
			float texel = Terrain1.w;
			float z0 = SampleTerrainZ(absPos.xy);
			float zx = SampleTerrainZ(absPos.xy + float2(texel, 0));
			float zy = SampleTerrainZ(absPos.xy + float2(0, texel));
			ctx.depth = max(absPos.z - z0, 0.0);
			ctx.terrainGrad = float2(zx - z0, zy - z0) / texel;
			ctx.hasTerrain = 1.0;
		}
		return ctx;
	}

	/// Distance fade of the displacement, so far water converges to the flat vanilla plane
	/// (and to the untessellated LOD meshes) instead of popping.
	float DisplacementDistanceFade(float distance)
	{
		return 1.0 - smoothstep(Params1.z, max(Params1.w, Params1.z + 1.0), distance);
	}

	/// tanh for x >= 0 that cannot overflow. FXC lowers the intrinsic to (e^2x - 1) / (e^2x + 1),
	/// which is inf / inf = NaN once kh exceeds ~44, i.e. for any deep water.
	float SafeTanh(float x)
	{
		return 1.0 - 2.0 / (exp(2.0 * min(max(x, 0.0), 20.0)) + 1.0);
	}

	/**
	 * Vertical amplitude multiplier of cascade `c` at this location. Fetch: the ratio of the local
	 * Pierson-Moskowitz low-frequency cut-off to the open-sea one at the cascade's mean frequency, so long
	 * swells cannot exist on a pond while short chop is unaffected, times the young sea's energy gain.
	 * Depth: orbital motion is limited by the bottom (tanh(kh) from linear wave theory).
	 */
	float CascadeWeight(uint c, WaveContext ctx)
	{
		float r = ctx.peakOmega / Cascade1[c].x;
		float r2 = r * r;
		float w = saturate(exp(-1.25 * r2 * r2) * Cascade1[c].y) * ctx.energyGain;
		w *= SafeTanh(Cascade1[c].z * ctx.depth);
		return w * ctx.damping;
	}

	/// Tile coordinate of camera-relative position `p` in cascade `c` (world-anchored, re-based on the
	/// reference camera so other cameras still sample the same water).
	float2 CascadeUV(float2 p, uint c)
	{
		float2 rel = p + (FrameBuffer::CameraPosAdjust.xy - RefCamPos.xy);
		return rel * Cascade0[c].x + Cascade0[c].yz;
	}

	struct WaveResult
	{
		float3 displacement;          // world-space offset (units)
		float3 previousDisplacement;  // the same last frame (motion vectors)
		float3 normal;                // surface normal
		float jacobian;               // < 1 where the surface compresses, < 0 where it folds (breaking)
		float slopeVariance;          // slope variance filtered out of the pixel (for roughness)
		float crestTrail;             // 0..1 foam left behind by crests that folded a moment ago
		float shoreBreak;             // 0..1 breaking intensity of the shore waves
		float shoreMask;              // 0..1 presence of the shore waves
		float shoreCrest;             // 0..1 how close this point is to a shore wave crest (swash foam)
	};

	/// Running sums of the cascades' fields.
	struct WaveSums
	{
		float3 displacement;
		float3 previous;
		float dzdx, dzdy, dxdx, dydy, dxdy;
		float slopeVariance;
		float crestTrail;
		float compressionVariance;  // open-sea compression variance of the cascades here (shore wave budget)
	};

	WaveSums EmptySums()
	{
		WaveSums sums;
		sums.displacement = 0.0;
		sums.previous = 0.0;
		sums.dzdx = sums.dzdy = sums.dxdx = sums.dydy = sums.dxdy = 0.0;
		sums.slopeVariance = 0.0;
		sums.crestTrail = 0.0;
		sums.compressionVariance = 0.0;
		return sums;
	}

	/// Adds one cascade: `vertical` scales heights and slopes, `horizontal` the choppy displacement.
	void AddCascade(inout WaveSums sums, float vertical, float horizontal, float4 displacement, float4 derivatives)
	{
		sums.displacement += float3(displacement.xy * horizontal, displacement.z * vertical);
		sums.dzdx += derivatives.x * vertical;
		sums.dzdy += derivatives.y * vertical;
		sums.dxdx += derivatives.z * horizontal;
		sums.dydy += derivatives.w * horizontal;
		sums.dxdy += displacement.w * horizontal;
	}

	/// Adds the compression spread of cascade `c` scaled by its horizontal weight here.
	void AddCompressionVariance(inout WaveSums sums, uint c, float horizontal)
	{
		float sigma = Cascade1[c].w * horizontal;
		sums.compressionVariance += sigma * sigma;
	}

	/// Nominal shoaling travel time from depth h to the shoreline on a beach of slope s:
	/// integral of dx / sqrt(g h) = 2 sqrt(h) / (s sqrt(g)). Its gradient gives wavefronts that
	/// follow the depth contours and shorten towards the beach, i.e. refraction and shoaling.
	float ShoreTravelTime(float depthUnits)
	{
		float hm = max(depthUnits * MetresPerUnit, 0.02);
		return 2.0 * sqrt(hm) / (max(Shore0.z, 0.005) * sqrt(9.81));
	}

	/// Shoreline waves: a separate train that travels up the depth gradient and breaks.
	void AddShoreWaves(inout WaveSums sums, inout WaveResult o, WaveContext ctx)
	{
		o.shoreBreak = 0.0;
		o.shoreMask = 0.0;
		o.shoreCrest = 0.0;
		if (ctx.hasTerrain < 0.5 || Shore0.x <= 0.0 || ctx.depth >= Shore0.w)
			return;
		float gradLen = length(ctx.terrainGrad);
		if (gradLen <= 1e-4)
			return;
		float2 dir = ctx.terrainGrad / gradLen;
		float h = max(ctx.depth, 1.0);
		float onset = Shore0.w;

		// Wave height grows with Green's law (h^-1/4) and is capped by depth-limited breaking (H <= 0.78 h).
		float A0 = Shore0.x * ctx.fetchRatio * ctx.damping;
		float A = A0 * sqrt(sqrt(onset / h));
		float Amax = 0.39 * h;
		o.shoreBreak = saturate(A / Amax - 1.0);
		A = min(A, Amax);

		// Fade in as the bottom is felt, fade out in the last few centimetres of the swash zone.
		float envelope = smoothstep(onset, onset * 0.5, h) * smoothstep(0.05 * UnitsPerMetre, 0.25 * UnitsPerMetre, h);
		o.shoreMask = envelope;
		A *= envelope;

		// d(theta)/dx: theta' (h) * dh/dx, with dh/dx = -terrainGrad
		float omega = Shore0.y;
		float hm = max(h * MetresPerUnit, 0.02);
		float dTdh = (1.0 / (max(Shore0.z, 0.005) * sqrt(9.81 * hm))) * MetresPerUnit;
		float2 dTheta = omega * dTdh * ctx.terrainGrad;
		float kLocal = length(dTheta);
		// The wavelength collapses as 1 / sqrt(h) in the swash, and much faster on a steep (cliff)
		// shore: hold the slope k A to the breaking limit (H / L ~ 1/7) so the last metre of water
		// does not stand up into a bright, near-vertical seam along the waterline.
		A = min(A, 0.4 / max(kLocal, 1e-5));

		float travel = -omega * ShoreTravelTime(h);
		float s, c;
		sincos(travel - Shore1.z, s, c);
		float sp, cp;
		sincos(travel - Shore1.w, sp, cp);
		o.shoreCrest = envelope * saturate(s);

		// Breaking waves pitch forward; keep the total compression below 1 with the open sea on top (its
		// 3-sigma compression), or crests that coincide fold the surface over itself (inside-out patches).
		float steepness = 3.0 * sqrt(sums.compressionVariance);
		float Q = saturate(Shore1.x + o.shoreBreak * 0.5);
		float QA = min(Q * A, max(0.9 - steepness, 0.0) / max(kLocal, 1e-5));

		sums.displacement.xy += dir * (QA * c);
		sums.displacement.z += A * s;
		sums.previous.xy += dir * (QA * cp);
		sums.previous.z += A * sp;
		sums.dzdx += dTheta.x * A * c;
		sums.dzdy += dTheta.y * A * c;
		float QK = QA * kLocal;
		sums.dxdx -= dir.x * dir.x * QK * s;
		sums.dydy -= dir.y * dir.y * QK * s;
		sums.dxdy -= dir.x * dir.y * QK * s;
	}

	WaveResult FinishWaves(WaveSums sums, WaveResult o)
	{
		// Normal of the parametric surface P(x, y) = (x + Dx, y + Dy, Dz)
		float3 tx = float3(1.0 + sums.dxdx, sums.dxdy, sums.dzdx);
		float3 ty = float3(sums.dxdy, 1.0 + sums.dydy, sums.dzdy);
		// tx and ty become parallel where the surface folds over itself; keep the normal defined there,
		// and facing up where it has folded past vertical (the visible side of the fold).
		float3 n = cross(tx, ty);
		o.normal = SafeNormalize(n.z < 0.0 ? -n : n, float3(0, 0, 1));
		o.jacobian = (1.0 + sums.dxdx) * (1.0 + sums.dydy) - sums.dxdy * sums.dxdy;
		o.displacement = sums.displacement;
		o.previousDisplacement = sums.previous;
		o.slopeVariance = sums.slopeVariance;
		o.crestTrail = sums.crestTrail;
		return o;
	}

	/**
	 * Waves for geometry (vertex, domain, underwater shaders) at undisplaced camera-relative `positionWS`.
	 * Each cascade is read from the mip whose texels match the vertex spacing, so waves the mesh cannot
	 * carry are averaged out instead of aliasing.
	 * @param spacing vertex spacing (units); 0 reads the full resolution
	 * @param previous also read last frame's displacement (compiled out when unused)
	 */
	WaveResult EvaluateWaves(float3 positionWS, WaveContext ctx, float spacing, bool previous)
	{
		WaveResult o = (WaveResult)0;
		WaveSums sums = EmptySums();
		float steepnessScale = rcp(ctx.energyGain);
		if (OceanActive()) {
			[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
			{
				float vertical = CascadeWeight(c, ctx);
				[branch] if (vertical > 0.0)
				{
					float3 uv = float3(CascadeUV(positionWS.xy, c), c);
					float lod = spacing > 0.0 ? clamp(log2(spacing / Cascade0[c].w) - 0.5, 0.0, Params2.w) : 0.0;
					float horizontal = vertical * steepnessScale;
					float4 d = OceanDisplacement.SampleLevel(OceanSampler, uv, lod);
					float4 g = OceanDerivatives.SampleLevel(OceanSampler, uv, lod);
					AddCascade(sums, vertical, horizontal, d, g);
					AddCompressionVariance(sums, c, horizontal);
					if (previous) {
						float4 dp = OceanPreviousDisplacement.SampleLevel(OceanSampler, uv, lod);
						sums.previous += float3(dp.xy * horizontal, dp.z * vertical);
					}
				}
			}
		}
		AddShoreWaves(sums, o, ctx);
		return FinishWaves(sums, o);
	}

	/**
	 * Waves for shading at undisplaced camera-relative `positionWS`, filtered over the pixel footprint
	 * (trilinear / anisotropic). The slope variance the filter removes is recovered from the mip-filtered
	 * second moments (LEAN mapping) and returned for the roughness; foam trails come from the cascades
	 * that carry the waves at this location.
	 */
	WaveResult EvaluateWavesFiltered(float3 positionWS, WaveContext ctx)
	{
		WaveResult o = (WaveResult)0;
		WaveSums sums = EmptySums();
		float steepnessScale = rcp(ctx.energyGain);
		float2 dpdx = ddx(positionWS.xy);
		float2 dpdy = ddy(positionWS.xy);
		if (OceanActive()) {
			[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
			{
				float vertical = CascadeWeight(c, ctx);
				float3 uv = float3(CascadeUV(positionWS.xy, c), c);
				float2 gx = dpdx * Cascade0[c].x;
				float2 gy = dpdy * Cascade0[c].x;
				// Sampled unconditionally: gradient sampling must stay in uniform control flow.
				float4 d = OceanDisplacement.SampleGrad(OceanSampler, uv, gx, gy);
				float4 g = OceanDerivatives.SampleGrad(OceanSampler, uv, gx, gy);
				float4 m = OceanSurface.SampleGrad(OceanSampler, uv, gx, gy);
				float horizontal = vertical * steepnessScale;
				AddCascade(sums, vertical, horizontal, d, g);
				AddCompressionVariance(sums, c, horizontal);
				float variance = max(m.x - g.x * g.x, 0.0) + max(m.y - g.y * g.y, 0.0);
				sums.slopeVariance += variance * vertical * vertical;
				// The trail was laid down by the open-sea cascade; where fetch or depth calm it, it folds less.
				float presence = saturate(horizontal);
				sums.crestTrail = max(sums.crestTrail, m.z * presence * presence);
			}
		}
		AddShoreWaves(sums, o, ctx);
		return FinishWaves(sums, o);
	}

	/**
	 * Curvature of the displaced surface along unit direction `e` (units^-1) around undisplaced position
	 * `positionWS`, at the scale `scale` (units) the tessellation is aiming for, so crests get the
	 * subdivision and broad troughs and calm water do not. Uses the mean |laplacian| of the height over
	 * that scale (mip-filtered, so short waves are not averaged away), sharpened by the horizontal
	 * compression along `e` as on Gerstner-like crests.
	 */
	float SurfaceCurvature(float3 positionWS, WaveContext ctx, float2 e, float scale)
	{
		if (!OceanActive())
			return 0.0;
		float curvature = 0.0;
		float compression = 0.0;
		float steepnessScale = rcp(ctx.energyGain);
		[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
		{
			float vertical = CascadeWeight(c, ctx);
			[branch] if (vertical > 0.0)
			{
				float3 uv = float3(CascadeUV(positionWS.xy, c), c);
				float lod = clamp(log2(max(scale, 1e-3) / Cascade0[c].w), 0.0, Params2.w);
				float4 m = OceanSurface.SampleLevel(OceanSampler, uv, lod);
				float4 g = OceanDerivatives.SampleLevel(OceanSampler, uv, lod);
				float shear = OceanDisplacement.SampleLevel(OceanSampler, uv, lod).w;
				curvature += m.w * vertical;
				compression += (g.z * e.x * e.x + 2.0 * shear * e.x * e.y + g.w * e.y * e.y) * vertical * steepnessScale;
			}
		}
		float stretch = max(1.0 + compression, 0.2);
		return curvature / (stretch * stretch);
	}

	// ------------------------------------------------------------------------
	// Interactive ripples (GPU wave-equation simulation centred on the camera)
	// ------------------------------------------------------------------------
	float RippleEdgeFade(float2 uv)
	{
		float2 e = saturate(min(uv, 1.0 - uv) * 10.0);
		return e.x * e.y;
	}

	/// Displayed ripple height. The simulation runs at a fixed rate, so the two stored steps are
	/// interpolated to the render time; `previous` samples last frame's copy for motion vectors.
	float RippleHeight(float3 positionWS, bool previous)
	{
		if (Ripple0.w < 0.5)
			return 0.0;
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		float2 uv = (absXY - (previous ? Ripple2.xy : Ripple0.xy)) * Ripple0.z;
		if (any(uv <= 0.0) || any(uv >= 1.0))
			return 0.0;
		float2 steps = previous ? RipplePreviousTexture.SampleLevel(LinearClampSampler, uv, 0).xy :
		                          RippleTexture.SampleLevel(LinearClampSampler, uv, 0).xy;
		return lerp(steps.y, steps.x, previous ? Ripple2.w : Ripple2.z) * Ripple1.x * RippleEdgeFade(uv);
	}

	float RippleDisplayed(float2 uv)
	{
		float2 steps = RippleTexture.SampleLevel(LinearClampSampler, uv, 0).xy;
		return lerp(steps.y, steps.x, Ripple2.z);
	}

	/// Silt stirred up from the bottom by bodies wading through shallow water (ripple simulation w channel).
	float RippleSilt(float3 positionWS)
	{
		if (Ripple0.w < 0.5)
			return 0.0;
		float2 uv = (positionWS.xy + FrameBuffer::CameraPosAdjust.xy - Ripple0.xy) * Ripple0.z;
		if (any(uv <= 0.0) || any(uv >= 1.0))
			return 0.0;
		return max(RippleTexture.SampleLevel(LinearClampSampler, uv, 0).w, 0.0) * RippleEdgeFade(uv);
	}

	/// Ripples cannot be taller than the water is deep; keeps them from dipping below the shore.
	float RippleDepthFade(float depth)
	{
		return saturate(depth / max(3.0 * Ripple1.x, 1.0));
	}

	/// xy = height gradient (units/unit), z = foam
	float3 RippleSlopeFoam(float3 positionWS, float depth)
	{
		if (Ripple0.w < 0.5)
			return 0.0;
		float2 uv = (positionWS.xy + FrameBuffer::CameraPosAdjust.xy - Ripple0.xy) * Ripple0.z;
		if (any(uv <= 0.0) || any(uv >= 1.0))
			return 0.0;
		float t = Ripple1.w;
		float hL = RippleDisplayed(uv - float2(t, 0));
		float hR = RippleDisplayed(uv + float2(t, 0));
		float hD = RippleDisplayed(uv - float2(0, t));
		float hU = RippleDisplayed(uv + float2(0, t));
		// Foam churned up by bodies and breaking ripples; the simulation carries it with the surface drift
		// and lets it thin out, so it stays where the water was disturbed instead of flickering with crests.
		float foam = RippleTexture.SampleLevel(LinearClampSampler, uv, 0).z;
		float texelUnits = t / Ripple0.z;
		float2 grad = float2(hR - hL, hU - hD) / (2.0 * texelUnits) * Ripple1.x * Ripple1.y * RippleDepthFade(depth);
		return float3(grad, foam) * RippleEdgeFade(uv);
	}

	// ------------------------------------------------------------------------
	// Full surface displacement used by geometry (VS / DS)
	// ------------------------------------------------------------------------
	struct SurfaceDisplacement
	{
		float3 current;
		float3 previous;
		float jacobian;
		float shoreBreak;
		float depth;
	};

	SurfaceDisplacement DisplaceSurface(float3 positionWS, float flowDamping, float spacing)
	{
		SurfaceDisplacement o;
		float distance = length(positionWS);
		float damping = DisplacementDistanceFade(distance) * flowDamping;

		WaveContext ctx = BuildWaveContext(positionWS, damping);
		WaveResult now = EvaluateWaves(positionWS, ctx, spacing, true);

		float rippleFade = damping * RippleDepthFade(ctx.depth);
		o.current = now.displacement + float3(0, 0, RippleHeight(positionWS, false) * rippleFade);
		o.previous = now.previousDisplacement + float3(0, 0, RippleHeight(positionWS, true) * rippleFade);

		// Hard bound: waves stay within their statistical bound (doubled for the fetch energy gain, the shore
		// waves for shoaling) and the ripple sim within a few times its scale. Clamping also flushes NaN
		// (D3D min/max return the non-NaN operand), so a bad input can never throw the mesh off-screen.
		float limit = 2.0 * max(Params2.z, 0.0) + 2.0 * max(Shore0.x, 0.0) + 4.0 * max(Ripple1.x, 0.0) + 1.0;
		o.current = clamp(o.current, -limit, limit);
		o.previous = clamp(o.previous, -limit, limit);
		o.jacobian = now.jacobian;
		o.shoreBreak = now.shoreBreak;
		o.depth = ctx.depth;
		return o;
	}

	/**
	 * Height of the displaced surface (waves and ripples) directly above camera-relative `xy`, for a flat
	 * water plane at camera-relative height `flatZ`. The waves also move the water sideways, so the
	 * undisplaced point that ends up above `xy` is found with a short fixed-point iteration, as
	 * WaveSnapshot::SampleAt does on the CPU.
	 */
	float SurfaceHeightAt(float2 xy, float flatZ, uint iterations)
	{
		float3 p = float3(xy, flatZ);
		WaveContext ctx = BuildWaveContext(p, DisplacementDistanceFade(length(p)));
		float2 x0 = xy;
		[unroll] for (uint i = 0; i < iterations; i++)
		{
			x0 = xy - EvaluateWaves(float3(x0, flatZ), ctx, 0.0, false).displacement.xy;
		}
		float3 undisplaced = float3(x0, flatZ);
		float height = EvaluateWaves(undisplaced, ctx, 0.0, false).displacement.z;
		height += RippleHeight(undisplaced, false) * RippleDepthFade(ctx.depth);
		float limit = 2.0 * max(Params2.z, 0.0) + 2.0 * max(Shore0.x, 0.0) + 4.0 * max(Ripple1.x, 0.0) + 1.0;
		return flatZ + clamp(height, -limit, limit);
	}
}

#endif  // __PBR_WATER_HLSLI__
