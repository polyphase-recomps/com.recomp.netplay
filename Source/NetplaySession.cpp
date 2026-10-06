/**
 * @file NetplaySession.cpp
 * @brief The process-wide netplay session (see NetplaySession.h).
 */
#include "NetplaySession.h"
#include "NetplaySessionInternal.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
struct GameInfo
{
    std::string id = "game", version, name = "Game", flavor;
    int maxPlayers = 4;
    bool set = false;
};

GameInfo sGame;
std::string sSaveFile;
std::string sPlayerName;
int sInputDelay = 2;
Netplay* sSession = nullptr;
bool sSessionMatchesGame = false;
NetplayState sLastState = NETPLAY_IDLE;
NetplaySessionInternal::LogFn sLog = nullptr;

void Log(const char* text)
{
    if (sLog != nullptr)
    {
        sLog(text);
    }
}

std::vector<unsigned char> ReadFile(const std::string& path)
{
    std::vector<unsigned char> data;
    FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");

    if (f == nullptr)
    {
        return data;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0 && size <= NETPLAY_MAX_SAVE)
    {
        data.resize((size_t)size);
        if (std::fread(data.data(), 1, data.size(), f) != data.size())
        {
            data.clear();
        }
    }
    std::fclose(f);
    return data;
}

// A session object for the current game and settings (only replaced while idle).
Netplay* Session()
{
    if (sSession != nullptr && (sSessionMatchesGame || netplay_state(sSession) != NETPLAY_IDLE))
    {
        return sSession;
    }
    netplay_destroy(sSession);
    NetplayConfig config = {};
    config.game_id = sGame.id.c_str();
    config.game_version = sGame.version.c_str();
    config.game_name = sGame.name.c_str();
    config.flavor = sGame.flavor.c_str();
    config.player_name = sPlayerName.c_str();
    config.max_players = sGame.maxPlayers;
    config.input_delay = sInputDelay;
    sSession = netplay_create(&config);
    sSessionMatchesGame = true;
    return sSession;
}

void Invalidate()
{
    sSessionMatchesGame = false;
}
}

namespace NetplaySessionInternal
{
void SetLogger(LogFn log)
{
    sLog = log;
}
}

void NetplaySession::SetGame(const char* id, const char* version, const char* name, const char* flavor, int maxPlayers)
{
    GameInfo info;
    info.id = id ? id : "game";
    info.version = version ? version : "";
    info.name = (name && name[0]) ? name : info.id;
    info.flavor = flavor ? flavor : "";
    info.maxPlayers = maxPlayers;
    info.set = true;
    if (info.id != sGame.id || info.version != sGame.version || info.name != sGame.name || info.flavor != sGame.flavor ||
        info.maxPlayers != sGame.maxPlayers || !sGame.set)
    {
        sGame = info;
        Invalidate();
    }
}

void NetplaySession::SetSaveFile(const char* path)
{
    sSaveFile = path ? path : "";
}

const char* NetplaySession::GameName()
{
    return sGame.name.c_str();
}

bool NetplaySession::HasGame()
{
    return sGame.set;
}

Netplay* NetplaySession::Get()
{
    return Session();
}

bool NetplaySession::Host(int udpPort)
{
    Netplay* np = Session();

    if (!sGame.set)
    {
        Log("[netplay] start the game first (Play), so the session knows which game it is");
        return false;
    }
    if (!netplay_host(np, udpPort))
    {
        Log((std::string("[netplay] ") + netplay_status_text(np)).c_str());
        return false;
    }
    const std::vector<unsigned char> save = ReadFile(sSaveFile);
    netplay_set_save(np, save.data(), save.size());
    char line[160];
    std::snprintf(line, sizeof(line), "[netplay] hosting %s on UDP port %d (save data: %u bytes)", sGame.name.c_str(),
                  udpPort > 0 ? udpPort : NETPLAY_DEFAULT_PORT, (unsigned)save.size());
    Log(line);
    return true;
}

bool NetplaySession::Join(const char* address)
{
    Netplay* np = Session();

    if (!sGame.set)
    {
        Log("[netplay] start the game first (Play), so the session knows which game it is");
        return false;
    }
    if (!netplay_join(np, address))
    {
        Log((std::string("[netplay] ") + netplay_status_text(np)).c_str());
        return false;
    }
    Log((std::string("[netplay] joining ") + address).c_str());
    return true;
}

bool NetplaySession::Start()
{
    Netplay* np = Session();

    if (netplay_state(np) == NETPLAY_LOBBY)
    {
        // the save as it is now (the host may have played since hosting)
        const std::vector<unsigned char> save = ReadFile(sSaveFile);
        netplay_set_save(np, save.data(), save.size());
    }
    if (!netplay_start(np))
    {
        Log("[netplay] cannot start yet: every player must have joined and received the save data");
        return false;
    }
    Log("[netplay] started");
    return true;
}

void NetplaySession::Leave()
{
    if (sSession != nullptr && netplay_state(sSession) != NETPLAY_IDLE)
    {
        netplay_stop(sSession);
        Log("[netplay] left the session");
    }
}

void NetplaySession::SetPlayerName(const char* name)
{
    std::string n = name ? name : "";
    if (n != sPlayerName)
    {
        sPlayerName = n;
        Invalidate();
    }
}

const char* NetplaySession::PlayerName()
{
    return sPlayerName.c_str();
}

void NetplaySession::SetInputDelay(int frames)
{
    if (frames < 1 || frames > NETPLAY_MAX_DELAY)
    {
        return;
    }
    sInputDelay = frames;
    if (sSession != nullptr && netplay_state(sSession) == NETPLAY_LOBBY)
    {
        netplay_set_input_delay(sSession, frames);
    }
    else
    {
        Invalidate();
    }
}

int NetplaySession::InputDelay()
{
    return sSession != nullptr && netplay_state(sSession) != NETPLAY_IDLE ? netplay_input_delay(sSession) : sInputDelay;
}

bool NetplaySession::IsPlaying()
{
    const NetplayState state = netplay_state(sSession);
    return sSession != nullptr && (state == NETPLAY_RUNNING);
}

void NetplaySession::Poll()
{
    if (sSession == nullptr)
    {
        return;
    }
    netplay_poll(sSession);
    const NetplayState state = netplay_state(sSession);
    if (state != sLastState)
    {
        sLastState = state;
        if (state != NETPLAY_IDLE)
        {
            Log((std::string("[netplay] ") + netplay_status_text(sSession)).c_str());
        }
    }
}

void NetplaySession::Shutdown()
{
    netplay_destroy(sSession);
    sSession = nullptr;
    sSessionMatchesGame = false;
    sLastState = NETPLAY_IDLE;
}
