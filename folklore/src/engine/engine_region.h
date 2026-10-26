/*
 * engine_region.h -- which release of Folklore this build was lifted from.
 *
 * tools/relift.sh writes src/recomp/folklore_region.h next to the lifted code
 * (folklore/docs/REGIONS.md). Lifted code from before that existed is the US
 * release, so that is the default.
 */
#pragma once

#if defined(__has_include)
#if __has_include("folklore_region.h")
#include "folklore_region.h"
#endif
#endif

#ifndef FOLKLORE_REGION_NAME
#define FOLKLORE_REGION_US 1
#define FOLKLORE_REGION_NAME "US"
#define FOLKLORE_TITLE_ID "BCUS98147"
#define FOLKLORE_EBOOT_SIZE 6518936ull
#define FOLKLORE_TOC_BASE 0x0056EB00u
#endif

/* The releases this project supports, for telling the player which one a
 * dump is when it is not the one this build was made from. */
#define FOLKLORE_US_TITLE_ID "BCUS98147"
#define FOLKLORE_US_EBOOT_SIZE 6518936ull
#define FOLKLORE_EU_TITLE_ID "BCES00050"
#define FOLKLORE_EU_EBOOT_SIZE 6518912ull

#if defined(FOLKLORE_REGION_EU)
#define FOLKLORE_RELEASE_NAME "European"
#else
#define FOLKLORE_RELEASE_NAME "North American"
#endif
