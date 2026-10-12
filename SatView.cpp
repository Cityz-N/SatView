// ============================================================================
//  SatView - EuroScope plugin: aerial imagery of France as a map layer
//  Imagery: IGN Geoplateforme (BD ORTHO, layer ORTHOIMAGERY.ORTHOPHOTOS), open service.
//  Credit shown on screen:  (c) IGN - BD ORTHO
//
//  How it works
//   * Works out which map tiles cover your screen, at the detail that matches your zoom.
//   * Tiles are loaded by background threads (never blocks EuroScope): first from a cache
//     folder on your disk, otherwise downloaded and saved there.
//   * While a tile is missing, a blurrier piece of a lower-detail tile is shown instead.
//   * The cache folder is limited in size (default 8 GB); the oldest tiles are deleted first.
//
//  Commands (EuroScope command line, not case-sensitive):
//     .sat on | off | toggle
//     .sat opacity 0-100
//     .sat layer front | back   (front = drawn like the weather layer; back = background stage)
//     .sat gray on | off        (black-and-white imagery, easier to combine with weather colours)
//     .sat maxzoom 10-19        (sharpest tile level that will be used)
//     .sat sharp -1..2          (0 = tile pixels match screen pixels; 1 = sharper, 4x more tiles)
//     .sat cache [GB]           (show cache use, or set the size limit)
//     .sat reload | status
//
//  Version 0.2: the picture is composed once into an off-screen surface and only copied to
//  the screen on every refresh (re-composed only when the view or the tiles change): much faster.
// ============================================================================

#include <afxwin.h>          // MFC (EuroScope's SDK header relies on it)
#include <winhttp.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "EuroScopePlugIn.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "EuroScopePlugInDll.lib")

class CSatApp : public CWinApp {};
static CSatApp theApp;

// ----------------------------------------------------------------------------
//  Small pure helpers (map maths)
// ----------------------------------------------------------------------------
// BEGIN PURE
static const double kPi = 3.14159265358979323846;

static inline int iclampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline uint64_t MakeKey(int z, int x, int y)
{
    return ((uint64_t)z << 58) | ((uint64_t)x << 29) | (uint64_t)y;
}
static inline int KeyZ(uint64_t k) { return (int)(k >> 58); }
static inline int KeyX(uint64_t k) { return (int)((k >> 29) & 0x1FFFFFFFULL); }
static inline int KeyY(uint64_t k) { return (int)(k & 0x1FFFFFFFULL); }

// tile corner (x, y may be fractional) -> longitude / latitude
static double TileToLon(double x, int z) { return x / (double)(1LL << z) * 360.0 - 180.0; }
static double TileToLat(double y, int z)
{
    double n = kPi * (1.0 - 2.0 * y / (double)(1LL << z));
    return std::atan(std::sinh(n)) * 180.0 / kPi;
}
// longitude / latitude -> fractional tile coordinates
static double LonToTileX(double lon, int z) { return (lon + 180.0) / 360.0 * (double)(1LL << z); }
static double LatToTileY(double lat, int z)
{
    double r = lat * kPi / 180.0;
    return (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / kPi) / 2.0 * (double)(1LL << z);
}

static double HaversineM(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371008.8;
    double p1 = lat1 * kPi / 180.0, p2 = lat2 * kPi / 180.0;
    double dp = p2 - p1, dl = (lon2 - lon1) * kPi / 180.0;
    double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2.0 * R * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

// tile level whose pixels are about as big as the screen pixels
static int ChooseZoom(double metresPerScreenPixel, double lat, int sharp, int minZ, int maxZ)
{
    double mppEq = 156543.03392 * std::cos(lat * kPi / 180.0);
    double zf = std::log(mppEq / metresPerScreenPixel) / std::log(2.0);
    return iclampi((int)std::floor(zf + 0.5) + sharp, minZ, maxZ);
}
// END PURE

// ----------------------------------------------------------------------------
//  Settings
// ----------------------------------------------------------------------------
struct Settings {
    std::atomic<int> enabled{ 1 };
    std::atomic<int> opacity{ 45 };
    std::atomic<int> maxZoom{ 17 };
    std::atomic<int> sharp{ 0 };
    std::atomic<int> layerBack{ 0 };       // 0 = BEFORE_TAGS ("front"), 1 = BACK_BITMAP
    std::atomic<int> gray{ 0 };
    std::atomic<int> fast{ 0 };            // 1 = faster, slightly rougher tile scaling
    std::atomic<int> threads{ 3 };
    std::atomic<long long> cacheBytes{ 8LL * 1024 * 1024 * 1024 };
    std::atomic<int> version{ 0 };         // bumped when drawing settings change
};
static Settings g_set;
static std::string g_iniPath, g_cacheDir, g_layer = "ORTHOIMAGERY.ORTHOPHOTOS", g_tms = "PM_0_19";
static std::mutex g_cfgMtx;

static std::string CfgGet(const std::string& s) { std::lock_guard<std::mutex> l(g_cfgMtx); return s; }

static std::string DllFolder()
{
    HMODULE hm = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&DllFolder, &hm);
    char p[MAX_PATH] = {};
    GetModuleFileNameA(hm, p, MAX_PATH);
    std::string s(p);
    size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : s.substr(0, slash + 1);
}

