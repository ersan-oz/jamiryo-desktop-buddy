#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace buddy_memory {
constexpr unsigned capacity = 20;
constexpr size_t key_size = 24, value_size = 192;
struct Note { char key[key_size]{}; char value[value_size]{}; };
enum class Change { Invalid, Full, Unchanged, Saved };

inline bool valid_key(const char *key)
{
    if (!key || !*key || std::strlen(key) >= key_size) return false;
    for (const char *p = key; *p; ++p)
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') && *p != '_') return false;
    return true;
}
inline bool valid_value(const char *value)
{
    if (!value || !*value || std::strlen(value) >= value_size) return false;
    bool non_space = false;
    const auto *p = reinterpret_cast<const unsigned char *>(value);
    while (*p) {
        uint32_t cp = *p++; unsigned remaining = 0; uint32_t minimum = 0;
        if (cp < 0x80) {
            if (cp < 0x20 || cp == 0x7f) return false;
            non_space |= cp != ' '; continue;
        }
        if (cp >= 0xc2 && cp <= 0xdf) { remaining = 1; cp &= 31; minimum = 0x80; }
        else if (cp >= 0xe0 && cp <= 0xef) { remaining = 2; cp &= 15; minimum = 0x800; }
        else if (cp >= 0xf0 && cp <= 0xf4) { remaining = 3; cp &= 7; minimum = 0x10000; }
        else return false;
        while (remaining--) {
            if ((*p & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (*p++ & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        non_space = true;
    }
    return non_space;
}
struct Notes {
    unsigned count = 0;
    Note entries[capacity]{};
    int find(const char *key) const
    {
        if (!valid_key(key)) return -1;
        for (unsigned i = 0; i < count; ++i) if (std::strcmp(key, entries[i].key) == 0) return i;
        return -1;
    }
    Change remember(const char *key, const char *value)
    {
        if (!valid_key(key) || !valid_value(value)) return Change::Invalid;
        int index = find(key);
        if (index >= 0 && std::strcmp(value, entries[index].value) == 0) return Change::Unchanged;
        if (index < 0) {
            if (count >= capacity) return Change::Full;
            index = count++;
        }
        entries[index] = {};
        std::strcpy(entries[index].key, key); std::strcpy(entries[index].value, value);
        return Change::Saved;
    }
    bool forget(const char *key)
    {
        const int index = find(key);
        if (index < 0) return false;
        for (unsigned i = index; i + 1 < count; ++i) entries[i] = entries[i + 1];
        entries[--count] = {};
        return true;
    }
    void clear() { *this = Notes{}; }
};
} // namespace buddy_memory
