#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "dia_native_layout.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>

namespace dia_native {
namespace {
struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };
void Require(bool condition, const char* reason) { if (!condition) throw Unsupported(reason); }
struct Section { uint32_t start, size, raw, rawSize, flags; };
struct Function { uint32_t start, end; };

// Only reads file-backed RVAs. Virtual/BSS addresses may be checked for mapping
// but are never silently translated into unrelated file bytes.
class PE {
public:
    const std::vector<uint8_t>& data;
    uint32_t imageSize = 0, stamp = 0;
    uint64_t base = 0;
    uint32_t imports = 0, importsSize = 0;
    std::vector<Section> sections;
    std::vector<Function> functions;
    explicit PE(const std::vector<uint8_t>& bytes) : data(bytes) {
        Require(data.size() >= 64 && File<uint16_t>(0) == IMAGE_DOS_SIGNATURE, "Not a Windows PE image.");
        uint32_t nt = File<uint32_t>(60);
        Require(File<uint32_t>(nt) == IMAGE_NT_SIGNATURE, "Invalid Windows PE header.");
        Require(File<uint16_t>(uint64_t(nt) + 4) == IMAGE_FILE_MACHINE_AMD64, "Native local browsing requires an x64 Dia image.");
        uint16_t count = File<uint16_t>(uint64_t(nt) + 6), optionalSize = File<uint16_t>(uint64_t(nt) + 20);
        Require(count && count <= 96 && optionalSize >= 144, "Invalid PE section or optional-header layout.");
        uint64_t optional = uint64_t(nt) + 24;
        Require(File<uint16_t>(optional) == IMAGE_NT_OPTIONAL_HDR64_MAGIC, "Unsupported PE optional-header architecture.");
        base = File<uint64_t>(optional + 24); imageSize = File<uint32_t>(optional + 56); stamp = File<uint32_t>(uint64_t(nt) + 8);
        imports = File<uint32_t>(optional + 120); importsSize = File<uint32_t>(optional + 124);
        Require(imageSize && File<uint32_t>(optional + 108) >= 4, "PE exception metadata is unavailable.");
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t at = optional + optionalSize + uint64_t(i) * 40;
            Section s{File<uint32_t>(at + 12), File<uint32_t>(at + 8), File<uint32_t>(at + 20), File<uint32_t>(at + 16), File<uint32_t>(at + 36)};
            Require(uint64_t(s.start) + std::max(s.size, s.rawSize) <= imageSize && uint64_t(s.raw) + s.rawSize <= data.size(), "PE section exceeds its image bounds.");
            for (const auto& previous : sections)
                Require(uint64_t(s.start) + std::max(s.size, s.rawSize) <= previous.start ||
                    uint64_t(previous.start) + std::max(previous.size, previous.rawSize) <= s.start, "Overlapping PE sections are ambiguous.");
            sections.push_back(s);
        }
        uint32_t table = File<uint32_t>(optional + 112 + 3 * 8), size = File<uint32_t>(optional + 116 + 3 * 8);
        Require(size && size % 12 == 0 && size / 12 <= 2000000, "Invalid native function table.");
        Require(uint64_t(table) + size <= imageSize, "Native function table exceeds the image.");
        for (uint64_t at = table; at < uint64_t(table) + size; at += 12) {
            Function f{At<uint32_t>((uint32_t)at), At<uint32_t>((uint32_t)at + 4)};
            Require(f.start < f.end && Executable(f.start, f.end - f.start), "Invalid native function boundary.");
            functions.push_back(f);
        }
        std::sort(functions.begin(), functions.end(), [](auto a, auto b) { return a.start < b.start; });
        for (size_t i = 1; i < functions.size(); ++i)
            Require(functions[i - 1].end <= functions[i].start, "Overlapping native function boundaries are ambiguous.");
    }
    template<class T> T File(uint64_t offset) const {
        Require(offset <= data.size() && sizeof(T) <= data.size() - offset, "Truncated PE data.");
        T value; memcpy(&value, data.data() + offset, sizeof(value)); return value;
    }
    size_t Offset(uint32_t rva, size_t count = 1) const {
        for (auto s : sections) if (rva >= s.start && uint64_t(rva) + count <= uint64_t(s.start) + s.rawSize)
            return size_t(s.raw) + rva - s.start;
        throw Unsupported("Required native data is not file-backed.");
    }
    template<class T> T At(uint32_t rva) const { return File<T>(Offset(rva, sizeof(T))); }
    bool Executable(uint32_t rva, size_t size = 1) const {
        for (auto s : sections) if (rva >= s.start && uint64_t(rva) + size <= uint64_t(s.start) + s.rawSize)
            return (s.flags & IMAGE_SCN_MEM_EXECUTE) != 0;
        return false;
    }
    bool Writable(uint32_t rva, size_t size = 1) const {
        for (auto s : sections) if (rva >= s.start && uint64_t(rva) + size <= uint64_t(s.start) + std::max(s.size, s.rawSize))
            return (s.flags & IMAGE_SCN_MEM_WRITE) != 0;
        return false;
    }
    uint32_t Relative(uint32_t field) const {
        int64_t target = int64_t(field) + At<int32_t>(field);
        Require(target > 0 && target < imageSize, "Invalid relative native metadata reference.");
        return (uint32_t)target;
    }
    std::vector<uint8_t> Bytes(uint32_t rva, size_t size) const {
        size_t offset = Offset(rva, size); return {data.begin() + offset, data.begin() + offset + size};
    }
    std::string String(uint32_t rva) const {
        std::string result;
        for (unsigned i = 0; i < 1024; ++i) {
            char c = At<char>(rva + i);
            if (!c) return result;
            Require(c >= 32 && c <= 126, "Malformed native reflection name.");
            result += c;
        }
        throw Unsupported("Unterminated native reflection name.");
    }
    std::vector<uint32_t> Strings(const std::string& needle) const {
        std::vector<uint32_t> found;
        for (auto s : sections) {
            auto begin = data.begin() + s.raw, end = begin + s.rawSize;
            while (begin != end) {
                auto hit = std::search(begin, end, needle.begin(), needle.end());
                if (hit == end) break;
                if (size_t(end - hit) > needle.size() && hit[needle.size()] == 0)
                    found.push_back(s.start + (uint32_t)(hit - (data.begin() + s.raw)));
                begin = hit + 1;
            }
        }
        return found;
    }
    const Function* Containing(uint32_t rva) const {
        auto it = std::upper_bound(functions.begin(), functions.end(), rva, [](uint32_t r, Function f) { return r < f.start; });
        if (it == functions.begin()) return nullptr;
        --it; return rva < it->end ? &*it : nullptr;
    }
    std::string Import(uint32_t slot) const {
        Require(uint64_t(imports) + importsSize <= imageSize, "Invalid PE import bounds.");
        for (uint32_t n = 0; n + 20 <= importsSize; n += 20) {
            uint32_t at = imports + n, names = At<uint32_t>(at), first = At<uint32_t>(at + 16);
            if (!first && !names) break;
            if (!names || slot < first || ((slot - first) & 7)) continue;
            uint32_t index = (slot - first) / 8; if (index > 65536) continue;
            for (uint32_t i = 0; i <= index; ++i) {
                uint64_t name = At<uint64_t>(names + i * 8); if (!name) break;
                if (i == index && !(name >> 63) && name + 2 < imageSize) return String(uint32_t(name) + 2);
            }
        }
        return {};
    }
};

