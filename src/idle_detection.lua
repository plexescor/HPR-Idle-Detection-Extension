HPR.extensionName = "HPR Idle Detection Extension"
HPR.authorName = "Plexescor"

local initializer, err
local started = true
local defaultThreshold = 8 * 60 * 1000 --8min
local currentIdleThreshold

local ignoredTitles = {
    "youtube",
}

function init()
    HPR.log(HPR.extensionName, HPR.extensionName .. " Initialized")

    local extDir = (HPR.getExtensionAbsoluteDir ~= nil) and HPR.getExtensionAbsoluteDir() or HPR.getExtensionDir()

    if HPR.getOsName() == "Windows" then
        dllPath = extDir .. "HPR_Idle_Detection_Extension.dll"
    else
        dllPath = extDir .. "HPR_Idle_Detection_Extension.so"
    end

    HPR.log(HPR.extensionName, "Extension dir: " .. extDir)
    HPR.log(HPR.extensionName, "DLL path: " .. dllPath)
    HPR.log(HPR.extensionName, "package.loadlib = " .. tostring(package.loadlib))

    initializer, err = package.loadlib(dllPath, "initialiseFunctions")

    HPR.log(HPR.extensionName, "initializer = " .. tostring(initializer))
    HPR.log(HPR.extensionName, "error = " .. tostring(err))

    if not initializer then
        return
    end

    HPR.log(HPR.extensionName, "Calling initialiseFunctions...")
    initializer()
    HPR.log(HPR.extensionName, "initialiseFunctions returned successfully")

    currentIdleThreshold = HPR.readCsv(
        HPR.getExtensionDir() .. "idle_detection_config.csv",
        "idle-threshold"
    )

    if currentIdleThreshold == "" then
        HPR.writeCsv(
            HPR.getExtensionDir() .. "idle_detection_config.csv",
            "idle-threshold",
            defaultThreshold
        )
        currentIdleThreshold = defaultThreshold
    end

    return 5000
end
function onTick(delta)
    --What we will do is call a function and it returns some status
    --sort of like this

    --int
    --0 -> Not Idle
    --1 -> Idle
    
    local status = getIdleStatus(currentIdleThreshold)

    local title = HPR.getCurrentTitle()
    local foundIgnored = false

    for _, searchTerm in ipairs(ignoredTitles) do
        if string.find(string.lower(title), searchTerm, 1, true) then
            foundIgnored = true
            break
        end
    end

    if status == 1 and started and not foundIgnored then
        HPR.stopTracking()
        started = false
    elseif status == 0 and not started then
        HPR.startTracking()
        started = true
    end
end

function onExit()

end