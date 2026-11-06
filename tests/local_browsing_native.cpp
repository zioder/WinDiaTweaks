// Standalone native fixture: no Dia process, package, account, or user profile.
// Build this single TU; it includes the production engine with one-shot API faults.
#define LOCAL_BROWSING_TEST
#include "../src/local_browsing.cpp"
#include <atomic>
#include <cstdio>
#include <thread>

namespace {
volatile LONG marker = 0;
__declspec(noinline) void StartupMarker() { InterlockedIncrement(&marker); }

constexpr std::array<BYTE, 1024> FixtureInstructions() {
    std::array<BYTE, 1024> code{};
    for (auto& b : code) b = 0x90;
    code[0] = 0x31; code[1] = 0xC0; // xor eax,eax: ZF=1
    for (size_t i = 0; i < 6; ++i) code[2 + i] = local_browsing::kWindowOriginal[i];
    code[8] = 0xB8; code[9] = 1; code[10] = code[11] = code[12] = 0;
    code[13] = 0xC3; // Patched fallthrough returns 1.
    code[32] = 0x75; code[33] = 0x4D; // Separate automatic-revocation branch fixture.
    for (size_t i = 0; i < 32; ++i) code[64 + i] = local_browsing::kCommandsOriginal[i];
    for (size_t i = 0; i < 6; ++i) code[128 + i] = local_browsing::kMissingOriginal[i];
    for (size_t i = 0; i < 2; ++i) code[144 + i] = local_browsing::kEmptyOriginal[i];
    for (size_t i = 0; i < 6; ++i) code[160 + i] = local_browsing::kQueuedOriginal[i];
    code[1007] = 0x31; code[1008] = 0xC0; code[1009] = 0xC3; // Original JE returns 0.
    return code;
}
#pragma section(".lbtest", read, execute)
__declspec(allocate(".lbtest")) const auto fixture = FixtureInstructions();

bool ConsoleCleanupTests() {
    using namespace local_browsing;
    // Exercise an in-flight callback waiting on an unsignaled cleanup event.
    // Destruction must wake it before draining its shared lifetime lock.
    Handle waitingEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!waitingEvent) return false;
    SetCleanupEvent(waitingEvent.value);
    std::thread waiting([] { ConsoleStop(CTRL_C_EVENT); });
    bool protectedWait = false;
    const auto deadline = GetTickCount64() + 2000;
    while (GetTickCount64() < deadline) {
        if (!TryAcquireSRWLockExclusive(&cleanupLock)) { protectedWait = true; break; }
        ReleaseSRWLockExclusive(&cleanupLock);
        SwitchToThread();
    }
    {
        ConsoleRegistration console{waitingEvent.value};
    }
    waiting.join();

    // Race repeated publication/closure against callbacks already in progress.
    // Use the production teardown, rather than just clearing the shared handle.
    std::atomic<bool> finished{false}, invalid{false};
    std::atomic<unsigned> calls{0};
    std::vector<std::thread> callbacks;
    for (unsigned n = 0; n < 4; ++n) callbacks.emplace_back([&] {
        while (!finished.load()) {
            SetLastError(ERROR_SUCCESS);
            ConsoleStop(CTRL_C_EVENT);
            if (GetLastError() == ERROR_INVALID_HANDLE) invalid = true;
            ++calls;
        }
    });
    bool created = true;
    for (unsigned n = 0; n < 4096; ++n) {
        Handle done(CreateEventW(nullptr, TRUE, TRUE, nullptr));
        if (!done) { created = false; break; }
        SetCleanupEvent(done.value);
        ConsoleRegistration console{done.value};
        SwitchToThread();
    }
    finished = true;
    for (auto& callback : callbacks) callback.join();
    bool ok = protectedWait && created && calls.load() != 0 && !invalid.load() && cleanupDone == nullptr;
    printf("console cleanup wait/close lifetime: %s (%u callbacks)\n", ok ? "PASS" : "FAIL", calls.load());
    return ok;
}

