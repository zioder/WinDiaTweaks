#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "local_browsing.h"
#include "dia_native_layout.h"
#include <appmodel.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <map>
#include <vector>
#include <mutex>
#include <utility>

#pragma comment(lib, "bcrypt.lib")

namespace local_browsing {
namespace {
// Fixtures describe their own machine-code child, not a production release.
#ifdef LOCAL_BROWSING_TEST
constexpr std::array<BYTE, 6> kWindowOriginal{0x0F,0x84,0xE7,0x03,0x00,0x00};
constexpr std::array<BYTE, 6> kNops{0x90,0x90,0x90,0x90,0x90,0x90};
constexpr std::array<BYTE, 2> kRevocationOriginal{0x75,0x4D};
constexpr std::array<BYTE, 2> kRevocationChoice{0x90,0x90};
// Swift's immutable signed-out command array: count, capacity flags, then bytes.
// Append only Settings, default-browser, import, new-window, and incognito IDs
// inside its existing allocation. The object header and next object are untouched.
constexpr std::array<BYTE, 32> kCommandsOriginal{
    10,0,0,0,0,0,0,0,20,0,0,0,0,0,0,0,
    7,8,9,0,20,22,26,16,17,15,0,0,0,0,0,0
};
constexpr std::array<BYTE, 32> kCommandsLocal{
    15,0,0,0,0,0,0,0,30,0,0,0,0,0,0,0,
    7,8,9,0,20,22,26,16,17,15,4,5,6,11,12,0
};
// These predicates have only New Window / Incognito command callers. Redirect
// absent identities to the existing engine/termination checks, preserving ARC.
constexpr std::array<BYTE, 6> kMissingOriginal{0x0F,0x84,0x90,0,0,0};
constexpr std::array<BYTE, 6> kMissingReady{0x0F,0x84,0x63,0,0,0};
constexpr std::array<BYTE, 2> kEmptyOriginal{0x74,0x2D};
constexpr std::array<BYTE, 2> kEmptyReady{0x74,0};
constexpr std::array<BYTE, 6> kQueuedOriginal{0x0F,0x84,0xB2,0x04,0,0};
#endif
std::mutex launchMutex;
volatile LONG stopRequested = 0;
SRWLOCK cleanupLock = SRWLOCK_INIT;
HANDLE cleanupDone = nullptr;

void SetCleanupEvent(HANDLE done) {
    AcquireSRWLockExclusive(&cleanupLock);
    cleanupDone = done;
    ReleaseSRWLockExclusive(&cleanupLock);
}

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& h) noexcept : value(std::exchange(h.value, nullptr)) {}
    Handle& operator=(Handle&& h) noexcept {
        if (this != &h) {
            if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = std::exchange(h.value, nullptr);
        }
        return *this;
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

BOOL WINAPI ConsoleStop(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT &&
        event != CTRL_LOGOFF_EVENT && event != CTRL_SHUTDOWN_EVENT) return FALSE;
    InterlockedExchange(&stopRequested, 1);
    // Keep the event alive until this callback has finished waiting on it.
    AcquireSRWLockShared(&cleanupLock);
    if (cleanupDone) WaitForSingleObject(cleanupDone, 4000);
    ReleaseSRWLockShared(&cleanupLock);
    return TRUE;
}
struct ConsoleRegistration {
    HANDLE done;
    ~ConsoleRegistration() {
        // Signal before taking the exclusive lock: callbacks can be holding
        // the shared lock while waiting for cleanup to finish.
        SetEvent(done);
        SetConsoleCtrlHandler(ConsoleStop, FALSE);
        SetCleanupEvent(nullptr); // Drain existing waits before the owner closes done.
    }
};
struct EmergencySession {
    HANDLE process;
    DWORD pid;
    DEBUG_EVENT& event;
    bool& pending;
    bool safe = false;
    ~EmergencySession() {
        if (safe) return;
        // Also covers a C++ allocation exception in the tracking containers.
        TerminateProcess(process, ERROR_CANCELLED);
        if (pending) ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
        DebugActiveProcessStop(pid);
    }
};

using Fingerprint = std::array<BYTE, 32>;
bool HashFile(HANDLE file, Fingerprint& digest) {
    if (!file || file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER zero{}, saved{};
    if (!SetFilePointerEx(file, zero, &saved, FILE_CURRENT) ||
        !SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<BYTE, 65536> buffer{};
    while (ok) {
        DWORD count = 0;
        if (!ReadFile(file, buffer.data(), (DWORD)buffer.size(), &count, nullptr)) { ok = false; break; }
        if (!count) break;
        ok = BCryptHashData(hash, buffer.data(), count, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest.data(), (ULONG)digest.size(), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!SetFilePointerEx(file, saved, nullptr, FILE_BEGIN)) ok = false;
    return ok;
}

template<class T> bool Read(HANDLE process, uintptr_t address, T& result) {
    SIZE_T count = 0;
    return ReadProcessMemory(process, (void*)address, &result, sizeof(result), &count) && count == sizeof(result);
}

// Test builds inject one API failure at a time. Production has no fault controls.
#ifdef LOCAL_BROWSING_TEST
enum class Fault { None, Write, Flush, Protection, Detach, Restore };
Fault testFault = Fault::None;
unsigned faultSkip = 0;
bool FaultOnce(Fault f) {
    if (testFault != f) return false;
    if (faultSkip) { --faultSkip; return false; }
    testFault = Fault::None; SetLastError(ERROR_ACCESS_DENIED); return true;
}
#endif

bool ReadBytes(HANDLE p, uintptr_t address, std::vector<BYTE>& bytes) {
    SIZE_T count = 0;
    return !bytes.empty() && ReadProcessMemory(p, (void*)address, bytes.data(), bytes.size(), &count) && count == bytes.size();
}
bool WriteBytes(HANDLE p, uintptr_t address, const std::vector<BYTE>& bytes) {
    SIZE_T written = 0;
#ifdef LOCAL_BROWSING_TEST
    if (FaultOnce(Fault::Write)) {
        // Model a real short write, not merely a failure before modification.
        WriteProcessMemory(p, (void*)address, bytes.data(), bytes.size() / 2, &written);
        SetLastError(ERROR_PARTIAL_COPY);
        return false;
    }
#endif
    return WriteProcessMemory(p, (void*)address, bytes.data(), bytes.size(), &written) && written == bytes.size();
}
bool Flush(HANDLE p, uintptr_t address, SIZE_T length) {
#ifdef LOCAL_BROWSING_TEST
    if (FaultOnce(Fault::Flush)) return false;
#endif
    return FlushInstructionCache(p, (void*)address, length) != FALSE;
}
bool Protect(HANDLE p, uintptr_t address, SIZE_T length, DWORD protection, DWORD& previous, bool restoring) {
#ifdef LOCAL_BROWSING_TEST
    if (restoring && FaultOnce(Fault::Protection)) return false;
#else
    (void)restoring;
#endif
    return VirtualProtectEx(p, (void*)address, length, protection, &previous) != FALSE;
}

struct Patch {
    std::vector<BYTE> original;
    std::vector<BYTE> replacement;
    DWORD rva = 0;
    uintptr_t address = 0;
    DWORD protection = 0;
    DWORD writableProtection;
    bool changed = false;
    bool protectionChanged = false;
    template<size_t N> Patch(std::array<BYTE, N> before, std::array<BYTE, N> after, DWORD writable = PAGE_EXECUTE_READWRITE)
        : original(before.begin(), before.end()), replacement(after.begin(), after.end()), writableProtection(writable) {}
    explicit Patch(const dia_native::PatchSite& site)
        : original(site.original.begin(), site.original.end()), replacement(site.replacement.begin(), site.replacement.end()),
          rva(site.rva), writableProtection(site.executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE) {}

    // Caller must have all process threads stopped by a debug event.
    bool Apply(HANDLE p, uintptr_t at) {
        std::vector<BYTE> actual(original.size());
        if (original.empty() || original.size() > 4096 || original.size() != replacement.size() ||
            changed || protectionChanged || !ReadBytes(p, at, actual) || actual != original) return false;
        address = at;
        if (!Protect(p, address, original.size(), writableProtection, protection, false)) return false;
        protectionChanged = true;
        // Treat a failed/partial write as dirty: rollback restores exact originals.
        changed = true;
        bool ok = WriteBytes(p, address, replacement) && Flush(p, address, original.size());
        DWORD ignored = 0;
        if (Protect(p, address, original.size(), protection, ignored, true)) protectionChanged = false;
        else ok = false;
        std::vector<BYTE> verified(original.size());
        return ok && ReadBytes(p, address, verified) && verified == replacement;
    }
    bool Rollback(HANDLE p) {
        if (!changed && !protectionChanged) return true;
        DWORD ignored = 0;
        std::vector<BYTE> current(original.size());
        bool owned = ReadBytes(p, address, current);
        for (size_t i = 0; owned && i < original.size(); ++i)
            owned = current[i] == original[i] || current[i] == replacement[i];
        if (!owned) {
            // A concurrent foreign modification is not ours to overwrite.
            if (protectionChanged && Protect(p, address, original.size(), protection, ignored, true)) protectionChanged = false;
            return false;
        }
        if (!protectionChanged && !Protect(p, address, original.size(), writableProtection, ignored, false)) return false;
        protectionChanged = true;
        bool ok = WriteBytes(p, address, original) && Flush(p, address, original.size());
        if (Protect(p, address, original.size(), protection, ignored, true)) protectionChanged = false;
        else ok = false;
        std::vector<BYTE> verified(original.size());
        if (ok && ReadBytes(p, address, verified) && verified == original) { changed = false; return true; }
        return false;
    }
};

struct Thread {
    Handle handle;
    CONTEXT saved{};
    bool armed = false;
};
bool Arm(Thread& thread, uintptr_t first, uintptr_t second) {
    CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(thread.handle.value, &c)) return false;
    if (!thread.armed) {
        if (c.Dr7 & 0xFF) return false; // Do not replace another debugger's breakpoints.
        thread.saved = c;
    }
    c.Dr0 = first; c.Dr1 = second; c.Dr6 = 0; c.Dr7 = 5;
    if (!SetThreadContext(thread.handle.value, &c)) return false;
    thread.armed = true;
    return true;
}
bool Restore(Thread& thread) {
    if (!thread.armed) return true;
    DWORD code = 0;
    if (GetExitCodeThread(thread.handle.value, &code) && code != STILL_ACTIVE) { thread.armed = false; return true; }
#ifdef LOCAL_BROWSING_TEST
    if (FaultOnce(Fault::Restore)) return false;
#endif
    CONTEXT c = thread.saved; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!SetThreadContext(thread.handle.value, &c)) return false;
    thread.armed = false;
    return true;
}
bool Detach(DWORD pid) {
#ifdef LOCAL_BROWSING_TEST
    if (FaultOnce(Fault::Detach)) return false;
#endif
    return DebugActiveProcessStop(pid) != FALSE;
}

struct NativeState {
    BYTE termination = 0, engine = 0, restored = 0, launch = 0;
    uintptr_t auth = 0, manager = 0;
};
bool State(HANDLE p, uintptr_t base, uintptr_t self, const dia_native::NativeLayout& layout, NativeState& s) {
    uintptr_t metadata = 0, isa = 0, kind = 0, descriptor = 0;
    if (!self || !layout.metadataCache || !Read(p, base + layout.metadataCache, metadata) || !metadata ||
        !Read(p, self, isa) || isa != metadata || !Read(p, metadata, kind)) return false;
    // Swift's published 64-bit class metadata ABI distinguishes native classes
    // from Objective-C-compatible classes; neither choice is a Dia version gate.
    const bool native = kind == 0;
    if (!native && kind <= 2047) return false;
    uint32_t instanceSize = 0;
    if (!Read(p, metadata + (native ? 40 : 64), descriptor) || descriptor != base + layout.applicationDescriptor ||
        !Read(p, metadata + (native ? 24 : 48), instanceSize) || instanceSize < 16 || instanceSize > 1024 * 1024) return false;
    std::map<std::string, uintptr_t> fields;
    for (auto& field : layout.applicationFields) {
        uintptr_t offset = 0;
        if (!Read(p, metadata + (layout.fieldOffsetVectorWords + field.index) * sizeof(uintptr_t), offset) ||
            offset < 16 || offset >= instanceSize || (field.codeOffset && offset != field.codeOffset)) return false;
        fields.emplace(field.name, offset);
    }
    auto address = [&](const char* name, size_t width) -> uintptr_t {
        auto at = fields.find(name);
        return at != fields.end() && at->second <= instanceSize - width ? self + at->second : 0;
    };
    if (layout.launchTagOffset >= instanceSize) return false;
    return Read(p, address("terminationState", 1), s.termination) &&
        Read(p, address("hasInitializedBrowserEngine", 1), s.engine) &&
        Read(p, address("hasFinishedRestoringWindows", 1), s.restored) &&
        Read(p, self + layout.launchTagOffset, s.launch) &&
        Read(p, address("authController", sizeof(uintptr_t)), s.auth) &&
        Read(p, address("windowCoordinationController", sizeof(uintptr_t)), s.manager);
}
bool Ready(const NativeState& s, const dia_native::NativeLayout& layout) {
    return s.termination == layout.terminationNilTag && s.engine == 1 && s.auth && s.manager;
}
uintptr_t Register(const CONTEXT& c, uint8_t reg) {
    switch (reg) {
    case 0: return c.Rax; case 1: return c.Rcx; case 2: return c.Rdx; case 3: return c.Rbx;
    case 4: return c.Rsp; case 5: return c.Rbp; case 6: return c.Rsi; case 7: return c.Rdi;
    case 8: return c.R8; case 9: return c.R9; case 10: return c.R10; case 11: return c.R11;
    case 12: return c.R12; case 13: return c.R13; case 14: return c.R14; case 15: return c.R15;
    default: return 0;
    }
}

bool Image(HANDLE p, uintptr_t base, IMAGE_NT_HEADERS64& nt) {
    IMAGE_DOS_HEADER dos{};
    return Read(p, base, dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && dos.e_lfanew < 4096 &&
        Read(p, base + dos.e_lfanew, nt) && nt.Signature == IMAGE_NT_SIGNATURE &&
        nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 && nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
}

struct Options {
    bool fixture = false;
    dia_native::NativeLayout layout;
    Fingerprint fingerprint{};
    DWORD timeout = 30000;
#ifdef LOCAL_BROWSING_TEST
    DWORD first = 0, second = 0, revocation = 0, commands = 0, missing = 0, empty = 0, queued = 0;
#endif
};
struct Outcome {
    bool success = false, detached = false, rollback = false, terminated = false;
    DWORD pid = 0;
    std::wstring error;
};

bool RetailEnvironment(std::vector<wchar_t>& result) {
    // Debugging at process creation otherwise opts Dia into Windows' debug
    // heap for its entire lifetime, even after we detach. Change only the
    // child's heap selection; keep every other inherited environment entry.
    struct Inherited {
        LPWCH block = GetEnvironmentStringsW();
        ~Inherited() { if (block) FreeEnvironmentStringsW(block); }
    } inherited;
    if (!inherited.block) return false;
    constexpr wchar_t overridePrefix[] = L"_NO_DEBUG_HEAP=";
    std::vector<std::wstring> entries;
    for (const wchar_t* entry = inherited.block; *entry; entry += wcslen(entry) + 1)
        if (_wcsnicmp(entry, overridePrefix, std::size(overridePrefix) - 1) != 0) entries.emplace_back(entry);
    entries.emplace_back(L"_NO_DEBUG_HEAP=1");
    auto keyLength = [](const std::wstring& entry) {
        // Hidden per-drive variables are named '=C:', with a second '='
        // separating the value. Keep these entries as well as ordinary ones.
        auto separator = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
        return separator == std::wstring::npos ? entry.size() : separator;
    };
    std::stable_sort(entries.begin(), entries.end(), [&](const auto& left, const auto& right) {
        return CompareStringOrdinal(left.data(), (int)keyLength(left), right.data(), (int)keyLength(right), TRUE) == CSTR_LESS_THAN;
    });
    result.clear();
    for (const auto& entry : entries) {
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(0);
    }
    result.push_back(0);
    return true;
}

Outcome Run(const std::wstring& exe, const std::wstring& command, const std::wstring& directory, const Options& options) {
    Outcome out;
    auto layout = options.layout;
#ifdef LOCAL_BROWSING_TEST
    if (options.fixture) {
        layout.bootstrapBranch = options.first;
        layout.windowBranch = options.second;
        auto add = [&](const char* name, DWORD rva, auto before, auto after, bool executable = true) {
            layout.patches.push_back({name, rva, {before.begin(), before.end()}, {after.begin(), after.end()}, executable});
        };
        add("fixture window", options.second, kWindowOriginal, kNops);
        add("fixture automatic sign-out", options.revocation, kRevocationOriginal, kRevocationChoice);
        add("fixture commands", options.commands, kCommandsOriginal, kCommandsLocal, false);
        add("fixture absent identity", options.missing, kMissingOriginal, kMissingReady);
        add("fixture empty identity", options.empty, kEmptyOriginal, kEmptyReady);
        add("fixture queued window", options.queued, kQueuedOriginal, kNops);
    }
#endif
    if (!layout.bootstrapBranch || !layout.windowBranch || layout.patches.empty() || layout.patches.size() > 16) {
        out.error = L"Required native browsing capabilities were not resolved."; return out;
    }
    std::vector<Patch> patches;
    patches.reserve(layout.patches.size());
    for (auto& site : layout.patches) {
        uint64_t end = uint64_t(site.rva) + site.original.size();
        if (!site.rva || site.original.empty() || site.original.size() > 4096 || site.original.size() != site.replacement.size() ||
            (!options.fixture && end > layout.imageSize)) {
            out.error = L"The native capability plan has an invalid byte range."; return out;
        }
        for (auto& other : patches)
            if (site.rva < uint64_t(other.rva) + other.original.size() && other.rva < end) {
                out.error = L"The native capability plan contains overlapping changes."; return out;
            }
        patches.emplace_back(site);
    }
    std::vector<wchar_t> environment;
    if (!RetailEnvironment(environment)) {
        out.error = L"Could not prepare Dia's normal-heap startup environment."; return out;
    }
    Handle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!done) { out.error = L"Could not create the startup cleanup event."; return out; }
    SetCleanupEvent(done.value);
    InterlockedExchange(&stopRequested, 0);
    if (!SetConsoleCtrlHandler(ConsoleStop, TRUE)) { SetCleanupEvent(nullptr); out.error = L"Could not install startup cleanup."; return out; }
    ConsoleRegistration console{done.value};
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(command.begin(), command.end()); cmd.push_back(0);
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS | CREATE_UNICODE_ENVIRONMENT,
        environment.data(), directory.c_str(), &si, &pi)) {
        out.error = L"Could not start Dia (Windows error " + std::to_wstring(GetLastError()) + L").";
        return out;
    }
    Handle process(pi.hProcess), initialThread(pi.hThread);
    bool pending = false;
    DEBUG_EVENT event{};
    EmergencySession emergency{process.value, pi.dwProcessId, event, pending};
    out.pid = pi.dwProcessId;
    bool ok = DebugSetProcessKillOnExit(FALSE) != FALSE;
    auto fail = [&](const wchar_t* message) { ok = false; if (out.error.empty()) out.error = message; };
    if (!ok) fail(L"Could not establish safe debugger cleanup.");
    std::map<DWORD, Thread> threads;
    uintptr_t base = 0, self = 0;
    bool initialBreak = false, bootstrapSeen = false, bootstrapChanged = false;
    bool ready = false, cleaning = false, exited = false, transactionApplied = false;
    DWORD disposition = DBG_CONTINUE;
    auto rollback = [&]() {
        bool result = true;
        for (auto it = patches.rbegin(); it != patches.rend(); ++it)
            if (!it->Rollback(process.value)) result = false;
        return result;
    };
    auto ntdllBase = (uintptr_t)GetModuleHandleW(L"ntdll.dll");
    auto localDos = (IMAGE_DOS_HEADER*)ntdllBase;
    auto localNt = (IMAGE_NT_HEADERS64*)(ntdllBase + localDos->e_lfanew);
    DWORD ntdllSize = localNt->OptionalHeader.SizeOfImage, ntdllStamp = localNt->FileHeader.TimeDateStamp;
    ULONGLONG started = GetTickCount64(), cleanupAt = 0;
    while (!exited) {
        NativeState state;
        bool applied = transactionApplied && std::all_of(patches.begin(), patches.end(), [](const Patch& p) { return p.changed && !p.protectionChanged; });
        if (!options.fixture && bootstrapSeen && applied && base && State(process.value, base, self, layout, state) &&
            Ready(state, layout) && state.restored == 1 && state.launch == layout.launchCompleteSuccessTag) ready = true;
        if (!cleaning && (ready || !ok || stopRequested || GetTickCount64() - started >= options.timeout)) {
            if (!ready && ok) fail(stopRequested ? L"Local browsing startup was interrupted." : L"Dia did not finish local browsing startup in time.");
            cleaning = true; cleanupAt = GetTickCount64();
            if (!DebugBreakProcess(process.value)) { fail(L"Could not stop Dia for safe cleanup."); break; }
        }
        if (cleaning && GetTickCount64() - cleanupAt > 3000) { fail(L"Dia did not acknowledge startup cleanup."); break; }
        if (!WaitForDebugEventEx(&event, 100)) {
            if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
            fail(L"The native startup debugger failed while waiting."); break;
        }
        pending = true; disposition = DBG_CONTINUE;
        bool cleanEvent = false, abortEvent = false;
        if (event.dwProcessId != out.pid) { fail(L"Unexpected process in startup debugger."); break; }
        switch (event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
        case CREATE_THREAD_DEBUG_EVENT: {
            Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                FALSE, event.dwThreadId));
            if (!thread) fail(L"Could not track a Dia startup thread.");
            else {
                auto& t = threads[event.dwThreadId]; t.handle = std::move(thread);
                if (base && !cleaning && !Arm(t, base + layout.bootstrapBranch, base + layout.windowBranch)) fail(L"Could not guard a Dia startup thread.");
            }
            if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
                // Event hProcess/hThread belong to Windows. Own only OpenThread duplicates.
                Handle imageFile(event.u.CreateProcessInfo.hFile);
                if (options.fixture) {
                    base = (uintptr_t)event.u.CreateProcessInfo.lpBaseOfImage;
                    for (auto& [id, t] : threads)
                        if (!Arm(t, base + layout.bootstrapBranch, base + layout.windowBranch)) fail(L"Could not guard fixture thread.");
                }
            }
            break;
        }
        case LOAD_DLL_DEBUG_EVENT: {
            Handle file(event.u.LoadDll.hFile);
            uintptr_t address = (uintptr_t)event.u.LoadDll.lpBaseOfDll;
            IMAGE_NT_HEADERS64 nt{};
            bool pe = Image(process.value, address, nt);
            if (pe && nt.OptionalHeader.SizeOfImage == ntdllSize && nt.FileHeader.TimeDateStamp == ntdllStamp) ntdllBase = address;
            wchar_t path[32768]{};
            DWORD pathLength = file ? GetFinalPathNameByHandleW(file.value, path, (DWORD)std::size(path), 0) : 0;
            const wchar_t* leaf = pathLength && pathLength < std::size(path) ? wcsrchr(path, L'\\') : nullptr;
            bool namedDia = leaf && _wcsicmp(leaf + 1, L"Dia.dll") == 0;
            bool matchingPE = pe && nt.FileHeader.TimeDateStamp == layout.timeStamp && nt.OptionalHeader.SizeOfImage == layout.imageSize;
            if (!options.fixture && (namedDia || matchingPE)) {
                Fingerprint loaded{};
                bool matches = !base && matchingPE && HashFile(file.value, loaded) && loaded == options.fingerprint;
                std::vector<BYTE> bootstrap(layout.bootstrapOriginal.size());
                matches = matches && ReadBytes(process.value, address + layout.bootstrapBranch, bootstrap) && bootstrap == layout.bootstrapOriginal;
                for (auto& patch : patches) {
                    std::vector<BYTE> actual(patch.original.size());
                    if (!ReadBytes(process.value, address + patch.rva, actual) || actual != patch.original) matches = false;
                }
                if (!matches) fail(L"The loaded Dia image changed or did not match its resolved native capabilities.");
                else {
                    base = address;
                    for (auto& [id, t] : threads)
                        if (!Arm(t, base + layout.bootstrapBranch, base + layout.windowBranch)) fail(L"Could not guard Dia startup.");
                }
            }
            break;
        }
        case EXIT_THREAD_DEBUG_EVENT: threads.erase(event.dwThreadId); break;
        case EXCEPTION_DEBUG_EVENT: {
            auto& exception = event.u.Exception;
            auto& record = exception.ExceptionRecord;
            bool ours = false;
            auto it = threads.find(event.dwThreadId);
            if (base && record.ExceptionCode == EXCEPTION_SINGLE_STEP && exception.dwFirstChance && it != threads.end()) {
                CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_DEBUG_REGISTERS;
                if (!GetThreadContext(it->second.handle.value, &c)) { fail(L"Could not inspect the startup breakpoint."); abortEvent = true; }
                else {
                    bool first = c.Rip == base + layout.bootstrapBranch && (c.Dr6 & 15) == 1;
                    bool second = c.Rip == base + layout.windowBranch && (c.Dr6 & 15) == 2;
                    if (first || second) {
                        ours = true;
                        bool commitBootstrap = false;
                        uintptr_t candidateSelf = 0;
                        if (first && !cleaning && ok && !bootstrapSeen) {
                            if (options.fixture) commitBootstrap = true;
                            else {
                                BYTE condition = 255;
                                NativeState s;
                                if (!Read(process.value, c.R14 + layout.bootstrapFrameSelf, candidateSelf) ||
                                    !Read(process.value, c.R14 + layout.bootstrapFrameCondition, condition) ||
                                    condition > 1 || !State(process.value, base, candidateSelf, layout, s) || !Ready(s, layout))
                                    fail(L"Dia's authentication startup layout did not match the guarded path.");
                                else {
                                    commitBootstrap = true;
                                    if (condition == 0) { c.Rip = base + layout.bootstrapContinue; bootstrapChanged = true; }
                                }
                            }
                        }
                        if (second && !cleaning && ok && bootstrapSeen && !transactionApplied) {
                            NativeState s;
                            bool valid = options.fixture || (Register(c, layout.windowSelfRegister) == self &&
                                State(process.value, base, self, layout, s) && Ready(s, layout));
                            if (!valid) fail(L"Dia's local window state did not match the guarded path.");
                            else {
                                for (auto& patch : patches)
                                    if (!patch.Apply(process.value, base + patch.rva)) { fail(L"Could not install a resolved local browsing capability safely."); break; }
                                if (ok) { transactionApplied = true; if (options.fixture) ready = true; }
                            }
                            if (!ok) {
                                // Never execute a partially written instruction even
                                // briefly while waiting for a later cleanup breakpoint.
                                out.rollback = rollback();
                                if (!out.rollback) abortEvent = true;
                            }
                        }
                        c.EFlags |= 0x10000; // Resume flag avoids retriggering this hardware execution breakpoint.
                        c.Dr6 = 0;
                        if (!SetThreadContext(it->second.handle.value, &c)) { fail(L"Could not resume Dia's guarded startup."); abortEvent = true; }
                        else if (commitBootstrap) { bootstrapSeen = true; self = candidateSelf; }
                    }
                }
            }
            auto address = (uintptr_t)record.ExceptionAddress;
            bool systemBreak = record.ExceptionCode == EXCEPTION_BREAKPOINT && exception.dwFirstChance &&
                address >= ntdllBase && address < ntdllBase + ntdllSize;
            if (!ours && systemBreak && !initialBreak) initialBreak = true;
            else if (!ours && systemBreak && cleaning) cleanEvent = true;
            else if (!ours) {
                disposition = DBG_EXCEPTION_NOT_HANDLED;
                if (!exception.dwFirstChance) fail(L"Dia encountered an unhandled exception during local startup.");
            }
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT: exited = true; fail(L"Dia exited before local browsing startup completed."); break;
        default: break;
        }
        if (abortEvent) break;
        if (cleanEvent) {
            // A debug event freezes every target thread: code rollback and DR restoration
            // are done before allowing any thread to execute or detaching.
            if (!ok || !ready) {
                out.rollback = rollback();
                if (!out.rollback) { fail(L"Could not roll back the process-local changes."); break; }
            }
            bool restored = true;
            for (auto& [id, t] : threads) if (!Restore(t)) restored = false;
            if (!restored) { fail(L"Could not restore startup thread state."); break; }
        }
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition)) { fail(L"Could not release the native debug event."); break; }
        pending = false;
        if (cleanEvent) {
            if (Detach(out.pid)) { out.detached = true; break; }
            fail(L"Could not detach the startup debugger.");
            break;
        }
    }
    // A cleanup API failure is not a success. Never leave our newly created
    // process running with orphan hardware breakpoints, dirty page protections,
    // or a debugger association whose owner is about to return.
    if (!exited && !out.detached) {
        if (pending) {
            out.rollback = rollback();
            for (auto& [id, t] : threads) Restore(t);
        }
        out.terminated = TerminateProcess(process.value, ERROR_CANCELLED) != FALSE;
        if (!out.terminated) out.error += L" Windows refused emergency process cleanup.";
        if (pending) { ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition); pending = false; }
        ULONGLONG end = GetTickCount64() + 2500;
        while (GetTickCount64() < end && !exited) {
            DEBUG_EVENT tail{};
            if (!WaitForDebugEventEx(&tail, 50)) continue;
            if (tail.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && tail.u.LoadDll.hFile) CloseHandle(tail.u.LoadDll.hFile);
            if (tail.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT && tail.u.CreateProcessInfo.hFile) CloseHandle(tail.u.CreateProcessInfo.hFile);
            if (tail.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) exited = true;
            ContinueDebugEvent(tail.dwProcessId, tail.dwThreadId, DBG_CONTINUE);
        }
        // A terminated debuggee can have no surviving patched instructions/DRs.
        if (!exited) DebugActiveProcessStop(out.pid);
    }
    out.success = ok && ready && out.detached;
    emergency.safe = exited || out.detached;
    (void)bootstrapChanged;
    return out;
}
std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (count <= 0) return L"The native capability report could not be decoded.";
    std::wstring result(count, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), result.data(), count);
    return result;
}
std::wstring FullPath(const std::wstring& input) {
    DWORD count = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (!count) return {};
    std::vector<wchar_t> out(count);
    DWORD written = GetFullPathNameW(input.c_str(), count, out.data(), nullptr);
    return written && written < count ? std::wstring(out.data(), written) : std::wstring();
}
bool InstalledDia(const std::wstring& exe) {
    // Bind runtime modifications to this application's registered package. The
    // package version is deliberately not part of admission.
    UINT32 count = 0, length = 0;
    constexpr wchar_t family[] = L"TheBrowserCompany.Dia_ttt1ap7aakyb4";
    if (FindPackagesByPackageFamily(family, PACKAGE_FILTER_HEAD, &count, nullptr, &length, nullptr, nullptr) != ERROR_INSUFFICIENT_BUFFER || !count)
        return false;
    std::vector<wchar_t*> names(count);
    std::vector<wchar_t> buffer(length);
    if (FindPackagesByPackageFamily(family, PACKAGE_FILTER_HEAD, &count, names.data(), &length, buffer.data(), nullptr) != ERROR_SUCCESS)
        return false;
    auto requested = FullPath(exe);
    for (UINT32 i = 0; i < count; ++i) {
        UINT32 size = 0;
        if (GetPackagePathByFullName(names[i], &size, nullptr) != ERROR_INSUFFICIENT_BUFFER) continue;
        std::vector<wchar_t> path(size);
        if (GetPackagePathByFullName(names[i], &size, path.data()) != ERROR_SUCCESS) continue;
        auto installed = FullPath(std::wstring(path.data()) + L"\\Dia.exe");
        if (!installed.empty() && !requested.empty() && _wcsicmp(installed.c_str(), requested.c_str()) == 0) return true;
    }
    return false;
}
struct Source {
    Handle file;
    Fingerprint fingerprint{};
    dia_native::Resolution capabilities;
};
bool OpenSource(const std::wstring& diaExe, Source& source, std::wstring& message) {
    auto separator = diaExe.find_last_of(L"\\/");
    if (separator == std::wstring::npos || _wcsicmp(diaExe.c_str() + separator + 1, L"Dia.exe") != 0 || !InstalledDia(diaExe)) {
        message = L"Local browsing requires the registered Dia package's executable.";
        return false;
    }
    auto dll = diaExe.substr(0, separator) + L"\\Dia.dll";
    source.file = Handle(CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!source.file || !HashFile(source.file.value, source.fingerprint)) {
        message = L"Dia's native image could not be read for compatibility and freshness checks.";
        return false;
    }
    source.capabilities = dia_native::ResolveFile(dll);
    if (!source.capabilities.supported) {
        message = L"Local browsing capability unavailable: " + Wide(source.capabilities.message);
        return false;
    }
    message = L"The installed Dia exposes the required native browsing capabilities.";
    return true;
}
} // namespace

