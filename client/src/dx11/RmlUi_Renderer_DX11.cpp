// DirectX 11 renderer for RmlUi — implements the same ExtendedRenderInterface as DX12.
// Significantly simpler than DX12: no command lists, no descriptor heaps, no resource barriers,
// no fences, no deferred deletion. Uses immediate device context for all rendering.

// clang-format off
#include "../windows/RmlUi_Include_Windows.h"
#include "RmlUi_Renderer_DX11.h"
// clang-format on

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Log.h>

#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwmapi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")

#include <parties/profiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#define DX_CHECK(expr, msg) \
	do { \
		HRESULT hr_ = (expr); \
		if (FAILED(hr_)) { \
			std::fprintf(stderr, "[DX11] %s failed: 0x%08lX at %s:%d\n", \
				(msg), static_cast<unsigned long>(hr_), __FILE__, __LINE__); \
			return false; \
		} \
	} while (0)

#define DX_CHECK_VOID(expr, msg) \
	do { \
		HRESULT hr_ = (expr); \
		if (FAILED(hr_)) { \
			std::fprintf(stderr, "[DX11] %s failed: 0x%08lX at %s:%d\n", \
				(msg), static_cast<unsigned long>(hr_), __FILE__, __LINE__); \
			return; \
		} \
	} while (0)

// ---------------------------------------------------------------------------
// Shader sources (identical to DX12 — same register bindings)
// ---------------------------------------------------------------------------

static const char g_vs_source[] = R"HLSL(
cbuffer TransformCB : register(b0) {
	column_major float4x4 transform;
	float2 translate;
	float2 _pad;
};

struct VS_Input {
	float2 position : POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

struct VS_Output {
	float4 position : SV_POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

VS_Output main(VS_Input input) {
	VS_Output output;
	float2 pos = input.position + translate;
	output.position = mul(transform, float4(pos, 0.0, 1.0));
	output.color = input.color;
	output.texcoord = input.texcoord;
	return output;
}
)HLSL";

static const char g_ps_color_source[] = R"HLSL(
struct PS_Input {
	float4 position : SV_POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	return input.color;
}
)HLSL";

static const char g_ps_texture_source[] = R"HLSL(
Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	return tex.Sample(samp, input.texcoord) * input.color;
}
)HLSL";

static const char g_vs_passthrough_source[] = R"HLSL(
struct VS_Input {
	float2 position : POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

struct VS_Output {
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

VS_Output main(VS_Input input) {
	VS_Output output;
	output.position = float4(input.position, 0.0, 1.0);
	output.texcoord = input.texcoord;
	return output;
}
)HLSL";

static const char g_ps_yuv_source[] = R"HLSL(
Texture2D tex_y : register(t0);
Texture2D tex_u : register(t1);
Texture2D tex_v : register(t2);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	float Y = tex_y.Sample(samp, input.texcoord).r * 255.0;
	float U = tex_u.Sample(samp, input.texcoord).r * 255.0;
	float V = tex_v.Sample(samp, input.texcoord).r * 255.0;

	float C = Y - 16.0;
	float D = U - 128.0;
	float E = V - 128.0;

	float R = (298.0 * C + 459.0 * E + 128.0) / 65280.0;
	float G = (298.0 * C -  55.0 * D - 136.0 * E + 128.0) / 65280.0;
	float B = (298.0 * C + 541.0 * D + 128.0) / 65280.0;

	return float4(saturate(R), saturate(G), saturate(B), 1.0);
}
)HLSL";

static const char g_ps_nv12_source[] = R"HLSL(
Texture2D tex_y  : register(t0);
Texture2D tex_uv : register(t1);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	float Y = tex_y.Sample(samp, input.texcoord).r * 255.0;
	float U = tex_uv.Sample(samp, input.texcoord).r * 255.0;
	float V = tex_uv.Sample(samp, input.texcoord).g * 255.0;

	float C = Y - 16.0;
	float D = U - 128.0;
	float E = V - 128.0;

	float R = (298.0 * C + 459.0 * E + 128.0) / 65280.0;
	float G = (298.0 * C -  55.0 * D - 136.0 * E + 128.0) / 65280.0;
	float B = (298.0 * C + 541.0 * D + 128.0) / 65280.0;

	return float4(saturate(R), saturate(G), saturate(B), 1.0);
}
)HLSL";

static const char g_ps_passthrough_source[] = R"HLSL(
Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	return tex.Sample(samp, input.texcoord);
}
)HLSL";

static const char g_ps_color_matrix_source[] = R"HLSL(
cbuffer FilterCB : register(b1) {
	column_major float4x4 color_matrix;
};

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	float4 texColor = tex.Sample(samp, input.texcoord);
	float3 transformed = mul(color_matrix, texColor).rgb;
	return float4(transformed, texColor.a);
}
)HLSL";

static const char g_ps_blur_source[] = R"HLSL(
cbuffer FilterCB : register(b1) {
	float2 texel_offset;
	float2 _pad0;
	float4 weights;
};

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	float4 color = tex.Sample(samp, input.texcoord) * weights.x;
	[unroll] for (int i = 1; i < 4; i++) {
		float w = (i == 1) ? weights.y : (i == 2) ? weights.z : weights.w;
		float2 offset = texel_offset * float(i);
		color += tex.Sample(samp, input.texcoord + offset) * w;
		color += tex.Sample(samp, input.texcoord - offset) * w;
	}
	return color;
}
)HLSL";

static const char g_ps_drop_shadow_source[] = R"HLSL(
cbuffer FilterCB : register(b1) {
	float2 uv_offset;
	float2 _pad0;
	float4 shadow_color;
};

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	float alpha = tex.Sample(samp, input.texcoord + uv_offset).a;
	return alpha * shadow_color;
}
)HLSL";

static const char g_ps_blend_mask_source[] = R"HLSL(
Texture2D tex : register(t0);
Texture2D mask_tex : register(t1);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	float4 texColor = tex.Sample(samp, input.texcoord);
	float maskAlpha = mask_tex.Sample(samp, input.texcoord).a;
	return texColor * maskAlpha;
}
)HLSL";

static const char g_ps_gradient_source[] = R"HLSL(
#define LINEAR 0
#define RADIAL 1
#define CONIC 2
#define REPEATING_LINEAR 3
#define REPEATING_RADIAL 4
#define REPEATING_CONIC 5
#define MAX_NUM_STOPS 16
#define PI 3.14159265

cbuffer GradientCB : register(b1) {
	int func;
	int num_stops;
	float2 p;
	float2 v;
	float2 _pad0;
	float4 stop_colors[MAX_NUM_STOPS];
	float stop_positions[MAX_NUM_STOPS];
};

struct PS_Input {
	float4 position : SV_POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

float4 mix_stop_colors(float t) {
	float4 color = stop_colors[0];
	for (int i = 1; i < num_stops; i++)
		color = lerp(color, stop_colors[i], smoothstep(stop_positions[i-1], stop_positions[i], t));
	return color;
}

float4 main(PS_Input input) : SV_TARGET {
	float t = 0.0;

	if (func == LINEAR || func == REPEATING_LINEAR) {
		float dist_square = dot(v, v);
		float2 V = input.texcoord - p;
		t = dot(v, V) / dist_square;
	} else if (func == RADIAL || func == REPEATING_RADIAL) {
		float2 V = input.texcoord - p;
		t = length(v * V);
	} else if (func == CONIC || func == REPEATING_CONIC) {
		float2x2 R = float2x2(v.x, v.y, -v.y, v.x);
		float2 V = mul(R, input.texcoord - p);
		t = 0.5 + atan2(-V.x, V.y) / (2.0 * PI);
	}

	if (func == REPEATING_LINEAR || func == REPEATING_RADIAL || func == REPEATING_CONIC) {
		float t0 = stop_positions[0];
		float t1 = stop_positions[num_stops - 1];
		t = t0 + fmod(t - t0, t1 - t0);
	}

	return input.color * mix_stop_colors(t);
}
)HLSL";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int MAX_NUM_STOPS = 16;
static constexpr int BLUR_SIZE = 7;
static constexpr int BLUR_NUM_WEIGHTS = (BLUR_SIZE + 1) / 2;

// ---------------------------------------------------------------------------
// Internal data structures
// ---------------------------------------------------------------------------

struct alignas(16) TransformCB {
	float transform[16];
	float translate[2];
	float _pad[2];
};

struct alignas(16) GradientCB {
	int func;
	int num_stops;
	float p[2];
	float v[2];
	float _pad0[2];
	float stop_colors[MAX_NUM_STOPS * 4];
	float stop_positions[MAX_NUM_STOPS];
};

struct alignas(16) FilterCB {
	float data[64]; // 256 bytes
};

struct DX11_GeometryData {
	Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer;
	Microsoft::WRL::ComPtr<ID3D11Buffer> index_buffer;
	int num_indices = 0;
	int num_vertices = 0;
};

struct DX11_TextureData {
	Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
	int width = 0, height = 0;
	bool is_layer_texture = false;
};

struct DX11_YUVTextureData {
	Microsoft::WRL::ComPtr<ID3D11Texture2D> y_tex, u_tex, v_tex;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y_srv, u_srv, v_srv;
	int width = 0, height = 0;
};

struct DX11_NV12TextureData {
	Microsoft::WRL::ComPtr<ID3D11Texture2D> y_tex, uv_tex;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y_srv, uv_srv;
	int width = 0, height = 0;
};

struct DX11_LayerData {
	Microsoft::WRL::ComPtr<ID3D11Texture2D> color_texture;        // MSAA if enabled
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> resolve_texture;      // non-MSAA for sampling (null if msaa==1)
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;         // SRV of resolve_texture (or color_texture if no MSAA)
	int width = 0, height = 0;
};

struct DX11_PostprocessTarget {
	Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
	int width = 0, height = 0;
};

enum class FilterType { Invalid = 0, Passthrough, Blur, DropShadow, ColorMatrix, MaskImage };

struct DX11_CompiledFilterData {
	FilterType type = FilterType::Invalid;
	float blend_factor = 1.0f;
	float sigma = 0.0f;
	Rml::Vector2f offset = {0.f, 0.f};
	Rml::ColourbPremultiplied color = {};
	Rml::Matrix4f color_matrix = Rml::Matrix4f::Identity();
	DX11_TextureData* mask_texture = nullptr;
};

enum class ShaderGradientFunction { Linear, Radial, Conic, RepeatingLinear, RepeatingRadial, RepeatingConic };
enum class CompiledShaderType { Invalid = 0, Gradient };

struct DX11_CompiledShaderData {
	CompiledShaderType type = CompiledShaderType::Invalid;
	ShaderGradientFunction gradient_function = ShaderGradientFunction::Linear;
	Rml::Vector2f p = {0.f, 0.f};
	Rml::Vector2f v = {0.f, 0.f};
	Rml::Vector<float> stop_positions;
	Rml::Vector<Rml::Colourf> stop_colors;
};