struct Instruction {
    uint32_t rva = 0, length = 0, target = 0, ripTarget = 0;
    uint8_t opcode = 0, second = 0, modrm = 0, rex = 0;
    bool conditional = false, jump = false, call = false, stop = false;
    uint32_t immediateAt = 0, displacementAt = 0;
    int32_t displacement = 0;
    int memoryBase = -1;
};

// A deliberately bounded x64 decoder for compiler-generated control flow. An
// unfamiliar opcode rejects a capability; it never guesses instruction lengths.
Instruction Decode(const PE& pe, uint32_t at) {
    Instruction in; in.rva = at;
    uint32_t p = at; bool operand16 = false, address32 = false;
    for (unsigned prefixes = 0; prefixes < 14; ++prefixes) {
        auto b = pe.At<uint8_t>(p);
        if (b == 0x66) operand16 = true;
        else if (b == 0x67) address32 = true;
        else if (b >= 0x40 && b <= 0x4F) in.rex = b;
        else if (b != 0xF0 && b != 0xF2 && b != 0xF3 && b != 0x2E && b != 0x36 && b != 0x3E && b != 0x26 && b != 0x64 && b != 0x65) break;
        ++p;
    }
    uint8_t op = pe.At<uint8_t>(p++); in.opcode = op;
    bool modrm = false, relative = false; unsigned immediate = 0;
    auto operand = operand16 ? 2u : 4u;
    if (op == 0x0F) {
        uint8_t second = pe.At<uint8_t>(p++); in.second = second;
        if (second >= 0x80 && second <= 0x8F) { immediate = 4; relative = true; in.conditional = true; }
        else if (second == 0x0B) in.stop = true;
        else if (second == 0x38 || second == 0x3A) { ++p; modrm = true; if (second == 0x3A) immediate = 1; }
        else if ((second >= 0x10 && second <= 0x1F) || (second >= 0x28 && second <= 0x2F) ||
            (second >= 0x40 && second <= 0x76) || second == 0x7E || second == 0x7F ||
            (second >= 0x90 && second <= 0x9F) || second == 0xAF || (second >= 0xB0 && second <= 0xBF) ||
            (second >= 0xC0 && second <= 0xC7) || second >= 0xD0) {
            modrm = true;
            if (second == 0x70 || (second >= 0x71 && second <= 0x73) || second == 0xBA ||
                second == 0xC2 || second == 0xC4 || second == 0xC5 || second == 0xC6) immediate = 1;
        } else throw Unsupported("Unsupported x64 instruction at RVA " + std::to_string(at) + " opcode " + std::to_string(op));
    } else if (op == 0xE8 || op == 0xE9 || op == 0xEB || (op >= 0x70 && op <= 0x7F)) {
        immediate = op == 0xE8 || op == 0xE9 ? 4 : 1; relative = true;
        in.call = op == 0xE8; in.jump = op == 0xE9 || op == 0xEB; in.conditional = !in.call && !in.jump;
    } else if (op >= 0x50 && op <= 0x5F) {}
    else if (op >= 0xB0 && op <= 0xB7) immediate = 1;
    else if (op >= 0xB8 && op <= 0xBF) immediate = in.rex & 8 ? 8 : operand;
    else if (op == 0xC3 || op == 0xCC) in.stop = true;
    else if (op == 0xC2) { immediate = 2; in.stop = true; }
    else if (op == 0x90 || op == 0x98 || op == 0x99 || op == 0x9C || op == 0x9D || op == 0xC9) {}
    else if (op == 0x68 || op == 0xA9) immediate = operand;
    else if (op == 0x6A || op == 0xA8) immediate = 1;
    else if ((op & 7) == 4 && op < 0x40) immediate = 1;
    else if ((op & 7) == 5 && op < 0x40) immediate = operand;
    else if ((op < 0x40 && (op & 7) <= 3) || op == 0x63 || op == 0x69 || op == 0x6B ||
        (op >= 0x80 && op <= 0x8F) || op == 0xC0 || op == 0xC1 || op == 0xC6 || op == 0xC7 ||
        (op >= 0xD0 && op <= 0xD3) || op == 0xF6 || op == 0xF7 || op == 0xFE || op == 0xFF) {
        modrm = true;
        if (op == 0x69 || op == 0x81 || op == 0xC7) immediate = operand;
        if (op == 0x6B || op == 0x80 || op == 0x82 || op == 0x83 || op == 0xC0 || op == 0xC1 || op == 0xC6) immediate = 1;
    } else throw Unsupported("Unsupported x64 instruction at RVA " + std::to_string(at) + " opcode " + std::to_string(op));
    uint32_t ripDisplacement = 0;
    if (modrm) {
        uint8_t m = pe.At<uint8_t>(p++); in.modrm = m;
        unsigned mode = m >> 6, rm = m & 7, group = (m >> 3) & 7;
        if (op == 0xF6 && group <= 1) immediate = 1;
        if (op == 0xF7 && group <= 1) immediate = operand;
        if (op == 0xFF && (group == 4 || group == 5)) { in.jump = true; in.stop = true; }
        if (op == 0xFF && (group == 2 || group == 3)) in.call = true;
        if (mode != 3) {
            in.memoryBase = int(rm + ((in.rex & 1) ? 8 : 0));
            if (rm == 4) {
                uint8_t sib = pe.At<uint8_t>(p++);
                in.memoryBase = int((sib & 7) + ((in.rex & 1) ? 8 : 0));
                if (mode == 0 && (sib & 7) == 5) { in.memoryBase = -1; p += 4; }
            } else if (mode == 0 && rm == 5) { in.memoryBase = -1; if (!address32) ripDisplacement = p; p += 4; }
            if (mode == 1) { in.displacementAt = p; in.displacement = pe.At<int8_t>(p++); }
            else if (mode == 2) { in.displacementAt = p; in.displacement = pe.At<int32_t>(p); p += 4; }
        }
    }
    uint32_t immediateAt = p; p += immediate;
    in.immediateAt = immediateAt;
    Require(p > at && p - at <= 15 && pe.Executable(at, p - at), "Invalid x64 instruction boundary.");
    in.length = p - at;
    if (ripDisplacement) in.ripTarget = uint32_t(int64_t(p) + pe.At<int32_t>(ripDisplacement));
    if (relative) {
        int64_t displacement = immediate == 1 ? pe.At<int8_t>(immediateAt) : pe.At<int32_t>(immediateAt);
        int64_t target = int64_t(p) + displacement;
        Require(target > 0 && target < pe.imageSize && pe.Executable((uint32_t)target), "Native branch target is outside executable code.");
        in.target = (uint32_t)target;
    }
    return in;
}

