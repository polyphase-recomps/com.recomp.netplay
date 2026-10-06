/*
 * The game-player side of netplay, headless: what an N64 game's player node does in the editor
 * (NetplaySession + NetplayN64::RunFrames), driven by a fake 60 Hz tick. Boots the game alone
 * first (as a player does), then hosts or joins; when the session starts every machine reboots
 * the game with the host's save and plays on the session's inputs, with the desync check on.
 *
 *   netplay_player --rom game.z64 --host [port] --players 2 [--save start.sra] --frames 3000
 *   netplay_player --rom game.z64 --join addr[:port] --frames 3000
 *
 * Ends with "frames N, final picture hash H, <status>": H must match on every machine, and the
 * status must not be a desync.
 *
 * Timing (how much a link slows the game down):
 *   --realtime         ticks at 60 Hz with the real frame time, as the engine does, and prints the
 *                      speed the session played at (100%: no slowdown)
 *   --lag ms[,jitter]  holds this machine's packets back ms + 0..jitter ms (Wi-Fi: e.g. 8,12)
 *   --delay N          the input delay (host; 0: auto)
 *   --nobg --nowait    without the network thread / without waiting for late input in the tick
 */
#include "port_host.h"

#include "NetplayN64.h"
#include "NetplaySession.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
static void Nap() { Sleep(1); }
#else
#include <time.h>
static void Nap()
{
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, nullptr);
}
#endif

// This machine's controller: random input from frame 400 on, a pure function of (seed, port,
// frame) so runs repeat exactly; port 0 also presses START early on to get past the intro.
static PortPad FuzzPad(unsigned seed, int port, int frame)
{
    static const unsigned short choices[] = {0x8000, 0x8000, 0x8000, 0x4000, 0x4000, 0x1000, 0x2000,
                                             0x0010, 0x0008, 0x0004, 0,      0,      0,      0, 0};
    static const signed char sticks[] = {0, 0, 0, 80, -80, 40, -40};
    PortPad pad = {};

    if (frame < 400)
    {
        return pad;
    }
    unsigned r = (seed * 2654435761u) ^ ((unsigned)port * 40503u) ^ ((unsigned)(frame / 10) * 0x9E3779B9u);
    r ^= r >> 15;
    r *= 0x2C1B3C6Du;
    r ^= r >> 12;
    pad.buttons = choices[r % (sizeof(choices) / sizeof(choices[0]))];
    pad.stick_x = sticks[(r >> 5) % sizeof(sticks)];
    pad.stick_y = sticks[(r >> 9) % sizeof(sticks)];
    pad.connected = 1;
    if (frame < 1000 && port == 0 && (r & 3) == 0)
    {
        pad.buttons = 0x1000;
    }
    return pad;
}

