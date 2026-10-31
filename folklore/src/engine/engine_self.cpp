/*
 * engine_self.cpp -- decrypt a PS3 SELF (EBOOT.BIN) to its ELF. See
 * engine_self.h and folklore/docs/SELF.md.
 *
 * The layout, in order of use:
 *   - SCE header (0x20 bytes): magic, version, key revision, header type,
 *     metadata offset, header length, data length.
 *   - Extended header (0x50): offsets of the application info, the ELF
 *     header, the program headers, the section headers, segment info,
 *     version info and control info (with its size).
 *   - Application info: the program type (4 = application, 8 = NPDRM).
 *   - Control info: for NPDRM, an NPD block with the licence type and the
 *     content ID.
 *   - Metadata info (0x40 at 0x20 + metadata offset): the key and IV of the
 *     metadata headers. AES-256-CBC with the program type's key for this key
 *     revision; for NPDRM first AES-128-CBC (zero IV) with the klicensee
 *     decrypted by NP_KLIC_KEY -- the free klicensee, or the RIF key of the
 *     content's .rap.
 *   - Metadata headers (to the header length): AES-128-CTR. A 0x20 header
 *     (section and key counts), 0x30 per section (data offset and size,
 *     type, program index, encrypted, key and IV index, compressed), then
 *     the keys, 0x10 each.
 *   - Sections: AES-128-CTR with their key and IV when encrypted (3); zlib
 *     when compressed (2). Program sections (type 2) go to their program
 *     header's file offset.
 * The ELF is written as the ELF header, the program headers, every program
 * section at its offset, and the section headers at e_shoff.
 */
#include "engine_self.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

/* zlib inflate: stb_image's, compiled into the ps3recomp runtime
 * (cellPngDec.c) and into tools/unself_main.cpp. */
extern "C" int stbi_zlib_decode_buffer(char* outputBuffer, int outputLength, const char* inputBuffer,
                                       int inputLength);

namespace
{
    /* ---- AES (FIPS-197), 128- and 256-bit keys ------------------------- */

    const uint8_t kSbox[256] = {
        0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
        0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
        0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
        0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
        0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
        0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
        0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
        0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
        0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
        0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
        0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
        0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
        0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
        0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
        0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
        0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
    };

    struct AesKey
    {
        uint8_t roundKeys[240];
        int rounds = 0;
    };

    uint8_t aesTimes2(uint8_t value)
    {
        return (uint8_t)((value << 1) ^ ((value & 0x80) ? 0x1B : 0x00));
    }

    uint8_t aesMultiply(uint8_t left, uint8_t right)
    {
        uint8_t product = 0;
        while (right)
        {
            if (right & 1)
            {
                product ^= left;
            }
            left = aesTimes2(left);
            right >>= 1;
        }
        return product;
    }

    const uint8_t* aesInverseSbox()
    {
        static uint8_t inverse[256];
        static bool built = false;
        if (!built)
        {
            for (int index = 0; index < 256; index++)
            {
                inverse[kSbox[index]] = (uint8_t)index;
            }
            built = true;
        }
        return inverse;
    }

    void aesExpandKey(const uint8_t* key, int keyBits, AesKey* expanded)
    {
        const int keyWords = keyBits / 32;
        expanded->rounds = keyWords + 6;
        const int totalWords = 4 * (expanded->rounds + 1);
        std::memcpy(expanded->roundKeys, key, (size_t)keyWords * 4);
        uint8_t roundConstant = 1;
        for (int wordIndex = keyWords; wordIndex < totalWords; wordIndex++)
        {
            uint8_t word[4];
            std::memcpy(word, expanded->roundKeys + (wordIndex - 1) * 4, 4);
            if (wordIndex % keyWords == 0)
            {
                const uint8_t first = word[0];
                word[0] = (uint8_t)(kSbox[word[1]] ^ roundConstant);
                word[1] = kSbox[word[2]];
                word[2] = kSbox[word[3]];
                word[3] = kSbox[first];
                roundConstant = aesTimes2(roundConstant);
            }
            else if (keyWords > 6 && wordIndex % keyWords == 4)
            {
                for (int byteIndex = 0; byteIndex < 4; byteIndex++)
                {
                    word[byteIndex] = kSbox[word[byteIndex]];
                }
            }
            for (int byteIndex = 0; byteIndex < 4; byteIndex++)
            {
                expanded->roundKeys[wordIndex * 4 + byteIndex] =
                    (uint8_t)(expanded->roundKeys[(wordIndex - keyWords) * 4 + byteIndex] ^ word[byteIndex]);
            }
        }
    }