static Rml::Colourf ConvertToColorf(Rml::ColourbPremultiplied c0) {
	Rml::Colourf result;
	for (int i = 0; i < 4; i++)
		result[i] = (1.f / 255.f) * float(c0[i]);
	return result;
}

// ---------------------------------------------------------------------------
// Shader compilation helper
// ---------------------------------------------------------------------------

static Microsoft::WRL::ComPtr<ID3DBlob> CompileHLSL(const char* source, size_t length,
	const char* entry, const char* target, const char* name) {
	Microsoft::WRL::ComPtr<ID3DBlob> blob;
	Microsoft::WRL::ComPtr<ID3DBlob> errors;

	UINT flags = 0;
#if defined(_DEBUG) || defined(RMLUI_DX_DEBUG)
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	HRESULT hr = D3DCompile(source, length, name, nullptr, nullptr,
		entry, target, flags, 0, &blob, &errors);

	if (FAILED(hr)) {
		const char* err_msg = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown";
		std::fprintf(stderr, "[DX11] Shader compilation failed (%s): %s\n", name, err_msg);
		return nullptr;
	}
	return blob;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

RenderInterface_DX11::RenderInterface_DX11(void* p_window_handle, const Backend::RmlRendererSettings& settings)
	: vsync_(settings.vsync), hwnd_(static_cast<HWND>(p_window_handle))
{
	RECT rc{};
	GetClientRect(hwnd_, &rc);
	int w = rc.right - rc.left;
	int h = rc.bottom - rc.top;
	if (w <= 0) w = 1;
	if (h <= 0) h = 1;

	width_ = w;
	height_ = h;

	if (!CreateDeviceAndSwapChain(w, h)) return;

	// Query MSAA support (after device creation)
	msaa_samples_ = settings.msaa_sample_count > 1 ? settings.msaa_sample_count : 1;
	if (msaa_samples_ > 1) {
		UINT quality = 0;
		HRESULT hr = device_->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, msaa_samples_, &quality);
		if (FAILED(hr) || quality == 0) {
			std::printf("[DX11] MSAA %ux not supported, falling back to 1x\n", msaa_samples_);
			msaa_samples_ = 1;
			msaa_quality_ = 0;
		} else {
			msaa_quality_ = quality - 1;
			std::printf("[DX11] MSAA %ux enabled (quality %u)\n", msaa_samples_, msaa_quality_);
		}
	}

	if (!CreateRenderTargetView()) return;
	if (!CreateMSAARenderTarget()) return;
	if (!CreateDepthStencilBuffer()) return;
	if (!CompileShaders()) return;
	if (!CreateStates()) return;
	if (!CreateFullscreenQuad()) return;
	if (!CreatePostprocessTargets()) return;

	projection_ = Rml::Matrix4f::ProjectOrtho(0.0f, static_cast<float>(w),
		static_cast<float>(h), 0.0f, -10000.0f, 10000.0f);

	valid_ = true;
	std::printf("[DX11] Renderer initialized (%dx%d)\n", width_, height_);
}

RenderInterface_DX11::~RenderInterface_DX11() {
	if (valid_) {
		// Flush any pending GPU work
		context_->Flush();

		ReleasePostprocessTargets();
		ReleaseAllLayers();

		if (fullscreen_quad_) {
			delete fullscreen_quad_;
			fullscreen_quad_ = nullptr;
		}
	}

	if (frame_latency_waitable_) {
		CloseHandle(frame_latency_waitable_);
		frame_latency_waitable_ = nullptr;
	}
}

RenderInterface_DX11::operator bool() const {
	return valid_;
}

// ---------------------------------------------------------------------------
// Device and swap chain creation
// ---------------------------------------------------------------------------

bool RenderInterface_DX11::CreateDeviceAndSwapChain(int width, int height) {
	UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG) || defined(RMLUI_DX_DEBUG)
	create_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_0 };
	D3D_FEATURE_LEVEL actual_level;

	DX_CHECK(D3D11CreateDevice(
		nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
		create_flags, feature_levels, 1, D3D11_SDK_VERSION,
		&device_, &actual_level, &context_),
		"D3D11CreateDevice");

	// Get DXGI factory
	ComPtr<IDXGIDevice> dxgi_device;
	DX_CHECK(device_.As(&dxgi_device), "QueryInterface(IDXGIDevice)");

	ComPtr<IDXGIAdapter> adapter;
	DX_CHECK(dxgi_device->GetAdapter(&adapter), "GetAdapter");

	DXGI_ADAPTER_DESC adapter_desc{};
	adapter->GetDesc(&adapter_desc);
	std::printf("[DX11] Using adapter: %ls\n", adapter_desc.Description);

	DX_CHECK(adapter->GetParent(IID_PPV_ARGS(&factory_)), "GetParent(IDXGIFactory2)");

	// Disable Alt+Enter
	factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

	// Create swap chain for composition (DirectComposition for VRR)
	DXGI_SWAP_CHAIN_DESC1 sc_desc{};
	sc_desc.Width = static_cast<UINT>(width);
	sc_desc.Height = static_cast<UINT>(height);
	sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sc_desc.Stereo = FALSE;
	sc_desc.SampleDesc.Count = 1;
	sc_desc.SampleDesc.Quality = 0;
	sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sc_desc.BufferCount = NUM_BACK_BUFFERS;
	sc_desc.Scaling = DXGI_SCALING_STRETCH;
	sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sc_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	sc_desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

	DX_CHECK(factory_->CreateSwapChainForComposition(
		device_.Get(), &sc_desc, nullptr, &swap_chain_),
		"CreateSwapChainForComposition");

	// Frame latency
	ComPtr<IDXGISwapChain2> sc2;
	if (SUCCEEDED(swap_chain_.As(&sc2))) {
		sc2->SetMaximumFrameLatency(NUM_BACK_BUFFERS);
		frame_latency_waitable_ = sc2->GetFrameLatencyWaitableObject();
	}

	// DirectComposition
	DX_CHECK(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&dcomp_device_)),
		"DCompositionCreateDevice");
	DX_CHECK(dcomp_device_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_),
		"CreateTargetForHwnd");
	DX_CHECK(dcomp_device_->CreateVisual(&dcomp_visual_), "CreateVisual");
	DX_CHECK(dcomp_visual_->SetContent(swap_chain_.Get()), "SetContent");
	DX_CHECK(dcomp_target_->SetRoot(dcomp_visual_.Get()), "SetRoot");
	DX_CHECK(dcomp_device_->Commit(), "DComp Commit");

	std::printf("[DX11] DirectComposition swap chain created\n");
	return true;
}

// ---------------------------------------------------------------------------
// Render target view
// ---------------------------------------------------------------------------

bool RenderInterface_DX11::CreateRenderTargetView() {
	back_buffer_rtv_.Reset();

	ComPtr<ID3D11Texture2D> back_buffer;
	DX_CHECK(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)), "GetBuffer(0)");
	DX_CHECK(device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &back_buffer_rtv_),
		"CreateRenderTargetView(BackBuffer)");
	return true;
}

// ---------------------------------------------------------------------------
// MSAA render target (intermediate, resolved to back buffer before present)
// ---------------------------------------------------------------------------

bool RenderInterface_DX11::CreateMSAARenderTarget() {
	msaa_color_texture_.Reset();
	msaa_rtv_.Reset();

	if (msaa_samples_ <= 1)
		return true;  // no MSAA — render directly to back buffer

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = static_cast<UINT>(width_);
	desc.Height = static_cast<UINT>(height_);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = msaa_samples_;
	desc.SampleDesc.Quality = msaa_quality_;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;

	DX_CHECK(device_->CreateTexture2D(&desc, nullptr, &msaa_color_texture_),
		"CreateTexture2D(MSAA)");
	DX_CHECK(device_->CreateRenderTargetView(msaa_color_texture_.Get(), nullptr, &msaa_rtv_),
		"CreateRenderTargetView(MSAA)");
	return true;
}

// ---------------------------------------------------------------------------
// Depth/stencil buffer
// ---------------------------------------------------------------------------

bool RenderInterface_DX11::CreateDepthStencilBuffer() {
	depth_stencil_texture_.Reset();
	dsv_.Reset();

	D3D11_TEXTURE2D_DESC ds_desc{};
	ds_desc.Width = static_cast<UINT>(width_);
	ds_desc.Height = static_cast<UINT>(height_);
	ds_desc.MipLevels = 1;
	ds_desc.ArraySize = 1;
	ds_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	ds_desc.SampleDesc.Count = msaa_samples_;
	ds_desc.SampleDesc.Quality = msaa_quality_;
	ds_desc.Usage = D3D11_USAGE_DEFAULT;
	ds_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

	DX_CHECK(device_->CreateTexture2D(&ds_desc, nullptr, &depth_stencil_texture_),
		"CreateTexture2D(DepthStencil)");
	DX_CHECK(device_->CreateDepthStencilView(depth_stencil_texture_.Get(), nullptr, &dsv_),
		"CreateDepthStencilView");
	return true;
}

// ---------------------------------------------------------------------------
// Shader compilation
// ---------------------------------------------------------------------------

bool RenderInterface_DX11::CompileShaders() {
	// Main vertex shader
	auto vs_blob = CompileHLSL(g_vs_source, sizeof(g_vs_source) - 1, "main", "vs_5_0", "DX11_VS");
	if (!vs_blob) return false;
	DX_CHECK(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_main_),
		"CreateVertexShader(main)");

	// Input layout
	D3D11_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,     0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	DX_CHECK(device_->CreateInputLayout(layout, 3,
		vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &input_layout_),
		"CreateInputLayout");

	// Passthrough vertex shader
	auto vs_pt_blob = CompileHLSL(g_vs_passthrough_source, sizeof(g_vs_passthrough_source) - 1, "main", "vs_5_0", "DX11_VS_PT");
	if (!vs_pt_blob) return false;
	DX_CHECK(device_->CreateVertexShader(vs_pt_blob->GetBufferPointer(), vs_pt_blob->GetBufferSize(), nullptr, &vs_passthrough_),
		"CreateVertexShader(passthrough)");

	// Pixel shaders
	auto compile_ps = [&](const char* src, size_t len, const char* name, ComPtr<ID3D11PixelShader>& ps) -> bool {
		auto blob = CompileHLSL(src, len, "main", "ps_5_0", name);
		if (!blob) return false;
		HRESULT hr = device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps);
		if (FAILED(hr)) { std::fprintf(stderr, "[DX11] CreatePixelShader(%s) failed\n", name); return false; }
		return true;
	};

	if (!compile_ps(g_ps_color_source, sizeof(g_ps_color_source) - 1, "DX11_PS_Color", ps_color_)) return false;
	if (!compile_ps(g_ps_texture_source, sizeof(g_ps_texture_source) - 1, "DX11_PS_Texture", ps_texture_)) return false;
	if (!compile_ps(g_ps_passthrough_source, sizeof(g_ps_passthrough_source) - 1, "DX11_PS_PT", ps_passthrough_)) return false;
	if (!compile_ps(g_ps_yuv_source, sizeof(g_ps_yuv_source) - 1, "DX11_PS_YUV", ps_yuv_)) return false;
	if (!compile_ps(g_ps_nv12_source, sizeof(g_ps_nv12_source) - 1, "DX11_PS_NV12", ps_nv12_)) return false;
	if (!compile_ps(g_ps_color_matrix_source, sizeof(g_ps_color_matrix_source) - 1, "DX11_PS_CM", ps_color_matrix_)) return false;
	if (!compile_ps(g_ps_blur_source, sizeof(g_ps_blur_source) - 1, "DX11_PS_Blur", ps_blur_)) return false;
	if (!compile_ps(g_ps_drop_shadow_source, sizeof(g_ps_drop_shadow_source) - 1, "DX11_PS_DS", ps_drop_shadow_)) return false;
	if (!compile_ps(g_ps_blend_mask_source, sizeof(g_ps_blend_mask_source) - 1, "DX11_PS_BM", ps_blend_mask_)) return false;
	if (!compile_ps(g_ps_gradient_source, sizeof(g_ps_gradient_source) - 1, "DX11_PS_Grad", ps_gradient_)) return false;

	// Constant buffers
	D3D11_BUFFER_DESC cb_desc{};
	cb_desc.Usage = D3D11_USAGE_DYNAMIC;
	cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	cb_desc.ByteWidth = sizeof(TransformCB);
	DX_CHECK(device_->CreateBuffer(&cb_desc, nullptr, &cb_transform_), "CreateBuffer(TransformCB)");

	cb_desc.ByteWidth = 512; // enough for GradientCB
	DX_CHECK(device_->CreateBuffer(&cb_desc, nullptr, &cb_filter_), "CreateBuffer(FilterCB)");

	return true;
}

