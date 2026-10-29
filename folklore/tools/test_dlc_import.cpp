/*
 * test_dlc_import.cpp -- run the DLC importer (src/engine/engine_dlc.cpp) off
 * Windows, against a copy of an RPCS3 dev_hdd0, to check it before a build.
 *
 *   g++ -std=c++20 -I src/engine -I ../ps3recomp/libs/filesystem \
 *       -DFOLKLORE_TITLE_ID='"BCUS98147"' tools/test_dlc_import.cpp \
 *       src/engine/engine_dlc.cpp ../ps3recomp/libs/filesystem/edat.c -lcrypto
 *   PS3_HDD0_ROOT=<empty folder> ./a.out <RPCS3 folder or dev_hdd0\game\<id>>
 *
 * On Windows the importer uses BCrypt for its one AES block; here OpenSSL
 * stands in.
 */
#include "engine_dlc.h"

#include <openssl/evp.h>

#include <cstdio>

extern "C" bool dlcAesDecryptBlock(const unsigned char key[16], const unsigned char input[16],
                                   unsigned char output[16])
{
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    int length = 0;
    int finalLength = 0;
    bool decrypted = EVP_DecryptInit_ex(context, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1;
    EVP_CIPHER_CTX_set_padding(context, 0);
    decrypted = decrypted && EVP_DecryptUpdate(context, output, &length, input, 16) == 1 &&
                EVP_DecryptFinal_ex(context, output + length, &finalLength) == 1;
    EVP_CIPHER_CTX_free(context);
    return decrypted && length + finalLength == 16;
}

int main(int argumentCount, char** arguments)
{
    if (argumentCount < 2)
    {
        std::fprintf(stderr, "usage: %s <folder to import from>\n", arguments[0]);
        return 2;
    }
    DlcSource source;
    std::string problem;
    if (!engineDlcScan(arguments[1], &source, &problem))
    {
        std::printf("scan: %s\n", problem.c_str());
        return 1;
    }
    std::printf("game folder: %s\n", source.gameFolder.c_str());
    for (const std::string& folder : source.licenceFolders)
    {
        std::printf("licences: %s\n", folder.c_str());
    }
    for (const DlcPackInfo& pack : source.packs)
    {
        std::printf("pack %d: %s, %d files, content id '%s', licence %s\n", pack.pack, engineDlcPackName(pack).c_str(),
                    pack.fileCount, pack.contentId.c_str(), pack.licenceFound ? "found" : "missing");
    }
    int lastPercent = -1;
    const std::string report = engineDlcInstall(source, [&](int done, int total, const std::string&) {
        const int percent = total ? done * 100 / total : 100;
        if (percent / 25 != lastPercent / 25)
        {
            std::printf("progress %d%%\n", percent);
            lastPercent = percent;
        }
    });
    std::printf("report:\n%s\n", report.c_str());
    std::printf("installed packs:");
    for (const int pack : engineDlcInstalledPacks())
    {
        std::printf(" %d (%s)", pack, engineDlcPackNameByNumber(pack).c_str());
    }
    std::printf("\n");
    return 0;
}
