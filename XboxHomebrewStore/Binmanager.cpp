#include "BinManager.h"
#include "Debug.h"
#include "WebManager.h"
#include "Main.h"

#include <xtl.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

// ==========================================================================
// Constants
// ==========================================================================

const char *BinManager::BIN_URL      = "https://api.xboxhomebrew.store/api/db";
const char *BinManager::BIN_PATH     = "homebrew.bin";
const char *BinManager::BIN_TMP_PATH = "homebrew_tmp.bin";

// Magic bytes at offset 0
static const uint32_t XHBS_MAGIC = 0x53424858; // 'XHBS' little-endian

// ==========================================================================
// Struct sizes -- must match generateBin.js exactly
// ==========================================================================

#define SIZEOF_HEADER   36
#define SIZEOF_APP      740
#define SIZEOF_VERSION  472
#define SIZEOF_PART     260

// ==========================================================================
// FileHeader field offsets
// ==========================================================================

#define HDR_MAGIC           0   // uint32
#define HDR_FORMAT_VERSION  4   // uint8
// pad[3] at 5
#define HDR_APP_COUNT       8   // uint32
#define HDR_VERSION_COUNT   12  // uint32
#define HDR_PART_COUNT      16  // uint32
// reserved[16] at 20

// ==========================================================================
// AppRecord field offsets (within each record)
// ==========================================================================

#define APP_ID              0   // char[32]
#define APP_NAME            32  // char[64]
#define APP_AUTHOR          96  // char[64]
#define APP_CATEGORY        160 // char[32]
#define APP_DESCRIPTION     192 // char[512]
#define APP_LATEST_VERSION  704 // char[32]
#define APP_STATE           736 // uint8
// pad[3] at 737

// ==========================================================================
// VersionRecord field offsets (within each record)
// ==========================================================================

#define VER_ID              0   // char[32]
#define VER_APP_ID          32  // char[32]
#define VER_VERSION         64  // char[32]
#define VER_RELEASE_DATE    96  // char[16]
#define VER_CHANGELOG       112 // char[256]
#define VER_TITLE_ID        368 // char[16]
#define VER_REGION          384 // char[8]
#define VER_FOLDER_NAME     392 // char[64]
#define VER_SIZE            456 // uint32
#define VER_PART_COUNT      460 // uint8
// pad[3] at 461
#define VER_PARTS_OFFSET    464 // uint32  -- absolute byte offset into file
// reserved[4] at 468

// ==========================================================================
// PartRecord field offsets (within each record)
// ==========================================================================

#define PART_FILENAME       0   // char[256]  (filename or full URL)
#define PART_TYPE           256 // uint8      0=file, 1=url
#define PART_NUMBER         257 // uint8
// pad[2] at 258

// ==========================================================================
// BinFile -- RAII file handle that maps the entire bin into memory
// ==========================================================================

struct BinFile {
    HANDLE  hFile;
    HANDLE  hMap;
    LPVOID  pData;
    DWORD   size;

    BinFile() : hFile(INVALID_HANDLE_VALUE), hMap(NULL), pData(NULL), size(0) {}

    ~BinFile() { Close(); }

    bool Open(const char *path) {
        hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            Debug::Print("BinManager: CreateFile failed (%lu)\n", GetLastError());
            return false;
        }

        size = GetFileSize(hFile, NULL);
        if (size < SIZEOF_HEADER) {
            Debug::Print("BinManager: file too small (%lu bytes)\n", size);
            Close();
            return false;
        }

        hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (hMap == NULL) {
            Debug::Print("BinManager: CreateFileMapping failed (%lu)\n", GetLastError());
            Close();
            return false;
        }

        pData = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        if (pData == NULL) {
            Debug::Print("BinManager: MapViewOfFile failed (%lu)\n", GetLastError());
            Close();
            return false;
        }

