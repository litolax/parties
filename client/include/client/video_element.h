#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>

#include <cstdint>

namespace parties::client {

// Positioning proxy for video playback via DirectComposition.
// Reserves layout space in the RmlUi document and reports its screen
// coordinates each frame so the DComp video visual can be positioned to match.
class VideoElement : public Rml::Element {
public:
    explicit VideoElement(const Rml::String& tag);
    ~VideoElement() override = default;

    // Set the video dimensions (for layout). No pixel data needed —
    // the actual frames go through UiManager::present_video_frame().
    void SetVideoDimensions(uint32_t width, uint32_t height);
    void Clear();

    uint32_t frame_width() const { return frame_width_; }
    uint32_t frame_height() const { return frame_height_; }

    // Computed display rect (after aspect-ratio fit within element box)
    float render_x() const { return render_x_; }
    float render_y() const { return render_y_; }
    float render_w() const { return render_w_; }
    float render_h() const { return render_h_; }
    bool position_dirty() const { return position_dirty_; }
    void clear_position_dirty() { position_dirty_ = false; }

protected:
    bool GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio) override;
    void OnRender() override;

private:
    uint32_t frame_width_ = 0;
    uint32_t frame_height_ = 0;
    bool has_frame_ = false;

    // Computed position for DComp overlay
    float render_x_ = 0;
    float render_y_ = 0;
    float render_w_ = 0;
    float render_h_ = 0;
    bool position_dirty_ = false;
};

class VideoElementInstancer : public Rml::ElementInstancer {
public:
    VideoElementInstancer() = default;
    Rml::ElementPtr InstanceElement(Rml::Element* parent, const Rml::String& tag,
                                     const Rml::XMLAttributes& attributes) override;
    void ReleaseElement(Rml::Element* element) override;
};

} // namespace parties::client
