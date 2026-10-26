// dshaptics_ue3.dll - log every sound an Unreal Engine 3 game starts (32-bit, Batman: Arkham Asylum first).
//
// UE3 plays every sound - one-shots from PlaySound, anim notifies, ambient and Matinee sounds - by calling
// UAudioComponent::Play() on a component. That function is hooked; one line per sound is appended to the
// file the daemon tails, the same line the UE4 hook writes:
//     ue:<FMOD project>.<SoundCue name>|<owner actor name without instance number>
// (Asylum: each SoundCue names an FMOD Designer event; the cue's project object is at ProjectOffset - the line
// reads like the game's own sound list, "FMOD [PL_CombatImpactsSFX.Batman_Punch_Enemy_Face]". No project: the cue alone.)
// A sound ending (played out, stopped, its scene skipped) goes through UAudioComponent::Cleanup(), which
// itself checks the component's "was playing" flag; for names containing one of EndFilter=<part>[,...]:
//     ue_end:<same name>|<owner name>
// Names are resolved inside the game with FName::ToString (Number = 0, so no "_12" suffix), cached per index.
// Movies (Bink, binkw32.dll beside the game): a movie plays its own sound - no sound event - so its opening and
// closing (played out or skipped) are lines of their own, named after the movie file:
//     ue:BIK.<file name without .bik>|Bink        ue_end:BIK.<same>|Bink        (Bink=0 in sigs_ue3.txt: off)
// Asylum hands BinkOpen the movie already read into memory (the "name" points at the file's "BIKi" header), so the
// name is the .bik the game opened (CreateFile) just before. Unknown or not a plain file name: no line at all
// (10-25: movie bytes written as a name broke the engine's reading of the whole event file).
//
// sigs_ue3.txt next to the DLL ("name=pattern" or "name=+rva" with Build=<hex>, first that resolves wins):
//     AudioComponentPlay=...   AudioComponentCleanup=...   FNameToString=...
//     CueOffset=4c  OwnerOffset=40  NameOffset=20  FlagsOffset=60  PlayingBit=80  ProjectOffset=8c   (hex; Asylum's)
//     EventFile=C:\Users\Public\dshaptics\events_ue.log   EndFilter=<part>[,<part>...]
// Loaded by the version.dll loader or injected by the daemon. Hooks via MinHook (hde32).
// build: build.bat dshaptics_ue3 x86

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>
#include "MinHook.h"
#include "rumble_tap.h"

struct FString { wchar_t* data; int32_t num, max; };
// thiscall functions, called and hooked as fastcall (ecx = this, edx unused)
using Comp_t = void(__fastcall*)(uint8_t* self, void* edx);
using ToString_t = FString*(__fastcall*)(const uint32_t* fname, void* edx, FString* out);

static Comp_t g_play = nullptr, g_cleanup = nullptr;
static ToString_t g_toString = nullptr;
static size_t g_cueOff = 0x4c, g_ownerOff = 0x40, g_nameOff = 0x20, g_flagsOff = 0x60, g_projOff = 0;
static uint32_t g_playingBit = 0x80;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static std::unordered_map<uint32_t, std::string> g_names;
static std::vector<std::string> g_endFilter;
static HANDLE g_dbg = INVALID_HANDLE_VALUE;   // Debug=1 in sigs_ue3.txt: raw reads of the first calls -> <dll>_debug.txt
static volatile LONG g_calls = 0;
using BinkOpen_t = void*(__stdcall*)(const char* name, uint32_t flags);
using BinkClose_t = void(__stdcall*)(void* bink);
static BinkOpen_t g_binkOpen = nullptr;
static BinkClose_t g_binkClose = nullptr;
static std::unordered_map<void*, std::string> g_movies;   // open movie -> its name, for the closing line
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileW_t g_createFileW = nullptr;
static char g_lastBik[128] = {};                 // the last .bik file the game opened, and when
static volatile DWORD g_lastBikAt = 0;

