#include "lua.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

#if defined(_WIN32) || defined(WIN32)
    #include <windows.h>
    #define EXPORT_SYMBOL __declspec(dllexport)
#elif defined(__linux__)
    #include <gio/gio.h>
    #define EXPORT_SYMBOL __attribute__((visibility("default")))
#else
    #define EXPORT_SYMBOL
#endif

// ─── GNOME D-Bus idle detection ────────────────────────────────────────────
#ifdef __linux__
uint64_t getGnomeIdleTimeMs()
{
    GError* error = nullptr;
    GDBusProxy* proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SESSION,
        G_DBUS_PROXY_FLAGS_NONE,
        nullptr,
        "org.gnome.Mutter.IdleMonitor",
        "/org/gnome/Mutter/IdleMonitor/Core",
        "org.gnome.Mutter.IdleMonitor",
        nullptr, &error);

    if (!proxy)
    {
        if (error)
            g_error_free(error);
        return 0;
    }

    GVariant* result = g_dbus_proxy_call_sync(
        proxy, "GetIdletime", nullptr,
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);

    uint64_t idleMs = 0;
    if (result)
    {
        g_variant_get(result, "(t)", &idleMs);
        g_variant_unref(result);
    }
    if (error)
    {
        g_error_free(error);
    }

    g_object_unref(proxy);
    return idleMs;
}
#endif // __linux__

// ─── Wayland ext-idle-notify-v1 idle detection ─────────────────────────────
#if defined(__linux__) && defined(HAVE_WAYLAND_IDLE)
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ext-idle-notify-v1-client-protocol.h"

namespace {

// Minimum notification timeout passed to get_idle_notification (ms).
// The compositor fires "idled" only after this many ms of inactivity.
constexpr uint32_t WAYLAND_IDLE_NOTIFY_TIMEOUT_MS = 1000;

struct WaylandIdleTracker
{
    wl_display*               display      = nullptr;
    wl_registry*              registry     = nullptr;
    ext_idle_notifier_v1*     notifier     = nullptr;
    wl_seat*                  seat         = nullptr;
    ext_idle_notification_v1* notification = nullptr;

    // Written by Wayland thread, read by calling thread.
    std::atomic<bool>     isIdle{false};
    // Milliseconds since epoch when "idled" fired, 0 when not idle.
    std::atomic<uint64_t> idleStartMs{0};

    std::thread workerThread;
    bool        initialized = false;

    // Pipe used to wake the worker thread for clean shutdown.
    // [0] = read end (polled by worker), [1] = write end (written by shutdown()).
    int wakeupPipe[2] = { -1, -1 };

    // ── registry listener ──────────────────────────────────────────────────
    static void onGlobal(void* data, wl_registry* reg,
                         uint32_t name, const char* interface, uint32_t /*version*/)
    {
        auto* self = static_cast<WaylandIdleTracker*>(data);
        if (std::string_view(interface) == ext_idle_notifier_v1_interface.name)
        {
            self->notifier = static_cast<ext_idle_notifier_v1*>(
                wl_registry_bind(reg, name, &ext_idle_notifier_v1_interface, 1));
        }
        else if (std::string_view(interface) == wl_seat_interface.name)
        {
            if (!self->seat)
            {
                self->seat = static_cast<wl_seat*>(
                    wl_registry_bind(reg, name, &wl_seat_interface, 1));
            }
        }
    }

    static void onGlobalRemove(void*, wl_registry*, uint32_t) {}
    static const wl_registry_listener registryListener;

    // ── idle notification listeners ────────────────────────────────────────
    static void onIdled(void* data, ext_idle_notification_v1*)
    {
        auto* self = static_cast<WaylandIdleTracker*>(data);
        using namespace std::chrono;
        uint64_t nowMs = static_cast<uint64_t>(
            duration_cast<milliseconds>(
                steady_clock::now().time_since_epoch()).count());
        self->idleStartMs.store(nowMs, std::memory_order_relaxed);
        self->isIdle.store(true, std::memory_order_release);
    }