using Code = std::map<uint32_t, Instruction>;
Code ControlFlow(const PE& pe, Function f) {
    Code code; std::vector<uint32_t> pending{f.start}; std::map<uint32_t,uint32_t> owners;
    while (!pending.empty()) {
        uint32_t at = pending.back(); pending.pop_back();
        while (at >= f.start && at < f.end && !code.count(at)) {
            Require(!owners.count(at), "A native branch targets the middle of an instruction.");
            auto instruction = Decode(pe, at);
            Require(uint64_t(at) + instruction.length <= f.end, "Instruction crosses its native function boundary.");
            for (uint32_t i = 0; i < instruction.length; ++i) {
                Require(!owners.count(at + i), "Overlapping native instruction streams are unsupported.");
                owners[at + i] = at;
            }
            code[at] = instruction;
            if ((instruction.conditional || instruction.jump) && instruction.target >= f.start && instruction.target < f.end)
                pending.push_back(instruction.target);
            if (instruction.stop || instruction.jump) break;
            at += instruction.length;
        }
    }
    return code;
}

std::vector<uint32_t> RipCandidates(const PE& pe, const std::set<uint32_t>& targets) {
    std::vector<uint32_t> refs;
    for (auto s : pe.sections) if (s.flags & IMAGE_SCN_MEM_EXECUTE) {
        for (uint32_t i = 0; i + 7 <= s.rawSize; ++i) {
            auto p = pe.data.data() + s.raw + i;
            if ((p[0] & 0xF0) != 0x40 || (p[1] != 0x8D && p[1] != 0x8B) || (p[2] & 0xC7) != 5) continue;
            int32_t displacement; memcpy(&displacement, p + 3, 4);
            if (targets.count(uint32_t(int64_t(s.start + i + 7) + displacement))) refs.push_back(s.start + i);
        }
    }
    return refs;
}

std::set<uint32_t> AnchorFunctions(const PE& pe, const std::string& text) {
    std::set<uint32_t> targets;
    for (uint32_t rva : pe.Strings(text)) { targets.insert(rva); if (rva >= 32) targets.insert(rva - 32); }
    Require(!targets.empty(), ("Missing native anchor: " + text).c_str());
    std::set<uint32_t> functions;
    for (uint32_t ref : RipCandidates(pe, targets)) {
        const auto* f = pe.Containing(ref);
        if (!f) continue;
        auto code = ControlFlow(pe, *f);
        auto hit = code.find(ref);
        if (hit != code.end() && targets.count(hit->second.ripTarget)) functions.insert(f->start);
    }
    Require(!functions.empty(), ("Native anchor has no validated code reference: " + text).c_str());
    return functions;
}

struct Nominal { uint32_t descriptor, accessor, fieldDescriptor, fieldVector; std::vector<std::string> fields; };
Nominal Reflection(const PE& pe, const std::string& name, uint8_t kind) {
    auto strings = pe.Strings(name); std::set<uint32_t> names(strings.begin(), strings.end());
    std::vector<Nominal> matches;
    for (auto section : pe.sections) if (!(section.flags & IMAGE_SCN_MEM_EXECUTE)) {
        for (uint32_t i = 8; i + 36 <= section.rawSize; i += 4) {
            uint32_t field = section.start + i;
            int32_t relative; memcpy(&relative,pe.data.data()+section.raw+i,4);
            if (!names.count(uint32_t(int64_t(field) + relative))) continue;
            uint32_t descriptor = field - 8;
            if ((pe.At<uint32_t>(descriptor) & 0x1F) != kind) continue;
            try {
                uint32_t fd = pe.Relative(descriptor + 16);
                uint32_t count = pe.At<uint32_t>(fd + 12);
                uint16_t stride = pe.At<uint16_t>(fd + 10);
                Require(count && count < 4096 && stride == 12, "Unsupported Swift field reflection layout.");
                Nominal result{descriptor, pe.Relative(descriptor + 12), fd, kind == 16 ? pe.At<uint32_t>(descriptor + 40) : 0, {}};
                Require(pe.Executable(result.accessor), "Swift metadata accessor is not executable.");
                for (uint32_t j = 0; j < count; ++j) result.fields.push_back(pe.String(pe.Relative(fd + 16 + j * stride + 8)));
                matches.push_back(std::move(result));
            } catch (const Unsupported&) { /* Not a complete nominal descriptor. */ }
        }
    }
    Require(matches.size() == 1, ("Missing or ambiguous Swift type: " + name).c_str());
    return matches.front();
}

void ResolveController(const PE& pe, NativeLayout& layout) {
    auto type = Reflection(pe, "ApplicationController", 16);
    Require(type.fieldVector > 0 && type.fieldVector < 256, "Unsupported ApplicationController field-vector layout.");
    layout.applicationDescriptor = type.descriptor; layout.fieldOffsetVectorWords = type.fieldVector;
    for (const char* name : {"authController", "windowCoordinationController", "launchState", "terminationState",
        "hasInitializedBrowserEngine", "hasFinishedRestoringWindows", "sessionRestorationInProgress"}) {
        auto it = std::find(type.fields.begin(), type.fields.end(), name);
        Require(it != type.fields.end() && std::count(type.fields.begin(), type.fields.end(), name) == 1,
            ("Missing or ambiguous ApplicationController field: " + std::string(name)).c_str());
        layout.applicationFields.push_back({name, (uint32_t)(it - type.fields.begin())});
    }
    // Swift emits this reflection-addressed metadata accessor without .pdata.
    // Its bounded, terminating CFG and descriptor/cache relationship establish
    // the entry; do not borrow a neighboring function's unwind boundary.
    auto function = pe.Containing(type.accessor);
    Require(!function || function->start == type.accessor, "Swift metadata accessor is inside an unrelated function.");
    Function bounded = function ? *function : Function{type.accessor, type.accessor + 96};
    auto code = ControlFlow(pe, bounded);
    bool returns = false;
    for (auto [at, in] : code) {
        if (in.opcode == 0xC3) returns = true;
        Require(!(in.jump && in.target && (in.target < bounded.start || in.target >= bounded.end)),
            "Swift metadata accessor leaves its validated boundary.");
    }
    Require(returns, "Swift metadata accessor does not terminate within its validated boundary.");
    bool descriptorReference = false; std::vector<uint32_t> cache;
    for (auto [at, instruction] : code) {
        if (instruction.opcode == 0x8D && instruction.ripTarget == type.descriptor) descriptorReference = true;
        if (instruction.opcode == 0x8B && instruction.rex == 0x48 && instruction.modrm == 5 &&
            instruction.ripTarget && pe.Writable(instruction.ripTarget, 8)) cache.push_back(instruction.ripTarget);
    }
    Require(descriptorReference && cache.size() == 1, "Swift ApplicationController metadata cache is ambiguous.");
    layout.metadataCache = cache.front();
}

