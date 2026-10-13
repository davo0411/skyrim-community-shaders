#include "HullMaskAtlas.h"

#include "Features/Effects11/D3D11StateBackup.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <bit>

namespace
{
	constexpr uint32_t BuildsPerFrame = 2;        // a hull coming near costs a few draws and a few dozen tiny dispatches, once
	constexpr uint32_t FloodRounds = 6;           // sweeps in all four directions: plenty for any hull-like basin
	constexpr uint32_t MaxRawBytes = 64u << 20;   // larger meshes are left out
	constexpr uint32_t VerticesPerTriangle = 21;  // the triangle and a quad per edge (HullMask.hlsl)
	constexpr float Far = 1e30f;                  // no surface (HullMask.hlsl)

	std::unique_ptr<Texture2D> CreateMap(DXGI_FORMAT format, uint32_t slices, UINT bindFlags, const char* name)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = HullMaskAtlas::Size;
		desc.Height = HullMaskAtlas::Size;
		desc.MipLevels = 1;
		desc.ArraySize = slices;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = bindFlags;
		auto texture = std::make_unique<Texture2D>(desc, name);

		if (bindFlags & D3D11_BIND_SHADER_RESOURCE) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = format;
			if (slices > 1) {
				srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				srv.Texture2DArray.MipLevels = 1;
				srv.Texture2DArray.ArraySize = slices;
			} else {
				srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srv.Texture2D.MipLevels = 1;
			}
			texture->CreateSRV(srv);
		}
		if (bindFlags & D3D11_BIND_UNORDERED_ACCESS) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = format;
			if (slices > 1) {
				uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
				uav.Texture2DArray.ArraySize = slices;
			} else {
				uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			}
			texture->CreateUAV(uav);
		}
		if (bindFlags & D3D11_BIND_RENDER_TARGET) {
			D3D11_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = format;
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			texture->CreateRTV(rtv);
		}
		return texture;
	}
}

void HullMaskAtlas::SetupResources()
{
	auto device = globals::d3d::device;

	atlas = CreateMap(DXGI_FORMAT_R32G32_FLOAT, Slots, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, "PBRWater::HullMaskAtlas");
	bottom = CreateMap(DXGI_FORMAT_R32_FLOAT, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "PBRWater::HullMaskBottom");
	top = CreateMap(DXGI_FORMAT_R32_FLOAT, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "PBRWater::HullMaskTop");
	image = CreateMap(DXGI_FORMAT_R32_FLOAT, 1, D3D11_BIND_UNORDERED_ACCESS, "PBRWater::HullMaskImage");
	level = CreateMap(DXGI_FORMAT_R32_FLOAT, 1, D3D11_BIND_UNORDERED_ACCESS, "PBRWater::HullMaskLevel");
	constants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<MaskCB>(), "PBRWater::HullMaskCB");

	// Lowest surface in target 0, highest in target 1.
	D3D11_BLEND_DESC blendDesc{};
	blendDesc.IndependentBlendEnable = TRUE;
	for (uint32_t i = 0; i < 2; ++i) {
		auto& target = blendDesc.RenderTarget[i];
		target.BlendEnable = TRUE;
		target.SrcBlend = target.DestBlend = target.SrcBlendAlpha = target.DestBlendAlpha = D3D11_BLEND_ONE;
		target.BlendOp = target.BlendOpAlpha = i == 0 ? D3D11_BLEND_OP_MIN : D3D11_BLEND_OP_MAX;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	}
	DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, blendState.put()));
	Util::SetResourceName(blendState.get(), "PBRWater::HullMaskBlend");

	// Both faces, nothing clipped in height.
	D3D11_RASTERIZER_DESC rasterizerDesc{};
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.CullMode = D3D11_CULL_NONE;
	rasterizerDesc.DepthClipEnable = FALSE;
	DX::ThrowIfFailed(device->CreateRasterizerState(&rasterizerDesc, rasterizerState.put()));
	Util::SetResourceName(rasterizerState.get(), "PBRWater::HullMaskRasterizer");

	D3D11_DEPTH_STENCIL_DESC depthDesc{};
	depthDesc.DepthEnable = FALSE;
	depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
	DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthState.put()));
	Util::SetResourceName(depthState.get(), "PBRWater::HullMaskDepth");

	ClearShaderCache();
}

