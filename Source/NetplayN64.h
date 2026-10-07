/**
 * @file NetplayN64.h
 * @brief Netplay for an N64 game player (com.recomp.n64's embedding API). Header-only: include
 *        it after the game API (N64GameApi.h / port_host.h: PortPad, n64_*).
 *
 * In the player:
 *
 *   // once the game has booted (and whenever it boots another file)
 *   NetplayN64::Configure(N64_GAME_ID, "Super Smash Bros.", flavor, bootedPath, savePath);
 *
 *   // in Tick, before the player's own frame loop
 *   const int ran = NetplayN64::RunFrames(deltaTime, LocalPad(), reboot, afterFrame, saveDir, mFrameAccumulator);
 *   if (ran >= 0) { if (ran > 0) UpdateDisplay(); return; } // the session ran this tick's frames
 *
 * where LocalPad() is this machine's controller (what it would put on port 1), afterFrame runs
 * after every game frame (e.g. to submit its audio), and reboot is
 * [&](const std::string& save) { n64_shutdown(); n64_set_save_path(save.c_str()); n64_boot(rom); }.
 *
 * A session reboots the game from frame 0 with the host's save (kept in saveDir/netplay.sra, so
 * this machine's own save is never touched); when the session ends, the game reboots again with
 * this machine's own save.
 */
#pragma once

#include "NetplaySession.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace NetplayN64
{
const float kFrameTime = 1.0f / 60.0f;
// How long a tick may wait for a late input before giving the frame up (the engine's frame is
// 16.7 ms: a short wait still shows the frame on time, a skipped one is a visible hitch).
inline int& WaitBudgetMs()
{
    static int sMs = 10;
    return sMs;
}

// this machine's own save (Configure) and whether a session was playing (RunFrames)
inline std::string& OwnSave()
{
    static std::string sPath;
    return sPath;
}

inline bool& WasPlaying()
{
    static bool sPlaying = false;
    return sPlaying;
}

// A hash of the file the game booted: the session's game version, so machines with another ROM
// (or another asset pack) are refused.
inline std::string FileVersion(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    uint64_t hash = NETPLAY_HASH_SEED;
    static unsigned char buffer[1 << 16];
    size_t got;
    char text[24];

    if (f == nullptr)
    {
        return "";
    }
    while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
    {
        hash = netplay_hash(hash, buffer, got);
    }
    std::fclose(f);
    std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)hash);
    return text;
}

// What this machine plays, once the game has booted. flavor: e.g. "recomp" or "decomp"
// (machines on another flavor are told; the state hash check catches real differences).
inline void Configure(const char* gameId, const char* gameName, const char* flavor, const std::string& bootedPath,
                      const std::string& savePath, int maxPlayers = 4)
{
    static std::string sPath, sVersion;

    if (bootedPath != sPath)
    {
        sPath = bootedPath;
        sVersion = FileVersion(bootedPath);
    }
    NetplaySession::SetGame(gameId, sVersion.c_str(), gameName, flavor, maxPlayers);
    NetplaySession::SetSaveFile(savePath.c_str());
    OwnSave() = savePath;
}

// While a session plays, runs the game's frames on the session's inputs and returns how many
// (0: waiting for another player); the player must not run frames itself this tick. Returns -1
// (do as usual) when no session plays.
//   local        this machine's controller
//   reboot       void(const std::string& savePath): shut the game down and boot it again from
//                frame 0 with that save file (when the session starts, and when it ends)
//   afterFrame   void(): after every game frame (e.g. submit its audio)
//   saveDir      where the session's save file goes (kept apart from the player's own save)
//   accumulator  the player's frame time accumulator
template <typename Reboot, typename AfterFrame>
int RunFrames(float deltaTime, const PortPad& local, Reboot reboot, AfterFrame afterFrame, const std::string& saveDir,
              float& accumulator, int maxFrames = 3)
{
    Netplay* np = NetplaySession::Get();

    if (!NetplaySession::IsPlaying())
    {
        if (WasPlaying())
        {
            // the session is over (left, disconnected, desync): back to this machine's own game
            WasPlaying() = false;
            reboot(OwnSave());
            accumulator = 0.0f;
        }
        return -1;
    }
    WasPlaying() = true;
    if (netplay_take_start(np))
    {
        size_t size = 0;
        const uint8_t* save = netplay_save(np, &size);
        const std::string path = saveDir + "/netplay.sra";

        if (size > 0)
        {
            FILE* f = std::fopen(path.c_str(), "wb");
            if (f != nullptr)
            {
                std::fwrite(save, 1, size, f);
                std::fclose(f);
            }
        }
        else
        {
            std::remove(path.c_str());
        }
        reboot(path);
        accumulator = 0.0f;
    }

    accumulator += deltaTime;
    if (accumulator > 0.25f)
    {
        accumulator = 0.25f;
    }
    const auto tickStart = std::chrono::steady_clock::now();
    n64_set_skip_draw(0); // every frame is drawn (see below)
    int ran = 0;
    for (; ran < maxFrames && accumulator >= kFrameTime; ran++)
    {
        NetplayPad pad = {};
        NetplayPad out[NETPLAY_MAX_PLAYERS];
        unsigned mask = 0;
        const int frame = netplay_current_frame(np);
        const bool hashed = (frame + 1) % NETPLAY_HASH_INTERVAL == 0;

        pad.buttons = local.buttons;
        pad.axis[0] = local.stick_x;
        pad.axis[1] = local.stick_y;
        if (!netplay_frame(np, &pad, out, &mask))
        {
            // another player's input is late: wait for it a little (it usually comes within a few
            // ms) rather than skip the whole frame
            const int waited = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tickStart).count();
            if (ran > 0 || waited >= WaitBudgetMs() || !NetplaySession::IsPlaying())
            {
                break;
            }
            netplay_wait(np, WaitBudgetMs() - waited);
            ran--; // try this frame again
            continue;
        }
        for (int port = 0; port < NETPLAY_MAX_PLAYERS; port++)
        {
            PortPad p = {};
            p.buttons = (unsigned short)out[port].buttons;
            p.stick_x = out[port].axis[0];
            p.stick_y = out[port].axis[1];
            p.connected = (unsigned char)((mask >> port) & 1u);
            n64_set_pad(port, &p);
        }
        // Every frame is drawn, also when catching up: a game can build its picture on the last one
        // (fades) or read its framebuffer, so a skipped draw made the hashed picture (and maybe
        // the game) depend on timing: a false desync when one machine caught up and another not.
        n64_run_frame();
        accumulator -= kFrameTime;

        uint64_t hash = 0;
        if (hashed)
        {
#ifdef N64_HAS_FRAME_SIGNATURE
            // what the frame drew (its display lists): the same whatever render scale each
            // machine picked, where a hash of the picture would differ
            const unsigned long long signature = n64_frame_signature();
            if (signature != 0)
            {
                hash = netplay_hash(NETPLAY_HASH_SEED, &signature, sizeof(signature));
            }
            else
#endif
            {
                int w = 0, h = 0;
                const unsigned char* fb = n64_framebuffer(&w, &h);
                hash = fb != nullptr ? netplay_hash(NETPLAY_HASH_SEED, fb, (size_t)w * (size_t)h * 4) : 0;
            }
        }
        netplay_frame_done(np, hash);
        afterFrame();
    }
    if (accumulator > kFrameTime * 2)
    {
        accumulator = kFrameTime * 2; // waiting: do not bank time to rush through later
    }
    return ran;
}
}
