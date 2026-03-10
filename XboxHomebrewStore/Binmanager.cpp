#include "BinManager.h"
#include "Debug.h"
#include "WebManager.h"
#include "Main.h"

#include <xtl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

const char *BinManager::BIN_URL      = "https://api.xboxhomebrew.store/api/db";
const char *BinManager::BIN_PATH     = "homebrew.bin";
const char *BinManager::BIN_TMP_PATH = "homebrew_tmp.bin";

static const uint32_t XHBS_MAGIC = 0x53424858;

#define SIZEOF_HEADER   36
#define SIZEOF_APP      740
#define SIZEOF_VERSION  472
#define SIZEOF_PART     260

#define HDR_MAGIC           0
#define HDR_FORMAT_VERSION  4
#define HDR_APP_COUNT       8
#define HDR_VERSION_COUNT   12
#define HDR_PART_COUNT      16

#define APP_ID              0
#define APP_NAME            32
#define APP_AUTHOR          96
#define APP_CATEGORY        160
#define APP_DESCRIPTION     192
#define APP_LATEST_VERSION  704
#define APP_STATE           736

#define VER_ID              0
#define VER_APP_ID          32
#define VER_VERSION         64
#define VER_RELEASE_DATE    96
#define VER_CHANGELOG       112
#define VER_TITLE_ID        368
#define VER_REGION          384
#define VER_FOLDER_NAME     392
#define VER_SIZE            456
#define VER_PART_COUNT      460
#define VER_PARTS_OFFSET    464

#define PART_FILENAME       0
#define PART_TYPE           256
#define PART_NUMBER         257

// ==========================================================================
// BinData -- loads entire file into a malloc'd buffer
// ==========================================================================

struct BinData {
    uint8_t *data;
    uint32_t size;

    BinData() : data(NULL), size(0) {}

    ~BinData() {
        if (data != NULL) { free(data); data = NULL; }
    }

    bool Load(const char *path) {
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            Debug::Print("BinManager: fopen failed for %s\n", path);
            return false;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (len < SIZEOF_HEADER) {
            Debug::Print("BinManager: file too small (%ld bytes)\n", len);
            fclose(f);
            return false;
        }

        data = (uint8_t*)malloc(len);
        if (data == NULL) {
            Debug::Print("BinManager: malloc failed\n");
            fclose(f);
            return false;
        }

        size_t read = fread(data, 1, len, f);
        fclose(f);

        if (read != (size_t)len) {
            Debug::Print("BinManager: fread short\n");
            free(data); data = NULL;
            return false;
        }

        size = (uint32_t)len;
        return true;
    }

    std::string ReadStr(uint32_t offset, uint32_t fieldLen) const {
        if (offset + fieldLen > size) return std::string();
        const char *p = reinterpret_cast<const char *>(data + offset);
        uint32_t len = 0;
        while (len < fieldLen && p[len] != '\0') len++;
        return std::string(p, len);
    }

    uint32_t ReadU32(uint32_t offset) const {
        if (offset + 4 > size) return 0;
        uint32_t v; memcpy(&v, data + offset, 4); return v;
    }

    uint8_t ReadU8(uint32_t offset) const {
        if (offset >= size) return 0;
        return data[offset];
    }
};

// ==========================================================================
// Helpers
// ==========================================================================

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

static bool ValidateMagic(const BinData &f) {
    uint32_t magic = f.ReadU32(HDR_MAGIC);
    if (magic != XHBS_MAGIC) {
        Debug::Print("BinManager: bad magic 0x%08X\n", magic);
        return false;
    }
    if (f.ReadU8(HDR_FORMAT_VERSION) != 1) {
        Debug::Print("BinManager: unsupported format version\n");
        return false;
    }
    return true;
}

static bool AppItemNameLess(const AppItem &a, const AppItem &b) {
    return a.name < b.name;
}

// ==========================================================================
// TryDownloadBin
// ==========================================================================

bool BinManager::TryDownloadBin() {
    Debug::Print("BinManager: downloading %s\n", BIN_URL);

    DeleteFileA(BIN_TMP_PATH);

    bool ok = WebManager::TryDownloadApiData(
        std::string(BIN_URL),
        std::string(BIN_TMP_PATH),
        NULL, NULL, NULL, NULL, NULL);

    if (!ok) {
        Debug::Print("BinManager: download failed\n");
        DeleteFileA(BIN_TMP_PATH);
        return false;
    }

    {
        BinData probe;
        if (!probe.Load(BIN_TMP_PATH) || !ValidateMagic(probe)) {
            Debug::Print("BinManager: validation failed\n");
            DeleteFileA(BIN_TMP_PATH);
            return false;
        }
        uint32_t appCount = probe.ReadU32(HDR_APP_COUNT);
        if (appCount == 0) {
            Debug::Print("BinManager: zero apps in bin\n");
            DeleteFileA(BIN_TMP_PATH);
            return false;
        }
        Debug::Print("BinManager: validated -- %d apps\n", (int)appCount);
    }

    DeleteFileA(BIN_PATH);
    if (!MoveFileA(BIN_TMP_PATH, BIN_PATH)) {
        Debug::Print("BinManager: MoveFile failed (%lu)\n", GetLastError());
        DeleteFileA(BIN_TMP_PATH);
        return false;
    }

    Debug::Print("BinManager: ready\n");
    return true;
}

