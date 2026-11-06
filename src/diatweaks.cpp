// diatweaks.exe - attaches DiaTweaksTap.dll to a running Dia (Windows) and manages tweaks.conf.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <appmodel.h>
#include <shlobj.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>

#include "shared.h"
#include "local_browsing.h"
#include "version.h"  // bin\version.h, written by build.cmd
#ifndef WDT_VERSION
#define WDT_VERSION L"dev"
#endif

static std::wstring g_dir;  // folder of diatweaks.exe

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// ---------------------------------------------------------------- finding Dia

struct WinSearch {
    DWORD pid = 0;
};

static BOOL CALLBACK FindWinUIWindow(HWND hwnd, LPARAM lp) {
    auto* ws = reinterpret_cast<std::vector<DWORD>*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t cls[128];
    GetClassNameW(hwnd, cls, 128);
    if (wcscmp(cls, L"WinUIDesktopWin32WindowClass") != 0) return TRUE;
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    ws->push_back(pid);
    return TRUE;
}

// Dia's visible main window, once it has one.
static HWND DiaWindow(DWORD pid) {
    struct Search {
        DWORD pid;
        HWND hwnd;
    } s{pid, nullptr};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto s = reinterpret_cast<Search*>(lp);
            DWORD p = 0;
            wchar_t cls[128];
            GetWindowThreadProcessId(h, &p);
            if (p != s->pid || !IsWindowVisible(h) || !GetClassNameW(h, cls, 128) ||
                wcscmp(cls, L"WinUIDesktopWin32WindowClass") != 0)
                return TRUE;
            s->hwnd = h;
            return FALSE;
        },
        (LPARAM)&s);
    return s.hwnd;
}

static std::vector<DWORD> DiaPids() {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, L"Dia.exe") == 0) out.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return out;
}

// The browser (UI) process is the Dia.exe that owns a WinUI window.
static DWORD FindDiaUiPid() {
    auto dia = DiaPids();
    std::vector<DWORD> winPids;
    EnumWindows(FindWinUIWindow, (LPARAM)&winPids);
    for (DWORD p : winPids)
        for (DWORD d : dia)
            if (p == d) return p;
    return 0;
}

// Dia's main (browser) process: the Dia.exe not started by another Dia.exe (those are its helper
// processes). It exists from the first moment Dia starts, long before its window shows.
// Opening Dia while it's open briefly starts a second such Dia.exe, which hands the request to the
// open one and exits: when there are several, the one with helper processes is the browser.
static DWORD FindDiaMainPid() {
    std::map<DWORD, DWORD> dia;  // pid -> parent
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, L"Dia.exe") == 0) dia[pe.th32ProcessID] = pe.th32ParentProcessID;
    CloseHandle(snap);
    DWORD best = 0;
    size_t bestChildren = 0;
    for (auto& [pid, parent] : dia) {
        if (dia.count(parent)) continue;  // a helper
        size_t children = 0;
        for (auto& [p2, parent2] : dia) children += parent2 == pid;
        if (!best || children > bestChildren) best = pid, bestChildren = children;
    }
    return best;
}

static std::wstring ProcessPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring r;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) r = buf;
    CloseHandle(h);
    return r;
}

static bool TapLoaded(DWORD pid) {
    // While a process is starting its module list can be mid-change: Windows then asks to try again.
    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 20; i++) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
        Sleep(5);
    }
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{sizeof(me)};
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
        if (_wcsicmp(me.szModule, L"DiaTweaksTap.dll") == 0) found = true;
    CloseHandle(snap);
    return found;
}

// ---------------------------------------------------------------- launching with the tap

static std::wstring W(const std::string& s);
static std::string N(const std::wstring& w);

static std::wstring Guid() {
    LPOLESTR s = nullptr;
    StringFromCLSID(CLSID_DiaTweaksTap, &s);
    std::wstring r = s ? s : L"";
    CoTaskMemFree(s);
    return r;  // "{....}"
}

// Dia's install folder from the package registry (works with Dia closed). Family name is fixed.
static std::wstring DiaPackageDir() {
    const wchar_t* family = L"TheBrowserCompany.Dia_ttt1ap7aakyb4";
    UINT32 count = 0, bufLen = 0;
    long r = FindPackagesByPackageFamily(family, PACKAGE_FILTER_HEAD, &count, nullptr, &bufLen, nullptr, nullptr);
    if (r != ERROR_INSUFFICIENT_BUFFER || !count) return {};
    std::vector<wchar_t*> names(count);
    std::vector<wchar_t> buf(bufLen);
    if (FindPackagesByPackageFamily(family, PACKAGE_FILTER_HEAD, &count, names.data(), &bufLen, buf.data(),
                                    nullptr) != ERROR_SUCCESS)
        return {};
    UINT32 pathLen = 0;
    r = GetPackagePathByFullName(names[0], &pathLen, nullptr);
    if (r != ERROR_INSUFFICIENT_BUFFER) return {};
    std::vector<wchar_t> path(pathLen);
    if (GetPackagePathByFullName(names[0], &pathLen, path.data()) != ERROR_SUCCESS) return {};
    return std::wstring(path.data());
}

// Full path to Dia's real executable. Prefer a running Dia (also caches it), else the package
// registry. Used as a fallback launch path when app activation isn't available.
static std::wstring DiaExePath() {
    DWORD pid = FindDiaUiPid();
    if (!pid)
        for (DWORD p : DiaPids()) { pid = p; break; }
    std::wstring cache = g_dir + L"\\dia_path.txt";
    if (pid) {
        std::wstring p = ProcessPath(pid);
        if (!p.empty()) {
            std::ofstream out(cache, std::ios::binary);
            std::string n = N(p);
            out.write(n.data(), n.size());
            return p;
        }
    }
    std::wstring dir = DiaPackageDir();
    if (!dir.empty()) return dir + L"\\Dia.exe";
    std::ifstream in(cache, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return W(s);
}

// Put DiaTweaksTap.dll into a running Dia. The add-on then attaches itself from inside Dia via the
// in-process WinUI diagnostics API. Nothing in Dia's install folder is touched.
static bool InjectTap(DWORD pid) {
    if (TapLoaded(pid)) return true;
    std::wstring dll = g_dir + L"\\DiaTweaksTap.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"%ls is missing. Run build.cmd first.\n", dll.c_str());
        return false;
    }
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                  PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                              FALSE, pid);
    if (!proc) {
        fwprintf(stderr, L"Can't open Dia (error %lu).\n", GetLastError());
        return false;
    }
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = mem && WriteProcessMemory(proc, mem, dll.c_str(), bytes, nullptr);
    if (ok) {
        auto loadLib =
            (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, mem, 0, nullptr);
        if (!th) {
            ok = false;
        } else {
            // The thread's exit code is LoadLibraryW's result (its low 32 bits): non-zero means loaded.
            DWORD loaded = 0;
            if (WaitForSingleObject(th, 15000) == WAIT_OBJECT_0 && GetExitCodeThread(th, &loaded) && loaded) {
                CloseHandle(th);
                VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
                CloseHandle(proc);
                return true;
            }
            CloseHandle(th);
        }
    }
    if (mem) VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    if (!ok) {
        fwprintf(stderr, L"Couldn't load the add-on into Dia (error %lu).\n", GetLastError());
        return false;
    }
    for (int i = 0; i < 60 && !TapLoaded(pid); i++) Sleep(100);
    if (TapLoaded(pid)) return true;
    fwprintf(stderr, L"The add-on didn't stay loaded in Dia (pid %lu). See %ls\\tap.log\n", pid, g_dir.c_str());
    return false;
}

static const wchar_t kDiaAppId[] = L"TheBrowserCompany.Dia_ttt1ap7aakyb4!Dia";

// Activate Dia the way the Start menu does; *pid gets the process it started or brought forward. With
// Dia already open, Dia handles it like a click on its own icon.
// Windows only lets the app that got the last input bring a window to the front. The launcher, started
// from Start or a taskbar pin, doesn't qualify (the app you were in is active again by then), so Dia
// would open behind it. A no-op input of our own (no movement, no buttons; the way PowerToys does it)
// makes us qualify.
static void TakeForegroundRight() {
    INPUT in{};
    in.type = INPUT_MOUSE;
    SendInput(1, &in, sizeof(in));
}

