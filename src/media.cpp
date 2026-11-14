// media.cpp - the Media Controls tweak, after DiaTweaks' Media Controls on macOS: when a tab you've
// switched away from is playing, a card at the bottom of the sidebar shows it: artwork, title, artist and
// an equalizer in the profile's color. Hovering opens it into a small player (progress bar to seek, times,
// go to tab, previous, play/pause, next, mute, ✕); clicking the card goes back to the tab.
//
// On macOS DiaTweaks hooks each tab's Chromium media session. Dia for Windows gives an add-on no such
// access, so this uses what Windows and Dia's own interface show:
//   * Which tabs make sound: Dia's tab row (in the sidebar) shows a speaker button (column 2 of the row's
//     grid) while the tab is audible or muted. Pressing that button is Dia's own mute toggle.
//   * What plays, and play/pause/next/previous/seek: Windows' system media controls (SMTC, the media
//     flyout). Dia's Chromium reports its active media session there with title, artist, artwork,
//     position and which actions the page supports. Only one session at a time, so the transport buttons
//     go to the card whose tab that session belongs to (matched by title, and kept across song changes);
//     other cards offer mute only.
//   * The profile's color: the gradient Dia tints the floating sidebar with (SidebarFloatingTint).
//   * The equalizer: the peak meter Windows keeps for Dia's audio sessions (nothing is captured).
//   * Going back to a tab: a synthetic touch tap on its row (see TouchTap for why not anything else).
//   * Picture in picture: Chromium's PiP window, a top-level window of Dia's process.
// SMTC is asked on a thread of its own (its calls block); the UI threads read a snapshot of it.

#include <unknwn.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Media.Media3D.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>

#include "engine.h"
#include "media_state.h"

namespace wf = winrt::Windows::Foundation;
namespace wmc = winrt::Windows::Media::Control;
namespace wss = winrt::Windows::Storage::Streams;
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;
namespace muxsh = winrt::Microsoft::UI::Xaml::Shapes;
namespace map = winrt::Microsoft::UI::Xaml::Automation::Peers;
namespace mapr = winrt::Microsoft::UI::Xaml::Automation::Provider;

#define Log Engine_Log

HWND Ui_TopWindowFor(const mux::XamlRoot& root);  // (ui.cpp)

