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
// hand-authored ripple shapes. A third channel accumulates foam on the ripple crests, where the
// raised water spills over, so foam rides the rings and wakes instead of pooling under the body.
//
// The grid scrolls with the camera in whole texels; `Shift` re-addresses the previous state so the
// simulation stays fixed in world space.
//
// A fourth channel carries silt: feet dragging over the bed in shallow water kick sediment up into a
// cloud that spreads slowly (diffusion) and settles out over tens of seconds. The water shaders add it
// to the turbidity near the bottom.
//
// Output: x height, y previous height, z foam, w silt.
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
	float Damping;     // amplitude retained per step
	float FoamDecay;   // foam retained per step
	float WaveSpeed2;  // C^2
	float FoamFromMotion;
	float SiltDecay;      // silt retained per step (settling)
	float SiltDiffusion;  // fraction of the neighbour difference exchanged per step
	float Viscosity;      // nu
	float Pad;
};

Texture2D<float4> PreviousState : register(t0);
StructuredBuffer<RippleSource> Sources : register(t1);
RWTexture2D<float4> CurrentState : register(u0);

float4 LoadState(int2 p)
{
	if (any(p < 0) || any(p >= (int)GridSize))
		return 0.0;
	return PreviousState.Load(int3(p, 0));
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= GridSize))
		return;

	int2 p = int2(id.xy) + Shift;
	float4 centre = LoadState(p);
	float h = centre.x;
	float hPrev = centre.y;

	float4 neighbours = LoadState(p + int2(1, 0)) + LoadState(p - int2(1, 0)) +
	                    LoadState(p + int2(0, 1)) + LoadState(p - int2(0, 1));
	float lap = neighbours.x - 4.0 * h;
	float lapPrev = neighbours.y - 4.0 * hPrev;

	float hNext = (2.0 * h - hPrev + WaveSpeed2 * lap + Viscosity * (lap - lapPrev)) * Damping;
	float foam = centre.z * FoamDecay;
	float silt = (centre.w + SiltDiffusion * (neighbours.w * 0.25 - centre.w)) * SiltDecay;

	// Absorb waves at the border so nothing reflects off the edge of the simulated area.
	float2 border = min(float2(id.xy), float(GridSize - 1) - float2(id.xy));
	hNext *= saturate(min(border.x, border.y) / 8.0);

	float2 texel = float2(id.xy) + 0.5;
	[loop] for (uint i = 0; i < NumSources; i++)
	{
		RippleSource s = Sources[i];
		float2 d = texel - s.Position;
		float r2 = dot(d, d) / max(s.Radius * s.Radius, 1e-3);
		if (r2 > 4.0)
			continue;
		// Constraint: a moving body pushes the surface down to its depression, it never pulls it up.
		// Depth scales with the body's speed (CPU side), so a resting body excites nothing; silt-only
		// sources (feet on the bed) leave the surface alone.
		if (s.Depth > 0.0)
			hNext = lerp(hNext, min(hNext, -s.Depth), exp(-2.0 * r2));
		// Silt billows out a little wider than the foot itself.
		silt += s.Silt * exp(-r2);
	}

	// Foam only where the surface is pushed up into a crest and still rising or breaking over.
	// Hard bound: a bad source can never feed back into a runaway.
	hNext = clamp(hNext, -2.0, 2.0);

	float crest = saturate((hNext - 0.08) * 4.0);
	foam += crest * saturate(abs(hNext - h) * FoamFromMotion);
	CurrentState[id.xy] = float4(hNext, h, saturate(foam), clamp(silt, 0.0, 4.0));
}
