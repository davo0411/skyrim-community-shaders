#ifndef __WATER_ACTOR_RIPPLES_HLSLI__
#define __WATER_ACTOR_RIPPLES_HLSLI__

// Actor Ripple System for PBR Water
// Generates procedural wading ripples around actors (player and NPCs) in water.
//
// Physics model:
//   Concentric expanding ring waves from each actor as a continuous point source.
//   h(r,t) = A * sin(k*r - omega*t) / sqrt(r)   (2D circular wave with energy conservation)
//   Normal offset = radial_dir * dh/dr ≈ radial_dir * A * k * cos(k*r - omega*t) / sqrt(r)
//
//   For moving actors, a Doppler shift compresses rings ahead and stretches them behind,
//   plus a Kelvin V-wake envelope at ~19.47 degrees creates the characteristic wake arms.
//
// Returns float2 tangent-space normal offsets that are added directly to finalNormal.xy,
// avoiding the accumulation/normalization bugs of the previous system.

#define MAX_ACTOR_RIPPLES 32

struct ActorRippleData
{
	float PosX;
	float PosY;
	float Speed;
	float InWater;
	float VelocityX;
	float VelocityY;
	float WaterDepth;
	float Pad0;
};

cbuffer ActorRippleBuffer : register(b10)
{
	ActorRippleData ActorRipples[MAX_ACTOR_RIPPLES];
	uint NumActorRipples;
	uint ActorRipplePad0;
	uint ActorRipplePad1;
	uint ActorRipplePad2;
}

namespace PlayerRipples
{
	// ========================================================================
	// Concentric expanding ring ripples
	// ========================================================================
	// Works for both stationary and moving actors. For moving actors, a Doppler
	// shift is applied: rings compress ahead (higher frequency) and stretch
	// behind (lower frequency), naturally creating a bow-wave appearance.

	float2 ExpandingRings(float2 worldPos, float2 sourcePos, float time,
	                      float strength, float2 velocity)
	{
		float2 delta = worldPos - sourcePos;
		float dist = length(delta);

		if (dist < 2.0 || dist > RippleRadius)
			return float2(0, 0);

		float2 radialDir = delta / dist;
		float speed = length(velocity);

		// Doppler shift for moving sources
		// cosAngle: +1 = ahead of actor's movement, -1 = behind
		float2 moveDir = speed > 15.0 ? velocity / speed : float2(0, 0);
		float speedRatio = saturate(speed / 300.0);
		float cosAngle = dot(radialDir, moveDir);
		float doppler = 1.0 + speedRatio * cosAngle * 0.6;

		// Ring wavenumbers with Doppler shift applied
		float k1 = RippleWaveFreq1 * doppler;
		float k2 = RippleWaveFreq2 * doppler;

		// Phase: k*r - omega*t gives outward expanding rings
		float omega = RippleWaveSpeed;
		float phase1 = k1 * dist - omega * time;
		float phase2 = k2 * dist - omega * 1.4 * time;  // slightly offset speed for variety

		// 2D circular wave spreading: amplitude ~ 1/sqrt(r)
		float spreading = rsqrt(max(dist, 4.0));

		// Gradient: d/dr[A*sin(kr-wt)/sqrt(r)] ≈ A*k*cos(kr-wt)/sqrt(r)
		float gradient = k1 * cos(phase1) + 0.5 * k2 * cos(phase2);
		gradient *= spreading * strength;

		// Distance envelope
		float fade = 1.0 - smoothstep(RippleRadius * 0.7, RippleRadius, dist);
		float nearBlend = smoothstep(2.0, 12.0, dist);  // avoid center singularity

		// More disturbance when actor is moving
		float moveBoost = 1.0 + speedRatio * 0.5;

		return radialDir * gradient * fade * nearBlend * moveBoost;
	}

	// ========================================================================
	// Kelvin V-wake for moving actors
	// ========================================================================
	// Creates the characteristic arrowhead wake pattern at ~19.47 degrees
	// behind a moving actor (the universal Kelvin wake angle).

	float2 KelvinWake(float2 worldPos, float2 actorPos, float2 velocity,
	                  float time, float strength)
	{
		float speed = length(velocity);
		if (speed < 20.0)
			return float2(0, 0);

		float2 moveDir = velocity / speed;
		float2 perpDir = float2(-moveDir.y, moveDir.x);
		float2 delta = worldPos - actorPos;
		float dist = length(delta);

		if (dist < 4.0 || dist > RippleRadius)
			return float2(0, 0);

		float2 dir = delta / dist;

		// Coordinates relative to movement direction
		float ahead = dot(delta, moveDir);    // positive = in front of actor
		float lateral = dot(delta, perpDir);  // signed lateral distance
		float absLateral = abs(lateral);

		// Only behind the actor (with a small transition zone)
		if (ahead > 32.0)
			return float2(0, 0);

		float behind = max(0.0, -ahead + 32.0);

		// Kelvin wake: waves concentrate along lines at tan(19.47°) ≈ 0.354
		float expectedLateral = behind * 0.354;
		float lateralDeviation = absLateral - expectedLateral;

		// Gaussian envelope along wake arms (width scales with distance)
		float armWidth = max(16.0, behind * 0.12);
		float armStrength = exp(-lateralDeviation * lateralDeviation / (2.0 * armWidth * armWidth));

		// Transverse wave pattern along the wake arms
		float armDist = sqrt(behind * behind + lateral * lateral);
		float wakePhase = armDist * 0.06 - time * 3.0;
		float wakeGrad = cos(wakePhase) * 0.06;

		// Scaling
		float speedFactor = saturate(speed / 250.0);
		float spreading = rsqrt(max(armDist, 8.0));
		float fade = 1.0 - smoothstep(RippleRadius * 0.5, RippleRadius, dist);

		return dir * wakeGrad * armStrength * spreading * speedFactor * strength * fade;
	}

	// ========================================================================
	// Main entry: accumulate normal offsets from all actors
	// ========================================================================
	// Returns float2 tangent-space normal offset to add to finalNormal.xy.
	// This is intentionally additive (not weighted by wave height) so every
	// actor's contribution is directly visible.

	float2 GetAllActorRippleOffsets(float2 worldPos, float time,
	                                float2 playerPos, float2 playerVelocity,
	                                float playerInWater, float playerWaterDepth)
	{
		float2 total = float2(0, 0);

		// Player ripples
		if (playerInWater > 0.5 && playerWaterDepth <= 100.0) {
			float playerSpeed = length(playerVelocity);
			float strength = lerp(0.8, 1.5, saturate(playerSpeed / 300.0));

			total += ExpandingRings(worldPos, playerPos, time, strength, playerVelocity);
			total += KelvinWake(worldPos, playerPos, playerVelocity, time, strength);
		}

		// NPC ripples
		uint count = min(NumActorRipples, MAX_ACTOR_RIPPLES);
		for (uint i = 0; i < count; i++) {
			ActorRippleData actor = ActorRipples[i];
			if (actor.InWater < 0.5 || actor.WaterDepth > 100.0)
				continue;

			float2 actorPos = float2(actor.PosX, actor.PosY);
			float2 actorVel = float2(actor.VelocityX, actor.VelocityY);
			float actorSpeed = length(actorVel);
			float strength = lerp(0.5, 1.2, saturate(actorSpeed / 300.0));

			total += ExpandingRings(worldPos, actorPos, time, strength, actorVel);
			total += KelvinWake(worldPos, actorPos, actorVel, time, strength);
		}

		return total;
	}
}

#endif // __WATER_ACTOR_RIPPLES_HLSLI__