// ---------------------------------------------------------------------------
// States
// ---------------------------------------------------------------------------

bool RenderInterface_DX11::CreateStates() {
	// Blend: premultiplied alpha
	{
		D3D11_BLEND_DESC desc{};
		auto& rt = desc.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_ONE;
		rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		rt.BlendOp = D3D11_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D11_BLEND_ONE;
		rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		DX_CHECK(device_->CreateBlendState(&desc, &blend_premul_), "CreateBlendState(premul)");
	}
	// Blend: replace
	{
		D3D11_BLEND_DESC desc{};
		desc.RenderTarget[0].BlendEnable = FALSE;
		desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		DX_CHECK(device_->CreateBlendState(&desc, &blend_replace_), "CreateBlendState(replace)");
	}
	// Blend: no color write
	{
		D3D11_BLEND_DESC desc{};
		desc.RenderTarget[0].BlendEnable = FALSE;
		desc.RenderTarget[0].RenderTargetWriteMask = 0;
		DX_CHECK(device_->CreateBlendState(&desc, &blend_no_color_), "CreateBlendState(nocolor)");
	}

	// DepthStencil: disabled
	{
		D3D11_DEPTH_STENCIL_DESC desc{};
		desc.DepthEnable = FALSE;
		desc.StencilEnable = FALSE;
		DX_CHECK(device_->CreateDepthStencilState(&desc, &dss_disabled_), "CreateDSS(disabled)");
	}
	// DepthStencil: stencil EQUAL
	{
		D3D11_DEPTH_STENCIL_DESC desc{};
		desc.DepthEnable = FALSE;
		desc.StencilEnable = TRUE;
		desc.StencilReadMask = 0xFF;
		desc.StencilWriteMask = 0xFF;
		desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		desc.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
		desc.BackFace = desc.FrontFace;
		DX_CHECK(device_->CreateDepthStencilState(&desc, &dss_equal_), "CreateDSS(equal)");
	}
	// DepthStencil: stencil SET (REPLACE, ALWAYS)
	{
		D3D11_DEPTH_STENCIL_DESC desc{};
		desc.DepthEnable = FALSE;
		desc.StencilEnable = TRUE;
		desc.StencilReadMask = 0xFF;
		desc.StencilWriteMask = 0xFF;
		desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		desc.BackFace = desc.FrontFace;
		DX_CHECK(device_->CreateDepthStencilState(&desc, &dss_set_), "CreateDSS(set)");
	}
	// DepthStencil: stencil INTERSECT (INCR_SAT, EQUAL)
	{
		D3D11_DEPTH_STENCIL_DESC desc{};
		desc.DepthEnable = FALSE;
		desc.StencilEnable = TRUE;
		desc.StencilReadMask = 0xFF;
		desc.StencilWriteMask = 0xFF;
		desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_INCR_SAT;
		desc.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
		desc.BackFace = desc.FrontFace;
		DX_CHECK(device_->CreateDepthStencilState(&desc, &dss_intersect_), "CreateDSS(intersect)");
	}

	// Rasterizer: solid, no cull, scissor enabled
	{
		D3D11_RASTERIZER_DESC desc{};
		desc.FillMode = D3D11_FILL_SOLID;
		desc.CullMode = D3D11_CULL_NONE;
		desc.ScissorEnable = TRUE;
		desc.DepthClipEnable = TRUE;
		DX_CHECK(device_->CreateRasterizerState(&desc, &rasterizer_), "CreateRasterizerState");
	}

	// Sampler: linear, clamp
	{
		D3D11_SAMPLER_DESC desc{};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		DX_CHECK(device_->CreateSamplerState(&desc, &sampler_linear_), "CreateSamplerState");
	}

	return true;
}

// ---------------------------------------------------------------------------
// Fullscreen quad
// ---------------------------------------------------------------------------

bool RenderInterface_DX11::CreateFullscreenQuad() {
	Rml::Vertex vertices[4];
	vertices[0].position = {-1.0f, +1.0f}; vertices[0].colour = {255,255,255,255}; vertices[0].tex_coord = {0.0f, 0.0f};
	vertices[1].position = {+1.0f, +1.0f}; vertices[1].colour = {255,255,255,255}; vertices[1].tex_coord = {1.0f, 0.0f};
	vertices[2].position = {-1.0f, -1.0f}; vertices[2].colour = {255,255,255,255}; vertices[2].tex_coord = {0.0f, 1.0f};
	vertices[3].position = {+1.0f, -1.0f}; vertices[3].colour = {255,255,255,255}; vertices[3].tex_coord = {1.0f, 1.0f};

	int indices[6] = { 0, 1, 2, 2, 1, 3 };

	auto handle = CompileGeometry(
		Rml::Span<const Rml::Vertex>(vertices, 4),
		Rml::Span<const int>(indices, 6));
	if (!handle) return false;

	fullscreen_quad_ = reinterpret_cast<DX11_GeometryData*>(handle);
	return true;
}

// ---------------------------------------------------------------------------
// Postprocess targets
// ---------------------------------------------------------------------------

bool RenderInterface_DX11::CreatePostprocessTargets() {
	for (int i = 0; i < NUM_POSTPROCESS_TARGETS; ++i) {
		auto* pp = new DX11_PostprocessTarget();
		pp->width = width_;
		pp->height = height_;

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = static_cast<UINT>(width_);
		desc.Height = static_cast<UINT>(height_);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

		HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &pp->texture);
		if (FAILED(hr)) { delete pp; return false; }

		hr = device_->CreateRenderTargetView(pp->texture.Get(), nullptr, &pp->rtv);
		if (FAILED(hr)) { delete pp; return false; }

		hr = device_->CreateShaderResourceView(pp->texture.Get(), nullptr, &pp->srv);
		if (FAILED(hr)) { delete pp; return false; }

		postprocess_targets_[i] = pp;
	}
	return true;
}

void RenderInterface_DX11::ReleasePostprocessTargets() {
	for (int i = 0; i < NUM_POSTPROCESS_TARGETS; ++i) {
		delete postprocess_targets_[i];
		postprocess_targets_[i] = nullptr;
	}
}

// ---------------------------------------------------------------------------
// CB helper
// ---------------------------------------------------------------------------

void RenderInterface_DX11::UpdateCB(ID3D11Buffer* buffer, const void* data, UINT size) {
	D3D11_MAPPED_SUBRESOURCE mapped{};
	HRESULT hr = context_->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (SUCCEEDED(hr)) {
		std::memcpy(mapped.pData, data, size);
		context_->Unmap(buffer, 0);
	}
}

// ---------------------------------------------------------------------------
// SetViewport
// ---------------------------------------------------------------------------

void RenderInterface_DX11::SetViewport(int viewport_width, int viewport_height, bool force) {
	ZoneScopedN("DX11::SetViewport");
	if (viewport_width <= 0 || viewport_height <= 0) return;
	if (!force && viewport_width == width_ && viewport_height == height_) return;

	context_->Flush();

	ReleasePostprocessTargets();
	ReleaseAllLayers();
	msaa_color_texture_.Reset();
	msaa_rtv_.Reset();
	back_buffer_rtv_.Reset();

	HRESULT hr = swap_chain_->ResizeBuffers(
		NUM_BACK_BUFFERS,
		static_cast<UINT>(viewport_width),
		static_cast<UINT>(viewport_height),
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);

	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX11] ResizeBuffers failed: 0x%08lX\n", static_cast<unsigned long>(hr));
		valid_ = false;
		return;
	}

	width_ = viewport_width;
	height_ = viewport_height;

	CreateRenderTargetView();
	CreateMSAARenderTarget();
	CreateDepthStencilBuffer();
	CreatePostprocessTargets();

	projection_ = Rml::Matrix4f::ProjectOrtho(0.0f, static_cast<float>(viewport_width),
		static_cast<float>(viewport_height), 0.0f, -10000.0f, 10000.0f);

	if (dcomp_device_)
		dcomp_device_->Commit();
}

// ---------------------------------------------------------------------------
// BeginFrame
// ---------------------------------------------------------------------------

void RenderInterface_DX11::BeginFrame() {
	ZoneScopedN("DX11::BeginFrame");

	if (frame_latency_waitable_) {
		ZoneScopedN("DX11::WaitFrameLatency");
		WaitForSingleObjectEx(frame_latency_waitable_, 1000, TRUE);
	}

	// Set viewport
	D3D11_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(width_);
	viewport.Height = static_cast<float>(height_);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	context_->RSSetViewports(1, &viewport);

	// Default scissor
	scissor_rect_ = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
	context_->RSSetScissorRects(1, &scissor_rect_);

	// Bind render target + depth/stencil (MSAA target if enabled, else back buffer)
	SetRenderTargetToBackBuffer();

	// Set common state
	context_->IASetInputLayout(input_layout_.Get());
	context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context_->RSSetState(rasterizer_.Get());
	context_->PSSetSamplers(0, 1, sampler_linear_.GetAddressOf());

	// Bind constant buffers
	ID3D11Buffer* vs_cbs[] = { cb_transform_.Get() };
	context_->VSSetConstantBuffers(0, 1, vs_cbs);
	ID3D11Buffer* ps_cbs[] = { nullptr, cb_filter_.Get() };
	context_->PSSetConstantBuffers(0, 2, ps_cbs);
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

void RenderInterface_DX11::Clear() {
	ZoneScopedN("DX11::Clear");
	const float clear_color[4] = { 0.051f, 0.055f, 0.090f, 1.0f };
	ID3D11RenderTargetView* rtv = (msaa_samples_ > 1) ? msaa_rtv_.Get() : back_buffer_rtv_.Get();
	context_->ClearRenderTargetView(rtv, clear_color);
	context_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_STENCIL, 1.0f, 0);

	clip_mask_enabled_ = false;
	stencil_ref_ = 0;
}

