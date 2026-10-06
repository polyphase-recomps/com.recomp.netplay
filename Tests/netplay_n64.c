/*
 * Headless netplay runner for an N64 game library (the com.recomp.n64 embedding API: recomp or
 * decomp build). Two or more of these, on one machine or several, play one game together:
 *
 *   netplay_n64 --rom game.z64 --host [port] --players 2 [--save start.sra] --frames 3000 --dump d1
 *   netplay_n64 --rom game.z64 --join 192.168.1.20[:port]          --frames 3000 --dump d2
 *
 * Each machine plays its own controller port with random input (--fuzz SEED, as the game's own
 * headless runner does, from frame 400), so the frames only agree if every machine got every
 * player's input for every frame. --dump DIR --every N writes frame_NNNNN.ppm files to compare;
 * the run ends with a summary line ("frames 3000, final hash ..., stalls ..."), which must be the
 * same on every machine.
 *
 * The game state hash sent for the desync check is the hash of the frame the game drew.
 */
#include "netplay.h"
#include "port_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
static void nap(void) { Sleep(1); }
#else
#include <time.h>
static void nap(void)
{
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
}
#endif

static void write_ppm(const char *path, const unsigned char *rgba, int width, int height)
{
    FILE *f = fopen(path, "wb");
    int i;

    if (f == NULL)
    {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (i = 0; i < width * height; i++)
    {
        fwrite(rgba + i * 4, 1, 3, f);
    }
    fclose(f);
}

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long n;

    *size = 0;
    if (f == NULL)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (unsigned char *)malloc(n > 0 ? (size_t)n : 1);
    if (data != NULL && fread(data, 1, (size_t)n, f) == (size_t)n)
    {
        *size = (size_t)n;
    }
    fclose(f);
    return data;
}

/* This machine's controller: random input from frame 400 on (menus and fights), a pure function
 * of (seed, port, frame) so runs repeat exactly; port 0 also presses START now and then early on
 * to get past the intro. */
static NetplayPad fuzz_input(unsigned seed, int port, int frame)
{
    static const unsigned short choices[] = {0x8000, 0x8000, 0x8000, 0x4000, 0x4000, 0x1000, 0x2000,
                                             0x0010, 0x0008, 0x0004, 0,      0,      0,      0, 0};
    static const signed char sticks[] = {0, 0, 0, 80, -80, 40, -40};
    NetplayPad pad;
    unsigned r;

    memset(&pad, 0, sizeof(pad));
    if (seed == 0 || frame < 400)
    {
        return pad;
    }
    r = (seed * 2654435761u) ^ ((unsigned)port * 40503u) ^ ((unsigned)(frame / 10) * 0x9E3779B9u);
    r ^= r >> 15;
    r *= 0x2C1B3C6Du;
    r ^= r >> 12;
    pad.buttons = choices[r % (sizeof(choices) / sizeof(choices[0]))];
    pad.axis[0] = sticks[(r >> 5) % sizeof(sticks)];
    pad.axis[1] = sticks[(r >> 9) % sizeof(sticks)];
    if (frame < 1000 && port == 0 && (r & 3) == 0)
    {
        pad.buttons = 0x1000;
    }
    return pad;
}