    void aesEncryptBlock(const AesKey& key, const uint8_t input[16], uint8_t output[16])
    {
        uint8_t state[16];
        for (int index = 0; index < 16; index++)
        {
            state[index] = (uint8_t)(input[index] ^ key.roundKeys[index]);
        }
        for (int round = 1; round <= key.rounds; round++)
        {
            uint8_t shifted[16];
            for (int column = 0; column < 4; column++)
            {
                for (int row = 0; row < 4; row++)
                {
                    shifted[column * 4 + row] = kSbox[state[((column + row) % 4) * 4 + row]];
                }
            }
            if (round != key.rounds)
            {
                for (int column = 0; column < 4; column++)
                {
                    uint8_t* cell = shifted + column * 4;
                    const uint8_t all = (uint8_t)(cell[0] ^ cell[1] ^ cell[2] ^ cell[3]);
                    const uint8_t first = cell[0];
                    cell[0] ^= (uint8_t)(all ^ aesTimes2((uint8_t)(cell[0] ^ cell[1])));
                    cell[1] ^= (uint8_t)(all ^ aesTimes2((uint8_t)(cell[1] ^ cell[2])));
                    cell[2] ^= (uint8_t)(all ^ aesTimes2((uint8_t)(cell[2] ^ cell[3])));
                    cell[3] ^= (uint8_t)(all ^ aesTimes2((uint8_t)(cell[3] ^ first)));
                }
            }
            for (int index = 0; index < 16; index++)
            {
                state[index] = (uint8_t)(shifted[index] ^ key.roundKeys[round * 16 + index]);
            }
        }
        std::memcpy(output, state, 16);
    }

    void aesDecryptBlock(const AesKey& key, const uint8_t input[16], uint8_t output[16])
    {
        const uint8_t* inverseSbox = aesInverseSbox();
        uint8_t state[16];
        for (int index = 0; index < 16; index++)
        {
            state[index] = (uint8_t)(input[index] ^ key.roundKeys[key.rounds * 16 + index]);
        }
        for (int round = key.rounds - 1; round >= 0; round--)
        {
            uint8_t shifted[16];
            for (int column = 0; column < 4; column++)
            {
                for (int row = 0; row < 4; row++)
                {
                    shifted[((column + row) % 4) * 4 + row] = inverseSbox[state[column * 4 + row]];
                }
            }
            for (int index = 0; index < 16; index++)
            {
                shifted[index] ^= key.roundKeys[round * 16 + index];
            }
            if (round != 0)
            {
                for (int column = 0; column < 4; column++)
                {
                    const uint8_t* cell = shifted + column * 4;
                    uint8_t mixed[4];
                    mixed[0] = (uint8_t)(aesMultiply(cell[0], 14) ^ aesMultiply(cell[1], 11) ^
                                         aesMultiply(cell[2], 13) ^ aesMultiply(cell[3], 9));
                    mixed[1] = (uint8_t)(aesMultiply(cell[0], 9) ^ aesMultiply(cell[1], 14) ^
                                         aesMultiply(cell[2], 11) ^ aesMultiply(cell[3], 13));
                    mixed[2] = (uint8_t)(aesMultiply(cell[0], 13) ^ aesMultiply(cell[1], 9) ^
                                         aesMultiply(cell[2], 14) ^ aesMultiply(cell[3], 11));
                    mixed[3] = (uint8_t)(aesMultiply(cell[0], 11) ^ aesMultiply(cell[1], 13) ^
                                         aesMultiply(cell[2], 9) ^ aesMultiply(cell[3], 14));
                    std::memcpy(shifted + column * 4, mixed, 4);
                }
            }
            std::memcpy(state, shifted, 16);
        }
        std::memcpy(output, state, 16);
    }

    void aesCbcDecrypt(const uint8_t* key, int keyBits, const uint8_t iv[16], uint8_t* data, size_t length)
    {
        AesKey expanded;
        aesExpandKey(key, keyBits, &expanded);
        uint8_t previous[16];
        std::memcpy(previous, iv, 16);
        for (size_t offset = 0; offset + 16 <= length; offset += 16)
        {
            uint8_t cipherBlock[16];
            std::memcpy(cipherBlock, data + offset, 16);
            aesDecryptBlock(expanded, cipherBlock, data + offset);
            for (int index = 0; index < 16; index++)
            {
                data[offset + index] ^= previous[index];
            }
            std::memcpy(previous, cipherBlock, 16);
        }
    }

