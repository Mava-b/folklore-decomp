/*
 * engine_capture_finder.h -- F5-F9: search the game's memory for the
 * capture state (engine_capture_finder.cpp).
 */
#pragma once

/* key: 5 the Id is red, 6 it is not, 7 the bar went up, 8 it went down,
 * 9 start again. */
void engineCaptureFinderKey(int key);