bool Is(const PE& pe, uint32_t at, std::initializer_list<uint8_t> bytes) {
    return pe.Bytes(at, bytes.size()) == std::vector<uint8_t>(bytes);
}
void Bind(NativeLayout& layout, const char* name, uint32_t offset) {
    Require(offset && offset < 0x100000, "Implausible ApplicationController field displacement.");
    auto field = std::find_if(layout.applicationFields.begin(), layout.applicationFields.end(),
        [&](const Field& f) { return f.name == name; });
    Require(field != layout.applicationFields.end() && (!field->codeOffset || field->codeOffset == offset),
        "Native controller instructions disagree about a reflected field.");
    field->codeOffset = offset;
}
uint32_t Bound(const NativeLayout& layout, const char* name) {
    for (const auto& f : layout.applicationFields) if (f.name == name) return f.codeOffset;
    throw Unsupported("Required controller field is absent.");
}
void Patch(const PE& pe, NativeLayout& layout, const char* capability, uint32_t rva,
    std::vector<uint8_t> replacement, bool executable = true) {
    Require(pe.Executable(rva, replacement.size()) == executable, "Native patch has the wrong section permissions.");
    layout.patches.push_back({capability, rva, pe.Bytes(rva, replacement.size()), std::move(replacement), executable});
}
uint32_t Unique(const std::set<uint32_t>& values, const char* why) {
    Require(values.size() == 1, why); return *values.begin();
}

std::set<uint32_t> Reachable(const Code& code,uint32_t entry) {
    std::set<uint32_t> reached; std::vector<uint32_t> pending{entry};
    while(!pending.empty()) {
        uint32_t at=pending.back();pending.pop_back();
        auto it=code.find(at); if(it==code.end() || !reached.insert(at).second) continue;
        const auto& in=it->second;
        if(in.conditional || in.jump) if(in.target) pending.push_back(in.target);
        if(!in.stop && !in.jump) pending.push_back(at+in.length);
    }
    return reached;
}

bool DestroyHelper(const PE& pe,uint32_t entry,const char* typeName) {
    // A Swift value-witness destroy thunk, entered by a decoded direct call.
    // The metadata and witness addresses are derived from its RIP references.
    if(!Is(pe,entry,{0x56,0x48,0x83,0xEC,0x20,0x48,0x89,0xCE})) return false;
    auto witness=Decode(pe,entry+8), metadata=Decode(pe,entry+15);
    if(witness.opcode!=0x8B || witness.rex!=0x48 || witness.modrm!=5 ||
        metadata.opcode!=0x8D || metadata.rex!=0x48 || metadata.modrm!=0x15 ||
        !witness.ripTarget || witness.ripTarget+8!=metadata.ripTarget ||
        !Is(pe,entry+22,{0xFF,0x50,0x08,0x48,0x89,0xF0,0x48,0x83,0xC4,0x20,0x5E,0xC3})) return false;
    uint64_t table=pe.At<uint64_t>(witness.ripTarget), descriptor=pe.At<uint64_t>(metadata.ripTarget+8);
    if(table<pe.base || table-pe.base>=pe.imageSize || descriptor<pe.base || descriptor-pe.base>=pe.imageSize ||
        pe.At<uint64_t>(metadata.ripTarget)!=0x200) return false;
    uint64_t destroy=pe.At<uint64_t>(uint32_t(table-pe.base)+8);
    if(destroy<pe.base || destroy-pe.base>=pe.imageSize || !pe.Executable(uint32_t(destroy-pe.base))) return false;
    uint32_t nominal=uint32_t(descriptor-pe.base);
    return (pe.At<uint32_t>(nominal)&31)==17 && pe.String(pe.Relative(nominal+8))==typeName;
}

