// ============================================================================
// PBR Water - the surf simulation: every broken wave's own water through the inner surf and up the beach.
//
// A broken wave runs in as a bore, meets the backwash of the wave before it, collapses at the waterline and sends its
// water up the beach as a thin sheet, which slows on the slope and drains back (Shen & Meyer 1963; Hibberd & Peregrine
// 1979: the nonlinear shallow water equations carry all of it, the bore as their shock). None of that is a function of
// one wave, so it is simulated on a camera-centred grid.
//
// The scheme is Stelling & Duinmeijer's (2003), the one the coastal models SWASH and XBeach use for the swash: depths at
// the cell centres, velocities at the faces between them, the momentum advected in conservation form and the depth carried
// by the upwind cell's. It keeps the momentum balance across a bore (so bores travel at the right speed and stay two
// cells sharp), never makes a depth negative, leaves water at rest over any bed, and lets a thin sheet climb a slope on
// its own momentum against gravity alone. The Rusanov fluxes on the hydrostatic reconstruction this replaces smeared a
// bore over seven cells, and stopped a sheet shallower than the bed's rise from one cell to the next dead, as if against
// a wall: on a 1:10 beach it carried a dam-break uprush a third of the way up (the exact solution: Shen & Meyer).
//
// Two passes a step. The momentum pass moves the face velocities under the old depths; the continuity pass moves the
// depths (and the foam) with the new velocities, soaks the sheet into the sand, and makes and decays the foam.
//
// The waves come in through the hand-over (PBRWater::SurfSharesAt, laid out by ShoreFoamCS): there the simulation takes
// the train's incoming Riemann invariant, u + 2 sqrt(g h) along the waves' travel, and keeps its own outgoing one,
// u - 2 sqrt(g h), so what the beach sends back leaves freely (the characteristic boundary of the swash's theory; in
// the bulk, Van Dongeren & Svendsen 1997). Relaxed to the train's whole state instead, the hand-over was a wall to
// everything going out: backwash piled up against it and each released bore was drained again behind its crest.
// Further out, where the train alone is the water, the outgoing invariant is given up too and the state is the train's.
//
// State: x depth (m), y velocity through the east face, z through the north face (m/s), w foam per area (0..1).
// ============================================================================

#include "PBRWater/PBRWater.hlsli"

cbuffer SwashStepCB : register(b0)
{
	int2 Shift;  // texels the grid scrolled since the last step (the first momentum pass of a frame; 0 otherwise)
	uint GridSize;
	float Dt;         // s, this step
	float2 Origin;    // absolute position of the grid corner (units)
	float TexelSize;  // units
	float Pad0;
	float4 Pad1;
};

Texture2D<float4> Source : register(t0);  // momentum pass: the state; continuity pass: the momentum pass's output
Texture2D<float4> Prep : register(t1);    // ShoreFoamCS: x bed (m above the still water), y incoming share, z outgoing share, w the train's level (m)
Texture2D<float4> Stage : register(t2);   // ShoreFoamCS: yz the waves' direction of travel, w the foam they carry
RWTexture2D<float4> Target : register(u0);

static const float G = PBRWater::GravityMetres;
/// A cell shallower than this (m) sends no water on.
static const float DryDepth = 1e-4;
/// Manning's roughness of the bed (s/m^1/3): sand, 0.02 (the range the swash models use). Its drag grows as the sheet
/// thins, which is what stops the last of the backwash. Swash Linger multiplies the drag.
static const float Manning = 0.02;
/// How fast the sand above the still water soaks up the sheet over it (m/s): the hydraulic conductivity of a medium to
/// coarse beach sand. It takes the last millimetres the drag has stopped.
static const float Infiltration = 0.001;
/// Foam stranded on the sand drains into it within a few seconds (s): long enough for the swash mark to show where the
/// uprush reached.
static const float StrandedLifetime = 2.5;
/// The dense foam of a bore's roller thins to what lingers on the water (Tune3.w) within about this long (s).
static const float RollerLifetime = 0.4;
/// How quickly the simulation takes the train's waves in the hand-over (s).
static const float HandOverTime = 0.08;
/// No ground: a wall (ShoreFoamCS).
static const float Wall = 100.0;

int2 Clamped(int2 c)
{
	return clamp(c, 0, (int)GridSize - 1);
}

float4 LoadPrep(int2 c)
{
	return Prep.Load(int3(Clamped(c), 0));
}

