#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#undef SERVER_EXECUTE
#undef ARRAYSIZE
#else
#include <dlfcn.h>
#endif

#include <stdio.h>
#include <string.h>
#include "extdll.h"
#include "meta_api.h"
#include "rehlds_api.h"
#include <hiredis/hiredis.h>

plugin_info_t Plugin_info = {
    META_INTERFACE_VERSION, "WebAuth", "1.0", "2026-08", "YourName", 
    "https://yoursite.com", "WEBAUTH", PT_ANYTIME, PT_ANYTIME
};

redisContext* redis = nullptr;
meta_globals_t *gpMetaGlobals;
gamedll_funcs_t *gpGamedllFuncs;
mutil_funcs_t *gpMetaUtilFuncs;
enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals;

// Correct ReHLDS API Pointers
IRehldsApi* g_RehldsApi = nullptr;
IRehldsServerStatic* g_RehldsStatic = nullptr;
IRehldsHookchains* g_Hookchains = nullptr;

char g_ApprovedSteamID[33][32] = {0};

C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t* pengfuncsFromEngine, globalvars_t *pGlobals) {
    memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(enginefuncs_t));
    gpGlobals = pGlobals;
}

// ------------------------------------------------------------------------
// Cross-Platform ReHLDS Factory Loader
// ------------------------------------------------------------------------
typedef void* (*CreateInterfaceFn)(const char *pName, int *pReturnCode);
CreateInterfaceFn GetEngineFactory() {
#ifdef _WIN32
    HMODULE hModule = GetModuleHandleA("swds.dll");
    if (!hModule) hModule = GetModuleHandleA("hw.dll");
    if (!hModule) return nullptr;
    return (CreateInterfaceFn)GetProcAddress(hModule, "CreateInterface");
#else
    void *hModule = dlopen("engine_i486.so", RTLD_NOW);
    if (!hModule) return nullptr;
    auto factory = (CreateInterfaceFn)dlsym(hModule, "CreateInterface");
    dlclose(hModule);
    return factory;
#endif
}

// ------------------------------------------------------------------------
// 1. Initial Connection: Fetch ID from Redis
// ------------------------------------------------------------------------
qboolean ClientConnect_Hook(edict_t *pEntity, const char *pszName, const char *pszAddress, char szRejectReason[128]) {
    int index = g_engfuncs.pfnIndexOfEdict(pEntity);
    if (index > 0 && index <= 32) g_ApprovedSteamID[index][0] = '\0';

    if (!redis || redis->err) {
        RETURN_META_VALUE(MRES_IGNORED, TRUE); 
    }

    char ip[64];
    strncpy(ip, pszAddress, sizeof(ip) - 1);
    ip[sizeof(ip) - 1] = '\0';
    char* colon = strchr(ip, ':');
    if (colon) *colon = '\0';

    redisReply* reply = (redisReply*)redisCommand(redis, "GET session:%s", ip);
    
    if (reply != nullptr && reply->type == REDIS_REPLY_STRING) {
        char extractedSteamId[64] = {0};
        
        char* steamKey = strstr(reply->str, "\"steamId\":\"");
        if (steamKey) {
            steamKey += 11;
            char* endQuote = strchr(steamKey, '"');
            if (endQuote) {
                int length = endQuote - steamKey;
                if (length > 0 && length < 32) {
                    strncpy(extractedSteamId, steamKey, length);
                    extractedSteamId[length] = '\0';
                }
            }
        } else if (strncmp(reply->str, "STEAM_", 6) == 0) {
            strncpy(extractedSteamId, reply->str, 31);
            extractedSteamId[31] = '\0';
        }

        if (extractedSteamId[0] != '\0') {
            if (index > 0 && index <= 32) {
                strncpy(g_ApprovedSteamID[index], extractedSteamId, 31);
                g_ApprovedSteamID[index][31] = '\0';
            }
            freeReplyObject(reply);
            RETURN_META_VALUE(MRES_IGNORED, TRUE); 
        } else {
            if (reply) freeReplyObject(reply);
            strncpy(szRejectReason, "Invalid session data format!", 127);
            RETURN_META_VALUE(MRES_SUPERCEDE, FALSE);
        }
    } else {
        if (reply) freeReplyObject(reply);
        strncpy(szRejectReason, "Please login to the website first!", 127);
        RETURN_META_VALUE(MRES_SUPERCEDE, FALSE);
    }
}

