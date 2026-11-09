// ============================================================================
// PBR Water - the swash: every breaking wave's own water running up the beach.
//
// After a bore reaches the waterline its water runs up the beach as a thin sheet, slows on the slope and drains back
// (Shen & Meyer 1963). Each uprush is water of its own, travelling its own way: it overtakes or meets the backwash of
// the one before (a hydraulic jump, white water), runs into the uprushes beside it where crests reached the shore at
// other times or angles, and leaves its foam to thin out and drain as the sheet spreads and soaks away. None of that is
// a function of one wave, so it is simulated: the shallow water equations with wetting and drying, on the foam's
// camera-centred grid.
//
// Finite volumes: Rusanov fluxes on the hydrostatic reconstruction of Audusse et al. (2004), which keeps water at rest
// over any bed at rest, never makes a depth negative and lets the water's edge run over dry ground. Bed friction is
// implicit (it drags the thin sheet to a stop instead of reversing it), with the drag coefficient set by Swash Linger
// (Shore3.x). The surf hands the water over: where ShoreFoamCS marks the analytic waves as the water (Prep.y), the state
// is relaxed to their level, velocity and foam, so each bore enters the simulation as itself. The foam is a tracer per
// area carried with the water (upwind), decaying as on the water, quickly where it is stranded on the sand, and made
// where the water converges hard: uprushes and backwash colliding.
//
// State: x depth (m), yz discharge h u (m^2/s), w foam per area (0..1).
// ============================================================================

#include "PBRWater/PBRWater.hlsli"

cbuffer SwashStepCB : register(b0)
{
	int2 Shift;  // texels the grid scrolled since the last step (the first step of a frame)
	uint GridSize;
	float Dt;         // s, this step
	float2 Origin;    // absolute position of the grid corner (units)
	float TexelSize;  // units
	float Pad0;
	float4 Pad1;
};

Texture2D<float4> PreviousState : register(t0);
Texture2D<float4> Prep : register(t1);   // ShoreFoamCS: x bed (m), y forcing 0..1, zw the waves' water velocity (m/s)
Texture2D<float4> Stage : register(t2);  // ShoreFoamCS: y the waves' level (m), z the bores' foam
RWTexture2D<float4> State : register(u0);

static const float G = PBRWater::GravityMetres;
/// Shallower than this (m) the water has no velocity of its own.
static const float DryDepth = 0.001;
/// How fast the sand above the still water soaks up the sheet over it (m/s). Without it, friction stopped the last
/// millimetres on flat ground and they stayed there: the water's edge stood still on the beach long after the backwash.
static const float Infiltration = 0.003;
/// Foam stranded on dry sand drains into it within a few seconds (s): long enough for the swash mark to show where the
/// uprush reached and thin out as the sand soaks it up.
static const float StrandedLifetime = 2.5;
/// How quickly the surf hands its water to the simulation (s).
static const float HandOver = 0.05;

int2 Clamped(int2 c)
{
	return clamp(c, 0, (int)GridSize - 1);
}

float4 LoadPrep(int2 c)
{
	return Prep.Load(int3(Clamped(c), 0));
}

/// The water in cell `c` (this step's grid) before the step: last step's, or at rest over the bed where the grid has
/// just scrolled in. Outside the grid, the edge cell's (water flows out freely).
float4 LoadState(int2 c)
{
	c = Clamped(c);
	int2 q = c + Shift;
	float4 state = float4(max(-Prep.Load(int3(c, 0)).x, 0.0), 0.0, 0.0, 0.0);
	[branch] if (all(q >= 0) && all(q < (int)GridSize))
		state = PreviousState.Load(int3(q, 0));
	return state;
}

float2 Velocity(float4 s)
{
	if (s.x < DryDepth)
		return 0.0;
	float2 v = s.yz / s.x;
	float speed = length(v);
	return speed > 9.0 ? v * (9.0 / speed) : v;
}

/**
 * Flux across the face from cell L (bed zL) to cell R (bed zR) along x: depth, x and y discharge, foam. hL and hR are the
 * depths reconstructed at the face (hydrostatic: over the higher of the two beds).
 */
float4 Flux(float4 L, float zL, float4 R, float zR, out float hL, out float hR)
{
	float z = max(zL, zR);
	hL = max(L.x + zL - z, 0.0);
	hR = max(R.x + zR - z, 0.0);
	float2 vL = Velocity(L);
	float2 vR = Velocity(R);
	float a = max(abs(vL.x) + sqrt(G * hL), abs(vR.x) + sqrt(G * hR));
	float3 qL = float3(hL, hL * vL);
	float3 qR = float3(hR, hR * vR);
	float3 fL = float3(hL * vL.x, hL * vL.x * vL.x + 0.5 * G * hL * hL, hL * vL.x * vL.y);
	float3 fR = float3(hR * vR.x, hR * vR.x * vR.x + 0.5 * G * hR * hR, hR * vR.x * vR.y);
	float3 f = 0.5 * (fL + fR) - 0.5 * a * (qR - qL);
	// The foam rides the water out of the cell it leaves (its share per depth there).
	float foam = f.x > 0.0 ? f.x * L.w / max(L.x, 0.002) : f.x * R.w / max(R.x, 0.002);
	return float4(f, foam);
}