namespace {

// ---------------------------------------------------------------- SMTC (its own thread)

enum CmdKind { kToggle = 1, kNext, kPrev, kSeek };
struct Cmd {
    CmdKind kind;
    int session;
    ULONGLONG at;
    double seconds = 0;  // kSeek: where to
};

struct Smtc {
    bool has = false;
    std::wstring title, artist;
    bool playing = false, canToggle = false, canNext = false, canPrev = false, canSeek = false;
    double duration = 0, position = 0, rate = 1;  // seconds; position as of `at`
    ULONGLONG at = 0;                             // GetTickCount64() of `position`
    double PositionNow() const {
        double p = position + (playing ? (GetTickCount64() - at) / 1000.0 * rate : 0);
        return p < 0 ? 0 : p > duration ? duration : p;
    }
    wss::IRandomAccessStream art{nullptr};  // the artwork (of `artOwner`: a new title's comes a bit later)
    std::wstring artOwner;
    int artGen = 0;
    int session = 0;  // which session (changes when another one is picked)
};

std::mutex g_mx;
Smtc g_smtc;
std::vector<Cmd> g_cmds;
std::atomic<bool> g_want{false}, g_running{false};
std::atomic<int> g_sessionSequence{0};
std::atomic<HANDLE> g_wake{nullptr};

Smtc Snapshot() {
    std::lock_guard lk(g_mx);
    return g_smtc;
}

void Send(CmdKind kind, int session, double seconds = 0) {
    Cmd c{kind, session, GetTickCount64(), seconds};
    {
        std::lock_guard lk(g_mx);
        if (!g_want) return;
        // Dragging seeks can replace a queued seek; never let a stalled service grow an unbounded queue.
        if (kind == kSeek && !g_cmds.empty() && g_cmds.back().kind == kSeek) g_cmds.pop_back();
        if (g_cmds.size() >= 16) return;
        g_cmds.push_back(c);
    }
    if (g_wake) SetEvent(g_wake);
}

template <class T>
auto Await(const T& op, const char* name) {
    ULONGLONG start = GetTickCount64();
    if (op.wait_for(std::chrono::seconds(2)) == wf::AsyncStatus::Started) {
        Log("media: %s timed out (worker only)", name);
        op.Cancel();
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    auto result = op.get();
    ULONGLONG elapsed = GetTickCount64() - start;
    if (elapsed >= 100) Log("media: slow %s %llu ms (worker only)", name, elapsed);
    return result;
}

bool IsDia(const wmc::GlobalSystemMediaTransportControlsSession& s) {
    std::wstring id(s.SourceAppUserModelId());
    return id.find(L"TheBrowserCompany.Dia") != std::wstring::npos;
}

void WorkerBody() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
    wmc::GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
    wmc::GlobalSystemMediaTransportControlsSession last{nullptr}, watched{nullptr};
    winrt::event_token tokSessions{}, tokProps{}, tokInfo{}, tokTimeline{};
    auto unwatch = [&] {
        if (watched) {
            try { watched.MediaPropertiesChanged(tokProps); } catch (...) {}
            try { watched.PlaybackInfoChanged(tokInfo); } catch (...) {}
            try { watched.TimelinePropertiesChanged(tokTimeline); } catch (...) {}
            watched = nullptr;
        }
    };
    auto disconnect = [&] {
        unwatch();
        if (mgr) {
            try { mgr.SessionsChanged(tokSessions); } catch (...) {}
            mgr = nullptr;
        }
        last = nullptr;
    };
    int session = 0;
    // A change in what plays wakes this thread at once (instead of at its next look, up to 0.4 s later).
    auto wake = [](auto&&, auto&&) {
        if (g_wake) SetEvent(g_wake);
    };
    std::wstring artFor, lastFirstTitle;
    ULONGLONG artSince = 0, artRead = 0;
    size_t artHash = 0, lastFirstHash = 0, pendHash = 0;
    std::vector<size_t> standIns;  // pictures learned to be stand-ins, never shown
    ULONGLONG pendSince = 0;
    bool artTaken = false;
    std::wstring artOwner;  // the title the artwork is for
    std::vector<size_t> recentHashes;
    wss::IRandomAccessStream pendArt{nullptr};
    int artGen = 0;
    wss::IRandomAccessStream art{nullptr};
    while (g_want) {
        try {
            if (!mgr) {
                mgr = Await(wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync(), "session manager");
                tokSessions = mgr.SessionsChanged(wake);
            }
            // Dia's session: the one playing, else the one shown last (paused: it stays), else any of Dia's.
            wmc::GlobalSystemMediaTransportControlsSession pick{nullptr}, playing{nullptr};
            bool lastThere = false;
            for (auto const& s : mgr.GetSessions()) {
                if (!IsDia(s)) continue;
                if (!pick) pick = s;
                if (last && s == last) lastThere = true;
                if (!playing && s.GetPlaybackInfo().PlaybackStatus() == wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
                    playing = s;
            }
            if (lastThere && last.GetPlaybackInfo().PlaybackStatus() == wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
                pick = last;
            else if (playing) pick = playing;
            else if (lastThere) pick = last;
            last = pick;
            if (pick != watched) {
                unwatch();
                watched = pick;
                session = ++g_sessionSequence;
                art = pendArt = nullptr;
                artHash = pendHash = 0;
                artFor.clear();
                artOwner.clear();
                artGen++;
                artRead = 0;
                artSince = GetTickCount64();
                if (pick) {
                    tokProps = pick.MediaPropertiesChanged(wake);
                    tokInfo = pick.PlaybackInfoChanged(wake);
                    tokTimeline = pick.TimelinePropertiesChanged(wake);
                }
            }
            std::vector<Cmd> cmds;
            {
                std::lock_guard lk(g_mx);
                cmds.swap(g_cmds);
            }
            for (Cmd c : cmds) {
                if (!pick) break;
                if (!g_want || c.session != session || GetTickCount64() - c.at > 2000) continue;
                bool ok = c.kind == kToggle ? Await(pick.TryTogglePlayPauseAsync(), "play/pause")
                        : c.kind == kNext   ? Await(pick.TrySkipNextAsync(), "next")
                        : c.kind == kPrev   ? Await(pick.TrySkipPreviousAsync(), "previous")
                                            : Await(pick.TryChangePlaybackPositionAsync((int64_t)(c.seconds * 1e7)), "seek");
                const char* what[] = {"", "play/pause", "next", "previous", "seek"};
                Log("media: %s %s", what[c.kind], ok ? "sent" : "refused");
            }
            Smtc s;
            if (pick) {
                auto props = Await(pick.TryGetMediaPropertiesAsync(), "metadata");
                auto info = pick.GetPlaybackInfo();
                auto ctl = info.Controls();
                s.has = true;
                s.title = props.Title();
                s.artist = props.Artist();
                s.playing = info.PlaybackStatus() == wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                s.canToggle = ctl.IsPlayPauseToggleEnabled() || ctl.IsPlayEnabled() || ctl.IsPauseEnabled();
                s.canNext = ctl.IsNextEnabled();
                s.canPrev = ctl.IsPreviousEnabled();
                // Chromium reports the position only when it changes course (play, pause, seek), with the
                // time it did: carried forward to now while playing.
                auto tl = pick.GetTimelineProperties();
                s.duration = (tl.EndTime() - tl.StartTime()).count() / 1e7;
                s.canSeek = ctl.IsPlaybackPositionEnabled() && s.duration > 0;
                if (auto r = info.PlaybackRate()) s.rate = r.Value() > 0 ? r.Value() : 1;
                double age = (winrt::clock::now() - tl.LastUpdatedTime()).count() / 1e7;
                if (age < 0 || age > 24 * 3600) age = 0;
                s.position = (tl.Position() - tl.StartTime()).count() / 1e7 + (s.playing ? age * s.rate : 0);
                s.at = GetTickCount64();
                // The artwork. While a song changes, Chromium hands over stand-ins before the page's own
                // artwork: while the session is blank between two songs Spotify shows its logo, and other
                // sites their icon or Chromium's logo for a moment under the new title. Those pictures are
                // learned and never taken: anything seen while the session is blank, and the first picture
                // of a song that's replaced within 3 s. A new picture is taken once it has stayed 0.35 s
                // (a stand-in is replaced before that), one from the last few songs only after 4 s
                // (Chromium can hand the old one over for a moment). The last song's artwork stays until then; after 8 s with
                // nothing, the artwork goes (the card shows the site's icon). Read every 0.25 s for the
                // first 5 s of a song, then every 0.7 s up to 20 s.
                ULONGLONG nowTick = GetTickCount64();
                if (s.title != artFor) {
                    if (artHash) {
                        recentHashes.push_back(artHash);
                        if (recentHashes.size() > 4) recentHashes.erase(recentHashes.begin());
                    }
                    artFor = s.title, artSince = nowTick, artRead = 0, pendHash = 0, artTaken = false;
                }
                if (nowTick - artSince < 20000 && nowTick - artRead >= (nowTick - artSince < 5000 ? 250u : 700u)) {
                    bool first = artRead == 0;
                    artRead = nowTick;
                    size_t hash = 0;
                    bool png = false;
                    wss::IRandomAccessStream got{nullptr};
                    if (auto ref = props.Thumbnail()) {
                        try {
                            auto in = Await(ref.OpenReadAsync(), "artwork open");
                            if (in.Size() > 8 * 1024 * 1024) throw winrt::hresult_error(E_INVALIDARG);
                            wss::InMemoryRandomAccessStream mem;
                            // Bound the copy even if a provider reports an incorrect stream size.
                            Await(wss::RandomAccessStream::CopyAsync(in.GetInputStreamAt(0), mem.GetOutputStreamAt(0),
                                                                   8 * 1024 * 1024 + 1), "artwork copy");
                            if (mem.Size() > 8 * 1024 * 1024) throw winrt::hresult_error(E_INVALIDARG);
                            uint32_t n = (uint32_t)mem.Size();
                            if (n) {
                                wss::DataReader rd(mem.GetInputStreamAt(0));
                                Await(rd.LoadAsync(n), "artwork read");
                                std::vector<uint8_t> bytes(n);
                                rd.ReadBytes(bytes);
                                hash = std::hash<std::string_view>{}(std::string_view((const char*)bytes.data(), n));
                                png = n > 4 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G';
                                mem.Seek(0);
                                got = mem;
                            }
                        } catch (...) {
                        }
                    }
                    auto learn = [&](size_t h, const char* why) {
                        if (!h || std::find(standIns.begin(), standIns.end(), h) != standIns.end()) return;
                        standIns.push_back(h);
                        if (standIns.size() > 32) standIns.erase(standIns.begin());
                        Log("media: stand-in artwork learned (%s)", why);
                    };
                    auto take = [&](wss::IRandomAccessStream a, size_t h) {
                        if (h != artHash) artHash = h, art = a, artGen++;
                        artTaken = true;
                        artOwner = artFor;
                    };
                    bool standIn = std::find(standIns.begin(), standIns.end(), hash) != standIns.end();
                    // Chromium's logo is the same for every page, so the first PNG two titles share is it.
                    if (png && first && !s.title.empty() && hash == lastFirstHash && artFor != lastFirstTitle)
                        learn(hash, "shared by two songs"), standIn = true;
                    if (hash && first && !s.title.empty()) lastFirstHash = hash, lastFirstTitle = artFor;
                    if (s.title.empty()) {
                        learn(hash, "shown between songs");  // (a blank session: whatever it shows is a stand-in)
                    } else if (hash && !standIn) {
                        bool stale = std::find(recentHashes.begin(), recentHashes.end(), hash) != recentHashes.end();
                        if (hash == artHash) {
                            artTaken = true;  // (the same picture as showing: an album's cover, say)
                            artOwner = artFor;
                        } else {
                            // A new picture has to stay a moment (0.35 s; one from the last songs 4 s) before
                            // it's taken. One replaced meanwhile, early in a song, was a stand-in.
                            if (hash != pendHash) {
                                if (pendHash && !artTaken && nowTick - artSince < 3000) learn(pendHash, "replaced at once");
                                pendHash = hash, pendSince = nowTick, pendArt = got;
                            }
                            if (nowTick - pendSince >= (stale ? 4000u : 350u)) take(pendArt, pendHash);
                        }
                    }
                    if (!artTaken && !s.title.empty() && nowTick - artSince >= 8000) take(nullptr, 0);
                }
                s.art = art;
                s.artGen = artGen;
                s.artOwner = artOwner;
                s.session = session;
            }
            {
                std::lock_guard lk(g_mx);
                g_smtc = s;
            }
        } catch (winrt::hresult_error const& e) {
            Log("media: system media controls 0x%08lx", (unsigned long)e.code());
            disconnect();
            Sleep(1000);
        } catch (...) {
            disconnect();
            Sleep(1000);
        }
        WaitForSingleObject(g_wake, GetTickCount64() - artSince < 5000 ? 250 : 400);
    }
    disconnect();
}

void Worker() noexcept {
    try {
        WorkerBody();
    } catch (winrt::hresult_error const& e) {
        Log("media: worker initialization 0x%08lx", (unsigned long)e.code());
    } catch (...) {
        Log("media: worker stopped after an exception");
    }
    {
        std::lock_guard lk(g_mx);
        g_smtc = {};
        g_cmds.clear();
    }
    g_running = false;
}

void RunWorker(bool on) {
    g_want = on;
    if (on && !g_running.exchange(true)) {
        if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_wake) { g_running = false; return; }
        try { std::thread(Worker).detach(); }
        catch (...) { g_running = false; Log("media: couldn't start metadata worker"); }
    } else if (!on && g_wake) {
        SetEvent(g_wake);
        std::lock_guard lk(g_mx);
        g_cmds.clear();
    }
}

// ---------------------------------------------------------------- reading Dia's tab rows

template <class T>
T Obj(InstanceHandle h) {
    IInspectable* p = Engine_Object(h);
    if (!p) return nullptr;
    wf::IInspectable i;
    winrt::attach_abi(i, p);
    return i.try_as<T>();
}

template <class F>
void Walk(const mux::DependencyObject& d, F&& fn, int depth = 0) {
    if (!d || depth > 40) return;
    if (!fn(d)) return;
    int n = muxm::VisualTreeHelper::GetChildrenCount(d);
    for (int i = 0; i < n; i++) Walk(muxm::VisualTreeHelper::GetChild(d, i), fn, depth + 1);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

// "(3) Song - YouTube" → "song - youtube": the unread count some sites put in front goes.
std::wstring Plain(const std::wstring& title) {
    std::wstring t = title;
    if (t.size() > 3 && t[0] == L'(') {
        size_t close = t.find(L") ");
        if (close != std::wstring::npos && close < 8) t = t.substr(close + 2);
    }
    return Lower(t);
}

struct Row {
    mux::FrameworkElement el{nullptr};  // Dia's row (the UserControl whose grid has five columns)
    muxc::Grid grid{nullptr};           // its content: the five-column grid (what a card remembers, see RowRef)
    muxc::Button speaker{nullptr};      // column 2: Dia's sound / mute button, shown while the tab has sound
    std::wstring title;
    std::wstring glyph;
    bool audible = false, active = false;
    muxm::ImageSource favicon{nullptr};
};

bool Shown(const mux::FrameworkElement& fe) {
    for (mux::DependencyObject d = fe; d; d = muxm::VisualTreeHelper::GetParent(d))
        if (auto u = d.try_as<mux::UIElement>(); u && u.Visibility() == mux::Visibility::Collapsed) return false;
    return true;
}

bool ReadRow(const muxc::UserControl& uc, Row& r) {
    auto g = uc.Content().try_as<muxc::Grid>();
    if (!g || g.ColumnDefinitions().Size() != 5) return false;
    r.el = uc;
    r.grid = g;
    if (auto b = uc.Background().try_as<muxm::SolidColorBrush>()) r.active = b.Color().A == 255;  // the selected tab
    for (auto const& c : g.Children()) {
        auto fe = c.try_as<mux::FrameworkElement>();
        if (!fe) continue;
        int col = muxc::Grid::GetColumn(fe);
        if (col == 1 && !r.favicon) {
            Walk(fe, [&](const mux::DependencyObject& d) {
                if (r.favicon) return false;
                auto u = d.try_as<mux::UIElement>();
                if (u && u.Visibility() == mux::Visibility::Collapsed) return false;
                if (auto img = d.try_as<muxc::Image>(); img && img.Source()) r.favicon = img.Source();
                return true;
            });
        } else if (col == 2) {
            if (auto b = fe.try_as<muxc::Button>()) {
                r.speaker = b;
                r.audible = b.Visibility() == mux::Visibility::Visible;
                Walk(b, [&](const mux::DependencyObject& d) {
                    if (auto fi = d.try_as<muxc::FontIcon>()) return r.glyph = fi.Glyph(), false;
                    return true;
                });
            }
        } else if (col == 3 && r.title.empty()) {
            Walk(fe, [&](const mux::DependencyObject& d) {
                if (auto tb = d.try_as<muxc::TextBlock>(); tb && !tb.Text().empty()) return r.title = tb.Text(), false;
                return true;
            });
        }
    }
    return true;
}

// Dia's tab rows are its own controls, written in Swift. They aren't kept between ticks, not even weakly: a
// weak reference to one is made by Dia's Swift side, and resolving it once Dia has let the row go makes
// Swift's runtime stop the process. Feeds that swap tabs' sound on and off quickly (Instagram reels) did that
// within seconds (#63). The row's content grid is plain XAML: that's what's kept, and the row is found from
// it while it's still in the tree.
using RowRef = winrt::weak_ref<muxc::Grid>;

muxc::UserControl RowFrom(const RowRef& ref) {
    auto g = ref.get();
    if (!g) return nullptr;
    mux::DependencyObject d = g;
    for (int i = 0; i < 3; i++) {
        d = muxm::VisualTreeHelper::GetParent(d);
        if (!d) return nullptr;
        if (auto uc = d.try_as<muxc::UserControl>()) return uc.Content() == g ? uc : nullptr;
    }
    return nullptr;
}

struct RowCache {
    std::vector<RowRef> rows;
    unsigned long long revision = ~0ull;
    ULONGLONG scanned = 0;
};

std::vector<Row> RowsIn(const mux::FrameworkElement& paging, const mux::UIElement& skip, RowCache& cache) {
    std::vector<Row> out;
    auto now = GetTickCount64();
    auto revision = Engine_TreeRevision();
    if (cache.revision != revision || now - cache.scanned >= 2000) {
        cache.rows.clear();
        Walk(paging, [&](const mux::DependencyObject& d) {
            if (skip && d == skip) return false;
            if (auto uc = d.try_as<muxc::UserControl>()) {
                auto grid = uc.Content().try_as<muxc::Grid>();
                if (grid && grid.ColumnDefinitions().Size() == 5) {
                    cache.rows.push_back(winrt::make_weak(grid));
                    return false;
                }
            }
            return true;
        });
        cache.revision = revision;
        cache.scanned = now;
    }
    // Sound, title, selection and favicon stay live; only discovery of the tab controls is cached.
    for (auto& ref : cache.rows) {
        auto uc = RowFrom(ref);
        if (!uc || !uc.IsLoaded()) { cache.revision = ~0ull; continue; }
        Row row;
        if (uc.ActualHeight() > 0 && ReadRow(uc, row)) out.push_back(std::move(row));
    }
    return out;
}

// Glyphs of the speaker button. Logged once each, to tell muted from playing.
bool MutedGlyph(const std::wstring& g) {
    static std::set<std::wstring> seen;
    if (!g.empty() && seen.insert(g).second) Log("media: speaker glyph U+%04X", (unsigned)g[0]);
    if (g.empty()) return false;
    wchar_t c = g[0];
    // Segoe Fluent: Mute, Volume0 / Volume with a cross, Volume disabled.
    return c == 0xE74F || c == 0xE992 || c == 0xE198 || c == 0xF3B1 || c == 0xEA8E;
}

bool PipOpen() {
    struct Ctx { bool found = false; } ctx;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
        wchar_t t[128] = {};
        GetWindowTextW(h, t, 128);
        std::wstring s = Lower(t);
        if (s.find(L"picture in picture") != std::wstring::npos || s.find(L"picture-in-picture") != std::wstring::npos)
            return reinterpret_cast<Ctx*>(lp)->found = true, FALSE;
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

// ---------------------------------------------------------------- the cards
// At rest a card is a slim row: the artwork, the title over the artist, and a little equalizer in the
// profile's color that moves while it plays. With the pointer over it, the card grows upward into a small
// player: a progress bar you can click or drag to seek, the time played and left, and a row of controls
// (go to the tab, previous, play/pause, next, mute), with a ✕ in the corner. With more tabs playing, the
// newest is in front and the next one peeks out above it; opening fans them all out. The tab list makes
// room only for the resting stack; the open player lies over the bottom of the list.
//
// Everything moves: cards rise in and sink away (when they come, when they're dismissed, when you go back
// to their tab), the stack's height and the cards' places glide, opening runs on one curve, a new song
// flips the artwork over and rolls the title and artist, and the play and mute symbols morph.

const double kRestH = 52, kOpenH = 144, kPeekH = 7, kPeekInset = 9, kFanGap = 8, kOpenMs = 240,
             kCloseDelayMs = 120, kBtn = 32, kPlayBtn = 38, kClose = 24, kGap = 6;
const wchar_t kPlay = 0xE768, kPause = 0xE769, kNextG = 0xE893, kPrevG = 0xE892, kVolume = 0xE767,
              kMute = 0xE74F, kCloseG = 0xE711, kGlobe = 0xE774, kOpenTab = 0xE8A7;

using Color = winrt::Windows::UI::Color;

double NowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

// cubic-bezier(0.2, 0.8, 0.2, 1): moves at once, settles softly.
double Ease(double p) {
    if (p <= 0) return 0;
    if (p >= 1) return 1;
    auto bez = [](double a, double b, double s) { return 3 * a * s * (1 - s) * (1 - s) + 3 * b * s * s * (1 - s) + s * s * s; };
    double lo = 0, hi = 1, s = p;
    for (int i = 0; i < 30; i++) {
        s = (lo + hi) / 2;
        if (bez(0.2, 0.2, s) < p) lo = s;
        else hi = s;
    }
    return bez(0.8, 1.0, s);
}

double Lerp(double a, double b, double t) { return a + (b - a) * t; }

// A value gliding toward its target, whatever the target does meanwhile (retargets stay smooth).
bool Approach(double& cur, double target, double dt, double tau) {
    if (std::fabs(target - cur) < 0.002) return cur = target, false;
    cur += (target - cur) * (1 - std::exp(-dt / tau));
    return true;
}

std::wstring Clock(double s) {
    if (s < 0) s = 0;
    long t = (long)std::floor(s + 0.5);
    wchar_t buf[32];
    if (t >= 3600) swprintf(buf, 32, L"%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
    else swprintf(buf, 32, L"%ld:%02ld", t / 60, t % 60);
    return buf;
}

struct Theme {
    bool dark = true;
    Color text, text2, border, hover, press, track, fill, profile, tint, artBg;
};

Color Accent(bool dark) {
    try {
        auto r = mux::Application::Current().Resources();
        auto k = winrt::box_value(winrt::hstring(dark ? L"SystemAccentColorLight2" : L"SystemAccentColorDark1"));
        if (r.HasKey(k)) return winrt::unbox_value<Color>(r.Lookup(k));
    } catch (...) {
    }
    return dark ? Color{0xff, 0x99, 0xeb, 0xff} : Color{0xff, 0x00, 0x5f, 0xb8};
}

// `profile`: the profile's color (Dia tints the sidebar with it), or none yet (alpha 0).
Theme ThemeOf(const mux::FrameworkElement& el, Color profile) {
    Theme t;
    t.dark = !el || el.ActualTheme() != mux::ElementTheme::Light;
    uint8_t v = t.dark ? 0xff : 0x00;
    t.text = {0xff, v, v, v};
    t.text2 = {(uint8_t)(t.dark ? 0x9e : 0x8f), v, v, v};
    t.border = {(uint8_t)(t.dark ? 0x1a : 0x14), v, v, v};
    t.hover = {0x14, v, v, v};
    t.press = {0x0c, v, v, v};
    t.track = {(uint8_t)(t.dark ? 0x2a : 0x1f), v, v, v};
    t.profile = profile.A ? Color{0xff, profile.R, profile.G, profile.B} : Accent(t.dark);
    t.fill = t.profile;
    t.tint = t.dark ? Color{0xff, 0x2c, 0x2c, 0x2c} : Color{0xff, 0xfc, 0xfc, 0xfc};
    t.artBg = {0x14, v, v, v};
    return t;
}

muxm::SolidColorBrush Solid(Color c) { return muxm::SolidColorBrush(c); }

void Put(const mux::UIElement& el, double x, double y) {
    if (muxc::Canvas::GetLeft(el) != x) muxc::Canvas::SetLeft(el, x);
    if (muxc::Canvas::GetTop(el) != y) muxc::Canvas::SetTop(el, y);
}

// One line of text, trimmed with … when it doesn't fit. A new
// text rolls in: the old one slides up and fades as the new one comes up from below.
struct FadeText {
    muxc::Canvas box;
    muxc::TextBlock tb, old;
    muxm::SolidColorBrush brush;
    muxm::RectangleGeometry clip;
    Color color{};
    double rollAt = -1;

    FadeText(double size, bool semibold = false) {
        for (auto* t : {&tb, &old}) {
            t->FontSize(size);
            if (semibold) t->FontWeight(winrt::Windows::UI::Text::FontWeight{600});
            t->TextWrapping(mux::TextWrapping::NoWrap);
            t->TextTrimming(mux::TextTrimming::CharacterEllipsis);
            t->IsHitTestVisible(false);
            t->Foreground(brush);
            box.Children().Append(*t);
        }
        old.Opacity(0);
        box.Clip(clip);
        box.IsHitTestVisible(false);
    }
    // Returns true when it starts rolling (the caller keeps the frames coming).
    bool Text(const std::wstring& s, bool animate) {
        if (tb.Text() == s) return false;
        bool roll = animate && !tb.Text().empty();
        if (roll) old.Text(tb.Text()), rollAt = NowMs();
        tb.Text(s);
        if (!roll) Frame(1e18);
        return roll;
    }
    bool Frame(double now) {
        double p = rollAt < 0 ? 1 : (now - rollAt) / 300;
        if (p >= 1) {
            rollAt = -1;
            old.Opacity(0);
            tb.Opacity(1);
            tb.Translation({0, 0, 0});
            return false;
        }
        double e = Ease(p);
        old.Opacity(1 - e);
        old.Translation({0, (float)(-9 * e), 0});
        tb.Opacity(e);
        tb.Translation({0, (float)(9 * (1 - e)), 0});
        return true;
    }
    void Color_(Color c) {
        if (c == color) return;
        color = c;
        brush.Color(c);
    }
    void Place(double x, double y, double w, double h, double alpha) {
        if (w < 0) w = 0;
        Put(box, x, y);
        box.Width(w);
        box.Height(h);
        clip.Rect({0, 0, (float)w, (float)h});
        if (tb.Width() != w) tb.Width(w), old.Width(w);  // (trimmed with … at the box's end)
        box.Opacity(alpha);
    }
};

// A borderless symbol button with a soft fill behind it while the pointer is over it. Its symbol changes
// with a little morph: the old one shrinks and fades, the new one grows back.
struct IconBtn {
    muxc::Button b;
    muxc::FontIcon icon;
    wchar_t glyph = 0, pending = 0;
    double morphAt = -1;  // NowMs() the morph began, or -1
    bool round = false;   // the play button: a filled circle

    IconBtn(wchar_t g, double size, double box, bool round_ = false) {
        glyph = g;
        round = round_;
        icon.Glyph(std::wstring(1, g));
        icon.FontSize(size);
        icon.FontFamily(muxm::FontFamily(L"Segoe Fluent Icons,Segoe MDL2 Assets"));
        b.Content(icon);
        b.Width(box);
        b.Height(box);
        b.Padding({0, 0, 0, 0});
        b.BorderThickness({0, 0, 0, 0});
        double r = round ? box / 2 : 8;
        b.CornerRadius({r, r, r, r});
        b.Background(Solid({0, 0, 0, 0}));
        b.UseSystemFocusVisuals(false);
    }
    void Theme_(const Theme& t) {
        auto r = b.Resources();
        auto put = [&](const wchar_t* k, Color c) { r.Insert(winrt::box_value(winrt::hstring(k)), Solid(c)); };
        Color rest = round ? t.hover : Color{0, 0, 0, 0};
        Color over = round ? Color{(uint8_t)(t.hover.A * 2), t.hover.R, t.hover.G, t.hover.B} : t.hover;
        put(L"ButtonBackground", rest);
        put(L"ButtonBackgroundPointerOver", over);
        put(L"ButtonBackgroundPressed", t.press);
        put(L"ButtonBackgroundDisabled", round ? Color{0x08, t.hover.R, t.hover.G, t.hover.B} : Color{0, 0, 0, 0});
        for (auto k : {L"ButtonBorderBrush", L"ButtonBorderBrushPointerOver", L"ButtonBorderBrushPressed", L"ButtonBorderBrushDisabled"})
            put(k, {0, 0, 0, 0});
        for (auto k : {L"ButtonForeground", L"ButtonForegroundPointerOver", L"ButtonForegroundPressed"}) put(k, t.text);
        put(L"ButtonForegroundDisabled", {0x55, t.text.R, t.text.G, t.text.B});
        b.Background(Solid(rest));
        b.Foreground(Solid(t.text));
    }
    // Returns true when a morph started (the caller keeps the frames coming).
    bool Set(wchar_t g, bool animate) {
        if ((pending ? pending : glyph) == g) return false;
        if (!animate) {
            glyph = g, pending = 0, morphAt = -1;
            icon.Glyph(std::wstring(1, g));
            icon.Scale({1, 1, 1});
            icon.Opacity(1);
            return false;
        }
        pending = g;
        if (morphAt < 0) morphAt = NowMs();
        return true;
    }
    // One frame of the morph: 100 ms down to 0.7 and 0.6 opacity, swap, 160 ms back. False when done.
    bool Frame(double now) {
        if (morphAt < 0) return false;
        double e = now - morphAt, p;
        if (e < 100) {
            p = std::sin(e / 100 * 3.14159265 / 2);
        } else {
            if (pending) glyph = pending, pending = 0, icon.Glyph(std::wstring(1, glyph));
            p = 1 - Ease((e - 100) / 160);
            if (e >= 260) p = 0, morphAt = -1;
        }
        float s = (float)(1 - 0.3 * p);
        icon.CenterPoint({(float)(icon.ActualWidth() / 2), (float)(icon.ActualHeight() / 2), 0});
        icon.Scale({s, s, 1});
        icon.Opacity(1 - 0.4 * p);
        return morphAt >= 0;
    }
};

// Dia's sound level, for the equalizer. Windows keeps a peak meter for every app's audio session, so
// nothing is captured or analysed. All audio interfaces belong to the background worker below; even a
// peak read can wait on Windows' audio service. Dia's tabs play through its audio service (a
// Dia.exe process of its own), so the sessions of any Dia.exe count; they're looked up again every 2 s.
struct Meter {
    std::vector<winrt::com_ptr<IAudioMeterInformation>> meters;
    std::map<DWORD, bool> dia;  // process id -> is it Dia's
    ULONGLONG found = 0;

    bool IsDia(DWORD pid) {
        auto it = dia.find(pid);
        if (it != dia.end()) return it->second;
        bool yes = false;
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH];
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(p, 0, path, &n)) {
                std::wstring s = Lower(std::wstring(path, n));
                yes = s.size() >= 8 && s.compare(s.size() - 8, 8, L"\\dia.exe") == 0;
            }
            CloseHandle(p);
        }
        return dia[pid] = yes;
    }
    void Refresh() {
        ULONGLONG start = GetTickCount64();
        found = GetTickCount64();
        meters.clear();
        dia.clear();  // process ids can be reused
        winrt::com_ptr<IMMDeviceEnumerator> en;
        winrt::com_ptr<IMMDeviceCollection> all;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(en.put()))) ||
            FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, all.put())))
            return;
        UINT devices = 0;
        all->GetCount(&devices);
        for (UINT d = 0; d < devices; d++) {
            winrt::com_ptr<IMMDevice> dev;
            winrt::com_ptr<IAudioSessionManager2> sm;
            winrt::com_ptr<IAudioSessionEnumerator> list;
            if (FAILED(all->Item(d, dev.put())) ||
                FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, sm.put_void())) ||
                FAILED(sm->GetSessionEnumerator(list.put())))
                continue;
            int n = 0;
            list->GetCount(&n);
            for (int i = 0; i < n; i++) {
                winrt::com_ptr<IAudioSessionControl> c;
                if (FAILED(list->GetSession(i, c.put()))) continue;
                auto c2 = c.try_as<IAudioSessionControl2>();
                DWORD pid = 0;
                if (!c2 || FAILED(c2->GetProcessId(&pid)) || !IsDia(pid)) continue;
                if (auto m = c.try_as<IAudioMeterInformation>()) meters.push_back(m);
            }
        }
        static thread_local size_t logged = (size_t)-1;
        if (meters.size() != logged) logged = meters.size(), Log("media: %zu sound meter(s) of Dia's", logged);
        auto elapsed = GetTickCount64() - start;
        if (elapsed >= 100) Log("media: slow audio discovery %llu ms (worker only)", elapsed);
    }
    // The loudest of Dia's sessions now (0..1), or -1 when Dia has none.
    float Peak() {
        if (GetTickCount64() - found > 2000) Refresh();
        if (meters.empty()) return -1;
        float best = -1;
        for (auto& m : meters) {
            float v = 0;
            if (SUCCEEDED(m->GetPeakValue(&v)) && v > best) best = v;
        }
        return best;
    }
};