    static void onResumed(void* data, ext_idle_notification_v1*)
    {
        auto* self = static_cast<WaylandIdleTracker*>(data);
        self->isIdle.store(false, std::memory_order_release);
        self->idleStartMs.store(0, std::memory_order_relaxed);
    }

    static const ext_idle_notification_v1_listener idleListener;

    // ── worker loop ────────────────────────────────────────────────────────
    bool init()
    {
        display = wl_display_connect(nullptr);
        if (!display)
            return false;

        registry = wl_display_get_registry(display);
        if (!registry)
        {
            wl_display_disconnect(display);
            display = nullptr;
            return false;
        }

        wl_registry_add_listener(registry, &registryListener, this);
        wl_display_roundtrip(display);
        wl_display_roundtrip(display);

        if (!notifier || !seat)
        {
            cleanup();
            return false;
        }

        notification = ext_idle_notifier_v1_get_idle_notification(
            notifier, WAYLAND_IDLE_NOTIFY_TIMEOUT_MS, seat);
        if (!notification)
        {
            cleanup();
            return false;
        }

        ext_idle_notification_v1_add_listener(notification, &idleListener, this);
        wl_display_roundtrip(display);

        if (pipe(wakeupPipe) != 0)
        {
            cleanup();
            return false;
        }

        initialized = true;

        // Dispatch loop using prepare_read + poll so the thread can be
        // interrupted cleanly via the wakeup pipe without data races.
        workerThread = std::thread([this]()
        {
            const int wlFd     = wl_display_get_fd(display);
            const int wakeFd   = wakeupPipe[0];

            while (true)
            {
                // Flush any pending outgoing requests.
                if (wl_display_flush(display) < 0)
                    break;

                // Prepare to read — serialise with any concurrent readers.
                while (wl_display_prepare_read(display) != 0)
                    wl_display_dispatch_pending(display);

                struct pollfd fds[2];
                fds[0] = { wlFd,   POLLIN, 0 };
                fds[1] = { wakeFd, POLLIN, 0 };

                const int ret = poll(fds, 2, -1);

                if (ret < 0 || (fds[1].revents & POLLIN))
                {
                    // Woken by shutdown() or poll error — cancel the read lock and exit.
                    wl_display_cancel_read(display);
                    break;
                }

                if (fds[0].revents & POLLIN)
                    wl_display_read_events(display);
                else
                    wl_display_cancel_read(display);

                wl_display_dispatch_pending(display);
            }
        });

        return true;
    }

    // ── clean shutdown — safe to call from any thread ──────────────────────
    void shutdown()
    {
        if (!initialized)
            return;

        // Signal the worker thread via the wakeup pipe, then join it.
        if (wakeupPipe[1] != -1)
        {
            const char sig = 1;
            (void)write(wakeupPipe[1], &sig, 1);
        }

        if (workerThread.joinable())
            workerThread.join();

        // Close the pipe ends.
        if (wakeupPipe[0] != -1) { close(wakeupPipe[0]); wakeupPipe[0] = -1; }
        if (wakeupPipe[1] != -1) { close(wakeupPipe[1]); wakeupPipe[1] = -1; }

        // Destroy Wayland objects (single-threaded now — worker has exited).
        if (notification) { ext_idle_notification_v1_destroy(notification); notification = nullptr; }
        if (notifier)     { ext_idle_notifier_v1_destroy(notifier);          notifier     = nullptr; }
        if (seat)         { wl_seat_release(seat);                           seat         = nullptr; }
        if (registry)     { wl_registry_destroy(registry);                  registry     = nullptr; }
        if (display)      { wl_display_disconnect(display);                  display      = nullptr; }

        initialized = false;
    }