    void aesCtrApply(const uint8_t key[16], const uint8_t iv[16], uint8_t* data, size_t length)
    {
        AesKey expanded;
        aesExpandKey(key, 128, &expanded);
        uint8_t counter[16];
        std::memcpy(counter, iv, 16);
        for (size_t offset = 0; offset < length; offset += 16)
        {
            uint8_t stream[16];
            aesEncryptBlock(expanded, counter, stream);
            const size_t blockLength = length - offset < 16 ? length - offset : 16;
            for (size_t index = 0; index < blockLength; index++)
            {
                data[offset + index] ^= stream[index];
            }
            for (int index = 15; index >= 0; index--)
            {
                counter[index]++;
                if (counter[index] != 0)
                {
                    break;
                }
            }
        }
    }

    /* ---- Keys -------------------------------------------------------------
     * The published key-revision-1 keys for applications and NPDRM programs,
     * the NPDRM klicensee constants and the RAP conversion constants -- the
     * values RPCS3 (Crypto/key_vault), scetool and psdevwiki give. */
    const uint8_t kApplicationKey[32] = {
        0x79, 0x48, 0x18, 0x39, 0xC4, 0x06, 0xA6, 0x32, 0xBD, 0xB4, 0xAC, 0x09, 0x3D, 0x73, 0xD9, 0x9A,
        0xE1, 0x58, 0x7F, 0x24, 0xCE, 0x7E, 0x69, 0x19, 0x2C, 0x1C, 0xD0, 0x01, 0x02, 0x74, 0xA8, 0xAB,
    };
    const uint8_t kApplicationIv[16] = {
        0x6F, 0x0F, 0x25, 0xE1, 0xC8, 0xC4, 0xB7, 0xAE, 0x70, 0xDF, 0x96, 0x8B, 0x04, 0x52, 0x1D, 0xDA,
    };
    const uint8_t kNpdrmKey[32] = {
        0xF9, 0xED, 0xD0, 0x30, 0x1F, 0x77, 0x0F, 0xAB, 0xBA, 0x88, 0x63, 0xD9, 0x89, 0x7F, 0x0F, 0xEA,
        0x65, 0x51, 0xB0, 0x94, 0x31, 0xF6, 0x13, 0x12, 0x65, 0x4E, 0x28, 0xF4, 0x35, 0x33, 0xEA, 0x6B,
    };
    const uint8_t kNpdrmIv[16] = {
        0xA5, 0x51, 0xCC, 0xB4, 0xA4, 0x2C, 0x37, 0xA7, 0x34, 0xA2, 0xB4, 0xF9, 0x65, 0x7D, 0x55, 0x40,
    };
    const uint8_t kKlicenseeFree[16] = {
        0x72, 0xF9, 0x90, 0x78, 0x8F, 0x9C, 0xFF, 0x74, 0x57, 0x25, 0xF0, 0x8E, 0x4C, 0x12, 0x83, 0x87,
    };
    const uint8_t kKlicenseeKey[16] = {
        0xF2, 0xFB, 0xCA, 0x7A, 0x75, 0xB0, 0x4E, 0xDC, 0x13, 0x90, 0x63, 0x8C, 0xCD, 0xFD, 0xD1, 0xEE,
    };
    const uint8_t kRapKey[16] = {
        0x86, 0x9F, 0x77, 0x45, 0xC1, 0x3F, 0xD8, 0x90, 0xCC, 0xF2, 0x91, 0x88, 0xE3, 0xCC, 0x3E, 0xDF,
    };
    const uint8_t kRapPbox[16] = {
        0x0C, 0x03, 0x06, 0x04, 0x01, 0x0B, 0x0F, 0x08, 0x02, 0x07, 0x00, 0x05, 0x0A, 0x0E, 0x0D, 0x09,
    };
    const uint8_t kRapE1[16] = {
        0xA9, 0x3E, 0x1F, 0xD6, 0x7C, 0x55, 0xA3, 0x29, 0xB7, 0x5F, 0xDD, 0xA6, 0x2A, 0x95, 0xC7, 0xA5,
    };
    const uint8_t kRapE2[16] = {
        0x67, 0xD4, 0x5D, 0xA3, 0x29, 0x6D, 0x00, 0x6A, 0x4E, 0x7C, 0x53, 0x7B, 0xF5, 0x53, 0x8C, 0x74,
    };

