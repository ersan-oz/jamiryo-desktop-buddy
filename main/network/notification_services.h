#pragma once
#include <cstddef>
#include <cstring>

namespace buddy_notify {
constexpr unsigned capacity = 16;
constexpr size_t name_size = 96;
inline bool valid_target(const char *name)
{
    if (!name || std::strncmp(name, "mobile_app_", 11) || !name[11] || std::strlen(name) >= name_size) return false;
    for (const char *p = name; *p; ++p) if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') && *p != '_') return false;
    return true;
}
struct Targets { unsigned count = 0; char names[capacity][name_size]{}; };

// Select only notify.mobile_app_* keys while streaming /api/services.
// Service descriptions can be much larger than the ESP32 internal heap.
class Scanner {
    unsigned depth = 0, services_depth = 0;
    bool quoted = false, escaped = false, token_escaped = false, key = false;
    bool domain_notify = false, domain_value = false, services_value = false;
    bool root_seen = false, closed = false, bad = false;
    char previous = 0, token[128]{};
    size_t used = 0, received = 0;
    Targets candidates{};
public:
    Targets result{};
    void feed(const char *data, size_t size)
    {
        received += size;
        if (received > 1024 * 1024) { bad = true; return; }
        for (size_t i = 0; i < size && !bad; ++i) {
            const char c = data[i];
            if (quoted) {
                if (escaped) { escaped = false; token_escaped = true; }
                else if (c == '\\') { escaped = true; continue; }
                else if (c == '"') {
                    quoted = false; token[used] = 0;
                    if (depth == 2 && key) {
                        domain_value = !token_escaped && !std::strcmp(token, "domain");
                        services_value = !token_escaped && !std::strcmp(token, "services");
                    } else if (depth == 2 && domain_value) {
                        domain_notify = !token_escaped && !std::strcmp(token, "notify"); domain_value = false;
                    } else if (depth == services_depth && services_depth == 3 && key && !token_escaped && valid_target(token)) {
                        if (candidates.count == capacity) { bad = true; continue; }
                        std::strcpy(candidates.names[candidates.count++], token);
                    }
                    previous = '"'; continue;
                }
                if (used + 1 < sizeof(token)) token[used++] = c;
                else token_escaped = true; // Oversized description strings are ignored.
                continue;
            }
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
            if (closed) { bad = true; continue; }
            if (c == '"') {
                quoted = true; escaped = token_escaped = false; used = 0;
                key = previous == '{' || previous == ','; continue;
            }
            if (c == '{' || c == '[') {
                if (!depth) { if (root_seen || c != '[') { bad = true; continue; } root_seen = true; }
                if (depth == 1 && c == '{') { domain_notify = domain_value = services_value = false; candidates.count = 0; }
                if (depth == 2 && services_value && c == '{') { services_depth = 3; services_value = false; }
                if (++depth > 40) bad = true;
            } else if (c == '}' || c == ']') {
                if (!depth) { bad = true; continue; }
                if (depth == 3 && services_depth == 3) services_depth = 0;
                if (depth == 2 && c == '}' && domain_notify) {
                    if (result.count + candidates.count > capacity) { bad = true; continue; }
                    for (unsigned j = 0; j < candidates.count; ++j) std::strcpy(result.names[result.count++], candidates.names[j]);
                }
                if (!--depth) closed = c == ']';
            }
            previous = c;
        }
    }
    bool complete() const { return !bad && root_seen && closed && !depth && !quoted; }
};
}
