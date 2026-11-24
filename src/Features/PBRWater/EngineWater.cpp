#include "EngineWater.h"

namespace PBRWaterEngine
{
	/// The engine's ripple list; nothing reads it once the vanilla ripple meshes are culled.
	struct TESWaterSystem_AddRipple
	{
		static void thunk(RE::TESWaterSystem*, const RE::NiPoint3&, float) {}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/// Same call site Wetness Effects patches; the enable argument is forced off and the previous target runs, so install order does not matter.
	struct ToggleWaterSplashes
	{
		static void thunk(RE::TESWaterSystem* waterSystem, bool, float fadeAmount)
		{
			func(waterSystem, false, fadeAmount);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void Install()
	{
		stl::detour_thunk<TESWaterSystem_AddRipple>(REL::RelocationID(31410, 32217));
		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(25638, 26179), REL::VariantOffset(0x238, 0x223, 0x238) };
		stl::write_thunk_call<ToggleWaterSplashes>(target.address());
	}
}