static void LoadIni()
{
    const char* f = g_iniPath.c_str();
    g_set.enabled = GetPrivateProfileIntA("Satellite", "Enabled", 1, f) ? 1 : 0;
    g_set.opacity = iclampi(GetPrivateProfileIntA("Satellite", "Opacity", 45, f), 0, 100);
    g_set.maxZoom = iclampi(GetPrivateProfileIntA("Satellite", "MaxZoom", 17, f), 10, 19);
    g_set.sharp = iclampi(GetPrivateProfileIntA("Satellite", "Sharpness", 0, f), -1, 2);
    g_set.gray = GetPrivateProfileIntA("Satellite", "Grayscale", 0, f) ? 1 : 0;
    g_set.threads = iclampi(GetPrivateProfileIntA("Satellite", "Threads", 3, f), 1, 6);
    {
        char qb[32] = {};
        GetPrivateProfileStringA("Satellite", "Quality", "good", qb, sizeof(qb), f);
        std::string q = qb;
        for (char& ch : q) ch = (char)tolower((unsigned char)ch);
        g_set.fast = (q == "fast") ? 1 : 0;
    }
    ++g_set.version;
    char b[512] = {};
    GetPrivateProfileStringA("Satellite", "Layer", "front", b, sizeof(b), f);
    std::string lay = b;
    for (char& ch : lay) ch = (char)tolower((unsigned char)ch);
    g_set.layerBack = (lay == "back") ? 1 : 0;
    GetPrivateProfileStringA("Satellite", "MaxCacheGB", "8", b, sizeof(b), f);
    double gb = atof(b);
    if (gb < 0.5) gb = 0.5;
    if (gb > 200.0) gb = 200.0;
    g_set.cacheBytes = (long long)(gb * 1024.0 * 1024.0 * 1024.0);

    std::string dir, layer, tms;
    GetPrivateProfileStringA("Satellite", "CacheDir", "", b, sizeof(b), f);
    dir = b;
    GetPrivateProfileStringA("Satellite", "WmtsLayer", "ORTHOIMAGERY.ORTHOPHOTOS", b, sizeof(b), f);
    layer = b;
    GetPrivateProfileStringA("Satellite", "TileMatrixSet", "PM_0_19", b, sizeof(b), f);
    tms = b;
    if (dir.empty()) dir = DllFolder() + "SatCache";
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    {
        std::lock_guard<std::mutex> l(g_cfgMtx);
        g_cacheDir = dir; g_layer = layer; g_tms = tms;
    }
}

static void SaveIni()
{
    const char* f = g_iniPath.c_str();
    char b[64];
    sprintf_s(b, "%d", (int)g_set.enabled);  WritePrivateProfileStringA("Satellite", "Enabled", b, f);
    sprintf_s(b, "%d", (int)g_set.opacity);  WritePrivateProfileStringA("Satellite", "Opacity", b, f);
    sprintf_s(b, "%d", (int)g_set.maxZoom);  WritePrivateProfileStringA("Satellite", "MaxZoom", b, f);
    sprintf_s(b, "%d", (int)g_set.sharp);    WritePrivateProfileStringA("Satellite", "Sharpness", b, f);
    sprintf_s(b, "%d", (int)g_set.gray);     WritePrivateProfileStringA("Satellite", "Grayscale", b, f);
    WritePrivateProfileStringA("Satellite", "Layer", g_set.layerBack ? "back" : "front", f);
    WritePrivateProfileStringA("Satellite", "Quality", g_set.fast ? "fast" : "good", f);
    sprintf_s(b, "%.1f", (double)g_set.cacheBytes / (1024.0 * 1024.0 * 1024.0));
    WritePrivateProfileStringA("Satellite", "MaxCacheGB", b, f);
}

// ----------------------------------------------------------------------------
//  Shared state: memory cache of decoded tiles, wish list, disk-cache index
// ----------------------------------------------------------------------------
typedef std::shared_ptr<Gdiplus::Bitmap> BmpPtr;

struct MemTile { BmpPtr bmp; uint64_t used = 0; };
static std::unordered_map<uint64_t, MemTile> g_mem;
static std::mutex g_memMtx;
static uint64_t g_useCounter = 0;
static std::atomic<uint64_t> g_tileVersion{ 0 };      // changes whenever a tile arrives
static std::atomic<int> g_renderUs{ 0 }, g_blitUs{ 0 };   // timing shown by .sat status
static const size_t kMemMax = 420;                 // decoded tiles kept in memory (about 110 MB)

static BmpPtr MemGet(uint64_t key)
{
    std::lock_guard<std::mutex> l(g_memMtx);
    auto it = g_mem.find(key);
    if (it == g_mem.end()) return nullptr;
    it->second.used = ++g_useCounter;
    return it->second.bmp;
}

static bool MemHas(uint64_t key)
{
    std::lock_guard<std::mutex> l(g_memMtx);
    return g_mem.find(key) != g_mem.end();
}

static void MemPut(uint64_t key, BmpPtr b)
{
    std::lock_guard<std::mutex> l(g_memMtx);
    MemTile t; t.bmp = b; t.used = ++g_useCounter;
    g_mem[key] = t;
    ++g_tileVersion;
    while (g_mem.size() > kMemMax) {
        auto oldest = g_mem.begin();
        for (auto it = g_mem.begin(); it != g_mem.end(); ++it)
            if (it->second.used < oldest->second.used) oldest = it;
        g_mem.erase(oldest);
    }
}

static void MemClear()
{
    std::lock_guard<std::mutex> l(g_memMtx);
    g_mem.clear();
}

