#pragma once

#ifdef _WIN32
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace leonida::nvidia {

#pragma pack(push, 8)
struct NVDRS_PROFILE {
    uint32_t version;
    wchar_t profileName[2048];
    uint32_t gpuSupport[32];
    uint32_t isPredefined;
    uint32_t numOfApps;
    uint32_t numOfTargettedDatabases;
};
#define NVDRS_PROFILE_VER (uint32_t)(sizeof(NVDRS_PROFILE) | 0x10000)

struct NVDRS_APPLICATION {
    uint32_t version;
    uint32_t isPredefined;
    wchar_t appName[2048];
    wchar_t userFriendlyName[2048];
    wchar_t launcher[2048];
    wchar_t fileInFolder[2048];
};
#define NVDRS_APPLICATION_VER (uint32_t)(sizeof(NVDRS_APPLICATION) | 0x10000)

struct NVDRS_SETTING {
    uint32_t version;
    wchar_t settingName[2048];
    uint32_t settingId;
    uint32_t settingType; // 0 = DWORD, 1 = BINARY, 2 = UNICODE
    uint32_t settingLocation;
    uint32_t isCurrentPredefined;
    uint32_t isPredefinedValid;
    union {
        uint32_t u32CurrentValue;
        struct {
            uint32_t binaryLength;
            uint8_t* pBinaryData;
        } binaryValue;
        wchar_t wsCurrentValue[2048];
    };
    union {
        uint32_t u32PredefinedValue;
        struct {
            uint32_t binaryLength;
            uint8_t* pPredefinedBinaryData;
        } predefinedBinaryValue;
        wchar_t wsPredefinedValue[2048];
    };
};
#define NVDRS_SETTING_VER (uint32_t)(sizeof(NVDRS_SETTING) | 0x10000)
#pragma pack(pop)

using NvAPI_QueryInterface_t = void* (*)(uint32_t id);
using NvAPI_Status_t = int;

constexpr uint32_t ID_NvAPI_Initialize = 0x0150E828;
constexpr uint32_t ID_NvAPI_DRS_CreateSession = 0x0694D52E;
constexpr uint32_t ID_NvAPI_DRS_DestroySession = 0xDAD9CFF8;
constexpr uint32_t ID_NvAPI_DRS_LoadSettings = 0x375DBD6B;
constexpr uint32_t ID_NvAPI_DRS_SaveSettings = 0xFCBC7E14;
constexpr uint32_t ID_NvAPI_DRS_GetBaseProfile = 0xDA8466A0;
constexpr uint32_t ID_NvAPI_DRS_FindProfileByName = 0x7E4A9A0B;
constexpr uint32_t ID_NvAPI_DRS_CreateProfile = 0xCC176068;
constexpr uint32_t ID_NvAPI_DRS_CreateApplication = 0x4347A9DE;
constexpr uint32_t ID_NvAPI_DRS_SetSetting = 0x577DD202;

// Correct NVIDIA DRS Setting IDs (from NvApiDriverSettings.h)
constexpr uint32_t PS_SHADERDISKCACHE_MAX_SIZE_ID = 0x00AC8497; // Shader Disk Cache Max Size
constexpr uint32_t OGL_THREAD_CONTROL_ID = 0x20C1221E;          // Threaded Optimization

inline bool configure_nvidia_profile() {
    static bool executed = false;
    static bool success = false;
    if (executed) return success;
    executed = true;

    HMODULE hNvApi = LoadLibraryA("nvapi64.dll");
    if (!hNvApi) return false;

    auto queryInterface = reinterpret_cast<NvAPI_QueryInterface_t>(GetProcAddress(hNvApi, "nvapi_QueryInterface"));
    if (!queryInterface) {
        FreeLibrary(hNvApi);
        return false;
    }

    auto nvInit = reinterpret_cast<NvAPI_Status_t (*)()>(queryInterface(ID_NvAPI_Initialize));
    auto nvCreateSession = reinterpret_cast<NvAPI_Status_t (*)(void**)>(queryInterface(ID_NvAPI_DRS_CreateSession));
    auto nvDestroySession = reinterpret_cast<NvAPI_Status_t (*)(void*)>(queryInterface(ID_NvAPI_DRS_DestroySession));
    auto nvLoadSettings = reinterpret_cast<NvAPI_Status_t (*)(void*)>(queryInterface(ID_NvAPI_DRS_LoadSettings));
    auto nvSaveSettings = reinterpret_cast<NvAPI_Status_t (*)(void*)>(queryInterface(ID_NvAPI_DRS_SaveSettings));
    auto nvGetBaseProfile = reinterpret_cast<NvAPI_Status_t (*)(void*, void**)>(queryInterface(ID_NvAPI_DRS_GetBaseProfile));
    auto nvFindProfile = reinterpret_cast<NvAPI_Status_t (*)(void*, const wchar_t*, void**)>(queryInterface(ID_NvAPI_DRS_FindProfileByName));
    auto nvCreateProfile = reinterpret_cast<NvAPI_Status_t (*)(void*, NVDRS_PROFILE*, void**)>(queryInterface(ID_NvAPI_DRS_CreateProfile));
    auto nvCreateApp = reinterpret_cast<NvAPI_Status_t (*)(void*, void*, NVDRS_APPLICATION*)>(queryInterface(ID_NvAPI_DRS_CreateApplication));
    auto nvSetSetting = reinterpret_cast<NvAPI_Status_t (*)(void*, void*, NVDRS_SETTING*)>(queryInterface(ID_NvAPI_DRS_SetSetting));

    if (!nvInit || !nvCreateSession || !nvDestroySession || !nvLoadSettings || !nvSaveSettings || !nvSetSetting) {
        FreeLibrary(hNvApi);
        return false;
    }

    if (nvInit() != 0) {
        FreeLibrary(hNvApi);
        return false;
    }

    void* session = nullptr;
    if (nvCreateSession(&session) != 0 || !session) {
        FreeLibrary(hNvApi);
        return false;
    }

    if (nvLoadSettings(session) != 0) {
        nvDestroySession(session);
        FreeLibrary(hNvApi);
        return false;
    }

    void* profile = nullptr;
    const wchar_t* profileName = L"Leonida GTA VI";
    if (nvFindProfile && nvFindProfile(session, profileName, &profile) != 0) {
        profile = nullptr;
    }
    if (!profile && nvCreateProfile) {
        NVDRS_PROFILE profInfo{};
        profInfo.version = NVDRS_PROFILE_VER;
        wcsncpy_s(profInfo.profileName, profileName, _TRUNCATE);
        profInfo.isPredefined = 0;
        nvCreateProfile(session, &profInfo, &profile);
    }
    if (!profile && nvGetBaseProfile) {
        nvGetBaseProfile(session, &profile);
        profileName = L"Base / Global Profile";
    }

    if (profile) {
        if (nvCreateApp) {
            const wchar_t* apps[] = { L"gta6rt.exe", L"game_scarlett_master.exe" };
            for (const wchar_t* app : apps) {
                NVDRS_APPLICATION appInfo{};
                appInfo.version = NVDRS_APPLICATION_VER;
                appInfo.isPredefined = 0;
                wcsncpy_s(appInfo.appName, app, _TRUNCATE);
                wcsncpy_s(appInfo.userFriendlyName, app, _TRUNCATE);
                nvCreateApp(session, profile, &appInfo); // If already exists or base profile, safe to ignore
            }
        }

        // Setting 1: PS_SHADERDISKCACHE_MAX_SIZE -> 0xFFFFFFFF (unlimited on-disk cache)
        NVDRS_SETTING cacheSetting{};
        cacheSetting.version = NVDRS_SETTING_VER;
        cacheSetting.settingId = PS_SHADERDISKCACHE_MAX_SIZE_ID;
        cacheSetting.settingType = 0; // DWORD
        cacheSetting.u32CurrentValue = 0xFFFFFFFF; // Unlimited
        nvSetSetting(session, profile, &cacheSetting);

        // Setting 2: OGL_THREAD_CONTROL -> 1 (Threaded Optimization)
        NVDRS_SETTING threadSetting{};
        threadSetting.version = NVDRS_SETTING_VER;
        threadSetting.settingId = OGL_THREAD_CONTROL_ID;
        threadSetting.settingType = 0; // DWORD
        threadSetting.u32CurrentValue = 1; // On
        nvSetSetting(session, profile, &threadSetting);

        if (nvSaveSettings(session) == 0) {
            std::fprintf(stderr, "[rt] nvidia-drs: configured profile '%ls' for gta6rt.exe & game_scarlett_master.exe\n"
                                 "                (ShaderDiskCacheMaxSize: Unlimited, ThreadControl: On)\n"
                                 "                [Note: Driver DRS settings apply to subsequent launches;\n"
                                 "                 Vulkan driver cache is pre-warmed via --precompile-pipelines]\n",
                         profileName);
            success = true;
        }
    }

    nvDestroySession(session);
    FreeLibrary(hNvApi);
    return success;
}

} // namespace leonida::nvidia
#endif
