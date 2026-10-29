/*
 * engine_dlc.cpp -- importing Folklore's downloadable content (engine_dlc.h,
 * folklore/docs/DLC.md).
 *
 * The game's own pack format (found in the EBOOT, func_00155FA0 and its
 * caller in v1.00): a pack is a folder USRDIR\bin\NN (NN = 00..89) holding a
 * list, download.dat_, and the files it names. The list -- and the game's
 * record of installed files, install.dat_ -- is a big-endian file count, then
 * per file its pack number (one byte) and its path under bin
 * (NUL-terminated). On start the game copies each new pack's files into bin
 * and adds them to install.dat_; RPCS3 and a PS3 leave the pack folders
 * empty afterwards. A file the game finds as <name>.edat it reads through
 * sceNpDrmIsAvailable and cellFsOpen (func_0014EC0C / func_0014F278), so a
 * paid pack's files stay encrypted in bin.
 *
 * Paid packs' files are EDATs of licence type 2: their key is the RIF key of
 * the buyer's licence, which RPCS3 keeps as a .rap file named after the
 * content ID (dev_hdd0\home\<user>\exdata). The .rap becomes the RIF key by
 * the published conversion (one AES-128 decryption with the RAP key, then
 * five rounds of a byte shuffle; RPCS3's rap_to_rif does the same), and the
 * runtime's edat_decrypt_file (libs/filesystem/edat.c) decrypts with it,
 * checking every block's hash -- a wrong key is refused, never written. The
 * decrypted file keeps its .edat name: the game opens that name, and the
 * runtime opens a file without the NPD header as it is.
 *
 * Everything here is the user's own content and licence; nothing is
 * downloaded and nothing is written outside this port's hdd0.
 */
#include "engine_dlc.h"
#include "engine_region.h"

#include "edat.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace fs = std::filesystem;

/* One AES-128 block decryption (ECB): BCrypt on Windows. The test harness
 * (folklore/tools/test_dlc_import.cpp) supplies its own off Windows. */
#ifdef _WIN32
static bool dlcAesDecryptBlock(const unsigned char key[16], const unsigned char input[16], unsigned char output[16])
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    bool decrypted = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0)
    {
        if (BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                              (ULONG)sizeof(BCRYPT_CHAIN_MODE_ECB), 0) == 0 &&
            BCryptGenerateSymmetricKey(algorithm, &keyHandle, nullptr, 0, (PUCHAR)key, 16, 0) == 0)
        {
            unsigned char block[16];
            std::memcpy(block, input, 16);
            ULONG written = 0;
            decrypted = BCryptDecrypt(keyHandle, block, 16, nullptr, nullptr, 0, output, 16, &written, 0) == 0 &&
                        written == 16;
            BCryptDestroyKey(keyHandle);
        }
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return decrypted;
}
#else
extern "C" bool dlcAesDecryptBlock(const unsigned char key[16], const unsigned char input[16],
                                   unsigned char output[16]);
#endif

namespace
{
    constexpr int kUpdatePack = 17;     /* the 1.10 update's own data (engine_portable.cpp) */

    /* The published RAP-to-RIF constants (RPCS3 Crypto/key_vault.h, psdevwiki). */
    const unsigned char kRapKey[16] = {0x86, 0x9F, 0x77, 0x45, 0xC1, 0x3F, 0xD8, 0x90,
                                       0xCC, 0xF2, 0x91, 0x88, 0xE3, 0xCC, 0x3E, 0xDF};
    const unsigned char kRapPbox[16] = {0x0C, 0x03, 0x06, 0x04, 0x01, 0x0B, 0x0F, 0x08,
                                        0x02, 0x07, 0x00, 0x05, 0x0A, 0x0E, 0x0D, 0x09};
    const unsigned char kRapE1[16] = {0xA9, 0x3E, 0x1F, 0xD6, 0x7C, 0x55, 0xA3, 0x29,
                                      0xB7, 0x5F, 0xDD, 0xA6, 0x2A, 0x95, 0xC7, 0xA5};
    const unsigned char kRapE2[16] = {0x67, 0xD4, 0x5D, 0xA3, 0x29, 0x6D, 0x00, 0x6A,
                                      0x4E, 0x7C, 0x53, 0x7B, 0xF5, 0x53, 0x8C, 0x74};

