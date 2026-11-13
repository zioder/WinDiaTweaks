#pragma once
// llhook.h - low-level input hooks (WH_MOUSE_LL, WH_KEYBOARD_LL) on a thread of their own.
//
// Windows calls a low-level hook on the thread that set it, and holds every mouse move and key press on
// the whole system until that thread answers (up to LowLevelHooksTimeout). Set on Dia's UI thread, which
// is often busy (WinUI layout, a page being made, our own ticks), they make the pointer and typing stall
// everywhere (zioder/WinDiaTweaks#39). This thread does nothing but wait for hook calls, so it answers at
// once.
//
// Hook procs run on this thread: they must not touch Dia's UI (XAML) or state only the UI thread may use,
// nor take a lock the UI thread can hold for long. Read atomics, post messages to the UI thread.
//
// When Dia crashes, Swift's crash reporter (swift-backtrace) stops the whole process while it prints its
// report and waits for a key, this thread included, and the hooks would stall all input on the PC for as
// long (zioder/WinDiaTweaks#58). ReleaseAll() takes every hook off; the crash guard in tap.cpp calls it
// before that happens. It takes no lock and works from any thread, the crashed one included.

#include <windows.h>

#include <atomic>

class LowLevelHookThread {
public:
    LowLevelHookThread() = default;
    LowLevelHookThread(const LowLevelHookThread&) = delete;
    LowLevelHookThread& operator=(const LowLevelHookThread&) = delete;

    // Sets the hooks (mouse or keys may be null) on a new thread. True once they're set.
    bool Start(HOOKPROC mouse, HOOKPROC keys) {
        if (thread_) return true;
        mouse_ = mouse;
        keys_ = keys;
        ok_ = false;
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready) return false;
        ready_ = ready;
        thread_ = CreateThread(nullptr, 0, &LowLevelHookThread::Main, this, 0, &id_);
        if (!thread_) {
            CloseHandle(ready);
            ready_ = nullptr;
            return false;
        }
        WaitForSingleObject(ready, INFINITE);  // (setting a hook doesn't block)
        CloseHandle(ready);
        ready_ = nullptr;
        if (!ok_) Stop();
        return ok_;
    }

    // Takes the hooks off and ends the thread.
    void Stop() {
        if (!thread_) return;
        PostThreadMessageW(id_, WM_QUIT, 0, 0);
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
        id_ = 0;
    }

    bool Running() const { return thread_ != nullptr; }
    DWORD Error() const { return error_; }

    // Takes off the hooks of every hook thread in this process, at once, and tells the threads to end.
    // For a crash: no lock, no waiting, any thread. (A hook may be taken off by any thread of the
    // process that set it.) Returns how many hooks it took off.
    static int ReleaseAll() {
        int n = 0;
        for (auto& slot : Registry()) {
            LowLevelHookThread* t = slot.load();
            if (!t) continue;
            std::atomic<HHOOK>* hooks[] = {&t->mouseHook_, &t->keysHook_};
            for (auto* h : hooks)
                if (HHOOK hook = h->exchange(nullptr)) n += UnhookWindowsHookEx(hook) ? 1 : 0;
            if (DWORD id = t->id_) PostThreadMessageW(id, WM_QUIT, 0, 0);
        }
        return n;
    }

private:
    static constexpr int kMaxThreads = 8;
    static std::atomic<LowLevelHookThread*> (&Registry())[kMaxThreads] {
        static std::atomic<LowLevelHookThread*> slots[kMaxThreads];
        return slots;
    }
    void Register() {
        for (auto& slot : Registry()) {
            LowLevelHookThread* none = nullptr;
            if (slot.compare_exchange_strong(none, this)) return;
        }
    }
    void Unregister() {
        for (auto& slot : Registry()) {
            LowLevelHookThread* me = this;
            slot.compare_exchange_strong(me, nullptr);
        }
    }

    static DWORD WINAPI Main(LPVOID arg) {
        auto* self = static_cast<LowLevelHookThread*>(arg);
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // (the queue, before Stop can post to it)
        // Its own module (the procs live in it), held while the hooks are set.
        HMODULE self_mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&LowLevelHookThread::Main, &self_mod);
        HHOOK mouse = self->mouse_ ? SetWindowsHookExW(WH_MOUSE_LL, self->mouse_, self_mod, 0) : nullptr;
        HHOOK keys = self->keys_ ? SetWindowsHookExW(WH_KEYBOARD_LL, self->keys_, self_mod, 0) : nullptr;
        self->ok_ = (!self->mouse_ || mouse) && (!self->keys_ || keys);
        self->error_ = self->ok_ ? 0 : GetLastError();
        self->mouseHook_ = mouse;
        self->keysHook_ = keys;
        if (self->ok_) self->Register();
        SetEvent(self->ready_);
        if (self->ok_)
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        self->Unregister();
        // (Null if ReleaseAll already took them off.)
        if (HHOOK h = self->mouseHook_.exchange(nullptr)) UnhookWindowsHookEx(h);
        if (HHOOK h = self->keysHook_.exchange(nullptr)) UnhookWindowsHookEx(h);
        return 0;
    }

    HOOKPROC mouse_ = nullptr, keys_ = nullptr;
    std::atomic<HHOOK> mouseHook_{nullptr}, keysHook_{nullptr};
    HANDLE thread_ = nullptr, ready_ = nullptr;
    DWORD id_ = 0, error_ = 0;
    volatile bool ok_ = false;
};
