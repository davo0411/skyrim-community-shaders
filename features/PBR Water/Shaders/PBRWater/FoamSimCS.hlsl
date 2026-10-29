// ============================================================================
// PBR Water - the foam on the water around the camera.
//
// The whitecap opacity map of Tessendorf, Reinhard & Gao (2023) and Crest's foam simulation (wave-harmonic/crest, MIT,
// UpdateFoam.compute): where waves break, foam is written (PBRWater::BreakingFoam, PBRWater::SwashFoam), and
// everywhere it decays. Whitecap foam decays exponentially, with an area-weighted time scale of 1.4 to 4.8 s in the
// open ocean (Callaghan, Deane & Stokes 2012); thicker foam, such as that of the bores of the surf zone, takes longer to
// thin out (the coverage is the thickness, clamped at 1). Freshness decays within the second a crest is actively
// breaking (stage A).
//
// The grid is indexed by the undisplaced (Lagrangian) position, as the water shaders sample it: the foam stays with
// the water it lies on while the crests run on, so a breaking crest leaves a trail. Two levels, the second
// FoamSimulation::LevelRatio times coarser, each scrolled with the camera in whole texels so the state is never
// resampled; what scrolls into the fine level is taken from the coarse one.
//
// Output: x foam thickness, y freshness.
// ============================================================================

#include "PBRWater/PBRWater.hlsli"

cbuffer FoamSimCB : register(b0)
{
	int4 Shift;     // xy fine level: texels the grid scrolled since the last step, zw coarse level
	float4 Origin;  // xy fine level grid corner (absolute units), zw coarse level
	float4 Texel;   // x fine level texel (units), y coarse level texel, z grid size, w unused
	float4 Decay;   // x thickness retained this step, y freshness retained this step, z water level where the shore field has none (absolute z), w unused
};

Texture2DArray<float2> Previous : register(t0);
RWTexture2DArray<float2> Current : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint size = (uint)Texel.z;
	if (any(id.xy >= size))
		return;
	uint level = id.z;
	float texel = level == 0 ? Texel.x : Texel.y;
	float2 origin = level == 0 ? Origin.xy : Origin.zw;
	int2 shift = level == 0 ? Shift.xy : Shift.zw;
	float2 absXY = origin + (float2(id.xy) + 0.5) * texel;

	// Last step's foam here, decayed.
	float2 state = 0.0;
	int2 source = int2(id.xy) + shift;
	if (all(source >= 0) && all(source < (int)size)) {
		state = Previous.Load(int4(source, level, 0));
	} else if (level == 0) {
		float2 uv = (absXY - (Origin.zw - float2(Shift.zw) * Texel.y)) / (Texel.y * size);
		if (all(uv > 0.0) && all(uv < 1.0))
			state = Previous.SampleLevel(PBRWater::LinearClampSampler, float3(uv, 1.0), 0);
	}
	state *= Decay.xy;

	// The water here: the shore field's still water level (it knows each lake's and the sea's), else the camera's.
	float waterZ = PBRWater::ShoreLevel(absXY);
	if (waterZ < -1e6)
		waterZ = Decay.z;
	float3 positionWS = float3(absXY, waterZ) - FrameBuffer::CameraPosAdjust.xyz;
	PBRWater::WaveContext ctx = PBRWater::BuildWaveContext(positionWS, PBRWater::DisplacementFade(positionWS));
	PBRWater::WaveResult waves = PBRWater::EvaluateWaves(positionWS, ctx, texel, false, true);
	float2 tip = float2(-1e6, 0.0);
	[branch] if (ctx.beach > 0.0)
		tip = PBRWater::SwashTip(positionWS, ctx, false);

	// Foam is only made on water: the shore train runs on up the beach (it carries the waterline's phase there), but
	// breaks only where its surface or the swash sheet covers the bed.
	float surface = max(waves.displacement.z, PBRWater::SwashTop(ctx.elevation, tip.x, tip.y));
	float wet = smoothstep(0.0, 0.03 * PBRWater::UnitsPerMetre, surface - ctx.elevation);
	float2 made = PBRWater::BreakingFoam(waves, ctx);
	// Left on the sand the foam drains into it and bursts: it thins three times as fast as on the water, or a heavy surf
	// whitened the whole beach for most of a minute.
	state.x *= lerp(Decay.x * Decay.x, 1.0, wet);
	state = max(state, made * wet);
	Current[id] = state;
}
