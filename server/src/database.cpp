#include <server/database.h>

#include <sqlite3.h>
#include <cstdio>

namespace parties::server {

Database::Database() = default;

Database::~Database() {
    close();
}

bool Database::open(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::fprintf(stderr, "[DB] Failed to open %s: %s\n",
                     path.c_str(), sqlite3_errmsg(db_));
        return false;
    }

    // Enable WAL and foreign keys
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");

    if (!create_schema()) {
        std::fprintf(stderr, "[DB] Failed to create schema\n");
        close();
        return false;
    }

    std::printf("[DB] Opened %s\n", path.c_str());
    return true;
}

void Database::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::fprintf(stderr, "[DB] SQL error: %s\n", err);
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool Database::create_schema() {
    const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS users (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
            password_hash TEXT NOT NULL,
            role          INTEGER NOT NULL DEFAULT 3,
            created_at    TEXT NOT NULL DEFAULT (datetime('now')),
            last_login    TEXT
        );

        CREATE TABLE IF NOT EXISTS channels (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            name       TEXT NOT NULL UNIQUE,
            max_users  INTEGER NOT NULL DEFAULT 0,
            sort_order INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS channel_permissions (
            channel_id INTEGER NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
            role       INTEGER NOT NULL,
            permission INTEGER NOT NULL,
            PRIMARY KEY (channel_id, role)
        );

        CREATE TABLE IF NOT EXISTS server_meta (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        INSERT OR IGNORE INTO channels (id, name, sort_order) VALUES (1, 'General', 0);
    )SQL";

    return exec(schema);
}

// --- Users ---

bool Database::create_user(const std::string& username, const std::string& password_hash,
                           Role role) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO users (username, password_hash, role) VALUES (?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, static_cast<int>(role));

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<UserRow> Database::get_user_by_name(const std::string& username) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, username, password_hash, role, created_at, last_login "
                      "FROM users WHERE username = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    UserRow row;
    row.id = static_cast<UserId>(sqlite3_column_int(stmt, 0));
    row.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    row.password_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    row.role = sqlite3_column_int(stmt, 3);
    row.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    if (sqlite3_column_type(stmt, 5) != SQLITE_NULL)
        row.last_login = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));

    sqlite3_finalize(stmt);
    return row;
}

std::optional<UserRow> Database::get_user_by_id(UserId id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, username, password_hash, role, created_at, last_login "
                      "FROM users WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_int(stmt, 1, static_cast<int>(id));

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    UserRow row;
    row.id = static_cast<UserId>(sqlite3_column_int(stmt, 0));
    row.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    row.password_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    row.role = sqlite3_column_int(stmt, 3);
    row.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    if (sqlite3_column_type(stmt, 5) != SQLITE_NULL)
        row.last_login = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));

    sqlite3_finalize(stmt);
    return row;
}

bool Database::update_last_login(UserId id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE users SET last_login = datetime('now') WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, static_cast<int>(id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::set_user_role(UserId id, Role role) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE users SET role = ? WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, static_cast<int>(role));
    sqlite3_bind_int(stmt, 2, static_cast<int>(id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<UserRow> Database::get_all_users() {
    std::vector<UserRow> result;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, username, password_hash, role, created_at, last_login "
                      "FROM users ORDER BY id";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        UserRow row;
        row.id = static_cast<UserId>(sqlite3_column_int(stmt, 0));
        row.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        row.password_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        row.role = sqlite3_column_int(stmt, 3);
        row.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        if (sqlite3_column_type(stmt, 5) != SQLITE_NULL)
            row.last_login = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        result.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool Database::delete_user(UserId id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM users WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, static_cast<int>(id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// --- Channels ---

bool Database::create_channel(const std::string& name, int max_users, int sort_order) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO channels (name, max_users, sort_order) VALUES (?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, max_users);
    sqlite3_bind_int(stmt, 3, sort_order);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<ChannelRow> Database::get_channel(ChannelId id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, max_users, sort_order FROM channels WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_int(stmt, 1, static_cast<int>(id));

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    ChannelRow row;
    row.id = static_cast<ChannelId>(sqlite3_column_int(stmt, 0));
    row.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    row.max_users = sqlite3_column_int(stmt, 2);
    row.sort_order = sqlite3_column_int(stmt, 3);

    sqlite3_finalize(stmt);
    return row;
}

std::vector<ChannelRow> Database::get_all_channels() {
    std::vector<ChannelRow> result;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, max_users, sort_order FROM channels ORDER BY sort_order, id";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ChannelRow row;
        row.id = static_cast<ChannelId>(sqlite3_column_int(stmt, 0));
        row.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        row.max_users = sqlite3_column_int(stmt, 2);
        row.sort_order = sqlite3_column_int(stmt, 3);
        result.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool Database::delete_channel(ChannelId id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM channels WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, static_cast<int>(id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// --- Channel permissions ---

bool Database::set_channel_permission(ChannelId channel_id, Role role, uint32_t permissions) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO channel_permissions (channel_id, role, permission) "
                      "VALUES (?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, static_cast<int>(channel_id));
    sqlite3_bind_int(stmt, 2, static_cast<int>(role));
    sqlite3_bind_int(stmt, 3, static_cast<int>(permissions));

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<uint32_t> Database::get_channel_permission(ChannelId channel_id, Role role) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT permission FROM channel_permissions "
                      "WHERE channel_id = ? AND role = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_int(stmt, 1, static_cast<int>(channel_id));
    sqlite3_bind_int(stmt, 2, static_cast<int>(role));

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    uint32_t perm = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
    sqlite3_finalize(stmt);
    return perm;
}

// --- Server metadata ---

bool Database::set_meta(const std::string& key, const std::string& value) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO server_meta (key, value) VALUES (?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<std::string> Database::get_meta(const std::string& key) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT value FROM server_meta WHERE key = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    std::string val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return val;
}

// --- Admin ---

bool Database::has_any_users() {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM users";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    bool has_users = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        has_users = sqlite3_column_int(stmt, 0) > 0;

    sqlite3_finalize(stmt);
    return has_users;
}

} // namespace parties::server
