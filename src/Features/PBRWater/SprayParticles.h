#pragma once

#include "Buffer.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>

/**
 * @brief Sea spray: GPU particles thrown up by the shore's breakers and the open sea's whitecaps.
 *
 * SprayEmitCS lays particles out where the waves break this frame, SpraySimCS flies them (gravity, air drag towards the
 * wind) and lists the living, SprayDraw.hlsl draws them after the water (see Spray.hlsli). Each emitter is a setting,
 * edited in game, in the vocabulary of the vanilla particle NIFs (NiPSysEmitter, its modifiers and
 * BSEffectShaderProperty), and drawn with one of the game's own effect textures.
 */
class SprayParticles
{
public:
	static constexpr uint32_t EmitterCount = 8;
	static constexpr uint32_t TextureCount = 4;
	static constexpr uint32_t EmitGridSize = 256;  ///< emission cells per side

	/// What makes an emitter's particles (Spray.hlsli Source*).
	enum Source : int
	{
		Plunge = 0,     ///< a plunging breaker's splash-up
		Roller = 1,     ///< a broken wave's churning front
		Surf = 2,       ///< the surf simulation's white water running up the beach
		Whitecap = 3,   ///< the open sea's breaking crests
		Spindrift = 4,  ///< spray a strong wind tears off them
		SourceCount
	};

	/// The vanilla effect textures an emitter can draw with, and their atlases (BSPSysSubTexModifier).
	struct TextureInfo
	{
		const char* path;
		uint32_t columns, rows;
	};
	static constexpr std::array<TextureInfo, TextureCount> Textures = { {
		{ "textures\\effects\\fxsplash01sub.dds", 4, 4 },
		{ "textures\\effects\\fxsplash02sub_whiteralpha.dds", 4, 4 },
		{ "textures\\effects\\fxsmokesoftsub.dds", 4, 4 },
		{ "textures\\effects\\fluiddrops.dds", 4, 2 },
	} };

	/// One emitter (metres, seconds, degrees). Plain data: it lives in PBR Water's settings.
	struct Emitter
	{
		bool Enabled = true;
		int SourceKind = Plunge;
		int Texture = 0;              ///< Textures index
		bool Animate = false;         ///< the atlas frames play over the life; otherwise one random frame each
		float Rate = 1.0f;            ///< particles per m^2 per s at full strength
		float StrengthMin = 0.05f;    ///< source strength below which it makes none
		float StrengthFull = 1.0f;    ///< source strength at which it makes Rate
		float MaxDistance = 200.0f;   ///< m from the camera
		float Speed = 2.0f;           ///< m/s (shore sources: at a 1 m wave, growing as its square root)
		float SpeedVariation = 0.3f;  ///< share
		float Declination = 30.0f;    ///< degrees from straight up
		float DeclinationVariation = 15.0f;
		float PlanarSpread = 45.0f;      ///< degrees either side of the waves' travel
		float InheritWave = 0.5f;        ///< share of the wave's speed added along its travel
		float InheritWind = 0.0f;        ///< share of the wind added
		float StartHeight = 0.1f;        ///< m above the surface
		float LifeSpan = 1.0f;           ///< s
		float LifeVariation = 0.3f;      ///< share
		float Radius = 0.5f;             ///< m
		float RadiusVariation = 0.3f;    ///< share
		float EndSize = 1.0f;            ///< size multiplier at the end of life
		float GrowTime = 0.1f;           ///< s
		float FadeTime = 0.4f;           ///< s
		float FadeIn = 0.0f;             ///< share of life
		float Gravity = 1.0f;            ///< g
		float Drag = 0.05f;              ///< quadratic air drag k (1/m)
		float RotationSpeed = 0.0f;      ///< degrees/s
		float RotationVariation = 0.5f;  ///< share
		float3 Color{ 1.0f, 1.0f, 1.0f };
		float Alpha = 1.0f;
		float LightingInfluence = 1.0f;  ///< 0 unlit .. 1 lit
		float ForwardScattering = 0.5f;  ///< Henyey-Greenstein g
		float Additive = 0.0f;           ///< 0 alpha blended .. 1 additive
		float SoftDepth = 0.5f;          ///< m
		float MotionStretch = 0.0f;      ///< s of travel the particle is drawn out over
	};
	using Emitters = std::array<Emitter, EmitterCount>;

