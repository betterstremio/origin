/* The Nuvio account: sign-in (a code on the TV approved on the phone, or email), the session kept fresh, the
   account's profiles, and each profile's addons, library, watch progress and watched history kept in step with
   Nuvio on the phone. Used only while a Nuvio account is in use (acct_is_nuvio): a Stremio account never reaches
   it. Every answer comes back on the main thread through a callback. */
#ifndef SB_NUVIO_H
#define SB_NUVIO_H
#include <stddef.h>

typedef void (*NvDoneCb)(int ok, const char *err, void *ud);

int  nv_configured(void);   /* this build carries Nuvio's server settings */
int  nv_signed_in(void);    /* a session is kept on this device */
const char *nv_email(void); /* the account's email ("" when unknown) */
void nv_logout(void);       /* ends the session and forgets everything kept for it here */
int  nv_session_lost(void); /* the server refused the kept session: once true, the app shows sign-in again */

/* the code on the TV: cb(ok, err) then nv_qr_code() / nv_qr_link() hold what to show */
void nv_qr_start(NvDoneCb cb, void *ud);
const char *nv_qr_code(void);
const char *nv_qr_link(void);
double nv_qr_poll_s(void);  /* seconds between checks */
/* one check: cb(1) once approved and signed in; cb(0, "pending"), cb(0, "expired") or cb(0, an error) */
void nv_qr_poll(NvDoneCb cb, void *ud);
void nv_login_email(const char *email, const char *password, NvDoneCb cb, void *ud);

/* profiles: index is Nuvio's profile_index (1 = the primary). cb may be NULL */
void nv_pull_profiles(NvDoneCb cb, void *ud);
int  nv_profile_count(void);
typedef struct { int index; char name[64]; unsigned color; int pin; int primaryAddons; } NvProfile;
int  nv_profile_get(int i, NvProfile *out);
int  nv_profile_current(void);           /* 0 when none chosen yet */
const char *nv_profile_name(void);
void nv_profile_verify_pin(int index, const char *pin, NvDoneCb cb, void *ud);
void nv_profile_add(const char *name, NvDoneCb cb, void *ud); /* one more profile on the account, using the primary's addons; cb after the list is pulled again */
void nv_profile_select(int index);       /* another profile: its data swapped in from this device's kept copy, no download */
int  nv_profile_kept(int index);         /* this device already holds that profile's copy */
void nv_scope(int index, char *out, size_t cap);
const char *nv_user_key(void);           /* account and profile: whose a remembered stream is */

/* the chosen profile's addons and library (with progress and watched history) */
void nv_addons_force_next(void);         /* the next addon sync asks every manifest again (Sync now) */
void nv_sync_addons(void (*cb)(int changed, void *ud), void *ud);
void nv_sync_library(void (*cb)(int changed, void *ud), void *ud);
unsigned nv_lib_sync_age(void);          /* ms since the last library sync began (huge when none yet) */
void nv_push_soon(void);                 /* a change here: synced a few seconds later */
int  nv_has_tmdb_key(void);              /* the signed-in account keeps a TMDB key (seen at the last sync) */
void nv_use_tmdb_key(void);              /* use that key here: the user's own one goes */
void nv_tick(void);                      /* per frame, only with a Nuvio account */
#endif
