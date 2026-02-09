
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

cbuffer PerFrameCB : register(b0)
{
	float2 PosOffset;  // cell origin in camera space
	uint2 ArrayOrigin; // xy: array origin (clipmap wrapping)

	int2 ValidMargin;
	float TimeDelta;
	uint BoundingBoxCount;

	float CameraHeightDelta;
}

struct BoundingBoxPacked
{
	float2 MinExtent;
	float2 MaxExtent;
	uint IndexStart;
	uint IndexEnd;
	float2 pad0;
};

StructuredBuffer<BoundingBoxPacked> CollisionBoundingBoxes : register(t0);

StructuredBuffer<float4> CollisionInstances : register(t1);

RWTexture2D<float4> Collision : register(u0);

groupshared BoundingBoxPacked SharedBoundingBoxes[64];

[numthreads(8, 8, 1)] void main(
	uint3 groupId
	: SV_GroupID, uint3 dispatchThreadId
	: SV_DispatchThreadID, uint3 groupThreadId
	: SV_GroupThreadID, uint groupIndex
	: SV_GroupIndex) {

	if (groupIndex < BoundingBoxCount)
		SharedBoundingBoxes[groupIndex] = CollisionBoundingBoxes[groupIndex];

	GroupMemoryBarrierWithGroupSync();

	const uint TEXTURE_SIZE = 4096;
	const float WORLD_SIZE = 4096;
	float2 ZRANGE = float2(2048.0, -2048.0);

	uint2 cellID = uint2(max(int2(dispatchThreadId.xy) - ArrayOrigin, 0) % TEXTURE_SIZE);

	float2 cellCentreMS = cellID + 0.5 - TEXTURE_SIZE / 2;
	cellCentreMS = cellCentreMS / TEXTURE_SIZE * WORLD_SIZE + PosOffset.xy;

	uint2 validMin = (uint2)max(0, ValidMargin.xy);
	uint2 validMax = TEXTURE_SIZE - 1 + (uint2)min(0, ValidMargin.xy);
	bool isValid = all(cellID >= validMin) && all(cellID <= validMax);

	float2 collision = max(ZRANGE.x, ZRANGE.y);
	float2 previousCollision = collision;

	float2 fadeRate = TimeDelta * 100 * float2(0.002, 0.2);

	if (isValid) {
		previousCollision = Collision[dispatchThreadId.xy];
		previousCollision = lerp(ZRANGE.x, ZRANGE.y, previousCollision);
		previousCollision += CameraHeightDelta;
		collision = previousCollision + fadeRate;
	}

	for (uint i = 0; i < BoundingBoxCount; i++) {
		BoundingBoxPacked boundingBox = SharedBoundingBoxes[i];
		if (all(cellCentreMS >= boundingBox.MinExtent && cellCentreMS <= boundingBox.MaxExtent)){
			for (uint j = boundingBox.IndexStart; j < boundingBox.IndexEnd; j++) {
				float4 collisionInstance = CollisionInstances[j];
				float baseRadius = collisionInstance.w * 0.5 * 1.2;
				float centerHeight = collisionInstance.z;
				
				if (baseRadius < 9.6)
					continue;
				
				if (centerHeight - baseRadius < collision.y){
					float dist = distance(collisionInstance.xy, cellCentreMS);
					
					// Extend influence by 1 cell beyond the radius so boundary-adjacent
					// cells store centerHeight (zero depression) instead of the sentinel
					// value. This prevents bilinear interpolation in the consumer from
					// blending collision heights with the 2048 sentinel, which causes
					// pixelated staircase edges on the snow parallax deformity.
					float CELL_SIZE_LOCAL = WORLD_SIZE / TEXTURE_SIZE;
					if (dist < baseRadius + CELL_SIZE_LOCAL) {
						float t = dist / baseRadius;
						float depthFactor = max(0.0, 1.0 - t * t);
						
						float depth = baseRadius * 0.4 * depthFactor * 0.8;
						float height = centerHeight - depth;
						
						collision.x = min(collision.x, height);
						if (height < collision.y) {
							collision.y = height;
						}
					}
				}
			}
		}
	}

	collision = (collision - ZRANGE.x) / (ZRANGE.y - ZRANGE.x);
	previousCollision = (previousCollision - ZRANGE.x) / (ZRANGE.y - ZRANGE.x);

	Collision[dispatchThreadId.xy] = float4(collision, previousCollision);
}