    bool rapToRifKey(const unsigned char rap[16], unsigned char rifKey[16])
    {
        unsigned char key[16];
        if (!dlcAesDecryptBlock(kRapKey, rap, key))
        {
            return false;
        }
        for (int round = 0; round < 5; round++)
        {
            for (int index = 0; index < 16; index++)
            {
                const int position = kRapPbox[index];
                key[position] ^= kRapE1[position];
            }
            for (int index = 15; index >= 1; index--)
            {
                const int position = kRapPbox[index];
                const int previous = kRapPbox[index - 1];
                key[position] ^= key[previous];
            }
            int borrow = 0;
            for (int index = 0; index < 16; index++)
            {
                const int position = kRapPbox[index];
                const unsigned char current = (unsigned char)(key[position] - borrow);
                const unsigned char subtrahend = kRapE2[position];
                if (borrow != 1 || current != 0xFF)
                {
                    borrow = current < subtrahend ? 1 : 0;
                    key[position] = (unsigned char)(current - subtrahend);
                }
                else
                {
                    key[position] = (unsigned char)(current - subtrahend);
                }
            }
        }
        std::memcpy(rifKey, key, 16);
        return true;
    }

    struct ListEntry
    {
        int pack = 0;
        std::string path;     /* under USRDIR\bin, '/' separated as the game writes it */
    };

