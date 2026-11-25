// ============================================================================
// PBR Water - the sea's projected grid's vertices (ProjectedGrid::Build), once a frame for every pass that draws it.
//
// Each vertex is a point of the screen, (columns + 1) x (rows + 1) of them spread evenly over the part of the screen the sea
// can cover (the rows end at the horizon), cast onto the sea's plane: as dense near the camera as at the horizon, in pixels.
// The waves are evaluated there as the tessellator evaluates its vertices (BisectionCS VertexCS), filtered to the grid's
// spacing on the plane at that point, so a cell never carries a wave shorter than itself: the vertices slide over the sea
// as the camera turns, and only what they can resolve moves with them. The vertex holds its camera-relative position
// (Water.hlsl GridWaterVertex brings it into the draw's model space).
// ============================================================================

#include "PBRWater/PBRWater.hlsli"
#include "PBRWater/ComputeTessellation.hlsli"
#include "PBRWater/ProjectedGrid.hlsli"
#include "PBRWater/VertexWaves.hlsli"

cbuffer ProjectedGridCB : register(b0)
{
	row_major float4x4 ClipToWorld;  // camera-relative inverse view-projection, unjittered
	uint4 Size;                      // x columns, y rows
	float4 Range;                    // xy the grid's NDC x from and to, zw its NDC y from and to
	float4 Plane;                    // x the sea's level relative to the camera (units, never within a unit of 0), y the farthest a vertex is cast (units)
};

RWStructuredBuffer<PBRWaterTess::Vertex> Vertices : register(u0);

/// Where the ray through NDC `ndc` meets the sea's plane, camera-relative. A ray that misses it (past the horizon) or meets
/// it beyond Plane.y gives the point Plane.y away on the plane in its direction: the grid's last rows fold onto that ring.
float3 Cast(float2 ndc)
{
	float4 p = mul(ClipToWorld, float4(ndc, 0.5, 1.0));
	float3 dir = normalize(p.xyz / p.w);
	float level = Plane.x;
	float3 far = float3(dir.xy * (Plane.y / max(length(dir.xy), 1e-6)), level);
	if (dir.z * level <= 0.0)
		return far;
	float3 hit = dir * (level / dir.z);
	return dot(hit.xy, hit.xy) < Plane.y * Plane.y ? hit : far;
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy > Size.xy))
		return;
	float2 step = float2(Range.y - Range.x, Range.w - Range.z) / float2(Size.xy);
	float2 ndc = float2(Range.x, Range.z) + float2(id.xy) * step;
	float3 p = Cast(ndc);
	// The cell's longer side on the plane: the waves shorter than it are filtered out of the displacement.
	float spacing = max(distance(Cast(ndc + float2(step.x, 0.0)), p), distance(Cast(ndc + float2(0.0, step.y)), p));
	PBRWater::VertexWaves waves = PBRWater::EvaluateVertexWaves(p, 0.0, max(spacing, 1.0), true);

	float2 absXY = p.xy + FrameBuffer::CameraPosAdjust.xy;
	PBRWaterTess::Vertex o;
	o.position = p;
	o.river = 0.0;
	o.current = waves.surface.current;
	o.previous = waves.surface.previous;
	o.texCoordX = dot(PBRWater::Proj2.xy, absXY) + PBRWater::Proj2.z;
	o.texCoordY = dot(PBRWater::Proj3.xy, absXY) + PBRWater::Proj3.z;
	o.shore = waves.surface.shore;
	o.color = uint2(0x3C003C00, 0x3C003C00);  // white, alpha 1 (half floats)
	Vertices[PBRWaterGrid::VertexIndex(id.xy, Size.x)] = o;
}
