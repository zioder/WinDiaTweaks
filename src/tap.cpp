// DiaTweaksTap.dll - loaded inside Dia by WinUI's XAML diagnostics (InitializeXamlDiagnosticsEx).
// Tracks Dia's live visual tree and applies property overrides from tweaks.conf.
// Nothing on disk in Dia's install folder is touched.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ocidl.h>
#include <xamlOM.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <exdisp.h>
#include <shldisp.h>
#include <shlobj.h>

#include "engine.h"
#include "llhook.h"
#include "shared.h"
#pragma comment(lib, "uuid.lib")

// ---------------------------------------------------------------- utils

static HMODULE g_module;
static thread_local unsigned long long t_treeRevision = 0;
static std::wstring g_dir;  // folder holding the DLL, tweaks.conf, logs

static std::wstring Narrow2Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string Wide2Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring B(BSTR b) { return b ? std::wstring(b, SysStringLen(b)) : std::wstring(); }

static std::mutex g_logMutex;
static void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_logMutex);
    FILE* f = _wfopen((g_dir + L"\\tap.log").c_str(), L"a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d [%lu] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentThreadId(), buf);
    fclose(f);
}

static std::wstring ShortType(const std::wstring& t) {
    size_t p = t.find_last_of(L'.');
    return p == std::wstring::npos ? t : t.substr(p + 1);
}

// ---------------------------------------------------------------- tree model

struct Node {
    InstanceHandle parent = 0;
    unsigned childIndex = 0;
    std::wstring type, name;
    DWORD thread = 0;
    std::vector<InstanceHandle> children;
};

static std::recursive_mutex g_mx;
static std::unordered_map<InstanceHandle, Node> g_nodes;
static std::vector<InstanceHandle> g_roots;

// ---------------------------------------------------------------- rules

struct Cond {
    std::wstring field;  // type name ancestor ancestorType parentType parentName index
    bool contains = false;  // value given as ~text
    bool negate = false;    // field!=value
    std::wstring value;
};
struct Setter {
    std::wstring prop, value;
};
struct Rule {
    std::wstring tweak;
    std::vector<Cond> conds;
    std::vector<Setter> sets;
};
using Tweak = TweakInfo;

