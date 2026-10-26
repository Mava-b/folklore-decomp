/*
 * folklore_hle.cpp -- HLE handlers for the Folklore (BCUS98147) firmware
 * imports that ps3recomp does not cover yet.
 *
 * Found by cross-checking analysis/EBOOT.imports.json (167 imports) against
 * the runtime's generated NID table and its ctx-handler registrations: these
 * 28 had no handler. Without one, ps3_hle_call fakes CELL_OK and reports the
 * NID once, which is the wrong answer for several of them (online calls should
 * fail cleanly offline, callbacks the title waits on must actually fire).
 *
 * Behaviour mirrors what RPCS3 does for each call (Emu/Cell/Modules), since
 * that is what the title is known to be happy with -- re-implemented here,
 * not copied. Registered as ctx handlers so they are dispatched ahead of the
 * generic table and can read every argument register directly.
 */
#include "ppu_recomp.h"
#include "engine_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" int32_t cellSysutilQueueGuestCallback(uint32_t opd, uint64_t arg0, uint64_t arg1);
extern "C" int32_t cellRtcFormatRfc3339(char* dateTimeOut, const void* tick, int32_t timeZoneMinutes);
extern "C" int32_t cellPadSetSensorMode(uint32_t portNumber, uint32_t mode);
extern "C" int32_t cellPadInfoSensorMode(uint32_t portNumber);
extern "C" int32_t cellSysutilGetSystemParamInt(int32_t id, uint32_t valueAddress);
extern "C" int folklore_system_language(void);   /* src/engine/engine_settings.cpp */

namespace
{
    constexpr uint64_t kCellOk = 0;
    constexpr uint64_t kGuestMinusOne = 0xFFFFFFFFFFFFFFFFull;
    constexpr uint32_t kMsgDialogButtonOk = 1;                 /* CELL_MSGDIALOG_BUTTON_OK */
    constexpr uint32_t kWebBrowserInitializingFinished = 1;    /* CELL_SYSUTIL_WEBBROWSER_INITIALIZING_FINISHED */
    constexpr uint32_t kNpErrorNotInitialized = 0x8002AA01u;   /* SCE_NP_ERROR_NOT_INITIALIZED */
    constexpr uint32_t kNpCommunityNotInitialized = 0x8002A102u; /* SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED */

    void logOnce(const char* name)
    {
        /* One line per distinct import, like the runtime's own unresolved report. */
        static const char* seenNames[64];
        static int seenCount = 0;
        for (int index = 0; index < seenCount; index++)
        {
            if (seenNames[index] == name)
            {
                return;
            }
        }
        if (seenCount < 64)
        {
            seenNames[seenCount++] = name;
        }
        std::fprintf(stderr, "[folklore-hle] %s\n", name);
    }

    void returnValue(ppu_context* context, uint64_t value)
    {
        context->gpr[3] = value;
    }

    /* ---- sys_io: SIXAXIS sensor mode ------------------------------------
     * Folklore turns sensor mode on to read the "pull" gesture used to
     * capture Folks. This used to accept the mode without recording it, so
     * cellPadGetData left the four sensor words 0 -- full scale on every
     * axis, which the game took as a completed gesture: every capture
     * finished on its own. The runtime now records the mode and fills the
     * words from the DualShock 3 (or the engine menu's motion setting). */
    void hlePadSetSensorMode(ppu_context* context)
    {
        logOnce("cellPadSetSensorMode");
        const uint32_t portNumber = (uint32_t)context->gpr[3];
        const uint32_t mode = (uint32_t)context->gpr[4];
        returnValue(context, (uint32_t)cellPadSetSensorMode(portNumber, mode));
    }

    void hlePadInfoSensorMode(ppu_context* context)
    {
        logOnce("cellPadInfoSensorMode");
        const uint32_t portNumber = (uint32_t)context->gpr[3];
        returnValue(context, (uint32_t)cellPadInfoSensorMode(portNumber));
    }

