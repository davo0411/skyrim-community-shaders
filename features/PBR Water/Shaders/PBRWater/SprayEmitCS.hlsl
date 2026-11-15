// ============================================================================
// PBR Water - sea spray, pass 1: where the waves throw water into the air this frame.
//
// One thread per cell of a camera-centred grid, at a random point in its cell each frame (the grid never shows). The
// waves there are evaluated as the water draws them (EvaluateWaves), and each emitter reads its source from them:
//   plunge     the splash-up of a plunging breaker: the aerated water where the jet has landed, before the bore forms
//              (ShoreTrain::aeration, ::bore). Its speed grows with the wave as sqrt(g H) does (Peregrine 1983: the
//              splash-up rises at about the jet's speed, which is the crest's);
//   roller     the churning roller on a bore's front;
//   surf       the white water the surf simulation runs up the beach;
//   whitecap   the open sea's crests where the surface folds over itself: the Jacobian of the horizontal displacement
//              below the foam's bias (the sea's own foam is made the same way, OceanAssembleCS);
//   spindrift  the spray a gale tears off those crests: from about 9 m/s of wind (Monahan, Spiel & Davidson 1986), full
//              at 17 m/s.
// Each emitter makes rate x area x dt x strength particles here, a whole number drawn at random, and writes them into
// the pool's ring at the slots it reserves. When a frame asks for more than its budget the rest are dropped.
// ============================================================================

#include "PBRWater/PBRWater.hlsli"
#include "PBRWater/Spray.hlsli"

cbuffer SprayEmitCB : register(b0)
{
	float4 Grid;            // xy grid corner (absolute units), z cell size (units), w cells per side
	float4 Frame;           // x time step (s), y frame counter, z pool size, w emitter count
	float4 Wind;            // xy wind velocity (units/s), z wind speed (m/s), w particles a frame may make
	float4 LevelGrid;       // xy absolute cell index of the camera's cell, z cell size (units), w unused
	float4 CellLevels[25];  // x still water level of cell (camera cell + (i - 2, j - 2)) at i + 5 j (absolute z); < -1e8: none
};

StructuredBuffer<PBRWaterSpray::Emitter> Emitters : register(t0);
RWStructuredBuffer<PBRWaterSpray::Particle> Particles : register(u0);
RWByteAddressBuffer Counters : register(u1);  // [0] ring head, [1] particles made this frame

/// The still water level of the exterior cell over `absXY` (absolute z), < -1e8 where there is none.
float CellWaterLevel(float2 absXY)
{
	int2 cell = int2(floor(absXY / LevelGrid.z)) - int2(LevelGrid.xy) + 2;
	if (any(cell < 0) || any(cell > 4))
		return -1e9;
	return CellLevels[cell.x + 5 * cell.y].x;
}