bool CheckSupported(const std::wstring& diaExe, std::wstring& message) {
    try { Source source; return OpenSource(diaExe, source, message); }
    catch (...) { message = L"Dia's native capabilities could not be inspected."; return false; }
}

LaunchResult Launch(const std::wstring& diaExe) {
    std::lock_guard<std::mutex> lock(launchMutex);
    try {
        Source source;
        std::wstring message;
        if (!OpenSource(diaExe, source, message)) return {LaunchStatus::Unsupported, 0, message};
        std::wstring directory = diaExe.substr(0, diaExe.find_last_of(L"\\/"));
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot) return {LaunchStatus::Failed, 0, L"Could not check whether Dia is already running."};
        PROCESSENTRY32W entry{sizeof(entry)};
        if (!Process32FirstW(snapshot.value, &entry))
            return {LaunchStatus::Failed, 0, L"Could not enumerate running processes."};
        do {
            if (_wcsicmp(entry.szExeFile, L"Dia.exe") == 0)
                return {LaunchStatus::Failed, 0, L"Close Dia completely before starting local browsing."};
        } while (Process32NextW(snapshot.value, &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            return {LaunchStatus::Failed, 0, L"Could not finish checking running processes."};
        Options options;
        options.layout = std::move(source.capabilities.layout);
        options.fingerprint = source.fingerprint;
        // The source handle remains open through launch, preventing replacement.
        // The loaded-file hash must equal this measured snapshot, not a release list.
        Outcome result = Run(diaExe, L"\"" + diaExe + L"\"", directory, options);
        return {result.success ? LaunchStatus::Started : LaunchStatus::Failed, result.pid,
            result.success ? L"Local browsing started. Dia account and AI services retain their actual sign-in state."
                           : result.error + (result.terminated ? L" The newly launched Dia process was closed for cleanup." : L"")};
    } catch (...) {
        return {LaunchStatus::Failed, 0, L"Local browsing startup failed unexpectedly; emergency cleanup was requested for its new process."};
    }
}
} // namespace local_browsing
