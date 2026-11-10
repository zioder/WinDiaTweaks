#pragma once
// Shared between tap.cpp (tree tracking + property rules) and ui.cpp (the Patches page and tweaks that
// need real code). Everything here is called on one of Dia's UI threads unless noted.

#include <windows.h>
#include <xamlOM.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

struct TweakInfo {
    std::wstring key, title, desc, note, section, icon, value;  // note: a caution shown under desc
    std::vector<std::pair<std::wstring, std::wstring>> choices;  // value -> label
    bool enabled = false;
    bool noSwitch = false;  // "switch: none": its drop-down only (an option of another tweak)
    bool restart = false;   // "restart: yes": takes effect when Dia next opens (Settings offers to reopen it)
};

// ---- provided by tap.cpp
void Engine_Log(const char* fmt, ...);
std::wstring Engine_Dir();
std::vector<TweakInfo> Engine_Tweaks();
bool Engine_TweakOn(const std::wstring& key);
std::wstring Engine_TweakValue(const std::wstring& key);
// For a "restart: yes" tweak: true while its setting differs from the one Dia was started with.
bool Engine_NeedsRestart(const std::wstring& key);
// Writes tweaks.conf and applies it right away. value may be null (leave as is).
void Engine_SetTweak(const std::wstring& key, bool on, const std::wstring* value);
void Engine_SetAll(bool on);
// Several tweaks in one write (presets): on/off states and values.
void Engine_SetMany(const std::vector<std::pair<std::wstring, bool>>& states,
                    const std::vector<std::pair<std::wstring, std::wstring>>& values);
bool Engine_SafeMode();
bool Engine_Hover();  // pointer at the top edge (reveal toolbar on hover)
void Engine_LeaveSafeMode();
// Live elements on the calling UI thread.
struct NodeRef {
    InstanceHandle handle;
    std::wstring type, name;
};
void Engine_ForEachNode(const std::function<void(const NodeRef&)>& fn);
// Changes when elements enter/leave the calling UI thread's tree (for weak discovery caches).
unsigned long long Engine_TreeRevision();
InstanceHandle Engine_Parent(InstanceHandle h);
std::wstring Engine_Name(InstanceHandle h);
std::wstring Engine_Type(InstanceHandle h);
IInspectable* Engine_Object(InstanceHandle h);  // AddRef'd, or null
// Dia changed a property that a tweak overrides; newText is Dia's new value as text.
void Engine_OnDiaChanged(InstanceHandle h, const std::wstring& prop, const std::wstring& newText);

// ---- provided by ui.cpp
// Runs every timer tick on each UI thread. Returns true when a state used by rules changed.
bool Ui_Tick();
// Window states rules can depend on (state=<name>): sidebarOpen, newTab.
bool Ui_State(const std::wstring& name);
// Called right after rules that depend on a state were re-applied (e.g. the toolbar was just shown).
void Ui_StateApplied();
// Start watching a property the engine overrides, to catch Dia changing it back.
void Ui_WatchProperty(InstanceHandle h, const std::wstring& prop);
// A named element just entered Dia's UI tree (called from the tree-change notification).
void Ui_ElementAdded(const std::wstring& name);
// Opens a real new tab and goes to the text there (an address or a search), as the Ctrl+T bar does.
void Ui_OpenInNewTab(const std::wstring& text);

// The address Dia shows at the bottom of a window while the pointer is over a link, or empty.
std::wstring Ui_HoveredLink(HWND top);

// Peek's preview window (or null): the toolbar-hiding tweaks leave its toolbar alone.
void Ui_SetPeekWindow(HWND w);
// Peek's preview window: no resizing, maximizing or minimizing (false until WinUI has its AppWindow).
bool Ui_PeekFixedSize(HWND w);

// Peek "open as tab": where a window's tab row is (screen pixels), and, with its tab menu open, Dia's
// "Move to Window" to the window `to` (1 done, 0 menu not open yet, -1 can't).
bool Ui_PeekTabRow(HWND win, RECT& out);
std::wstring Ui_PeekTabInfo(HWND win);  // (for the log: the window's title and its tab list's labels)
std::wstring Ui_PeekMenuInfo(HWND win);  // (for the log: the menus open in the window, and their items)
int Ui_PeekMoveTab(HWND win, HWND to);
// Peek's split: `win`'s tab titled `title` opened as a split beside the tab it's on (1 done, 0 not yet, -1 can't).
int Ui_PeekSplitWith(HWND win, const std::wstring& title);
bool Ui_PeekTabRowNamed(HWND win, const std::wstring& title, RECT& out);
bool Ui_PeekShowTabMenu(HWND window);
bool Ui_PeekMenuOpen(HWND win);  // a menu (or any popup) open on the preview's window
void Ui_PeekCloseTabMenu(HWND window);
void Peek_BeforeMove();  // (peek.cpp: just before the tab moves)

// ---- provided by history.cpp (Hide history suggestions)
// Runs every timer tick on each UI thread.
void History_Tick();

// ---- provided by media.cpp (Media Controls: a card for tabs playing in the background)
// Runs every timer tick on each UI thread.
void Media_Tick();

// ---- provided by peek.cpp (the Peek tweak: hold on a link to preview it)
// Runs every timer tick on each UI thread.
void Peek_Tick();