static std::vector<Tweak> g_tweaks;
// Safe mode: Dia crashed twice in a row right after tweaks loaded, so nothing is applied until the
// user turns tweaks back on from Settings > Patches.
static bool g_safeMode = false;
static void CheckSafeMode();
static void NoteTweaksForCrashReport();
static void InstallCrashReport();
static void ClearCrashCount();
static void InstallCrashGuard();
static DWORD WINAPI StableTimer(LPVOID);
static void ApplyPinnedExtensionsLimit();
static std::vector<Rule> g_rules;
// (ui thread, type, value) -> value instance. Value handles belong to the XAML island they were made
// in, so they are kept per thread and thrown away whenever the config reloads.
static std::map<std::tuple<DWORD, std::wstring, std::wstring>, InstanceHandle> g_valueCache;

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Splits on spaces, but keeps "quoted strings" together.
static std::vector<std::wstring> Tokens(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool q = false;
    for (wchar_t c : s) {
        if (c == L'"') { q = !q; continue; }
        if (!q && (c == L' ' || c == L'\t')) {
            if (!cur.empty()) out.push_back(cur), cur.clear();
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool g_startRecorded = false;
static std::map<std::wstring, std::wstring> g_startStates;
static void LoadConfig() {
    std::vector<Tweak> tweaks;
    std::vector<Rule> rules;
    std::ifstream in(g_dir + L"\\tweaks.conf");
    std::string line8;
    Tweak* cur = nullptr;
    while (std::getline(in, line8)) {
        std::wstring line = Trim(Narrow2Wide(line8));
        if (line.empty() || line[0] == L'#') continue;
        if (line[0] == L'[') {
            size_t e = line.find(L']');
            if (e == std::wstring::npos) continue;
            Tweak t;
            t.key = line.substr(1, e - 1);
            t.enabled = Trim(line.substr(e + 1)) == L"on";
            tweaks.push_back(t);
            cur = &tweaks.back();
            continue;
        }
        if (!cur) continue;
        if (line.rfind(L"title:", 0) == 0) { cur->title = Trim(line.substr(6)); continue; }
        if (line.rfind(L"desc:", 0) == 0) { cur->desc = Trim(line.substr(5)); continue; }
        if (line.rfind(L"note:", 0) == 0) { cur->note = Trim(line.substr(5)); continue; }
        if (line.rfind(L"section:", 0) == 0) { cur->section = Trim(line.substr(8)); continue; }
        if (line.rfind(L"icon:", 0) == 0) { cur->icon = Trim(line.substr(5)); continue; }
        if (line.rfind(L"value:", 0) == 0) { cur->value = Trim(line.substr(6)); continue; }
        if (line.rfind(L"switch:", 0) == 0) { cur->noSwitch = Trim(line.substr(7)) == L"none"; continue; }
        if (line.rfind(L"restart:", 0) == 0) { cur->restart = Trim(line.substr(8)) == L"yes"; continue; }
        if (line.rfind(L"choices:", 0) == 0) {
            std::wstringstream cs(line.substr(8));
            std::wstring part;
            while (std::getline(cs, part, L',')) {
                size_t eq = part.find(L'=');
                if (eq == std::wstring::npos) continue;
                cur->choices.push_back({Trim(part.substr(0, eq)), Trim(part.substr(eq + 1))});
            }
            continue;
        }
        if (line.rfind(L"match ", 0) == 0) {
            size_t arrow = line.find(L"=>");
            if (arrow == std::wstring::npos) continue;
            Rule r;
            r.tweak = cur->key;
            for (auto& tok : Tokens(line.substr(6, arrow - 6))) {
                size_t eq = tok.find(L'=');
                if (eq == std::wstring::npos) continue;
                Cond c;
                c.field = tok.substr(0, eq);
                if (!c.field.empty() && c.field.back() == L'!') c.negate = true, c.field.pop_back();
                c.value = tok.substr(eq + 1);
                if (!c.value.empty() && c.value[0] == L'~') c.contains = true, c.value = c.value.substr(1);
                r.conds.push_back(c);
            }
            std::wstringstream ss(line.substr(arrow + 2));
            std::wstring part;
            while (std::getline(ss, part, L';')) {
                part = Trim(part);
                size_t eq = part.find(L'=');
                if (eq == std::wstring::npos) continue;
                r.sets.push_back({Trim(part.substr(0, eq)), Trim(part.substr(eq + 1))});
            }
            rules.push_back(r);
        }
    }
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    // What "restart: yes" tweaks were set to as Dia started: that's what is in effect until it reopens.
    if (!g_startRecorded) {
        g_startRecorded = true;
        for (auto& t : tweaks)
            if (t.restart) g_startStates[t.key] = t.enabled && !g_safeMode ? t.value : L"off";
    }
    g_tweaks = tweaks;
    g_rules = rules;
    g_valueCache.clear();
    Log("config: %zu tweaks, %zu rules", tweaks.size(), rules.size());
    NoteTweaksForCrashReport();
}

static bool TweakOn(const std::wstring& key) {
    if (g_safeMode) return false;
    for (auto& t : g_tweaks)
        if (t.key == key) return t.enabled;
    return false;
}

static bool StrMatch(const Cond& c, const std::wstring& s, bool typeField) {
    bool m;
    if (c.contains) m = s.find(c.value) != std::wstring::npos;
    else m = s == c.value || (typeField && ShortType(s) == c.value);
    return c.negate ? !m : m;
}

// Live window state that rules can depend on (state=<name>). Updated on the UI thread by the timer.
static bool g_hoverTop = false;

static std::wstring TweakValueOf(const std::wstring& key) {
    for (auto& t : g_tweaks)
        if (t.key == key) return t.value;
    return {};
}

static bool CondHolds(const Cond& c, const std::wstring& tweak, InstanceHandle h, const Node& n) {
    if (c.field == L"type") return StrMatch(c, n.type, true);
    if (c.field == L"tweak") return TweakOn(c.value) != c.negate;
    if (c.field == L"value") return (TweakValueOf(tweak) == c.value) != c.negate;  // the rule's own tweak
    if (c.field == L"state") {
        bool on;
        if (c.value == L"hover") on = g_hoverTop;
        else if (c.value == L"showToolbar")  // the toolbar is hidden, but a "show toolbar" tweak wants it now
            on = (TweakOn(L"hideToolbar") && g_hoverTop) ||  // the hidden toolbar always comes back on hover
                 (TweakOn(L"showToolbarWithSidebar") && Ui_State(L"sidebarOpen")) ||
                 (TweakOn(L"showToolbarOnNewTab") && Ui_State(L"newTab"));
        else on = Ui_State(c.value);
        return on != c.negate;
    }
    // upN=<name>: the element N levels up has this name (up1 is the parent). Pins one exact element
    // when it has no name of its own.
    if (c.field.size() > 2 && c.field.compare(0, 2, L"up") == 0 && iswdigit(c.field[2])) {
        int levels = _wtoi(c.field.c_str() + 2);
        InstanceHandle p = n.parent;
        for (int i = 1; i < levels && p; i++) {
            auto it = g_nodes.find(p);
            p = it == g_nodes.end() ? 0 : it->second.parent;
        }
        auto it = g_nodes.find(p);
        if (!p || it == g_nodes.end()) return c.negate;
        return StrMatch(c, it->second.name, false);
    }
    if (c.field == L"name") return StrMatch(c, n.name, false);
    if (c.field == L"index") return (std::to_wstring(n.childIndex) == c.value) != c.negate;
    if (c.field == L"parentType" || c.field == L"parentName") {
        auto it = g_nodes.find(n.parent);
        if (it == g_nodes.end()) return c.negate;
        return c.field == L"parentType" ? StrMatch(c, it->second.type, true) : StrMatch(c, it->second.name, false);
    }
    if (c.field == L"ancestor" || c.field == L"ancestorType") {
        bool found = false;
        Cond pos = c;
        pos.negate = false;
        for (InstanceHandle p = n.parent; p;) {
            auto it = g_nodes.find(p);
            if (it == g_nodes.end()) break;
            if (c.field == L"ancestor" ? StrMatch(pos, it->second.name, false) : StrMatch(pos, it->second.type, true)) {
                found = true;
                break;
            }
            p = it->second.parent;
        }
        return found != c.negate;
    }
    return false;
}

static bool RuleMatches(const Rule& r, InstanceHandle h, const Node& n) {
    for (auto& c : r.conds)
        if (!CondHolds(c, r.tweak, h, n)) return false;
    return true;
}

// ---------------------------------------------------------------- applying

static IXamlDiagnostics* g_diag;
static IVisualTreeService2* g_vts;

struct Applied {
    unsigned prop;
    InstanceHandle value;
    bool hadLocal;
    // The element's own local value before we touched it, kept as text and re-created on undo:
    // value handles can go stale while the element lives on, text cannot.
    std::wstring origType, origText;
    InstanceHandle origHandle = 0;  // fallback for values WinUI can't print (e.g. CornerRadius)
    std::wstring ourText;           // the value the tweak sets, as written in tweaks.conf
};
// True while the engine itself changes a property, so the change watchers in ui.cpp ignore it.
static bool g_selfChange = false;
struct SelfChange {
    bool prev;
    SelfChange() : prev(g_selfChange) { g_selfChange = true; }
    ~SelfChange() { g_selfChange = prev; }
};
// element -> (prop index -> applied)
static std::unordered_map<InstanceHandle, std::map<unsigned, Applied>> g_applied;
static std::map<std::wstring, std::wstring> g_valueType;  // prop name -> value type for CreateInstance

static InstanceHandle MakeValue(const std::wstring& vt, const std::wstring& val) {
    BSTR bt = SysAllocString(vt.c_str()), bv = SysAllocString(val.c_str());
    InstanceHandle out = 0;
    HRESULT hr = g_vts->CreateInstance(bt, bv, &out);
    SysFreeString(bt);
    SysFreeString(bv);
    if (FAILED(hr)) {
        Log("CreateInstance(%s,%s) failed 0x%08lx", Wide2Narrow(vt).c_str(), Wide2Narrow(val).c_str(), hr);
        return 0;
    }
    return out;
}

// Resolves a property by name straight from the element's property chain. GetPropertyIndex() rejects
// plain names on this WinUI build, but every chain entry already carries the index we need.
static bool LookupProp(InstanceHandle h, const std::wstring& prop, unsigned& idx, std::wstring& valueType,
                       bool& hasLocal, std::wstring* localType = nullptr, std::wstring* localText = nullptr) {
    hasLocal = false;
    unsigned srcCount = 0, propCount = 0;
    PropertyChainSource* srcs = nullptr;
    PropertyChainValue* vals = nullptr;
    bool found = false;
    HRESULT hr = g_vts->GetPropertyValuesChain(h, &srcCount, &srcs, &propCount, &vals);
    if (SUCCEEDED(hr)) {
        for (unsigned i = 0; i < propCount; i++) {
            if (B(vals[i].PropertyName) != prop) continue;
            if (!found) {
                idx = vals[i].Index;
                found = true;
            }
            if (valueType.empty()) valueType = B(vals[i].ValueType);
            if (!vals[i].Overridden && vals[i].PropertyChainIndex < srcCount &&
                srcs[vals[i].PropertyChainIndex].Source == BaseValueSourceLocal) {
                hasLocal = true;
                if (localType) *localType = B(vals[i].ValueType);
                if (localText) *localText = B(vals[i].Value);
            }
        }
        for (unsigned i = 0; i < srcCount; i++) {
            SysFreeString(srcs[i].TargetType);
            SysFreeString(srcs[i].Name);
            SysFreeString(srcs[i].SrcInfo.FileName);
            SysFreeString(srcs[i].SrcInfo.Hash);
        }
        for (unsigned i = 0; i < propCount; i++) {
            SysFreeString(vals[i].Type);
            SysFreeString(vals[i].DeclaringType);
            SysFreeString(vals[i].ValueType);
            SysFreeString(vals[i].ItemType);
            SysFreeString(vals[i].Value);
            SysFreeString(vals[i].PropertyName);
        }
        CoTaskMemFree(srcs);
        CoTaskMemFree(vals);
    }
    return found;
}

static void ApplySetter(InstanceHandle h, const Setter& s) {
    unsigned idx = 0;
    bool hasLocal = false;
    std::wstring vt;
    auto vtIt = g_valueType.find(s.prop);
    if (vtIt != g_valueType.end()) vt = vtIt->second;

    auto& perElem = g_applied[h];
    std::wstring localType, localText;
    if (!LookupProp(h, s.prop, idx, vt, hasLocal, &localType, &localText)) {
        Log("no property %s on element", Wide2Narrow(s.prop).c_str());
        return;
    }
    // A bound or untyped property reports its binding's type, not the value's: use the real type.
    if (vt.empty() || vt.find(L"Binding") != std::wstring::npos || vt == L"Windows.Foundation.Object") {
        static const std::map<std::wstring, std::wstring> known = {
            {L"Opacity", L"Windows.Foundation.Double"},       {L"Width", L"Windows.Foundation.Double"},
            {L"Height", L"Windows.Foundation.Double"},        {L"FontSize", L"Windows.Foundation.Double"},
            {L"Visibility", L"Microsoft.UI.Xaml.Visibility"}, {L"Margin", L"Microsoft.UI.Xaml.Thickness"},
            {L"Padding", L"Microsoft.UI.Xaml.Thickness"},     {L"BorderThickness", L"Microsoft.UI.Xaml.Thickness"},
            {L"CornerRadius", L"Microsoft.UI.Xaml.CornerRadius"}, {L"IsHitTestVisible", L"Windows.Foundation.Boolean"}};
        auto k = known.find(s.prop);
        if (k == known.end()) return Log("unknown value type for %s", Wide2Narrow(s.prop).c_str());
        vt = k->second;
    } else g_valueType[s.prop] = vt;

    auto key = std::make_tuple(GetCurrentThreadId(), vt, s.value);
    InstanceHandle val = 0;
    auto vc = g_valueCache.find(key);
    if (vc != g_valueCache.end()) val = vc->second;
    else {
        val = MakeValue(vt, s.value);
        if (!val) return;
        g_valueCache[key] = val;
    }

    auto ex = perElem.find(idx);
    if (ex == perElem.end()) {
        Applied a{idx, val, hasLocal, localType, localText};
        if (hasLocal) g_vts->GetProperty(h, idx, &a.origHandle);
        perElem[idx] = a;
    }
    else ex->second.value = val;

    perElem[idx].ourText = s.value;
    SelfChange guard;
    HRESULT hr = g_vts->SetProperty(h, val, idx);
    if (hr == E_NOTFOUND) {
        // The cached handle came from an island that has since gone away; make a fresh one and retry.
        val = MakeValue(vt, s.value);
        if (!val) return;
        g_valueCache[key] = val;
        perElem[idx].value = val;
        hr = g_vts->SetProperty(h, val, idx);
    }
    if (FAILED(hr)) Log("SetProperty %s failed 0x%08lx", Wide2Narrow(s.prop).c_str(), hr);
    else Ui_WatchProperty(h, s.prop);  // put it back the moment Dia changes it
}

// Enum values are reported as numbers but created from names.
static std::wstring EnumText(const std::wstring& type, const std::wstring& text) {
    if (type.find(L"Visibility") != std::wstring::npos) return text == L"1" ? L"Collapsed" : text == L"0" ? L"Visible" : text;
    return text;
}

static void UndoOne(InstanceHandle h, unsigned idx, const Applied& a) {
    SelfChange guard;
    if (a.hadLocal && !a.origType.empty() && !a.origText.empty() &&
        a.origType.find(L"Binding") == std::wstring::npos) {
        InstanceHandle v = MakeValue(a.origType, EnumText(a.origType, a.origText));
        if (v && SUCCEEDED(g_vts->SetProperty(h, v, idx))) return;
    }
    if (a.hadLocal && a.origHandle && SUCCEEDED(g_vts->SetProperty(h, a.origHandle, idx))) return;
    g_vts->ClearProperty(h, idx);
}

static void Restore(InstanceHandle h) {
    auto it = g_applied.find(h);
    if (it == g_applied.end()) return;
    for (auto& [idx, a] : it->second) UndoOne(h, idx, a);
    g_applied.erase(it);
}

// Re-evaluates one element against all enabled rules. UI thread only.
static void Evaluate(InstanceHandle h) {
    auto it = g_nodes.find(h);
    if (it == g_nodes.end()) return;
    std::vector<const Setter*> wanted;
    for (auto& r : g_rules)
        if (TweakOn(r.tweak) && RuleMatches(r, h, it->second))
            for (auto& s : r.sets) wanted.push_back(&s);

    if (wanted.empty()) {
        Restore(h);
        return;
    }
    // drop props no longer wanted
    auto ap = g_applied.find(h);
    if (ap != g_applied.end()) {
        std::vector<unsigned> keep;
        for (auto* s : wanted) {
            unsigned idx;
            bool dummyLocal;
            std::wstring dummyType;
            if (LookupProp(h, s->prop, idx, dummyType, dummyLocal)) keep.push_back(idx);
        }
        for (auto a = ap->second.begin(); a != ap->second.end();) {
            if (std::find(keep.begin(), keep.end(), a->first) == keep.end()) {
                UndoOne(h, a->first, a->second);
                a = ap->second.erase(a);
            } else ++a;
        }
    }
    for (auto* s : wanted) ApplySetter(h, *s);
}

// Evaluates an element and every descendant already in the model.
static void EvaluateSubtree(InstanceHandle root) {
    std::vector<InstanceHandle> todo{root};
    std::unordered_set<InstanceHandle> seen;
    while (!todo.empty()) {
        InstanceHandle h = todo.back();
        todo.pop_back();
        if (!seen.insert(h).second) continue;
        auto it = g_nodes.find(h);
        if (it == g_nodes.end()) continue;
        todo.insert(todo.end(), it->second.children.begin(), it->second.children.end());
        Evaluate(h);
    }
}

// Dia's own code may reset values (e.g. toggling Visibility); re-assert ours.
static void Reassert() {
    for (auto& [h, props] : g_applied) {
        auto n = g_nodes.find(h);
        if (n == g_nodes.end() || n->second.thread != GetCurrentThreadId()) continue;
        SelfChange guard;
        for (auto& [idx, a] : props) g_vts->SetProperty(h, a.value, idx);
    }
}

static std::wstring NoSpaces(std::wstring v) {
    v.erase(std::remove(v.begin(), v.end(), L' '), v.end());
    return v;
}

// Dia just changed a property a tweak overrides (e.g. its page inset while the sidebar animates).
// Remember Dia's new value for when the tweak is turned off, and put the tweak's value back right away,
// in the same frame, so the change never reaches the screen.
void Engine_OnDiaChanged(InstanceHandle h, const std::wstring& prop, const std::wstring& newText) {
    if (g_selfChange || !g_vts) return;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto el = g_applied.find(h);
    if (el == g_applied.end()) return;
    unsigned idx = 0;
    bool hasLocal = false;
    std::wstring vt;
    if (!LookupProp(h, prop, idx, vt, hasLocal)) return;
    auto a = el->second.find(idx);
    if (a == el->second.end()) return;
    if (NoSpaces(newText) == NoSpaces(a->second.ourText)) return;
    if (!newText.empty()) {
        a->second.hadLocal = true;
        a->second.origText = newText;
        if (a->second.origType.empty() || a->second.origType.find(L"Binding") != std::wstring::npos) {
            auto t = g_valueType.find(prop);
            if (t != g_valueType.end()) a->second.origType = t->second;
        }
        a->second.origHandle = 0;
    }
    SelfChange guard;
    g_vts->SetProperty(h, a->second.value, idx);
}

// ---------------------------------------------------------------- commands (dump etc.)

static std::atomic<bool> g_reloadPending{false};
static std::atomic<bool> g_dumpPending{false};
static std::atomic<int> g_reassertTick{0};

static void DumpNode(std::wofstream& out, InstanceHandle h, int depth);

// Writes the handful of properties tweaks care about, with their exact names, values and value types.
static void DumpProps(std::wofstream& out, InstanceHandle h, int depth) {
    if (!g_vts) return;
    unsigned srcCount = 0, propCount = 0;
    PropertyChainSource* srcs = nullptr;
    PropertyChainValue* vals = nullptr;
    if (FAILED(g_vts->GetPropertyValuesChain(h, &srcCount, &srcs, &propCount, &vals))) return;
    static const wchar_t* keep[] = {L"Visibility", L"Width", L"Height", L"MinWidth", L"MaxWidth",
                                    L"MinHeight", L"MaxHeight", L"Padding", L"Margin", L"Opacity",
                                    L"CornerRadius", L"Background", L"BorderThickness", L"BorderBrush",
                                    L"HorizontalAlignment", L"VerticalAlignment", L"IsHitTestVisible",
                                    L"Spacing", L"FontSize", L"Foreground"};
    for (unsigned i = 0; i < propCount; i++) {
        std::wstring nm = B(vals[i].PropertyName);
        bool k = false;
        for (auto p : keep)
            if (nm == p) k = true;
        if (!k) continue;
        out << std::wstring(depth * 2 + 4, L' ') << L"." << nm << L" = " << B(vals[i].Value)
            << L"   [" << B(vals[i].ValueType) << L"]  idx=" << vals[i].Index << L"\n";
    }
    for (unsigned i = 0; i < srcCount; i++) {
        SysFreeString(srcs[i].TargetType);
        SysFreeString(srcs[i].Name);
        SysFreeString(srcs[i].SrcInfo.FileName);
        SysFreeString(srcs[i].SrcInfo.Hash);
    }
    for (unsigned i = 0; i < propCount; i++) {
        SysFreeString(vals[i].Type);
        SysFreeString(vals[i].DeclaringType);
        SysFreeString(vals[i].ValueType);
        SysFreeString(vals[i].ItemType);
        SysFreeString(vals[i].Value);
        SysFreeString(vals[i].PropertyName);
    }
    CoTaskMemFree(srcs);
    CoTaskMemFree(vals);
}

static bool g_dumpAll = false;

static void DumpNode(std::wofstream& out, InstanceHandle h, int depth) {
    auto it = g_nodes.find(h);
    if (it == g_nodes.end()) return;
    const Node& n = it->second;
    out << std::wstring(depth * 2, L' ') << ShortType(n.type);
    if (!n.name.empty()) out << L" \"" << n.name << L"\"";
    out << L"  #" << n.childIndex << L"  {" << n.type << L"}";
    if (g_applied.count(h)) out << L"  *tweaked*";
    out << L"\n";
    if (!n.name.empty() || g_dumpAll) DumpProps(out, h, depth);
    for (auto c : n.children) DumpNode(out, c, depth + 1);
}

static void DoDump() {
    g_dumpAll = GetFileAttributesW((g_dir + L"\\allprops.flag").c_str()) != INVALID_FILE_ATTRIBUTES;
    std::wofstream out(g_dir + L"\\tree.txt");
    out << L"# Dia visual tree, " << g_nodes.size() << L" elements\n";
    for (auto r : g_roots) DumpNode(out, r, 0);
    out.close();
    Log("dumped %zu nodes", g_nodes.size());
}

static std::vector<DWORD> g_uiThreads;

static bool RulesUseState() {
    for (auto& r : g_rules)
        if (TweakOn(r.tweak))
            for (auto& c : r.conds)
                if (c.field == L"state") return true;
    return false;
}

// ---- hover: a watcher thread checks the pointer every few milliseconds and pings the UI thread the
// moment it enters or leaves the top edge, instead of waiting for the next (low-priority) timer tick.

static std::atomic<bool> g_watchHover{false};
static std::atomic<DWORD> g_hoverThread{0};  // UI thread that owns the main window(s)
static std::atomic<HWND> g_pingWnd{nullptr};
static const UINT WM_DT_HOVER = WM_APP + 0x3a1;

// True while the pointer sits at the top edge of a Dia window owned by `uiThread`. Once shown, it
// stays true until the pointer moves below the toolbar, so the toolbar can be used.
static bool PointerAtTop(DWORD uiThread, bool shown) {
    POINT pt;
    if (!GetCursorPos(&pt)) return false;
    HWND w = WindowFromPoint(pt);
    if (!w) return false;
    HWND root = GetAncestor(w, GA_ROOT);
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(root, &pid);
    if (pid != GetCurrentProcessId()) return false;
    // Only Dia's own windows have a top edge. A context menu (or any other popup) opens with its top
    // at the pointer, so measuring from it would reveal the toolbar on every right-click (#38). Over a
    // popup, keep things as they are: a menu opened from the shown toolbar doesn't hide it.
    wchar_t cls[64];
    if (!GetClassNameW(root, cls, ARRAYSIZE(cls)) || wcscmp(cls, L"WinUIDesktopWin32WindowClass") != 0)
        return shown;
    if (tid != uiThread) return false;
    // A button held (dragging the window, a tab, a selection): never starts showing the toolbar, or
    // moving the window by its top would bring it down. (Already shown, it stays while you're on it.)
    if (!shown && (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000)) return false;
    POINT origin{0, 0};
    ClientToScreen(root, &origin);
    UINT dpi = GetDpiForWindow(root);
    if (!dpi) dpi = 96;
    int y = pt.y - origin.y;
    int showBand = MulDiv(6, dpi, 96), keepBand = MulDiv(52, dpi, 96);
    return y >= 0 && y < (shown ? keepBand : showBand);
}

static DWORD WINAPI HoverWatcher(LPVOID) {
    bool last = false;
    for (;;) {
        if (!g_watchHover || !g_pingWnd) {
            if (last) last = false, PostMessageW(g_pingWnd, WM_DT_HOVER, 0, 0);
            Sleep(60);
            continue;
        }
        bool now = PointerAtTop(g_hoverThread, last);
        if (now != last) {
            last = now;
            PostMessageW(g_pingWnd, WM_DT_HOVER, now, 0);
        }
        Sleep(8);
    }
}

static void EvaluateThread() {
    for (auto& [h, n] : g_nodes)
        if (n.thread == GetCurrentThreadId()) Evaluate(h);
}

// Re-evaluates only the elements that rules with a state= condition can apply to (by name), so a
// state change touches a couple of elements instead of the whole tree.
static void EvaluateStateNodes() {
    std::vector<std::wstring> names;
    bool anyUnnamed = false;
    for (auto& r : g_rules) {
        bool usesState = false;
        const Cond* nameCond = nullptr;
        for (auto& c : r.conds) {
            if (c.field == L"state") usesState = true;
            if (c.field == L"name" && !c.negate && !c.contains) nameCond = &c;
        }
        if (!usesState) continue;
        if (nameCond) names.push_back(nameCond->value);
        else anyUnnamed = true;
    }
    if (anyUnnamed) return EvaluateThread();
    DWORD tid = GetCurrentThreadId();
    for (auto& [h, n] : g_nodes)
        if (n.thread == tid && std::find(names.begin(), names.end(), n.name) != names.end()) Evaluate(h);
}

static LRESULT CALLBACK PingProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DT_HOVER) {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        bool hover = wp != 0;
        if (hover != g_hoverTop) {
            g_hoverTop = hover;
            LARGE_INTEGER f, a, b;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&a);
            EvaluateStateNodes();
            Ui_StateApplied();
            QueryPerformanceCounter(&b);
            static int logged = 0;
            if (logged++ < 6)
                Log("hover %s applied in %.1f ms", hover ? "on" : "off",
                    (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart);
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Runs on every UI thread that produced tree events (WinUI pumps Win32 messages there).
static VOID CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    static thread_local int seenReload = 0;
    static thread_local int tick = 0;
    static std::atomic<int> reloadGen{0};
    if (g_reloadPending.exchange(false)) reloadGen++;
    bool reevaluate = false;
    if (seenReload != reloadGen) {
        seenReload = reloadGen;
        reevaluate = true;
    }
    bool stateChanged = Ui_Tick();
    g_watchHover = RulesUseState() || TweakOn(L"hideToolbar");
    if (reevaluate) EvaluateThread();
    else if (stateChanged) EvaluateStateNodes(), Ui_StateApplied();
    if (g_dumpPending.exchange(false)) {
        DoDump();
        HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, DT_EVENT_DONE);
        if (ev) SetEvent(ev), CloseHandle(ev);
    }
    if (++tick % 4 == 0 || reevaluate) Reassert();
}

static void EnsureTimerOnThisThread() {
    DWORD tid = GetCurrentThreadId();
    for (auto t : g_uiThreads)
        if (t == tid) return;
    g_uiThreads.push_back(tid);
    SetTimer(nullptr, 0, 100, TimerProc);
    if (!g_pingWnd) {
        // The first UI thread is the one that owns Dia's browser windows.
        WNDCLASSW wc{};
        wc.lpfnWndProc = PingProc;
        wc.hInstance = g_module;
        wc.lpszClassName = L"DiaTweaksPing";
        RegisterClassW(&wc);
        g_hoverThread = tid;
        g_pingWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_module, nullptr);
        CloseHandle(CreateThread(nullptr, 0, HoverWatcher, nullptr, 0, nullptr));
    }
    Log("timer installed on ui thread");
}

static DWORD WINAPI CommandThread(LPVOID) {
    HANDLE evReload = CreateEventW(nullptr, FALSE, FALSE, DT_EVENT_RELOAD);
    HANDLE evDump = CreateEventW(nullptr, FALSE, FALSE, DT_EVENT_DUMP);
    HANDLE evs[2] = {evReload, evDump};
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, evs, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) {
            LoadConfig();
            g_reloadPending = true;
        } else if (w == WAIT_OBJECT_0 + 1) {
            g_dumpPending = true;
        } else break;
    }
    return 0;
}