media::PeakMailbox g_peak;
HANDLE g_peakWake = nullptr;

void MeterWorker() noexcept {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
        Meter meter;
        ULONGLONG lastSlowLog = 0;
        for (;;) {
            if (!g_peak.Wanted(GetTickCount64())) {
                meter.meters.clear();
                meter.dia.clear();
                meter.found = 0;
                WaitForSingleObject(g_peakWake, INFINITE);
                continue;
            }
            ULONGLONG start = GetTickCount64();
            float peak = meter.Peak();
            ULONGLONG end = GetTickCount64();
            g_peak.Publish(peak, end);
            if (end - start >= 100 && end - lastSlowLog >= 10000) {
                lastSlowLog = end;
                Log("media: slow audio sample %llu ms (worker only)", end - start);
            }
            Sleep(33);
        }
    } catch (...) {
        // A failed worker leaves an unavailable/stale sample; the UI still animates without waiting.
        Log("media: audio worker unavailable; using the equalizer fallback");
    }
}

float ReadPeak() {
    static std::once_flag once;
    try {
        std::call_once(once, [] {
            g_peakWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!g_peakWake) throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
            try { std::thread(MeterWorker).detach(); }
            catch (...) { CloseHandle(g_peakWake); g_peakWake = nullptr; throw; }
        });
        auto now = GetTickCount64();
        g_peak.Request(now);
        SetEvent(g_peakWake);
        return g_peak.Read(now);
    } catch (...) {
        return -1;
    }
}

struct Equalizer;
thread_local std::set<Equalizer*> t_eqs;  // equalizers bouncing now (on this thread)
thread_local winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer t_eqTimer{nullptr};
void EqTick();

