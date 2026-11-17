/* Per-card fill metrics for the fast-scroll test (SB_SCROLL / SB_INPUT, see scrollin.c). A card is "final" when its
   at-rest picture (sharp crop and logo for a poster card, the box picture for the open box) is drawn at full fade.
   One "[fill] phase=..." line per phase. Development only: every call is a cheap no-op when the test is off. */
#ifndef SB_FILLSTAT_H
#define SB_FILLSTAT_H
int fill_on(void);
void fill_screen_ref(void); /* board_draw start, before any offset: the screen's corners in surface pixels */
void fill_begin(const char *id, int kind, int rowDist); /* before a card is drawn: the title's id, kind (bit 0: the open box; bit 1: a wide row), the row's distance from the focus */
void fill_why(int imgCode); /* the card's picture is not final: img_why_shaped of the picture it waits on */
void fill_why_prep(void); /* the card has no prep or holds it */
void fill_rebuild(void); /* the board rebuilt its rows */
void fill_art(int final); /* card_art / the open card: what the card's picture drew */
void fill_logo(int final); /* crop_logo: the logo's state */
/* after a card is drawn: x,y,w,h in layout units (offsets applied by the gfx stack); needLogo: a poster crop;
   skip: not counted now (the card is opening) */
void fill_card(const void *row, int idx, float x, float y, float w, float h, int needLogo, int skip);
void fill_box(int ready); /* the open box got its title: its picture was in on arrival or not */
void fill_focus_row(const void *row); /* the focused row (BR profile: its cards are kept apart from the others) */
void fill_tick(void); /* once per frame, before keys are sent */
void fill_phase_begin(const char *name, int expectRow, int expectCol);
void fill_phase_active(int on); /* keys are being sent (frame numbers count); off: settling */
void fill_phase_expect(int row, int col); /* the expected final focus, settled late */
void fill_phase_end(int row, int col); /* prints the line */
void fill_key(double lateMs); /* a motion key was sent, scheduled lateMs ago */
void fill_dev_decode_delay(void); /* SB_DEV_SLOWDEC=ms: every decode waits (proves the gate) */
int fill_dev_no_ahead(void); /* SB_DEV_NOAHEAD=1: the row-ahead picture loading off */
int fill_dev_budget_pct(void); /* SB_DEV_BUDGET_PCT=50: the picture budget scaled */
double fill_vfr_now(void); /* the last finished frame's visible-final ratio (1 when no card) */
#endif
