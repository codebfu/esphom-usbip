#ifdef USE_ESP32

#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"

#include "esphome/core/log.h"
#include "tcp_server.hpp"
#include "usbip_protocol.hpp"

static const char *const TAG = "usbip_tcp";

static uint8_t rx_buffer[4 * 1024];

static void close_socket(int sock) {
  shutdown(sock, 0);
  close(sock);
}

static void (*s_socket_close_cb)(int) = nullptr;

static void do_retransmit(void *p) {
  const int sock = (int)(intptr_t)p;
  int len;
  do {
    len = recv(sock, rx_buffer, sizeof(rx_buffer), MSG_DONTWAIT);
    if (len < 0 && errno == EWOULDBLOCK) {
      vTaskDelay(1);
      continue;
    } else if (len < 0) {
      ESP_LOGE(TAG, "Error during receive: errno %d", errno);
      break;
    } else if (len == 0) {
      break;
    } else {
      parse_request(sock, rx_buffer, len);
    }
  } while (1);

  if (s_socket_close_cb) {
    s_socket_close_cb(sock);
  }
  close_socket(sock);
  vTaskDelete(NULL);
}

static void tcp_server_task(void *pvParameters) {
  const TcpServerConfig *config = (const TcpServerConfig *)pvParameters;
  s_socket_close_cb = config->socket_close_callback;
  char addr_str[128];
  int keepAlive = 0;
  int keepIdle = config->keepalive_idle;
  int keepInterval = config->keepalive_interval;
  int keepCount = config->keepalive_count;

  struct sockaddr_in dest_addr;
  dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(config->port);

  int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (listen_sock < 0) {
    ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    vTaskDelete(NULL);
    return;
  }

  int opt = 1;
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
  if (err != 0) {
    ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
    close(listen_sock);
    vTaskDelete(NULL);
    return;
  }

  err = listen(listen_sock, 1);
  if (err != 0) {
    ESP_LOGE(TAG, "Error during listen: errno %d", errno);
    close(listen_sock);
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "Socket listening on port %d", config->port);

  while (1) {
    struct sockaddr_storage source_addr;
    socklen_t addr_len = sizeof(source_addr);
    int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
    if (sock < 0) {
      ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
      continue;
    }

    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));

    if (source_addr.ss_family == PF_INET) {
      inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
      ESP_LOGI(TAG, "Socket accepted from %s", addr_str);
    }

    xTaskCreatePinnedToCore(do_retransmit, "tcp_tx", 1 * 4096, (void *)(intptr_t)sock, 21, NULL, 1);
  }
}

void tcp_server_start(const TcpServerConfig *config) {
  static TcpServerConfig s_config;
  s_config = *config;
  xTaskCreatePinnedToCore(tcp_server_task, "tcp_server", 1 * 4096, &s_config, 21, NULL, 1);
}

#endif