// The little equalizer on a resting card: four bars that follow how loud Dia's sound is (30 times a
// second, only while the card shows and its tab plays), and lie low while it's paused. Each bar shows the
// level a moment earlier than the one before it, so they ripple rather than move as one.
struct Equalizer {
    std::weak_ptr<void> owner;  // keeps the card alive for a timer callback, without forming a cycle
    muxc::StackPanel panel;
    std::vector<muxm::ScaleTransform> scales;
    std::vector<muxsh::Rectangle> bars;
    double level[4] = {0.2, 0.2, 0.2, 0.2};
    bool running = false;
    Color color{};

    Equalizer() {
        panel.Orientation(muxc::Orientation::Horizontal);
        panel.Spacing(2);
        panel.Height(18);
        panel.IsHitTestVisible(false);
        for (int i = 0; i < 4; i++) {
            muxsh::Rectangle r;
            r.Width(3);
            r.Height(18);
            r.RadiusX(1.5);
            r.RadiusY(1.5);
            muxm::ScaleTransform st;
            st.ScaleY(0.2);
            r.RenderTransform(st);
            r.RenderTransformOrigin({0.5f, 0.5f});
            panel.Children().Append(r);
            scales.push_back(st);
            bars.push_back(r);
        }
    }
    ~Equalizer() { t_eqs.erase(this); }
    void Color_(Color c) {
        if (c == color) return;
        color = c;
        for (auto& r : bars) r.Fill(Solid(c));
    }
    void Run(bool on) {
        if (on == running) return;
        running = on;
        if (on) {
            t_eqs.insert(this);
            if (!t_eqTimer) {
                t_eqTimer = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
                t_eqTimer.Interval(std::chrono::milliseconds(33));
                t_eqTimer.Tick([](auto&&, auto&&) {
                    try { EqTick(); }
                    catch (...) {
                        if (t_eqTimer) t_eqTimer.Stop();
                        Log("media: equalizer update failed; stopped its timer");
                    }
                });
            }
            if (!t_eqTimer.IsRunning()) t_eqTimer.Start();
        } else {
            t_eqs.erase(this);
            for (int i = 0; i < 4; i++) level[i] = 0.2, scales[i].ScaleY(0.2);
            if (t_eqs.empty() && t_eqTimer) t_eqTimer.Stop();
        }
    }
};

void EqTick() {
    if (t_eqs.empty()) {
        if (t_eqTimer) t_eqTimer.Stop();
        return;
    }
    std::vector<std::pair<std::shared_ptr<void>, Equalizer*>> visible;
    for (auto* eq : t_eqs)
        if (auto owner = eq->owner.lock(); owner && eq->panel.IsLoaded() && eq->panel.Opacity() > 0 && Shown(eq->panel))
            visible.push_back({std::move(owner), eq});
    if (visible.empty()) {
        if (t_eqTimer) t_eqTimer.Interval(std::chrono::milliseconds(250));
        return;  // no demand: the audio worker releases its meters and sleeps
    }
    if (t_eqTimer.Interval() != std::chrono::milliseconds(33)) t_eqTimer.Interval(std::chrono::milliseconds(33));
    static thread_local float history[8] = {};
    static thread_local int at = 0;
    static thread_local float lo = 0, hi = 0.05f;
    float peak = ReadPeak();
    if (peak < 0) {  // (no session of Dia's found: a gentle pulse instead)
        double t = GetTickCount64() / 1000.0;
        peak = (float)(0.5 + 0.4 * std::sin(t * 5.3));
        lo = 0, hi = 1;
    } else {
        // Auto-range: music is mixed loud, so its raw level hardly moves. The bars span between a quiet
        // level that creeps up and a loud level that creeps down (each jumping at once past a new extreme),
        // so the ups and downs of the music fill them.
        lo = peak < lo ? peak : lo + (peak - lo) * 0.03f;
        hi = peak > hi ? peak : hi + (peak - hi) * 0.015f;
        if (hi - lo < 0.06f) hi = lo + 0.06f;
    }
    float n = (peak - lo) / (hi - lo);
    n = n < 0 ? 0 : n > 1 ? 1 : n;
    at = (at + 1) % 8;
    history[at] = n;
    const double weight[4] = {0.8, 1.0, 0.9, 0.7};
    for (auto& [owner, eq] : visible) {
        if (!eq->running) continue;
        for (int i = 0; i < 4; i++) {
            double v = std::pow((double)history[(at - i + 8) % 8], 1.3) * weight[i] * 1.15;
            double target = 0.12 + 0.88 * (v > 1 ? 1 : v);
            double& l = eq->level[i];
            l += (target - l) * (target > l ? 0.9 : 0.45);  // (jumps up, falls back quickly)
            if (std::fabs(eq->scales[i].ScaleY() - l) > 0.01) eq->scales[i].ScaleY(l);
        }
    }
}

struct Host;
struct Card {
    bool active = true;
    Host* host = nullptr;
    std::weak_ptr<Host> owner;
    RowRef row;  // Dia's tab row
    std::wstring title;
    bool dismissed = false, smtc = false, playing = false, muted = false, shown = false;
    bool dragging = false;
    double dragFrac = 0;
    int playWant = -1;  // after a click on play/pause: what it should show (until the session agrees)
    ULONGLONG playWantUntil = 0;
    ULONGLONG lastSound = 0, hideUntil = 0;  // hideUntil: going to its tab (it sinks away at once)
    bool hadSong = false;                    // the session has given real song details (not Chromium's stand-in)
    bool holding = false;                    // the last song stays on screen meanwhile (progress bar too)
    ULONGLONG standInSince = 0;              // since when it shows Chromium's stand-in
    muxm::ImageSource favicon{nullptr};
    int artGen = -1;
    int artSession = -1;
    unsigned long long artRequest = 0;
    std::wstring artTitle;
    bool artIsFavicon = false;
    // On the stack: its place (0 front, 1 behind...), and whether it's
    // on its way out.
    bool onStack = false, leaving = false, armed = false, loaded = false;
    double pos = 0;
    double addedAt = 0, leftAt = 0;  // NowMs() it came onto the stack / began leaving
    double sizedW = -1, sizedH = -1;  // the size last given (set again only when it changes)
    double layoutW = -1, layoutH = -1, layoutE = -1, layoutA = -1;
    // A new artwork: the tile turns over, and the new picture is on its back.
    double flipAt = -1;
    mux::UIElement nextArt{nullptr};
    mux::UIElement loadingArt{nullptr};  // a new artwork still decoding
    wf::IAsyncAction artLoad{nullptr};
    std::weak_ptr<Card> self;

    muxc::Border root;
    muxc::Canvas inner;
    muxm::RectangleGeometry innerClip;
    muxm::AcrylicBrush surface;
    muxc::Grid artBox;  // (holds the perspective the tile turns in)
    muxc::Border art;
    FadeText name{13.5, true}, sub{12};
    Equalizer eq;
    // The player, shown while open.
    muxc::Canvas bar;  // the hit area of the progress bar (taller than the track)
    muxc::Border track, fill;
    muxc::TextBlock elapsed, left;
    IconBtn open{kOpenTab, 14, kBtn}, prev{kPrevG, 15, kBtn}, play{kPause, 16, kPlayBtn, true}, next{kNextG, 15, kBtn},
        mute{kVolume, 15, kBtn}, close{kCloseG, 10, kClose};
};

struct Host : std::enable_shared_from_this<Host> {
    bool active = true;
    RowCache rows;
    winrt::weak_ref<muxc::Grid> paging;
    winrt::weak_ref<mux::FrameworkElement> switcher;  // Dia's space switcher (the dots), whose row we share
    mux::VerticalAlignment switcherAlign = mux::VerticalAlignment::Stretch;
    double below = 0;  // its height: the stack sits above it
    muxc::Canvas stack{nullptr};
    std::vector<std::shared_ptr<Card>> cards;    // tracked tabs
    std::vector<std::shared_ptr<Card>> shown;    // wanted on screen, front first
    std::vector<std::shared_ptr<Card>> onStack;  // on screen now (the shown ones and those leaving)
    bool open = false, dark = true, themed = false;
    Color profile{};
    double from = 0, to = 0, startMs = 0, t = 0;  // open-ness: t eased, animating from -> to
    double closeAt = -1;
    double height = 0;  // the resting stack's height now (glides to the wanted one)
    double lastFrame = 0;
    winrt::event_token render{}, sizeChanged{};
    Smtc smtc;  // (as last read, for the progress bar between reads)
    RowRef smtcRow;  // the tab the media session was last matched to
    int smtcSession = -1;                            // (and which session that was)
    Smtc lastGood;            // the session as it last was with a song (ridden over a blank moment)
    ULONGLONG lastGoodAt = 0;
    ULONGLONG tapThroughUntil = 0;  // the cards let a tap through to the tabs until then
    double seekTo = -1;  // a seek sent, held until the session agrees
    ULONGLONG seekAt = 0;
};

// An event keeps both its card and host alive until it returns; queued events become inert at teardown.
struct CardAccess {
    std::shared_ptr<Card> card;
    std::shared_ptr<Host> host;
    explicit operator bool() const { return card && card->active && host && host->active; }
    Card* operator->() const { return card.get(); }
    Card& operator*() const { return *card; }
};

CardAccess LockCard(const std::weak_ptr<Card>& weak) {
    auto card = weak.lock();
    auto host = card ? card->owner.lock() : nullptr;
    return {std::move(card), std::move(host)};
}

thread_local std::map<void*, std::shared_ptr<Host>> t_hosts;
thread_local struct {
    RowRef row;  // a tab to tap once it's scrolled into view
    std::weak_ptr<Host> host;
    int tries = 0;
} t_goTo;

void EnsureFrames(Host& h);
void SetIn(Card& c, bool in);

// Starts a card's exit now (dismissed, or going to its tab), not at the next look at the tabs.
void Leave(Card& c) {
    if (!c.onStack || c.leaving) return;
    c.leaving = true;
    c.leftAt = NowMs();
    SetIn(c, false);
    EnsureFrames(*c.host);
}

// Taps a screen point with a synthetic touch: Dia's row selects its tab on a tap. (Chromium's own way back
// to a tab, the one picture in picture uses, isn't reachable from here; mouse messages sent to the sidebar
// are placed where the real pointer is, not where they say; and Dia's tab rows do have automation peers
// that select, but creating one makes Dia crash a little later.) A touch moves the pointer to where it
// touched and hides it: it's put back where it was while hidden, then a mouse move of zero shows it
// there. Runs on a thread of its own.
std::atomic<bool> g_tapping{false};

bool TouchTap(POINT at, HWND target) {
    if (g_tapping.exchange(true)) return false;
    try {
        Log("media: tapping the tab at %ld,%ld", at.x, at.y);  // (before it happens, in case it goes wrong)
        std::thread([at, target] {
            struct Done { ~Done() { g_tapping = false; } } done;
            auto isTarget = [=] {
                auto under = WindowFromPoint(at);
                return IsWindow(target) && GetForegroundWindow() == target && under &&
                       GetAncestor(under, GA_ROOT) == target;
            };
            // Don't send a delayed tap into a different window or over a newly opened menu.
            if (!isTarget() || GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0) return;
            static std::once_flag once;
            static bool ready = false;
            std::call_once(once, [] { ready = InitializeTouchInjection(1, TOUCH_FEEDBACK_NONE) != FALSE; });
            if (!ready) { Log("media: touch injection unavailable (%lu)", GetLastError()); return; }
            POINTER_TOUCH_INFO t{};
            t.pointerInfo.pointerType = PT_TOUCH;
            t.pointerInfo.pointerId = 0;
            t.pointerInfo.ptPixelLocation = at;
            t.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
            t.orientation = 90;
            t.pressure = 512;  // TOUCH_MASK_PRESSURE accepts 0..1024
            t.rcContact = {at.x - 2, at.y - 2, at.x + 2, at.y + 2};
            t.pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
            POINT back{};
            if (!GetCursorPos(&back) || !isTarget()) return;
            bool ok = InjectTouchInput(1, &t) != FALSE;
            if (!ok) { Log("media: touch down failed (%lu)", GetLastError()); return; }
            Sleep(35);
            t.pointerInfo.pointerFlags = POINTER_FLAG_UP;
            // Always release a successful contact, even if focus changed while it was down.
            for (int retry = 0; retry < 3; ++retry) {
                ok = InjectTouchInput(1, &t) != FALSE;
                if (ok || GetLastError() != ERROR_NOT_READY) break;
                Sleep(1);
            }
            if (!ok) {
                t.pointerInfo.pointerFlags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
                InjectTouchInput(1, &t);
            }
            Sleep(20);
            POINT current{};
            // Touch normally parks the cursor at its contact point. Only restore it if it is still there:
            // don't restore over a different observed position or in another foreground app.
            bool restored = ok && GetForegroundWindow() == target && GetCursorPos(&current) &&
                current.x == at.x && current.y == at.y && GetAsyncKeyState(VK_LBUTTON) >= 0 &&
                GetAsyncKeyState(VK_RBUTTON) >= 0;
            if (restored) {
                SetCursorPos(back.x, back.y);
                INPUT nudge{};
                nudge.type = INPUT_MOUSE;
                nudge.mi.dwFlags = MOUSEEVENTF_MOVE;  // show it at the restored position
                SendInput(1, &nudge, sizeof(INPUT));
            }
            Log(ok ? "media: tab tap sent" : "media: tap failed (%lu)", GetLastError());
        }).detach();
        return true;
    } catch (...) {
        g_tapping = false;
        return false;
    }
}

