#include "amf_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

namespace parties::client::amd {

bool load_amf(amf::AMFFactory*& factory) {
    static bool tried = false;
    static bool ok = false;
    static amf::AMFFactory* cached_factory = nullptr;

    if (tried) {
        factory = cached_factory;
        return ok;
    }
    tried = true;

    HMODULE dll = LoadLibraryA(AMF_DLL_NAMEA);
    if (!dll) return false;

    auto init_fn = reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll, AMF_INIT_FUNCTION_NAME));
    if (!init_fn) return false;

    AMF_RESULT res = init_fn(AMF_FULL_VERSION, &cached_factory);
    if (res != AMF_OK || !cached_factory) {
        std::fprintf(stderr, "[AMF] AMFInit failed: %d\n", res);
        return false;
    }

    factory = cached_factory;
    ok = true;
    return true;
}

} // namespace parties::client::amd
