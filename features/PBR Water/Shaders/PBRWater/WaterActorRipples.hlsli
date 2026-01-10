#ifndef __WATER_ACTOR_RIPPLES_HLSLI__
#define __WATER_ACTOR_RIPPLES_HLSLI__

// Player Ripples System for Unified Water
// Generates procedural wading ripples around actors (player and NPCs) in water
// Uses proper wave physics: height field + analytical gradient for correct normals

#define MAX_ACTOR_RIPPLES 32

struct ActorRippleData
{
	float PosX;
	float PosY;
	float Speed;
	float InWater;
	float VelocityX;  // Actual velocity for wake direction
	float VelocityY;
	float WaterDepth;  // Depth below water surface (negative = above)
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
	// Constants for ripple behavior
	static const float MAX_RIPPLE_RADIUS = 512.0f;   // Maximum expansion radius
	static const float KELVIN_ANGLE = 0.3398f;       // ~19.47° in radians (arcsin(1/3))
	
	// Hash function for procedural variation
	float hash(float2 p)
	{
		float h = dot(p, float2(127.1f, 311.7f));
		return frac(sin(h) * 43758.5453123f);
	}
	
	// Structure to accumulate wave height and its gradient
	struct WaveResult
	{
		float height;     // Wave displacement
		float2 gradient;  // dHeight/dx, dHeight/dy for normal calculation
	};
	
	// Add a circular ripple wave centered at origin
	// Returns height and analytical gradient
	WaveResult CircularWave(float2 pos, float frequency, float speed, float time, float amplitude)
	{
		WaveResult result;
		result.height = 0.0f;
		result.gradient = float2(0, 0);
		
		float dist = length(pos);
		if (dist < 0.001f) {
			result.height = amplitude * sin(-speed * time);
			return result;
		}
		
		float phase = dist * frequency - speed * time;
		float wave = sin(phase);
		float dWave = cos(phase) * frequency;  // derivative of sin(phase) w.r.t. distance
		
		result.height = wave * amplitude;
		// Gradient = dHeight/dPos = dHeight/dDist * dDist/dPos
		// dDist/dPos = pos/dist (unit direction)
		result.gradient = (pos / dist) * dWave * amplitude;
		
		return result;
	}
	
