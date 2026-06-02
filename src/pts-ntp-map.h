#ifndef PTS_NTP_MAP_H
#define PTS_NTP_MAP_H

#include "ntp-client.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PTS_NTP_MAP_SIZE 64

typedef struct pts_ntp_entry {
  int64_t pts;
  ntp_timestamp_t ntp_time;
  bool valid;
} pts_ntp_entry_t;

typedef struct pts_ntp_map {
  pts_ntp_entry_t entries[PTS_NTP_MAP_SIZE];
  uint32_t write_idx;
} pts_ntp_map_t;

static inline void pts_ntp_map_store(pts_ntp_map_t *map, int64_t pts,
                                     const ntp_timestamp_t *ntp_time) {
  uint32_t idx = map->write_idx % PTS_NTP_MAP_SIZE;
  map->entries[idx].pts = pts;
  map->entries[idx].ntp_time = *ntp_time;
  map->entries[idx].valid = true;
  map->write_idx++;
}

static inline bool pts_ntp_map_lookup(const pts_ntp_map_t *map, int64_t pts,
                                      ntp_timestamp_t *ntp_out) {
  for (uint32_t i = 0; i < PTS_NTP_MAP_SIZE; i++) {
    if (map->entries[i].valid && map->entries[i].pts == pts) {
      *ntp_out = map->entries[i].ntp_time;
      return true;
    }
  }
  return false;
}

#endif /* PTS_NTP_MAP_H */