    std::vector<unsigned char> readBytes(const fs::path& path, size_t limit)
    {
        std::vector<unsigned char> bytes;
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return bytes;
        }
        bytes.resize(limit);
        file.read((char*)bytes.data(), (std::streamsize)limit);
        bytes.resize((size_t)file.gcount());
        return bytes;
    }

    bool parseList(const std::vector<unsigned char>& bytes, std::vector<ListEntry>* entries)
    {
        if (bytes.size() < 4)
        {
            return false;
        }
        const size_t count = ((size_t)bytes[0] << 24) | ((size_t)bytes[1] << 16) | ((size_t)bytes[2] << 8) | bytes[3];
        size_t position = 4;
        for (size_t index = 0; index < count; index++)
        {
            if (position >= bytes.size())
            {
                return false;
            }
            ListEntry entry;
            entry.pack = bytes[position++];
            while (position < bytes.size() && bytes[position] != 0)
            {
                entry.path.push_back((char)bytes[position++]);
            }
            if (position >= bytes.size())
            {
                return false;
            }
            position++;
            entries->push_back(entry);
        }
        return true;
    }

    bool readList(const fs::path& path, std::vector<ListEntry>* entries)
    {
        return parseList(readBytes(path, 4 * 1024 * 1024), entries);
    }

    bool writeList(const fs::path& path, const std::vector<ListEntry>& entries)
    {
        std::vector<unsigned char> bytes;
        const size_t count = entries.size();
        bytes.push_back((unsigned char)(count >> 24));
        bytes.push_back((unsigned char)(count >> 16));
        bytes.push_back((unsigned char)(count >> 8));
        bytes.push_back((unsigned char)count);
        for (const ListEntry& entry : entries)
        {
            bytes.push_back((unsigned char)entry.pack);
            bytes.insert(bytes.end(), entry.path.begin(), entry.path.end());
            bytes.push_back(0);
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return false;
        }
        file.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        return (bool)file;
    }

    bool sameText(const std::string& left, const std::string& right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (size_t index = 0; index < left.size(); index++)
        {
            if (std::tolower((unsigned char)left[index]) != std::tolower((unsigned char)right[index]))
            {
                return false;
            }
        }
        return true;
    }

    bool isDirectory(const fs::path& path)
    {
        std::error_code error;
        return fs::is_directory(path, error);
    }

    bool isFile(const fs::path& path)
    {
        std::error_code error;
        return fs::is_regular_file(path, error);
    }

    /* The content ID of an encrypted file (the NPD header's 0x10..0x3F), or
     * an empty string for a plain file. */
    std::string encryptedContentId(const fs::path& path)
    {
        const std::vector<unsigned char> header = readBytes(path, 0x40);
        if (header.size() < 0x40 || header[0] != 'N' || header[1] != 'P' || header[2] != 'D' || header[3] != 0)
        {
            return std::string();
        }
        std::string contentId;
        for (size_t index = 0x10; index < 0x40 && header[index] != 0; index++)
        {
            contentId.push_back((char)header[index]);
        }
        return contentId;
    }

    /* One file of a pack: where it is in the source and where it goes. */
    struct PackFile
    {
        std::string relative;     /* under bin, as the list has it */
        fs::path source;
        std::string contentId;    /* empty: plain */
    };

    struct PackPlan
    {
        DlcPackInfo info;
        std::vector<PackFile> files;
        std::vector<ListEntry> entries;
    };

    fs::path relativeToPath(const std::string& relative)
    {
        fs::path result;
        size_t start = 0;
        while (start <= relative.size())
        {
            const size_t slash = relative.find_first_of("/\\", start);
            const std::string part = relative.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (!part.empty())
            {
                result /= part;
            }
            if (slash == std::string::npos)
            {
                break;
            }
            start = slash + 1;
        }
        return result;
    }

    /* The .rap for a content ID in any of the licence folders. */
    bool findLicence(const std::vector<std::string>& folders, const std::string& contentId, unsigned char rifKey[16])
    {
        const std::string wanted = contentId + ".rap";
        for (const std::string& folder : folders)
        {
            std::error_code error;
            for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(folder), error))
            {
                if (!entry.is_regular_file(error) || !sameText(entry.path().filename().string(), wanted))
                {
                    continue;
                }
                const std::vector<unsigned char> rap = readBytes(entry.path(), 64);
                if (rap.size() == 16 && rapToRifKey(rap.data(), rifKey))
                {
                    return true;
                }
            }
        }
        return false;
    }

    std::string hdd0Directory()
    {
#ifdef _WIN32
        char value[MAX_PATH * 2];
        const DWORD length = GetEnvironmentVariableA("PS3_HDD0_ROOT", value, (DWORD)sizeof(value));
        if (length > 0 && length < sizeof(value))
        {
            return std::string(value, length);
        }
        return "hdd0";
#else
        const char* value = std::getenv("PS3_HDD0_ROOT");
        return value ? value : "hdd0";
#endif
    }

    fs::path targetBin()
    {
        return fs::path(hdd0Directory()) / "game" / FOLKLORE_TITLE_ID / "USRDIR" / "bin";
    }

    /* Which paid pack is which ("Add-on pack N"): kept beside install.dat_,
     * since an installed pack's files no longer carry their content ID. */
    fs::path packNamesPath()
    {
        return targetBin() / "port_dlc_packs.txt";
    }

    std::map<int, std::string> readPackNames()
    {
        std::map<int, std::string> names;
        std::ifstream file(packNamesPath());
        std::string line;
        while (std::getline(file, line))
        {
            const size_t separator = line.find('=');
            if (separator == std::string::npos)
            {
                continue;
            }
            const int pack = std::atoi(line.substr(0, separator).c_str());
            std::string contentId = line.substr(separator + 1);
            while (!contentId.empty() && (contentId.back() == '\r' || contentId.back() == ' '))
            {
                contentId.pop_back();
            }
            if (pack > 0)
            {
                names[pack] = contentId;
            }
        }
        return names;
    }

    void writePackNames(const std::map<int, std::string>& names)
    {
        std::ofstream file(packNamesPath(), std::ios::trunc);
        file << "# Folklore port: the content ID of each paid pack installed (engine_dlc.cpp)\n";
        for (const auto& [pack, contentId] : names)
        {
            file << pack << "=" << contentId << "\n";
        }
    }

    bool decryptInto(const fs::path& from, const fs::path& to, const unsigned char rifKey[16]);

    /* The source's packs: those already installed there (install.dat_) and
     * those still waiting in their folders (bin\NN\download.dat_). */
    std::vector<PackPlan> planPacks(const DlcSource& source, std::string* problem)
    {
        const fs::path sourceBin = fs::path(source.gameFolder) / "USRDIR" / "bin";
        std::map<int, PackPlan> plans;
        std::set<std::string> seen;

        /* Waiting packs first: if a file is both waiting and installed, the
         * waiting copy is the newer one. */
        for (int folderNumber = 0; folderNumber <= 89; folderNumber++)
        {
            char folderName[8];
            std::snprintf(folderName, sizeof(folderName), "%02d", folderNumber);
            const fs::path packFolder = sourceBin / folderName;
            std::vector<ListEntry> listed;
            if (isFile(packFolder / "download.dat_"))
            {
                if (!readList(packFolder / "download.dat_", &listed))
                {
                    continue;
                }
            }
            else if (isFile(packFolder / "download.edat"))
            {
                /* A paid pack's list may itself be encrypted: read it with
                 * the pack's licence, through a temporary decrypted copy. */
                unsigned char rifKey[16];
                const std::string listContentId = encryptedContentId(packFolder / "download.edat");
                std::error_code error;
                const fs::path temporary = fs::temp_directory_path(error) / ("folklore_dlc_list_" + std::string(folderName));
                if (listContentId.empty() || !findLicence(source.licenceFolders, listContentId, rifKey) ||
                    !decryptInto(packFolder / "download.edat", temporary, rifKey) || !readList(temporary, &listed))
                {
                    fs::remove(temporary, error);
                    PackPlan& plan = plans[100 + folderNumber];
                    plan.info.pack = folderNumber;
                    plan.info.contentId = listContentId.empty() ? std::string("(unknown)") : listContentId;
                    plan.info.licenceFound = false;
                    continue;
                }
                fs::remove(temporary, error);
            }
            else
            {
                continue;
            }
            for (const ListEntry& entry : listed)
            {
                if (entry.pack == kUpdatePack || entry.pack < 10)
                {
                    continue;
                }
                const fs::path sourceFile = packFolder / relativeToPath(entry.path);
                if (!isFile(sourceFile))
                {
                    continue;
                }
                std::string key = entry.path;
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character) { return (char)std::tolower(character); });
                if (!seen.insert(key).second)
                {
                    continue;
                }
                PackPlan& plan = plans[entry.pack];
                plan.info.pack = entry.pack;
                plan.files.push_back({entry.path, sourceFile, encryptedContentId(sourceFile)});
                plan.entries.push_back(entry);
            }
        }

        std::vector<ListEntry> installed;
        if (isFile(sourceBin / "install.dat_") && !readList(sourceBin / "install.dat_", &installed))
        {
            *problem = "The game's record of installed packs there (USRDIR\\bin\\install.dat_) could not be read.";
            return {};
        }
        for (const ListEntry& entry : installed)
        {
            if (entry.pack == kUpdatePack || entry.pack < 10)
            {
                continue;
            }
            const fs::path sourceFile = sourceBin / relativeToPath(entry.path);
            if (!isFile(sourceFile))
            {
                continue;
            }
            std::string key = entry.path;
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character) { return (char)std::tolower(character); });
            if (!seen.insert(key).second)
            {
                continue;
            }
            PackPlan& plan = plans[entry.pack];
            plan.info.pack = entry.pack;
            plan.files.push_back({entry.path, sourceFile, encryptedContentId(sourceFile)});
            plan.entries.push_back(entry);
        }

        std::vector<PackPlan> result;
        for (auto& [pack, plan] : plans)
        {
            for (const PackFile& file : plan.files)
            {
                if (!file.contentId.empty() && plan.info.contentId.empty())
                {
                    plan.info.contentId = file.contentId;
                }
            }
            plan.info.fileCount = (int)plan.files.size();
            if (!plan.info.contentId.empty())
            {
                unsigned char rifKey[16];
                plan.info.licenceFound = true;
                std::set<std::string> contentIds;
                for (const PackFile& file : plan.files)
                {
                    if (!file.contentId.empty())
                    {
                        contentIds.insert(file.contentId);
                    }
                }
                for (const std::string& contentId : contentIds)
                {
                    if (!findLicence(source.licenceFolders, contentId, rifKey))
                    {
                        plan.info.licenceFound = false;
                    }
                }
            }
            result.push_back(plan);
        }
        return result;
    }

    bool copyFileOver(const fs::path& from, const fs::path& to)
    {
        std::error_code error;
        fs::create_directories(to.parent_path(), error);
        return fs::copy_file(from, to, fs::copy_options::overwrite_existing, error);
    }

    /* Decrypt `from` into `to` with the licence for its content ID: through
     * a working copy in the target folder, with the RIF key in the
     * <file>.rifkey sidecar edat_decrypt_file reads, both removed after. */
    /* The file is there already, as an earlier import left it: a plain file
     * of the same size, or a decrypted one (no NPD header) of the size its
     * EDAT header gives. */
    bool alreadyInPlace(const PackFile& file, const fs::path& target)
    {
        std::error_code error;
        if (!isFile(target))
        {
            return false;
        }
        const uintmax_t targetSize = fs::file_size(target, error);
        if (error)
        {
            return false;
        }
        if (file.contentId.empty())
        {
            return targetSize == fs::file_size(file.source, error) && !error;
        }
        const std::vector<unsigned char> header = readBytes(file.source, 0x90);
        if (header.size() < 0x90 || !encryptedContentId(target).empty())
        {
            return false;
        }
        uintmax_t plainSize = 0;
        for (size_t index = 0x88; index < 0x90; index++)
        {
            plainSize = (plainSize << 8) | header[index];
        }
        return targetSize == plainSize;
    }

    bool decryptInto(const fs::path& from, const fs::path& to, const unsigned char rifKey[16])
    {
        std::error_code error;
        fs::create_directories(to.parent_path(), error);
        const fs::path working = fs::path(to.string() + ".importing");
        const fs::path sidecar = fs::path(working.string() + ".rifkey");
        const fs::path decrypted = fs::path(to.string() + ".decrypted");
        if (!fs::copy_file(from, working, fs::copy_options::overwrite_existing, error))
        {
            return false;
        }
        {
            std::ofstream keyFile(sidecar, std::ios::binary | std::ios::trunc);
            keyFile.write((const char*)rifKey, 16);
        }
        const int result = edat_decrypt_file(working.string().c_str(), decrypted.string().c_str());
        fs::remove(sidecar, error);
        fs::remove(working, error);
        if (result != 0)
        {
            fs::remove(decrypted, error);
            return false;
        }
        fs::rename(decrypted, to, error);
        if (error)
        {
            fs::remove(to, error);
            fs::rename(decrypted, to, error);
        }
        return !error;
    }
}