// ---------------------------------------------------------------------------
// EndFrame
// ---------------------------------------------------------------------------

void RenderInterface_DX11::EndFrame() {
	ZoneScopedN("DX11::EndFrame");

	// Resolve MSAA render target to swap chain back buffer
	if (msaa_samples_ > 1) {
		ComPtr<ID3D11Texture2D> back_buffer;
		swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
		context_->ResolveSubresource(back_buffer.Get(), 0, msaa_color_texture_.Get(), 0,
			DXGI_FORMAT_R8G8B8A8_UNORM);
	}

	HRESULT hr;
	{
		ZoneScopedN("DX11::Present");
		hr = swap_chain_->Present(vsync_ ? 1 : 0, 0);
	}

	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
		std::fprintf(stderr, "[DX11] Device lost during Present: 0x%08lX\n",
			static_cast<unsigned long>(hr));
		valid_ = false;
		return;
	}

	if (vsync_) {
		ZoneScopedN("DX11::DwmFlush");
		DwmFlush();
	}

	FrameMark;
}

// ===========================================================================
// Geometry
// ===========================================================================

Rml::CompiledGeometryHandle RenderInterface_DX11::CompileGeometry(
	Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
	ZoneScopedN("DX11::CompileGeometry");
	if (vertices.empty() || indices.empty()) return Rml::CompiledGeometryHandle(0);

	auto* geom = new DX11_GeometryData();
	geom->num_indices = static_cast<int>(indices.size());
	geom->num_vertices = static_cast<int>(vertices.size());

	// Vertex buffer (dynamic for UpdateGeometryVertices)
	D3D11_BUFFER_DESC vb_desc{};
	vb_desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Rml::Vertex));
	vb_desc.Usage = D3D11_USAGE_DYNAMIC;
	vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	D3D11_SUBRESOURCE_DATA vb_data{};
	vb_data.pSysMem = vertices.data();

	HRESULT hr = device_->CreateBuffer(&vb_desc, &vb_data, &geom->vertex_buffer);
	if (FAILED(hr)) { delete geom; return Rml::CompiledGeometryHandle(0); }

	// Index buffer (immutable)
	D3D11_BUFFER_DESC ib_desc{};
	ib_desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(int));
	ib_desc.Usage = D3D11_USAGE_IMMUTABLE;
	ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;

	D3D11_SUBRESOURCE_DATA ib_data{};
	ib_data.pSysMem = indices.data();

	hr = device_->CreateBuffer(&ib_desc, &ib_data, &geom->index_buffer);
	if (FAILED(hr)) { delete geom; return Rml::CompiledGeometryHandle(0); }

	return reinterpret_cast<Rml::CompiledGeometryHandle>(geom);
}

void RenderInterface_DX11::UpdateGeometryVertices(
	Rml::CompiledGeometryHandle geometry, Rml::Span<const Rml::Vertex> vertices) {
	if (!geometry || vertices.empty()) return;
	auto* geom = reinterpret_cast<DX11_GeometryData*>(geometry);

	D3D11_MAPPED_SUBRESOURCE mapped{};
	HRESULT hr = context_->Map(geom->vertex_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (SUCCEEDED(hr)) {
		std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(Rml::Vertex));
		context_->Unmap(geom->vertex_buffer.Get(), 0);
	}
}

void RenderInterface_DX11::RenderGeometry(
	Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
	Rml::TextureHandle texture) {
	ZoneScopedN("DX11::RenderGeometry");
	if (!geometry) return;

	auto* geom = reinterpret_cast<DX11_GeometryData*>(geometry);
	auto* tex = reinterpret_cast<DX11_TextureData*>(texture);

	// Update transform CB
	Rml::Matrix4f mvp = transform_active_ ? projection_ * transform_ : projection_;
	TransformCB cb{};
	std::memcpy(cb.transform, mvp.data(), 16 * sizeof(float));
	cb.translate[0] = translation.x;
	cb.translate[1] = translation.y;
	UpdateCB(cb_transform_.Get(), &cb, sizeof(cb));

	// Set shaders
	context_->VSSetShader(vs_main_.Get(), nullptr, 0);
	bool has_texture = (tex && tex->srv);
	context_->PSSetShader(has_texture ? ps_texture_.Get() : ps_color_.Get(), nullptr, 0);

	// Bind texture
	if (has_texture) {
		context_->PSSetShaderResources(0, 1, tex->srv.GetAddressOf());
	}

	// Set blend and stencil state
	float blend_factor[4] = {1,1,1,1};
	context_->OMSetBlendState(blend_premul_.Get(), blend_factor, 0xFFFFFFFF);

	if (clip_mask_enabled_) {
		context_->OMSetDepthStencilState(dss_equal_.Get(), stencil_ref_);
	} else {
		context_->OMSetDepthStencilState(dss_disabled_.Get(), 0);
	}

	// Scissor
	if (scissor_enabled_) {
		context_->RSSetScissorRects(1, &scissor_rect_);
	} else {
		D3D11_RECT full = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
		context_->RSSetScissorRects(1, &full);
	}

	// Draw
	UINT stride = sizeof(Rml::Vertex);
	UINT offset = 0;
	context_->IASetVertexBuffers(0, 1, geom->vertex_buffer.GetAddressOf(), &stride, &offset);
	context_->IASetIndexBuffer(geom->index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
	context_->DrawIndexed(static_cast<UINT>(geom->num_indices), 0, 0);

	// Unbind texture to avoid hazards
	if (has_texture) {
		ID3D11ShaderResourceView* null_srv = nullptr;
		context_->PSSetShaderResources(0, 1, &null_srv);
	}
}

void RenderInterface_DX11::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
	if (!geometry) return;
	delete reinterpret_cast<DX11_GeometryData*>(geometry);
}

// ===========================================================================
// Textures
// ===========================================================================

Rml::TextureHandle RenderInterface_DX11::GenerateTexture(
	Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) {
	ZoneScopedN("DX11::GenerateTexture");
	if (source_data.empty() || source_dimensions.x <= 0 || source_dimensions.y <= 0)
		return Rml::TextureHandle(0);

	auto* tex = new DX11_TextureData();
	tex->width = source_dimensions.x;
	tex->height = source_dimensions.y;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = static_cast<UINT>(source_dimensions.x);
	desc.Height = static_cast<UINT>(source_dimensions.y);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA init_data{};
	init_data.pSysMem = source_data.data();
	init_data.SysMemPitch = static_cast<UINT>(source_dimensions.x) * 4;

	HRESULT hr = device_->CreateTexture2D(&desc, &init_data, &tex->texture);
	if (FAILED(hr)) { delete tex; return Rml::TextureHandle(0); }

	hr = device_->CreateShaderResourceView(tex->texture.Get(), nullptr, &tex->srv);
	if (FAILED(hr)) { delete tex; return Rml::TextureHandle(0); }

	return reinterpret_cast<Rml::TextureHandle>(tex);
}

Rml::TextureHandle RenderInterface_DX11::LoadTexture(
	Rml::Vector2i& /*texture_dimensions*/, const Rml::String& /*source*/) {
	return Rml::TextureHandle(0);
}

void RenderInterface_DX11::ReleaseTexture(Rml::TextureHandle texture_handle) {
	if (!texture_handle) return;
	auto* tex = reinterpret_cast<DX11_TextureData*>(texture_handle);
	if (!tex->is_layer_texture) {
		// Normal texture — release everything
		delete tex;
	} else {
		// Layer texture wrapper — layer owns the resources
		delete tex;
	}
}

void RenderInterface_DX11::UpdateTextureData(
	Rml::TextureHandle texture_handle, Rml::Span<const Rml::byte> source_data,
	Rml::Vector2i source_dimensions) {
	ZoneScopedN("DX11::UpdateTextureData");
	if (!texture_handle) return;
	auto* tex = reinterpret_cast<DX11_TextureData*>(texture_handle);

	context_->UpdateSubresource(tex->texture.Get(), 0, nullptr,
		source_data.data(), static_cast<UINT>(source_dimensions.x) * 4, 0);
}

// ===========================================================================
// YUV Textures
// ===========================================================================

void RenderInterface_DX11::UploadR8Texture(ID3D11Texture2D* tex, const uint8_t* data,
	uint32_t src_stride, int width, int height) {
	if (static_cast<int>(src_stride) == width) {
		context_->UpdateSubresource(tex, 0, nullptr, data, static_cast<UINT>(width), 0);
	} else {
		// Row-by-row upload for stride mismatch
		D3D11_MAPPED_SUBRESOURCE mapped{};
		// Can't Map a DEFAULT texture — use staging or UpdateSubresource with box
		// Use UpdateSubresource row by row via a temp buffer
		std::vector<uint8_t> tmp(static_cast<size_t>(width) * height);
		for (int y = 0; y < height; ++y)
			std::memcpy(tmp.data() + y * width, data + y * src_stride, width);
		context_->UpdateSubresource(tex, 0, nullptr, tmp.data(), static_cast<UINT>(width), 0);
	}
}

void RenderInterface_DX11::UploadR8G8Texture(ID3D11Texture2D* tex, const uint8_t* data,
	uint32_t src_stride, int width, int height) {
	UINT row_pitch = static_cast<UINT>(width) * 2;
	if (src_stride == row_pitch) {
		context_->UpdateSubresource(tex, 0, nullptr, data, row_pitch, 0);
	} else {
		std::vector<uint8_t> tmp(static_cast<size_t>(row_pitch) * height);
		for (int y = 0; y < height; ++y)
			std::memcpy(tmp.data() + y * row_pitch, data + y * src_stride, row_pitch);
		context_->UpdateSubresource(tex, 0, nullptr, tmp.data(), row_pitch, 0);
	}
}

static Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateR8Tex(ID3D11Device* dev, int w, int h) {
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = static_cast<UINT>(w);
	desc.Height = static_cast<UINT>(h);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
	dev->CreateTexture2D(&desc, nullptr, &tex);
	return tex;
}

static Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateR8G8Tex(ID3D11Device* dev, int w, int h) {
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = static_cast<UINT>(w);
	desc.Height = static_cast<UINT>(h);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
	dev->CreateTexture2D(&desc, nullptr, &tex);
	return tex;
}

