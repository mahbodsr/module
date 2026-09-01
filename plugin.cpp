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
    META_INTERFACE_VERSION, "WebAuth", "1.0", "2026-08", "YourName", 
    "https://yoursite.com", "WEBAUTH", PT_ANYTIME, PT_ANYTIME
};

redisContext* redis = nullptr;
meta_globals_t *gpMetaGlobals;
gamedll_funcs_t *gpGamedllFuncs;
mutil_funcs_t *gpMetaUtilFuncs;
enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals;

char g_ApprovedSteamID[33][32] = {0};

C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t* pengfuncsFromEngine, globalvars_t *pGlobals) {
    memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(enginefuncs_t));
    gpGlobals = pGlobals;
}

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
            gpMetaUtilFuncs->pfnLogConsole(PLID, "\n============================================\n");
            gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] PHASE 1: ClientConnect (%s)\n", pszName);
            gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] Extracted Target ID: %s\n", extractedSteamId);
            
            if (index > 0 && index <= 32) {
                strncpy(g_ApprovedSteamID[index], extractedSteamId, 31);
                g_ApprovedSteamID[index][31] = '\0';
            }

            char* engine_authid = (char*)g_engfuncs.pfnGetPlayerAuthId(pEntity);
            if (engine_authid) {
                gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] Engine Memory BEFORE write: %s\n", engine_authid);
                
                // Write the ID directly to memory
                strncpy(engine_authid, extractedSteamId, 31);
                engine_authid[31] = '\0';
                
                // Immediately read it back to verify C++ didn't fail
                gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] Engine Memory AFTER write: %s\n", (char*)g_engfuncs.pfnGetPlayerAuthId(pEntity));
            } else {
                gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] ERROR: pfnGetPlayerAuthId returned NULL pointer!\n");
            }
            gpMetaUtilFuncs->pfnLogConsole(PLID, "============================================\n\n");

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

// Check the memory again when they actually spawn in. 
// If the engine itself is overwriting our changes after ClientConnect, we will catch it here.
void ClientPutInServer_Hook(edict_t *pEntity) {
    int index = g_engfuncs.pfnIndexOfEdict(pEntity);
    if (index > 0 && index <= 32 && g_ApprovedSteamID[index][0] != '\0') {
        
        char* engine_authid = (char*)g_engfuncs.pfnGetPlayerAuthId(pEntity);
        
        gpMetaUtilFuncs->pfnLogConsole(PLID, "\n============================================\n");
        gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] PHASE 2: ClientPutInServer\n");
        gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] Current Engine Memory: %s\n", engine_authid);
        
        if (engine_authid && strcmp(engine_authid, g_ApprovedSteamID[index]) != 0) {
            gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] WARNING: Engine reverted our ID back to %s!\n", engine_authid);
            gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] RE-APPLYING: %s...\n", g_ApprovedSteamID[index]);
            
            strncpy(engine_authid, g_ApprovedSteamID[index], 31);
            engine_authid[31] = '\0';
            
            gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] FINAL VERIFY: %s\n", (char*)g_engfuncs.pfnGetPlayerAuthId(pEntity));
        } else {
            gpMetaUtilFuncs->pfnLogConsole(PLID, "[WebAuth] SUCCESS: Engine retained our SteamID correctly.\n");
        }
        gpMetaUtilFuncs->pfnLogConsole(PLID, "============================================\n\n");
    }
    RETURN_META(MRES_IGNORED);
}

void ClientDisconnect_Hook(edict_t *pEntity) {
    int index = g_engfuncs.pfnIndexOfEdict(pEntity);
    if (index > 0 && index <= 32) {
        g_ApprovedSteamID[index][0] = '\0'; 
    }
    RETURN_META(MRES_IGNORED);
}

C_DLLEXPORT int GetEntityAPI(DLL_FUNCTIONS *pFunctionTable, int interfaceVersion) {
    if (!pFunctionTable) return FALSE;
    memset(pFunctionTable, 0, sizeof(DLL_FUNCTIONS));
    pFunctionTable->pfnClientConnect = ClientConnect_Hook;
    pFunctionTable->pfnClientPutInServer = ClientPutInServer_Hook;
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