#pragma once

#include "../RmlUi_RenderInterface_Extended.h"

#ifndef RMLUI_PLATFORM_WIN32
	#error "DirectX 11 renderer only supported on Windows"
#endif

// Forward declarations
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11ShaderResourceView;
struct ID3D11Buffer;
struct ID3D11Texture2D;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11InputLayout;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11RasterizerState;
struct ID3D11SamplerState;
struct IDXGISwapChain1;
struct IDXGIFactory2;
struct IDCompositionDevice;
struct IDCompositionTarget;
struct IDCompositionVisual;

// clang-format off
#include "../dx12/RmlUi_Include_Windows.h"
// clang-format on

#include <wrl/client.h>
#include <cstdint>
#include <vector>

// Internal data structures — forward declared, defined in .cpp
struct DX11_GeometryData;
struct DX11_TextureData;
struct DX11_LayerData;
struct DX11_PostprocessTarget;
struct DX11_CompiledFilterData;
struct DX11_CompiledShaderData;
struct DX11_YUVTextureData;
struct DX11_NV12TextureData;

class RenderInterface_DX11 : public ExtendedRenderInterface {
public:
	static constexpr int NUM_BACK_BUFFERS = 2;
	static constexpr int MAX_LAYER_COUNT = 16;
	static constexpr int NUM_POSTPROCESS_TARGETS = 2;

	RenderInterface_DX11(void* p_window_handle, const Backend::RmlRendererSettings& settings);
	~RenderInterface_DX11();

	explicit operator bool() const override;

	void SetViewport(int viewport_width, int viewport_height, bool force = false) override;
	void BeginFrame() override;
	void Clear() override;
	void EndFrame() override;

	// -- Inherited from Rml::RenderInterface --

	Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
	void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
	void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

	void UpdateGeometryVertices(Rml::CompiledGeometryHandle geometry, Rml::Span<const Rml::Vertex> vertices) override;

	Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
	Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) override;
	void ReleaseTexture(Rml::TextureHandle texture_handle) override;

	void UpdateTextureData(Rml::TextureHandle texture_handle, Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) override;

	// YUV (I420)
	uintptr_t GenerateYUVTexture(
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) override;
	void UpdateYUVTexture(uintptr_t handle,
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) override;
	void ReleaseYUVTexture(uintptr_t handle) override;
	void RenderYUVGeometry(Rml::CompiledGeometryHandle geometry,
		Rml::Vector2f translation, uintptr_t yuv_handle) override;

	// NV12
	uintptr_t GenerateNV12Texture(
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* uv_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) override;
	void UpdateNV12Texture(uintptr_t handle,
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* uv_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) override;
	void ReleaseNV12Texture(uintptr_t handle) override;
	void RenderNV12Geometry(Rml::CompiledGeometryHandle geometry,
		Rml::Vector2f translation, uintptr_t nv12_handle) override;

	void EnableScissorRegion(bool enable) override;
	void SetScissorRegion(Rml::Rectanglei region) override;

	void SetTransform(const Rml::Matrix4f* transform) override;

	void EnableClipMask(bool enable) override;
	void RenderToClipMask(Rml::ClipMaskOperation mask_operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;

	Rml::LayerHandle PushLayer() override;
	void PopLayer() override;
	void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
		Rml::Span<const Rml::CompiledFilterHandle> filters) override;

	Rml::TextureHandle SaveLayerAsTexture() override;
	Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;

	Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
	void ReleaseFilter(Rml::CompiledFilterHandle filter) override;

	Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
	void RenderShader(Rml::CompiledShaderHandle shader_handle, Rml::CompiledGeometryHandle geometry_handle, Rml::Vector2f translation,
		Rml::TextureHandle texture) override;
	void ReleaseShader(Rml::CompiledShaderHandle effect_handle) override;

