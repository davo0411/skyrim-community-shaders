#ifndef __PBR_WATER_SHORE_SURF_HLSLI__
#define __PBR_WATER_SHORE_SURF_HLSLI__

// PBR Water - the surf simulation under the shore wave train (ComposeSurf). Part of PBRWater.hlsli, included where it
// stands in the wave program; not included on its own.

namespace PBRWater
{
	// ------------------------------------------------------------------------
	// The surf simulation: the broken wave's own water through the inner surf and up the beach
	//
	// A broken wave's bore runs into the previous backwash, collapses at the waterline and runs up the beach as a thin
	// sheet that slows and drains back (Hibberd and Peregrine 1979; Shen and Meyer 1963). The inner surf and the swash
	// are therefore simulated: the shallow water equations with wetting and drying on a camera-centred grid
	// (ShoreFoamCS lays out what the waves bring, SwashStepCS steps it, SwashOutCS packs it for the draws).
	//
	// The hand-over is fixed in space by the depth against the waves' height (SurfSharesAt): the simulation is given
	// the train's waves where a mean wave has become a bore, runs free a little further in, and the drawn surface
	// passes from the train to the simulation across that band (ComposeSurf). Past the grid the train runs to the
	// waterline and ends there.
	//
	// The sheet's edge is found per pixel: the mesh lies on the sand wherever the water can reach (DisplaceSurface),
	// the stencil pass keeps what the water covers (AboveWaterline), and the colour pass finds the exact edge
	// (Water.hlsl).
	// ------------------------------------------------------------------------

	/// Water shallower than this (units, 3 mm) is the sand's wet sheen, not water.
	static const float SwashMinDepth = 0.003 * UnitsPerMetre;

	/// The hand-over in break ratios (ShoreBreakRatio: 1 where a wave of the sea's mean height breaks, growing as the
	/// depth^-5/4 inshore of that): the simulation is held to the train's incoming waves up to x and free from y, and it
	/// is the whole drawn surface from z. On beaches up to about 1:5 a plunger's lip has landed by a ratio of 2.
	static const float3 SurfHandOver = float3(2.0, 3.5, 5.0);

	/// How the train and the surf simulation share the water at a point.
	struct SurfShares
	{
		float incoming;  // 0..1 the simulation takes the train's incoming waves here
		float outgoing;  // 0..1 and gives up what it sends back out to sea: further out, where the train alone is the water
		float drawn;     // 0..1 share of the simulation in the drawn surface
	};

	/// The shares at the context's point: a function of the place only (the depth and the sea arriving there).
	SurfShares SurfSharesAt(WaveContext ctx)
	{
		SurfShares o;
		// Up the beach the simulation is all the water there is.
		o.incoming = 0.0;
		o.outgoing = 0.0;
		o.drawn = 1.0;
		[branch] if (ctx.elevation < 0.0)
		{
			float ratio = ShoreBreakRatio(ctx.shoreHeight * ctx.damping * MetresPerUnit, ctx.depth * MetresPerUnit);
			o.incoming = 1.0 - smoothstep(SurfHandOver.x, SurfHandOver.y, ratio);
			o.outgoing = 1.0 - smoothstep(1.0, SurfHandOver.x, ratio);
			o.drawn = smoothstep(SurfHandOver.x, SurfHandOver.z, ratio);
		}
		return o;
	}

	/**
	 * The surf's output (SwashOutCS) at grid `uv`. At high frame rates the surf runs every other frame and the last two
	 * outputs are blended (ShoreSimulation::GetFoamBlendCode): Shore0.z is the newest slice * 2 + its weight, Shore2.z the
	 * texels the grid scrolled from the older one, packed (x + 2048) * 4096 + (y + 2048).
	 */
	float4 SurfSample(float2 uv)
	{
		float slice = Shore0.z >= 2.0 ? 1.0 : 0.0;
		float weight = Shore0.z - 2.0 * slice;
		float4 result = ShoreFoamTexture.SampleLevel(LinearClampSampler, float3(uv, slice), 0);
		[branch] if (weight < 0.999)
		{
			uint w, h, slices;
			ShoreFoamTexture.GetDimensions(w, h, slices);
			uint code = (uint)Shore2.z;
			float2 shift = float2(code / 4096u, code % 4096u) - 2048.0;
			float4 older = ShoreFoamTexture.SampleLevel(LinearClampSampler, float3(uv + shift / float2(w, h), 1.0 - slice), 0);
			result = lerp(older, result, weight);
		}
		return result;
	}