std::string engineDlcPackNameByNumber(int pack)
{
    const std::map<int, std::string> names = readPackNames();
    DlcPackInfo info;
    info.pack = pack;
    const auto found = names.find(pack);
    if (found != names.end())
    {
        info.contentId = found->second;
    }
    return engineDlcPackName(info);
}

std::string engineDlcPackName(const DlcPackInfo& pack)
{
    /* Paid packs' content IDs end in their number: ...FOLKLORE20000003. */
    if (!pack.contentId.empty())
    {
        size_t digits = pack.contentId.size();
        while (digits > 0 && std::isdigit((unsigned char)pack.contentId[digits - 1]))
        {
            digits--;
        }
        const std::string tail = pack.contentId.substr(digits);
        const int number = tail.size() > 2 ? std::atoi(tail.substr(tail.size() - 2).c_str()) : std::atoi(tail.c_str());
        if (number > 0)
        {
            return "Add-on pack " + std::to_string(number);
        }
        return "Add-on pack (" + std::to_string(pack.pack) + ")";
    }
    return "Free pack (" + std::to_string(pack.pack) + ")";
}

bool engineDlcScan(const std::string& pickedFolder, DlcSource* source, std::string* problem)
{
    *source = DlcSource();
    const fs::path picked(pickedFolder);
    const std::string titleId = FOLKLORE_TITLE_ID;
    /* Whichever of these the user picked: the game's folder itself, or a
     * folder above it on the way down from RPCS3's own folder. */
    const fs::path candidates[] = {picked, picked / titleId, picked / "game" / titleId,
                                   picked / "dev_hdd0" / "game" / titleId};
    fs::path gameFolder;
    for (const fs::path& candidate : candidates)
    {
        if (isDirectory(candidate / "USRDIR" / "bin"))
        {
            gameFolder = candidate;
            break;
        }
    }
    if (gameFolder.empty())
    {
        *problem = "No Folklore (" + titleId + ") game data in \"" + pickedFolder +
                   "\". Choose RPCS3's folder (the one with dev_hdd0 in it), or its dev_hdd0\\game\\" + titleId +
                   " folder.";
        return false;
    }
    const std::vector<unsigned char> sfo = readBytes(gameFolder / "PARAM.SFO", 64 * 1024);
    if (!sfo.empty() && std::search(sfo.begin(), sfo.end(), titleId.begin(), titleId.end()) == sfo.end())
    {
        *problem = "\"" + gameFolder.string() + "\" is not " + titleId + ", the release this build plays.";
        return false;
    }
    source->gameFolder = gameFolder.string();

    /* Licences: RPCS3 keeps them in dev_hdd0\home\<user>\exdata, two folders
     * up from the game's; also a "licences" folder beside the data (or the
     * exe), and the picked folder itself. */
    std::error_code error;
    const fs::path dev_hdd0 = gameFolder.parent_path().parent_path();
    for (const fs::directory_entry& user : fs::directory_iterator(dev_hdd0 / "home", error))
    {
        if (user.is_directory(error) && isDirectory(user.path() / "exdata"))
        {
            source->licenceFolders.push_back((user.path() / "exdata").string());
        }
    }
    const fs::path ownLicences = fs::path(hdd0Directory()).parent_path() / "licences";
    for (const fs::path& folder : {ownLicences, picked, picked / "exdata"})
    {
        if (isDirectory(folder))
        {
            source->licenceFolders.push_back(folder.string());
        }
    }

    for (const PackPlan& plan : planPacks(*source, problem))
    {
        source->packs.push_back(plan.info);
    }
    if (!problem->empty())
    {
        return false;
    }
    if (source->packs.empty())
    {
        *problem = "No downloadable content in \"" + gameFolder.string() +
                   "\". In RPCS3, install the packs first: File > Install Packages/Raps/Edats, choose each pack's "
                   ".pkg and its .rap licence.";
        return false;
    }
    return true;
}

