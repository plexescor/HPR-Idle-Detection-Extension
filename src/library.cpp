#include "lua.hpp"
#include <iostream>

#ifdef WIN32
    #include <windows.h>
#endif
#include <iostream>

uint64_t getIdleTimeMs()
{
    #ifdef WIN32
        LASTINPUTINFO lii{};
        lii.cbSize = sizeof(LASTINPUTINFO);

        if (!GetLastInputInfo(&lii))
            return 0;

        return GetTickCount64() - lii.dwTime;
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
extern "C" __declspec(dllexport)
int initialiseFunctions(lua_State* L) 
{
    lua_register(L, "getIdleStatus", l_getIdleStatus); // Registers function globally
    return 0;
}