    /* ---- cellSysutil: system settings ------------------------------------
     * cellSysutilGetSystemParamInt(id, value*): the runtime answers every
     * setting, and always English for the language. The language comes
     * from the engine menu instead (the European release has five). */
    constexpr int32_t kSystemParamLanguage = 0x0111;   /* CELL_SYSUTIL_SYSTEMPARAM_ID_LANG */

    void hleGetSystemParamInt(ppu_context* context)
    {
        const int32_t paramId = (int32_t)context->gpr[3];
        const uint32_t valueAddress = (uint32_t)context->gpr[4];
        if (paramId == kSystemParamLanguage && valueAddress != 0)
        {
            vm_write32(valueAddress, (uint32_t)folklore_system_language());
            returnValue(context, kCellOk);
            return;
        }
        returnValue(context, (uint32_t)cellSysutilGetSystemParamInt(paramId, valueAddress));
    }

    /* ---- cellSysutil: dialogs, storage, web browser --------------------- */
    void hleMsgDialogOpenErrorCode(ppu_context* context)
    {
        uint32_t errorCode = (uint32_t)context->gpr[3];
        uint32_t callbackOpd = (uint32_t)context->gpr[4];
        uint32_t userData = (uint32_t)context->gpr[5];
        std::fprintf(stderr, "[folklore-hle] cellMsgDialogOpenErrorCode(0x%08X) -> auto OK\n", errorCode);
        if (callbackOpd != 0)
        {
            cellSysutilQueueGuestCallback(callbackOpd, kMsgDialogButtonOk, userData);
        }
        returnValue(context, kCellOk);
    }

    /* cellStorageDataImport/Export(version, src, dst, param, funcFinish, container, userdata):
     * no removable media here, so report the copy finished. */
    void hleStorageDataTransfer(ppu_context* context)
    {
        logOnce("cellStorageDataImport/Export");
        uint32_t finishOpd = (uint32_t)context->gpr[7];
        uint32_t userData = (uint32_t)context->gpr[9];
        if (finishOpd != 0)
        {
            cellSysutilQueueGuestCallback(finishOpd, kCellOk, userData);
        }
        returnValue(context, kCellOk);
    }

    void hleWebBrowserInitialize(ppu_context* context)
    {
        logOnce("cellWebBrowserInitialize");
        uint32_t systemCallbackOpd = (uint32_t)context->gpr[3];
        if (systemCallbackOpd != 0)
        {
            cellSysutilQueueGuestCallback(systemCallbackOpd, kWebBrowserInitializingFinished, 0);
        }
        returnValue(context, kCellOk);
    }

    void hleWebBrowserNoop(ppu_context* context)
    {
        logOnce("cellWebBrowser* (no browser in this port)");
        returnValue(context, kCellOk);
    }

    /* ---- cellGcmSys ------------------------------------------------------ */
    void hleGcmFunc15(ppu_context* context)
    {
        (void)context;   /* void function; RPCS3 also treats it as a no-op */
        logOnce("_cellGcmFunc15");
    }

    /* ---- cellL10n -------------------------------------------------------- */