void ClientDisconnect_Hook(edict_t *pEntity) {
    int index = g_engfuncs.pfnIndexOfEdict(pEntity);
    if (index > 0 && index <= 32) g_ApprovedSteamID[index][0] = '\0'; 
    RETURN_META(MRES_IGNORED);
}

// ------------------------------------------------------------------------
// 2. The ReHLDS Native Hook: Intercept Reunion's string dynamically
// ------------------------------------------------------------------------
class IGameClient; // Forward declare the client class to bypass layout issues

// We catch the ReHLDS Certificate Check right after Reunion generates its dummy ID.
void SV_FinishCertificateCheck_Hook(IRehldsHook_SV_FinishCertificateCheck* chain, IGameClient* cl, int bIsLegit, const char* authid) {
    if (!g_RehldsStatic) {
        chain->callNext(cl, bIsLegit, authid);
        return;
    }

    int index = -1;
    // Compare the opaque pointer against ReHLDS to find the player index
    for (int i = 0; i < gpGlobals->maxClients; i++) {
        if (g_RehldsStatic->GetClient(i) == cl) {
            index = i + 1;
            break;
        }
    }

    if (index > 0 && index <= 32 && g_ApprovedSteamID[index][0] != '\0') {
        // We intercept Reunion's dummy "authid" and substitute it with our Redis ID.
        // When callNext() runs, AMX Mod X and the Engine officially receive our Redis ID!
        chain->callNext(cl, bIsLegit, g_ApprovedSteamID[index]);
    } else {
        // Pass the original ID through if they aren't authenticated
        chain->callNext(cl, bIsLegit, authid);
    }
}

C_DLLEXPORT int GetEntityAPI(DLL_FUNCTIONS *pFunctionTable, int interfaceVersion) {
    if (!pFunctionTable) return FALSE;
    memset(pFunctionTable, 0, sizeof(DLL_FUNCTIONS));
    pFunctionTable->pfnClientConnect = ClientConnect_Hook;
    pFunctionTable->pfnClientDisconnect = ClientDisconnect_Hook;
    return TRUE;
}

C_DLLEXPORT int Meta_Query(char *ifvers, plugin_info_t **pPlugInfo, mutil_funcs_t *pMetaUtilFuncs) {
    *pPlugInfo = &Plugin_info;
    gpMetaUtilFuncs = pMetaUtilFuncs;
    return TRUE;
}

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME now, META_FUNCTIONS *pFunctionTable, meta_globals_t *pMGlobals, gamedll_funcs_t *pGamedllFuncs) {
    gpMetaGlobals = pMGlobals;
    gpGamedllFuncs = pGamedllFuncs;

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
#endif

    // Hook ReHLDS Native API
    CreateInterfaceFn factory = GetEngineFactory();
    if (factory) {
        int ret;
        g_RehldsApi = (IRehldsApi*)factory("VREHLDS_API_VERSION001", &ret);
        if (g_RehldsApi) {
            g_Hookchains = g_RehldsApi->GetHookchains();
            g_RehldsStatic = g_RehldsApi->GetServerStatic();
            
            if (g_Hookchains && g_RehldsStatic) {
                g_Hookchains->SV_FinishCertificateCheck()->registerHook(SV_FinishCertificateCheck_Hook);
                gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] ReHLDS Native Interceptor Registered!\n");
            }
        }
    } else {
        gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] WARNING: Could not link ReHLDS API.\n");
    }

    redis = redisConnect("127.0.0.1", 6379);
    if (redis == nullptr || redis->err) {
        gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] FATAL ERROR: Could not connect to Redis!\n");
        return FALSE;
    }

    pFunctionTable->pfnGetEntityAPI = GetEntityAPI;
    return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME now, PL_UNLOAD_REASON reason) {
    if (redis) redisFree(redis);
#ifdef _WIN32
    WSACleanup();
#endif
    if (g_Hookchains) {
        g_Hookchains->SV_FinishCertificateCheck()->unregisterHook(SV_FinishCertificateCheck_Hook);
    }
    return TRUE;
}