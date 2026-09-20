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

// ─── GNOME / Cinnamon D-Bus idle detection ──────────────────────────────────
#ifdef __linux__
static bool queryDbusIdleTime(const char* serviceName,
                               const char* objectPath,
                               const char* interfaceName,
                               uint64_t& outIdleMs)
{
    GError* error = nullptr;
    GDBusProxy* proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SESSION,
        G_DBUS_PROXY_FLAGS_NONE,
        nullptr,
        serviceName,
        objectPath,
        interfaceName,
        nullptr, &error);

    if (!proxy)
    {
        if (error)
            g_error_free(error);
        return false;
    }

    GVariant* result = g_dbus_proxy_call_sync(
        proxy, "GetIdletime", nullptr,
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);

    bool success = false;
    if (result && !error)
    {
        g_variant_get(result, "(t)", &outIdleMs);
        g_variant_unref(result);
        success = true;
    }

    if (error)
    {
        g_error_free(error);
    }

    g_object_unref(proxy);
    return success;
}

uint64_t getDbusIdleTimeMs()
{
    uint64_t idleMs = 0;
    // 1. Try GNOME Mutter IdleMonitor
    if (queryDbusIdleTime("org.gnome.Mutter.IdleMonitor",
                          "/org/gnome/Mutter/IdleMonitor/Core",
                          "org.gnome.Mutter.IdleMonitor", idleMs))
    {
        return idleMs;
    }

    // 2. Try Cinnamon Muffin IdleMonitor
    if (queryDbusIdleTime("org.cinnamon.Muffin.IdleMonitor",
                          "/org/cinnamon/Muffin/IdleMonitor/Core",
                          "org.cinnamon.Muffin.IdleMonitor", idleMs))
    {
        return idleMs;
    }

    return 0;
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

struct WaylandIdleTracker
{
    wl_display*               display      = nullptr;
    wl_registry*              registry     = nullptr;
    ext_idle_notifier_v1*     notifier     = nullptr;
    wl_seat*                  seat         = nullptr;
    ext_idle_notification_v1* notification = nullptr;

    uint32_t boundNotifierVersion = 1;

    // Written by Wayland thread, read by Lua calling thread.
    std::atomic<bool>     isIdle{false};
    std::atomic<uint64_t> currentThresholdMs{0};

    // Synchronization for threshold updates and worker thread.
    std::mutex  trackerMutex;
    std::thread workerThread;
    bool        initialized = false;

    // Pipe used to wake the worker thread for threshold change or clean shutdown.
    // [0] = read end (polled by worker), [1] = write end.
    int wakeupPipe[2] = { -1, -1 };

    // ── registry listener ──────────────────────────────────────────────────
    static void onGlobal(void* data, wl_registry* reg,
                         uint32_t name, const char* interface, uint32_t version)
    {
        auto* self = static_cast<WaylandIdleTracker*>(data);
        if (std::string_view(interface) == ext_idle_notifier_v1_interface.name)
        {
            // Prefer version 2 if available (supports get_input_idle_notification)
            uint32_t bindVer = (version >= 2) ? 2 : 1;
            self->boundNotifierVersion = bindVer;
            self->notifier = static_cast<ext_idle_notifier_v1*>(
                wl_registry_bind(reg, name, &ext_idle_notifier_v1_interface, bindVer));
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
        self->isIdle.store(true, std::memory_order_release);
    }

    static void onResumed(void* data, ext_idle_notification_v1*)
    {
        auto* self = static_cast<WaylandIdleTracker*>(data);
        self->isIdle.store(false, std::memory_order_release);
    }

    static const ext_idle_notification_v1_listener idleListener;

    // Must be called with trackerMutex held
    void setupNotificationLocked(uint64_t thresholdMs)
    {
        if (!notifier || !seat || !display)
            return;

        if (notification)
        {
            ext_idle_notification_v1_destroy(notification);
            notification = nullptr;
        }

        uint32_t timeout = static_cast<uint32_t>(thresholdMs);
        if (boundNotifierVersion >= 2)
        {
            notification = ext_idle_notifier_v1_get_input_idle_notification(
                notifier, timeout, seat);
        }
        else
        {
            notification = ext_idle_notifier_v1_get_idle_notification(
                notifier, timeout, seat);
        }

        if (notification)
        {
            ext_idle_notification_v1_add_listener(notification, &idleListener, this);
            isIdle.store(false, std::memory_order_release);
            currentThresholdMs.store(thresholdMs, std::memory_order_release);
            wl_display_flush(display);
        }
    }

    // ── worker loop ────────────────────────────────────────────────────────
    bool init(uint64_t initialThresholdMs)
    {
        std::lock_guard<std::mutex> lock(trackerMutex);
        if (initialized)
            return true;

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

        if (pipe(wakeupPipe) != 0)
        {
            cleanup();
            return false;
        }

        setupNotificationLocked(initialThresholdMs);

        initialized = true;

        workerThread = std::thread([this]()
        {
            const int wlFd   = wl_display_get_fd(display);
            const int wakeFd = wakeupPipe[0];

            while (true)
            {
                if (wl_display_flush(display) < 0)
                    break;

                while (wl_display_prepare_read(display) != 0)
                    wl_display_dispatch_pending(display);

                struct pollfd fds[2];
                fds[0] = { wlFd,   POLLIN, 0 };
                fds[1] = { wakeFd, POLLIN, 0 };

                const int ret = poll(fds, 2, -1);

                if (ret < 0)
                {
                    wl_display_cancel_read(display);
                    break;
                }

                if (fds[1].revents & POLLIN)
                {
                    char cmd = 0;
                    (void)read(wakeFd, &cmd, 1);
                    wl_display_cancel_read(display);

                    if (cmd == 'Q') // Quit
                    {
                        break;
                    }
                    else if (cmd == 'U') // Update threshold notification
                    {
                        std::lock_guard<std::mutex> lk(trackerMutex);
                        setupNotificationLocked(currentThresholdMs.load(std::memory_order_acquire));
                    }
                    continue;
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

    void updateThreshold(uint64_t newThresholdMs)
    {
        if (newThresholdMs == currentThresholdMs.load(std::memory_order_acquire))
            return;

        currentThresholdMs.store(newThresholdMs, std::memory_order_release);

        if (initialized && wakeupPipe[1] != -1)
        {
            const char cmd = 'U';
            (void)write(wakeupPipe[1], &cmd, 1);
        }
    }

    // ── clean shutdown — safe to call from any thread ──────────────────────
    void shutdown()
    {
        std::lock_guard<std::mutex> lock(trackerMutex);
        if (!initialized)
            return;

        if (wakeupPipe[1] != -1)
        {
            const char cmd = 'Q';
            (void)write(wakeupPipe[1], &cmd, 1);
        }

        if (workerThread.joinable())
            workerThread.join();

        if (wakeupPipe[0] != -1) { close(wakeupPipe[0]); wakeupPipe[0] = -1; }
        if (wakeupPipe[1] != -1) { close(wakeupPipe[1]); wakeupPipe[1] = -1; }

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

    bool getIsIdle() const
    {
        return initialized && isIdle.load(std::memory_order_acquire);
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

bool getWaylandIdleStatus(uint64_t thresholdMs)
{
    std::call_once(g_trackerInitFlag, [thresholdMs]()
    {
        getTracker().init(thresholdMs);
    });

    getTracker().updateThreshold(thresholdMs);
    return getTracker().getIsIdle();
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
    // GNOME / Cinnamon: try D-Bus IdleMonitor (Mutter / Muffin)
    const char* xdgDesktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (xdgDesktop != nullptr)
    {
        std::string desktopStr(xdgDesktop);
        if (desktopStr.find("GNOME") != std::string::npos ||
            desktopStr.find("gnome") != std::string::npos ||
            desktopStr.find("Cinnamon") != std::string::npos ||
            desktopStr.find("cinnamon") != std::string::npos)
        {
            return getDbusIdleTimeMs();
        }
    }

    // Always attempt D-Bus query before other fallbacks
    uint64_t dbusIdle = getDbusIdleTimeMs();
    if (dbusIdle > 0)
        return dbusIdle;

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

#if defined(__linux__)
    // Check if running on GNOME / Cinnamon first
    const char* xdgDesktop = std::getenv("XDG_CURRENT_DESKTOP");
    bool isGnomeOrCinnamon = false;
    if (xdgDesktop != nullptr)
    {
        std::string desktopStr(xdgDesktop);
        if (desktopStr.find("GNOME") != std::string::npos ||
            desktopStr.find("gnome") != std::string::npos ||
            desktopStr.find("Cinnamon") != std::string::npos ||
            desktopStr.find("cinnamon") != std::string::npos)
        {
            isGnomeOrCinnamon = true;
        }
    }

    if (isGnomeOrCinnamon)
    {
        int status = (getDbusIdleTimeMs() >= threshold) ? 1 : 0;
        lua_pushinteger(L, status);
        return 1;
    }

    // Attempt D-Bus idle monitor if Mutter/Muffin happens to be running
    uint64_t dbusIdle = getDbusIdleTimeMs();
    if (dbusIdle > 0)
    {
        int status = (dbusIdle >= threshold) ? 1 : 0;
        lua_pushinteger(L, status);
        return 1;
    }

#if defined(HAVE_WAYLAND_IDLE)
    // Non-GNOME Wayland (KDE Plasma 6, Hyprland, Sway, niri, etc.)
    if (std::getenv("WAYLAND_DISPLAY") != nullptr)
    {
        int status = getWaylandIdleStatus(threshold) ? 1 : 0;
        lua_pushinteger(L, status);
        return 1;
    }
#endif

    int status = (getIdleTimeMs() >= threshold) ? 1 : 0;
    lua_pushinteger(L, status);
    return 1;

#else
    // Windows / Other OS
    int status = (getIdleTimeMs() >= threshold) ? 1 : 0;
    lua_pushinteger(L, status);
    return 1;
#endif
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