/******************************************************************************
    NTP Client Module - Implementation
    Copyright (C) 2026

    Implements NTP (Network Time Protocol) client for time synchronization
******************************************************************************/

#include "ntp-client.h"
#include <obs-module.h>
#include <string.h>
#include <time.h>
#include <util/platform.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#endif

/* NTP常量 */
#define NTP_TIMESTAMP_DELTA 2208988800ULL /* 1900到1970的秒数 */
#define NTP_VERSION 3
#define NTP_MODE_CLIENT 3
#define NTP_PACKET_SIZE 48

/* 日志宏 */
#define ntp_log(level, format, ...)                                            \
  blog(level, "[NTP Client] " format, ##__VA_ARGS__)

/* 辅助函数:将网络字节序转换为主机字节序 */
static uint32_t ntohl_swap(uint32_t netlong) { return ntohl(netlong); }

/* 辅助函数:将主机字节序转换为网络字节序 */
static uint32_t htonl_swap(uint32_t hostlong) { return htonl(hostlong); }

/* 辅助函数:获取单调时钟(纳秒) — 仅用于测量间隔,不是墙上时钟 */
static uint64_t get_monotonic_ns(void) { return os_gettime_ns(); }

/* Get the system wall-clock time as nanoseconds since the Unix epoch (1970).
 * This is the OS clock, which on Windows/Linux is already disciplined by the
 * OS's own NTP service — so it serves as both the NTP round-trip reference
 * and the fallback when our own NTP query fails. */
static uint64_t get_system_wallclock_ns(void) {
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
  /* FILETIME is 100ns ticks since 1601-01-01; 116444736000000000 ticks
   * separate 1601 from 1970. */
  t -= 116444736000000000ULL;
  return t * 100ULL;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* 辅助函数:将NTP时间戳转换为纳秒 */
static uint64_t ntp_to_ns(ntp_timestamp_t *ntp) {
  uint64_t seconds = (uint64_t)ntp->seconds;
  uint64_t fraction = (uint64_t)ntp->fraction;

  /* 转换为Unix时间戳 */
  if (seconds > NTP_TIMESTAMP_DELTA) {
    seconds -= NTP_TIMESTAMP_DELTA;
  }

  /* 秒转纳秒 */
  uint64_t ns = seconds * 1000000000ULL;

  /* 分数部分转纳秒: fraction / 2^32 * 10^9 */
  ns += (fraction * 1000000000ULL) >> 32;

  return ns;
}

/* 辅助函数:将纳秒转换为NTP时间戳 */
static void ns_to_ntp(uint64_t ns, ntp_timestamp_t *ntp) {
  /* 纳秒转秒 */
  uint64_t seconds = ns / 1000000000ULL;
  uint64_t fraction_ns = ns % 1000000000ULL;

  /* 转换为NTP时间戳(从1900年开始) */
  ntp->seconds = (uint32_t)(seconds + NTP_TIMESTAMP_DELTA);

  /* 分数部分: fraction_ns / 10^9 * 2^32 */
  ntp->fraction = (uint32_t)((fraction_ns << 32) / 1000000000ULL);
}

/* 初始化Winsock(仅Windows) */
#ifdef _WIN32
static bool init_winsock(void) {
  static bool initialized = false;
  if (initialized) {
    return true;
  }

  WSADATA wsa_data;
  int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
  if (result != 0) {
    ntp_log(LOG_ERROR, "WSAStartup failed: %d", result);
    return false;
  }

  initialized = true;
  return true;
}
#endif

/* 初始化NTP客户端 */
bool ntp_client_init(ntp_client_t *client, const char *server, uint16_t port) {
  if (!client || !server) {
    ntp_log(LOG_ERROR, "Invalid parameters");
    return false;
  }

  memset(client, 0, sizeof(ntp_client_t));

#ifdef _WIN32
  if (!init_winsock()) {
    return false;
  }
#endif

  /* 复制服务器地址 */
  strncpy(client->server_address, server, sizeof(client->server_address) - 1);
  client->server_port = port;
  client->socket_fd = -1;
  client->is_initialized = true;

  ntp_log(LOG_INFO, "NTP client initialized (server: %s:%d)", server, port);

  return true;
}

/* 执行NTP时间同步 */
bool ntp_client_sync(ntp_client_t *client) {
  if (!client || !client->is_initialized) {
    ntp_log(LOG_ERROR, "Client not initialized");
    return false;
  }

  int sock = -1;
  struct addrinfo hints, *server_info = NULL;
  ntp_packet_t packet;
  bool success = false;

  /* 创建UDP socket. Force IPv4 — AF_UNSPEC can hand back an IPv6 address
   * first, and on networks with a broken IPv6 UDP path the reply never
   * arrives (sendto succeeds locally, recvfrom then fails fast with
   * WSAECONNRESET from an ICMP unreachable). IPv4 NTP is universal. */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", client->server_port);

  int ret = getaddrinfo(client->server_address, port_str, &hints, &server_info);
  if (ret != 0) {
    ntp_log(LOG_ERROR, "getaddrinfo failed for %s: %d", client->server_address,
            ret);
    goto cleanup;
  }

  sock = (int)socket(server_info->ai_family, server_info->ai_socktype,
                     server_info->ai_protocol);
  if (sock < 0) {
    ntp_log(LOG_ERROR, "socket creation failed");
    goto cleanup;
  }

  /* 设置接收超时. NOTE: Windows SO_RCVTIMEO takes a DWORD of milliseconds,
   * NOT a struct timeval — passing a timeval makes Windows read tv_sec(=5)
   * as 5 milliseconds, which is far too short for a network round-trip and
   * causes a spurious WSAETIMEDOUT. Use the correct type per platform. */
#ifdef _WIN32
  DWORD timeout_ms = 3000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
             sizeof(timeout_ms));
#else
  struct timeval timeout;
  timeout.tv_sec = 3;
  timeout.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
             sizeof(timeout));
#endif

  /* 构建NTP请求包 */
  memset(&packet, 0, sizeof(packet));
  packet.li_vn_mode = (0 << 6) | (NTP_VERSION << 3) | NTP_MODE_CLIENT;

  /* Send time (T1), measured against the system wall clock so the offset
   * calculation below is dimensionally consistent with the server's
   * wall-clock T2/T3. (We separately keep a monotonic reference for
   * interpolating between syncs — see last_sync_mono.) */
  uint64_t t1_ns = get_system_wallclock_ns();
  ns_to_ntp(t1_ns, &packet.transmit_timestamp);
  packet.transmit_timestamp.seconds =
      htonl_swap(packet.transmit_timestamp.seconds);
  packet.transmit_timestamp.fraction =
      htonl_swap(packet.transmit_timestamp.fraction);

  /* 发送请求 */
  ret = sendto(sock, (const char *)&packet, sizeof(packet), 0,
               server_info->ai_addr, (int)server_info->ai_addrlen);
  if (ret < 0) {
    ntp_log(LOG_ERROR, "sendto failed");
    goto cleanup;
  }

  /* 接收响应 */
  struct sockaddr_storage from_addr;
  socklen_t from_len = sizeof(from_addr);
  ret = recvfrom(sock, (char *)&packet, sizeof(packet), 0,
                 (struct sockaddr *)&from_addr, &from_len);

  /* Receive time (T4), and monotonic reference for inter-sync interpolation. */
  uint64_t t4_ns = get_system_wallclock_ns();
  uint64_t t4_mono = get_monotonic_ns();

  /* Detect failure FIRST. recvfrom returns -1 on error; comparing a signed
   * -1 against the unsigned sizeof() would wrongly pass, so check the sign
   * explicitly. A short read is also invalid for a 48-byte NTP packet. */
  if (ret <= 0 || (size_t)ret < sizeof(packet)) {
#ifdef _WIN32
    ntp_log(LOG_WARNING,
            "recvfrom failed (ret=%d, WSA=%d); falling back to system clock",
            ret, WSAGetLastError());
#else
    ntp_log(LOG_WARNING,
            "recvfrom failed (ret=%d); falling back to system clock", ret);
#endif
    goto cleanup;
  }

  /* 解析响应 */
  ntp_timestamp_t t2, t3;
  t2.seconds = ntohl_swap(packet.receive_timestamp.seconds);
  t2.fraction = ntohl_swap(packet.receive_timestamp.fraction);
  t3.seconds = ntohl_swap(packet.transmit_timestamp.seconds);
  t3.fraction = ntohl_swap(packet.transmit_timestamp.fraction);

  /* Sanity check: a real NTP timestamp is well past the epoch delta. If the
   * server echoed our originate (or sent garbage), reject it. */
  if (t3.seconds < NTP_TIMESTAMP_DELTA) {
    ntp_log(LOG_WARNING,
            "NTP response implausible (T3 seconds=%u < epoch delta); "
            "falling back to system clock", t3.seconds);
    goto cleanup;
  }

  uint64_t t2_ns = ntp_to_ns(&t2);
  uint64_t t3_ns = ntp_to_ns(&t3);

  /* Clock offset (server - local), all wall-clock ns:
   *   offset = ((T2 - T1) + (T3 - T4)) / 2 */
  int64_t offset = ((int64_t)(t2_ns - t1_ns) + (int64_t)(t3_ns - t4_ns)) / 2;

  /* 更新客户端状态 */
  client->time_offset_ns = offset;
  client->last_sync_mono = t4_mono;
  client->is_synced = true;
  client->sync_count++;

  ntp_log(LOG_INFO,
          "NTP sync successful (offset: %lld ms vs system clock, count: %u)",
          (long long)(offset / 1000000), client->sync_count);

  success = true;

cleanup:
  if (server_info) {
    freeaddrinfo(server_info);
  }
  if (sock >= 0) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
  }

  if (!success) {
    client->error_count++;
  }

  return success;
}

