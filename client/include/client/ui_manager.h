#pragma once

#include <client/rmlui_backend.h>

#include <RmlUi/Core.h>

#include <memory>
#include <string>
#include <functional>
#include <unordered_map>

// Forward declarations — avoid pulling windows.h into header
typedef struct HWND__* HWND;
class RenderInterface_DX12;
class SystemInterface_Win32;
class TextInputMethodEditor_Win32;

// DirectComposition + DX12 video surface forward declarations
struct IDCompositionDesktopDevice;
struct IDCompositionTarget;
struct IDCompositionVisual2;
struct IDXGISwapChain3;
struct ID3D12CommandAllocator;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct ID3D12Fence;

namespace parties::client {

class UiManager {
public:
    UiManager();
    ~UiManager();

    bool init(HWND hwnd);
    void shutdown();

    Rml::ElementDocument* load_document(const std::string& path);
    void show_document(Rml::ElementDocument* doc);
    void hide_document(Rml::ElementDocument* doc);
    void unload_all();

    void update();
    void render();

    void on_resize(int width, int height);
    void on_dpi_change(float scale);

    Rml::Context* context() { return context_; }

    using EventCallback = std::function<void(Rml::Event&)>;
    void bind_event(const std::string& element_id, const std::string& event_type,
                    EventCallback callback);

    // Window controls
    void minimize_window();
    void toggle_maximize();
    void close_window();
    bool is_maximized() const;

    // Video surface (DirectComposition overlay)
    void create_video_surface(uint32_t width, uint32_t height);
    void destroy_video_surface();
    void present_video_frame(const uint8_t* rgba_data, uint32_t width, uint32_t height, uint32_t stride);
    void update_video_position(float x, float y, float w, float h);
    bool has_video_surface() const { return video_swapchain_ != nullptr; }

    // Accessors for WndProc in main.cpp
    TextInputMethodEditor_Win32& text_input_editor();
    RenderInterface_DX12* dx12_renderer() { return render_interface_.get(); }
    float dpi_scale() const { return dpi_scale_; }

private:
    std::unique_ptr<RenderInterface_DX12> render_interface_;
    std::unique_ptr<SystemInterface_Win32> system_interface_;
    std::unique_ptr<TextInputMethodEditor_Win32> text_input_editor_;
    EmbeddedFileInterface file_interface_;

    Rml::Context* context_ = nullptr;
    HWND hwnd_ = nullptr;
    float dpi_scale_ = 1.0f;
    bool initialised_ = false;

    class EventListenerInstancer;
    class GenericEventListener;

    // DirectComposition
    IDCompositionDesktopDevice* dcomp_device_  = nullptr;
    IDCompositionTarget*        dcomp_target_  = nullptr;
    IDCompositionVisual2*       root_visual_   = nullptr;
    IDCompositionVisual2*       ui_visual_     = nullptr;
    IDCompositionVisual2*       video_visual_  = nullptr;

    // Video surface (DComp overlay)
    IDXGISwapChain3*            video_swapchain_  = nullptr;
    ID3D12CommandAllocator*     video_cmd_alloc_  = nullptr;
    ID3D12GraphicsCommandList*  video_cmd_list_   = nullptr;
    ID3D12Resource*             video_staging_    = nullptr;
    ID3D12Fence*                video_fence_      = nullptr;
    void*                       video_fence_event_ = nullptr;
    uint64_t                    video_fence_val_  = 0;
    uint32_t                    video_sc_width_   = 0;
    uint32_t                    video_sc_height_  = 0;
    uint32_t                    video_staging_row_pitch_ = 0;
};

} // namespace parties::client
