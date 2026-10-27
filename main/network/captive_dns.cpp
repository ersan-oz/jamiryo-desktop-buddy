#include "captive_dns.h"
#include "setup_network.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include "lwip/sockets.h"

namespace {
std::atomic<int> server{-1};
std::atomic<unsigned> queries{0};
constexpr size_t packet_limit = 512;
uint16_t word(const uint8_t *p) { return uint16_t(p[0]) << 8 | p[1]; }

size_t response(const uint8_t *query, size_t length, uint8_t *reply, size_t capacity)
{
    // One uncompressed IN question. Ignore optional EDNS data and never echo it.
    if (length < 17 || length > packet_limit || (word(query + 2) & 0xfa00)
        || word(query + 4) != 1 || word(query + 6) || word(query + 8)) return 0;
    size_t position = 12;
    for (;;) {
        if (position >= length) return 0;
        const auto label = query[position++];
        if (!label) break;
        if (label > 63 || label > length - position) return 0;
        position += label;
        if (position - 12 > 254) return 0;
    }
    if (position - 12 > 255 || length - position < 4 || word(query + position + 2) != 1) return 0;
    const bool answer_a = word(query + position) == 1;
    const size_t question_end = position + 4, size = question_end + (answer_a ? 16 : 0);
    if (size > capacity) return 0;
    std::memcpy(reply, query, question_end);
    reply[2] = uint8_t(0x84 | (query[2] & 1)); reply[3] = 0;
    reply[6] = 0; reply[7] = answer_a ? 1 : 0;
    std::memset(reply + 8, 0, 4);
    if (answer_a) {
        const uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0};
        std::memcpy(reply + question_end, answer, sizeof(answer));
        std::memcpy(reply + question_end + 12, buddy_setup::address, 4);
    }
    // AAAA/HTTPS and other types receive NODATA, allowing IPv4 fallback.
    return size;
}
}

bool buddy_captive_dns_start()
{
    if (server.load() >= 0) return true;
    const int socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket < 0) return false;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(53);
    address.sin_addr.s_addr = inet_addr(BUDDY_SETUP_IP);
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK)
        || bind(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
        close(socket); return false;
    }
    queries.store(0); server.store(socket); return true;
}
void buddy_captive_dns_stop()
{
    const int socket = server.exchange(-1);
    if (socket >= 0) close(socket);
}
bool buddy_captive_dns_ready() { return server.load() >= 0; }
unsigned buddy_captive_dns_queries() { return queries.load(); }
void buddy_captive_dns_poll()
{
    const int socket = server.load();
    if (socket < 0) return;
    uint8_t query[packet_limit + 1], reply[packet_limit];
    // Bound work per tick to preserve voice streaming and sensor processing.
    for (unsigned i = 0; i < 2; ++i) {
        sockaddr_in peer{}; socklen_t length = sizeof(peer);
        const int received = recvfrom(socket, query, sizeof(query), 0, reinterpret_cast<sockaddr *>(&peer), &length);
        if (received <= 0) break;
        if (peer.sin_family != AF_INET || (ntohl(peer.sin_addr.s_addr) & 0xffffff00U) != buddy_setup::subnet) continue;
        const size_t size = response(query, received, reply, sizeof(reply));
        if (size) { ++queries; sendto(socket, reply, size, 0, reinterpret_cast<sockaddr *>(&peer), length); }
    }
}
bool buddy_captive_dns_test()
{
    uint8_t query[packet_limit + 1] = {0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
        7, 'c', 'a', 'p', 't', 'i', 'v', 'e', 4, 't', 'e', 's', 't', 0, 0, 1, 0, 1};
    uint8_t reply[packet_limit]{};
    constexpr size_t length = 30;
    bool ok = response(query, length, reply, sizeof(reply)) == length + 16
        && word(reply) == 0x1234 && word(reply + 6) == 1
        && !std::memcmp(reply + length + 12, buddy_setup::address, 4);
    for (size_t i = 0; i < length; ++i) ok &= response(query, i, reply, sizeof(reply)) == 0;
    ok &= response(query, length, reply, length + 15) == 0;
    query[27] = 28; ok &= response(query, length, reply, sizeof(reply)) == length && word(reply + 6) == 0;
    query[27] = 65; ok &= response(query, length, reply, sizeof(reply)) == length && word(reply + 6) == 0;
    query[27] = 1; query[29] = 3; ok &= response(query, length, reply, sizeof(reply)) == 0;
    query[29] = 1; query[12] = 0xc0; ok &= response(query, length, reply, sizeof(reply)) == 0;
    query[12] = 7; query[5] = 2; ok &= response(query, length, reply, sizeof(reply)) == 0;
    query[5] = 1; query[2] = 0x81; ok &= response(query, length, reply, sizeof(reply)) == 0;
    query[2] = 1; ok &= response(query, sizeof(query), reply, sizeof(reply)) == 0;
    return ok;
}
bool buddy_captive_dns_probe()
{
    if (!buddy_captive_dns_ready()) return false;
    const int socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket < 0) return false;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = inet_addr(BUDDY_SETUP_IP);
    timeval timeout{2, 0};
    bool ok = bind(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0
        && setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
    address.sin_port = htons(53);
    uint8_t query[] = {0x56, 0x78, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
        7, 'c', 'a', 'p', 't', 'i', 'v', 'e', 4, 't', 'e', 's', 't', 0, 0, 1, 0, 1};
    uint8_t reply[packet_limit]{};
    for (unsigned i = 0; ok && i < 2; ++i) {
        query[27] = i ? 28 : 1;
        ok &= sendto(socket, query, sizeof(query), 0, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == sizeof(query);
        const int received = ok ? recv(socket, reply, sizeof(reply), 0) : -1;
        ok &= received == int(sizeof(query) + (i ? 0 : 16)) && word(reply) == 0x5678
            && word(reply + 6) == (i ? 0 : 1);
        if (!i && ok) ok &= !std::memcmp(reply + received - 4, &address.sin_addr.s_addr, 4);
    }
    close(socket); return ok;
}
