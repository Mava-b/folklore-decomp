/*
 * engine_region.h -- which executable of Folklore this build was lifted from.
 *
 * tools/relift.sh writes src/recomp/folklore_region.h next to the lifted code
 * (folklore/docs/REGIONS.md): the release (US BCUS98147 or EU BCES00050) and
 * the version (the v1.00 disc, or the 1.10 update's EBOOT). Lifted code from
 * before that existed is the US v1.00 disc, so that is the default; headers
 * from before the version was recorded are v1.00.
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

#ifndef FOLKLORE_GAME_VERSION
#define FOLKLORE_GAME_VERSION 100
#define FOLKLORE_VERSION_NAME "1.00"
#endif

/* The releases this project supports, for telling the player which one a
 * dump is when it is not the one this build was made from. */
#define FOLKLORE_US_TITLE_ID "BCUS98147"
#define FOLKLORE_US_EBOOT_SIZE 6518936ull
#define FOLKLORE_EU_TITLE_ID "BCES00050"
#define FOLKLORE_EU_EBOOT_SIZE 6518912ull
/* The 1.10 update's decrypted EBOOT.elf. */
#define FOLKLORE_US_UPDATE_EBOOT_SIZE 6520504ull
#define FOLKLORE_EU_UPDATE_EBOOT_SIZE 6520480ull

#if defined(FOLKLORE_REGION_EU)
#define FOLKLORE_RELEASE_NAME "European"
#else
#define FOLKLORE_RELEASE_NAME "North American"
#endif

/* A 1.10 build runs the update's EBOOT (installed in RPCS3's or a PS3's
 * dev_hdd0\game\<title id>) on top of the disc's data. */
#if FOLKLORE_GAME_VERSION == 110
#define FOLKLORE_USES_UPDATE 1
#else
#define FOLKLORE_USES_UPDATE 0
#endif
