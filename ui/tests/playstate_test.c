/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Off-device host test for playstate.c (pure play/pause inference). No globals -> no fork isolation
 * needed; each case uses its own playstate_t. Built under ASan/UBSan via tests/Makefile. */
#include <stdio.h>
#include "playstate.h"

static int fails = 0;
#define CHECK(c,m) do{ if(!(c)){ printf("  FAIL: %s\n", m); fails++; } }while(0)

int main(void){
    /* 1. a PAUSED track (position constant): reads playing within the grace window on first sight, then
     *    self-corrects to paused once it never advances (matches the original heuristic; a false-paused
     *    at startup would break autorouting - see the reducer's first-observation note). */
    { playstate_t ps = {-1,0}; unsigned t = 100000;
      CHECK(playstate_playing(&ps,1,60000,t,      1600,0)==1, "first obs within grace -> playing");
      CHECK(playstate_playing(&ps,1,60000,t+1000, 1600,0)==1, "still within grace -> playing");
      CHECK(playstate_playing(&ps,1,60000,t+1700, 1600,0)==0, "no advance past grace -> paused"); }

    /* 2. an advancing position reads as playing (from the 2nd sample on) */
    { playstate_t ps = {-1,0}; unsigned t = 0;
      playstate_playing(&ps,1,1000,t,1600,0);                                  /* first obs -> 0 */
      CHECK(playstate_playing(&ps,1,2000,t+1000,1600,0)==1, "advance -> playing");
      CHECK(playstate_playing(&ps,1,3000,t+2000,1600,0)==1, "keeps playing"); }

    /* 3. playback that stalls (position stops advancing) flips to paused after the grace window */
    { playstate_t ps = {-1,0}; unsigned t = 0;
      playstate_playing(&ps,1,1000,t,1600,0);
      playstate_playing(&ps,1,2000,t+1000,1600,0);                              /* playing, change@t+1000 */
      CHECK(playstate_playing(&ps,1,2000,t+1500,1600,0)==1, "within grace still playing");
      CHECK(playstate_playing(&ps,1,2000,t+3000,1600,0)==0, "stalled past grace -> paused"); }

    /* 4. a backward/rewind jump does NOT count as advancing (original code wrongly did) */
    { playstate_t ps = {-1,0}; unsigned t = 0;
      playstate_playing(&ps,1,5000,t,1600,0);
      playstate_playing(&ps,1,6000,t+1000,1600,0);                              /* playing */
      CHECK(playstate_playing(&ps,1,6000,t+3000,1600,0)==0, "paused after grace");
      CHECK(playstate_playing(&ps,1,1000,t+3100,1600,0)==0, "backward jump not advancing"); }

    /* 5. no track -> not playing, and the observation resets */
    { playstate_t ps = {-1,0};
      playstate_playing(&ps,1,1000,0,1600,0);
      CHECK(playstate_playing(&ps,0,0,1000,1600,0)==0, "no track -> paused");
      CHECK(ps.last_pos_ms == -1, "no track resets observation"); }

    /* 6. with the upper bound set, a large forward PAUSED seek is not playback; a small advance is */
    { playstate_t ps = {-1,0}; unsigned t = 0, bound = 1500;
      playstate_playing(&ps,1,60000,t,1600,bound);
      playstate_playing(&ps,1,60000,t+3000,1600,bound);                         /* paused (stale) */
      CHECK(playstate_playing(&ps,1,63000,t+3100,1600,bound)==0, "+3s paused seek not playing (bounded)");
      playstate_playing(&ps,1,63000,t+3200,1600,bound);
      CHECK(playstate_playing(&ps,1,64000,t+4200,1600,bound)==1, "normal +1s advance -> playing (bounded)"); }

    /* 7. monotonic tick wraparound near UINT_MAX is handled (unsigned subtraction) */
    { playstate_t ps = {-1,0}; unsigned t = 0xFFFFFC00u;
      playstate_playing(&ps,1,1000,t,1600,0);
      CHECK(playstate_playing(&ps,1,2000,t+500,1600,0)==1, "advance across near-wrap -> playing");
      CHECK(playstate_playing(&ps,1,2000,(unsigned)(t+500+2000),1600,0)==0, "stall across wrap -> paused"); }

    printf("%s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails==1?"":"s");
    return fails ? 1 : 0;
}