static void log_line(const std::string& s) {
    if (g_log == INVALID_HANDLE_VALUE || s.empty()) return;
    DWORD w; WriteFile(g_log, s.c_str(), (DWORD)s.size(), &w, nullptr);   // FILE_APPEND_DATA: each line lands whole
}

static bool call_to_string(const uint32_t* fn, FString* s) {
    __try { g_toString(fn, nullptr, s); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The FString buffer is the engine's and is never freed: one small allocation per distinct name.
static std::string name_of(uint32_t idx, bool resolve = true) {
    if (!idx || !g_toString) return "";
    EnterCriticalSection(&g_lock);
    auto it = g_names.find(idx);
    if (it != g_names.end()) { std::string r = it->second; LeaveCriticalSection(&g_lock); return r; }
    LeaveCriticalSection(&g_lock);
    if (!resolve) return "";
    uint32_t fn[2] = { idx, 0 };
    FString s{};
    std::string out;
    if (!call_to_string(fn, &s)) s.data = nullptr;
    if (s.data)
        for (int i = 0; i < s.num && s.data[i]; i++) out += (char)(s.data[i] < 128 ? s.data[i] : '?');
    EnterCriticalSection(&g_lock);
    g_names[idx] = out;
    LeaveCriticalSection(&g_lock);
    return out;
}

static bool read_ids(uint8_t* comp, uint32_t* cue, uint32_t* proj, uint32_t* owner, uint32_t* flags) {
    __try {
        uint8_t* c = *(uint8_t**)(comp + g_cueOff);
        uint8_t* o = *(uint8_t**)(comp + g_ownerOff);
        uint8_t* p = c && g_projOff ? *(uint8_t**)(c + g_projOff) : nullptr;
        *cue = c ? *(uint32_t*)(c + g_nameOff) : 0;
        *proj = p ? *(uint32_t*)(p + g_nameOff) : 0;
        *owner = o ? *(uint32_t*)(o + g_nameOff) : 0;
        *flags = *(uint32_t*)(comp + g_flagsOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static std::string full_name(uint32_t cue, uint32_t proj, bool resolve) {
    std::string c = name_of(cue, resolve), p = proj ? name_of(proj, resolve) : "";
    return p.empty() || c.empty() ? c : p + "." + c;
}

static void __fastcall hk_play(uint8_t* comp, void* edx) {
    uint32_t cue = 0, proj = 0, owner = 0, flags = 0;
    bool ok = comp && read_ids(comp, &cue, &proj, &owner, &flags);
    if (g_dbg != INVALID_HANDLE_VALUE) {
        LONG n = InterlockedIncrement(&g_calls);
        if (n <= 40 || n % 500 == 0) {
            char b[256]; DWORD w;
            int k = sprintf_s(b, "call %ld comp=%p read=%d cue=%x proj=%x owner=%x flags=%x\n", n, comp, ok, cue, proj, owner, flags);
            WriteFile(g_dbg, b, k, &w, nullptr);
        }
    }
    if (ok && cue) {
        std::string line = "ue:" + full_name(cue, proj, true) + "|" + name_of(owner) + "\n";
        log_line(line);
        rumble_tap::note(line.c_str(), (int)line.size());
    }
    g_play(comp, edx);
}

// Cleanup runs for every component, playing or not; only one that was playing is a sound ending.
// Names come from the cache only: no engine call while the component may be going away.
static void __fastcall hk_cleanup(uint8_t* comp, void* edx) {
    uint32_t cue = 0, proj = 0, owner = 0, flags = 0;
    if (comp && !g_endFilter.empty() && read_ids(comp, &cue, &proj, &owner, &flags) && cue && (flags & g_playingBit)) {
        std::string n = full_name(cue, proj, false);
        for (auto& f : g_endFilter)
            if (!n.empty() && n.find(f) != std::string::npos) {
                log_line("ue_end:" + n + "|" + name_of(owner, false) + "\n");
                break;
            }
    }
    g_cleanup(comp, edx);
}

// a movie's name as it goes into a line: letters, digits, _ and - only, or nothing
static std::string plain(const std::string& m) {
    if (m.empty() || m.size() > 100) return "";
    for (char c : m) if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return "";
    return m;
}

static HANDLE WINAPI hk_createFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                    DWORD flags, HANDLE tmpl) {
    if (path) {
        size_t n = wcslen(path);
        if (n > 4 && _wcsicmp(path + n - 4, L".bik") == 0) {
            const wchar_t* b = path + n;
            while (b > path && b[-1] != L'\\' && b[-1] != L'/') b--;
            char m[128]; size_t k = 0;
            for (; b < path + n - 4 && k + 1 < sizeof m; b++) m[k++] = *b < 128 ? (char)*b : '?';
            m[k] = 0;
            EnterCriticalSection(&g_lock); memcpy(g_lastBik, m, k + 1); g_lastBikAt = GetTickCount(); LeaveCriticalSection(&g_lock);
        }
    }
    return g_createFileW(path, access, share, sa, disp, flags, tmpl);
}

// BinkOpen's name: a path, a file the game opened itself (BINKFILEHANDLE 0x00800000), or the movie in memory
// (starts with Bink's own header) - then the .bik opened in the last 10 s
static bool movie_path(const char* name, uint32_t flags, char* path, DWORD n) {
    __try {
        if (flags & 0x00800000) return GetFinalPathNameByHandleA((HANDLE)name, path, n, 0) != 0;
        if ((name[0] == 'B' && name[1] == 'I' && name[2] == 'K') || (name[0] == 'K' && name[1] == 'B' && name[2] == '2'))
            return false;
        strncpy_s(path, n, name, _TRUNCATE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static std::string movie_name(const char* name, uint32_t flags) {
    char path[1024] = {};
    if (!movie_path(name, flags, path, sizeof path)) {
        std::string m;
        EnterCriticalSection(&g_lock);
        if (g_lastBik[0] && GetTickCount() - g_lastBikAt < 10000) m = g_lastBik;   // kept: the game opens a movie twice
        LeaveCriticalSection(&g_lock);
        return plain(m);
    }
    std::string m(path);
    m = m.substr(m.find_last_of("\\/") + 1);
    size_t dot = m.find_last_of('.');
    return plain(dot == std::string::npos ? m : m.substr(0, dot));
}

static void* __stdcall hk_binkOpen(const char* name, uint32_t flags) {
    std::string m = name ? movie_name(name, flags) : "";
    void* b = g_binkOpen(name, flags);
    if (b && !m.empty()) {
        EnterCriticalSection(&g_lock); g_movies[b] = m; LeaveCriticalSection(&g_lock);
        log_line("ue:BIK." + m + "|Bink\n");
    }
    return b;
}

static void __stdcall hk_binkClose(void* b) {
    std::string m;
    EnterCriticalSection(&g_lock);
    auto it = g_movies.find(b);
    if (it != g_movies.end()) { m = it->second; g_movies.erase(it); }
    LeaveCriticalSection(&g_lock);
    if (!m.empty()) log_line("ue_end:BIK." + m + "|Bink\n");
    g_binkClose(b);
}

// ---------------------------------------------------------------- pattern scan
static std::vector<int> parse_pattern(const std::string& p) {
    std::vector<int> out;
    for (size_t i = 0; i < p.size();) {
        if (p[i] == ' ') { i++; continue; }
        if (p[i] == '?') { out.push_back(-1); i += (i + 1 < p.size() && p[i + 1] == '?') ? 2 : 1; continue; }
        out.push_back(std::stoi(p.substr(i, 2), nullptr, 16)); i += 2;
    }
    return out;
}

static uint8_t* scan(const std::vector<int>& pat, int* hits) {
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* first = nullptr;
    *hits = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p = base + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize;
        for (size_t i = 0; i + pat.size() <= n; i++) {
            size_t k = 0;
            while (k < pat.size() && (pat[k] < 0 || p[i + k] == pat[k])) k++;
            if (k == pat.size()) { if (!first) first = p + i; if (++*hits == 2) return first; }
        }
    }
    return first;
}

static unsigned long exe_build() {
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    return ((IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew))->FileHeader.TimeDateStamp;
}

static std::string dll_dir(HMODULE self) {
    char path[MAX_PATH]; GetModuleFileNameA(self, path, MAX_PATH);
    std::string s(path); return s.substr(0, s.find_last_of("\\/") + 1);
}

// The status file is named after this DLL ("dshaptics_ue3_x86.dll" -> dshaptics_ue3_x86.log): the engine looks for it so
static std::string status_name(HMODULE self) {
    char path[MAX_PATH]; GetModuleFileNameA(self, path, MAX_PATH);
    std::string s(path); s = s.substr(s.find_last_of("\\/") + 1);
    return s.substr(0, s.find_last_of('.')) + ".log";
}

static std::vector<std::string> split(const std::string& v) {
    std::vector<std::string> out;
    for (size_t a = 0; a <= v.size();) {
        size_t b = v.find(',', a); if (b == std::string::npos) b = v.size();
        if (b > a) out.push_back(v.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

static bool hook(uint8_t* at, void* mine, void** orig, MH_STATUS* st) {
    void* tramp = nullptr;
    *st = MH_CreateHook(at, mine, &tramp);
    if (*st != MH_OK) return false;
    *orig = tramp;                                   // set before enabling: the hook may fire at once
    *st = MH_EnableHook(at);
    if (*st != MH_OK) { MH_RemoveHook(at); *orig = nullptr; return false; }
    return true;
}

static DWORD WINAPI init(LPVOID self) {
    InitializeCriticalSection(&g_lock);
    std::string dir = dll_dir((HMODULE)self);
    std::string eventFile = "C:\\Users\\Public\\dshaptics\\events_ue.log";
    bool mh = false, bink = true;
    for (int attempt = 1; attempt <= 100 && !g_play; attempt++) {    // every 3 s for 5 minutes, as the UE4 hook
        if (attempt > 1) Sleep(3000);
        FILE* f = fopen((dir + "sigs_ue3.txt").c_str(), "r");
        FILE* status = fopen((dir + status_name((HMODULE)self)).c_str(), "w");
        if (status) fprintf(status, "attempt %d\n", attempt);
        uint8_t *playAt = nullptr, *cleanupAt = nullptr;
        char line[1024];
        unsigned long build = 0;
        while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "Build=", 6)) build = strtoul(line + 6, nullptr, 16);
        if (f) rewind(f);
        while (f && fgets(line, sizeof line, f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            auto eq = l.find('=');
            if (l.empty() || l[0] == '#' || eq == std::string::npos) continue;
            std::string name = l.substr(0, eq), val = l.substr(eq + 1);
            if (val.empty() || name == "Build") continue;
            if (name == "CueOffset") { g_cueOff = strtoul(val.c_str(), nullptr, 16); continue; }
            if (name == "OwnerOffset") { g_ownerOff = strtoul(val.c_str(), nullptr, 16); continue; }
            if (name == "NameOffset") { g_nameOff = strtoul(val.c_str(), nullptr, 16); continue; }
            if (name == "FlagsOffset") { g_flagsOff = strtoul(val.c_str(), nullptr, 16); continue; }
            if (name == "ProjectOffset") { g_projOff = strtoul(val.c_str(), nullptr, 16); continue; }
            if (name == "PlayingBit") { g_playingBit = strtoul(val.c_str(), nullptr, 16); continue; }
            if (name == "EventFile") { eventFile = val; continue; }
            if (name == "EndFilter") { g_endFilter = split(val); continue; }
            if (name == "Bink") { bink = val != "0"; continue; }
            if (name == "Debug") {
                if (val != "0" && g_dbg == INVALID_HANDLE_VALUE) {
                    std::string d = dir + status_name((HMODULE)self);
                    g_dbg = CreateFileA((d.substr(0, d.size() - 4) + "_debug.txt").c_str(), GENERIC_WRITE,
                                        FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
                }
                continue;
            }
            if (val[0] == '+' && build != exe_build()) {
                if (status) fprintf(status, "%s: fixed address skipped (taken from build %08lx, this exe is %08lx)\n",
                                    name.c_str(), build, exe_build());
                continue;
            }
            int hits = 1;
            uint8_t* at = val[0] == '+' ? (uint8_t*)GetModuleHandleW(nullptr) + strtoul(val.c_str() + 1, nullptr, 16)
                                        : scan(parse_pattern(val), &hits);
            if (!at) { if (status) fprintf(status, "%s: not found (%s)\n", name.c_str(), val.c_str()); continue; }
            if (hits > 1 && status) fprintf(status, "%s: pattern matches more than one place - the first is used\n", name.c_str());
            if (name == "AudioComponentPlay" && !playAt) playAt = at;
            if (name == "AudioComponentCleanup" && !cleanupAt) cleanupAt = at;
            if (name == "FNameToString" && !g_toString) g_toString = (ToString_t)at;
            if (status) fprintf(status, "%s: %p\n", name.c_str(), at);
        }
        if (f) fclose(f);
        if (g_log == INVALID_HANDLE_VALUE)
            g_log = CreateFileA(eventFile.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_ALWAYS, 0, nullptr);
        if (!mh) { mh = MH_Initialize() == MH_OK; if (mh) rumble_tap::start(eventFile.substr(0, eventFile.find_last_of("\\/") + 1)); }
        MH_STATUS st = MH_ERROR_NOT_INITIALIZED, stEnd = MH_ERROR_NOT_INITIALIZED;
        if (mh && g_toString && playAt) {
            if (cleanupAt && !g_endFilter.empty() && !g_cleanup) hook(cleanupAt, (void*)hk_cleanup, (void**)&g_cleanup, &stEnd);
            hook(playAt, (void*)hk_play, (void**)&g_play, &st);
        }
        HMODULE bk = bink && mh && !g_binkOpen ? GetModuleHandleA("binkw32.dll") : nullptr;
        if (bk) {
            auto open = (uint8_t*)GetProcAddress(bk, "_BinkOpen@8"), close = (uint8_t*)GetProcAddress(bk, "_BinkClose@4");
            auto cf = (uint8_t*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateFileW");
            MH_STATUS sb;
            if (cf && !g_createFileW) hook(cf, (void*)hk_createFileW, (void**)&g_createFileW, &sb);
            if (open && close && g_createFileW && hook(close, (void*)hk_binkClose, (void**)&g_binkClose, &sb))
                hook(open, (void*)hk_binkOpen, (void**)&g_binkOpen, &sb);
        }
        if (status) {
            fprintf(status, "events -> %s\nAudioComponentPlay %s%s\n", eventFile.c_str(), g_play ? "hooked" : "NOT hooked: ",
                    g_play ? "" : (!g_toString || !playAt) ? "addresses not found" : MH_StatusToString(st));
            fprintf(status, "AudioComponentCleanup %s (%d end filters)\n", g_cleanup ? "hooked" : "not hooked", (int)g_endFilter.size());
            fprintf(status, "Bink movies %s\n", g_binkOpen ? "hooked" : !bink ? "off" : "not hooked (no binkw32.dll loaded)");
            fclose(status);
        }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, init, h, 0, nullptr);   // no heavy work under the loader lock
    }
    return TRUE;
}
