#pragma once

#include <array>
#include <cstdint>

#include "Globals.h"
#include "Profiler.h"

/**
 * PBR Water's GPU/CPU timing, in the shared profiler (Profiling menu, performance overlay, DevBench) under "PBRWater::".
 *
 * The profiler's passes are flat: one open at a time. PBR Water's nest (the wave heightfield is drawn inside the first
 * water draw of a frame), so a pass begun inside another of ours suspends it and resumes it once it ends, under the same
 * name: the profiler sums a name's samples over the frame, so the outer pass reads its own cost without the inner one.
 * Inside a pass another feature opened, ours stay untimed rather than cut theirs short.
 *
 * Render thread only (the profiler issues D3D11 queries). Names must outlive the frame: pass string literals.
 */
namespace PBRWaterProfiling
{
	namespace Detail
	{
		inline std::array<const char*, 8> stack{};
		inline uint32_t depth = 0;
		/// Depth at which the passes went untimed because another feature's pass was open (0: timed).
		inline uint32_t untimedFrom = 0;
	}

	/** @brief Opens a pass; pair with End(). */
	inline void Begin(const char* name)
	{
		using namespace Detail;
		auto* profiler = globals::profiler;
		if (depth >= stack.size()) {
			++depth;  // too deep: counted so End stays balanced, never timed
			return;
		}
		if (depth == 0)
			untimedFrom = profiler->IsPassOpen() ? 1 : 0;
		const bool timed = untimedFrom == 0;
		if (timed && depth > 0)
			profiler->EndPass();
		stack[depth++] = name;
		if (timed)
			profiler->BeginPass(name);
	}

	/** @brief Closes the pass Begin() opened last and resumes the one it suspended. */
	inline void End()
	{
		using namespace Detail;
		if (depth == 0)
			return;
		auto* profiler = globals::profiler;
		const bool timed = untimedFrom == 0;
		if (depth-- > stack.size())
			return;
		if (timed) {
			profiler->EndPass();
			if (depth > 0)
				profiler->BeginPass(stack[depth - 1]);
		}
		if (depth == 0)
			untimedFrom = 0;
	}

	/** @brief True while a pass of ours is open. */
	inline bool IsOpen() { return Detail::depth > 0; }

	/** @brief Records a pass that did not run this frame on purpose, so its row reads 0 instead of its last cost. */
	inline void Skipped(const char* name) { globals::profiler->MarkPassSkipped(name); }

	/** @brief Marks a pass skipped at the end of the scope unless `ran` was set: for functions with several early returns. */
	struct SkipUnlessRun
	{
		const char* name;
		bool ran = false;
		explicit SkipUnlessRun(const char* n) :
			name(n) {}
		~SkipUnlessRun()
		{
			if (!ran)
				Skipped(name);
		}
		SkipUnlessRun(const SkipUnlessRun&) = delete;
		SkipUnlessRun& operator=(const SkipUnlessRun&) = delete;
	};

	/** @brief A pass for the enclosing scope. */
	struct Scope
	{
		explicit Scope(const char* name) { Begin(name); }
		~Scope() { End(); }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
	};
}