static size_t MemCount() { std::lock_guard<std::mutex> l(g_memMtx); return g_mem.size(); }

// what the screens would like to have, closest to the centre first
static std::vector<uint64_t> g_wanted;
static std::set<uint64_t> g_inflight;
static std::unordered_map<uint64_t, DWORD> g_failed;
static std::mutex g_wantMtx;
static std::condition_variable g_wantCv;
static std::string g_lastError;

static std::atomic<bool> g_stop{ false };
static std::atomic<bool> g_newTiles{ false };

// disk cache index (key -> time, size) so that the oldest tiles can be removed
struct CEnt { uint64_t time; uint32_t size; };
static std::unordered_map<uint64_t, CEnt> g_idx;
static long long g_idxBytes = 0;
static std::mutex g_idxMtx;
static std::atomic<int> g_added{ 0 };
static std::atomic<bool> g_scanned{ false };

static uint64_t NowFileTime()
{
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static std::string TilePath(uint64_t key)
{
    char b[96];
    sprintf_s(b, "\\%d\\%d\\%d.jpg", KeyZ(key), KeyX(key), KeyY(key));
    return CfgGet(g_cacheDir) + b;
}

static void MakeDirs(const std::string& dir)
{
    for (size_t i = 3; i < dir.size(); ++i)
        if (dir[i] == '\\' || dir[i] == '/') CreateDirectoryA(dir.substr(0, i).c_str(), NULL);
    CreateDirectoryA(dir.c_str(), NULL);
}

static bool ReadFileBytes(const std::string& path, std::vector<unsigned char>& out)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 4 * 1024 * 1024) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

static bool WriteFileBytes(const std::string& path, const std::vector<unsigned char>& data)
{
    size_t slash = path.find_last_of('\\');
    if (slash != std::string::npos) MakeDirs(path.substr(0, slash));
    std::string tmp = path + ".tmp";
    FILE* f = nullptr;
    if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f) return false;
    size_t put = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    if (put != data.size()) { DeleteFileA(tmp.c_str()); return false; }
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp.c_str()); return false; }
    return true;
}

// ----------------------------------------------------------------------------
//  HTTP client with a kept-open connection (fast, one per loader thread)
// ----------------------------------------------------------------------------
struct Http {
    HINTERNET ses = nullptr, con = nullptr;
    ~Http() { Close(); }
    void Close()
    {
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
        con = ses = nullptr;
    }
    bool Open()
    {
        ses = WinHttpOpen(L"EuroScope-SatView/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) return false;
        WinHttpSetTimeouts(ses, 5000, 5000, 10000, 20000);
        con = WinHttpConnect(ses, L"data.geopf.fr", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!con) { Close(); return false; }
        return true;
    }
    // returns the HTTP status (0 = network problem); the body is filled for status 200
    int Get(const std::wstring& pathAndQuery, std::vector<unsigned char>& out)
    {
        out.clear();
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!con && !Open()) return 0;
            HINTERNET req = WinHttpOpenRequest(con, L"GET", pathAndQuery.c_str(), NULL, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (!req) { Close(); continue; }
            bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, NULL);
            if (!ok) { WinHttpCloseHandle(req); Close(); continue; }
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
            bool readOk = true;
            if (status == 200) {
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail)) { readOk = false; break; }
                    if (avail == 0) break;
                    size_t old = out.size();
                    out.resize(old + avail);
                    DWORD got = 0;
                    if (!WinHttpReadData(req, &out[old], avail, &got)) { readOk = false; break; }
                    out.resize(old + got);
                    if (out.size() > 4 * 1024 * 1024) { readOk = false; break; }
                }
            }
            WinHttpCloseHandle(req);
            if (status == 200 && !readOk) { Close(); continue; }
            return (int)status;
        }
        return 0;
    }
};

// ----------------------------------------------------------------------------
//  Decode a JPEG/PNG into a ready-to-draw bitmap
// ----------------------------------------------------------------------------
static BmpPtr DecodeTile(const std::vector<unsigned char>& data)
{
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!hg) return nullptr;
    void* mem = GlobalLock(hg);
    memcpy(mem, data.data(), data.size());
    GlobalUnlock(hg);
    IStream* st = nullptr;
    if (CreateStreamOnHGlobal(hg, TRUE, &st) != S_OK) { GlobalFree(hg); return nullptr; }

    BmpPtr result;
    {
        Gdiplus::Bitmap src(st);
        if (src.GetLastStatus() == Gdiplus::Ok && src.GetWidth() >= 16 && src.GetWidth() <= 1024 &&
            src.GetHeight() >= 16 && src.GetHeight() <= 1024) {
            // clone forces the decoding now (in the loader thread) into the fastest drawing format
            Gdiplus::Bitmap* c = src.Clone(0, 0, (INT)src.GetWidth(), (INT)src.GetHeight(), PixelFormat32bppPARGB);
            if (c && c->GetLastStatus() == Gdiplus::Ok) result.reset(c);
            else delete c;
        }
    }
    st->Release();
    return result;
}

// ----------------------------------------------------------------------------
//  Loader threads: disk cache first, otherwise download
// ----------------------------------------------------------------------------
static void SetLastError(const std::string& s)
{
    std::lock_guard<std::mutex> l(g_wantMtx);
    g_lastError = s;
}