/// The water in cell `c` before the pass. Where the grid has just scrolled in (the momentum pass), at rest over the bed.
/// Outside the grid, the edge cell's.
float4 LoadState(int2 c)
{
	c = Clamped(c);
	int2 q = c + Shift;
	float4 state = float4(max(-Prep.Load(int3(c, 0)).x, 0.0), 0.0, 0.0, 0.0);
	[branch] if (all(q >= 0) && all(q < (int)GridSize))
		state = Source.Load(int3(q, 0));
	return state;
}

/**
 * The hand-over, in the Riemann invariants along the waves' travel: the incoming one, u + 2c, is drawn to the train's, the
 * outgoing one, u - 2c, kept (drawn to the still water's only further out). The train's water moves as a simple wave
 * running shorewards over still water, u = 2 (sqrt(g h) - sqrt(g d)): any other velocity splits each wave in two, one
 * half running back out to sea.
 * @param along    the water's velocity along the waves' travel (m/s)
 * @param depth    its depth (m)
 * @param bed      the bed (m above the still water)
 * @param level    the train's surface (m above the still water)
 * @param incoming, outgoing the shares (Prep.yz)
 * @return x the velocity along the travel, y the depth
 */
float2 HandOver(float along, float depth, float bed, float level, float incoming, float outgoing)
{
	float still = max(-bed, 0.0);
	float trainDepth = max(still + level, 0.0);
	float trainSpeed = sqrt(G * trainDepth);
	float trainAlong = 2.0 * (trainSpeed - sqrt(G * still));
	float speed = sqrt(G * max(depth, 0.0));
	float rate = 1.0 - exp(-Dt / HandOverTime);
	// Water running out faster than its own waves takes nothing from the sea: both invariants leave with it.
	float subcritical = 1.0 - smoothstep(0.6, 1.0, -along / max(speed, 1e-3));
	float plus = lerp(along + 2.0 * speed, trainAlong + 2.0 * trainSpeed, incoming * subcritical * rate);
	float minus = lerp(along - 2.0 * speed, trainAlong - 2.0 * trainSpeed, outgoing * rate);
	float newSpeed = max(0.25 * (plus - minus), 0.0);
	return float2(0.5 * (plus + minus), newSpeed * newSpeed / G);
}

/// The train's own water at a cell: x its depth (m), y its velocity along the travel (m/s).
float2 TrainState(float bed, float level)
{
	float still = max(-bed, 0.0);
	float depth = max(still + level, 0.0);
	return float2(depth, 2.0 * (sqrt(G * depth) - sqrt(G * still)));
}

#if !defined(SWASH_CONTINUITY)

/**
 * The velocity through the face between cell C and the next cell N along an axis, after the step.
 * @param uP           the face's velocity now
 * @param uBack, uFore the velocities through the faces behind and ahead of it on the same axis
 * @param hBack, hC, hN, hFore the depths of the four cells along the axis (behind C, C, N, ahead of N)
 * @param etaC, etaN   the levels of the two cells
 * @param across       the velocity across the axis at the face
 * @param uLeft, uRight the same component at the faces either side of this one, across the axis (left: towards negative)
 */