void HullMaskAtlas::ClearShaderCache()
{
	constexpr auto path = L"Data\\Shaders\\PBRWater\\HullMask.hlsl";
	rasterVS = nullptr;
	rasterPS = nullptr;
	prepareCS = nullptr;
	floodCS = nullptr;
	packCS = nullptr;
	rasterVS.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(path, {}, "vs_5_0")));
	rasterPS.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(path, {}, "ps_5_0")));
	prepareCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path, {}, "cs_5_0", "Prepare")));
	floodCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path, {}, "cs_5_0", "Flood")));
	packCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path, {}, "cs_5_0", "Pack")));
	ready = rasterVS && rasterPS && prepareCS && floodCS && packCS && atlas && constants;
	if (!ready)
		logger::warn("[PBR Water] Hull mask shaders failed to compile; the waves show inside boats");
}

int HullMaskAtlas::FindSlot(const FloatingObjects::HullShape* shape) const
{
	if (!ready || !shape)
		return -1;
	for (uint32_t i = 0; i < Slots; ++i) {
		if (slots[i].get() == shape)
			return static_cast<int>(i);
	}
	return -1;
}

void HullMaskAtlas::Update(const std::vector<FloatingObjects::HullMask>& masks)
{
	if (!ready)
		return;

	std::array<bool, Slots> wanted{};
	std::vector<const std::shared_ptr<const FloatingObjects::HullShape>*> missing;
	for (const auto& mask : masks) {
		if (const int slot = FindSlot(mask.shape.get()); slot >= 0)
			wanted[slot] = true;
		else if (mask.shape && !mask.shape->pieces.empty())
			missing.push_back(&mask.shape);
	}
	// A shape only this atlas still holds belongs to a hull that is gone: let its engine buffers go.
	for (uint32_t i = 0; i < Slots; ++i) {
		if (!wanted[i] && slots[i] && slots[i].use_count() == 1)
			slots[i].reset();
	}
	if (missing.empty())
		return;

	auto context = globals::d3d::context;
	Effects11Util::D3D11FullStateBackup backup;
	backup.Save(context);
	winrt::com_ptr<ID3D11HullShader> savedHS;
	winrt::com_ptr<ID3D11DomainShader> savedDS;
	winrt::com_ptr<ID3D11GeometryShader> savedGS;
	context->HSGetShader(savedHS.put(), nullptr, nullptr);
	context->DSGetShader(savedDS.put(), nullptr, nullptr);
	context->GSGetShader(savedGS.put(), nullptr, nullptr);

	uint32_t built = 0;
	for (const auto* shape : missing) {
		if (built == BuildsPerFrame)
			break;
		// An empty slot, else one no published hull uses.
		uint32_t slot = Slots;
		for (uint32_t i = 0; i < Slots && slot == Slots; ++i) {
			if (!wanted[i] && !slots[i])
				slot = i;
		}
		for (uint32_t i = 0; i < Slots && slot == Slots; ++i) {
			if (!wanted[i])
				slot = i;
		}
		if (slot == Slots)
			break;
		Build(slot, **shape);
		slots[slot] = *shape;
		wanted[slot] = true;
		++built;
	}

	backup.Restore(context);
	backup.Release();
	context->HSSetShader(savedHS.get(), nullptr, 0);
	context->DSSetShader(savedDS.get(), nullptr, 0);
	context->GSSetShader(savedGS.get(), nullptr, 0);
}

bool HullMaskAtlas::CopyRaw(std::unique_ptr<Buffer>& raw, ID3D11Buffer* source, const char* name)
{
	D3D11_BUFFER_DESC sourceDesc{};
	source->GetDesc(&sourceDesc);
	const uint32_t bytes = sourceDesc.ByteWidth;
	if (bytes == 0 || bytes > MaxRawBytes)
		return false;
	if (!raw || raw->desc.ByteWidth < bytes) {
		const uint32_t capacity = std::max(std::bit_ceil(bytes), 64u << 10);
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = capacity;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		raw = std::make_unique<Buffer>(desc, nullptr, name);
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_R32_TYPELESS;
		srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
		srv.BufferEx.NumElements = capacity / 4;
		srv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
		raw->CreateSRV(srv);
	}
	const D3D11_BOX box{ 0, 0, 0, bytes, 1, 1 };
	globals::d3d::context->CopySubresourceRegion(raw->resource.get(), 0, 0, 0, 0, source, 0, &box);
	return true;
}