static void LoadOne(uint64_t key, Http& http)
{
    std::vector<unsigned char> data;
    BmpPtr bmp;

    std::string path = TilePath(key);
    if (ReadFileBytes(path, data)) {
        bmp = DecodeTile(data);
        if (bmp) {
            std::lock_guard<std::mutex> l(g_idxMtx);
            auto it = g_idx.find(key);
            if (it != g_idx.end()) it->second.time = NowFileTime();   // used just now
        }
        else {
            DeleteFileA(path.c_str());                                // damaged file
        }
    }
    if (!bmp) {
        char q[512];
        sprintf_s(q, "/wmts?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=%s&STYLE=normal&TILEMATRIXSET=%s"
            "&TILEMATRIX=%d&TILEROW=%d&TILECOL=%d&FORMAT=image%%2Fjpeg",
            CfgGet(g_layer).c_str(), CfgGet(g_tms).c_str(), KeyZ(key), KeyY(key), KeyX(key));
        int status = http.Get(std::wstring(q, q + strlen(q)), data);
        bool looksImage = data.size() > 200 &&
            ((data[0] == 0xFF && data[1] == 0xD8) || (data[0] == 0x89 && data[1] == 'P'));
        if (status == 200 && looksImage) {
            bmp = DecodeTile(data);
            if (bmp && WriteFileBytes(path, data)) {
                std::lock_guard<std::mutex> l(g_idxMtx);
                auto it = g_idx.find(key);
                if (it != g_idx.end()) g_idxBytes -= it->second.size;
                g_idx[key] = CEnt{ NowFileTime(), (uint32_t)data.size() };
                g_idxBytes += (long long)data.size();
                ++g_added;
            }
            Sleep(25);                                               // be polite to the server
        }
        else {
            char m[160];
            if (status == 0) sprintf_s(m, "no connection to the imagery server");
            else if (status == 200) sprintf_s(m, "the server returned something that is not an image");
            else sprintf_s(m, "imagery server answered HTTP %d (z%d)", status, KeyZ(key));
            SetLastError(m);
        }
    }

    if (bmp) {
        MemPut(key, bmp);
        g_newTiles = true;
    }
    else {
        std::lock_guard<std::mutex> l(g_wantMtx);
        g_failed[key] = GetTickCount();
    }
}

static void LoaderMain()
{
    Http http;
    while (!g_stop) {
        uint64_t key = 0;
        bool have = false;
        {
            std::unique_lock<std::mutex> l(g_wantMtx);
            DWORD now = GetTickCount();
            for (uint64_t k : g_wanted) {
                if (g_inflight.count(k) || MemHas(k)) continue;
                auto f = g_failed.find(k);
                if (f != g_failed.end() && now - f->second < 60000) continue;   // do not hammer failures
                key = k; have = true;
                g_inflight.insert(k);
                break;
            }
            if (!have) g_wantCv.wait_for(l, std::chrono::milliseconds(300));
        }
        if (!have) continue;
        LoadOne(key, http);
        {
            std::lock_guard<std::mutex> l(g_wantMtx);
            g_inflight.erase(key);
        }
    }
}

// ----------------------------------------------------------------------------
//  Cache folder: scan at start, remove the oldest tiles when above the size limit
// ----------------------------------------------------------------------------
static void ScanCache()
{
    std::unordered_map<uint64_t, CEnt> found;
    long long bytes = 0;
    std::string root = CfgGet(g_cacheDir);
    WIN32_FIND_DATAA fz;
    HANDLE hz = FindFirstFileA((root + "\\*").c_str(), &fz);
    if (hz != INVALID_HANDLE_VALUE) {
        do {
            if (!(fz.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fz.cFileName[0] == '.') continue;
            int z = atoi(fz.cFileName);
            if (z < 0 || z > 19) continue;
            WIN32_FIND_DATAA fx;
            HANDLE hx = FindFirstFileA((root + "\\" + fz.cFileName + "\\*").c_str(), &fx);
            if (hx == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fx.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fx.cFileName[0] == '.') continue;
                int x = atoi(fx.cFileName);
                WIN32_FIND_DATAA fy;
                HANDLE hy = FindFirstFileA((root + "\\" + fz.cFileName + "\\" + fx.cFileName + "\\*.jpg").c_str(), &fy);
                if (hy == INVALID_HANDLE_VALUE) continue;
                do {
                    if (fy.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    int y = atoi(fy.cFileName);
                    uint64_t t = ((uint64_t)fy.ftLastWriteTime.dwHighDateTime << 32) | fy.ftLastWriteTime.dwLowDateTime;
                    found[MakeKey(z, x, y)] = CEnt{ t, (uint32_t)fy.nFileSizeLow };
                    bytes += (long long)fy.nFileSizeLow;
                } while (FindNextFileA(hy, &fy) && !g_stop);
                FindClose(hy);
            } while (FindNextFileA(hx, &fx) && !g_stop);
            FindClose(hx);
        } while (FindNextFileA(hz, &fz) && !g_stop);
        FindClose(hz);
    }
    std::lock_guard<std::mutex> l(g_idxMtx);
    for (auto& kv : found) {                      // merge with tiles added while scanning
        if (g_idx.find(kv.first) == g_idx.end()) { g_idx[kv.first] = kv.second; g_idxBytes += kv.second.size; }
    }
    g_scanned = true;
}

static void TrimCache()
{
    long long cap = g_set.cacheBytes;
    std::vector<std::pair<uint64_t, uint64_t>> order;      // (time, key)
    {
        std::lock_guard<std::mutex> l(g_idxMtx);
        if (g_idxBytes <= cap) return;
        order.reserve(g_idx.size());
        for (auto& kv : g_idx) order.push_back(std::make_pair(kv.second.time, kv.first));
    }
    std::sort(order.begin(), order.end());
    long long target = cap / 10 * 9;
    for (auto& e : order) {
        {
            std::lock_guard<std::mutex> l(g_idxMtx);
            if (g_idxBytes <= target) break;
        }
        if (g_stop) break;
        uint64_t key = e.second;
        DeleteFileA(TilePath(key).c_str());
        std::lock_guard<std::mutex> l(g_idxMtx);
        auto it = g_idx.find(key);
        if (it != g_idx.end()) { g_idxBytes -= it->second.size; g_idx.erase(it); }
    }
}

static void MaintenanceMain()
{
    ScanCache();
    int idle = 0;
    while (!g_stop) {
        Sleep(1000);
        if (++idle >= 20 || g_added > 300) {
            idle = 0; g_added = 0;
            TrimCache();
        }
    }
}

// ----------------------------------------------------------------------------
//  Off-screen surface: the composed imagery lives here and is copied to the screen
// ----------------------------------------------------------------------------
static double NowUs()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000000.0 / (double)f.QuadPart;
}