// Resolves the two local window constructors through their own diagnostics.
// The identity call, its ARC release, its boolean test and the rejecting log
// target must all be in one decoded native function.
uint32_t ResolveWindows(const PE& pe, NativeLayout& layout) {
    const auto rejected = AnchorFunctions(pe, "Attempted to create a window without authentication");
    const auto rejectionStrings = pe.Strings("Attempted to create a window without authentication");
    const std::array<std::vector<uint32_t>,2> readinessDiagnostics{
        pe.Strings("Ignoring window creation request during app termination"),
        pe.Strings("Attempted to create a window before ArcCore is ready")};
    uint32_t predicate = 0;
    for (auto method : {"createWindowAssumingContextLoaded(_:)", "createWindowLoadingContextIfNeeded(_:)"}) {
        auto named = AnchorFunctions(pe, method); std::set<uint32_t> common;
        std::set_intersection(named.begin(), named.end(), rejected.begin(), rejected.end(), std::inserter(common, common.end()));
        uint32_t start = Unique(common, "Missing or ambiguous local window constructor.");
        auto code = ControlFlow(pe, *pe.Containing(start));
        std::vector<Instruction> compares, guards; uint32_t lastCall = 0, lastCallAt = 0, authCall = 0;
        bool preservedSelf = false;
        for (auto [at, in] : code) {
            if (in.opcode == 0x80 && ((in.modrm >> 3) & 7) == 7 && in.memoryBase == 13) compares.push_back(in);
            if (guards.empty() && in.opcode == 0x8B && in.memoryBase == 13 && in.displacement > 0 && in.displacement < 0x10000 &&
                ((in.modrm >> 3) & 7) == 5 && (in.rex & 4)) Bind(layout, "authController", in.displacement);
            if (Is(pe, at, {0x4C,0x89,0xEF})) preservedSelf = true; // mov rdi,r13
            if (in.call && in.target) { lastCall = in.target; lastCallAt = at; }
            if (Is(pe, at, {0xF6,0xC3,0x01})) {
                auto next = code.find(at + in.length);
                if (next != code.end() && next->second.opcode == 0x0F && next->second.second == 0x84) {
                    // The rejected branch must reach the named authentication diagnostic.
                    bool diagnostic = false;
                    for (auto [logAt, log] : code) if (logAt >= next->second.target && logAt < next->second.target + 256 && log.ripTarget) {
                        for (auto text : rejectionStrings)
                            if (log.ripTarget == text || log.ripTarget + 32 == text) diagnostic = true;
                    }
                    if (diagnostic) {
                        Require(lastCallAt && Is(pe,lastCallAt+5,{0x89,0xC3,0x4C,0x89,0xE9}),
                            "Local identity predicate no longer preserves its result across ARC cleanup.");
                        auto release=Decode(pe,lastCallAt+10);
                        Require(release.call && release.ripTarget && pe.Import(release.ripTarget)=="swift_release" &&
                            lastCallAt+10+release.length==at,"Local identity ARC cleanup changed.");
                        guards.push_back(next->second); authCall = lastCall;
                    }
                }
            }
        }
        Require(guards.size() == 1 && compares.size() >= 2 && authCall, "Local window identity/readiness control flow changed.");
        auto guard = guards.front();
        std::vector<Instruction> pre;
        for (auto in : compares) if (in.rva < guard.rva) pre.push_back(in);
        Require(pre.size() == 2 && pe.At<uint8_t>(pre[1].immediateAt) == 1, "Local window readiness guards are ambiguous.");
        for(size_t i=0;i<pre.size();++i) {
            auto it=code.find(pre[i].rva+pre[i].length);
            Require(it!=code.end() && it->second.opcode==0x0F && it->second.second==0x85 &&
                code.count(it->second.target),"Local window readiness comparison no longer rejects through JNE.");
            auto rejectedPath=Reachable(code,it->second.target); bool diagnostic=false;
            Require(!rejectedPath.count(guard.rva),"Local window readiness rejection can reach window creation.");
            for(auto pc:rejectedPath) for(auto text:readinessDiagnostics[i]) {
                uint32_t ref=code.at(pc).ripTarget;
                if(ref && (ref==text || ref+32==text)) diagnostic=true;
            }
            Require(diagnostic,"Local window readiness rejection no longer reaches its named diagnostic.");
            if(i==0) {
                uint32_t next=it->first+it->second.length;
                if(next!=pre[1].rva) {
                    auto reload=code.find(next); bool sameController=false;
                    if(reload!=code.end() && reload->second.opcode==0x8B && reload->second.memoryBase==14 &&
                        (reload->second.rex&4) && ((reload->second.modrm>>3)&7)==5 && next+reload->second.length==pre[1].rva) {
                        for(auto [pc,load]:code) if(pc<pre[0].rva && load.opcode==0x8B && load.memoryBase==14 &&
                            (load.rex&4) && ((load.modrm>>3)&7)==5 && load.displacement==reload->second.displacement) sameController=true;
                    }
                    Require(sameController,"Local window termination success no longer continues with the same controller into the engine guard.");
                }
            }
        }
        Bind(layout, "terminationState", pre[0].displacement); Bind(layout, "hasInitializedBrowserEngine", pre[1].displacement);
        uint8_t nil = pe.At<uint8_t>(pre[0].immediateAt);
        Require(!layout.terminationNilTag || layout.terminationNilTag == nil, "Window constructors disagree about termination state.");
        layout.terminationNilTag = nil;
        Require(!predicate || predicate == authCall, "Local window constructors use different identity predicates."); predicate = authCall;
        if (method == std::string("createWindowAssumingContextLoaded(_:)")) {
            Require(preservedSelf, "Local window controller is not preserved in the supported register.");
            layout.windowSelfRegister = 7; layout.windowBranch = guard.rva;
            Patch(pe, layout, "local window identity guard", guard.rva, std::vector<uint8_t>(guard.length, 0x90));
            std::set<uint32_t> manager;
            for (auto [at,in] : code) if (at > guard.rva && at < guard.rva + 128 && in.opcode == 0x8B &&
                in.memoryBase == 7 && (in.rex & 4) && ((in.modrm >> 3) & 7) == 5) manager.insert(in.displacement);
            Bind(layout,"windowCoordinationController",Unique(manager,"Local window manager binding is ambiguous."));
        } else Patch(pe, layout, "queued local window identity guard", guard.rva, std::vector<uint8_t>(guard.length, 0x90));
    }
    return predicate;
}

void ResolveBootstrap(const PE& pe, NativeLayout& layout) {
    auto authentication = Reflection(pe, "AuthenticationMethod", 18);
    Require(pe.At<uint32_t>(authentication.descriptor+20) == 0 &&
        pe.At<uint32_t>(authentication.descriptor+24) == authentication.fields.size(),
        "Authentication launch reasons are no longer a simple byte enum.");
    auto restored = std::find(authentication.fields.begin(), authentication.fields.end(), "restored");
    Require(restored != authentication.fields.end() && authentication.fields.size() < 256,
        "The authentication bootstrap no longer exposes a restored launch reason.");
    layout.restoredAuthenticationOrdinal = uint8_t(restored - authentication.fields.begin());
    auto anchors = AnchorFunctions(pe, "initializeAuth()");
    std::set<uint32_t> candidates, matches;
    for (auto section : pe.sections) if (section.flags & IMAGE_SCN_MEM_EXECUTE) {
        for (uint32_t n = 0; n + 8 <= section.rawSize; ++n) {
            auto p = pe.data.data() + section.raw + n;
            if (p[0] == 0x41 && p[1] == 0x80 && p[2] == 0x7E && p[4] == 1 && p[5] == 0x75) {
                auto f = pe.Containing(section.start + n); if (f) candidates.insert(f->start);
            }
        }
    }
    for (uint32_t entry : candidates) {
      try {
        auto function = pe.Containing(entry); auto code = ControlFlow(pe, *function);
        for (auto [at, compare] : code) {
            if (compare.opcode != 0x80 || compare.memoryBase != 14 || ((compare.modrm >> 3) & 7) != 7 ||
                pe.At<uint8_t>(compare.immediateAt) != 1 || compare.displacement <= 0) continue;
            auto branch = code.find(at + compare.length);
            if (branch == code.end() || branch->second.opcode != 0x75 || branch->second.target <= branch->first) continue;
            uint32_t selfOffset = 0; bool reason = false, clearError = false, returned = false;
            for (uint32_t pc = branch->first + branch->second.length; pc < branch->second.target;) {
                auto it = code.find(pc); if (it == code.end()) break;
                auto in = it->second;
                if (in.opcode == 0x8B && in.memoryBase == 14 && (in.rex & 4) && ((in.modrm >> 3) & 7) == 5)
                    selfOffset = in.displacement;
                if (in.opcode == 0xB1 && pe.At<uint8_t>(in.immediateAt) == layout.restoredAuthenticationOrdinal) reason = true;
                if (Is(pe, pc, {0x45,0x31,0xED})) clearError = true;
                if (in.jump && !in.target && in.modrm == 0xE0) { returned = true; break; }
                if (in.conditional || in.jump || in.stop) break;
                pc += in.length;
            }
            if (!(selfOffset && reason && clearError && returned)) continue;
            // Follow only explicitly referenced async entries, including Swift
            // leaf continuations omitted from .pdata. Every instruction is decoded;
            // an entry must terminate within the bounded block.
            std::set<uint32_t> pending;
            for (auto [pc, in] : code) if (pc >= branch->second.target && in.opcode == 0x8D && pe.Executable(in.ripTarget)) pending.insert(in.ripTarget);
            bool reachesAnchor = false;
            for (unsigned depth = 0; depth < 2; ++depth) {
                std::set<uint32_t> next;
                for (auto target : pending) {
                    if (anchors.count(target)) { reachesAnchor = true; continue; }
                    auto f = pe.Containing(target);
                    if (f && f->start != target) continue;
                    auto continuation = ControlFlow(pe, f ? *f : Function{target, target + 256});
                    bool terminates = false;
                    for (auto [pc, in] : continuation) {
                        if (in.stop || in.jump) terminates = true;
                        if (in.opcode == 0x8D && pe.Executable(in.ripTarget)) next.insert(in.ripTarget);
                    }
                    Require(terminates, "Authentication continuation is not bounded.");
                }
                pending = std::move(next);
            }
            if (reachesAnchor) {
                matches.insert(branch->first); layout.bootstrapBranch = branch->first;
                layout.bootstrapContinue = branch->first + branch->second.length;
                layout.bootstrapOriginal = pe.Bytes(branch->first, branch->second.length);
                layout.bootstrapFrameSelf = selfOffset; layout.bootstrapFrameCondition = compare.displacement;
            }
        }
      } catch (const Unsupported&) { /* An unrelated candidate is not a capability. */ }
    }
    Unique(matches, "Missing or ambiguous restored-reason authentication bootstrap continuation.");
}