static bool ActivateDia(DWORD* pid = nullptr) {
    IApplicationActivationManager* am = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&am))))
        return false;
    TakeForegroundRight();
    AllowSetForegroundWindow(ASFW_ANY);  // so Dia can come to the front, as from its own icon
    DWORD started = 0;
    bool ok = SUCCEEDED(am->ActivateApplication(kDiaAppId, nullptr, AO_NONE, &started));
    am->Release();
    // The grant above ends with your next input; this one stays with Dia until its window shows.
    if (ok && started) AllowSetForegroundWindow(started);
    if (pid) *pid = ok ? started : 0;
    return ok;
}

// The same through the shell (shell:AppsFolder), for when activation fails.
static bool OpenDiaViaShell() {
    HINSTANCE h = ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                (std::wstring(L"shell:AppsFolder\\") + kDiaAppId).c_str(), nullptr, SW_SHOWNORMAL);
    return (INT_PTR)h > 32;
}

// Start Dia the normal way: through the package's own activation path, so it comes up exactly as the
// user expects. Dia hands its UI off to a broker-launched process, so environment variables can't be
// passed in - the add-on is attached as Dia starts instead. *pid gets Dia's pid when known.
static bool LaunchTweaked(DWORD* pid) {
    if (ActivateDia(pid)) return true;
    std::wstring exe = DiaExePath();
    if (exe.empty()) {
        fwprintf(stderr, L"Can't find Dia's location. Open Dia once (normally), then run this again.\n");
        return false;
    }
    if (OpenDiaViaShell()) return true;

    std::wstring cwd = exe.substr(0, exe.find_last_of(L'\\'));
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + exe + L"\"";
    std::vector<wchar_t> buf(command.begin(), command.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(),
                             &si, &pi);
    if (!ok) {
        fwprintf(stderr, L"Could not start Dia (error %lu)\n", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void Signal(LPCWSTR name) {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (ev) SetEvent(ev), CloseHandle(ev);
}

static std::wstring PidEvent(LPCWSTR name, DWORD pid) { return name + std::to_wstring(pid); }

// Whether the add-on in this Dia is attached to its UI, not just loaded: it sets this event once WinUI
// hands it the visual tree.
static bool TapAttached(DWORD pid) {
    HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, PidEvent(DT_EVENT_ATTACHED, pid).c_str());
    if (!ev) return false;
    bool set = WaitForSingleObject(ev, 0) == WAIT_OBJECT_0;
    CloseHandle(ev);
    return set;
}

// Get the tweaks attached to this Dia: load the add-on if it isn't there, ask it to try again if it's
// there but couldn't attach, then wait until it says it's attached.
static bool EnsureTweaked(DWORD pid, int seconds) {
    if (!TapLoaded(pid)) {
        if (!InjectTap(pid)) return false;
    } else if (!TapAttached(pid)) {
        Signal(PidEvent(DT_EVENT_RETRY, pid).c_str());
    }
    for (ULONGLONG end = GetTickCount64() + seconds * 1000ull; GetTickCount64() < end;) {
        if (TapAttached(pid)) return true;
        Sleep(10);
    }
    fwprintf(stderr, L"The add-on is in Dia but the tweaks didn't attach. See %ls\\tap.log\n", g_dir.c_str());
    return false;
}

// ---------------------------------------------------------------- config

struct TweakLine {
    size_t line;
    std::wstring key, title;
    bool on;
};

static std::vector<std::string> ReadLines() {
    std::vector<std::string> lines;
    std::ifstream in(g_dir + L"\\tweaks.conf");
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    return lines;
}

static std::wstring W(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string N(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::vector<TweakLine> ParseTweaks(const std::vector<std::string>& lines) {
    std::vector<TweakLine> out;
    for (size_t i = 0; i < lines.size(); i++) {
        std::wstring l = W(lines[i]);
        size_t a = l.find_first_not_of(L" \t");
        if (a == std::wstring::npos || l[a] != L'[') continue;
        size_t e = l.find(L']', a);
        if (e == std::wstring::npos) continue;
        TweakLine t{i, l.substr(a + 1, e - a - 1), L"", l.find(L"on", e) != std::wstring::npos};
        for (size_t j = i + 1; j < lines.size(); j++) {
            std::wstring m = W(lines[j]);
            size_t b = m.find_first_not_of(L" \t");
            if (b != std::wstring::npos && m[b] == L'[') break;
            if (b != std::wstring::npos && m.compare(b, 6, L"title:") == 0) {
                t.title = m.substr(b + 6);
                t.title.erase(0, t.title.find_first_not_of(L" \t"));
                break;
            }
        }
        out.push_back(t);
    }
    return out;
}

static void ListTweaks() {
    auto tweaks = ParseTweaks(ReadLines());
    for (auto& t : tweaks)
        wprintf(L"  [%ls] %-26ls %ls\n", t.on ? L"on " : L"off", t.key.c_str(), t.title.c_str());
}

static int SetTweak(const std::wstring& which, bool on) {
    auto lines = ReadLines();
    auto tweaks = ParseTweaks(lines);
    std::wstring w = Lower(which);
    bool hit = false;
    bool startupChanged = false;
    for (auto& t : tweaks) {
        if (w != L"all" && Lower(t.key) != w && Lower(t.title) != w) continue;
        if (w == L"all" && t.key == L"normalLaunch") continue;  // a setting, not a patch
        // The sign-in bypass is an explicit opt-in, independent of visual patches.
        if (w == L"all" && on && t.key == L"localBrowsing") continue;
        if (t.key == L"localBrowsing" && t.on != on) startupChanged = true;
        lines[t.line] = "[" + N(t.key) + "] " + (on ? "on" : "off");
        wprintf(L"%ls %ls\n", on ? L"Enabled " : L"Disabled", t.key.c_str());
        hit = true;
    }
    if (!hit) {
        fwprintf(stderr, L"No tweak named \"%ls\". Run: diatweaks list\n", which.c_str());
        return 1;
    }
    std::ofstream out(g_dir + L"\\tweaks.conf", std::ios::binary);
    if (!out) return fwprintf(stderr, L"Could not write tweaks.conf.\n"), 1;
    for (auto& l : lines) out << l << "\n";
    out.close();
    if (!out) return fwprintf(stderr, L"Could not save tweaks.conf.\n"), 1;
    Signal(DT_EVENT_RELOAD);
    if (startupChanged) wprintf(L"Restart Dia to apply the forced sign-in bypass: diatweaks restart\n");
    return 0;
}

// ---------------------------------------------------------------- launching

static DWORD WaitForTap(int seconds, DWORD expectedPid) {
    // Polled often: the launcher raises Dia's window as soon as this returns.
    for (ULONGLONG end = GetTickCount64() + seconds * 1000ull; GetTickCount64() < end;) {
        DWORD p = expectedPid ? expectedPid : FindDiaUiPid();
        if (p && TapAttached(p) && (!expectedPid || DiaWindow(p))) return p;
        Sleep(20);
    }
    return 0;
}

static BOOL CALLBACK CloseWindows(HWND hwnd, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    for (DWORD d : *reinterpret_cast<std::vector<DWORD>*>(lp))
        if (d == pid) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

// Close every running Dia process so the next launch is a fresh instance we can attach to.
static void CloseDia() {
    auto pids = DiaPids();
    if (pids.empty()) return;
    EnumWindows(CloseWindows, (LPARAM)&pids);
    for (int i = 0; i < 40 && !DiaPids().empty(); i++) Sleep(250);
    for (DWORD p : DiaPids()) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, p);
        if (h) TerminateProcess(h, 0), CloseHandle(h);
    }
    for (int i = 0; i < 20 && !DiaPids().empty(); i++) Sleep(250);
}

// ---------------------------------------------------------------- commands

static const wchar_t kLaunchMutex[] = L"Local\\WinDiaTweaks_Launch";

static bool ExplicitLaunchInProgress() {
    HANDLE launch = OpenMutexW(SYNCHRONIZE, FALSE, kLaunchMutex);
    if (!launch) return false;
    CloseHandle(launch);
    return true;
}

// Start a tweaked Dia, closing any plain instance first. Returns the tapped pid (0 on failure).
static DWORD StartTweaked(bool quiet) {
    struct LaunchLock {
        HANDLE handle = CreateMutexW(nullptr, FALSE, kLaunchMutex);
        bool owned = false;
        LaunchLock() {
            if (handle) {
                DWORD wait = WaitForSingleObject(handle, 0);
                owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
            }
        }
        ~LaunchLock() { if (owned) ReleaseMutex(handle); if (handle) CloseHandle(handle); }
    } launch;
    if (!launch.owned) {
        fwprintf(stderr, L"Another Dia launch is in progress. Wait for it to finish.\n");
        return 0;
    }
    bool localBrowsing = false;
    for (auto& t : ParseTweaks(ReadLines()))
        if (t.key == L"localBrowsing") localBrowsing = t.on;
    int quickExits = 0;
    { std::ifstream state(g_dir + L"\\launch.state"); state >> quickExits; }
    if (localBrowsing && quickExits >= 2) {
        fwprintf(stderr, L"Safe mode: the forced sign-in bypass is skipped with the other tweaks.\n");
        localBrowsing = false;
    }
    std::wstring localExe;
    if (localBrowsing) {
        localExe = DiaExePath();
        std::wstring message;
        if (!local_browsing::CheckSupported(localExe, message)) {
            fwprintf(stderr, L"Forced sign-in bypass could not start: %ls\n", message.c_str());
            return 0;
        }
    }
    CloseDia();
    DWORD pid = 0;
    if (localBrowsing) {
        auto result = local_browsing::Launch(localExe);
        if (result.status != local_browsing::LaunchStatus::Started) {
            fwprintf(stderr, L"Forced sign-in bypass could not start: %ls\n", result.message.c_str());
            return 0;
        }
        pid = result.pid;
        if (!quiet) wprintf(L"Forced sign-in bypass enabled for this Dia process.\n");
    } else {
        if (!LaunchTweaked(&pid)) return 0;
        // Attach as soon as Dia's main process starts. The add-on waits for WinUI inside Dia.
        if (pid && FindDiaMainPid() != pid) pid = 0;
        for (ULONGLONG end = GetTickCount64() + 40000; !pid && GetTickCount64() < end;) {
            pid = FindDiaMainPid();
            if (!pid) Sleep(10);
        }
    }
    if (!pid) {
        fwprintf(stderr, L"Dia didn't come up. Try opening Dia yourself, then run: diatweaks\n");
        return 0;
    }
    if (!InjectTap(pid)) return 0;

    pid = WaitForTap(30, localBrowsing ? pid : 0);
    if (!pid) {
        fwprintf(stderr, L"Dia started but tweaks didn't load. See %ls\\tap.log\n", g_dir.c_str());
        return 0;
    }
    if (!quiet) wprintf(L"Dia running with tweaks (pid %lu)\n", pid);
    return pid;
}

static int CmdApply() {
    DWORD pid = FindDiaUiPid();
    if (pid && TapAttached(pid)) {
        Signal(DT_EVENT_RELOAD);
        wprintf(L"Tweaks reloaded in Dia (pid %lu)\n", pid);
        return 0;
    }
    if (pid && TapLoaded(pid)) {  // loaded but never attached: have it try again
        if (!EnsureTweaked(pid, 15)) return 1;
        wprintf(L"Dia running with tweaks (pid %lu)\n", pid);
        return 0;
    }
    return StartTweaked(false) ? 0 : 1;
}

// Only attach to an existing UI process; normal launches must not be restarted.
static int CmdAttach() {
    DWORD pid = FindDiaMainPid();
    if (!pid) return 2;
    if (ExplicitLaunchInProgress())
        return fwprintf(stderr, L"Another Dia launch is in progress. Wait for it to finish.\n"), 1;
    if (!EnsureTweaked(pid, 15)) return 1;
    wprintf(L"Tweaks attached to running Dia (pid %lu)\n", pid);
    return 0;
}

static int CmdDump() {
    DWORD pid = FindDiaUiPid();
    if (!pid || !TapLoaded(pid)) {
        pid = StartTweaked(true);
        if (!pid) return 1;
        Sleep(1500);  // let the initial tree replay finish
    }
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, DT_EVENT_DONE);
    Signal(DT_EVENT_DUMP);
    if (WaitForSingleObject(done, 15000) != WAIT_OBJECT_0) {
        fwprintf(stderr, L"Dia didn't answer. See %ls\\tap.log\n", g_dir.c_str());
        return 1;
    }
    wprintf(L"Wrote %ls\\tree.txt\n", g_dir.c_str());
    return 0;
}

static int CmdStatus() {
    DWORD pid = FindDiaUiPid();
    if (!pid) wprintf(L"Dia: not running\n");
    else
        wprintf(L"Dia: running (pid %lu)\n  %ls\nTweaks loaded: %ls\n", pid, ProcessPath(pid).c_str(),
                TapLoaded(pid) ? L"yes" : L"no");
    wprintf(L"\nTweaks:\n");
    ListTweaks();
    return 0;
}

// Ask the add-on to describe live elements (for making tweaks): names go to probe.txt, the answer
// is appended to probe.out. A trailing * also lists everything under that element.
static int CmdProbe(int argc, wchar_t** argv) {
    DWORD pid = FindDiaUiPid();
    if (!pid || !TapLoaded(pid)) {
        fwprintf(stderr, L"Dia isn't running with tweaks. Run: diatweaks\n");
        return 1;
    }
    std::wstring out = g_dir + L"\\probe.out";
    DeleteFileW(out.c_str());
    {
        std::ofstream f(g_dir + L"\\probe.txt", std::ios::binary);
        for (int i = 2; i < argc; i++) f << N(argv[i]) << "\n";
    }
    for (int i = 0; i < 40; i++) {
        Sleep(250);
        if (GetFileAttributesW((g_dir + L"\\probe.txt").c_str()) == INVALID_FILE_ATTRIBUTES) {
            Sleep(300);
            std::ifstream in(out);
            std::string l;
            while (std::getline(in, l)) printf("%s\n", l.c_str());
            return 0;
        }
    }
    fwprintf(stderr, L"Dia didn't answer.\n");
    return 1;
}

// Set a tweak's value (for tweaks with a choice or text, e.g. windowMaterial acrylic).
static int CmdSet(const std::wstring& key, const std::wstring& value) {
    auto lines = ReadLines();
    auto tweaks = ParseTweaks(lines);
    for (auto& t : tweaks) {
        if (Lower(t.key) != Lower(key)) continue;
        size_t insertAt = t.line + 1;
        bool done = false;
        for (size_t j = t.line + 1; j < lines.size(); j++) {
            std::wstring m = W(lines[j]);
            size_t b = m.find_first_not_of(L" \t");
            if (b != std::wstring::npos && m[b] == L'[') break;
            if (b != std::wstring::npos && m.compare(b, 6, L"value:") == 0) {
                lines[j] = "value: " + N(value);
                done = true;
                break;
            }
            if (b != std::wstring::npos && m.compare(b, 6, L"title:") == 0) insertAt = j + 1;
        }
        if (!done) lines.insert(lines.begin() + insertAt, "value: " + N(value));
        std::ofstream out(g_dir + L"\\tweaks.conf", std::ios::binary);
        for (auto& l : lines) out << l << "\n";
        out.close();
        Signal(DT_EVENT_RELOAD);
        wprintf(L"%ls = %ls\n", t.key.c_str(), value.c_str());
        return 0;
    }
    fwprintf(stderr, L"No tweak named \"%ls\". Run: diatweaks list\n", key.c_str());
    return 1;
}

// ---------------------------------------------------------------- versions and updates
// `check` reports the installed and newest versions of Dia and WinDiaTweaks (read by Settings > Patches
// > Updates); `update` installs them. WinDiaTweaks comes from this repo's GitHub releases (or `git pull`
// for a clone), Dia from the App Installer feed its package was installed from.

namespace wdm = winrt::Windows::Management::Deployment;
namespace wweb = winrt::Windows::Web::Http;
using winrt::Windows::Foundation::Uri;

static const wchar_t kRepo[] = L"zioder/WinDiaTweaks";
static const wchar_t kZip[] = L"WinDiaTweaks.zip";
static const wchar_t kDiaFamily[] = L"TheBrowserCompany.Dia_ttt1ap7aakyb4";

// "v1.2.3", "1.5.0.591" or git's "v1.2.0-3-gabc123-dirty" (3 commits after v1.2.0). No digits: unknown.
static std::vector<int> VersionNumbers(const std::wstring& s) {
    std::vector<int> n;
    size_t i = (!s.empty() && (s[0] == L'v' || s[0] == L'V')) ? 1 : 0;
    while (i < s.size() && iswdigit(s[i])) {
        int x = 0;
        while (i < s.size() && iswdigit(s[i])) x = x * 10 + (s[i++] - L'0');
        n.push_back(x);
        if (i < s.size() && s[i] == L'.') i++;
        else break;
    }
    return n;
}

// Is `latest` newer than `installed`? A build with no version (a plain clone) counts as older.
static bool Newer(const std::wstring& latest, const std::wstring& installed) {
    auto l = VersionNumbers(latest), i = VersionNumbers(installed);
    if (l.empty()) return false;
    if (i.empty()) return true;
    for (size_t k = 0; k < std::max(l.size(), i.size()); k++) {
        int a = k < l.size() ? l[k] : 0, b = k < i.size() ? i[k] : 0;
        if (a != b) return a > b;
    }
    return false;
}

// Built in a git clone (bin\ next to .git): updates come with `git pull` and a rebuild.
static std::wstring RepoRoot() {
    std::wstring root = g_dir.substr(0, g_dir.find_last_of(L'\\'));
    return GetFileAttributesW((root + L"\\.git").c_str()) != INVALID_FILE_ATTRIBUTES ? root : L"";
}

// Runs a command hidden and returns what it printed, if it succeeded.
static bool Capture(std::wstring cmdline, std::string& out) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }
    char buf[4096];
    DWORD n = 0, code = 1;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return code == 0;
}