struct Surface {
    HBITMAP hbm = nullptr;
    HDC dc = nullptr;
    HGDIOBJ old = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;
    Gdiplus::Bitmap* gb = nullptr;

    ~Surface() { Free(); }
    bool ok() const { return gb != nullptr; }
    void Free()
    {
        delete gb; gb = nullptr;
        if (dc) { SelectObject(dc, old); DeleteDC(dc); dc = nullptr; }
        if (hbm) { DeleteObject(hbm); hbm = nullptr; }
        bits = nullptr; w = h = 0;
    }
    bool Make(int W, int H)
    {
        Free();
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;                 // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        hbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!hbm || !bits) { Free(); return false; }
        dc = CreateCompatibleDC(NULL);
        old = SelectObject(dc, hbm);
        gb = new Gdiplus::Bitmap(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        if (gb->GetLastStatus() != Gdiplus::Ok) { Free(); return false; }
        w = W; h = H;
        return true;
    }
};

// ----------------------------------------------------------------------------
//  Radar screen
// ----------------------------------------------------------------------------
class CSatScreen;
static std::vector<CSatScreen*> g_screens;      // only touched from EuroScope's own thread

class CSatScreen : public EuroScopePlugIn::CRadarScreen
{
public:
    CSatScreen(bool geoReferenced) : m_geo(geoReferenced) { g_screens.push_back(this); }
    virtual ~CSatScreen()
    {
        g_screens.erase(std::remove(g_screens.begin(), g_screens.end(), this), g_screens.end());
    }

    virtual void OnAsrContentToBeClosed(void) override { delete this; }

    virtual void OnRefresh(HDC hDC, int Phase) override
    {
        if (!m_geo || !g_set.enabled) return;
        int want = g_set.layerBack ? EuroScopePlugIn::REFRESH_PHASE_BACK_BITMAP
            : EuroScopePlugIn::REFRESH_PHASE_BEFORE_TAGS;
        if (Phase == want) DrawImagery(hDC);
        if (Phase == EuroScopePlugIn::REFRESH_PHASE_AFTER_LISTS) DrawCredit(hDC);
    }

private:
    bool m_geo;
    int m_pending = 0;            // tiles missing in the last composition (for the credit line)
    Surface m_surf;
    RECT m_rc = { 0, 0, 0, 0 };
    double m_sig[6] = { 0, 0, 0, 0, 0, 0 };
    uint64_t m_tv = ~0ULL;
    int m_sv = -1;

    struct Item {
        uint64_t key;
        int tx, ty;
        POINT p00, p10, p01;
        double dist;
    };

