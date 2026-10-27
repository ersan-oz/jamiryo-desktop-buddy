#pragma once
bool buddy_remote_start();
void buddy_remote_tick(bool wifi_connected);
bool buddy_remote_command(const char *line);
