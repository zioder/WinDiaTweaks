#pragma once
#include <windows.h>
#include <string>

namespace local_browsing {
enum class LaunchStatus { Started, Unsupported, Failed };
struct LaunchResult {
    LaunchStatus status;
    DWORD pid;
    std::wstring message;
};

// Starts a fresh process without onboarding flags. Required native capabilities
// are resolved before launch and verified against the actual loaded image.
// Returns only after detaching; no background debugger or profile changes.
LaunchResult Launch(const std::wstring& diaExe);
// Read-only preflight for callers that would otherwise close an existing window.
bool CheckSupported(const std::wstring& diaExe, std::wstring& message);
}
