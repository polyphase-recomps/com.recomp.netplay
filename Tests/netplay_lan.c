/*
 * Lists the netplay sessions on the LAN (hosts on the default port answer a broadcast), as the
 * editor's "Search the LAN" does. For checking a network: run it on one machine while another
 * hosts.
 *
 *   netplay_lan [seconds] [game_id]
 */
#include "netplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
static void nap(void) { Sleep(10); }
#else
#include <time.h>
static void nap(void)
{
    struct timespec ts = {0, 10000000};
    nanosleep(&ts, NULL);
}
#endif

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 3, i, n;
    NetplayConfig cfg;
    Netplay *np;
    NetplayFoundHost hosts[16];

    memset(&cfg, 0, sizeof(cfg));
    cfg.game_id = argc > 2 ? argv[2] : "ssb64";
    np = netplay_create(&cfg);
    netplay_search(np, 1);
    for (i = 0; i < seconds * 100; i++)
    {
        netplay_poll(np);
        nap();
    }
    n = netplay_found(np, hosts, 16);
    printf("%d session(s) found on UDP port %d\n", n, NETPLAY_DEFAULT_PORT);
    for (i = 0; i < n; i++)
    {
        printf("  %-22s %-16s %s (%d/%d players)%s%s\n", hosts[i].address, hosts[i].host_name, hosts[i].game_name,
               hosts[i].players, hosts[i].max_players, hosts[i].in_game ? ", in game" : "",
               hosts[i].compatible ? "" : ", other game or ROM");
    }
    netplay_destroy(np);
    return 0;
}