// Taps Dia's row for the tab. False if the row isn't on screen yet (it's being scrolled into view).
bool TapRow(Host& h, const mux::FrameworkElement& row) {
    if (!row || !row.XamlRoot() || row.ActualHeight() <= 0) return false;
    HWND top = Ui_TopWindowFor(row.XamlRoot());
    if (!top) return Log("media: no window for the tab"), true;
    // On screen: inside the tab list's visible part.
    mux::FrameworkElement view{nullptr};
    for (auto d = muxm::VisualTreeHelper::GetParent(row); d && !view; d = muxm::VisualTreeHelper::GetParent(d))
        if (auto sv = d.try_as<muxc::ScrollViewer>()) view = sv;
    double x = row.ActualWidth() / 2 < 60 ? row.ActualWidth() / 2 : 60, y = row.ActualHeight() / 2;
    auto p = row.TransformToVisual(nullptr).TransformPoint({(float)x, (float)y});
    if (view) {
        auto v = view.TransformToVisual(nullptr).TransformPoint({0, 0});
        if (p.Y < v.Y + 2 || p.Y > v.Y + view.ActualHeight() - 2) return false;
    }
    double scale = row.XamlRoot().RasterizationScale();
    POINT at{(LONG)std::lround(p.X * scale), (LONG)std::lround(p.Y * scale)};
    ClientToScreen(top, &at);
    // The cards may lie over the row (an open player covers the bottom of the list): the tap goes through
    // them for a moment.
    h.stack.IsHitTestVisible(false);
    h.tapThroughUntil = GetTickCount64() + 400;
    return TouchTap(at, top);
}

// Goes to the card's tab. The card sinks away at once, not when Dia gets there.
void GoToTab(Card& c) {
    auto row = RowFrom(c.row);
    if (!row || !row.XamlRoot()) return;
    c.hideUntil = GetTickCount64() + 1500;
    Leave(c);
    if (TapRow(*c.host, row)) return;
    mux::BringIntoViewOptions opts;
    opts.AnimationDesired(false);
    row.StartBringIntoView(opts);
    t_goTo.row = c.row, t_goTo.host = c.owner, t_goTo.tries = 10;  // tapped once it's in view (Media_Tick)
}

void PressSpeaker(Card& c) {
    muxc::UserControl uc = RowFrom(c.row);
    Row r;
    if (!uc || !ReadRow(uc, r) || !r.speaker) return Log("media: no sound button on the tab");
    try {
        map::ButtonAutomationPeer(r.speaker).Invoke();
        Log("media: mute toggled");
    } catch (winrt::hresult_error const& e) {
        Log("media: mute 0x%08lx", (unsigned long)e.code());
    }
}

void SetOpen(Host& h, bool open) {
    if (!h.active) return;
    h.closeAt = -1;
    if (open && h.shown.empty()) open = false;
    if (!open)
        for (auto& c : h.shown)
            if (c->dragging) return;  // (a seek in progress keeps it open)
    if (open == h.open) return;
    h.open = open;
    h.from = h.t;
    h.to = open ? 1 : 0;
    h.startMs = NowMs();
    EnsureFrames(h);
}

void ApplyTheme(Card& c, const Theme& t) {
    c.surface.TintColor(t.tint);
    c.surface.TintOpacity(t.dark ? 0.7 : 0.75);
    c.surface.TintLuminosityOpacity(0.92);
    c.surface.FallbackColor(t.tint);
    c.root.BorderBrush(Solid(t.border));
    c.name.Color_(t.text);
    c.sub.Color_(t.text2);
    c.eq.Color_(t.profile);
    c.art.Background(Solid(t.artBg));
    c.track.Background(Solid(t.track));
    c.fill.Background(Solid(t.fill));
    c.elapsed.Foreground(Solid(t.text2));
    c.left.Foreground(Solid(t.text2));
    for (auto* b : {&c.open, &c.prev, &c.play, &c.next, &c.mute, &c.close}) b->Theme_(t);
    c.close.b.Foreground(Solid(t.text2));
}

// The progress bar: click or drag to seek (the time shown follows the pointer until it's let go).
void HookSeek(const std::shared_ptr<Card>& card) {
    std::weak_ptr<Card> weak = card;
    auto frac = [](Card& c, mux::Input::PointerRoutedEventArgs const& e) {
        double w = c.bar.ActualWidth();
        double x = e.GetCurrentPoint(c.bar).Position().X;
        return w > 0 ? std::clamp(x / w, 0.0, 1.0) : 0.0;
    };
    card->bar.PointerPressed([weak, frac](wf::IInspectable const&, mux::Input::PointerRoutedEventArgs const& e) {
        auto c = LockCard(weak);
        if (!c || !c->smtc || !c->host->smtc.canSeek || c->host->t < 0.5) return;
        c->dragging = c->bar.CapturePointer(e.Pointer());
        c->dragFrac = frac(*c, e);
        EnsureFrames(*c->host);
        e.Handled(true);
    });
    card->bar.PointerMoved([weak, frac](wf::IInspectable const&, mux::Input::PointerRoutedEventArgs const& e) {
        if (auto c = LockCard(weak); c && c->dragging) c->dragFrac = frac(*c, e), EnsureFrames(*c->host);
    });
    auto end = [weak, frac](wf::IInspectable const&, mux::Input::PointerRoutedEventArgs const& e) {
        auto c = LockCard(weak);
        if (!c || !c->dragging) return;
        c->dragging = false;
        c->dragFrac = frac(*c, e);
        double to = c->dragFrac * c->host->smtc.duration;
        Send(kSeek, c->host->smtc.session, to);
        // Show it there at once; the session catches up on its next read.
        c->host->smtc.position = to, c->host->smtc.at = GetTickCount64();
        c->host->seekTo = to, c->host->seekAt = c->host->smtc.at;
        c->bar.ReleasePointerCaptures();
        e.Handled(true);
    };
    card->bar.PointerReleased(end);
    card->bar.PointerCaptureLost([weak](auto&&, auto&&) {
        if (auto c = LockCard(weak)) c->dragging = false;
    });
}

std::shared_ptr<Card> MakeCard(Host& h) {
    auto c = std::make_shared<Card>();
    c->host = &h;
    c->owner = h.shared_from_this();
    c->eq.owner = c;
    c->self = c;
    std::weak_ptr<Card> weak = c;

    c->root.Background(c->surface);
    c->root.CornerRadius({12, 12, 12, 12});
    c->root.BorderThickness({1, 1, 1, 1});
    c->root.Child(c->inner);
    // In and out run on the compositor (WinUI's implicit transitions), not frame by frame here: it fades in
    // while rising a little, and fades out sinking back.
    c->root.Opacity(0);
    try {
        mux::ScalarTransition fade;
        fade.Duration(std::chrono::milliseconds(260));
        c->root.OpacityTransition(fade);
        mux::Vector3Transition rise;
        rise.Duration(std::chrono::milliseconds(300));
        c->root.TranslationTransition(rise);
    } catch (...) {
    }
    c->root.Loaded([weak](auto&&, auto&&) {
        if (auto c = LockCard(weak)) c->loaded = true;
    });
    c->inner.Background(Solid({0x01, 0, 0, 0}));  // (takes the pointer everywhere on the card)
    c->inner.Clip(c->innerClip);
    try {  // lifted off the sidebar a little
        c->root.Shadow(muxm::ThemeShadow());
        c->root.Translation({0, 10, 16});
    } catch (...) {
    }

    c->art.CornerRadius({7, 7, 7, 7});
    c->art.IsHitTestVisible(false);
    c->art.RotationAxis({0, 1, 0});
    c->artBox.Children().Append(c->art);
    c->artBox.IsHitTestVisible(false);
    try {
        muxm::Media3D::PerspectiveTransform3D persp;
        persp.Depth(260);
        c->artBox.Transform3D(persp);
    } catch (...) {
    }
    c->inner.Children().Append(c->artBox);
    c->inner.Children().Append(c->name.box);
    c->inner.Children().Append(c->sub.box);
    c->inner.Children().Append(c->eq.panel);

    c->bar.Height(18);
    c->bar.Background(Solid({0x01, 0, 0, 0}));
    c->track.Height(4);
    c->track.CornerRadius({2, 2, 2, 2});
    c->fill.Height(4);
    c->fill.CornerRadius({2, 2, 2, 2});
    Put(c->track, 0, 7);
    Put(c->fill, 0, 7);
    c->bar.Children().Append(c->track);
    c->bar.Children().Append(c->fill);
    c->inner.Children().Append(c->bar);
    for (auto* tb : {&c->elapsed, &c->left}) {
        tb->FontSize(11);
        tb->IsHitTestVisible(false);
        c->inner.Children().Append(*tb);
    }
    c->left.TextAlignment(mux::TextAlignment::Right);
    c->left.Width(70);
    for (auto* b : {&c->open, &c->prev, &c->play, &c->next, &c->mute, &c->close}) c->inner.Children().Append(b->b);
    HookSeek(c);

    c->open.b.Click([weak](auto&&, auto&&) { if (auto c = LockCard(weak)) GoToTab(*c); });
    c->prev.b.Click([weak](auto&&, auto&&) { if (auto c = LockCard(weak); c && c->smtc) Send(kPrev, c->host->smtc.session); });
    c->next.b.Click([weak](auto&&, auto&&) { if (auto c = LockCard(weak); c && c->smtc) Send(kNext, c->host->smtc.session); });
    c->play.b.Click([weak](auto&&, auto&&) {
        if (auto c = LockCard(weak); c && c->smtc) {
            Send(kToggle, c->host->smtc.session);
            // Show the change at once; the session's own state follows a moment later.
            c->playing = !c->playing;
            c->playWant = c->playing, c->playWantUntil = GetTickCount64() + 2500;
            if (c->play.Set(c->playing ? kPause : kPlay, true)) EnsureFrames(*c->host);
            c->eq.Run(c->playing && !c->muted);
        }
    });
    c->mute.b.Click([weak](auto&&, auto&&) {
        if (auto c = LockCard(weak)) PressSpeaker(*c);
    });
    c->close.b.Click([weak](auto&&, auto&&) {
        if (auto c = LockCard(weak)) c->dismissed = true, Leave(*c), Log("media: card dismissed");
    });
    c->root.PointerEntered([weak](auto&&, auto&&) {
        if (auto c = LockCard(weak); c && !c->leaving) SetOpen(*c->host, true);
    });
    // A short wait, so moving between the cards, or out and straight back, doesn't flicker.
    c->root.PointerExited([weak](auto&&, auto&&) {
        if (auto c = LockCard(weak)) c->host->closeAt = NowMs() + kCloseDelayMs, EnsureFrames(*c->host);
    });
    // A click on the card itself (not a button or the bar) goes to the tab.
    c->root.Tapped([weak](wf::IInspectable const&, mux::Input::TappedRoutedEventArgs const& e) {
        auto c = LockCard(weak);
        if (!c || c->leaving) return;
        for (auto d = e.OriginalSource().try_as<mux::DependencyObject>(); d; d = muxm::VisualTreeHelper::GetParent(d)) {
            if (d.try_as<muxc::Primitives::ButtonBase>()) return;
            if (d == c->bar) return;
        }
        GoToTab(*c);
    });
    return c;
}

void ShowArt(Card& c, const mux::UIElement& next, bool animate) {
    if (animate && c.art.Child()) {
        c.nextArt = next;
        if (c.flipAt < 0) c.flipAt = NowMs();
        EnsureFrames(*c.host);
    } else {
        c.art.Child(next);
    }
}