    /* UTF8toUTF16(src, dst, dstLen): converts ONE code point, returns the
     * number of source bytes consumed (0 on a malformed sequence). */
    void hleUtf8ToUtf16(ppu_context* context)
    {
        uint32_t sourceAddress = (uint32_t)context->gpr[3];
        uint32_t destAddress = (uint32_t)context->gpr[4];
        uint32_t destLengthAddress = (uint32_t)context->gpr[5];

        uint8_t leadByte = vm_read8(sourceAddress);
        uint32_t codePoint = 0;
        uint32_t byteCount = 0;
        if (leadByte < 0x80)
        {
            codePoint = leadByte;
            byteCount = 1;
        }
        else if ((leadByte & 0xE0) == 0xC0)
        {
            codePoint = leadByte & 0x1F;
            byteCount = 2;
        }
        else if ((leadByte & 0xF0) == 0xE0)
        {
            codePoint = leadByte & 0x0F;
            byteCount = 3;
        }
        else if ((leadByte & 0xF8) == 0xF0)
        {
            codePoint = leadByte & 0x07;
            byteCount = 4;
        }
        else
        {
            returnValue(context, 0);
            return;
        }

        for (uint32_t index = 1; index < byteCount; index++)
        {
            uint8_t continuation = vm_read8(sourceAddress + index);
            if ((continuation & 0xC0) != 0x80)
            {
                returnValue(context, 0);
                return;
            }
            codePoint = (codePoint << 6) | (continuation & 0x3F);
        }

        if (codePoint >= 0x10000)
        {
            uint32_t offsetPoint = codePoint - 0x10000;
            vm_write16(destAddress, (uint16_t)(0xD800 | (offsetPoint >> 10)));
            vm_write16(destAddress + 2, (uint16_t)(0xDC00 | (offsetPoint & 0x3FF)));
            vm_write32(destLengthAddress, 2);
        }
        else
        {
            vm_write16(destAddress, (uint16_t)codePoint);
            vm_write32(destLengthAddress, 1);
        }
        returnValue(context, byteCount);
    }

    /* SJISstoUTF8s(src, srcLen*, dst, dstLen*): the US build's strings are
     * ASCII, which Shift-JIS and UTF-8 share. Pass ASCII through and mark
     * anything else, so a real Japanese string shows up in the log. */
    void hleSjisStringToUtf8(ppu_context* context)
    {
        uint32_t sourceAddress = (uint32_t)context->gpr[3];
        uint32_t sourceLengthAddress = (uint32_t)context->gpr[4];
        uint32_t destAddress = (uint32_t)context->gpr[5];
        uint32_t destLengthAddress = (uint32_t)context->gpr[6];

        uint32_t sourceLength = vm_read32(sourceLengthAddress);
        uint32_t destCapacity = vm_read32(destLengthAddress);
        uint32_t written = 0;
        bool sawNonAscii = false;
        for (uint32_t index = 0; index < sourceLength && written < destCapacity; index++)
        {
            uint8_t sourceByte = vm_read8(sourceAddress + index);
            if (sourceByte >= 0x80)
            {
                sawNonAscii = true;
                sourceByte = '?';
            }
            vm_write8(destAddress + written, sourceByte);
            written++;
        }
        vm_write32(destLengthAddress, written);
        if (sawNonAscii)
        {
            std::fprintf(stderr, "[folklore-hle] SJISstoUTF8s: non-ASCII Shift-JIS replaced with '?'\n");
        }
        returnValue(context, 0);   /* ConversionOK */
    }

    void hleL10nTodo(ppu_context* context)
    {
        logOnce("SJIStoUTF8 / UTF8stoSJISs (RPCS3 also returns 0)");
        returnValue(context, 0);
    }

    /* ---- sceNp: offline -------------------------------------------------- */
    void hleNpNetworkTime(ppu_context* context)
    {
        logOnce("sceNpManagerGetNetworkTime -> NOT_INITIALIZED");
        returnValue(context, kNpErrorNotInitialized);
    }

    void hleNpScoreGameData(ppu_context* context)
    {
        logOnce("sceNpScore*GameDataAsync -> NOT_INITIALIZED");
        returnValue(context, kNpCommunityNotInitialized);
    }

    /* ---- cellRtc --------------------------------------------------------- */
    void hleRtcFormatRfc3339LocalTime(ppu_context* context)
    {
        std::time_t nowTime = std::time(nullptr);
        std::tm localParts{};
        std::tm utcParts{};
#ifdef _WIN32
        localtime_s(&localParts, &nowTime);
        gmtime_s(&utcParts, &nowTime);
#else
        localtime_r(&nowTime, &localParts);
        gmtime_r(&nowTime, &utcParts);
#endif
        utcParts.tm_isdst = localParts.tm_isdst;
        int32_t offsetMinutes = (int32_t)(std::difftime(std::mktime(&localParts), std::mktime(&utcParts)) / 60.0);

        /* The generic HLE ABI hands guest addresses straight through, so these
         * are guest pointers, as cellRtcFormatRfc3339 expects. */
        char* dateTimeOut = (char*)(uintptr_t)(uint32_t)context->gpr[3];
        const void* utcTick = (const void*)(uintptr_t)(uint32_t)context->gpr[4];
        returnValue(context, (uint64_t)(int64_t)cellRtcFormatRfc3339(dateTimeOut, utcTick, offsetMinutes));
    }