// A clone someone works in (another branch than main, or changes not committed) is a development
// build: its version is whatever was built, so it isn't offered releases and is never pulled.
// Returns why it is one, or nothing for a release or a clean main.
static std::wstring DevBuildReason() {
    std::wstring root = RepoRoot();
    if (root.empty()) return {};
    std::wstring git = L"git -C \"" + root + L"\" ";
    std::string branch, changes;
    if (!Capture(git + L"rev-parse --abbrev-ref HEAD", branch)) return L"git isn't available to update it";
    if (branch != "main") return L"on branch " + W(branch);
    if (!Capture(git + L"status --porcelain --untracked-files=no", changes)) return L"git isn't available to update it";
    if (!changes.empty()) return L"with changes that aren't committed";
    return {};
}

static std::wstring Message(const winrt::hresult_error& e) {
    std::wstring m(e.message());
    while (!m.empty() && (m.back() == L'\n' || m.back() == L'\r' || m.back() == L' ')) m.pop_back();
    if (m.empty()) {
        wchar_t b[32];
        swprintf(b, 32, L"error 0x%08lx", (unsigned long)e.code());
        m = b;
    }
    return m;
}

static wweb::HttpClient Http() {
    wweb::Filters::HttpBaseProtocolFilter filter;
    filter.CacheControl().ReadBehavior(wweb::Filters::HttpCacheReadBehavior::NoCache);
    filter.CacheControl().WriteBehavior(wweb::Filters::HttpCacheWriteBehavior::NoCache);
    wweb::HttpClient client(filter);
    client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"WinDiaTweaks");
    return client;
}

