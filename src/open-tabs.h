// Included by ui.cpp after SendKeys. Uses live sidebar and address-bar UI, not browser storage.
#pragma once

namespace open_tabs {
struct Match {
    winrt::weak_ref<mux::FrameworkElement> target;
    std::wstring title, text;
};
static thread_local std::vector<Match> matches;
static thread_local muxc::StackPanel header{nullptr};
static thread_local winrt::weak_ref<muxc::ListView> list;
static thread_local winrt::weak_ref<muxc::ItemsPresenter> presenter;
static thread_local std::wstring query;
static thread_local int selected = -1;
static thread_local bool navigating = false;
static thread_local bool revealPending = false;
static thread_local winrt::weak_ref<mux::FrameworkElement> pending;
static thread_local int pendingFrames = 0;
static thread_local bool pendingRevealRequested = false;
static thread_local bool pendingScrollRequested = false;
static thread_local int pendingSidebarWait = 0;
static thread_local mux::XamlRoot restoreSidebarRoot{nullptr};
static thread_local winrt::weak_ref<mux::FrameworkElement> restoreSidebarContainer;
static thread_local double restoreSidebarOpacity = 1;
static thread_local int restoreSidebarFrames = 0;

static bool IsTemporarilyRevealed(const mux::XamlRoot& root) {
    return restoreSidebarRoot == root;
}
static thread_local std::vector<std::pair<winrt::weak_ref<mux::UIElement>, double>> nativeHighlights;

static void RestoreNativeHighlights() {
    for (auto& [weak, opacity] : nativeHighlights)
        if (auto element = weak.get()) element.Opacity(opacity);
    nativeHighlights.clear();
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static std::wstring Text(const wf::IInspectable& value) {
    if (!value) return {};
    if (auto s = value.try_as<wf::IPropertyValue>(); s && s.Type() == wf::PropertyType::String)
        return std::wstring(s.GetString());
    if (auto t = value.try_as<muxc::TextBlock>()) return std::wstring(t.Text());
    if (auto t = value.try_as<muxc::ToolTip>()) return Text(t.Content());
    if (auto t = value.try_as<muxc::UserControl>()) return Text(t.Content());
    if (auto t = value.try_as<muxc::ContentControl>()) return Text(t.Content());
    if (auto t = value.try_as<muxc::ContentPresenter>()) return Text(t.Content());
    if (auto t = value.try_as<muxc::Border>()) return Text(t.Child());
    if (auto t = value.try_as<muxc::Panel>()) {
        std::wstring result;
        for (auto c : t.Children()) {
            auto text = Text(c);
            if (!text.empty()) result += (result.empty() ? L"" : L" ") + text;
        }
        return result;
    }
    return {};
}

static bool Under(mux::DependencyObject child, const mux::DependencyObject& parent) {
    for (; child; child = muxm::VisualTreeHelper::GetParent(child))
        if (child == parent) return true;
    return false;
}

static bool SidebarVisible(const mux::XamlRoot& root) {
    for (auto h : FindNodes(L"SidebarBorder")) {
        auto sidebar = Obj<mux::FrameworkElement>(h);
        if (sidebar && sidebar.XamlRoot() == root && ShownOnScreen(sidebar) &&
            sidebar.ActualWidth() > 0 && sidebar.ActualHeight() > 0) return true;
    }
    return false;
}

static void RestoreHiddenSidebar() {
    auto container = restoreSidebarContainer.get();
    restoreSidebarContainer = nullptr;
    restoreSidebarRoot = nullptr;
    restoreSidebarFrames = 0;
    if (container) {
        container.Visibility(mux::Visibility::Collapsed);
        container.Opacity(restoreSidebarOpacity);
    }
}

static bool RevealHiddenSidebar(const mux::XamlRoot& root) {
    if (restoreSidebarRoot) return restoreSidebarRoot == root;
    for (auto h : FindNodes(L"SidebarBorder")) {
        auto sidebar = Obj<mux::FrameworkElement>(h);
        if (!sidebar || sidebar.XamlRoot() != root) continue;
        for (auto d = muxm::VisualTreeHelper::GetParent(sidebar); d; d = muxm::VisualTreeHelper::GetParent(d)) {
            auto fe = d.try_as<mux::FrameworkElement>();
            if (!fe || fe.Visibility() != mux::Visibility::Collapsed) continue;
            restoreSidebarContainer = winrt::make_weak(fe);
            restoreSidebarOpacity = fe.Opacity();
            restoreSidebarRoot = root;
            try {
                // Keep native sidebar state and page bounds unchanged. Opacity does not disable hit testing.
                fe.Opacity(0);
                fe.Visibility(mux::Visibility::Visible);
                fe.UpdateLayout();
            } catch (...) {
                RestoreHiddenSidebar();
                throw;
            }
            return true;
        }
    }
    return false;
}

static void RestoreSidebar() {
    if (restoreSidebarFrames && --restoreSidebarFrames == 0) RestoreHiddenSidebar();
}

static void PrepareSidebar() {
    for (auto h : FindNodes(L"SidebarBorder")) {
        auto sidebar = Obj<mux::FrameworkElement>(h);
        if (!sidebar || !sidebar.XamlRoot() || sidebar.ActualWidth() > 0) continue;
        auto root = sidebar.XamlRoot();
        auto environment = root.ContentIslandEnvironment();
        if (!environment || environment.AppWindowId().Value !=
            (uint64_t)GetForegroundWindow()) continue;
        // Realize cold-start tab controls in one layout pass, never a rendered frame.
        if (RevealHiddenSidebar(root)) RestoreHiddenSidebar();
    }
}

static bool SearchableSidebarTab(const mux::FrameworkElement& tab) {
    if (tab.ActualWidth() <= 0 || tab.ActualHeight() <= 0) return false;
    mux::FrameworkElement sidebar{nullptr};
    for (mux::DependencyObject d = tab; d; d = muxm::VisualTreeHelper::GetParent(d)) {
        auto fe = d.try_as<mux::FrameworkElement>();
        if (!fe) continue;
        // Focus mode hides an ancestor of SidebarBorder. Ignore that outer
        // visibility, but still exclude collapsed groups inside the sidebar.
        if (fe.Name() == L"SidebarBorder") { sidebar = fe; break; }
        if (fe.Visibility() != mux::Visibility::Visible) return false;
    }
    if (!sidebar) return false;
    auto p = tab.TransformToVisual(sidebar).TransformPoint({0, 0});
    // Dia also keeps adjacent Spaces laid out next to the active one.
    // Their rows must not be treated as tabs in this sidebar.
    return p.X < sidebar.ActualWidth() && p.X + tab.ActualWidth() > 0;
}

#include "favorite-labels.h"

static void Clear() {
    RestoreNativeHighlights();
    if (auto p = presenter.get(); p && p.Header() == header) p.Header(nullptr);
    list = nullptr;
    presenter = nullptr;
    header = nullptr;
    matches.clear();
    query.clear();
    selected = -1;
    navigating = false;
    revealPending = false;
}

static muxm::Brush SuggestionBrush(mux::DependencyObject element, const wchar_t* key) {
    for (; element; element = muxm::VisualTreeHelper::GetParent(element)) {
        auto fe = element.try_as<mux::FrameworkElement>();
        if (!fe) continue;
        auto value = fe.Resources().TryLookup(box_value(key));
        if (auto brush = value.try_as<muxm::Brush>()) return brush;
    }
    return Resource(key).try_as<muxm::Brush>();
}

static void Paint() {
    if (!header) return;
    auto nativeList = list.get();
    auto selectedBrush = SuggestionBrush(nativeList, L"SuggestionSelectedBrush");
    auto hoverBrush = SuggestionBrush(nativeList, L"SuggestionHoverBrush");
    auto pressedBrush = SuggestionBrush(nativeList, L"SuggestionPressedBrush");
    auto selectedHoverBrush = SuggestionBrush(nativeList, L"SuggestionSelectedPointerOverBrush");
    auto selectedPressedBrush = SuggestionBrush(nativeList, L"SuggestionSelectedPressedBrush");
    for (uint32_t i = 0; i < header.Children().Size(); ++i) {
        auto row = header.Children().GetAt(i).as<muxc::ListViewItem>();
        row.IsSelected((int)i == selected);
        row.ApplyTemplate();
        auto visual = FindVisual(row, [](auto d) {
            return (bool)d.template try_as<muxc::Primitives::ListViewItemPresenter>();
        }).try_as<muxc::Primitives::ListViewItemPresenter>();
        if (!visual) continue;
        // Header items are outside ListView's container preparation. Use Dia's own
        // brushes and suppress WinUI's accent selection bar explicitly.
        visual.SelectionIndicatorVisualEnabled(false);
        if (selectedBrush) visual.SelectedBackground(selectedBrush);
        if (hoverBrush) visual.PointerOverBackground(hoverBrush);
        if (pressedBrush) visual.PressedBackground(pressedBrush);
        if (selectedHoverBrush) visual.SelectedPointerOverBackground(selectedHoverBrush);
        if (selectedPressedBrush) visual.SelectedPressedBackground(selectedPressedBrush);
    }
    // Changing Dia's SelectedIndex here also changes its autocomplete text and
    // selection range. Do not mutate its query while painting our own selection.
    if (selected < 0) return RestoreNativeHighlights();
    if (auto l = list.get()) {
        for (uint32_t i = 0; i < l.Items().Size(); ++i) {
            auto container = l.ContainerFromIndex(i).try_as<mux::DependencyObject>();
            auto background = FindVisual(container, [](auto d) {
                auto fe = d.template try_as<mux::FrameworkElement>();
                return fe && fe.Name() == L"SuggestionBackground";
            }).try_as<mux::UIElement>();
            if (!background) continue;
            bool known = false;
            for (auto& [weak, opacity] : nativeHighlights)
                if (weak.get() == background) known = true;
            if (!known) nativeHighlights.push_back({winrt::make_weak(background), background.Opacity()});
            background.Opacity(0);
        }
    }
}

static void RevealSelection() {
    if (!revealPending || !header) return;
    if (selected >= 0 && selected < (int)header.Children().Size()) {
        auto row = header.Children().GetAt(selected).as<mux::FrameworkElement>();
        if (row.ActualHeight() <= 0) return;
        row.StartBringIntoView();
    } else if (auto l = list.get()) {
        int index = l.SelectedIndex();
        if (index < 0 || index >= (int)l.Items().Size()) return;
        l.ScrollIntoView(l.Items().GetAt(index));
    }
    revealPending = false;
}

static void CancelPendingActivation() {
    pending = nullptr;
    pendingFrames = 0;
    if (restoreSidebarRoot) restoreSidebarFrames = 1;
}

static void Activate(const winrt::weak_ref<mux::FrameworkElement>& target) {
    // Close the popover first. A later tick revalidates the live target before acting.
    pending = target;
    pendingFrames = 30;
    pendingRevealRequested = false;
    pendingScrollRequested = false;
    pendingSidebarWait = 0;
    t_newTabMode = false;
    SendKeys({{VK_ESCAPE, true}, {VK_ESCAPE, false}});
    Clear();
}

static void CannotSwitch() {
    Log("open tabs: target unavailable; no new tab opened");
    pending = nullptr;
    pendingFrames = 0;
    if (restoreSidebarRoot) restoreSidebarFrames = 1;
    // Do not silently create a duplicate when a tab closes or moves during selection.
    MessageBoxW(nullptr, L"The sidebar tab is no longer available. Open the sidebar and try again.",
                L"WinDiaTweaks", MB_OK | MB_ICONINFORMATION);
}

static void FinishActivation() {
    if (!pendingFrames) return;
    if (--pendingFrames > 26) return;
    auto tab = pending.get();
    if (!tab || !tab.XamlRoot()) return CannotSwitch();
    DWORD pid = 0;
    HWND window = GetForegroundWindow();
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId()) {
        pending = nullptr;
        pendingFrames = 0;
        if (restoreSidebarRoot) restoreSidebarFrames = 1;
        return;  // Never send input to another app after focus changes.
    }
    for (auto h : FindNodes(L"PopoverLayoutRoot")) {
        auto p = Obj<mux::FrameworkElement>(h);
        if (p && p.XamlRoot() == tab.XamlRoot() && ShownOnScreen(p) && p.ActualWidth() > 0) {
            if (!pendingFrames) CannotSwitch();
            return;
        }
    }
    namespace peers = mux::Automation::Peers;
    namespace providers = mux::Automation::Provider;
    if (auto peer = peers::FrameworkElementAutomationPeer::CreatePeerForElement(tab)) {
        if (auto p = peer.GetPattern(peers::PatternInterface::SelectionItem).try_as<providers::ISelectionItemProvider>()) {
            p.Select();
            pendingFrames = 0;
            pending = nullptr;
            if (restoreSidebarRoot) restoreSidebarFrames = 3;
            return;
        }
        if (auto p = peer.GetPattern(peers::PatternInterface::Invoke).try_as<providers::IInvokeProvider>()) {
            p.Invoke();
            pendingFrames = 0;
            pending = nullptr;
            if (restoreSidebarRoot) restoreSidebarFrames = 3;
            return;
        }
    }
    // Dia's custom tab UserControls do not all expose an Invoke provider. Use a
    // live, hit-tested tab point, not saved coordinates or a guessed sidebar index.
    auto root = tab.XamlRoot();
    if (!SidebarVisible(root)) {
        if (!pendingRevealRequested) {
            if (!RevealHiddenSidebar(root)) return CannotSwitch();
            pendingRevealRequested = true;
            pendingSidebarWait = 1;
        }
        if (!pendingFrames) CannotSwitch();
        return;
    }
    if (pendingSidebarWait > 0) { --pendingSidebarWait; return; }
    if (!ShownOnScreen(tab)) return CannotSwitch();
    if (!pendingScrollRequested) {
        tab.StartBringIntoView();
        pendingScrollRequested = true;
        return;  // Allow the sidebar's scroll/layout pass before hit testing.
    }
    wf::Point point = tab.TransformToVisual(nullptr).TransformPoint(
        {(float)(std::min)(tab.ActualWidth() / 2, 24.0), (float)(tab.ActualHeight() / 2)});
    if (tab.ActualWidth() < 1 || tab.ActualHeight() < 1 || point.X < 0 || point.Y < 0 ||
        point.X >= root.Size().Width || point.Y >= root.Size().Height) return CannotSwitch();
    auto hits = muxm::VisualTreeHelper::FindElementsInHostCoordinates(point, root.Content());
    auto first = hits.First();
    if (!first.HasCurrent() || !Under(first.Current(), tab)) return CannotSwitch();
    auto scale = root.RasterizationScale();
    POINT screen{(LONG)std::lround(point.X * scale), (LONG)std::lround(point.Y * scale)};
    if (!ClientToScreen(window, &screen)) return CannotSwitch();
    DWORD hitPid = 0;
    HWND hitWindow = WindowFromPoint(screen);
    GetWindowThreadProcessId(hitWindow, &hitPid);
    if (hitPid != GetCurrentProcessId()) return CannotSwitch();
    if (restoreSidebarRoot) {
        wchar_t name[128]{};
        GetClassNameW(hitWindow, name, 128);
        // A transparent XAML subtree must not send a click into the underlying web page.
        if (wcscmp(name, L"InputSiteWindowClass") &&
            wcscmp(name, L"Microsoft.UI.Content.DesktopChildSiteBridge") &&
            wcscmp(name, L"WinUIDesktopWin32WindowClass")) return CannotSwitch();
    }
    POINT cursor;
    if (!GetCursorPos(&cursor)) return CannotSwitch();
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 1 || h <= 1) return CannotSwitch();
    INPUT inputs[4]{};
    for (auto& i : inputs) i.type = INPUT_MOUSE;
    auto move = [&](INPUT& i, POINT p) {
        i.mi.dx = MulDiv(p.x - x, 65535, w - 1);
        i.mi.dy = MulDiv(p.y - y, 65535, h - 1);
        i.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    };
    move(inputs[0], screen);
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    move(inputs[3], cursor);
    pending = nullptr;
    pendingFrames = 0;
    if (restoreSidebarRoot) restoreSidebarFrames = 3;
    if (SendInput(4, inputs, sizeof(INPUT)) != 4) {
        INPUT release{};
        release.type = INPUT_MOUSE;
        release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &release, sizeof(INPUT));
        CannotSwitch();
    }
}