uintptr_t RenderInterface_DX11::GenerateYUVTexture(
	const uint8_t* y_data, uint32_t y_stride,
	const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
	uint32_t width, uint32_t height) {
	ZoneScopedN("DX11::GenerateYUVTexture");

	auto* yuv = new DX11_YUVTextureData();
	yuv->width = static_cast<int>(width);
	yuv->height = static_cast<int>(height);

	int half_w = static_cast<int>(width / 2);
	int half_h = static_cast<int>(height / 2);

	yuv->y_tex = CreateR8Tex(device_.Get(), static_cast<int>(width), static_cast<int>(height));
	yuv->u_tex = CreateR8Tex(device_.Get(), half_w, half_h);
	yuv->v_tex = CreateR8Tex(device_.Get(), half_w, half_h);

	if (!yuv->y_tex || !yuv->u_tex || !yuv->v_tex) { delete yuv; return 0; }

	device_->CreateShaderResourceView(yuv->y_tex.Get(), nullptr, &yuv->y_srv);
	device_->CreateShaderResourceView(yuv->u_tex.Get(), nullptr, &yuv->u_srv);
	device_->CreateShaderResourceView(yuv->v_tex.Get(), nullptr, &yuv->v_srv);

	UploadR8Texture(yuv->y_tex.Get(), y_data, y_stride, static_cast<int>(width), static_cast<int>(height));
	UploadR8Texture(yuv->u_tex.Get(), u_data, uv_stride, half_w, half_h);
	UploadR8Texture(yuv->v_tex.Get(), v_data, uv_stride, half_w, half_h);

	return reinterpret_cast<uintptr_t>(yuv);
}

void RenderInterface_DX11::UpdateYUVTexture(uintptr_t handle,
	const uint8_t* y_data, uint32_t y_stride,
	const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
	uint32_t width, uint32_t height) {
	ZoneScopedN("DX11::UpdateYUVTexture");
	if (!handle) return;
	auto* yuv = reinterpret_cast<DX11_YUVTextureData*>(handle);
	int half_w = static_cast<int>(width / 2);
	int half_h = static_cast<int>(height / 2);

	UploadR8Texture(yuv->y_tex.Get(), y_data, y_stride, static_cast<int>(width), static_cast<int>(height));
	UploadR8Texture(yuv->u_tex.Get(), u_data, uv_stride, half_w, half_h);
	UploadR8Texture(yuv->v_tex.Get(), v_data, uv_stride, half_w, half_h);
}

void RenderInterface_DX11::ReleaseYUVTexture(uintptr_t handle) {
	if (!handle) return;
	delete reinterpret_cast<DX11_YUVTextureData*>(handle);
}

void RenderInterface_DX11::RenderYUVGeometry(
	Rml::CompiledGeometryHandle geometry,
	Rml::Vector2f translation, uintptr_t yuv_handle) {
	ZoneScopedN("DX11::RenderYUVGeometry");
	if (!geometry || !yuv_handle) return;

	auto* geom = reinterpret_cast<DX11_GeometryData*>(geometry);
	auto* yuv = reinterpret_cast<DX11_YUVTextureData*>(yuv_handle);

	Rml::Matrix4f mvp = transform_active_ ? projection_ * transform_ : projection_;
	TransformCB cb{};
	std::memcpy(cb.transform, mvp.data(), 16 * sizeof(float));
	cb.translate[0] = translation.x;
	cb.translate[1] = translation.y;
	UpdateCB(cb_transform_.Get(), &cb, sizeof(cb));

	context_->VSSetShader(vs_main_.Get(), nullptr, 0);
	context_->PSSetShader(ps_yuv_.Get(), nullptr, 0);

	ID3D11ShaderResourceView* srvs[3] = { yuv->y_srv.Get(), yuv->u_srv.Get(), yuv->v_srv.Get() };
	context_->PSSetShaderResources(0, 3, srvs);

	float blend_factor[4] = {1,1,1,1};
	context_->OMSetBlendState(blend_replace_.Get(), blend_factor, 0xFFFFFFFF);

	if (clip_mask_enabled_)
		context_->OMSetDepthStencilState(dss_equal_.Get(), stencil_ref_);
	else
		context_->OMSetDepthStencilState(dss_disabled_.Get(), 0);

	if (scissor_enabled_)
		context_->RSSetScissorRects(1, &scissor_rect_);
	else {
		D3D11_RECT full = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
		context_->RSSetScissorRects(1, &full);
	}

	UINT stride = sizeof(Rml::Vertex);
	UINT off = 0;
	context_->IASetVertexBuffers(0, 1, geom->vertex_buffer.GetAddressOf(), &stride, &off);
	context_->IASetIndexBuffer(geom->index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
	context_->DrawIndexed(static_cast<UINT>(geom->num_indices), 0, 0);

	ID3D11ShaderResourceView* null_srvs[3] = {};
	context_->PSSetShaderResources(0, 3, null_srvs);
}

// ===========================================================================
// NV12 Textures
// ===========================================================================

uintptr_t RenderInterface_DX11::GenerateNV12Texture(
	const uint8_t* y_data, uint32_t y_stride,
	const uint8_t* uv_data, uint32_t uv_stride,
	uint32_t width, uint32_t height) {
	ZoneScopedN("DX11::GenerateNV12Texture");

	auto* nv12 = new DX11_NV12TextureData();
	nv12->width = static_cast<int>(width);
	nv12->height = static_cast<int>(height);

	int half_w = static_cast<int>(width / 2);
	int half_h = static_cast<int>(height / 2);

	nv12->y_tex = CreateR8Tex(device_.Get(), static_cast<int>(width), static_cast<int>(height));
	nv12->uv_tex = CreateR8G8Tex(device_.Get(), half_w, half_h);

	if (!nv12->y_tex || !nv12->uv_tex) { delete nv12; return 0; }

	device_->CreateShaderResourceView(nv12->y_tex.Get(), nullptr, &nv12->y_srv);
	device_->CreateShaderResourceView(nv12->uv_tex.Get(), nullptr, &nv12->uv_srv);

	UploadR8Texture(nv12->y_tex.Get(), y_data, y_stride, static_cast<int>(width), static_cast<int>(height));
	UploadR8G8Texture(nv12->uv_tex.Get(), uv_data, uv_stride, half_w, half_h);

	return reinterpret_cast<uintptr_t>(nv12);
}

void RenderInterface_DX11::UpdateNV12Texture(uintptr_t handle,
	const uint8_t* y_data, uint32_t y_stride,
	const uint8_t* uv_data, uint32_t uv_stride,
	uint32_t width, uint32_t height) {
	ZoneScopedN("DX11::UpdateNV12Texture");
	if (!handle) return;
	auto* nv12 = reinterpret_cast<DX11_NV12TextureData*>(handle);

	UploadR8Texture(nv12->y_tex.Get(), y_data, y_stride, static_cast<int>(width), static_cast<int>(height));
	UploadR8G8Texture(nv12->uv_tex.Get(), uv_data, uv_stride, static_cast<int>(width / 2), static_cast<int>(height / 2));
}

void RenderInterface_DX11::ReleaseNV12Texture(uintptr_t handle) {
	if (!handle) return;
	delete reinterpret_cast<DX11_NV12TextureData*>(handle);
}

void RenderInterface_DX11::RenderNV12Geometry(
	Rml::CompiledGeometryHandle geometry,
	Rml::Vector2f translation, uintptr_t nv12_handle) {
	ZoneScopedN("DX11::RenderNV12Geometry");
	if (!geometry || !nv12_handle) return;

	auto* geom = reinterpret_cast<DX11_GeometryData*>(geometry);
	auto* nv12 = reinterpret_cast<DX11_NV12TextureData*>(nv12_handle);

	Rml::Matrix4f mvp = transform_active_ ? projection_ * transform_ : projection_;
	TransformCB cb{};
	std::memcpy(cb.transform, mvp.data(), 16 * sizeof(float));
	cb.translate[0] = translation.x;
	cb.translate[1] = translation.y;
	UpdateCB(cb_transform_.Get(), &cb, sizeof(cb));

	context_->VSSetShader(vs_main_.Get(), nullptr, 0);
	context_->PSSetShader(ps_nv12_.Get(), nullptr, 0);

	ID3D11ShaderResourceView* srvs[2] = { nv12->y_srv.Get(), nv12->uv_srv.Get() };
	context_->PSSetShaderResources(0, 2, srvs);

	float blend_factor[4] = {1,1,1,1};
	context_->OMSetBlendState(blend_replace_.Get(), blend_factor, 0xFFFFFFFF);

	if (clip_mask_enabled_)
		context_->OMSetDepthStencilState(dss_equal_.Get(), stencil_ref_);
	else
		context_->OMSetDepthStencilState(dss_disabled_.Get(), 0);

	if (scissor_enabled_)
		context_->RSSetScissorRects(1, &scissor_rect_);
	else {
		D3D11_RECT full = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
		context_->RSSetScissorRects(1, &full);
	}

	UINT stride = sizeof(Rml::Vertex);
	UINT off = 0;
	context_->IASetVertexBuffers(0, 1, geom->vertex_buffer.GetAddressOf(), &stride, &off);
	context_->IASetIndexBuffer(geom->index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
	context_->DrawIndexed(static_cast<UINT>(geom->num_indices), 0, 0);

	ID3D11ShaderResourceView* null_srvs[2] = {};
	context_->PSSetShaderResources(0, 2, null_srvs);
}

// ===========================================================================
// Scissor & Transform
// ===========================================================================

void RenderInterface_DX11::EnableScissorRegion(bool enable) { scissor_enabled_ = enable; }

void RenderInterface_DX11::SetScissorRegion(Rml::Rectanglei region) {
	scissor_rect_.left = static_cast<LONG>(region.Left());
	scissor_rect_.top = static_cast<LONG>(region.Top());
	scissor_rect_.right = static_cast<LONG>(region.Right());
	scissor_rect_.bottom = static_cast<LONG>(region.Bottom());
}

void RenderInterface_DX11::SetTransform(const Rml::Matrix4f* transform) {
	if (transform) {
		transform_ = *transform;
		transform_active_ = true;
	} else {
		transform_active_ = false;
	}
}

// ===========================================================================
// Clip Mask (stencil)
// ===========================================================================

void RenderInterface_DX11::EnableClipMask(bool enable) {
	clip_mask_enabled_ = enable;
	if (!enable) stencil_ref_ = 0;
}

void RenderInterface_DX11::RenderToClipMask(
	Rml::ClipMaskOperation mask_operation, Rml::CompiledGeometryHandle geometry,
	Rml::Vector2f translation) {
	ZoneScopedN("DX11::RenderToClipMask");
	if (!geometry) return;

	float blend_factor[4] = {1,1,1,1};

	switch (mask_operation) {
	case Rml::ClipMaskOperation::Set:
		context_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_STENCIL, 1.0f, 0);
		stencil_ref_ = 1;
		context_->OMSetDepthStencilState(dss_set_.Get(), 1);
		context_->OMSetBlendState(blend_no_color_.Get(), blend_factor, 0xFFFFFFFF);
		break;
	case Rml::ClipMaskOperation::SetInverse:
		context_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_STENCIL, 1.0f, 1);
		stencil_ref_ = 1;
		context_->OMSetDepthStencilState(dss_set_.Get(), 0);
		context_->OMSetBlendState(blend_no_color_.Get(), blend_factor, 0xFFFFFFFF);
		break;
	case Rml::ClipMaskOperation::Intersect:
		context_->OMSetDepthStencilState(dss_intersect_.Get(), stencil_ref_);
		context_->OMSetBlendState(blend_no_color_.Get(), blend_factor, 0xFFFFFFFF);
		break;
	}

	auto* geom = reinterpret_cast<DX11_GeometryData*>(geometry);

	Rml::Matrix4f mvp = transform_active_ ? projection_ * transform_ : projection_;
	TransformCB cb{};
	std::memcpy(cb.transform, mvp.data(), 16 * sizeof(float));
	cb.translate[0] = translation.x;
	cb.translate[1] = translation.y;
	UpdateCB(cb_transform_.Get(), &cb, sizeof(cb));

	context_->VSSetShader(vs_main_.Get(), nullptr, 0);
	context_->PSSetShader(ps_color_.Get(), nullptr, 0);

	if (scissor_enabled_)
		context_->RSSetScissorRects(1, &scissor_rect_);
	else {
		D3D11_RECT full = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
		context_->RSSetScissorRects(1, &full);
	}

	UINT stride = sizeof(Rml::Vertex);
	UINT off = 0;
	context_->IASetVertexBuffers(0, 1, geom->vertex_buffer.GetAddressOf(), &stride, &off);
	context_->IASetIndexBuffer(geom->index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
	context_->DrawIndexed(static_cast<UINT>(geom->num_indices), 0, 0);

	if (mask_operation == Rml::ClipMaskOperation::Intersect)
		stencil_ref_++;
}

// ===========================================================================
// Layers
// ===========================================================================

int RenderInterface_DX11::AllocateLayer() {
	for (size_t i = 0; i < layer_in_use_.size(); ++i) {
		if (!layer_in_use_[i]) {
			auto* layer = layer_pool_[i];
			if (layer->width == width_ && layer->height == height_) {
				layer_in_use_[i] = true;
				return static_cast<int>(i);
			}
			// Dimensions changed — recreate
			delete layer_pool_[i];
			layer_pool_[i] = nullptr;
		}
	}

	// Find cleared slot or add new
	int slot = -1;
	for (size_t i = 0; i < layer_pool_.size(); ++i) {
		if (!layer_in_use_[i] && !layer_pool_[i]) {
			slot = static_cast<int>(i);
			break;
		}
	}

	if (slot < 0) {
		if (static_cast<int>(layer_pool_.size()) >= MAX_LAYER_COUNT) {
			std::fprintf(stderr, "[DX11] Layer pool exhausted\n");
			return -1;
		}
		slot = static_cast<int>(layer_pool_.size());
		layer_pool_.push_back(nullptr);
		layer_in_use_.push_back(false);
	}

	auto* layer = new DX11_LayerData();
	layer->width = width_;
	layer->height = height_;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = static_cast<UINT>(width_);
	desc.Height = static_cast<UINT>(height_);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = msaa_samples_;
	desc.SampleDesc.Quality = msaa_quality_;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	if (msaa_samples_ <= 1)
		desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;  // can sample directly when no MSAA

	HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &layer->color_texture);
	if (FAILED(hr)) { delete layer; return -1; }

	hr = device_->CreateRenderTargetView(layer->color_texture.Get(), nullptr, &layer->rtv);
	if (FAILED(hr)) { delete layer; return -1; }

	if (msaa_samples_ > 1) {
		// Non-MSAA resolve target for sampling
		D3D11_TEXTURE2D_DESC resolve_desc = desc;
		resolve_desc.SampleDesc.Count = 1;
		resolve_desc.SampleDesc.Quality = 0;
		resolve_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		hr = device_->CreateTexture2D(&resolve_desc, nullptr, &layer->resolve_texture);
		if (FAILED(hr)) { delete layer; return -1; }

		hr = device_->CreateShaderResourceView(layer->resolve_texture.Get(), nullptr, &layer->srv);
		if (FAILED(hr)) { delete layer; return -1; }
	} else {
		hr = device_->CreateShaderResourceView(layer->color_texture.Get(), nullptr, &layer->srv);
		if (FAILED(hr)) { delete layer; return -1; }
	}

	layer_pool_[slot] = layer;
	layer_in_use_[slot] = true;
	return slot;
}