// The artwork the media session has, else the site's icon on a soft tile, else a globe. A change on a card
// already showing turns the tile over (see FlipFrame); otherwise it's put in place. A new song's artwork
// turns in only once it has loaded, and the last song's stays until then: no site icon in between (that
// shows only if no artwork comes at all).
void SetArt(Card& c, const Smtc& s, bool animate) {
    int session = c.smtc ? s.session : -1;
    std::wstring title = c.smtc ? s.title : L"";
    if (session != c.artSession || title != c.artTitle) {
        ++c.artRequest;
        try { if (c.artLoad) c.artLoad.Cancel(); } catch (...) {}
        c.artLoad = nullptr;
        c.artSession = session;
        c.artTitle = std::move(title);
        c.artGen = -1;
        c.nextArt = nullptr;
        if (auto previous = c.loadingArt) {
            c.loadingArt = nullptr;
            uint32_t i;
            if (c.artBox.Children().IndexOf(previous, i)) c.artBox.Children().RemoveAt(i);
        }
    }
    mux::UIElement next{nullptr};
    bool ours = s.artOwner == s.title;
    if (c.smtc && s.art && ours) {
        if (c.artGen == s.artGen && !c.artIsFavicon) return;
        try {
            muxm::Imaging::BitmapImage bmp;
            bmp.DecodePixelWidth(96);
            muxc::Image img;
            img.Source(bmp);
            img.Stretch(muxm::Stretch::UniformToFill);
            img.HorizontalAlignment(mux::HorizontalAlignment::Center);
            img.VerticalAlignment(mux::VerticalAlignment::Center);
            c.artGen = s.artGen, c.artIsFavicon = false;
            if (auto prev = c.loadingArt) {
                uint32_t i;
                if (c.artBox.Children().IndexOf(prev, i)) c.artBox.Children().RemoveAt(i);
            }
            c.loadingArt = img;
            auto request = ++c.artRequest;
            std::weak_ptr<Card> weak = c.self;
            auto wimg = winrt::make_weak(img);
            bmp.ImageOpened([weak, wimg, animate, request](auto&&, auto&&) {
                auto c = LockCard(weak);
                auto img = wimg.get();
                if (!c || !img || c->artRequest != request) return;
                uint32_t i;
                if (c->artBox.Children().IndexOf(img, i)) c->artBox.Children().RemoveAt(i);
                if (c->loadingArt != img) return;  // (a newer one came meanwhile)
                c->loadingArt = nullptr;
                c->artLoad = nullptr;
                img.Opacity(1);
                ShowArt(*c, img, animate);
            });
            bmp.ImageFailed([weak, wimg, request](auto&&, auto&&) {
                auto c = LockCard(weak);
                auto img = wimg.get();
                if (!c || !img || c->artRequest != request) return;
                uint32_t i;
                if (c->artBox.Children().IndexOf(img, i)) c->artBox.Children().RemoveAt(i);
                if (c->loadingArt == img) c->loadingArt = nullptr, c->artLoad = nullptr;
            });
            // It loads out of sight in the tile (an image that isn't on screen may never load).
            img.Opacity(0);
            img.IsHitTestVisible(false);
            c.artBox.Children().InsertAt(0, img);
            c.artLoad = bmp.SetSourceAsync(s.art.CloneStream());
        } catch (...) {
        }
        return;
    }
    if (c.loadingArt) return;
    if (c.smtc && !ours && !c.artIsFavicon && c.art.Child()) return;  // the last song's stays until the new one's comes
    if (!next && c.favicon) {
        auto img = c.art.Child().try_as<muxc::Image>();
        if (c.artIsFavicon && img && img.Source() == c.favicon) return;
        muxc::Image i;
        i.Source(c.favicon);
        i.Width(18);
        i.Height(18);
        i.Stretch(muxm::Stretch::Uniform);
        next = i;
        c.artGen = -1, c.artIsFavicon = true;
    }
    if (!next) {
        if (c.art.Child().try_as<muxc::FontIcon>()) return;
        muxc::FontIcon i;
        i.Glyph(std::wstring(1, kGlobe));
        i.FontSize(16);
        i.FontFamily(muxm::FontFamily(L"Segoe Fluent Icons,Segoe MDL2 Assets"));
        next = i;
        c.artGen = -1, c.artIsFavicon = true;
    }
    ShowArt(c, next, animate);
}

// One frame of the tile turning over, unhurried: 320 ms easing to edge-on (shrinking and dimming a little),
// the new picture goes in, 480 ms settling back flat.
bool FlipFrame(Card& c, double now) {
    if (c.flipAt < 0) return false;
    double e = now - c.flipAt, angle, k;  // k: 1 edge-on, 0 flat
    if (e < 320) {
        double p = e / 320;
        k = p < 0.5 ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3) / 2;  // ease-in-out
        angle = 90 * k;
    } else {
        if (c.nextArt) c.art.Child(c.nextArt), c.nextArt = nullptr;
        double p = (e - 320) / 480;
        k = 1 - Ease(p);
        angle = -90 * k;
        if (p >= 1) angle = 0, k = 0, c.flipAt = -1;
    }
    float s = (float)(1 - 0.1 * k);
    c.art.CenterPoint({(float)(c.art.ActualWidth() / 2), (float)(c.art.ActualHeight() / 2), 0});
    c.art.Rotation((float)angle);
    c.art.Scale({s, s, 1});
    c.art.Opacity(1 - 0.35 * k);
    return c.flipAt >= 0;
}

// The progress bar and the two times, from the session's position (or where a drag is).
void UpdateProgress(Card& c, const Smtc& s, double w) {
    if (c.holding && c.bar.Width() == w - 24) return;  // (between songs: as it was)
    bool has = c.smtc && s.duration > 0;
    double pos = has ? (c.dragging ? c.dragFrac * s.duration : s.PositionNow()) : 0;
    double frac = has ? pos / s.duration : 0;
    double tw = w - 24;
    if (tw < 0) tw = 0;
    if (c.bar.Width() != tw) c.bar.Width(tw), c.track.Width(tw);
    double fw = tw * frac, mn = has ? 4.0 : 0.0;
    fw = fw < mn ? mn : fw;
    if (std::fabs(c.fill.Width() - fw) > 0.25 || std::isnan(c.fill.Width())) c.fill.Width(fw);
    c.fill.Opacity(has ? 1 : 0);
    std::wstring el = has ? Clock(pos) : L"", lf = has ? L"−" + Clock(s.duration - pos) : L"";
    if (c.elapsed.Text() != el) c.elapsed.Text(el);
    if (c.left.Text() != lf) c.left.Text(lf);
}

// Everything in a card, for its size now and how open it is (e: 0 rest, 1 open). `a` fades what's on a
// card tucked behind the front one (0 while it only peeks). Coordinates are inside the 1px border.
void LayoutCard(Card& c, double w, double h, double e, double a, const Smtc& s) {
    w -= 2, h -= 2;
    if (c.layoutW == w && c.layoutH == h && c.layoutE == e && c.layoutA == a) {
        UpdateProgress(c, s, w);
        return;
    }
    c.innerClip.Rect({0, 0, (float)w, (float)h});
    // Header: the artwork, the title over the artist, the equalizer (rest) or ✕ (open).
    double artSize = Lerp(36, 44, e), artX = Lerp(7, 11, e), artY = Lerp(7, 11, e);
    c.artBox.Width(artSize);
    c.artBox.Height(artSize);
    c.art.Width(artSize);
    c.art.Height(artSize);
    Put(c.artBox, artX, artY);
    c.artBox.Opacity(a);
    double textX = artX + artSize + Lerp(10, 12, e);
    double textW = Lerp(w - textX - 14 - 18 - 8, w - textX - kClose - 14, e);  // (up to the equalizer, or the ✕)
    c.name.Place(textX, Lerp(8, 13, e), textW, 20, a);
    c.sub.Place(textX, Lerp(27, 33, e), textW, 18, a);
    Put(c.eq.panel, w - 14 - 18, (kRestH - 2 - 18) / 2 + Lerp(0, -8, e));
    c.eq.panel.Opacity(a * (1 - e) * (1 - e));
    Put(c.close.b, w - kClose - 10, 11);
    c.close.b.Opacity(a * e);
    c.close.b.IsHitTestVisible(e > 0.6 && a > 0.5);

    // The player, under the header: it rides in a little as the card opens.
    double lift = (1 - e) * 10, pe = a * e * e;
    UpdateProgress(c, s, w);
    Put(c.bar, 12, 62 + lift);
    c.bar.Opacity(pe);
    c.bar.IsHitTestVisible(e > 0.6 && a > 0.5);
    Put(c.elapsed, 12, 80 + lift);
    Put(c.left, w - 12 - 70, 80 + lift);
    c.elapsed.Opacity(pe);
    c.left.Opacity(pe);
    IconBtn* row[] = {&c.open, &c.prev, &c.play, &c.next, &c.mute};
    double slot = (w - 16) / 5;
    for (int i = 0; i < 5; i++) {
        double size = i == 2 ? kPlayBtn : kBtn;
        Put(row[i]->b, 8 + slot * (i + 0.5) - size / 2, 99 + (kPlayBtn - size) / 2 + lift);
        row[i]->b.Opacity(pe);
        row[i]->b.IsHitTestVisible(e > 0.6 && a > 0.5);
    }
    c.layoutW = w, c.layoutH = h, c.layoutE = e, c.layoutA = a;
}

// Into place, or away (the compositor animates the change).
void SetIn(Card& c, bool in) {
    c.root.Opacity(in ? 1.0 : 0.0);
    c.root.Translation({0, in ? 0.0f : 10.0f, 16});
}

// A card waiting off stage: built and laid out in the stack, invisible and not taking the pointer, so
// showing it later is only its fade (no building or first layout in the frames it animates in).
void Park(Host& h, Card& c) {
    c.root.IsHitTestVisible(false);
    uint32_t i;
    if (!h.stack.Children().IndexOf(c.root, i)) h.stack.Children().InsertAt(0, c.root);
    double w = h.stack.ActualWidth();
    if (w <= 0)
        if (auto g = h.paging.get()) w = g.ActualWidth() - 16;
    if (w > 0 && (c.sizedW != w || c.sizedH != kRestH)) {
        c.sizedW = w, c.sizedH = kRestH;
        c.root.Width(w);
        c.root.Height(kRestH);
        Put(c.root, 0, 0);
        LayoutCard(c, w, kRestH, 0, 1, h.smtc);
    }
}

// The stack, laid out from its bottom edge up. At rest the second card sits behind the front one, a little
// narrower, with only its top edge showing; open, all of them fan out at full size above the front card.
// A card's place (pos) glides, so the cards behind move up smoothly when the front one goes.
void LayoutStack(Host& h) {
    if (!h.active) return;
    double rest = h.height;
    if (h.stack.Height() != rest) h.stack.Height(rest);
    mux::Thickness m{8, 0, 8, h.below + (rest > 0 ? kGap : 0)};
    if (h.stack.Margin() != m) h.stack.Margin(m);
    double w = h.stack.ActualWidth();
    if (w <= 0)
        if (auto g = h.paging.get()) w = g.ActualWidth() - 16;
    if (w <= 0) return;
    double e = h.t;
    for (auto& cp : h.onStack) {
        auto& c = *cp;
        double behind = c.pos > 1 ? 1 : c.pos;
        double restTop = rest - kRestH - behind * kPeekH, openTop = rest - kOpenH - c.pos * (kOpenH + kFanGap);
        double inset = Lerp(behind * kPeekInset, 0, e), top = Lerp(restTop, openTop, e), ch = Lerp(kRestH, kOpenH, e);
        double cw = w - 2 * inset;
        Put(c.root, inset, top);
        int z = 100 - (int)std::lround(c.pos * 10);
        if (muxc::Canvas::GetZIndex(c.root) != z) muxc::Canvas::SetZIndex(c.root, z);
        if (cw != c.sizedW || ch != c.sizedH) {
            c.sizedW = cw, c.sizedH = ch;
            c.root.Width(cw);
            c.root.Height(ch);
            c.root.CenterPoint({(float)(cw / 2), (float)ch, 0});  // (it grows in from its bottom edge)
        }
        if (c.root.IsHitTestVisible() == c.leaving) c.root.IsHitTestVisible(!c.leaving);
        LayoutCard(c, cw, ch, e, Lerp(1, e, behind), h.smtc);
    }
}

double WantedHeight(const Host& h) {
    size_t n = h.shown.size();
    return n ? kRestH + (n > 1 ? kPeekH : 0) : 0;
}

