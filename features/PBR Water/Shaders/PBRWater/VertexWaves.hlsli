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
	/// What the geometry needs of the waves at one vertex.
	struct VertexWaves
	{
		SurfaceDisplacement surface;
		float flowDamping;    // the river current's calming of the open-water waves (WaveParam.w)
		float2 flowVelocity;  // the river current (units/s)
	};

	// The deferred surface (Deferred.hlsli) only takes the struct: its pixel stage has the engine's flowmap in t8.
#if !defined(PSHADER)
	SamplerState FlowMapSamplerVS : register(s8);
	Texture2D<float4> FlowMapTexVS : register(t8);

	/**
	 * @param positionWS undisplaced camera-relative position of the vertex
	 * @param spacing    local vertex spacing (units) the waves are filtered to
	 * @param motion     also work out last frame's surface (motion vectors)
	 */
	VertexWaves EvaluateVertexWaves(float3 positionWS, float spacing, bool motion)
	{
		VertexWaves o;
		o.flowDamping = FlowDampingAt(FlowMapTexVS, FlowMapSamplerVS, positionWS.xy + FrameBuffer::CameraPosAdjust.xy, o.flowVelocity);
		o.surface = DisplaceSurface(positionWS, o.flowDamping, o.flowVelocity, spacing, motion);
		return o;
	}
#endif
}

#endif  // __PBR_WATER_VERTEX_WAVES_HLSLI__
