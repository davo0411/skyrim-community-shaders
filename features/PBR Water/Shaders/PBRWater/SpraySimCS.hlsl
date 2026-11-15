// ============================================================================
// PBR Water - sea spray, pass 2: the particles' flight, and the list of the living for the draw.
//
// Gravity and quadratic air drag towards the wind: dv/dt = g - k |v - w| (v - w). k is the emitter's drag, 3 rho_air
// / (8 rho_water r) for a droplet of radius r (about 2e-4 / r(m) per metre), much larger for the mist, whose terminal
// speed sqrt(g / k) is then a few centimetres a second and which drifts with the wind. The drag is integrated
// semi-implicitly (stable at any time step). A particle is gone at the end of its life or once it falls back below the
// water it was thrown from.
//
// Thread 0 also advances the pool's ring past the particles the emit pass made.
// ============================================================================

#include "PBRWater/Spray.hlsli"

cbuffer SpraySimCB : register(b0)
{
	float4 Frame;  // x time step (s), y gravity (units/s^2), z pool size, w particles a frame may make
	float4 Wind;   // xy wind velocity (units/s), zw unused
};

StructuredBuffer<PBRWaterSpray::Emitter> Emitters : register(t0);
RWStructuredBuffer<PBRWaterSpray::Particle> Particles : register(u0);
RWByteAddressBuffer Counters : register(u1);
AppendStructuredBuffer<uint> Alive : register(u2);

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint poolSize = (uint)Frame.z;
	if (id.x == 0) {
		uint made = min(Counters.Load(4), (uint)Frame.w);
		Counters.Store2(0, uint2((Counters.Load(0) + made) % max(poolSize, 1u), 0u));
	}
	if (id.x >= poolSize)
		return;

	PBRWaterSpray::Particle p = Particles[id.x];
	if (!(p.age < p.life))
		return;

	float dt = Frame.x;
	if (dt > 0.0) {
		PBRWaterSpray::Emitter em = Emitters[PBRWaterSpray::EmitterIndex(p)];
		float3 wind = float3(Wind.xy, 0.0);
		float3 relative = p.velocity - wind;
		relative /= 1.0 + max(em.Motion.y, 0.0) * length(relative) * dt;
		p.velocity = wind + relative;
		p.velocity.z -= Frame.y * em.Motion.x * dt;
		p.position += p.velocity * dt;
		p.age += dt;
		if (p.position.z < p.floorZ && p.velocity.z < 0.0)
			p.age = p.life;
		Particles[id.x] = p;
	}
	if (p.age < p.life)
		Alive.Append(id.x);
}