	/** @brief The emitters the feature ships with. */
	static Emitters DefaultEmitters();
	/** @brief Holds every value of an emitter to a range that keeps the maths finite (malformed settings files). */
	static void Sanitize(Emitter& e);

	struct Settings
	{
		const Emitters* emitters = nullptr;
		float amount = 1.0f;          ///< multiplier on every emitter's rate
		float range = 128.0f;         ///< m: half the emission grid's width
		uint32_t poolSize = 65536;    ///< particles alive at most
		uint32_t frameBudget = 4096;  ///< particles a frame may make
	};

	/** @brief Render thread: creates the buffers and states and compiles the shaders. */
	void SetupResources();
	/** @brief Render thread: recompiles the shaders. */
	void ClearShaderCache();
	/** @brief Render thread: kills every particle. */
	void Reset();

	/** @brief Main thread: loads the effect textures through the game's loader (BSAs and loose files), once. */
	void LoadTextures();

	/** @brief What the emit pass needs beyond PBR Water's own bindings (b7, b12, t110-t127, s12-s13), which the caller makes. */
	struct FrameInput
	{
		float dt = 0.0f;         ///< s, 0 while paused
		float2 windVelocity{};   ///< units/s
		float windSpeed = 0.0f;  ///< m/s
		float gravity = 0.0f;    ///< units/s^2
		float cameraX = 0.0f, cameraY = 0.0f;
		float cellX = 0.0f, cellY = 0.0f;  ///< the camera's exterior cell
		float cellSize = 4096.0f;
		std::array<float, 25> cellLevels{};  ///< still water of the 5 x 5 cells around it (< -1e8: none)
	};
	/** @brief Render thread: emits and flies the particles (compute). */
	void Update(const FrameInput& input, const Settings& settings);

	/**
	 * @brief Render thread, after the water is drawn: draws the living particles into the bound render target, soft
	 * against `sceneDepth` (no depth buffer is bound while they draw). Restores the state it changes.
	 */
	void Draw(ID3D11ShaderResourceView* sceneDepth);

	bool IsReady() const { return ready; }
	/** @brief Textures that failed to load, or empty. */
	std::string GetStatus() const;

private:
	/// Matches Spray.hlsli Emitter.
	struct EmitterGpu
	{
		float4 source, launch, angles, life, size, motion, color, shade, atlas, render;
	};
	static_assert(sizeof(EmitterGpu) == 160);

	struct alignas(16) EmitCB
	{
		float4 grid;
		float4 frame;
		float4 wind;
		float4 levelGrid;
		float4 cellLevels[25];
	};
	STATIC_ASSERT_ALIGNAS_16(EmitCB);

	struct alignas(16) SimCB
	{
		float4 frame;
		float4 wind;
	};
	STATIC_ASSERT_ALIGNAS_16(SimCB);

	static EmitterGpu ToGpu(const Emitter& e, float amount);
	void CreatePool(uint32_t size);
	void CompileShaders();

	/// The effect textures, loaded on the main thread and read by the render thread once texturesLoaded is set.
	std::array<RE::NiPointer<RE::NiSourceTexture>, TextureCount> textures{};
	std::atomic<bool> texturesLoaded{ false };
	std::string status;  ///< written before texturesLoaded

	// Render thread
	std::unique_ptr<Buffer> particles;  ///< the pool (Spray.hlsli Particle)
	std::unique_ptr<Buffer> counters;   ///< ring head, particles made this frame
	std::unique_ptr<Buffer> alive;      ///< the living, this frame (append)
	std::unique_ptr<Buffer> drawArgs;   ///< DrawInstancedIndirect: 4 vertices, the living
	std::unique_ptr<Buffer> emitters;   ///< EmitterGpu x EmitterCount
	std::unique_ptr<ConstantBuffer> emitCB;
	std::unique_ptr<ConstantBuffer> simCB;
	winrt::com_ptr<ID3D11ComputeShader> emitCS;
	winrt::com_ptr<ID3D11ComputeShader> simCS;
	winrt::com_ptr<ID3D11VertexShader> drawVS;
	winrt::com_ptr<ID3D11PixelShader> drawPS;
	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11SamplerState> sampler;
	uint32_t poolSize = 0;
	uint32_t frameIndex = 0;
	bool hasParticles = false;  ///< anything may be alive (else the draw is skipped)
	bool ready = false;
};

void to_json(nlohmann::json& j, const SprayParticles::Emitter& e);
void from_json(const nlohmann::json& j, SprayParticles::Emitter& e);