    void cleanup()
    {
        if (notification) { ext_idle_notification_v1_destroy(notification); notification = nullptr; }
        if (notifier)     { ext_idle_notifier_v1_destroy(notifier);          notifier     = nullptr; }
        if (seat)         { wl_seat_release(seat);                           seat         = nullptr; }
        if (registry)     { wl_registry_destroy(registry);                  registry     = nullptr; }
        if (display)      { wl_display_disconnect(display);                  display      = nullptr; }
    }

    uint64_t getIdleMs() const
    {
        if (!initialized || !isIdle.load(std::memory_order_acquire))
            return 0;

        using namespace std::chrono;
        uint64_t nowMs = static_cast<uint64_t>(
            duration_cast<milliseconds>(
                steady_clock::now().time_since_epoch()).count());
        uint64_t start = idleStartMs.load(std::memory_order_relaxed);
        return (nowMs > start) ? (nowMs - start) : 0;
    }
};

const wl_registry_listener WaylandIdleTracker::registryListener = {
    WaylandIdleTracker::onGlobal,
    WaylandIdleTracker::onGlobalRemove,
};

const ext_idle_notification_v1_listener WaylandIdleTracker::idleListener = {
    WaylandIdleTracker::onIdled,
    WaylandIdleTracker::onResumed,
};

// Lazy-initialized singleton — created once on first call.
WaylandIdleTracker& getTracker()
{
    static WaylandIdleTracker tracker;
    return tracker;
}

std::once_flag g_trackerInitFlag;

uint64_t getWaylandIdleTimeMs()
{
    std::call_once(g_trackerInitFlag, []()
    {
        getTracker().init();
    });
    return getTracker().getIdleMs();
}

void shutdownWaylandTracker()
{
    getTracker().shutdown();
}

} // anonymous namespace
#endif // __linux__ && HAVE_WAYLAND_IDLE

// ─── Public idle query ──────────────────────────────────────────────────────
uint64_t getIdleTimeMs()
{
#if defined(_WIN32) || defined(WIN32)
    LASTINPUTINFO lii{};
    lii.cbSize = sizeof(LASTINPUTINFO);

    if (!GetLastInputInfo(&lii))
        return 0;

    return GetTickCount64() - lii.dwTime;

#elif defined(__linux__)
    // GNOME: use the Mutter D-Bus IdleMonitor (most accurate for GNOME).
    const char* xdgDesktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (xdgDesktop != nullptr)
    {
        std::string desktopStr(xdgDesktop);
        if (desktopStr.find("GNOME") != std::string::npos ||
            desktopStr.find("gnome") != std::string::npos)
        {
            return getGnomeIdleTimeMs();
        }
    }

    // Non-GNOME Wayland (KDE/Plasma, Hyprland, Sway, niri, …)
    // Requires ext-idle-notify-v1 protocol support in the compositor.
#if defined(HAVE_WAYLAND_IDLE)
    if (std::getenv("WAYLAND_DISPLAY") != nullptr)
        return getWaylandIdleTimeMs();
#endif

    return 0;
#else
    return 0;
#endif
}

// ─── Lua bindings ───────────────────────────────────────────────────────────

// The native function called from Lua
int l_getIdleStatus(lua_State* L)
{
    uint64_t threshold = static_cast<uint64_t>(luaL_checkinteger(L, 1));

    int status = (getIdleTimeMs() >= threshold) ? 1 : 0;

    lua_pushinteger(L, status);
    return 1;
}

// Called from Lua's onExit() to cleanly stop any background threads
// before the .so is unloaded — prevents std::terminate on the static dtor.
int l_destroy(lua_State* /*L*/)
{
#if defined(__linux__) && defined(HAVE_WAYLAND_IDLE)
    shutdownWaylandTracker();
#endif
    return 0;
}

// Entry point called by Lua's package loader
extern "C" EXPORT_SYMBOL
int initialiseFunctions(lua_State* L)
{
    lua_register(L, "getIdleStatus", l_getIdleStatus);
    lua_register(L, "destroy",       l_destroy);
    return 0;
}