        return true;
    }

    void Close() {
        if (pData  != NULL)                  { UnmapViewOfFile(pData);  pData  = NULL; }
        if (hMap   != NULL)                  { CloseHandle(hMap);       hMap   = NULL; }
        if (hFile  != INVALID_HANDLE_VALUE)  { CloseHandle(hFile);      hFile  = INVALID_HANDLE_VALUE; }
    }

    // Raw byte access
    const uint8_t *Bytes() const {
        return reinterpret_cast<const uint8_t *>(pData);
    }

    // Read a null-terminated string from a fixed-size field
    std::string ReadStr(uint32_t offset, uint32_t fieldLen) const {
        if (offset + fieldLen > size) return std::string();
        const char *p = reinterpret_cast<const char *>(Bytes() + offset);
        // strnlen equivalent -- stop at null or fieldLen
        size_t len = 0;
        while (len < fieldLen && p[len] != '\0') len++;
        return std::string(p, len);
    }

    uint32_t ReadU32(uint32_t offset) const {
        if (offset + 4 > size) return 0;
        uint32_t v;
        memcpy(&v, Bytes() + offset, 4);
        return v;
    }

    uint8_t ReadU8(uint32_t offset) const {
        if (offset >= size) return 0;
        return Bytes()[offset];
    }
};

// ==========================================================================
// Helpers
// ==========================================================================

// Case-insensitive ASCII contains check
static bool StrContainsI(const std::string &haystack, const std::string &needle) {
    if (needle.empty()) return true;
    std::string h = haystack, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), tolower);
    std::transform(n.begin(), n.end(), n.begin(), tolower);
    return h.find(n) != std::string::npos;
}

