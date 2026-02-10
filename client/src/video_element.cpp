#include <client/video_element.h>

namespace parties::client {

// ── VideoElement ────────────────────────────────────────────────────

VideoElement::VideoElement(const Rml::String& tag)
    : Rml::Element(tag) {}

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
    render_x_ = render_y_ = render_w_ = render_h_ = 0;
    position_dirty_ = true;
    DirtyLayout();
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

void VideoElement::OnRender() {
    if (!has_frame_ || frame_width_ == 0 || frame_height_ == 0) return;

    // Compute aspect-ratio-preserving rect within element content box
    Rml::Vector2f offset = GetAbsoluteOffset(Rml::BoxArea::Content);
    Rml::Vector2f size   = GetBox().GetSize(Rml::BoxArea::Content);

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

    render_x_ = offset.x + ox;
    render_y_ = offset.y + oy;
    render_w_ = rw;
    render_h_ = rh;
    position_dirty_ = true;
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
