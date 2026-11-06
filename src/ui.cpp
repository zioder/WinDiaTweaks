// ui.cpp - everything in DiaTweaks that needs real WinUI code instead of property rules:
//   * the "Patches" page added to Dia's own Settings window
//   * window states that rules can depend on (sidebarOpen, newTab)
//   * code-driven tweaks (window material, profile emoji, downloads, tab switcher, ...)
// Runs on Dia's UI threads only (called from the engine's timer and from WinUI events).

#include <unknwn.h>
// windows.h maps GetCurrentTime to GetTickCount (a Win16 leftover), which clashes with WinUI's
// Storyboard::GetCurrentTime.
#undef GetCurrentTime
#include <algorithm>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.Graphics.h>
#include <cmath>
#include <climits>
#include <limits>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>

#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "engine.h"

namespace wf = winrt::Windows::Foundation;
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;
namespace muxsh = winrt::Microsoft::UI::Xaml::Shapes;
using winrt::box_value;
using winrt::unbox_value_or;

#define Log Engine_Log

// ---------------------------------------------------------------- helpers

template <class T>
static T Obj(InstanceHandle h) {
    IInspectable* p = Engine_Object(h);
    if (!p) return nullptr;
    wf::IInspectable i;
    winrt::attach_abi(i, p);
    return i.try_as<T>();
}

static std::string N(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static InstanceHandle FindNode(const std::wstring& name, const std::wstring& typeSuffix = L"") {
    InstanceHandle found = 0;
    Engine_ForEachNode([&](const NodeRef& r) {
        if (found || r.name != name) return;
        if (!typeSuffix.empty() && (r.type.size() < typeSuffix.size() ||
                                    r.type.compare(r.type.size() - typeSuffix.size(), typeSuffix.size(), typeSuffix)))
            return;
        found = r.handle;
    });
    return found;
}

static std::vector<InstanceHandle> FindNodes(const std::wstring& name) {
    std::vector<InstanceHandle> out;
    Engine_ForEachNode([&](const NodeRef& r) {
        if (r.name == name) out.push_back(r.handle);
    });
    return out;
}

static wf::IInspectable Resource(const wchar_t* key) {
    try {
        auto res = mux::Application::Current().Resources();
        auto k = box_value(key);
        if (auto v = res.TryLookup(k)) return v;
    } catch (...) {
    }
    return nullptr;
}

static muxm::Brush BrushRes(const wchar_t* key, winrt::Windows::UI::Color fallback) {
    if (auto b = Resource(key).try_as<muxm::Brush>()) return b;
    return muxm::SolidColorBrush(fallback);
}

static winrt::Windows::UI::Color Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return {a, r, g, b}; }

// Depth-first search of the live visual tree under an element.
template <class F>
static mux::DependencyObject FindVisual(mux::DependencyObject root, F pred, int depth = 0) {
    if (!root || depth > 60) return nullptr;
    if (pred(root)) return root;
    int n = muxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < n; i++)
        if (auto r = FindVisual(muxm::VisualTreeHelper::GetChild(root, i), pred, depth + 1)) return r;
    return nullptr;
}

// ---------------------------------------------------------------- probe (for making tweaks)
// `diatweaks probe Name1 Name2` writes probe.txt; this logs live facts about those elements.

static void DescribeElement(std::ostringstream& o, const mux::FrameworkElement& fe, int depth) {
    std::wstring type(winrt::get_class_name(fe));
    o << std::string(depth * 2, ' ') << N(type) << " \"" << N(std::wstring(fe.Name())) << "\""
      << " vis=" << (fe.Visibility() == mux::Visibility::Visible ? "V" : "C") << " op=" << fe.Opacity()
      << " size=" << fe.ActualWidth() << "x" << fe.ActualHeight();
    try {
        auto p = fe.TransformToVisual(nullptr).TransformPoint({0, 0});
        o << " at=" << p.X << "," << p.Y;
    } catch (...) {
    }
    if (auto c = fe.try_as<muxc::Control>()) {
        if (auto b = c.Background().try_as<muxm::SolidColorBrush>()) {
            auto col = b.Color();
            o << " bg=#" << std::hex << (int)col.A << "," << (int)col.R << "," << (int)col.G << "," << (int)col.B
              << std::dec << " bgop=" << b.Opacity();
        }
    } else if (auto bd = fe.try_as<muxc::Border>()) {
        if (auto b = bd.Background().try_as<muxm::SolidColorBrush>()) {
            auto col = b.Color();
            o << " bg=#" << std::hex << (int)col.A << "," << (int)col.R << "," << (int)col.G << "," << (int)col.B
              << std::dec << " bgop=" << b.Opacity();
        }
    } else if (auto pn = fe.try_as<muxc::Panel>()) {
        if (auto b = pn.Background().try_as<muxm::SolidColorBrush>()) {
            auto col = b.Color();
            o << " bg=#" << std::hex << (int)col.A << "," << (int)col.R << "," << (int)col.G << "," << (int)col.B
              << std::dec << " bgop=" << b.Opacity();
        }
    }
    auto cr = [&](mux::CornerRadius r) {
        if (r.TopLeft || r.TopRight || r.BottomLeft || r.BottomRight)
            o << " radius=" << r.TopLeft << "," << r.TopRight << "," << r.BottomRight << "," << r.BottomLeft;
    };
    if (auto c = fe.try_as<muxc::Control>()) cr(c.CornerRadius());
    else if (auto b = fe.try_as<muxc::Border>()) cr(b.CornerRadius());
    else if (auto g = fe.try_as<muxc::Grid>()) cr(g.CornerRadius());
    else if (auto cp = fe.try_as<muxc::ContentPresenter>()) cr(cp.CornerRadius());
    if (!fe.IsHitTestVisible()) o << " nohit";
    try {  // brushes in the element's own resources (Dia sets per-site hover colors this way)
        for (auto const& kv : fe.Resources()) {
            auto k = kv.Key().try_as<wf::IReference<winrt::hstring>>();
            o << " res[" << (k ? N(std::wstring(k.Value())) : std::string("?")) << "]";
            if (auto b = kv.Value().try_as<muxm::SolidColorBrush>()) {
                auto col = b.Color();
                o << "=#" << std::hex << (int)col.A << "," << (int)col.R << "," << (int)col.G << "," << (int)col.B << std::dec;
            }
        }
    } catch (...) {
    }
    if (int z = muxc::Canvas::GetZIndex(fe)) o << " z=" << z;
    if (auto reb = fe.try_as<muxc::RichEditBox>()) {
        o << (reb.IsReadOnly() ? " readonly" : "") << (reb.AcceptsReturn() ? " acceptsreturn" : "");
        auto hex = [&](winrt::Windows::UI::Color c) {
            o << "#" << std::hex << (int)c.A << "," << (int)c.R << "," << (int)c.G << "," << (int)c.B << std::dec;
        };
        if (auto f = reb.Foreground().try_as<muxm::SolidColorBrush>()) o << " fg=", hex(f.Color());
        try {  // text colors run by run (Dia colors parts of the address separately)
            auto doc = reb.Document();
            winrt::hstring all;
            doc.GetText(winrt::Microsoft::UI::Text::TextGetOptions::None, all);
            int len = (int)all.size();
            o << " runs=";
            for (int i = 0; i < len && i < 400;) {
                auto r = doc.GetRange(i, i + 1);
                r.Expand(winrt::Microsoft::UI::Text::TextRangeUnit::CharacterFormat);
                int end = r.EndPosition();
                o << "[" << r.StartPosition() << "-" << end << " ";
                hex(r.CharacterFormat().ForegroundColor());
                o << "]";
                i = end > i ? end : i + 1;
            }
        } catch (...) {
        }
    }
    if (fe.Clip()) o << " xamlclip";
    o << " row=" << muxc::Grid::GetRow(fe) << " col=" << muxc::Grid::GetColumn(fe);
    if (auto g = fe.try_as<muxc::Grid>()) {
        o << " rows=[";
        for (auto const& r : g.RowDefinitions()) {
            auto h = r.Height();
            o << (h.GridUnitType == mux::GridUnitType::Auto ? std::string("Auto") : std::to_string(h.Value) + (h.GridUnitType == mux::GridUnitType::Star ? "*" : "")) << " ";
        }
        o << "] cols=[";
        for (auto const& c : g.ColumnDefinitions()) {
            auto w = c.Width();
            o << (w.GridUnitType == mux::GridUnitType::Auto ? std::string("Auto") : std::to_string(w.Value) + (w.GridUnitType == mux::GridUnitType::Star ? "*" : "")) << " ";
        }
        o << "]";
    }
    try {
        auto v = winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(fe);
        if (auto clip = v.Clip()) {
            o << " visualclip=" << N(std::wstring(winrt::get_class_name(clip)));
            if (auto gc = clip.try_as<winrt::Microsoft::UI::Composition::CompositionGeometricClip>())
                if (auto rr = gc.Geometry().try_as<winrt::Microsoft::UI::Composition::CompositionRoundedRectangleGeometry>())
                    o << "(r=" << rr.CornerRadius().x << ")";
        }
    } catch (...) {
    }
    try {
        namespace comp = winrt::Microsoft::UI::Composition;
        std::function<void(comp::Visual const&, int)> dv = [&](comp::Visual const& v, int lvl) {
            o << " {" << N(std::wstring(winrt::get_class_name(v)));
            auto sz = v.Size();
            o << " " << sz.x << "x" << sz.y;
            if (auto clip = v.Clip()) {
                o << " clip=" << N(std::wstring(winrt::get_class_name(clip)));
                if (auto gc = clip.try_as<comp::CompositionGeometricClip>())
                    if (auto rr = gc.Geometry().try_as<comp::CompositionRoundedRectangleGeometry>())
                        o << "(r=" << rr.CornerRadius().x << ")";
            }
            if (auto cv = v.try_as<comp::ContainerVisual>(); cv && lvl < 3)
                for (auto const& ch : cv.Children()) dv(ch, lvl + 1);
            o << "}";
        };
        if (auto child = winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementChildVisual(fe))
            o << " childvisual=", dv(child, 0);
    } catch (...) {
    }
    if (auto tb = fe.try_as<muxc::TextBlock>()) {
        o << " text=\"" << N(std::wstring(tb.Text())) << "\"";
        if (auto f = tb.Foreground().try_as<muxm::SolidColorBrush>()) {
            auto c = f.Color();
            o << " fg=#" << std::hex << (int)c.A << "," << (int)c.R << "," << (int)c.G << "," << (int)c.B << std::dec;
        }
    }
    if (auto reb = fe.try_as<muxc::RichEditBox>()) {
        winrt::hstring t;
        reb.Document().GetText(winrt::Microsoft::UI::Text::TextGetOptions::None, t);
        o << " text=\"" << N(std::wstring(t)) << "\"";
    }
    if (auto ui = fe.try_as<mux::UIElement>()) {
        if (auto tr = ui.RenderTransform().try_as<muxm::TranslateTransform>())
            o << " translate=" << tr.X() << "," << tr.Y();
    }
    o << "\n";
}

static void RunProbe() {
    std::wstring path = Engine_Dir() + L"\\probe.txt";
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    std::vector<std::wstring> names;
    while (in >> line) names.push_back(std::wstring(line.begin(), line.end()));
    in.close();
    DeleteFileW(path.c_str());
    std::ostringstream o;
    o << "probe on thread " << GetCurrentThreadId() << "\n";
    for (auto& nm : names) {
        if (nm == L"REGIONS") {
            extern void ProbeRegions(std::ostringstream & o);
            ProbeRegions(o);
            continue;
        }
        if (nm == L"FOCUS") {  // which element has keyboard focus, and its ancestors
            for (auto h : FindNodes(L"SidebarBorder")) {
                auto any = Obj<mux::FrameworkElement>(h);
                if (!any || !any.XamlRoot()) continue;
                int depth = 0;
                for (auto f = mux::Input::FocusManager::GetFocusedElement(any.XamlRoot()).try_as<mux::DependencyObject>();
                     f && depth < 40; f = muxm::VisualTreeHelper::GetParent(f), depth++)
                    if (auto ffe = f.try_as<mux::FrameworkElement>()) {
                        auto nmf = std::wstring(ffe.Name());
                        o << "focus[" << depth << "] " << N(std::wstring(winrt::get_class_name(ffe))) << " \"" << N(nmf) << "\"\n";
                    }
                break;
            }
            continue;
        }
        bool deep = !nm.empty() && nm.back() == L'*';
        std::wstring name = deep ? nm.substr(0, nm.size() - 1) : nm;
        for (auto h : FindNodes(name)) {
            auto fe = Obj<mux::FrameworkElement>(h);
            if (!fe) continue;
            DescribeElement(o, fe, 0);
            if (deep) {
                std::function<void(mux::DependencyObject, int)> walk = [&](mux::DependencyObject d, int depth) {
                    if (depth > 12) return;
                    int n = muxm::VisualTreeHelper::GetChildrenCount(d);
                    for (int i = 0; i < n; i++) {
                        auto c = muxm::VisualTreeHelper::GetChild(d, i);
                        if (auto cfe = c.try_as<mux::FrameworkElement>()) DescribeElement(o, cfe, depth);
                        walk(c, depth + 1);
                    }
                };
                walk(fe, 1);
            }
        }
    }
    std::ofstream out(Engine_Dir() + L"\\probe.out", std::ios::app);
    out << o.str();
    Log("probe written (%zu names)", names.size());
}

// ---------------------------------------------------------------- the Patches page

struct NavHook {
    winrt::weak_ref<muxc::NavigationView> nav;
    muxc::NavigationViewItem item{nullptr};
    mux::FrameworkElement page{nullptr};
    muxc::Border updatesHost{nullptr};
    wf::IInspectable saved{nullptr};
    InstanceHandle handle = 0;
};
static thread_local std::vector<NavHook> t_navs;

struct CardLook {
    muxm::Brush background{nullptr}, border{nullptr};
    mux::CornerRadius radius{8, 8, 8, 8};
    bool fromDia = false;
};
static thread_local CardLook t_card;

// Borrow the look of Dia's own settings cards so the page matches whatever theme Dia is in.
static void CaptureCardLook(const mux::DependencyObject& root) {
    auto found = FindVisual(root, [](const mux::DependencyObject& d) {
        auto b = d.try_as<muxc::Border>();
        return b && b.BorderThickness().Left == 1 && b.MinHeight() == 40 && b.Background() != nullptr;
    });
    if (auto b = found.try_as<muxc::Border>()) {
        t_card.background = b.Background();
        t_card.border = b.BorderBrush();
        t_card.radius = b.CornerRadius();
        t_card.fromDia = true;
    }
}

static void EnsureCardLook() {
    if (t_card.background) return;
    t_card.background = BrushRes(L"CardBackgroundFillColorDefaultBrush", Rgba(0x2b, 0x2b, 0x2b));
    t_card.border = BrushRes(L"CardStrokeColorDefaultBrush", Rgba(0x1c, 0x1c, 0x1c));
}

static thread_local bool t_syncing = false;  // true while we set controls from the config
struct BoundToggle {
    std::wstring key;
    winrt::weak_ref<muxc::ToggleSwitch> sw;
};
static thread_local std::vector<BoundToggle> t_toggles;
// Drop-downs that are the tweak's switch: their "off" choice turns the tweak off.
struct BoundChoice {
    std::wstring key;
    winrt::weak_ref<muxc::ComboBox> box;
};
static thread_local std::vector<BoundChoice> t_choiceSwitches;

// A tweak whose choices include "off" is switched by its drop-down alone (no On/Off switch).
static bool ChoiceIsSwitch(const TweakInfo& t) {
    for (auto& [val, label] : t.choices)
        if (val == L"off") return true;
    return false;
}

// The choice a drop-down shows: "off" while the tweak is off.
static int ChoiceIndex(const std::wstring& key) {
    for (auto& t : Engine_Tweaks()) {
        if (t.key != key) continue;
        std::wstring want = t.enabled ? t.value : L"off";
        for (size_t i = 0; i < t.choices.size(); i++)
            if (t.choices[i].first == want) return (int)i;
    }
    return 0;
}

static bool RunToolWindow(const std::wstring& args);

static mux::FrameworkElement Row(const TweakInfo& t) {
    muxc::Grid row;
    row.Padding({12, 8, 12, 8});
    row.MinHeight(40);
    row.ColumnSpacing(12);
    muxc::ColumnDefinition c0, c1, c2;
    c0.Width(mux::GridLengthHelper::Auto());
    c1.Width({1, mux::GridUnitType::Star});
    c2.Width(mux::GridLengthHelper::Auto());
    row.ColumnDefinitions().Append(c0);
    row.ColumnDefinitions().Append(c1);
    row.ColumnDefinitions().Append(c2);

    muxc::FontIcon icon;
    std::wstring glyph = L"";
    if (!t.icon.empty()) glyph = std::wstring(1, (wchar_t)wcstoul(t.icon.c_str(), nullptr, 16));
    icon.Glyph(glyph);
    icon.FontSize(16);
    icon.VerticalAlignment(mux::VerticalAlignment::Center);
    muxc::Grid::SetColumn(icon, 0);
    row.Children().Append(icon);

    muxc::StackPanel text;
    text.VerticalAlignment(mux::VerticalAlignment::Center);
    muxc::TextBlock title;
    title.Text(t.title.empty() ? t.key : t.title);
    title.TextWrapping(mux::TextWrapping::Wrap);
    text.Children().Append(title);
    if (!t.desc.empty()) {
        muxc::TextBlock d;
        d.Text(t.desc);
        d.FontSize(12);
        d.TextWrapping(mux::TextWrapping::Wrap);
        d.Foreground(BrushRes(L"TextFillColorSecondaryBrush", Rgba(0xc5, 0xc5, 0xc5)));
        text.Children().Append(d);
    }
    muxc::Grid::SetColumn(text, 1);
    row.Children().Append(text);

    muxc::StackPanel controls;
    controls.Orientation(muxc::Orientation::Horizontal);
    controls.Spacing(12);
    controls.VerticalAlignment(mux::VerticalAlignment::Center);
    std::wstring key = t.key;
    if (key == L"localBrowsing") {
        muxc::Button restart;
        restart.Content(box_value(L"Restart Dia"));
        winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(restart, L"patch_localBrowsing_restart");
        winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(restart, L"Restart Dia");
        restart.Click([](auto&&, auto&&) { RunToolWindow(L"restart"); });
        controls.Children().Append(restart);
    }

    bool choiceSwitch = ChoiceIsSwitch(t);
    if (!t.choices.empty()) {
        muxc::ComboBox box;
        box.MinWidth(130);
        int sel = 0, i = 0;
        std::wstring shown = choiceSwitch && !t.enabled ? std::wstring(L"off") : t.value;
        for (auto& [val, label] : t.choices) {
            muxc::ComboBoxItem ci;
            ci.Content(box_value(label));
            ci.Tag(box_value(val));
            box.Items().Append(ci);
            if (val == shown) sel = i;
            i++;
        }
        box.SelectedIndex(sel);
        bool keepOn = t.noSwitch;  // (an option of another tweak: always on)
        box.SelectionChanged([key, choiceSwitch, keepOn](wf::IInspectable const& s, auto&&) {
            if (t_syncing) return;
            auto ci = s.as<muxc::ComboBox>().SelectedItem().try_as<muxc::ComboBoxItem>();
            if (!ci) return;
            std::wstring v(unbox_value_or<winrt::hstring>(ci.Tag(), L""));
            if (choiceSwitch && v == L"off") Engine_SetTweak(key, false, nullptr);
            else Engine_SetTweak(key, choiceSwitch || keepOn || Engine_TweakOn(key), &v);
        });
        if (choiceSwitch) {
            box.IsEnabled(!Engine_SafeMode());
            winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(box, L"patch_" + key);
            winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(box, t.title.empty() ? key : t.title);
            t_choiceSwitches.push_back({key, winrt::make_weak(box)});
        }
        controls.Children().Append(box);
    } else if (!t.value.empty()) {
        muxc::TextBox box;
        box.Text(t.value);
        box.MinWidth(64);
        box.MaxLength(16);
        box.TextAlignment(mux::TextAlignment::Center);
        box.LostFocus([key](wf::IInspectable const& s, auto&&) {
            std::wstring v(s.as<muxc::TextBox>().Text());
            if (v.empty() || v == Engine_TweakValue(key)) return;
            Engine_SetTweak(key, Engine_TweakOn(key), &v);
        });
        controls.Children().Append(box);
    }

    if (choiceSwitch || t.noSwitch) {
        muxc::Grid::SetColumn(controls, 2);
        row.Children().Append(controls);
        return row;
    }
    muxc::ToggleSwitch sw;
    sw.MinWidth(0);
    sw.OnContent(box_value(L"On"));
    sw.OffContent(box_value(L"Off"));
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(sw, L"patch_" + key);
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(sw, t.title.empty() ? key : t.title);
    sw.IsOn(t.enabled && !Engine_SafeMode());
    sw.IsEnabled(!Engine_SafeMode());
    sw.Toggled([key](wf::IInspectable const& s, auto&&) {
        if (t_syncing) return;
        Engine_SetTweak(key, s.as<muxc::ToggleSwitch>().IsOn(), nullptr);
    });
    t_toggles.push_back({key, winrt::make_weak(sw)});
    controls.Children().Append(sw);
    muxc::Grid::SetColumn(controls, 2);
    row.Children().Append(controls);
    return row;
}

static mux::FrameworkElement Section(const std::wstring& name, const std::vector<mux::FrameworkElement>& rows) {
    muxc::StackPanel sec;
    muxc::Border head;
    head.Height(38);
    head.Padding({12, 0, 12, 0});
    muxc::TextBlock ht;
    ht.Text(name);
    ht.FontSize(16);
    ht.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
    ht.VerticalAlignment(mux::VerticalAlignment::Center);
    head.Child(ht);
    sec.Children().Append(head);

    muxc::Border card;
    card.MinHeight(40);
    card.Background(t_card.background);
    card.BorderBrush(t_card.border);
    card.BorderThickness({1, 1, 1, 1});
    card.CornerRadius(t_card.radius);
    muxc::StackPanel list;
    for (size_t i = 0; i < rows.size(); i++) {
        if (i) {
            muxc::Border line;
            line.Height(1);
            line.Background(t_card.border);
            list.Children().Append(line);
        }
        list.Children().Append(rows[i]);
    }
    card.Child(list);
    sec.Children().Append(card);
    return sec;
}

static mux::FrameworkElement ButtonRow(const wchar_t* glyph, const std::wstring& title, const std::wstring& desc,
                                       const std::wstring& button, std::function<void()> onClick) {
    TweakInfo fake;
    fake.title = title;
    fake.desc = desc;
    auto row = Row(fake).as<muxc::Grid>();
    t_toggles.pop_back();  // Row() made a switch; replace it with a button
    row.Children().RemoveAtEnd();
    row.Children().GetAt(0).as<muxc::FontIcon>().Glyph(glyph);
    muxc::Button b;
    b.Content(box_value(button));
    b.VerticalAlignment(mux::VerticalAlignment::Center);
    b.Click([onClick](auto&&, auto&&) { onClick(); });
    muxc::Grid::SetColumn(b, 2);
    row.Children().Append(b);
    return row;
}

// ---- Presets: a ready-made set of patches. Picking one sets every patch; changing any patch by hand
// afterwards makes it Custom (done in Engine_SetTweak).
static thread_local winrt::weak_ref<muxc::ComboBox> t_presetBox;
static const std::pair<const wchar_t*, const wchar_t*> kPresets[] = {
    {L"none", L"None"}, {L"arc", L"Arc"}, {L"custom", L"Custom"}};

static void ApplyPreset(const std::wstring& preset) {
    std::vector<std::pair<std::wstring, std::wstring>> values{{L"preset", preset}};
    if (preset == L"custom") return Engine_SetMany({}, values);  // keep the patches as they are
    // Arc: the page fills the window with straight edges, the toolbar lives in the sidebar, Ctrl+T
    // floats, no tab hover cards, an acrylic window, and Peek.
    static const std::set<std::wstring> arc = {L"edgeToEdge",     L"squareCorners",       L"hideToolbar",
                                               L"arcUrlBar",      L"floatingNewTab",      L"hideTabSearchButton",
                                               L"hideChatButton", L"hideTabHoverCard",    L"windowMaterial",
                                               L"peek"};
    std::vector<std::pair<std::wstring, bool>> states;
    for (auto& t : Engine_Tweaks())
        if (!t.noSwitch && t.key != L"preset" && t.key != L"checkForUpdates" &&
            t.key != L"normalLaunch" && t.key != L"localBrowsing")
            states.push_back({t.key, preset == L"arc" && arc.count(t.key) > 0});
    if (preset == L"arc") values.push_back({L"windowMaterial", L"acrylic"});
    Engine_SetMany(states, values);
}