// ==========================================================================
// IsBinPresent / DeleteBin
// ==========================================================================

bool BinManager::IsBinPresent() {
    FILE *f = fopen(BIN_PATH, "rb");
    if (f == NULL) return false;
    fclose(f);
    return true;
}

void BinManager::DeleteBin() {
    DeleteFileA(BIN_PATH);
}

// ==========================================================================
// TryGetCategories
// ==========================================================================

bool BinManager::TryGetCategories(CategoriesResponse &result) {
    result.clear();

    BinData f;
    if (!f.Load(BIN_PATH) || !ValidateMagic(f)) return false;

    uint32_t appCount = f.ReadU32(HDR_APP_COUNT);
    uint32_t appBase  = SIZEOF_HEADER;

    std::map<std::string, int32_t> catCounts;
    int32_t totalCount = 0;

    for (uint32_t i = 0; i < appCount; i++) {
        uint32_t base  = appBase + SIZEOF_APP * i;
        if (f.ReadU8(base + APP_STATE) != 1) continue;
        std::string cat = f.ReadStr(base + APP_CATEGORY, 32);
        catCounts[cat]++;
        totalCount++;
    }

    CategoryItem allApps;
    allApps.name  = "All Apps";
    allApps.count = totalCount;
    result.push_back(allApps);

    for (std::map<std::string, int32_t>::iterator it = catCounts.begin();
         it != catCounts.end(); ++it) {
        CategoryItem cat;
        cat.name  = it->first;
        cat.count = it->second;
        result.push_back(cat);
    }

    Debug::Print("BinManager::TryGetCategories: %d categories (%d total)\n",
                 (int)result.size(), totalCount);
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

    BinData f;
    if (!f.Load(BIN_PATH) || !ValidateMagic(f)) return false;

    uint32_t appCount = f.ReadU32(HDR_APP_COUNT);
    uint32_t appBase  = SIZEOF_HEADER;

    std::vector<AppItem> matches;

    for (uint32_t i = 0; i < appCount; i++) {
        uint32_t base  = appBase + SIZEOF_APP * i;
        if (f.ReadU8(base + APP_STATE) != 1) continue;

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
        app.latestVersion = f.ReadStr(base + APP_LATEST_VERSION,  32);
        app.state         = (int32_t)f.ReadU8(base + APP_STATE);
        matches.push_back(app);
    }

    std::sort(matches.begin(), matches.end(), AppItemNameLess);

    int32_t total = (int32_t)matches.size();
    int32_t start = offset < total ? offset : total;
    int32_t end   = (start + count) < total ? (start + count) : total;

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

    BinData f;
    if (!f.Load(BIN_PATH) || !ValidateMagic(f)) return false;

    uint32_t appCount     = f.ReadU32(HDR_APP_COUNT);
    uint32_t versionCount = f.ReadU32(HDR_VERSION_COUNT);
    uint32_t appBase      = SIZEOF_HEADER;
    uint32_t verBase      = appBase + SIZEOF_APP * appCount;

    // 1. Find the app record
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

    // 2. Collect versions
    bool isFirst = true;
    for (uint32_t i = 0; i < versionCount; i++) {
        uint32_t    base     = verBase + SIZEOF_VERSION * i;
        std::string verAppId = f.ReadStr(base + VER_APP_ID, 32);
        if (verAppId != appId) continue;

        VersionItem ver;
        ver.id          = f.ReadStr(base + VER_ID,           32);
        ver.version     = f.ReadStr(base + VER_VERSION,      32);
        ver.releaseDate = f.ReadStr(base + VER_RELEASE_DATE,  16);
        ver.changeLog   = f.ReadStr(base + VER_CHANGELOG,    256);
        ver.titleId     = f.ReadStr(base + VER_TITLE_ID,      16);
        ver.region      = f.ReadStr(base + VER_REGION,         8);
        ver.folderName  = f.ReadStr(base + VER_FOLDER_NAME,   64);
        ver.size        = f.ReadU32(base + VER_SIZE);

        uint8_t  partCount   = f.ReadU8(base + VER_PART_COUNT);
        uint32_t partsOffset = f.ReadU32(base + VER_PARTS_OFFSET);

        if (isFirst) {
            result.latestVersion = ver.version;
            isFirst = false;
        }

        // 3. Read parts
        for (uint8_t p = 0; p < partCount; p++) {
            uint32_t partBase = partsOffset + SIZEOF_PART * p;
            if (partBase + SIZEOF_PART > f.size) break;
            std::string filename = f.ReadStr(partBase + PART_FILENAME, 256);
            if (!filename.empty()) {
                ver.downloadFiles.push_back(filename);
            }
        }

        result.versions.push_back(ver);
    }

    Debug::Print("BinManager::TryGetVersions: app='%s' versions=%d\n",
                 appId.c_str(), (int32_t)result.versions.size());

    return !result.versions.empty();
}