void RenderInterface_DX11::FreeLayer(int layer_index) {
	if (layer_index >= 0 && layer_index < static_cast<int>(layer_in_use_.size()))
		layer_in_use_[layer_index] = false;
}

void RenderInterface_DX11::ReleaseAllLayers() {
	for (auto* layer : layer_pool_)
		delete layer;
	layer_pool_.clear();
	layer_in_use_.clear();
	layer_stack_.clear();
}

void RenderInterface_DX11::ResolveLayer(int layer_index) {
	if (msaa_samples_ <= 1) return;
	auto* layer = layer_pool_[layer_index];
	if (layer && layer->resolve_texture)
		context_->ResolveSubresource(layer->resolve_texture.Get(), 0,
			layer->color_texture.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
}

void RenderInterface_DX11::SetRenderTargetToLayer(int layer_index) {
	auto* layer = layer_pool_[layer_index];
	ID3D11RenderTargetView* rtvs[] = { layer->rtv.Get() };
	context_->OMSetRenderTargets(1, rtvs, dsv_.Get());
}

void RenderInterface_DX11::SetRenderTargetToBackBuffer() {
	ID3D11RenderTargetView* rtv = (msaa_samples_ > 1) ? msaa_rtv_.Get() : back_buffer_rtv_.Get();
	context_->OMSetRenderTargets(1, &rtv, dsv_.Get());
}

Rml::LayerHandle RenderInterface_DX11::PushLayer() {
	ZoneScopedN("DX11::PushLayer");
	int idx = AllocateLayer();
	if (idx < 0) return Rml::LayerHandle(0);

	layer_stack_.push_back(idx);

	const float transparent[4] = { 0, 0, 0, 0 };
	context_->ClearRenderTargetView(layer_pool_[idx]->rtv.Get(), transparent);
	context_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_STENCIL, 1.0f, 0);

	SetRenderTargetToLayer(idx);

	clip_mask_enabled_ = false;
	stencil_ref_ = 0;

	return static_cast<Rml::LayerHandle>(idx + 1);
}

void RenderInterface_DX11::PopLayer() {
	ZoneScopedN("DX11::PopLayer");
	if (layer_stack_.empty()) return;

	layer_stack_.pop_back();

	if (layer_stack_.empty())
		SetRenderTargetToBackBuffer();
	else
		SetRenderTargetToLayer(layer_stack_.back());

	clip_mask_enabled_ = false;
	stencil_ref_ = 0;
}