// ---------------------------------------------------------------- COM object

class Tap : public IObjectWithSite, public IVisualTreeServiceCallback2 {
    std::atomic<ULONG> m_ref{1};
    IUnknown* m_site = nullptr;

public:
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IObjectWithSite) *ppv = static_cast<IObjectWithSite*>(this);
        else if (riid == __uuidof(IVisualTreeServiceCallback) || riid == __uuidof(IVisualTreeServiceCallback2))
            *ppv = static_cast<IVisualTreeServiceCallback2*>(this);
        else return (*ppv = nullptr), E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (!r) delete this;
        return r;
    }

    // IObjectWithSite
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        if (m_site) m_site->Release();
        m_site = site;
        if (!site) return S_OK;
        site->AddRef();
        HRESULT hr = site->QueryInterface(__uuidof(IXamlDiagnostics), (void**)&g_diag);
        if (FAILED(hr)) return Log("no IXamlDiagnostics 0x%08lx", hr), hr;
        hr = site->QueryInterface(__uuidof(IVisualTreeService2), (void**)&g_vts);
        if (FAILED(hr)) return Log("no IVisualTreeService2 0x%08lx", hr), hr;
        Log("attached to Dia (pid %lu)", GetCurrentProcessId());
        // The crash report first: vectored handlers added later run earlier, so the crash guard still
        // takes the input hooks off before anything else happens on a crash.
        InstallCrashReport();
        InstallCrashGuard();
        // Tells diatweaks the tweaks are live. Kept open (and set) for as long as Dia runs.
        static HANDLE attached = CreateEventW(
            nullptr, TRUE, TRUE, (DT_EVENT_ATTACHED + std::to_wstring(GetCurrentProcessId())).c_str());
        CheckSafeMode();
        CloseHandle(CreateThread(nullptr, 0, StableTimer, nullptr, 0, nullptr));
        LoadConfig();
        ApplyPinnedExtensionsLimit();
        CloseHandle(CreateThread(nullptr, 0, CommandThread, nullptr, 0, nullptr));
        // Advise must not run on the thread that called SetSite (it would deadlock).
        AddRef();
        CloseHandle(CreateThread(
            nullptr, 0,
            [](LPVOID p) -> DWORD {
                auto self = static_cast<Tap*>(p);
                HRESULT hr = g_vts->AdviseVisualTreeChange(self);
                Log("AdviseVisualTreeChange 0x%08lx", hr);
                self->Release();
                return 0;
            },
            this, 0, nullptr));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppv) override {
        if (!m_site) return (*ppv = nullptr), E_FAIL;
        return m_site->QueryInterface(riid, ppv);
    }

    // IVisualTreeServiceCallback
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation rel, VisualElement el,
                                                 VisualMutationType mt) override {
        std::wstring type = B(el.Type), name = B(el.Name);
        SysFreeString(el.Type);
        SysFreeString(el.Name);
        SysFreeString(el.SrcInfo.FileName);
        SysFreeString(el.SrcInfo.Hash);

        std::lock_guard<std::recursive_mutex> lk(g_mx);
        EnsureTimerOnThisThread();
        if (mt == Add) {
            ++t_treeRevision;
            Node& n = g_nodes[el.Handle];
            n.parent = rel.Parent;
            n.childIndex = rel.ChildIndex;
            n.type = type;
            n.name = name;
            n.thread = GetCurrentThreadId();
            if (rel.Parent) {
                auto& ch = g_nodes[rel.Parent].children;
                if (std::find(ch.begin(), ch.end(), el.Handle) == ch.end()) ch.push_back(el.Handle);
            } else if (std::find(g_roots.begin(), g_roots.end(), el.Handle) == g_roots.end())
                g_roots.push_back(el.Handle);
            // Its children can be reported before it (e.g. the existing tree, walked when the engine
            // attaches to a running Dia). Rules that look at ancestors (upN=, parentName=, ancestor=...)
            // couldn't match them then, so evaluate them again now that this element is known.
            EvaluateSubtree(el.Handle);
            if (!name.empty()) Ui_ElementAdded(name);
        } else {
            ++t_treeRevision;
            auto it = g_nodes.find(el.Handle);
            if (it != g_nodes.end()) {
                auto p = g_nodes.find(it->second.parent);
                if (p != g_nodes.end()) {
                    auto& ch = p->second.children;
                    ch.erase(std::remove(ch.begin(), ch.end(), el.Handle), ch.end());
                }
                // keep the node record for children that might be re-parented; drop links
                g_nodes.erase(it);
            }
            g_roots.erase(std::remove(g_roots.begin(), g_roots.end(), el.Handle), g_roots.end());
            // Leaving the tree (e.g. moved elsewhere by the Arc bar): undo our overrides; if it comes back
            // somewhere a rule matches, Evaluate applies them again.
            Restore(el.Handle);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) override {
        return S_OK;
    }
};

