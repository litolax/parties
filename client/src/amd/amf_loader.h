// Dynamic loader for AMD AMF runtime DLL (amfrt64.dll).
// Loads at runtime — returns false if AMD drivers not installed.
#pragma once

#include <AMF/core/Factory.h>

namespace parties::client::amd {

// Get the AMF factory singleton. Returns false if amfrt64.dll not available.
// Thread-safe, loads only once.
bool load_amf(amf::AMFFactory*& factory);

} // namespace parties::client::amd