std::string engineDlcInstall(const DlcSource& source,
                             const std::function<void(int, int, const std::string&)>& progress)
{
    std::string problem;
    std::vector<PackPlan> plans = planPacks(source, &problem);
    if (!problem.empty())
    {
        return problem;
    }
    const fs::path bin = targetBin();
    std::error_code error;
    fs::create_directories(bin, error);

    int totalFiles = 0;
    for (const PackPlan& plan : plans)
    {
        if (plan.info.contentId.empty() || plan.info.licenceFound)
        {
            totalFiles += plan.info.fileCount;
        }
    }

    std::vector<ListEntry> installed;
    if (isFile(bin / "install.dat_") && !readList(bin / "install.dat_", &installed))
    {
        installed.clear();
    }
    std::map<int, std::string> packNames = readPackNames();
    std::string report;
    int done = 0;
    int installedPacks = 0;
    for (const PackPlan& plan : plans)
    {
        const std::string name = engineDlcPackName(plan.info);
        if (!plan.info.contentId.empty() && !plan.info.licenceFound)
        {
            report += name + ": not installed -- no licence for it (" + plan.info.contentId + ".rap).\n";
            continue;
        }
        /* All of a pack's files or none: decrypt and copy into place, and
         * take back what was written if any file fails. */
        std::vector<fs::path> written;
        std::string failure;
        for (const PackFile& file : plan.files)
        {
            const fs::path target = bin / relativeToPath(file.relative);
            progress(done, totalFiles, name + ": " + file.relative);
            if (alreadyInPlace(file, target))
            {
                done++;
                continue;     /* imported before: not written now, so never taken back */
            }
            bool fileDone = false;
            if (file.contentId.empty())
            {
                fileDone = copyFileOver(file.source, target);
            }
            else
            {
                unsigned char rifKey[16];
                fileDone = findLicence(source.licenceFolders, file.contentId, rifKey) &&
                           decryptInto(file.source, target, rifKey);
            }
            done++;
            if (!fileDone)
            {
                failure = file.relative;
                break;
            }
            written.push_back(target);
        }
        if (!failure.empty())
        {
            for (const fs::path& path : written)
            {
                fs::remove(path, error);
            }
            report += name + ": not installed -- " + failure +
                      " could not be decrypted or copied (a licence for another account, or a damaged file).\n";
            continue;
        }
        for (const ListEntry& entry : plan.entries)
        {
            bool found = false;
            for (ListEntry& existing : installed)
            {
                if (sameText(existing.path, entry.path))
                {
                    existing.pack = entry.pack;
                    found = true;
                }
            }
            if (!found)
            {
                installed.push_back(entry);
            }
        }
        if (!plan.info.contentId.empty())
        {
            packNames[plan.info.pack] = plan.info.contentId;
        }
        installedPacks++;
        report += name + ": installed (" + std::to_string(plan.info.fileCount) + " files).\n";
    }
    if (installedPacks > 0)
    {
        if (!writeList(bin / "install.dat_", installed))
        {
            report += "Could not write the game's record of installed packs (" + (bin / "install.dat_").string() + ").\n";
        }
        writePackNames(packNames);
        report += "Restart the game to play them.";
    }
    progress(totalFiles, totalFiles, std::string());
    std::fprintf(stderr, "[engine] DLC import from %s:\n%s\n", source.gameFolder.c_str(), report.c_str());
    return report;
}

std::vector<int> engineDlcInstalledPacks()
{
    std::vector<ListEntry> installed;
    std::set<int> packs;
    if (readList(targetBin() / "install.dat_", &installed))
    {
        for (const ListEntry& entry : installed)
        {
            if (entry.pack >= 10 && entry.pack != kUpdatePack)
            {
                packs.insert(entry.pack);
            }
        }
    }
    return std::vector<int>(packs.begin(), packs.end());
}