// ---------------------------------------------------------------- engine API (see engine.h)

void Engine_Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("%s", buf);
}
std::wstring Engine_Dir() { return g_dir; }

std::vector<TweakInfo> Engine_Tweaks() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    return g_tweaks;
}
bool Engine_TweakOn(const std::wstring& key) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    return TweakOn(key);
}
bool Engine_NeedsRestart(const std::wstring& key) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto start = g_startStates.find(key);
    if (start == g_startStates.end()) return false;
    for (auto& t : g_tweaks)
        if (t.key == key) return (t.enabled ? t.value : L"off") != start->second;
    return false;
}
std::wstring Engine_TweakValue(const std::wstring& key) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    for (auto& t : g_tweaks)
        if (t.key == key) return t.value;
    return {};
}

static std::vector<std::string> ReadConfLines() {
    std::vector<std::string> lines;
    std::ifstream in(g_dir + L"\\tweaks.conf");
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    return lines;
}
static void WriteConfLines(const std::vector<std::string>& lines) {
    std::ofstream out(g_dir + L"\\tweaks.conf", std::ios::binary);
    for (auto& l : lines) out << l << "\n";
}
static bool IsHeader(const std::string& l, std::string* key) {
    size_t a = l.find_first_not_of(" \t");
    if (a == std::string::npos || l[a] != '[') return false;
    size_t e = l.find(']', a);
    if (e == std::string::npos) return false;
    if (key) *key = l.substr(a + 1, e - a - 1);
    return true;
}

