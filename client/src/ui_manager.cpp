#include <client/ui_manager.h>

#include "RmlUi_Renderer_DX12.h"
#include "RmlUi_Platform_Win32.h"

#include <RmlUi/Debugger.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <dcomp.h>

#include <cstdio>
#include <cstring>

namespace parties::client {

// ═══════════════════════════════════════════════════════════════════════
// Event listener helper
// ═══════════════════════════════════════════════════════════════════════

class UiManager::GenericEventListener : public Rml::EventListener {
public:
    GenericEventListener(EventCallback cb) : callback_(std::move(cb)) {}
    void ProcessEvent(Rml::Event& event) override { callback_(event); }

private:
    EventCallback callback_;
};

// ═══════════════════════════════════════════════════════════════════════
// UiManager
// ═══════════════════════════════════════════════════════════════════════

UiManager::UiManager() = default;
UiManager::~UiManager() { shutdown(); }

bool UiManager::init(HWND hwnd) {
    hwnd_ = hwnd;

    // Create system interface (from vendored Win32 platform)
    system_interface_ = std::make_unique<SystemInterface_Win32>();
    system_interface_->SetWindow(hwnd);

    Rml::SetSystemInterface(system_interface_.get());
    Rml::SetFileInterface(&file_interface_);

    // Create DX12 render interface
    Backend::RmlRendererSettings settings{};
    settings.vsync = true;
    settings.msaa_sample_count = 1;

    render_interface_ = std::make_unique<RenderInterface_DX12>(
        static_cast<void*>(hwnd), settings);
    if (!*render_interface_) {
        std::fprintf(stderr, "[UI] Failed to create DX12 render interface\n");
        return false;
    }

    Rml::SetRenderInterface(render_interface_.get());

    // Set up DirectComposition visual tree.
    // The main swapchain (CreateSwapChainForComposition) must be bound to a DComp
    // visual for anything to appear on screen.
    {
        HRESULT hr = DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&dcomp_device_));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[UI] Failed to create DComp device (0x%08lX)\n", hr);
            return false;
        }

        hr = dcomp_device_->CreateTargetForHwnd(hwnd, TRUE, &dcomp_target_);
        if (FAILED(hr)) {
            std::fprintf(stderr, "[UI] Failed to create DComp target (0x%08lX)\n", hr);
            return false;
        }

        dcomp_device_->CreateVisual(&root_visual_);
        dcomp_device_->CreateVisual(&ui_visual_);
        ui_visual_->SetContent(render_interface_->Get_SwapChain());
        root_visual_->AddVisual(ui_visual_, TRUE, nullptr);
        dcomp_target_->SetRoot(root_visual_);
        dcomp_device_->Commit();
    }

    if (!Rml::Initialise()) {
        std::fprintf(stderr, "[UI] Failed to initialise RmlUi\n");
        return false;
    }

    // Register custom property for Win32 window hit-testing.
    // Inherited so child elements of .titlebar get 'caption' automatically.
    Rml::StyleSheetSpecification::RegisterProperty("window-action", "none", true)
        .AddParser("keyword", "none, caption, close, minimize, maximize");

    // Get window dimensions
    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    render_interface_->SetViewport(width, height);

    // Create context
    context_ = Rml::CreateContext("main", Rml::Vector2i(width, height));
    if (!context_) {
        std::fprintf(stderr, "[UI] Failed to create RmlUi context\n");
        Rml::Shutdown();
        return false;
    }

    Rml::Debugger::Initialise(context_);

    // DPI scaling
    UINT dpi = GetDpiForWindow(hwnd);
    dpi_scale_ = static_cast<float>(dpi) / 96.0f;
    context_->SetDensityIndependentPixelRatio(dpi_scale_);

    // Load fonts
    if (!Rml::LoadFontFace("ui/fonts/NotoSans-Regular.ttf"))
        Rml::LoadFontFace("C:/Windows/Fonts/segoeui.ttf");
    Rml::LoadFontFace("ui/fonts/NotoSans-Bold.ttf", true);

    // Text input method editor for IME support
    text_input_editor_ = std::make_unique<TextInputMethodEditor_Win32>();

    initialised_ = true;
    std::printf("[UI] Initialised DX12 (%dx%d, DPI scale=%.2f)\n", width, height, dpi_scale_);
    return true;
}

void UiManager::shutdown() {
    if (!initialised_) return;

    destroy_video_surface();

    text_input_editor_.reset();
    if (context_) {
        Rml::RemoveContext("main");
        context_ = nullptr;
    }
    Rml::Shutdown();

    // Release DComp objects before the renderer (which owns the swapchain)
    if (ui_visual_)    { ui_visual_->Release();    ui_visual_    = nullptr; }
    if (root_visual_)  { root_visual_->Release();  root_visual_  = nullptr; }
    if (dcomp_target_) { dcomp_target_->Release(); dcomp_target_ = nullptr; }
    if (dcomp_device_) { dcomp_device_->Release(); dcomp_device_ = nullptr; }

    render_interface_.reset();
    system_interface_.reset();
    initialised_ = false;
}

