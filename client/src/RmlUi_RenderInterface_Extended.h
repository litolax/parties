#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <cstdint>

namespace Backend {
struct RmlRendererSettings {
	bool vsync;
	unsigned char msaa_sample_count;
};
} // namespace Backend

// Extended render interface with custom methods shared between DX11 and DX12 backends.
// Adds YUV/NV12 video texture support, streaming texture updates, and geometry vertex updates
// beyond what the base Rml::RenderInterface provides.
class ExtendedRenderInterface : public Rml::RenderInterface {
public:
	virtual ~ExtendedRenderInterface() = default;

	// Returns true if the renderer was successfully constructed.
	virtual explicit operator bool() const = 0;

	// The viewport should be updated whenever the window size changes.
	virtual void SetViewport(int viewport_width, int viewport_height, bool force = false) = 0;

	// Sets up GPU states for taking rendering commands from RmlUi.
	virtual void BeginFrame() = 0;

	// Optional, can be used to clear the active framebuffer.
	virtual void Clear() = 0;

	// Presents to screen and synchronizes.
	virtual void EndFrame() = 0;

	// Re-map existing VB with new vertex data (no GPU resource allocation).
	virtual void UpdateGeometryVertices(Rml::CompiledGeometryHandle geometry, Rml::Span<const Rml::Vertex> vertices) = 0;

	// Updates pixel data of an existing texture in-place (no resource/SRV reallocation).
	// Dimensions must match the original texture. For streaming video.
	virtual void UpdateTextureData(Rml::TextureHandle texture_handle, Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) = 0;

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
