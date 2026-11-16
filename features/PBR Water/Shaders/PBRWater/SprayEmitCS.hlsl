// ============================================================================
// PBR Water - sea spray, pass 1: where the waves throw water into the air this frame.
//
// Two grids, one dispatch each:
//   SPRAY_SHORE  the surf's foam grid (17 cm texels, ShoreFoamCS), where the breakers' spray has already been laid out
//                at the surface drawn over each texel (SpraySource): it is thrown from exactly where the breaker is, a
//                line a metre or two wide that a coarser grid sampled at random points scattered into dots. Sources:
//                  plunge  the splash-up of a plunging breaker: the aerated water where the jet has landed, before the
//                          bore forms. It rises at about the jet's speed, which is the crest's, sqrt(g H) (Peregrine 1983):
//                          its speed grows with the square root of the wave's height;
//                  roller  the churning roller on a bore's front;
//                  surf    the white water the surf simulation runs up the beach.
//   (default)    a camera-centred grid over the open sea, one thread per cell at a random point in it each frame, the waves
//                evaluated as the water draws them. Whitecaps are areas, not lines, so a coarse grid serves:
//                  whitecap   crests where the surface folds over itself: the Jacobian of the horizontal displacement below
//                             the foam's bias (the sea's own foam is made the same way, OceanAssembleCS);
//                  spindrift  the spray a gale tears off them: from about 9 m/s of wind (Monahan, Spiel & Davidson 1986),
//                             full at 17 m/s.
// Each emitter makes rate x area x dt x strength particles at a point, a whole number drawn at random, and writes them
// into the pool's ring at the slots it reserves; a frame that asks for more than its budget drops the rest. Thrown water
// keeps the speed of the wave that threw it along the wave's travel (Launch.z), so the spray rides the breaker.
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
#if defined(SPRAY_SHORE)
Texture2D<float4> SpraySource : register(t1);  // ShoreFoamCS: x splash-up, y roller, z surface (units above the still water), w crest (m)
Texture2D<float4> Stage : register(t2);        // ShoreFoamCS: yz the waves' direction of travel
Texture2D<float4> Prep : register(t3);         // ShoreFoamCS: x the bed (m above the still water)
#endif
RWStructuredBuffer<PBRWaterSpray::Particle> Particles : register(u0);
RWByteAddressBuffer Counters : register(u1);  // [0] ring head, [1] particles made this frame

/// m from the camera beyond which the puffs grow fewer and larger (Emit).
static const float LodDistance = 25.0;

/// Where and how the water is thrown from one point.
struct Launch
{
	float3 surface;    // absolute position of the water surface (units)
	float floorZ;      // absolute z it falls back into (units)
	float3 up;         // the surface's up
	float2 travel;     // the waves' direction of travel
	float celerity;    // their speed (units/s)
	float speedScale;  // launch speed multiplier (shore: the square root of the wave's height in metres)
	float area;        // m^2 this point stands for
	float spread;      // units: the particles start spread over this square around the point
	float distance;    // units from the camera
};

/// Makes each emitter's particles for the sources at one point.
void Emit(float sources[5], Launch l, inout uint seed)
{
	uint emitterCount = (uint)Frame.w;
	uint poolSize = (uint)Frame.z;
	uint budget = (uint)Wind.w;
	float dt = Frame.x;
	float3 forward = PBRWater::SafeNormalize(float3(l.travel, 0.0) - l.up * dot(float3(l.travel, 0.0), l.up), float3(1.0, 0.0, 0.0));
	float3 side = cross(l.up, forward);

	[loop] for (uint e = 0; e < emitterCount; e++)
	{
		PBRWaterSpray::Emitter em = Emitters[e];
		uint kind = (uint)em.Source.x;
		if (kind > PBRWaterSpray::SourceSpindrift)
			continue;
		float strength = saturate((sources[kind] - em.Source.z) / max(em.Source.w - em.Source.z, 1e-3));
		float distanceFade = 1.0 - smoothstep(0.7 * em.Size.w, em.Size.w, l.distance);
		// Level of detail: past LodDistance the spray is made of fewer, larger puffs, (LodDistance / d)^2 as many and
		// d / LodDistance as wide, which covers the same share of the screen at a fraction of the particles and overdraw.
		// Droplets only thin out (each is held at a pixel's width).
		bool droplet = em.Shape.x < 0.5;
		float lod = max(l.distance / (LodDistance * PBRWater::UnitsPerMetre), 1.0);
		float sizeScale = droplet ? 1.0 : lod;
		float expected = em.Source.y * l.area * dt * strength * distanceFade / (lod * lod);
		if (expected <= 0.0)
			continue;
		uint count = min((uint)floor(expected + PBRWaterSpray::Random(seed)), 16u);
		if (count == 0)
			continue;
		uint first;
		Counters.InterlockedAdd(4, count, first);
		if (first >= budget)
			continue;
		count = min(count, budget - first);
		uint head = Counters.Load(0);

		for (uint i = 0; i < count; i++)
		{
			float declination = max(em.Angles.x + em.Angles.y * PBRWaterSpray::RandomSigned(seed), 0.0);
			float planar = em.Angles.z * PBRWaterSpray::RandomSigned(seed);
			float3 direction = l.up * cos(declination) + (forward * cos(planar) + side * sin(planar)) * sin(declination);
			float speed = em.Launch.x * l.speedScale * max(1.0 + em.Launch.y * PBRWaterSpray::RandomSigned(seed), 0.0);

			PBRWaterSpray::Particle p;
			p.velocity = direction * speed + forward * (l.celerity * em.Launch.z) + float3(Wind.xy * em.Launch.w, 0.0);
			float2 spread = float2(PBRWaterSpray::RandomSigned(seed), PBRWaterSpray::RandomSigned(seed)) * 0.5 * l.spread;
			p.position = l.surface + float3(spread, 0.0) + l.up * em.Angles.w;
			p.age = 0.0;
			p.life = max(em.Life.x * (1.0 + em.Life.y * PBRWaterSpray::RandomSigned(seed)), 0.05);
			p.radius = max(em.Size.x * sizeScale * (1.0 + em.Size.y * PBRWaterSpray::RandomSigned(seed)), 0.1);
			p.rotation = PBRWaterSpray::Random(seed) * Math::TAU;
			p.rotationSpeed = em.Motion.z * (1.0 + em.Motion.w * PBRWaterSpray::RandomSigned(seed)) * (PBRWaterSpray::Random(seed) < 0.5 ? -1.0 : 1.0);
			p.packed = (e & 0xFu) | (PBRWaterSpray::Hash(seed) << 12u);
			p.floorZ = l.floorZ;
			p.pad = 0.0;
			if (!all(abs(p.velocity) < 1e6) || !all(abs(p.position) < 1e8))
				continue;
			Particles[(head + first + i) % poolSize] = p;
		}
	}
}

