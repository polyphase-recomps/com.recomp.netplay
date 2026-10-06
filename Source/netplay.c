/*
 * com.recomp.netplay core: see netplay.h for the model.
 *
 * Wire format (UDP, little-endian), every packet:
 *   "NPLY" u8 protocol u8 type u16 0  u32 session  u32 t  u32 echo  u16 hold  u16 0   payload
 * t: the sender's clock (ms); echo/hold: the last t it received from us and how long it held it,
 * so every packet measures the round trip. Input is sent with redundancy (every unacknowledged
 * frame, up to 32, in each packet), so lost packets cost nothing but a resend.
 */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET np_socket;
#define NP_BAD_SOCKET INVALID_SOCKET
#define np_close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
typedef int np_socket;
#define NP_BAD_SOCKET (-1)
#define np_close close
#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>
#endif

#include "netplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING 512           /* frames of input kept (a power of two) */
#define MAX_PER_PACKET 32  /* frames of input per packet */
#define SAVE_CHUNK 1024
#define SAVE_BURST 16      /* chunks sent per request */
#define HASH_SLOTS 32
#define MAX_FOUND 16
#define RESEND_MS 20       /* unacknowledged input is sent again this often */
#define LOBBY_MS 250       /* lobby keepalive / roster */
#define SEARCH_MS 1000
#define AUTO_DELAY_MARGIN_MS 8 /* auto input delay: on top of the round trip and its jitter */
#define AUTO_DELAY_MIN 2
#define AUTO_DELAY_MAX 8
#define BACKGROUND_WAIT_MS 2 /* the network thread looks at the socket at least this often */

enum
{
    PKT_DISCOVER = 1,
    PKT_ANNOUNCE,
    PKT_HELLO,
    PKT_WELCOME,
    PKT_REJECT,
    PKT_SAVE_REQ,
    PKT_SAVE_CHUNK,
    PKT_READY,
    PKT_ROSTER,
    PKT_START,
    PKT_START_ACK,
    PKT_INPUT,
    PKT_FRAMES,
    PKT_BYE
};

enum
{
    REJECT_GAME = 1,
    REJECT_VERSION,
    REJECT_FULL,
    REJECT_IN_GAME,
    REJECT_PROTOCOL
};

typedef struct Peer
{
    int used;
    struct sockaddr_in addr;
    uint32_t nonce;
    int port; /* controller port */
    int ready, connected, started;
    char name[32];
    uint64_t last_recv, last_send;
    uint32_t echo_t;     /* their last t, to echo */
    uint64_t echo_at;    /* when it arrived */
    int rtt;             /* -1 unknown */
    int rtt_dev;         /* its mean deviation (jitter) */
    int rtt_peak;        /* the highest recent round trip (decays slowly): Wi-Fi spikes */
    int32_t input_ack;   /* host: their input known up to here (contiguous) */
    int32_t frames_acked;/* host: they have the completed frames up to here */
    int32_t hash_frame[HASH_SLOTS];
    uint64_t hash_value[HASH_SLOTS];
} Peer;

struct Netplay
{
    char game_id[16], game_version[48], game_name[32], flavor[16], player_name[32];
    int max_players, delay, timeout_ms;

    np_socket sock;
    int is_host;
    NetplayState state;
    uint32_t session;
    uint32_t nonce;
    Peer peers[NETPLAY_MAX_PLAYERS]; /* host: clients, indexed by their port (1..3); client: [0] is the host */
    int local_port;
    int flavor_differs;
    char reason[96]; /* why we are DISCONNECTED / in ERROR */

    uint8_t *save;
    size_t save_size, save_received;
    uint32_t save_crc;
    uint64_t last_save_req;

    NetplayPlayerInfo roster[NETPLAY_MAX_PLAYERS]; /* client: the host's list */
    int roster_count;

    /* running */
    uint32_t start_token;
    int start_pending;
    unsigned port_mask, dropped_mask;
    int32_t cur_frame, local_recorded, complete_upto, host_input_ack;
    NetplayPad inputs[RING][NETPLAY_MAX_PLAYERS];
    uint8_t known[RING];
    int32_t slot_frame[RING];
    NetplayPad local_in[RING];
    int32_t own_hash_frame[HASH_SLOTS];
    uint64_t own_hash_value[HASH_SLOTS];
    int32_t last_hash_frame; /* client: the newest own hash, sent with every INPUT */
    uint64_t last_hash_value;
    int32_t desync_frame;
    uint32_t stalls;
    int dirty; /* something new to send */
    uint32_t recv_seq; /* packets received (netplay_wait) */

    /* netplay_set_background: a thread receives and answers as packets arrive */
#if defined(_WIN32)
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE received;
    HANDLE bg_thread;
#else
    pthread_mutex_t lock;
    pthread_cond_t received;
    pthread_t bg_thread;
#endif
    int bg_running;
    volatile int bg_quit;

    uint64_t last_lobby_send;
    int searching;
    uint64_t last_discover;
    NetplayFoundHost found[MAX_FOUND];
    uint64_t found_at[MAX_FOUND];
    int found_count;

    int loss;
    uint32_t loss_rng;
    int lag_ms, lag_jitter_ms;  /* netplay_debug_set_lag: outgoing packets held back */
    struct Lagged *lagged;
    int lagged_count, lagged_cap;
    char status[160];
};

typedef struct Lagged
{
    uint64_t due;
    struct sockaddr_in addr;
    size_t n;
    uint8_t buf[1400];
} Lagged;

static void np_lock(const Netplay *np);
static void np_unlock(const Netplay *np);

/* ---- platform -------------------------------------------------------------------------- */
static uint64_t now_ms(void)
{
#if defined(_WIN32)
    /* not GetTickCount64: it moves in ~15.6 ms steps (round trips read 0 or 16 ms) */
    static LARGE_INTEGER freq;
    LARGE_INTEGER count;

    if (freq.QuadPart == 0)
    {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart / freq.QuadPart) * 1000u +
           (uint64_t)((count.QuadPart % freq.QuadPart) * 1000 / freq.QuadPart);
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static int net_init(void)
{
#if defined(_WIN32)
    static int done;

    if (!done)
    {
        WSADATA wsa;

        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            return 0;
        }
        done = 1;
    }
#endif
    return 1;
}

static np_socket open_socket(int port, char *reason, size_t reason_size)
{
    np_socket s;
    struct sockaddr_in addr;
    int yes = 1;

    if (!net_init() || (s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == NP_BAD_SOCKET)
    {
        snprintf(reason, reason_size, "no network (cannot create a UDP socket)");
        return NP_BAD_SOCKET;
    }
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));
#if defined(_WIN32)
    {
        u_long nonblocking = 1;
        DWORD off = FALSE, got = 0;

        ioctlsocket(s, FIONBIO, &nonblocking);
        /* an ICMP "port unreachable" must not make the next recvfrom fail */
        WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof(off), NULL, 0, &got, NULL, NULL);
    }
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        snprintf(reason, reason_size, "UDP port %d is in use (another session or program)", port);
        np_close(s);
        return NP_BAD_SOCKET;
    }
    return s;
}

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void addr_text(const struct sockaddr_in *a, char *out, size_t size)
{
    const uint8_t *ip = (const uint8_t *)&a->sin_addr.s_addr;

    snprintf(out, size, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], ntohs(a->sin_port));
}

static int resolve(const char *text, struct sockaddr_in *out)
{
    char host[128];
    const char *colon = strrchr(text, ':');
    int port = NETPLAY_DEFAULT_PORT;
    struct addrinfo hints, *res = NULL;
    size_t n;

    if (!net_init())
    {
        return 0;
    }
    n = colon ? (size_t)(colon - text) : strlen(text);
    if (n == 0 || n >= sizeof(host))
    {
        return 0;
    }
    memcpy(host, text, n);
    host[n] = 0;
    if (colon)
    {
        port = atoi(colon + 1);
        if (port <= 0 || port > 65535)
        {
            return 0;
        }
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL)
    {
        return 0;
    }
    memcpy(out, res->ai_addr, sizeof(*out));
    out->sin_port = htons((unsigned short)port);
    freeaddrinfo(res);
    return 1;
}