static wweb::HttpResponseMessage Get(const std::wstring& url, std::wstring& error) {
    try {
        auto client = Http();
        if (url.find(L"api.github.com") != std::wstring::npos)
            client.DefaultRequestHeaders().Accept().TryParseAdd(L"application/vnd.github+json");
        auto r = client.GetAsync(Uri(url)).get();
        if (r.IsSuccessStatusCode()) return r;
        error = r.StatusCode() == wweb::HttpStatusCode::NotFound ? L"not found"
                                                                 : L"the server answered " + std::to_wstring((int)r.StatusCode());
    } catch (winrt::hresult_error const& e) {
        error = Message(e);
    }
    return nullptr;
}

static std::wstring GetText(const std::wstring& url, std::wstring& error) {
    auto r = Get(url, error);
    if (!r) return {};
    try {
        return std::wstring(r.Content().ReadAsStringAsync().get());
    } catch (winrt::hresult_error const& e) {
        error = Message(e);
    }
    return {};
}

static bool Download(const std::wstring& url, const std::wstring& file, std::wstring& error) {
    auto r = Get(url, error);
    if (!r) return false;
    try {
        auto buf = r.Content().ReadAsBufferAsync().get();
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(buf.data()), buf.Length());
        if (out.good()) return true;
        error = L"can't write " + file;
    } catch (winrt::hresult_error const& e) {
        error = Message(e);
    }
    return false;
}

struct Release {
    std::wstring tag, url, notes, zip, error;
};

// The GitHub repo releases come from; WDT_REPO=owner/name overrides it (for forks and testing).
static std::wstring Repo() {
    wchar_t b[256];
    DWORD n = GetEnvironmentVariableW(L"WDT_REPO", b, 256);
    return n && n < 256 ? std::wstring(b) : std::wstring(kRepo);
}

static Release LatestRelease() {
    Release r;
    std::wstring text = GetText(L"https://api.github.com/repos/" + Repo() + L"/releases/latest", r.error);
    if (text.empty()) {
        if (r.error == L"not found") r.error = L"no releases yet";
        return r;
    }
    try {
        namespace json = winrt::Windows::Data::Json;
        auto o = json::JsonObject::Parse(text);
        auto str = [&](const json::JsonObject& obj, const wchar_t* k) -> std::wstring {
            if (!obj.HasKey(k)) return {};
            auto v = obj.GetNamedValue(k);
            return v.ValueType() == json::JsonValueType::String ? std::wstring(v.GetString()) : L"";
        };
        r.tag = str(o, L"tag_name");
        r.url = str(o, L"html_url");
        r.notes = str(o, L"body");
        if (o.HasKey(L"assets"))
            for (auto const& a : o.GetNamedArray(L"assets")) {
                auto asset = a.GetObject();
                if (str(asset, L"name") == kZip) r.zip = str(asset, L"browser_download_url");
            }
        if (r.tag.empty()) r.error = L"unexpected answer from GitHub";
    } catch (winrt::hresult_error const& e) {
        r.error = L"unexpected answer from GitHub (" + Message(e) + L")";
    }
    return r;
}

struct DiaVersion {
    std::wstring installed, latest, feed, error;
};

static std::wstring VersionText(const winrt::Windows::ApplicationModel::PackageVersion& v) {
    wchar_t b[64];
    swprintf(b, 64, L"%u.%u.%u.%u", v.Major, v.Minor, v.Build, v.Revision);
    return b;
}

// Dia installs from an App Installer file (stable, release candidate, ...); Windows remembers which one,
// and that file names the newest Dia on the same channel.
static DiaVersion CheckDia() {
    DiaVersion d;
    bool arm = false;
    try {
        wdm::PackageManager pm;
        for (auto const& p : pm.FindPackagesForUser(L"", kDiaFamily)) {
            d.installed = VersionText(p.Id().Version());
            arm = p.Id().Architecture() == winrt::Windows::System::ProcessorArchitecture::Arm64;
            try {
                if (auto info = p.GetAppInstallerInfo()) d.feed = std::wstring(info.Uri().AbsoluteUri());
            } catch (...) {
            }
        }
    } catch (winrt::hresult_error const& e) {
        d.error = Message(e);
        return d;
    }
    if (d.installed.empty()) {
        d.error = L"Dia isn't installed";
        return d;
    }
    if (d.feed.empty())
        d.feed = std::wstring(L"https://releases.diabrowser.com/windows/release/Dia.") + (arm ? L"arm64" : L"x64") +
                 L".appinstaller";
    std::wstring xml = GetText(d.feed, d.error);
    size_t at = xml.find(L"<MainPackage");
    if (at != std::wstring::npos) at = xml.find(L"Version=\"", at);
    if (at != std::wstring::npos) {
        at += 9;
        d.latest = xml.substr(at, xml.find(L'"', at) - at);
    } else if (d.error.empty()) {
        d.error = L"unexpected answer from Dia's update server";
    }
    return d;
}

