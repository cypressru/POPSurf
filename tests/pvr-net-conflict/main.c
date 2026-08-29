#include <kos.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);

/* Same shape as the browser: the fetch runs on a worker thread while the main
 * thread renders a PVR scene every frame. The download on its own is already
 * known clean, so if this is not, the interaction is the bug. */

#define HOST "10.0.0.89"
#define PORT 8000
#define PATH "/gmbank.psb"

static volatile int g_busy;

static void *dl_thread(void *arg)
{
    struct sockaddr_in sa;
    char    req[256];
    char   *buf;
    int     fd;
    size_t  got = 0;
    ssize_t n;

    (void)arg;

    buf = (char *)malloc(64 * 1024);
    if(!buf)
        return NULL;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        printf("nettest: socket failed\n");
        return NULL;
    }

    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(PORT);
    sa.sin_addr.s_addr = inet_addr(HOST);

    if(connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        printf("nettest: connect failed\n");
        return NULL;
    }

    g_busy = 1;
    printf("nettest: connected\n");
    fflush(stdout);

    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
             PATH, HOST);
    send(fd, req, strlen(req), 0);

    for(;;) {
        n = recv(fd, buf, 64 * 1024, 0);
        if(n <= 0)
            break;
        got += (size_t)n;
        if((got / (256 * 1024)) != ((got - (size_t)n) / (256 * 1024))) {
            printf("nettest: %u bytes\n", (unsigned)got);
            fflush(stdout);
        }
    }

    g_busy = 0;
    printf("nettest: DONE %u bytes\n", (unsigned)got);
    fflush(stdout);
    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    /* Same parameters the browser uses, DMA off, so this is not testing a
     * configuration the browser never runs. */
    {
        pvr_init_params_t params = {
            { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0,
              PVR_BINSIZE_0 },
            512 * 1024,
            0,            /* dma_enabled */
            0,
            0,
            0
        };
        pvr_init(&params);
    }
    thd_create(0, dl_thread, NULL);

    /* Render normally, but submit nothing at all while a bulk transfer is in
     * flight. Throttling was not enough - any TA traffic during the transfer
     * is a fault - so the two are fully serialised. */
    for(;;) {
        if(g_busy) {
            thd_sleep(50);
            continue;
        }
        pvr_wait_ready();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        pvr_list_finish();
        pvr_scene_finish();
    }
    return 0;
}
