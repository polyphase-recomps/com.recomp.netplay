/**
 * @file ComRecompNetplay.cpp
 * @brief Native addon: com.recomp.netplay (networked multiplayer for the recomp game runtimes).
 *
 * - keeps the session alive every tick (lobby, connections) between game frames;
 * - the Netplay Lua table, for a game's own menus (host, join, LAN search, start, status);
 * - in the editor, Developer > Netplay...: a window to host or join and watch the session.
 *
 * Game players take their frames through the session (NetplayN64.h for N64 games).
 */
#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"
#if EDITOR
#include "Plugins/EditorUIHooks.h"
#include "imgui.h"
#endif

#include "NetplaySession.h"
#include "NetplaySessionInternal.h"

#include <cstdio>
#include <cstring>
#include <string>

static PolyphaseEngineAPI* sEngineAPI = nullptr;

static void LogLine(const char* line)
{
    if (sEngineAPI != nullptr && sEngineAPI->LogDebug != nullptr)
    {
        sEngineAPI->LogDebug("%s", line);
    }
}

static const char* StateName(NetplayState state)
{
    switch (state)
    {
    case NETPLAY_IDLE: return "idle";
    case NETPLAY_LOBBY: return "lobby";
    case NETPLAY_JOINING: return "joining";
    case NETPLAY_SYNCING: return "syncing";
    case NETPLAY_READY: return "ready";
    case NETPLAY_RUNNING: return "running";
    case NETPLAY_DESYNC: return "desync";
    case NETPLAY_DISCONNECTED: return "disconnected";
    case NETPLAY_ERROR: return "error";
    }
    return "idle";
}

// ---- Lua: the Netplay table -----------------------------------------------------------
namespace
{
typedef int (*LuaFunction)(lua_State* L);
struct LuaReg
{
    const char* name;
    LuaFunction func;
};

constexpr int kLuaTString = 4;

PolyphaseEngineAPI* Api()
{
    return sEngineAPI;
}

void SetString(lua_State* L, const char* field, const char* value)
{
    Api()->Lua_pushstring(L, value);
    Api()->Lua_setfield(L, -2, field);
}

void SetInt(lua_State* L, const char* field, long long value)
{
    Api()->Lua_pushinteger(L, value);
    Api()->Lua_setfield(L, -2, field);
}

void SetBool(lua_State* L, const char* field, bool value)
{
    Api()->Lua_pushboolean(L, value ? 1 : 0);
    Api()->Lua_setfield(L, -2, field);
}

int L_Host(lua_State* L)
{
    const int port = Api()->Lua_gettop(L) >= 1 ? (int)Api()->Lua_tonumber(L, 1) : 0;
    Api()->Lua_pushboolean(L, NetplaySession::Host(port) ? 1 : 0);
    return 1;
}

int L_Join(lua_State* L)
{
    const char* address = Api()->Lua_type(L, 1) == kLuaTString ? Api()->Lua_tostring(L, 1) : nullptr;
    Api()->Lua_pushboolean(L, address != nullptr && NetplaySession::Join(address) ? 1 : 0);
    return 1;
}

int L_Start(lua_State* L)
{
    Api()->Lua_pushboolean(L, NetplaySession::Start() ? 1 : 0);
    return 1;
}

int L_Leave(lua_State* L)
{
    (void)L;
    NetplaySession::Leave();
    return 0;
}

int L_Search(lua_State* L)
{
    const bool enable = Api()->Lua_gettop(L) < 1 || Api()->Lua_toboolean(L, 1) != 0;
    netplay_search(NetplaySession::Get(), enable ? 1 : 0);
    return 0;
}

int L_Hosts(lua_State* L)
{
    NetplayFoundHost hosts[16];
    const int n = netplay_found(NetplaySession::Get(), hosts, 16);

    Api()->Lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++)
    {
        Api()->Lua_pushinteger(L, i + 1);
        Api()->Lua_createtable(L, 0, 7);
        SetString(L, "address", hosts[i].address);
        SetString(L, "game", hosts[i].game_name);
        SetString(L, "host", hosts[i].host_name);
        SetInt(L, "players", hosts[i].players);
        SetInt(L, "maxPlayers", hosts[i].max_players);
        SetBool(L, "inGame", hosts[i].in_game != 0);
        SetBool(L, "compatible", hosts[i].compatible != 0);
        Api()->Lua_settable(L, -3);
    }
    return 1;
}

int L_Players(lua_State* L)
{
    NetplayPlayerInfo players[NETPLAY_MAX_PLAYERS];
    const int n = netplay_players(NetplaySession::Get(), players, NETPLAY_MAX_PLAYERS);

    Api()->Lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++)
    {
        Api()->Lua_pushinteger(L, i + 1);
        Api()->Lua_createtable(L, 0, 6);
        SetInt(L, "player", players[i].port + 1);
        SetString(L, "name", players[i].name);
        SetBool(L, "ready", players[i].ready != 0);
        SetBool(L, "connected", players[i].connected != 0);
        SetInt(L, "ping", players[i].rtt_ms);
        SetBool(L, "isLocal", players[i].is_local != 0);
        Api()->Lua_settable(L, -3);
    }
    return 1;
}

