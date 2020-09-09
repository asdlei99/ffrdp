#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "ffrdp.h"

#ifdef WIN32
#include <winsock2.h>
#define usleep(t) Sleep((t) / 1000)
#define get_tick_count GetTickCount
typedef   signed char    int8_t;
typedef unsigned char   uint8_t;
typedef   signed short  int16_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef   signed int    int32_t;
#pragma warning(disable:4996) // disable warnings
#else
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#define SOCKET int
#define closesocket close
#define stricmp strcasecmp
#define strtok_s strtok_r
static uint32_t get_tick_count()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#endif

#define FFRDP_RECVBUF_SIZE  (64 * 1024 - 4)
#define FFRDP_MTU_SIZE       1024 // should align to 4 bytes
#define FFRDP_MIN_RTO        20
#define FFRDP_MAX_RTO        2000
#define FFRDP_WIN_CYCLE      100
#define FFRDP_MAX_WAITSND    256
#define FFRDP_SNDPKT_FLOWCTL 32
#define FFRDP_UDPRBUF_SIZE  (128 * FFRDP_MTU_SIZE)
#define FFRDP_SELECT_SLEEP   0
#define FFRDP_SELECT_TIMEOUT 10000
#define FFRDP_USLEEP_TIMEOUT 1000
#define FFRDP_ENABLE_FEC     0
#define FFRDP_FEC_REDUNDANCY 8

#define MIN(a, b)               ((a) < (b) ? (a) : (b))
#define MAX(a, b)               ((a) > (b) ? (a) : (b))
#define GET_FRAME_SEQ(f)        (*(uint32_t*)(f)->data >> 8)
#define SET_FRAME_SEQ(f, seq)   do { *(uint32_t*)(f)->data  = ((seq) & 0xFFFFFF) << 8; } while (0)

enum {
    FFRDP_FRAME_TYPE_DATA,
    FFRDP_FRAME_TYPE_ACK ,
    FFRDP_FRAME_TYPE_WIN0,
    FFRDP_FRAME_TYPE_WIN1,
    FFRDP_FRAME_TYPE_BYE ,
};

typedef struct tagFFRDP_FRAME_NODE {
    struct tagFFRDP_FRAME_NODE *next;
    struct tagFFRDP_FRAME_NODE *prev;
    uint16_t size; // frame size
    uint8_t *data; // frame data
    #define FLAG_FIRST_SEND  (1 << 0) // after frame first send, this flag will be set
    #define FLAG_FAST_RESEND (1 << 1) // data frame need fast resend when next update
    uint32_t flags;        // frame flags
    uint32_t tick_send;    // frame send tick
    uint32_t tick_timeout; // frame ack timeout
} FFRDP_FRAME_NODE;

typedef struct {
    uint8_t  recv_buff[FFRDP_RECVBUF_SIZE];
    int32_t  recv_size;
    int32_t  recv_head;
    int32_t  recv_tail;
    #define FLAG_SERVER    (1 << 0)
    #define FLAG_CONNECTED (1 << 1)
    #define FLAG_BYEBYE0   (1 << 2)
    #define FLAG_BYEBYE1   (1 << 3)
    uint32_t flags;
    SOCKET   udp_fd;
    struct   sockaddr_in server_addr;
    struct   sockaddr_in client_addr;

    FFRDP_FRAME_NODE *send_list_head;
    FFRDP_FRAME_NODE *send_list_tail;
    FFRDP_FRAME_NODE *recv_list_head;
    FFRDP_FRAME_NODE *recv_list_tail;
    uint32_t send_seq;
    uint32_t recv_seq;
    uint32_t recv_win; // remote receive window
    uint32_t rttm, rtts, rttd, rto;
    uint32_t tick_query_rwin;
    uint32_t wait_snd;
    uint32_t counter_send_1sttime;
    uint32_t counter_send_failed;
    uint32_t counter_resend_fast;
    uint32_t counter_resend_rto;
    uint32_t counter_query_rwin;

#if FFRDP_ENABLE_FEC
    uint8_t  fec_txbuf[4 + FFRDP_MTU_SIZE + 2];
    uint8_t  fec_rxbuf[4 + FFRDP_MTU_SIZE + 2];
    uint32_t fec_txseq;
    uint32_t fec_rxseq;
    uint32_t fec_rxmask;
    uint32_t counter_fec_tx_short;
    uint32_t counter_fec_tx_full;
    uint32_t counter_fec_rx_short;
    uint32_t counter_fec_rx_full;
    uint32_t counter_fec_ok;
    uint32_t counter_fec_failed;
#endif
} FFRDPCONTEXT;