// Helper to draw fullscreen quad with passthrough shaders
static void DrawFullscreenQuad(ID3D11DeviceContext* ctx, DX11_GeometryData* quad) {
	UINT stride = sizeof(Rml::Vertex);
	UINT offset = 0;
	ctx->IASetVertexBuffers(0, 1, quad->vertex_buffer.GetAddressOf(), &stride, &offset);
	ctx->IASetIndexBuffer(quad->index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
	ctx->DrawIndexed(6, 0, 0);
}

void RenderInterface_DX11::CompositeLayers(
	Rml::LayerHandle source, Rml::LayerHandle destination,
	Rml::BlendMode blend_mode, Rml::Span<const Rml::CompiledFilterHandle> filters) {
	ZoneScopedN("DX11::CompositeLayers");

	int src_idx = static_cast<int>(source) - 1;
	int dst_idx = static_cast<int>(destination) - 1;

	if (src_idx < 0 || src_idx >= static_cast<int>(layer_pool_.size())) return;

	auto* src_layer = layer_pool_[src_idx];

	// Resolve MSAA source layer before sampling
	ResolveLayer(src_idx);

	bool has_filters = !filters.empty();
	if (has_filters)
		RenderFilters(filters, src_idx);

	ID3D11ShaderResourceView* composite_srv = has_filters
		? postprocess_targets_[0]->srv.Get()
		: src_layer->srv.Get();

	// Set destination render target (no DSV for compositing)
	ID3D11RenderTargetView* dst_rtv;
	if (dst_idx < 0)
		dst_rtv = (msaa_samples_ > 1) ? msaa_rtv_.Get() : back_buffer_rtv_.Get();
	else
		dst_rtv = layer_pool_[dst_idx]->rtv.Get();

	context_->OMSetRenderTargets(1, &dst_rtv, nullptr);

	D3D11_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(width_);
	viewport.Height = static_cast<float>(height_);
	viewport.MaxDepth = 1.0f;
	context_->RSSetViewports(1, &viewport);

	D3D11_RECT full = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
	context_->RSSetScissorRects(1, &full);

	float blend_factor[4] = {1,1,1,1};
	context_->OMSetBlendState(
		blend_mode == Rml::BlendMode::Replace ? blend_replace_.Get() : blend_premul_.Get(),
		blend_factor, 0xFFFFFFFF);
	context_->OMSetDepthStencilState(dss_disabled_.Get(), 0);

	context_->VSSetShader(vs_passthrough_.Get(), nullptr, 0);
	context_->PSSetShader(ps_passthrough_.Get(), nullptr, 0);
	context_->PSSetShaderResources(0, 1, &composite_srv);

	DrawFullscreenQuad(context_.Get(), fullscreen_quad_);

	// Unbind SRV
	ID3D11ShaderResourceView* null_srv = nullptr;
	context_->PSSetShaderResources(0, 1, &null_srv);

	// Restore DSV binding
	context_->OMSetRenderTargets(1, &dst_rtv, dsv_.Get());

	FreeLayer(src_idx);
}

Rml::TextureHandle RenderInterface_DX11::SaveLayerAsTexture() {
	ZoneScopedN("DX11::SaveLayerAsTexture");
	if (layer_stack_.empty()) return Rml::TextureHandle(0);

	int layer_idx = layer_stack_.back();
	ResolveLayer(layer_idx);
	auto* layer = layer_pool_[layer_idx];

	auto* tex = new DX11_TextureData();
	tex->texture = (msaa_samples_ > 1) ? layer->resolve_texture : layer->color_texture;
	tex->srv = layer->srv;
	tex->width = layer->width;
	tex->height = layer->height;
	tex->is_layer_texture = true;

	return reinterpret_cast<Rml::TextureHandle>(tex);
}

Rml::CompiledFilterHandle RenderInterface_DX11::SaveLayerAsMaskImage() {
	ZoneScopedN("DX11::SaveLayerAsMaskImage");
	if (layer_stack_.empty()) return Rml::CompiledFilterHandle(0);

	int layer_idx = layer_stack_.back();
	ResolveLayer(layer_idx);
	auto* layer = layer_pool_[layer_idx];

	auto* tex = new DX11_TextureData();
	tex->texture = (msaa_samples_ > 1) ? layer->resolve_texture : layer->color_texture;
	tex->srv = layer->srv;
	tex->width = layer->width;
	tex->height = layer->height;
	tex->is_layer_texture = true;

	return reinterpret_cast<Rml::CompiledFilterHandle>(tex);
}

// ===========================================================================
// Filters
// ===========================================================================

Rml::CompiledFilterHandle RenderInterface_DX11::CompileFilter(
	const Rml::String& name, const Rml::Dictionary& parameters) {

	auto* filter = new DX11_CompiledFilterData();

	if (name == "opacity") {
		filter->type = FilterType::Passthrough;
		filter->blend_factor = Rml::Get(parameters, "value", 1.0f);
	} else if (name == "blur") {
		filter->type = FilterType::Blur;
		filter->sigma = Rml::Get(parameters, "sigma", 1.0f);
	} else if (name == "drop-shadow") {
		filter->type = FilterType::DropShadow;
		filter->sigma = Rml::Get(parameters, "sigma", 0.f);
		filter->color = Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied();
		filter->offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.f));
	} else if (name == "brightness") {
		filter->type = FilterType::ColorMatrix;
		float v = Rml::Get(parameters, "value", 1.0f);
		filter->color_matrix = Rml::Matrix4f::Diag(v, v, v, 1.f);
	} else if (name == "contrast") {
		filter->type = FilterType::ColorMatrix;
		float v = Rml::Get(parameters, "value", 1.0f);
		float g = 0.5f - 0.5f * v;
		filter->color_matrix = Rml::Matrix4f::Diag(v, v, v, 1.f);
		filter->color_matrix.SetColumn(3, Rml::Vector4f(g, g, g, 1.f));
	} else if (name == "invert") {
		filter->type = FilterType::ColorMatrix;
		float v = Rml::Math::Clamp(Rml::Get(parameters, "value", 1.0f), 0.f, 1.f);
		float inv = 1.f - 2.f * v;
		filter->color_matrix = Rml::Matrix4f::Diag(inv, inv, inv, 1.f);
		filter->color_matrix.SetColumn(3, Rml::Vector4f(v, v, v, 1.f));
	} else if (name == "grayscale") {
		filter->type = FilterType::ColorMatrix;
		float v = Rml::Get(parameters, "value", 1.0f);
		float rv = 1.f - v;
		Rml::Vector3f gray = v * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
		filter->color_matrix = Rml::Matrix4f::FromRows(
			{gray.x + rv, gray.y, gray.z, 0.f},
			{gray.x, gray.y + rv, gray.z, 0.f},
			{gray.x, gray.y, gray.z + rv, 0.f},
			{0.f, 0.f, 0.f, 1.f});
	} else if (name == "sepia") {
		filter->type = FilterType::ColorMatrix;
		float v = Rml::Get(parameters, "value", 1.0f);
		float rv = 1.f - v;
		Rml::Vector3f r = v * Rml::Vector3f(0.393f, 0.769f, 0.189f);
		Rml::Vector3f g = v * Rml::Vector3f(0.349f, 0.686f, 0.168f);
		Rml::Vector3f b = v * Rml::Vector3f(0.272f, 0.534f, 0.131f);
		filter->color_matrix = Rml::Matrix4f::FromRows(
			{r.x + rv, r.y, r.z, 0.f},
			{g.x, g.y + rv, g.z, 0.f},
			{b.x, b.y, b.z + rv, 0.f},
			{0.f, 0.f, 0.f, 1.f});
	} else if (name == "hue-rotate") {
		filter->type = FilterType::ColorMatrix;
		float v = Rml::Get(parameters, "value", 1.0f);
		float s = Rml::Math::Sin(v), c = Rml::Math::Cos(v);
		filter->color_matrix = Rml::Matrix4f::FromRows(
			{0.213f+0.787f*c-0.213f*s, 0.715f-0.715f*c-0.715f*s, 0.072f-0.072f*c+0.928f*s, 0.f},
			{0.213f-0.213f*c+0.143f*s, 0.715f+0.285f*c+0.140f*s, 0.072f-0.072f*c-0.283f*s, 0.f},
			{0.213f-0.213f*c-0.787f*s, 0.715f-0.715f*c+0.715f*s, 0.072f+0.928f*c+0.072f*s, 0.f},
			{0.f, 0.f, 0.f, 1.f});
	} else if (name == "saturate") {
		filter->type = FilterType::ColorMatrix;
		float v = Rml::Get(parameters, "value", 1.0f);
		filter->color_matrix = Rml::Matrix4f::FromRows(
			{0.213f+0.787f*v, 0.715f-0.715f*v, 0.072f-0.072f*v, 0.f},
			{0.213f-0.213f*v, 0.715f+0.285f*v, 0.072f-0.072f*v, 0.f},
			{0.213f-0.213f*v, 0.715f-0.715f*v, 0.072f+0.928f*v, 0.f},
			{0.f, 0.f, 0.f, 1.f});
	} else {
		Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported filter '%s'.", name.c_str());
		delete filter;
		return Rml::CompiledFilterHandle(0);
	}

	return reinterpret_cast<Rml::CompiledFilterHandle>(filter);
}

void RenderInterface_DX11::ReleaseFilter(Rml::CompiledFilterHandle filter) {
	if (!filter) return;
	auto* as_filter = reinterpret_cast<DX11_CompiledFilterData*>(filter);
	if (as_filter->type >= FilterType::Passthrough && as_filter->type <= FilterType::MaskImage) {
		if (as_filter->type == FilterType::MaskImage && as_filter->mask_texture)
			delete as_filter->mask_texture;
		delete as_filter;
	} else {
		delete reinterpret_cast<DX11_TextureData*>(filter);
	}
}

// ===========================================================================
// Shaders (gradients)
// ===========================================================================

Rml::CompiledShaderHandle RenderInterface_DX11::CompileShader(
	const Rml::String& name, const Rml::Dictionary& parameters) {

	auto ApplyColorStopList = [](DX11_CompiledShaderData& shader, const Rml::Dictionary& params) {
		auto it = params.find("color_stop_list");
		if (it == params.end()) return;
		const auto& list = it->second.GetReference<Rml::ColorStopList>();
		int n = Rml::Math::Min(static_cast<int>(list.size()), MAX_NUM_STOPS);
		shader.stop_positions.resize(n);
		shader.stop_colors.resize(n);
		for (int i = 0; i < n; i++) {
			shader.stop_positions[i] = list[i].position.number;
			shader.stop_colors[i] = ConvertToColorf(list[i].color);
		}
	};

	auto* shader = new DX11_CompiledShaderData();

	if (name == "linear-gradient") {
		shader->type = CompiledShaderType::Gradient;
		bool rep = Rml::Get(parameters, "repeating", false);
		shader->gradient_function = rep ? ShaderGradientFunction::RepeatingLinear : ShaderGradientFunction::Linear;
		shader->p = Rml::Get(parameters, "p0", Rml::Vector2f(0.f));
		shader->v = Rml::Get(parameters, "p1", Rml::Vector2f(0.f)) - shader->p;
		ApplyColorStopList(*shader, parameters);
	} else if (name == "radial-gradient") {
		shader->type = CompiledShaderType::Gradient;
		bool rep = Rml::Get(parameters, "repeating", false);
		shader->gradient_function = rep ? ShaderGradientFunction::RepeatingRadial : ShaderGradientFunction::Radial;
		shader->p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
		shader->v = Rml::Vector2f(1.f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.f));
		ApplyColorStopList(*shader, parameters);
	} else if (name == "conic-gradient") {
		shader->type = CompiledShaderType::Gradient;
		bool rep = Rml::Get(parameters, "repeating", false);
		shader->gradient_function = rep ? ShaderGradientFunction::RepeatingConic : ShaderGradientFunction::Conic;
		shader->p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
		float angle = Rml::Get(parameters, "angle", 0.f);
		shader->v = {Rml::Math::Cos(angle), Rml::Math::Sin(angle)};
		ApplyColorStopList(*shader, parameters);
	} else {
		delete shader;
		return Rml::CompiledShaderHandle(0);
	}

	return reinterpret_cast<Rml::CompiledShaderHandle>(shader);
}