static void ApplyConfigNow() {
    LoadConfig();
    g_reloadPending = true;
}

static void SetInLines(std::vector<std::string>& lines, const std::wstring& key, const bool* onp, const std::wstring* value) {
    std::string k8 = Wide2Narrow(key);
    for (size_t i = 0; i < lines.size(); i++) {
        std::string k;
        if (!IsHeader(lines[i], &k) || k != k8) continue;
        if (onp) lines[i] = "[" + k8 + "] " + (*onp ? "on" : "off");
        if (value) {
            size_t j = i + 1, insertAt = i + 1;
            bool done = false;
            for (; j < lines.size() && !IsHeader(lines[j], nullptr); j++) {
                size_t a = lines[j].find_first_not_of(" \t");
                if (a == std::string::npos) continue;
                if (lines[j].compare(a, 6, "value:") == 0) {
                    lines[j] = "value: " + Wide2Narrow(*value);
                    done = true;
                    break;
                }
                if (lines[j].compare(a, 6, "title:") == 0) insertAt = j + 1;
            }
            if (!done) lines.insert(lines.begin() + insertAt, "value: " + Wide2Narrow(*value));
        }
        break;
    }
}

// Starts a program as Explorer's child rather than ours (through the desktop's shell view), so it runs
// outside Dia's package: it outlives Dia, and what it writes, like a sign-in entry, isn't kept inside
// the package.
static bool RunViaExplorer(const std::wstring& exe, const std::wstring& args) {
    bool ok = false;
    IShellWindows* windows = nullptr;
    IDispatch *desktop = nullptr, *viewDisp = nullptr, *app = nullptr;
    IServiceProvider* sp = nullptr;
    IShellBrowser* browser = nullptr;
    IShellView* view = nullptr;
    IShellFolderViewDual* folderView = nullptr;
    IShellDispatch2* shell = nullptr;
    VARIANT loc, empty;
    VariantInit(&loc);
    VariantInit(&empty);
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;
    long hwnd = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))) &&
        windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop) == S_OK &&
        SUCCEEDED(desktop->QueryInterface(IID_PPV_ARGS(&sp))) &&
        SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) &&
        SUCCEEDED(browser->QueryActiveShellView(&view)) &&
        SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&viewDisp))) &&
        SUCCEEDED(viewDisp->QueryInterface(IID_PPV_ARGS(&folderView))) && SUCCEEDED(folderView->get_Application(&app)) &&
        SUCCEEDED(app->QueryInterface(IID_PPV_ARGS(&shell)))) {
        BSTR file = SysAllocString(exe.c_str());
        VARIANT vArgs, vDir, vOp, vShow;
        VariantInit(&vArgs);
        VariantInit(&vDir);
        VariantInit(&vOp);
        VariantInit(&vShow);
        vArgs.vt = VT_BSTR;
        vArgs.bstrVal = SysAllocString(args.c_str());
        vDir.vt = VT_BSTR;
        vDir.bstrVal = SysAllocString(g_dir.c_str());
        vShow.vt = VT_I4;
        vShow.lVal = SW_SHOWNORMAL;
        ok = SUCCEEDED(shell->ShellExecute(file, vArgs, vDir, vOp, vShow));
        SysFreeString(file);
        VariantClear(&vArgs);
        VariantClear(&vDir);
    }
    for (IUnknown* p : std::initializer_list<IUnknown*>{shell, app, folderView, viewDisp, view, browser, sp, desktop, windows})
        if (p) p->Release();
    return ok;
}