static uint32_t ringbuf_write(uint8_t *rbuf, uint32_t maxsize, uint32_t tail, uint8_t *src, uint32_t len)
{
    uint8_t *buf1 = rbuf + tail;
    int      len1 = MIN(maxsize-tail, len);
    uint8_t *buf2 = rbuf;
    int      len2 = len  - len1;
    memcpy(buf1, src + 0   , len1);
    memcpy(buf2, src + len1, len2);
    return len2 ? len2 : tail + len1;
}

static uint32_t ringbuf_read(uint8_t *rbuf, uint32_t maxsize, uint32_t head, uint8_t *dst, uint32_t len)
{
    uint8_t *buf1 = rbuf + head;
    int      len1 = MIN(maxsize-head, len);
    uint8_t *buf2 = rbuf;
    int      len2 = len  - len1;
    if (dst) memcpy(dst + 0   , buf1, len1);
    if (dst) memcpy(dst + len1, buf2, len2);
    return len2 ? len2 : head + len1;
}

static int seq_distance(uint32_t seq1, uint32_t seq2) // calculate seq distance
{
    int c = seq1 - seq2;
    if      (c >=  0x7FFFFF) return c - 0x1000000;
    else if (c <= -0x7FFFFF) return c + 0x1000000;
    else return c;
}

static FFRDP_FRAME_NODE* frame_node_new(int size) // create a new frame node
{
    FFRDP_FRAME_NODE *node = malloc(sizeof(FFRDP_FRAME_NODE) + size);
    if (!node) return NULL;
    memset(node, 0, sizeof(FFRDP_FRAME_NODE));
    node->size = size;
    node->data = (uint8_t*)node + sizeof(FFRDP_FRAME_NODE);
    return node;
}

static void list_enqueue(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail, FFRDP_FRAME_NODE *node)
{
    FFRDP_FRAME_NODE *p;
    uint32_t seqnew, seqcur;
    int      dist;
    if (*head == NULL) {
        *head = node;
        *tail = node;
    } else {
        seqnew = GET_FRAME_SEQ(node);
        for (p=*tail; p; p=p->prev) {
            seqcur = GET_FRAME_SEQ(p);
            dist   = seq_distance(seqnew, seqcur);
            if (dist == 0) return;
            if (dist >  0) {
                if (p->next) p->next->prev = node;
                else *tail = node;
                node->next = p->next;
                node->prev = p;
                p->next    = node;
                return;
            }
        }
        node->next = *head;
        node->prev =  NULL;
        node->next->prev = node;
        *head = node;
    }
}

static void list_remove(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail, FFRDP_FRAME_NODE *node, int f)
{
    if (node->next) node->next->prev = node->prev;
    else *tail = node->prev;
    if (node->prev) node->prev->next = node->next;
    else *head = node->next;
    if (f) free(node);
}

static int list_free(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail, int n)
{
    int k = 0;
    while (*head && ++k != n) list_remove(head, tail, *head, 1);
    return k;
}

static int ffrdp_sleep(FFRDPCONTEXT *ffrdp, int flag)
{
    if (flag) {
        struct timeval tv;
        fd_set  rs;
        FD_ZERO(&rs);
        FD_SET(ffrdp->udp_fd, &rs);
        tv.tv_sec  = 0;
        tv.tv_usec = FFRDP_SELECT_TIMEOUT;
        if (select((int)ffrdp->udp_fd + 1, &rs, NULL, NULL, &tv) <= 0) return -1;
    } else usleep(FFRDP_USLEEP_TIMEOUT);
    return 0;
}

static void ffrdp_reset(FFRDPCONTEXT *ffrdp)
{
    struct sockaddr_in srcaddr;
    uint8_t  buf[FFRDP_MTU_SIZE + 4];
    uint32_t addrlen = sizeof(srcaddr);
    int      ret;
    ffrdp->recv_size = ffrdp->recv_head = ffrdp->recv_tail = ffrdp->send_seq = ffrdp->recv_seq = 0;
    ffrdp->rttm      = ffrdp->rtts = ffrdp->rttd = ffrdp->tick_query_rwin = ffrdp->wait_snd = 0;
    ffrdp->recv_win  = FFRDP_RECVBUF_SIZE / 2;
    ffrdp->rto       = FFRDP_MIN_RTO;
    ffrdp->flags    &=~(FLAG_CONNECTED|FLAG_BYEBYE0|FLAG_BYEBYE1);
    ffrdp->counter_send_1sttime = ffrdp->counter_resend_fast = ffrdp->counter_resend_rto = ffrdp->counter_query_rwin = 0;
    list_free(&ffrdp->send_list_head, &ffrdp->send_list_tail, -1);
    list_free(&ffrdp->recv_list_head, &ffrdp->recv_list_tail, -1);
    memset(&ffrdp->client_addr, 0, sizeof(ffrdp->client_addr));
    do { ret = recvfrom(ffrdp->udp_fd, buf, sizeof(buf), 0, (struct sockaddr*)&srcaddr, &addrlen); } while (ret > 0);
}