/// The still water level of the exterior cell over `absXY` (absolute z), < -1e8 where there is none.
float CellWaterLevel(float2 absXY)
{
	int2 cell = int2(floor(absXY / LevelGrid.z)) - int2(LevelGrid.xy) + 2;
	if (any(cell < 0) || any(cell > 4))
		return -1e9;
	return CellLevels[cell.x + 5 * cell.y].x;
}

#if defined(SPRAY_SHORE)

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint cells = (uint)Grid.w;
	if (any(id.xy >= cells) || Frame.x <= 0.0 || Frame.w < 0.5)
		return;
	int3 c = int3(id.xy, 0);
	float4 source = SpraySource.Load(c);
	// The surf simulation's white water (SwashOutCS: z its depth (units), w its foam).
	float4 sheet = PBRWater::ShoreFoamTexture.Load(c);
	float surf = sheet.w * saturate(sheet.z * PBRWater::MetresPerUnit / 0.2);
	if (source.x + source.y < 1e-3 && surf < 1e-3)
		return;

	uint seed = PBRWaterSpray::Hash(id.x + cells * id.y) ^ PBRWaterSpray::Hash((uint)Frame.y * 9781u + 17u);
	float2 absXY = Grid.xy + (float2(id.xy) + 0.5) * Grid.z;
	float level = PBRWater::ShoreLevel(absXY);
	if (level < -1e6)
		return;

	float crestMetres = source.w;
	float bed = Prep.Load(c).x;
	float depthMetres = max(-bed, 0.05);
	float sources[5] = { source.x * saturate(crestMetres / 1.5), source.y * saturate(crestMetres / 1.0), surf, 0.0, 0.0 };

	Launch l;
	// On the surf's sheet the surface is the simulation's (SwashOutCS y); elsewhere the train's.
	float surfaceHeight = source.x + source.y >= 1e-3 ? source.z : sheet.y;
	l.surface = float3(absXY, level + surfaceHeight);
	l.floorZ = min(l.surface.z, level) - 0.1 * PBRWater::UnitsPerMetre;
	l.up = float3(0.0, 0.0, 1.0);
	l.travel = Stage.Load(c).yz;
	// The breakers' speed: shallow water, the depth under the crest.
	l.celerity = sqrt(PBRWater::GravityMetres * (depthMetres + crestMetres)) * PBRWater::UnitsPerMetre;
	l.speedScale = sqrt(max(crestMetres, 0.05));
	l.area = Grid.z * Grid.z * PBRWater::MetresPerUnit * PBRWater::MetresPerUnit;
	l.spread = Grid.z;
	l.distance = length(l.surface - FrameBuffer::CameraPosAdjust.xyz);
	Emit(sources, l, seed);
}

#else

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
	if (any(id.xy >= cells) || Frame.x <= 0.0 || Frame.w < 0.5 || !PBRWater::OceanActive())
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
	float fade = PBRWater::DisplacementFade(positionWS);
	if (fade <= 0.0)
		return;
	PBRWater::WaveContext ctx = PBRWater::BuildWaveContext(positionWS, fade);
	// Only the open sea's crests: the shore's breakers throw their spray from the surf's grid, and on rivers the sea's
	// pattern is carried along the current (not sampled here).
	if (ctx.elevation >= 0.0 || ctx.train >= 1.0 || any(ctx.flow))
		return;
	float bias = PBRWater::Foam1.x;
	float whitecap = saturate((bias - SeaJacobian(positionWS, ctx)) / max(0.5 * bias, 0.05)) * (1.0 - ctx.train);
	if (whitecap <= 0.0)
		return;
	float sources[5] = { 0.0, 0.0, 0.0, whitecap, whitecap * saturate((Wind.z - 9.0) / 8.0) };

	PBRWater::WaveResult w = PBRWater::EvaluateWaves(positionWS, ctx, Grid.z, false);
	Launch l;
	l.surface = float3(absXY, level) + w.displacement;
	l.floorZ = min(l.surface.z, level) - 0.1 * PBRWater::UnitsPerMetre;
	l.up = normalize(lerp(float3(0.0, 0.0, 1.0), w.normal, 0.5));
	l.travel = PBRWater::Params1.xy;
	// The open sea's speed: the deep-water phase speed of its peak.
	l.celerity = PBRWater::Gravity() / max(ctx.peakOmega, 0.1);
	l.speedScale = 1.0;
	l.area = Grid.z * Grid.z * PBRWater::MetresPerUnit * PBRWater::MetresPerUnit;
	l.spread = Grid.z;
	l.distance = length(positionWS);
	Emit(sources, l, seed);
}

#endif
