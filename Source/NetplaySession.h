/**
 * @file NetplaySession.h
 * @brief The process-wide netplay session of com.recomp.netplay.
 *
 * One machine plays in at most one session. The addon owns it: the editor's Netplay window and
 * the Netplay Lua table host, join and start it; game players register what they play
 * (NetplaySession_SetGame, once the game has booted) and, while a session runs, take their
 * frames through it (NetplayN64.h does that for N64 players).
 */
#pragma once

#include "netplay.h"

#include <string>

namespace NetplaySession
{
// The game this machine plays: sessions only form between machines with the same id and
// version. Called by the game's player once it knows (e.g. version = a hash of its ROM).
NETPLAY_API void SetGame(const char* id, const char* version, const char* name, const char* flavor, int maxPlayers);
// The save data a host sends everyone (read when hosting and again at the start): the file the
// game's player keeps its save in. Empty: no save data.
NETPLAY_API void SetSaveFile(const char* path);
NETPLAY_API const char* GameName();
NETPLAY_API bool HasGame();

// The session (created on first use, for the registered game).
NETPLAY_API Netplay* Get();

// Menu actions (log what happens; false with the reason in netplay_status_text()).
NETPLAY_API bool Host(int udpPort);
NETPLAY_API bool Join(const char* address);
NETPLAY_API bool Start();
NETPLAY_API void Leave();

// Player settings (used by the next Host / Join).
NETPLAY_API void SetPlayerName(const char* name);
NETPLAY_API const char* PlayerName();
NETPLAY_API void SetInputDelay(int frames); // 0: auto (from the round trips at the start)
NETPLAY_API int InputDelay();              // 0 while auto, before the start

// The game runs on the session's inputs (from the start until the session ends). While true
// the game's player must take every frame from netplay_frame().
NETPLAY_API bool IsPlaying();

// Called every tick by the addon (keeps lobbies and connections alive between game frames).
NETPLAY_API void Poll();
NETPLAY_API void Shutdown();
}