bool PatchTests() {
    using namespace local_browsing;
    BYTE* memory = (BYTE*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory) return false;
    memcpy(memory, kWindowOriginal.data(), 6);
    DWORD old = 0;
    bool ok = VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &old) != FALSE;
    for (Fault fault : {Fault::None, Fault::Write, Fault::Flush, Fault::Protection}) {
        Patch patch(kWindowOriginal, kNops);
        testFault = fault;
        bool applied = patch.Apply(GetCurrentProcess(), (uintptr_t)memory);
        ok = ok && (applied == (fault == Fault::None));
        ok = patch.Rollback(GetCurrentProcess()) && ok;
        MEMORY_BASIC_INFORMATION information{};
        ok = VirtualQuery(memory, &information, sizeof(information)) && information.Protect == PAGE_EXECUTE_READ &&
            memcmp(memory, kWindowOriginal.data(), 6) == 0 && ok;
    }
    // Wrong expected bytes must not change either bytes or page protections.
    VirtualProtect(memory, 4096, PAGE_READWRITE, &old);
    memory[0] = 0xCC;
    VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &old);
    Patch mismatch(kWindowOriginal, kNops);
    ok = !mismatch.Apply(GetCurrentProcess(), (uintptr_t)memory) && !mismatch.changed &&
        !mismatch.protectionChanged && memory[0] == 0xCC && ok;
    VirtualProtect(memory, 4096, PAGE_READWRITE, &old);
    memcpy(memory, kRevocationOriginal.data(), 2);
    VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &old);
    for (Fault fault : {Fault::None, Fault::Write, Fault::Flush, Fault::Protection}) {
        Patch shortPatch(kRevocationOriginal, kRevocationChoice);
        testFault = fault;
        bool applied = shortPatch.Apply(GetCurrentProcess(), (uintptr_t)memory);
        ok = (applied == (fault == Fault::None)) && ok;
        ok = shortPatch.Rollback(GetCurrentProcess()) && ok;
        MEMORY_BASIC_INFORMATION information{};
        ok = VirtualQuery(memory, &information, sizeof(information)) && information.Protect == PAGE_EXECUTE_READ &&
            memcmp(memory, kRevocationOriginal.data(), 2) == 0 && ok;
    }
    // Two-byte expected-byte rejection also remains read-only.
    Patch shortMismatch(std::array<BYTE,2>{0,0}, kRevocationChoice);
    ok = !shortMismatch.Apply(GetCurrentProcess(), (uintptr_t)memory) && !shortMismatch.changed && ok;
    VirtualProtect(memory, 4096, PAGE_READWRITE, &old);
    memcpy(memory, kCommandsOriginal.data(), 32);
    VirtualProtect(memory, 4096, PAGE_READONLY, &old);
    for (Fault fault : {Fault::None, Fault::Write, Fault::Flush, Fault::Protection}) {
        Patch dataPatch(kCommandsOriginal, kCommandsLocal, PAGE_READWRITE);
        testFault = fault;
        bool applied = dataPatch.Apply(GetCurrentProcess(), (uintptr_t)memory);
        ok = (applied == (fault == Fault::None)) && ok;
        ok = dataPatch.Rollback(GetCurrentProcess()) && ok;
        MEMORY_BASIC_INFORMATION information{};
        ok = VirtualQuery(memory, &information, sizeof(information)) && information.Protect == PAGE_READONLY &&
            memcmp(memory, kCommandsOriginal.data(), 32) == 0 && ok;
    }
    VirtualFree(memory, 0, MEM_RELEASE);
    return ok;
}

