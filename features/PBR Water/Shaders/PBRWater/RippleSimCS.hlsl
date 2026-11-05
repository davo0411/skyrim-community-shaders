// ============================================================================
// PBR Water - interactive ripple simulation.
//
// A camera-centred heightfield integrated with the 2D wave equation (explicit leapfrog):
//     h(t+1) = 2 h(t) - h(t-1) + C^2 * laplacian(h(t)) + nu * (laplacian(h(t)) - laplacian(h(t-1)))
// with C = c dt / dx (stable for C^2 <= 0.5). The nu term is a viscous (Kelvin-Voigt) damping that grows
// with k^2: it leaves the visible ripples alone but removes the grid-scale checkerboard noise that a
// leapfrog scheme never dissipates by itself and that shows up as blocky "pixelated" ripples.
// Bodies in the water (actor limbs, loose Havok objects) act as moving constraints that hold
// the surface down under their footprint, which produces bow waves, wakes and rings without any
// hand-authored ripple shapes.
//
// The grid lives in the water's rest (Lagrangian) coordinates: sources are placed where the water under
// a body rests and the shaders sample at undisplaced positions, so the rings ride the waves with
// the water instead of sliding across them.
//
// The grid scrolls with the camera in whole texels; `Shift` re-addresses the previous state so the
// simulation stays fixed in world space.
//
// Ported from Crest 4 (wave-harmonic/crest, MIT) UpdateDynWaves.compute:
//  - Terrain awareness: the water depth per texel is the still water level of the cell (CellLevels) minus the
//    terrain height (PBR terrain texture t112). Dry texels hold 0 and shallow water damps the waves
//    (depthMul), so ripples no longer propagate under beaches.
//  - Flow: both leapfrog levels (and the silt) are advected semi-Lagrangian, sampled at p - dt * velocity from the
//    worldspace flowmap, so wakes drift downstream on rivers.
//  - Non-finite results are zeroed, so one bad value can never spread through the whole grid.
//
// A channel carries silt: feet dragging over the bed in shallow water kick sediment up into a
// cloud that spreads slowly (diffusion) and settles out over tens of seconds. The water shaders add it
// to the turbidity near the bottom.
//
// Output: x height, y previous height, w silt.
// ============================================================================

struct RippleSource
{
	float2 Position;  // texel coordinates in the current grid
	float Radius;     // texels
	float Depth;      // target surface depression (simulation units, > 0 pushes down)
	float Silt;       // sediment kicked up per step at the centre of the footprint
	float3 Pad;
};

cbuffer RippleSimCB : register(b0)
{
	int2 Shift;  // texel offset of the new grid origin relative to the previous one
	uint NumSources;
	uint GridSize;
	float Damping;        // amplitude retained per step
	float WaveSpeed2;     // C^2
	float SiltDecay;      // silt retained per step (settling)
	float SiltDiffusion;  // fraction of the neighbour difference exchanged per step
	float Viscosity;      // nu
	float TexelSize;      // units
	float StepTime;       // s
	float Pad0;
	int2 OriginTexel;     // grid corner in absolute texels (the grid being written)
	int2 CellBase;        // absolute cell of CellLevels entry 0
	float4 Terrain0;      // xy terrain uv scale, zw offset (PBRWater::Terrain0)
	float4 Terrain1;      // x terrain z at texel value 0, y at 1, z enabled (water levels and terrain known), w unused
	float4 Flow0;         // worldspace flowmap uv = absXY * xz + yw (x = 0: no flowmap)
	float4 FlowParams;    // x current speed at full flow strength (units/s), yzw unused
	float4 CellLevels[7]; // still water level (absolute z) of the 5 x 5 cells around the eye, at i + 5 j; < -1e8: no water
};

Texture2D<float4> PreviousState : register(t0);
StructuredBuffer<RippleSource> Sources : register(t1);
Texture2D<float> TerrainTexture : register(t2);
Texture2D<float4> FlowMap : register(t3);
SamplerState LinearClampSampler : register(s0);
RWTexture2D<float4> CurrentState : register(u0);

static const float CellSize = 4096.0;  // units: one exterior cell

float4 LoadState(int2 p)
{
	if (any(p < 0) || any(p >= (int)GridSize))
		return 0.0;
	return PreviousState.Load(int3(p, 0));
}

/// isfinite() by its bits: the compiler assumes no infinities and may fold the intrinsic away (warning X3577).
bool IsFinite(float x)
{
	return (asuint(x) & 0x7F800000u) != 0x7F800000u;
}

/// Bilinear read of the previous state at fractional texel `q` (texel centres at integers).
float4 SampleState(float2 q)
{
	float2 base = floor(q);
	float2 f = q - base;
	int2 i = int2(base);
	return lerp(lerp(LoadState(i), LoadState(i + int2(1, 0)), f.x),
		lerp(LoadState(i + int2(0, 1)), LoadState(i + int2(1, 1)), f.x), f.y);
}