static int PresetIndex() {
    std::wstring v = Engine_TweakValue(L"preset");
    for (int i = 0; i < (int)std::size(kPresets); i++)
        if (v == kPresets[i].first) return i;
    return 0;
}

static mux::FrameworkElement PresetRow() {
    TweakInfo info;
    info.title = L"Preset";
    info.desc = L"A ready-made set of patches.";
    info.icon = L"E771";
    auto row = Row(info).as<muxc::Grid>();
    t_toggles.pop_back();  // Row() made a switch; a drop-down instead
    row.Children().RemoveAtEnd();
    muxc::ComboBox box;
    box.MinWidth(130);
    box.VerticalAlignment(mux::VerticalAlignment::Center);
    for (auto& [val, label] : kPresets) {
        muxc::ComboBoxItem ci;
        ci.Content(box_value(label));
        ci.Tag(box_value(val));
        box.Items().Append(ci);
    }
    box.SelectedIndex(PresetIndex());
    box.IsEnabled(!Engine_SafeMode());
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(box, L"patch_preset");
    box.SelectionChanged([](wf::IInspectable const& s, auto&&) {
        if (t_syncing) return;
        auto ci = s.as<muxc::ComboBox>().SelectedItem().try_as<muxc::ComboBoxItem>();
        if (!ci) return;
        std::wstring v(unbox_value_or<winrt::hstring>(ci.Tag(), L""));
        if (v != Engine_TweakValue(L"preset")) ApplyPreset(v);
    });
    t_presetBox = winrt::make_weak(box);
    muxc::Grid::SetColumn(box, 2);
    row.Children().Append(box);
    return row;
}

// ---- Updates: newest Dia and WinDiaTweaks. `diatweaks check` finds them (in the background, so Dia never
// waits on the network); installing runs `diatweaks update` in a window of its own, outside Dia, because
// it closes Dia and replaces files Dia has loaded.

struct UpdateState {
    std::map<std::string, std::wstring> found;  // what `diatweaks check` printed
    bool checking = false, checked = false;
    std::wstring when;  // time of the last check, e.g. 14:05
};
static std::mutex g_updMx;
static UpdateState g_upd;
static std::atomic<int> g_updGen{0};               // bumped on every change, so pages know to redraw
static std::atomic<ULONGLONG> g_nextAutoCheck{0};  // GetTickCount64() of the next automatic check

static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::wstring ToolPath() { return Engine_Dir() + L"\\diatweaks.exe"; }

// Runs diatweaks.exe with these arguments, hidden, and returns what it printed.
static std::string ToolOutput(const std::wstring& args) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    // Hand the child only the pipe, none of Dia's other inheritable handles.
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attrs(size);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    InitializeProcThreadAttributeList(list, 1, 0, &size);
    UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &wr, sizeof(wr), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = wr;
    si.lpAttributeList = list;
    std::wstring exe = ToolPath(), cmd = L"\"" + exe + L"\" " + args;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                             nullptr, Engine_Dir().c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(list);
    CloseHandle(wr);
    if (!ok) {
        Log("updates: can't run diatweaks.exe (%lu)", GetLastError());
        CloseHandle(rd);
        return {};
    }
    // Never wait forever on a stuck network.
    HANDLE watch = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &watch, 0, FALSE, DUPLICATE_SAME_ACCESS))
        std::thread([watch] {
            if (WaitForSingleObject(watch, 90000) == WAIT_TIMEOUT) TerminateProcess(watch, 1);
            CloseHandle(watch);
        }).detach();
    std::string out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return out;
}

static void StartUpdateCheck() {
    {
        std::lock_guard<std::mutex> lk(g_updMx);
        if (g_upd.checking) return;
        g_upd.checking = true;
    }
    g_updGen++;
    std::thread([] {
        std::map<std::string, std::wstring> found;
        std::istringstream in(ToolOutput(L"check"));
        std::string line;
        while (std::getline(in, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string v;
            for (size_t i = eq + 1; i < line.size(); i++) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    char c = line[++i];
                    v += c == 'n' ? '\n' : c;
                } else if (line[i] != '\r') {
                    v += line[i];
                }
            }
            found[line.substr(0, eq)] = FromUtf8(v);
        }
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t when[16];
        swprintf(when, 16, L"%02u:%02u", t.wHour, t.wMinute);
        {
            std::lock_guard<std::mutex> lk(g_updMx);
            g_upd.found = found;
            g_upd.checking = false;
            g_upd.checked = true;
            g_upd.when = when;
        }
        Log("updates: Dia %s -> %s, WinDiaTweaks %s -> %s", N(found["dia.installed"]).c_str(),
            N(found["dia.latest"]).c_str(), N(found["tweaks.installed"]).c_str(), N(found["tweaks.latest"]).c_str());
        g_updGen++;
    }).detach();
}

static UpdateState UpdateSnapshot() {
    std::lock_guard<std::mutex> lk(g_updMx);
    return g_upd;
}

// "Check for updates automatically": shortly after Dia opens, then every 6 hours it stays open.
static void MaybeAutoCheck() {
    if (!Engine_TweakOn(L"checkForUpdates") || Engine_SafeMode()) return;
    ULONGLONG now = GetTickCount64(), next = g_nextAutoCheck;
    if (next == 0) {
        g_nextAutoCheck.compare_exchange_strong(next, now + 20000);
        return;
    }
    if (now >= next && g_nextAutoCheck.compare_exchange_strong(next, now + 6ull * 3600 * 1000)) StartUpdateCheck();
}