// `check`: one key=value per line, UTF-8, newlines in values written as \n.
static int CmdCheck() {
    _setmode(_fileno(stdout), _O_BINARY);
    auto put = [](const char* key, const std::wstring& value) {
        std::string v;
        for (char c : N(value)) {
            if (c == '\\') v += "\\\\";
            else if (c == '\n') v += "\\n";
            else if (c != '\r') v += c;
        }
        printf("%s=%s\n", key, v.c_str());
    };
    put("tweaks.installed", WDT_VERSION);
    put("tweaks.source", RepoRoot().empty() ? L"release" : L"git");
    std::wstring dev = DevBuildReason();
    put("tweaks.dev", dev);
    Release r = LatestRelease();
    put("tweaks.latest", r.tag);
    put("tweaks.url", r.url.empty() ? L"https://github.com/" + Repo() + L"/releases" : r.url);
    put("tweaks.notes", r.notes);
    put("tweaks.available", dev.empty() && Newer(r.tag, WDT_VERSION) ? L"1" : L"0");
    put("tweaks.error", r.error);
    DiaVersion d = CheckDia();
    put("dia.installed", d.installed);
    put("dia.latest", d.latest);
    put("dia.available", Newer(d.latest, d.installed) ? L"1" : L"0");
    put("dia.error", d.error);
    fflush(stdout);
    return 0;
}

// Ends a command that may have its own console window (opened from Settings): leave the result on
// screen for a moment, or until a key is pressed when something went wrong.
static int Finish(int code) {
    DWORD list[4];
    if (GetConsoleProcessList(list, 4) > 1) return code;  // started from a shell, which stays open
    if (code) {
        wprintf(L"\nPress any key to close this window.\n");
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        FlushConsoleInputBuffer(in);
        INPUT_RECORD rec;
        DWORD n;
        while (ReadConsoleInputW(in, &rec, 1, &n))
            if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) break;
    } else {
        Sleep(2500);
    }
    return code;
}

static bool RunAndWait(const std::wstring& exe, std::wstring cmdline, DWORD* exitCode = nullptr) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.empty() ? nullptr : exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (exitCode) GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static bool Start(const std::wstring& exe, std::wstring cmdline, DWORD flags = 0) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.empty() ? nullptr : exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

static int UpdateDia(bool relaunch) {
    wprintf(L"Checking for a new Dia...\n");
    DiaVersion d = CheckDia();
    if (d.latest.empty()) {
        fwprintf(stderr, L"Couldn't check for Dia updates: %ls\n", d.error.c_str());
        return 1;
    }
    if (!Newer(d.latest, d.installed)) {
        wprintf(L"Dia %ls is up to date.\n", d.installed.c_str());
        return 0;
    }
    wprintf(L"Updating Dia %ls to %ls. Dia closes while it installs.\n", d.installed.c_str(), d.latest.c_str());
    CloseDia();
    try {
        wdm::PackageManager pm;
        auto op = pm.AddPackageByAppInstallerFileAsync(Uri(d.feed), wdm::AddPackageByAppInstallerOptions::ForceTargetAppShutdown,
                                                       pm.GetDefaultPackageVolume());
        op.Progress([](auto&&, wdm::DeploymentProgress const& p) { wprintf(L"\r  %3u%%", p.percentage); });
        auto result = op.get();
        wprintf(L"\n");
        if (result.ExtendedErrorCode() < 0) {
            fwprintf(stderr, L"Dia didn't update: %ls\n", std::wstring(result.ErrorText()).c_str());
            if (relaunch) StartTweaked(true);
            return 1;
        }
    } catch (winrt::hresult_error const& e) {
        fwprintf(stderr, L"\nDia didn't update: %ls\n", Message(e).c_str());
        if (relaunch) StartTweaked(true);
        return 1;
    }
    wprintf(L"Dia updated to %ls.\n", d.latest.c_str());
    if (relaunch && !StartTweaked(false)) return 1;
    return 0;
}

// Installing replaces this program, so the last step always runs in another process: `install` from
// the downloaded release, or cmd for a clone. Returns once that process is started (handedOff), which
// reopens Dia when it's done.
static int UpdateTweaks(bool force, bool* handedOff = nullptr) {
    std::wstring dev = DevBuildReason();
    if (!dev.empty()) {
        fwprintf(stderr, L"This WinDiaTweaks is a development build (%ls), so it isn't updated from here. Use git.\n",
                 dev.c_str());
        return 1;
    }
    wprintf(L"Checking for a new WinDiaTweaks...\n");
    Release r = LatestRelease();
    if (r.tag.empty()) {
        fwprintf(stderr, L"Couldn't check for WinDiaTweaks updates: %ls\n", r.error.c_str());
        return 1;
    }
    if (!force && !Newer(r.tag, WDT_VERSION)) {
        wprintf(L"WinDiaTweaks %ls is up to date.\n", WDT_VERSION);
        return 0;
    }
    std::wstring root = RepoRoot();
    if (!root.empty()) {
        wprintf(L"Updating WinDiaTweaks in %ls to %ls with git, then rebuilding it.\n", root.c_str(), r.tag.c_str());
        CloseDia();  // a DLL Dia has loaded can't be rebuilt
        std::wstring cmd = L"cmd.exe /d /c \"timeout /t 1 /nobreak >nul & cd /d " + Quote(root) +
                           L" && git pull --ff-only && call install.cmd /update || pause\"";
        if (!Start(L"", cmd)) {
            fwprintf(stderr, L"Couldn't run git (error %lu).\n", GetLastError());
            return 1;
        }
        if (handedOff) *handedOff = true;
        return 0;
    }
    if (r.zip.empty()) {
        fwprintf(stderr, L"Release %ls has no %ls to install.\n", r.tag.c_str(), kZip);
        return 1;
    }
    std::wstring dir = g_dir + L"\\update";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir + L"\\new", ec);
    wprintf(L"Downloading WinDiaTweaks %ls...\n", r.tag.c_str());
    std::wstring err, zip = dir + L"\\" + kZip;
    if (!Download(r.zip, zip, err)) {
        fwprintf(stderr, L"Download failed: %ls\n", err.c_str());
        return 1;
    }
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    DWORD code = 1;
    if (!RunAndWait(tar, Quote(tar) + L" -xf " + Quote(zip) + L" -C " + Quote(dir + L"\\new"), &code) || code ||
        GetFileAttributesW((dir + L"\\new\\diatweaks.exe").c_str()) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"Couldn't unpack %ls.\n", zip.c_str());
        return 1;
    }
    std::wstring next = dir + L"\\new\\diatweaks.exe";
    if (!Start(next, Quote(next) + L" install --to " + Quote(g_dir) + L" --wait " + std::to_wstring(GetCurrentProcessId()))) {
        fwprintf(stderr, L"Couldn't start the new version (error %lu).\n", GetLastError());
        return 1;
    }
    if (handedOff) *handedOff = true;
    return 0;
}

static int CmdUpdate(const std::wstring& what) {
    if (what == L"dia") return UpdateDia(true);
    if (what == L"tweaks" || what == L"windiatweaks") return UpdateTweaks(false);
    if (what == L"tweaks-force") return UpdateTweaks(true);
    // Both: Dia first without reopening it, as installing WinDiaTweaks reopens it anyway.
    bool wasOpen = FindDiaUiPid() != 0, handedOff = false;
    int a = UpdateDia(false);
    int b = UpdateTweaks(false, &handedOff);
    if (!handedOff && wasOpen && !FindDiaUiPid()) StartTweaked(false);
    return a | b;
}

// tweaks.conf holds the tweak definitions and your choices. Takes a newer file's definitions and puts
// each tweak's on/off and value from the old one back in.
static std::vector<std::string> MergeConf(const std::vector<std::string>& fresh, const std::vector<std::string>& mine) {
    auto header = [](const std::string& l, std::string& key, std::string& rest) {
        size_t a = l.find_first_not_of(" \t");
        if (a == std::string::npos || l[a] != '[') return false;
        size_t e = l.find(']', a);
        if (e == std::string::npos) return false;
        key = l.substr(a + 1, e - a - 1);
        rest = l.substr(e + 1);
        rest.erase(0, rest.find_first_not_of(" \t"));
        rest.erase(rest.find_last_not_of(" \t") + 1);
        return true;
    };
    auto isValue = [](const std::string& l) {
        size_t a = l.find_first_not_of(" \t");
        return a != std::string::npos && l.compare(a, 6, "value:") == 0;
    };
    std::map<std::string, std::string> state, value;
    std::string key, rest;
    for (auto& l : mine) {
        if (header(l, key, rest)) state[key] = rest;
        else if (!key.empty() && isValue(l)) value[key] = l;
    }
    std::vector<std::string> out;
    key.clear();
    for (auto& l : fresh) {
        std::string k;
        if (header(l, k, rest)) {
            key = k;
            out.push_back(state.count(k) ? "[" + k + "] " + state[k] : l);
        } else if (!key.empty() && isValue(l) && value.count(key)) {
            out.push_back(value[key]);
        } else {
            out.push_back(l);
        }
    }
    return out;
}