/// The same across a face along y: the state's discharges swapped in and out.
float4 FluxY(float4 L, float zL, float4 R, float zR, out float hL, out float hR)
{
	float4 f = Flux(L.xzyw, zL, R.xzyw, zR, hL, hR);
	return f.xzyw;
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= GridSize))
		return;
	int2 c = int2(id.xy);
	float4 prep = LoadPrep(c);
	float z = prep.x;
	float4 u = LoadState(c);

	float4 east = LoadState(c + int2(1, 0)), west = LoadState(c - int2(1, 0));
	float4 north = LoadState(c + int2(0, 1)), south = LoadState(c - int2(0, 1));
	float zE = LoadPrep(c + int2(1, 0)).x, zW = LoadPrep(c - int2(1, 0)).x;
	float zN = LoadPrep(c + int2(0, 1)).x, zS = LoadPrep(c - int2(0, 1)).x;

	// Dry ground above the still water with dry neighbours, no hand-over and no foam left on it: nothing flows in or out. Most of the
	// grid's beach is this, and the grid is four times as large as it was.
	[branch] if (z >= 0.0 && prep.y <= 0.0 && u.w < 0.01 && max(max(u.x, east.x), max(max(west.x, north.x), south.x)) < 1e-5)
	{
		State[id.xy] = 0.0;
		return;
	}

	float dx = TexelSize * PBRWater::MetresPerUnit;
	float dt = Dt;
	float hl, hr;
	// Each face's flux, with the cell's own pressure at its reconstructed depth (Audusse): water at rest stays at rest.
	float4 fE = Flux(u, z, east, zE, hl, hr);
	fE.y += 0.5 * G * (u.x * u.x - hl * hl);
	float4 fW = Flux(west, zW, u, z, hl, hr);
	fW.y += 0.5 * G * (u.x * u.x - hr * hr);
	float4 fN = FluxY(u, z, north, zN, hl, hr);
	fN.z += 0.5 * G * (u.x * u.x - hl * hl);
	float4 fS = FluxY(south, zS, u, z, hl, hr);
	fS.z += 0.5 * G * (u.x * u.x - hr * hr);

	float4 next = u - (dt / dx) * (fE - fW + fN - fS);
	next.x = max(next.x, 0.0);
	next.w = max(next.w, 0.0);

	// Bed friction, implicit: the drag stops the thin sheet, it never turns it round.
	float cf = 0.006 * max(PBRWater::Shore3.x, 1.0);
	float2 v = Velocity(next);
	next.yz /= 1.0 + dt * cf * length(v) / max(next.x, 0.005);
	// The sand soaks it up where it lies above the still water (below it, the ground is saturated by the sea). The
	// discharge goes with the water, so what remains keeps its velocity.
	[branch] if (z > 0.0 && next.x > 0.0)
	{
		float soaked = max(next.x - Infiltration * dt, 0.0);
		next.yz *= soaked / next.x;
		next.x = soaked;
	}
	if (next.x < DryDepth)
		next.yz = 0.0;

	// White water where the water converges hard: an uprush running into the backwash or into another uprush.
	float wet = smoothstep(DryDepth, 0.01, next.x);
	float convergence = -((Velocity(east).x - Velocity(west).x) + (Velocity(north).y - Velocity(south).y)) / (2.0 * dx);
	next.w = max(next.w, wet * saturate(0.25 * convergence - 0.5));

	// White water piles up at the uprush's front: where the depth falls away ahead of fast water (a blunt wedge running
	// over the sand, its leading edge tumbling), and it is left behind as the front moves on, the swash mark.
	float2 frontVelocity = Velocity(next);
	float frontSpeed = length(frontVelocity);
	float2 depthGradient = float2(east.x - west.x, north.x - south.x) / (2.0 * dx);
	float fallsAhead = frontSpeed > 0.3 ? -dot(frontVelocity / frontSpeed, depthGradient) : 0.0;
	next.w = max(next.w, smoothstep(0.01, 0.03, next.x) * saturate(fallsAhead / 0.15) * saturate((frontSpeed - 0.8) / 2.0) * 0.9);

	// The foam decays: its bubbles burst on the water, the sand drains it.
	next.w *= exp(-dt / (next.x < 0.003 ? StrandedLifetime : max(PBRWater::Tune5.w, 0.1)));

	// The surf hands its water over: relaxed to the waves' level, velocity and foam.
	float forcing = prep.y * (1.0 - exp(-dt / HandOver));
	[branch] if (forcing > 0.0)
	{
		float4 stage = Stage.Load(int3(c, 0));
		float h = max(stage.y - z, 0.0);
		float4 target = float4(h, h * prep.zw, max(next.w, stage.z));
		next = lerp(next, target, forcing);
	}

	// A non-finite value would spread over the grid: the cell at rest.
	if (!all(abs(next) < 1e30))
		next = float4(max(-z, 0.0), 0.0, 0.0, 0.0);
	next.w = min(next.w, 1.0);
	State[id.xy] = next;
}