    void Signature(const RECT& rc, double sig[6])
    {
        POINT p0 = { rc.left, rc.top }, p1 = { rc.right, rc.bottom };
        POINT p2 = { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
        EuroScopePlugIn::CPosition a = ConvertCoordFromPixelToPosition(p0);
        EuroScopePlugIn::CPosition b = ConvertCoordFromPixelToPosition(p1);
        EuroScopePlugIn::CPosition c = ConvertCoordFromPixelToPosition(p2);
        sig[0] = a.m_Latitude; sig[1] = a.m_Longitude;
        sig[2] = b.m_Latitude; sig[3] = b.m_Longitude;
        sig[4] = c.m_Latitude; sig[5] = c.m_Longitude;
    }

    static bool SameSig(const double a[6], const double b[6])
    {
        for (int i = 0; i < 6; ++i) if (std::fabs(a[i] - b[i]) > 1e-9) return false;
        return true;
    }

    // Called on every refresh. Cheap unless the view, the tiles or the settings changed.
    void DrawImagery(HDC hDC)
    {
        RECT rc = GetRadarArea();
        int W = rc.right - rc.left, H = rc.bottom - rc.top;
        if (W < 50 || H < 50) return;

        double sig[6];
        Signature(rc, sig);
        uint64_t tv = g_tileVersion;                // read BEFORE composing so a tile arriving meanwhile is not missed
        int sv = g_set.version;
        bool valid = m_surf.ok() && m_surf.w == W && m_surf.h == H && EqualRect(&rc, &m_rc) &&
            SameSig(sig, m_sig) && tv == m_tv && sv == m_sv;
        if (!valid) {
            double t0 = NowUs();
            Compose(rc);
            g_renderUs = (int)(NowUs() - t0);
            m_rc = rc;
            memcpy(m_sig, sig, sizeof(sig));
            m_tv = tv;
            m_sv = sv;
        }
        if (!m_surf.ok()) return;

        int op = iclampi((int)g_set.opacity, 0, 100);
        if (op == 0) return;
        double t1 = NowUs();
        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = (BYTE)(op * 255 / 100);   // the opacity is applied here: changing it costs nothing
        bf.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(hDC, rc.left, rc.top, W, H, m_surf.dc, 0, 0, W, H, bf);
        g_blitUs = (int)(NowUs() - t1);
    }

    // Draws all visible tiles into the off-screen surface (fully opaque; opacity is applied when copying)
    void Compose(const RECT& rc)
    {
        int W = rc.right - rc.left, H = rc.bottom - rc.top;
        if (!m_surf.ok() || m_surf.w != W || m_surf.h != H) {
            if (!m_surf.Make(W, H)) return;
        }
        memset(m_surf.bits, 0, (size_t)W * (size_t)H * 4);

        // ---- how many metres is one screen pixel?
        POINT c = { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
        POINT pa = { c.x - 200, c.y }, pb = { c.x + 200, c.y };
        EuroScopePlugIn::CPosition la = ConvertCoordFromPixelToPosition(pa);
        EuroScopePlugIn::CPosition lb = ConvertCoordFromPixelToPosition(pb);
        double dist = HaversineM(la.m_Latitude, la.m_Longitude, lb.m_Latitude, lb.m_Longitude);
        double mpp = dist / 400.0;
        if (!(mpp > 0.01) || !(mpp < 200000.0)) return;
        double latc = (la.m_Latitude + lb.m_Latitude) / 2.0;
        int maxZ = (int)g_set.maxZoom;
        int z = ChooseZoom(mpp, latc, (int)g_set.sharp, 5, maxZ);

        // ---- which tiles cover the screen? (sample points over the screen, then a bounding box)
        std::vector<Item> items;
        for (;;) {
            double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
            for (int j = 0; j <= 4; ++j) {
                for (int i = 0; i <= 8; ++i) {
                    POINT p = { rc.left + (W * i) / 8, rc.top + (H * j) / 4 };
                    EuroScopePlugIn::CPosition ps = ConvertCoordFromPixelToPosition(p);
                    double tx = LonToTileX(ps.m_Longitude, z), ty = LatToTileY(ps.m_Latitude, z);
                    if (tx < minX) minX = tx;
                    if (tx > maxX) maxX = tx;
                    if (ty < minY) minY = ty;
                    if (ty > maxY) maxY = ty;
                }
            }
            int x0 = (int)std::floor(minX), x1 = (int)std::floor(maxX);
            int y0 = (int)std::floor(minY), y1 = (int)std::floor(maxY);
            long long limit = (1LL << z) - 1;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > limit) x1 = (int)limit;
            if (y1 > limit) y1 = (int)limit;
            if ((long long)(x1 - x0 + 1) * (y1 - y0 + 1) > 140 && z > 5) { --z; continue; }

            double cx = (minX + maxX) / 2.0, cy = (minY + maxY) / 2.0;
            items.clear();
            for (int ty = y0; ty <= y1; ++ty) {
                for (int tx = x0; tx <= x1; ++tx) {
                    Item it;
                    it.key = MakeKey(z, tx, ty);
                    it.tx = tx; it.ty = ty;
                    it.p00 = Corner(tx, ty, z);
                    it.p10 = Corner(tx + 1, ty, z);
                    it.p01 = Corner(tx, ty + 1, z);
                    POINT p11 = Corner(tx + 1, ty + 1, z);
                    int l = it.p00.x, r = it.p00.x, t = it.p00.y, b = it.p00.y;
                    const POINT* ps[3] = { &it.p10, &it.p01, &p11 };
                    for (int k = 0; k < 3; ++k) {
                        if (ps[k]->x < l) l = ps[k]->x;
                        if (ps[k]->x > r) r = ps[k]->x;
                        if (ps[k]->y < t) t = ps[k]->y;
                        if (ps[k]->y > b) b = ps[k]->y;
                    }
                    if (r < rc.left || l > rc.right || b < rc.top || t > rc.bottom) continue;   // off screen
                    it.dist = (tx + 0.5 - cx) * (tx + 0.5 - cx) + (ty + 0.5 - cy) * (ty + 0.5 - cy);
                    items.push_back(it);
                }
            }
            break;
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.dist < b.dist; });

        // ---- drawing options: black-and-white if wanted, no seams between tiles
        Gdiplus::ImageAttributes attrs;
        if (g_set.gray) {
            Gdiplus::ColorMatrix cm;
            memset(&cm, 0, sizeof(cm));
            for (int r = 0; r < 3; ++r) { cm.m[0][r] = 0.30f; cm.m[1][r] = 0.59f; cm.m[2][r] = 0.11f; }
            cm.m[3][3] = 1.0f;
            cm.m[4][4] = 1.0f;
            attrs.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
        }
        attrs.SetWrapMode(Gdiplus::WrapModeTileFlipXY);

        Gdiplus::Graphics gr(m_surf.gb);
        gr.SetInterpolationMode(g_set.fast ? Gdiplus::InterpolationModeLowQuality : Gdiplus::InterpolationModeBilinear);
        gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        std::vector<uint64_t> wish;
        int missing = 0;
        for (const Item& it : items) {
            // positions relative to the surface
            Gdiplus::Point pts[3] = { Gdiplus::Point(it.p00.x - rc.left, it.p00.y - rc.top),
                                      Gdiplus::Point(it.p10.x - rc.left, it.p10.y - rc.top),
                                      Gdiplus::Point(it.p01.x - rc.left, it.p01.y - rc.top) };
            BmpPtr bmp = MemGet(it.key);
            if (bmp) {
                gr.DrawImage(bmp.get(), pts, 3, 0, 0, (INT)bmp->GetWidth(), (INT)bmp->GetHeight(),
                    Gdiplus::UnitPixel, &attrs);
                continue;
            }
            ++missing;
            wish.push_back(it.key);
            for (int d = 1; d <= 4 && z - d >= 0; ++d) {            // blurrier stand-in from a coarser tile
                BmpPtr par = MemGet(MakeKey(z - d, it.tx >> d, it.ty >> d));
                if (!par) continue;
                int w = (int)par->GetWidth(), h = (int)par->GetHeight();
                int sw = w >> d, sh = h >> d;
                if (sw < 1 || sh < 1) break;
                int sx = (it.tx & ((1 << d) - 1)) * sw, sy = (it.ty & ((1 << d) - 1)) * sh;
                gr.DrawImage(par.get(), pts, 3, sx, sy, sw, sh, Gdiplus::UnitPixel, &attrs);
                break;
            }
        }
        m_pending = missing;
        PublishWishes(wish);
    }