static std::vector<std::string> ReadFileLines(const std::wstring& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    return lines;
}

static bool MergeConfFiles(const std::wstring& fresh, const std::wstring& mine) {
    auto a = ReadFileLines(fresh), b = ReadFileLines(mine);
    if (a.empty()) return false;
    auto merged = b.empty() ? a : MergeConf(a, b);
    std::ofstream out(mine, std::ios::binary);
    for (auto& l : merged) out << l << "\n";
    return out.good();
}

// ---------------------------------------------------------------- shortcuts

#pragma pack(push, 2)
struct IconGroupEntry {  // one image in an RT_GROUP_ICON resource
    BYTE width, height, colors, reserved;
    WORD planes, bits;
    DWORD bytes;
    WORD id;
};
#pragma pack(pop)

// Copies the icon of the Dia installed on this PC into an .ico file, so the shortcuts look like Dia's
// own (and keep working when a Dia update moves its folder). Nothing of Dia's ships with WinDiaTweaks.
static bool SaveDiaIcon(const std::wstring& ico) {
    std::wstring exe = DiaExePath();
    if (exe.empty()) return false;
    HMODULE m = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return false;
    struct First {
        LPCWSTR id = nullptr;
        std::wstring name;
    } first;
    EnumResourceNamesW(
        m, RT_GROUP_ICON,
        [](HMODULE, LPCWSTR, LPWSTR name, LONG_PTR p) -> BOOL {
            auto f = reinterpret_cast<First*>(p);
            if (IS_INTRESOURCE(name)) f->id = name;
            else f->name = name, f->id = f->name.c_str();
            return FALSE;  // the first group is the app's icon
        },
        (LONG_PTR)&first);
    bool ok = false;
    HRSRC group = first.id ? FindResourceW(m, first.id, RT_GROUP_ICON) : nullptr;
    auto* dir = group ? static_cast<const BYTE*>(LockResource(LoadResource(m, group))) : nullptr;
    if (dir) {
        WORD count = *reinterpret_cast<const WORD*>(dir + 4);
        auto* entries = reinterpret_cast<const IconGroupEntry*>(dir + 6);
        // .ico layout: 6-byte header, 16-byte entry per image (offset instead of resource id), then images.
        std::string head(6, '\0'), body;
        head[2] = 1;
        memcpy(&head[4], &count, 2);
        DWORD offset = 6 + 16 * count;
        ok = count > 0;
        for (WORD i = 0; i < count && ok; i++) {
            HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(entries[i].id), RT_ICON);
            const void* data = r ? LockResource(LoadResource(m, r)) : nullptr;
            if (!data) {
                ok = false;
                break;
            }
            DWORD size = SizeofResource(m, r), at = offset + (DWORD)body.size();
            head.append(reinterpret_cast<const char*>(&entries[i]), 8);  // width .. bits
            head.append(reinterpret_cast<const char*>(&size), 4);
            head.append(reinterpret_cast<const char*>(&at), 4);
            body.append(static_cast<const char*>(data), size);
        }
        if (ok) {
            std::wstring tmp = ico + L".tmp";
            std::ofstream out(tmp, std::ios::binary);
            out << head << body;
            out.close();
            ok = out.good() && MoveFileExW(tmp.c_str(), ico.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }
    FreeLibrary(m);
    return ok;
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

static bool SaveShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& icon,
                         const wchar_t* args = L"", const wchar_t* desc = L"Open Dia with WinDiaTweaks") {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(target.c_str());
    link->SetArguments(args);
    link->SetWorkingDirectory(target.substr(0, target.find_last_of(L'\\')).c_str());
    link->SetDescription(desc);
    if (!icon.empty()) link->SetIconLocation(icon.c_str(), 0);
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Save(lnk.c_str(), TRUE));
    if (file) file->Release();
    link->Release();
    return ok;
}

// "Dia (Tweaks)" in the Start menu and on the desktop, with Dia's icon, opening DiaLauncher.exe in `dir`.
// The desktop one is added once (or always with `force`): if you delete it, updates leave it deleted.
static bool CreateShortcuts(const std::wstring& dir, bool force) {
    std::wstring target = dir + L"\\DiaLauncher.exe";
    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) target = dir + L"\\diatweaks.exe";
    std::wstring icon = dir + L"\\dia.ico";
    if (!SaveDiaIcon(icon) && GetFileAttributesW(icon.c_str()) == INVALID_FILE_ATTRIBUTES) icon.clear();

    std::wstring programs = KnownFolder(FOLDERID_Programs);
    bool ok = !programs.empty() && SaveShortcut(programs + L"\\Dia (Tweaks).lnk", target, icon);
    if (!ok) fwprintf(stderr, L"Couldn't add \"Dia (Tweaks)\" to the Start menu.\n");

    std::wstring desktop = KnownFolder(FOLDERID_Desktop), lnk = desktop + L"\\Dia (Tweaks).lnk";
    std::wstring marker = dir + L"\\desktop-shortcut.added";
    bool has = GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES;
    bool added = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!desktop.empty() && (force || has || !added)) {
        if (SaveShortcut(lnk, target, icon)) std::ofstream(marker).put('\n');
        else fwprintf(stderr, L"Couldn't add \"Dia (Tweaks)\" to the desktop.\n"), ok = false;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);  // show the new icon right away
    return ok;
}

// What the shortcuts run (DiaLauncher.exe, no console window): open Dia with the tweaks, like Dia's own
// icon would. Already open with them: Dia handles it as a click on its icon. Open without them: they're
// added to it in place, nothing is closed. Fails (so the launcher says so) if the tweaks don't attach or
// Dia can't be brought forward.
static int CmdOpen() {
    DWORD pid = FindDiaMainPid();
    if (!pid) {
        // The app in front a second after the click (Start or the taskbar has let go by then). If that's
        // still the one in front when Dia's window shows, you were waiting for Dia: bring it forward.
        // If you switched to something else meanwhile, leave it, as Windows would.
        static HWND volatile activeAfterClick = nullptr;
        CloseHandle(CreateThread(
            nullptr, 0, [](LPVOID) -> DWORD { return Sleep(1000), activeAfterClick = GetForegroundWindow(), 0; },
            nullptr, 0, nullptr));
        if (!(pid = StartTweaked(true))) return 1;
        HWND w = DiaWindow(pid), front = GetForegroundWindow();
        if (w && front != w && (!activeAfterClick || front == activeAfterClick)) {
            TakeForegroundRight();
            SetForegroundWindow(w);
        }
        return 0;
    }
    if (ExplicitLaunchInProgress())
        return fwprintf(stderr, L"Another Dia launch is in progress. Wait for it to finish.\n"), 1;
    bool tweaked = EnsureTweaked(pid, 15);
    if (!ActivateDia() && !OpenDiaViaShell()) {  // still show Dia, even when the tweaks failed
        fwprintf(stderr, L"Couldn't bring Dia to the front.\n");
        return 1;
    }
    return tweaked ? 0 : 1;
}

// ---------------------------------------------------------------- normal Dia launches

// "Patch Dia however it's opened" (normalLaunch in tweaks.conf, off by default). A small helper,
// DiaLauncher.exe --watch, runs from sign-in and attaches the tweaks the moment Dia's main process
// starts, however Dia was opened: its own icon or pin, the Start menu, a link from another app. Like
// the Dia (Tweaks) shortcut, that's before Dia's window first shows.
static const wchar_t kNormalLaunch[] = L"normalLaunch";

static bool ConfOn(const wchar_t* key) {
    for (auto& t : ParseTweaks(ReadLines()))
        if (t.key == key) return t.on;
    return false;
}

static std::wstring WatcherLink() { return KnownFolder(FOLDERID_Startup) + L"\\WinDiaTweaks.lnk"; }