int main(int argc, char **argv)
{
    const char *rom = NULL, *join = NULL, *dump = NULL, *save_path = NULL, *name = "", *game = "ssb64";
    int host_port = -1, players = 2, frames = 3000, every = 100, delay = 2, i;
    unsigned seed = 1234;
    unsigned char *rom_data;
    size_t rom_size;
    char version[32], save_file[512];
    NetplayConfig cfg;
    Netplay *np;
    int booted = 0, done_frames = 0;
    uint64_t last_hash = 0;
    char last_status[160] = "";

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) rom = argv[++i];
        else if (strcmp(argv[i], "--host") == 0)
        {
            host_port = NETPLAY_DEFAULT_PORT;
            if (i + 1 < argc && argv[i + 1][0] != '-') host_port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--join") == 0 && i + 1 < argc) join = argv[++i];
        else if (strcmp(argv[i], "--players") == 0 && i + 1 < argc) players = atoi(argv[++i]);
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) dump = argv[++i];
        else if (strcmp(argv[i], "--every") == 0 && i + 1 < argc) every = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fuzz") == 0 && i + 1 < argc) seed = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--delay") == 0 && i + 1 < argc) delay = atoi(argv[++i]);
        else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) save_path = argv[++i];
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = argv[++i];
        else if (strcmp(argv[i], "--game") == 0 && i + 1 < argc) game = argv[++i];
        else
        {
            fprintf(stderr, "usage: netplay_n64 --rom game.z64 (--host [port] --players N [--save file] | --join addr[:port])\n"
                            "                   [--frames N] [--dump dir --every N] [--fuzz seed] [--delay frames] [--name n]\n");
            return 2;
        }
    }
    if (rom == NULL || (host_port < 0 && join == NULL))
    {
        fprintf(stderr, "need --rom and --host or --join\n");
        return 2;
    }

    /* the game version is the ROM itself: machines with different ROMs are refused */
    rom_data = read_file(rom, &rom_size);
    if (rom_data == NULL || rom_size == 0)
    {
        fprintf(stderr, "cannot read %s\n", rom);
        return 1;
    }
    snprintf(version, sizeof(version), "%016llx", (unsigned long long)netplay_hash(NETPLAY_HASH_SEED, rom_data, rom_size));
    free(rom_data);

    memset(&cfg, 0, sizeof(cfg));
    cfg.game_id = game;
    cfg.game_version = version;
    cfg.game_name = game;
    cfg.flavor = "headless";
    cfg.player_name = name;
    cfg.max_players = players > 4 ? 4 : players < 2 ? 2 : players;
    cfg.input_delay = delay;
    np = netplay_create(&cfg);

    if (host_port >= 0)
    {
        if (!netplay_host(np, host_port))
        {
            fprintf(stderr, "%s\n", netplay_status_text(np));
            return 1;
        }
        if (save_path != NULL)
        {
            size_t size;
            unsigned char *save = read_file(save_path, &size);

            netplay_set_save(np, save, size);
            free(save);
        }
        printf("hosting on UDP %d, waiting for %d player(s)\n", host_port, players - 1);
    }
    else if (!netplay_join(np, join))
    {
        fprintf(stderr, "%s\n", netplay_status_text(np));
        return 1;
    }
    fflush(stdout);

    while (done_frames < frames)
    {
        NetplayState state = netplay_state(np);
        const char *status = netplay_status_text(np);

        if (strcmp(status, last_status) != 0 && state != NETPLAY_RUNNING)
        {
            printf("%s\n", status);
            fflush(stdout);
            snprintf(last_status, sizeof(last_status), "%s", status);
        }
        if (state == NETPLAY_DESYNC || state == NETPLAY_DISCONNECTED || state == NETPLAY_ERROR)
        {
            printf("%s\n", status);
            break;
        }
        if (state == NETPLAY_LOBBY)
        {
            NetplayPlayerInfo list[4];
            int n = netplay_players(np, list, 4), ready = 0, k;

            for (k = 1; k < n; k++)
            {
                ready += list[k].ready;
            }
            if (n >= players && ready == n - 1)
            {
                netplay_start(np);
            }
        }
        if (netplay_take_start(np))
        {
            size_t size;
            const uint8_t *save = netplay_save(np, &size);
            FILE *f;

            /* everyone boots from frame 0 with the host's save, kept apart from any real save */
            snprintf(save_file, sizeof(save_file), "%s%snetplay_p%d.sra", dump ? dump : ".", "/", netplay_local_port(np));
            f = fopen(save_file, "wb");
            if (f != NULL)
            {
                if (size > 0)
                {
                    fwrite(save, 1, size, f);
                }
                fclose(f);
            }
            if (size == 0)
            {
                remove(save_file);
            }
            n64_set_save_path(save_file);
            if (!n64_boot(rom))
            {
                fprintf(stderr, "the game did not boot\n");
                break;
            }
            booted = 1;
            printf("started as player %d (input delay %d)\n", netplay_local_port(np) + 1, netplay_input_delay(np));
            fflush(stdout);
        }
        if (booted && state == NETPLAY_RUNNING)
        {
            NetplayPad local = fuzz_input(seed, netplay_local_port(np), netplay_current_frame(np) + netplay_input_delay(np));
            NetplayPad out[NETPLAY_MAX_PLAYERS];
            unsigned mask;

            if (netplay_frame(np, &local, out, &mask))
            {
                int w = 0, h = 0, p;
                const unsigned char *fb;

                for (p = 0; p < NETPLAY_MAX_PLAYERS; p++)
                {
                    PortPad pad;

                    pad.buttons = (unsigned short)out[p].buttons;
                    pad.stick_x = out[p].axis[0];
                    pad.stick_y = out[p].axis[1];
                    pad.connected = (unsigned char)((mask >> p) & 1u);
                    n64_set_pad(p, &pad);
                }
                n64_run_frame();
                done_frames++;
                fb = n64_framebuffer(&w, &h);
                last_hash = fb ? netplay_hash(NETPLAY_HASH_SEED, fb, (size_t)w * h * 4) : 0;
                netplay_frame_done(np, last_hash);
                if (dump != NULL && every > 0 && done_frames % every == 0 && fb != NULL)
                {
                    char path[512];

                    snprintf(path, sizeof(path), "%s/frame_%05d.ppm", dump, done_frames);
                    write_ppm(path, fb, w, h);
                }
                continue;
            }
        }
        else
        {
            netplay_poll(np);
        }
        nap();
    }
    /* stay a little: the others may still need our input for their last frames */
    for (i = 0; i < 2000; i++)
    {
        netplay_poll(np);
        nap();
    }
    printf("frames %d, final hash %016llx, stalls %u, %s\n", done_frames, (unsigned long long)last_hash,
           netplay_stall_count(np), netplay_status_text(np));
    netplay_destroy(np);
    if (booted)
    {
        n64_shutdown();
    }
    return done_frames == frames ? 0 : 1;
}
