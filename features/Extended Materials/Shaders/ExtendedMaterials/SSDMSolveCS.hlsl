// SSDM (Lobel 2008, §2.3): hierarchical init + damped Picard fixed-point on the mip0 displacement
// field. For each screen pixel it solves t = uvBuf + duv(t), i.e. the source UV that displaces TO
// this pixel (the paper's barycenter, refined coarse->fine). Writes absolute normalized fetch UV in
// the full buffer [0,1] (matches DeferredComposite remapCoord).
// SurfaceWidth/Height = dynamic render extent (matches Util::GetScreenDispatchCount + upscaling/DRS).
// .z = 1 when solve completed for a valid on-surface pixel.
// .w = averaged displacement coverage from the forward pass.

// Scalar-only layout - must match ExtendedMaterials::SSDMSolveCB in ExtendedMaterials.h (64 bytes).
cbuffer SSDMSolveCB : register(b0)
{
	float SurfaceWidth;
	float SurfaceHeight;
	float BufferWidth;
	float BufferHeight;
	float RcpBufferWidth;
	float RcpBufferHeight;
	float MaxStepUv;
	float Damping;
	int NumMips;
	int NumIters;
	int Pad0;
	int Pad1;
	int Pad2;
	int Pad3;
	int Pad4;
	int Pad5;
};

Texture2D<float4> DuvPyramid : register(t0);
SamplerState PointSampler : register(s0);
RWTexture2D<float4> OutAbsUV : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid
								: SV_DispatchThreadID) {
	if (float(dtid.x) >= SurfaceWidth || float(dtid.y) >= SurfaceHeight)
		return;

	float2 rcpBufferDim = float2(RcpBufferWidth, RcpBufferHeight);
	float2 uvBuf = (float2(dtid.xy) + 0.5) * rcpBufferDim;

	float coarseMip = max(0.0, float(NumMips - 1));
	float2 tCoarse = uvBuf + DuvPyramid.SampleLevel(PointSampler, uvBuf, coarseMip).xy;
	float2 t = saturate(tCoarse);

	float maxStepUv = MaxStepUv;
	float damping = Damping;

	int iters = min(max(NumIters, 1), 16);
	[loop] for (int i = 0; i < iters; ++i)
	{
		float2 d = DuvPyramid.SampleLevel(PointSampler, t, 0).xy;
		float2 picard = uvBuf + d;
		float2 delta = picard - t;
		delta = clamp(delta, -maxStepUv.xx, maxStepUv.xx);
		float2 next = t + delta;
		next = lerp(t, next, damping);
		t = saturate(next);
	}

	// Coverage/validity come from the SOLVED SOURCE (the texel the displacement pulled from), NOT the
	// dest. This is what lets a pixel with no seed of its own - a farther surface OR open sky - be
	// claimed by a foreground silhouette skirt: the dest is empty, but the source it resolves to is a
	// covered foreground texel. The composite then confirms with a depth test that the source is nearer.
	float srcCoverage = DuvPyramid.SampleLevel(PointSampler, t, 0).z;
	float validZ = srcCoverage > 0.0 ? 1.0 : 0.0;

	OutAbsUV[int2(dtid.xy)] = float4(t.xy, validZ, srcCoverage);
}
