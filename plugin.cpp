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

// ReHLDS API Pointers
IRehldsApi* g_RehldsApi = nullptr;
IRehldsServerData* g_RehldsData = nullptr;

enum AuthState {
    STATE_DISCONNECTED = 0,
    STATE_WAITING_FOR_REUNION,
    STATE_AUTHENTICATED
};

char g_ApprovedSteamID[33][32] = {0};
AuthState g_PlayerState[33] = {STATE_DISCONNECTED};

C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t* pengfuncsFromEngine, globalvars_t *pGlobals) {
    memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(enginefuncs_t));
    gpGlobals = pGlobals;
}

// ------------------------------------------------------------------------
// ReHLDS Factory Loader (Cross-Platform)
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
// STEP 1: Catch connection & fetch from Redis
// ------------------------------------------------------------------------
qboolean ClientConnect_Hook(edict_t *pEntity, const char *pszName, const char *pszAddress, char szRejectReason[128]) {
    int index = g_engfuncs.pfnIndexOfEdict(pEntity);
    if (index > 0 && index <= 32) {
        g_ApprovedSteamID[index][0] = '\0';
        g_PlayerState[index] = STATE_DISCONNECTED;
    }

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
                g_PlayerState[index] = STATE_WAITING_FOR_REUNION;
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

// ------------------------------------------------------------------------
// STEP 2: The True ReHLDS Memory Stomp
// ------------------------------------------------------------------------
void StompAuthID(int index) {
    if (!g_RehldsData) return;
    
    if (index > 0 && index <= 32 && g_PlayerState[index] == STATE_WAITING_FOR_REUNION) {
        
        // ReHLDS GetClient expects a 0-based index (0 to 31)
        IRehldsClient* pClient = g_RehldsData->GetClient(index - 1);
        
        if (pClient && pClient->IsConnected()) {
            
            // Bypass Metamod completely and fetch the exact memory pointer from the ReHLDS engine
            char* engine_authid = const_cast<char*>(pClient->GetAuthId());
            
            // If Reunion has finished generating its dummy ID and it is no longer PENDING...
            if (engine_authid && 
                strcmp(engine_authid, "STEAM_ID_PENDING") != 0 && 
                strcmp(engine_authid, "BOT") != 0 && 
                engine_authid[0] != '\0') 
            {
                gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] Reunion finished! Stomping '%s' with Redis ID '%s'\n", engine_authid, g_ApprovedSteamID[index]);
                
                // Write directly into ReHLDS core memory
                strncpy(engine_authid, g_ApprovedSteamID[index], 31);
                engine_authid[31] = '\0';
                
                g_PlayerState[index] = STATE_AUTHENTICATED;
            }
        }
    }
}

// Surround AMX Mod X on all fronts to guarantee we stomp before it reads
void ClientUserInfoChanged_Hook(edict_t *pEntity, char *infobuffer) {
    StompAuthID(g_engfuncs.pfnIndexOfEdict(pEntity));
    RETURN_META(MRES_IGNORED);
}

void PlayerPreThink_Hook(edict_t *pEntity) {
    StompAuthID(g_engfuncs.pfnIndexOfEdict(pEntity));
    RETURN_META(MRES_IGNORED);
}

void StartFrame_Hook() {
    for (int i = 1; i <= gpGlobals->maxClients; i++) {
        StompAuthID(i);
    }
    RETURN_META(MRES_IGNORED);
}

void ClientDisconnect_Hook(edict_t *pEntity) {
    int index = g_engfuncs.pfnIndexOfEdict(pEntity);
    if (index > 0 && index <= 32) {
        g_ApprovedSteamID[index][0] = '\0'; 
        g_PlayerState[index] = STATE_DISCONNECTED;
    }
    RETURN_META(MRES_IGNORED);
}

C_DLLEXPORT int GetEntityAPI(DLL_FUNCTIONS *pFunctionTable, int interfaceVersion) {
    if (!pFunctionTable) return FALSE;
    memset(pFunctionTable, 0, sizeof(DLL_FUNCTIONS));
    pFunctionTable->pfnClientConnect = ClientConnect_Hook;
    pFunctionTable->pfnClientDisconnect = ClientDisconnect_Hook;
    pFunctionTable->pfnClientUserInfoChanged = ClientUserInfoChanged_Hook;
    pFunctionTable->pfnPlayerPreThink = PlayerPreThink_Hook;
    pFunctionTable->pfnStartFrame = StartFrame_Hook;
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

    // Hook directly into the ReHLDS API
    CreateInterfaceFn factory = GetEngineFactory();
    if (factory) {
        int ret;
        g_RehldsApi = (IRehldsApi*)factory("VREHLDS_API_VERSION001", &ret);
        if (g_RehldsApi) {
            g_RehldsData = g_RehldsApi->GetServerData();
            gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] ReHLDS API successfully linked!\n");
        }
    } else {
        gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] WARNING: Could not hook ReHLDS API.\n");
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
    return TRUE;
}