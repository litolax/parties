#include <parties/net_common.h>

#include <enet.h>

namespace parties {

bool net_init() {
    return enet_initialize() == 0;
}

void net_cleanup() {
    enet_deinitialize();
}

} // namespace parties
