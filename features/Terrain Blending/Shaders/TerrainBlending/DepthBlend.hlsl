RWTexture2D<float> BlendedDepthTexture : register(u0);
RWTexture2D<unorm float> BlendedDepthTexture16 : register(u1);

// Unmodified copy of the main depth, read by landscape passes as TerrainBlendingMaskTexture (t55).
// Written here so no separate depth-to-depth CopyResource is needed.
RWTexture2D<float> MainDepthCopy : register(u2);

Texture2D<unorm float> MainDepthTexture : register(t0);
Texture2D<unorm float> TerrainDepthTexture : register(t1);

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	float mainDepth = MainDepthTexture[DTid.xy];
	float mixedDepth = min(mainDepth, TerrainDepthTexture[DTid.xy]);
	BlendedDepthTexture[DTid.xy] = mixedDepth;
	BlendedDepthTexture16[DTid.xy] = mixedDepth;
	MainDepthCopy[DTid.xy] = mainDepth;
}
