/*
 * Self-test of the netplay core: a host and two clients in one process, over 127.0.0.1, with
 * packets dropped on purpose. Checks that
 *   1. the host's save data reaches the clients intact;
 *   2. every machine runs every frame with the same inputs, which are what each player sent
 *      `delay` frames earlier;
 *   3. a machine whose game state differs is caught (desync);
 *   4. a client that vanishes is dropped and the others play on, still in step.
 *
 *   netplay_selftest [loss_percent] [base_port]
 */
#include "netplay.h"

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

#define FRAMES 3000
#define PEERS 3

static int sFailures;

#define CHECK(cond, ...)                                                                              \
    do                                                                                                \
    {                                                                                                 \
        if (!(cond))                                                                                  \
        {                                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                               \
            printf(__VA_ARGS__);                                                                      \
            printf("\n");                                                                             \
            sFailures++;                                                                              \
        }                                                                                             \
    } while (0)

static NetplayPad input_of(int port, int frame)
{
    NetplayPad pad;
    uint32_t h = (uint32_t)(frame * 2654435761u) ^ (uint32_t)(port * 40503u + 7u);

    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    pad.buttons = h & 0xFFFF;
    pad.axis[0] = (int8_t)(h >> 16);
    pad.axis[1] = (int8_t)(h >> 24);
    pad.axis[2] = 0;
    pad.axis[3] = 0;
    return pad;
}

typedef struct Machine
{
    Netplay *np;
    int frames;       /* frames run */
    int alive;        /* polled at all */
    uint64_t state;   /* "game state": a hash of every input it ran */
    int corrupt_at;   /* make the state differ from this frame on (-1 never) */
    NetplayPad log[FRAMES + 400][NETPLAY_MAX_PLAYERS];
    unsigned mask;
} Machine;

static Machine sM[PEERS];

static NetplayConfig config(const char *name)
{
    NetplayConfig c;

    memset(&c, 0, sizeof(c));
    c.game_id = "selftest";
    c.game_version = "1";
    c.game_name = "Self test";
    c.flavor = "test";
    c.player_name = name;
    c.max_players = 4;
    c.input_delay = 3;
    c.timeout_ms = 1500;
    return c;
}

static void poll_all(void)
{
    int i;

    for (i = 0; i < PEERS; i++)
    {
        if (sM[i].alive)
        {
            netplay_poll(sM[i].np);
        }
    }
}

static int wait_until(int (*cond)(void), int ms)
{
    int t;

    for (t = 0; t < ms; t++)
    {
        poll_all();
        if (cond())
        {
            return 1;
        }
        nap();
    }
    return 0;
}

static int clients_ready(void)
{
    return netplay_state(sM[1].np) == NETPLAY_READY && netplay_state(sM[2].np) == NETPLAY_READY;
}

/* the host starts once it has heard every client say it is ready */
static int host_started(void)
{
    return netplay_start(sM[0].np) || netplay_state(sM[0].np) == NETPLAY_RUNNING;
}

/* Step machine i once: run a frame if the gate allows it. */
static void step(int i, int stop_at)
{
    Machine *m = &sM[i];
    NetplayPad local, out[NETPLAY_MAX_PLAYERS];
    unsigned mask;

    if (!m->alive)
    {
        return;
    }
    if (m->frames >= stop_at)
    {
        netplay_poll(m->np); /* done, but still answering (the others may need its frames) */
        return;
    }
    netplay_take_start(m->np);
    local = input_of(netplay_local_port(m->np), m->frames + netplay_input_delay(m->np));
    if (netplay_frame(m->np, &local, out, &mask))
    {
        memcpy(m->log[m->frames], out, sizeof(out));
        m->mask = mask;
        m->state = netplay_hash(m->state, out, sizeof(out));
        m->frames++;
        netplay_frame_done(m->np, m->state + (m->corrupt_at >= 0 && m->frames > m->corrupt_at ? 1 : 0));
    }
}