static void machine_name(char *out, size_t size)
{
    out[0] = 0;
    if (net_init() && gethostname(out, (int)size) != 0)
    {
        out[0] = 0;
    }
    out[size - 1] = 0;
    if (out[0] == 0)
    {
        snprintf(out, size, "player");
    }
}

/* ---- serialization ------------------------------------------------------------------- */
typedef struct W
{
    uint8_t buf[1400];
    size_t n;
} W;

static void w8(W *w, uint32_t v) { if (w->n < sizeof(w->buf)) w->buf[w->n++] = (uint8_t)v; }
static void w16(W *w, uint32_t v) { w8(w, v); w8(w, v >> 8); }
static void w32(W *w, uint32_t v) { w16(w, v & 0xFFFF); w16(w, v >> 16); }
static void w64(W *w, uint64_t v) { w32(w, (uint32_t)v); w32(w, (uint32_t)(v >> 32)); }
static void wstr(W *w, const char *s, size_t len)
{
    size_t i, l = s ? strlen(s) : 0;

    for (i = 0; i < len; i++)
    {
        w8(w, (i < l && i < len - 1) ? (uint8_t)s[i] : 0);
    }
}
static void wpad(W *w, const NetplayPad *p)
{
    w32(w, p->buttons);
    w8(w, (uint8_t)p->axis[0]);
    w8(w, (uint8_t)p->axis[1]);
    w8(w, (uint8_t)p->axis[2]);
    w8(w, (uint8_t)p->axis[3]);
}

typedef struct R
{
    const uint8_t *p;
    size_t n, pos;
    int bad;
} R;

static uint32_t r8(R *r) { if (r->pos >= r->n) { r->bad = 1; return 0; } return r->p[r->pos++]; }
static uint32_t r16(R *r) { uint32_t a = r8(r); return a | (r8(r) << 8); }
static uint32_t r32(R *r) { uint32_t a = r16(r); return a | (r16(r) << 16); }
static uint64_t r64(R *r) { uint64_t a = r32(r); return a | ((uint64_t)r32(r) << 32); }
static void rstr(R *r, char *out, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        out[i] = (char)r8(r);
    }
    out[len - 1] = 0;
}
static void rpad(R *r, NetplayPad *p)
{
    p->buttons = r32(r);
    p->axis[0] = (int8_t)r8(r);
    p->axis[1] = (int8_t)r8(r);
    p->axis[2] = (int8_t)r8(r);
    p->axis[3] = (int8_t)r8(r);
}

