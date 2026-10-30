/*
 * engine_dlc.h -- bringing Folklore's downloadable content into this port.
 *
 * Folklore's packs come from the PlayStation Store as .pkg files with a
 * licence (.rap) each. RPCS3 (File > Install Packages/Raps/Edats) or a PS3
 * installs them into the game's HDD folder, dev_hdd0\game\<title id>, where
 * the game copies each pack from USRDIR\bin\<pack number> into USRDIR\bin on
 * its next start and records it in install.dat_. Paid packs keep their files
 * encrypted (.edat), readable only with the buyer's licence.
 *
 * The importer reads such a folder (RPCS3's, or one copied off a PS3) and
 * puts every pack it can into this port's own hdd0, installed: plain files
 * copied, encrypted ones decrypted with the matching licence, and the pack
 * recorded in install.dat_ as the game's installer would. It changes nothing
 * in the folder it reads. folklore/docs/DLC.md has the details.
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

/* One pack, by its folder number under USRDIR\bin (10 and up; 17 is the
 * 1.10 update's own data and is left to the update). */
struct DlcPackInfo
{
    int pack = 0;
    /* Paid packs: the content ID of their encrypted files, e.g.
     * UP9000-BCUS98147_00-FOLKLORE20000001. Empty for free packs. */
    std::string contentId;
    int fileCount = 0;
    bool licenceFound = false;     /* paid packs: a .rap for contentId was found */
};

/* What a folder offers: where it was found and its packs. */
struct DlcSource
{
    std::string gameFolder;                    /* ...\dev_hdd0\game\<title id> */
    std::vector<std::string> licenceFolders;   /* where .rap files are looked for */
    std::vector<DlcPackInfo> packs;
    /* The packs belong to the other release (North American packs for the
     * European build, or the other way round): used when the picked folder
     * has none of this build's own. The two releases name English
     * differently (_en / _uk, folders en / uk), so the English files are
     * installed under both names; the other languages are named alike. */
    bool otherRegion = false;
    std::string sourceTitleId;
};

/* The folder the user picked: RPCS3's own folder (holding dev_hdd0),
 * dev_hdd0, dev_hdd0\game, or the game's folder in it. Fills source with what
 * it offers; false with a sentence for the user when it is not usable. */
bool engineDlcScan(const std::string& pickedFolder, DlcSource* source, std::string* problem);

/* Install every pack of `source` that can be: all of a pack's files, or none
 * of them. progress(done, total, current file) is called as it goes. Returns
 * a few lines for the user saying what was installed and what was not. */
std::string engineDlcInstall(const DlcSource& source,
                             const std::function<void(int, int, const std::string&)>& progress);

/* The packs this port's hdd0 has installed (from install.dat_), in order. */
std::vector<int> engineDlcInstalledPacks();

/* "Add-on pack 3" for a paid pack (numbered by its content ID), "Free pack
 * (19)" for the others. */
std::string engineDlcPackName(const DlcPackInfo& pack);
std::string engineDlcPackNameByNumber(int pack);