float FaceVelocity(float uP, float uBack, float uFore, float hBack, float hC, float hN, float hFore, float etaC, float etaN, float across, float uLeft, float uRight, float dx)
{
	// The discharge through each face: the velocity times the depth of the cell the water comes from.
	float qP = uP * (uP > 0.0 ? hC : hN);
	float qBack = uBack * (uBack > 0.0 ? hBack : hC);
	float qFore = uFore * (uFore > 0.0 ? hN : hFore);
	// Momentum carried through the two cells either side of the face (Stelling & Duinmeijer eq. 24).
	float qC = 0.5 * (qBack + qP);
	float qN = 0.5 * (qP + qFore);
	float uC = qC >= 0.0 ? uBack : uP;
	float uN = qN >= 0.0 ? uP : uFore;
	float depth = max(0.5 * (hC + hN), DryDepth);
	float advection = ((qN * uN - qC * uC) - uP * (qN - qC)) / (dx * depth);
	float crosswise = across * (across > 0.0 ? uP - uLeft : uRight - uP) / dx;
	float u = uP - Dt * (advection + crosswise + G * (etaN - etaC) / dx);
	// No water comes out of a dry cell: over a bed above the level beside it, the face stays shut.
	if ((u > 0.0 && hC <= DryDepth) || (u < 0.0 && hN <= DryDepth))
		u = 0.0;
	// The bed's drag (Manning), implicit: it stops a thin sheet, it never turns it round.
	float wet = max(u > 0.0 ? hC : hN, 0.005);
	float drag = G * Manning * Manning * max(PBRWater::Shore3.x, 1.0) / pow(wet, 4.0 / 3.0);
	return u / (1.0 + Dt * drag * length(float2(u, across)));
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= GridSize))
		return;
	int2 c = int2(id.xy);
	float4 prep = LoadPrep(c);
	float4 s = LoadState(c);
	if (prep.x > Wall) {
		Target[id.xy] = 0.0;
		return;
	}
	float4 prepE = LoadPrep(c + int2(1, 0));
	float4 prepN = LoadPrep(c + int2(0, 1));
	float3 stage = Stage.Load(int3(c, 0)).yzw;
	float2 dir = stage.xy;

	// Where the train alone is the water, the state is the train's.
	[branch] if (min(prep.z, min(prepE.z, prepN.z)) > 0.999)
	{
		Target[id.xy] = float4(s.x, dir * TrainState(prep.x, prep.w).y, s.w);
		return;
	}

	float4 sE = LoadState(c + int2(1, 0));
	float4 sN = LoadState(c + int2(0, 1));
	// Dry here and beyond both faces: nothing moves. Most of the grid's beach is this.
	[branch] if (max(s.x, max(sE.x, sN.x)) <= DryDepth)
	{
		Target[id.xy] = float4(s.x, 0.0, 0.0, s.w);
		return;
	}

	float4 sW = LoadState(c - int2(1, 0));
	float4 sS = LoadState(c - int2(0, 1));
	float4 sEE = LoadState(c + int2(2, 0));
	float4 sNN = LoadState(c + int2(0, 2));
	float4 sSE = LoadState(c + int2(1, -1));
	float4 sNW = LoadState(c + int2(-1, 1));

	float dx = TexelSize * PBRWater::MetresPerUnit;
	float eta = s.x + prep.x;
	float vAtEast = 0.25 * (s.z + sE.z + sS.z + sSE.z);
	float uAtNorth = 0.25 * (s.y + sN.y + sW.y + sNW.y);
	float u = FaceVelocity(s.y, sW.y, sE.y, sW.x, s.x, sE.x, sEE.x, eta, sE.x + prepE.x, vAtEast, sS.y, sN.y, dx);
	float v = FaceVelocity(s.z, sS.z, sN.z, sS.x, s.x, sN.x, sNN.x, eta, sN.x + prepN.x, uAtNorth, sW.z, sE.z, dx);

	// The hand-over at each face: the water's velocity along the waves' travel takes the train's incoming invariant.
	float2 sharesE = 0.5 * (prep.yz + prepE.yz);
	[branch] if (sharesE.x + sharesE.y > 0.0 && prepE.x < Wall)
	{
		float along = u * dir.x + vAtEast * dir.y;
		u += dir.x * (HandOver(along, 0.5 * (s.x + sE.x), 0.5 * (prep.x + prepE.x), 0.5 * (prep.w + prepE.w), sharesE.x, sharesE.y).x - along);
	}
	float2 sharesN = 0.5 * (prep.yz + prepN.yz);
	[branch] if (sharesN.x + sharesN.y > 0.0 && prepN.x < Wall)
	{
		float along = uAtNorth * dir.x + v * dir.y;
		v += dir.y * (HandOver(along, 0.5 * (s.x + sN.x), 0.5 * (prep.x + prepN.x), 0.5 * (prep.w + prepN.w), sharesN.x, sharesN.y).x - along);
	}

	// Within a quarter of a cell a step: then a cell cannot send out more water than it holds, whatever its four faces do.
	float fastest = min(9.0, 0.24 * dx / max(Dt, 1e-5));
	float2 velocity = clamp(float2(u, v), -fastest, fastest);
	if (!all(abs(velocity) < 1e30))
		velocity = 0.0;
	Target[id.xy] = float4(s.x, velocity, s.w);
}