uint8_t Case(const Nominal& type, const char* name) {
    auto it = std::find(type.fields.begin(), type.fields.end(), name);
    Require(it != type.fields.end() && std::count(type.fields.begin(), type.fields.end(), name) == 1 && type.fields.size() <= 256,
        "Required local command case is missing or ambiguous.");
    return uint8_t(it - type.fields.begin());
}
void ResolveCommands(const PE& pe, NativeLayout& layout, uint32_t authPredicate) {
    auto type = Reflection(pe, "ApplicationAction", 18);
    Require(pe.At<uint32_t>(type.descriptor + 20) == 0 && pe.At<uint32_t>(type.descriptor + 24) == type.fields.size(),
        "ApplicationAction is no longer a simple byte enum.");
    std::vector<uint8_t> expected(32), replacement;
    expected[0] = 10; expected[8] = 20;
    unsigned n = 16;
    for (auto name : {"signIn", "signOut", "quit", "checkForUpdates", "chatWithSupport", "showStatus", "about",
        "setAppearanceLight", "setAppearanceDark", "setAppearanceAutomatic"}) expected[n++] = Case(type, name);
    std::set<uint32_t> arrays;
    for (auto section : pe.sections) if (!(section.flags & IMAGE_SCN_MEM_EXECUTE)) {
        auto begin = pe.data.begin() + section.raw, end = begin + section.rawSize;
        while (begin != end) {
            auto found = std::search(begin, end, expected.begin(), expected.end()); if (found == end) break;
            uint32_t rva = section.start + uint32_t(found - (pe.data.begin() + section.raw));
            if ((rva & 7) == 0) arrays.insert(rva); begin = found + 1;
        }
    }
    std::set<uint32_t> validatedArrays; uint32_t validator = 0, table = 0;
    for (auto array : arrays) {
        auto refs = RipCandidates(pe, {array - 16});
        if (refs.size() != 1) continue;
        auto f = pe.Containing(refs.front()); if (!f) continue;
        auto code = ControlFlow(pe, *f); if (!code.count(refs.front())) continue;
        bool auth = false, staticObject = false, countRead = false, elementRead = false; uint32_t dispatch = 0;
        for (auto [at, in] : code) {
            if (in.call && in.target == authPredicate) auth = true;
            if (at > refs.front() && at < refs.front()+24 && in.call && in.ripTarget &&
                pe.Import(in.ripTarget) == "swift_initStaticObject") staticObject = true;
            if (at > refs.front() && at < refs.front() + 64 && Is(pe, at, {0x48,0x8B,0x48,0x10})) countRead = true;
            if (at > refs.front() && at < refs.front() + 64 && Is(pe, at, {0x38,0x5C,0x10,0x20})) elementRead = true;
            if (in.opcode == 0x8D && in.ripTarget && !pe.Executable(in.ripTarget) &&
                Is(pe, at + in.length, {0x48,0x63,0x04,0x81,0x48,0x01,0xC8,0xFF,0xE0})) {
                bool valid = true;
                for (uint32_t i = 0; i < type.fields.size(); ++i) {
                    int64_t target = int64_t(in.ripTarget) + pe.At<int32_t>(in.ripTarget + i * 4);
                    if (target < f->start || target >= f->end) valid = false;
                }
                if (valid) { Require(!dispatch, "Local command dispatch table is ambiguous."); dispatch = in.ripTarget; }
            }
        }
        if (auth && staticObject && countRead && elementRead && dispatch) { validatedArrays.insert(array); validator = f->start; table = dispatch; }
    }
    uint32_t array = Unique(validatedArrays, "Missing or ambiguous reflected signed-out local command allowlist.");
    replacement = expected; replacement[0] = 15; replacement[8] = 30; n = 26;
    for (auto name : {"settings", "setAsDefaultBrowser", "importFromAnotherBrowser", "newWindow", "newIncognitoWindow"}) replacement[n++] = Case(type, name);
    Patch(pe, layout, "signed-out local command allowlist", array, replacement, false);

    // Follow the reflected New Window and Incognito dispatch arms to their
    // shared predicate. Additional callers are permitted; the predicate itself
    // must retain its local readiness-only fallthrough and ARC cleanup.
    auto f = pe.Containing(validator); std::set<uint32_t> commonCalls; bool first = true;
    for (auto name : {"newWindow", "newIncognitoWindow"}) {
        uint32_t entry = uint32_t(int64_t(table) + pe.At<int32_t>(table + 4 * Case(type, name)));
        auto code = ControlFlow(pe, Function{entry, f->end}); std::set<uint32_t> calls;
        for (auto [at, in] : code) if (in.call && in.target) calls.insert(in.target);
        if (first) { commonCalls = calls; first = false; }
        else { std::set<uint32_t> both; std::set_intersection(calls.begin(), calls.end(), commonCalls.begin(), commonCalls.end(), std::inserter(both,both.end())); commonCalls = std::move(both); }
    }
    std::set<uint32_t> predicates;
    for (auto target : commonCalls) {
        auto candidate = pe.Containing(target); if (!candidate || candidate->start != target || candidate->end - target > 512) continue;
        auto code = ControlFlow(pe, *candidate); uint32_t ready = 0, launch = 0, tag = 0; Instruction nil{}, empty{};
        bool self = false;
        for (auto [at, in] : code) {
            if (Is(pe, at, {0x4C,0x89,0xEE})) self = true; // mov rsi,r13
            if (in.opcode == 0x80 && in.memoryBase == 6 && ((in.modrm >> 3) & 7) == 7) {
                auto value = pe.At<uint8_t>(in.immediateAt);
                if (in.displacement == int32_t(Bound(layout,"hasInitializedBrowserEngine")) && value == 1) ready = at;
                else if (value == 0xFE) tag = in.displacement;
            }
            if (ready && at > ready && in.opcode == 0x8D && in.memoryBase == 6) launch = in.displacement;
            if (Is(pe, at, {0x48,0x85,0xC0})) {
                auto branch = code.find(at + in.length);
                if (branch != code.end() && branch->second.opcode == 0x0F && branch->second.second == 0x84) nil = branch->second;
                if (branch != code.end() && branch->second.opcode == 0x74) empty = branch->second;
            }
        }
        if (!self || !ready || !launch || !tag || !nil.rva || !empty.rva || nil.target != empty.target ||
            empty.rva + empty.length != ready || nil.rva >= empty.rva || !Is(pe,nil.target,{0x31,0xC0})) continue;
        bool authField=false, retained=false, released=false, writesController=false;
        uint32_t principalCall=0; Instruction lastBeforeNil{};
        for (auto [at,in] : code) {
            if ((in.memoryBase==6 || in.memoryBase==13) &&
                (in.opcode==0x88 || in.opcode==0x89 || in.opcode==0xC6 || in.opcode==0xC7)) writesController=true;
            if(at>=nil.rva) continue;
            if(in.opcode==0x8B && in.memoryBase==13 && in.displacement==int32_t(Bound(layout,"authController"))) authField=true;
            if(in.call) {
                lastBeforeNil=in;
                if(in.target) principalCall=in.target;
                if(in.ripTarget && pe.Import(in.ripTarget)=="swift_retain") retained=true;
                if(in.ripTarget && pe.Import(in.ripTarget)=="swift_release") released=true;
            }
        }
        if(!authField || !principalCall || !retained || !released || writesController ||
            !lastBeforeNil.ripTarget || pe.Import(lastBeforeNil.ripTarget)!="swift_release") continue;
        std::vector<Instruction> cleanup; bool ownedPath=true;
        for(uint32_t pc=nil.rva+nil.length;pc<empty.rva;) {
            auto it=code.find(pc); if(it==code.end()) {ownedPath=false;break;}
            const auto& in=it->second;
            if(in.conditional || in.jump || in.stop) {ownedPath=false;break;}
            if(in.call) cleanup.push_back(in);
            pc+=in.length;
        }
        if(!ownedPath || cleanup.size()!=2 || !cleanup[0].target || !cleanup[1].target ||
            !Is(pe,cleanup[0].rva-5,{0x48,0x8D,0x4C,0x24,0x40}) ||
            !Is(pe,cleanup[1].rva-5,{0x48,0x8D,0x4C,0x24,0x20}) ||
            !DestroyHelper(pe,cleanup[0].target,"UserID") || !DestroyHelper(pe,cleanup[1].target,"AccessToken")) continue;
        Require(tag > launch && tag - launch <= 16, "Unsupported native launch-state tag position.");
        predicates.insert(target); Bind(layout,"launchState",launch); layout.launchTagOffset = tag;
        auto replacementNil = pe.Bytes(nil.rva,nil.length); int32_t displacement = int32_t(ready - (nil.rva + nil.length));
        memcpy(replacementNil.data()+2,&displacement,4);
        Patch(pe,layout,"local window availability missing identity",nil.rva,replacementNil);
        Patch(pe,layout,"local window availability empty identity",empty.rva,{0x74,0});
    }
    Unique(predicates, "Missing or ambiguous local window readiness predicate.");
}