bool EngineTest(local_browsing::Fault fault, bool interrupt, unsigned mismatch = 0, unsigned skipWrites = 0) {
    using namespace local_browsing;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    std::wstring exe(path), dir = exe.substr(0, exe.find_last_of(L'\\'));
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    Options options;
    options.fixture = true;
    options.first = (DWORD)((uintptr_t)&StartupMarker - base);
    options.second = (DWORD)((uintptr_t)fixture.data() + 2 - base);
    options.revocation = (DWORD)((uintptr_t)fixture.data() + 32 - base);
    if (mismatch == 1) options.revocation += 2;
    options.commands = (DWORD)((uintptr_t)fixture.data() + 64 - base);
    if (mismatch == 2) ++options.commands;
    options.missing = (DWORD)((uintptr_t)fixture.data() + 128 - base);
    options.empty = (DWORD)((uintptr_t)fixture.data() + 144 - base);
    options.queued = (DWORD)((uintptr_t)fixture.data() + 160 - base);
    if (mismatch == 3) ++options.missing;
    if (mismatch == 4) ++options.empty;
    if (mismatch == 5) ++options.queued;
    options.timeout = 3000;
    testFault = fault;
    faultSkip = skipWrites;
    Handle interrupter;
    if (interrupt) {
        interrupter = Handle(CreateThread(nullptr, 0, [](void*) -> DWORD {
            Sleep(200); ConsoleStop(CTRL_C_EVENT); return 0;
        }, nullptr, 0, nullptr));
    }
    auto command = L"\"" + exe + (interrupt ? L"\" --interrupt-child" : fault == Fault::None && !mismatch ? L"\" --child-one" : L"\" --child-zero");
    Outcome outcome = Run(exe, command, dir, options);
    bool ok;
    if (fault == Fault::None && !interrupt && !mismatch) ok = outcome.success && outcome.detached && !outcome.terminated;
    else if (fault == Fault::Detach || fault == Fault::Restore) ok = !outcome.success && outcome.terminated;
    else ok = !outcome.success && outcome.detached && outcome.rollback;
    if (outcome.detached) {
        Handle child(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, outcome.pid));
        DWORD code = 999;
        ok = child && WaitForSingleObject(child.value, 4000) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(child.value, &code) && code == 0 && ok;
    }
    if (outcome.terminated) {
        Handle child(OpenProcess(SYNCHRONIZE, FALSE, outcome.pid));
        ok = (!child || WaitForSingleObject(child.value, 1000) == WAIT_OBJECT_0) && ok;
    }
    if (interrupter) ok = WaitForSingleObject(interrupter.value, 4000) == WAIT_OBJECT_0 && ok;
    wprintf(L"engine fault=%u interrupt=%u mismatch=%u skipWrites=%u: %ls (%ls)\n", (unsigned)fault, interrupt, mismatch, skipWrites, ok ? L"PASS" : L"FAIL", outcome.error.c_str());
    return ok;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && wcscmp(argv[1], L"--check-installed") == 0) {
        std::wstring message;
        bool supported = local_browsing::CheckSupported(argv[2], message);
        wprintf(L"Installed native capabilities: %ls (%ls)\n", supported ? L"PASS" : L"FAIL", message.c_str());
        return supported ? 0 : 1;
    }
    if (argc == 2) {
        wchar_t heapEnvironment[8]{}, inheritedMarker[32]{};
        if (GetEnvironmentVariableW(L"_NO_DEBUG_HEAP", heapEnvironment, 8) != 1 || wcscmp(heapEnvironment, L"1") != 0 ||
            !GetEnvironmentVariableW(L"WDT_ENVIRONMENT_FIXTURE", inheritedMarker, 32) || wcscmp(inheritedMarker, L"kept:\u03A9=\u6F22") != 0)
            return 11;
        bool interrupt = wcscmp(argv[1], L"--interrupt-child") == 0;
        StartupMarker();
        if (interrupt) { Sleep(1000); return 0; }
        auto function = (int(*)())fixture.data();
        function();
        Sleep(700); // Execute again after the debugger has detached/rolled back.
        int expected = wcscmp(argv[1], L"--child-one") == 0 ? 1 : 0;
        volatile const BYTE* branch = fixture.data() + 32;
        bool secondPatch = expected ? branch[0] == 0x90 && branch[1] == 0x90 : branch[0] == 0x75 && branch[1] == 0x4D;
        volatile const BYTE* data = fixture.data() + 64;
        bool dataPatch = true;
        auto desired = expected ? local_browsing::kCommandsLocal : local_browsing::kCommandsOriginal;
        for (size_t i = 0; i < 32; ++i) dataPatch = dataPatch && data[i] == desired[i];
        auto missing = expected ? local_browsing::kMissingReady : local_browsing::kMissingOriginal;
        auto empty = expected ? local_browsing::kEmptyReady : local_browsing::kEmptyOriginal;
        auto queued = expected ? local_browsing::kNops : local_browsing::kQueuedOriginal;
        data = fixture.data();
        for (size_t i = 0; i < 6; ++i) dataPatch = dataPatch && data[128 + i] == missing[i] && data[160 + i] == queued[i];
        for (size_t i = 0; i < 2; ++i) dataPatch = dataPatch && data[144 + i] == empty[i];
        return function() == expected && secondPatch && dataPatch ? 0 : 10;
    }
    // The production launcher must override the child's debug heap choice
    // without changing its parent or losing other Unicode environment values.
    SetEnvironmentVariableW(L"_NO_DEBUG_HEAP", L"0");
    SetEnvironmentVariableW(L"WDT_ENVIRONMENT_FIXTURE", L"kept:\u03A9=\u6F22");
    bool ok = ConsoleCleanupTests();
    bool patches = PatchTests();
    printf("patch identity/write/flush/protection rollback: %s\n", patches ? "PASS" : "FAIL");
    ok = patches && ok;
    using local_browsing::Fault;
    for (Fault fault : {Fault::None, Fault::Write, Fault::Flush, Fault::Protection, Fault::Detach, Fault::Restore})
        ok = EngineTest(fault, false) && ok;
    ok = EngineTest(Fault::None, true) && ok;
    for (unsigned mismatch = 1; mismatch <= 5; ++mismatch) ok = EngineTest(Fault::None, false, mismatch) && ok;
    ok = EngineTest(Fault::Write, false, 0, 5) && ok;
    wchar_t parentHeapEnvironment[8]{};
    bool parentUnchanged = GetEnvironmentVariableW(L"_NO_DEBUG_HEAP", parentHeapEnvironment, 8) == 1 && wcscmp(parentHeapEnvironment, L"0") == 0;
    printf("child retail heap environment / unchanged parent: %s\n", parentUnchanged ? "PASS" : "FAIL");
    ok = parentUnchanged && ok;
    std::wstring message;
    ok = !local_browsing::CheckSupported(L"C:\\nonexistent\\Dia.exe", message) && !message.empty() && ok;
    printf("local browsing native fixture: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