// "Patch Dia however it's opened" switched on in Settings: start its helper (it adds itself to
// sign-in). Switched off, the helper sees the setting change, removes its sign-in entry and stops.
static void StartWatcher() {
    std::thread([] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::wstring exe = g_dir + L"\\DiaLauncher.exe";
        bool ok = RunViaExplorer(exe, L"--watch");
        Log("normal launches: helper %s", ok ? "started" : "couldn't be started through Explorer");
        CoUninitialize();
    }).detach();
}

void Engine_SetTweak(const std::wstring& key, bool on, const std::wstring* value) {
    auto lines = ReadConfLines();
    SetInLines(lines, key, &on, value);
    // Changing one patch by hand leaves the preset: it becomes Custom.
    static const std::wstring custom = L"custom";
    if (key != L"preset" && key != L"checkForUpdates" && key != L"normalLaunch")
        SetInLines(lines, L"preset", nullptr, &custom);
    WriteConfLines(lines);
    ApplyConfigNow();
    if (key == L"normalLaunch" && on) StartWatcher();
}

void Engine_SetMany(const std::vector<std::pair<std::wstring, bool>>& states,
                    const std::vector<std::pair<std::wstring, std::wstring>>& values) {
    auto lines = ReadConfLines();
    for (auto& [k, on] : states) SetInLines(lines, k, &on, nullptr);
    for (auto& [k, v] : values) SetInLines(lines, k, nullptr, &v);
    WriteConfLines(lines);
    ApplyConfigNow();
}

void Engine_SetAll(bool on) {
    auto lines = ReadConfLines();
    for (auto& l : lines) {
        std::string k;
        if (IsHeader(l, &k)) l = "[" + k + "] " + (on ? "on" : "off");
    }
    WriteConfLines(lines);
    ApplyConfigNow();
}

bool Engine_SafeMode() { return g_safeMode; }
bool Engine_Hover() { return g_hoverTop; }
void Engine_LeaveSafeMode() {
    g_safeMode = false;
    DeleteFileW((g_dir + L"\\launch.state").c_str());
    DeleteFileW((g_dir + L"\\crash.state").c_str());
    Log("safe mode off");
    ApplyConfigNow();
}

unsigned long long Engine_TreeRevision() { return t_treeRevision; }

void Engine_ForEachNode(const std::function<void(const NodeRef&)>& fn) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    DWORD tid = GetCurrentThreadId();
    std::vector<NodeRef> list;
    for (auto& [h, n] : g_nodes)
        if (n.thread == tid) list.push_back({h, n.type, n.name});
    for (auto& r : list) fn(r);
}
InstanceHandle Engine_Parent(InstanceHandle h) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto it = g_nodes.find(h);
    return it == g_nodes.end() ? 0 : it->second.parent;
}
std::wstring Engine_Name(InstanceHandle h) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto it = g_nodes.find(h);
    return it == g_nodes.end() ? std::wstring() : it->second.name;
}
std::wstring Engine_Type(InstanceHandle h) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto it = g_nodes.find(h);
    return it == g_nodes.end() ? std::wstring() : it->second.type;
}
IInspectable* Engine_Object(InstanceHandle h) {
    if (!g_diag || !h) return nullptr;
    IInspectable* out = nullptr;
    if (FAILED(g_diag->GetIInspectableFromHandle(h, &out))) return nullptr;
    return out;
}

// ---------------------------------------------------------------- crash guard
// When Dia crashes, Swift's crash reporter (swift-backtrace) holds Dia stopped while it prints a report
// and waits for a key. Our low-level input hooks would then stall every mouse move and key press on the
// PC for as long (zioder/WinDiaTweaks#58). So on a crash the hooks come off first, before the reporter
// gets to run, and the crash is noted in tap.log (where it happened helps tell Dia's crashes from ours).
// Nothing here takes a lock or allocates: the crashed thread may hold any of them.
static wchar_t g_crashLogPath[MAX_PATH];
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevCrashFilter;
static std::atomic<bool> g_crashed{false};

static void OnCrash(EXCEPTION_POINTERS* ep) {
    if (g_crashed.exchange(true)) return;
    int released = LowLevelHookThread::ReleaseAll();
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    char where[MAX_PATH] = "?";
    uintptr_t offset = (uintptr_t)er->ExceptionAddress;
    // (On a stack overflow there's little stack left: just the code and address.)
    HMODULE mod = nullptr;
    if (er->ExceptionCode != EXCEPTION_STACK_OVERFLOW &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)er->ExceptionAddress, &mod) &&
        GetModuleFileNameA(mod, where, sizeof(where))) {
        offset -= (uintptr_t)mod;
        if (char* slash = strrchr(where, '\\')) memmove(where, slash + 1, strlen(slash));
    }
    char line[MAX_PATH + 160];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "%02d:%02d:%02d.%03d [%lu] Dia crashed: exception 0x%08lx at %s+0x%llx (%d input hooks "
                        "taken off)\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(),
                        er->ExceptionCode, where, (unsigned long long)offset, released);
    HANDLE f = CreateFileW(g_crashLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written;
        if (n > 0) WriteFile(f, line, (DWORD)n, &written, nullptr);
        CloseHandle(f);
    }
}
// First in line for the exceptions that only ever end a program: a Swift runtime error (fatalError,
// an index out of range, unwrapping nil...) is an illegal instruction.
static LONG CALLBACK CrashFirstChance(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW) OnCrash(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}
// Any other exception nothing handled, before passing it on to the handler that was there (Swift's).
static LONG WINAPI CrashUnhandled(EXCEPTION_POINTERS* ep) {
    OnCrash(ep);
    return g_prevCrashFilter ? g_prevCrashFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}
static void InstallCrashGuard() {
    static bool done = false;
    if (done) return;
    done = true;
    wcsncpy_s(g_crashLogPath, (g_dir + L"\\tap.log").c_str(), _TRUNCATE);
    AddVectoredExceptionHandler(1, CrashFirstChance);
    g_prevCrashFilter = SetUnhandledExceptionFilter(CrashUnhandled);
}

// ---------------------------------------------------------------- safe mode bookkeeping

// launch.state holds how many tweaked starts in a row ended within a minute (a clean exit or a full
// minute of running clears it). Two such starts in a row turn on safe mode for the next one.
// crash.state holds how many sessions in a row ended in a crash the add-on saw (see the crash report
// below), however long Dia had been running. Only a clean exit clears it. Two in a row also turn on
// safe mode, so a crash minutes in (#63) can't repeat forever.
static int ReadCount(const std::wstring& path) {
    int n = 0;
    std::ifstream in(path);
    in >> n;
    return n;
}
static void CheckSafeMode() {
    std::wstring path = g_dir + L"\\launch.state";
    int previous = ReadCount(path);
    int crashes = ReadCount(g_dir + L"\\crash.state");
    g_safeMode = previous >= 2 || crashes >= 2;
    std::ofstream out(path, std::ios::binary);
    out << (previous + 1);
    if (previous >= 2) Log("safe mode: %d quick exits in a row, tweaks are off", previous);
    else if (g_safeMode) Log("safe mode: Dia crashed %d times in a row (see crash.log), tweaks are off", crashes);
}

