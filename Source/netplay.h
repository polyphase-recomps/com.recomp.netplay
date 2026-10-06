/*
 * com.recomp.netplay: networked multiplayer for natively compiled / recompiled console games.
 *
 * Lockstep input sync, the way N64 / PS1 netplay is normally done: the games are deterministic
 * (their clocks come from the frame counter, and the same code gives the same results), so the
 * only thing the machines exchange is controller input. Frame N runs once the input of every
 * player for frame N is known; each player's input is scheduled `delay` frames ahead, which hides
 * the network latency (2-3 frames on a LAN, more over the internet).
 *
 * Topology: one host, up to 3 clients, connected to the host only (a star). The host collects
 * everyone's input, and sends each completed frame (all controller ports) to every client, so all
 * machines run the same inputs even when a player drops (their port then reads as idle).
 *
 * Joining: by the host's IP:port, or found on the LAN (UDP broadcast to the default port).
 * Before the game starts the host sends its save data to every client (games keep unlocks and
 * options there, which change how they play), and every machine boots the game at frame 0.
 * Every NETPLAY_HASH_INTERVAL frames each machine hashes its game state (the game picks what:
 * e.g. the rendered frame); a mismatch stops the session with a message (a "desync").
 *
 * Plain C99 with the OS's sockets (Winsock 2, BSD sockets on Linux / macOS); everything on the
 * wire is little-endian, so machines of any kind play together. No threads: call netplay_poll()
 * (or netplay_frame()) often. One Netplay* is one machine's session.
 */
#ifndef NETPLAY_H
#define NETPLAY_H

#include <stddef.h>
#include <stdint.h>

/* In the editor every addon is its own DLL: com.recomp.netplay exports these, the game addons
 * that use it import them. Shipped builds compile all addons into one program (and the tests
 * just compile netplay.c), so the macro is empty there. */
#ifndef NETPLAY_API
#if defined(EDITOR) && EDITOR && defined(_WIN32)
#if defined(NETPLAY_EXPORTS)
#define NETPLAY_API __declspec(dllexport)
#else
#define NETPLAY_API __declspec(dllimport)
#endif
#else
#define NETPLAY_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define NETPLAY_PROTOCOL 1
#define NETPLAY_MAX_PLAYERS 4
#define NETPLAY_DEFAULT_PORT 27464 /* UDP; LAN discovery looks for hosts on this port */
#define NETPLAY_HASH_INTERVAL 60   /* frames between state hash checks */
#define NETPLAY_MAX_DELAY 15
#define NETPLAY_MAX_SAVE (512 * 1024)

/* One controller port's input for one frame. Buttons are the console's own mask (N64: the
 * 16-bit libultra mask; PS1: the pad's 16 bits); axes are sticks (N64: x, y; PS1: lx, ly, rx, ry). */
typedef struct NetplayPad
{
    uint32_t buttons;
    int8_t axis[4];
} NetplayPad;

typedef enum NetplayState
{
    NETPLAY_IDLE,         /* no session */
    NETPLAY_LOBBY,        /* hosting, waiting for players (and for netplay_start) */
    NETPLAY_JOINING,      /* client: asking the host to let us in */
    NETPLAY_SYNCING,      /* client: receiving the host's save data */
    NETPLAY_READY,        /* client: in the lobby, waiting for the host to start */
    NETPLAY_RUNNING,      /* the game is on: use netplay_frame() */
    NETPLAY_DESYNC,       /* the machines' games diverged; the session is over */
    NETPLAY_DISCONNECTED, /* the host left, or we were refused / timed out */
    NETPLAY_ERROR         /* could not open the socket (port in use, no network) */
} NetplayState;

/* What a machine plays: sessions only form between machines whose game_id and game_version
 * match (version: e.g. the ROM's sha1). flavor is informational (e.g. "recomp", "decomp"):
 * different flavors can play together when they compute identical frames, which the hash check
 * verifies; netplay_peer_flavor_differs() tells the UI to warn. */
typedef struct NetplayConfig
{
    const char *game_id;      /* <= 15 chars, e.g. "ssb64" */
    const char *game_version; /* <= 47 chars, e.g. the ROM sha1 */
    const char *game_name;    /* <= 31 chars, shown in LAN listings */
    const char *flavor;       /* <= 15 chars */
    const char *player_name;  /* <= 31 chars; empty: the machine's name */
    int max_players;          /* host: 2..4 (0: 4) */
    int input_delay;          /* host: frames, 1..NETPLAY_MAX_DELAY (0: 2) */
    int timeout_ms;           /* a peer silent this long is gone (0: 5000) */
} NetplayConfig;

