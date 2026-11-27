/*
 * nhl94_tags.h - human-player name tags drawn onto the presented frames
 * (presentation only). See nhl94_tags.c and docs/PLAYER_LOCK.md, "Name tags".
 */
#ifndef NHL94_TAGS_H
#define NHL94_TAGS_H
#include <stdint.h>

/* Launcher setting (nhl94_mode.c, nhl94-control.ini "nametags"); the
 * NHL94_NAMETAGS=0/1 environment overrides it. Default on. */
void nhl94_tags_set_setting(int on);
int  nhl94_tags_setting(void);
/* Effective value (environment over setting); *from_env = 1 when the
 * environment decided. */
int  nhl94_tags_enabled(void);
int  nhl94_tags_env_override(void);

/* Call-for-pass icon (a ring at the feet and a "!" bubble for a teammate who
 * is calling for the puck, nhl94_seats_call_flags). Launcher setting
 * ("callicon" in nhl94-control.ini); NHL94_CALLICON=0/1/all overrides it
 * (-1 = unset). It rides on the name tags: no tags, no icon. Display only. */
void nhl94_tags_set_callicon(int on);
int  nhl94_tags_callicon_setting(void);
int  nhl94_tags_callicon_env_override(void);
/* Effective: 0 off, 1 the local team's calls, 2 every team's. */
int  nhl94_tags_callicon_mode(void);

/* Capture points (guest RAM reads, host statics only). */
/* Authoritative DoGameFrame -> setvideo ($007998): the frame setvideo is
 * about to build is the one the NEXT tick presents. */
void nhl94_tags_main_hook(void);
/* A view is about to render (clears its record). */
void nhl94_tags_view_begin(int view);
/* A view's setvideo hook, after the view's camera / mirror was applied. */
void nhl94_tags_view_capture(int view);

/* GameSpec.present_overlay (view_index -1 = the main frame). */
void nhl94_tags_present_overlay(uint32_t *pixels, int w, int h, int stride, int view_index);

#endif
