#ifndef __PBR_WATER_VERTEX_WAVES_HLSLI__
#define __PBR_WATER_VERTEX_WAVES_HLSLI__

// ============================================================================
// PBR Water - the waves at one vertex of the surface.
//
// The geometry's whole wave program: DisplaceSurface. The water vertex shader runs it for the engine's meshes, and the
// projected grid (ProjectedGridCS) runs it once a frame for every vertex it lays out, which every pass of the frame then
// draws. One function, so the two never drift apart. Needs PBRWater.hlsli.
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
	 * @param lipSpacing the spacing a breaker's lip is filtered to (< 0: `spacing`; DisplaceSurface)
	 */
	VertexWaves EvaluateVertexWaves(float3 positionWS, float river, float spacing, bool motion, float lipSpacing = -1.0)
	{
		VertexWaves o;
		o.river = river;
		o.surface = DisplaceSurface(positionWS, river, spacing, motion, lipSpacing);
		return o;
	}
#endif
}

#endif  // __PBR_WATER_VERTEX_WAVES_HLSLI__
