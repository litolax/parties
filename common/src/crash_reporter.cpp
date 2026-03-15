#include <parties/crash_reporter.h>
#include <parties/version.h>

#ifdef SENTRY_ENABLED
#include <sentry.h>
#endif

namespace parties {

void crash_reporter_init(const char* dsn) {
#ifdef SENTRY_ENABLED
    if (!dsn || dsn[0] == '\0') return;
    sentry_options_t* options = sentry_options_new();
    sentry_options_set_dsn(options, dsn);
    sentry_options_set_release(options, APP_VERSION);
#ifdef PARTIES_RETAIL
    sentry_options_set_environment(options, "production");
#else
    sentry_options_set_environment(options, "development");
#endif
    sentry_options_set_database_path(options, ".sentry-native");
    sentry_init(options);
#else
    (void)dsn;
#endif
}

void crash_reporter_set_user(const std::string& user_id, const std::string& display_name) {
#ifdef SENTRY_ENABLED
    sentry_value_t user = sentry_value_new_object();
    sentry_value_set_by_key(user, "id", sentry_value_new_string(user_id.c_str()));
    sentry_value_set_by_key(user, "username", sentry_value_new_string(display_name.c_str()));
    sentry_set_user(user);
#else
    (void)user_id;
    (void)display_name;
#endif
}

void crash_reporter_shutdown() {
#ifdef SENTRY_ENABLED
    sentry_close();
#endif
}

} // namespace parties