void OnFrame(Host& h) {
    if (!h.active) return;
    if (!h.stack.IsLoaded() || !Shown(h.stack)) {
        auto token = h.render;
        h.render = {};
        h.lastFrame = 0;
        if (token.value) muxm::CompositionTarget::Rendering(token);
        return;
    }
    double now = NowMs();
    double dt = h.lastFrame ? now - h.lastFrame : 16;
    if (dt > 50) dt = 50;
    h.lastFrame = now;
    bool busy = false;
    if (h.closeAt >= 0) {
        if (now >= h.closeAt) SetOpen(h, false);
        else busy = true;
    }
    double p = h.startMs ? (now - h.startMs) / kOpenMs : 1;
    h.t = p >= 1 ? h.to : h.from + (h.to - h.from) * Ease(p);
    if (p < 1) busy = true;
    else h.startMs = 0;
    for (auto& c : h.shown) busy |= c->dragging;

    // The room for the stack changes in one step (no frame-by-frame layout of the tab list): it grows at
    // once, under the card coming in, and shrinks only once the cards leaving have gone.
    bool anyLeaving = false;
    for (auto& c : h.onStack) anyLeaving |= c->leaving;
    double want = WantedHeight(h);
    h.height = anyLeaving && h.height > want ? h.height : want;
    for (size_t i = 0; i < h.onStack.size(); i++) {
        auto& c = *h.onStack[i];
        // A new card goes into place a frame after it's on screen, so the compositor animates it in.
        if (!c.leaving && !c.armed) {
            // (once it's loaded and the room for it has been made: a few frames in)
            if (c.loaded && now - c.addedAt > 50) SetIn(c, true), c.armed = true;
            busy = true;
        }
        if (c.leaving) busy = true;
        if (!c.leaving) {
            auto it = std::find(h.shown.begin(), h.shown.end(), h.onStack[i]);
            busy |= Approach(c.pos, (double)(it - h.shown.begin()), dt, 80);
        }
    }
    // Gone ones come off the stack once their exit has played.
    for (auto it = h.onStack.begin(); it != h.onStack.end();) {
        auto& c = **it;
        if (c.leaving && now - c.leftAt > 320) {
            c.onStack = c.leaving = false;
            c.root.IsHitTestVisible(false);  // (parked: it stays built while its tab is tracked)
            c.eq.Run(false);
            it = h.onStack.erase(it);
        } else {
            ++it;
        }
    }
    LayoutStack(h);
    for (auto& c : h.onStack) {
        for (auto* b : {&c->play, &c->mute}) busy |= b->Frame(now);
        busy |= c->name.Frame(now);
        busy |= c->sub.Frame(now);
        busy |= FlipFrame(*c, now);
    }
    if (!busy && h.render.value) {
        mux::Media::CompositionTarget::Rendering(h.render);
        h.render = {};
        h.lastFrame = 0;
    }
}

void EnsureFrames(Host& h) {
    if (!h.active || h.render.value) return;
    std::weak_ptr<Host> weak = h.shared_from_this();
    h.lastFrame = 0;
    h.render = mux::Media::CompositionTarget::Rendering([weak](auto&&, auto&&) {
        auto host = weak.lock();
        if (!host || !host->active) return;
        try {
            OnFrame(*host);
        } catch (...) {
            // A failed layout must not leave a failing callback subscribed on every frame forever.
            auto token = host->render;
            host->render = {};
            try { if (token.value) muxm::CompositionTarget::Rendering(token); } catch (...) {}
            Log("media: frame failed; animation stopped");
        }
    });
}

void RemoveHost(Host& h) {
    h.active = false;  // before any XAML call can deliver a nested event
    auto render = h.render, sizeChanged = h.sizeChanged;
    h.render = h.sizeChanged = {};
    try { if (render.value) muxm::CompositionTarget::Rendering(render); } catch (...) {}
    try { if (sizeChanged.value && h.stack) h.stack.SizeChanged(sizeChanged); } catch (...) {}
    // Each cleanup is independent: a disconnected element must not skip the other subscriptions.
    auto retire = [](const std::shared_ptr<Card>& c) {
        c->active = false;
        c->artRequest++;
        try { if (c->artLoad) c->artLoad.Cancel(); } catch (...) {}
        c->artLoad = nullptr;
        c->dragging = false;
        try { c->eq.Run(false); } catch (...) {}
        try { c->bar.ReleasePointerCaptures(); } catch (...) {}
        try { c->root.IsHitTestVisible(false); } catch (...) {}
    };
    for (auto& c : h.cards) retire(c);
    for (auto& c : h.onStack) retire(c);
    try {
        if (auto g = h.paging.get()) {
            uint32_t i;
            if (h.stack && g.Children().IndexOf(h.stack, i)) g.Children().RemoveAt(i);
        }
    } catch (...) {
    }
    try { if (auto sw = h.switcher.get()) sw.VerticalAlignment(h.switcherAlign); } catch (...) {}
    h.switcher = nullptr;
    h.cards.clear();
    h.shown.clear();
    h.onStack.clear();
    if (t_goTo.host.lock().get() == &h) t_goTo.tries = 0, t_goTo.row = nullptr, t_goTo.host.reset();
}

// The stack shares the sidebar's bottom row with the space switcher (the dots), sitting just above it:
// that row is sized to fit, and the tab list above makes room. Dia's own rows and their order are left
// alone (no row added, nothing moved to another row); only the switcher keeps to the row's bottom.
bool EnsureHost(Host& h, const muxc::Grid& paging) {
    if (!h.stack) {
        h.paging = winrt::make_weak(paging);
        h.stack = muxc::Canvas();
        h.stack.Name(L"diatweaksMedia");
        h.stack.VerticalAlignment(mux::VerticalAlignment::Bottom);
        h.stack.Margin({8, 0, 8, 0});
        h.stack.Height(0);
        std::weak_ptr<Host> weak = h.shared_from_this();
        h.sizeChanged = h.stack.SizeChanged([weak](auto&&, auto&&) {
            if (auto host = weak.lock(); host && host->active) {
                try { LayoutStack(*host); } catch (...) { Log("media: size layout failed"); }
            }
        });
        paging.Children().Append(h.stack);  // (last: drawn over the tab list while open)
        Log("media: card area added");
    }
    mux::FrameworkElement sw{nullptr};
    for (auto const& c : paging.Children())
        if (auto fe = c.try_as<mux::FrameworkElement>(); fe && fe.Name() == L"SpaceSwitcherHost") sw = fe;
    if (sw && sw != h.switcher.get()) {
        if (auto old = h.switcher.get()) old.VerticalAlignment(h.switcherAlign);
        h.switcher = winrt::make_weak(sw);
        h.switcherAlign = sw.VerticalAlignment();
        sw.VerticalAlignment(mux::VerticalAlignment::Bottom);
    }
    int rows = (int)paging.RowDefinitions().Size();
    int row = sw ? muxc::Grid::GetRow(sw) : rows - 1;
    if (row < 0) row = 0;
    if (muxc::Grid::GetRow(h.stack) != row) muxc::Grid::SetRow(h.stack, row);
    double below = sw && sw.Visibility() == mux::Visibility::Visible ? sw.ActualHeight() : 0;
    if (below != h.below) h.below = below, LayoutStack(h);
    return true;
}

bool Matches(const std::wstring& tab, const std::wstring& media) {
    auto a = Plain(tab), b = Lower(media);
    if (a.size() < 3 || b.size() < 3) return false;
    return a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos;
}

void UpdateHost(Host& h, const muxc::Grid& paging, const Smtc& now_, bool pip, Color profile) {
    if (!EnsureHost(h, paging)) return;
    // Between two songs the session goes blank for a moment (no title, closed; Spotify shows its logo
    // meanwhile), or goes away. For up to 3 s that's ridden over: the card goes on with the last song as it
    // was, and nothing on it changes until the next song's details come.
    ULONGLONG tick = GetTickCount64();
    bool gap = false;
    if (now_.has && !now_.title.empty()) h.lastGood = now_, h.lastGoodAt = tick;
    else if (h.lastGoodAt && tick - h.lastGoodAt < 3000) gap = true;
    const Smtc& s = gap ? h.lastGood : now_;
    bool dragging = false;
    for (auto& c : h.shown) dragging |= c->dragging;
    if (!dragging && !gap) {
        h.smtc = s;
        // After a seek, the session can still report the old position for a moment: keep the new one
        // until it reports about the same (or 3 s have gone by).
        if (h.seekTo >= 0) {
            ULONGLONG t = GetTickCount64();
            double expect = h.seekTo + (s.playing ? (t - h.seekAt) / 1000.0 * s.rate : 0);
            if (t - h.seekAt < 3000 && std::fabs(s.PositionNow() - expect) > 1.5)
                h.smtc.position = h.seekTo, h.smtc.at = h.seekAt;
            else
                h.seekTo = -1;
        }
    }
    auto rows = RowsIn(paging, h.stack, h.rows);
    ULONGLONG now = GetTickCount64();
    if (h.tapThroughUntil && now >= h.tapThroughUntil) h.tapThroughUntil = 0, h.stack.IsHitTestVisible(true);
    Theme theme = ThemeOf(paging, profile);
    bool retheme = !h.themed || theme.dark != h.dark || profile != h.profile;
    if (profile != h.profile || !h.themed)
        Log("media: profile color #%02x%02x%02x%02x (equalizer #%02x%02x%02x)", profile.A, profile.R, profile.G, profile.B,
            theme.profile.R, theme.profile.G, theme.profile.B);
    h.dark = theme.dark, h.themed = true, h.profile = profile;

    // Which row the system media session belongs to: by title; else, while it's the same session, the
    // row it belonged to (between two songs the titles don't match for a moment and the tab is briefly
    // silent: the card stays the player meanwhile); else the only tab with sound.
    const Row* smtcRow = nullptr;
    if (s.has) {
        for (auto& r : rows)
            if (Matches(r.title, s.title)) smtcRow = &r;
        if (!smtcRow && s.session == h.smtcSession)
            for (auto& r : rows)
                if (r.grid == h.smtcRow.get()) smtcRow = &r;
        if (!smtcRow) {
            int n = 0;
            for (auto& r : rows)
                if (r.audible) n++, smtcRow = &r;
            if (n != 1) smtcRow = nullptr;
        }
        if (smtcRow) h.smtcRow = winrt::make_weak(smtcRow->grid), h.smtcSession = s.session;
    }

    std::vector<std::shared_ptr<Card>> keep, shown;
    bool frames = false;
    for (auto& r : rows) {
        bool isSmtc = &r == smtcRow;
        std::shared_ptr<Card> card;
        for (auto& c : h.cards)
            if (c->row.get() == r.grid) card = c;
        bool sound = r.audible || (isSmtc && s.playing);
        bool fresh = false;
        if (!card) {
            if (!sound) continue;
            card = MakeCard(h);
            card->row = winrt::make_weak(r.grid);
            ApplyTheme(*card, theme);
            Park(h, *card);  // (built now, while its tab plays in front; shown when you switch away)
            fresh = true;
        } else if (retheme) {
            ApplyTheme(*card, theme);
        }
        if (sound) card->lastSound = now;
        // A card lasts while the tab has sound, while the system session is its (paused too), and a few
        // seconds more (Dia hides the speaker a moment after a pause).
        if (!sound && !isSmtc && now - card->lastSound > 4000) continue;
        if (r.title != card->title) {
            if (!card->title.empty() && Plain(r.title) != Plain(card->title)) card->dismissed = false;  // something new
            card->title = r.title;
        }
        if (r.favicon) card->favicon = r.favicon;
        card->smtc = isSmtc;
        card->muted = r.audible && MutedGlyph(r.glyph);
        card->playing = isSmtc ? s.playing : r.audible && !card->muted;
        // Right after a click, the session may still report the old state for a moment: the button keeps
        // what was clicked until the session agrees (or 2.5 s have gone by), so it doesn't flip back and forth.
        if (card->playWant >= 0) {
            if (now < card->playWantUntil && card->playing != (card->playWant == 1)) card->playing = card->playWant == 1;
            else card->playWant = -1;
        }
        keep.push_back(card);

        // Being on the tab ends a ✕ too: once you leave it again, the card comes back.
        if (r.active) card->hideUntil = 0, card->dismissed = false;
        bool show = !r.active && !card->dismissed && now >= card->hideUntil &&
                    !(pip && isSmtc && Engine_TweakOn(L"mediaControlsPiP"));
        card->eq.Run((show || card->onStack) && card->playing && !card->muted);
        if (!show) {
            // Parked: kept up to date (title, artist, artwork) so it shows ready.
            if (!card->onStack) {
                card->name.Text(isSmtc && !s.title.empty() ? s.title : r.title, false);
                card->sub.Text(isSmtc ? s.artist : L"", false);
                card->play.Set(card->playing ? kPause : kPlay, false);
                card->mute.Set(card->muted ? kMute : kVolume, false);
                SetArt(*card, s, false);
                if (isSmtc && !s.title.empty() && !s.artist.empty()) card->hadSong = true;
            }
            continue;
        }
        shown.push_back(card);
        // Changes on a card already showing animate: the text rolls, the symbols morph, the art turns.
        bool animate = card->shown && card->onStack && !card->leaving && !fresh;
        // Between two songs the page drops its song details for a moment, and Chromium fills in its own:
        // the tab's title, no artist. That stand-in isn't shown: the last song stays until the next one's
        // details come (a site that never gives any shows the stand-in after 3 s).
        bool hold = gap && isSmtc;
        if (isSmtc && !hold) {
            bool standIn = s.title.empty() || (s.artist.empty() && Plain(s.title) == Plain(r.title));
            if (standIn && card->hadSong) {
                if (!card->standInSince) card->standInSince = now, Log("media: holding the last song (stand-in details)");
                hold = now - card->standInSince < 3000;
            } else {
                card->standInSince = 0;
            }
            if (!standIn) card->hadSong = true;
        } else if (!isSmtc) {
            card->hadSong = false, card->standInSince = 0;
        }
        if (!hold) {
            frames |= card->name.Text(isSmtc && !s.title.empty() ? s.title : isSmtc ? std::wstring(card->name.tb.Text()) : r.title, animate);
            frames |= card->sub.Text(isSmtc ? s.artist : L"", animate);
        }
        frames |= card->play.Set(card->playing ? kPause : kPlay, animate);
        frames |= card->mute.Set(card->muted ? kMute : kVolume, animate);
        card->holding = hold;
        if (!hold) {
            card->play.b.IsEnabled(isSmtc && s.canToggle);
            card->next.b.IsEnabled(isSmtc && s.canNext);
            card->prev.b.IsEnabled(isSmtc && s.canPrev);
        }
        card->mute.b.IsEnabled(r.speaker && r.audible);
        if (!hold) SetArt(*card, s, animate);  // (between songs the artwork stays too)
    }
    for (auto& c : h.cards) c->shown = false;
    for (auto& c : shown) c->shown = true;
    for (auto& c : h.cards)
        if (std::find(keep.begin(), keep.end(), c) == keep.end() && !c->onStack) c->eq.Run(false);

    // Newest sound in front. New cards come onto the stack at their place; ones no longer wanted leave.
    std::stable_sort(shown.begin(), shown.end(), [](auto& a, auto& b) { return a->lastSound > b->lastSound; });
    for (size_t i = 0; i < shown.size(); i++) {
        auto& c = shown[i];
        if (c->leaving) c->leaving = false, SetIn(*c, true), frames = true;
        if (!c->onStack) {
            c->onStack = true;
            c->armed = false;
            c->addedAt = NowMs();
            c->pos = (double)i;
            h.onStack.push_back(c);
            Park(h, *c);  // (normally there already)
            frames = true;
        }
    }
    for (auto& c : h.onStack)
        if (!c->leaving && std::find(shown.begin(), shown.end(), c) == shown.end())
            c->leaving = true, c->leftAt = NowMs(), SetIn(*c, false), frames = true;
    // Cards no longer tracked go from the stack (parked ones at once; leaving ones once they've gone).
    for (auto& c : h.cards)
        if (!c->onStack && std::find(keep.begin(), keep.end(), c) == keep.end()) {
            c->active = false;
            ++c->artRequest;
            try { if (c->artLoad) c->artLoad.Cancel(); } catch (...) {}
            c->artLoad = nullptr;
            uint32_t i;
            if (h.stack.Children().IndexOf(c->root, i)) h.stack.Children().RemoveAt(i);
            c->eq.Run(false);
        }
    h.cards = std::move(keep);
    for (auto& c : h.onStack)
        if (std::find(h.cards.begin(), h.cards.end(), c) == h.cards.end()) h.cards.push_back(c);
    h.shown = std::move(shown);
    if (h.shown.empty() && h.open) SetOpen(h, false);
    if (h.height != WantedHeight(h)) frames = true;
    if (frames) EnsureFrames(h);
    LayoutStack(h);
}

}  // namespace

