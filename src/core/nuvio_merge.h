/* The Nuvio merge: what came down for a profile (library, watch progress, watched) against this device's copy of its
   library. Pure work on trees it is given: no store, no network, no drawing, so it runs on a thread of its own and is
   tested alone (tests/nuvio). Nuvio replaces a profile's whole list on each push, so what goes back is always the
   whole merged list, and never one built from a partial answer. */
#ifndef SB_NUVIO_MERGE_H
#define SB_NUVIO_MERGE_H
#include "cJSON.h"

typedef struct {
  cJSON *lib; /* this device's library, a copy: object keyed by id, changed in place */
  const cJSON *rLib, *rProg, *rWat; /* the rows that came down (arrays) */
  const cJSON *prevIds, *prevW; /* what came down last time (arrays of strings, or NULL) */
  double last, now; /* epoch ms: the last finished sync (0 when never), this one */
  int profile; /* p_profile_id */
  const char *client; /* p_origin_client_id */
  int libCapped, watCapped; /* the list was longer than we fetch: nothing is pushed or inferred from it */
} NmIn;

typedef struct {
  cJSON **changed;
  int nChanged; /* items of in->lib that differ from what the device had */
  char *pushLib, *pushProg, *delProg, *pushW, *delW; /* JSON bodies ready to send, NULL when nothing to do */
  int nLib, nProg, nW; /* the rows in those pushes (the log) */
  cJSON *libIds, *wKeys; /* to keep for next time (prevIds, prevW); owned here */
  int refusedShrink; /* a library push that would have emptied most of the list: not sent */
} NmOut;

void nm_merge(const NmIn *in, NmOut *out);
void nm_out_free(NmOut *out);

/* times as Nuvio's rows carry them: epoch ms, epoch seconds or ISO text */
double nm_row_ms(const cJSON *v);
void nm_ms_iso(double ms, char *out, size_t cap);
#endif
