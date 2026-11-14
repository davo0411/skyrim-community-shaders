// ============================================================================
// PBR Water - the foam the surf leaves on the water, and what the waves bring the surf simulation.
//
// Foam in the surf is a tracer on the water's surface: the breakers make it (the overturning crest and the bore's roller,
// where the shore profile carries white water), the water carries it, and it decays as its bubbles burst (coastal
// surfactants make surf foam outlast the bubble plume under it). So a breaker leaves a lace of foam behind it on the
// water as it flattens.
//
// A camera-centred grid in the water's rest coordinates, like the ripples: the surface the shaders draw is the rest
// position plus the waves' displacement, so the foam is carried by the water's velocity less the displacement's own
// rate. Under the shore waves the water moves with them at u = c eta / h (shallow water: the bores carry their water
// forward, the troughs pull it back). Semi-Lagrangian advection, as Crest 4 (wave-harmonic/crest, MIT) moves its foam.
// Inshore of the hand-over the surf simulation carries its own foam with its water (SwashStepCS); this one is drawn
// where the train is the water.
//
// The water shader draws the foam's pattern still, where the surface is: the foam moves over it. Every way of carrying the
// pattern along tore it: with the water sloshing under the waves, with the breakers next to still lace, and as a texture
// offset carried per texel.
//
// It also lays out, for the surf simulation (SwashStepCS), what the shore waves bring it this frame: the bed, how far the
// simulation takes the train's waves here and gives up its own (PBRWater::SurfSharesAt), the train's level, the
// direction its waves travel in and the white water they carry.
//
// Output: Stage (x foam coverage, yz the waves' direction of travel, w the white water they carry) and Prep (x bed (m
// above the still water), y incoming share, z outgoing share, w the train's level (m above the still water)).
// SwashOutCS copies the foam on.
// ============================================================================

#include "PBRWater/PBRWater.hlsli"

cbuffer ShoreFoamCB : register(b0)
{
	int2 Shift;  // texels the grid scrolled since the last update
	uint GridSize;
	float Dt;         // s (0 while paused)
	float2 Origin;    // absolute position of the grid corner (units)
	float TexelSize;  // units
	float Pad0;
};

Texture2D<float4> PreviousFoam : register(t0);
RWTexture2D<float4> Foam : register(u0);
RWTexture2D<float4> Prep : register(u1);

/// Deepest water the surf simulation holds (m): deeper, the train alone is the water in any sea, and the time step
/// shrinks with the depth.
static const float SwashDepthCap = 6.0;
/// No ground: a wall (m).
static const float SwashWall = 1000.0;

float4 LoadFoam(int2 p)
{
	if (any(p < 0) || any(p >= (int)GridSize))
		return 0.0;
	return PreviousFoam.Load(int3(p, 0));
}

/// Catmull-Rom weights of the four taps around fraction `f`.
float4 CatmullRom(float f)
{
	float f2 = f * f;
	float f3 = f2 * f;
	return float4(-0.5 * f3 + f2 - 0.5 * f, 1.5 * f3 - 2.5 * f2 + 1.0, -1.5 * f3 + 2.0 * f2 + 0.5 * f, 0.5 * f3 - 0.5 * f2);
}

/**
 * Last frame's foam at fractional texel `q` (texel centres at integers), Catmull-Rom, held within the four texels around
 * q. A bilinear read every frame blurred the foam: resampled 60 times a second over its lifetime it spread into smears.
 * The clamp keeps the cubic from ringing (Selle et al. 2008 limit their MacCormack step the same way).
 */