    constexpr uint32_t kSelfMagic = 0x53434500;   /* "SCE\0" */
    constexpr uint32_t kProgramApplication = 4;
    constexpr uint32_t kProgramNpdrm = 8;
    constexpr uint32_t kControlNpd = 3;
    constexpr uint32_t kLicenceNetwork = 1;
    constexpr uint32_t kLicenceLocal = 2;
    constexpr uint32_t kLicenceFree = 3;
    constexpr uint32_t kSectionProgram = 2;
    constexpr uint32_t kSectionEncrypted = 3;
    constexpr uint32_t kSectionCompressed = 2;

    uint16_t readBig16(const uint8_t* bytes)
    {
        return (uint16_t)((bytes[0] << 8) | bytes[1]);
    }

    uint32_t readBig32(const uint8_t* bytes)
    {
        return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
    }

    uint64_t readBig64(const uint8_t* bytes)
    {
        return ((uint64_t)readBig32(bytes) << 32) | readBig32(bytes + 4);
    }

    /* A .rap licence to its RIF key (the klicensee of a local licence). */
    void rapToRifKey(const uint8_t rap[16], uint8_t rifKey[16])
    {
        AesKey expanded;
        aesExpandKey(kRapKey, 128, &expanded);
        uint8_t key[16];
        aesDecryptBlock(expanded, rap, key);
        for (int round = 0; round < 5; round++)
        {
            for (int index = 0; index < 16; index++)
            {
                const int position = kRapPbox[index];
                key[position] ^= kRapE1[position];
            }
            for (int index = 15; index >= 1; index--)
            {
                key[kRapPbox[index]] ^= key[kRapPbox[index - 1]];
            }
            int borrow = 0;
            for (int index = 0; index < 16; index++)
            {
                const int position = kRapPbox[index];
                const uint8_t current = (uint8_t)(key[position] - borrow);
                const uint8_t subtrahend = kRapE2[position];
                if (borrow != 1 || current != 0xFF)
                {
                    borrow = current < subtrahend ? 1 : 0;
                }
                key[position] = (uint8_t)(current - subtrahend);
            }
        }
        std::memcpy(rifKey, key, 16);
    }

    bool readWholeFile(const std::string& path, std::vector<unsigned char>* contents)
    {
        std::ifstream file(std::filesystem::path(path), std::ios::binary);
        if (!file)
        {
            return false;
        }
        contents->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    }

    bool findRap(const std::string& contentId, const std::vector<std::string>& licenceFolders, uint8_t rap[16])
    {
        for (const std::string& folder : licenceFolders)
        {
            std::vector<unsigned char> contents;
            if (readWholeFile(folder + "/" + contentId + ".rap", &contents) && contents.size() >= 16)
            {
                std::memcpy(rap, contents.data(), 16);
                return true;
            }
        }
        return false;
    }

    bool fail(std::string* problem, const std::string& sentence)
    {
        if (problem)
        {
            *problem = sentence;
        }
        return false;
    }
}