void RenderInterface_DX11::RenderShader(
	Rml::CompiledShaderHandle shader_handle, Rml::CompiledGeometryHandle geometry_handle,
	Rml::Vector2f translation, Rml::TextureHandle /*texture*/) {
	ZoneScopedN("DX11::RenderShader");
	if (!shader_handle || !geometry_handle) return;

	auto* shader = reinterpret_cast<const DX11_CompiledShaderData*>(shader_handle);
	auto* geom = reinterpret_cast<DX11_GeometryData*>(geometry_handle);
	if (shader->type != CompiledShaderType::Gradient) return;

	Rml::Matrix4f mvp = transform_active_ ? projection_ * transform_ : projection_;
	TransformCB tcb{};
	std::memcpy(tcb.transform, mvp.data(), 16 * sizeof(float));
	tcb.translate[0] = translation.x;
	tcb.translate[1] = translation.y;
	UpdateCB(cb_transform_.Get(), &tcb, sizeof(tcb));

	int num_stops = static_cast<int>(shader->stop_positions.size());
	GradientCB gcb{};
	gcb.func = static_cast<int>(shader->gradient_function);
	gcb.num_stops = num_stops;
	gcb.p[0] = shader->p.x; gcb.p[1] = shader->p.y;
	gcb.v[0] = shader->v.x; gcb.v[1] = shader->v.y;
	for (int i = 0; i < num_stops && i < MAX_NUM_STOPS; ++i) {
		gcb.stop_colors[i*4+0] = shader->stop_colors[i].red;
		gcb.stop_colors[i*4+1] = shader->stop_colors[i].green;
		gcb.stop_colors[i*4+2] = shader->stop_colors[i].blue;
		gcb.stop_colors[i*4+3] = shader->stop_colors[i].alpha;
		gcb.stop_positions[i] = shader->stop_positions[i];
	}
	UpdateCB(cb_filter_.Get(), &gcb, sizeof(gcb));

	context_->VSSetShader(vs_main_.Get(), nullptr, 0);
	context_->PSSetShader(ps_gradient_.Get(), nullptr, 0);

	float blend_factor[4] = {1,1,1,1};
	context_->OMSetBlendState(blend_premul_.Get(), blend_factor, 0xFFFFFFFF);

	if (clip_mask_enabled_)
		context_->OMSetDepthStencilState(dss_equal_.Get(), stencil_ref_);
	else
		context_->OMSetDepthStencilState(dss_disabled_.Get(), 0);

	if (scissor_enabled_)
		context_->RSSetScissorRects(1, &scissor_rect_);
	else {
		D3D11_RECT full = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
		context_->RSSetScissorRects(1, &full);
	}

	UINT stride = sizeof(Rml::Vertex);
	UINT off = 0;
	context_->IASetVertexBuffers(0, 1, geom->vertex_buffer.GetAddressOf(), &stride, &off);
	context_->IASetIndexBuffer(geom->index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
	context_->DrawIndexed(static_cast<UINT>(geom->num_indices), 0, 0);
}

void RenderInterface_DX11::ReleaseShader(Rml::CompiledShaderHandle shader_handle) {
	if (!shader_handle) return;
	delete reinterpret_cast<DX11_CompiledShaderData*>(shader_handle);
}

// ===========================================================================
// Filter rendering helpers
// ===========================================================================

static void ComputeBlurWeights(float sigma, float* weights, int num_weights) {
	float norm = 0.0f;
	for (int i = 0; i < num_weights; i++) {
		if (std::fabs(sigma) < 0.1f)
			weights[i] = (i == 0) ? 1.0f : 0.0f;
		else
			weights[i] = std::exp(-float(i * i) / (2.0f * sigma * sigma)) /
				(std::sqrt(2.f * 3.14159265f) * sigma);
		norm += (i == 0 ? 1.f : 2.0f) * weights[i];
	}
	for (int i = 0; i < num_weights; i++)
		weights[i] /= norm;
}

void RenderInterface_DX11::RenderBlur(float sigma, int src_pp, int dst_pp) {
	ZoneScopedN("DX11::RenderBlur");
	auto* src = postprocess_targets_[src_pp];
	auto* dst = postprocess_targets_[dst_pp];

	float weights[BLUR_NUM_WEIGHTS];
	ComputeBlurWeights(sigma, weights, BLUR_NUM_WEIGHTS);

	D3D11_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(width_);
	viewport.Height = static_cast<float>(height_);
	viewport.MaxDepth = 1.0f;
	context_->RSSetViewports(1, &viewport);

	D3D11_RECT full = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
	context_->RSSetScissorRects(1, &full);

	float blend_factor[4] = {1,1,1,1};
	context_->OMSetBlendState(blend_replace_.Get(), blend_factor, 0xFFFFFFFF);
	context_->OMSetDepthStencilState(dss_disabled_.Get(), 0);
	context_->VSSetShader(vs_passthrough_.Get(), nullptr, 0);
	context_->PSSetShader(ps_blur_.Get(), nullptr, 0);

	// Pass 1: Vertical (src -> dst)
	{
		FilterCB cb{};
		cb.data[0] = 0.0f;
		cb.data[1] = 1.0f / static_cast<float>(height_);
		cb.data[4] = weights[0]; cb.data[5] = weights[1]; cb.data[6] = weights[2]; cb.data[7] = weights[3];
		UpdateCB(cb_filter_.Get(), &cb, sizeof(cb));

		context_->OMSetRenderTargets(1, dst->rtv.GetAddressOf(), nullptr);
		context_->PSSetShaderResources(0, 1, src->srv.GetAddressOf());
		DrawFullscreenQuad(context_.Get(), fullscreen_quad_);
		ID3D11ShaderResourceView* null_srv = nullptr;
		context_->PSSetShaderResources(0, 1, &null_srv);
	}

	// Pass 2: Horizontal (dst -> src)
	{
		FilterCB cb{};
		cb.data[0] = 1.0f / static_cast<float>(width_);
		cb.data[1] = 0.0f;
		cb.data[4] = weights[0]; cb.data[5] = weights[1]; cb.data[6] = weights[2]; cb.data[7] = weights[3];
		UpdateCB(cb_filter_.Get(), &cb, sizeof(cb));

		context_->OMSetRenderTargets(1, src->rtv.GetAddressOf(), nullptr);
		context_->PSSetShaderResources(0, 1, dst->srv.GetAddressOf());
		DrawFullscreenQuad(context_.Get(), fullscreen_quad_);
		ID3D11ShaderResourceView* null_srv = nullptr;
		context_->PSSetShaderResources(0, 1, &null_srv);
	}
}

void RenderInterface_DX11::RenderFilters(
	Rml::Span<const Rml::CompiledFilterHandle> filters, int source_layer_idx) {
	ZoneScopedN("DX11::RenderFilters");
	if (filters.empty()) return;

	auto* src_layer = layer_pool_[source_layer_idx];
	auto* pp0 = postprocess_targets_[0];
	auto* pp1 = postprocess_targets_[1];

	float blend_factor[4] = {1,1,1,1};
	D3D11_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(width_);
	viewport.Height = static_cast<float>(height_);
	viewport.MaxDepth = 1.0f;
	D3D11_RECT full = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };

	// Blit source layer -> pp0
	{
		context_->RSSetViewports(1, &viewport);
		context_->RSSetScissorRects(1, &full);
		context_->OMSetRenderTargets(1, pp0->rtv.GetAddressOf(), nullptr);
		context_->OMSetBlendState(blend_replace_.Get(), blend_factor, 0xFFFFFFFF);
		context_->OMSetDepthStencilState(dss_disabled_.Get(), 0);
		context_->VSSetShader(vs_passthrough_.Get(), nullptr, 0);
		context_->PSSetShader(ps_passthrough_.Get(), nullptr, 0);
		context_->PSSetShaderResources(0, 1, src_layer->srv.GetAddressOf());
		DrawFullscreenQuad(context_.Get(), fullscreen_quad_);
		ID3D11ShaderResourceView* null_srv = nullptr;
		context_->PSSetShaderResources(0, 1, &null_srv);
	}

	for (const Rml::CompiledFilterHandle fh : filters) {
		if (!fh) continue;

		auto* filter = reinterpret_cast<const DX11_CompiledFilterData*>(fh);

		// Check if this is a mask image (TextureData from SaveLayerAsMaskImage)
		if (filter->type < FilterType::Passthrough || filter->type > FilterType::MaskImage) {
			auto* mask_tex = reinterpret_cast<DX11_TextureData*>(fh);

			context_->OMSetRenderTargets(1, pp1->rtv.GetAddressOf(), nullptr);
			context_->OMSetBlendState(blend_replace_.Get(), blend_factor, 0xFFFFFFFF);
			context_->VSSetShader(vs_passthrough_.Get(), nullptr, 0);
			context_->PSSetShader(ps_blend_mask_.Get(), nullptr, 0);

			ID3D11ShaderResourceView* srvs[2] = { pp0->srv.Get(), mask_tex->srv.Get() };
			context_->PSSetShaderResources(0, 2, srvs);
			DrawFullscreenQuad(context_.Get(), fullscreen_quad_);
			ID3D11ShaderResourceView* null_srvs[2] = {};
			context_->PSSetShaderResources(0, 2, null_srvs);

			std::swap(postprocess_targets_[0], postprocess_targets_[1]);
			pp0 = postprocess_targets_[0];
			pp1 = postprocess_targets_[1];
			continue;
		}

		switch (filter->type) {
		case FilterType::Passthrough:
		case FilterType::ColorMatrix: {
			FilterCB cb{};
			if (filter->type == FilterType::Passthrough) {
				float bf = filter->blend_factor;
				cb.data[0]=bf; cb.data[5]=bf; cb.data[10]=bf; cb.data[15]=bf;
			} else {
				std::memcpy(cb.data, filter->color_matrix.data(), 16 * sizeof(float));
			}
			UpdateCB(cb_filter_.Get(), &cb, sizeof(cb));

			context_->OMSetRenderTargets(1, pp1->rtv.GetAddressOf(), nullptr);
			context_->OMSetBlendState(blend_replace_.Get(), blend_factor, 0xFFFFFFFF);
			context_->VSSetShader(vs_passthrough_.Get(), nullptr, 0);
			context_->PSSetShader(ps_color_matrix_.Get(), nullptr, 0);
			context_->PSSetShaderResources(0, 1, pp0->srv.GetAddressOf());
			DrawFullscreenQuad(context_.Get(), fullscreen_quad_);
			ID3D11ShaderResourceView* null_srv = nullptr;
			context_->PSSetShaderResources(0, 1, &null_srv);

			std::swap(postprocess_targets_[0], postprocess_targets_[1]);
			pp0 = postprocess_targets_[0]; pp1 = postprocess_targets_[1];
			break;
		}

		case FilterType::Blur:
			RenderBlur(filter->sigma, 0, 1);
			pp0 = postprocess_targets_[0]; pp1 = postprocess_targets_[1];
			break;

		case FilterType::DropShadow: {
			// Step 1: shadow in pp1
			{
				FilterCB cb{};
				cb.data[0] = -filter->offset.x / static_cast<float>(width_);
				cb.data[1] = filter->offset.y / static_cast<float>(height_);
				Rml::Colourf sc = ConvertToColorf(filter->color);
				cb.data[4] = sc.red; cb.data[5] = sc.green; cb.data[6] = sc.blue; cb.data[7] = sc.alpha;
				UpdateCB(cb_filter_.Get(), &cb, sizeof(cb));

				context_->OMSetRenderTargets(1, pp1->rtv.GetAddressOf(), nullptr);
				context_->OMSetBlendState(blend_replace_.Get(), blend_factor, 0xFFFFFFFF);
				context_->VSSetShader(vs_passthrough_.Get(), nullptr, 0);
				context_->PSSetShader(ps_drop_shadow_.Get(), nullptr, 0);
				context_->PSSetShaderResources(0, 1, pp0->srv.GetAddressOf());
				DrawFullscreenQuad(context_.Get(), fullscreen_quad_);
				ID3D11ShaderResourceView* null_srv = nullptr;
				context_->PSSetShaderResources(0, 1, &null_srv);
			}

			// Step 2: blur shadow
			if (filter->sigma >= 0.5f) {
				std::swap(postprocess_targets_[0], postprocess_targets_[1]);
				pp0 = postprocess_targets_[0]; pp1 = postprocess_targets_[1];
				RenderBlur(filter->sigma, 0, 1);
			}

			// Step 3: composite original on shadow
			{
				int psr_idx = (filter->sigma >= 0.5f) ? 1 : 0;
				int rt_idx = (filter->sigma >= 0.5f) ? 0 : 1;

				auto* psr_pp = postprocess_targets_[psr_idx];
				auto* rt_pp = postprocess_targets_[rt_idx];

				context_->OMSetRenderTargets(1, rt_pp->rtv.GetAddressOf(), nullptr);
				context_->OMSetBlendState(blend_premul_.Get(), blend_factor, 0xFFFFFFFF);
				context_->VSSetShader(vs_passthrough_.Get(), nullptr, 0);
				context_->PSSetShader(ps_passthrough_.Get(), nullptr, 0);
				context_->PSSetShaderResources(0, 1, psr_pp->srv.GetAddressOf());
				DrawFullscreenQuad(context_.Get(), fullscreen_quad_);
				ID3D11ShaderResourceView* null_srv = nullptr;
				context_->PSSetShaderResources(0, 1, &null_srv);

				if (rt_idx != 0) std::swap(postprocess_targets_[0], postprocess_targets_[1]);
				pp0 = postprocess_targets_[0]; pp1 = postprocess_targets_[1];
			}
			break;
		}

		case FilterType::MaskImage:
		case FilterType::Invalid:
			break;
		}
	}
}