    /* ---- sys_net: no online play ------------------------------------------
     * socket() itself is handled by the runtime; refusing to connect keeps the
     * title offline instead of letting it believe a fake connection worked. */
    void hleNetRefuse(ppu_context* context)
    {
        logOnce("sys_net connect/listen/accept/send -> -1 (offline)");
        returnValue(context, kGuestMinusOne);
    }

    void hleNetShowIfconfig(ppu_context* context)
    {
        returnValue(context, kCellOk);
    }

    /* inet_addr("a.b.c.d") -> address in network (big-endian) order, which on
     * the PS3 is simply (a << 24) | (b << 16) | (c << 8) | d. */
    void hleNetInetAddr(ppu_context* context)
    {
        uint32_t textAddress = (uint32_t)context->gpr[3];
        uint32_t address = 0;
        uint32_t octet = 0;
        int dotCount = 0;
        bool haveDigit = false;
        for (uint32_t index = 0; index < 16; index++)
        {
            char character = (char)vm_read8(textAddress + index);
            if (character >= '0' && character <= '9')
            {
                octet = octet * 10 + (uint32_t)(character - '0');
                haveDigit = true;
                if (octet > 255)
                {
                    returnValue(context, 0xFFFFFFFFu);   /* INADDR_NONE */
                    return;
                }
            }
            else if (character == '.' || character == '\0')
            {
                if (!haveDigit)
                {
                    returnValue(context, 0xFFFFFFFFu);
                    return;
                }
                address = (address << 8) | octet;
                octet = 0;
                haveDigit = false;
                if (character == '\0')
                {
                    break;
                }
                dotCount++;
            }
            else
            {
                returnValue(context, 0xFFFFFFFFu);
                return;
            }
        }
        returnValue(context, dotCount == 3 ? address : 0xFFFFFFFFu);
    }

    struct HandlerEntry
    {
        uint32_t nid;
        const char* name;
        void (*handler)(ppu_context*);
    };

    const HandlerEntry kHandlers[] = {
        { 0xBE5BE3BAu, "cellPadSetSensorMode",               hlePadSetSensorMode },
        { 0x78200559u, "cellPadInfoSensorMode",              hlePadInfoSensorMode },
        { 0x40E895D3u, "cellSysutilGetSystemParamInt",       hleGetSystemParamInt },
        { 0x3E22CB4Bu, "cellMsgDialogOpenErrorCode",         hleMsgDialogOpenErrorCode },
        { 0x918288FBu, "cellStorageDataImport",              hleStorageDataTransfer },
        { 0xABC3CD2Cu, "cellStorageDataExport",              hleStorageDataTransfer },
        { 0x749C9B5Fu, "cellWebBrowserInitialize",           hleWebBrowserInitialize },
        { 0x58C89C4Au, "cellWebBrowserEstimate",             hleWebBrowserNoop },
        { 0x6C1082AAu, "cellWebBrowserConfigWithVer",        hleWebBrowserNoop },
        { 0x3261A9C8u, "cellWebBrowserConfigSetDisableTabs", hleWebBrowserNoop },
        { 0x76FC8FB1u, "cellWebBrowserConfigSetHeapSize",    hleWebBrowserNoop },
        { 0x9C9FE6A4u, "cellWebBrowserConfigSetFunction",    hleWebBrowserNoop },
        { 0x8A4CB646u, "cellWebBrowserCreateWithConfig",     hleWebBrowserNoop },
        { 0x93CED48Du, "cellWebBrowserShutdown",             hleWebBrowserNoop },
        { 0x3A33C1FDu, "_cellGcmFunc15",                     hleGcmFunc15 },
        { 0x61FB9442u, "UTF8toUTF16",                        hleUtf8ToUtf16 },
        { 0x33435818u, "SJISstoUTF8s",                       hleSjisStringToUtf8 },
        { 0x6122E000u, "SJIStoUTF8",                         hleL10nTodo },
        { 0xDD5EBDEBu, "UTF8stoSJISs",                       hleL10nTodo },
        { 0xBDC07FD5u, "sceNpManagerGetNetworkTime",         hleNpNetworkTime },
        { 0xDB2E4DC2u, "sceNpScoreGetGameDataAsync",         hleNpScoreGameData },
        { 0xF76847C2u, "sceNpScoreRecordGameDataAsync",      hleNpScoreGameData },
        { 0x1324948Au, "cellRtcFormatRfc3339LocalTime",      hleRtcFormatRfc3339LocalTime },
        { 0x64F66D35u, "connect",                            hleNetRefuse },
        { 0x28E208BBu, "listen",                             hleNetRefuse },
        { 0xC94F6939u, "accept",                             hleNetRefuse },
        { 0xDC751B40u, "send",                               hleNetRefuse },
        { 0xDABBC2C0u, "inet_addr",                          hleNetInetAddr },
        { 0xB48636C4u, "sys_net_show_ifconfig",              hleNetShowIfconfig },
    };