bool selfDecrypt(const std::vector<unsigned char>& self, std::vector<unsigned char>* elf,
                 const std::vector<std::string>& licenceFolders, std::string* problem)
{
    const uint8_t* bytes = self.data();
    const size_t size = self.size();
    if (size < 0x70 || readBig32(bytes) != kSelfMagic)
    {
        return fail(problem, "This is not an encrypted PS3 executable (no SELF header).");
    }
    const uint16_t keyRevision = readBig16(bytes + 0x08);
    const uint32_t metadataOffset = readBig32(bytes + 0x0C);
    const uint64_t headerLength = readBig64(bytes + 0x10);
    const uint64_t applicationInfoOffset = readBig64(bytes + 0x28);
    const uint64_t elfOffset = readBig64(bytes + 0x30);
    const uint64_t programHeadersOffset = readBig64(bytes + 0x38);
    const uint64_t sectionHeadersOffset = readBig64(bytes + 0x40);
    const uint64_t controlInfoOffset = readBig64(bytes + 0x58);
    const uint64_t controlInfoSize = readBig64(bytes + 0x60);
    if (headerLength > size || applicationInfoOffset + 0x20 > size || elfOffset + 0x40 > size ||
        (uint64_t)metadataOffset + 0x60 > headerLength || controlInfoOffset + controlInfoSize > size)
    {
        return fail(problem, "The executable's header is damaged.");
    }
    if (keyRevision != 1)
    {
        return fail(problem, "This executable uses key revision " + std::to_string(keyRevision) +
                                 "; only Folklore's own (revision 1) is supported.");
    }
    const uint32_t programType = readBig32(bytes + applicationInfoOffset + 0x0C);
    if (programType != kProgramApplication && programType != kProgramNpdrm)
    {
        return fail(problem, "This is not a game executable (program type " + std::to_string(programType) + ").");
    }

    /* Metadata info: the NPDRM layer first, then the program type's key. */
    uint8_t metadataInfo[0x40];
    std::memcpy(metadataInfo, bytes + 0x20 + metadataOffset, sizeof(metadataInfo));
    if (programType == kProgramNpdrm)
    {
        const uint8_t* npd = nullptr;
        uint64_t controlOffset = controlInfoOffset;
        while (controlOffset + 0x10 <= controlInfoOffset + controlInfoSize)
        {
            const uint32_t controlType = readBig32(bytes + controlOffset);
            const uint32_t controlSize = readBig32(bytes + controlOffset + 4);
            if (controlType == kControlNpd && controlOffset + 0x10 + 0x40 <= size)
            {
                npd = bytes + controlOffset + 0x10;
            }
            if (controlSize == 0)
            {
                break;
            }
            controlOffset += controlSize;
        }
        if (!npd)
        {
            return fail(problem, "The update's executable has no licence information.");
        }
        const uint32_t licenceType = readBig32(npd + 0x08);
        char contentIdText[0x31] = {};
        std::memcpy(contentIdText, npd + 0x10, 0x30);
        const std::string contentId = contentIdText;
        uint8_t klicensee[16];
        if (licenceType == kLicenceFree)
        {
            std::memcpy(klicensee, kKlicenseeFree, 16);
        }
        else if (licenceType == kLicenceLocal || licenceType == kLicenceNetwork)
        {
            uint8_t rap[16];
            if (!findRap(contentId, licenceFolders, rap))
            {
                return fail(problem, "The update's executable needs its licence, " + contentId +
                                         ".rap, which was not found. RPCS3 keeps it in dev_hdd0\\home\\<user>\\exdata "
                                         "(File > Install Packages/Raps/Edats installs it with the update).");
            }
            rapToRifKey(rap, klicensee);
        }
        else
        {
            return fail(problem, "The update's executable has an unknown licence type (" +
                                     std::to_string(licenceType) + ").");
        }
        AesKey klicenseeKey;
        aesExpandKey(kKlicenseeKey, 128, &klicenseeKey);
        uint8_t layerKey[16];
        aesDecryptBlock(klicenseeKey, klicensee, layerKey);
        const uint8_t zeroIv[16] = {};
        aesCbcDecrypt(layerKey, 128, zeroIv, metadataInfo, sizeof(metadataInfo));
    }
    const bool npdrm = programType == kProgramNpdrm;
    aesCbcDecrypt(npdrm ? kNpdrmKey : kApplicationKey, 256, npdrm ? kNpdrmIv : kApplicationIv, metadataInfo,
                  sizeof(metadataInfo));
    if (metadataInfo[0x10] != 0 || metadataInfo[0x30] != 0)
    {
        return fail(problem, npdrm ? "The update's executable did not decrypt: its licence (.rap) belongs to "
                                     "another account or another game."
                                   : "The executable did not decrypt (not a retail Folklore EBOOT.BIN?).");
    }

    /* Metadata headers. */
    const size_t headersStart = 0x20 + (size_t)metadataOffset + 0x40;
    std::vector<uint8_t> headers(bytes + headersStart, bytes + headerLength);
    aesCtrApply(metadataInfo, metadataInfo + 0x20, headers.data(), headers.size());
    if (headers.size() < 0x20)
    {
        return fail(problem, "The executable's metadata is damaged.");
    }
    const uint32_t sectionCount = readBig32(headers.data() + 0x0C);
    const uint32_t keyCount = readBig32(headers.data() + 0x10);
    const size_t keysStart = 0x20 + (size_t)sectionCount * 0x30;
    if (keysStart + (size_t)keyCount * 0x10 > headers.size())
    {
        return fail(problem, "The executable's metadata is damaged.");
    }

    /* The ELF header and program headers, as the SELF carries them. */
    const uint8_t* elfHeader = bytes + elfOffset;
    const uint64_t programHeaderTable = readBig64(elfHeader + 0x20);
    const uint64_t sectionHeaderTable = readBig64(elfHeader + 0x28);
    const uint16_t programHeaderCount = readBig16(elfHeader + 0x38);
    const uint16_t sectionHeaderCount = readBig16(elfHeader + 0x3C);
    if (programHeadersOffset + (uint64_t)programHeaderCount * 0x38 > size ||
        (sectionHeaderTable && sectionHeadersOffset + (uint64_t)sectionHeaderCount * 0x40 > size))
    {
        return fail(problem, "The executable's header is damaged.");
    }
    elf->clear();
    auto place = [elf](uint64_t offset, const uint8_t* data, size_t length) {
        if (elf->size() < offset + length)
        {
            elf->resize((size_t)(offset + length), 0);
        }
        std::memcpy(elf->data() + offset, data, length);
    };
    place(0, elfHeader, 0x40);
    place(programHeaderTable, bytes + programHeadersOffset, (size_t)programHeaderCount * 0x38);

    for (uint32_t sectionIndex = 0; sectionIndex < sectionCount; sectionIndex++)
    {
        const uint8_t* section = headers.data() + 0x20 + (size_t)sectionIndex * 0x30;
        const uint64_t dataOffset = readBig64(section + 0x00);
        const uint64_t dataSize = readBig64(section + 0x08);
        const uint32_t sectionType = readBig32(section + 0x10);
        const uint32_t programIndex = readBig32(section + 0x14);
        const uint32_t encrypted = readBig32(section + 0x20);
        const uint32_t keyIndex = readBig32(section + 0x24);
        const uint32_t ivIndex = readBig32(section + 0x28);
        const uint32_t compressed = readBig32(section + 0x2C);
        if (sectionType != kSectionProgram)
        {
            continue;
        }
        if (dataOffset + dataSize > size || programIndex >= programHeaderCount)
        {
            return fail(problem, "The executable's data is damaged (a section lies outside the file).");
        }
        std::vector<uint8_t> data(bytes + dataOffset, bytes + dataOffset + dataSize);
        if (encrypted == kSectionEncrypted)
        {
            if (keyIndex >= keyCount || ivIndex >= keyCount)
            {
                return fail(problem, "The executable's metadata is damaged.");
            }
            aesCtrApply(headers.data() + keysStart + (size_t)keyIndex * 0x10,
                        headers.data() + keysStart + (size_t)ivIndex * 0x10, data.data(), data.size());
        }
        const uint8_t* programHeader = bytes + programHeadersOffset + (size_t)programIndex * 0x38;
        const uint64_t fileOffset = readBig64(programHeader + 0x08);
        const uint64_t fileSize = readBig64(programHeader + 0x20);
        if (compressed == kSectionCompressed)
        {
            std::vector<uint8_t> inflated((size_t)fileSize);
            const int inflatedLength =
                stbi_zlib_decode_buffer(reinterpret_cast<char*>(inflated.data()), (int)inflated.size(),
                                        reinterpret_cast<const char*>(data.data()), (int)data.size());
            if (inflatedLength < 0 || (uint64_t)inflatedLength != fileSize)
            {
                return fail(problem, "The executable's data did not decrypt (a segment did not decompress).");
            }
            data.swap(inflated);
        }
        place(fileOffset, data.data(), data.size());
    }
    if (sectionHeaderTable)
    {
        place(sectionHeaderTable, bytes + sectionHeadersOffset, (size_t)sectionHeaderCount * 0x40);
    }
    return true;
}

bool selfDecryptFile(const std::string& selfPath, const std::string& elfPath,
                     const std::vector<std::string>& licenceFolders, std::string* problem)
{
    std::vector<unsigned char> self;
    if (!readWholeFile(selfPath, &self))
    {
        return fail(problem, "Could not read " + selfPath + ".");
    }
    std::vector<unsigned char> elf;
    if (!selfDecrypt(self, &elf, licenceFolders, problem))
    {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(elfPath).parent_path(), error);
    const std::string temporaryPath = elfPath + ".part";
    {
        std::ofstream file(std::filesystem::path(temporaryPath), std::ios::binary | std::ios::trunc);
        if (!file || !file.write(reinterpret_cast<const char*>(elf.data()), (std::streamsize)elf.size()))
        {
            return fail(problem, "Could not write " + elfPath + ".");
        }
    }
    std::filesystem::rename(std::filesystem::path(temporaryPath), std::filesystem::path(elfPath), error);
    if (error)
    {
        return fail(problem, "Could not write " + elfPath + ".");
    }
    return true;
}

bool selfIsSelfFile(const std::string& path)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    unsigned char magic[4] = {};
    return file.read(reinterpret_cast<char*>(magic), 4) && readBig32(magic) == kSelfMagic;
}
