#pragma once

// Serviced by the existing network task; no extra task or heap buffers.
bool buddy_captive_dns_start();
void buddy_captive_dns_stop();
void buddy_captive_dns_poll();
bool buddy_captive_dns_ready();
bool buddy_captive_dns_test();
bool buddy_captive_dns_probe();
unsigned buddy_captive_dns_queries();
