/*
 * engine_capture_assist.h -- auto-capture for the boss Ids' capture
 * mechanics (engine_capture_assist.cpp).
 */
#pragma once

/* Once a frame: while a capture is on and auto-capture is in effect, fills
 * the capture bar. */
void engineCaptureAssistUpdate();
