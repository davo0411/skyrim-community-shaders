// ============================================================================
// PBR Water - the foam the surf leaves on the water.
//
// Foam in the surf is a tracer on the water's surface: the breakers make it (the overturning crest and the bore's roller,
// where the shore profile carries white water), the water carries it, and it decays as its bubbles burst, slowly on the
// water (coastal surfactants make surf foam outlast the bubble plume under it) and quickly once it is stranded on the sand,
// which drains it. So a breaker leaves a lace of foam behind it on the water as it flattens, the uprush carries the bore's
// foam up the beach and the backwash pulls it back down. (Before, the left-behind foam was a fixed lace wherever the surf
// was depth-limited and the bores' foam a window sliding over a still pattern, so the foam smeared as the waves moved.)
//
// A camera-centred grid in the water's rest coordinates, like the ripples: the surface the shaders draw is the rest
// position plus the waves' displacement, so the foam is carried by the water's velocity less the displacement's own
// rate. Under the shore waves the water moves with them at u = c eta / h (shallow water: the bores carry their water
// forward, the troughs pull it back); on the beach it moves with the swash sheet, up the slope at the tip's speed and
// back down with the backwash. Semi-Lagrangian advection, as Crest 4 (wave-harmonic/crest, MIT) moves its foam.
//
// The water shader draws the foam's pattern still, where the surface is: the foam moves over it. Every way of carrying the
// pattern along tore it: with the water sloshing under the waves, with the breakers next to still lace, with the swash
// where neighbouring water belonged to different uprushes, and as a texture offset carried per texel.
//
// It also lays out, for the swash simulation (SwashStepCS), what the shore waves bring it this frame: the bed, how much
// the waves hold the water here (the surf, where the analytic train is the water), and there the water's level and
// velocity and the foam the bores carry.
//
// Output: Stage (x foam coverage, y the waves' level (m above the still water), z the bores' foam, w unused) and Prep
// (x bed (m above the still water), y forcing 0..1, zw the waves' water velocity (m/s)). SwashOutCS copies the foam on.
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
Texture2D<float4> PreviousSwash : register(t1);  // SwashStepCS: x depth (m), yz discharge (m^2/s), w foam
RWTexture2D<float4> Foam : register(u0);
RWTexture2D<float4> Prep : register(u1);

/// Deepest water the swash simulation holds (m): its waves need nothing deeper, and the time step shrinks with the depth.
static const float SwashDepthCap = 2.0;
/// No ground: a wall (m).
static const float SwashWall = 1000.0;

/// Foam left on dry sand drains into it within about a second.
static const float StrandedLifetime = 1.0;

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
 * Last frame's state at fractional texel `q` (texel centres at integers), Catmull-Rom, the foam held within the four
 * texels around q. A bilinear read every frame blurred the foam: resampled 60 times a second over its lifetime it spread
 * into smears. The clamp keeps the cubic from ringing (Selle et al. 2008 limit their MacCormack step the same way).
 */