	/// The surf's newest output at texel `c`.
	float4 SurfLoad(int2 c)
	{
		return ShoreFoamTexture.Load(int4(c, Shore0.z >= 2.0 ? 1 : 0, 0));
	}

	/// The surf simulation's grid coordinates of absolute `absXY` (Foam2), and whether they are inside it.
	bool ShoreGridUV(float2 absXY, out float2 uv)
	{
		uv = (absXY - Foam2.xy) * Foam2.z;
		return Foam2.w > 0.5 && all(uv > 0.0) && all(uv < 1.0);
	}

	/// The surf simulation at a point (SampleSurf).
	struct SurfSheet
	{
		float surface;  // its surface (units above the still water): the bed where it is dry
		float depth;    // its depth (units)
		float foam;     // 0..1 the white water it carries
		float2 slope;   // the surface's gradient (units/unit)
		float share;    // 1 inside the grid, fading to 0 over its border; 0 outside
	};

	/**
	 * The surf simulation at absolute `absXY` (SwashOutCS).
	 * @param withSlope also work out the surface's gradient (four more reads)
	 * @param spacing vertex spacing (units; 0: exact): a mesh coarser than the grid reads the surface averaged over its
	 *                  spacing, so a bore's two-cell front does not step from vertex to vertex
	 */
	SurfSheet SampleSurf(float2 absXY, bool withSlope, float spacing)
	{
		SurfSheet o = (SurfSheet)0;
		float2 uv;
		[branch] if (ShoreGridUV(absXY, uv))
		{
			uint w, h;
			uint slices;
			ShoreFoamTexture.GetDimensions(w, h, slices);
			float2 texel = 1.0 / float2(w, h);
			float3 sheet;
			float2 reach = 0.35 * spacing * Foam2.z;
			[branch] if (reach.x <= 0.5 * texel.x)
			{
				// A cubic B-spline (four bilinear taps, as SampleShoreField0): bilinear reads draw a bore's two-cell front as
				// a staircase along the grid's rows and columns.
				float2 t = uv / texel - 0.5;
				float2 i = floor(t);
				float2 f = t - i;
				float2 f2 = f * f;
				float2 f3 = f2 * f;
				float2 w0 = (-f3 + 3.0 * f2 - 3.0 * f + 1.0) / 6.0;
				float2 w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
				float2 w3 = f3 / 6.0;
				float2 g0 = w0 + w1;
				float2 g1 = 1.0 - g0;
				float2 uv0 = (i - 0.5 + w1 / g0) * texel;
				float2 uv1 = (i + 1.5 + w3 / max(g1, 1e-6)) * texel;
				sheet = g0.y * (g0.x * SurfSample(uv0).yzw + g1.x * SurfSample(float2(uv1.x, uv0.y)).yzw) +
				        g1.y * (g0.x * SurfSample(float2(uv0.x, uv1.y)).yzw + g1.x * SurfSample(uv1).yzw);
			}
			else
			{
				sheet = 0.25 * (SurfSample(uv + reach).yzw +
								   SurfSample(uv - reach).yzw +
								   SurfSample(uv + float2(reach.x, -reach.y)).yzw +
								   SurfSample(uv + float2(-reach.x, reach.y)).yzw);
			}
			o.surface = sheet.x;
			o.depth = sheet.y;
			o.foam = sheet.z;
			o.share = BorderFade(uv);
			[branch] if (withSlope)
			{
				float east = SurfSample(uv + float2(texel.x, 0.0)).y;
				float west = SurfSample(uv - float2(texel.x, 0.0)).y;
				float north = SurfSample(uv + float2(0.0, texel.y)).y;
				float south = SurfSample(uv - float2(0.0, texel.y)).y;
				o.slope = float2(east - west, north - south) * (0.5 * Foam2.z / texel);
			}
		}
		return o;
	}

	/**
	 * How far inshore of the hand-over a point is, 0..1, from the simulation's share of the drawn surface there (its
	 * grid's border fade included): from halfway through the hand-over band the simulation alone says where the water
	 * is, and a cell it has drained is bare sand even below the still water; further out the sea holds (ComposeSurf,
	 * SeaFloorHold, AboveWaterline).
	 */
	float SurfInner(float drawnShare)
	{
		return smoothstep(0.5, 1.0, drawnShare);
	}

