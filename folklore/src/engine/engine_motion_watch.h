/*
 * engine_motion_watch.h -- F3: log which game functions read the motion
 * sensor values, and their callers (engine_motion_watch.cpp).
 */
#pragma once

/* Start a 20-second watch, or stop the one running (the report goes to the
 * log either way). */
void engineMotionWatchToggle();
/* Once a frame: ends the watch when its time is up. */
void engineMotionWatchUpdate();
/* Whether a watch is running, and the seconds it has left. */
bool engineMotionWatchActive(unsigned* secondsLeft);