static bool WatcherRunning() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, DT_WATCH_MUTEX);
    if (m) CloseHandle(m);
    return m != nullptr;
}

// Makes the sign-in entry (a shortcut in the Startup folder) and the running helper match the setting,
// and retires the PowerShell helper this replaces (auto-attach.ps1). Must run outside Dia: Dia's
// package would keep the entry to itself. Returns false if the entry couldn't be written.
static bool SyncWatcher(bool startNow) {
    Signal(L"Local\\WinDiaTweaksAutoAttachStop");
    DeleteFileW((KnownFolder(FOLDERID_Startup) + L"\\WinDiaTweaks helper.lnk").c_str());
    std::wstring launcher = g_dir + L"\\DiaLauncher.exe", lnk = WatcherLink();
    if (!ConfOn(kNormalLaunch)) {
        DeleteFileW(lnk.c_str());
        Signal(DT_WATCH_STOP);
        return true;
    }
    bool ok = SaveShortcut(lnk, launcher, L"", L"--watch", L"Adds WinDiaTweaks to Dia however Dia is opened");
    if (!ok) fwprintf(stderr, L"Couldn't add the WinDiaTweaks helper to sign-in.\n");
    if (startNow && !WatcherRunning()) Start(launcher, Quote(launcher) + L" --watch");
    return ok;
}

// Stops a running helper and waits for it to go, so its files can be replaced.
static void StopWatcher() {
    Signal(DT_WATCH_STOP);
    for (ULONGLONG end = GetTickCount64() + 5000; WatcherRunning() && GetTickCount64() < end;) Sleep(50);
}

static void WatchLog(const wchar_t* fmt, DWORD pid) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[256];
    swprintf(line, 256, L"%02u:%02u:%02u ", t.wHour, t.wMinute, t.wSecond);
    swprintf(line + 9, 247, fmt, pid);
    std::ofstream out(g_dir + L"\\watch.log", std::ios::app);
    out << N(line) << "\n";
}

// When a process started (0 if unknown): tells a new Dia from an old one that had the same id.
static ULONGLONG ProcessStartTime(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    FILETIME created{}, exited, kernel, user;
    GetProcessTimes(h, &created, &exited, &kernel, &user);
    CloseHandle(h);
    return (ULONGLONG)created.dwHighDateTime << 32 | created.dwLowDateTime;
}

// Process ids that created a window lately: whether each is Dia.exe, and whether it was looked at.
enum class SeenPid { other, dia, diaChecked };
static std::map<DWORD, SeenPid> g_seen;
static bool g_newDia = false;

// Windows reports every new window (hidden ones too). Dia's main process makes its first hidden windows
// a fraction of a second after it starts, long before its own window shows, so this tells us a Dia has
// started without listing every process on the PC several times a second.
static void CALLBACK OnWindowCreated(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    auto it = g_seen.find(pid);
    if (it == g_seen.end()) {
        std::wstring path = Lower(ProcessPath(pid));
        bool dia = path.size() >= 8 && path.compare(path.size() - 8, 8, L"\\dia.exe") == 0;
        it = g_seen.emplace(pid, dia ? SeenPid::dia : SeenPid::other).first;
    }
    if (it->second == SeenPid::dia) {
        it->second = SeenPid::diaChecked;  // look once per process, not on every window it makes
        g_newDia = true;
    }
}

// DiaLauncher.exe --watch. Wakes when a Dia process makes its first window (and every 10 s, in case
// that was missed), and attaches to a Dia it hasn't handled yet; stops when the setting goes off or
// `diatweaks auto off` asks it to.
static int CmdWatch() {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, DT_WATCH_MUTEX);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;  // one helper is enough
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, DT_WATCH_STOP);
    _wfreopen((g_dir + L"\\watch.log").c_str(), L"a", stderr);  // no console: keep the error messages
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (!ConfOn(kNormalLaunch)) return SyncWatcher(false), 0;
    SyncWatcher(false);  // started from Dia's Settings: add the sign-in entry
    WatchLog(L"helper started (pid %lu)", GetCurrentProcessId());
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, nullptr, OnWindowCreated, 0, 0,
                                         WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    DWORD handled = 0;
    ULONGLONG handledStart = 0, confCheck = GetTickCount64(), nextPoll = 0;
    for (;;) {
        if (MsgWaitForMultipleObjects(1, &stop, FALSE, 1000, QS_ALLINPUT) == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);  // runs OnWindowCreated
        ULONGLONG now = GetTickCount64();
        if (now - confCheck > 2000) {
            confCheck = now;
            if (!ConfOn(kNormalLaunch)) break;  // switched off in Settings
        }
        bool poll = now >= nextPoll;
        if (!g_newDia && !poll) continue;
        g_newDia = false;
        if (poll) {
            nextPoll = now + 10000;
            if (g_seen.size() > 2000) g_seen.clear();  // ids get reused
        }
        DWORD pid = FindDiaMainPid();
        if (!pid) {  // Dia is closed: forget it, as a new Dia may get the same process ids
            handled = 0;
            for (auto it = g_seen.begin(); it != g_seen.end();)
                it = it->second == SeenPid::other ? std::next(it) : g_seen.erase(it);
            continue;
        }
        ULONGLONG started = ProcessStartTime(pid);
        if (pid == handled && started == handledStart) continue;
        // An explicit launch attaches on its own, after the startup debugger has detached if used.
        if (ExplicitLaunchInProgress()) continue;
        handled = pid, handledStart = started;  // one go per Dia: it retries inside, and a failure shouldn't loop
        if (EnsureTweaked(pid, 40)) WatchLog(L"tweaks attached to Dia (pid %lu)", pid);
        else WatchLog(L"tweaks didn't attach to Dia (pid %lu), see tap.log", pid);
    }
    if (hook) UnhookWinEvent(hook);
    if (!ConfOn(kNormalLaunch)) DeleteFileW(WatcherLink().c_str());
    WatchLog(L"helper stopped (pid %lu)", GetCurrentProcessId());
    CloseHandle(stop);
    CloseHandle(mutex);
    return 0;
}

// `diatweaks auto on|off|status`.
static int CmdAuto(const std::wstring& what) {
    if (what == L"on" || what == L"off") {
        if (int r = SetTweak(kNormalLaunch, what == L"on")) return r;
        if (!SyncWatcher(true)) return 1;
        if (what == L"off") StopWatcher();  // report the helper's state once it has settled
        for (ULONGLONG end = GetTickCount64() + 3000; what == L"on" && !WatcherRunning() && GetTickCount64() < end;)
            Sleep(50);
    } else if (what != L"status") {
        fwprintf(stderr, L"Use: diatweaks auto on|off|status\n");
        return 1;
    }
    bool on = ConfOn(kNormalLaunch);
    wprintf(L"Patch Dia however it's opened: %ls\n", on ? L"on" : L"off");
    wprintf(L"  helper running:   %ls\n", WatcherRunning() ? L"yes" : L"no");
    wprintf(L"  starts at sign-in: %ls\n",
            GetFileAttributesW(WatcherLink().c_str()) != INVALID_FILE_ATTRIBUTES ? L"yes" : L"no");
    return 0;
}

// Copy, or when the file is in use (a DLL Dia still has loaded) move it aside first.
static bool Replace(const std::wstring& from, const std::wstring& to) {
    for (int i = 0; i < 20; i++) {
        if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return true;
        std::wstring old = to + L".old";
        DeleteFileW(old.c_str());
        if (MoveFileExW(to.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) && CopyFileW(from.c_str(), to.c_str(), FALSE))
            return true;
        Sleep(250);
    }
    return false;
}