static int ffrdp_send_data_frame(FFRDPCONTEXT *ffrdp, FFRDP_FRAME_NODE *frame, struct sockaddr_in *dstaddr)
{
#if !FFRDP_ENABLE_FEC
    frame->data[frame->size - 2] = frame->data[frame->size - 1] = 0;
    return sendto(ffrdp->udp_fd, frame->data, frame->size, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in)) == frame->size ? 0 : -1;
#else
    if (frame->size != 4 + FFRDP_MTU_SIZE + 2) { // short frame
        int ret = sendto(ffrdp->udp_fd, frame->data, frame->size, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
        ffrdp->counter_fec_tx_short++;
        return ret == frame->size ? 0 : -1;
    } else {
        uint32_t *psrc = (uint32_t*)frame->data;
        uint32_t *pdst = (uint32_t*)ffrdp->fec_txbuf;
        int       ret, i;
        *(uint16_t*)(frame->data + 4 + FFRDP_MTU_SIZE) = (uint16_t)ffrdp->fec_txseq++;
        ret = sendto(ffrdp->udp_fd, frame->data, frame->size, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
        if (ret != frame->size) return -1;
        for (i=0; i<(FFRDP_MTU_SIZE+4)/sizeof(uint32_t); i++) *pdst++ ^= *psrc++; // make xor fec frame
        if (ffrdp->fec_txseq % FFRDP_FEC_REDUNDANCY == FFRDP_FEC_REDUNDANCY - 1) {
            *(uint16_t*)(ffrdp->fec_txbuf + 4 + FFRDP_MTU_SIZE) = ffrdp->fec_txseq++;
            ffrdp->fec_txbuf[0] = FFRDP_FRAME_TYPE_DATA;
            sendto(ffrdp->udp_fd, ffrdp->fec_txbuf, sizeof(ffrdp->fec_txbuf), 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in)); // send fec frame
            memset(ffrdp->fec_txbuf, 0, sizeof(ffrdp->fec_txbuf)); // clear tx_fecbuf
        }
        ffrdp->counter_fec_tx_full++;
        return 0;
    }
#endif
}

static int ffrdp_recv_data_frame(FFRDPCONTEXT *ffrdp, FFRDP_FRAME_NODE *frame)
{
#if !FFRDP_ENABLE_FEC
    ffrdp = ffrdp; frame = frame; return 0;
#else
    uint16_t fecseq;
    int      i, j;
    if (frame->size != 4 + FFRDP_MTU_SIZE + 2) {
        ffrdp->counter_fec_rx_short++;
        return 0;
    }
    fecseq = *(uint16_t*)(frame->data + 4 + FFRDP_MTU_SIZE);
    if (fecseq / FFRDP_FEC_REDUNDANCY != ffrdp->fec_rxseq / FFRDP_FEC_REDUNDANCY) {
        memcpy(ffrdp->fec_rxbuf, frame->data, sizeof(ffrdp->fec_rxbuf));
        ffrdp->fec_rxseq = fecseq; ffrdp->fec_rxmask = 0;
        return fecseq % FFRDP_FEC_REDUNDANCY != FFRDP_FEC_REDUNDANCY - 1 ? 0 : -1;
    }
    ffrdp->fec_rxseq = fecseq;
    if (!(ffrdp->fec_rxmask & (1 << (fecseq % FFRDP_FEC_REDUNDANCY)))) {
        uint32_t *psrc = (uint32_t*)frame->data;
        uint32_t *pdst = (uint32_t*)ffrdp->fec_rxbuf;
        for (i=0; i<(FFRDP_MTU_SIZE+4)/sizeof(uint32_t); i++) *pdst++ ^= *psrc++;
        ffrdp->fec_rxmask |= 1 << (fecseq % FFRDP_FEC_REDUNDANCY);
    }
    if (fecseq % FFRDP_FEC_REDUNDANCY == FFRDP_FEC_REDUNDANCY - 1) {
        if (ffrdp->fec_rxmask == (1 << FFRDP_FEC_REDUNDANCY) - 1) return -1;
        for (i=0,j=0; i<FFRDP_FEC_REDUNDANCY-1 && j<=1; i++) {
            if (!(ffrdp->fec_rxmask & (1 << i))) j++;
        }
        if (j != 1) {
            ffrdp->counter_fec_failed++;
            return -1;
        }
        memcpy(frame->data, ffrdp->fec_rxbuf, frame->size);
        frame->data[0] = FFRDP_FRAME_TYPE_DATA;
        ffrdp->counter_fec_ok++;
    }
    ffrdp->counter_fec_rx_full++;
    return 0;
#endif
}

