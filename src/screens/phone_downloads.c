#include "screens/phone.h"
#include "screens/player.h"
#include "core/downloads.h"
#include "core/hit.h"
#include "core/i18n.h"
#include "core/keys.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Scroller page;
static Screen deleteSheet;
static int deleteKey, deleteSeason;

void phone_downloads_enter(void) {
  scroller_set(&page, 0);
}

void phone_downloads_tick(float dt) {
  scroller_tick(&page, dt);
}

static int in_progress(DownloadState state) {
  return state == DL_QUEUED || state == DL_FINDING || state == DL_RUNNING;
}

static int same_season(const Download *a, const Download *b) {
  return a->season >= 0 && a->season == b->season && !strcmp(a->id, b->id);
}

static int compare(const void *left, const void *right) {
  const Download *a = *(const Download *const *)left, *b = *(const Download *const *)right;
  int n = strcasecmp(a->title, b->title);
  if (!n)
    n = strcmp(a->id, b->id);
  if (n)
    return n;
  if (a->season != b->season)
    return a->season < b->season ? -1 : 1;
  if (a->episode != b->episode)
    return a->episode < b->episode ? -1 : 1;
  return a->key < b->key ? -1 : a->key != b->key;
}

static void wifi_act(int a, int b) {
  (void)a;
  (void)b;
  downloads_set_wifi_only(!downloads_wifi_only());
}

static void item_act(int key, int b) {
  (void)b;
  const Download *d = downloads_get(key);
  if (!d)
    return;
  if (d->state == DL_READY) {
    if (!player_play_download(key))
      toast(T("The downloaded file could not be opened"), 2400, NULL);
  } else if (in_progress(d->state)) {
    downloads_pause(key);
  } else {
    downloads_retry(key);
  }
}

static void season_action(int key, int pause) {
  const Download *group = downloads_get(key);
  if (!group)
    return;
  int n = downloads_count();
  for (int i = 0; i < n; i++) {
    const Download *d = downloads_at(i);
    if (!d || !same_season(group, d))
      continue;
    if (pause)
      downloads_pause(d->key);
    else
      downloads_retry(d->key);
  }
}

static void finish_delete(int yes, int unused) {
  (void)unused;
  app_overlay = NULL;
  if (!yes)
    return;
  if (!deleteSeason) {
    downloads_remove(deleteKey);
    return;
  }
  const Download *selected = downloads_get(deleteKey);
  if (!selected)
    return;
  Download group = *selected;
  for (int i = downloads_count() - 1; i >= 0; i--) {
    const Download *d = downloads_at(i);
    if (d && same_season(&group, d))
      downloads_remove(d->key);
  }
}

static void remove_act(int key, int season) {
  deleteKey = key;
  deleteSeason = season;
  app_overlay = &deleteSheet;
}

static int delete_key(int k) {
  if (k == K_BACK)
    finish_delete(0, 0);
  return 1;
}

static void draw_delete(void) {
  /* app.c keeps drawing every overlay that has shown (for its closing motion): closed, the sheet draws nothing and
     takes no touch, or it stayed on the page after Delete or Cancel */
  if (app_overlay != &deleteSheet)
    return;
  float w = SB_W - 2 * PH_M;
  if (w > 420)
    w = 420;
  float x = (SB_W - w) / 2, h = 160, y = (SB_H - h) / 2;
  float tx = x + 16, tw = w - 32, bw = (tw - 8) / 2;
  gfx_rect(0, 0, SB_W, SB_H, BLACK_A(152));
  hit_act(0, 0, SB_W, SB_H, finish_delete, 0, 0);
  gfx_rrect(x, y, w, h, 16, PH_SURF);
  hit_shield(x, y, w, h);
  text_draw_wrapped(T(deleteSeason ? "Delete season downloads?" : "Delete download?"), tx, y + 16, tw, 2, PH_TX_ROW,
                    800, PH_TXT, 0, ALIGN_LEFT, 21);
  text_draw_wrapped(T(deleteSeason ? "This clears the files and queue for this season."
                                   : "This removes the saved file from your device."),
                    tx, y + 58, tw, 2, PH_TX_BODY, 500, PH_TXT2, 0, ALIGN_LEFT, 18);
  phone_button(T("Cancel"), NULL, tx, y + 101, bw, 44, 0, finish_delete, 0, 0);
  phone_button(T("Delete"), NULL, tx + bw + 8, y + 101, bw, 44, 1, finish_delete, 1, 0);
}
static Screen deleteSheet = { NULL, NULL, delete_key, NULL, draw_delete, NULL, NULL, NULL, NULL };

static void size_label(char *out, size_t cap, long long bytes) {
  if (bytes >= 1000000000)
    snprintf(out, cap, "%.2f GB", bytes / 1e9);
  else if (bytes > 0 && bytes < 1000000)
    snprintf(out, cap, "%.0f KB", bytes / 1e3);
  else
    snprintf(out, cap, "%.0f MB", bytes / 1e6);
}