int L_State(lua_State* L)
{
    Api()->Lua_pushstring(L, StateName(netplay_state(NetplaySession::Get())));
    return 1;
}

int L_Status(lua_State* L)
{
    Api()->Lua_pushstring(L, netplay_status_text(NetplaySession::Get()));
    return 1;
}

int L_IsHost(lua_State* L)
{
    Api()->Lua_pushboolean(L, netplay_is_host(NetplaySession::Get()));
    return 1;
}

int L_IsPlaying(lua_State* L)
{
    Api()->Lua_pushboolean(L, NetplaySession::IsPlaying() ? 1 : 0);
    return 1;
}

int L_LocalPlayer(lua_State* L)
{
    Api()->Lua_pushinteger(L, netplay_local_port(NetplaySession::Get()) + 1);
    return 1;
}

int L_Frame(lua_State* L)
{
    Api()->Lua_pushinteger(L, netplay_current_frame(NetplaySession::Get()));
    return 1;
}

int L_SetName(lua_State* L)
{
    if (Api()->Lua_type(L, 1) == kLuaTString)
    {
        NetplaySession::SetPlayerName(Api()->Lua_tostring(L, 1));
    }
    return 0;
}

int L_SetDelay(lua_State* L)
{
    NetplaySession::SetInputDelay((int)Api()->Lua_tonumber(L, 1));
    return 0;
}

int L_Delay(lua_State* L)
{
    Api()->Lua_pushinteger(L, NetplaySession::InputDelay());
    return 1;
}

int L_DefaultPort(lua_State* L)
{
    Api()->Lua_pushinteger(L, NETPLAY_DEFAULT_PORT);
    return 1;
}
}

static void RegisterScriptFuncs(lua_State* L)
{
    PolyphaseEngineAPI* api = sEngineAPI;
    if (L == nullptr || api == nullptr || api->Lua_createtable == nullptr || api->LuaL_setfuncs == nullptr ||
        api->Lua_setglobal == nullptr)
    {
        return;
    }
    static const LuaReg kNetplay[] = {
        {"Host", L_Host},         {"Join", L_Join},           {"Start", L_Start},
        {"Leave", L_Leave},       {"Search", L_Search},       {"Hosts", L_Hosts},
        {"Players", L_Players},   {"State", L_State},         {"Status", L_Status},
        {"IsHost", L_IsHost},     {"IsPlaying", L_IsPlaying}, {"LocalPlayer", L_LocalPlayer},
        {"Frame", L_Frame},       {"SetName", L_SetName},     {"SetDelay", L_SetDelay},
        {"Delay", L_Delay},       {"DefaultPort", L_DefaultPort}, {nullptr, nullptr},
    };
    api->Lua_createtable(L, 0, 17);
    api->LuaL_setfuncs(L, kNetplay, 0);
    api->Lua_setglobal(L, "Netplay");
}

// ---- editor window ----------------------------------------------------------------------
#if EDITOR
static const char* const kWindowId = "com.recomp.netplay.window";

