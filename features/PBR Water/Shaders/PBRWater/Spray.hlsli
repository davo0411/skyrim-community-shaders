#ifndef __PBR_WATER_SPRAY_HLSLI__
#define __PBR_WATER_SPRAY_HLSLI__

// ============================================================================
// PBR Water - sea spray: the particles the breakers and the sea's whitecaps throw into the air.
//
// A GPU particle system in three passes: SprayEmitCS lays particles out over a camera-centred grid where the waves are
// breaking now (the shore train's splash-up and roller, the surf simulation's white water, the open sea's folding
// crests), SpraySimCS carries them through the air (gravity, quadratic air drag towards the wind) and lists the living,
// and SprayDraw.hlsl draws those as billboards after the water, lit as the vanilla particles are (Particle.hlsl).
//
// What a particle is and does comes from its emitter, a setting edited in game (SprayParticles.h), in the vocabulary of
// the vanilla particle NIFs - NiPSysEmitter's speed, declination and planar angle, lifespan and radius with their
// variations, NiPSysGravityModifier, NiPSysDragModifier, NiPSysGrowFadeModifier, NiPSysRotationModifier and
// BSEffectShaderProperty's base colour, soft falloff depth and lighting influence. Their shapes are procedural: droplets
// as motion streaks, the spray sheet and mist as noise-torn puffs (SprayDraw.hlsl).
// ============================================================================

namespace PBRWaterSpray
{

	/// What makes an emitter's particles (Emitter::Source.x).
	static const uint SourcePlunge = 0;     // a plunging breaker's splash-up: the jet lands and throws the water up (shore train)
	static const uint SourceRoller = 1;     // the roller on a broken wave's front: a bore's churning white water (shore train)
	static const uint SourceSurf = 2;       // the surf simulation's white water running up the beach
	static const uint SourceWhitecap = 3;   // the open sea's breaking crests: where the surface folds over (Jacobian)
	static const uint SourceSpindrift = 4;  // spray torn off those crests by a strong wind

	struct Particle
	{
		float3 position;      // absolute (units)
		float age;            // s; at or past life: dead
		float3 velocity;      // units/s
		float life;           // s
		float radius;         // units, at birth
		float rotation;       // rad, at birth
		float rotationSpeed;  // rad/s
		uint packed;          // bits 0-3 emitter, 4-11 unused, 12-31 random bits
		float floorZ;         // absolute z of the water it was thrown from: falling back below it, it is gone (units)
		float3 pad;
	};

	/// An emitter definition (SprayParticles::EmitterGpu): every particle it makes and how they are drawn.
	struct Emitter
	{
		float4 Source;  // x source (Source*), y particles per m^2 per s at full strength, z strength below which none are made, w strength of full rate
		float4 Launch;  // x speed (units/s; shore sources: at a 1 m wave), y its variation (share), z share of the wave's speed added along its travel, w share of the wind added
		float4 Angles;  // x declination (rad from straight up), y its variation (rad), z planar spread around the travel (rad), w height above the surface they start at (units)
		float4 Life;    // x lifespan (s), y its variation (share), z grow time (s), w fade time (s)
		float4 Size;    // x radius (units), y its variation (share), z size multiplier at the end of life, w the farthest the camera sees them (units)
		float4 Motion;  // x gravity multiplier, y air drag (1/unit: a(v) = -k |v - wind| (v - wind)), z rotation speed (rad/s), w its variation (share)
		float4 Color;   // rgb base colour (linear), a alpha
		float4 Shade;   // x fade-in share of life, y lighting influence (0 unlit .. 1 lit), z forward scattering (Henyey-Greenstein g), w additive share (0 alpha blended .. 1 additive)
		float4 Shape;   // x 0 droplet streaks, 1 puffs; y a puff's edge softness, z how much noise tears it apart, w unused
		float4 Render;  // x soft falloff depth (units), y motion stretch (s of travel), zw unused
	};

	uint Hash(uint x)
	{
		// PCG (Jarzynski & Olano 2020, "Hash Functions for GPU Rendering")
		uint state = x * 747796405u + 2891336453u;
		uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		return (word >> 22u) ^ word;
	}

	/// Uniform in [0, 1), advancing `seed`.
	float Random(inout uint seed)
	{
		seed = Hash(seed);
		return float(seed >> 8) * (1.0 / 16777216.0);
	}

	/// Uniform in [-1, 1), advancing `seed`.
	float RandomSigned(inout uint seed)
	{
		return Random(seed) * 2.0 - 1.0;
	}

	uint EmitterIndex(Particle p) { return p.packed & 0xFu; }  // up to 16 emitters
	uint ParticleBits(Particle p) { return p.packed >> 12u; }
}

#endif  // __PBR_WATER_SPRAY_HLSLI__