	/**
	 * Hands the train's surface to the surf simulation where the simulation is the water (SurfSharesAt): the heights
	 * pass to its surface and the train's forward lean fades out (the simulation is a height field), over the hand-over
	 * band. A breaker's lip stays (ShoreTrain::lip), drawn over whichever water is under it.
	 * @param pixel shading: the surface's gradient is read too, and where the simulation is dry the surface is far
	 *   below
	 *                the ground (the pixel is not water). For geometry the dry surface is the bed, so the mesh lies on it.
	 * @param spacing vertex spacing of the geometry (units; 0 for shading): SampleSurf
	 * @param scale vertex spacing or pixel footprint (units): the swash's churn shorter than it fades out
	 */
	void ComposeSurf(inout ShoreTrain train, float3 positionWS, WaveContext ctx, bool pixel, float spacing, float scale)
	{
		train.rawHeight = train.displacement.z;
		train.seaHold = 1.0;
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		float2 uv;
		[branch] if (ctx.hasTerrain > 0.5 && ShoreGridUV(absXY, uv))
		{
			float drawn = SurfSharesAt(ctx).drawn;
			[branch] if (drawn > 0.0)
			{
				SurfSheet sheet = SampleSurf(absXY + (1.0 - drawn) * train.displacement.xy, pixel, spacing);
				drawn *= sheet.share;
				// Where the simulation has drained a cell whose bed lies below the still water in the hand-over band,
				// the sea is still there: the train's surface is drawn instead. Inshore of it the simulation is the
				// water and what it drains is bare sand.
				float inner = SurfInner(drawn);
				train.seaHold = 1.0 - inner;
				float belowSea = (1.0 - smoothstep(-0.05 * UnitsPerMetre, 0.0, ctx.elevation)) * (inner < 0.5 ? 1.0 : 0.0);
				drawn *= lerp(1.0, smoothstep(0.0, 2.0 * SwashMinDepth, sheet.depth), belowSea);
				float keep = 1.0 - drawn;
				// Dry: no water for the pixels; the geometry keeps the train's height and DisplaceSurface lays it on
				// the bed up to where water can reach.
				float surface = pixel ? (sheet.depth > SwashMinDepth ? sheet.surface : (belowSea > 0.0 ? train.displacement.z : -1e6)) :
				                        lerp(train.displacement.z, sheet.surface, smoothstep(0.0, 2.0 * SwashMinDepth, sheet.depth));
				train.displacement = float3(train.displacement.xy * keep, lerp(train.displacement.z, surface, drawn));
				train.previousDisplacement = float3(train.previousDisplacement.xy * keep, lerp(train.previousDisplacement.z, surface, drawn));
				train.slope = lerp(train.slope, sheet.slope, drawn);
				train.compression *= keep;
				// The train's white water gives way with its surface: the simulation carries its own.
				train.foam *= keep;
				train.bore *= keep;
				train.aeration *= keep;
				train.impact *= keep;
				train.clearFace *= keep;
				train.crest = lerp(train.crest, max(sheet.surface, 0.0), drawn);
				train.surf = drawn;
				train.sheetSurface = sheet.surface;
				train.sheetDepth = sheet.depth;
				train.sheetFoam = sheet.foam * drawn;

				// The swash is churned like the bore's roller (ShoreRollerChurn): the grid's 17 cm cells carry none of
				// the small waves that run through it. Lumps of Swash1.z, as high as a share of the depth up to
				// Swash1.x, held off the bed so the water's edge stays where the simulation puts it. Lumps shorter than
				// the vertex spacing or pixel fade out.
				[branch] if (sheet.depth > SwashMinDepth)
				{
					float SwashLumpLength = max(Swash1.z, 1.0);
					float churnAmount = min(Swash1.y * sheet.depth, Swash1.x) * lerp(0.4, 1.0, saturate(sheet.foam)) * drawn;
					float lumps = churnAmount * smoothstep(Math::PI, 0.5 * Math::PI, Math::TAU / SwashLumpLength * scale);
					[branch] if (lumps > 0.0)
					{
						float2 q = absXY / SwashLumpLength;
						float3 churn = ShoreRollerChurn(q, Swash1.w * Shore2.x);
						float floorZ = sheet.surface - 0.7 * sheet.depth;
						train.displacement.z = max(train.displacement.z + lumps * churn.x, lerp(train.displacement.z, floorZ, drawn));
						train.previousDisplacement.z = max(train.previousDisplacement.z + lumps * ShoreRollerChurn(q, Swash1.w * Shore2.y).x, lerp(train.previousDisplacement.z, floorZ, drawn));
						train.slope += (lumps / SwashLumpLength) * churn.yz;
					}
				}
			}
		}
	}
}

#endif  // __PBR_WATER_SHORE_SURF_HLSLI__