// ---------------------------------------------------------------- crash report
// Dia's Swift runtime catches its own crashes and hands them to swift-backtrace.exe, which (at least on
// some Windows builds) dies itself before writing anything, and Windows logs no crash for Dia (#63). So
// the add-on writes what it can see to crash.log: Swift's "Fatal error" line (the runtime writes it to
// standard error, which a windowed app doesn't have, so it's pointed at crash.log) and, for a crash, the
// code, the stack (module+offset, with the nearest exported name: Swift's exported symbols name the
// function) and which tweaks were on. Nothing is changed about how the crash then proceeds.

static HANDLE g_crashFile = INVALID_HANDLE_VALUE;
static char g_crashTweaks[1024] = "";  // enabled tweaks, kept ready (the handler can't take locks)
static std::atomic<int> g_crashReports{0}, g_avReports{0};

static void NoteTweaksForCrashReport() {
    std::string s;
    for (auto& t : g_tweaks)
        if (t.enabled && t.key != L"preset" && !t.noSwitch) {
            if (!s.empty()) s += ' ';
            s += Wide2Narrow(t.key);
            if (!t.value.empty()) s += "=" + Wide2Narrow(t.value);
        }
    if (g_safeMode) s = "(safe mode) " + s;
    strncpy_s(g_crashTweaks, s.c_str(), _TRUNCATE);
}

static void CrashWrite(const char* s) {
    DWORD n;
    if (g_crashFile != INVALID_HANDLE_VALUE) WriteFile(g_crashFile, s, (DWORD)strlen(s), &n, nullptr);
}

// "Dia.dll+0x1234 ($s3Dia7TabViewC6updateyyF+0x40)". Reads only the module's own headers.
static void DescribeAddress(DWORD64 addr, char* out, size_t cap) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &m) || !m) {
        snprintf(out, cap, "0x%llx", (unsigned long long)addr);
        return;
    }
    char path[MAX_PATH] = "";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    BYTE* img = (BYTE*)m;
    DWORD64 rva = addr - (DWORD64)img;
    const char* best = nullptr;
    DWORD bestRva = 0;
    auto nt = (IMAGE_NT_HEADERS*)(img + ((IMAGE_DOS_HEADER*)img)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress && dir.Size) {
        auto ex = (IMAGE_EXPORT_DIRECTORY*)(img + dir.VirtualAddress);
        auto names = (DWORD*)(img + ex->AddressOfNames);
        auto ords = (WORD*)(img + ex->AddressOfNameOrdinals);
        auto funcs = (DWORD*)(img + ex->AddressOfFunctions);
        for (DWORD i = 0; i < ex->NumberOfNames; i++) {
            DWORD f = funcs[ords[i]];
            if (f <= rva && f > bestRva) bestRva = f, best = (const char*)(img + names[i]);
        }
    }
    if (best) snprintf(out, cap, "%s+0x%llx (%.400s+0x%llx)", base, (unsigned long long)rva, best,
                       (unsigned long long)(rva - bestRva));
    else snprintf(out, cap, "%s+0x%llx", base, (unsigned long long)rva);
}

static bool ReadReturnAddress(DWORD64 rsp, DWORD64* out) {
    __try {
        *out = *(DWORD64*)rsp;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // A Swift trap (fatalError, a failed `try!`, a nil unwrap, an index out of range) is an illegal
    // instruction; Chromium's CHECKs are a breakpoint or an illegal instruction. Access violations can be
    // handled further on, so only the first few are written (as "maybe").
    bool fatal = code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_BREAKPOINT;
    if (!fatal && code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (g_crashFile == INVALID_HANDLE_VALUE || (fatal ? g_crashReports : g_avReports).fetch_add(1) >= 3)
        return EXCEPTION_CONTINUE_SEARCH;
    if (fatal) {
        // Counted now: the process won't get to say anything after this.
        int n = ReadCount(g_dir + L"\\crash.state");
        HANDLE f = CreateFileW((g_dir + L"\\crash.state").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char b[16];
            snprintf(b, sizeof(b), "%d", n + 1);
            DWORD w;
            WriteFile(f, b, (DWORD)strlen(b), &w, nullptr);
            CloseHandle(f);
        }
    }
    char line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(line, sizeof(line),
             "\r\n*** %04d-%02d-%02d %02d:%02d:%02d.%03d %s 0x%08lx on thread %lu (Dia pid %lu)\r\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             fatal ? "crash" : "maybe a crash: access violation", code, GetCurrentThreadId(), GetCurrentProcessId());
    CrashWrite(line);
    snprintf(line, sizeof(line), "tweaks on: %s\r\n", g_crashTweaks[0] ? g_crashTweaks : "(none)");
    CrashWrite(line);
    CONTEXT ctx = *ep->ContextRecord;
    char where[700];
    for (int i = 0; i < 64 && ctx.Rip; i++) {
        DescribeAddress(ctx.Rip, where, sizeof(where));
        snprintf(line, sizeof(line), "  %2d %s\r\n", i, where);
        CrashWrite(line);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (fn) {
            PVOID handlerData;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        } else {  // a leaf function: the return address is on top of the stack
            DWORD64 ret;
            if (!ReadReturnAddress(ctx.Rsp, &ret)) break;
            ctx.Rip = ret;
            ctx.Rsp += 8;
        }
    }
    FlushFileBuffers(g_crashFile);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void InstallCrashReport() {
    std::wstring path = g_dir + L"\\crash.log";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 512 * 1024)
        MoveFileExW(path.c_str(), (path + L".prev").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_crashFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_crashFile == INVALID_HANDLE_VALUE) return Log("crash report: can't open crash.log (%lu)", GetLastError());
    AddVectoredExceptionHandler(1, CrashHandler);
    // Point the C runtime's standard error (the one Swift's runtime writes its fatal errors to) at
    // crash.log, unless Dia was started with somewhere to write it already.
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    auto openHandle = ucrt ? (int(__cdecl*)(intptr_t, int))GetProcAddress(ucrt, "_open_osfhandle") : nullptr;
    auto dup2 = ucrt ? (int(__cdecl*)(int, int))GetProcAddress(ucrt, "_dup2") : nullptr;
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    bool hasStderr = err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN;
    if (openHandle && dup2 && !hasStderr) {
        HANDLE dup = nullptr;
        DuplicateHandle(GetCurrentProcess(), g_crashFile, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
        int fd = dup ? openHandle((intptr_t)dup, 0x0008 /* _O_APPEND */) : -1;
        if (fd >= 0 && dup2(fd, 2) == 0) Log("crash report: on (standard error goes to crash.log)");
        else Log("crash report: on (standard error not redirected)");
    } else {
        Log("crash report: on");
    }
}

static void ClearCrashCount() { DeleteFileW((g_dir + L"\\crash.state").c_str()); }
static DWORD WINAPI StableTimer(LPVOID) {
    Sleep(60000);
    if (!g_safeMode) DeleteFileW((g_dir + L"\\launch.state").c_str());
    return 0;
}

// ---------------------------------------------------------------- class factory / exports

class Factory : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) return (*ppv = this), S_OK;
        return (*ppv = nullptr), E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        Tap* t = new Tap();
        HRESULT hr = t->QueryInterface(riid, ppv);
        t->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
static Factory g_factory;

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_DiaTweaksTap) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}
extern "C" HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }

typedef HRESULT(WINAPI* PFN_InitXamlDiag)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);