static void run_session(int stop_at, int max_ms)
{
    int t, i;
    uint32_t rng = 12345;

    for (t = 0; t < max_ms * 4; t++)
    {
        int done = 1;

        for (i = 0; i < PEERS; i++)
        {
            /* machines run at slightly different speeds */
            rng = rng * 1103515245u + 12345u;
            if ((rng >> 16) % 8 != 0)
            {
                step(i, stop_at);
            }
            if (sM[i].alive && sM[i].frames < stop_at && netplay_state(sM[i].np) == NETPLAY_RUNNING)
            {
                done = 0;
            }
        }
        if (done)
        {
            return;
        }
        if (t % 4 == 0)
        {
            nap();
        }
    }
}

static int check_inputs(int from, int to, unsigned dropped_mask, int dropped_from)
{
    int i, f, p, bad = 0;
    int delay = netplay_input_delay(sM[0].np);

    for (f = from; f < to && bad < 5; f++)
    {
        for (p = 0; p < NETPLAY_MAX_PLAYERS; p++)
        {
            NetplayPad want;

            memset(&want, 0, sizeof(want));
            if ((sM[0].mask & (1u << p)) && f >= delay && !((dropped_mask & (1u << p)) && f >= dropped_from))
            {
                want = input_of(p, f);
            }
            for (i = 0; i < PEERS; i++)
            {
                if (!sM[i].alive || sM[i].frames <= f)
                {
                    continue;
                }
                if ((dropped_mask & (1u << p)) && f >= dropped_from)
                {
                    /* after a drop: every machine must agree, whatever the host decided */
                    if (memcmp(&sM[i].log[f][p], &sM[0].log[f][p], sizeof(want)) != 0)
                    {
                        printf("FAIL: frame %d port %d: machine %d differs from the host after the drop\n", f, p, i);
                        bad++;
                    }
                }
                else if (memcmp(&sM[i].log[f][p], &want, sizeof(want)) != 0)
                {
                    printf("FAIL: frame %d port %d: machine %d ran %08X, expected %08X\n", f, p, i,
                           sM[i].log[f][p].buttons, want.buttons);
                    bad++;
                }
            }
        }
    }
    sFailures += bad;
    return bad == 0;
}