void* ffrdp_init(char *ip, int port, int server)
{
#ifdef WIN32
    WSADATA wsaData;
#endif
    unsigned long opt;
    FFRDPCONTEXT *ffrdp = calloc(1, sizeof(FFRDPCONTEXT));
    if (!ffrdp) return NULL;

#ifdef WIN32
    timeBeginPeriod(1);
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed !\n");
        return NULL;
    }
#endif

    ffrdp->recv_win = FFRDP_RECVBUF_SIZE / 2;
    ffrdp->rtts     = (uint32_t) -1;
    ffrdp->rto      = FFRDP_MIN_RTO;

    ffrdp->server_addr.sin_family      = AF_INET;
    ffrdp->server_addr.sin_port        = htons(port);
    ffrdp->server_addr.sin_addr.s_addr = inet_addr(ip);
    ffrdp->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ffrdp->udp_fd < 0) {
        printf("failed to open socket !\n");
        goto failed;
    }

    if (server) {
        ffrdp->flags |= FLAG_SERVER;
        if (bind(ffrdp->udp_fd, (struct sockaddr*)&ffrdp->server_addr, sizeof(ffrdp->server_addr)) == -1) {
            printf("failed to bind !\n");
            goto failed;
        }
    }

#ifdef WIN32
    opt = 1; ioctlsocket(ffrdp->udp_fd, FIONBIO, &opt); // setup non-block io mode
#else
    fcntl(ffrdp->udp_fd, F_SETFL, fcntl(ffrdp->udp_fd, F_GETFL, 0) | O_NONBLOCK);  // setup non-block io mode
#endif
    opt = FFRDP_UDPRBUF_SIZE; setsockopt(ffrdp->udp_fd, SOL_SOCKET, SO_RCVBUF, (char*)&opt, sizeof(int)); // setup udp recv buffer size
    return ffrdp;

failed:
    if (ffrdp->udp_fd > 0) closesocket(ffrdp->udp_fd);
    free(ffrdp);
    return NULL;
}

void ffrdp_free(void *ctxt)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ctxt) return;
    if (ffrdp->udp_fd > 0) closesocket(ffrdp->udp_fd);
    list_free(&ffrdp->send_list_head, &ffrdp->send_list_tail, -1);
    list_free(&ffrdp->recv_list_head, &ffrdp->recv_list_tail, -1);
    free(ffrdp);
#ifdef WIN32
    WSACleanup();
    timeEndPeriod(1);
#endif
}

int ffrdp_send(void *ctxt, char *buf, int len)
{
    FFRDPCONTEXT     *ffrdp = (FFRDPCONTEXT*)ctxt;
    FFRDP_FRAME_NODE *node  = NULL;
    int               n = len, size;
    if (  !ffrdp || ((ffrdp->flags & FLAG_SERVER) && (ffrdp->flags & FLAG_CONNECTED) == 0)
       || ((len + FFRDP_MTU_SIZE - 1) / FFRDP_MTU_SIZE + ffrdp->wait_snd > FFRDP_MAX_WAITSND)) {
        ffrdp->counter_send_failed++;
        return -1;
    }
    while (n > 0) {
        size = MIN(FFRDP_MTU_SIZE, n);
        if (!(node = frame_node_new(4 + size + 2))) break;
        SET_FRAME_SEQ(node, ffrdp->send_seq);
        memcpy(node->data + 4, buf, size);
        list_enqueue(&ffrdp->send_list_head, &ffrdp->send_list_tail, node);
        ffrdp->send_seq++; ffrdp->send_seq &= 0xFFFFFF; ffrdp->wait_snd++;
        buf += size; n -= size;
    }
    return len - n;
}

int ffrdp_recv(void *ctxt, char *buf, int len)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    int           ret;
    if (!ctxt) return -1;
    ret = MIN(len, ffrdp->recv_size);
    if (ret > 0) {
        ffrdp->recv_head = ringbuf_read(ffrdp->recv_buff, sizeof(ffrdp->recv_buff), ffrdp->recv_head, (uint8_t*)buf, ret);
        ffrdp->recv_size-= ret;
    }
    return ret;
}

int ffrdp_byebye(void *ctxt)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ffrdp || (ffrdp->flags & FLAG_SERVER)) return -1;
    ffrdp->flags |= FLAG_BYEBYE0;
    return 0;
}