#else

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= GridSize))
		return;
	int2 c = int2(id.xy);
	float4 prep = LoadPrep(c);
	float4 s = Source.Load(int3(c, 0));
	float bed = prep.x;
	if (bed > Wall) {
		Target[id.xy] = 0.0;
		return;
	}
	float3 stage = Stage.Load(int3(c, 0)).yzw;

	// Where the train alone is the water, the state is the train's: its level, and the white water it carries.
	[branch] if (prep.z > 0.999)
	{
		Target[id.xy] = float4(TrainState(bed, prep.w).x, s.yz, stage.z);
		return;
	}

	float4 sW = Source.Load(int3(Clamped(c - int2(1, 0)), 0));
	float4 sS = Source.Load(int3(Clamped(c - int2(0, 1)), 0));
	float4 sE = Source.Load(int3(Clamped(c + int2(1, 0)), 0));
	float4 sN = Source.Load(int3(Clamped(c + int2(0, 1)), 0));

	// Dry ground with dry neighbours and no foam left on it: nothing flows in or out.
	[branch] if (bed >= 0.0 && s.w < 0.01 && max(max(s.x, sE.x), max(max(sW.x, sN.x), sS.x)) < 1e-5)
	{
		Target[id.xy] = 0.0;
		return;
	}

	float dx = TexelSize * PBRWater::MetresPerUnit;
	// The discharge through each face: the new velocity times the depth of the cell the water comes from. The foam rides
	// the water out of the cell it leaves (its share per depth there).
	float qE = s.y * (s.y > 0.0 ? s.x : sE.x);
	float qW = sW.y * (sW.y > 0.0 ? sW.x : s.x);
	float qN = s.z * (s.z > 0.0 ? s.x : sN.x);
	float qS = sS.z * (sS.z > 0.0 ? sS.x : s.x);
	float fE = qE * (s.y > 0.0 ? s.w / max(s.x, 0.002) : sE.w / max(sE.x, 0.002));
	float fW = qW * (sW.y > 0.0 ? sW.w / max(sW.x, 0.002) : s.w / max(s.x, 0.002));
	float fN = qN * (s.z > 0.0 ? s.w / max(s.x, 0.002) : sN.w / max(sN.x, 0.002));
	float fS = qS * (sS.z > 0.0 ? sS.w / max(sS.x, 0.002) : s.w / max(s.x, 0.002));
	float depth = max(s.x - (Dt / dx) * (qE - qW + qN - qS), 0.0);
	float foam = max(s.w - (Dt / dx) * (fE - fW + fN - fS), 0.0);

	// A bore makes foam: where the water runs together hard across a step in its depth, it is breaking (the shock of the
	// shallow water equations is the bore's roller, where its energy goes). The step gates it: the edge of a thin sheet
	// and the last of the backwash run together too, with no bore to speak of, and drew white lines along the water's
	// edge when they counted.
	float converging = -(s.y - sW.y + s.z - sS.z);
	// The step is in the water's level between wet cells: the depth alone steps with every sloping bed, and a dry cell's
	// level is its ground (both drew a white line along every waterline).
	float level = s.x + bed;
	float levelE = sE.x > 0.01 ? sE.x + LoadPrep(c + int2(1, 0)).x : level;
	float levelW = sW.x > 0.01 ? sW.x + LoadPrep(c - int2(1, 0)).x : level;
	float levelN = sN.x > 0.01 ? sN.x + LoadPrep(c + int2(0, 1)).x : level;
	float levelS = sS.x > 0.01 ? sS.x + LoadPrep(c - int2(0, 1)).x : level;
	float jump = s.x > 0.01 ? max(abs(levelE - levelW), abs(levelN - levelS)) : 0.0;
	float breaking = saturate(4.0 * converging / max(sqrt(G * depth), 0.3) - 0.2) * smoothstep(0.02, 0.08, jump);
	foam = max(foam, breaking);

	// The sand soaks the sheet up where it lies above the still water (below it, the ground is saturated by the sea).
	if (bed > 0.0)
		depth = max(depth - Infiltration * Dt, 0.0);

	// The foam decays: the roller's dense foam thins quickly to what lingers, whose bubbles burst slowly on the water;
	// the sand drains what is stranded on it.
	float lingering = min(foam, PBRWater::Tune3.w);
	foam = lingering * exp(-Dt / (depth < 0.003 ? StrandedLifetime : max(PBRWater::Tune5.w, 0.1))) + (foam - lingering) * exp(-Dt / RollerLifetime);

	// The hand-over: the depth takes the train's incoming invariant with the velocity (the momentum pass), and the foam
	// the white water the train's waves carry.
	[branch] if (bed < 0.0 && prep.y + prep.z > 0.0)
	{
		float along = 0.5 * ((s.y + sW.y) * stage.x + (s.z + sS.z) * stage.y);
		depth = HandOver(along, depth, bed, prep.w, prep.y, prep.z).y;
		foam = max(foam, stage.z * prep.y);
	}

	// A non-finite value would spread over the grid: the cell at rest.
	float4 next = float4(depth, s.yz, min(foam, 1.0));
	if (!all(abs(next) < 1e30))
		next = float4(max(-bed, 0.0), 0.0, 0.0, 0.0);
	Target[id.xy] = next;
}

#endif
