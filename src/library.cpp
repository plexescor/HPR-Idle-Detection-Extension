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
#endif

uint64_t getIdleTimeMs()
{
#if defined(_WIN32) || defined(WIN32)
    LASTINPUTINFO lii{};
    lii.cbSize = sizeof(LASTINPUTINFO);

    if (!GetLastInputInfo(&lii))
        return 0;

    return GetTickCount64() - lii.dwTime;
#elif defined(__linux__)
    const char* xdgDesktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (xdgDesktop != nullptr)
    {
        std::string desktopStr(xdgDesktop);
        if (desktopStr.find("GNOME") != std::string::npos || desktopStr.find("gnome") != std::string::npos)
        {
            return getGnomeIdleTimeMs();
        }
    }
    return 0;
#else
    return 0;
#endif
}

// The native function called from Lua
int l_getIdleStatus(lua_State* L)
{
    uint64_t threshold = static_cast<uint64_t>(luaL_checkinteger(L, 1));

    int status = (getIdleTimeMs() >= threshold) ? 1 : 0;

    lua_pushinteger(L, status);
    return 1;
}

// Entry point called by Lua's package loader
extern "C" EXPORT_SYMBOL
int initialiseFunctions(lua_State* L) 
{
    lua_register(L, "getIdleStatus", l_getIdleStatus); // Registers function globally
    return 0;
}