void ffrdp_update(void *ctxt)
{
    FFRDPCONTEXT       *ffrdp   = (FFRDPCONTEXT*)ctxt;
    FFRDP_FRAME_NODE   *node    = NULL, *p = NULL, *t = NULL;
    struct sockaddr_in *dstaddr = NULL;
    struct sockaddr_in  srcaddr;
    uint32_t addrlen = sizeof(srcaddr);
    int32_t  una, mack, size, ret, got_data = 0, send_una, send_mack = 0, recv_una, recv_mack = 0, recv_win, dist, maxack, i;
    uint8_t  data[8];

    if (!ctxt) return;
    dstaddr  = ffrdp->flags & FLAG_SERVER ? &ffrdp->client_addr : &ffrdp->server_addr;
    send_una = ffrdp->send_list_head ? GET_FRAME_SEQ(ffrdp->send_list_head) : 0;
    recv_una = ffrdp->recv_seq;

    for (i=0,p=ffrdp->send_list_head; i<FFRDP_SNDPKT_FLOWCTL&&p; i++,p=p->next) {
        if (!(p->flags & FLAG_FIRST_SEND)) { // first send
            if (p->size - 4 - 2 <= (int)ffrdp->recv_win) {
                if (ffrdp_send_data_frame(ffrdp, p, dstaddr) != 0) break;
                ffrdp->recv_win -= p->size - 4 - 2;
                p->tick_send     = get_tick_count();
                p->tick_timeout  = p->tick_send + ffrdp->rto;
                p->flags        |= FLAG_FIRST_SEND;
                ffrdp->counter_send_1sttime++;
            } else if ((int32_t)get_tick_count() - (int32_t)ffrdp->tick_query_rwin > FFRDP_WIN_CYCLE) { // query remote receive window size
                data[0] = FFRDP_FRAME_TYPE_WIN0; sendto(ffrdp->udp_fd, data, 1, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
                ffrdp->counter_query_rwin++;
                break;
            }
        } else if ((p->flags & FLAG_FIRST_SEND) && ((int32_t)get_tick_count() - (int32_t)p->tick_timeout > 0 || (p->flags & FLAG_FAST_RESEND))) { // resend
            if (ffrdp_send_data_frame(ffrdp, p, dstaddr) != 0) break;
            if (!(p->flags & FLAG_FAST_RESEND)) {
                ffrdp->counter_resend_rto++;
            } else {
                ffrdp->counter_resend_fast++;
            }
            p->tick_timeout+= (p->tick_timeout - p->tick_send) / 2; // rto = rto + rto / 2
            p->flags       &=~FLAG_FAST_RESEND;
            if (ffrdp->rto == FFRDP_MAX_RTO) break; // if rto reach FFRDP_MAX_RTO, we only try to resend one packet
        }
    }

    if (ffrdp_sleep(ffrdp, FFRDP_SELECT_SLEEP) != 0) return;
    for (node=NULL;;) { // receive data
        if (!node && !(node = frame_node_new(4 + FFRDP_MTU_SIZE + 2))) break;;
        if ((ret = recvfrom(ffrdp->udp_fd, node->data, node->size, 0, (struct sockaddr*)&srcaddr, &addrlen)) <= 0) break;
        if ((ffrdp->flags & FLAG_SERVER) && (ffrdp->flags & FLAG_CONNECTED) == 0) {
            if (ffrdp->flags & FLAG_CONNECTED) {
                if (memcmp(&srcaddr, &ffrdp->client_addr, sizeof(srcaddr)) != 0) continue;
            } else {
                ffrdp->flags |= FLAG_CONNECTED;
                memcpy(&ffrdp->client_addr, &srcaddr, sizeof(ffrdp->client_addr));
            }
        }

        switch (node->data[0]) {
        case FFRDP_FRAME_TYPE_DATA:
            node->size = ret; // frame size is the return size of recvfrom
            if (ffrdp_recv_data_frame(ffrdp, node) == 0) {
                dist = seq_distance(GET_FRAME_SEQ(node), recv_una);
                if (dist == 0) recv_una++;
                if (dist >= 0) { list_enqueue(&ffrdp->recv_list_head, &ffrdp->recv_list_tail, node); node = NULL; }
                got_data = 1;
            }
            break;
        case FFRDP_FRAME_TYPE_ACK:
            una  = *(uint32_t*)(node->data + 0) >> 8;
            mack = *(uint32_t*)(node->data + 4) & 0xFFFF;
            dist = seq_distance(una, send_una);
            if (dist == 0) {
                send_mack|= mack;
            } if (dist > 0) {
                send_una  = una;
                send_mack = (send_mack >> dist) | mack;
                recv_win  = *(uint32_t*)(node->data + 4) >> 16;
            }
            break;
        case FFRDP_FRAME_TYPE_WIN0:
            size    = (!ffrdp->recv_list_head || GET_FRAME_SEQ(ffrdp->recv_list_head) != ffrdp->recv_seq) ? sizeof(ffrdp->recv_buff) - ffrdp->recv_size : 0;
            data[0] = FFRDP_FRAME_TYPE_WIN1;
            data[1] = (size >> 0) & 0xFF;
            data[2] = (size >> 8) & 0xFF;
            sendto(ffrdp->udp_fd, data, 3, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
            break;
        case FFRDP_FRAME_TYPE_WIN1:
            ffrdp->recv_win = (data[1] << 0) | (data[2] << 8); ffrdp->tick_query_rwin = get_tick_count();
            break;
        case FFRDP_FRAME_TYPE_BYE:
            if (ffrdp->flags & FLAG_SERVER) {
                data[0] = FFRDP_FRAME_TYPE_BYE; sendto(ffrdp->udp_fd, data, 1, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
                ffrdp_reset(ffrdp);
            } else {
                ffrdp->flags |= FLAG_BYEBYE1;
                ffrdp_reset(ffrdp);
            }
            break;
        }
    }
    if (node) free(node);

    if ((ffrdp->flags & FLAG_SERVER) == 0 && (ffrdp->flags & FLAG_BYEBYE0) && (ffrdp->flags & FLAG_BYEBYE1) == 0) {
        data[0] = FFRDP_FRAME_TYPE_BYE; sendto(ffrdp->udp_fd, data, 1, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
    }

    if (got_data) { // got data frame
        while (ffrdp->recv_list_head) {
            dist = seq_distance(GET_FRAME_SEQ(ffrdp->recv_list_head), ffrdp->recv_seq);
            if (dist == 0 && ffrdp->recv_list_head->size - 4 - 2 <= (int)(sizeof(ffrdp->recv_buff) - ffrdp->recv_size)) {
                ffrdp->recv_tail = ringbuf_write(ffrdp->recv_buff, sizeof(ffrdp->recv_buff), ffrdp->recv_tail, ffrdp->recv_list_head->data + 4, ffrdp->recv_list_head->size - 4 - 2);
                ffrdp->recv_size+= ffrdp->recv_list_head->size - 4 - 2;
                ffrdp->recv_seq++; ffrdp->recv_seq &= 0xFFFFFF;
                list_remove(&ffrdp->recv_list_head, &ffrdp->recv_list_tail, ffrdp->recv_list_head, 1);
            } else break;
        }
        for (recv_mack=0,i=0,p=ffrdp->recv_list_head; i<=16&&p; i++,p=p->next) {
            dist = seq_distance(GET_FRAME_SEQ(p), ffrdp->recv_seq);
            if (dist > 1 && dist <= 16) recv_mack |= 1 << (dist - 1);
        }
        *(uint32_t*)(data + 0) = (FFRDP_FRAME_TYPE_ACK << 0) | (ffrdp->recv_seq << 8);
        *(uint32_t*)(data + 4) = (recv_mack << 0);;
        if (!ffrdp->recv_list_head || GET_FRAME_SEQ(ffrdp->recv_list_head) != ffrdp->recv_seq) {
            *(uint32_t*)(data + 4) |= (sizeof(ffrdp->recv_buff) - ffrdp->recv_size) << 16;
        }
        sendto(ffrdp->udp_fd, data, sizeof(data), 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in)); // send ack frame
    }

    if (ffrdp->send_list_head && seq_distance(send_una, GET_FRAME_SEQ(ffrdp->send_list_head)) > 0) { // got ack frame
        ffrdp->recv_win = recv_win; ffrdp->tick_query_rwin = get_tick_count(); // update rx recv window size
        for (p=ffrdp->send_list_head; p;) {
            dist= seq_distance(GET_FRAME_SEQ(p), send_una);
            for (i=15; i>=0 && !(send_mack&(1<<i)); i--);
            if (i < 0) maxack = (send_una - 1) & 0xFFFFFF;
            else maxack = (send_una + i + 1) & 0xFFFFFF;

            if (dist > 16 || !(p->flags & FLAG_FIRST_SEND)) {
                break;
            } else if (dist < 0 || (dist > 0 && (send_mack & (1 << (dist-1))))) { // this frame got ack
                ffrdp->rttm = (int32_t)get_tick_count() - (int32_t)p->tick_send;
                if (ffrdp->rtts == (uint32_t)-1) {
                    ffrdp->rtts = ffrdp->rttm;
                    ffrdp->rttd = ffrdp->rttm / 2;
                } else {
                    ffrdp->rtts = (7 * ffrdp->rtts + 1 * ffrdp->rttm) / 8;
                    ffrdp->rttd = (3 * ffrdp->rttd + 1 * abs((int)ffrdp->rttm - (int)ffrdp->rtts)) / 4;
                }
                ffrdp->rto = ffrdp->rtts + 4 * ffrdp->rttd;
                ffrdp->rto = MAX(FFRDP_MIN_RTO, ffrdp->rto);
                ffrdp->rto = MIN(FFRDP_MAX_RTO, ffrdp->rto);

                t = p; p = p->next; list_remove(&ffrdp->send_list_head, &ffrdp->send_list_tail, t, 1);
                ffrdp->wait_snd--; continue;
            } else if (seq_distance(maxack, GET_FRAME_SEQ(p)) > 0) {
                p->flags |= FLAG_FAST_RESEND;
            }
            p = p->next;
        }
    }
}

void ffrdp_dump(void *ctxt)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ctxt) return;
    printf("rttm: %u, rtts: %u, rttd: %u, rto: %u\n", ffrdp->rttm, ffrdp->rtts, ffrdp->rttd, ffrdp->rto);
    printf("recv_size           : %d\n"  , ffrdp->recv_size           );
    printf("flags               : %x\n"  , ffrdp->flags               );
    printf("send_seq            : %u\n"  , ffrdp->send_seq            );
    printf("recv_seq            : %u\n"  , ffrdp->recv_seq            );
    printf("recv_win            : %u\n"  , ffrdp->recv_win            );
    printf("wait_snd            : %u\n"  , ffrdp->wait_snd            );
    printf("tick_query_rwin     : %u\n"  , ffrdp->tick_query_rwin     );
    printf("counter_send_1sttime: %u\n"  , ffrdp->counter_send_1sttime);
    printf("counter_send_failed : %u\n"  , ffrdp->counter_send_failed );
    printf("counter_resend_rto  : %u\n"  , ffrdp->counter_resend_rto  );
    printf("counter_resend_fast : %u\n"  , ffrdp->counter_resend_fast );
    printf("counter_resend_ratio: %.2f%%\n", 100.0 * (ffrdp->counter_resend_rto + ffrdp->counter_resend_fast) / ffrdp->counter_send_1sttime);
    printf("counter_query_rwin  : %u\n"  , ffrdp->counter_query_rwin  );
#if FFRDP_ENABLE_FEC
    printf("fec_txseq           : %d\n"  , ffrdp->fec_txseq           );
    printf("fec_rxseq           : %d\n"  , ffrdp->fec_rxseq           );
    printf("fec_rxmask          : %08x\n", ffrdp->fec_rxmask          );
    printf("counter_fec_tx_short: %u\n"  , ffrdp->counter_fec_tx_short);
    printf("counter_fec_tx_full : %u\n"  , ffrdp->counter_fec_tx_full );
    printf("counter_fec_rx_short: %u\n"  , ffrdp->counter_fec_rx_short);
    printf("counter_fec_rx_full : %u\n"  , ffrdp->counter_fec_rx_full );
    printf("counter_fec_ok      : %u\n"  , ffrdp->counter_fec_ok      );
    printf("counter_fec_failed  : %u\n"  , ffrdp->counter_fec_failed  );
#endif
    printf("\r\n");
}

#if 1
static int  g_exit = 0;
static char server_bind_ip[32]   = "0.0.0.0";
static char client_cnnt_ip[32]   = "127.0.0.1";
static int  server_bind_port     = 8000;
static int  client_cnnt_port     = 8000;
static int  server_max_send_size = 16 * 1024;
static int  client_max_send_size = 16 * 1024;
static pthread_mutex_t g_mutex;
static void* server_thread(void *param)
{
    void    *ffrdp  = ffrdp_init(server_bind_ip, server_bind_port, 1);
    uint8_t *sendbuf= malloc(server_max_send_size);
    uint8_t *recvbuf= malloc(client_max_send_size);
    uint32_t tick_start, total_bytes;
    int      size, ret;

    if (!sendbuf || !recvbuf) {
        printf("server failed to allocate send or recv buffer !\n");
        goto done;
    }

    tick_start  = get_tick_count();
    total_bytes = 0;
    while (!g_exit) {
        size = 1 + rand() % server_max_send_size;
        *(uint32_t*)(sendbuf + 4) = get_tick_count();
        strcpy((char*)sendbuf + 8, "rockcarry server data");
        ret = ffrdp_send(ffrdp, (char*)sendbuf, size);
        if (ret != size) {
//          printf("server send data failed: %d\n", size);
        }

        ret = ffrdp_recv(ffrdp, (char*)recvbuf, client_max_send_size);
        if (ret > 0) {
//          printf("ret: %d\n", ret);
            total_bytes += ret;
            if ((int32_t)get_tick_count() - (int32_t)tick_start > 10 * 1000) {
                pthread_mutex_lock(&g_mutex);
                printf("server receive: %.2f KB/s\n", (float)total_bytes / 10240);
                ffrdp_dump(ffrdp);
                pthread_mutex_unlock(&g_mutex);
                tick_start = get_tick_count();
                total_bytes= 0;
            }
        }

        ffrdp_update(ffrdp);
    }

done:
    free(sendbuf);
    free(recvbuf);
    ffrdp_free(ffrdp);
    return NULL;
}

static void* client_thread(void *param)
{
    void    *ffrdp  = ffrdp_init(client_cnnt_ip, client_cnnt_port, 0);
    uint8_t *sendbuf= malloc(client_max_send_size);
    uint8_t *recvbuf= malloc(server_max_send_size);
    uint32_t tick_start, total_bytes;
    int      size, ret;

    if (!sendbuf || !recvbuf) {
        printf("client failed to allocate send or recv buffer !\n");
        goto done;
    }

    tick_start  = get_tick_count();
    total_bytes = 0;
    while (!g_exit) {
        size = 1 + rand() % client_max_send_size;
        *(uint32_t*)(sendbuf + 4) = get_tick_count();
        strcpy((char*)sendbuf + 8, "rockcarry client data");

        ret = ffrdp_send(ffrdp, (char*)sendbuf, size);
        if (ret != size) {
//          printf("client send data failed: %d\n", size);
        }

        ret = ffrdp_recv(ffrdp, (char*)recvbuf, server_max_send_size);
        if (ret > 0) {
//          printf("ret: %d\n", ret);
            total_bytes += ret;
            if ((int32_t)get_tick_count() - (int32_t)tick_start > 10 * 1000) {
                pthread_mutex_lock(&g_mutex);
                printf("client receive: %.2f KB/s\n", (float)total_bytes / 10240);
                ffrdp_dump(ffrdp);
                pthread_mutex_unlock(&g_mutex);
                tick_start = get_tick_count();
                total_bytes= 0;
            }
        }

        ffrdp_update(ffrdp);
    }

done:
    free(sendbuf);
    free(recvbuf);
    ffrdp_free(ffrdp);
    return NULL;
}

int main(int argc, char *argv[])
{
    int server_en = 0, client_en = 0, i;
    pthread_t hserver = 0, hclient = 0;
    char *str;

    if (argc <= 1) {
        printf("ffrdp test program - v1.0.0\n");
        printf("usage: ffrdp_test --server=ip:port --client=ip:port\n\n");
        return 0;
    }

    for (i=1; i<argc; i++) {
        if (strstr(argv[i], "--server=") == argv[i]) {
            server_en = 1;
            if (strcmp(argv[i] + 9, "") != 0) {
                strncpy(server_bind_ip, argv[i] + 9, sizeof(server_bind_ip));
            }
        } else if (strstr(argv[i], "--client=") == argv[i]) {
            client_en = 1;
            if (strcmp(argv[i] + 9, "") != 0) {
                strncpy(client_cnnt_ip, argv[i] + 9, sizeof(client_cnnt_ip));
            }
        } else if (strcmp(argv[i], "--server") == 0) {
            server_en = 1;
        } else if (strcmp(argv[i], "--client") == 0) {
            client_en = 1;
        } else if (strstr(argv[i], "--server_max_send_size=") == argv[i]) {
            server_max_send_size = atoi(argv[i] + 23);
        } else if (strstr(argv[i], "--client_max_send_size=") == argv[i]) {
            client_max_send_size = atoi(argv[i] + 23);
        }
    }

    strtok_s(server_bind_ip, ":", &str); if (str && *str) server_bind_port = atoi(str);
    strtok_s(client_cnnt_ip, ":", &str); if (str && *str) client_cnnt_port = atoi(str);
    if (server_en) {
        printf("server bind ip      : %s\n", server_bind_ip      );
        printf("server bind port    : %d\n", server_bind_port    );
        printf("server_max_send_size: %d\n", server_max_send_size);
    }
    if (client_en) {
        printf("client connect ip   : %s\n", client_cnnt_ip      );
        printf("client connect port : %d\n", client_cnnt_port    );
        printf("client_max_send_size: %d\n", client_max_send_size);
    }

    pthread_mutex_init(&g_mutex, NULL);
    if (server_en) pthread_create(&hserver, NULL, server_thread, NULL);
    if (client_en) pthread_create(&hclient, NULL, client_thread, NULL);

    while (!g_exit) {
        char cmd[256];
        scanf("%256s", cmd);
        if (stricmp(cmd, "quit") == 0 || stricmp(cmd, "exit") == 0) {
            g_exit = 1;
        }
    }

    if (hserver) pthread_join(hserver, NULL);
    if (hclient) pthread_join(hclient, NULL);
    pthread_mutex_destroy(&g_mutex);
    return 0;
}
#endif