// Runs `diatweaks <args>` in its own console window. Through `detach`, so the window's process is
// started outside Dia's package: Dia's package would close it together with Dia.
static bool RunToolWindow(const std::wstring& args) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attrs(size);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    InitializeProcThreadAttributeList(list, 1, 0, &size);
    DWORD policy = PROCESS_CREATION_DESKTOP_APP_BREAKAWAY_ENABLE_PROCESS_TREE;
    bool haveAttr = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_DESKTOP_APP_POLICY, &policy, sizeof(policy),
                                              nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    if (haveAttr) si.lpAttributeList = list;
    std::wstring exe = ToolPath(), cmd = L"\"" + exe + L"\" detach " + args;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW | (haveAttr ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
                             Engine_Dir().c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(list);
    if (!ok) {
        Log("updates: can't start diatweaks %s (%lu)", N(args).c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void OpenUrl(const std::wstring& url) {
    if (!url.empty()) ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Release notes are Markdown; show them as plain text.
static std::wstring PlainNotes(const std::wstring& md) {
    std::wistringstream in(md);
    std::wstring line, out;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty() && line.back() == L'\\') line.pop_back();  // Markdown line break
        size_t a = line.find_first_not_of(L" \t");
        // Pictures, tables and code fences don't read as text: leave them to the release page.
        if (a != std::wstring::npos && (line[a] == L'<' || line[a] == L'|' || line.compare(a, 2, L"![") == 0 ||
                                        line.compare(a, 3, L"```") == 0))
            continue;
        if (a != std::wstring::npos && line[a] == L'#') {
            size_t text = line.find_first_not_of(L"# \t", a);
            line = text == std::wstring::npos ? L"" : line.substr(text);
        }
        else if (a != std::wstring::npos && (line.compare(a, 2, L"- ") == 0 || line.compare(a, 2, L"* ") == 0))
            line = line.substr(0, a) + L"• " + line.substr(a + 2);
        std::wstring clean;
        for (size_t i = 0; i < line.size(); i++) {
            if (line.compare(i, 2, L"**") == 0) {
                i++;
                continue;
            }
            if (line[i] == L'`') continue;
            if (line[i] == L'[') {  // [text](url) -> text
                size_t close = line.find(L"](", i), end = close == std::wstring::npos ? close : line.find(L')', close);
                if (end != std::wstring::npos) {
                    clean += line.substr(i + 1, close - i - 1);
                    i = end;
                    continue;
                }
            }
            clean += line[i];
        }
        if (clean.empty() && (out.empty() || out.size() >= 2 && out.compare(out.size() - 2, 2, L"\n\n") == 0)) continue;
        out += clean + L"\n";
    }
    while (!out.empty() && out.back() == L'\n') out.pop_back();
    return out;
}


// A row with an optional button (empty label: none).
static mux::FrameworkElement UpdateRow(const wchar_t* glyph, const std::wstring& title, const std::wstring& desc,
                                       const std::wstring& button, std::function<void()> onClick, bool accent = false) {
    auto row = ButtonRow(glyph, title, desc, button.empty() ? L"-" : button, onClick).as<muxc::Grid>();
    auto b = row.Children().GetAt(row.Children().Size() - 1).as<muxc::Button>();
    if (button.empty()) row.Children().RemoveAtEnd();
    else if (accent)
        if (auto style = Resource(L"AccentButtonStyle").try_as<mux::Style>()) b.Style(style);
    return row;
}

static void FillUpdates(muxc::Border host) {
    const int generation = g_updGen;
    UpdateState u = UpdateSnapshot();
    auto get = [&](const char* k) { return u.found.count(k) ? u.found[k] : std::wstring(); };
    std::vector<mux::FrameworkElement> rows;
    bool noAnswer = u.checked && get("tweaks.installed").empty();  // diatweaks.exe missing or failed

    // "Updating..." on the button, so a second click doesn't start a second update.
    auto starting = [](const std::wstring& args) {
        return [args] {
            if (!RunToolWindow(args)) return;
            std::lock_guard<std::mutex> lk(g_updMx);
            g_upd.found["busy"] = args;
            g_updGen++;
        };
    };
    bool busy = !get("busy").empty();

    bool diaNew = get("dia.available") == L"1", tweaksNew = get("tweaks.available") == L"1";
    if (diaNew && tweaksNew)
        rows.push_back(UpdateRow(L"\uE896", L"Dia and WinDiaTweaks both have updates",
                                 L"Dia restarts to install them.",
                                 busy ? L"Updating..." : L"Update both", starting(L"update"), true));

    // Dia
    std::wstring diaDesc, diaButton = L"Changelog";
    std::function<void()> diaClick = [] { OpenUrl(L"https://www.diabrowser.com/changelog"); };
    if (!u.checked) diaDesc = u.checking ? L"Checking for updates..." : L"Not checked yet.";
    else if (noAnswer) diaDesc = L"Couldn't check: diatweaks.exe didn't answer. See tap.log.";
    else if (get("dia.installed").empty()) diaDesc = L"Couldn't find Dia's version: " + get("dia.error") + L".";
    else if (diaNew) {
        diaDesc = L"Dia " + get("dia.latest") + L" is available.";
        diaButton = busy ? L"Updating..." : L"Update Dia";
        diaClick = starting(L"update dia");
    } else if (get("dia.latest").empty())
        diaDesc = L"Version " + get("dia.installed") + L". Couldn't check for a newer one: " + get("dia.error") + L".";
    else diaDesc = L"Version " + get("dia.installed") + L". Up to date.";
    rows.push_back(UpdateRow(L"\uE774", L"Dia", diaDesc, diaButton, diaClick, diaNew && !busy));

    // WinDiaTweaks
    std::wstring have = get("tweaks.installed"), latest = get("tweaks.latest"), url = get("tweaks.url");
    std::wstring twDesc, twButton = L"Release notes";
    std::function<void()> twClick = [url] { OpenUrl(url.empty() ? L"https://github.com/zioder/WinDiaTweaks/releases" : url); };
    if (!u.checked) twDesc = u.checking ? L"Checking for updates..." : L"Not checked yet.";
    else if (noAnswer) twDesc = L"Couldn't check.";
    else if (!get("tweaks.dev").empty())
        twDesc = L"Development build " + have + L" (" + get("tweaks.dev") + L").";
    else if (tweaksNew) {
        twDesc = L"WinDiaTweaks " + latest + L" is available.";
        twButton = busy ? L"Updating..." : L"Update";
        twClick = starting(L"update tweaks");
    } else if (latest.empty())
        twDesc = L"Version " + have + L". " +
                 (get("tweaks.error") == L"no releases yet" ? std::wstring(L"No releases published yet.")
                                                            : L"Couldn't check for a newer one: " + get("tweaks.error") + L".");
    else twDesc = L"Version " + have + L". Up to date.";
    auto twRow = UpdateRow(L"\uE945", L"WinDiaTweaks", twDesc, twButton, twClick, tweaksNew && !busy);
    std::wstring notes = PlainNotes(get("tweaks.notes"));
    if (!notes.empty()) {
        muxc::StackPanel withNotes;
        withNotes.Children().Append(twRow);
        muxc::StackPanel box;
        box.Padding({40, 0, 12, 12});
        box.Spacing(4);
        muxc::TextBlock head;
        head.Text((tweaksNew ? L"What's new in " : L"What's new in WinDiaTweaks ") + latest);
        head.FontSize(12);
        head.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
        box.Children().Append(head);
        muxc::TextBlock body;
        body.Text(notes);
        body.FontSize(12);
        body.TextWrapping(mux::TextWrapping::Wrap);
        body.MaxLines(16);
        body.TextTrimming(mux::TextTrimming::CharacterEllipsis);
        body.IsTextSelectionEnabled(true);
        body.Foreground(BrushRes(L"TextFillColorSecondaryBrush", Rgba(0xc5, 0xc5, 0xc5)));
        box.Children().Append(body);
        withNotes.Children().Append(box);
        rows.push_back(withNotes);
    } else {
        rows.push_back(twRow);
    }

    // Settings
    for (auto& t : Engine_Tweaks())
        if (t.key == L"checkForUpdates") rows.push_back(Row(t));
    std::wstring when = u.checking ? L"Checking..." : u.checked ? L"Last checked at " + u.when + L"." : L"Not checked yet.";
    auto check = UpdateRow(L"\uE895", L"Check for updates", when, L"Check now", [] { StartUpdateCheck(); });
    if (u.checking) check.as<muxc::Grid>().Children().GetAt(2).as<muxc::Button>().IsEnabled(false);
    rows.push_back(check);

    muxc::StackPanel list;
    for (size_t i = 0; i < rows.size(); i++) {
        if (i) {
            muxc::Border line;
            line.Height(1);
            line.Background(t_card.border);
            list.Children().Append(line);
        }
        list.Children().Append(rows[i]);
    }
    host.Child(list);
    host.Tag(box_value(generation));
}

static mux::FrameworkElement UpdatesSection(NavHook& hook) {
    muxc::Border host;
    hook.updatesHost = host;
    {
        std::lock_guard<std::mutex> lk(g_updMx);
        g_upd.found.erase("busy");  // a fresh page: show the buttons again
    }
    UpdateState u = UpdateSnapshot();
    if (!u.checked && !u.checking) StartUpdateCheck();
    FillUpdates(host);
    return Section(L"Updates", {host});
}

// Redraws the Updates section when a check finishes, and badges the Patches item when there's an update.
static void SyncUpdates() {
    MaybeAutoCheck();
    int gen = g_updGen;
    for (auto& h : t_navs) {
        if (!h.updatesHost || unbox_value_or<int>(h.updatesHost.Tag(), -1) == gen) continue;
        // Only the Patches page on screen; a hidden one is rebuilt when it's shown again.
        auto nav = h.nav.get();
        if (!nav || !h.page || nav.Content() != h.page) continue;
        try {
            FillUpdates(h.updatesHost);
        } catch (...) {
            // Don't retry every tick (and starve the badge below); try again on the next change.
            Log("updates: can't redraw the Updates section");
            h.updatesHost.Tag(box_value(gen));
        }
    }
    UpdateState u = UpdateSnapshot();
    int count = (u.found["dia.available"] == L"1") + (u.found["tweaks.available"] == L"1");
    for (auto& h : t_navs) {
        if (!h.item) continue;
        try {
            auto badge = h.item.InfoBadge();
            if (!count && badge) h.item.InfoBadge(nullptr);
            else if (count && (!badge || badge.Value() != count)) {
                muxc::InfoBadge b;
                b.Value(count);
                h.item.InfoBadge(b);
            }
        } catch (...) {
        }
    }
}

static mux::FrameworkElement BuildPage(NavHook& hook) {
    EnsureCardLook();
    t_toggles.clear();

    muxc::Grid root;
    root.Padding({24, 44, 24, 24});
    muxc::RowDefinition r0, r1;
    r0.Height(mux::GridLengthHelper::Auto());
    r1.Height({1, mux::GridUnitType::Star});
    root.RowDefinitions().Append(r0);
    root.RowDefinitions().Append(r1);

    muxc::TextBlock title;
    title.Text(L"Patches");
    title.FontSize(20);
    title.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
    title.Margin({16, 0, 0, 24});
    root.Children().Append(title);

    muxc::ScrollViewer scroll;
    scroll.Margin({0, 0, -24, 0});
    scroll.Padding({0, 0, 24, 0});
    scroll.HorizontalScrollBarVisibility(muxc::ScrollBarVisibility::Disabled);
    muxc::Grid::SetRow(scroll, 1);
    muxc::StackPanel stack;
    stack.Spacing(16);
    stack.Padding({0, 0, 0, 24});

    if (Engine_SafeMode()) {
        stack.Children().Append(Section(
            L"Safe mode",
            {ButtonRow(L"", L"Patches are off: Dia closed twice right after opening",
                       L"Turn them back on when ready.", L"Turn patches back on", [] {
                           Engine_LeaveSafeMode();
                       })}));
    }

    stack.Children().Append(UpdatesSection(hook));
    stack.Children().Append(Section(L"Preset", {PresetRow()}));

    std::vector<std::wstring> order;
    std::map<std::wstring, std::vector<mux::FrameworkElement>> sections;
    for (auto& t : Engine_Tweaks()) {
        if (t.key == L"preset" || t.key == L"checkForUpdates") continue;  // shown on top, under Preset / Updates
        std::wstring s = t.section.empty() ? L"Other" : t.section;
        if (!sections.count(s)) order.push_back(s);
        sections[s].push_back(Row(t));
    }
    for (auto& s : order) stack.Children().Append(Section(s, sections[s]));

    stack.Children().Append(Section(
        L"DiaTweaks",
        {ButtonRow(L"", L"Turn off visual patches", L"Keeps browsing, launch, and update settings.",
                   L"Turn visuals off", [] { ApplyPreset(L"none"); }),
         ButtonRow(L"", L"DiaTweaks for Windows",
                   L"Unofficial, not affiliated with The Browser Company.",
                   L"Open folder", [] {
                       ShellExecuteW(nullptr, L"open", Engine_Dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                   })}));

    scroll.Content(stack);
    root.Children().Append(scroll);
    return root;
}

// Keeps switches in step with tweaks.conf after config changes.
static void RebuildShownPages();
static void SyncToggles() {
    static thread_local bool lastSafe = Engine_SafeMode();
    if (lastSafe != Engine_SafeMode()) {
        lastSafe = Engine_SafeMode();
        RebuildShownPages();
        return;
    }
    t_syncing = true;
    for (auto& b : t_toggles)
        if (auto sw = b.sw.get()) {
            bool want = Engine_TweakOn(b.key);
            if (sw.IsOn() != want) sw.IsOn(want);
            sw.IsEnabled(!Engine_SafeMode());
        }
    for (auto& c : t_choiceSwitches)
        if (auto box = c.box.get()) {
            int want = ChoiceIndex(c.key);
            if (box.SelectedIndex() != want) box.SelectedIndex(want);
            box.IsEnabled(!Engine_SafeMode());
        }
    if (auto box = t_presetBox.get())
        if (box.SelectedIndex() != PresetIndex()) box.SelectedIndex(PresetIndex());
    t_syncing = false;
}

static bool IsOurs(const NavHook& h, const muxc::NavigationViewSelectionChangedEventArgs& a) {
    if (!h.item) return false;
    if (auto c = a.SelectedItemContainer()) return c == h.item;
    return a.SelectedItem() == h.item;
}

static void RebuildShownPages() {
    for (auto& h : t_navs)
        if (auto nav = h.nav.get(); nav && h.page && nav.Content() == h.page) {
            h.page = BuildPage(h);
            nav.Content(h.page);
        }
}

static void HookNav(InstanceHandle navH) {
    for (auto& h : t_navs)
        if (h.handle == navH && h.nav.get()) return;
    auto nav = Obj<muxc::NavigationView>(navH);
    if (!nav) return;

    // Already there (e.g. after a config reload)?
    for (auto const& it : nav.MenuItems())
        if (auto fe = it.try_as<mux::FrameworkElement>(); fe && fe.Name() == L"diatweaksPatches") return;

    muxc::NavigationViewItem item;
    item.Name(L"diatweaksPatches");
    item.Content(box_value(L"Patches"));
    muxc::FontIcon icon;
    icon.Glyph(L"");  // puzzle piece
    item.Icon(icon);
    item.Tag(box_value(L"patches"));

    try {
        auto items = nav.MenuItems();
        uint32_t at = items.Size();
        for (uint32_t i = 0; i < items.Size(); i++)
            if (auto fe = items.GetAt(i).try_as<mux::FrameworkElement>(); fe && fe.Name() == L"advanced") at = i + 1;
        items.InsertAt(at, item);
    } catch (winrt::hresult_error const& e) {
        Log("Patches: can't add to Settings menu (0x%08lx)", (unsigned long)e.code());
        return;
    }

    NavHook hook;
    hook.nav = winrt::make_weak(nav);
    hook.item = item;
    hook.handle = navH;
    t_navs.push_back(hook);

    if (auto c = nav.Content().try_as<mux::DependencyObject>()) CaptureCardLook(c);

    nav.SelectionChanged([navH](muxc::NavigationView const& sender,
                                muxc::NavigationViewSelectionChangedEventArgs const& a) {
        for (auto& h : t_navs) {
            if (h.handle != navH) continue;
            if (IsOurs(h, a)) {
                auto cur = sender.Content();
                if (!h.page || cur != h.page) h.saved = cur;
                if (auto c = cur.try_as<mux::DependencyObject>()) CaptureCardLook(c);
                h.page = BuildPage(h);
                sender.Content(h.page);
            } else if (h.page && sender.Content() == h.page) {
                sender.Content(h.saved);  // Dia switched pages inside its own content; show it again
            }
        }
    });
    Log("Patches: added to Settings");
}

static void ScanForSettings() {
    // Dia's Settings window is a NavigationView with an item named "advanced".
    for (auto adv : FindNodes(L"advanced")) {
        InstanceHandle p = Engine_Parent(adv);
        for (int i = 0; p && i < 40; i++, p = Engine_Parent(p)) {
            if (Engine_Type(p) == L"Microsoft.UI.Xaml.Controls.NavigationView") {
                HookNav(p);
                break;
            }
        }
    }
    t_navs.erase(std::remove_if(t_navs.begin(), t_navs.end(), [](const NavHook& h) { return !h.nav.get(); }),
                 t_navs.end());
}

// ---------------------------------------------------------------- states for rules

static thread_local std::map<std::wstring, bool> t_states;

bool Ui_State(const std::wstring& name) {
    auto it = t_states.find(name);
    return it != t_states.end() && it->second;
}

// Visible on screen: the element and every element above it are Visible.
static bool ShownOnScreen(mux::FrameworkElement fe) {
    for (mux::DependencyObject d = fe; d; d = muxm::VisualTreeHelper::GetParent(d))
        if (auto u = d.try_as<mux::UIElement>(); u && u.Visibility() != mux::Visibility::Visible) return false;
    return true;
}

// The sidebar is open when it's shown, docked or floating over the page. Closing it collapses the
// grid that holds it.
static bool ComputeSidebarOpen() {
    for (auto h : FindNodes(L"SidebarBorder")) {
        auto fe = Obj<mux::FrameworkElement>(h);
        if (fe && fe.ActualWidth() >= 1 && ShownOnScreen(fe)) return true;
    }
    return false;
}

// A new tab shows Dia's start page (the "Ask anything" page with its cards) instead of a website.
static bool ComputeNewTab() {
    for (auto h : FindNodes(L"CardCanvas")) {
        auto fe = Obj<mux::FrameworkElement>(h);
        if (fe && fe.ActualWidth() >= 1 && ShownOnScreen(fe)) return true;
    }
    for (auto h : FindNodes(L"AddressRichEditBox")) {
        auto reb = Obj<muxc::RichEditBox>(h);
        if (!reb) continue;
        winrt::hstring t;
        reb.Document().GetText(winrt::Microsoft::UI::Text::TextGetOptions::None, t);
        std::wstring s(t);
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        if (s.empty()) return true;
    }
    return false;
}

static bool UpdateStates() {
    std::map<std::wstring, bool> now;
    now[L"sidebarOpen"] = ComputeSidebarOpen();
    now[L"newTab"] = ComputeNewTab();
    {  // split view: more than one page toolbar on screen
        int bars = 0;
        for (auto h : FindNodes(L"NavigationBarContent"))
            if (auto fe = Obj<mux::FrameworkElement>(h); fe && ShownOnScreen(fe)) bars++;
        now[L"split"] = bars > 1;
    }
    bool changed = now != t_states;
    t_states = now;
    return changed;
}

// ---------------------------------------------------------------- code-driven tweaks

// The main browser windows on this thread: the DesktopWindowXamlSource roots that hold a sidebar.
static std::vector<InstanceHandle> BrowserRoots() {
    std::vector<InstanceHandle> roots;
    for (auto sb : FindNodes(L"SidebarBorder")) {
        InstanceHandle p = sb, last = sb;
        while (p) last = p, p = Engine_Parent(p);
        if (Engine_Type(last) == L"Microsoft.UI.Xaml.Hosting.DesktopWindowXamlSource" &&
            std::find(roots.begin(), roots.end(), last) == roots.end())
            roots.push_back(last);
    }
    return roots;
}

// The element directly under the window's ClientAreaPresenter: it paints the sidebar/tab-bar color.
static muxc::Panel WindowBackgroundPanel(InstanceHandle root) {
    InstanceHandle found = 0;
    Engine_ForEachNode([&](const NodeRef& r) {
        if (found || r.type != L"Microsoft.UI.Xaml.Controls.Grid") return;
        InstanceHandle up = Engine_Parent(r.handle);
        if (Engine_Name(up) != L"ClientAreaPresenter") return;
        InstanceHandle p = up, last = up;
        while (p) last = p, p = Engine_Parent(p);
        if (last == root) found = r.handle;
    });
    return Obj<muxc::Panel>(found);
}

// --- Window material: Mica / Mica Alt / Acrylic behind the tabs and sidebar.
struct MaterialState {
    winrt::weak_ref<winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource> src;
    winrt::weak_ref<muxc::Panel> panel;
    muxm::Brush savedBrush{nullptr};
    muxm::Brush clear{muxm::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0})};
    std::wstring applied;
    InstanceHandle root = 0;
};
static thread_local std::vector<MaterialState> t_material;

static void UpdateWindowMaterial() {
    bool on = Engine_TweakOn(L"windowMaterial");
    std::wstring kind = Engine_TweakValue(L"windowMaterial");
    if (kind.empty()) kind = L"mica";
    if (on) {
        for (auto root : BrowserRoots()) {
            bool known = false;
            for (auto& m : t_material) known |= m.root == root;
            if (known) continue;
            MaterialState m;
            m.root = root;
            m.src = winrt::make_weak(Obj<winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource>(root));
            t_material.push_back(m);
        }
    }
    for (auto& m : t_material) {
        auto src = m.src.get();
        if (!src) continue;
        std::wstring want = on ? kind : L"";
        if (m.applied == want) {
            // Dia may repaint the background; keep it see-through.
            if (on)
                if (auto p = m.panel.get(); p && p.Background() != m.clear) p.Background(m.clear);
            continue;
        }
        auto panel = m.panel.get();
        if (!panel) {
            panel = WindowBackgroundPanel(m.root);
            if (panel) m.panel = winrt::make_weak(panel), m.savedBrush = panel.Background();
        }
        if (want.empty()) {
            src.SystemBackdrop(nullptr);
            if (panel) panel.Background(m.savedBrush);
        } else {
            if (want == L"acrylic") src.SystemBackdrop(muxm::DesktopAcrylicBackdrop());
            else {
                muxm::MicaBackdrop mica;
                if (want == L"micaAlt") mica.Kind(winrt::Microsoft::UI::Composition::SystemBackdrops::MicaKind::BaseAlt);
                src.SystemBackdrop(mica);
            }
            if (panel) panel.Background(m.clear);
        }
        m.applied = want;
        Log("window material: %s", want.empty() ? "off" : N(want).c_str());
    }
    if (!on)
        t_material.erase(std::remove_if(t_material.begin(), t_material.end(),
                                        [](const MaterialState& m) { return m.applied.empty(); }),
                         t_material.end());
}

// --- Toolbar color: Dia paints the toolbar strip (and tints its icons and the window buttons)
// in the site's color. With this on, the strip is see-through, so the sidebar's color or the window
// material behind it shows, and icons and text take the sidebar's text color.
struct LookSaved {
    winrt::weak_ref<mux::DependencyObject> el;
    mux::DependencyProperty dp{nullptr};
    wf::IInspectable value{nullptr};          // Dia's local value; null = none
    mux::Data::BindingBase binding{nullptr};  // or Dia's binding
    int64_t token = 0;
};
static thread_local std::map<std::pair<void*, void*>, LookSaved> t_look;
static thread_local muxm::Brush t_lookText{nullptr};
static thread_local bool t_lookPending = false;
static thread_local winrt::event_token t_lookToken{};

// On with "sidebar"; "off" (Adaptive) also covers `diatweaks set toolbarColor off`.
static bool ToolbarSidebar() { return Engine_TweakOn(L"toolbarColor") && Engine_TweakValue(L"toolbarColor") != L"off"; }

static muxm::Brush LookClear() {
    static thread_local muxm::Brush b = muxm::SolidColorBrush(Rgba(0, 0, 0, 0));
    return b;
}

// The sidebar's text color, taken from Dia's ⌄ button in the sidebar header (as the Arc URL bar does).
static muxm::Brush SidebarTextBrush() {
    for (auto h : FindNodes(L"OverflowButton")) {
        bool inHeader = false;
        for (auto p = Engine_Parent(h); p && !inHeader; p = Engine_Parent(p)) inHeader = Engine_Name(p) == L"WindowDragHeader";
        if (!inHeader) continue;
        if (auto c = Obj<muxc::Control>(h))
            if (auto b = c.Foreground()) return b;
    }
    return BrushRes(L"TextFillColorPrimaryBrush", Rgba(0xff, 0xff, 0xff));
}

static mux::DependencyProperty LookForegroundDp(const mux::DependencyObject& d) {
    if (d.try_as<muxc::Control>()) return muxc::Control::ForegroundProperty();
    if (d.try_as<muxc::IconElement>()) return muxc::IconElement::ForegroundProperty();
    if (d.try_as<muxc::TextBlock>()) return muxc::TextBlock::ForegroundProperty();
    if (d.try_as<muxc::ContentPresenter>()) return muxc::ContentPresenter::ForegroundProperty();
    return nullptr;
}

static muxm::Brush LookWant(const mux::DependencyProperty& dp) {
    if (dp == muxc::Panel::BackgroundProperty()) return LookClear();
    return t_lookText;  // foregrounds and icon fills
}

// Remember Dia's value (local or bound) for this property.
static void LookRemember(LookSaved& s, const mux::DependencyObject& d) {
    s.value = nullptr, s.binding = nullptr;
    if (auto fe = d.try_as<mux::FrameworkElement>())
        if (auto be = fe.GetBindingExpression(s.dp)) {
            s.binding = be.ParentBinding();
            return;
        }
    auto v = d.ReadLocalValue(s.dp);
    if (v && v != mux::DependencyProperty::UnsetValue()) s.value = v;
}

static void LookOverride(const mux::DependencyObject& d, const mux::DependencyProperty& dp) {
    auto key = std::make_pair(winrt::get_abi(d), winrt::get_abi(dp));
    auto want = LookWant(dp);
    if (!want) return;
    if (!t_look.count(key)) {
        LookSaved s;
        s.el = winrt::make_weak(d);
        s.dp = dp;
        LookRemember(s, d);
        // Dia re-tints when the site changes: put the sidebar look straight back, before the frame is drawn,
        // and keep Dia's new value for when the tweak is turned off.
        s.token = d.RegisterPropertyChangedCallback(dp, [key](mux::DependencyObject const& o, mux::DependencyProperty const& p) {
            if (!ToolbarSidebar()) return;
            auto it = t_look.find(key);
            auto want = LookWant(p);
            if (it == t_look.end() || !want || o.ReadLocalValue(p) == want) return;
            LookRemember(it->second, o);
            o.SetValue(p, want);
        });
        t_look[key] = s;
    }
    if (d.ReadLocalValue(dp) != want) d.SetValue(dp, want);
}

// Dia keeps each toolbar's site palette in the strip's own resources (NavigationBarBackgroundBrush,
// ButtonBackgroundPointerOver, SubtleFillColorSecondaryBrush for the address field's hover,
// ControlFillColorDefaultBrush for the Chat button, ...). Those brushes are recolored to the app's own
// values for the same keys, the ones the sidebar uses, so hover and pressed states look like the sidebar's.
struct PaletteSaved {
    winrt::weak_ref<muxm::SolidColorBrush> brush;
    winrt::Windows::UI::Color dia{}, want{};
    int64_t token = 0;
};
static thread_local std::map<void*, PaletteSaved> t_palette;

static void LookPalette(const mux::FrameworkElement& strip) {
    for (auto const& kv : strip.Resources()) {
        auto key = kv.Key().try_as<wf::IReference<winrt::hstring>>();
        auto b = kv.Value().try_as<muxm::SolidColorBrush>();
        if (!key || !b) continue;
        winrt::Windows::UI::Color want{};  // the strip itself: see-through
        if (key.Value() != L"NavigationBarBackgroundBrush") {
            auto app = Resource(key.Value().c_str()).try_as<muxm::SolidColorBrush>();
            if (!app || app == b) continue;
            want = app.Color();
        }
        void* id = winrt::get_abi(b);
        auto it = t_palette.find(id);
        if (it == t_palette.end()) {
            PaletteSaved p;
            p.brush = winrt::make_weak(b);
            p.dia = b.Color();
            // Dia recolors the palette when the site changes: keep its color for later, put ours back.
            p.token = b.RegisterPropertyChangedCallback(muxm::SolidColorBrush::ColorProperty(),
                                                        [id](mux::DependencyObject const& o, auto&&) {
                                                            auto it = t_palette.find(id);
                                                            if (it == t_palette.end()) return;
                                                            auto br = o.as<muxm::SolidColorBrush>();
                                                            if (br.Color() == it->second.want) return;
                                                            it->second.dia = br.Color();
                                                            br.Color(it->second.want);
                                                        });
            it = t_palette.emplace(id, p).first;
        }
        it->second.want = want;
        if (b.Color() != want) b.Color(want);
    }
}

// The address field: on hover Dia fills it with a site-colored brush of its own; use the sidebar's hover fill.
struct FieldHover {
    winrt::weak_ref<muxc::Border> bar;
    muxm::Brush dia{nullptr};
    int64_t token = 0;
};
static thread_local std::map<void*, FieldHover> t_fieldHover;

static muxm::Brush SidebarHoverBrush() {
    return BrushRes(L"SubtleFillColorSecondaryBrush", Rgba(0xff, 0xff, 0xff, 0x0f));
}

static bool Opaque(const muxm::Brush& b) {
    auto s = b.try_as<muxm::SolidColorBrush>();
    return s && s.Color().A == 255;
}

static void LookField(const muxc::Border& bar) {
    void* id = winrt::get_abi(bar);
    if (!t_fieldHover.count(id)) {
        FieldHover f;
        f.bar = winrt::make_weak(bar);
        f.dia = bar.Background();
        f.token = bar.RegisterPropertyChangedCallback(muxc::Border::BackgroundProperty(), [id](mux::DependencyObject const& o, auto&&) {
            auto it = t_fieldHover.find(id);
            if (it == t_fieldHover.end()) return;
            auto b = o.as<muxc::Border>();
            auto cur = b.Background();
            if (cur == SidebarHoverBrush()) return;
            it->second.dia = cur;
            if (Opaque(cur)) b.Background(SidebarHoverBrush());
        });
        t_fieldHover[id] = f;
    }
    if (Opaque(bar.Background())) bar.Background(SidebarHoverBrush());
}

// The address text: Dia colors it run by run inside the text (the site part dim, the page title bright,
// both in site colors). Sidebar: the bright part gets the sidebar's text color, the dim part its secondary
// text color. Left alone while you type.
struct AddressRun {
    int start, end;
    winrt::Windows::UI::Color color;
};
struct AddressSaved {
    winrt::weak_ref<muxc::RichEditBox> box;
    winrt::hstring text;
    std::vector<AddressRun> dia;  // Dia's colors, for Adaptive
};
static thread_local std::map<void*, AddressSaved> t_address;
static thread_local std::set<void*> t_addressHooked;
static void ScheduleToolbarLook();
static winrt::Windows::UI::Color SiteColorFor(mux::DependencyObject d);

static winrt::Windows::UI::Color BrushColor(const muxm::Brush& b, winrt::Windows::UI::Color fallback) {
    if (auto s = b.try_as<muxm::SolidColorBrush>()) return s.Color();
    return fallback;
}

static std::vector<AddressRun> AddressRuns(const winrt::Microsoft::UI::Text::RichEditTextDocument& doc, int len) {
    std::vector<AddressRun> runs;
    for (int i = 0; i < len && runs.size() < 64;) {
        auto r = doc.GetRange(i, i + 1);
        r.Expand(winrt::Microsoft::UI::Text::TextRangeUnit::CharacterFormat);
        int end = (std::max)(r.EndPosition(), i + 1);
        AddressRun run{(std::max)(r.StartPosition(), i), end, r.CharacterFormat().ForegroundColor()};
        runs.push_back(run);
        i = end;
    }
    return runs;
}

static void LookAddressText(const muxc::RichEditBox& box) {
    void* id = winrt::get_abi(box);
    if (!t_address.count(id)) {
        AddressSaved a;
        a.box = winrt::make_weak(box);
        t_address[id] = a;
    }
    if (t_addressHooked.insert(id).second) {
        // Dia rewrites the address (and its colors) on navigation and when you leave the field.
        box.TextChanged([](auto&&, auto&&) { ScheduleToolbarLook(); });
        box.LostFocus([](auto&&, auto&&) { ScheduleToolbarLook(); });
    }
    if (box.FocusState() != mux::FocusState::Unfocused) return;
    auto doc = box.Document();
    winrt::hstring text;
    doc.GetText(winrt::Microsoft::UI::Text::TextGetOptions::None, text);
    auto primary = BrushColor(t_lookText, Rgba(0xff, 0xff, 0xff));
    auto secondary = BrushColor(BrushRes(L"TextFillColorSecondaryBrush", Rgba(0xc5, 0xc5, 0xc5)), Rgba(0xc5, 0xc5, 0xc5));
    int under = primary.R + primary.G + primary.B > 384 ? 0x2b : 0xf3;  // the bar behind: dark or light theme
    secondary = {255, (uint8_t)((secondary.R * secondary.A + under * (255 - secondary.A)) / 255),
                 (uint8_t)((secondary.G * secondary.A + under * (255 - secondary.A)) / 255),
                 (uint8_t)((secondary.B * secondary.A + under * (255 - secondary.A)) / 255)};
    primary.A = 255;
    auto runs = AddressRuns(doc, (int)text.size());
    bool plain = true;
    for (auto& r : runs) plain &= r.color == primary || r.color == secondary;
    if (plain) return;
    // The part Dia emphasizes stands out most from the site's color (darker on light sites, lighter on dark).
    auto lum = [](winrt::Windows::UI::Color c) { return 0.2126 * c.R + 0.7152 * c.G + 0.0722 * c.B; };
    double site = lum(SiteColorFor(box));
    auto contrast = [&](winrt::Windows::UI::Color c) { return std::abs(lum(c) - site); };
    double top = 0;
    for (auto& r : runs)
        if (r.end - r.start > 1 || runs.size() == 1) top = (std::max)(top, contrast(r.color));  // (not the trailing newline)
    auto& saved = t_address[id];
    saved.text = text;
    saved.dia = runs;
    doc.BatchDisplayUpdates();
    for (auto& r : runs) {
        auto range = doc.GetRange(r.start, r.end);
        range.CharacterFormat().ForegroundColor(contrast(r.color) >= top - 1 ? primary : secondary);
    }
    doc.ApplyDisplayUpdates();
}

static void AddressRestoreAll() {
    for (auto& [id, a] : t_address) {
        auto box = a.box.get();
        if (!box || a.dia.empty()) continue;
        auto doc = box.Document();
        winrt::hstring text;
        doc.GetText(winrt::Microsoft::UI::Text::TextGetOptions::None, text);
        if (text != a.text) continue;  // Dia has written it anew: its own colors already
        doc.BatchDisplayUpdates();
        for (auto& r : a.dia) doc.GetRange(r.start, r.end).CharacterFormat().ForegroundColor(r.color);
        doc.ApplyDisplayUpdates();
    }
    t_address.clear();
}

// Dia's color for the site in the toolbar this element is in (kept from before the palette was recolored).
static winrt::Windows::UI::Color SiteColorFor(mux::DependencyObject d) {
    auto key = box_value(L"NavigationBarBackgroundBrush");
    for (int i = 0; d && i < 40; i++, d = muxm::VisualTreeHelper::GetParent(d))
        if (auto fe = d.try_as<mux::FrameworkElement>(); fe && fe.Resources().HasKey(key))
            if (auto b = fe.Resources().Lookup(key).try_as<muxm::SolidColorBrush>()) {
                auto it = t_palette.find(winrt::get_abi(b));
                return it != t_palette.end() ? it->second.dia : b.Color();
            }
    return Rgba(0x2b, 0x2b, 0x2b);
}

static void PaletteRestoreAll() {
    AddressRestoreAll();
    for (auto& [id, f] : t_fieldHover)
        if (auto bar = f.bar.get()) {
            bar.UnregisterPropertyChangedCallback(muxc::Border::BackgroundProperty(), f.token);
            if (bar.Background() == SidebarHoverBrush()) bar.Background(f.dia);
        }
    t_fieldHover.clear();
    for (auto& [id, p] : t_palette)
        if (auto b = p.brush.get()) {
            b.UnregisterPropertyChangedCallback(muxm::SolidColorBrush::ColorProperty(), p.token);
            b.Color(p.dia);
        }
    t_palette.clear();
}

// Still in the toolbar or the window buttons (the Arc URL bar moves toolbar buttons into the sidebar,
// where they keep the sidebar color it gave them).
static bool LookInChrome(mux::DependencyObject d) {
    for (int i = 0; d && i < 40; i++, d = muxm::VisualTreeHelper::GetParent(d))
        if (auto fe = d.try_as<mux::FrameworkElement>(); fe && (fe.Name() == L"NavigationBarHost" || fe.Name() == L"RootGrid"))
            return true;
    return false;
}

static void LookRestoreAll() {
    for (auto& [k, s] : t_look) {
        auto d = s.el.get();
        if (!d) continue;
        try {
            d.UnregisterPropertyChangedCallback(s.dp, s.token);
            if (!LookInChrome(d)) continue;
            if (s.binding) d.as<mux::FrameworkElement>().SetBinding(s.dp, s.binding);
            else if (s.value) d.SetValue(s.dp, s.value);
            else d.ClearValue(s.dp);
        } catch (...) {
        }
    }
    t_look.clear();
}

// Icons and text under the toolbar or the window buttons: buttons always (Dia colors them through their
// style), anything inside only where Dia set a color of its own.
static void LookWalk(const mux::DependencyObject& d, int depth) {
    if (!d || depth > 16) return;
    // Not the address box: setting its foreground recolors all of its text, wiping Dia's (or our) colors
    // for the site and the title. Its text is colored run by run instead (LookAddressText).
    if (auto dp = d.try_as<muxc::RichEditBox>() ? nullptr : LookForegroundDp(d)) {
        bool own = d.try_as<muxc::Primitives::ButtonBase>() != nullptr;
        if (!own) {
            auto v = d.ReadLocalValue(dp);
            own = v && v != mux::DependencyProperty::UnsetValue();
        }
        if (!own)
            if (auto fe = d.try_as<mux::FrameworkElement>()) own = fe.GetBindingExpression(dp) != nullptr;
        if (own) LookOverride(d, dp);
    }
    if (auto path = d.try_as<muxsh::Path>(); path && path.Name() == L"ChatButtonIcon")  // a filled shape
        LookOverride(d, muxsh::Shape::FillProperty());
    int n = muxm::VisualTreeHelper::GetChildrenCount(d);
    for (int i = 0; i < n; i++) LookWalk(muxm::VisualTreeHelper::GetChild(d, i), depth + 1);
}

static void UpdateToolbarLook() {
    if (!ToolbarSidebar()) {
        if (!t_look.empty() || !t_palette.empty() || !t_fieldHover.empty() || !t_address.empty())
            LookRestoreAll(), PaletteRestoreAll(), t_lookText = nullptr, Log("toolbar color: adaptive");
        return;
    }
    auto text = SidebarTextBrush();
    if (text != t_lookText) {
        bool first = !t_lookText;
        t_lookText = text;
        // The sidebar's color changed (theme): re-apply everywhere.
        for (auto& [k, s] : t_look)
            if (auto d = s.el.get(); d && LookWant(s.dp) == text) d.SetValue(s.dp, text);
        if (first) Log("toolbar color: sidebar");
    }
    for (auto h : FindNodes(L"NavigationBarHost")) {
        auto host = Obj<mux::DependencyObject>(h);
        if (!host || !muxm::VisualTreeHelper::GetChildrenCount(host)) continue;
        // NavigationBarHost > ContentPresenter > Grid: the Grid paints the strip.
        auto cp = muxm::VisualTreeHelper::GetChild(host, 0);
        if (muxm::VisualTreeHelper::GetChildrenCount(cp))
            if (auto strip = muxm::VisualTreeHelper::GetChild(cp, 0).try_as<muxc::Panel>()) {
                LookOverride(strip, muxc::Panel::BackgroundProperty());
                LookPalette(strip);
            }
        LookWalk(host, 0);
    }
    for (auto h : FindNodes(L"AssistantBar"))
        if (auto bar = Obj<muxc::Border>(h); bar && LookInChrome(bar)) LookField(bar);
    for (auto h : FindNodes(L"AddressRichEditBox"))
        if (auto box = Obj<muxc::RichEditBox>(h); box && LookInChrome(box)) LookAddressText(box);
    for (auto h : FindNodes(L"MinimizeButton")) {  // the window buttons' grid
        auto grid = Engine_Parent(h);
        if (Engine_Name(grid) == L"RootGrid")
            if (auto g = Obj<mux::DependencyObject>(grid)) LookWalk(g, 0);
    }
    for (auto it = t_look.begin(); it != t_look.end();) it = it->second.el.get() ? std::next(it) : t_look.erase(it);
    for (auto it = t_palette.begin(); it != t_palette.end();) it = it->second.brush.get() ? std::next(it) : t_palette.erase(it);
    for (auto it = t_fieldHover.begin(); it != t_fieldHover.end();) it = it->second.bar.get() ? std::next(it) : t_fieldHover.erase(it);
    for (auto it = t_address.begin(); it != t_address.end();) it = it->second.box.get() ? std::next(it) : t_address.erase(it);
}

// Dia builds a toolbar for each tab it shows; give it the sidebar look before its first frame is drawn.
static void ScheduleToolbarLook() {
    if (t_lookPending || !ToolbarSidebar()) return;
    t_lookPending = true;
    t_lookToken = muxm::CompositionTarget::Rendering([](auto&&, auto&&) {
        muxm::CompositionTarget::Rendering(t_lookToken);
        t_lookPending = false;
        try {
            UpdateToolbarLook();
        } catch (...) {
        }
    });
}

// --- Square corners by the sidebar: Dia rounds the page with several unnamed borders and grids
// (radius 7) between ContentHost's parent and the page; square them all, put Dia's values back after.
struct CornerState {
    winrt::weak_ref<mux::FrameworkElement> el;
    mux::CornerRadius saved;
};
static thread_local std::vector<CornerState> t_corners;

static mux::CornerRadius GetRadius(const mux::FrameworkElement& fe, bool& ok) {
    ok = true;
    if (auto b = fe.try_as<muxc::Border>()) return b.CornerRadius();
    if (auto g = fe.try_as<muxc::Grid>()) return g.CornerRadius();
    if (auto c = fe.try_as<muxc::Control>()) return c.CornerRadius();
    if (auto cp = fe.try_as<muxc::ContentPresenter>()) return cp.CornerRadius();
    ok = false;
    return {};
}
static void SetRadius(const mux::FrameworkElement& fe, mux::CornerRadius r) {
    if (auto b = fe.try_as<muxc::Border>()) b.CornerRadius(r);
    else if (auto g = fe.try_as<muxc::Grid>()) g.CornerRadius(r);
    else if (auto c = fe.try_as<muxc::Control>()) c.CornerRadius(r);
    else if (auto cp = fe.try_as<muxc::ContentPresenter>()) cp.CornerRadius(r);
}
static bool SmallRounded(const mux::CornerRadius& r) {
    double m = (std::max)((std::max)(r.TopLeft, r.TopRight), (std::max)(r.BottomLeft, r.BottomRight));
    return m > 0 && m <= 10;
}

static void SquareUnder(const mux::DependencyObject& d, int depth) {
    if (!d || depth > 9) return;
    if (auto fe = d.try_as<mux::FrameworkElement>()) {
        // Stop at the page itself and at overlays with their own look.
        if (fe.Name() == L"NavigationChromeHost" || fe.Name() == L"FindInPageHost") return;
        bool ok;
        auto r = GetRadius(fe, ok);
        if (ok && SmallRounded(r)) {
            t_corners.push_back({winrt::make_weak(fe), r});
            SetRadius(fe, {0, 0, 0, 0});
        }
    }
    int n = muxm::VisualTreeHelper::GetChildrenCount(d);
    for (int i = 0; i < n; i++) SquareUnder(muxm::VisualTreeHelper::GetChild(d, i), depth + 1);
}

// The web page's own bottom corners are rounded inside Dia's browser engine, which nothing outside Dia
// can switch off. Windows clips a child window at its parent's edge, so the page area is stretched a
// few pixels past the bottom of the window: the rounded corners land outside and get cut off.
struct PageStretch {
    winrt::weak_ref<mux::FrameworkElement> host;
    mux::Thickness saved{};
};
static thread_local std::vector<PageStretch> t_stretch;
static thread_local bool t_sliding = false;  // the hidden toolbar is mid-slide; don't fight it
static const double kCornerOverhang = 9;    // > Dia's 7px corner radius

static void StretchPage(bool on) {
    if (!on) {
        for (auto& p : t_stretch)
            if (auto fe = p.host.get()) fe.Margin(p.saved);
        t_stretch.clear();
        return;
    }
    if (t_sliding) return;
    for (auto h : FindNodes(L"ContentHost")) {
        auto fe = Obj<mux::FrameworkElement>(h);
        if (!fe || fe.ActualHeight() < 1) continue;
        PageStretch* st = nullptr;
        for (auto& p : t_stretch)
            if (p.host.get() == fe) st = &p;
        if (!st) {
            t_stretch.push_back({winrt::make_weak(fe), fe.Margin()});
            st = &t_stretch.back();
        }
        auto root = fe.XamlRoot() ? fe.XamlRoot().Content().try_as<mux::FrameworkElement>() : nullptr;
        if (!root) continue;
        double bottom = fe.TransformToVisual(root).TransformPoint({0, 0}).Y + fe.ActualHeight();
        double delta = root.ActualHeight() + kCornerOverhang - bottom;  // how far the page must still grow
        if (std::abs(delta) < 0.5) continue;
        auto m = fe.Margin();
        m.Bottom -= delta;
        if (m.Bottom > st->saved.Bottom) m.Bottom = st->saved.Bottom;  // never shrink the page
        fe.Margin(m);
    }
}

static void UpdateSquareCorners() {
    bool on = Engine_TweakOn(L"squareCorners");
    StretchPage(on);
    if (!on) {
        for (auto& c : t_corners)
            if (auto fe = c.el.get()) SetRadius(fe, c.saved);
        t_corners.clear();
        return;
    }
    // Re-square anything Dia rounded again (e.g. after a tab switch rebuilt the page).
    for (auto& c : t_corners)
        if (auto fe = c.el.get()) {
            bool ok;
            if (SmallRounded(GetRadius(fe, ok))) SetRadius(fe, {0, 0, 0, 0});
        }
    for (auto h : FindNodes(L"SharedSurface")) {
        auto fe = Obj<mux::FrameworkElement>(h);
        bool ok;
        if (fe && SmallRounded(GetRadius(fe, ok))) t_corners.push_back({winrt::make_weak(fe), GetRadius(fe, ok)}), SetRadius(fe, {0, 0, 0, 0});
    }
    for (auto h : FindNodes(L"ContentHost")) SquareUnder(Obj<mux::DependencyObject>(h), 0);
    t_corners.erase(std::remove_if(t_corners.begin(), t_corners.end(), [](const CornerState& c) { return !c.el.get(); }),
                    t_corners.end());
}

// --- Always show address field: the field's background and its buttons, not just on hover.
struct FieldState {
    winrt::weak_ref<muxc::Border> bar;
    muxm::Brush saved{nullptr};
};
struct ButtonsState {
    winrt::weak_ref<mux::FrameworkElement> stack;
    mux::Data::BindingBase binding{nullptr};  // Dia binds the buttons' opacity to hover; put it back after
    double opacity = 0;
};
static thread_local std::vector<ButtonsState> t_buttons;
static thread_local std::vector<FieldState> t_field;

static void UpdateAddressField() {
    bool on = Engine_TweakOn(L"alwaysShowAddressField") || Engine_TweakOn(L"arcUrlBar");
    if (on) {
        // In Arc mode the pill's background is drawn by our own (persistent) row, so Dia's per-tab field
        // stays transparent and nothing about the pill changes when tabs come and go.
        static thread_local muxm::Brush clear = muxm::SolidColorBrush(Rgba(0, 0, 0, 0));
        auto fill = Engine_TweakOn(L"arcUrlBar") ? clear
                                                  : BrushRes(L"ControlFillColorDefaultBrush", Rgba(0xff, 0xff, 0xff, 0x0f));
        for (auto h : FindNodes(L"AssistantBar")) {
            auto bar = Obj<muxc::Border>(h);
            if (!bar) continue;
            bool known = false;
            for (auto& f : t_field)
                if (f.bar.get() == bar) known = true;
            if (!known) {
                t_field.push_back({winrt::make_weak(bar), bar.Background()});
                // Dia paints the field in the site's color on hover; put the neutral fill straight
                // back, before the frame is drawn, so the tint never shows.
                bar.RegisterPropertyChangedCallback(muxc::Border::BackgroundProperty(),
                                                    [](mux::DependencyObject const& o, auto&&) {
                                                        if (!Engine_TweakOn(L"alwaysShowAddressField") &&
                                                            !Engine_TweakOn(L"arcUrlBar"))
                                                            return;
                                                        static thread_local muxm::Brush clear2 =
                                                            muxm::SolidColorBrush(Rgba(0, 0, 0, 0));
                                                        auto want = Engine_TweakOn(L"arcUrlBar")
                                                                        ? clear2
                                                                        : BrushRes(L"ControlFillColorDefaultBrush",
                                                                                   Rgba(0xff, 0xff, 0xff, 0x0f));
                                                        auto b = o.as<muxc::Border>();
                                                        auto cur = b.Background().try_as<muxm::SolidColorBrush>();
                                                        auto w = want.try_as<muxm::SolidColorBrush>();
                                                        if (!cur || !w || cur.Color() != w.Color()) b.Background(want);
                                                    });
            }
            if (bar.Background() != fill) bar.Background(fill);
            if (bar.CornerRadius().TopLeft < 1) bar.CornerRadius({8, 8, 8, 8});
        }
        for (auto h : FindNodes(L"UrlBarTrailingStack")) {
            if (Engine_TweakOn(L"arcUrlBar")) break;  // Arc URL bar shows them on hover itself
            auto st = Obj<mux::FrameworkElement>(h);
            if (!st) continue;
            bool known = false;
            for (auto& b : t_buttons)
                if (b.stack.get() == st) known = true;
            if (!known) {
                ButtonsState b;
                b.stack = winrt::make_weak(st);
                b.opacity = st.Opacity();
                if (auto be = st.GetBindingExpression(mux::UIElement::OpacityProperty())) b.binding = be.ParentBinding();
                t_buttons.push_back(b);
            }
            if (st.Opacity() != 1) st.Opacity(1);
        }
    } else {
        for (auto& f : t_field)
            if (auto bar = f.bar.get()) bar.Background(f.saved);
        t_field.clear();
        for (auto& b : t_buttons)
            if (auto st = b.stack.get()) {
                if (b.binding) st.SetBinding(mux::UIElement::OpacityProperty(), b.binding);
                else st.Opacity(b.opacity);
            }
        t_buttons.clear();
    }
}

// --- Profile emoji: an emoji for this profile at the top of the sidebar.
static thread_local std::vector<winrt::weak_ref<muxc::TextBlock>> t_emoji;

static void UpdateProfileEmoji() {
    bool on = Engine_TweakOn(L"profileEmoji");
    std::wstring emoji = Engine_TweakValue(L"profileEmoji");
    if (!on || emoji.empty()) {
        for (auto& w : t_emoji)
            if (auto tb = w.get())
                if (auto parent = muxm::VisualTreeHelper::GetParent(tb).try_as<muxc::Panel>()) {
                    uint32_t i;
                    if (parent.Children().IndexOf(tb, i)) parent.Children().RemoveAt(i);
                }
        t_emoji.clear();
        return;
    }
    for (auto h : FindNodes(L"HeaderContentHost")) {
        auto host = Obj<mux::FrameworkElement>(h);
        auto header = host ? muxm::VisualTreeHelper::GetParent(host).try_as<muxc::Grid>() : nullptr;
        if (!header) continue;
        muxc::TextBlock tb{nullptr};
        for (auto const& c : header.Children())
            if (auto fe = c.try_as<muxc::TextBlock>(); fe && fe.Name() == L"diatweaksProfileEmoji") tb = fe;
        if (!tb) {
            tb = muxc::TextBlock();
            tb.Name(L"diatweaksProfileEmoji");
            tb.FontFamily(muxm::FontFamily(L"Segoe UI Emoji"));
            tb.FontSize(16);
            tb.VerticalAlignment(mux::VerticalAlignment::Center);
            tb.HorizontalAlignment(mux::HorizontalAlignment::Left);
            tb.Margin({4, 0, 0, 0});
            tb.IsHitTestVisible(false);
            muxc::Grid::SetColumn(tb, muxc::Grid::GetColumn(host));
            muxc::Grid::SetRow(tb, muxc::Grid::GetRow(host));
            header.Children().Append(tb);
            t_emoji.push_back(winrt::make_weak(tb));
        }
        if (tb.Text() != emoji) tb.Text(emoji);
    }
}

// --- Don't pop up downloads: close the downloads list when Dia opens it by itself for a new
// download (not when you click the downloads button).
static thread_local std::set<InstanceHandle> t_seenPopups;

static void UpdateDownloadsPopup() {
    if (!Engine_TweakOn(L"dontPopUpDownloads")) {
        t_seenPopups.clear();
        return;
    }
    LASTINPUTINFO li{sizeof(li)};
    GetLastInputInfo(&li);
    bool userJustActed = GetTickCount() - li.dwTime < 600;
    Engine_ForEachNode([&](const NodeRef& r) {
        if (r.type != L"Microsoft.UI.Xaml.Controls.Primitives.Popup" || t_seenPopups.count(r.handle)) return;
        auto popup = Obj<muxc::Primitives::Popup>(r.handle);
        if (!popup || !popup.IsOpen()) return;
        t_seenPopups.insert(r.handle);
        auto child = popup.Child();
        if (!child) return;
        auto dl = FindVisual(child, [](const mux::DependencyObject& d) {
            auto fe = d.try_as<mux::FrameworkElement>();
            if (!fe) return false;
            std::wstring n(fe.Name());
            std::wstring t(winrt::get_class_name(fe));
            return n.find(L"Download") != std::wstring::npos || t.find(L"Download") != std::wstring::npos;
        });
        if (dl && !userJustActed) {
            popup.IsOpen(false);
            Log("closed downloads pop-up");
        }
    });
}

// --- Hide tab hover card: the card (title, site, pin/bookmark/... buttons) Dia shows when the pointer
// rests on a tab or pinned tab. It's a flyout whose content holds TitleText, SubtitleText and
// ActionsRow; its FlyoutPresenter is collapsed, so nothing is drawn and nothing can be clicked.
static thread_local std::vector<winrt::weak_ref<muxc::FlyoutPresenter>> t_hiddenCards;
static thread_local bool t_cardPending = false;
static thread_local winrt::event_token t_cardToken{};

static void HideHoverCard(const mux::FrameworkElement& row) {
    auto panel = muxm::VisualTreeHelper::GetParent(row);
    if (!panel) return;
    auto has = [&](const wchar_t* name) {
        return FindVisual(panel, [name](const mux::DependencyObject& d) {
                   auto fe = d.try_as<mux::FrameworkElement>();
                   return fe && fe.Name() == name;
               }) != nullptr;
    };
    if (!has(L"TitleText") || !has(L"SubtitleText")) return;
    mux::DependencyObject p = panel;
    for (int i = 0; p && i < 20; i++, p = muxm::VisualTreeHelper::GetParent(p)) {
        auto fp = p.try_as<muxc::FlyoutPresenter>();
        if (!fp) continue;
        if (fp.Visibility() != mux::Visibility::Collapsed) {
            fp.Visibility(mux::Visibility::Collapsed);
            t_hiddenCards.push_back(winrt::make_weak(fp));
            static int n = 0;
            if (n++ < 5) Log("hid tab hover card");
        }
        return;
    }
}

static void UpdateHoverCards() {
    if (!Engine_TweakOn(L"hideTabHoverCard")) {
        for (auto& w : t_hiddenCards)
            if (auto fp = w.get()) fp.Visibility(mux::Visibility::Visible);
        t_hiddenCards.clear();
        return;
    }
    for (auto h : FindNodes(L"ActionsRow"))
        if (auto row = Obj<mux::FrameworkElement>(h)) HideHoverCard(row);
}

// Runs before the next frame, so the card never shows up even for one frame.
static void ScheduleHoverCards() {
    if (t_cardPending || !Engine_TweakOn(L"hideTabHoverCard")) return;
    t_cardPending = true;
    t_cardToken = muxm::CompositionTarget::Rendering([](auto&&, auto&&) {
        muxm::CompositionTarget::Rendering(t_cardToken);
        t_cardPending = false;
        try {
            UpdateHoverCards();
        } catch (...) {
        }
    });
}

// ---------------------------------------------------------------- Arc URL bar
static double NowMs();
// Arc-style sidebar: Dia's own sidebar toggle, back/forward/reload buttons and its real address
// field (with its suggestions, bookmark and site-menu buttons) are moved out of the top toolbar into two
// rows under the sidebar header; the top toolbar is then hidden. Like Arc's single-toolbar mode, the
// real elements are moved (not copied), so everything keeps working. Turning the patch off puts each
// element back exactly where it was.

struct Moved {
    winrt::weak_ref<mux::UIElement> el;
    winrt::weak_ref<muxc::Panel> from;  // original parent
    uint32_t index = 0;
    mux::Thickness margin{};
    mux::HorizontalAlignment halign{};
    mux::Visibility visibility{};
    // Kept visible while in the sidebar (Dia hides the sidebar button in every split pane but the
    // leftmost): Dia's own value to put back, and our watch on it.
    bool pinned = false;
    mux::Data::BindingBase visBinding{nullptr};
    winrt::Windows::Foundation::IInspectable visLocal{nullptr};
    int64_t visToken = 0;
};
struct ArcState {
    winrt::weak_ref<muxc::Grid> paging;     // Dia's SpacePagingRoot (sidebar grid)
    winrt::weak_ref<muxc::Grid> bar;        // our two rows
    winrt::weak_ref<muxc::Panel> left, right, url;
    winrt::weak_ref<muxc::Grid> header;     // Dia's WindowDragHeader (logo row); left/right live in it
    winrt::weak_ref<mux::FrameworkElement> display;  // the pill's address text (AddressRichEditBox)
    bool displayHit = true, displayTab = true;
    double displayOpacity = 1;
    muxm::RectangleGeometry displayClip{nullptr};
    winrt::weak_ref<muxc::TextBlock> label;  // our domain-only text drawn over the pill
    bool swapped = false;                     // just took over a new tab's controls
    std::vector<std::pair<winrt::weak_ref<muxc::Control>, muxm::Brush>> tint;  // Dia's site-tinted icon colors
    // Icons inside the buttons that Dia colored directly: (icon, property, Dia's value)
    std::vector<std::tuple<winrt::weak_ref<mux::DependencyObject>, mux::DependencyProperty, wf::IInspectable>> iconTint;
    bool hover = false, trailingShown = true; // pointer over the pill; ☆ / ⋯ currently visible
    std::vector<std::pair<winrt::weak_ref<mux::FrameworkElement>, int>> shifted;  // Dia rows we moved down
    std::vector<Moved> moved;               // the active tab's toolbar items, currently in our rows
    winrt::weak_ref<mux::FrameworkElement> source;  // the active tab's (now emptied) toolbar
    // Dia header values we changed to line the row up (logo icon, panel placement): put back on off.
    std::vector<std::tuple<winrt::weak_ref<mux::DependencyObject>, mux::DependencyProperty, wf::IInspectable>> headerStyle;
    winrt::weak_ref<mux::FrameworkElement> overflowWatched;
};
static thread_local std::map<InstanceHandle, ArcState> t_arc;

static mux::FrameworkElement FindInRoot(const wchar_t* name, const mux::XamlRoot& root) {
    for (auto h : FindNodes(name))
        if (auto fe = Obj<mux::FrameworkElement>(h); fe && fe.XamlRoot() == root) return fe;
    return nullptr;
}

static mux::FrameworkElement FindByName(const mux::DependencyObject& under, const wchar_t* name) {
    return FindVisual(under, [name](const mux::DependencyObject& d) {
        auto fe = d.try_as<mux::FrameworkElement>();
        return fe && fe.Name() == name;
    }).try_as<mux::FrameworkElement>();
}

static bool MoveInto(ArcState& st, const mux::FrameworkElement& el, const muxc::Panel& to, mux::Thickness margin,
                     mux::HorizontalAlignment halign) {
    auto from = muxm::VisualTreeHelper::GetParent(el).try_as<muxc::Panel>();
    if (!from) return false;
    uint32_t idx;
    if (!from.Children().IndexOf(el, idx)) return false;
    Moved m;
    m.el = winrt::make_weak(el.as<mux::UIElement>());
    m.from = winrt::make_weak(from);
    m.index = idx;
    m.margin = el.Margin();
    m.halign = el.HorizontalAlignment();
    m.visibility = el.Visibility();
    from.Children().RemoveAt(idx);
    el.Margin(margin);
    el.HorizontalAlignment(halign);
    to.Children().Append(el);
    st.moved.push_back(m);
    return true;
}

// Put the items we borrowed back into their tab's toolbar, last moved first so indexes line up.
static void ReturnMoved(ArcState& st) {
    for (auto& [w, brush] : st.tint)
        if (auto c = w.get()) c.Foreground(brush);
    st.tint.clear();
    for (auto& [w, dp, v] : st.iconTint)
        if (auto o = w.get()) o.SetValue(dp, v);
    st.iconTint.clear();
    st.trailingShown = true;
    if (auto d = st.display.get()) {
        d.Opacity(st.displayOpacity);
        d.Clip(st.displayClip);
        d.IsHitTestVisible(st.displayHit);
        if (auto c = d.try_as<muxc::Control>()) c.IsTabStop(st.displayTab);
    }
    st.display = nullptr;
    for (auto it = st.moved.rbegin(); it != st.moved.rend(); ++it) {
        auto el = it->el.get();
        auto from = it->from.get();
        if (!el) continue;
        if (auto cur = muxm::VisualTreeHelper::GetParent(el).try_as<muxc::Panel>()) {
            uint32_t i;
            if (cur.Children().IndexOf(el, i)) cur.Children().RemoveAt(i);
        }
        if (it->pinned) {
            el.UnregisterPropertyChangedCallback(mux::UIElement::VisibilityProperty(), it->visToken);
            if (it->visBinding) el.as<mux::FrameworkElement>().SetBinding(mux::UIElement::VisibilityProperty(), it->visBinding);
            else if (it->visLocal && it->visLocal != mux::DependencyProperty::UnsetValue())
                el.SetValue(mux::UIElement::VisibilityProperty(), it->visLocal);
            else el.ClearValue(mux::UIElement::VisibilityProperty());
        }
        if (auto fe = el.try_as<mux::FrameworkElement>()) {
            fe.Margin(it->margin);
            fe.HorizontalAlignment(it->halign);
            if (fe.Name() == L"AssistantBarHost") {
                fe.Visibility(it->visibility);
                if (auto tr = FindByName(fe, L"UrlBarTrailingStack")) {  // undo the hover fade
                    tr.IsHitTestVisible(true);
                    try {
                        auto v = winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(tr);
                        v.StopAnimation(L"Opacity");
                        v.Opacity(1.0f);
                    } catch (...) {
                    }
                }
            }
        }
        if (from) from.Children().InsertAt((std::min)(it->index, from.Children().Size()), el);
    }
    st.moved.clear();
    st.source = nullptr;
}

static void ArcOff(ArcState& st) {
    ReturnMoved(st);
    for (auto it = st.headerStyle.rbegin(); it != st.headerStyle.rend(); ++it) {
        auto& [w, dp, v] = *it;
        if (auto o = w.get()) {
            if (v && v != mux::DependencyProperty::UnsetValue()) o.SetValue(dp, v);
            else o.ClearValue(dp);
        }
    }
    st.headerStyle.clear();
    if (auto header = st.header.get())
        for (auto w : {st.left.get(), st.right.get()}) {
            uint32_t i;
            if (w && header.Children().IndexOf(w, i)) header.Children().RemoveAt(i);
        }
    auto paging = st.paging.get();
    if (auto bar = st.bar.get(); bar && paging) {
        uint32_t i;
        if (paging.Children().IndexOf(bar, i)) paging.Children().RemoveAt(i);
    }
    for (auto& [w, row] : st.shifted)
        if (auto fe = w.get()) muxc::Grid::SetRow(fe, row);
    st.shifted.clear();
    if (paging && paging.RowDefinitions().Size() > 3) paging.RowDefinitions().RemoveAt(1);
}

// Keeps the pill's label in step with Dia's address text and clear of the ☆ / ⋯ buttons.
// Is the mouse pointer over this element right now (and over its window, not something on top)?
static bool PointerInside(const mux::FrameworkElement& el) {
    POINT pt;
    if (!el) return false;
    auto xamlRoot = el.XamlRoot();
    if (!xamlRoot || !GetCursorPos(&pt)) return false;
    HWND under = WindowFromPoint(pt);
    HWND root = under ? GetAncestor(under, GA_ROOT) : nullptr;
    DWORD pid = 0;
    if (!root || (GetWindowThreadProcessId(root, &pid), pid) != GetCurrentProcessId()) return false;
    try {
        auto environment = xamlRoot.ContentIslandEnvironment();
        if (!environment) return false;
        auto id = environment.AppWindowId();
        if ((HWND)(uintptr_t)id.Value != root) return false;
    } catch (...) {
    }
    ScreenToClient(root, &pt);
    double scale = xamlRoot.RasterizationScale();
    double x = pt.x / scale, y = pt.y / scale;
    auto o = el.TransformToVisual(nullptr).TransformPoint({0, 0});
    return x >= o.X && y >= o.Y && x < o.X + el.ActualWidth() && y < o.Y + el.ActualHeight();
}

// ☆ / ⋯ fade in while the pointer is over the pill and out when it leaves (Arc's clean pill).
static void UpdateArcTrailing(ArcState& st, bool animate) {
    auto url = st.url.get();
    if (!url) return;
    auto host = FindByName(url, L"AssistantBarHost");
    auto trailing = host ? FindByName(host, L"UrlBarTrailingStack") : nullptr;
    if (!trailing) return;
    if (trailing.Opacity() != 1) trailing.Opacity(1);  // Dia's own hover binding; ours drives the fade
    bool show = st.hover;
    trailing.IsHitTestVisible(show);
    if (show == st.trailingShown && animate) return;
    st.trailingShown = show;
    try {
        auto v = winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(trailing);
        if (!animate) {
            v.StopAnimation(L"Opacity");
            v.Opacity(show ? 1.0f : 0.0f);
            return;
        }
        auto c = v.Compositor();
        auto fade = c.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(1.0f, show ? 1.0f : 0.0f);
        fade.Duration(std::chrono::milliseconds(show ? 120 : 180));
        v.StartAnimation(L"Opacity", fade);
    } catch (...) {
    }
}

static void AttachDisplay(ArcState& st, const mux::FrameworkElement& address);
static void UpdateArcLabel(ArcState& st) {
    auto label = st.label.get();
    // The weak reference can stop resolving while the label is still in the pill; find it again, or the
    // pill would keep showing whatever site it showed last.
    if (!label)
        if (auto bar = st.bar.get())
            if ((label = FindByName(bar, L"diatweaksArcLabel").try_as<muxc::TextBlock>())) {
                st.label = winrt::make_weak(label);
                if (!st.url.get())
                    if (auto pill = muxm::VisualTreeHelper::GetParent(label).try_as<muxc::Panel>()) st.url = winrt::make_weak(pill);
            }
    if (!label) return;
    if (auto url = st.url.get()) AttachDisplay(st, FindByName(url, L"AssistantBarHost"));
    auto reb = st.display.get().try_as<muxc::RichEditBox>();
    if (!reb) {
        if (!label.Text().empty()) label.Text(L"");  // never show the previous tab's site
        return;
    }
    winrt::hstring raw;
    reb.Document().GetText(winrt::Microsoft::UI::Text::TextGetOptions::None, raw);
    std::wstring t(raw);
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    size_t sep = t.find(L" / ");
    if (sep != std::wstring::npos) t = t.substr(0, sep);  // "site / page title" -> "site"
    if (t.rfind(L"www.", 0) == 0) t = t.substr(4);
    if (label.Text() != t) label.Text(t);
    if (reb.Opacity() != 0) reb.Opacity(0);
    if (auto header = st.header.get())
        if (auto ref = FindByName(header, L"OverflowButton").try_as<muxc::Control>())
            for (auto& [w, brush] : st.tint)
                if (auto c = w.get(); c && c.Foreground() != ref.Foreground()) c.Foreground(ref.Foreground());
    // Same left edge and color as Dia's text; stop before the trailing buttons.
    auto url = muxm::VisualTreeHelper::GetParent(label).try_as<mux::UIElement>();
    if (!url) return;
    url.UpdateLayout();  // after a tab switch the new tab's chip/buttons must be measured before we place the label
    double left = 12, right = 12;
    try {
        // Never closer to the pill's edge than on the right.
        left = (std::max)(12.0, reb.TransformToVisual(url).TransformPoint({0, 0}).X + reb.Padding().Left + 1);
        auto host = FindByName(url, L"AssistantBarHost");
        auto trailing = host ? FindByName(host, L"UrlBarTrailingStack") : nullptr;
        if (!st.hover)
            right = 12;  // buttons hidden: the site can use the whole pill
        else if (trailing && trailing.ActualWidth() > 0)
            right = url.as<mux::FrameworkElement>().ActualWidth() - trailing.TransformToVisual(url).TransformPoint({0, 0}).X + 4;
        else
            right = label.Margin().Right > 0 ? label.Margin().Right : right;  // not measured yet: keep it put
    } catch (...) {
    }
    mux::Thickness m{left, 0, right, 0};
    auto cur = label.Margin();
    if (std::abs(cur.Left - m.Left) > 0.5 || std::abs(cur.Right - m.Right) > 0.5) label.Margin(m);
    if (label.Foreground() != reb.Foreground()) label.Foreground(reb.Foreground());
    if (label.FontFamily() != reb.FontFamily()) label.FontFamily(reb.FontFamily());
}

// Opens Dia's command bar exactly like its own Ctrl+L shortcut does.
static void OpenCommandBar() {
    INPUT in[4] = {};
    for (auto& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = 'L';
    in[2].ki.wVk = 'L';
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

// Set a Dia property for the Arc header, remembering Dia's own value the first time.
static void HeaderSet(ArcState& st, const mux::DependencyObject& o, const mux::DependencyProperty& dp, const wf::IInspectable& v) {
    bool known = false;
    for (auto& [w, p, old] : st.headerStyle)
        if (p == dp && w.get() == o) known = true;
    if (!known) st.headerStyle.push_back({winrt::make_weak(o), dp, o.ReadLocalValue(dp)});
    o.SetValue(dp, v);
}

// Arc's header row: every icon the same size, color and height, in one tight row. The logo, then the
// sidebar button right next to it; back / forward / reload at the right end, against Dia's ⌄ button
// (or against the edge, at the logo's distance, when that button is hidden).
static void ArcStyleHeader(ArcState& st) {
    auto header = st.header.get();
    auto left = st.left.get().try_as<mux::FrameworkElement>(), right = st.right.get().try_as<mux::FrameworkElement>();
    if (!header || !left || !right) return;
    auto logo = FindByName(header, L"AppMenuButton");
    auto overflow = FindByName(header, L"OverflowButton");
    if (!logo || logo.ActualWidth() <= 0 || header.ActualWidth() <= 0) return;
    if (overflow && st.overflowWatched.get() != overflow) {
        st.overflowWatched = winrt::make_weak(overflow);
        overflow.RegisterPropertyChangedCallback(mux::UIElement::VisibilityProperty(), [](mux::DependencyObject const&, auto&&) {
            for (auto& [h, s] : t_arc) ArcStyleHeader(s);
        });
    }
    double logoX = 0, top = 8;
    try {
        logoX = logo.TransformToVisual(header).TransformPoint({0, 0}).X;
        if (auto bg = FindByName(logo, L"BackgroundBorder"))  // Dia's buttons draw a 32 px square inside a 44 px row
            top = bg.TransformToVisual(header).TransformPoint({0, 0}).Y;
    } catch (...) {
        return;
    }
    double edge = logoX;  // same margin at both ends
    if (overflow && overflow.Visibility() == mux::Visibility::Visible && overflow.ActualWidth() > 0)
        try {
            edge = header.ActualWidth() - overflow.TransformToVisual(header).TransformPoint({0, 0}).X;
        } catch (...) {
        }
    int cols = (int)header.ColumnDefinitions().Size();
    for (auto [el, m, ha] : {std::tuple{left, mux::Thickness{logoX + logo.ActualWidth(), top, 0, 0}, mux::HorizontalAlignment::Left},
                             std::tuple{right, mux::Thickness{0, top, edge, 0}, mux::HorizontalAlignment::Right}}) {
        if (muxc::Grid::GetColumn(el) != 0) muxc::Grid::SetColumn(el, 0);
        if (cols > 1 && muxc::Grid::GetColumnSpan(el) != cols) muxc::Grid::SetColumnSpan(el, cols);
        if (el.HorizontalAlignment() != ha) el.HorizontalAlignment(ha);
        if (el.VerticalAlignment() != mux::VerticalAlignment::Top) el.VerticalAlignment(mux::VerticalAlignment::Top);
        auto c = el.Margin();
        if (std::abs(c.Left - m.Left) > 0.1 || std::abs(c.Top - m.Top) > 0.1 || std::abs(c.Right - m.Right) > 0.1) el.Margin(m);
    }
    // Profile emoji patch: right after the sidebar button, on the same line (it would sit under it).
    for (auto const& c : header.Children())
        if (auto em = c.try_as<muxc::TextBlock>(); em && em.Name() == L"diatweaksProfileEmoji") {
            double x = logoX + logo.ActualWidth() + (left.Visibility() == mux::Visibility::Visible ? left.ActualWidth() : 0) + 6;
            // Same visual size as the icons; color emoji glyphs sit low in their line box, so lift it.
            if (em.FontSize() != 14) HeaderSet(st, em, muxc::TextBlock::FontSizeProperty(), winrt::box_value(14.0));
            double y = top + (std::max)(0.0, (32 - em.ActualHeight()) / 2) - 1.5;
            if (muxc::Grid::GetColumn(em) != 0) HeaderSet(st, em, muxc::Grid::ColumnProperty(), winrt::box_value(0));
            if (cols > 1 && muxc::Grid::GetColumnSpan(em) != cols) HeaderSet(st, em, muxc::Grid::ColumnSpanProperty(), winrt::box_value(cols));
            if (em.VerticalAlignment() != mux::VerticalAlignment::Top)
                HeaderSet(st, em, mux::FrameworkElement::VerticalAlignmentProperty(), winrt::box_value(mux::VerticalAlignment::Top));
            auto m = em.Margin();
            if (std::abs(m.Left - x) > 0.1 || std::abs(m.Top - y) > 0.1)
                HeaderSet(st, em, mux::FrameworkElement::MarginProperty(), winrt::box_value(mux::Thickness{x, y, 0, 0}));
        }
    // The Dia logo: a faint 16 px shape; drawn like the other icons (their color, optically their size).
    auto ref = FindByName(st.right.get(), L"BackButton").try_as<muxc::Control>();
    if (!ref && overflow) ref = overflow.try_as<muxc::Control>();
    auto path = FindVisual(logo, [](const mux::DependencyObject& d) { return (bool)d.try_as<muxsh::Path>(); }).try_as<muxsh::Path>();
    if (!path || !ref) return;
    double icon = 14.4;
    if (auto fi = FindVisual(ref, [](const mux::DependencyObject& d) { return (bool)d.try_as<muxc::FontIcon>(); }).try_as<mux::FrameworkElement>())
        if (fi.ActualWidth() > 0) icon = fi.ActualWidth();
    icon *= 0.85;  // a solid shape looks bigger than the line icons at the same size
    if (path.Opacity() != 1) HeaderSet(st, path, mux::UIElement::OpacityProperty(), winrt::box_value(1.0));
    if (path.Fill() != ref.Foreground()) HeaderSet(st, path, muxsh::Shape::FillProperty(), ref.Foreground());
    if (path.Stretch() != muxm::Stretch::Uniform) HeaderSet(st, path, muxsh::Shape::StretchProperty(), winrt::box_value(muxm::Stretch::Uniform));
    if (std::abs(path.Width() - icon) > 0.1 || std::isnan(path.Width())) HeaderSet(st, path, mux::FrameworkElement::WidthProperty(), winrt::box_value(icon));
    if (std::abs(path.Height() - icon) > 0.1 || std::isnan(path.Height())) HeaderSet(st, path, mux::FrameworkElement::HeightProperty(), winrt::box_value(icon));
}

// Two rows under the sidebar header: [toggle .... back forward reload] and the address pill.
static void BuildArcRows(ArcState& st, const muxc::Grid& paging) {
    muxc::RowDefinition row;
    row.Height(mux::GridLengthHelper::Auto());
    paging.RowDefinitions().InsertAt(1, row);
    for (auto const& c : paging.Children()) {
        auto fe = c.try_as<mux::FrameworkElement>();
        if (!fe) continue;
        int r = muxc::Grid::GetRow(fe);
        if (r >= 1) st.shifted.push_back({winrt::make_weak(fe), r}), muxc::Grid::SetRow(fe, r + 1);
    }
    muxc::Grid bar;
    bar.Name(L"diatweaksArcBar");
    bar.Padding({4, 0, 4, 8});
    muxc::Grid::SetRow(bar, 1);
    // Header row (Dia's own, with the Dia logo on the left): sidebar toggle right after the logo,
    // back / forward / reload on the right.
    muxc::StackPanel left, right;
    left.Orientation(muxc::Orientation::Horizontal);
    left.HorizontalAlignment(mux::HorizontalAlignment::Left);
    left.VerticalAlignment(mux::VerticalAlignment::Center);
    right.Orientation(muxc::Orientation::Horizontal);
    right.HorizontalAlignment(mux::HorizontalAlignment::Right);
    right.VerticalAlignment(mux::VerticalAlignment::Center);
    right.Spacing(0);
    left.Name(L"diatweaksArcLeft");
    right.Name(L"diatweaksArcRight");
    muxc::Grid::SetColumn(left, 1);
    muxc::Grid::SetColumn(right, 1);
    auto header = FindByName(paging, L"WindowDragHeader").try_as<muxc::Grid>();
    if (header) {
        header.Children().Append(left);
        header.Children().Append(right);
        st.header = winrt::make_weak(header);
    }
    // Row under the header: the address field, as a full-width pill. Clicking it opens Dia's command
    // bar (what Ctrl+L does), which Arc mode anchors over the pill; the ☆ and ⋯ buttons work as usual.
    muxc::Grid url;
    url.Margin({4, 0, 4, 0});
    url.MinHeight(32);
    url.CornerRadius({8, 8, 8, 8});
    bar.Children().Append(url);
    // Arc shows just the site in the pill: Dia's own text ("site / page title") stays in place but
    // invisible, and this label shows the site, trimmed with … before the ☆ / ⋯ buttons.
    muxc::TextBlock label;
    label.Name(L"diatweaksArcLabel");
    label.IsHitTestVisible(false);
    label.VerticalAlignment(mux::VerticalAlignment::Center);
    label.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    label.TextWrapping(mux::TextWrapping::NoWrap);
    label.FontSize(14);
    url.Children().Append(label);
    st.label = winrt::make_weak(label);
    url.PointerEntered([](wf::IInspectable const& sender, auto&&) {
        for (auto& [h, st] : t_arc)
            if (st.url.get() == sender.try_as<muxc::Panel>()) st.hover = true, UpdateArcTrailing(st, true), UpdateArcLabel(st);
    });
    url.PointerExited([](wf::IInspectable const& sender, auto&&) {
        for (auto& [h, st] : t_arc)
            if (st.url.get() == sender.try_as<muxc::Panel>()) st.hover = false, UpdateArcTrailing(st, true), UpdateArcLabel(st);
    });
    url.Background(BrushRes(L"ControlFillColorDefaultBrush", Rgba(0xff, 0xff, 0xff, 0x0f)));  // the pill itself
    url.AddHandler(mux::UIElement::TappedEvent(),
                   winrt::box_value(mux::Input::TappedEventHandler([](wf::IInspectable const&,
                                                                        mux::Input::TappedRoutedEventArgs const& e) {
                       for (auto d = e.OriginalSource().try_as<mux::DependencyObject>(); d;
                            d = muxm::VisualTreeHelper::GetParent(d)) {
                           if (d.try_as<muxc::Primitives::ButtonBase>()) return;  // ☆ / ⋯ / site buttons
                           if (auto fe = d.try_as<mux::FrameworkElement>(); fe && fe.Name() == L"diatweaksArcBar") break;
                       }
                       OpenCommandBar();
                   })),
                   true);
    paging.Children().Append(bar);
    st.paging = winrt::make_weak(paging);
    st.bar = winrt::make_weak(bar);
    st.left = winrt::make_weak(left.as<muxc::Panel>());
    st.right = winrt::make_weak(right.as<muxc::Panel>());
    st.url = winrt::make_weak(url.as<muxc::Panel>());
    Log("Arc URL bar: on");
}

// The pill's text is only a display. In the toolbar Dia turns a click on it into its command bar;
// here we do that ourselves (see BuildArcRows), so the text itself must not take clicks or focus, and
// our domain-only label is shown instead. Dia may build the field's insides a moment after the tab
// appears, so this runs on every pass until it finds the field.
static void AttachDisplay(ArcState& st, const mux::FrameworkElement& address) {
    if (!address) return;
    auto d = FindByName(address, L"AddressRichEditBox");
    auto cur = st.display.get();
    // Dia rebuilds the field when a tab navigates (e.g. a new tab leaving its start page). The old box
    // can still report the pill as an ancestor, so compare with the box that's in the pill now: holding
    // on to the old one leaves Dia's new text showing under a stale label.
    if (cur && cur == d) return;
    if (cur) {
        st.display = nullptr;  // first: our opacity watch would hide it again
        cur.Opacity(st.displayOpacity);
        cur.Clip(st.displayClip);
        cur.IsHitTestVisible(st.displayHit);
        if (auto c = cur.try_as<muxc::Control>()) c.IsTabStop(st.displayTab);
    }
    if (d) {
        st.display = winrt::make_weak(d);
        st.displayOpacity = d.Opacity();
        d.Opacity(0);  // our domain-only label shows instead (UpdateArcLabel)
        // Dia animates its text on changes (which overrides opacity while it runs); an empty clip
        // keeps it hidden no matter what. Layout is unchanged.
        st.displayClip = d.Clip();
        muxm::RectangleGeometry none;
        none.Rect({0, 0, 0, 0});
        d.Clip(none);
        // Dia fades its text back in on some updates; put it straight back to invisible, same frame.
        static thread_local std::set<void*> watchedDisplays;
        void* key = winrt::get_abi(d);
        if (!watchedDisplays.count(key)) {
            watchedDisplays.insert(key);
            d.RegisterPropertyChangedCallback(mux::UIElement::OpacityProperty(), [](mux::DependencyObject const& o, auto&&) {
                if (!Engine_TweakOn(L"arcUrlBar")) return;
                auto fe = o.as<mux::UIElement>();
                for (auto& [h, st] : t_arc)
                    if (st.display.get() == fe && fe.Opacity() != 0) fe.Opacity(0);
            });
        }
        st.displayHit = d.IsHitTestVisible();
        d.IsHitTestVisible(false);
        if (auto c = d.try_as<muxc::Control>()) st.displayTab = c.IsTabStop(), c.IsTabStop(false);
    }
}

// Dia keeps a separate toolbar for every tab. Find the one belonging to the tab on screen; if its
// items are still in it (you just switched tabs), give the previous tab its items back and borrow these.
static void AdoptActiveTab(ArcState& st) {
    auto paging = st.paging.get();
    auto left = st.left.get(), right = st.right.get(), url = st.url.get();
    if (!paging || !left || !right || !url) return;
    auto root = paging.XamlRoot();
    // Every toolbar on screen: one per tab normally, one per pane in split view.
    std::vector<mux::FrameworkElement> bars;
    for (auto h : FindNodes(L"NavigationBarContent")) {
        auto fe = Obj<mux::FrameworkElement>(h);
        if (fe && fe.XamlRoot() == root && ShownOnScreen(fe)) bars.push_back(fe);
    }
    mux::FrameworkElement active{nullptr};
    if (bars.size() == 1) {
        active = bars[0];
    } else if (bars.size() > 1) {
        // Split view: the selected pane is the one holding keyboard focus (clicking a pane focuses its
        // page). Pick the toolbar sitting closest to the focused element. If focus is elsewhere (sidebar,
        // command bar), keep the pane we have, so Ctrl+L / the pill keep editing the selected pane.
        std::vector<mux::DependencyObject> focusChain;
        try {
            for (auto f = mux::Input::FocusManager::GetFocusedElement(root).try_as<mux::DependencyObject>(); f;
                 f = muxm::VisualTreeHelper::GetParent(f))
                focusChain.push_back(f);
        } catch (...) {
        }
        // For each toolbar, the first of its ancestors that also contains the focus. If that meeting
        // point contains no other toolbar, the focus is inside this toolbar's pane: that's the selected one.
        for (auto& b : bars) {
            mux::DependencyObject meet{nullptr};
            for (mux::DependencyObject d = b; d; d = muxm::VisualTreeHelper::GetParent(d))
                if (std::find(focusChain.begin(), focusChain.end(), d) != focusChain.end()) {
                    meet = d;
                    break;
                }
            if (!meet) continue;
            bool own = true;
            for (auto& other : bars) {
                if (other == b) continue;
                for (mux::DependencyObject d = other; d; d = muxm::VisualTreeHelper::GetParent(d))
                    if (d == meet) {
                        own = false;
                        break;
                    }
                if (!own) break;
            }
            if (own) {
                active = b;
                break;
            }
        }
    }
    if (active && active == st.source.get()) return;  // already showing this pane's controls
    if (active && !FindByName(active, L"BackButton")) active = nullptr;  // its controls are elsewhere
    if (!active) {
        if (auto addr = FindByName(url, L"AssistantBarHost"); addr && addr.Visibility() != mux::Visibility::Visible)
            addr.Visibility(mux::Visibility::Visible);  // Dia hides it on its start page; the pill stays
        return;
    }
    auto toggle = FindByName(active, L"SidebarToggleButton");
    auto back = FindByName(active, L"BackButton");
    auto fwd = FindByName(active, L"ForwardButton");
    auto reload = FindByName(active, L"ReloadButton");
    auto address = FindByName(active, L"AssistantBarHost");
    if (!back || !fwd || !reload || !address) return;
    Log("arc: swap to new tab controls (%.0f)", NowMs());
    ReturnMoved(st);
    mux::Thickness none{0, 0, 0, 0};
    bool moveToggle = toggle && MoveInto(st, toggle, left, none, mux::HorizontalAlignment::Left);
    if (moveToggle && !Engine_TweakOn(L"hideSidebarButton")) {
        auto& m = st.moved.back();
        m.pinned = true;
        if (auto be = toggle.GetBindingExpression(mux::UIElement::VisibilityProperty())) m.visBinding = be.ParentBinding();
        m.visLocal = toggle.ReadLocalValue(mux::UIElement::VisibilityProperty());
        toggle.Visibility(mux::Visibility::Visible);
        m.visToken = toggle.RegisterPropertyChangedCallback(mux::UIElement::VisibilityProperty(), [](mux::DependencyObject const& o, auto&&) {
            if (auto u = o.try_as<mux::UIElement>(); u && u.Visibility() != mux::Visibility::Visible) u.Visibility(mux::Visibility::Visible);
        });
    }
    MoveInto(st, back, right, none, mux::HorizontalAlignment::Left);
    MoveInto(st, fwd, right, none, mux::HorizontalAlignment::Left);
    MoveInto(st, reload, right, none, mux::HorizontalAlignment::Left);
    MoveInto(st, address, url, {0, 0, 0, 0}, mux::HorizontalAlignment::Stretch);
    address.Visibility(mux::Visibility::Visible);
    st.source = winrt::make_weak(active);
    st.swapped = true;
    // Dia tints toolbar icons with the site's color; in the sidebar they use the sidebar's own color
    // (taken from Dia's ⌄ button in the same header).
    if (auto header = st.header.get())
        if (auto ref = FindByName(header, L"OverflowButton").try_as<muxc::Control>())
            for (auto el : {toggle, back, fwd, reload})
                if (auto c = el.try_as<muxc::Control>()) {
                    st.tint.push_back({winrt::make_weak(c), c.Foreground()});
                    c.Foreground(ref.Foreground());
                    // Icons with their own color: let them take the button's (sidebar) color.
                    std::function<void(mux::DependencyObject, int)> walk = [&](mux::DependencyObject d, int depth) {
                        if (!d || depth > 6) return;
                        mux::DependencyProperty dp{nullptr};
                        if (d.try_as<muxc::IconElement>()) dp = muxc::IconElement::ForegroundProperty();
                        else if (d.try_as<muxc::TextBlock>()) dp = muxc::TextBlock::ForegroundProperty();
                        else if (d.try_as<muxc::ContentPresenter>()) dp = muxc::ContentPresenter::ForegroundProperty();
                        if (dp) {
                            auto local = d.ReadLocalValue(dp);
                            if (local && local != mux::DependencyProperty::UnsetValue()) {
                                st.iconTint.push_back({winrt::make_weak(d), dp, local});
                                d.ClearValue(dp);
                            }
                        }
                        int n = muxm::VisualTreeHelper::GetChildrenCount(d);
                        for (int i = 0; i < n; i++) walk(muxm::VisualTreeHelper::GetChild(d, i), depth + 1);
                    };
                    walk(c, 0);
                }
    AttachDisplay(st, address);
}

// The address editor opens in one of Dia's popovers (PopoverLayoutRoot on a placement canvas), which
// Dia stretches across the page under the old toolbar spot. In Arc mode it becomes Arc's command bar:
// anchored over the sidebar pill and capped in width. Re-applied the moment Dia positions it.
static const double kCommandBarWidth = 600;
static thread_local std::set<InstanceHandle> t_popoverHooked;

static mux::FrameworkElement ArcPill(const mux::XamlRoot& root) {
    for (auto& [h, st] : t_arc)
        for (auto& m : st.moved)
            if (auto el = m.el.get().try_as<mux::FrameworkElement>(); el && el.Name() == L"AssistantBarHost" && el.XamlRoot() == root)
                return el;
    return nullptr;
}

// Dia's popovers live in a layer that the floating sidebar (sidebar closed, shown by hovering the left
// edge) is drawn above, so the command bar would open underneath it. Lift that layer above its siblings.
static thread_local std::vector<std::pair<winrt::weak_ref<mux::UIElement>, int>> t_raised;

static void RaisePopoverLayer(const mux::FrameworkElement& layout) {
    mux::DependencyObject d = layout;
    while (d) {
        auto fe = d.try_as<mux::FrameworkElement>();
        if (fe && fe.Name() == L"DynamicPopoverRoot") break;
        d = muxm::VisualTreeHelper::GetParent(d);
    }
    if (!d) return;
    auto layer = muxm::VisualTreeHelper::GetParent(d).try_as<mux::UIElement>();
    auto panel = layer ? muxm::VisualTreeHelper::GetParent(layer).try_as<muxc::Panel>() : nullptr;
    if (!panel) return;
    int top = 0;
    for (auto const& c : panel.Children())
        if (c != layer) top = (std::max)(top, muxc::Canvas::GetZIndex(c));
    int z = muxc::Canvas::GetZIndex(layer);
    if (z > top) return;
    bool known = false;
    for (auto& [w, orig] : t_raised)
        if (w.get() == layer) known = true;
    if (!known) t_raised.push_back({winrt::make_weak(layer), z});
    muxc::Canvas::SetZIndex(layer, top + 1);
}

static const double kCenteredWidth = 760;

// The sidebar is docked (open, beside the page) rather than closed or only floating out on hover.
static bool SidebarDocked(const mux::XamlRoot& root) {
    for (auto h : FindNodes(L"SidebarBorder")) {
        auto sb = Obj<mux::FrameworkElement>(h);
        if (!sb || sb.XamlRoot() != root) continue;
        if (!ShownOnScreen(sb) || sb.ActualWidth() < 1) return false;  // closed
        // Floating out over the page, Dia shows the sidebar in a popup of its own, outside the window's
        // tree (and drawn above all of it).
        auto top = [](mux::DependencyObject d) {
            while (auto p = muxm::VisualTreeHelper::GetParent(d)) d = p;
            return d;
        };
        if (auto content = root.Content(); content && top(sb) != top(content)) return false;
        auto host = muxm::VisualTreeHelper::GetParent(sb);
        auto floating = host ? FindVisual(host, [](const mux::DependencyObject& d) {
            auto fe = d.try_as<mux::FrameworkElement>();
            return fe && fe.Name() == L"SidebarFloatingSurface";
        }).try_as<mux::UIElement>() : nullptr;
        return !(floating && floating.Visibility() == mux::Visibility::Visible);
    }
    return true;
}

static bool InNewTabMode();

// Puts a bar we sized back to Dia's own layout (Dia positions it itself each time it opens).
static void UnplaceCommandBar(const mux::FrameworkElement& layout, const mux::FrameworkElement& bar) {
    if (layout.MaxWidth() > kCenteredWidth) return;  // not ours
    for (mux::DependencyObject d = bar; d && d != layout; d = muxm::VisualTreeHelper::GetParent(d))
        if (auto fe = d.try_as<mux::FrameworkElement>()) fe.MaxWidth(std::numeric_limits<double>::infinity());
    layout.MaxWidth(std::numeric_limits<double>::infinity());
}

// With the sidebar floating, Dia shows it in a windowed popup (its own window over Dia's), which nothing
// in Dia's window can be drawn above. While the command bar is open over it, the bar's card is lent to a
// popup of ours, opened after the sidebar's so it sits on top; it goes back when the bar closes.
struct LiftedBar {
    winrt::weak_ref<mux::FrameworkElement> layout;
    winrt::weak_ref<muxc::Panel> from;  // Dia's placement canvas
    uint32_t index = 0;
    muxc::Primitives::Popup popup{nullptr};
};
static thread_local LiftedBar t_lift;

static bool SidebarInPopup(const mux::XamlRoot& root) {
    try {
        for (auto const& p : muxm::VisualTreeHelper::GetOpenPopupsForXamlRoot(root)) {
            if (p == t_lift.popup) continue;
            if (auto ch = p.Child(); ch && FindVisual(ch, [](const mux::DependencyObject& d) {
                    auto fe = d.try_as<mux::FrameworkElement>();
                    return fe && fe.Name() == L"SidebarBorder";
                }))
                return true;
        }
    } catch (...) {
    }
    return false;
}

static void UnliftCommandBar() {
    if (!t_lift.popup) return;
    auto popup = t_lift.popup;
    auto layout = t_lift.layout.get();
    auto from = t_lift.from.get();
    uint32_t index = t_lift.index;
    t_lift = {};
    popup.Child(nullptr);
    popup.IsOpen(false);
    if (layout) {
        layout.ClearValue(mux::FrameworkElement::WidthProperty());
        layout.ClearValue(mux::FrameworkElement::MinHeightProperty());
        if (from && from.IsLoaded()) {
            from.Children().InsertAt((std::min)(index, from.Children().Size()), layout);
            return Log("arc: command bar back in Dia's popover layer");
        }
    }
    Log("arc: command bar closed over the floating sidebar");
}

static void PlaceCommandBar(const mux::FrameworkElement& layout);
static void LiftCommandBar(const mux::FrameworkElement& layout, double x, double y) {
    if (t_lift.popup && t_lift.layout.get() != layout) UnliftCommandBar();
    // Dia sizes the card from its own placement canvas, which doesn't run out here (it keeps the size the
    // card had when it opened): same width as over the docked sidebar, and as tall as its content.
    if (layout.Width() != kCommandBarWidth) layout.Width(kCommandBarWidth);
    auto content = FindByName(layout, L"CommandBarContainerRoot");
    if (content && content.ActualHeight() > 0 && std::abs(layout.MinHeight() - content.ActualHeight()) > 0.5)
        layout.MinHeight(content.ActualHeight());  // (wins over the Height Dia keeps setting)
    if (!t_lift.popup) {
        auto from = muxm::VisualTreeHelper::GetParent(layout).try_as<muxc::Panel>();
        uint32_t i;
        if (!from || !from.Children().IndexOf(layout, i)) return;
        auto root = layout.XamlRoot();
        from.Children().RemoveAt(i);
        muxc::Primitives::Popup popup;
        popup.XamlRoot(root);
        popup.ShouldConstrainToRootBounds(false);
        popup.Child(layout);
        t_lift.layout = winrt::make_weak(layout);
        t_lift.from = winrt::make_weak(from);
        t_lift.index = i;
        t_lift.popup = popup;
        popup.HorizontalOffset(x);
        popup.VerticalOffset(y);
        popup.IsOpen(true);
        // Taking the card out of the window dropped keyboard focus: give it back to the bar's box.
        if (auto box = FindVisual(layout, [](const mux::DependencyObject& d) {
                auto fe = d.try_as<mux::FrameworkElement>();
                return fe && fe.Name() == L"QueryTextBox";
            }).try_as<muxc::Control>())
            box.Focus(mux::FocusState::Programmatic);
        // Dia removes its popover when the bar closes: hand the card back then.
        from.Unloaded([](wf::IInspectable const& sender, auto&&) {
            if (t_lift.popup && t_lift.from.get() == sender.try_as<muxc::Panel>()) UnliftCommandBar();
        });
        // Suggestions appear as you type: keep the card as tall as its content.
        if (content)
            content.SizeChanged([weak = winrt::make_weak(layout)](auto&&, auto&&) {
                if (auto l = weak.get(); l && t_lift.popup && t_lift.layout.get() == l) PlaceCommandBar(l);
            });
        Log("arc: command bar lifted above the floating sidebar");
        return;
    }
    if (std::abs(t_lift.popup.HorizontalOffset() - x) > 0.5) t_lift.popup.HorizontalOffset(x);
    if (std::abs(t_lift.popup.VerticalOffset() - y) > 0.5) t_lift.popup.VerticalOffset(y);
}

// Every pass: let go of the card once Dia removed its popover, or give it back if the sidebar docked.
static void CheckLiftedBar() {
    if (!t_lift.popup) return;
    auto layout = t_lift.layout.get();
    auto from = t_lift.from.get();
    if (!layout || !from || !from.IsLoaded() || !Engine_TweakOn(L"arcUrlBar")) return UnliftCommandBar();
    // Open, but the sidebar is no longer floating: back to Dia's layer, placed for the docked sidebar.
    if (layout.Visibility() == mux::Visibility::Visible && !SidebarInPopup(layout.XamlRoot())) UnliftCommandBar();
}

static void PlaceCommandBar(const mux::FrameworkElement& layout) {
    bool lifted = t_lift.popup && t_lift.layout.get() == layout;
    // Closing: leave a lifted card where it is (collapsed, so not shown). Moving it mid-close cuts Dia's
    // closing short, and Dia then never starts the page's loading animation in the pill.
    if (layout.Visibility() != mux::Visibility::Visible) return;
    auto bar = FindVisual(layout, [](const mux::DependencyObject& d) {
        auto fe = d.try_as<mux::FrameworkElement>();
        return fe && fe.Name() == L"CommandBarContainerRoot";
    }).try_as<mux::FrameworkElement>();
    if (!bar) return;  // some other popover (site menu, ...): leave it alone
    bool floating = InNewTabMode();  // the Ctrl+T bar (Floating new tab bar patch)
    bool arc = Engine_TweakOn(L"arcUrlBar");
    if (!floating && !arc) {
        if (lifted) UnliftCommandBar();
        return UnplaceCommandBar(layout, bar);
    }
    auto pill = ArcPill(layout.XamlRoot());
    // Arc mode with the sidebar floating out over the page: anchored over the pill, above the sidebar.
    bool overFloating = !floating && pill && SidebarInPopup(layout.XamlRoot());
    if (overFloating) {
        double width = kCommandBarWidth;
        for (mux::DependencyObject d = bar; d && d != layout; d = muxm::VisualTreeHelper::GetParent(d))
            if (auto fe = d.try_as<mux::FrameworkElement>()) {
                if (fe.MaxWidth() != width) fe.MaxWidth(width);
                if (!std::isnan(fe.Width()) && fe.Width() > width) fe.Width(width);
            }
        if (layout.MaxWidth() != width) layout.MaxWidth(width);
        layout.HorizontalAlignment(mux::HorizontalAlignment::Left);
        auto at = pill.TransformToVisual(nullptr).TransformPoint({0, 0});
        LiftCommandBar(layout, at.X - 4, at.Y - 6);
        return;
    }
    if (lifted) UnliftCommandBar();
    auto canvas = muxm::VisualTreeHelper::GetParent(layout).try_as<mux::UIElement>();
    if (!canvas || (!floating && !pill)) return;
    RaisePopoverLayer(layout);
    // Ctrl+T bar, or Arc mode with the sidebar closed: a large bar centered over the page, like Dia's New
    // Tab box. Arc mode with the sidebar docked: anchored over the pill.
    bool docked = !floating && SidebarDocked(layout.XamlRoot());
    auto cfe = canvas.as<mux::FrameworkElement>();
    double width, left, top;
    if (docked) {
        auto at = pill.TransformToVisual(canvas).TransformPoint({0, 0});
        width = kCommandBarWidth;
        left = at.X - 4;
        top = at.Y - 6;
    } else {
        // Centered over the page: right of the sidebar when it is docked, else the whole window.
        double areaL = 0;
        if (SidebarDocked(layout.XamlRoot()))
            if (auto sb = FindInRoot(L"SidebarBorder", layout.XamlRoot()))
                try {
                    areaL = (std::max)(0.0, (double)sb.TransformToVisual(canvas).TransformPoint({0, 0}).X + sb.ActualWidth());
                } catch (...) {
                }
        double areaW = cfe.ActualWidth() - areaL;
        width = (std::min)(kCenteredWidth, areaW - 64);
        left = areaL + (areaW - width) / 2;
        top = (std::max)(48.0, cfe.ActualHeight() * 0.14);  // upper part of the page, clear of its content
    }
    if (std::abs(muxc::Canvas::GetLeft(layout) - left) > 0.5) muxc::Canvas::SetLeft(layout, left);
    if (std::abs(muxc::Canvas::GetTop(layout) - top) > 0.5) muxc::Canvas::SetTop(layout, top);
    for (mux::DependencyObject d = bar; d && d != layout; d = muxm::VisualTreeHelper::GetParent(d))
        if (auto fe = d.try_as<mux::FrameworkElement>()) {
            if (fe.MaxWidth() != width) fe.MaxWidth(width);
            if (!std::isnan(fe.Width()) && fe.Width() > width) fe.Width(width);
        }
    if (layout.MaxWidth() != width) layout.MaxWidth(width);
    layout.HorizontalAlignment(mux::HorizontalAlignment::Left);
}

static void HookCommandBarPopovers() {
    for (auto h : FindNodes(L"PopoverLayoutRoot")) {
        auto layout = Obj<mux::FrameworkElement>(h);
        if (!layout) continue;
        if (!t_popoverHooked.count(h)) {
            t_popoverHooked.insert(h);
            auto weak = winrt::make_weak(layout);
            auto again = [weak](auto&&...) {
                try {
                    if (auto l = weak.get()) PlaceCommandBar(l);
                } catch (...) {
                }
            };
            layout.RegisterPropertyChangedCallback(muxc::Canvas::LeftProperty(), again);
            layout.RegisterPropertyChangedCallback(muxc::Canvas::TopProperty(), again);
            layout.RegisterPropertyChangedCallback(mux::UIElement::VisibilityProperty(), again);
            layout.SizeChanged(again);
        }
        PlaceCommandBar(layout);
    }
}

static void ResetCommandBars() {
    UnliftCommandBar();
    for (auto& [w, orig] : t_raised)
        if (auto l = w.get()) muxc::Canvas::SetZIndex(l, orig);
    t_raised.clear();
    for (auto h : FindNodes(L"CommandBarContainerRoot")) {
        auto bar = Obj<mux::FrameworkElement>(h);
        for (mux::DependencyObject d = bar; d; d = muxm::VisualTreeHelper::GetParent(d)) {
            auto fe = d.try_as<mux::FrameworkElement>();
            if (!fe) continue;
            if (fe.MaxWidth() <= kCenteredWidth) fe.MaxWidth(std::numeric_limits<double>::infinity());
            if (fe.Name() == L"PopoverLayoutRoot") break;
        }
    }
}

static void UpdateAddressField();
static thread_local bool t_adoptPending = false;
static thread_local winrt::event_token t_adoptToken{};

static void UpdateArcBar();
// Dia shows a tab by inserting that tab's view (with its own toolbar) and dropping the previous one.
// Swap our sidebar controls over right before the next frame is drawn, so no stale or half-styled
// frame is ever shown.
static void StyleStartPages();
static void ScheduleArcAdopt() {
    if (t_adoptPending || !Engine_TweakOn(L"arcUrlBar")) return;
    t_adoptPending = true;
    t_adoptToken = muxm::CompositionTarget::Rendering([](auto&&, auto&&) {
        muxm::CompositionTarget::Rendering(t_adoptToken);
        t_adoptPending = false;
        try {
            UpdateArcBar();
            UpdateAddressField();
            StyleStartPages();
        } catch (...) {
        }
    });
}

void Ui_ElementAdded(const std::wstring& name) {
    if (name == L"NavigationBarContent" || name == L"AssistantBarHost" || name == L"BackButton" ||
        name == L"CommandBarContainerRoot" || name == L"VariantHost") {
        static int n = 0;
        if (n++ < 40) Log("arc: %s added (%.0f)", N(name).c_str(), NowMs());
        ScheduleArcAdopt();
    }
    if (name == L"ActionsRow") ScheduleHoverCards();
    if (name == L"NavigationBarContent" || name == L"NavigationBarHost" || name == L"MinimizeButton") ScheduleToolbarLook();
}

static void UpdateArcBar() {
    bool on = Engine_TweakOn(L"arcUrlBar");
    if (!on) {
        for (auto& [h, st] : t_arc) ArcOff(st);
        if (!t_arc.empty()) Log("Arc URL bar: off"), ResetCommandBars();
        t_arc.clear();
        return;
    }
    HookCommandBarPopovers();
    CheckLiftedBar();
    for (auto h : FindNodes(L"SpacePagingRoot")) {
        auto paging = Obj<muxc::Grid>(h);
        if (!paging) continue;
        auto& st = t_arc[h];
        if (!st.bar.get()) {
            // Dia can show a sidebar that doesn't have its usual rows (yet); adding ours there would throw
            // on every pass and stop every other tweak from updating.
            if (paging.RowDefinitions().Size() < 3) continue;
            BuildArcRows(st, paging);
        }
        AdoptActiveTab(st);
        if (auto u = st.url.get().try_as<mux::FrameworkElement>()) st.hover = PointerInside(u);  // events can miss a leave
        UpdateArcTrailing(st, !st.swapped);
        UpdateArcLabel(st);
        ArcStyleHeader(st);
        if (st.swapped) st.swapped = false, UpdateAddressField();
        else if (auto u = st.url.get())
            if (auto bar = FindByName(u, L"AssistantBar").try_as<muxc::Border>())
                if (auto b = bar.Background().try_as<muxm::SolidColorBrush>(); b && b.Color().A != 0)
                    UpdateAddressField();  // Dia just built this tab's field: make it transparent now
    }
    for (auto it = t_arc.begin(); it != t_arc.end();)
        it = it->second.paging.get() ? std::next(it) : t_arc.erase(it);
}

// ---------------------------------------------------------------- Arc: New Tab page = the command bar
// In Arc mode Dia's New Tab page (Ctrl+T) shows only its box, sized and placed exactly like the centered
// Ctrl+L command bar; the logo effect, cards and promo text are hidden (they're also what made it lag).
// The box is Dia's own, so typing and one Enter open the URL right in that new tab.

struct StartPageState {
    winrt::weak_ref<mux::FrameworkElement> box;
    mux::Thickness margin{};
    mux::VerticalAlignment valign{};
    double width = 0, maxWidth = 0;
    std::vector<std::pair<winrt::weak_ref<mux::UIElement>, mux::Visibility>> hidden;
};
static thread_local std::vector<StartPageState> t_start;

static void RestoreStartPages() {
    for (auto& st : t_start) {
        if (auto b = st.box.get()) {
            b.Margin(st.margin);
            b.VerticalAlignment(st.valign);
            b.Width(st.width);
            b.MaxWidth(st.maxWidth);
        }
        for (auto& [w, v] : st.hidden)
            if (auto e = w.get()) e.Visibility(v);
    }
    t_start.clear();
}

static void StyleStartPages() {
    // Dia's New Tab page is left exactly as Dia draws it (restyling it broke it); this only undoes any
    // earlier restyle.
    if (true) {
        RestoreStartPages();
        return;
    }
    for (auto vh : FindNodes(L"VariantHost")) {
        auto host = Obj<mux::FrameworkElement>(vh);
        if (!host || !ShownOnScreen(host)) continue;
        auto box = FindByName(host, L"CommandBarContainerRoot");
        if (!box) continue;
        auto page = muxm::VisualTreeHelper::GetParent(box).try_as<muxc::Panel>();
        if (!page) continue;
        StartPageState* st = nullptr;
        for (auto& x : t_start)
            if (x.box.get() == box) st = &x;
        if (!st) {
            StartPageState n;
            n.box = winrt::make_weak(box);
            n.margin = box.Margin();
            n.valign = box.VerticalAlignment();
            n.width = box.Width();
            n.maxWidth = box.MaxWidth();
            t_start.push_back(n);
            st = &t_start.back();
        }
        // Everything on the page except the box goes.
        for (auto const& c : page.Children()) {
            if (c == box || c.Visibility() != mux::Visibility::Visible) continue;
            st->hidden.push_back({winrt::make_weak(c), c.Visibility()});
            c.Visibility(mux::Visibility::Collapsed);
        }
        // Same size and spot as the centered Ctrl+L command bar.
        auto root = host.XamlRoot();
        if (!root) continue;  // (a window being made or closed: calling through null would crash)
        double winW = root.Size().Width, winH = root.Size().Height;
        double width = (std::min)(kCenteredWidth, winW - 64);
        double pageTop = page.TransformToVisual(nullptr).TransformPoint({0, 0}).Y;
        double top = (std::max)(48.0, winH * 0.2) - pageTop;
        if (box.VerticalAlignment() != mux::VerticalAlignment::Top) box.VerticalAlignment(mux::VerticalAlignment::Top);
        if (box.Width() != width) box.Width(width), box.MaxWidth(width);
        auto m = box.Margin();
        if (std::abs(m.Top - top) > 0.5 || m.Bottom != 0) box.Margin({m.Left, top, m.Right, 0});
    }
    t_start.erase(std::remove_if(t_start.begin(), t_start.end(), [](const StartPageState& x) { return !x.box.get(); }),
                  t_start.end());
}

// ---------------------------------------------------------------- Arc: Ctrl+T = empty command bar
// In Arc mode Ctrl+T doesn't build a New Tab page. It opens the same command bar as Ctrl+L, emptied;
// Enter in it becomes Dia's Alt+Enter (open in a new tab). Esc just closes it, nothing is created.

static thread_local bool t_newTabMode = false;
static bool InNewTabMode() { return t_newTabMode; }
static thread_local bool t_passCtrlT = false;     // let our own Ctrl+T through to Dia once
static thread_local std::wstring t_pendingQuery;  // text to submit in the new tab
static thread_local int t_submitFrames = 0;
static thread_local winrt::event_token t_submitToken{};
static thread_local int t_enterIn = -1;  // frames until Enter, after the text was typed
static thread_local std::wstring t_typedQuery;  // what we typed, to know when Dia has it
static thread_local int t_settle = -1;          // frames to wait once Dia has the text
static thread_local winrt::weak_ref<mux::UIElement> t_hiddenPage;  // the New Tab page, hidden during the relay
static thread_local int t_unhideFrames = 0;
static thread_local winrt::event_token t_unhideToken{};

// Shows the New Tab page again once the new tab has moved on to the URL (or after ~3 s).
static void UnhidePageLater() {
    t_unhideFrames = 180;
    if (t_unhideToken) return;
    t_unhideToken = muxm::CompositionTarget::Rendering([](auto&&, auto&&) {
        auto page = t_hiddenPage.get();
        bool gone = !page || !page.try_as<mux::FrameworkElement>() || !ShownOnScreen(page.as<mux::FrameworkElement>());
        if (gone || --t_unhideFrames <= 0) {
            if (page) page.Opacity(1);
            t_hiddenPage = nullptr;
            muxm::CompositionTarget::Rendering(t_unhideToken);
            t_unhideToken = {};
        }
    });
}
static thread_local int t_clearFrames = 0;
static thread_local winrt::event_token t_clearToken{};
static HWND g_arcMsgWnd = nullptr;
static const UINT WM_ARC_CTRL_T = WM_APP + 0x3b1, WM_ARC_ENTER = WM_APP + 0x3b2;

static mux::FrameworkElement FocusedQueryBox() {
    for (auto h : FindNodes(L"QueryTextBox")) {
        auto fe = Obj<mux::FrameworkElement>(h);
        if (!fe || !fe.XamlRoot()) continue;
        auto f = mux::Input::FocusManager::GetFocusedElement(fe.XamlRoot()).try_as<mux::FrameworkElement>();
        if (!f || f != fe) continue;
        // Only the command bar popover's box (Dia's New Tab page has its own box with the same name).
        for (mux::DependencyObject d = fe; d; d = muxm::VisualTreeHelper::GetParent(d))
            if (auto a = d.try_as<mux::FrameworkElement>(); a && a.Name() == L"PopoverLayoutRoot") return fe;
    }
    return nullptr;
}

static void SendKeys(std::initializer_list<std::pair<WORD, bool>> keys) {  // (vk, down)
    std::vector<INPUT> in;
    for (auto& [vk, down] : keys) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        in.push_back(i);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

static void StartNewTabMode() {
    t_newTabMode = true;
    // Ctrl is usually still held from Ctrl+T, so only L needs pressing; otherwise press Ctrl+L.
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) SendKeys({{'L', true}, {'L', false}});
    else SendKeys({{VK_CONTROL, true}, {'L', true}, {'L', false}, {VK_CONTROL, false}});
    // Empty the box as soon as the command bar has focus (a few frames later).
    t_clearFrames = 60;
    if (t_clearToken) return;
    t_clearToken = muxm::CompositionTarget::Rendering([](auto&&, auto&&) {
        bool done = --t_clearFrames <= 0;
        if (auto q = FocusedQueryBox()) {
            if (auto reb = q.try_as<muxc::RichEditBox>()) reb.Document().SetText(winrt::Microsoft::UI::Text::TextSetOptions::None, L"");
            done = true;
        }
        if (done) {
            muxm::CompositionTarget::Rendering(t_clearToken);
            t_clearToken = {};
        }
    });
}

// The New Tab page's own box, once it has focus (never the command bar popover's box).
static muxc::RichEditBox FocusedNewTabBox() {
    for (auto h : FindNodes(L"QueryTextBox")) {
        auto fe = Obj<mux::FrameworkElement>(h);
        if (!fe || !fe.XamlRoot()) continue;
        auto f = mux::Input::FocusManager::GetFocusedElement(fe.XamlRoot()).try_as<mux::FrameworkElement>();
        if (!f || f != fe) continue;
        bool inPopover = false;
        for (mux::DependencyObject d = fe; d; d = muxm::VisualTreeHelper::GetParent(d))
            if (auto a = d.try_as<mux::FrameworkElement>(); a && a.Name() == L"PopoverLayoutRoot") inPopover = true;
        if (!inPopover) return fe.try_as<muxc::RichEditBox>();
    }
    return nullptr;
}

static void OpenTextInNewTab(const std::wstring& text);

// Enter in the Ctrl+T bar: take what's typed (with Dia's autocomplete), close the bar, open a real new
// tab, and submit the text in that tab's box. The current tab is never touched.
static void SubmitInNewTab() {
    auto q = FocusedQueryBox().try_as<muxc::RichEditBox>();
    if (!q) return;
    winrt::hstring raw;
    q.Document().GetText(winrt::Microsoft::UI::Text::TextGetOptions::None, raw);
    std::wstring text(raw);
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    t_newTabMode = false;
    SendKeys({{VK_ESCAPE, true}, {VK_ESCAPE, false}});
    if (text.empty()) return;
    OpenTextInNewTab(text);
}

// Opens a real new tab and submits the text in its box (also used by Peek's "open as tab").
static void OpenTextInNewTab(const std::wstring& text) {
    t_pendingQuery = text;
    t_passCtrlT = true;
    bool ctrlHeld = GetAsyncKeyState(VK_CONTROL) & 0x8000;
    if (ctrlHeld) SendKeys({{'T', true}, {'T', false}});
    else SendKeys({{VK_CONTROL, true}, {'T', true}, {'T', false}, {VK_CONTROL, false}});
    // Wait for the new tab's box to take focus, then fill it and press Enter.
    t_submitFrames = 180;
    if (t_submitToken) return;
    t_submitToken = muxm::CompositionTarget::Rendering([](auto&&, auto&&) {
        bool done = --t_submitFrames <= 0;
        if (t_enterIn > 0) {
            // Enter only works once Dia's own query has the text (it reads it a moment after the box
            // shows it): wait for the box to hold exactly what we typed, then a few frames more.
            if (t_settle < 0) {
                if (auto box = FocusedNewTabBox()) {
                    winrt::hstring raw;
                    box.Document().GetText(winrt::Microsoft::UI::Text::TextGetOptions::None, raw);
                    std::wstring cur(raw);
                    while (!cur.empty() && iswspace(cur.back())) cur.pop_back();
                    if (cur.rfind(t_typedQuery, 0) == 0) t_settle = 8;  // (Dia may add autocomplete after it)
                }
            }
            bool fire = (t_settle >= 0 && --t_settle < 0) || --t_enterIn == 0;
            if (fire) {
                SendKeys({{VK_RETURN, true}, {VK_RETURN, false}});  // open it
                UnhidePageLater();
                done = true;
            }
        } else if (auto box = FocusedNewTabBox(); box && !t_pendingQuery.empty()) {
            // Hide the New Tab page while this runs: you see the new tab go straight to the URL.
            for (mux::DependencyObject d = box; d; d = muxm::VisualTreeHelper::GetParent(d))
                if (auto fe = d.try_as<mux::FrameworkElement>(); fe && fe.Name() == L"VariantHost") {
                    fe.Opacity(0);
                    t_hiddenPage = winrt::make_weak(fe.as<mux::UIElement>());
                    break;
                }
            // Type it as real input so Dia's own query logic sees it (setting the text directly doesn't).
            std::vector<INPUT> in;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {  // Ctrl still held from Ctrl+T would turn letters into shortcuts
                INPUT up{};
                up.type = INPUT_KEYBOARD;
                up.ki.wVk = VK_CONTROL;
                up.ki.dwFlags = KEYEVENTF_KEYUP;
                in.push_back(up);
            }
            for (wchar_t ch : t_pendingQuery) {
                INPUT d{};
                d.type = INPUT_KEYBOARD;
                d.ki.wScan = ch;
                d.ki.dwFlags = KEYEVENTF_UNICODE;
                INPUT u = d;
                u.ki.dwFlags |= KEYEVENTF_KEYUP;
                in.push_back(d);
                in.push_back(u);
            }
            SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
            t_typedQuery = t_pendingQuery;
            t_pendingQuery.clear();
            t_settle = -1;
            t_enterIn = 120;  // until Dia has the text (see above), at most ~2 s
            t_submitFrames = (std::max)(t_submitFrames, 120);
        }
        if (done) {
            muxm::CompositionTarget::Rendering(t_submitToken);
            t_submitToken = {};
            t_pendingQuery.clear();
            t_passCtrlT = false;
            t_enterIn = -1;
            if (t_hiddenPage.get() && !t_unhideToken) UnhidePageLater();  // gave up: never leave it hidden
        }
    });
}

void Ui_OpenInNewTab(const std::wstring& text) {
    try {
        if (!text.empty()) OpenTextInNewTab(text);
    } catch (...) {
    }
}

static LRESULT CALLBACK ArcMsgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    try {
        if (msg == WM_ARC_CTRL_T) StartNewTabMode();
        else if (msg == WM_ARC_ENTER) SubmitInNewTab();
    } catch (...) {
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static bool DiaIsForeground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Low-level keyboard hook (runs on this UI thread's message loop). Only acts while Dia is the active app.
static LRESULT CALLBACK ArcKeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        // (Our own Ctrl+L / Alt+Enter never match below, so they can't loop.)
        if (Engine_TweakOn(L"floatingNewTab") && DiaIsForeground()) {
            bool ctrl = GetAsyncKeyState(VK_CONTROL) & 0x8000, alt = GetAsyncKeyState(VK_MENU) & 0x8000,
                 shift = GetAsyncKeyState(VK_SHIFT) & 0x8000;
            if (k->vkCode == 'T' && ctrl && !alt && !shift && t_passCtrlT) {
                t_passCtrlT = false;  // our own new-tab request: Dia handles this one
            } else if (k->vkCode == 'T' && ctrl && !alt && !shift) {
                PostMessageW(g_arcMsgWnd, WM_ARC_CTRL_T, 0, 0);
                return 1;  // Dia never sees it: no New Tab page
            }
            if (k->vkCode == VK_RETURN && !ctrl && !alt && !shift && t_newTabMode && FocusedQueryBox()) {
                PostMessageW(g_arcMsgWnd, WM_ARC_ENTER, 0, 0);
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void InstallArcKeys() {
    if (g_arcMsgWnd) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = ArcMsgProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DiaTweaksArcKeys";
    RegisterClassW(&wc);
    g_arcMsgWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ArcKeyHook, &self);
    if (!SetWindowsHookExW(WH_KEYBOARD_LL, ArcKeyHook, self, 0)) Log("Arc keys: hook failed %lu", GetLastError());
}

// The empty-bar mode ends when the command bar closes.
static void UpdateNewTabMode() {
    if (t_newTabMode && !t_clearToken && !FocusedQueryBox()) t_newTabMode = false;
}

// ---------------------------------------------------------------- window buttons (minimize/maximize/close)
// Hidden window buttons must also stop being buttons: Dia registers their areas with Windows
// (InputNonClientPointerSource), so a click on the empty spot would still minimize or close Dia. While
// hidden, those regions are cleared; Dia's latest ones are put back when the buttons return.

namespace mui = winrt::Microsoft::UI::Input;

struct CaptionState {
    winrt::weak_ref<mux::FrameworkElement> grid;  // the grid holding Dia's three buttons
    mui::InputNonClientPointerSource src{nullptr};
    std::map<int, std::vector<winrt::Windows::Graphics::RectInt32>> saved;  // Dia's regions
    winrt::event_token token{};
    bool hidden = false, self = false;
    // Window-drag (caption) areas Dia registers for the toolbar's empty space. With the toolbar hidden
    // the page sits there, so they're trimmed back to the sidebar (pageLeft, in window pixels).
    std::vector<winrt::Windows::Graphics::RectInt32> diaCaption;
    bool captionTrimmed = false;
    int pageLeft = 0;
};
static thread_local std::map<InstanceHandle, CaptionState> t_caption;
static const mui::NonClientRegionKind kButtonKinds[] = {
    mui::NonClientRegionKind::Close, mui::NonClientRegionKind::Minimize, mui::NonClientRegionKind::Maximize};

void ProbeRegions(std::ostringstream& o) {
    for (auto& [h, c] : t_caption) {
        if (!c.src) continue;
        o << "pageLeft=" << c.pageLeft << " trimmed=" << c.captionTrimmed << "\n";
        const char* names[] = {"Caption", "Close", "Maximize", "Minimize", "Icon", "TopBorder", "LeftBorder",
                               "BottomBorder", "RightBorder", "Passthrough"};
        for (int k = 0; k <= 9; k++) {
            auto rects = c.src.GetRegionRects((mui::NonClientRegionKind)k);
            if (!rects.size()) continue;
            o << "  " << names[k] << ":";
            for (auto r : rects) o << " [" << r.X << "," << r.Y << " " << r.Width << "x" << r.Height << "]";
            o << "\n";
        }
    }
}

static void TrimCaption(CaptionState& c) {
    if (!c.src) return;
    auto rects = c.src.GetRegionRects(mui::NonClientRegionKind::Caption);
    std::vector<winrt::Windows::Graphics::RectInt32> keep;
    bool changed = false;
    for (auto r : rects) {
        if (r.X >= c.pageLeft) { changed = true; continue; }            // over the page: drop
        if (r.X + r.Width > c.pageLeft) r.Width = c.pageLeft - r.X, changed = true;  // straddles: clip
        keep.push_back(r);
    }
    if (!changed) return;
    if (!c.captionTrimmed) c.diaCaption.assign(rects.begin(), rects.end());
    c.self = true;
    c.src.SetRegionRects(mui::NonClientRegionKind::Caption, keep);
    c.self = false;
    c.captionTrimmed = true;
}

static void UntrimCaption(CaptionState& c) {
    if (!c.src || !c.captionTrimmed) return;
    c.self = true;
    c.src.SetRegionRects(mui::NonClientRegionKind::Caption, c.diaCaption);
    c.self = false;
    c.captionTrimmed = false;
}

static void ClearButtonRegions(CaptionState& c) {
    if (!c.src) return;
    c.self = true;
    for (auto k : kButtonKinds) {
        auto rects = c.src.GetRegionRects(k);
        if (rects.size()) c.saved[(int)k] = std::vector<winrt::Windows::Graphics::RectInt32>(rects.begin(), rects.end());
        c.src.ClearRegionRects(k);
    }
    c.self = false;
}

static void RestoreButtonRegions(CaptionState& c) {
    if (!c.src) return;
    c.self = true;
    for (auto& [k, rects] : c.saved)
        if (!rects.empty()) c.src.SetRegionRects((mui::NonClientRegionKind)k, rects);
    c.self = false;
}

// Find the window-button grid for every browser window and remember it.
static void FindCaptions() {
    for (auto it = t_caption.begin(); it != t_caption.end();) {
        auto grid = it->second.grid.get();
        if (grid && grid.XamlRoot()) {
            ++it;
            continue;
        }
        if (it->second.src) {
            try {
                it->second.src.RegionsChanged(it->second.token);
            } catch (...) {
            }
        }
        it = t_caption.erase(it);
    }
    for (auto h : FindNodes(L"MinimizeButton")) {
        InstanceHandle grid = Engine_Parent(h);
        if (Engine_Name(grid) != L"RootGrid" || t_caption.count(grid)) continue;
        auto fe = Obj<mux::FrameworkElement>(grid);
        if (!fe) continue;
        auto root = fe.XamlRoot();
        if (!root) continue;
        CaptionState c;
        c.grid = winrt::make_weak(fe);
        try {
            auto environment = root.ContentIslandEnvironment();
            if (environment) c.src = mui::InputNonClientPointerSource::GetForWindowId(environment.AppWindowId());
        } catch (...) {
        }
        Log("window buttons: found grid, region source %s", c.src ? "ok" : "missing");
        t_caption[grid] = c;
        if (auto& cs = t_caption[grid]; cs.src) {
            cs.token = cs.src.RegionsChanged([grid](auto&&, mui::NonClientRegionsChangedEventArgs const&) {
                auto it = t_caption.find(grid);
                if (it == t_caption.end() || it->second.self) return;
                if (it->second.captionTrimmed) {
                    it->second.captionTrimmed = false;  // Dia set fresh ones: these are now Dia's
                    TrimCaption(it->second);
                }
                if (it->second.hidden) ClearButtonRegions(it->second);  // keep hidden buttons unclickable
            });
        }
    }
}

// Show or hide one window's buttons; `animate` fades them along with the toolbar slide.
static void SetCaptionHidden(CaptionState& c, bool hide, bool animate) {
    auto fe = c.grid.get();
    if (!fe || c.hidden == hide) return;
    c.hidden = hide;
    try {
        auto visual = winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(fe);
        if (animate) {
            auto comp = visual.Compositor();
            auto fade = comp.CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(1.0f, hide ? 0.0f : 1.0f,
                                comp.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f}));
            fade.Duration(std::chrono::milliseconds(hide ? 140 : 200));
            visual.StartAnimation(L"Opacity", fade);
        } else {
            visual.StopAnimation(L"Opacity");
            visual.Opacity(hide ? 0.0f : 1.0f);
        }
    } catch (...) {
    }
    fe.IsHitTestVisible(!hide);
    if (hide) ClearButtonRegions(c);
    else RestoreButtonRegions(c);
}

// Dia draws each button's hover background inset (4px from the top, and from the right on Close), so the
// buttons don't reach the window's corner. Stretch that background to fill the whole button. The buttons
// are also 4px taller than the toolbar strip behind them, so they're cut to its height.
// Close has a rounded top-right corner to follow the window's corner; when the window is maximized or full
// screen the window has square corners, so Close does too.
static bool WindowFillsScreen(const mux::FrameworkElement& el) {
    try {
        // An element can outlive its window and lose its XAML root during teardown.
        if (!el) return false;
        auto root = el.XamlRoot();
        if (!root) return false;
        auto environment = root.ContentIslandEnvironment();
        if (!environment) return false;
        auto id = environment.AppWindowId();
        HWND top = GetAncestor((HWND)(uintptr_t)id.Value, GA_ROOT);
        if (!top) return false;
        if (IsZoomed(top)) return true;
        MONITORINFO mi{sizeof(mi)};
        RECT r;
        if (!GetWindowRect(top, &r) || !GetMonitorInfoW(MonitorFromWindow(top, MONITOR_DEFAULTTONEAREST), &mi)) return false;
        return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
               r.bottom >= mi.rcMonitor.bottom;
    } catch (...) {
        return false;
    }
}

static void SetCloseCorner(const mux::DependencyObject& el, bool square) {
    static thread_local mux::CornerRadius diaRadius{0, 3, 0, 0};  // Dia's own, learned when first seen
    auto set = [&](mux::CornerRadius cur, auto apply) {
        bool isSquare = !cur.TopLeft && !cur.TopRight && !cur.BottomRight && !cur.BottomLeft;
        if (!isSquare && !square) diaRadius = cur;
        mux::CornerRadius want = square ? mux::CornerRadius{0, 0, 0, 0} : diaRadius;
        if (cur.TopLeft != want.TopLeft || cur.TopRight != want.TopRight || cur.BottomRight != want.BottomRight ||
            cur.BottomLeft != want.BottomLeft)
            apply(want);
    };
    if (auto c = el.try_as<muxc::Control>()) set(c.CornerRadius(), [&](mux::CornerRadius r) { c.CornerRadius(r); });
    else if (auto b = el.try_as<muxc::Border>()) set(b.CornerRadius(), [&](mux::CornerRadius r) { b.CornerRadius(r); });
}

static void GlueCaptionButtons(const mux::FrameworkElement& grid, double height) {
    bool square = WindowFillsScreen(grid);
    int n = muxm::VisualTreeHelper::GetChildrenCount(grid);
    for (int i = 0; i < n; i++) {
        auto child = muxm::VisualTreeHelper::GetChild(grid, i);
        auto btn = child.try_as<muxc::Button>();
        if (btn && height > 1) {
            if (std::isnan(btn.Height()) || std::abs(btn.Height() - height) > 0.1) btn.Height(height);
            if (btn.VerticalAlignment() != mux::VerticalAlignment::Top) btn.VerticalAlignment(mux::VerticalAlignment::Top);
        }
        auto bg = FindVisual(child, [](const mux::DependencyObject& d) {
            auto fe = d.try_as<mux::FrameworkElement>();
            return fe && fe.Name() == L"BackgroundBorder";
        }).try_as<mux::FrameworkElement>();
        if (btn && btn.Name() == L"CloseButton") {
            SetCloseCorner(btn, square);
            if (bg) SetCloseCorner(bg, square);
        }
        if (!bg) continue;
        auto m = bg.Margin();
        if (m.Left || m.Top || m.Right || m.Bottom) bg.Margin({0, 0, 0, 0});
        if (!std::isnan(bg.Width())) bg.Width(std::numeric_limits<double>::quiet_NaN());
        if (!std::isnan(bg.Height())) bg.Height(std::numeric_limits<double>::quiet_NaN());
        if (bg.HorizontalAlignment() != mux::HorizontalAlignment::Stretch) bg.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        if (bg.VerticalAlignment() != mux::VerticalAlignment::Stretch) bg.VerticalAlignment(mux::VerticalAlignment::Stretch);
    }
}

// For the "Hide window buttons" tweak (no animation) and for windows with no hidden toolbar.
static void UpdateCaptionButtons(const std::function<bool(const mux::FrameworkElement&)>& toolbarHidden) {
    FindCaptions();
    bool always = Engine_TweakOn(L"hideWindowButtons");
    for (auto& [h, c] : t_caption) {
        auto fe = c.grid.get();
        if (!fe) continue;
        auto root = fe.XamlRoot();
        if (!root) continue;
        double barH = 0;  // this window's toolbar strip
        for (auto bh : FindNodes(L"NavigationBarHost"))
            if (auto bar = Obj<mux::FrameworkElement>(bh); bar && bar.XamlRoot() == root && bar.ActualHeight() > 1) {
                barH = bar.ActualHeight();
                break;
            }
        GlueCaptionButtons(fe, barH);
        bool hide = always || toolbarHidden(fe);
        if (hide != c.hidden) SetCaptionHidden(c, hide, !always);
    }
}

// ---------------------------------------------------------------- hidden toolbar (slides with the page)
// Hidden = the toolbar stays laid out but the page container (ContentGroup) is moved up by the toolbar's
// height, so the toolbar sits above the window. Showing/hiding animates that offset every frame.
// While sliding, the page keeps its size (only its position changes, which Dia passes on to its
// web-content window as a cheap move); the one resize happens at the start of a hide or the end of a
// show. It slides the toolbar with transforms instead of reflowing.

struct BarAnim {
    winrt::weak_ref<mux::FrameworkElement> group, bar;
    mux::Thickness saved{};
    double offset = 0;  // 0 = shown, -h = hidden
    double from = 0, to = 0;
    double start = 0;
    bool animating = false, managed = false;
    bool gpu = false;  // this slide moves the page on the compositor (Dia's start page, see below)
};

// Dia's start page (New Tab) is drawn in XAML with effects, so moving it by margin re-lays it out every
// frame and stutters. For it, the slide runs as a compositor translation and the margin is set once,
// at the end. Web pages are separate windows that only move, so they keep the margin slide.
static void SetTranslation(const mux::UIElement& el, float y, bool animateTo, double ms) {
    using winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview;
    ElementCompositionPreview::SetIsTranslationEnabled(el, true);
    auto v = ElementCompositionPreview::GetElementVisual(el);
    if (!animateTo) {
        v.StopAnimation(L"Translation");
        v.Properties().InsertVector3(L"Translation", {0.0f, y, 0.0f});
        return;
    }
    auto c = v.Compositor();
    auto anim = c.CreateVector3KeyFrameAnimation();
    anim.InsertKeyFrame(0.0f, {0.0f, 0.0f, 0.0f});
    anim.InsertKeyFrame(1.0f, {0.0f, y, 0.0f}, c.CreateCubicBezierEasingFunction({0.215f, 0.61f}, {0.355f, 1.0f}));
    anim.Duration(std::chrono::milliseconds((int)ms));
    v.StartAnimation(L"Translation", anim);
}
static thread_local std::map<InstanceHandle, BarAnim> t_bars;
static thread_local winrt::event_token t_renderToken{};
static thread_local bool t_renderHooked = false;
static const double kSlideMs = 200;

static double NowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

static double BarHeight(BarAnim& a) {
    auto b = a.bar.get();
    double h = b ? b.ActualHeight() : 0;
    return h > 1 ? h : 40;
}

// settled: fit the page to the window. Otherwise keep the page at its full (hidden-toolbar) height so
// moving it doesn't resize it.
static void ApplyOffset(BarAnim& a, double h, bool settled) {
    auto g = a.group.get();
    if (!g) return;
    double top = a.offset;
    // On Dia's start page the page keeps one height even when settled (it centers its content, so a
    // resize would make it jump); websites fit the window once the slide ends.
    bool constantHeight = !settled || Ui_State(L"newTab");
    double bottom = constantHeight ? -(h + top) : 0;
    mux::Thickness m{a.saved.Left, a.saved.Top + top, a.saved.Right, a.saved.Bottom + bottom};
    auto cur = g.Margin();
    if (cur.Left != m.Left || cur.Top != m.Top || cur.Right != m.Right || cur.Bottom != m.Bottom) g.Margin(m);
}

static void OnRendering(wf::IInspectable const&, wf::IInspectable const&);

static void EnsureRendering(bool on) {
    t_sliding = on;
    if (on && !t_renderHooked) {
        t_renderToken = muxm::CompositionTarget::Rendering(OnRendering);
        t_renderHooked = true;
    } else if (!on && t_renderHooked) {
        muxm::CompositionTarget::Rendering(t_renderToken);
        t_renderHooked = false;
    }
}

static void OnRendering(wf::IInspectable const&, wf::IInspectable const&) {
    bool any = false;
    double now = NowMs();
    for (auto& [hnd, a] : t_bars) {
        if (!a.animating) continue;
        double h = BarHeight(a);
        double t = (now - a.start) / kSlideMs;
        if (t >= 1) {
            a.offset = a.to;
            a.animating = false;
            ApplyOffset(a, h, true);
            if (a.gpu) {
                // Same frame: the margin takes over from the translation, so nothing jumps.
                if (auto g = a.group.get()) SetTranslation(g, 0, false, 0);
                a.gpu = false;
            }
            continue;
        }
        double e = 1 - std::pow(1 - t, 3);  // ease-out cubic
        a.offset = a.from + (a.to - a.from) * e;
        if (!a.gpu) ApplyOffset(a, h, false);
        any = true;
    }
    if (!any) EnsureRendering(false);
}

static bool FocusWithin(const mux::FrameworkElement& bar) {
    try {
        auto root = bar.XamlRoot();
        if (!root) return false;
        for (auto f = mux::Input::FocusManager::GetFocusedElement(root).try_as<mux::DependencyObject>(); f;
             f = muxm::VisualTreeHelper::GetParent(f))
            if (f == bar) return true;
    } catch (...) {
    }
    return false;
}

static void UpdateCaptionsForToolbar();

// Peek's preview is a Dia window shown clipped to its page: its toolbar stays where Dia puts it (so the
// page is laid out plainly under it, and the clip cuts the toolbar away).
static HWND TopWindowFor(const mux::XamlRoot& root);
static HWND t_peekWindow = nullptr;
void Ui_SetPeekWindow(HWND w) { t_peekWindow = w; }

// Peek's preview window can't be resized, maximized or minimized (its edges, a double-click, Win+Up...).
// False until WinUI has made its AppWindow.
bool Ui_PeekFixedSize(HWND w) {
    try {
        auto app = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(
            winrt::Microsoft::UI::WindowId{(uint64_t)(uintptr_t)w});
        if (!app) return false;
        auto presenter = app.Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>();
        if (!presenter) return false;
        presenter.IsResizable(false);
        presenter.IsMaximizable(false);
        presenter.IsMinimizable(false);
        return true;
    } catch (...) {
        return false;
    }
}

static void UpdateHiddenToolbar() {
    bool hide = Engine_TweakOn(L"hideToolbar") || Engine_TweakOn(L"arcUrlBar");
    bool show = (Engine_TweakOn(L"hideToolbar") && Engine_Hover()) ||
                (Engine_TweakOn(L"showToolbarWithSidebar") && Ui_State(L"sidebarOpen")) ||
                (Engine_TweakOn(L"showToolbarOnNewTab") && Ui_State(L"newTab"));
    bool hideAll = hide;
    for (auto gh : FindNodes(L"ContentGroup")) {
        auto& a = t_bars[gh];
        auto group = a.group.get();
        if (!group) {
            group = Obj<mux::FrameworkElement>(gh);
            if (!group) continue;
            a.group = winrt::make_weak(group);
            auto bar = FindVisual(group, [](const mux::DependencyObject& d) {
                auto fe = d.try_as<mux::FrameworkElement>();
                return fe && fe.Name() == L"NavigationBarHost";
            });
            if (auto fe = bar.try_as<mux::FrameworkElement>()) a.bar = winrt::make_weak(fe);
        }
        // Peek's preview window: left exactly as Dia lays it out (no hiding, no slide), so its page keeps
        // its size while Peek places it.
        bool peek = t_peekWindow && group.XamlRoot() && TopWindowFor(group.XamlRoot()) == t_peekWindow;
        hide = hideAll && !peek;
        if (!hide && !a.managed) continue;
        if (!a.managed) {
            a.saved = group.Margin();
            a.managed = true;
            a.offset = 0;
            a.to = 0;
        }
        auto barEl = a.bar.get();
        bool focused = barEl && FocusWithin(barEl);  // typing in the address bar (Ctrl+L) keeps it out
        bool want = !hide || show || focused;
        double h = BarHeight(a);
        double target = want ? 0 : -h;
        if (!hide && !a.animating && a.offset > -0.5) {
            // Tweak off and the toolbar is fully back: hand Dia its own margin back.
            group.Margin(a.saved);
            a.managed = false;
            continue;
        }
        if (a.to == target && (a.animating || std::abs(a.offset - target) < 0.5)) {
            if (!a.animating) ApplyOffset(a, h, true);  // keep it put if Dia re-laid out the page
            continue;
        }
        if (a.gpu && a.animating) {
            // Changing direction mid-slide: settle where it is now, then start the new slide from there.
            ApplyOffset(a, h, false);
            SetTranslation(group, 0, false, 0);
        }
        a.from = a.offset;
        a.to = target;
        a.start = NowMs();
        a.animating = true;
        a.gpu = Ui_State(L"newTab");
        if (!want || a.gpu) ApplyOffset(a, h, false);  // grow the page once up front; then it only moves
        if (a.gpu) SetTranslation(group, (float)(target - a.from), true, kSlideMs);
        EnsureRendering(true);
    }
    for (auto it = t_bars.begin(); it != t_bars.end();)
        it = it->second.group.get() ? std::next(it) : t_bars.erase(it);
    UpdateCaptionsForToolbar();
}

// Dia also lays an invisible window over the toolbar strip (BoostBrowser.WindowCaptionInputSink) that
// turns presses there into window drags. With the toolbar hidden the page sits under it, so its shape is
// cut down to the sidebar part; Dia's own shape is put back when the toolbar shows.
struct SinkState {
    HWND sink = nullptr;
    HRGN dia = nullptr;     // Dia's shape (owned copy), or null if it had none
    bool diaHadRegion = false;
    HRGN mine = nullptr;    // the shape we last applied (owned copy, for comparing)
};
static thread_local std::map<HWND, SinkState> t_sinks;  // keyed by top-level window

static HWND FindSink(HWND top) {
    struct Ctx { HWND found = nullptr; } ctx;
    EnumChildWindows(top, [](HWND h, LPARAM lp) -> BOOL {
        wchar_t cls[64];
        GetClassNameW(h, cls, 64);
        if (wcscmp(cls, L"BoostBrowser.WindowCaptionInputSink") == 0) {
            reinterpret_cast<Ctx*>(lp)->found = h;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

static HRGN CopyRgn(HRGN r) {
    if (!r) return nullptr;
    HRGN c = CreateRectRgn(0, 0, 0, 0);
    CombineRgn(c, r, nullptr, RGN_COPY);
    return c;
}

static void RestoreSink(SinkState& st) {
    if (!st.sink || !IsWindow(st.sink)) return;
    SetWindowRgn(st.sink, st.diaHadRegion ? CopyRgn(st.dia) : nullptr, TRUE);
    if (st.mine) DeleteObject(st.mine), st.mine = nullptr;
}

// pageLeftPx: where the page starts, in the top-level window's client pixels.
static void TrimSink(HWND top, bool trim, int pageLeftPx) {
    auto& st = t_sinks[top];
    if (!st.sink || !IsWindow(st.sink)) st = SinkState{}, st.sink = FindSink(top);
    if (!st.sink) return;
    if (!trim) {
        if (st.mine) RestoreSink(st);
        return;
    }
    // What shape does the sink have now? If it's not the one we set, Dia changed it: that's Dia's.
    HRGN cur = CreateRectRgn(0, 0, 0, 0);
    bool has = GetWindowRgn(st.sink, cur) != ERROR;
    if (!st.mine || !has || !EqualRgn(cur, st.mine)) {
        if (st.dia) DeleteObject(st.dia);
        st.dia = has ? CopyRgn(cur) : nullptr;
        st.diaHadRegion = has;
        RECT wr;
        GetWindowRect(st.sink, &wr);
        POINT origin{0, 0};
        ClientToScreen(top, &origin);
        int keepW = pageLeftPx - (wr.left - origin.x);  // sidebar part, in the sink's own coordinates
        HRGN want = CreateRectRgn(0, 0, (std::max)(0, keepW), wr.bottom - wr.top);
        if (has) CombineRgn(want, want, cur, RGN_AND);
        if (st.mine) DeleteObject(st.mine);
        st.mine = CopyRgn(want);
        SetWindowRgn(st.sink, want, TRUE);  // the system now owns `want`
    }
    DeleteObject(cur);
}

// The top-level Dia window showing this XAML root: one of this thread's WinUI windows with a caption
// sink, whose client size matches the root's.
static HWND TopWindowFor(const mux::XamlRoot& root) {
    struct Ctx { double w, h; HWND best = nullptr; double bestDiff = 1e9; } ctx;
    if (!root) return nullptr;  // (a window being made or closed: calling through null would crash)
    auto sz = root.Size();
    double scale = root.RasterizationScale();
    ctx.w = sz.Width * scale;
    ctx.h = sz.Height * scale;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM lp) -> BOOL {
        auto& c = *reinterpret_cast<Ctx*>(lp);
        wchar_t cls[64];
        GetClassNameW(h, cls, 64);
        if (wcscmp(cls, L"WinUIDesktopWin32WindowClass") != 0 || !FindSink(h)) return TRUE;
        RECT r;
        GetClientRect(h, &r);
        double d = std::abs((r.right - r.left) - c.w) + std::abs((r.bottom - r.top) - c.h);
        if (d < c.bestDiff) c.bestDiff = d, c.best = h;
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.best;
}

// While a window's toolbar is hidden, its old drag areas over the page are trimmed away.
static void UpdateCaptionTrim() {
    for (auto& [h, c] : t_caption) {
        auto grid = c.grid.get();
        if (!grid || !c.src) continue;
        auto root = grid.XamlRoot();
        if (!root) continue;
        bool hiddenBar = false;
        double left = 0;
        for (auto& [bh, a] : t_bars) {
            auto g = a.group.get();
            if (!g || !a.managed || g.XamlRoot() != root) continue;
            hiddenBar = a.to < -0.5;
            left = g.TransformToVisual(nullptr).TransformPoint({0, 0}).X * root.RasterizationScale();
        }
        HWND top = TopWindowFor(root);
        if (hiddenBar) {
            int pl = (int)left;
            if (pl != c.pageLeft) {
                c.pageLeft = pl, UntrimCaption(c);  // sidebar resized: start from Dia's again
                if (top) TrimSink(top, false, pl);
            }
            TrimCaption(c);
        } else
            UntrimCaption(c);
        if (top) TrimSink(top, hiddenBar, c.pageLeft);
    }
}

// Window buttons follow the toolbar: they fade out as it slides away and back in as it comes back.
static void UpdateCaptionsForToolbar() {
    UpdateCaptionButtons([](const mux::FrameworkElement& grid) {
        auto root = grid.XamlRoot();
        for (auto& [h, a] : t_bars) {
            auto g = a.group.get();
            if (!g || !a.managed || g.XamlRoot() != root) continue;
            return a.to < -0.5;  // heading to (or at) hidden
        }
        return false;
    });
    UpdateCaptionTrim();
}

void Ui_StateApplied() {
    try {
        UpdateHiddenToolbar();
    } catch (...) {
    }
}

// ---------------------------------------------------------------- watching overridden properties
// When Dia sets a property that a tweak overrides, WinUI calls us synchronously, before the frame is
// drawn, so the engine can put the tweak's value straight back. Without this, Dia's value shows until
// the next re-assert (a visible flash, e.g. the page inset while the sidebar opens or closes).

static thread_local std::set<std::pair<InstanceHandle, std::wstring>> t_watched;

static std::wstring Num(double v) {
    wchar_t b[32];
    swprintf(b, 32, L"%g", v);
    return b;
}

static mux::DependencyProperty PropFor(const mux::DependencyObject& o, const std::wstring& prop) {
    if (prop == L"Margin") return mux::FrameworkElement::MarginProperty();
    if (prop == L"Visibility") return mux::UIElement::VisibilityProperty();
    if (prop == L"Opacity") return mux::UIElement::OpacityProperty();
    if (prop == L"Width") return mux::FrameworkElement::WidthProperty();
    if (prop == L"Height") return mux::FrameworkElement::HeightProperty();
    if (prop == L"CornerRadius") {
        if (o.try_as<muxc::Border>()) return muxc::Border::CornerRadiusProperty();
        if (o.try_as<muxc::Grid>()) return muxc::Grid::CornerRadiusProperty();
        if (o.try_as<muxc::Control>()) return muxc::Control::CornerRadiusProperty();
    }
    if (prop == L"BorderThickness") {
        if (o.try_as<muxc::Border>()) return muxc::Border::BorderThicknessProperty();
        if (o.try_as<muxc::Grid>()) return muxc::Grid::BorderThicknessProperty();
        if (o.try_as<muxc::Control>()) return muxc::Control::BorderThicknessProperty();
    }
    if (prop == L"Padding") {
        if (o.try_as<muxc::Border>()) return muxc::Border::PaddingProperty();
        if (o.try_as<muxc::Grid>()) return muxc::Grid::PaddingProperty();
        if (o.try_as<muxc::Control>()) return muxc::Control::PaddingProperty();
    }
    return nullptr;
}

static std::wstring ValueText(const wf::IInspectable& v) {
    if (!v) return {};
    if (auto t = v.try_as<wf::IReference<mux::Thickness>>()) {
        auto x = t.Value();
        return Num(x.Left) + L"," + Num(x.Top) + L"," + Num(x.Right) + L"," + Num(x.Bottom);
    }
    if (auto c = v.try_as<wf::IReference<mux::CornerRadius>>()) {
        auto x = c.Value();
        return Num(x.TopLeft) + L"," + Num(x.TopRight) + L"," + Num(x.BottomRight) + L"," + Num(x.BottomLeft);
    }
    if (auto d = v.try_as<wf::IReference<double>>()) return Num(d.Value());
    if (auto vis = v.try_as<wf::IReference<mux::Visibility>>())
        return vis.Value() == mux::Visibility::Visible ? L"Visible" : L"Collapsed";
    return {};
}

void Ui_WatchProperty(InstanceHandle h, const std::wstring& prop) {
    if (t_watched.count({h, prop})) return;
    try {
        auto o = Obj<mux::DependencyObject>(h);
        if (!o) return;
        auto dp = PropFor(o, prop);
        if (!dp) return;
        t_watched.insert({h, prop});
        o.RegisterPropertyChangedCallback(dp, [h, prop](mux::DependencyObject const& sender,
                                                        mux::DependencyProperty const& p) {
            try {
                Engine_OnDiaChanged(h, prop, ValueText(sender.ReadLocalValue(p)));
            } catch (...) {
            }
        });
    } catch (...) {
    }
}

// ---------------------------------------------------------------- tick

bool Ui_Tick() {
    static thread_local int tick = 0;
    tick++;
    if (tick == 20) {
        size_t n = 0;
        Engine_ForEachNode([&](const NodeRef&) { n++; });
        Log("ui running on thread %lu (%zu elements)", GetCurrentThreadId(), n);
    }
    bool changed = false;
    try {
        if (tick % 2 == 0) changed = UpdateStates();
        UpdateArcBar();
        StyleStartPages();
        UpdateHiddenToolbar();
        if (Engine_TweakOn(L"floatingNewTab")) {
            InstallArcKeys();
            if (!Engine_TweakOn(L"arcUrlBar")) HookCommandBarPopovers();  // (Arc mode hooks them already)
        }
        UpdateNewTabMode();
        Peek_Tick();
        UpdateHoverCards();
        if (tick % 3 == 0) {
            UpdateWindowMaterial();
            UpdateToolbarLook();
            UpdateAddressField();
            UpdateProfileEmoji();
            UpdateSquareCorners();
        }
        UpdateDownloadsPopup();
        if (tick % 5 == 0) {
            ScanForSettings();
            SyncToggles();
            SyncUpdates();
            RunProbe();
        }
    } catch (winrt::hresult_error const& e) {
        Log("ui tick error 0x%08lx", (unsigned long)e.code());
    } catch (...) {
        Log("ui tick error");
    }
    return changed;
}

// The address Dia shows at the bottom left of a window while the pointer is over a link: empty when the
// pointer isn't on one. Dia sets the text as soon as the pointer reaches a link and clears it when it
// leaves; the bubble itself only fades in a moment later, so its visibility isn't waited for.
std::wstring Ui_HoveredLink(HWND top) {
    try {
        for (auto h : FindNodes(L"StatusText")) {
            auto tb = Obj<muxc::TextBlock>(h);
            if (!tb || !tb.XamlRoot() || TopWindowFor(tb.XamlRoot()) != top) continue;
            std::wstring text(tb.Text());
            if (text.empty())
                for (auto const& in : tb.Inlines())
                    if (auto run = in.try_as<winrt::Microsoft::UI::Xaml::Documents::Run>()) text += run.Text();
            if (!text.empty()) return text;
        }
    } catch (...) {
    }
    return L"";
}

// ---------------------------------------------------------------- Peek: move the preview's tab

static mux::FrameworkElement FirstIn(const mux::DependencyObject& root,
                                     const std::function<bool(const mux::FrameworkElement&)>& want) {
    std::vector<mux::DependencyObject> st{root};
    int n = 0;
    while (!st.empty() && n++ < 5000) {
        auto d = st.back();
        st.pop_back();
        if (auto fe = d.try_as<mux::FrameworkElement>(); fe && want(fe)) return fe;
        int c = muxm::VisualTreeHelper::GetChildrenCount(d);
        for (int i = c - 1; i >= 0; i--) st.push_back(muxm::VisualTreeHelper::GetChild(d, i));
    }
    return nullptr;
}

static mux::FrameworkElement PeekTabList(HWND window) {
    for (auto h : FindNodes(L"TabListPresenter")) {
        auto list = Obj<mux::FrameworkElement>(h);
        auto root = list ? list.XamlRoot() : nullptr;
        if (!root) continue;
        auto env = root.ContentIslandEnvironment();
        HWND top = env ? GetAncestor((HWND)(uintptr_t)env.AppWindowId().Value, GA_ROOT) : nullptr;
        if ((top ? top : TopWindowFor(root)) == window) return list;
    }
    return nullptr;
}

static mux::FrameworkElement PeekTabLabel(const mux::FrameworkElement& list, HWND window) {
    wchar_t title[512]{};
    GetWindowTextW(window, title, ARRAYSIZE(title));
    auto untrimmed = [](std::wstring text) {
        if (text.ends_with(L"…")) text.pop_back();
        else if (text.ends_with(L"...")) text.resize(text.size() - 3);
        return text;
    };
    std::wstring name = untrimmed(title);
    if (name.empty()) return nullptr;
    return FirstIn(list, [&](const mux::FrameworkElement& fe) {
        auto text = fe.try_as<muxc::TextBlock>();
        if (!text || fe.ActualWidth() <= 1 || fe.ActualHeight() <= 1) return false;
        std::wstring label = untrimmed(std::wstring(text.Text()));
        return !label.empty() && label != L"New Tab" &&
               (label.starts_with(name) || name.starts_with(label));
    });
}

bool Ui_PeekTabRow(HWND win, RECT& out) {
    try {
        auto list = PeekTabList(win);
        auto label = list ? PeekTabLabel(list, win) : nullptr;
        if (!label) return false;
        double scale = label.XamlRoot().RasterizationScale();
        auto o = label.TransformToVisual(nullptr).TransformPoint({0, 0});
        POINT origin{};
        if (!ClientToScreen(win, &origin)) return false;
        out.left = origin.x + (LONG)std::lround(o.X * scale);
        out.top = origin.y + (LONG)std::lround(o.Y * scale);
        out.right = out.left + (LONG)std::lround(label.ActualWidth() * scale);
        out.bottom = out.top + (LONG)std::lround(label.ActualHeight() * scale);
        return !IsRectEmpty(&out);
    } catch (...) {
        return false;
    }
}

bool Ui_PeekShowTabMenu(HWND window) {
    try {
        auto list = PeekTabList(window);
        auto label = list ? PeekTabLabel(list, window) : nullptr;
        for (auto element = label; element && element != list;
             element = muxm::VisualTreeHelper::GetParent(element).try_as<mux::FrameworkElement>()) {
            if (auto flyout = element.ContextFlyout().try_as<muxc::MenuFlyout>()) {
                flyout.ShowAt(element);
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

void Ui_PeekCloseTabMenu(HWND window) {
    try {
        if (auto list = PeekTabList(window))
            for (auto const& popup : muxm::VisualTreeHelper::GetOpenPopupsForXamlRoot(list.XamlRoot()))
                popup.IsOpen(false);
    } catch (...) {
    }
}

// Dia's menu labels omit window IDs. Select the other window only when the menu and native
// windows make the destination unambiguous.
// Peek's split: in `win`'s tab list, the tab titled `title` (your tab before the Peek), its menu opened (0),
// then its "Open as split" chosen (1): Dia shows it beside the tab you're on (the Peek's). -1: can't.
int Ui_PeekSplitWith(HWND win, const std::wstring& title) {
    (void)title;
    try {
        auto list = PeekTabList(win);
        if (!list) return Log("peek: split: no tab list"), -1;
        // Its menu open: choose "Open as split".
        for (auto const& pop : muxm::VisualTreeHelper::GetOpenPopupsForXamlRoot(list.XamlRoot())) {
            auto child = pop.Child();
            if (!child) continue;
            auto item = FirstIn(child, [](const mux::FrameworkElement& fe) {
                auto mi = fe.try_as<muxc::MenuFlyoutItem>();
                return mi && std::wstring_view(mi.Text()).starts_with(L"Open as split");
            });
            if (!item) continue;
            auto mi = item.as<muxc::MenuFlyoutItem>();
            if (!mi.IsEnabled()) return Log("peek: split: Open as split is off"), -1;
            Log("peek: split: invoking Open as split");
            winrt::Microsoft::UI::Xaml::Automation::Peers::MenuFlyoutItemAutomationPeer(mi).Invoke();
            return 1;
        }
        return 0;  // (its menu not open yet: see Ui_PeekTabRowNamed)
    } catch (winrt::hresult_error const& e) {
        Log("peek: split: error 0x%08lx", (unsigned long)e.code());
        return -1;
    } catch (...) {
        return -1;
    }
}

// Where the tab titled `title` is in `win`'s tab list (screen pixels), for a right-click on it.
bool Ui_PeekTabRowNamed(HWND win, const std::wstring& title, RECT& out) {
    try {
        auto list = PeekTabList(win);
        if (!list) return false;
        auto untrimmed = [](std::wstring text) {
            if (text.ends_with(L"…")) text.pop_back();
            else if (text.ends_with(L"...")) text.resize(text.size() - 3);
            return text;
        };
        std::wstring name = untrimmed(title);
        if (name.empty()) return false;
        auto label = FirstIn(list, [&](const mux::FrameworkElement& fe) {
            auto text = fe.try_as<muxc::TextBlock>();
            if (!text || fe.ActualWidth() <= 1 || fe.ActualHeight() <= 1) return false;
            std::wstring l = untrimmed(std::wstring(text.Text()));
            return !l.empty() && l != L"New Tab" && (l.starts_with(name) || name.starts_with(l));
        });
        if (!label) return false;
        double scale = label.XamlRoot().RasterizationScale();
        auto o = label.TransformToVisual(nullptr).TransformPoint({0, 0});
        POINT origin{};
        if (!ClientToScreen(win, &origin)) return false;
        out.left = origin.x + (LONG)std::lround(o.X * scale);
        out.top = origin.y + (LONG)std::lround(o.Y * scale);
        out.right = out.left + (LONG)std::lround(label.ActualWidth() * scale);
        out.bottom = out.top + (LONG)std::lround(label.ActualHeight() * scale);
        return !IsRectEmpty(&out);
    } catch (...) {
        return false;
    }
}

int Ui_PeekMoveTab(HWND win, HWND to) {
    try {
        auto list = PeekTabList(win);
        if (!list || !IsWindow(to)) return Log("peek: no tab list (%d)", list ? 1 : 0), -1;
        auto pops = muxm::VisualTreeHelper::GetOpenPopupsForXamlRoot(list.XamlRoot());
        for (auto const& pop : pops) {
            auto child = pop.Child();
            if (!child) continue;
            auto element = FirstIn(child, [](const mux::FrameworkElement& fe) {
                auto sub = fe.try_as<muxc::MenuFlyoutSubItem>();
                return sub && std::wstring_view(sub.Text()).starts_with(L"Move to Window");
            });
            if (!element) continue;
            auto sub = element.as<muxc::MenuFlyoutSubItem>();
            namespace ap = winrt::Microsoft::UI::Xaml::Automation;
            auto peer = ap::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(sub);
            auto pattern = peer ? peer.GetPattern(ap::Peers::PatternInterface::ExpandCollapse) : nullptr;
            auto provider = pattern.try_as<ap::Provider::IExpandCollapseProvider>();
            if (!provider) return Log("peek: Move to Window can't expand"), -1;
            if (provider.ExpandCollapseState() != ap::ExpandCollapseState::Expanded) {
                provider.Expand();
                return 0;
            }

            struct { HWND from, to; bool other = false; } windows{win, to};
            EnumWindows([](HWND h, LPARAM param) -> BOOL {
                auto& ctx = *reinterpret_cast<decltype(windows)*>(param);
                if (h == ctx.from || h == ctx.to || !IsWindowVisible(h)) return TRUE;
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                wchar_t cls[64]{};
                if (pid == GetCurrentProcessId() && GetClassNameW(h, cls, ARRAYSIZE(cls)) &&
                    wcscmp(cls, L"WinUIDesktopWin32WindowClass") == 0 && FindSink(h)) {
                    ctx.other = true;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&windows));
            if (windows.other) {
                Log("peek: multiple possible destination windows");
                return -1;
            }

            std::vector<muxc::MenuFlyoutItem> destinations;
            for (auto const& item : sub.Items()) {
                auto candidate = item.try_as<muxc::MenuFlyoutItem>();
                if (candidate && candidate.Text() != L"New Window") destinations.push_back(candidate);
            }
            for (auto const& d : destinations)
                Log("peek: Move to Window item '%ls'%s", d.Text().c_str(), d.IsEnabled() ? "" : " (disabled)");
            if (destinations.size() == 2 && destinations[0].IsEnabled() != destinations[1].IsEnabled()) {
                destinations.erase(destinations.begin() + (destinations[0].IsEnabled() ? 1 : 0));
            }
            if (destinations.size() == 2) {
                // The source window holds only the preview's tab. When yours holds one tab too, the labels
                // can't tell them apart: Dia lists the window last active (the preview's) first.
                auto singleTab = [](const muxc::MenuFlyoutItem& item) {
                    return std::wstring_view(item.Text()).ends_with(L" & 1 Tab");
                };
                bool firstIsSource = singleTab(destinations[0]), secondIsSource = singleTab(destinations[1]);
                if (firstIsSource == secondIsSource) firstIsSource = true;
                destinations.erase(destinations.begin() + (firstIsSource ? 0 : 1));
            }
            if (destinations.size() != 1 || !destinations.front().IsEnabled()) {
                Log("peek: no single destination (%zu)", destinations.size());
                return -1;
            }
            Log("peek: invoking Move to Window: %ls", destinations.front().Text().c_str());
            Peek_BeforeMove();  // (a still picture covers the page while it changes windows)
            ap::Peers::MenuFlyoutItemAutomationPeer(destinations.front()).Invoke();
            return 1;
        }
    } catch (winrt::hresult_error const& e) {
        Log("peek: tab menu error 0x%08lx", (unsigned long)e.code());
        return -1;
    } catch (...) {
        return -1;
    }
    return 0;
}