/// Jacobian of the open sea's horizontal displacement here: below the foam's bias the surface folds over (whitecaps).
float SeaJacobian(float3 positionWS, PBRWater::WaveContext ctx)
{
	float dxdx = 0.0, dydy = 0.0, dxdy = 0.0;
	float steepnessScale = rcp(ctx.energyGain);
	[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
	{
		float h = ctx.cascade[c] * steepnessScale;
		[branch] if (h > 0.0)
		{
			float3 uv = float3(PBRWater::CascadeUV(positionWS.xy, c), c);
			float shear = PBRWater::OceanDisplacement.SampleLevel(PBRWater::OceanSampler, uv, 0).w;
			float4 g = PBRWater::OceanDerivatives.SampleLevel(PBRWater::OceanSampler, uv, 0);
			dxdx += g.z * h;
			dydy += g.w * h;
			dxdy += shear * h;
		}
	}
	return (1.0 + dxdx) * (1.0 + dydy) - dxdy * dxdy;
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint cells = (uint)Grid.w;
	if (any(id.xy >= cells))
		return;
	float dt = Frame.x;
	uint emitterCount = (uint)Frame.w;
	if (dt <= 0.0 || emitterCount == 0)
		return;

	uint seed = PBRWaterSpray::Hash(id.x + cells * id.y) ^ PBRWaterSpray::Hash((uint)Frame.y * 9781u + 17u);
	float2 jitter = float2(PBRWaterSpray::Random(seed), PBRWaterSpray::Random(seed));
	float2 absXY = Grid.xy + (float2(id.xy) + jitter) * Grid.z;

	float level = PBRWater::ShoreLevel(absXY);
	if (level < -1e6)
		level = CellWaterLevel(absXY);
	if (level < -1e8)
		return;

	float3 positionWS = float3(absXY, level) - FrameBuffer::CameraPosAdjust.xyz;
	float distance = length(positionWS);
	float fade = PBRWater::DisplacementFade(positionWS);
	if (fade <= 0.0)
		return;
	PBRWater::WaveContext ctx = PBRWater::BuildWaveContext(positionWS, fade);
	// Dry land past the highest run-up: no water to throw.
	if (ctx.elevation > PBRWater::Shore0.w + PBRWater::UnitsPerMetre)
		return;
	PBRWater::WaveResult w = PBRWater::EvaluateWaves(positionWS, ctx, Grid.z, false);

	// The sources (0..1 each).
	float crestMetres = max(w.crest, 0.0) * PBRWater::MetresPerUnit;
	bool wet = ctx.elevation < 0.0;
	float sources[5];
	sources[PBRWaterSpray::SourcePlunge] = wet ? w.aeration * (1.0 - w.bore) * saturate(crestMetres / 1.5) : 0.0;
	sources[PBRWaterSpray::SourceRoller] = wet ? w.aeration * w.bore * saturate(crestMetres / 1.0) : 0.0;
	sources[PBRWaterSpray::SourceSurf] = w.sheetFoam * w.surf * saturate(w.sheetDepth * PBRWater::MetresPerUnit / 0.2);
	float whitecap = 0.0;
	[branch] if (PBRWater::OceanActive() && wet && ctx.train < 1.0 && !any(ctx.flow))
	{
		float bias = PBRWater::Foam1.x;
		whitecap = saturate((bias - SeaJacobian(positionWS, ctx)) / max(0.5 * bias, 0.05)) * (1.0 - ctx.train);
	}
	sources[PBRWaterSpray::SourceWhitecap] = whitecap;
	sources[PBRWaterSpray::SourceSpindrift] = whitecap * saturate((Wind.z - 9.0) / 8.0);

	// Where the water goes: the shore waves' travel, out at sea the wind's.
	float2 windDir = PBRWater::Params1.xy;
	float3 surface = float3(absXY, level) + w.displacement;
	float3 up = normalize(lerp(float3(0.0, 0.0, 1.0), w.normal, 0.5));
	float depthMetres = max(ctx.depth * PBRWater::MetresPerUnit, 0.05);
	// The breakers' speed: shallow water, the depth under the crest. The open sea's: the deep-water phase speed of its peak.
	float shoreCelerity = sqrt(PBRWater::GravityMetres * (depthMetres + crestMetres)) * PBRWater::UnitsPerMetre;
	float seaCelerity = PBRWater::Gravity() / max(ctx.peakOmega, 0.1);
	float cellArea = Grid.z * Grid.z * PBRWater::MetresPerUnit * PBRWater::MetresPerUnit;

	uint poolSize = (uint)Frame.z;
	uint budget = (uint)Wind.w;
	[loop] for (uint e = 0; e < emitterCount; e++)
	{
		PBRWaterSpray::Emitter em = Emitters[e];
		uint kind = (uint)em.Source.x;
		if (kind > PBRWaterSpray::SourceSpindrift)
			continue;
		float strength = saturate((sources[kind] - em.Source.z) / max(em.Source.w - em.Source.z, 1e-3));
		float distanceFade = 1.0 - smoothstep(0.7 * em.Size.w, em.Size.w, distance);
		float expected = em.Source.y * cellArea * dt * strength * distanceFade;
		if (expected <= 0.0)
			continue;
		uint count = min((uint)floor(expected + PBRWaterSpray::Random(seed)), 8u);
		if (count == 0)
			continue;
		uint first;
		Counters.InterlockedAdd(4, count, first);
		if (first >= budget)
			continue;
		count = min(count, budget - first);
		uint head = Counters.Load(0);

		bool shore = kind <= PBRWaterSpray::SourceSurf;
		float2 travel2 = shore ? ctx.shore.direction : windDir;
		float3 forward = PBRWater::SafeNormalize(float3(travel2, 0.0) - up * dot(float3(travel2, 0.0), up), float3(1.0, 0.0, 0.0));
		float3 side = cross(up, forward);
		float celerity = shore ? shoreCelerity : seaCelerity;
		float speedScale = shore ? sqrt(max(crestMetres, 0.05)) : 1.0;

		for (uint i = 0; i < count; i++) {
			float declination = max(em.Angles.x + em.Angles.y * PBRWaterSpray::RandomSigned(seed), 0.0);
			float planar = em.Angles.z * PBRWaterSpray::RandomSigned(seed);
			float3 direction = up * cos(declination) + (forward * cos(planar) + side * sin(planar)) * sin(declination);
			float speed = em.Launch.x * speedScale * max(1.0 + em.Launch.y * PBRWaterSpray::RandomSigned(seed), 0.0);

			PBRWaterSpray::Particle p;
			p.velocity = direction * speed + forward * (celerity * em.Launch.z) + float3(Wind.xy * em.Launch.w, 0.0);
			// Spread over the cell, so a crest makes a sheet of spray rather than a row of fountains.
			float2 spread = float2(PBRWaterSpray::RandomSigned(seed), PBRWaterSpray::RandomSigned(seed)) * 0.5 * Grid.z;
			p.position = surface + float3(spread, 0.0) + up * em.Angles.w;
			p.age = 0.0;
			p.life = max(em.Life.x * (1.0 + em.Life.y * PBRWaterSpray::RandomSigned(seed)), 0.05);
			p.radius = max(em.Size.x * (1.0 + em.Size.y * PBRWaterSpray::RandomSigned(seed)), 0.1);
			p.rotation = PBRWaterSpray::Random(seed) * Math::TAU;
			p.rotationSpeed = em.Motion.z * (1.0 + em.Motion.w * PBRWaterSpray::RandomSigned(seed)) * (PBRWaterSpray::Random(seed) < 0.5 ? -1.0 : 1.0);
			p.packed = (e & 0xFu) | (PBRWaterSpray::Hash(seed) << 12u);
			// The water it falls back into: the still level, or the trough it was thrown from (a crest's spray lands lower down).
			p.floorZ = min(surface.z, level) - 0.1 * PBRWater::UnitsPerMetre;
			p.pad = 0.0;
			if (!all(abs(p.velocity) < 1e6) || !all(abs(p.position) < 1e8))
				continue;
			Particles[(head + first + i) % poolSize] = p;
		}
	}
}