void ResolveRevocation(const PE& pe, NativeLayout& layout) {
    auto type = Reflection(pe,"SessionRevocationType",18);
    Require(pe.At<uint32_t>(type.descriptor+20) == 0 && pe.At<uint32_t>(type.descriptor+24) == 2,
        "Session revocation modes are no longer a simple byte enum.");
    uint8_t permissive = Case(type,"permissive");
    Require(type.fields.size() == 2 && Case(type,"strict") != permissive, "Unknown automatic session revocation modes.");
    auto dialogs = AnchorFunctions(pe,"confirmPermissiveSessionRevocation()");
    auto candidates = AnchorFunctions(pe,"Session revocation"); std::set<uint32_t> matches;
    for (auto entry : candidates) {
        auto code = ControlFlow(pe,*pe.Containing(entry));
        unsigned modeComparisons = 0, modeLoads = 0; bool otherModeUse = false;
        for (auto [pc,in] : code) {
            if (in.opcode == 0x80 && in.rex == 0x40 && in.modrm == 0xFD &&
                pe.At<uint8_t>(in.immediateAt) == permissive) { ++modeComparisons; continue; }
            if (in.opcode == 0x0F && in.second == 0xB6 && in.memoryBase == 14 &&
                ((in.modrm >> 3) & 7) == 5 && !(in.rex & 4)) { ++modeLoads; continue; }
            if ((in.modrm >> 6) == 3 && in.modrm &&
                ((((in.modrm & 7) == 5) && !(in.rex & 1)) ||
                 ((((in.modrm >> 3) & 7) == 5) && !(in.rex & 4) &&
                    (in.opcode == 0x8B || in.opcode == 0x89 || in.opcode == 0x01 || in.opcode == 0x09 || in.opcode == 0x31 || in.opcode == 0x85 || in.opcode == 0x0F)))) otherModeUse = true;
        }
        // One mode load feeds the log comparison and the routing comparison.
        // New consumers in this chunk invalidate this capability.
        if (modeLoads != 1 || modeComparisons != 2 || otherModeUse) continue;
        for (auto [at,in] : code) {
            if (in.opcode != 0x80 || in.rex != 0x40 || in.modrm != 0xFD || pe.At<uint8_t>(in.immediateAt) != permissive) continue;
            auto branch = code.find(at + in.length);
            if (branch == code.end() || branch->second.opcode != 0x75 || branch->second.target <= branch->first) continue;
            bool dialog = false, strict = false;
            for (auto [pc,child] : code) {
                if (pc > branch->first && pc < branch->second.target && child.opcode == 0x8D && dialogs.count(child.ripTarget)) dialog = true;
                if (pc >= branch->second.target && child.jump && child.target) strict = true;
            }
            if (dialog && strict) { matches.insert(branch->first); Patch(pe,layout,"automatic revocation offers Not Now",branch->first,{0x90,0x90}); }
        }
    }
    Unique(matches,"Missing or ambiguous automatic revocation dialog choice.");
}

