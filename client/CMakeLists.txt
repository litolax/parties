find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(rnnoise CONFIG REQUIRED)
find_package(RmlUi CONFIG REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h")


find_path(SPEEXDSP_INCLUDE_DIRS "speex/speex_echo.h")
find_library(SPEEXDSP_LIBRARY speexdsp)

if(NOT APPLE)
    find_package(D3D12MemoryAllocator CONFIG REQUIRED)

    # Generate .rc with version info and icon
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/logo.ico" ICON_PATH)
    set(RC_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
    set(RC_VERSION_MINOR ${PROJECT_VERSION_MINOR})
    set(RC_VERSION_PATCH ${PROJECT_VERSION_PATCH})
    set(RC_VERSION_STR "${PROJECT_VERSION}")
    configure_file(res/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
    configure_file(res/app.manifest "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" COPYONLY)
endif()

# ── Cross-platform sources ────────────────────────────────────────────────────
# Windows-only files (HWND / WRL / Win32 / DX renderers):
#   app.cpp, context_menu.cpp, ui_manager.cpp
# Those live in CLIENT_PLATFORM_SOURCES on Windows.
# On macOS app_macos.mm replaces app.cpp + ui_manager.cpp.
set(CLIENT_COMMON_SOURCES
    src/lobby_model.cpp
    src/server_list_model.cpp
    src/settings.cpp
    src/audio_engine.cpp
    src/voice_mixer.cpp
    src/miniaudio_impl.cpp
    src/RmlUI_Backend.cpp
    src/video_element.cpp
    src/level_meter_element.cpp
    src/gradient_circle_element.cpp
    src/stream_audio_player.cpp
    src/app_core.cpp
    src/sound_player.cpp
    src/net_client_msquic.cpp
)

# ── Platform-specific sources ─────────────────────────────────────────────────
if(APPLE)
    set(CLIENT_PLATFORM_SOURCES
        # Entry point & window (AppKit / Metal)
        src/macos/main.mm
        src/macos/app_macos.mm

        # RmlUI Metal backend (shared with client-ios)
        src/metal/RmlUi_Renderer_Metal.mm
        src/metal/RmlUi_Backend_macOS_Metal.mm
        src/macos/RmlUi_Platform_macOS.mm
    
        # Screen capture — ScreenCaptureKit (macOS 12.3+)
        src/macos/screen_capture_macos.mm

        # Sound player — pure miniaudio, same as Windows
    )
    # miniaudio CoreAudio backend requires Objective-C++
    set_source_files_properties(
        src/audio_engine.cpp
        src/miniaudio_impl.cpp
        PROPERTIES LANGUAGE OBJCXX
    )
else()
    set(CLIENT_PLATFORM_SOURCES
        # Platform specific
        src/windows/main.cpp
        src/windows/app.cpp
        src/windows/stream_audio_capture.cpp
        src/windows/RmlUi_Platform_Win32.cpp
        src/windows/ui_manager.cpp
        src/windows/context_menu.cpp

        # RmlUI rendering backends
        src/dx11/RmlUi_Renderer_DX11.cpp
        src/dx12/RmlUi_Renderer_DX12.cpp
        src/dx12/RmlUi_Renderer_DX12WL.cpp
        src/dx12/offsetAllocator.cpp
        src/vulkan/RmlUi_Renderer_VK.cpp

        # Screen capture — DXGI
        src/windows/screen_capture.cpp

        # Video — Media Foundation / hardware encoders
        src/windows/video_encoder.cpp
        src/windows/video_decoder.cpp

        # Windows resource file
        "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
    )
endif()

add_executable(parties_client
    ${CLIENT_COMMON_SOURCES}
    ${CLIENT_PLATFORM_SOURCES}
)

# Disable CMake's auto-generated manifest — we embed our own via app.rc
if(NOT APPLE)
    set_target_properties(parties_client PROPERTIES LINK_FLAGS "/MANIFEST:NO")
endif()

# #embed dependencies — recompile when embedded files change
file(GLOB EMBED_UI_RESOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/*.rml
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/*.rcss
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/*.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/fonts/*.ttf
)
file(GLOB EMBED_SOUND_RESOURCES
    ${CMAKE_SOURCE_DIR}/sounds/*.wav
)
set_source_files_properties(src/rmlui_backend.cpp PROPERTIES
    OBJECT_DEPENDS "${EMBED_UI_RESOURCES}"
)
set_source_files_properties(src/sound_player.cpp PROPERTIES
    OBJECT_DEPENDS "${EMBED_SOUND_RESOURCES}"
)

target_include_directories(parties_client
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        $<$<BOOL:${APPLE}>:${CMAKE_CURRENT_SOURCE_DIR}/src/macos>
        ${MINIAUDIO_INCLUDE_DIRS}
        $<$<NOT:$<BOOL:${APPLE}>>:${CMAKE_CURRENT_SOURCE_DIR}/src/dx12>
        $<$<NOT:$<BOOL:${APPLE}>>:${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan>
        ${SPEEXDSP_INCLUDE_DIRS}
        $<$<BOOL:${APPLE}>:${CMAKE_CURRENT_SOURCE_DIR}/../client-ios/Backends>
)

# Retail: no console window on Windows
if(NOT APPLE AND BUILD_IN_RETAIL)
    target_link_options(parties_client PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

if(APPLE)
    set_target_properties(parties_client PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/res/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Parties Client"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.parties.client"
        MACOSX_BUNDLE_COPYRIGHT "MIT License"
    )
endif()

if(APPLE)
    target_link_libraries(parties_client
        PRIVATE
            parties_common
            parties_encdec
            rnnoise::rnnoise
            unofficial::sqlite3::sqlite3
            RmlUi::RmlUi
            $<$<NOT:$<BOOL:${BUILD_IN_RETAIL}>>:RmlUi::Debugger>
            ${SPEEXDSP_LIBRARY}
            "-framework Metal"
            "-framework MetalKit"
            "-framework AppKit"
            "-framework Foundation"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework AVFoundation"
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework ScreenCaptureKit"
            "-framework Security"
    )
else()
    target_link_libraries(parties_client
        PRIVATE
            parties_common
            parties_encdec
            rnnoise::rnnoise
            unofficial::sqlite3::sqlite3
            RmlUi::RmlUi
            $<$<NOT:$<BOOL:${BUILD_IN_RETAIL}>>:RmlUi::Debugger>
            GPUOpen::D3D12MemoryAllocator
            ${SPEEXDSP_LIBRARY}
            d3d11 d3d12 dxgi d3dcompiler
            windowsapp
            dwmapi
    )
endif()

if(ENABLE_SENTRY AND NOT SENTRY_DSN_CLIENT STREQUAL "")
    target_compile_definitions(parties_client PRIVATE "SENTRY_DSN_VALUE=\"${SENTRY_DSN_CLIENT}\"")
endif()

# Copy client exe to additional test folders for multi-instance testing (Windows only)
if(NOT APPLE)
    foreach(COPY_DIR client2 client3 client4)
        add_custom_command(TARGET parties_client POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/${COPY_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:parties_client>"
                "${CMAKE_BINARY_DIR}/${COPY_DIR}/parties_client.exe"
        )
    endforeach()
endif()

