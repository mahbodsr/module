#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#undef SERVER_EXECUTE
#undef ARRAYSIZE
#endif

#include <stdio.h>
#include <string.h>
#include "extdll.h"
#include "meta_api.h"
#include <hiredis/hiredis.h>

plugin_info_t Plugin_info = {
    META_INTERFACE_VERSION, "WebAuth", "1.1", "2026-08", "YourName", 
    "https://yoursite.com", "WEBAUTH", PT_ANYTIME, PT_ANYTIME
};

redisContext* redis = nullptr;

meta_globals_t *gpMetaGlobals;
gamedll_funcs_t *gpGamedllFuncs;
mutil_funcs_t *gpMetaUtilFuncs;
enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals;

C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t* pengfuncsFromEngine, globalvars_t *pGlobals) {
    memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(enginefuncs_t));
    gpGlobals = pGlobals;
}

// Intercept standard API requests (AMXX, etc.)
const char* GetPlayerAuthId_Hook(edict_t *pEntity) {
    if (!pEntity || pEntity->v.flags & FL_DORMANT) {
        RETURN_META_VALUE(MRES_IGNORED, NULL);
    }
    const char* webAuthId = g_engfuncs.pfnInfoKeyValue(g_engfuncs.pfnGetInfoKeyBuffer(pEntity), "*web_auth_id");
    if (webAuthId && webAuthId[0] != '\0') {
        RETURN_META_VALUE(MRES_SUPERCEDE, webAuthId);
    }
    RETURN_META_VALUE(MRES_IGNORED, NULL);
}

// Re-applies the memory overwrite to bypass ReHLDS native `status` and ReUnion overrides
void EnforceSteamIDInMemory(edict_t *pEntity) {
    const char* webAuthId = g_engfuncs.pfnInfoKeyValue(g_engfuncs.pfnGetInfoKeyBuffer(pEntity), "*web_auth_id");
    if (webAuthId && webAuthId[0] != '\0') {
        const char* engineAuthId = g_engfuncs.pfnGetPlayerAuthId(pEntity);
        if (engineAuthId && strcmp(engineAuthId, webAuthId) != 0) {
            // Force overwrite the internal ReHLDS client_t buffer
            strncpy((char*)engineAuthId, webAuthId, 63); 
            ((char*)engineAuthId)[63] = '\0';
        }
    }
}

qboolean ClientConnect_Hook(edict_t *pEntity, const char *pszName, const char *pszAddress, char szRejectReason[128]) {
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
        // Store it in the info buffer for later retrieval
        g_engfuncs.pfnSetClientKeyValue(ENTINDEX(pEntity), g_engfuncs.pfnGetInfoKeyBuffer(pEntity), "*web_auth_id", reply->str);
        
        // Initial enforcement
        EnforceSteamIDInMemory(pEntity);

        freeReplyObject(reply);
        RETURN_META_VALUE(MRES_IGNORED, TRUE);
    } else {
        if (reply) freeReplyObject(reply);
        strncpy(szRejectReason, "Please login to the website first!", 127);
        szRejectReason[127] = '\0';
        RETURN_META_VALUE(MRES_SUPERCEDE, FALSE);
    }
}

// Hook when player has fully loaded into the server (ReUnion auth is completely done by now)
void ClientPutInServer_Hook(edict_t *pEntity) {
    EnforceSteamIDInMemory(pEntity);
    RETURN_META(MRES_IGNORED);
}

// Hook whenever client data updates to prevent ReUnion/Engine from resetting it
void ClientUserInfoChanged_Hook(edict_t *pEntity, char *infobuffer) {
    EnforceSteamIDInMemory(pEntity);
    RETURN_META(MRES_IGNORED);
}

C_DLLEXPORT int GetEntityAPI(DLL_FUNCTIONS *pFunctionTable, int interfaceVersion) {
    if (!pFunctionTable) return FALSE;
    memset(pFunctionTable, 0, sizeof(DLL_FUNCTIONS));
    pFunctionTable->pfnClientConnect = ClientConnect_Hook;
    pFunctionTable->pfnClientPutInServer = ClientPutInServer_Hook;
    pFunctionTable->pfnClientUserInfoChanged = ClientUserInfoChanged_Hook;
    return TRUE;
}

C_DLLEXPORT int GetEngineFunctions(enginefuncs_t *pengfuncsFromEngine, int *interfaceVersion) {
    if (!pengfuncsFromEngine) return FALSE;
    memset(pengfuncsFromEngine, 0, sizeof(enginefuncs_t));
    pengfuncsFromEngine->pfnGetPlayerAuthId = GetPlayerAuthId_Hook;
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

    redis = redisConnect("127.0.0.1", 6379);
    if (redis == nullptr || redis->err) {
        gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] FATAL ERROR: Could not connect to Redis!\n");
        return FALSE;
    }

    pFunctionTable->pfnGetEntityAPI = GetEntityAPI;
    pFunctionTable->pfnGetEngineFunctions = GetEngineFunctions;
    return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME now, PL_UNLOAD_REASON reason) {
    if (redis) redisFree(redis);
#ifdef _WIN32
    WSACleanup();
#endif
    return TRUE;
}