    POINT Corner(int tx, int ty, int z)
    {
        EuroScopePlugIn::CPosition pos;
        pos.m_Latitude = TileToLat((double)ty, z);
        pos.m_Longitude = TileToLon((double)tx, z);
        return ConvertCoordFromPositionToPixel(pos);
    }

    static void PublishWishes(const std::vector<uint64_t>& wish)
    {
        std::lock_guard<std::mutex> l(g_wantMtx);
        if (wish != g_wanted) { g_wanted = wish; g_wantCv.notify_all(); }
    }

    void DrawCredit(HDC hDC)
    {
        RECT rc = GetRadarArea();
        HGDIOBJ oldFont = SelectObject(hDC, GetStockObject(DEFAULT_GUI_FONT));
        int oldBk = SetBkMode(hDC, TRANSPARENT);
        COLORREF oldCol = SetTextColor(hDC, RGB(215, 215, 215));
        std::string s = "Imagery \xA9 IGN - BD ORTHO";
        if (m_pending > 0) { char b[48]; sprintf_s(b, "  (loading %d tiles)", m_pending); s += b; }
        {
            std::string err;
            { std::lock_guard<std::mutex> l(g_wantMtx); err = g_lastError; }
            if (m_pending > 0 && !err.empty()) s += "  -  " + err;
        }
        TextOutA(hDC, rc.left + 10, rc.bottom - 26, s.c_str(), (int)s.size());
        SetTextColor(hDC, oldCol);
        SetBkMode(hDC, oldBk);
        SelectObject(hDC, oldFont);
    }
};

// ----------------------------------------------------------------------------
//  The plugin
// ----------------------------------------------------------------------------
static std::vector<std::thread> g_threads;

class CSatPlugIn : public EuroScopePlugIn::CPlugIn
{
public:
    CSatPlugIn()
        : EuroScopePlugIn::CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE,
            "SatView", "0.2.0", "SatView", "Free for VATSIM use")
    {
        g_stop = false;
        MakeDirs(CfgGet(g_cacheDir));
        g_threads.emplace_back(MaintenanceMain);
        for (int i = 0; i < (int)g_set.threads; ++i) g_threads.emplace_back(LoaderMain);
        Say("Loaded. Type .sat status for help. Imagery (c) IGN.");
    }

    virtual ~CSatPlugIn()
    {
        g_stop = true;
        g_wantCv.notify_all();
        for (auto& t : g_threads) if (t.joinable()) t.join();
        g_threads.clear();
    }

    virtual EuroScopePlugIn::CRadarScreen* OnRadarScreenCreated(const char* sDisplayName,
        bool NeedRadarContent, bool GeoReferenced, bool CanBeSaved, bool CanBeCreated) override
    {
        return new CSatScreen(GeoReferenced);
    }

    // called once a second by EuroScope: redraw the screens when new tiles have arrived
    virtual void OnTimer(int Counter) override
    {
        if (g_newTiles.exchange(false))
            for (CSatScreen* s : g_screens) s->RequestRefresh();
    }