void HullMaskAtlas::Build(uint32_t slot, const FloatingObjects::HullShape& shape)
{
	auto context = globals::d3d::context;
	MaskCB cb{};
	cb.projection = { shape.minX, shape.minY, static_cast<float>(Size) / (shape.maxX - shape.minX), static_cast<float>(Size) / (shape.maxY - shape.minY) };
	cb.size = Size;
	cb.ceiling = shape.maxZ;
	cb.slice = slot;
	ID3D11Buffer* cbs[1] = { constants->CB() };

	// Every piece seen from straight above: the lowest and the highest surface per texel.
	const float clearBottom[4] = { Far, Far, Far, Far };
	const float clearTop[4] = { -Far, -Far, -Far, -Far };
	context->ClearRenderTargetView(bottom->rtv.get(), clearBottom);
	context->ClearRenderTargetView(top->rtv.get(), clearTop);
	ID3D11RenderTargetView* rtvs[2] = { bottom->rtv.get(), top->rtv.get() };
	context->OMSetRenderTargets(2, rtvs, nullptr);
	context->OMSetBlendState(blendState.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(depthState.get(), 0);
	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(Size), static_cast<float>(Size), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->RSSetState(rasterizerState.get());
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(rasterVS.get(), nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(rasterPS.get(), nullptr, 0);
	context->VSSetConstantBuffers(0, 1, cbs);

	ID3D11ShaderResourceView* nullSrvs[2] = {};
	for (const auto& piece : shape.pieces) {
		// The engine's buffers cannot be read by index: copy them into the raw buffers, unbound while they are written.
		context->VSSetShaderResources(0, 2, nullSrvs);
		if (!CopyRaw(vertices, piece.vertices.get(), "PBRWater::HullMaskVertices") || !CopyRaw(indices, piece.indices.get(), "PBRWater::HullMaskIndices"))
			continue;
		ID3D11ShaderResourceView* meshSrvs[2] = { vertices->srv.get(), indices->srv.get() };
		context->VSSetShaderResources(0, 2, meshSrvs);

		for (int r = 0; r < 3; ++r)
			cb.meshToHull[r] = { piece.meshToHull[r][0], piece.meshToHull[r][1], piece.meshToHull[r][2], piece.meshToHull[r][3] };
		cb.vertexStride = piece.stride;
		cb.positionOffset = piece.positionOffset;
		cb.halfPositions = piece.halfPositions ? 1u : 0u;
		constants->Update(cb);
		context->Draw(piece.triangles * VerticesPerTriangle, 0);
	}
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->VSSetShaderResources(0, 2, nullSrvs);

	// Where the water must reach to flow in, then into the slot.
	ID3D11ShaderResourceView* srvs[2] = { bottom->srv.get(), top->srv.get() };
	ID3D11UnorderedAccessView* uavs[3] = { image->uav.get(), level->uav.get(), atlas->uav.get() };
	context->CSSetShaderResources(0, 2, srvs);
	context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
	context->CSSetConstantBuffers(0, 1, cbs);
	constants->Update(cb);
	context->CSSetShader(prepareCS.get(), nullptr, 0);
	context->Dispatch(Size / 8, Size / 8, 1);
	context->CSSetShader(floodCS.get(), nullptr, 0);
	for (uint32_t round = 0; round < FloodRounds; ++round) {
		for (uint32_t direction = 0; direction < 4; ++direction) {
			cb.direction = direction;
			constants->Update(cb);
			context->Dispatch(Size / 64, 1, 1);
		}
	}
	context->CSSetShader(packCS.get(), nullptr, 0);
	context->Dispatch(Size / 8, Size / 8, 1);

	ID3D11UnorderedAccessView* nullUavs[3] = {};
	context->CSSetShaderResources(0, 2, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
}
