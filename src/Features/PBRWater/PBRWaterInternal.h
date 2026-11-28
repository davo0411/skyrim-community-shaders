#pragma once

#include "Features/PBRWater.h"
#include "ShaderCache.h"

/// What the PBR Water sources share that is no part of the feature's interface.
namespace PBRWaterDetail
{
	constexpr float UnitsPerMetre = static_cast<float>(PBRWaterModel::UnitsPerMetre);
	constexpr float CellSize = static_cast<float>(PBRWaterModel::CellSize);
	constexpr float InteriorFetchMetres = 50.0f;
	constexpr float TwoPi = static_cast<float>(PBRWaterModel::TwoPi);
	/// Open-sea foam added per 1/60 s per unit of the biased Jacobian (OceanAssembleCS), before Foam Amount.
	constexpr float OceanFoamGrowth = 0.1f;

	using SC = SIE::ShaderCache;

	/// Profiler names of the water draw batches by technique (DrawPassIndex): the surface, its point light passes, then
	/// Underwater, LOD, Stencil and Simple; in the world and off it (reflections, cubemaps, the map).
	constexpr const char* DrawPassNames[2][6] = {
		{ "PBRWater::Draw Surface", "PBRWater::Draw Point Lights", "PBRWater::Draw Underwater", "PBRWater::Draw LOD", "PBRWater::Draw Stencil", "PBRWater::Draw Simple" },
		{ "PBRWater::Draw Surface (Offscreen)", "PBRWater::Draw Point Lights (Offscreen)", "PBRWater::Draw Underwater (Offscreen)", "PBRWater::Draw LOD (Offscreen)", "PBRWater::Draw Stencil (Offscreen)", "PBRWater::Draw Simple (Offscreen)" },
	};

	/// Row of DrawPassNames for a water technique (WaterTechnique): 0 the surface, 1-7 its point light passes, 8-11 the rest.
	inline uint32_t DrawPassIndex(uint32_t technique)
	{
		if (technique == 0)
			return 0;
		if (technique < 8)
			return 1;
		return technique <= 11 ? technique - 6 : 0;
	}

	inline uint32_t WaterTechnique(uint32_t descriptor)
	{
		return (descriptor >> 11) & 0xF;
	}

	inline bool IsTessellatedTechnique(uint32_t technique)
	{
		return technique != static_cast<uint32_t>(SC::WaterShaderTechniques::Lod);
	}

	inline bool IsStencilTechnique(uint32_t technique)
	{
		return technique == static_cast<uint32_t>(SC::WaterShaderTechniques::Stencil);
	}

	inline float SmoothFactor(float dt, float timeConstant)
	{
		return timeConstant > 0.0f ? 1.0f - std::exp(-std::max(dt, 0.0f) / timeConstant) : 1.0f;
	}

	inline bool IsDynamicMotion(const RE::hkpRigidBody* body)
	{
		using Type = RE::hkpMotion::MotionType;
		const auto type = body->motion.type.get();
		return type == Type::kDynamic || type == Type::kSphereInertia || type == Type::kBoxInertia || type == Type::kThinBoxInertia;
	}

	/// Held by a constraint (chains, ropes, hanging nets, things hinged to the world): the waves must not drag it.
	inline bool IsConstrained(const RE::hkpRigidBody* body)
	{
		// hkSmallArray keeps its size private: a uint16 after the data pointer.
		static_assert(sizeof(RE::hkSmallArray<RE::hkConstraintInternal>) == 0x10);
		const auto masterCount = *reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::byte*>(&body->constraintsMaster) + 0x08);
		return masterCount != 0 || !body->constraintsSlave.empty();
	}

	/// The render targets and depth target bound now, with references held for the scope.
	struct BoundTargets
	{
		ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* dsv = nullptr;
		UINT count = 0;

		explicit BoundTargets(ID3D11DeviceContext* context)
		{
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
			while (count < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT && rtv[count])
				++count;
		}
		~BoundTargets()
		{
			for (auto* view : rtv) {
				if (view)
					view->Release();
			}
			if (dsv)
				dsv->Release();
		}
		BoundTargets(const BoundTargets&) = delete;
		BoundTargets& operator=(const BoundTargets&) = delete;
	};
}
