#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>

#include <cstdint>
#include <vector>

namespace parties::client {

// Custom RmlUi element that renders video frames as GPU textures.
// Receives RGBA pixel data, creates an RmlUi texture, and draws it
// as a textured quad during the render pass.
class VideoElement : public Rml::Element {
public:
    explicit VideoElement(const Rml::String& tag);
    ~VideoElement() override;

    // Upload a new RGBA video frame.
    void UpdateFrame(const uint8_t* rgba_data, uint32_t width, uint32_t height);

    // Set layout dimensions without pixel data (for placeholder sizing).
    void SetVideoDimensions(uint32_t width, uint32_t height);
    void Clear();

    uint32_t frame_width() const { return frame_width_; }
    uint32_t frame_height() const { return frame_height_; }

protected:
    bool GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio) override;
    void OnRender() override;
    void OnResize() override;

private:
    void ReleaseResources();
    void RebuildGeometry();

    uint32_t frame_width_ = 0;
    uint32_t frame_height_ = 0;
    bool has_frame_ = false;

    // Texture from latest frame
    std::vector<uint8_t> frame_data_;
    Rml::TextureHandle video_texture_ = 0;
    uint32_t texture_w_ = 0;
    uint32_t texture_h_ = 0;
    bool texture_dirty_ = false;

    // Compiled quad geometry
    Rml::CompiledGeometryHandle video_geom_ = 0;
    float geom_w_ = 0;
    float geom_h_ = 0;
};

class VideoElementInstancer : public Rml::ElementInstancer {
public:
    VideoElementInstancer() = default;
    Rml::ElementPtr InstanceElement(Rml::Element* parent, const Rml::String& tag,
                                     const Rml::XMLAttributes& attributes) override;
    void ReleaseElement(Rml::Element* element) override;
};

} // namespace parties::client
