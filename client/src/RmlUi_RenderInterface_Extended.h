#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <cstdint>

namespace Backend {
struct RmlRendererSettings {
	bool vsync;
	unsigned char msaa_sample_count;
};
} // namespace Backend

// Extended render interface adding YUV/NV12 video texture support, streaming texture updates,
// and geometry vertex updates beyond what the base Rml::RenderInterface provides.
// DX11/DX12 backends implement all methods; Metal implements only the NV12/YUV video methods.
class ExtendedRenderInterface : public Rml::RenderInterface {
public:
	virtual ~ExtendedRenderInterface() = default;

	// Returns true if the renderer was successfully constructed.
	virtual explicit operator bool() const { return true; }

	// The viewport should be updated whenever the window size changes.
	// DX renderers override this; Metal has its own SetViewport(int,int) entry point.
	virtual void SetViewport(int /*viewport_width*/, int /*viewport_height*/, bool /*force*/ = false) {}

	// Sets up GPU states for taking rendering commands from RmlUi.
	// DX renderers override; Metal uses BeginFrame(MTLCommandBuffer, MTLRenderPassDescriptor*).
	virtual void BeginFrame() {}

	// Optional, can be used to clear the active framebuffer.
	virtual void Clear() {}

	// Presents to screen and synchronizes.
	// DX renderers override; Metal uses its own EndFrame() entry point.
	virtual void EndFrame() {}

	// Re-map existing VB with new vertex data (no GPU resource allocation).
	// DX renderers override; Metal does not currently implement this.
	virtual void UpdateGeometryVertices(Rml::CompiledGeometryHandle /*geometry*/, Rml::Span<const Rml::Vertex> /*vertices*/) {}

	// Updates pixel data of an existing texture in-place (no resource/SRV reallocation).
	// DX renderers override; Metal does not currently implement this.
	virtual void UpdateTextureData(Rml::TextureHandle /*texture_handle*/, Rml::Span<const Rml::byte> /*source_data*/, Rml::Vector2i /*source_dimensions*/) {}

	// YUV texture support (I420: 3 x R8 planes -> RGB in pixel shader)
	virtual uintptr_t GenerateYUVTexture(
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) = 0;
	virtual void UpdateYUVTexture(uintptr_t handle,
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) = 0;
	virtual void ReleaseYUVTexture(uintptr_t handle) = 0;
	virtual void RenderYUVGeometry(Rml::CompiledGeometryHandle geometry,
		Rml::Vector2f translation, uintptr_t yuv_handle) = 0;

	// NV12 texture support (R8 Y + R8G8 UV -> RGB in pixel shader)
	virtual uintptr_t GenerateNV12Texture(
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* uv_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) = 0;
	virtual void UpdateNV12Texture(uintptr_t handle,
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* uv_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) = 0;
	virtual void ReleaseNV12Texture(uintptr_t handle) = 0;
	virtual void RenderNV12Geometry(Rml::CompiledGeometryHandle geometry,
		Rml::Vector2f translation, uintptr_t nv12_handle) = 0;
};