    virtual bool OnCompileCommand(const char* sCommandLine) override
    {
        std::string c = sCommandLine ? sCommandLine : "";
        for (char& ch : c) ch = (char)toupper((unsigned char)ch);
        if (c.compare(0, 4, ".SAT") != 0) return false;
        if (c.size() > 4 && c[4] != ' ') return false;

        std::string arg = c.size() > 5 ? c.substr(5) : "";
        std::string word = arg.substr(0, arg.find(' '));
        std::string rest = arg.find(' ') == std::string::npos ? "" : arg.substr(arg.find(' ') + 1);
        int n = atoi(rest.c_str());

        if (word == "ON") { g_set.enabled = 1; SaveIni(); Say("imagery ON"); }
        else if (word == "OFF") { g_set.enabled = 0; SaveIni(); Say("imagery OFF"); }
        else if (word == "TOGGLE") { g_set.enabled = g_set.enabled ? 0 : 1; SaveIni(); Say(g_set.enabled ? "imagery ON" : "imagery OFF"); }
        else if (word == "OPACITY") { g_set.opacity = iclampi(n, 0, 100); SaveIni(); Say("opacity " + std::to_string((int)g_set.opacity)); }
        else if (word == "QUALITY") {
            if (rest.find("FAST") == 0) { g_set.fast = 1; ++g_set.version; SaveIni(); Say("quality: fast (rougher scaling, lighter)"); }
            else if (rest.find("GOOD") == 0) { g_set.fast = 0; ++g_set.version; SaveIni(); Say("quality: good"); }
            else Say("usage: .sat quality good | fast");
        }
        else if (word == "LAYER") {
            if (rest.find("BACK") == 0) { g_set.layerBack = 1; SaveIni(); Say("layer: background stage (below the sector lines, if EuroScope allows it)"); }
            else if (rest.find("FRONT") == 0) { g_set.layerBack = 0; SaveIni(); Say("layer: front (same stage as the weather layer)"); }
            else Say("usage: .sat layer front | back");
        }
        else if (word == "GRAY" || word == "GREY") {
            g_set.gray = (rest.find("ON") == 0 || rest.find("1") == 0) ? 1 : 0;
            ++g_set.version; SaveIni(); Say(g_set.gray ? "black-and-white imagery" : "colour imagery");
        }
        else if (word == "MAXZOOM") { g_set.maxZoom = iclampi(n, 10, 19); ++g_set.version; SaveIni(); Say("maximum tile level " + std::to_string((int)g_set.maxZoom)); }
        else if (word == "SHARP") { g_set.sharp = iclampi(n, -1, 2); ++g_set.version; SaveIni(); Say("sharpness " + std::to_string((int)g_set.sharp)); }
        else if (word == "RELOAD") { LoadIni(); ++g_set.version; Say("settings reloaded from SatView.ini"); }
        else if (word == "CACHE" && n > 0) {
            g_set.cacheBytes = (long long)n * 1024LL * 1024LL * 1024LL; SaveIni();
            Say("cache limit " + std::to_string(n) + " GB");
        }
        else {
            long long used; size_t tiles;
            { std::lock_guard<std::mutex> l(g_idxMtx); used = g_idxBytes; tiles = g_idx.size(); }
            char b[256];
            sprintf_s(b, "cache %.2f of %.1f GB (%u tiles on disk%s), %u tiles in memory",
                used / 1073741824.0, (double)g_set.cacheBytes / 1073741824.0, (unsigned)tiles,
                g_scanned ? "" : ", still counting", (unsigned)MemCount());
            Say(b);
            if (word != "CACHE") {
                std::string err;
                { std::lock_guard<std::mutex> l(g_wantMtx); err = g_lastError; }
                Say(std::string("status: ") + (g_set.enabled ? "on" : "off") +
                    ", opacity " + std::to_string((int)g_set.opacity) +
                    ", max level " + std::to_string((int)g_set.maxZoom) +
                    ", sharpness " + std::to_string((int)g_set.sharp) +
                    ", layer " + (g_set.layerBack ? "back" : "front") +
                    ", " + (g_set.gray ? "black-and-white" : "colour") +
                    ", quality " + (g_set.fast ? "fast" : "good") +
                    ", last composing " + std::to_string((int)g_renderUs / 1000) + " ms, copy to screen " +
                    std::to_string((int)g_blitUs / 1000) + " ms" +
                    (err.empty() ? "" : ", last problem: " + err));
                Say("commands: .sat on|off|toggle | opacity 0-100 | layer front|back | gray on|off | "
                    "maxzoom 10-19 | sharp -1..2 | quality good|fast | cache [GB] | reload");
            }
        }
        return true;
    }

private:
    void Say(const std::string& msg)
    {
        DisplayUserMessage("SatView", "Imagery", msg.c_str(), true, true, true, false, false);
    }
};

// ----------------------------------------------------------------------------
//  Entry points that EuroScope looks for
// ----------------------------------------------------------------------------
static CSatPlugIn* g_plugin = nullptr;
static ULONG_PTR g_gdiToken = 0;

void __declspec(dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&g_gdiToken, &in, NULL);

    g_iniPath = DllFolder() + "SatView.ini";
    LoadIni();
    if (GetFileAttributesA(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SaveIni();
        WritePrivateProfileStringA("Satellite", "CacheDir", "", g_iniPath.c_str());
        WritePrivateProfileStringA("Satellite", "Threads", "3", g_iniPath.c_str());
        WritePrivateProfileStringA("Satellite", "WmtsLayer", "ORTHOIMAGERY.ORTHOPHOTOS", g_iniPath.c_str());
        WritePrivateProfileStringA("Satellite", "TileMatrixSet", "PM_0_19", g_iniPath.c_str());
    }
    *ppPlugInInstance = g_plugin = new CSatPlugIn();
}

void __declspec(dllexport) EuroScopePlugInExit(void)
{
    delete g_plugin;          // stops the threads
    g_plugin = nullptr;
    MemClear();               // free every Gdiplus bitmap before Gdiplus is shut down
    Gdiplus::GdiplusShutdown(g_gdiToken);
}
