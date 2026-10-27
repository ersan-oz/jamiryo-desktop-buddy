#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "memory/notes.h"

namespace buddy_faces {
constexpr unsigned dimensions = 512, capacity = 5, samples_per_person = 3, name_size = 48;
constexpr float match_threshold = 0.55f, match_margin = 0.08f;
using Feature = float[dimensions];
struct Person { char name[name_size]{}; uint32_t samples = 0; Feature features[samples_per_person]{}; };
struct Database {
    uint32_t magic = 0x4a464331, version = 1, count = 0;
    Person people[capacity]{};
};
inline bool valid_name(const char *name)
{
    if (!buddy_memory::valid_value(name) || std::strlen(name) >= name_size || !std::strcmp(name, "all")) return false;
    for (const auto *p = reinterpret_cast<const unsigned char *>(name); *p; ++p)
        if (*p < 128 && !(*p >= 'A' && *p <= 'Z') && !(*p >= 'a' && *p <= 'z')
            && !(*p >= '0' && *p <= '9') && *p != ' ' && *p != '-' && *p != '\'') return false;
    return true;
}
inline bool valid_feature(const float *feature)
{
    if (!feature) return false;
    float norm = 0;
    for (unsigned i = 0; i < dimensions; ++i) {
        if (!std::isfinite(feature[i])) return false;
        norm += feature[i] * feature[i];
    }
    return std::isfinite(norm) && norm > 0.98f && norm < 1.02f;
}
inline float similarity(const float *a, const float *b)
{
    float dot = 0;
    for (unsigned i = 0; i < dimensions; ++i) dot += a[i] * b[i];
    return std::clamp(dot, -1.0f, 1.0f);
}
inline int find(const Database &db, const char *name)
{
    if (!valid_name(name)) return -1;
    for (unsigned i = 0; i < db.count; ++i) if (!std::strcmp(db.people[i].name, name)) return i;
    return -1;
}
inline bool valid_database(const Database &db)
{
    if (db.magic != 0x4a464331 || db.version != 1 || db.count > capacity) return false;
    for (unsigned i = 0; i < db.count; ++i) {
        const Person &person = db.people[i];
        if (!std::memchr(person.name, 0, name_size) || !valid_name(person.name)
            || person.samples != samples_per_person) return false;
        for (unsigned j = 0; j < i; ++j) if (!std::strcmp(db.people[j].name, person.name)) return false;
        for (const auto &feature : person.features) if (!valid_feature(feature)) return false;
    }
    return true;
}
struct Match { int index = -1; float best = -1, second = -1; };
inline Match match(const Database &db, const float *feature)
{
    Match result;
    if (!valid_feature(feature)) return result;
    int candidate = -1;
    for (unsigned i = 0; i < db.count; ++i) {
        float score = -1;
        for (unsigned j = 0; j < db.people[i].samples; ++j)
            score = std::max(score, similarity(feature, db.people[i].features[j]));
        if (score > result.best) { result.second = result.best; result.best = score; candidate = i; }
        else result.second = std::max(result.second, score);
    }
    if (result.best >= match_threshold && result.best - result.second >= match_margin) result.index = candidate;
    return result;
}
inline bool forget(Database &db, const char *name)
{
    if (name && !std::strcmp(name, "all")) {
        const bool changed = db.count != 0;
        std::memset(static_cast<void *>(db.people), 0, sizeof(db.people));
        db.count = 0; return changed;
    }
    const int index = find(db, name);
    if (index < 0) return false;
    for (unsigned i = index; i + 1 < db.count; ++i) db.people[i] = db.people[i + 1];
    std::memset(static_cast<void *>(&db.people[--db.count]), 0, sizeof(Person)); return true;
}
} // namespace buddy_faces