private:
	template <typename T>
	using ComPtr = Microsoft::WRL::ComPtr<T>;

	// --- Initialization ---
	bool CreateDeviceAndSwapChain(int width, int height);
	bool CreateRenderTargetView();
	bool CreateMSAARenderTarget();
	bool CreateDepthStencilBuffer();
	bool CompileShaders();
	bool CreateStates();
	bool CreateFullscreenQuad();
	bool CreatePostprocessTargets();

	// --- Layer management ---
	int AllocateLayer();
	void FreeLayer(int layer_index);
	void ReleaseAllLayers();
	void ResolveLayer(int layer_index);
	void SetRenderTargetToLayer(int layer_index);
	void SetRenderTargetToBackBuffer();

	// --- Postprocess ---
	void ReleasePostprocessTargets();
	void RenderFilters(Rml::Span<const Rml::CompiledFilterHandle> filters, int source_layer_idx);
	void RenderBlur(float sigma, int src_pp, int dst_pp);

	// --- CB helpers ---
	void UpdateCB(ID3D11Buffer* buffer, const void* data, UINT size);

	// --- Texture upload helpers ---
	void UploadR8Texture(ID3D11Texture2D* tex, const uint8_t* data, uint32_t src_stride, int width, int height);
	void UploadR8G8Texture(ID3D11Texture2D* tex, const uint8_t* data, uint32_t src_stride, int width, int height);

	// --- State ---
	bool valid_ = false;
	bool vsync_ = true;
	uint32_t msaa_samples_ = 1;
	uint32_t msaa_quality_ = 0;
	int width_ = 0;
	int height_ = 0;
	HWND hwnd_ = nullptr;
	HANDLE frame_latency_waitable_ = nullptr;

	// Core objects
	ComPtr<IDXGIFactory2> factory_;
	ComPtr<ID3D11Device> device_;
	ComPtr<ID3D11DeviceContext> context_;
	ComPtr<IDXGISwapChain1> swap_chain_;

	// DirectComposition
	ComPtr<IDCompositionDevice> dcomp_device_;
	ComPtr<IDCompositionTarget> dcomp_target_;
	ComPtr<IDCompositionVisual> dcomp_visual_;

	// Back buffer (non-MSAA, from swap chain)
	ComPtr<ID3D11RenderTargetView> back_buffer_rtv_;

	// MSAA intermediate render target (resolved to back buffer before present)
	ComPtr<ID3D11Texture2D> msaa_color_texture_;
	ComPtr<ID3D11RenderTargetView> msaa_rtv_;

	// Depth/stencil (MSAA-matched)
	ComPtr<ID3D11Texture2D> depth_stencil_texture_;
	ComPtr<ID3D11DepthStencilView> dsv_;

	// Shaders
	ComPtr<ID3D11VertexShader> vs_main_;
	ComPtr<ID3D11VertexShader> vs_passthrough_;
	ComPtr<ID3D11PixelShader> ps_color_;
	ComPtr<ID3D11PixelShader> ps_texture_;
	ComPtr<ID3D11PixelShader> ps_passthrough_;
	ComPtr<ID3D11PixelShader> ps_yuv_;
	ComPtr<ID3D11PixelShader> ps_nv12_;
	ComPtr<ID3D11PixelShader> ps_color_matrix_;
	ComPtr<ID3D11PixelShader> ps_blur_;
	ComPtr<ID3D11PixelShader> ps_drop_shadow_;
	ComPtr<ID3D11PixelShader> ps_blend_mask_;
	ComPtr<ID3D11PixelShader> ps_gradient_;
	ComPtr<ID3D11InputLayout> input_layout_;

	// States
	ComPtr<ID3D11BlendState> blend_premul_;        // premultiplied alpha
	ComPtr<ID3D11BlendState> blend_replace_;        // no blending, overwrite
	ComPtr<ID3D11BlendState> blend_no_color_;       // color write disabled (stencil only)
	ComPtr<ID3D11DepthStencilState> dss_disabled_;  // stencil disabled
	ComPtr<ID3D11DepthStencilState> dss_equal_;     // stencil EQUAL test
	ComPtr<ID3D11DepthStencilState> dss_set_;       // stencil REPLACE (always)
	ComPtr<ID3D11DepthStencilState> dss_intersect_; // stencil INCR_SAT (equal)
	ComPtr<ID3D11RasterizerState> rasterizer_;
	ComPtr<ID3D11SamplerState> sampler_linear_;

	// Constant buffers
	ComPtr<ID3D11Buffer> cb_transform_;  // b0: TransformCB
	ComPtr<ID3D11Buffer> cb_filter_;     // b1: FilterCB/GradientCB

	// Fullscreen quad
	DX11_GeometryData* fullscreen_quad_ = nullptr;

	// Postprocess targets
	DX11_PostprocessTarget* postprocess_targets_[NUM_POSTPROCESS_TARGETS] = {};

	// Layer pool and stack
	std::vector<DX11_LayerData*> layer_pool_;
	std::vector<bool> layer_in_use_;
	std::vector<int> layer_stack_;

	// Render state
	Rml::Matrix4f projection_ = Rml::Matrix4f::Identity();
	Rml::Matrix4f transform_ = Rml::Matrix4f::Identity();
	bool transform_active_ = false;
	bool scissor_enabled_ = false;
	RECT scissor_rect_ = {};

	// Clip mask state
	bool clip_mask_enabled_ = false;
	uint32_t stencil_ref_ = 0;
};
