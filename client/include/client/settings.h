#pragma once

#include <string>
#include <vector>
#include <optional>

struct sqlite3;

namespace parties::client {

struct SavedServer {
    int         id = 0;
    std::string name;
    std::string host;
    int         port = 7800;
    std::string fingerprint;   // TOFU cert fingerprint
    std::string last_username;
    std::string last_password;
};

class Settings {
public:
    Settings();
    ~Settings();

    bool open(const std::string& path);
    void close();

    // --- TOFU certificate store ---
    // Returns true if the fingerprint is known and matches
    // Returns false if unknown (new server) or mismatched
    enum class TofuResult { Trusted, Unknown, Mismatch };
    TofuResult check_fingerprint(const std::string& host, int port,
                                  const std::string& fingerprint);
    bool trust_fingerprint(const std::string& host, int port,
                           const std::string& fingerprint);

    // --- Saved servers ---
    bool save_server(const std::string& name, const std::string& host, int port,
                     const std::string& fingerprint, const std::string& last_username,
                     const std::string& last_password);
    std::vector<SavedServer> get_saved_servers();
    bool delete_server(int id);

    // --- Preferences ---
    bool set_pref(const std::string& key, const std::string& value);
    std::optional<std::string> get_pref(const std::string& key);

private:
    bool exec(const std::string& sql);
    bool create_schema();

    sqlite3* db_ = nullptr;
};

} // namespace parties::client