int main(int argc, char **argv)
{
    int loss = argc > 1 ? atoi(argv[1]) : 20;
    int base = argc > 2 ? atoi(argv[2]) : 27500;
    char address[32];
    static uint8_t save[40000];
    const uint8_t *got;
    size_t size;
    int i;

    for (i = 0; i < (int)sizeof(save); i++)
    {
        save[i] = (uint8_t)(i * 7 + (i >> 8));
    }

    /* ---- session 1: save sync, lockstep, desync ---- */
    printf("session 1: host + 2 clients, %d%% packet loss\n", loss);
    memset(sM, 0, sizeof(sM));
    for (i = 0; i < PEERS; i++)
    {
        NetplayConfig c = config(i == 0 ? "host" : i == 1 ? "client1" : "client2");

        sM[i].np = netplay_create(&c);
        sM[i].alive = 1;
        sM[i].corrupt_at = -1;
        sM[i].state = NETPLAY_HASH_SEED;
        netplay_debug_set_loss(sM[i].np, loss, 100 + i);
    }
    CHECK(netplay_host(sM[0].np, base), "host on port %d: %s", base, netplay_status_text(sM[0].np));
    netplay_set_save(sM[0].np, save, sizeof(save));
    snprintf(address, sizeof(address), "127.0.0.1:%d", base);
    CHECK(netplay_join(sM[1].np, address), "join");
    CHECK(netplay_join(sM[2].np, address), "join");
    CHECK(wait_until(clients_ready, 10000), "clients ready: %s / %s", netplay_status_text(sM[1].np),
          netplay_status_text(sM[2].np));
    for (i = 1; i < PEERS; i++)
    {
        got = netplay_save(sM[i].np, &size);
        CHECK(got != NULL && size == sizeof(save) && memcmp(got, save, size) == 0, "client %d save data", i);
    }
    CHECK(netplay_players(sM[0].np, (NetplayPlayerInfo[4]){0}, 4) == 3, "host sees 3 players");
    CHECK(wait_until(host_started, 5000), "start: %s", netplay_status_text(sM[0].np));
    run_session(FRAMES, 30000);
    for (i = 0; i < PEERS; i++)
    {
        CHECK(sM[i].frames == FRAMES, "machine %d ran %d frames (%s)", i, sM[i].frames, netplay_status_text(sM[i].np));
        CHECK(netplay_state(sM[i].np) == NETPLAY_RUNNING, "machine %d: %s", i, netplay_status_text(sM[i].np));
    }
    CHECK(sM[0].mask == 0x7, "ports in play: %X", sM[0].mask);
    if (check_inputs(0, FRAMES, 0, 0))
    {
        printf("  %d frames, identical inputs on all machines (stalls: %u %u %u)\n", FRAMES,
               netplay_stall_count(sM[0].np), netplay_stall_count(sM[1].np), netplay_stall_count(sM[2].np));
    }

    /* client 2's game diverges: the host notices within a hash interval and tells everyone */
    sM[2].corrupt_at = FRAMES + 30;
    run_session(FRAMES + 300, 10000);
    {
        int t;

        for (t = 0; t < 3000 && netplay_state(sM[1].np) != NETPLAY_DESYNC; t++)
        {
            poll_all();
            nap();
        }
    }
    CHECK(netplay_state(sM[0].np) == NETPLAY_DESYNC, "host desync: %s", netplay_status_text(sM[0].np));
    CHECK(netplay_state(sM[1].np) == NETPLAY_DESYNC, "client 1 told of the desync: %s", netplay_status_text(sM[1].np));
    printf("  desync caught: %s\n", netplay_status_text(sM[0].np));
    for (i = 0; i < PEERS; i++)
    {
        netplay_destroy(sM[i].np);
    }

    /* ---- session 2: a client vanishes ---- */
    printf("session 2: client 2 vanishes at frame 600\n");
    memset(sM, 0, sizeof(sM));
    for (i = 0; i < PEERS; i++)
    {
        NetplayConfig c = config(i == 0 ? "host" : "client");

        sM[i].np = netplay_create(&c);
        sM[i].alive = 1;
        sM[i].corrupt_at = -1;
        sM[i].state = NETPLAY_HASH_SEED;
        netplay_debug_set_loss(sM[i].np, loss, 200 + i);
    }
    CHECK(netplay_host(sM[0].np, base + 1), "host");
    snprintf(address, sizeof(address), "127.0.0.1:%d", base + 1);
    netplay_join(sM[1].np, address);
    netplay_join(sM[2].np, address);
    CHECK(wait_until(clients_ready, 10000), "clients ready");
    CHECK(wait_until(host_started, 5000), "start: %s", netplay_status_text(sM[0].np));
    run_session(600, 20000);
    sM[2].alive = 0; /* gone without a goodbye */
    run_session(1500, 20000);
    CHECK(sM[0].frames == 1500 && sM[1].frames == 1500, "host and client 1 played on: %d %d (%s)", sM[0].frames,
          sM[1].frames, netplay_status_text(sM[0].np));
    {
        int f, dropped_from = -1;

        for (f = 600; f < 1500; f++)
        {
            if (sM[0].log[f][2].buttons == 0 && sM[0].log[f][2].axis[0] == 0 && sM[0].log[f][2].axis[1] == 0 &&
                sM[0].log[f + 1][2].buttons == 0)
            {
                dropped_from = f;
                break;
            }
        }
        CHECK(dropped_from > 0, "port 3 went idle after the drop");
        check_inputs(0, 1500, 1u << 2, dropped_from > 0 ? dropped_from : 600);
        if (dropped_from > 0)
        {
            NetplayPlayerInfo players[4];
            int n = netplay_players(sM[0].np, players, 4), k, gone = 0;

            for (k = 0; k < n; k++)
            {
                gone += players[k].port == 2 && !players[k].connected;
            }
            CHECK(gone == 1, "the host lists player 3 as gone");
            printf("  player 3 dropped; idle from frame %d; host and client 1 still identical\n", dropped_from);
        }
    }
    for (i = 0; i < PEERS; i++)
    {
        netplay_destroy(sM[i].np);
    }

    printf(sFailures ? "FAILED (%d)\n" : "all passed\n", sFailures);
    return sFailures ? 1 : 0;
}
