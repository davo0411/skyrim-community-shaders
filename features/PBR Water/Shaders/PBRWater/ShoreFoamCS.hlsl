// ============================================================================
// PBR Water - what the shore waves bring the surf simulation each frame, and where they throw spray.
//
// For the surf simulation (SwashStepCS): the bed, how far the simulation takes the train's waves here and gives up its own
// (PBRWater::SurfSharesAt), the train's level, the direction its waves travel in and the white water they carry. The
// simulation carries its own foam with its water; outside it the breakers' white water travels with the train
// (PBRWater::BreakerFoam), and nothing is left lying on the water once a wave has passed.
//
// Output: Stage (x unused, yz the waves' direction of travel, w the white water they carry) and Prep (x bed (m above
// the still water), y incoming share, z outgoing share, w the train's level (m above the still water)), and SpraySource.
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

RWTexture2D<float4> Foam : register(u0);
RWTexture2D<float4> Prep : register(u1);
/// Where the breakers throw spray (SprayEmitCS), at the surface drawn over the texel: x a plunging breaker's splash-up, y a
/// bore's roller (0..1 each), z the surface (units above the still water), w the crest's height (m).
RWTexture2D<float4> SpraySource : register(u2);

/// Deepest water the surf simulation holds (m): deeper, the train alone is the water in any sea, and the time step
/// shrinks with the depth.
static const float SwashDepthCap = 6.0;
/// No ground: a wall (m).
static const float SwashWall = 1000.0;

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= GridSize))
		return;
	float2 absXY = Origin + (float2(id.xy) + 0.5) * TexelSize;

	float level = PBRWater::ShoreLevel(absXY);
	if (level < -1e6) {
		Foam[id.xy] = 0.0;
		Prep[id.xy] = float4(SwashWall, 0.0, 0.0, 0.0);
		SpraySource[id.xy] = 0.0;
		return;
	}
	float3 positionWS = float3(absXY, level) - FrameBuffer::CameraPosAdjust.xyz;
	PBRWater::WaveContext ctx = PBRWater::BuildWaveContext(positionWS, PBRWater::DisplacementFade(positionWS), false);
	float bed = max(ctx.elevation * PBRWater::MetresPerUnit, -SwashDepthCap);
	float2 dir = ctx.shore.direction;
	if (ctx.train <= 0.0 && ctx.beach <= 0.0) {
		// Still water, or ground no wave reaches: the water is held at rest.
		float held = bed < -0.05 ? 1.0 : 0.0;
		Foam[id.xy] = float4(0.0, dir, 0.0);
		Prep[id.xy] = float4(bed, held, held, 0.0);
		SpraySource[id.xy] = 0.0;
		return;
	}

	PBRWater::SurfShares shares = PBRWater::SurfSharesAt(ctx);
	float trainLevel = 0.0;
	float trainFoam = 0.0;
	float4 spray = 0.0;
	[branch] if (ctx.train > 0.0)
	{
		// Only its displacement is wanted (the lean below): no foam profile.
		PBRWater::ShoreTrain train = PBRWater::ShoreWaves(positionWS, ctx, TexelSize, false, false);
		// What the surf simulation takes: the train's surface and breaker foam where it is drawn over this texel. The
		// simulation is a height field on the ground and the train a surface displaced from its rest position: the
		// water drawn over this texel rests behind it by the crest's forward lean, x + D(x) = X, solved by one
		// fixed-point step x = X - D(x) (the lean changes slowly along the wave). A breaker's lip is drawn over the
		// simulation's water and stays out.
		PBRWater::ShoreTrain drawnHere = train;
		[branch] if (any(abs(train.displacement.xy) > 0.01 * PBRWater::UnitsPerMetre))
			drawnHere = PBRWater::ShoreWaves(float3(positionWS.xy - train.displacement.xy, positionWS.z), ctx, TexelSize, false, true);
		trainLevel = drawnHere.displacement.z * PBRWater::MetresPerUnit;
		trainFoam = drawnHere.foam;
		// The spray the breaker throws here: the splash-up where its jet has landed, before the bore forms (the aerated,
		// crushed water ahead of the old crest), and later the roller on the bore's front; both grow with the wave.
		// Where the water is (the drawn surface over this texel), so the spray rides the crest that throws it.
		float crestMetres = max(drawnHere.crest, 0.0) * PBRWater::MetresPerUnit;
		bool wet = ctx.elevation < 0.0;
		spray = float4(wet ? drawnHere.aeration * (1.0 - drawnHere.bore) : 0.0, wet ? drawnHere.aeration * drawnHere.bore : 0.0,
			drawnHere.displacement.z, crestMetres);
	}

	// Each wave comes in a little turned of its own (its crests bend with the bed and the swell's spread), so neighbouring
	// uprushes cross on the beach.
	float2 coast = PBRWater::CoastPoint(absXY, ctx.shore);
	float cycles = PBRWater::ShorePhase(ctx.shore, coast, PBRWater::Shore2.x) / Math::TAU;
	float turn = 0.35 * (frac(sin(floor(cycles) * 12.9898 + dot(floor(coast / 2048.0), float2(4.1, 7.3))) * 43758.5453) - 0.5);
	float2 travel = float2(dir.x * cos(turn) - dir.y * sin(turn), dir.x * sin(turn) + dir.y * cos(turn));

	// A non-finite value would spread over the grid: zeroed.
	if (!(abs(trainLevel) < 1e30))
		trainLevel = 0.0;
	Foam[id.xy] = float4(0.0, travel, trainFoam);
	Prep[id.xy] = float4(bed, shares.incoming, shares.outgoing, trainLevel);
	SpraySource[id.xy] = all(abs(spray) < 1e30) ? spray : 0.0;
}