	// Evaluate wake pattern for moving actors
	// Uses position relative to movement direction with smooth, realistic wake shape
	WaveResult EvaluateWakeRipples(float2 worldPos, float2 actorPos, float2 moveDir, float time, float speed, float baseStrength)
	{
		WaveResult result;
		result.height = 0.0f;
		result.gradient = float2(0, 0);
		
		float2 toPos = worldPos - actorPos;
		float dist = length(toPos);
		
		if (dist > MAX_RIPPLE_RADIUS || dist < 0.001f)
			return result;
		
		float2 dir = toPos / dist;
		
		// Transform to movement-aligned coordinate system
		float2 perpDir = float2(-moveDir.y, moveDir.x);
		float forward = dot(toPos, moveDir);   // Distance along movement direction
		float lateral = dot(toPos, perpDir);   // Distance perpendicular to movement
		float absLateral = abs(lateral);
		float signLateral = sign(lateral);
		
		// Speed factor (normalized) with smooth ramp
		float speedFactor = smoothstep(0.0f, 250.0f, speed);
		
		// Smooth distance-based falloff
		float distFalloff = 1.0f - smoothstep(0.0f, MAX_RIPPLE_RADIUS, dist);
		distFalloff *= distFalloff;  // Quadratic for smoother fade
		
		// Near-actor boost for visible splash effect
		float nearBoost = 1.0f + smoothstep(120.0f, 0.0f, dist) * 3.0f;
		
		// === BOW WAVE (curved front wave) ===
		// Create a smooth parabolic bow shape instead of sharp V
		float bowRegion = smoothstep(-20.0f, 40.0f, forward);
		if (bowRegion > 0.01f) {
			// Parabolic distance: creates smooth curved bow shape
			// Points on the bow satisfy: forward = k * lateral^2 (parabola)
			float bowCurvature = 0.008f;  // Controls how wide the bow curves
			float idealForward = absLateral * absLateral * bowCurvature;
			float bowProximity = exp(-abs(forward - idealForward) * 0.03f);
			
			// Effective distance along the bow curve
			float bowDist = sqrt(forward * forward + lateral * lateral * 2.0f);
			float bowFalloff = smoothstep(MAX_RIPPLE_RADIUS * 0.5f, 20.0f, bowDist);
			
			// Wave emanating outward from bow
			float bowFreq = 0.10f;
			float bowSpeed = 4.5f;
			float bowPhase = bowDist * bowFreq - time * bowSpeed;
			
			float bowHeight = sin(bowPhase) * bowFalloff * bowProximity * bowRegion * speedFactor * baseStrength;
			
			// Smooth gradient along bow curve
			float2 dBowDist_dPos = float2(0, 0);
			if (bowDist > 0.001f) {
				dBowDist_dPos = (forward * moveDir + lateral * 2.0f * perpDir) / bowDist;
			}
			
			float dWave_dDist = cos(bowPhase) * bowFreq * bowFalloff * bowProximity * bowRegion;
			
			result.height += bowHeight;
			result.gradient += dBowDist_dPos * dWave_dDist * speedFactor * baseStrength;
		}
		
		// === KELVIN WAKE (smooth V-shape behind actor) ===
		float wakeRegion = smoothstep(80.0f, -30.0f, forward);
		if (wakeRegion > 0.01f) {
			float behind = max(0.0f, -forward + 30.0f);
			
			// Smooth wake arm shape using distance from ideal wake line
			// Wake arms curve slightly outward (realistic hydrodynamic behavior)
			float wakeSpread = KELVIN_ANGLE + behind * 0.0003f;  // Arms spread slightly with distance
			float idealLateral = behind * tan(wakeSpread);
			
			// Soft Gaussian falloff from wake arm centerline
			float armWidth = 30.0f + behind * 0.15f;  // Arms get wider further back
			float lateralDiff = absLateral - idealLateral;
			float armProximity = exp(-lateralDiff * lateralDiff / (armWidth * armWidth * 2.0f));
			
			// Also include inner region (between the two arms) with reduced strength
			float innerRegion = smoothstep(idealLateral, 0.0f, absLateral) * 0.4f;
			float wakeStrength = max(armProximity, innerRegion);
			
			// Transverse waves along wake arms
			float transFreq = 0.06f;
			float transSpeed = 3.0f;
			float wakeDist = sqrt(behind * behind + lateral * lateral);
			float transPhase = wakeDist * transFreq - time * transSpeed;
			
			// Smooth distance falloff
			float wakeFalloff = smoothstep(MAX_RIPPLE_RADIUS, MAX_RIPPLE_RADIUS * 0.3f, wakeDist);
			wakeFalloff *= smoothstep(0.0f, 80.0f, behind);  // Fade in behind actor
			
			float wakeHeight = sin(transPhase) * wakeStrength * wakeFalloff * wakeRegion * speedFactor * baseStrength * 0.7f;
			
			// Gradient perpendicular to wake arm direction
			float2 dWakeDist_dPos = float2(0, 0);
			if (wakeDist > 0.001f) {
				dWakeDist_dPos = (-behind * moveDir + lateral * perpDir) / wakeDist;
			}
			
			float dWakeWave = cos(transPhase) * transFreq * wakeStrength * wakeFalloff * wakeRegion;
			
			result.height += wakeHeight;
			result.gradient += dWakeDist_dPos * dWakeWave * speedFactor * baseStrength * 0.7f;
		}
		
		// === TURBULENT CENTER (smooth churning behind actor) ===
		float turbRegion = smoothstep(20.0f, -40.0f, forward) * smoothstep(100.0f, 0.0f, absLateral);
		if (turbRegion > 0.01f) {
			// Smooth elliptical falloff for turbulent region
			float turbWidth = 60.0f;
			float turbLength = 150.0f;
			float normalizedDist = sqrt(forward * forward / (turbLength * turbLength) + lateral * lateral / (turbWidth * turbWidth));
			float turbFalloff = smoothstep(1.0f, 0.0f, normalizedDist);
			
			// Multiple overlapping frequencies for organic churning
			float turbPhase1 = dist * 0.15f - time * 6.0f + hash(actorPos) * 6.283f;
			float turbPhase2 = dist * 0.11f - time * 4.5f + hash(actorPos.yx) * 6.283f;
			float turbPhase3 = dist * 0.08f - time * 3.0f + hash(actorPos + float2(1, 1)) * 6.283f;
			
			float turbHeight = (sin(turbPhase1) * 0.5f + sin(turbPhase2) * 0.3f + sin(turbPhase3) * 0.2f);
			turbHeight *= turbFalloff * turbRegion * speedFactor * baseStrength * 0.6f;
			
			// Smooth radial gradient
			float dTurb = (cos(turbPhase1) * 0.15f * 0.5f + cos(turbPhase2) * 0.11f * 0.3f + cos(turbPhase3) * 0.08f * 0.2f);
			
			result.height += turbHeight;
			result.gradient += dir * dTurb * turbFalloff * turbRegion * speedFactor * baseStrength * 0.6f;
		}
		
		// Apply overall smooth modulation
		result.height *= distFalloff * nearBoost;
		result.gradient *= distFalloff * nearBoost;
		
		return result;
	}
	
	// Gentle circular ripples for stationary actors
	WaveResult GetStationaryRipples(float2 actorPos, float2 worldPos, float time, float rippleStrength)
	{
		WaveResult result;
		result.height = 0.0f;
		result.gradient = float2(0, 0);
		
		float2 toPos = worldPos - actorPos;
		float dist = length(toPos);
		
		if (dist > MAX_RIPPLE_RADIUS * 0.5f)
			return result;
		
		// Distance falloff
		float falloff = 1.0f - smoothstep(0.0f, MAX_RIPPLE_RADIUS * 0.4f, dist);
		
		// Gentle breathing ripples - two slow frequencies
		WaveResult wave1 = CircularWave(toPos, 0.06f, 1.2f, time, 0.25f * rippleStrength);
		WaveResult wave2 = CircularWave(toPos, 0.09f, 1.8f, time, 0.15f * rippleStrength);
		
		result.height = (wave1.height + wave2.height) * falloff;
		result.gradient = (wave1.gradient + wave2.gradient) * falloff;
		
		return result;
	}
	