static void status_label(char *out, size_t cap, const Download *d) {
  const char *name;
  switch (d->state) {
  case DL_QUEUED:
    name = downloads_waiting_wifi() ? "Waiting for Wi-Fi" : "Queued";
    break;
  case DL_FINDING:
    name = "Asking your addons";
    break;
  case DL_RUNNING:
    name = "Downloading";
    break;
  case DL_READY:
    name = "Downloaded";
    break;
  case DL_PAUSED:
    name = "Paused";
    break;
  case DL_UNAVAILABLE:
    name = "No downloadable source";
    break;
  case DL_NO_SPACE:
    name = "Not enough storage";
    break;
  default:
    name = "Download failed";
    break;
  }
  char bytes[32];
  size_label(bytes, sizeof bytes, d->bytes);
  if (d->state == DL_RUNNING && d->total > 0) {
    double pct = d->bytes * 100.0 / d->total;
    snprintf(out, cap, "%s · %.0f%%", T(name), pct > 100 ? 100 : pct);
  } else if (d->bytes > 0) {
    snprintf(out, cap, "%s · %s", T(name), bytes);
  } else {
    snprintf(out, cap, "%s", T(name));
  }
}

static void item_draw(const Download *d, float x, float y, float w, int group) {
  gfx_rrect(x, y, w, 104, 10, WHITE_A(12));
  const char *title = group && d->label[0] ? d->label : d->title;
  text_draw(title, x + 12, y + 10, PH_TX_BODY, 700, PH_TXT, 0, w - 24, ALIGN_LEFT);
  char status[96];
  status_label(status, sizeof status, d);
  text_draw(status, x + 12, y + 31, PH_TX_SMALL, 500, PH_TXT2, 0, w - 24, ALIGN_LEFT);
  const char *action = d->state == DL_READY    ? d->position > 0 ? "Resume" : "Play"
                       : in_progress(d->state) ? "Pause"
                       : d->state == DL_PAUSED ? "Resume"
                                               : "Retry";
  float bw = (w - 32) / 2;
  phone_button(T(action), NULL, x + 12, y + 53, bw, 44, d->state == DL_READY, item_act, d->key, 0);
  phone_button(T("Delete"), NULL, x + 20 + bw, y + 53, bw, 44, 0, remove_act, d->key, 0);
}

static void group_draw(const Download *d, float x, float y, float w, int n, int ready, int busy) {
  char title[350], progress[100];
  if (d->season)
    snprintf(title, sizeof title, "%s · %s %d", d->title, T("Season"), d->season);
  else
    snprintf(title, sizeof title, "%s · %s", d->title, T("Specials"));
  text_draw(title, x, y + 4, PH_TX_ROW, 800, PH_TXT, 0, w, ALIGN_LEFT);
  char done[24], total[24];
  snprintf(done, sizeof done, "%d", ready);
  snprintf(total, sizeof total, "%d", n);
  snprintf(progress, sizeof progress, "%s", T2("{done} of {total} downloaded", "done", done, "total", total));
  text_draw(progress, x, y + 30, PH_TX_SMALL, 500, PH_TXT2, 0, w, ALIGN_LEFT);
  float bw = (w - 8) / 2;
  if (ready < n)
    phone_button(T(busy ? "Pause season" : "Retry missing"), NULL, x, y + 50, bw, 44, 0, season_action, d->key, busy);
  phone_button(T("Delete season"), NULL, x + (ready < n ? bw + 8 : 0), y + 50, bw, 44, 0, remove_act, d->key, 1);
}

void phone_downloads_draw(float x, float w, float top, float bottom) {
  float y0 = top + 80, y = y0 - page.off;
  const char *network = T(downloads_wifi_only() ? "Wi-Fi only" : "Wi-Fi and mobile data");
  float networkWidth = phone_is_tablet() ? text_width(network, PH_TX_BODY, 700, 0) + 32 : w;
  phone_button(network, NULL, x, top, networkWidth, 44, 0, wifi_act, 0, 0);
  int n = downloads_count();
  const Download **items = n ? malloc((size_t)n * sizeof *items) : NULL;
  if (!items)
    n = 0;
  long long used = 0;
  for (int i = 0; i < n; i++) {
    items[i] = downloads_at(i);
    used += items[i]->bytes;
  }
  if (n > 1)
    qsort(items, (size_t)n, sizeof *items, compare);
  char bytes[32];
  size_label(bytes, sizeof bytes, used);
  text_draw(T1("{size} used by downloads", "size", bytes), x, top + 53, PH_TX_SMALL, 600, PH_TXT2, 0, w, ALIGN_LEFT);
  touch_region(x, y0, w, bottom - y0, NULL, &page);
  gfx_clip_push(x, y0, w, bottom - y0);
  for (int i = 0; i < n;) {
    const Download *d = items[i];
    int end = i + 1;
    while (end < n && same_season(d, items[end]))
      end++;
    if (d->season >= 0) {
      int ready = 0, busy = 0;
      for (int j = i; j < end; j++) {
        ready += items[j]->state == DL_READY;
        busy += in_progress(items[j]->state);
      }
      float h = 102;
      if (y + h > y0 && y < bottom)
        group_draw(d, x, y, w, end - i, ready, busy);
      y += h;
    }
    for (; i < end; i++) {
      if (y + 104 > y0 && y < bottom)
        item_draw(items[i], x, y, w, d->season >= 0);
      y += 112;
    }
  }
  if (!n)
    text_draw(T("No downloads yet"), x + w / 2, y + 36, PH_TX_BODY, 600, PH_TXT2, 0, w, ALIGN_CENTER);
  gfx_clip_pop();
  free(items);
  scroller_limits(&page, 0, y + page.off + 12 - bottom);
}