static std::vector<Match> Find(const mux::XamlRoot& root, const std::wstring& needle) {
    std::vector<Match> result;
    Engine_ForEachNode([&](const NodeRef& n) {
        auto tab = Obj<muxc::UserControl>(n.handle);
        if (!tab || tab.XamlRoot() != root || !SearchableSidebarTab(tab)) return;
        bool dock = false, rows = false;
        for (auto d = muxm::VisualTreeHelper::GetParent(tab); d; d = muxm::VisualTreeHelper::GetParent(d)) {
            if (auto fe = d.try_as<mux::FrameworkElement>()) {
                dock |= fe.Name() == L"DockHost";
                rows |= fe.Name() == L"RowsPanel";
            }
        }
        if (!dock && !rows) return;
        // Ignore icon/label UserControls inside a tab, and editable folder headers.
        auto parent = muxm::VisualTreeHelper::GetParent(tab);
        if (dock && !parent.try_as<muxc::Canvas>()) return;
        if (!dock) {
            if (FindVisual(tab, [](auto d) { return (bool)d.template try_as<muxc::TextBox>(); })) return;
            auto label = FindVisual(tab, [](auto d) {
                auto t = d.template try_as<muxc::TextBlock>();
                return t && !t.Text().empty();
            }).try_as<muxc::TextBlock>();
            if (!label) return;
            // The tab body contains its own action buttons; icons and badges do not.
            if (!FindVisual(tab, [](auto d) { return (bool)d.template try_as<muxc::Button>(); })) return;
        }
        auto tip = Text(muxc::ToolTipService::GetToolTip(tab));
        if (tip.empty()) {
            // Dia can attach a pinned tab's tooltip to its root grid, not the UserControl.
            FindVisual(tab, [&](auto d) {
                if (auto fe = d.template try_as<mux::FrameworkElement>()) {
                    auto value = Text(muxc::ToolTipService::GetToolTip(fe));
                    if (!value.empty()) tip += (tip.empty() ? L"" : L" ") + value;
                }
                return false;
            });
        }
        std::wstring title = std::wstring(mux::Automation::AutomationProperties::GetName(tab));
        if (title.empty()) title = tip.substr(0, tip.find_first_of(L"\r\n"));
        if (title.empty() && !dock) {
            // The first TextBlock can be a private-use icon glyph, not the tab title.
            FindVisual(tab, [&](auto d) {
                auto t = d.template try_as<muxc::TextBlock>();
                if (t && t.Text().size() > title.size()) {
                    std::wstring value(t.Text());
                    if (value[0] < 0xe000 || value[0] > 0xf8ff) title = value;
                }
                return false;
            });
        }
        if (dock) {
            auto host = favorites::Host(tab);
            if (title.empty()) title = host;
            if (!host.empty()) tip += (tip.empty() ? L"" : L" ") + host;
        }
        if (title.empty() || Lower(title + L" " + tip).find(needle) == std::wstring::npos) return;
        for (auto& m : result) {
            auto existing = m.target.get();
            if (existing && (Under(tab, existing) || Under(existing, tab))) return;
        }
        result.push_back({winrt::make_weak(tab.as<mux::FrameworkElement>()), title, tip});
    });
    std::stable_sort(result.begin(), result.end(), [&](auto& a, auto& b) {
        auto rank = [&](auto& m) {
            auto t = Lower(m.title);
            return t == needle ? 0 : t.rfind(needle, 0) == 0 ? 1 : 2;
        };
        return rank(a) < rank(b);
    });
    if (result.size() > 5) result.resize(5);
    return result;
}