static uint32_t crc32_of(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int k;

    for (i = 0; i < size; i++)
    {
        crc ^= data[i];
        for (k = 0; k < 8; k++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint64_t netplay_hash(uint64_t seed, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;

    for (i = 0; i < size; i++)
    {
        seed = (seed ^ p[i]) * 0x100000001B3ull;
    }
    return seed;
}

/* ---- sending ------------------------------------------------------------------------- */
static void begin(Netplay *np, W *w, int type, const Peer *to)
{
    uint64_t t = now_ms();

    w->n = 0;
    w8(w, 'N'); w8(w, 'P'); w8(w, 'L'); w8(w, 'Y');
    w8(w, NETPLAY_PROTOCOL);
    w8(w, (uint32_t)type);
    w16(w, 0);
    w32(w, np->session);
    w32(w, (uint32_t)t);
    w32(w, to ? to->echo_t : 0);
    w16(w, (to && to->echo_at) ? (uint32_t)(t - to->echo_at > 0xFFFF ? 0xFFFF : t - to->echo_at) : 0xFFFF);
    w16(w, 0);
}

static void send_to(Netplay *np, const W *w, const struct sockaddr_in *addr)
{
    if (np->sock == NP_BAD_SOCKET)
    {
        return;
    }
    if (np->loss > 0)
    {
        np->loss_rng = np->loss_rng * 1103515245u + 12345u;
        if ((int)((np->loss_rng >> 16) % 100) < np->loss)
        {
            return;
        }
    }
    if (np->lag_ms > 0 || np->lag_jitter_ms > 0)
    {
        Lagged *l;

        if (np->lagged_count == np->lagged_cap)
        {
            int cap = np->lagged_cap ? np->lagged_cap * 2 : 64;
            Lagged *grown = (Lagged *)realloc(np->lagged, (size_t)cap * sizeof(Lagged));

            if (grown == NULL)
            {
                return;
            }
            np->lagged = grown;
            np->lagged_cap = cap;
        }
        np->loss_rng = np->loss_rng * 1103515245u + 12345u;
        l = &np->lagged[np->lagged_count++];
        l->due = now_ms() + (uint64_t)np->lag_ms +
                 (np->lag_jitter_ms > 0 ? (uint64_t)((np->loss_rng >> 16) % (uint32_t)(np->lag_jitter_ms + 1)) : 0);
        l->addr = *addr;
        l->n = w->n;
        memcpy(l->buf, w->buf, w->n);
        return;
    }
    sendto(np->sock, (const char *)w->buf, (int)w->n, 0, (const struct sockaddr *)addr, sizeof(*addr));
}

/* netplay_debug_set_lag: sends the held-back packets that are due */
static void send_lagged(Netplay *np)
{
    uint64_t now;
    int i, kept = 0;

    if (np->lagged_count == 0)
    {
        return;
    }
    now = now_ms();
    for (i = 0; i < np->lagged_count; i++)
    {
        Lagged *l = &np->lagged[i];

        if (l->due <= now)
        {
            if (np->sock != NP_BAD_SOCKET)
            {
                sendto(np->sock, (const char *)l->buf, (int)l->n, 0, (const struct sockaddr *)&l->addr, sizeof(l->addr));
            }
        }
        else
        {
            if (kept != i)
            {
                np->lagged[kept] = *l;
            }
            kept++;
        }
    }
    np->lagged_count = kept;
}

static void send_peer(Netplay *np, W *w, Peer *peer)
{
    send_to(np, w, &peer->addr);
    peer->last_send = now_ms();
}

/* ---- input ring ---------------------------------------------------------------------- */
static int slot_of(int32_t frame)
{
    return (int)((uint32_t)frame & (RING - 1));
}

static void slot_claim(Netplay *np, int32_t frame)
{
    int s = slot_of(frame);

    if (np->slot_frame[s] != frame)
    {
        np->slot_frame[s] = frame;
        np->known[s] = 0;
        memset(np->inputs[s], 0, sizeof(np->inputs[s]));
    }
}

static int frame_known(const Netplay *np, int32_t frame, int port)
{
    int s = slot_of(frame);

    return np->slot_frame[s] == frame && (np->known[s] & (1u << port));
}

static void set_input(Netplay *np, int32_t frame, int port, const NetplayPad *pad)
{
    int s;

    slot_claim(np, frame);
    s = slot_of(frame);
    np->inputs[s][port] = *pad;
    np->known[s] |= (uint8_t)(1u << port);
}

/* Host: complete every frame whose players' input is all here (a dropped player is idle). */
static void host_advance(Netplay *np)
{
    for (;;)
    {
        int32_t f = np->complete_upto + 1;
        unsigned need = np->port_mask, p;
        int s = slot_of(f);

        if (f > np->local_recorded)
        {
            break;
        }
        slot_claim(np, f);
        for (p = 0; p < NETPLAY_MAX_PLAYERS; p++)
        {
            if ((need & (1u << p)) && (np->dropped_mask & (1u << p)) && !(np->known[s] & (1u << p)))
            {
                NetplayPad idle;

                memset(&idle, 0, sizeof(idle));
                set_input(np, f, (int)p, &idle);
            }
        }
        if ((np->known[s] & need) != need)
        {
            break;
        }
        np->complete_upto = f;
        np->dirty = 1;
    }
}

static void start_running(Netplay *np, uint32_t token, int delay, unsigned mask)
{
    int32_t f;

    np->start_token = token;
    np->delay = delay;
    np->port_mask = mask;
    np->dropped_mask = 0;
    np->cur_frame = 0;
    np->local_recorded = delay - 1;
    np->complete_upto = delay - 1;
    np->host_input_ack = delay - 1;
    np->last_hash_frame = -1;
    np->desync_frame = -1;
    np->stalls = 0;
    memset(np->known, 0, sizeof(np->known));
    for (f = 0; f < RING; f++)
    {
        np->slot_frame[f] = -1;
        np->own_hash_frame[f % HASH_SLOTS] = -1;
    }
    /* the first `delay` frames have no input from anyone */
    for (f = 0; f < delay; f++)
    {
        slot_claim(np, f);
        np->known[slot_of(f)] = 0xFF;
    }
    for (f = 0; f < NETPLAY_MAX_PLAYERS; f++)
    {
        int k;

        np->peers[f].input_ack = delay - 1;
        np->peers[f].frames_acked = delay - 1;
        np->peers[f].started = 0;
        for (k = 0; k < HASH_SLOTS; k++)
        {
            np->peers[f].hash_frame[k] = -1;
        }
    }
    np->state = NETPLAY_RUNNING;
    np->start_pending = 1;
    np->dirty = 1;
}

/* ---- the host's sends ---------------------------------------------------------------- */
static void host_send_roster(Netplay *np)
{
    int i;
    W w;

    for (i = 1; i < NETPLAY_MAX_PLAYERS; i++)
    {
        Peer *to = &np->peers[i];
        int j, count = 1;

        if (!to->used)
        {
            continue;
        }
        for (j = 1; j < NETPLAY_MAX_PLAYERS; j++)
        {
            count += np->peers[j].used;
        }
        begin(np, &w, PKT_ROSTER, to);
        w8(&w, (uint32_t)count);
        w8(&w, (uint32_t)np->delay);
        w8(&w, (uint32_t)np->max_players);
        w8(&w, (uint32_t)np->state);
        /* the host itself, then the clients */
        w8(&w, (uint32_t)np->local_port);
        w8(&w, 1);
        w8(&w, 1);
        w16(&w, 0);
        wstr(&w, np->player_name, 32);
        for (j = 1; j < NETPLAY_MAX_PLAYERS; j++)
        {
            const Peer *p = &np->peers[j];

            if (p->used)
            {
                w8(&w, (uint32_t)p->port);
                w8(&w, (uint32_t)p->ready);
                w8(&w, (uint32_t)p->connected);
                w16(&w, (uint32_t)(p->rtt < 0 ? 0xFFFF : p->rtt));
                wstr(&w, p->name, 32);
            }
        }
        send_peer(np, &w, to);
    }
}

static void host_send_frames(Netplay *np, Peer *to, int force)
{
    int32_t first = to->frames_acked + 1, f;
    int count;
    W w;

    if (first > np->complete_upto && !force)
    {
        return;
    }
    count = (int)(np->complete_upto - first + 1);
    if (count < 0)
    {
        count = 0;
    }
    if (count > MAX_PER_PACKET)
    {
        count = MAX_PER_PACKET;
    }
    begin(np, &w, PKT_FRAMES, to);
    w32(&w, (uint32_t)to->input_ack);
    w32(&w, (uint32_t)np->desync_frame);
    w32(&w, (uint32_t)first);
    w8(&w, (uint32_t)count);
    for (f = first; f < first + count; f++)
    {
        int p;

        for (p = 0; p < NETPLAY_MAX_PLAYERS; p++)
        {
            wpad(&w, &np->inputs[slot_of(f)][p]);
        }
    }
    send_peer(np, &w, to);
}

static void host_send_start(Netplay *np, Peer *to)
{
    W w;

    begin(np, &w, PKT_START, to);
    w32(&w, np->start_token);
    w8(&w, (uint32_t)np->delay);
    w8(&w, np->port_mask);
    send_peer(np, &w, to);
}

static void client_send_input(Netplay *np)
{
    Peer *host = &np->peers[0];
    int32_t first = np->host_input_ack + 1, f;
    int count = (int)(np->local_recorded - first + 1);
    W w;

    if (count < 0)
    {
        count = 0;
    }
    if (count > MAX_PER_PACKET)
    {
        count = MAX_PER_PACKET;
    }
    begin(np, &w, PKT_INPUT, host);
    w8(&w, (uint32_t)np->local_port);
    w32(&w, (uint32_t)np->complete_upto);
    w32(&w, (uint32_t)np->last_hash_frame);
    w64(&w, np->last_hash_value);
    w32(&w, (uint32_t)first);
    w8(&w, (uint32_t)count);
    for (f = first; f < first + count; f++)
    {
        wpad(&w, &np->local_in[slot_of(f)]);
    }
    send_peer(np, &w, host);
}

/* ---- session bookkeeping --------------------------------------------------------------- */
static void set_reason(Netplay *np, NetplayState state, const char *text)
{
    np->state = state;
    snprintf(np->reason, sizeof(np->reason), "%s", text);
}

static void host_drop(Netplay *np, Peer *p, const char *why)
{
    (void)why;
    if (np->state == NETPLAY_RUNNING)
    {
        /* their port is idle from now on (decided here, so every machine plays the same) */
        np->dropped_mask |= 1u << p->port;
        p->connected = 0;
        host_advance(np);
    }
    else
    {
        memset(p, 0, sizeof(*p));
    }
}

static void host_check_hash(Netplay *np, Peer *p, int32_t frame, uint64_t hash)
{
    int s = (int)(((uint32_t)frame / NETPLAY_HASH_INTERVAL) % HASH_SLOTS);

    if (frame < 0 || np->desync_frame >= 0)
    {
        return;
    }
    if (np->own_hash_frame[s] == frame)
    {
        if (np->own_hash_value[s] != hash)
        {
            np->desync_frame = frame;
            np->state = NETPLAY_DESYNC;
            np->dirty = 1;
        }
        return;
    }
    p->hash_frame[s] = frame;
    p->hash_value[s] = hash;
}

/* ---- receiving ------------------------------------------------------------------------- */
static Peer *peer_by_addr(Netplay *np, const struct sockaddr_in *from)
{
    int i;

    for (i = 0; i < NETPLAY_MAX_PLAYERS; i++)
    {
        if (np->peers[i].used && same_addr(&np->peers[i].addr, from))
        {
            return &np->peers[i];
        }
    }
    return NULL;
}

static void note_timing(Peer *p, uint32_t t, uint32_t echo, uint32_t hold)
{
    uint64_t now = now_ms();

    p->last_recv = now;
    p->echo_t = t;
    p->echo_at = now;
    if (echo != 0 && hold != 0xFFFF)
    {
        int rtt = (int)((uint32_t)now - echo) - (int)hold;

        if (rtt >= 0 && rtt < 10000)
        {
            p->rtt_peak = rtt > p->rtt_peak ? rtt : p->rtt_peak - (p->rtt_peak - rtt) / 16;
            if (p->rtt < 0)
            {
                p->rtt = rtt;
                p->rtt_dev = 0;
            }
            else
            {
                const int dev = rtt > p->rtt ? rtt - p->rtt : p->rtt - rtt;

                p->rtt_dev = (p->rtt_dev * 3 + dev) / 4;
                p->rtt = (p->rtt * 7 + rtt) / 8;
            }
        }
    }
}

static void on_discover(Netplay *np, R *r, const struct sockaddr_in *from)
{
    char game[16];
    int i, players = 1;
    W w;

    rstr(r, game, sizeof(game));
    if (!np->is_host || (np->state != NETPLAY_LOBBY && np->state != NETPLAY_RUNNING))
    {
        return;
    }
    for (i = 1; i < NETPLAY_MAX_PLAYERS; i++)
    {
        players += np->peers[i].used;
    }
    begin(np, &w, PKT_ANNOUNCE, NULL);
    wstr(&w, np->game_id, 16);
    wstr(&w, np->game_version, 48);
    wstr(&w, np->game_name, 32);
    wstr(&w, np->player_name, 32);
    w8(&w, (uint32_t)players);
    w8(&w, (uint32_t)np->max_players);
    w8(&w, np->state == NETPLAY_RUNNING);
    send_to(np, &w, from);
}

static void on_announce(Netplay *np, R *r, const struct sockaddr_in *from)
{
    NetplayFoundHost h;
    char version[48], id[16];
    int i;

    memset(&h, 0, sizeof(h));
    rstr(r, id, sizeof(id));
    rstr(r, version, sizeof(version));
    rstr(r, h.game_name, sizeof(h.game_name));
    rstr(r, h.host_name, sizeof(h.host_name));
    h.players = (int)r8(r);
    h.max_players = (int)r8(r);
    h.in_game = (int)r8(r);
    if (r->bad)
    {
        return;
    }
    h.compatible = strcmp(id, np->game_id) == 0 && strcmp(version, np->game_version) == 0;
    addr_text(from, h.address, sizeof(h.address));
    for (i = 0; i < np->found_count; i++)
    {
        if (strcmp(np->found[i].address, h.address) == 0)
        {
            break;
        }
    }
    if (i == np->found_count)
    {
        if (np->found_count == MAX_FOUND)
        {
            return;
        }
        np->found_count++;
    }
    np->found[i] = h;
    np->found_at[i] = now_ms();
}

static void on_hello(Netplay *np, R *r, const struct sockaddr_in *from, uint32_t t, uint32_t echo, uint32_t hold)
{
    char id[16], version[48], flavor[16], name[32];
    uint32_t nonce;
    int reason = 0, i;
    Peer *p;
    W w;

    rstr(r, id, sizeof(id));
    rstr(r, version, sizeof(version));
    rstr(r, flavor, sizeof(flavor));
    rstr(r, name, sizeof(name));
    nonce = r32(r);
    if (r->bad || !np->is_host)
    {
        return;
    }
    p = peer_by_addr(np, from);
    if (p == NULL)
    {
        if (strcmp(id, np->game_id) != 0)
        {
            reason = REJECT_GAME;
        }
        else if (strcmp(version, np->game_version) != 0)
        {
            reason = REJECT_VERSION;
        }
        else if (np->state != NETPLAY_LOBBY)
        {
            reason = REJECT_IN_GAME;
        }
        else
        {
            for (i = 1; i < np->max_players; i++)
            {
                if (!np->peers[i].used)
                {
                    break;
                }
            }
            if (i >= np->max_players)
            {
                reason = REJECT_FULL;
            }
            else
            {
                p = &np->peers[i];
                memset(p, 0, sizeof(*p));
                p->used = 1;
                p->addr = *from;
                p->nonce = nonce;
                p->port = i;
                p->connected = 1;
                p->rtt = -1;
                p->rtt_dev = 0;
                p->rtt_peak = 0;
                snprintf(p->name, sizeof(p->name), "%s", name);
                if (strcmp(flavor, np->flavor) != 0)
                {
                    np->flavor_differs = 1;
                }
            }
        }
    }
    if (reason != 0)
    {
        static const char *const text[] = {"", "a different game", "a different version of the game (another ROM)",
                                           "the session is full", "the game has already started", "another netplay version"};

        begin(np, &w, PKT_REJECT, NULL);
        w32(&w, nonce);
        w8(&w, (uint32_t)reason);
        wstr(&w, text[reason], 64);
        send_to(np, &w, from);
        return;
    }
    note_timing(p, t, echo, hold);
    begin(np, &w, PKT_WELCOME, p);
    w32(&w, nonce);
    w8(&w, (uint32_t)p->port);
    w8(&w, (uint32_t)np->max_players);
    w8(&w, (uint32_t)np->delay);
    w32(&w, (uint32_t)np->save_size);
    w32(&w, np->save_crc);
    wstr(&w, np->flavor, 16);
    send_peer(np, &w, p);
    host_send_roster(np);
}

static void on_welcome(Netplay *np, R *r, uint32_t session)
{
    uint32_t nonce = r32(r), size, crc;
    int port = (int)r8(r), maxp = (int)r8(r), delay = (int)r8(r);
    char flavor[16];

    size = r32(r);
    crc = r32(r);
    rstr(r, flavor, sizeof(flavor));
    if (r->bad || np->state != NETPLAY_JOINING || nonce != np->nonce || size > NETPLAY_MAX_SAVE ||
        port >= NETPLAY_MAX_PLAYERS)
    {
        return;
    }
    np->session = session;
    np->local_port = port;
    np->max_players = maxp;
    np->delay = delay;
    np->flavor_differs = strcmp(flavor, np->flavor) != 0;
    free(np->save);
    np->save = NULL;
    np->save_size = size;
    np->save_received = 0;
    np->save_crc = crc;
    if (size > 0)
    {
        np->save = (uint8_t *)malloc(size);
        if (np->save == NULL)
        {
            set_reason(np, NETPLAY_ERROR, "out of memory for the save data");
            return;
        }
        np->state = NETPLAY_SYNCING;
        np->last_save_req = 0;
    }
    else
    {
        np->state = NETPLAY_READY;
    }
}

static void on_save_req(Netplay *np, Peer *p, R *r)
{
    uint32_t offset = r32(r);
    int k;
    W w;

    if (r->bad || !np->is_host)
    {
        return;
    }
    for (k = 0; k < SAVE_BURST && offset < np->save_size; k++)
    {
        uint32_t len = (uint32_t)(np->save_size - offset);
        uint32_t i;

        if (len > SAVE_CHUNK)
        {
            len = SAVE_CHUNK;
        }
        begin(np, &w, PKT_SAVE_CHUNK, p);
        w32(&w, offset);
        w16(&w, len);
        for (i = 0; i < len; i++)
        {
            w8(&w, np->save[offset + i]);
        }
        send_peer(np, &w, p);
        offset += len;
    }
}

static void on_save_chunk(Netplay *np, R *r)
{
    uint32_t offset = r32(r), len = r16(r), i;

    if (r->bad || np->state != NETPLAY_SYNCING || offset != np->save_received || offset + len > np->save_size)
    {
        return;
    }
    for (i = 0; i < len; i++)
    {
        np->save[offset + i] = (uint8_t)r8(r);
    }
    if (r->bad)
    {
        return;
    }
    np->save_received += len;
    np->last_save_req = 0; /* ask for the next part right away */
    if (np->save_received == np->save_size)
    {
        if (crc32_of(np->save, np->save_size) != np->save_crc)
        {
            np->save_received = 0; /* corrupted on the way: again */
            return;
        }
        np->state = NETPLAY_READY;
        np->last_lobby_send = 0;
    }
}

static void on_roster(Netplay *np, R *r)
{
    int count = (int)r8(r), i;
    NetplayPlayerInfo list[NETPLAY_MAX_PLAYERS];

    np->delay = (int)r8(r);
    np->max_players = (int)r8(r);
    r8(r); /* host state */
    if (count > NETPLAY_MAX_PLAYERS)
    {
        return;
    }
    for (i = 0; i < count; i++)
    {
        uint32_t rtt;

        memset(&list[i], 0, sizeof(list[i]));
        list[i].port = (int)r8(r);
        list[i].ready = (int)r8(r);
        list[i].connected = (int)r8(r);
        rtt = r16(r);
        list[i].rtt_ms = rtt == 0xFFFF ? -1 : (int)rtt;
        rstr(r, list[i].name, sizeof(list[i].name));
        list[i].is_local = list[i].port == np->local_port;
    }
    if (!r->bad)
    {
        memcpy(np->roster, list, sizeof(list));
        np->roster_count = count;
    }
}

static void on_input(Netplay *np, Peer *p, R *r)
{
    int port = (int)r8(r);
    int32_t ack = (int32_t)r32(r), hash_frame = (int32_t)r32(r), first, f;
    uint64_t hash = r64(r);
    int count;

    first = (int32_t)r32(r);
    count = (int)r8(r);
    if (r->bad || port != p->port || count > MAX_PER_PACKET)
    {
        return;
    }
    p->started = 1;
    if (ack > p->frames_acked)
    {
        p->frames_acked = ack;
    }
    for (f = first; f < first + count; f++)
    {
        NetplayPad pad;

        rpad(r, &pad);
        if (r->bad)
        {
            return;
        }
        /* only frames still to complete, and not absurdly far ahead */
        if (f > np->complete_upto && f <= np->complete_upto + RING / 2 && !frame_known(np, f, port) &&
            !(np->dropped_mask & (1u << port)))
        {
            set_input(np, f, port, &pad);
        }
    }
    while (frame_known(np, p->input_ack + 1, port) || p->input_ack + 1 <= np->complete_upto)
    {
        p->input_ack++;
    }
    host_check_hash(np, p, hash_frame, hash);
    host_advance(np);
}

static void on_frames(Netplay *np, R *r)
{
    int32_t ack = (int32_t)r32(r), desync = (int32_t)r32(r), first = (int32_t)r32(r), f;
    int count = (int)r8(r);

    if (r->bad || count > MAX_PER_PACKET)
    {
        return;
    }
    if (ack > np->host_input_ack)
    {
        np->host_input_ack = ack;
    }
    for (f = first; f < first + count; f++)
    {
        NetplayPad pads[NETPLAY_MAX_PLAYERS];
        int p;

        for (p = 0; p < NETPLAY_MAX_PLAYERS; p++)
        {
            rpad(r, &pads[p]);
        }
        if (r->bad)
        {
            return;
        }
        if (f > np->complete_upto && f <= np->cur_frame + RING / 2)
        {
            int s;

            slot_claim(np, f);
            s = slot_of(f);
            memcpy(np->inputs[s], pads, sizeof(pads));
            np->known[s] = 0xFF;
        }
    }
    while (np->slot_frame[slot_of(np->complete_upto + 1)] == np->complete_upto + 1 &&
           np->known[slot_of(np->complete_upto + 1)] == 0xFF)
    {
        np->complete_upto++;
    }
    if (desync >= 0 && np->state == NETPLAY_RUNNING)
    {
        np->desync_frame = desync;
        np->state = NETPLAY_DESYNC;
    }
}

static void receive_all(Netplay *np)
{
    uint8_t buf[2048];

    if (np->sock == NP_BAD_SOCKET)
    {
        return;
    }
    for (;;)
    {
        struct sockaddr_in from;
#if defined(_WIN32)
        int fromlen = sizeof(from);
#else
        socklen_t fromlen = sizeof(from);
#endif
        int n = (int)recvfrom(np->sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        R r;
        int type;
        uint32_t session, t, echo, hold;
        Peer *p;

        if (n < 0)
        {
#if defined(_WIN32)
            if (WSAGetLastError() == WSAEWOULDBLOCK)
                break;
            continue; /* e.g. a reset from a gone peer */
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR || errno == ECONNREFUSED)
                continue;
            break;
#endif
        }
        np->recv_seq++;
        if (n < 24 || memcmp(buf, "NPLY", 4) != 0)
        {
            continue;
        }
        r.p = buf;
        r.n = (size_t)n;
        r.pos = 4;
        r.bad = 0;
        if (r8(&r) != NETPLAY_PROTOCOL)
        {
            continue;
        }
        type = (int)r8(&r);
        r16(&r);
        session = r32(&r);
        t = r32(&r);
        echo = r32(&r);
        hold = r16(&r);
        r16(&r);

        if (type == PKT_DISCOVER)
        {
            on_discover(np, &r, &from);
            continue;
        }
        if (type == PKT_ANNOUNCE)
        {
            on_announce(np, &r, &from);
            continue;
        }
        if (np->is_host)
        {
            if (type == PKT_HELLO)
            {
                on_hello(np, &r, &from, t, echo, hold);
                continue;
            }
            p = peer_by_addr(np, &from);
            if (p == NULL || session != np->session)
            {
                continue;
            }
            note_timing(p, t, echo, hold);
            switch (type)
            {
            case PKT_SAVE_REQ:
                on_save_req(np, p, &r);
                break;
            case PKT_READY:
                p->ready = 1;
                break;
            case PKT_START_ACK:
                p->started = 1;
                break;
            case PKT_INPUT:
                if (np->state == NETPLAY_RUNNING || np->state == NETPLAY_DESYNC)
                {
                    on_input(np, p, &r);
                }
                break;
            case PKT_BYE:
                host_drop(np, p, "left");
                break;
            default:
                break;
            }
            continue;
        }

        /* client: only the host talks to us */
        p = &np->peers[0];
        if (!p->used || !same_addr(&p->addr, &from))
        {
            continue;
        }
        if (type == PKT_WELCOME)
        {
            note_timing(p, t, echo, hold);
            on_welcome(np, &r, session);
            continue;
        }
        if (type == PKT_REJECT)
        {
            char text[64];

            r32(&r);
            r8(&r);
            rstr(&r, text, sizeof(text));
            if (np->state == NETPLAY_JOINING)
            {
                char why[96];

                snprintf(why, sizeof(why), "refused by the host: %s", text);
                set_reason(np, NETPLAY_DISCONNECTED, why);
            }
            continue;
        }
        if (session != np->session)
        {
            continue;
        }
        note_timing(p, t, echo, hold);
        switch (type)
        {
        case PKT_SAVE_CHUNK:
            on_save_chunk(np, &r);
            break;
        case PKT_ROSTER:
            on_roster(np, &r);
            break;
        case PKT_START:
        {
            uint32_t token = r32(&r);
            int delay = (int)r8(&r);
            unsigned mask = r8(&r);
            W w;

            if (!r.bad && np->state == NETPLAY_READY && delay >= 1 && delay <= NETPLAY_MAX_DELAY)
            {
                start_running(np, token, delay, mask);
            }
            if (!r.bad && token == np->start_token && np->state != NETPLAY_READY)
            {
                begin(np, &w, PKT_START_ACK, p);
                send_peer(np, &w, p);
            }
            break;
        }
        case PKT_FRAMES:
            if (np->state == NETPLAY_RUNNING)
            {
                on_frames(np, &r);
            }
            break;
        case PKT_BYE:
            set_reason(np, NETPLAY_DISCONNECTED, "the host left the session");
            break;
        default:
            break;
        }
    }
}

/* ---- periodic work ------------------------------------------------------------------------ */
static void send_discover(Netplay *np)
{
    struct sockaddr_in to;
    W w;

    begin(np, &w, PKT_DISCOVER, NULL);
    wstr(&w, np->game_id, 16);
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(NETPLAY_DEFAULT_PORT);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    send_to(np, &w, &to);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* a host on this machine */
    send_to(np, &w, &to);
}

static void tick(Netplay *np)
{
    uint64_t now = now_ms();
    int i;

    send_lagged(np);
    if (np->searching && now - np->last_discover >= SEARCH_MS)
    {
        np->last_discover = now;
        send_discover(np);
    }
    for (i = 0; i < np->found_count; i++)
    {
        if (now - np->found_at[i] > 3000)
        {
            np->found[i] = np->found[np->found_count - 1];
            np->found_at[i] = np->found_at[np->found_count - 1];
            np->found_count--;
            i--;
        }
    }

    if (np->is_host)
    {
        if (np->state == NETPLAY_LOBBY || np->state == NETPLAY_RUNNING || np->state == NETPLAY_DESYNC)
        {
            for (i = 1; i < NETPLAY_MAX_PLAYERS; i++)
            {
                Peer *p = &np->peers[i];

                if (p->used && p->connected && now - p->last_recv > (uint64_t)np->timeout_ms)
                {
                    host_drop(np, p, "timed out");
                }
            }
        }
        if (np->state == NETPLAY_LOBBY && now - np->last_lobby_send >= LOBBY_MS)
        {
            np->last_lobby_send = now;
            host_send_roster(np);
        }
        if (np->state == NETPLAY_RUNNING || np->state == NETPLAY_DESYNC)
        {
            for (i = 1; i < NETPLAY_MAX_PLAYERS; i++)
            {
                Peer *p = &np->peers[i];

                if (!p->used || !p->connected)
                {
                    continue;
                }
                if (!p->started)
                {
                    /* until it answers, only the start (frames would hold back its resend) */
                    if (now - p->last_send >= 50)
                    {
                        host_send_start(np, p);
                    }
                }
                else if (np->dirty || now - p->last_send >= RESEND_MS)
                {
                    host_send_frames(np, p, 1);
                }
            }
        }
        np->dirty = 0;
        return;
    }

    /* client */
    if (np->state == NETPLAY_JOINING || np->state == NETPLAY_SYNCING || np->state == NETPLAY_READY ||
        np->state == NETPLAY_RUNNING)
    {
        Peer *host = &np->peers[0];

        if (host->last_recv != 0 && now - host->last_recv > (uint64_t)np->timeout_ms)
        {
            set_reason(np, NETPLAY_DISCONNECTED, "lost the connection to the host");
            return;
        }
        if (np->state == NETPLAY_JOINING)
        {
            if (now - host->last_send >= LOBBY_MS)
            {
                W w;

                if (host->last_recv == 0 && host->last_send != 0 && now - np->last_lobby_send > (uint64_t)np->timeout_ms)
                {
                    set_reason(np, NETPLAY_DISCONNECTED, "no answer from the host (address, port, firewall?)");
                    return;
                }
                begin(np, &w, PKT_HELLO, host);
                wstr(&w, np->game_id, 16);
                wstr(&w, np->game_version, 48);
                wstr(&w, np->flavor, 16);
                wstr(&w, np->player_name, 32);
                w32(&w, np->nonce);
                send_peer(np, &w, host);
            }
        }
        else if (np->state == NETPLAY_SYNCING)
        {
            if (now - np->last_save_req >= 30)
            {
                W w;

                np->last_save_req = now;
                begin(np, &w, PKT_SAVE_REQ, host);
                w32(&w, (uint32_t)np->save_received);
                send_peer(np, &w, host);
            }
        }
        else if (np->state == NETPLAY_READY)
        {
            if (now - np->last_lobby_send >= LOBBY_MS)
            {
                W w;

                np->last_lobby_send = now;
                begin(np, &w, PKT_READY, host);
                send_peer(np, &w, host);
            }
        }
        else if (np->dirty || now - host->last_send >= RESEND_MS)
        {
            client_send_input(np);
        }
    }
    np->dirty = 0;
}

/* ---- API ---------------------------------------------------------------------------------- */
static void copy_text(char *out, size_t size, const char *in, const char *fallback)
{
    snprintf(out, size, "%s", (in && in[0]) ? in : fallback);
}

Netplay *netplay_create(const NetplayConfig *config)
{
    Netplay *np = (Netplay *)calloc(1, sizeof(Netplay));

    if (np == NULL)
    {
        return NULL;
    }
    copy_text(np->game_id, sizeof(np->game_id), config ? config->game_id : NULL, "game");
    copy_text(np->game_version, sizeof(np->game_version), config ? config->game_version : NULL, "");
    copy_text(np->game_name, sizeof(np->game_name), config ? config->game_name : NULL, np->game_id);
    copy_text(np->flavor, sizeof(np->flavor), config ? config->flavor : NULL, "");
    if (config && config->player_name && config->player_name[0])
    {
        copy_text(np->player_name, sizeof(np->player_name), config->player_name, "");
    }
    else
    {
        machine_name(np->player_name, sizeof(np->player_name));
    }
    np->max_players = (config && config->max_players >= 2 && config->max_players <= NETPLAY_MAX_PLAYERS) ? config->max_players : NETPLAY_MAX_PLAYERS;
    /* 0: auto (from the players' round trips, at the start) */
    np->delay = (config && config->input_delay >= 1 && config->input_delay <= NETPLAY_MAX_DELAY) ? config->input_delay : 0;
    np->timeout_ms = (config && config->timeout_ms > 0) ? config->timeout_ms : 5000;
    np->sock = NP_BAD_SOCKET;
    np->local_port = -1;
    np->desync_frame = -1;
    np->state = NETPLAY_IDLE;
#if defined(_WIN32)
    InitializeCriticalSection(&np->lock);
    InitializeConditionVariable(&np->received);
#else
    {
        pthread_mutexattr_t attr;

        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&np->lock, &attr);
        pthread_mutexattr_destroy(&attr);
        pthread_cond_init(&np->received, NULL);
    }
#endif
    return np;
}

static void say_goodbye(Netplay *np)
{
    int i, k;
    W w;

    for (k = 0; k < 3; k++)
    {
        for (i = 0; i < NETPLAY_MAX_PLAYERS; i++)
        {
            Peer *p = &np->peers[i];

            if (p->used && p->connected)
            {
                begin(np, &w, PKT_BYE, p);
                send_to(np, &w, &p->addr);
            }
        }
    }
}

static void netplay_stop_unlocked(Netplay *np)
{
    if (np == NULL)
    {
        return;
    }
    if (np->state != NETPLAY_IDLE)
    {
        say_goodbye(np);
    }
    if (np->sock != NP_BAD_SOCKET && !np->searching)
    {
        np_close(np->sock);
        np->sock = NP_BAD_SOCKET;
    }
    memset(np->peers, 0, sizeof(np->peers));
    free(np->save);
    np->save = NULL;
    np->save_size = np->save_received = 0;
    np->is_host = 0;
    np->local_port = -1;
    np->roster_count = 0;
    np->start_pending = 0;
    np->flavor_differs = 0;
    np->desync_frame = -1;
    np->state = NETPLAY_IDLE;
    np->reason[0] = 0;
}

void netplay_destroy(Netplay *np)
{
    if (np == NULL)
    {
        return;
    }
    netplay_set_background(np, 0);
    np->searching = 0;
    netplay_stop(np);
    if (np->sock != NP_BAD_SOCKET)
    {
        np_close(np->sock);
    }
#if defined(_WIN32)
    DeleteCriticalSection(&np->lock);
#else
    pthread_cond_destroy(&np->received);
    pthread_mutex_destroy(&np->lock);
#endif
    free(np->lagged);
    free(np);
}

static int netplay_host_unlocked(Netplay *np, int udp_port)
{
    netplay_stop(np);
    if (np->sock != NP_BAD_SOCKET)
    {
        np_close(np->sock); /* a search socket: the host needs its own port */
        np->sock = NP_BAD_SOCKET;
    }
    np->sock = open_socket(udp_port > 0 ? udp_port : NETPLAY_DEFAULT_PORT, np->reason, sizeof(np->reason));
    if (np->sock == NP_BAD_SOCKET)
    {
        np->state = NETPLAY_ERROR;
        return 0;
    }
    np->is_host = 1;
    np->session = (uint32_t)now_ms() * 2654435761u ^ (uint32_t)(uintptr_t)np;
    if (np->session == 0)
    {
        np->session = 1;
    }
    np->local_port = 0;
    np->state = NETPLAY_LOBBY;
    return 1;
}

static int netplay_join_unlocked(Netplay *np, const char *address)
{
    struct sockaddr_in addr;
    int searching = np->searching;

    netplay_stop(np);
    np->searching = searching;
    if (!resolve(address, &addr))
    {
        set_reason(np, NETPLAY_DISCONNECTED, "cannot resolve the host's address");
        return 0;
    }
    if (np->sock == NP_BAD_SOCKET)
    {
        np->sock = open_socket(0, np->reason, sizeof(np->reason));
        if (np->sock == NP_BAD_SOCKET)
        {
            np->state = NETPLAY_ERROR;
            return 0;
        }
    }
    memset(&np->peers[0], 0, sizeof(np->peers[0]));
    np->peers[0].used = 1;
    np->peers[0].connected = 1;
    np->peers[0].addr = addr;
    np->peers[0].rtt = -1;
    np->peers[0].port = 0;
    np->nonce = (uint32_t)now_ms() * 2246822519u ^ (uint32_t)(uintptr_t)np;
    np->session = 0;
    np->last_lobby_send = now_ms(); /* join timeout starts now */
    np->state = NETPLAY_JOINING;
    return 1;
}

static void netplay_set_save_unlocked(Netplay *np, const void *data, size_t size)
{
    if (!np->is_host || np->state != NETPLAY_LOBBY || size > NETPLAY_MAX_SAVE)
    {
        return;
    }
    free(np->save);
    np->save = NULL;
    np->save_size = 0;
    if (size > 0 && (np->save = (uint8_t *)malloc(size)) != NULL)
    {
        memcpy(np->save, data, size);
        np->save_size = size;
    }
    np->save_crc = crc32_of(np->save, np->save_size);
}

/* The input delay that keeps the slowest link from stalling: a client's input reaches the host,
   and the frame it completes comes back, within the delay (the round trip + 2x its jitter, or its
   recent peak if higher, + a margin). */
static int auto_delay(const Netplay *np)
{
    int i, need = 0, delay;

    for (i = 1; i < NETPLAY_MAX_PLAYERS; i++)
    {
        const Peer *p = &np->peers[i];

        if (p->used && p->connected)
        {
            int ms = p->rtt < 0 ? 50 : p->rtt + 2 * p->rtt_dev;

            ms = (p->rtt_peak > ms ? p->rtt_peak : ms) + AUTO_DELAY_MARGIN_MS;
            need = ms > need ? ms : need;
        }
    }
    delay = (need * 60 + 999) / 1000; /* frames of 1/60 s, rounded up */
    return delay < AUTO_DELAY_MIN ? AUTO_DELAY_MIN : delay > AUTO_DELAY_MAX ? AUTO_DELAY_MAX : delay;
}

static int netplay_start_unlocked(Netplay *np)
{
    int i, clients = 0;
    unsigned mask = 1u << np->local_port;

    if (!np->is_host || np->state != NETPLAY_LOBBY)
    {
        return 0;
    }
    for (i = 1; i < NETPLAY_MAX_PLAYERS; i++)
    {
        if (np->peers[i].used)
        {
            if (!np->peers[i].ready)
            {
                return 0;
            }
            clients++;
            mask |= 1u << np->peers[i].port;
        }
    }
    if (clients == 0)
    {
        return 0;
    }
    start_running(np, (uint32_t)now_ms() | 1u, np->delay >= 1 ? np->delay : auto_delay(np), mask);
    for (i = 1; i < NETPLAY_MAX_PLAYERS; i++)
    {
        if (np->peers[i].used)
        {
            host_send_start(np, &np->peers[i]);
        }
    }
    return 1;
}

static void netplay_poll_unlocked(Netplay *np)
{
    if (np == NULL)
    {
        return;
    }
    receive_all(np);
    tick(np);
}

static void netplay_search_unlocked(Netplay *np, int enable)
{
    np->searching = enable != 0;
    if (enable && np->sock == NP_BAD_SOCKET)
    {
        np->sock = open_socket(0, np->reason, sizeof(np->reason));
    }
    if (enable)
    {
        np->last_discover = 0;
    }
}

static int netplay_found_unlocked(Netplay *np, NetplayFoundHost *out, int max)
{
    int i;

    for (i = 0; i < np->found_count && i < max; i++)
    {
        out[i] = np->found[i];
    }
    return i;
}

static int netplay_take_start_unlocked(Netplay *np)
{
    int pending = np->start_pending;

    np->start_pending = 0;
    return pending;
}

static const uint8_t *netplay_save_unlocked(Netplay *np, size_t *size)
{
    int have = np->is_host || np->state == NETPLAY_READY || np->state == NETPLAY_RUNNING;

    *size = have ? np->save_size : 0;
    return have ? np->save : NULL;
}

static int netplay_frame_unlocked(Netplay *np, const NetplayPad *local, NetplayPad out[NETPLAY_MAX_PLAYERS], unsigned *out_mask)
{
    int32_t f, record;
    int s, p;

    if (out_mask)
    {
        *out_mask = 0;
    }
    receive_all(np);
    if (np->state != NETPLAY_RUNNING)
    {
        tick(np);
        return 0;
    }
    f = np->cur_frame;
    record = f + np->delay;
    if (record > np->local_recorded)
    {
        NetplayPad pad;

        memset(&pad, 0, sizeof(pad));
        if (local)
        {
            pad = *local;
        }
        np->local_in[slot_of(record)] = pad;
        np->local_recorded = record;
        if (np->is_host)
        {
            set_input(np, record, np->local_port, &pad);
            host_advance(np);
        }
        np->dirty = 1;
    }
    tick(np);
    if (f > np->complete_upto || np->state != NETPLAY_RUNNING)
    {
        np->stalls++;
        return 0;
    }
    s = slot_of(f);
    for (p = 0; p < NETPLAY_MAX_PLAYERS; p++)
    {
        if (np->port_mask & (1u << p))
        {
            out[p] = np->inputs[s][p];
        }
        else
        {
            memset(&out[p], 0, sizeof(out[p]));
        }
    }
    if (out_mask)
    {
        *out_mask = np->port_mask;
    }
    np->cur_frame++;
    return 1;
}

static void netplay_frame_done_unlocked(Netplay *np, uint64_t state_hash)
{
    int32_t frame = np->cur_frame - 1;
    int s, i;

    if (frame < 0 || (frame + 1) % NETPLAY_HASH_INTERVAL != 0 || np->state != NETPLAY_RUNNING)
    {
        return;
    }
    if (!np->is_host)
    {
        np->last_hash_frame = frame;
        np->last_hash_value = state_hash;
        return;
    }
    s = (int)(((uint32_t)frame / NETPLAY_HASH_INTERVAL) % HASH_SLOTS);
    np->own_hash_frame[s] = frame;
    np->own_hash_value[s] = state_hash;
    for (i = 1; i < NETPLAY_MAX_PLAYERS; i++)
    {
        Peer *p = &np->peers[i];

        if (p->used && p->hash_frame[s] == frame && p->hash_value[s] != state_hash && np->desync_frame < 0)
        {
            np->desync_frame = frame;
            np->state = NETPLAY_DESYNC;
            np->dirty = 1;
        }
    }
}

NetplayState netplay_state(const Netplay *np) { return np ? np->state : NETPLAY_IDLE; }
int netplay_is_host(const Netplay *np) { return np && np->is_host; }
int netplay_local_port(const Netplay *np) { return np ? np->local_port : -1; }
int netplay_input_delay(const Netplay *np) { return np ? np->delay : 0; }
int netplay_current_frame(const Netplay *np) { return np ? np->cur_frame : 0; }
int netplay_peer_flavor_differs(const Netplay *np) { return np && np->flavor_differs; }
uint32_t netplay_stall_count(const Netplay *np) { return np ? np->stalls : 0; }

static void netplay_set_input_delay_unlocked(Netplay *np, int frames)
{
    if (np->is_host && np->state == NETPLAY_LOBBY && frames >= 0 && frames <= NETPLAY_MAX_DELAY)
    {
        np->delay = frames;
    }
}

static int netplay_players_unlocked(const Netplay *np, NetplayPlayerInfo *out, int max)
{
    int n = 0, i;

    if (np->is_host)
    {
        if (n < max)
        {
            memset(&out[n], 0, sizeof(out[n]));
            out[n].port = np->local_port;
            out[n].ready = 1;
            out[n].connected = 1;
            out[n].rtt_ms = 0;
            out[n].is_local = 1;
            snprintf(out[n].name, sizeof(out[n].name), "%s", np->player_name);
            n++;
        }
        for (i = 1; i < NETPLAY_MAX_PLAYERS && n < max; i++)
        {
            const Peer *p = &np->peers[i];

            if (p->used)
            {
                memset(&out[n], 0, sizeof(out[n]));
                out[n].port = p->port;
                out[n].ready = p->ready;
                out[n].connected = p->connected;
                out[n].rtt_ms = p->rtt;
                snprintf(out[n].name, sizeof(out[n].name), "%s", p->name);
                n++;
            }
        }
        return n;
    }
    for (i = 0; i < np->roster_count && n < max; i++)
    {
        out[n] = np->roster[i];
        if (out[n].is_local)
        {
            out[n].rtt_ms = np->peers[0].rtt;
        }
        n++;
    }
    return n;
}

static const char *netplay_status_text_unlocked(const Netplay *np)
{
    Netplay *m = (Netplay *)np;
    int i, players = 1;

    switch (np->state)
    {
    case NETPLAY_IDLE:
        snprintf(m->status, sizeof(m->status), "Not in a session");
        break;
    case NETPLAY_LOBBY:
        for (i = 1; i < NETPLAY_MAX_PLAYERS; i++)
        {
            players += np->peers[i].used;
        }
        if (np->delay >= 1)
        {
            snprintf(m->status, sizeof(m->status), "Hosting: %d/%d players, input delay %d", players, np->max_players, np->delay);
        }
        else
        {
            snprintf(m->status, sizeof(m->status), "Hosting: %d/%d players, input delay auto (%d now)", players,
                     np->max_players, auto_delay(np));
        }
        break;
    case NETPLAY_JOINING:
        snprintf(m->status, sizeof(m->status), "Joining...");
        break;
    case NETPLAY_SYNCING:
        snprintf(m->status, sizeof(m->status), "Receiving the host's save data (%u/%u bytes)", (unsigned)np->save_received,
                 (unsigned)np->save_size);
        break;
    case NETPLAY_READY:
        snprintf(m->status, sizeof(m->status), "Ready as player %d: waiting for the host to start", np->local_port + 1);
        break;
    case NETPLAY_RUNNING:
        snprintf(m->status, sizeof(m->status), "Playing as player %d: frame %d, delay %d%s", np->local_port + 1,
                 (int)np->cur_frame, np->delay, np->is_host ? " (host)" : "");
        break;
    case NETPLAY_DESYNC:
        snprintf(m->status, sizeof(m->status),
                 "Desync at frame %d: the games diverged (different builds or ROMs?). The session is over.",
                 (int)np->desync_frame);
        break;
    case NETPLAY_DISCONNECTED:
        snprintf(m->status, sizeof(m->status), "Disconnected: %s", np->reason);
        break;
    case NETPLAY_ERROR:
        snprintf(m->status, sizeof(m->status), "Network error: %s", np->reason);
        break;
    }
    return np->status;
}

void netplay_debug_set_lag(Netplay *np, int ms, int jitter_ms)
{
    np_lock(np);
    np->lag_ms = ms < 0 ? 0 : ms;
    np->lag_jitter_ms = jitter_ms < 0 ? 0 : jitter_ms;
    np_unlock(np);
}

void netplay_debug_set_loss(Netplay *np, int percent, uint32_t seed)
{
    np->loss = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    np->loss_rng = seed ? seed : 1;
}

/* ---- thread safety and the background network thread ------------------------------------------
   Every call below holds the session's lock, so the network thread (netplay_set_background) and
   the game can use the session at the same time. The lock is recursive (calls nest). */
static void np_lock(const Netplay *np)
{
#if defined(_WIN32)
    EnterCriticalSection((CRITICAL_SECTION *)&np->lock);
#else
    pthread_mutex_lock((pthread_mutex_t *)&np->lock);
#endif
}

static void np_unlock(const Netplay *np)
{
#if defined(_WIN32)
    LeaveCriticalSection((CRITICAL_SECTION *)&np->lock);
#else
    pthread_mutex_unlock((pthread_mutex_t *)&np->lock);
#endif
}

void netplay_stop(Netplay *np)
{
    np_lock(np);
    netplay_stop_unlocked(np);
    np_unlock(np);
}

int netplay_host(Netplay *np, int udp_port)
{
    int r;

    np_lock(np);
    r = netplay_host_unlocked(np, udp_port);
    np_unlock(np);
    return r;
}

int netplay_join(Netplay *np, const char *address)
{
    int r;

    np_lock(np);
    r = netplay_join_unlocked(np, address);
    np_unlock(np);
    return r;
}

void netplay_set_save(Netplay *np, const void *data, size_t size)
{
    np_lock(np);
    netplay_set_save_unlocked(np, data, size);
    np_unlock(np);
}

int netplay_start(Netplay *np)
{
    int r;

    np_lock(np);
    r = netplay_start_unlocked(np);
    np_unlock(np);
    return r;
}

void netplay_poll(Netplay *np)
{
    if (np == NULL)
    {
        return;
    }
    np_lock(np);
    netplay_poll_unlocked(np);
    np_unlock(np);
}

void netplay_search(Netplay *np, int enable)
{
    np_lock(np);
    netplay_search_unlocked(np, enable);
    np_unlock(np);
}

int netplay_found(Netplay *np, NetplayFoundHost *out, int max)
{
    int r;

    np_lock(np);
    r = netplay_found_unlocked(np, out, max);
    np_unlock(np);
    return r;
}

int netplay_take_start(Netplay *np)
{
    int r;

    np_lock(np);
    r = netplay_take_start_unlocked(np);
    np_unlock(np);
    return r;
}

const uint8_t *netplay_save(Netplay *np, size_t *size)
{
    const uint8_t *r;

    np_lock(np);
    r = netplay_save_unlocked(np, size);
    np_unlock(np);
    return r;
}

int netplay_frame(Netplay *np, const NetplayPad *local, NetplayPad out[NETPLAY_MAX_PLAYERS], unsigned *out_mask)
{
    int r;

    np_lock(np);
    r = netplay_frame_unlocked(np, local, out, out_mask);
    np_unlock(np);
    return r;
}

void netplay_frame_done(Netplay *np, uint64_t state_hash)
{
    np_lock(np);
    netplay_frame_done_unlocked(np, state_hash);
    np_unlock(np);
}

void netplay_set_input_delay(Netplay *np, int frames)
{
    np_lock(np);
    netplay_set_input_delay_unlocked(np, frames);
    np_unlock(np);
}

int netplay_players(const Netplay *np, NetplayPlayerInfo *out, int max)
{
    int r;

    np_lock(np);
    r = netplay_players_unlocked(np, out, max);
    np_unlock(np);
    return r;
}

const char *netplay_status_text(const Netplay *np)
{
    const char *r;

    np_lock(np);
    r = netplay_status_text_unlocked(np);
    np_unlock(np);
    return r;
}

/* Waits up to ms for the socket to have something to read; >0: it has. */
static int wait_readable(np_socket s, int ms)
{
    fd_set set;
    struct timeval tv;

    FD_ZERO(&set);
    FD_SET(s, &set);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select((int)s + 1, &set, NULL, NULL, &tv);
}

static void sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* The network thread: receives as packets arrive and answers at once (a host passes a client's
   input on to the others then, not at its next game frame), and wakes netplay_wait. */
static void background_loop(Netplay *np)
{
    while (!np->bg_quit)
    {
        np_socket s;
        uint32_t before;

        np_lock(np);
        s = np->sock;
        np_unlock(np);
        if (s == NP_BAD_SOCKET)
        {
            sleep_ms(BACKGROUND_WAIT_MS);
            continue;
        }
        if (wait_readable(s, BACKGROUND_WAIT_MS) < 0)
        {
            sleep_ms(1); /* the socket was just closed (stop / host / join) */
        }
        np_lock(np);
        before = np->recv_seq;
        receive_all(np);
        tick(np);
        if (np->recv_seq != before)
        {
#if defined(_WIN32)
            WakeAllConditionVariable(&np->received);
#else
            pthread_cond_broadcast(&np->received);
#endif
        }
        np_unlock(np);
    }
}

#if defined(_WIN32)
static DWORD WINAPI background_main(LPVOID arg)
{
    background_loop((Netplay *)arg);
    return 0;
}
#else
static void *background_main(void *arg)
{
    background_loop((Netplay *)arg);
    return NULL;
}
#endif

void netplay_set_background(Netplay *np, int enable)
{
    if (np == NULL || (enable != 0) == (np->bg_running != 0))
    {
        return;
    }
    if (enable)
    {
        np->bg_quit = 0;
#if defined(_WIN32)
        np->bg_thread = CreateThread(NULL, 0, background_main, np, 0, NULL);
        np->bg_running = np->bg_thread != NULL;
#else
        np->bg_running = pthread_create(&np->bg_thread, NULL, background_main, np) == 0;
#endif
        return;
    }
    np->bg_quit = 1;
#if defined(_WIN32)
    WaitForSingleObject(np->bg_thread, INFINITE);
    CloseHandle(np->bg_thread);
    np->bg_thread = NULL;
#else
    pthread_join(np->bg_thread, NULL);
#endif
    np->bg_running = 0;
}

int netplay_wait(Netplay *np, int timeout_ms)
{
    uint32_t before;
    int got;

    if (np == NULL || timeout_ms <= 0)
    {
        return 0;
    }
    if (!np->bg_running)
    {
        np_socket s;

        np_lock(np);
        s = np->sock;
        np_unlock(np);
        if (s == NP_BAD_SOCKET)
        {
            sleep_ms(timeout_ms);
            return 0;
        }
        got = wait_readable(s, timeout_ms) > 0;
        netplay_poll(np);
        return got;
    }
    np_lock(np);
    before = np->recv_seq;
#if defined(_WIN32)
    SleepConditionVariableCS(&np->received, &np->lock, (DWORD)timeout_ms);
#else
    {
        struct timeval now;
        struct timespec until;

        gettimeofday(&now, NULL);
        until.tv_sec = now.tv_sec + timeout_ms / 1000;
        until.tv_nsec = (long)now.tv_usec * 1000L + (long)(timeout_ms % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L)
        {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&np->received, &np->lock, &until);
    }
#endif
    got = np->recv_seq != before;
    np_unlock(np);
    return got;
}
