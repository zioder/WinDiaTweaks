#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dia_native {
struct PatchSite {
    std::string capability;
    uint32_t rva = 0;
    std::vector<uint8_t> original;
    std::vector<uint8_t> replacement;
    bool executable = true;
};
struct Field {
    std::string name;
    uint32_t index = 0;
    // Zero means reflection-only. Other values must equal the runtime Swift
    // metadata offset before reading an instance or changing its process.
    uint32_t codeOffset = 0;
};
struct NativeLayout {
    uint32_t imageSize = 0;
    uint32_t timeStamp = 0; // Diagnostic/freshness information, never admission.
    uint64_t preferredBase = 0;
    uint32_t applicationDescriptor = 0;
    uint32_t metadataCache = 0;
    uint32_t fieldOffsetVectorWords = 0;
    std::vector<Field> applicationFields;
    uint32_t bootstrapBranch = 0;
    std::vector<uint8_t> bootstrapOriginal;
    uint32_t bootstrapContinue = 0;
    uint32_t bootstrapFrameSelf = 0;
    uint32_t bootstrapFrameCondition = 0;
    uint32_t windowBranch = 0;
    uint8_t terminationNilTag = 0;
    uint8_t launchCompleteSuccessTag = 0;
    uint32_t launchTagOffset = 0; // Absolute ApplicationController displacement.
    uint8_t restoredAuthenticationOrdinal = 0;
    // x64 register number, checked against the native self-preservation move.
    uint8_t windowSelfRegister = 0;
    std::vector<PatchSite> patches;
};
struct Resolution {
    bool supported = false;
    NativeLayout layout;
    // Names the missing/ambiguous capability. No version or hash allowlist.
    std::string message;
};

// Pure byte-image analysis, with no code execution or process/profile access.
Resolution Resolve(const std::vector<uint8_t>& image);
Resolution ResolveFile(const std::wstring& path);
}