void Media_Tick() {
    static thread_local int tick = 0;
    tick++;
    if (t_goTo.tries > 0) {
        auto row = RowFrom(t_goTo.row);
        auto host = t_goTo.host.lock();
        if (!host || !host->active || !row || TapRow(*host, row) || --t_goTo.tries == 0)
            t_goTo.tries = 0, t_goTo.row = nullptr, t_goTo.host.reset();
    }
    bool on = Engine_TweakOn(L"mediaControls");
    RunWorker(on);
    if (!on) {
        for (auto& [k, h] : t_hosts) RemoveHost(*h);
        t_hosts.clear();
        return;
    }
    if (tick % 3) return;
    auto tickStart = GetTickCount64();
    try {
        Smtc s = Snapshot();
        bool pip = Engine_TweakOn(L"mediaControlsPiP") && PipOpen();
        // The profile's color: Dia tints the floating sidebar with it (SidebarFloatingTint, a gradient), one per window.
        // That border sits collapsed (and without a XamlRoot) while the sidebar is docked, so it's matched
        // to its window by the top of the visual tree they share.
        auto topOf = [](mux::DependencyObject d) {
            for (int i = 0; d && i < 200; i++) {
                auto p = muxm::VisualTreeHelper::GetParent(d);
                if (!p) break;
                d = p;
            }
            return d;
        };
        static thread_local std::vector<winrt::weak_ref<muxc::Border>> tintRefs;
        static thread_local std::vector<winrt::weak_ref<muxc::Grid>> pagingRefs;
        static thread_local unsigned long long revision = ~0ull;
        static thread_local ULONGLONG scanned = 0;
        auto currentRevision = Engine_TreeRevision();
        if (revision != currentRevision || tickStart - scanned >= 2000) {
            tintRefs.clear();
            pagingRefs.clear();
            Engine_ForEachNode([&](const NodeRef& n) {
                if (n.name == L"SidebarFloatingTint") {
                    if (auto border = Obj<muxc::Border>(n.handle)) tintRefs.push_back(winrt::make_weak(border));
                } else if (n.name == L"SpacePagingRoot") {
                    if (auto grid = Obj<muxc::Grid>(n.handle)) pagingRefs.push_back(winrt::make_weak(grid));
                }
            });
            revision = currentRevision;
            scanned = tickStart;
        }
        std::vector<std::pair<mux::DependencyObject, Color>> tints;
        std::vector<muxc::Grid> pagings;
        for (auto& ref : tintRefs) {
            if (auto border = ref.get()) {
                // A gradient (or a solid color): its most colorful stop.
                auto bg = border.Background();
                std::vector<Color> cs;
                if (auto sb = bg.try_as<muxm::SolidColorBrush>()) cs.push_back(sb.Color());
                if (auto gb = bg.try_as<muxm::GradientBrush>())
                    for (auto const& st : gb.GradientStops()) cs.push_back(st.Color());
                Color best{};
                int bestSat = -1;
                for (auto c : cs) {
                    int sat = c.A < 0x40 ? -1 : (std::max)({c.R, c.G, c.B}) - (std::min)({c.R, c.G, c.B});
                    if (sat > bestSat) bestSat = sat, best = c;
                }
                if (bestSat >= 0) tints.push_back({topOf(border), best});
            }
        }
        for (auto& ref : pagingRefs)
            if (auto grid = ref.get(); grid && grid.IsLoaded()) pagings.push_back(grid);
        std::set<void*> live;
        for (auto& g : pagings) {
            void* key = winrt::get_abi(g);
            auto& entry = t_hosts[key];
            // XAML can reuse an address after a window/space is destroyed. A new root needs a new host,
            // even if its interface pointer happens to match the old map key.
            if (entry && entry->paging.get() != g) {
                RemoveHost(*entry);
                entry.reset();
            }
            if (!entry) entry = std::make_shared<Host>();
            auto host = entry;
            Host& h = *host;
            Color profile{};
            auto top = topOf(g);
            for (auto& [root, c] : tints)
                if (root == top) profile = c;
            if (!profile.A) profile = h.profile;  // (the last one seen)
            if (!profile.A && !tints.empty()) profile = tints[0].second;
            live.insert(key);
            UpdateHost(h, g, s, pip, profile);
        }
        for (auto it = t_hosts.begin(); it != t_hosts.end();)
            if (!live.count(it->first)) RemoveHost(*it->second), it = t_hosts.erase(it);
            else ++it;
    } catch (winrt::hresult_error const& e) {
        Log("media: tick 0x%08lx", (unsigned long)e.code());
    } catch (...) {
        Log("media: tick error");
    }
    auto elapsed = GetTickCount64() - tickStart;
    static thread_local ULONGLONG lastSlowLog = 0;
    if (elapsed >= 50 && tickStart - lastSlowLog >= 10000) {
        lastSlowLog = tickStart;
        Log("media: slow UI update %llu ms", elapsed);
    }
}


// `diatweaks probe COLORS`: the colorful brushes on screen and where, and colorful app resources (to find
// where Dia keeps a profile's color).
std::string Media_DebugColors() {
    std::string out;
    auto hex = [](winrt::Windows::UI::Color c) {
        char b[16];
        snprintf(b, sizeof b, "#%02x%02x%02x%02x", c.A, c.R, c.G, c.B);
        return std::string(b);
    };
    auto colorful = [](winrt::Windows::UI::Color c) {
        int mx = (std::max)({c.R, c.G, c.B}), mn = (std::min)({c.R, c.G, c.B});
        return c.A > 0x30 && mx - mn > 50;
    };
    auto narrow = [](const std::wstring& w) {
        std::string s;
        for (wchar_t ch : w) s += ch < 128 ? (char)ch : '?';
        return s;
    };
    std::map<std::string, int> seen;
    Engine_ForEachNode([&](const NodeRef& n) {
        auto o = Obj<mux::DependencyObject>(n.handle);
        if (!o) return;
        auto note = [&](const char* prop, const muxm::Brush& b) {
            std::vector<winrt::Windows::UI::Color> cs;
            if (auto s = b.try_as<muxm::SolidColorBrush>()) cs.push_back(s.Color());
            if (auto g = b.try_as<muxm::GradientBrush>())
                for (auto const& st : g.GradientStops()) cs.push_back(st.Color());
            for (auto c : cs) {
                if (!colorful(c)) continue;
                std::string path;
                int d = 0;
                for (auto p = muxm::VisualTreeHelper::GetParent(o); p && d < 6; p = muxm::VisualTreeHelper::GetParent(p), d++)
                    if (auto fe = p.try_as<mux::FrameworkElement>(); fe && !fe.Name().empty()) path += " < " + narrow(std::wstring(fe.Name()));
                std::string line = hex(c) + " " + prop + " " + narrow(n.type) + " \"" + narrow(n.name) + "\"" + path;
                seen[line]++;
            }
        };
        try {
            if (auto x = o.try_as<muxc::Panel>(); x && x.Background()) note("bg", x.Background());
            if (auto x = o.try_as<muxc::Border>(); x && x.Background()) note("bg", x.Background());
            if (auto x = o.try_as<muxc::Border>(); x && x.BorderBrush()) note("border", x.BorderBrush());
            if (auto x = o.try_as<muxc::Control>(); x && x.Background()) note("bg", x.Background());
            if (auto x = o.try_as<muxc::Control>(); x && x.Foreground()) note("fg", x.Foreground());
            if (auto x = o.try_as<muxc::TextBlock>(); x && x.Foreground()) note("fg", x.Foreground());
            if (auto x = o.try_as<muxc::IconElement>(); x && x.Foreground()) note("fg", x.Foreground());
            if (auto x = o.try_as<muxsh::Shape>(); x && x.Fill()) note("fill", x.Fill());
            if (auto x = o.try_as<muxsh::Shape>(); x && x.Stroke()) note("stroke", x.Stroke());
        } catch (...) {
        }
    });
    for (auto& [l, k] : seen) out += std::to_string(k) + "x " + l + "\n";
    try {
        std::function<void(const mux::ResourceDictionary&, int)> scan = [&](const mux::ResourceDictionary& r, int depth) {
            for (auto const& kv : r) {
                auto key = kv.Key().try_as<winrt::hstring>();
                winrt::Windows::UI::Color c{};
                bool has = false;
                if (auto b = kv.Value().try_as<muxm::SolidColorBrush>()) c = b.Color(), has = true;
                else if (auto v = kv.Value().try_as<winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color>>()) c = v.Value(), has = true;
                if (has && key && colorful(c)) out += "res " + hex(c) + " " + narrow(std::wstring(*key)) + "\n";
            }
            if (depth < 3) {
                for (auto const& m : r.MergedDictionaries()) scan(m, depth + 1);
                for (auto const& kv : r.ThemeDictionaries())
                    if (auto d = kv.Value().try_as<mux::ResourceDictionary>()) scan(d, depth + 1);
            }
        };
        scan(mux::Application::Current().Resources(), 0);
    } catch (...) {
    }
    return out;
}
