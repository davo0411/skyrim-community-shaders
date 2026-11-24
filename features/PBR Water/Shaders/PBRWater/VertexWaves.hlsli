#ifndef __PBR_WATER_VERTEX_WAVES_HLSLI__
#define __PBR_WATER_VERTEX_WAVES_HLSLI__

// ============================================================================
// PBR Water - the waves at one vertex of the surface.
//
// The geometry's whole wave program: DisplaceSurface. The water vertex shader runs it for untessellated draws, and the
// bisection tessellator (BisectionCS VertexCS) runs it once a frame for every vertex it makes, which every tessellated
// pass then draws. One function, so the two never drift apart. Needs PBRWater.hlsli.
// ============================================================================

namespace PBRWater
{
	/// What the geometry needs of the waves at one vertex.
	struct VertexWaves
	{
		SurfaceDisplacement surface;
		float river;  // 1 on a river's mesh (WaveParam.w)
	};

#if !defined(PSHADER)
	/**
	 * @param positionWS undisplaced camera-relative position of the vertex
	 * @param river      1 on a river's mesh: the per-mesh bit (the stencil pass is compiled without FLOWMAP, so the vanilla define cannot say)
	 * @param spacing    local vertex spacing (units) the waves are filtered to
	 * @param motion     also work out last frame's surface (motion vectors)
	 */
	VertexWaves EvaluateVertexWaves(float3 positionWS, float river, float spacing, bool motion)
	{
		VertexWaves o;
		o.river = river;
		o.surface = DisplaceSurface(positionWS, river, spacing, motion);
		return o;
	}
#endif
}

#endif  // __PBR_WATER_VERTEX_WAVES_HLSLI__