// Once this DLL is running inside Dia, Microsoft.ui.xaml.dll is already loaded, so the sanctioned
// InitializeXamlDiagnosticsEx is live in THIS process. Call it with our own pid to have WinUI create
// the Tap (via DllGetClassObject) and hand it IXamlDiagnostics. This is the in-process entry the API
// is designed for; it just needs code running in Dia, which the one-time loader provides.
static bool TryAttach() {
    // diatweaks loads us the moment Dia starts, before WinUI: poll often, so we attach before Dia's
    // window first shows and its UI comes up already tweaked.
    HMODULE udk = nullptr;
    ULONGLONG end = GetTickCount64() + 40000;
    while (!udk && GetTickCount64() < end) {
        udk = GetModuleHandleW(L"Microsoft.Internal.FrameworkUdk.dll");
        if (!udk) Sleep(10);
    }
    if (!udk) return Log("FrameworkUdk not present in this process"), false;
    auto init = (PFN_InitXamlDiag)GetProcAddress(udk, "InitializeXamlDiagnosticsEx");
    if (!init) return Log("InitializeXamlDiagnosticsEx not found"), false;
    // Wait for the WinUI framework dll to be loaded in this process.
    HMODULE xaml = nullptr;
    end = GetTickCount64() + 40000;
    while (!xaml && GetTickCount64() < end) {
        xaml = GetModuleHandleW(L"Microsoft.ui.xaml.dll");
        if (!xaml) Sleep(10);
    }
    if (!xaml) return Log("Microsoft.ui.xaml.dll not present in this process"), false;

    // arg3 (wszDllXamlDiagnostics) must point at the loaded WinUI framework dll so the diagnostics
    // code knows which XAML runtime to attach to. Passing L"" is why it returned NOT_FOUND.
    wchar_t xamlPath[MAX_PATH];
    GetModuleFileNameW(xaml, xamlPath, MAX_PATH);
    wchar_t tapPath[MAX_PATH];
    GetModuleFileNameW(g_module, tapPath, MAX_PATH);
    Log("xaml dll: %ls", xamlPath);
    Log("tap dll:  %ls", tapPath);

    // Microsoft.ui.xaml.dll registers its diagnostics port under "WinUIVisualDiagConnection" plus a
    // counter (it formats L"%s%d"), so the live endpoint is usually "WinUIVisualDiagConnection0".
    // "VisualDiagConnection1" is the old UWP (Windows.UI.Xaml) name.
    const wchar_t* kDiagDlls[] = {xamlPath, L"Microsoft.ui.xaml.dll"};

    // Early in Dia's start the endpoint may not answer yet: keep probing until it does.
    HRESULT hr = E_FAIL;
    end = GetTickCount64() + 30000;
    for (int round = 0; GetTickCount64() < end; round++) {
        for (int n = -1; n < 8; n++) {
            std::wstring ep = L"WinUIVisualDiagConnection";
            if (n >= 0) ep += std::to_wstring(n);
            for (auto dll : kDiagDlls) {
                if (g_diag) return Log("already attached via SetSite; stopping probe"), true;
                hr = init(ep.c_str(), GetCurrentProcessId(), dll, tapPath, CLSID_DiaTweaksTap, L"");
                if (!round) Log("probe ep=%ls diagDll=%ls -> 0x%08lx", ep.c_str(), dll, hr);
                if (SUCCEEDED(hr)) return Log("attached via ep=%ls (probe round %d)", ep.c_str(), round), true;
            }
        }
        Sleep(20);
    }
    Log("in-process InitializeXamlDiagnosticsEx failed everywhere, last 0x%08lx", hr);
    return false;
}

// Attaches, and if that fails, waits for diatweaks to ask for another try (it does when it finds us
// loaded but not attached) instead of leaving Dia untweaked for good.
// ---------------------------------------------------------------- pinned extensions shown
// Dia's toolbar shows at most 4 pinned extensions: PinnedExtensionsViewModel takes min(count, 4) in two
// places and shows the overflow button from 5. The pinnedExtensions tweak raises those numbers in
// Dia's code in memory (Dia's files aren't touched), once, when the add-on attaches at startup.
// The code is found by its bytes; if a Dia update changes it, nothing is patched and that's logged.
static bool PatchBytes(uint8_t* at, const uint8_t* want, size_t len) {
    DWORD old;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, want, len);
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    return true;
}

// Finds the one place `pat` occurs in [lo, hi), or nullptr if it's missing or occurs more than once.
static uint8_t* FindOnce(uint8_t* lo, uint8_t* hi, const uint8_t* pat, size_t len) {
    uint8_t* p = std::search(lo, hi, pat, pat + len);
    if (p == hi || std::search(p + 1, hi, pat, pat + len) != hi) return nullptr;
    return p;
}

static void ApplyPinnedExtensionsLimit() {
    if (!Engine_TweakOn(L"pinnedExtensions")) return;
    int cap = _wtoi(Engine_TweakValue(L"pinnedExtensions").c_str());
    if (cap <= 4 || cap > 100) return Log("pinned extensions: %d ignored", cap);
    auto dia = (uint8_t*)GetModuleHandleW(L"Dia.dll");
    if (!dia) return Log("pinned extensions: Dia.dll not loaded");
    auto nt = (IMAGE_NT_HEADERS*)(dia + ((IMAGE_DOS_HEADER*)dia)->e_lfanew);
    // cmp rsi,4 / mov r14d,4 / cmovb r14,rsi   (then cmp rsi,5 for the overflow button)
    static const uint8_t a[] = {0x48, 0x83, 0xfe, 0x04, 0x41, 0xbe, 0x04, 0x00, 0x00, 0x00, 0x4c, 0x0f, 0x42, 0xf6};
    // cmp r15,4 / mov edx,4 / cmovb rdx,r15
    static const uint8_t b[] = {0x49, 0x83, 0xff, 0x04, 0xba, 0x04, 0x00, 0x00, 0x00, 0x49, 0x0f, 0x42, 0xd7};
    static const uint8_t c[] = {0x48, 0x83, 0xfe, 0x05};
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (memcmp(sec->Name, ".text", 6)) continue;
        uint8_t *lo = dia + sec->VirtualAddress, *hi = lo + sec->Misc.VirtualSize;
        uint8_t* pa = FindOnce(lo, hi, a, sizeof(a));
        uint8_t* pb = FindOnce(lo, hi, b, sizeof(b));
        uint8_t* pc = pa ? FindOnce(pa, pa + 64, c, sizeof(c)) : nullptr;
        if (!pa || !pb || !pc) return Log("pinned extensions: Dia's code not found (Dia changed?), not patched");
        uint8_t na[sizeof(a)], nb[sizeof(b)], nc[sizeof(c)];
        memcpy(na, a, sizeof(a)), memcpy(nb, b, sizeof(b)), memcpy(nc, c, sizeof(c));
        na[3] = na[6] = (uint8_t)cap;
        nb[3] = nb[5] = (uint8_t)cap;
        nc[3] = (uint8_t)(cap + 1);
        bool ok = PatchBytes(pa, na, sizeof(na)) && PatchBytes(pb, nb, sizeof(nb)) && PatchBytes(pc, nc, sizeof(nc));
        Log("pinned extensions: up to %d (%s)", cap, ok ? "patched" : "patch FAILED");
        return;
    }
}

static DWORD WINAPI Bootstrap(LPVOID) {
    HANDLE retry =
        CreateEventW(nullptr, FALSE, FALSE, (DT_EVENT_RETRY + std::to_wstring(GetCurrentProcessId())).c_str());
    for (;;) {
        if (TryAttach()) {
            // WinUI accepted us; it hands over the UI (SetSite) on its own thread shortly after.
            for (ULONGLONG end = GetTickCount64() + 5000; !g_diag && GetTickCount64() < end;) Sleep(10);
            if (g_diag) return 0;
            Log("diagnostics answered but never handed over Dia's UI");
        }
        if (!retry || g_diag) return 0;
        Log("waiting for diatweaks to ask for another try");
        WaitForSingleObject(retry, INFINITE);
        if (g_diag) return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH && g_diag && !g_safeMode) {
        // Dia is closing normally: not a crash.
        DeleteFileW((g_dir + L"\\launch.state").c_str());
        ClearCrashCount();
    }
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = m;
        DisableThreadLibraryCalls(m);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(m, path, MAX_PATH);
        g_dir = path;
        g_dir = g_dir.substr(0, g_dir.find_last_of(L'\\'));
        // If WinUI loaded us via XAML_DM_* env vars, SetSite is called directly and this no-ops.
        // If a one-time loader put us here, this kicks off the sanctioned in-process attach.
        CloseHandle(CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr));
    }
    return TRUE;
}