Rml::ElementDocument* UiManager::load_document(const std::string& path) {
    if (!context_) return nullptr;
    auto* doc = context_->LoadDocument(path);
    if (!doc)
        std::fprintf(stderr, "[UI] Failed to load document: %s\n", path.c_str());
    return doc;
}

void UiManager::show_document(Rml::ElementDocument* doc) {
    if (doc) doc->Show();
}

void UiManager::hide_document(Rml::ElementDocument* doc) {
    if (doc) doc->Hide();
}

void UiManager::unload_all() {
    if (context_) context_->UnloadAllDocuments();
}

void UiManager::update() {
    if (context_) context_->Update();
}

void UiManager::render() {
    if (!render_interface_ || !*render_interface_) return;

    render_interface_->BeginFrame();
    render_interface_->Clear();
    if (context_) context_->Render();
    render_interface_->EndFrame();
}

void UiManager::on_resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (render_interface_)
        render_interface_->SetViewport(width, height);
    if (context_)
        context_->SetDimensions(Rml::Vector2i(width, height));
    if (dcomp_device_)
        dcomp_device_->Commit();
}

void UiManager::on_dpi_change(float scale) {
    dpi_scale_ = scale;
    if (context_)
        context_->SetDensityIndependentPixelRatio(scale);
}

void UiManager::bind_event(const std::string& element_id, const std::string& event_type,
                            EventCallback callback) {
    if (!context_) return;

    for (int i = 0; i < context_->GetNumDocuments(); i++) {
        auto* doc = context_->GetDocument(i);
        auto* element = doc->GetElementById(element_id);
        if (element) {
            auto* listener = new GenericEventListener(std::move(callback));
            element->AddEventListener(event_type, listener);
            return;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Video surface (DirectComposition overlay)
// ═══════════════════════════════════════════════════════════════════════

void UiManager::create_video_surface(uint32_t width, uint32_t height) {
    if (video_swapchain_) destroy_video_surface();
    if (!render_interface_ || !dcomp_device_) return;

    auto* device = render_interface_->Get_Device();
    auto* queue  = render_interface_->Get_CommandQueue();

    // Create DXGI factory for the video swapchain
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[UI] Failed to create DXGI factory for video (0x%08lX)\n", hr);
        return;
    }

    // Create BGRA composition swapchain for video
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width       = width;
    desc.Height      = height;
    desc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc  = {1, 0};
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    IDXGISwapChain1* sc1 = nullptr;
    hr = factory->CreateSwapChainForComposition(queue, &desc, nullptr, &sc1);
    factory->Release();
    if (FAILED(hr)) {
        std::fprintf(stderr, "[UI] Failed to create video swapchain (0x%08lX)\n", hr);
        return;
    }

    sc1->QueryInterface(IID_PPV_ARGS(&video_swapchain_));
    sc1->Release();
    video_sc_width_  = width;
    video_sc_height_ = height;

    // Command allocator + list for video uploads
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   IID_PPV_ARGS(&video_cmd_alloc_));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                              video_cmd_alloc_, nullptr,
                              IID_PPV_ARGS(&video_cmd_list_));
    video_cmd_list_->Close();

    // Fence for synchronizing video uploads
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&video_fence_));
    video_fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    video_fence_val_ = 0;

    // Staging buffer (upload heap) sized to match the back buffer layout
    ID3D12Resource* back_buffer = nullptr;
    video_swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    D3D12_RESOURCE_DESC bb_desc = back_buffer->GetDesc();
    back_buffer->Release();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 total_bytes;
    device->GetCopyableFootprints(&bb_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);
    video_staging_row_pitch_ = footprint.Footprint.RowPitch;

    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC buf_desc = {};
    buf_desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width            = total_bytes;
    buf_desc.Height           = 1;
    buf_desc.DepthOrArraySize = 1;
    buf_desc.MipLevels        = 1;
    buf_desc.SampleDesc.Count = 1;
    buf_desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE,
                                    &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                    nullptr, IID_PPV_ARGS(&video_staging_));

    // Create DComp visual for video (above the UI visual = in front)
    dcomp_device_->CreateVisual(&video_visual_);
    video_visual_->SetContent(video_swapchain_);
    root_visual_->AddVisual(video_visual_, TRUE, ui_visual_);
    dcomp_device_->Commit();

    std::printf("[UI] Created video surface %ux%u\n", width, height);
}

