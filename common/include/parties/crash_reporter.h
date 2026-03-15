#pragma once

#include <string>

namespace parties {

// Initialize Sentry crash reporting.
// Does nothing if Sentry is not linked or dsn is null/empty.
// Call once at startup, before any other initialization.
void crash_reporter_init(const char* dsn);

// Set user identity after authentication succeeds.
void crash_reporter_set_user(const std::string& user_id, const std::string& display_name);

// Flush events and shut down. Call at shutdown.
void crash_reporter_shutdown();

} // namespace parties