static bool StrEqualsI(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

// Validate the file header -- returns false and prints error if bad
static bool ValidateHeader(const BinFile &f) {
    uint32_t magic = f.ReadU32(HDR_MAGIC);
    if (magic != XHBS_MAGIC) {
        Debug::Print("BinManager: bad magic 0x%08lX (expected 0x%08lX)\n",
                     magic, XHBS_MAGIC);
        return false;
    }
    uint8_t fmtVer = f.ReadU8(HDR_FORMAT_VERSION);
    if (fmtVer != 1) {
        Debug::Print("BinManager: unsupported format version %d\n", fmtVer);
        return false;
    }
    return true;
}

// ==========================================================================
// TryDownloadBin
// ==========================================================================

bool BinManager::TryDownloadBin() {
    Debug::Print("BinManager: downloading %s -> %s\n", BIN_URL, BIN_TMP_PATH);

    DeleteFileA(BIN_TMP_PATH);

    bool ok = WebManager::TryDownloadApiData(
        std::string(BIN_URL),
        std::string(BIN_TMP_PATH),
        nullptr,   // no progress callback
        nullptr,   // no progress userdata
        nullptr,   // no cancel flag
        nullptr,   // no header capture
        nullptr);  // no content-type capture

    if (!ok) {
        Debug::Print("BinManager: download failed\n");
        DeleteFileA(BIN_TMP_PATH);
        return false;
    }

    // Validate magic before replacing the live file
    {
        BinFile probe;
        if (!probe.Open(BIN_TMP_PATH) || !ValidateHeader(probe)) {
            Debug::Print("BinManager: downloaded file is not a valid XHBS bin\n");
            DeleteFileA(BIN_TMP_PATH);
            return false;
        }
        uint32_t appCount = probe.ReadU32(HDR_APP_COUNT);
        if (appCount == 0) {
            Debug::Print("BinManager: downloaded bin has zero apps\n");
            DeleteFileA(BIN_TMP_PATH);
            return false;
        }
        Debug::Print("BinManager: validated -- %lu apps\n", appCount);
    }

    // Atomically replace live file
    DeleteFileA(BIN_PATH);
    if (!MoveFileA(BIN_TMP_PATH, BIN_PATH)) {
        Debug::Print("BinManager: MoveFile failed (%lu)\n", GetLastError());
        DeleteFileA(BIN_TMP_PATH);
        return false;
    }

    Debug::Print("BinManager: bin ready at %s\n", BIN_PATH);
    return true;
}

// ==========================================================================
// IsBinPresent / DeleteBin
// ==========================================================================

bool BinManager::IsBinPresent() {
    DWORD attr = GetFileAttributesA(BIN_PATH);
    return (attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

void BinManager::DeleteBin() {
    DeleteFileA(BIN_PATH);
}

// ==========================================================================
// TryGetCategories
// ==========================================================================

bool BinManager::TryGetCategories(CategoriesResponse &result) {
    result.clear();

    BinFile f;
    if (!f.Open(BIN_PATH) || !ValidateHeader(f)) return false;

    uint32_t appCount = f.ReadU32(HDR_APP_COUNT);
    uint32_t appBase  = SIZEOF_HEADER;

    // Count per category using a map, preserve insertion order for "All Apps"
    std::map<std::string, int32_t> catCounts;
    int32_t totalCount = 0;

    for (uint32_t i = 0; i < appCount; i++) {
        uint32_t base  = appBase + SIZEOF_APP * i;
        uint8_t  state = f.ReadU8(base + APP_STATE);
        if (state != 1) continue;

        std::string cat = f.ReadStr(base + APP_CATEGORY, 32);
        catCounts[cat]++;
        totalCount++;
    }

    // "All Apps" always first
    CategoryItem allApps;
    allApps.name  = "All Apps";
    allApps.count = totalCount;
    result.push_back(allApps);

    // Rest alphabetically (std::map is already sorted)
    for (std::map<std::string, int32_t>::iterator it = catCounts.begin();
         it != catCounts.end(); ++it) {
        CategoryItem cat;
        cat.name  = it->first;
        cat.count = it->second;
        result.push_back(cat);
    }

    Debug::Print("BinManager::TryGetCategories: %d categories (%d total)\n",
                 (int32_t)result.size(), totalCount);
    return true;
}

// ==========================================================================
// TryGetApps
// ==========================================================================

bool BinManager::TryGetApps(AppsResponse      &result,
                             int32_t            offset,
                             int32_t            count,
                             const std::string &category,
                             const std::string &name) {
    result.items.clear();

    BinFile f;
    if (!f.Open(BIN_PATH) || !ValidateHeader(f)) return false;

    uint32_t appCount = f.ReadU32(HDR_APP_COUNT);
    uint32_t appBase  = SIZEOF_HEADER;

    // Collect matching apps into a temp vector, then apply offset/count
    std::vector<AppItem> matches;

    for (uint32_t i = 0; i < appCount; i++) {
        uint32_t base  = appBase + SIZEOF_APP * i;
        uint8_t  state = f.ReadU8(base + APP_STATE);
        if (state != 1) continue;

        std::string appCategory = f.ReadStr(base + APP_CATEGORY, 32);
        std::string appName     = f.ReadStr(base + APP_NAME,     64);

        if (!category.empty() && !StrEqualsI(appCategory, category)) continue;
        if (!name.empty()     && !StrContainsI(appName, name))        continue;

        AppItem app;
        app.id            = f.ReadStr(base + APP_ID,             32);
        app.name          = appName;
        app.author        = f.ReadStr(base + APP_AUTHOR,         64);
        app.category      = appCategory;
        app.description   = f.ReadStr(base + APP_DESCRIPTION,   512);
        app.latestVersion = f.ReadStr(base + APP_LATEST_VERSION, 32);
        app.state         = (int32_t)state;
        matches.push_back(app);
    }

    // Sort by name ASC to match API ordering
    std::sort(matches.begin(), matches.end(),
              [](const AppItem &a, const AppItem &b) {
                  return a.name < b.name;
              });

    // Apply paging
    int32_t total = (int32_t)matches.size();
    int32_t start = offset < total ? offset : total;
    int32_t end   = start + count < total ? start + count : total;

    for (int32_t i = start; i < end; i++) {
        result.items.push_back(matches[i]);
    }

    Debug::Print("BinManager::TryGetApps: offset=%d count=%d matched=%d returned=%d\n",
                 offset, count, total, (int32_t)result.items.size());
    return true;
}

// ==========================================================================
// TryGetVersions
// ==========================================================================

bool BinManager::TryGetVersions(const std::string &appId,
                                 VersionsResponse  &result) {
    result.id.clear();
    result.name.clear();
    result.author.clear();
    result.description.clear();
    result.latestVersion.clear();
    result.versions.clear();

    BinFile f;
    if (!f.Open(BIN_PATH) || !ValidateHeader(f)) return false;

    uint32_t appCount     = f.ReadU32(HDR_APP_COUNT);
    uint32_t versionCount = f.ReadU32(HDR_VERSION_COUNT);
    uint32_t appBase      = SIZEOF_HEADER;
    uint32_t verBase      = appBase + SIZEOF_APP * appCount;

    // -- 1. Find the app record ----------------------------------------
    bool found = false;
    for (uint32_t i = 0; i < appCount; i++) {
        uint32_t    base = appBase + SIZEOF_APP * i;
        std::string id   = f.ReadStr(base + APP_ID, 32);
        if (id != appId) continue;

        result.id          = id;
        result.name        = f.ReadStr(base + APP_NAME,        64);
        result.author      = f.ReadStr(base + APP_AUTHOR,      64);
        result.description = f.ReadStr(base + APP_DESCRIPTION, 512);
        found = true;
        break;
    }

    if (!found) {
        Debug::Print("BinManager::TryGetVersions: app '%s' not found\n", appId.c_str());
        return false;
    }

    // -- 2. Collect all versions for this app -------------------------
    bool isFirst = true;

    for (uint32_t i = 0; i < versionCount; i++) {
        uint32_t    base     = verBase + SIZEOF_VERSION * i;
        std::string verAppId = f.ReadStr(base + VER_APP_ID, 32);
        if (verAppId != appId) continue;

        VersionItem ver;
        ver.id          = f.ReadStr(base + VER_ID,          32);
        ver.version     = f.ReadStr(base + VER_VERSION,     32);
        ver.releaseDate = f.ReadStr(base + VER_RELEASE_DATE, 16);
        ver.changeLog   = f.ReadStr(base + VER_CHANGELOG,   256);
        ver.titleId     = f.ReadStr(base + VER_TITLE_ID,     16);
        ver.region      = f.ReadStr(base + VER_REGION,        8);
        ver.folderName  = f.ReadStr(base + VER_FOLDER_NAME,  64);
        ver.size        = f.ReadU32(base + VER_SIZE);

        uint8_t  partCount   = f.ReadU8(base + VER_PART_COUNT);
        uint32_t partsOffset = f.ReadU32(base + VER_PARTS_OFFSET);

        // Latest version = first version we see in the file
        // (generateBin writes them in release_date DESC order)
        if (isFirst) {
            result.latestVersion = ver.version;
            isFirst = false;
        }

        // -- 3. Read parts -> downloadFiles ---------------------------
        for (uint8_t p = 0; p < partCount; p++) {
            uint32_t partBase = partsOffset + SIZEOF_PART * p;
            if (partBase + SIZEOF_PART > f.size) break; // safety

            std::string filename = f.ReadStr(partBase + PART_FILENAME, 256);
            uint8_t     type     = f.ReadU8(partBase + PART_TYPE);

            // type 0 = local file (push filename)
            // type 1 = remote URL (push URL, stored in the filename field)
            if (!filename.empty()) {
                ver.downloadFiles.push_back(filename);
            }
            // type is metadata only -- the value itself is always in filename field
            (void)type;
        }

        result.versions.push_back(ver);
    }

    Debug::Print("BinManager::TryGetVersions: app='%s' versions=%d\n",
                 appId.c_str(), (int32_t)result.versions.size());

    return !result.versions.empty();
}
