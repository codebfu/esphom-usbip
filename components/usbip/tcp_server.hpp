#pragma once

#ifdef USE_ESP32

#include <cstdint>

struct TcpServerConfig {
  uint16_t port;
  int keepalive_idle;
  int keepalive_interval;
  int keepalive_count;
  void (*socket_close_callback)(int sock) = nullptr;
};

void tcp_server_start(const TcpServerConfig *config);

#endif
