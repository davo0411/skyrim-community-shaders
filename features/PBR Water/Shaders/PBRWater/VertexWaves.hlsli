#ifndef __PBR_WATER_VERTEX_WAVES_HLSLI__
#define __PBR_WATER_VERTEX_WAVES_HLSLI__

// ============================================================================
// PBR Water - the waves at one vertex of the surface.
//
// The geometry's whole wave program: the river current read from the worldspace flowmap, then DisplaceSurface. The water
// vertex shader runs it for untessellated draws, and the compute tessellator (TessellationCS) runs it once a frame for
// every vertex it makes, which every tessellated pass then draws. One function, so the two can never drift apart.
// Needs PBRWater.hlsli.
// ============================================================================

namespace PBRWater
{
	SamplerState FlowMapSamplerVS : register(s8);
	Texture2D<float4> FlowMapTexVS : register(t8);

	/**
	 * River current flattens the open-water waves. The worldspace flowmap is looked up by world position,
	 * not by mesh UVs: the stencil pass (depth and motion vectors) is compiled without FLOWMAP and must
	 * displace the surface exactly like the colour pass, and neighbouring meshes must agree on shared edges.
	 * The same sample gives the current the waves are carried along (PBRWater::FlowPhases).
	 */
	float GetWaveFlowDamping(float3 positionWS, out float2 velocity)
	{
		velocity = 0.0;
		if (Flow0.x == 0.0)
			return 1.0;
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		float4 flow = FlowMapTexVS.SampleLevel(FlowMapSamplerVS, absXY * Flow0.xz + Flow0.yw, 0);
		velocity = FlowVelocity(flow);
		return 1.0 - Foam1.w * saturate(flow.z * flow.w);
	}

	/// What the geometry needs of the waves at one vertex.
	struct VertexWaves
	{
		SurfaceDisplacement surface;
		float flowDamping;    // the river current's calming of the open-water waves (WaveParam.w)
		float2 flowVelocity;  // the river current (units/s)
	};

	/**
	 * @param positionWS undisplaced camera-relative position of the vertex
	 * @param spacing    local vertex spacing (units) the waves are filtered to
	 * @param motion     also work out last frame's surface (motion vectors)
	 */
	VertexWaves EvaluateVertexWaves(float3 positionWS, float spacing, bool motion)
	{
		VertexWaves o;
		o.flowDamping = GetWaveFlowDamping(positionWS, o.flowVelocity);
		o.surface = DisplaceSurface(positionWS, o.flowDamping, o.flowVelocity, spacing, motion);
		return o;
	}
}

#endif  // __PBR_WATER_VERTEX_WAVES_HLSLI__
