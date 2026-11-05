// Included inside open_tabs. Keep only hostnames learned from live UI controls.
#pragma once

namespace favorites {
struct Label {
    winrt::weak_ref<mux::FrameworkElement> tab;
    winrt::weak_ref<muxm::ImageSource> icon;
    std::wstring host, pendingHost;
    ULONGLONG since = 0;
    bool observed = false;
};
static thread_local std::vector<Label> labels;

static bool IsFavorite(const mux::FrameworkElement& tab) {
    auto canvas = muxm::VisualTreeHelper::GetParent(tab).try_as<muxc::Canvas>();
    if (!canvas) return false;
    auto dock = muxm::VisualTreeHelper::GetParent(canvas).try_as<mux::FrameworkElement>();
    return dock && dock.Name() == L"DockHost";
}

static muxm::ImageSource Icon(const mux::FrameworkElement& tab) {
    auto image = FindVisual(tab, [](auto d) {
        auto i = d.template try_as<muxc::Image>();
        return i && i.Source();
    }).try_as<muxc::Image>();
    return image ? image.Source() : nullptr;
}

static bool IsSelected(const mux::FrameworkElement& tab) {
    if (!muxm::VisualTreeHelper::GetChildrenCount(tab)) return false;
    auto grid = muxm::VisualTreeHelper::GetChild(tab, 0).try_as<muxc::Grid>();
    if (!grid || grid.Children().Size() < 6) return false;
    // Dia 1.5's favorite template uses these two borders for the selected state.
    // Validate the template shape; an unknown layout must not learn a wrong host.
    for (uint32_t i = 0; i < 5; ++i)
        if (!grid.Children().GetAt(i).try_as<muxc::Border>()) return false;
    return grid.Children().GetAt(1).Visibility() == mux::Visibility::Visible &&
           grid.Children().GetAt(4).Visibility() == mux::Visibility::Visible;
}

static std::wstring AddressHost(const mux::XamlRoot& root) {
    muxc::RichEditBox address{nullptr};
    for (auto h : FindNodes(L"AddressRichEditBox")) {
        auto box = Obj<muxc::RichEditBox>(h);
        if (!box || box.XamlRoot() != root || !ShownOnScreen(box) || box.ActualWidth() <= 0) continue;
        if (address) return {};  // Split panes do not identify a unique favorite address.
        address = box;
    }
    if (!address) return {};
    winrt::hstring raw;
    address.Document().GetText(winrt::Microsoft::UI::Text::TextGetOptions::None, raw);
    std::wstring text(raw);
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    if (text.empty() || text.find_first_of(L" \t\r\n") != std::wstring::npos) return {};
    if (text.find(L"://") == std::wstring::npos) text = L"https://" + text;
    try {
        wf::Uri uri(text);
        if (uri.SchemeName() != L"https" && uri.SchemeName() != L"http") return {};
        auto host = Lower(std::wstring(uri.Host()));
        if (host.rfind(L"www.", 0) == 0) host.erase(0, 4);
        return host;
    } catch (winrt::hresult_error const&) {
        return {};
    }
}

static bool Observe() {
    auto previousSize = labels.size();
    labels.erase(std::remove_if(labels.begin(), labels.end(), [](auto& label) {
        auto tab = label.tab.get();
        auto icon = label.icon.get();
        return !tab || !icon || !IsFavorite(tab) || Icon(tab) != icon;
    }), labels.end());
    bool changed = labels.size() != previousSize;
    for (auto& label : labels) label.observed = false;
    for (auto h : FindNodes(L"DockHost")) {
        auto dock = Obj<mux::FrameworkElement>(h);
        if (!dock || !dock.XamlRoot() || !ShownOnScreen(dock) || dock.ActualHeight() <= 0) continue;
        mux::FrameworkElement active{nullptr};
        bool ambiguous = false;
        auto canvas = FindVisual(dock, [](auto d) { return (bool)d.template try_as<muxc::Canvas>(); })
                          .try_as<muxc::Canvas>();
        if (!canvas) continue;
        for (auto child : canvas.Children()) {
            auto tab = child.try_as<muxc::UserControl>();
            if (!tab || !ShownOnScreen(tab) || !IsSelected(tab)) continue;
            if (active) { ambiguous = true; break; }
            active = tab;
        }
        if (!active || ambiguous) continue;
        auto host = AddressHost(dock.XamlRoot());
        auto icon = Icon(active);
        if (host.empty() || !icon) continue;
        auto entry = std::find_if(labels.begin(), labels.end(), [&](auto& label) {
            return label.tab.get() == active;
        });
        if (entry == labels.end()) {
            labels.push_back({winrt::make_weak(active), winrt::make_weak(icon)});
            entry = labels.end() - 1;
        }
        entry->observed = true;
        if (entry->pendingHost != host) {
            entry->pendingHost = host;
            entry->since = GetTickCount64();
        }
        // Selection and address controls can update on different frames.
        if (GetTickCount64() - entry->since >= 500 && entry->host != host) {
            entry->host = host;
            changed = true;
        }
    }
    for (auto& label : labels)
        if (!label.observed) label.pendingHost.clear();
    return changed;
}

static std::wstring Host(const mux::FrameworkElement& tab) {
    for (auto& label : labels)
        if (label.tab.get() == tab && label.icon.get() == Icon(tab)) return label.host;
    return {};
}
}  // namespace favorites