float4 SampleFoam(float2 q)
{
	float2 base = floor(q);
	float2 f = q - base;
	int2 i = int2(base);
	float4 wx = CatmullRom(f.x);
	float4 wy = CatmullRom(f.y);
	float4 result = 0.0;
	[unroll] for (int y = 0; y < 4; y++)
	{
		float4 row = 0.0;
		[unroll] for (int x = 0; x < 4; x++)
			row += wx[x] * LoadFoam(i + int2(x - 1, y - 1));
		result += wy[y] * row;
	}
	float4 a = LoadFoam(i), b = LoadFoam(i + int2(1, 0)), c = LoadFoam(i + int2(0, 1)), d = LoadFoam(i + int2(1, 1));
	result.x = clamp(result.x, min(min(a.x, b.x), min(c.x, d.x)), max(max(a.x, b.x), max(c.x, d.x)));
	// The pattern's velocity only needs to be smooth.
	result.yz = lerp(lerp(a.yz, b.yz, f.x), lerp(c.yz, d.yz, f.x), f.y);
	return result;
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
	float bed = ctx.elevation * PBRWater::MetresPerUnit;
	float depthMetres = -bed;
	if (ctx.train <= 0.0 && ctx.beach <= 0.0) {
		// Still water, or ground no wave reaches.
		Foam[id.xy] = 0.0;
		Prep[id.xy] = float4(max(bed, -SwashDepthCap), depthMetres > 0.05 ? 1.0 : 0.0, 0.0, 0.0);
		return;
	}

	float dt = max(Dt, 0.0);
	float2 dir = ctx.shore.direction;
	float2 velocity = 0.0;  // units/s, in the rest frame
	float source = 0.0;
	bool stranded = false;

	// The swash at the waterline and on the beach: the foam moves with the simulated sheet (no waves displace the surface
	// there). Where no sheet covers the bed it is stranded on the sand.
	if (ctx.elevation > -0.1 * PBRWater::UnitsPerMetre) {
		int2 q = p;
		float4 swash = all(q >= 0) && all(q < (int)GridSize) ? PreviousSwash.Load(int3(q, 0)) : 0.0;
		if (swash.x > 0.003)
			velocity = swash.yz / swash.x * PBRWater::UnitsPerMetre;
		else if (ctx.elevation > 0.0)
			stranded = true;
	}

	// For the swash: in the surf the analytic waves are the water. They hand it over to the simulation while the bore is
	// still a bore (PBRWater::SwashHandOver; a breaker's bore stands about 0.8 of the depth high, so it carries a good part
	// of the wave's water and momentum), and the drawn waves give way to it in the same measure (DrawnShoreWaves). Held to
	// the waves down to 8 cm, the simulation was pulled down with the collapsing front, lost the bore's water and launched
	// a few centimetres of it at the waterline, which crawled there until the water behind caught up and tipped it over.
	float forcing = PBRWater::SwashHandOver(ctx);
	float eta = 0.0;
	float2 waterVelocity = 0.0;
	float boreFoam = 0.0;
	[branch] if (ctx.train > 0.0)
	{
		// The shore waves here and as they were last frame: their white water is the source, their displacement's rate
		// is the part of the water's motion the drawn surface already carries.
		PBRWater::ShoreTrain train = PBRWater::ShoreWaves(positionWS, ctx, TexelSize, dt > 0.0, true);
		// Up the beach the profile's bores have no water to be in: the foam there is what the swash carried up.
		source = ctx.elevation > 0.0 ? 0.0 : train.foam;

		// Each bore brings its own water: the level the waves stand at, the water's velocity under them (shallow water,
		// u = c eta / h, which runs to sqrt(g eta) as the bed comes up to the still water) and its foam. Every wave comes in
		// a little turned of its own (its crests bend with the bed and the swell's spread), so neighbouring uprushes cross.
		eta = train.displacement.z * PBRWater::MetresPerUnit;
		float columnDepth = max(max(depthMetres, 0.0) + eta, 0.05);
		float speed = clamp(eta * sqrt(PBRWater::GravityMetres / columnDepth), -6.0, 6.0);
		// Under a crest the water is a bore's (Rankine-Hugoniot, Stoker 1957): a bore of depth h2 running into water h1 moves at
		// C = sqrt(g h2 (h1 + h2) / (2 h1)) and drives the water behind it at u2 = C (1 - h1 / h2), which runs to
		// sqrt(g eta) for small eta/h like the linear long wave above but much higher for a bore on shallow water (Hughes
		// 1992: the collapsing bores' uprush starts at up to 5 m/s; Shen & Meyer 1963: twice sqrt(g h) at the tip). Over
		// ground at or above the still water the bore runs into a film: h1 is held at a fifth of h2.
		[branch] if (eta > 0.02)
		{
			float h1 = max(max(depthMetres, 0.0), 0.2 * columnDepth);
			float bore = sqrt(PBRWater::GravityMetres * columnDepth * (h1 + columnDepth) / (2.0 * h1));
			speed = clamp(bore * saturate(1.0 - h1 / columnDepth), 0.0, 6.0);
		}
		float2 coast = PBRWater::CoastPoint(absXY, ctx.shore);
		float cycles = PBRWater::ShorePhase(ctx.shore, coast, PBRWater::Shore2.x, 0) / Math::TAU - PBRWater::ShoreBoreFront;
		float turn = 0.35 * (frac(sin(floor(cycles) * 12.9898 + dot(floor(coast / 2048.0), float2(4.1, 7.3))) * 43758.5453) - 0.5);
		float2 waveDir = float2(dir.x * cos(turn) - dir.y * sin(turn), dir.x * sin(turn) + dir.y * cos(turn));
		waterVelocity = waveDir * speed;
		boreFoam = train.foam * PBRWater::Tune3.w;
		if (ctx.elevation <= -0.1 * PBRWater::UnitsPerMetre) {
			// Shallow water: the water under a wave moves with it at u = c eta / (h + eta) along the travel.
			float depth = max(ctx.depth, 0.05 * PBRWater::UnitsPerMetre);
			float celerity = sqrt(PBRWater::GravityMetres * depth * PBRWater::MetresPerUnit) * PBRWater::UnitsPerMetre;
			float eta = train.displacement.z;
			float water = clamp(celerity * eta / max(depth + eta, 0.05 * PBRWater::UnitsPerMetre), -celerity, celerity);
			velocity = dir * water;
			if (dt > 0.0)
				velocity -= (train.displacement.xy - train.previousDisplacement.xy) / dt;
		}
	}

	// Semi-Lagrangian: the water here came from upstream, and its foam with it.
	float2 upstream = float2(p) - velocity * (dt / TexelSize);
	float4 carried = any(upstream != float2(p)) ? SampleFoam(upstream) : LoadFoam(p);
	float foam = carried.x * exp(-dt / max(stranded ? StrandedLifetime : PBRWater::Tune5.w, 0.1));

	// The breakers make foam; what they leave behind is a share of it (Tune3.w).
	float made = source * PBRWater::Tune3.w;
	foam = max(foam, made);

	// A non-finite value would spread over the grid: zeroed.
	if (!(abs(foam) < 1e30))
		foam = 0.0;
	Foam[id.xy] = float4(saturate(foam), eta, boreFoam, 0.0);
	Prep[id.xy] = float4(max(bed, -SwashDepthCap), forcing, waterVelocity);
}