int main(int argc, char** argv)
{
    const char *rom = nullptr, *join = nullptr, *savePath = nullptr, *dir = ".";
    int hostPort = -1, players = 2, frames = 3000, lagMs = 0, jitterMs = 0, delay = -1;
    bool realtime = false, background = true;
    unsigned seed = 1234;

    for (int i = 1; i < argc; i++)
    {
        if (!std::strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
        else if (!std::strcmp(argv[i], "--host"))
        {
            hostPort = NETPLAY_DEFAULT_PORT;
            if (i + 1 < argc && argv[i + 1][0] != '-') hostPort = std::atoi(argv[++i]);
        }
        else if (!std::strcmp(argv[i], "--join") && i + 1 < argc) join = argv[++i];
        else if (!std::strcmp(argv[i], "--players") && i + 1 < argc) players = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--save") && i + 1 < argc) savePath = argv[++i];
        else if (!std::strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!std::strcmp(argv[i], "--fuzz") && i + 1 < argc) seed = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--realtime")) realtime = true;
        else if (!std::strcmp(argv[i], "--lag") && i + 1 < argc)
        {
            const char* text = argv[++i];
            lagMs = std::atoi(text);
            if (const char* comma = std::strchr(text, ',')) jitterMs = std::atoi(comma + 1);
        }
        else if (!std::strcmp(argv[i], "--delay") && i + 1 < argc) delay = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--nobg")) background = false;
        else if (!std::strcmp(argv[i], "--nowait")) NetplayN64::WaitBudgetMs() = 0;
        else
        {
            std::fprintf(stderr, "usage: netplay_player --rom game.z64 (--host [port] --players N [--save f] | --join a[:p])"
                                 " [--frames N] [--dir d] [--fuzz s] [--realtime] [--lag ms[,jitter]] [--delay N]"
                                 " [--nobg] [--nowait]\n");
            return 2;
        }
    }
    // neither: the session comes from the environment (NETPLAY=host / join:<address>), as in a
    // packaged build
    const bool fromEnvironment = hostPort < 0 && join == nullptr;
    if (rom == nullptr || (fromEnvironment && std::getenv("NETPLAY") == nullptr))
    {
        return 2;
    }
    frames -= frames % NETPLAY_HASH_INTERVAL; // end on a frame that is always drawn

    // the player boots the game on its own first, and tells the session what it plays
    const std::string ownSave = savePath ? savePath : "";
    n64_set_save_path(ownSave.c_str());
    if (!n64_boot(rom))
    {
        std::fprintf(stderr, "the game did not boot\n");
        return 1;
    }
    NetplayN64::Configure("ssb64", "Super Smash Bros.", "recomp", rom, ownSave, players);
    if (delay >= 0)
    {
        NetplaySession::SetInputDelay(delay);
    }

    if (!fromEnvironment && (hostPort >= 0 ? !NetplaySession::Host(hostPort) : !NetplaySession::Join(join)))
    {
        std::printf("%s\n", netplay_status_text(NetplaySession::Get()));
        return 1;
    }
    netplay_debug_set_lag(NetplaySession::Get(), lagMs, jitterMs);
    netplay_set_background(NetplaySession::Get(), background ? 1 : 0);
#if defined(_WIN32)
    timeBeginPeriod(1); // 1 ms sleeps, as a game's frame pacing has
#endif

    using Clock = std::chrono::steady_clock;
    const auto kTick = std::chrono::microseconds(16667);
    auto lastTick = Clock::now(), nextTick = lastTick + kTick, runStart = lastTick;
    int runStartFrame = -1;
    auto readySince = Clock::time_point();
    float accumulator = 0.0f;
    int localFrames = 0, reboots = 0;
    std::string last;
    for (;;)
    {
        Netplay* np = NetplaySession::Get();
        NetplaySession::Poll(); // the addon's tick
        const NetplayState state = netplay_state(np);
        const std::string status = netplay_status_text(np);

        if (status != last && state != NETPLAY_RUNNING)
        {
            std::printf("%s\n", status.c_str());
            std::fflush(stdout);
            last = status;
        }
        if (state == NETPLAY_DESYNC || state == NETPLAY_DISCONNECTED || state == NETPLAY_ERROR)
        {
            break;
        }
        if (state == NETPLAY_LOBBY && !fromEnvironment)
        {
            NetplayPlayerInfo list[4];
            const int n = netplay_players(np, list, 4);
            int ready = 0;
            for (int k = 1; k < n; k++)
            {
                ready += list[k].ready;
            }
            // everyone ready: start after a moment in the lobby, as a person would (the round trips
            // are measured meanwhile, for the auto input delay)
            if (n >= players && ready == n - 1)
            {
                if (readySince == Clock::time_point())
                {
                    readySince = Clock::now();
                }
                if (Clock::now() - readySince > std::chrono::seconds(2))
                {
                    NetplaySession::Start();
                }
            }
            else
            {
                readySince = Clock::time_point();
            }
        }
        if (state == NETPLAY_RUNNING && netplay_current_frame(np) >= frames)
        {
            break;
        }

        // the player's Tick: netplay first, its own frames otherwise
        const int port = netplay_local_port(np) < 0 ? 0 : netplay_local_port(np);
        const PortPad local = FuzzPad(seed, port, netplay_current_frame(np) + netplay_input_delay(np));
        auto reboot = [&](const std::string& save) {
            n64_shutdown();
            n64_set_save_path(save.c_str());
            if (!n64_boot(rom))
            {
                std::fprintf(stderr, "reboot failed\n");
                std::exit(1);
            }
            reboots++;
        };
        float dt = 1.0f / 60.0f;
        int maxFrames = 1;
        if (realtime)
        {
            const auto now = Clock::now();
            dt = std::chrono::duration<float>(now - lastTick).count();
            lastTick = now;
            maxFrames = 3; // as the players: catch up a little after a slow tick
            if (state == NETPLAY_RUNNING && runStartFrame < 0 && netplay_current_frame(np) >= 600)
            {
                runStartFrame = netplay_current_frame(np); // measured past the boot
                runStart = now;
            }
        }
        if (NetplayN64::RunFrames(dt, local, reboot, [] {}, dir, accumulator, maxFrames) < 0)
        {
            // not in a running session: the game plays alone (the lobby keeps it ticking)
            PortPad idle = {};
            idle.connected = 1;
            n64_set_pad(0, &idle);
            n64_run_frame();
            localFrames++;
        }
        if (realtime)
        {
            std::this_thread::sleep_until(nextTick);
            nextTick += kTick;
            if (Clock::now() > nextTick + kTick * 4)
            {
                nextTick = Clock::now() + kTick; // far behind (a reboot): don't rush
            }
        }
        else
        {
            Nap();
        }
    }

    Netplay* np = NetplaySession::Get();
    int w = 0, h = 0;
    const unsigned char* fb = n64_framebuffer(&w, &h);
    const uint64_t hash = fb ? netplay_hash(NETPLAY_HASH_SEED, fb, (size_t)w * h * 4) : 0;
    const int ran = netplay_current_frame(np);
    std::printf("frames %d, final picture hash %016llx, reboots %d, waits %u, %s\n", ran, (unsigned long long)hash, reboots,
                netplay_stall_count(np), netplay_status_text(np));
    if (realtime && runStartFrame >= 0)
    {
        const double seconds = std::chrono::duration<double>(Clock::now() - runStart).count();
        const double fps = (ran - runStartFrame) / seconds;
        std::printf("speed: %.1f fps over %d frames (%.1f%% of full speed)\n", fps, ran - runStartFrame,
                    fps / 60.0 * 100.0);
    }
    std::fflush(stdout);
    for (int i = 0; i < 2000; i++) // the others may still need our input
    {
        NetplaySession::Poll();
        Nap();
    }
    NetplaySession::Shutdown();
    n64_shutdown();
    return ran == frames && reboots == 1 ? 0 : 1;
}