static void DrawWindow(void*)
{
    static char name[32] = "";
    static char address[64] = "";
    static int port = NETPLAY_DEFAULT_PORT;
    static bool namesLoaded = false;
    static bool searching = false;
    Netplay* np = NetplaySession::Get();
    const NetplayState state = netplay_state(np);
    const bool idle = state == NETPLAY_IDLE || state == NETPLAY_DISCONNECTED || state == NETPLAY_ERROR ||
                      state == NETPLAY_DESYNC;

    if (!namesLoaded)
    {
        std::snprintf(name, sizeof(name), "%s", NetplaySession::PlayerName());
        namesLoaded = true;
    }

    if (NetplaySession::HasGame())
    {
        ImGui::Text("Game: %s", NetplaySession::GameName());
    }
    else
    {
        ImGui::TextWrapped("Start the game (Play) first: the session needs to know which game this machine plays.");
    }
    ImGui::TextWrapped("%s", netplay_status_text(np));
    if (netplay_peer_flavor_differs(np))
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           "Another machine runs another build flavor (recomp / decomp). If the games diverge the\n"
                           "session stops with a desync.");
    }
    ImGui::Separator();

    if (idle)
    {
        if (ImGui::InputText("Your name", name, sizeof(name)))
        {
            NetplaySession::SetPlayerName(name);
        }
        int delay = NetplaySession::InputDelay();
        if (ImGui::SliderInt("Input delay (frames)", &delay, 1, 8))
        {
            NetplaySession::SetInputDelay(delay);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Each player's input takes effect this many frames later, which hides the network\n"
                              "latency. LAN: 2. Internet: about ping / 16 + 1 (the host's choice counts).");
        }

        ImGui::SeparatorText("Host");
        ImGui::InputInt("UDP port", &port);
        if (ImGui::Button("Host a session"))
        {
            NetplaySession::Host(port);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(others join this machine's IP:port; on the internet, forward this UDP port)");

        ImGui::SeparatorText("Join");
        ImGui::InputText("Host address", address, sizeof(address));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("IP or name, with :port when it is not %d", NETPLAY_DEFAULT_PORT);
        }
        if (ImGui::Button("Join") && address[0] != 0)
        {
            NetplaySession::Join(address);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Search the LAN", &searching))
        {
            netplay_search(np, searching ? 1 : 0);
        }
        if (searching)
        {
            NetplayFoundHost hosts[16];
            const int n = netplay_found(np, hosts, 16);
            if (n == 0)
            {
                ImGui::TextDisabled("No sessions found on port %d yet...", NETPLAY_DEFAULT_PORT);
            }
            for (int i = 0; i < n; i++)
            {
                ImGui::PushID(i);
                const bool joinable = hosts[i].compatible && !hosts[i].in_game && hosts[i].players < hosts[i].max_players;
                if (!joinable)
                {
                    ImGui::BeginDisabled();
                }
                if (ImGui::SmallButton("Join"))
                {
                    NetplaySession::Join(hosts[i].address);
                }
                if (!joinable)
                {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                ImGui::Text("%s: %s (%d/%d)  %s%s%s", hosts[i].host_name, hosts[i].game_name, hosts[i].players,
                            hosts[i].max_players, hosts[i].address, hosts[i].in_game ? "  in game" : "",
                            hosts[i].compatible ? "" : "  other game or ROM");
                ImGui::PopID();
            }
        }
        if (state != NETPLAY_IDLE && ImGui::Button("Close"))
        {
            NetplaySession::Leave();
        }
        return;
    }

    // in a session
    NetplayPlayerInfo players[NETPLAY_MAX_PLAYERS];
    const int n = netplay_players(np, players, NETPLAY_MAX_PLAYERS);
    if (ImGui::BeginTable("players", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Player");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Ping");
        ImGui::TableHeadersRow();
        for (int i = 0; i < n; i++)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d%s", players[i].port + 1, players[i].is_local ? " (you)" : "");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(players[i].name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(!players[i].connected ? "left" : players[i].ready ? "ready" : "joining");
            ImGui::TableNextColumn();
            if (players[i].rtt_ms >= 0)
            {
                ImGui::Text("%d ms", players[i].rtt_ms);
            }
            else
            {
                ImGui::TextDisabled("-");
            }
        }
        ImGui::EndTable();
    }
    if (state == NETPLAY_LOBBY)
    {
        int delay = netplay_input_delay(np);
        if (ImGui::SliderInt("Input delay (frames)", &delay, 1, 8))
        {
            NetplaySession::SetInputDelay(delay);
        }
        if (ImGui::Button("Start"))
        {
            NetplaySession::Start();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(when everyone is ready; every machine's game restarts from the beginning)");
    }
    if (state == NETPLAY_RUNNING)
    {
        ImGui::Text("Frame %d, input delay %d, waits %u", netplay_current_frame(np), netplay_input_delay(np),
                    netplay_stall_count(np));
    }
    if (ImGui::Button("Leave"))
    {
        NetplaySession::Leave();
    }
}

static EditorUIHooks* sHooks = nullptr;

static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    sHooks = hooks;
    if (hooks->RegisterWindow != nullptr)
    {
        hooks->RegisterWindow(hookId, "Netplay", kWindowId, DrawWindow, nullptr);
    }
    if (hooks->AddMenuItem != nullptr)
    {
        hooks->AddMenuItem(hookId, "Developer", "Netplay...",
                           [](void*) {
                               if (sHooks != nullptr && sHooks->OpenWindow != nullptr)
                               {
                                   sHooks->OpenWindow(kWindowId);
                               }
                           },
                           nullptr, nullptr);
    }
}

static void TickEditor(float deltaTime)
{
    (void)deltaTime;
    NetplaySession::Poll();
}
#endif

// ---- plugin -------------------------------------------------------------------------------
static int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;
    NetplaySessionInternal::SetLogger(LogLine);
    return 0;
}

static void OnUnload()
{
    NetplaySession::Shutdown(); // says goodbye to the other machines
    NetplaySessionInternal::SetLogger(nullptr);
    sEngineAPI = nullptr;
}

static void RegisterTypes(void* nodeFactory)
{
    (void)nodeFactory;
}

static void Tick(float deltaTime)
{
    (void)deltaTime;
    NetplaySession::Poll();
}

static int FillDesc(PolyphasePluginDesc* desc)
{
    desc->apiVersion = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName = "com.recomp.netplay";
    desc->pluginVersion = "1.0.0";
    desc->OnLoad = OnLoad;
    desc->OnUnload = OnUnload;
    desc->Tick = Tick;
    desc->RegisterTypes = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI = RegisterEditorUI;
    desc->TickEditor = TickEditor;
#else
    desc->RegisterEditorUI = nullptr;
    desc->TickEditor = nullptr;
#endif
    desc->OnEditorPreInit = nullptr;
    desc->OnEditorReady = nullptr;
    return 0;
}

#if EDITOR
extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    return FillDesc(desc);
}
#else
extern "C" int PolyphasePlugin_GetDesc_com_recomp_netplay(PolyphasePluginDesc* desc)
{
    return FillDesc(desc);
}
#endif