/* 获取当前的NTP时间戳
 *
 * Always succeeds: returns the system wall clock corrected by the NTP offset
 * when we have one. If our NTP query never succeeded, the offset is 0 and we
 * return the system clock directly — which on Windows/Linux is itself
 * NTP-disciplined by the OS, so it's a correct wall-clock fallback rather
 * than garbage. */
bool ntp_client_get_time(ntp_client_t *client, ntp_timestamp_t *timestamp) {
  if (!client || !timestamp) {
    return false;
  }

  uint64_t now_wall_ns = get_system_wallclock_ns();
  int64_t offset = client->is_synced ? client->time_offset_ns : 0;
  uint64_t corrected_ns = (uint64_t)((int64_t)now_wall_ns + offset);

  ns_to_ntp(corrected_ns, timestamp);
  return true;
}

/* 获取时间偏移 */
int64_t ntp_client_get_offset(ntp_client_t *client) {
  if (!client) {
    return 0;
  }
  return client->time_offset_ns;
}

/* 检查是否需要重新同步 */
bool ntp_client_needs_resync(ntp_client_t *client, uint32_t max_age_seconds) {
  if (!client || !client->is_synced) {
    return true;
  }

  uint64_t current = get_monotonic_ns();
  uint64_t age_ns = current - client->last_sync_mono;
  uint64_t max_age_ns = (uint64_t)max_age_seconds * 1000000000ULL;

  return age_ns > max_age_ns;
}

/* 销毁NTP客户端 */
void ntp_client_destroy(ntp_client_t *client) {
  if (!client) {
    return;
  }

  ntp_log(LOG_INFO, "NTP client destroyed (syncs: %u, errors: %u)",
          client->sync_count, client->error_count);

  memset(client, 0, sizeof(ntp_client_t));
}
