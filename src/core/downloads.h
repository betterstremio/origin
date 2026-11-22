#ifndef SB_DOWNLOADS_H
#define SB_DOWNLOADS_H
#include "core/stremio.h"
#include <stddef.h>

typedef enum {
  DL_QUEUED,
  DL_FINDING,
  DL_RUNNING,
  DL_PAUSED,
  DL_READY,
  DL_FAILED,
  DL_UNAVAILABLE,
  DL_NO_SPACE,
  DL_REMOVING
} DownloadState;

typedef struct {
  int key, season, episode;
  char type[16], id[64], vid[80], title[256], label[300];
  DownloadState state;
  long long bytes, total;
  double position;
} Download;

enum { DL_ADD_FAILED = -1, DL_ADD_EXISTS, DL_ADD_OK };
int downloads_enabled(void);
void downloads_pump(void);
void downloads_shutdown(void);
int downloads_eligible(const Stream *s);
/* A NULL source lets the queue choose a compatible stream when the episode starts. */
int downloads_add(const Download *item, const Stream *source, int maxResolution);
int downloads_count(void);
const Download *downloads_at(int i);
const Download *downloads_get(int key);
int downloads_waiting_wifi(void);
int downloads_wifi_only(void);
void downloads_set_wifi_only(int on);
int downloads_path(int key, char *out, size_t cap);
void downloads_pause(int key);
void downloads_retry(int key);
void downloads_remove(int key);
void downloads_save_position(int key, double position);
#endif
