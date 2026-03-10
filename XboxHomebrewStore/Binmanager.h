#pragma once
#include "Main.h"
#include "Models.h"

// ==========================================================================
// BinManager
// Replaces DbManager(obsolete) entirely. Reads from a locally cached homebrew.bin
// instead of SQLite. No external libraries required.
//
// Binary layout (must match generateBin.js exactly):
//   FileHeader      36 bytes
//   AppRecord[]     740 bytes x appCount        -- O(1) seek by index
//   VersionRecord[] 472 bytes x versionCount    -- partsOffset points into file
//   PartRecord[]    260 bytes x total parts     -- packed, referenced by partsOffset
//
// Typical usage:
//   1. On startup: BinManager::TryDownloadBin()
//   2. Replace DbManager::TryGetCategories  -> BinManager::TryGetCategories
//   3. Replace DbManager::TryGetApps        -> BinManager::TryGetApps
//   4. Replace DbManager::TryGetVersions    -> BinManager::TryGetVersions
// ==========================================================================

class BinManager {
public:
    // Download https://api.xboxhomebrew.store/api/db and save to BIN_PATH.
    // Downloads to a temp file first, validates the magic header, then
    // atomically replaces the live file. Returns false and shows an error
    // on failure -- caller should treat this as fatal on first boot.
    static bool TryDownloadBin();

    // Returns true if BIN_PATH exists on disk.
    static bool IsBinPresent();

    // Delete the cached file.
    static void DeleteBin();

    // Equivalent to DbManager::TryGetCategories.
    // Scans AppRecords, counts per category, prepends "All Apps" at index 0.
    static bool TryGetCategories(CategoriesResponse &result);

    // Equivalent to DbManager::TryGetApps.
    // Supports offset / count / category / name filters.
    // O(n) scan over AppRecords -- fast enough for 300-500 apps on Xbox.
    static bool TryGetApps(AppsResponse      &result,
                           int32_t            offset,
                           int32_t            count,
                           const std::string &category,
                           const std::string &name);

    // Equivalent to DbManager::TryGetVersions.
    // Scans VersionRecords for matching appId, reads parts via partsOffset.
    static bool TryGetVersions(const std::string &appId,
                               VersionsResponse  &result);

    // Path where the bin is stored relative to the executable.
    // Uses a relative path so it works wherever the app is installed.
    static const char *BIN_PATH;
    static const char *BIN_TMP_PATH;
    static const char *BIN_URL;
};