    struct Registrar
    {
        Registrar()
        {
            for (const HandlerEntry& entry : kHandlers)
            {
                ps3_hle_register_ctx(entry.nid, entry.name, entry.handler);
            }
        }
    };

    Registrar gRegistrar;
}

/* Ultrawide: Folklore builds its perspective projections from a 16:9 aspect
 * constant held in seven TOC pool slots, one per module that builds a camera
 * (m[0][0] = cot(fov/2) / aspect, func_0005CFFC, func_00145B20,
 * func_0015E5C4's callers, func_0015F158, func_001B1C60, func_00230CF8,
 * func_00269xxx...). Setting them to the target aspect widens the horizontal
 * field of view (Hor+); the renderer stretches the squeezed frame back out
 * (rsx_live_draw_set_aspect). Each slot is only changed if it holds 16:9 or
 * a value this function wrote before. */
extern "C" void folklore_apply_aspect(float aspect)
{
    /* TOC-relative; the EU executable's TOC is laid out a few entries
     * differently (folklore/docs/REGIONS.md). */
#if defined(FOLKLORE_REGION_EU)
    static const int32_t kAspectSlots[] = { -28188, -16232, -13544, -13372, -6692, 284, 4328 };
#else
    static const int32_t kAspectSlots[] = { -28188, -16228, -13536, -13364, -6676, 316, 4360 };
#endif
    const uint32_t tocBase = FOLKLORE_TOC_BASE;
    const float original = 16.0f / 9.0f;
    static float applied = 0.0f;
    if (aspect < original)
    {
        aspect = original;
    }
    uint32_t aspectBits = 0;
    std::memcpy(&aspectBits, &aspect, sizeof aspectBits);
    uint32_t originalBits = 0;
    std::memcpy(&originalBits, &original, sizeof originalBits);
    uint32_t appliedBits = 0;
    std::memcpy(&appliedBits, &applied, sizeof appliedBits);
    int patched = 0;
    for (const int32_t slotOffset : kAspectSlots)
    {
        const uint32_t slotAddress = (uint32_t)((int64_t)tocBase + slotOffset);
        const uint32_t current = vm_read32(slotAddress);
        if (current == originalBits || current == 0x3FE38E38u || (applied > 0.0f && current == appliedBits))
        {
            vm_write32(slotAddress, aspectBits);
            patched++;
        }
    }
    applied = aspect;
    std::fprintf(stderr, "[folklore] projection aspect %.3f (%d of %d camera constants)\n", aspect, patched,
                 (int)(sizeof(kAspectSlots) / sizeof(kAspectSlots[0])));
}