typedef struct NetplayPlayerInfo
{
    int port;        /* controller port 0..3 */
    int ready;       /* has the save data and is waiting for the start */
    int connected;   /* still in the session (a dropped player's port reads as idle) */
    int rtt_ms;      /* round trip to the host (host: to this client; -1 unknown) */
    int is_local;
    char name[32];
} NetplayPlayerInfo;

typedef struct NetplayFoundHost
{
    char address[64]; /* "a.b.c.d:port" */
    char game_name[32];
    char host_name[32];
    int players, max_players;
    int in_game;      /* already running: cannot be joined */
    int compatible;   /* same game_id / game_version as ours */
} NetplayFoundHost;

typedef struct Netplay Netplay;

NETPLAY_API Netplay *netplay_create(const NetplayConfig *config);
NETPLAY_API void netplay_destroy(Netplay *np); /* says goodbye to the peers */

/* Host on a UDP port (0: NETPLAY_DEFAULT_PORT). The host is player 1 (port 0). */
NETPLAY_API int netplay_host(Netplay *np, int udp_port);
/* Join "host", "host:port" or "a.b.c.d:port" (default port when none is given). */
NETPLAY_API int netplay_join(Netplay *np, const char *address);
/* Host: the save data sent to everyone before the start (copied; may be empty). */
NETPLAY_API void netplay_set_save(Netplay *np, const void *data, size_t size);
/* Host: start the game when every joined client is READY. Returns 0 (and does nothing) if one
 * is not ready yet or nobody joined. */
NETPLAY_API int netplay_start(Netplay *np);
/* Leave the session (back to NETPLAY_IDLE). */
NETPLAY_API void netplay_stop(Netplay *np);

/* Receive and send; call every engine tick (netplay_frame calls it too). */
NETPLAY_API void netplay_poll(Netplay *np);

/* LAN discovery: broadcast now and every second while searching; netplay_found() lists the
 * hosts that answered (heard from in the last 3 seconds). Works while idle. */
NETPLAY_API void netplay_search(Netplay *np, int enable);
NETPLAY_API int netplay_found(Netplay *np, NetplayFoundHost *out, int max);

/* Running: once, when the session starts. The game must then (re)boot from frame 0 with the
 * session's save data (netplay_save) and its controller ports set from netplay_frame(). */
NETPLAY_API int netplay_take_start(Netplay *np);
/* The save data the session plays with (host: its own; client: the host's, once SYNCING ends). */
NETPLAY_API const uint8_t *netplay_save(Netplay *np, size_t *size);

/* The frame gate. Call before each game frame with this machine's controller input: returns 1
 * and fills out[NETPLAY_MAX_PLAYERS] with every port's input for this frame (run it), or 0 when
 * a player's input has not arrived yet (do not run a frame; try again next tick). Ports nobody
 * plays are idle; `connected` in out_mask (bit per port) says which ports have a player. */
NETPLAY_API int netplay_frame(Netplay *np, const NetplayPad *local, NetplayPad out[NETPLAY_MAX_PLAYERS], unsigned *out_mask);
/* After running the frame netplay_frame allowed: the game state's hash (only every
 * NETPLAY_HASH_INTERVAL-th is used). Optional, but without it a desync goes unnoticed. */
NETPLAY_API void netplay_frame_done(Netplay *np, uint64_t state_hash);

NETPLAY_API NetplayState netplay_state(const Netplay *np);
NETPLAY_API int netplay_is_host(const Netplay *np);
NETPLAY_API int netplay_local_port(const Netplay *np);    /* this machine's controller port, -1 none */
NETPLAY_API int netplay_input_delay(const Netplay *np);
NETPLAY_API int netplay_current_frame(const Netplay *np); /* the next frame to run */
NETPLAY_API int netplay_players(const Netplay *np, NetplayPlayerInfo *out, int max);
NETPLAY_API int netplay_peer_flavor_differs(const Netplay *np);
NETPLAY_API uint32_t netplay_stall_count(const Netplay *np); /* netplay_frame calls that had to wait */
/* What is going on, for a status line ("Waiting for players (2/4)", "Desync at frame 1200", ...) */
NETPLAY_API const char *netplay_status_text(const Netplay *np);

/* Host: change the input delay before the start (1..NETPLAY_MAX_DELAY). */
NETPLAY_API void netplay_set_input_delay(Netplay *np, int frames);

/* 64-bit FNV-1a, for state hashes (e.g. of the frame the game drew). */
NETPLAY_API uint64_t netplay_hash(uint64_t seed, const void *data, size_t size);
#define NETPLAY_HASH_SEED 0xCBF29CE484222325ull

/* Tests: drop this share of outgoing packets (0..100), deterministically from seed. */
NETPLAY_API void netplay_debug_set_loss(Netplay *np, int percent, uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_H */
