#include <client/video_element.h>

#include "dx12/RmlUi_Renderer_DX12.h"

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Context.h>

#include <cstring>

namespace parties::client {

// ── VideoElement ────────────────────────────────────────────────────

VideoElement::VideoElement(const Rml::String& tag)
    : Rml::Element(tag) {}

VideoElement::~VideoElement() {
    ReleaseResources();
}

void VideoElement::UpdateFrame(const uint8_t* rgba_data, uint32_t width, uint32_t height) {
    bool size_changed = (frame_width_ != width || frame_height_ != height);
    frame_width_  = width;
    frame_height_ = height;
    has_frame_ = true;

    // Store RGBA pixel data
    size_t byte_count = static_cast<size_t>(width) * height * 4;
    frame_data_.resize(byte_count);
    std::memcpy(frame_data_.data(), rgba_data, byte_count);
    texture_dirty_ = true;

    if (size_changed)
        DirtyLayout();
}

void VideoElement::SetVideoDimensions(uint32_t width, uint32_t height) {
    bool size_changed = (frame_width_ != width || frame_height_ != height);
    frame_width_  = width;
    frame_height_ = height;
    has_frame_ = true;

    if (size_changed)
        DirtyLayout();
}

void VideoElement::Clear() {
    has_frame_ = false;
    frame_width_  = 0;
    frame_height_ = 0;
    frame_data_.clear();
    texture_dirty_ = false;
    ReleaseResources();
    DirtyLayout();
}

void VideoElement::ReleaseResources() {
    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;

    if (video_texture_) {
        ri->ReleaseTexture(video_texture_);
        video_texture_ = 0;
    }
    texture_w_ = texture_h_ = 0;
    if (video_geom_) {
        ri->ReleaseGeometry(video_geom_);
        video_geom_ = 0;
    }
    geom_w_ = geom_h_ = 0;
}

bool VideoElement::GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio) {
    if (frame_width_ > 0 && frame_height_ > 0) {
        dimensions.x = static_cast<float>(frame_width_);
        dimensions.y = static_cast<float>(frame_height_);
        ratio = dimensions.x / dimensions.y;
        return true;
    }
    return false;
}

void VideoElement::RebuildGeometry() {
    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;

    if (video_geom_) {
        ri->ReleaseGeometry(video_geom_);
        video_geom_ = 0;
    }

    // Compute aspect-ratio-preserving rect within element content box
    Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    float elem_w = size.x, elem_h = size.y;
    if (elem_w <= 0 || elem_h <= 0) return;

    float video_aspect = static_cast<float>(frame_width_) / static_cast<float>(frame_height_);
    float elem_aspect  = elem_w / elem_h;

    float rw, rh;
    if (video_aspect > elem_aspect) {
        rw = elem_w;
        rh = elem_w / video_aspect;
    } else {
        rh = elem_h;
        rw = elem_h * video_aspect;
    }

    float ox = (elem_w - rw) * 0.5f;
    float oy = (elem_h - rh) * 0.5f;

    // Build a textured quad (2 triangles)
    Rml::Vertex vertices[4];
    for (auto& v : vertices)
        v.colour = Rml::ColourbPremultiplied(255, 255, 255, 255);

    vertices[0].position = {ox, oy};
    vertices[0].tex_coord = {0.0f, 0.0f};

    vertices[1].position = {ox + rw, oy};
    vertices[1].tex_coord = {1.0f, 0.0f};

    vertices[2].position = {ox + rw, oy + rh};
    vertices[2].tex_coord = {1.0f, 1.0f};

    vertices[3].position = {ox, oy + rh};
    vertices[3].tex_coord = {0.0f, 1.0f};

    int indices[6] = {0, 1, 2, 0, 2, 3};

    video_geom_ = ri->CompileGeometry({vertices, 4}, {indices, 6});
    geom_w_ = elem_w;
    geom_h_ = elem_h;
}

void VideoElement::OnResize() {
    Rml::Element::OnResize();
    // Invalidate geometry so it gets rebuilt with new dimensions
    Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (size.x != geom_w_ || size.y != geom_h_) {
        if (video_geom_) {
            auto* ri = Rml::GetRenderInterface();
            if (ri) { ri->ReleaseGeometry(video_geom_); video_geom_ = 0; }
        }
    }
}

void VideoElement::OnRender() {
    if (!has_frame_ || frame_width_ == 0 || frame_height_ == 0) return;
    if (frame_data_.empty()) return;

    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;

    // Update texture if frame data changed
    if (texture_dirty_) {
        Rml::Vector2i dims(static_cast<int>(frame_width_), static_cast<int>(frame_height_));
        Rml::Span<const Rml::byte> data{frame_data_.data(), frame_data_.size()};

        if (video_texture_ && texture_w_ == frame_width_ && texture_h_ == frame_height_) {
            // Same dimensions — update data in-place (no resource/SRV reallocation)
            auto* dx12_ri = static_cast<RenderInterface_DX12*>(ri);
            dx12_ri->UpdateTextureData(video_texture_, data, dims);
        } else {
            // Dimensions changed or first frame — recreate texture
            if (video_texture_) {
                ri->ReleaseTexture(video_texture_);
                video_texture_ = 0;
            }
            video_texture_ = ri->GenerateTexture(data, dims);
            texture_w_ = frame_width_;
            texture_h_ = frame_height_;
        }
        texture_dirty_ = false;
    }

    // Rebuild geometry if element size changed
    Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (!video_geom_ || size.x != geom_w_ || size.y != geom_h_)
        RebuildGeometry();

    if (!video_texture_ || !video_geom_) return;

    // Draw the textured quad at the element's content box origin
    Rml::Vector2f offset = GetAbsoluteOffset(Rml::BoxArea::Content);
    ri->RenderGeometry(video_geom_, offset, video_texture_);
}

// ── Instancer ───────────────────────────────────────────────────────

Rml::ElementPtr VideoElementInstancer::InstanceElement(
    Rml::Element* /*parent*/, const Rml::String& tag,
    const Rml::XMLAttributes& /*attributes*/)
{
    return Rml::ElementPtr(new VideoElement(tag));
}

void VideoElementInstancer::ReleaseElement(Rml::Element* element) {
    delete element;
}

} // namespace parties::client