float SampleFoam(float2 q)
{
	float2 base = floor(q);
	float2 f = q - base;
	int2 i = int2(base);
	float4 wx = CatmullRom(f.x);
	float4 wy = CatmullRom(f.y);
	float result = 0.0;
	[unroll] for (int y = 0; y < 4; y++)
	{
		float row = 0.0;
		[unroll] for (int x = 0; x < 4; x++)
			row += wx[x] * LoadFoam(i + int2(x - 1, y - 1)).x;
		result += wy[y] * row;
	}
	float a = LoadFoam(i).x, b = LoadFoam(i + int2(1, 0)).x, c = LoadFoam(i + int2(0, 1)).x, d = LoadFoam(i + int2(1, 1)).x;
	return clamp(result, min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= GridSize))
		return;
	int2 p = int2(id.xy) + Shift;
	float2 absXY = Origin + (float2(id.xy) + 0.5) * TexelSize;

	float level = PBRWater::ShoreLevel(absXY);
	if (level < -1e6) {
		Foam[id.xy] = 0.0;
		Prep[id.xy] = float4(SwashWall, 0.0, 0.0, 0.0);
		return;
	}
	float3 positionWS = float3(absXY, level) - FrameBuffer::CameraPosAdjust.xyz;
	PBRWater::WaveContext ctx = PBRWater::BuildWaveContext(positionWS, PBRWater::DisplacementFade(positionWS));
	float bed = max(ctx.elevation * PBRWater::MetresPerUnit, -SwashDepthCap);
	float2 dir = ctx.shore.direction;
	if (ctx.train <= 0.0 && ctx.beach <= 0.0) {
		// Still water, or ground no wave reaches: the water is held at rest.
		float held = bed < -0.05 ? 1.0 : 0.0;
		Foam[id.xy] = float4(0.0, dir, 0.0);
		Prep[id.xy] = float4(bed, held, held, 0.0);
		return;
	}

	float dt = max(Dt, 0.0);
	PBRWater::SurfShares shares = PBRWater::SurfSharesAt(ctx);
	float2 velocity = 0.0;  // units/s, in the rest frame
	float source = 0.0;
	float trainLevel = 0.0;
	float trainFoam = 0.0;
	[branch] if (ctx.train > 0.0)
	{
		// The shore waves here and as they were last frame: their white water is the source, their displacement's rate
		// is the part of the water's motion the drawn surface already carries.
		PBRWater::ShoreTrain train = PBRWater::ShoreWaves(positionWS, ctx, TexelSize, dt > 0.0, true);
		source = train.foam;
		// What the surf simulation takes: the train's level where its water rests (a height field: the crest's forward
		// lean is left out, and a breaker's lip, which is drawn over the simulation's water), and the white water its
		// breakers carry (the profile's line of it: with the whole churned roller, the inner surf stayed white all over).
		trainLevel = train.displacement.z * PBRWater::MetresPerUnit;
		trainFoam = train.foam;

		// Shallow water: the water under a wave moves with it at u = c eta / (h + eta) along the travel.
		float depth = max(ctx.depth, 0.05 * PBRWater::UnitsPerMetre);
		float celerity = sqrt(PBRWater::GravityMetres * depth * PBRWater::MetresPerUnit) * PBRWater::UnitsPerMetre;
		float eta = train.displacement.z;
		velocity = dir * clamp(celerity * eta / max(depth + eta, 0.05 * PBRWater::UnitsPerMetre), -celerity, celerity);
		if (dt > 0.0)
			velocity -= (train.displacement.xy - train.previousDisplacement.xy) / dt;
	}

	// Each wave comes in a little turned of its own (its crests bend with the bed and the swell's spread), so neighbouring
	// uprushes cross on the beach.
	float2 coast = PBRWater::CoastPoint(absXY, ctx.shore);
	float cycles = PBRWater::ShorePhase(ctx.shore, coast, PBRWater::Shore2.x) / Math::TAU;
	float turn = 0.35 * (frac(sin(floor(cycles) * 12.9898 + dot(floor(coast / 2048.0), float2(4.1, 7.3))) * 43758.5453) - 0.5);
	float2 travel = float2(dir.x * cos(turn) - dir.y * sin(turn), dir.x * sin(turn) + dir.y * cos(turn));

	// Semi-Lagrangian: the water here came from upstream, and its foam with it. Up the beach there is none of this foam:
	// the simulation carries its own.
	float2 upstream = float2(p) - velocity * (dt / TexelSize);
	float carried = any(upstream != float2(p)) ? SampleFoam(upstream) : LoadFoam(p).x;
	float foam = ctx.elevation > 0.0 ? 0.0 : carried * exp(-dt / max(PBRWater::Tune5.w, 0.1));

	// The breakers make foam; what they leave behind is a share of it (Tune3.w).
	foam = max(foam, source * PBRWater::Tune3.w);

	// A non-finite value would spread over the grid: zeroed.
	if (!(abs(foam) < 1e30))
		foam = 0.0;
	if (!(abs(trainLevel) < 1e30))
		trainLevel = 0.0;
	Foam[id.xy] = float4(saturate(foam), travel, trainFoam);
	Prep[id.xy] = float4(bed, shares.incoming, shares.outgoing, trainLevel);
}