	// Convert gradient to normal vector
	float3 GradientToNormal(float2 gradient, float strength)
	{
		// Normal = normalize(-dH/dx, -dH/dy, 1)
		// The negative sign because gradient points uphill, but normal points up
		return normalize(float3(-gradient * strength, 1.0f));
	}

	// Reorient a ripple normal onto the base water normal using UDN blending
	float3 ApplyRippleNormal(float3 rippleNormal, float3 baseNormal)
	{
		// UDN (Unreal Developer Network) normal blending
		float3 result;
		result.xy = baseNormal.xy + rippleNormal.xy;
		result.z = baseNormal.z;
		return normalize(result);
	}
	
	// Calculate ripples from a single actor
	// Returns float4(normal.xyz, height)
	float4 GetActorRipples(float2 actorPos, float2 worldPos, float time, float2 velocity, float inWater, float waterDepth)
	{
		if (inWater < 0.5f)
			return float4(0, 0, 1, 0);
		
		// Don't create ripples if actor is fully submerged
		if (waterDepth > 100.0f)
			return float4(0, 0, 1, 0);
			
		float2 toPos = worldPos - actorPos;
		float dist = length(toPos);
		
		if (dist > MAX_RIPPLE_RADIUS)
			return float4(0, 0, 1, 0);
		
		float actualSpeed = length(velocity);
		float movementThreshold = 15.0f;  // Units/sec
		
		float baseStrength = lerp(0.6f, 1.8f, saturate(actualSpeed / 250.0f));
		
		WaveResult wave;
		if (actualSpeed < movementThreshold) {
			wave = GetStationaryRipples(actorPos, worldPos, time, baseStrength * 0.4f);
		} else {
			float2 moveDir = normalize(velocity);
			wave = EvaluateWakeRipples(worldPos, actorPos, moveDir, time, actualSpeed, baseStrength);
		}
		
		float3 normal = GradientToNormal(wave.gradient, 1.5f);
		return float4(normal, wave.height);
	}
	
	// Main function: Calculate ripples from ALL actors (player + NPCs)
	// Returns float4(blended_normal.xyz, total_height)
	float4 GetAllActorRipples(float2 worldPos, float time, float2 playerPos, float2 playerVelocity, float playerInWater, float playerWaterDepth)
	{
		// Accumulate gradients (not normals!) for correct blending
		float2 totalGradient = float2(0, 0);
		float totalHeight = 0.0f;
		
		// Player ripples
		if (playerInWater > 0.5f && playerWaterDepth <= 100.0f) {
			float actualSpeed = length(playerVelocity);
			float movementThreshold = 15.0f;
			
			float baseStrength = lerp(0.8f, 2.2f, saturate(actualSpeed / 250.0f));
			
			WaveResult playerWave;
			if (actualSpeed < movementThreshold) {
				playerWave = GetStationaryRipples(playerPos, worldPos, time, baseStrength * 0.4f);
			} else {
				float2 moveDir = normalize(playerVelocity);
				playerWave = EvaluateWakeRipples(worldPos, playerPos, moveDir, time, actualSpeed, baseStrength);
			}
			
			totalGradient += playerWave.gradient;
			totalHeight += playerWave.height;
		}
		
		// NPC ripples
		uint actorCount = min(NumActorRipples, MAX_ACTOR_RIPPLES);
		for (uint i = 0; i < actorCount; i++) {
			ActorRippleData actor = ActorRipples[i];
			
			if (actor.InWater < 0.5f || actor.WaterDepth > 100.0f)
				continue;
			
			float2 actorPos = float2(actor.PosX, actor.PosY);
			float2 actorVelocity = float2(actor.VelocityX, actor.VelocityY);
			float actualSpeed = length(actorVelocity);
			
			float2 toActor = worldPos - actorPos;
			if (length(toActor) > MAX_RIPPLE_RADIUS)
				continue;
			
			float baseStrength = lerp(0.6f, 1.8f, saturate(actualSpeed / 250.0f));
			
			WaveResult actorWave;
			if (actualSpeed < 15.0f) {
				actorWave = GetStationaryRipples(actorPos, worldPos, time, baseStrength * 0.4f);
			} else {
				float2 moveDir = normalize(actorVelocity);
				actorWave = EvaluateWakeRipples(worldPos, actorPos, moveDir, time, actualSpeed, baseStrength);
			}
			
			totalGradient += actorWave.gradient;
			totalHeight += actorWave.height;
		}
		
		// Convert accumulated gradient to final normal
		float3 resultNormal = GradientToNormal(totalGradient, 1.5f);
		
		return float4(resultNormal, totalHeight);
	}
}

#endif // __WATER_ACTOR_RIPPLES_HLSLI__