void UiManager::destroy_video_surface() {
    if (!video_swapchain_) return;

    // Wait for all GPU work on this path to complete
    if (video_fence_ && render_interface_) {
        auto* queue = render_interface_->Get_CommandQueue();
        ++video_fence_val_;
        queue->Signal(video_fence_, video_fence_val_);
        if (video_fence_->GetCompletedValue() < video_fence_val_) {
            video_fence_->SetEventOnCompletion(video_fence_val_, video_fence_event_);
            WaitForSingleObject(static_cast<HANDLE>(video_fence_event_), INFINITE);
        }
    }

    // Remove visual from tree
    if (video_visual_ && root_visual_) {
        root_visual_->RemoveVisual(video_visual_);
        if (dcomp_device_) dcomp_device_->Commit();
    }

    if (video_visual_)      { video_visual_->Release();      video_visual_      = nullptr; }
    if (video_staging_)     { video_staging_->Release();     video_staging_     = nullptr; }
    if (video_cmd_list_)    { video_cmd_list_->Release();    video_cmd_list_    = nullptr; }
    if (video_cmd_alloc_)   { video_cmd_alloc_->Release();   video_cmd_alloc_   = nullptr; }
    if (video_fence_event_) { CloseHandle(static_cast<HANDLE>(video_fence_event_)); video_fence_event_ = nullptr; }
    if (video_fence_)       { video_fence_->Release();       video_fence_       = nullptr; }
    if (video_swapchain_)   { video_swapchain_->Release();   video_swapchain_   = nullptr; }

    video_sc_width_  = 0;
    video_sc_height_ = 0;
    video_fence_val_ = 0;
    video_staging_row_pitch_ = 0;
}

void UiManager::present_video_frame(const uint8_t* bgra_data,
                                     uint32_t width, uint32_t height,
                                     uint32_t stride) {
    if (!video_swapchain_ || !video_cmd_alloc_ || !render_interface_) return;
    if (width != video_sc_width_ || height != video_sc_height_) return;

    auto* queue = render_interface_->Get_CommandQueue();

    // Wait for previous video frame's GPU work
    if (video_fence_->GetCompletedValue() < video_fence_val_) {
        video_fence_->SetEventOnCompletion(video_fence_val_, video_fence_event_);
        WaitForSingleObject(static_cast<HANDLE>(video_fence_event_), INFINITE);
    }

    video_cmd_alloc_->Reset();
    video_cmd_list_->Reset(video_cmd_alloc_, nullptr);

    // Get current back buffer
    UINT bb_index = video_swapchain_->GetCurrentBackBufferIndex();
    ID3D12Resource* back_buffer = nullptr;
    video_swapchain_->GetBuffer(bb_index, IID_PPV_ARGS(&back_buffer));

    // Copy BGRA pixel data into staging buffer (respecting row pitch alignment)
    uint8_t* mapped = nullptr;
    video_staging_->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (uint32_t y = 0; y < height; y++) {
        std::memcpy(mapped + y * video_staging_row_pitch_,
                    bgra_data + y * stride,
                    width * 4);
    }
    video_staging_->Unmap(0, nullptr);

    // Transition back buffer: PRESENT → COPY_DEST
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource    = back_buffer;
    barrier.Transition.StateBefore  = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter   = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource  = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    video_cmd_list_->ResourceBarrier(1, &barrier);

    // Copy from staging buffer to back buffer texture
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = video_staging_;
    src.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset                = 0;
    src.PlacedFootprint.Footprint.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width       = width;
    src.PlacedFootprint.Footprint.Height      = height;
    src.PlacedFootprint.Footprint.Depth       = 1;
    src.PlacedFootprint.Footprint.RowPitch    = video_staging_row_pitch_;

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource        = back_buffer;
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    video_cmd_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Transition back buffer: COPY_DEST → PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    video_cmd_list_->ResourceBarrier(1, &barrier);

    video_cmd_list_->Close();
    ID3D12CommandList* lists[] = { video_cmd_list_ };
    queue->ExecuteCommandLists(1, lists);

    // Signal fence and present
    ++video_fence_val_;
    queue->Signal(video_fence_, video_fence_val_);
    video_swapchain_->Present(1, 0);

    back_buffer->Release();
}

void UiManager::update_video_position(float x, float y, float w, float h) {
    if (!video_visual_ || !dcomp_device_) return;

    video_visual_->SetOffsetX(x);
    video_visual_->SetOffsetY(y);

    // Scale the video swapchain content to match the element display size
    if (video_sc_width_ > 0 && video_sc_height_ > 0) {
        float sx = w / static_cast<float>(video_sc_width_);
        float sy = h / static_cast<float>(video_sc_height_);
        D2D_MATRIX_3X2_F matrix = {};
        matrix._11 = sx;
        matrix._22 = sy;
        video_visual_->SetTransform(matrix);
    }

    dcomp_device_->Commit();
}

// ═══════════════════════════════════════════════════════════════════════
// Window controls
// ═══════════════════════════════════════════════════════════════════════

void UiManager::minimize_window() {
    if (hwnd_) ShowWindow(hwnd_, SW_MINIMIZE);
}

void UiManager::toggle_maximize() {
    if (!hwnd_) return;
    if (IsZoomed(hwnd_))
        ShowWindow(hwnd_, SW_RESTORE);
    else
        ShowWindow(hwnd_, SW_MAXIMIZE);
}

void UiManager::close_window() {
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

bool UiManager::is_maximized() const {
    return hwnd_ && IsZoomed(hwnd_);
}

TextInputMethodEditor_Win32& UiManager::text_input_editor() {
    return *text_input_editor_;
}

} // namespace parties::client