// `install [--to <folder>] [--wait <pid>]`: puts this copy of WinDiaTweaks (an unpacked release) in
// place, keeping the choices in the old tweaks.conf, then opens Dia with it. Default folder:
// %USERPROFILE%\WinDiaTweaks\bin (not AppData: Dia's writes to AppData would be redirected into its
// package, out of sight of this program).
static int CmdInstall(int argc, wchar_t** argv) {
    std::wstring to;
    DWORD wait = 0;
    for (int i = 2; i + 1 < argc; i++) {
        if (Lower(argv[i]) == L"--to") to = argv[++i];
        else if (Lower(argv[i]) == L"--wait") wait = wcstoul(argv[++i], nullptr, 10);
    }
    if (to.empty()) {
        wchar_t home[MAX_PATH];
        if (!GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH)) return 1;
        to = std::wstring(home) + L"\\WinDiaTweaks\\bin";
    }
    while (!to.empty() && (to.back() == L'\\' || to.back() == L'/')) to.pop_back();
    if (wait)
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, wait)) WaitForSingleObject(h, 30000), CloseHandle(h);
    if (_wcsicmp(to.c_str(), g_dir.c_str()) == 0) {
        fwprintf(stderr, L"Already installed in %ls.\n", to.c_str());
        return 1;
    }
    wprintf(L"Installing WinDiaTweaks %ls in %ls...\n", WDT_VERSION, to.c_str());
    SHCreateDirectoryExW(nullptr, to.c_str(), nullptr);
    CloseDia();
    StopWatcher();  // it keeps DiaLauncher.exe in use
    for (const wchar_t* name : {L"diatweaks.exe", L"DiaLauncher.exe", L"DiaTweaksTap.dll", L"uninstall.cmd", L"LICENSE"}) {
        std::wstring from = g_dir + L"\\" + name;
        if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (!Replace(from, to + L"\\" + name)) {
            fwprintf(stderr, L"Couldn't replace %ls\\%ls (error %lu). Close Dia and try again.\n", to.c_str(), name,
                     GetLastError());
            return 1;
        }
    }
    if (!MergeConfFiles(g_dir + L"\\tweaks.conf", to + L"\\tweaks.conf")) {
        fwprintf(stderr, L"Couldn't write %ls\\tweaks.conf.\n", to.c_str());
        return 1;
    }
    CreateShortcuts(to, false);
    DeleteFileW((to + L"\\auto-attach.ps1").c_str());  // replaced by DiaLauncher.exe --watch
    std::wstring tool = to + L"\\diatweaks.exe";
    RunAndWait(tool, Quote(tool) + L" sync-helper");  // start the new helper, if the setting is on
    wprintf(L"Opening Dia with WinDiaTweaks %ls...\n", WDT_VERSION);
    std::wstring exe = to + L"\\diatweaks.exe";
    DWORD code = 1;
    RunAndWait(exe, Quote(exe) + L" restart", &code);
    if (!code) wprintf(L"Done. Open Dia from \"Dia (Tweaks)\" in the Start menu or on the desktop from now on.\n");
    return (int)code;
}

// `detach <command...>`: runs diatweaks <command...> in its own console window, outside Dia. Settings
// starts updates this way: Dia's package would otherwise own the process (and close it with Dia).
static int CmdDetach(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = Quote(self);
    for (int i = 2; i < argc; i++) cmd += L" " + Quote(argv[i]);
    return Start(self, cmd, CREATE_NEW_CONSOLE) ? 0 : 1;
}

// Left behind by an earlier update: a replaced file still in use, and the unpacked release.
static void CleanUpAfterUpdate() {
    for (const wchar_t* name : {L"diatweaks.exe.old", L"DiaLauncher.exe.old", L"DiaTweaksTap.dll.old"}) DeleteFileW((g_dir + L"\\" + name).c_str());
    std::error_code ec;
    std::filesystem::remove_all(g_dir + L"\\update", ec);
}

static void Usage() {
    wprintf(
        L"WinDiaTweaks " WDT_VERSION L" (unofficial)\n\n"
        L"  diatweaks                 start Dia with tweaks (or reload if already tweaked)\n"
        L"  diatweaks restart         close Dia and start it again with tweaks\n"
        L"  diatweaks attach          attach to running Dia without restarting it\n"
        L"  diatweaks auto on|off     add the tweaks however Dia is opened (a helper runs from sign-in)\n"
        L"  diatweaks list            show tweaks and whether they're on\n"
        L"  diatweaks enable <name>   turn a tweak on (name or 'all')\n"
        L"  diatweaks disable <name>  turn a tweak off\n"
        L"  diatweaks status          Dia process + tweak state\n"
        L"  diatweaks set <name> <v>  set a tweak's option (e.g. set windowMaterial acrylic)\n"
        L"  diatweaks dump            write Dia's UI tree to tree.txt (for making tweaks)\n"
        L"  diatweaks probe <names>   describe live elements by name (for making tweaks)\n"
        L"  diatweaks shortcuts       add \"Dia (Tweaks)\" with Dia's icon to the Start menu and desktop\n"
        L"  diatweaks check           show installed and newest versions of Dia and WinDiaTweaks\n"
        L"  diatweaks update [what]   install updates: dia, tweaks, or both (the default)\n"
        L"  diatweaks version         show this WinDiaTweaks version\n\n"
        L"Every tweak can also be switched in Dia: Settings > Patches.\n"
        L"Visual tweaks reload instantly. The forced sign-in bypass requires a restart.\n");
}

int wmain(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_dir = self;
    g_dir = g_dir.substr(0, g_dir.find_last_of(L'\\'));
    std::wstring cmd = argc > 1 ? Lower(argv[1]) : L"apply";
    // Update commands wait on WinRT calls, which must not block a single-threaded apartment.
    bool winrtCmd = cmd == L"check" || cmd == L"update" || cmd == L"install";
    CoInitializeEx(nullptr, winrtCmd ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);

#ifdef WDT_LAUNCHER
    if (cmd == L"--watch") return CmdWatch();
    // DiaLauncher.exe: no console. Says what went wrong in a message box, as nothing shows its output.
    if (argc < 2) {
        CleanUpAfterUpdate();
        int r = CmdOpen();
        if (r)
            MessageBoxW(nullptr,
                        L"Dia didn't open with WinDiaTweaks. For details, run diatweaks in PowerShell from this "
                        L"folder, or see tap.log in it.",
                        L"WinDiaTweaks", MB_OK | MB_ICONWARNING);
        return r;
    }
#endif
    if (cmd == L"version" || cmd == L"--version") return wprintf(L"%ls\n", WDT_VERSION), 0;
    if (cmd == L"check") return CmdCheck();
    if (cmd == L"update") return Finish(CmdUpdate(argc > 2 ? Lower(argv[2]) : L"all"));
    if (cmd == L"install") return Finish(CmdInstall(argc, argv));
    if (cmd == L"detach" && argc > 2) return CmdDetach(argc, argv);
    if (cmd == L"merge-conf" && argc > 3) return MergeConfFiles(argv[2], argv[3]) ? 0 : 1;
    if (cmd == L"apply" || cmd == L"launch" || cmd == L"restart") CleanUpAfterUpdate();
    if (cmd == L"apply") return CmdApply();
    if (cmd == L"attach") return CmdAttach();
    if (cmd == L"auto") return CmdAuto(argc > 2 ? Lower(argv[2]) : L"status");
    if (cmd == L"sync-helper") return SyncWatcher(true) ? 0 : 1;
    if (cmd == L"stop-helper") return StopWatcher(), 0;
    if (cmd == L"launch" || cmd == L"restart") {
        int result = StartTweaked(false) ? 0 : 1;
        return cmd == L"restart" ? Finish(result) : result;
    }
    if (cmd == L"open") return CmdOpen();
    if (cmd == L"shortcuts") return CreateShortcuts(g_dir, !(argc > 2 && Lower(argv[2]) == L"--update")) ? 0 : 1;
    if (cmd == L"list") return ListTweaks(), 0;
    if (cmd == L"status") return CmdStatus();
    if (cmd == L"dump") return CmdDump();
    if (cmd == L"probe") return CmdProbe(argc, argv);
    if (cmd == L"set" && argc > 3) return CmdSet(argv[2], argv[3]);
    if ((cmd == L"enable" || cmd == L"disable") && argc > 2) {
        std::wstring name = argv[2];
        for (int i = 3; i < argc; i++) name += std::wstring(L" ") + argv[i];
        bool wasAuto = ConfOn(kNormalLaunch);
        int r = SetTweak(name, cmd == L"enable");
        if (!r && ConfOn(kNormalLaunch) != wasAuto) SyncWatcher(true);
        if (!r) {
            DWORD pid = FindDiaUiPid();
            if (pid && TapLoaded(pid)) Signal(DT_EVENT_RELOAD);
        }
        return r;
    }
    Usage();
    return cmd == L"help" || cmd == L"-h" || cmd == L"--help" ? 0 : 1;
}