/// Water depth (units) at absolute `absXY`: the cell's still water level above the terrain. 1e6: deep or unknown.
float WaterDepth(float2 absXY)
{
	if (Terrain1.z < 0.5)
		return 1e6;
	int2 cell = int2(floor(absXY / CellSize)) - CellBase;
	if (any(cell < 0) || any(cell >= 5))
		return 1e6;
	int index = cell.x + 5 * cell.y;
	// A cell without cell water can still hold placed water (rivers, streams are often separate water meshes): unknown,
	// so deep. The level is also never taken below the water at the eye (entry 12, the eye's own cell, from the water
	// the eye is at): placed water above a cell's own level would otherwise read as dry land and lose its ripples.
	float level = CellLevels[index >> 2][index & 3];
	float eyeLevel = CellLevels[3].x;
	if (level < -1e8 && eyeLevel < -1e8)
		return 1e6;
	level = max(level, eyeLevel);

	float2 uv = absXY * Terrain0.xy + Terrain0.zw;
	if (any(uv <= 0.0) || any(uv >= 1.0))
		return 1e6;
	// Texel loads, like PBRWater::BilinearLoadGrad: the heights must not depend on the sampler slot's state.
	uint w, h;
	TerrainTexture.GetDimensions(w, h);
	float2 p = uv * float2(w, h) - 0.5;
	int2 i0 = int2(floor(p));
	float2 f = p - i0;
	int2 hi = int2(w, h) - 1;
	int2 a = clamp(i0, 0, hi);
	int2 b = clamp(i0 + 1, 0, hi);
	float t = lerp(lerp(TerrainTexture.Load(int3(a.x, a.y, 0)), TerrainTexture.Load(int3(b.x, a.y, 0)), f.x),
		lerp(TerrainTexture.Load(int3(a.x, b.y, 0)), TerrainTexture.Load(int3(b.x, b.y, 0)), f.x), f.y);
	return level - lerp(Terrain1.x, Terrain1.y, t);
}

/**
 * River current (units/s) at absolute `absXY` from Unified Water's worldspace flowmap, decoded like PBRWater::FlowVelocity
 * (the waves are carried by the same current): the water travels along -(rg * 2 - 1), at FlowParams.x at full strength b * a.
 */
float2 FlowVelocity(float2 absXY)
{
	if (Flow0.x == 0.0)
		return 0.0;
	float4 flow = FlowMap.SampleLevel(LinearClampSampler, absXY * Flow0.xz + Flow0.yw, 0);
	float2 direction = -(flow.xy * 2.0 - 1.0);
	float lengthSq = dot(direction, direction);
	direction = lengthSq > 1e-4 ? direction * rsqrt(lengthSq) : 0.0;
	return direction * (saturate(flow.z * flow.w) * FlowParams.x);
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= GridSize))
		return;

	int2 p = int2(id.xy) + Shift;
	float2 absXY = (float2(OriginTexel) + float2(id.xy) + 0.5) * TexelSize;

	// Dry texels (the bed above the still water) hold nothing: waves reflect off the shore instead of running under it.
	float depth = WaterDepth(absXY);
	if (depth <= 0.0) {
		CurrentState[id.xy] = 0.0;
		return;
	}

	// Semi-Lagrangian advection by the river current: every sample below is taken upstream, which carries both
	// leapfrog levels (and the silt) downstream. Texel centres sit at integer coordinates of the previous grid.
	float2 upstream = float2(p) - FlowVelocity(absXY) * (StepTime / TexelSize);
	float4 centre;
	float4 neighbours;
	[branch] if (any(upstream != float2(p)))
	{
		centre = SampleState(upstream);
		neighbours = SampleState(upstream + float2(1, 0)) + SampleState(upstream - float2(1, 0)) +
		             SampleState(upstream + float2(0, 1)) + SampleState(upstream - float2(0, 1));
	}
	else
	{
		centre = LoadState(p);
		neighbours = LoadState(p + int2(1, 0)) + LoadState(p - int2(1, 0)) + LoadState(p + int2(0, 1)) + LoadState(p - int2(0, 1));
	}
	float h = centre.x;
	float hPrev = centre.y;
	float lap = neighbours.x - 4.0 * h;
	float lapPrev = neighbours.y - 4.0 * hPrev;

	float hNext = (2.0 * h - hPrev + WaveSpeed2 * lap + Viscosity * (lap - lapPrev)) * Damping;
	float silt = (centre.w + SiltDiffusion * (neighbours.w * 0.25 - centre.w)) * SiltDecay;

	// Shallow water damps waves (Crest UpdateDynWaves): water deeper than half a wavelength is deep water and
	// leaves them alone, shallower ramps the damping up. The shortest wave the grid holds is two texels long.
	float wavelength = 2.0 * TexelSize;
	hNext *= 1.0 - (1.0 - saturate(2.0 * depth / wavelength)) * StepTime * 2.0;

	// Absorb waves at the border so nothing reflects off the edge of the simulated area.
	float2 border = min(float2(id.xy), float(GridSize - 1) - float2(id.xy));
	hNext *= saturate(min(border.x, border.y) / 8.0);

	float2 texel = float2(id.xy) + 0.5;
	[loop] for (uint i = 0; i < NumSources; i++)
	{
		RippleSource s = Sources[i];
		float2 d = texel - s.Position;
		float r2 = dot(d, d) / max(s.Radius * s.Radius, 1e-3);
		if (r2 > 9.0)
			continue;
		// Constraint: a moving body pushes the surface down to its depression, it never pulls it up.
		// Depth scales with the body's speed (CPU side), so a resting body excites nothing; silt-only
		// sources (feet on the bed) leave the surface alone.
		if (s.Depth > 0.0)
			hNext = lerp(hNext, min(hNext, -s.Depth), exp(-2.0 * r2));
		// Silt billows out a little wider than the foot itself.
		silt += s.Silt * exp(-r2);
	}

	// A non-finite value (Crest clears these too) would spread through the whole grid, so it is zeroed together with
	// the level it came from. This comes before the clamps: min / max return the non-NaN operand and would hide it.
	if (!IsFinite(hNext) || !IsFinite(h) || !IsFinite(silt)) {
		CurrentState[id.xy] = 0.0;
		return;
	}
	// Hard bound: a bad source can never feed back into a runaway.
	hNext = clamp(hNext, -2.0, 2.0);
	silt = clamp(silt, 0.0, 4.0);
	CurrentState[id.xy] = float4(hNext, h, 0.0, silt);
}