void ResolveStateBindings(const PE& pe, NativeLayout& layout) {
    for (auto name : {"hasFinishedRestoringWindows", "sessionRestorationInProgress"}) {
        std::set<uint32_t> targets;
        for (auto rva : pe.Strings("_" + std::string(name))) if (rva >= 32) targets.insert(rva - 32);
        std::set<uint32_t> offsets;
        for (auto ref : RipCandidates(pe,targets)) {
            auto f = pe.Containing(ref); if (!f) continue;
            auto code = ControlFlow(pe,*f); if (!code.count(ref)) continue;
            bool self = false;
            for (auto [at,in] : code) {
                if (at < ref && Is(pe,at,{0x4D,0x89,0xEF})) self = true; // r15 = self r13
                if (self && at < ref && ref - at <= 80 && in.opcode == 0x0F && in.second == 0xB6 &&
                    in.memoryBase == 15 && in.displacement > 0) offsets.insert(in.displacement);
            }
        }
        Bind(layout,name,Unique(offsets,"Native restoration-state diagnostic binding is ambiguous."));
    }
}

void ResolveLaunchSuccess(const PE& pe, NativeLayout& layout) {
    auto state = Reflection(pe,"LaunchState",18);
    Require(state.fields == std::vector<std::string>{"inProgress", "complete"} &&
        (pe.At<uint32_t>(state.descriptor + 20) & 0xFFFFFF) == 2 && pe.At<uint32_t>(state.descriptor + 24) == 0,
        "The native launch state is no longer the supported two-payload enum.");
    // The complete payload is Result<Void, Error>, with symbolic references
    // resolved through the PE import table rather than build-specific addresses.
    uint32_t completeType = pe.Relative(state.fieldDescriptor + 16 + 12 + 4);
    Require(pe.At<uint8_t>(completeType) == 2 && Is(pe,completeType+5,{0x79,0x79,0x74,0x02}) &&
        Is(pe,completeType+13,{0x5F,0x70,0x47,0}), "Unsupported launch completion payload encoding.");
    Require(pe.Import(pe.Relative(completeType+1)) == "$ss6ResultOMn" &&
        pe.Import(pe.Relative(completeType+9)) == "$ss5ErrorMp", "Launch completion is not Swift Result<Void, Error>.");

    uint32_t storage = Bound(layout,"launchState"); std::set<uint8_t> successTags; std::set<uint32_t> matches;
    for (auto section : pe.sections) if (section.flags & IMAGE_SCN_MEM_EXECUTE) {
        for (uint32_t n = 0; n + 24 < section.rawSize; ++n) {
            auto p = pe.data.data()+section.raw+n;
            if (p[0] != 0x48 || p[1] != 0xC7 || (p[2] & 0xF8) != 0x80) continue;
            uint32_t displacement; memcpy(&displacement,p+3,4);
            if (displacement != storage || memcmp(p+7,"\0\0\0\0",4)) continue;
            uint32_t at = section.start+n; auto f = pe.Containing(at); if (!f) continue;
            auto code = ControlFlow(pe,*f); auto store = code.find(at); if (store == code.end()) continue;
            auto tag = code.find(at+store->second.length); if (tag == code.end()) continue;
            const auto& in = tag->second;
            if (in.opcode != 0xC6 || in.memoryBase != store->second.memoryBase || in.displacement != int32_t(layout.launchTagOffset)) continue;
            auto destroy = code.find(tag->first+in.length); if (destroy == code.end() || !destroy->second.call || !destroy->second.target) continue;
            uint32_t d = destroy->second.target;
            // The optional-state destructor distinguishes nil, a retained task,
            // and a signed completion payload. Its low bits are the Result tag.
            if (!Is(pe,d,{0x80,0xFA,0xFD,0x0F,0x86}) || pe.At<uint8_t>(d+9) != 0xC3) continue;
            auto toPayload = Decode(pe,d+3); uint32_t body=toPayload.target;
            if (!Is(pe,body,{0x84,0xD2,0x78})) continue;
            auto toResult = Decode(pe,body+2); uint32_t resultBody=toResult.target;
            if (!Is(pe,resultBody,{0x80,0xE2})) continue;
            uint8_t mask=pe.At<uint8_t>(resultBody+2);
            auto resultJump=Decode(pe,resultBody+3);
            if (!resultJump.jump || !resultJump.target || mask != 0x7F) continue;
            uint32_t r=resultJump.target;
            if (!Is(pe,r,{0x80,0xFA,0x01,0x75})) continue;
            auto success=Decode(pe,r+3), release=Decode(pe,r+5);
            if (pe.At<uint8_t>(success.target) != 0xC3 || !release.jump ||
                pe.Import(release.ripTarget) != "swift_release") continue;
            uint8_t value=pe.At<uint8_t>(in.immediateAt);
            if (value != uint8_t(~mask)) continue;
            matches.insert(at); successTags.insert(value);
        }
    }
    Require(!matches.empty() && successTags.size() == 1, "Missing or ambiguous native successful launch completion store.");
    layout.launchCompleteSuccessTag=*successTags.begin();
}

} // namespace

Resolution Resolve(const std::vector<uint8_t>& image) {
    Resolution result;
    try {
        PE pe(image);
        result.layout.imageSize = pe.imageSize; result.layout.timeStamp = pe.stamp; result.layout.preferredBase = pe.base;
        ResolveController(pe, result.layout);
        uint32_t authPredicate = ResolveWindows(pe, result.layout);
        ResolveBootstrap(pe, result.layout);
        ResolveCommands(pe, result.layout, authPredicate);
        ResolveRevocation(pe, result.layout);
        ResolveStateBindings(pe, result.layout);
        ResolveLaunchSuccess(pe, result.layout);
        // Further capabilities are resolved together before admission. A partial
        // reflection match alone must never authorize any process modification.
        result.supported = true;
        result.message = "Native local browsing capabilities verified from this Dia image.";
    } catch (const Unsupported& error) { result.message = error.what(); }
    return result;
}

Resolution ResolveFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {false, {}, "Could not open the installed Dia native image."};
    struct FileOwner { HANDLE value; ~FileOwner(){CloseHandle(value);} } owner{file};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024ll * 1024 * 1024) {
        return {false, {}, "Dia native image has an invalid or unsupported file size."};
    }
    std::vector<uint8_t> bytes((size_t)size.QuadPart);
    size_t offset = 0; bool ok = true;
    while (offset < bytes.size()) {
        DWORD got = 0, requested = (DWORD)std::min<size_t>(bytes.size() - offset, 1024 * 1024);
        if (!ReadFile(file, bytes.data() + offset, requested, &got, nullptr) || !got) { ok = false; break; }
        offset += got;
    }
    if (!ok) return {false, {}, "Could not read the complete Dia native image."};
    return Resolve(bytes);
}
}
