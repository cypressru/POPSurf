/* Bookmarks stored as a VMS package on a VMU. */
#ifndef PS_MARKS_H
#define PS_MARKS_H

#include "ps_types.h"
#include "ps_url.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sized to keep the complete VMS package to seven VMU blocks. */
#define PS_MARKS_MAX 8

/* Long enough for a page title worth recognising. */
#define PS_MARK_TITLE_MAX 48

/* Longer URLs are rejected rather than truncated. */
#define PS_MARK_URL_MAX 256

typedef struct {
    char title[PS_MARK_TITLE_MAX];
    char url[PS_MARK_URL_MAX];
} ps_mark;

typedef struct {
    ps_mark list[PS_MARKS_MAX];
    int     count;

    /* Set when memory differs from the saved copy. */
    int dirty;

    /* VMU path, or empty for session-only bookmarks. */
    char path[16];
} ps_marks;

/* Finds a memory card and reads whatever is on it. Safe to call with no card
 * present; the list is then simply empty. */
void ps_marks_init(ps_marks *m);

static inline int ps_marks_count(const ps_marks *m) { return m->count; }
static inline int ps_marks_have_card(const ps_marks *m) { return m->path[0] != '\0'; }

const char *ps_marks_title(const ps_marks *m, int i);
const char *ps_marks_url(const ps_marks *m, int i);

/* Non-zero if this URL is already bookmarked, so the menu can offer removing
 * it rather than adding a second copy. */
int ps_marks_find(const ps_marks *m, const char *url);

/* Returns -1 when full or already present. Empty titles use the URL. */
int ps_marks_add(ps_marks *m, const char *url, const char *title);

int ps_marks_remove(ps_marks *m, int i);

/* Writes the list to the card. Blocking, on the order of a second, so this is
 * called at the moment the user asks for it and never from the frame loop.
 * Returns 0 on success. */
int ps_marks_save(ps_marks *m);

#ifdef __cplusplus
}
#endif

#endif /* PS_MARKS_H */