static void Update() {
    RestoreSidebar();
    if (!Engine_TweakOn(L"floatingNewTab")) {
        if (restoreSidebarRoot) restoreSidebarFrames = 1;
        favorites::labels.clear();
        pending = nullptr;
        pendingFrames = 0;
        return Clear();
    }
    if (favorites::Observe()) query.clear();
    try {
        FinishActivation();
    } catch (winrt::hresult_error const& e) {
        Log("open tabs: activation failed 0x%08lx", (unsigned long)e.code());
        CannotSwitch();
    }
    auto box = FocusedQueryBox().try_as<muxc::RichEditBox>();
    if (!t_newTabMode || !box) return Clear();
    // Header rows are outside ListView.Items, so ListView cannot auto-scroll
    // them. Wait for their first layout before revealing a keyboard selection.
    RevealSelection();
    mux::DependencyObject popover = box;
    while (popover) {
        auto fe = popover.try_as<mux::FrameworkElement>();
        if (fe && fe.Name() == L"PopoverLayoutRoot") break;
        popover = muxm::VisualTreeHelper::GetParent(popover);
    }
    auto l = FindVisual(popover, [&](auto d) {
        auto fe = d.template try_as<mux::FrameworkElement>();
        return fe && fe.Name() == L"SuggestionsListView";
    }).try_as<muxc::ListView>();
    if (!l) return Clear();
    // Arrow selection updates Dia's autocomplete text. It is not a new query.
    if (navigating && list.get() == l) return Paint();
    winrt::hstring raw;
    box.Document().GetText(winrt::Microsoft::UI::Text::TextGetOptions::None, raw);
    std::wstring typed(raw);
    // Dia selects its autocomplete suffix. Match what the person typed, not that suffix.
    auto selection = box.Document().Selection();
    if (selection.StartPosition() > 0 && selection.EndPosition() > selection.StartPosition() &&
        selection.EndPosition() >= (int)typed.find_last_not_of(L"\r\n") + 1)
        typed.resize(selection.StartPosition());
    while (!typed.empty() && iswspace(typed.back())) typed.pop_back();
    while (!typed.empty() && iswspace(typed.front())) typed.erase(typed.begin());
    typed = Lower(typed);
    if (typed.empty()) return Clear();
    if (typed == query && list.get() == l && (!header || presenter.get())) return Paint();
    Clear();
    auto items = FindVisual(l, [](auto d) {
        return (bool)d.template try_as<muxc::ItemsPresenter>();
    }).try_as<muxc::ItemsPresenter>();
    // Dia's custom list template does not bind ListView.Header. Put the header
    // on the actual ItemsPresenter, and leave any future native header intact.
    if (!items || items.Header()) return;
    matches = Find(box.XamlRoot(), typed);
    Log("open tabs: %zu matching sidebar controls", matches.size());
    query = typed;
    list = winrt::make_weak(l);
    presenter = winrt::make_weak(items);
    if (matches.empty()) return;
    header = muxc::StackPanel();
    auto sample = l.ContainerFromIndex(0).try_as<muxc::ListViewItem>();
    for (auto& m : matches) {
        auto b = muxc::ListViewItem();
        b.Style(l.ItemContainerStyle() ? l.ItemContainerStyle() : sample ? sample.Style() : nullptr);
        if (sample) {
            b.Template(sample.Template());
            b.Padding(sample.Padding());
            b.Margin(sample.Margin());
            b.MinHeight(sample.MinHeight());
            b.CornerRadius(sample.CornerRadius());
            b.Foreground(sample.Foreground());
        }
        b.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        b.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        auto row = muxc::Grid();
        row.ColumnSpacing(8);
        for (auto unit : {mux::GridUnitType::Auto, mux::GridUnitType::Star,
                          mux::GridUnitType::Auto, mux::GridUnitType::Auto}) {
            auto col = muxc::ColumnDefinition();
            col.Width({1, unit});
            row.ColumnDefinitions().Append(col);
        }
        if (auto tab = m.target.get()) {
            auto original = FindVisual(tab, [](auto d) {
                auto image = d.template try_as<muxc::Image>();
                return image && image.Source();
            }).try_as<muxc::Image>();
            if (original) {
                auto icon = muxc::Image();
                icon.Source(original.Source());
                icon.Width(16);
                icon.Height(16);
                row.Children().Append(icon);
            }
        }
        auto text = muxc::TextBlock();
        text.Text(m.title);
        text.TextTrimming(mux::TextTrimming::CharacterEllipsis);
        text.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(text, 1);
        row.Children().Append(text);
        auto action = muxc::TextBlock();
        action.Text(L"Switch to tab");
        action.Opacity(0.65);
        action.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(action, 2);
        row.Children().Append(action);
        auto arrow = muxc::FontIcon();
        arrow.FontFamily(muxm::FontFamily(L"Segoe Fluent Icons"));
        arrow.Glyph(L"\uE8AB");
        arrow.FontSize(14);
        arrow.Opacity(0.65);
        muxc::Grid::SetColumn(arrow, 3);
        row.Children().Append(arrow);
        b.Content(row);
        mux::Automation::AutomationProperties::SetName(b, m.title + L", switch to tab");
        muxc::ToolTipService::SetToolTip(b, box_value(m.text.empty() ? m.title : m.text));
        auto target = m.target;
        b.Tapped([target](auto&&, auto&& args) { args.Handled(true); Activate(target); });
        header.Children().Append(b);
    }
    items.Header(header);
    selected = 0;
    revealPending = true;
    Paint();
}

static bool Submit() {
    Update();
    if (selected < 0 || selected >= (int)matches.size()) return false;
    Activate(matches[selected].target);
    return true;
}

static bool Arrow(WORD key) {
    if (matches.empty()) return false;
    auto l = list.get();
    if (!l) return false;
    navigating = true;
    if (key == VK_DOWN) {
        if (selected < 0) {
            int next = l.SelectedIndex() + 1;
            if (next < (int)l.Items().Size()) l.SelectedIndex(next);
        } else if (++selected >= (int)matches.size()) {
            selected = -1;
            if (l.Items().Size()) l.SelectedIndex(0);
        }
    } else if (key == VK_UP) {
        if (selected < 0) {
            if (l.SelectedIndex() > 0) l.SelectedIndex(l.SelectedIndex() - 1);
            else selected = (int)matches.size() - 1;
        } else if (selected > 0) --selected;
    } else return false;
    revealPending = true;
    Paint();
    return true;
}

static void Edit() { navigating = false; }
}  // namespace open_tabs
