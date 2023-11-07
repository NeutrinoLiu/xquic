/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <event2/event.h>
#include <memory.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <xquic/xquic.h>
#include <xquic/xquic_typedef.h>
#include <xquic/xqc_http3.h>
#include <getopt.h>

int
printf_null(const char *format, ...)
{
    return 0;
}

#define XQC_ALPN_TRANSPORT      "transport"
#define XQC_ALPN_TRANSPORT_TEST "transport-test"


//#define printf printf_null

#define DEBUG printf("%s:%d (%s)\n", __FILE__, __LINE__, __FUNCTION__);

#define TEST_DROP (g_drop_rate != 0 && rand() % 1000 < g_drop_rate)

#define TEST_SERVER_ADDR "127.0.0.1"
#define TEST_SERVER_PORT 8443


#define XQC_PACKET_TMP_BUF_LEN 1500
#define MAX_BUF_SIZE (100*1024*1024)

#define XQC_MAX_TOKEN_LEN 256

#define XQC_TEST_SHORT_HEADER_PACKET_A "\x40\xAB\x3f\x12\x0a\xcd\xef\x00\x89"
#define XQC_TEST_SHORT_HEADER_PACKET_B "\x80\xAB\x3f\x12\x0a\xcd\xef\x00\x89"

#define MAX_HEADER 100

#define XQC_MAX_LOG_LEN 2048

typedef struct user_conn_s user_conn_t;


#define XQC_TEST_DGRAM_BATCH_SZ 32

typedef struct user_datagram_block_s {
    unsigned char *recv_data;
    unsigned char *data;
    size_t         data_len;
    size_t         data_sent;
    size_t         data_recv;
    size_t         data_lost;
    size_t         dgram_lost;
    uint32_t       dgram_id;
} user_dgram_blk_t;
typedef struct client_ctx_s {
    xqc_engine_t   *engine;
    struct event   *ev_engine;
    int             log_fd;
    int             keylog_fd;
    struct event   *ev_delay;
    struct event_base *eb;
    struct event   *ev_conc;
    int             cur_conn_num;
} client_ctx_t;

typedef struct user_stream_s {
    xqc_stream_t            *stream;
    xqc_h3_request_t        *h3_request;
    user_conn_t             *user_conn;
    uint64_t                 send_offset;
    int                      header_sent;
    int                      header_recvd;
    char                    *send_body;
    size_t                   send_body_len;
    size_t                   send_body_max;
    char                    *recv_body;
    size_t                   recv_body_len;
    FILE                    *recv_body_fp;
    int                      recv_fin;
    xqc_msec_t               start_time;
    xqc_msec_t               first_frame_time;   /* first frame download time */
    xqc_msec_t               last_read_time;
    int                      abnormal_count;
    int                      body_read_notify_cnt;
    xqc_msec_t               last_recv_log_time;
    uint64_t                 recv_log_bytes;

    xqc_h3_ext_bytestream_t *h3_ext_bs;
    struct event            *ev_bytestream_timer;

    int                      snd_times;
    int                      rcv_times;

} user_stream_t;

typedef struct user_conn_s {
    int                 fd;
    xqc_cid_t           cid;

    struct sockaddr    *local_addr;
    socklen_t           local_addrlen;
    xqc_flag_t          get_local_addr;
    struct sockaddr    *peer_addr;
    socklen_t           peer_addrlen;

    unsigned char      *token;
    unsigned            token_len;

    struct event       *ev_socket;
    struct event       *ev_timeout;
    struct event       *ev_abs_timeout;
    uint64_t            conn_create_time;

    /* 用于路径增删debug */
    struct event       *ev_path;
    struct event       *ev_epoch;

    struct event       *ev_request;


    int                 h3;

    user_dgram_blk_t   *dgram_blk;
    size_t              dgram_mss;
    uint8_t             dgram_not_supported;
    int                 dgram_retry_in_hs_cb;

    xqc_connection_t   *quic_conn;
    xqc_h3_conn_t      *h3_conn;
    client_ctx_t       *ctx;
    int                 cur_stream_num;

    uint64_t            black_hole_start_time;
    int                 tracked_pkt_cnt;
} user_conn_t;

#define XQC_DEMO_INTERFACE_MAX_LEN 64
#define XQC_DEMO_MAX_PATH_COUNT    8
#define MAX_HEADER_KEY_LEN 128
#define MAX_HEADER_VALUE_LEN 4096

typedef struct xqc_user_path_s {
    int                 path_fd;
    uint64_t            path_id;
    int                 is_in_used;
    size_t              send_size;

    struct sockaddr    *peer_addr;
    socklen_t           peer_addrlen;
    struct sockaddr    *local_addr;
    socklen_t           local_addrlen;

    struct event       *ev_socket;

    int                 rebinding_path_fd;
    struct event       *rebinding_ev_socket;
} xqc_user_path_t;


typedef struct {
    double p;
    int val;
} cdf_entry_t;


static char *g_server_addr = NULL;
int g_server_port = TEST_SERVER_PORT;
int g_transport = 0;
int g_conn_count = 0;
int g_max_conn_num = 1000;
int g_conn_num = 100;
int g_process_num = 2;
int g_test_qch_mode = 0;
int g_random_cid = 0;
xqc_data_qos_level_t g_dgram_qos_level;
xqc_conn_settings_t *g_conn_settings;

size_t dgram1_size = 0;
size_t dgram2_size = 0;

int dgram_drop_pkt1 = 0;
client_ctx_t ctx;
struct event_base *eb;
int g_send_dgram;
int g_max_dgram_size;
int g_req_cnt;
int g_bytestream_cnt;
int g_req_max;
int g_send_body_size;
int g_send_body_size_defined;
int g_send_body_size_from_cdf;
cdf_entry_t *cdf_list;
int cdf_list_size;
int g_req_paral = 1;
int g_recovery = 0;
int g_save_body;
int g_read_body;
int g_echo_check;
int g_drop_rate;
int g_spec_url;
int g_is_get;
uint64_t g_last_sock_op_time;
//currently, the maximum used test case id is 19
//please keep this comment updated if you are adding more test cases. :-D
//99 for pure fin
//2XX for datagram testcases
//3XX for h3 ext bytestream testcases
//4XX for conn_settings configuration
int g_test_case;
int g_ipv6;
int g_no_crypt;
int g_conn_timeout = 1;
int g_conn_abs_timeout = 0;
int g_path_timeout = 5000000; /* 5s */
int g_epoch_timeout = 1000000; /* us */
char g_write_file[256];
char g_read_file[256];
char g_log_path[256];
char g_host[64] = "test.xquic.com";
char g_url_path[256] = "/path/resource";
char g_scheme[8] = "https";
char g_url[2048];
char g_headers[MAX_HEADER][256];
int g_header_cnt = 0;
int g_ping_id = 1;
int g_enable_multipath = 0;
xqc_multipath_version_t g_multipath_version = XQC_MULTIPATH_04;
int g_enable_reinjection = 0;
int g_verify_cert = 0;
int g_verify_cert_allow_self_sign = 0;
int g_header_num = 6;
int g_epoch = 0;
int g_cur_epoch = 0;
int g_mp_backup_mode = 0;
int g_mp_request_accelerate = 0;
double g_copa_ai = 1.0;
double g_copa_delta = 0.05;
int g_pmtud_on = 0;
int g_mp_ping_on = 0;
char g_header_key[MAX_HEADER_KEY_LEN];
char g_header_value[MAX_HEADER_VALUE_LEN];

char g_multi_interface[XQC_DEMO_MAX_PATH_COUNT][64];
xqc_user_path_t g_client_path[XQC_DEMO_MAX_PATH_COUNT];
int g_multi_interface_cnt = 0;
int mp_has_closed = 0;
int mp_has_recved = 0;
char g_priority[64] = {'\0'};

/* 用于路径增删debug */
int g_debug_path = 0;

#define XQC_TEST_LONG_HEADER_LEN 32769
char test_long_value[XQC_TEST_LONG_HEADER_LEN] = {'\0'};

int hsk_completed = 0;


int g_periodically_request = 0;

static uint64_t last_recv_ts = 0;

static inline uint64_t 
now()
{
    /* get microsecond unit time */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t ul = tv.tv_sec * (uint64_t)1000000 + tv.tv_usec;
    return  ul;
}

static void xqc_client_socket_event_callback(int fd, short what, void *arg);
static void xqc_client_timeout_callback(int fd, short what, void *arg);

static void xqc_client_timeout_multi_process_callback(int fd, short what, void *arg);

void
xqc_client_set_event_timer(xqc_msec_t wake_after, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *) user_data;
    //printf("xqc_engine_wakeup_after %llu us, now %llu\n", wake_after, now());

    struct timeval tv;
    tv.tv_sec = wake_after / 1000000;
    tv.tv_usec = wake_after % 1000000;
    event_add(ctx->ev_engine, &tv);

}

void
save_session_cb(const char * data, size_t data_len, void *user_data)
{
    user_conn_t *user_conn = (user_conn_t*)user_data;
    printf("save_session_cb use server domain as the key. h3[%d]\n", user_conn->h3);

    FILE * fp  = fopen("test_session", "wb");
    int write_size = fwrite(data, 1, data_len, fp);
    if (data_len != write_size) {
        printf("save _session_cb error\n");
        fclose(fp);
        return;
    }
    fclose(fp);
    return;
}


void
save_tp_cb(const char * data, size_t data_len, void * user_data)
{
    user_conn_t *user_conn = (user_conn_t*)user_data;
    printf("save_tp_cb use server domain as the key. h3[%d]\n", user_conn->h3);

    FILE * fp = fopen("tp_localhost", "wb");
    int write_size = fwrite(data, 1, data_len, fp);
    if (data_len != write_size) {
        printf("save _tp_cb error\n");
        fclose(fp);
        return;
    }
    fclose(fp);
    return;
}

void
xqc_client_save_token(const unsigned char *token, unsigned token_len, void *user_data)
{
    user_conn_t *user_conn = (user_conn_t*)user_data;
    printf("xqc_client_save_token use client ip as the key. h3[%d]\n", user_conn->h3);

    int fd = open("./xqc_token", O_TRUNC | O_CREAT | O_WRONLY, S_IRWXU);
    if (fd < 0) {
        printf("save token error %s\n", strerror(errno));
        return;
    }

    ssize_t n = write(fd, token, token_len);
    if (n < token_len) {
        printf("save token error %s\n", strerror(errno));
        close(fd);
        return;
    }
    close(fd);
}

int
xqc_client_read_token(unsigned char *token, unsigned token_len)
{
    int fd = open("./xqc_token", O_RDONLY);
    if (fd < 0) {
        printf("read token error %s\n", strerror(errno));
        return -1;
    }

    ssize_t n = read(fd, token, token_len);
    printf("read token size %zu\n", n);
    close(fd);
    return n;
}

int
read_file_data(char *data, size_t data_len, char *filename)
{
    int ret = 0;
    size_t total_len, read_len;
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
        ret = -1;
        goto end;
    }

    fseek(fp, 0, SEEK_END);
    total_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (total_len > data_len) {
        ret = -1;
        goto end;
    }

    read_len = fread(data, 1, total_len, fp);
    if (read_len != total_len) {
        ret = -1;
        goto end;
    }

    ret = read_len;

end:
    if (fp) {
        fclose(fp);
    }
    return ret;
}

ssize_t 
xqc_client_write_socket(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *user)
{
    user_conn_t *user_conn = (user_conn_t *) user;
    ssize_t res = 0;
    int fd = user_conn->fd;

    /* COPY to run corruption test cases */
    unsigned char send_buf[XQC_PACKET_TMP_BUF_LEN];
    size_t send_buf_size = 0;

    if (size > XQC_PACKET_TMP_BUF_LEN) {
        printf("xqc_client_write_socket err: size=%zu is too long\n", size);
        return XQC_SOCKET_ERROR;
    }
    send_buf_size = size;
    memcpy(send_buf, buf, send_buf_size);


    do {
        errno = 0;

        g_last_sock_op_time = now();

        if (TEST_DROP) {
            return send_buf_size;
        }

        res = sendto(fd, send_buf, send_buf_size, 0, peer_addr, peer_addrlen);
        if (res < 0) {
            printf("xqc_client_write_socket err %zd %s\n", res, strerror(errno));
            if (errno == EAGAIN) {
                res = XQC_SOCKET_EAGAIN;
            }
            if (errno == EMSGSIZE) {
                res = send_buf_size;
            }
        }

    } while ((res < 0) && (errno == EINTR));

    return res;
}

int
xqc_client_get_path_fd_by_id(user_conn_t *user_conn, uint64_t path_id)
{
    int fd = user_conn->fd;

    if (!g_enable_multipath) {
        return fd;
    }

    for (int i = 0; i < g_multi_interface_cnt; i++) {
        if (g_client_path[i].path_id == path_id) {
            fd = g_client_path[i].path_fd;
            break;
        }
    }

    return fd;
}

/* 多路必须保证传正确的path id，因为conn_fd写死了，跟initial path不一定匹配 */
ssize_t
xqc_client_write_socket_ex(uint64_t path_id,
    const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr,
    socklen_t peer_addrlen, void *user_data)
{
    user_conn_t *user_conn = (user_conn_t *)user_data;
    ssize_t res;
    int fd = 0;


    /* get path fd */
    fd = xqc_client_get_path_fd_by_id(user_conn, path_id);

    /* COPY to run corruption test cases */
    unsigned char send_buf[XQC_PACKET_TMP_BUF_LEN];
    size_t send_buf_size = 0;
    
    if (size > XQC_PACKET_TMP_BUF_LEN) {
        printf("xqc_client_write_socket err: size=%zu is too long\n", size);
        return XQC_SOCKET_ERROR;
    }
    send_buf_size = size;
    memcpy(send_buf, buf, send_buf_size);

    if (g_enable_multipath) {
        g_client_path[path_id].send_size += size;
    }

    do {
        errno = 0;

        g_last_sock_op_time = now();

        if (TEST_DROP) {
            return send_buf_size;
        }

        res = sendto(fd, send_buf, send_buf_size, 0, peer_addr, peer_addrlen);
        if (res < 0) {
            if (errno == EAGAIN) {
                // printf("-");
                res = XQC_SOCKET_EAGAIN;
                continue;
            } else {
                res = XQC_SOCKET_ERROR;
            }
            printf("[ts:%zu]xqc_client_write_socket_ex path:%"PRIu64" err %zd %s %zu\n", now(), path_id, res, strerror(errno), send_buf_size);
            if (errno == EMSGSIZE) {
                res = send_buf_size;
            }
        } else {
                // printf(">");
        }

    } while ((res < 0) && (errno == EINTR));

    return res;
}

xqc_int_t 
xqc_client_conn_closing_notify(xqc_connection_t *conn,
    const xqc_cid_t *cid, xqc_int_t err_code, void *conn_user_data)
{
    printf("conn closing: %d\n", err_code);
    return XQC_OK;
}

static int
xqc_client_bind_to_interface(int fd, 
    const char *interface_name)
{
    struct ifreq ifr;
    memset(&ifr, 0x00, sizeof(ifr));
    strncpy(ifr.ifr_name, interface_name, sizeof(ifr.ifr_name) - 1);

#if !defined(__APPLE__)
// #if (XQC_TEST_MP)
    printf("fd: %d. bind to nic: %s\n", fd, interface_name);
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, (char *)&ifr, sizeof(ifr)) < 0) {
        printf("bind to nic error: %d, try use sudo\n", errno);
        return XQC_ERROR;
    }
// #endif
#endif

    return XQC_OK;
}

static int 
xqc_client_create_socket(int type, 
    const struct sockaddr *saddr, socklen_t saddr_len, char *interface)
{
    int size;
    int fd = -1;

    /* create fd & set socket option */
    fd = socket(type, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("create socket failed, errno: %d\n", errno);
        return -1;
    }

    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
        printf("set socket nonblock failed, errno: %d\n", errno);
        goto err;
    }

    size = 1 * 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(int)) < 0) {
        printf("setsockopt failed, errno: %d\n", errno);
        goto err;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(int)) < 0) {
        printf("setsockopt failed, errno: %d\n", errno);
        goto err;
    }

#if !defined(__APPLE__)
    int val = IP_PMTUDISC_DO;
    setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
#endif

    g_last_sock_op_time = now();

    if (interface != NULL
        && xqc_client_bind_to_interface(fd, interface) < 0) 
    {
        printf("|xqc_client_bind_to_interface error|");
        goto err;
    }

    /* connect to peer addr */
#if !defined(__APPLE__)
    if (connect(fd, (struct sockaddr *)saddr, saddr_len) < 0) {
        printf("connect socket failed, errno: %d\n", errno);
        goto err;
    }
#endif

    return fd;

err:
    close(fd);
    return -1;
}


void 
xqc_convert_addr_text_to_sockaddr(int type,
    const char *addr_text, unsigned int port,
    struct sockaddr **saddr, socklen_t *saddr_len)
{
    if (type == AF_INET6) {
        *saddr = calloc(1, sizeof(struct sockaddr_in6));
        memset(*saddr, 0, sizeof(struct sockaddr_in6));
        struct sockaddr_in6 *addr_v6 = (struct sockaddr_in6 *)(*saddr);
        inet_pton(type, addr_text, &(addr_v6->sin6_addr.s6_addr));
        addr_v6->sin6_family = type;
        addr_v6->sin6_port = htons(port);
        *saddr_len = sizeof(struct sockaddr_in6);

    } else {
        *saddr = calloc(1, sizeof(struct sockaddr_in));
        memset(*saddr, 0, sizeof(struct sockaddr_in));
        struct sockaddr_in *addr_v4 = (struct sockaddr_in *)(*saddr);
        inet_pton(type, addr_text, &(addr_v4->sin_addr.s_addr));
        addr_v4->sin_family = type;
        addr_v4->sin_port = htons(port);
        *saddr_len = sizeof(struct sockaddr_in);
    }
}

void
xqc_client_init_addr(user_conn_t *user_conn,
    const char *server_addr, int server_port)
{
    int ip_type = (g_ipv6 ? AF_INET6 : AF_INET);
    xqc_convert_addr_text_to_sockaddr(ip_type, 
                                      server_addr, server_port,
                                      &user_conn->peer_addr, 
                                      &user_conn->peer_addrlen);

    if (ip_type == AF_INET6) {
        user_conn->local_addr = (struct sockaddr *)calloc(1, sizeof(struct sockaddr_in6));
        memset(user_conn->local_addr, 0, sizeof(struct sockaddr_in6));
        user_conn->local_addrlen = sizeof(struct sockaddr_in6);

    } else {
        user_conn->local_addr = (struct sockaddr *)calloc(1, sizeof(struct sockaddr_in));
        memset(user_conn->local_addr, 0, sizeof(struct sockaddr_in));
        user_conn->local_addrlen = sizeof(struct sockaddr_in);
    }
}


static int
xqc_client_create_path_socket(xqc_user_path_t *path,
    char *path_interface)
{
    path->path_fd = xqc_client_create_socket((g_ipv6 ? AF_INET6 : AF_INET), 
                                             path->peer_addr, path->peer_addrlen, path_interface);
    if (path->path_fd < 0) {
        printf("|xqc_client_create_path_socket error|");
        return XQC_ERROR;
    }

    return XQC_OK;
}


static int
xqc_client_create_path(xqc_user_path_t *path, 
    char *path_interface, user_conn_t *user_conn)
{
    path->path_id = 0;
    path->is_in_used = 0;

    path->peer_addr = calloc(1, user_conn->peer_addrlen);
    memcpy(path->peer_addr, user_conn->peer_addr, user_conn->peer_addrlen);
    path->peer_addrlen = user_conn->peer_addrlen;
    
    if (xqc_client_create_path_socket(path, path_interface) < 0) {
        printf("xqc_client_create_path_socket error\n");
        return XQC_ERROR;
    }
    
    path->ev_socket = event_new(eb, path->path_fd, 
                EV_READ | EV_PERSIST, xqc_client_socket_event_callback, user_conn);
    event_add(path->ev_socket, NULL);

    return XQC_OK;
}

user_conn_t * 
xqc_client_user_conn_create(const char *server_addr, int server_port,
    int transport)
{
    user_conn_t *user_conn = calloc(1, sizeof(user_conn_t));

    /* use HTTP3? */
    user_conn->h3 = transport;

    user_conn->ev_timeout = event_new(eb, -1, 0, xqc_client_timeout_callback, user_conn);
    /* set connection timeout */
    struct timeval tv;
    tv.tv_sec = g_conn_timeout;
    tv.tv_usec = 0;
    event_add(user_conn->ev_timeout, &tv);

    user_conn->conn_create_time = now();

    int ip_type = (g_ipv6 ? AF_INET6 : AF_INET);
    xqc_client_init_addr(user_conn, server_addr, server_port);
                                      
    return user_conn;
}

int
xqc_client_create_conn_socket(user_conn_t *user_conn)
{
    int ip_type = (g_ipv6 ? AF_INET6 : AF_INET);
    user_conn->fd = xqc_client_create_socket(ip_type, 
                                             user_conn->peer_addr, user_conn->peer_addrlen, NULL);
    if (user_conn->fd < 0) {
        printf("xqc_create_socket error\n");
        return -1;
    }

    user_conn->ev_socket = event_new(eb, user_conn->fd, EV_READ | EV_PERSIST, 
                                     xqc_client_socket_event_callback, user_conn);
    event_add(user_conn->ev_socket, NULL);

    return 0;
}

void
xqc_client_path_removed(const xqc_cid_t *scid, uint64_t path_id,
    void *conn_user_data)
{
    user_conn_t *user_conn = (user_conn_t *) conn_user_data;

    if (!g_enable_multipath) {
        return;
    }

    for (int i = 0; i < g_multi_interface_cnt; i++) {
        if (g_client_path[i].path_id == path_id) {
            g_client_path[i].path_id = 0;
            g_client_path[i].is_in_used = 0;
            
            printf("***** path removed. index: %d, path_id: %" PRIu64 "\n", i, path_id);
            break;
        }
    }
}


int
xqc_client_conn_create_notify(xqc_connection_t *conn, const xqc_cid_t *cid, void *user_data, void *conn_proto_data)
{
    DEBUG;

    user_conn_t *user_conn = (user_conn_t *)user_data;
    xqc_conn_set_alp_user_data(conn, user_conn);

    user_conn->dgram_mss = xqc_datagram_get_mss(conn);
    user_conn->quic_conn = conn;

    printf("xqc_conn_is_ready_to_send_early_data:%d\n", xqc_conn_is_ready_to_send_early_data(conn));
    return 0;
}

int
xqc_client_conn_close_notify(xqc_connection_t *conn, const xqc_cid_t *cid, void *user_data, void *conn_proto_data)
{
    DEBUG;

    user_conn_t *user_conn = (user_conn_t *)user_data;

    client_ctx_t *p_ctx;
    p_ctx = &ctx;

    xqc_int_t err = xqc_conn_get_errno(conn);
    printf("should_clear_0rtt_ticket, conn_err:%d, clear_0rtt_ticket:%d\n", err, xqc_conn_should_clear_0rtt_ticket(err));

    xqc_conn_stats_t stats = xqc_conn_get_stats(p_ctx->engine, cid);
    printf("send_count:%u, lost_count:%u, tlp_count:%u, recv_count:%u, srtt:%"PRIu64" early_data_flag:%d, conn_err:%d, mp_state:%d, ack_info:%s, alpn:%s\n",
           stats.send_count, stats.lost_count, stats.tlp_count, stats.recv_count, stats.srtt, stats.early_data_flag, stats.conn_err, stats.mp_state, stats.ack_info, stats.alpn);

    printf("conn_info: \"%s\"\n", stats.conn_info);


    event_base_loopbreak(eb);
    return 0;
}

void
xqc_client_conn_ping_acked_notify(xqc_connection_t *conn, const xqc_cid_t *cid, void *ping_user_data, void *user_data, void *conn_proto_data)
{
    DEBUG;
    if (ping_user_data) {
        printf("====>ping_id:%d\n", *(int *) ping_user_data);

    } else {
        printf("====>no ping_id\n");
    }
}

void
xqc_client_conn_update_cid_notify(xqc_connection_t *conn, const xqc_cid_t *retire_cid, const xqc_cid_t *new_cid, void *user_data)
{
    DEBUG;

    user_conn_t *user_conn = (user_conn_t *) user_data;

    memcpy(&user_conn->cid, new_cid, sizeof(*new_cid));

    printf("====>RETIRE SCID:%s\n", xqc_scid_str(retire_cid));
    printf("====>SCID:%s\n", xqc_scid_str(new_cid));
    printf("====>DCID:%s\n", xqc_dcid_str_by_scid(ctx.engine, new_cid));

}

void
xqc_client_conn_handshake_finished(xqc_connection_t *conn, void *user_data, void *conn_proto_data)
{
    DEBUG;
    user_conn_t *user_conn = (user_conn_t *) user_data;
    if (!g_mp_ping_on) {
        xqc_conn_send_ping(ctx.engine, &user_conn->cid, NULL);
        xqc_conn_send_ping(ctx.engine, &user_conn->cid, &g_ping_id);
    }  

    printf("====>DCID:%s\n", xqc_dcid_str_by_scid(ctx.engine, &user_conn->cid));
    printf("====>SCID:%s\n", xqc_scid_str(&user_conn->cid));

    hsk_completed = 1;

    user_conn->dgram_mss = xqc_datagram_get_mss(conn);
    if (user_conn->dgram_mss == 0) {
        user_conn->dgram_not_supported = 1; 
    }
}

int
xqc_client_stream_send(xqc_stream_t *stream, void *user_data)
{
    static int send_cnt = 0;
    printf("|xqc_client_stream_send|cnt:%d|\n", ++send_cnt);

    ssize_t ret;
    user_stream_t *user_stream = (user_stream_t *) user_data;

    if (user_stream->start_time == 0) {
        user_stream->start_time = now();
    }

    if (user_stream->send_body == NULL) {
        user_stream->send_body_max = MAX_BUF_SIZE;
        if (g_read_body) {
            user_stream->send_body = malloc(user_stream->send_body_max);
        } else {
            user_stream->send_body = malloc(g_send_body_size);
            memset(user_stream->send_body, 1, g_send_body_size);
        }
        if (user_stream->send_body == NULL) {
            printf("send_body malloc error\n");
            return -1;
        }

        /* specified size > specified file > default size */
        if (g_send_body_size_defined) {
            user_stream->send_body_len = g_send_body_size;
        } else if (g_read_body) {
            ret = read_file_data(user_stream->send_body, user_stream->send_body_max, g_read_file);
            if (ret < 0) {
                printf("read body error\n");
                return -1;
            } else {
                user_stream->send_body_len = ret;
            }
        } else {
            user_stream->send_body_len = g_send_body_size;
        }
    }

    int fin = 1;

    if (user_stream->send_offset < user_stream->send_body_len) {
        ret = xqc_stream_send(stream, user_stream->send_body + user_stream->send_offset, user_stream->send_body_len - user_stream->send_offset, fin);
        if (ret < 0) {
            printf("xqc_stream_send error %zd\n", ret);
            return 0;

        } else {
            user_stream->send_offset += ret;
            printf("xqc_stream_send offset=%"PRIu64"\n", user_stream->send_offset);
        }
    }

    return 0;
}

int
xqc_client_stream_write_notify(xqc_stream_t *stream, void *user_data)
{
    static int write_notify_cnt = 0;
    printf("|xqc_client_stream_write_notify|cnt:%d|\n", ++write_notify_cnt);

    //DEBUG;
    int ret = 0;
    user_stream_t *user_stream = (user_stream_t *) user_data;
    ret = xqc_client_stream_send(stream, user_stream);
    return ret;
}

int
xqc_client_stream_read_notify(xqc_stream_t *stream, void *user_data)
{
    //DEBUG;
    unsigned char fin = 0;
    user_stream_t *user_stream = (user_stream_t *) user_data;
    char buff[4096] = {0};
    size_t buff_size = 4096;
    int save = g_save_body;

    if (save && user_stream->recv_body_fp == NULL) {
        user_stream->recv_body_fp = fopen(g_write_file, "wb");
        if (user_stream->recv_body_fp == NULL) {
            printf("open error\n");
            return -1;
        }
    }

    if (g_echo_check && user_stream->recv_body == NULL) {
        user_stream->recv_body = malloc(user_stream->send_body_len);
        if (user_stream->recv_body == NULL) {
            printf("recv_body malloc error\n");
            return -1;
        }
    }

    ssize_t read;
    ssize_t read_sum = 0;

    do {
        read = xqc_stream_recv(stream, buff, buff_size, &fin);
        if (read == -XQC_EAGAIN) {
            break;

        } else if (read < 0) {
            printf("xqc_stream_recv error %zd\n", read);
            return 0;
        }

        if (save && fwrite(buff, 1, read, user_stream->recv_body_fp) != read) {
            printf("fwrite error\n");
            return -1;
        }
        if (save) fflush(user_stream->recv_body_fp);

        /* write received body to memory */
        if (g_echo_check && user_stream->recv_body_len + read <= user_stream->send_body_len) {
            memcpy(user_stream->recv_body + user_stream->recv_body_len, buff, read);
        }

        read_sum += read;
        user_stream->recv_body_len += read;
        user_stream->recv_log_bytes += read;

        xqc_msec_t curr_time = now();
        if ((curr_time - user_stream->last_recv_log_time) >= 200000) {
            printf("\n[qperf]|ts:%"PRIu64"|recv_size:%"PRIu64"|\n", curr_time, user_stream->recv_log_bytes);
            user_stream->last_recv_log_time = curr_time;
            user_stream->recv_log_bytes = 0;
        }

    } while (read > 0 && !fin);

    // mpshell
    // printf("xqc_stream_recv read:%zd, offset:%zu, fin:%d\n", read_sum, user_stream->recv_body_len, fin);

    /* test first frame rendering time */

    if (fin) {
        user_stream->recv_fin = 1;
        xqc_msec_t now_us = now();
        printf("\033[33m>>>>>>>> request time cost:%"PRIu64" us, speed:%"PRIu64" K/s \n"
               ">>>>>>>> send_body_size:%zu, recv_body_size:%zu \033[0m\n",
               now_us - user_stream->start_time,
               (user_stream->send_body_len + user_stream->recv_body_len)*1000/(now_us - user_stream->start_time),
               user_stream->send_body_len, user_stream->recv_body_len);
        
        printf("test_result_speed: %"PRIu64" K/s\n", 
                (user_stream->send_body_len + user_stream->recv_body_len)*1000/(now_us - user_stream->start_time));

        printf("[rr_benchmark]|request_time:%"PRIu64"|"
               "request_size:%zu|response_size:%zu|\n",
               now_us - user_stream->start_time,
               user_stream->send_body_len, user_stream->recv_body_len);


        /* write to eval file */
        /*{
            FILE* fp = NULL;
            fp = fopen("eval_result.txt", "a+");
            if (fp == NULL) {
                exit(1);
            }

            fprintf(fp, "recv_size: %lu; cost_time: %lu\n", stats.recv_body_size, (uint64_t)((now_us - user_stream->start_time)/1000));
            fclose(fp);

            exit(0);
        }*/

    }
    return 0;
}

int
xqc_client_stream_close_notify(xqc_stream_t *stream, void *user_data)
{
    DEBUG;
    user_stream_t *user_stream = (user_stream_t*)user_data;
    if (g_echo_check) {
        int pass = 0;
        printf("user_stream->recv_fin:%d, user_stream->send_body_len:%zu, user_stream->recv_body_len:%zd\n",
               user_stream->recv_fin, user_stream->send_body_len, user_stream->recv_body_len);
        if (user_stream->recv_fin && user_stream->send_body_len == user_stream->recv_body_len
            && memcmp(user_stream->send_body, user_stream->recv_body, user_stream->send_body_len) == 0) {
            pass = 1;
        }
        printf(">>>>>>>> pass:%d\n", pass);
    }

    free(user_stream->send_body);
    free(user_stream->recv_body);
    free(user_stream);
    return 0;
}

void
xqc_client_socket_write_handler(user_conn_t *user_conn)
{
    DEBUG
    client_ctx_t *p_ctx;
    p_ctx = &ctx;
    xqc_conn_continue_send(p_ctx->engine, &user_conn->cid);
}


void
xqc_client_socket_read_handler(user_conn_t *user_conn, int fd)
{
    //DEBUG;

    xqc_int_t ret;
    ssize_t recv_size = 0;
    ssize_t recv_sum = 0;
    uint64_t path_id = XQC_UNKNOWN_PATH_ID;
    xqc_user_path_t *path;
    int i;

    client_ctx_t *p_ctx;
    p_ctx = &ctx;

    for (i = 0; i < g_multi_interface_cnt; i++) {
        path = &g_client_path[i];
        if (path->path_fd == fd || path->rebinding_path_fd == fd) {
            path_id = path->path_id;
        }
    }

#ifdef __linux__
    int batch = 0;
    if (batch) {
#define VLEN 100
#define BUFSIZE XQC_PACKET_TMP_BUF_LEN
#define TIMEOUT 10
        struct sockaddr_in6 pa[VLEN];
        struct mmsghdr msgs[VLEN];
        struct iovec iovecs[VLEN];
        char bufs[VLEN][BUFSIZE+1];
        struct timespec timeout;
        int retval;

        do {
            memset(msgs, 0, sizeof(msgs));
            for (int i = 0; i < VLEN; i++) {
                iovecs[i].iov_base = bufs[i];
                iovecs[i].iov_len = BUFSIZE;
                msgs[i].msg_hdr.msg_iov = &iovecs[i];
                msgs[i].msg_hdr.msg_iovlen = 1;
                msgs[i].msg_hdr.msg_name = &pa[i];
                msgs[i].msg_hdr.msg_namelen = user_conn->peer_addrlen;
            }

            timeout.tv_sec = TIMEOUT;
            timeout.tv_nsec = 0;

            retval = recvmmsg(fd, msgs, VLEN, 0, &timeout);
            if (retval == -1) {
                break;
            }

            uint64_t recv_time = now();
            for (int i = 0; i < retval; i++) {
                recv_sum += msgs[i].msg_len;

#ifdef XQC_NO_PID_PACKET_PROCESS
                ret = xqc_engine_packet_process(p_ctx->engine, iovecs[i].iov_base, msgs[i].msg_len,
                                              user_conn->local_addr, user_conn->local_addrlen,
                                              user_conn->peer_addr, user_conn->peer_addrlen,
                                              (xqc_msec_t)recv_time, user_conn);
#else
                ret = xqc_engine_packet_process(p_ctx->engine, iovecs[i].iov_base, msgs[i].msg_len,
                                              user_conn->local_addr, user_conn->local_addrlen,
                                              user_conn->peer_addr, user_conn->peer_addrlen,
                                              path_id, (xqc_msec_t)recv_time, user_conn);
#endif                                              
                if (ret != XQC_OK)
                {
                    printf("xqc_server_read_handler: packet process err, ret: %d\n", ret);
                    return;
                }
            }
        } while (retval > 0);
        goto finish_recv;
    }
#endif

    unsigned char packet_buf[XQC_PACKET_TMP_BUF_LEN];

    static ssize_t last_rcv_sum = 0;
    static ssize_t rcv_sum = 0;

    do {
        recv_size = recvfrom(fd,
                             packet_buf, sizeof(packet_buf), 0, 
                             user_conn->peer_addr, &user_conn->peer_addrlen);
        if (recv_size < 0 && errno == EAGAIN) {
            break;
        }

        if (recv_size < 0) {
            printf("recvfrom: recvmsg = %zd(%s)\n", recv_size, strerror(errno));
            break;
        }

        /* if recv_size is 0, break while loop, */
        if (recv_size == 0) {
            break;
        }

        recv_sum += recv_size;
        rcv_sum += recv_size;

        if (user_conn->get_local_addr == 0) {
            user_conn->get_local_addr = 1;
            socklen_t tmp = sizeof(struct sockaddr_in6);
            int ret = getsockname(user_conn->fd, (struct sockaddr *) user_conn->local_addr, &tmp);
            if (ret < 0) {
                printf("getsockname error, errno: %d\n", errno);
                break;
            }
            user_conn->local_addrlen = tmp;
        }

        uint64_t recv_time = now();
        g_last_sock_op_time = recv_time;


        if (TEST_DROP) continue;

#ifdef XQC_NO_PID_PACKET_PROCESS
        ret = xqc_engine_packet_process(p_ctx->engine, packet_buf, recv_size,
                                      user_conn->local_addr, user_conn->local_addrlen,
                                      user_conn->peer_addr, user_conn->peer_addrlen,
                                      (xqc_msec_t)recv_time, user_conn);
#else
        ret = xqc_engine_packet_process(p_ctx->engine, packet_buf, recv_size,
                                      user_conn->local_addr, user_conn->local_addrlen,
                                      user_conn->peer_addr, user_conn->peer_addrlen,
                                      path_id, (xqc_msec_t)recv_time, user_conn);
#endif                                      
        if (ret != XQC_OK) {
            printf("xqc_client_read_handler: packet process err, ret: %d\n", ret);
            return;
        }

    } while (recv_size > 0);

    if ((now() - last_recv_ts) > 200000) {
        // mpshell
        // printf("recving rate: %.3lf Kbps\n", (rcv_sum - last_rcv_sum) * 8.0 * 1000 / (now() - last_recv_ts));
        last_recv_ts = now();
        last_rcv_sum = rcv_sum;
    }

finish_recv:
    // mpshell: 批量测试，无需打印
    // printf("recvfrom size:%zu\n", recv_sum);
    xqc_engine_finish_recv(p_ctx->engine);
}


static void
xqc_client_socket_event_callback(int fd, short what, void *arg)
{
    //DEBUG;
    user_conn_t *user_conn = (user_conn_t *) arg;

    if (what & EV_WRITE) {
        xqc_client_socket_write_handler(user_conn);

    } else if (what & EV_READ) {
        xqc_client_socket_read_handler(user_conn, fd);

    } else {
        printf("event callback: what=%d\n", what);
        exit(1);
    }
}


static void
xqc_client_engine_callback(int fd, short what, void *arg)
{
    // mpshell: 批量测试，无需打印
    // printf("engine timer wakeup now:%"PRIu64"\n", now());
    client_ctx_t *ctx = (client_ctx_t *) arg;

    xqc_engine_main_logic(ctx->engine);
}


static void xqc_client_timeout_multi_process_callback(int fd, short what, void *arg)
{
    user_conn_t *user_conn = (user_conn_t *) arg;
    int rc;
    client_ctx_t *ctx = user_conn->ctx;

    rc = xqc_conn_close(ctx->engine, &user_conn->cid);
    if (rc) {
        printf("xqc_conn_close error\n");
        return;
    }
    ctx->cur_conn_num--;

    printf("xqc_conn_close, %d connetion rest\n", ctx->cur_conn_num);
}

static void
xqc_client_timeout_callback(int fd, short what, void *arg)
{
    // mpshell
    // printf("xqc_client_timeout_callback now %"PRIu64"\n", now());
    user_conn_t *user_conn = (user_conn_t *) arg;
    int rc;
    static int restart_after_a_while = 1;

    /* write to eval file */
    /*{
        FILE* fp = NULL;
        fp = fopen("eval_result.txt", "a+");
        if (fp == NULL) {
            exit(1);
        }

        fprintf(fp, "recv_size: %u; cost_time: %u\n", 11, 60 * 1000);
        fclose(fp);

    }*/

    if (now() - g_last_sock_op_time < (uint64_t)g_conn_timeout * 1000000) {
        struct timeval tv;
        tv.tv_sec = g_conn_timeout;
        tv.tv_usec = 0;
        event_add(user_conn->ev_timeout, &tv);
        return;
    }

conn_close:

    printf("xqc_client_timeout_callback | conn_close\n");
    rc = xqc_conn_close(ctx.engine, &user_conn->cid);
    if (rc) {
        printf("xqc_conn_close error\n");
        return;
    }
    //event_base_loopbreak(eb);
}


int
xqc_client_open_log_file(void *engine_user_data)
{
    client_ctx_t *ctx = (client_ctx_t*)engine_user_data;
    //ctx->log_fd = open("/home/jiuhai.zjh/ramdisk/clog", (O_WRONLY | O_APPEND | O_CREAT), 0644);
    ctx->log_fd = open(g_log_path, (O_WRONLY | O_APPEND | O_CREAT), 0644);
    if (ctx->log_fd <= 0) {
        return -1;
    }
    return 0;
}

int
xqc_client_close_log_file(void *engine_user_data)
{
    client_ctx_t *ctx = (client_ctx_t*)engine_user_data;
    if (ctx->log_fd <= 0) {
        return -1;
    }
    close(ctx->log_fd);
    return 0;
}


void 
xqc_client_write_log(xqc_log_level_t lvl, const void *buf, size_t count, void *engine_user_data)
{
    unsigned char log_buf[XQC_MAX_LOG_LEN + 1];

    client_ctx_t *ctx = (client_ctx_t*)engine_user_data;
    if (ctx->log_fd <= 0) {
        printf("xqc_client_write_log fd err\n");
        return;
    }

    int log_len = snprintf(log_buf, XQC_MAX_LOG_LEN + 1, "%s\n", (char *)buf);
    if (log_len < 0) {
        printf("xqc_client_write_log err\n");
        return;
    }

    int write_len = write(ctx->log_fd, log_buf, log_len);
    if (write_len < 0) {
        printf("write log failed, errno: %d\n", errno);
    }
}


/**
 * key log functions
 */

int
xqc_client_open_keylog_file(client_ctx_t *ctx)
{
    ctx->keylog_fd = open("./ckeys.log", (O_WRONLY | O_APPEND | O_CREAT), 0644);
    if (ctx->keylog_fd <= 0) {
        return -1;
    }

    return 0;
}

int
xqc_client_close_keylog_file(client_ctx_t *ctx)
{
    if (ctx->keylog_fd <= 0) {
        return -1;
    }

    close(ctx->keylog_fd);
    ctx->keylog_fd = 0;
    return 0;
}


void 
xqc_keylog_cb(const xqc_cid_t *scid, const char *line, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t*)user_data;
    if (ctx->keylog_fd <= 0) {
        printf("write keys error!\n");
        return;
    }

    printf("scid:%s\n", xqc_scid_str(scid));

    int write_len = write(ctx->keylog_fd, line, strlen(line));
    if (write_len < 0) {
        printf("write keys failed, errno: %d\n", errno);
        return;
    }

    write_len = write(ctx->keylog_fd, "\n", 1);
    if (write_len < 0) {
        printf("write keys failed, errno: %d\n", errno);
    }
}


int 
xqc_client_cert_verify(const unsigned char *certs[], 
    const size_t cert_len[], size_t certs_len, void *conn_user_data)
{
    /* self-signed cert used in test cases, return >= 0 means success */
    return 0;
}

void 
xqc_client_ready_to_create_path(const xqc_cid_t *cid, 
    void *conn_user_data)
{
    printf("***** on_ready_to_create_path\n");
    uint64_t path_id = 0;
    user_conn_t *user_conn = (user_conn_t *) conn_user_data;

    if (!g_enable_multipath) {
        return;
    }

    for (int i = 0; i < g_multi_interface_cnt; i++) {
        if (g_client_path[i].is_in_used == 1) {
            continue;
        }
    
        int ret = xqc_conn_create_path(ctx.engine, &(user_conn->cid), &path_id, 0);

        if (ret < 0) {
            printf("not support mp, xqc_conn_create_path err = %d\n", ret);
            return;
        }

        printf("***** create a new path. index: %d, path_id: %" PRIu64 "\n", i, path_id);
        g_client_path[i].path_id = path_id;
        g_client_path[i].is_in_used = 1;

        if (g_mp_backup_mode) {
            ret = xqc_conn_mark_path_standby(ctx.engine, &(user_conn->cid), path_id);
            if (ret < 0) {
                printf("xqc_conn_mark_path_standby err = %d\n", ret);
            }
        }
    }
}


void usage(int argc, char *argv[]) {
    char *prog = argv[0];
    char *const slash = strrchr(prog, '/');
    if (slash) {
        prog = slash + 1;
    }
    printf(
"Usage: %s [Options]\n"
"\n"
"Options:\n"
"   -a    Server addr.\n"
"   -p    Server port.\n"
"   -P    Number of Parallel requests per single connection. Default 1.\n"
"   -n    Total number of requests to send. Defaults 1.\n"
"   -c    Congestion Control Algorithm. r:reno b:bbr c:cubic B:bbr2 bbr+ bbr2+\n"
"   -C    Pacing on.\n"
"   -t    Connection timeout. Default 3 seconds.\n"
"   -T    Transport protocol: 0 H3 (default), 1 Transport layer, 2 H3-ext.\n"
"   -1    Force 1RTT.\n"
"   -s    Body size to send.\n"
"   -F    Abs_timeout to close conn. >=0.\n"
"   -w    Write received body to file.\n"
"   -r    Read sending body from file. priority s > r\n"
"   -l    Log level. e:error d:debug.\n"
"   -E    Echo check on. Compare sent data with received data.\n"
"   -d    Drop rate ‰.\n"
"   -u    Url. default https://test.xquic.com/path/resource\n"
"   -H    Header. eg. key:value\n"
"   -h    Host & sni. eg. test.xquic.com\n"
"   -G    GET on. Default is POST\n"
"   -x    Test case ID\n"
"   -N    No encryption\n"
"   -6    IPv6\n"
"   -M    Enable multi-path on. |\n"
"   -v    Multipath Version Negotiation.\n"
"   -i    Multi-path interface. e.g. -i interface1 -i interface2.\n"
"   -R    Enable reinjection. Default is 0, no reinjection.\n"
"   -V    Force cert verification. 0: don't allow self-signed cert. 1: allow self-signed cert.\n"
"   -q    name-value pair num of request header, default and larger than 6\n"
"   -o    Output log file path, default ./clog\n"
"   -f    Debug endless loop.\n"
"   -e    Epoch, default is 0.\n"
"   -D    Process num. default is 2.\n"
"   -b    Create connection per second. default is 100.\n"
"   -B    Max connection num. default is 1000.\n"
"   -J    Random CID. default is 0.\n"
"   -Q    Multipath backup path standby, set backup_mode on(1). default backup_mode is 0(off).\n"
"   -A    Multipath request accelerate on. default is 0(off).\n"
"   -y    multipath backup path standby.\n"
"   -z    periodically send request.\n"
, prog);
}

int main(int argc, char *argv[]) {

    g_req_cnt = 0;
    g_bytestream_cnt = 0;
    g_req_max = 1;
    g_send_body_size = 1024*1024;
    g_send_body_size_defined = 0;
    g_send_body_size_from_cdf = 0;
    cdf_list_size  = 0;
    cdf_list = NULL;
    g_save_body = 0;
    g_read_body = 0;
    g_echo_check = 0;
    g_drop_rate = 0;
    g_spec_url = 0;
    g_is_get = 0;
    g_test_case = 0;
    g_ipv6 = 0;
    g_no_crypt = 0;
    g_max_dgram_size = 0;
    g_send_dgram = 0;
    g_req_paral = 1;
    g_copa_ai = 1.0;
    g_copa_delta = 0.05;
    g_dgram_qos_level = XQC_DATA_QOS_HIGH;
    g_pmtud_on = 0;

    char server_addr[64] = TEST_SERVER_ADDR;
    g_server_addr = server_addr;
    int server_port = TEST_SERVER_PORT;
    char c_cong_ctl = 'b';
    char c_log_level = 'd';
    int c_cong_plus = 0;
    int pacing_on = 0;
    int transport = 0;
    int use_1rtt = 0;
    uint64_t rate_limit = 0;

    strcpy(g_log_path, "./clog");

    srand(0); //fix the random seed

    int long_opt_index;

    const struct option long_opts[] = {
        {"copa_delta", required_argument, &long_opt_index, 1},
        {"copa_ai_unit", required_argument, &long_opt_index, 2},
        {"epoch_timeout", required_argument, &long_opt_index, 3},
        {"dgram_qos", required_argument, &long_opt_index, 4},
        {"pmtud", required_argument, &long_opt_index, 5},
        {"mp_ping", required_argument, &long_opt_index, 6},
        {"rate_limit", required_argument, &long_opt_index, 7},
        {0, 0, 0, 0}
    };

    int ch = 0;
    while ((ch = getopt_long(argc, argv, "a:p:P:n:c:Ct:T:1s:w:r:l:Ed:u:H:h:Gx:6NMR:i:V:v:q:o:fe:F:D:b:B:J:Q:U:Ayz", long_opts, NULL)) != -1) {
        switch (ch) {
        case 'U':
            printf("option send_datagram 0 (off), 1 (on), 2(on + batch): %s\n", optarg);
            g_send_dgram = atoi(optarg);
            break;
        case 'Q':
            /* max_datagram_frame_size */
            printf("option max_datagram_frame_size: %s\n", optarg);
            g_max_dgram_size = atoi(optarg);
            break;
        case 'a': /* Server addr. */
            printf("option addr :%s\n", optarg);
            snprintf(server_addr, sizeof(server_addr), optarg);
            break;
        case 'p': /* Server port. */
            printf("option port :%s\n", optarg);
            server_port = atoi(optarg);
            g_server_port = server_port;
            break;
        case 'P': /* Number of Parallel requests per single connection. Default 1. */
            printf("option req_paral :%s\n", optarg);
            g_req_paral = atoi(optarg);
            break;
        case 'n': /* Total number of requests to send. Defaults 1. */
            printf("option req_max :%s\n", optarg);
            g_req_max = atoi(optarg);
            break;
        case 'c': /* Congestion Control Algorithm. r:reno b:bbr c:cubic B:bbr2 bbr+ bbr2+ */
            c_cong_ctl = optarg[0];
            if (strncmp("bbr2", optarg, 4) == 0) {
                c_cong_ctl = 'B';
            }

            if (strncmp("bbr2+", optarg, 5) == 0
                || strncmp("bbr+", optarg, 4) == 0)
            {
                c_cong_plus = 1;
            }
            printf("option cong_ctl : %c: %s: plus? %d\n", c_cong_ctl, optarg, c_cong_plus);
            break;
        case 'C': /* Pacing on */
            printf("option pacing :%s\n", "on");
            pacing_on = 1;
            break;
        case 't': /* Connection timeout. Default 3 seconds. */
            printf("option g_conn_timeout :%s\n", optarg);
            g_conn_timeout = atoi(optarg);
            break;
        case 'T': /* Transport layer. No HTTP3. */
            printf("option transport :%s\n", "on");
            transport = atoi(optarg);
            g_transport = transport;
            break;
        case '1': /* Force 1RTT. */
            printf("option 1RTT :%s\n", "on");
            use_1rtt = 1;
            break;
        case 's': /* Body size to send. */
            printf("option send_body_size :%s\n", optarg);
            g_send_body_size = atoi(optarg);
            g_send_body_size_defined = 1;
            if (g_send_body_size > MAX_BUF_SIZE) {
                printf("max send_body_size :%d\n", MAX_BUF_SIZE);
                exit(0);
            }
            break;
        case 'F':
            printf("option abs_timeout to close conn:%s\n", optarg);
            g_conn_abs_timeout = atoi(optarg);
            if (g_conn_abs_timeout < 0) {
                printf("timeout must be positive!\n");
                exit(0);
            }
            break;
        case 'w': /* Write received body to file. */
            printf("option save body :%s\n", optarg);
            snprintf(g_write_file, sizeof(g_write_file), optarg);
            g_save_body = 1;
            break;
        case 'r': /* Read sending body from file. priority s > r */
            printf("option read body :%s\n", optarg);
            snprintf(g_read_file, sizeof(g_read_file), optarg);
            g_read_body = 1;
            break;
        case 'l': /* Log level. e:error d:debug. */
            printf("option log level :%s\n", optarg);
            c_log_level = optarg[0];
            break;
        case 'E': /* Echo check on. Compare sent data with received data. */
            printf("option echo check :%s\n", "on");
            g_echo_check = 1;
            break;
        case 'd': /* Drop rate ‰. */
            printf("option drop rate :%s\n", optarg);
            g_drop_rate = atoi(optarg);
            break;
        case 'u': /* Url. default https://test.xquic.com/path/resource */
            printf("option url :%s\n", optarg);
            snprintf(g_url, sizeof(g_url), optarg);
            g_spec_url = 1;
            sscanf(g_url, "%[^://]://%[^/]%s", g_scheme, g_host, g_url_path);
            break;
        case 'H': /* Header. eg. key:value */
            printf("option header :%s\n", optarg);
            snprintf(g_headers[g_header_cnt], sizeof(g_headers[g_header_cnt]), "%s", optarg);
            g_header_cnt++;
            break;
        case 'h': /* Host & sni. eg. test.xquic.com */
            printf("option host & sni :%s\n", optarg);
            snprintf(g_host, sizeof(g_host), optarg);
            break;
        case 'G': /* GET on. Default is POST */
            printf("option get :%s\n", "on");
            g_is_get = 1;
            break;
        case 'x': /* Test case ID */
            printf("option test case id: %s\n", optarg);
            g_test_case = atoi(optarg);
            break;
        case '6': /* IPv6 */
            printf("option IPv6 :%s\n", "on");
            g_ipv6 = 1;
            break;
        case 'N': /* No encryption */
            printf("option No crypt: %s\n", "yes");
            g_no_crypt = 1;
            break;
        case 'M':
            printf("option enable multi-path: %s\n", optarg);
            g_enable_multipath = 1;
            break;

        case 'v': /* Negotiate multipath version. 4: Multipath-04. 5: Multipath-05*/
            printf("option multipath version: %s\n", optarg);
            if (atoi(optarg) == 4) {
                g_multipath_version = XQC_MULTIPATH_04;
                
            } else if (atoi(optarg) == 5) {
                g_multipath_version = XQC_MULTIPATH_05;
            }
            break;
        case 'R':
            printf("option enable reinjection: %s\n", "on");
            g_enable_reinjection = atoi(optarg);
            break;
        case 'i':
            printf("option multi-path interface: %s\n", optarg);
            memset(g_multi_interface[g_multi_interface_cnt], 0, XQC_DEMO_INTERFACE_MAX_LEN);
            snprintf(g_multi_interface[g_multi_interface_cnt], 
                        XQC_DEMO_INTERFACE_MAX_LEN, optarg);
            ++g_multi_interface_cnt;
            break;
        case 'V': /* Force cert verification. 0: don't allow self-signed cert. 1: allow self-signed cert. */
            printf("option enable cert verify: %s\n", "yes");
            g_verify_cert = 1;
            g_verify_cert_allow_self_sign = atoi(optarg);
            break;
        case 'q': /* name-value pair num of request header, default and larger than 6. */
            printf("option name-value pair num: %s\n", optarg);
            g_header_num = atoi(optarg);
            break;
        case 'o':
            printf("option log path :%s\n", optarg);
            snprintf(g_log_path, sizeof(g_log_path), optarg);
            break;
        case 'f':
            printf("option debug endless loop\n");
            g_debug_path = 1;
            g_conn_timeout = 5;
            break;
        case 'e':
            printf("option epoch: %s\n", optarg);
            g_epoch = atoi(optarg);
            break;
        case 'D':
            printf("process num:%s\n", optarg);
            g_process_num = atoi(optarg);
            g_test_qch_mode = 1; /* -D 开关用于测试qch */
            break;
        case 'b':
            printf("create connection per second :%s\n", optarg);
            g_conn_num = atoi(optarg);
            break;
        case 'B':
            printf("MAX connection num:%s\n", optarg);
            g_max_conn_num = atoi(optarg);
            break;
        case 'J':
            printf("random cid:%s\n", optarg);
            g_random_cid = atoi(optarg);
            break;
        case 'y':
            printf("option multipath backup path standby :%s\n", "on");
            g_mp_backup_mode = 1;
            break;
        case 'A':
            printf("option multipath request accelerate :%s\n", "on");
            g_mp_request_accelerate = 1;
            break;
        case 'z':
            printf("option periodically send request :%s\n", "on");
            g_periodically_request = 1;
            break;
        /* long options */
        case 0:

            switch (long_opt_index)
            {
            case 1: /* copa_delta */
                g_copa_delta = atof(optarg);
                if (g_copa_delta <= 0 || g_copa_delta > 0.5) {
                    printf("option g_copa_delta must be in (0, 0.5]\n");
                    exit(0);
                } else {
                    printf("option g_copa_delta: %.4lf\n", g_copa_delta);
                }
                break;

            case 2: /* copa_ai_unit */

                g_copa_ai = atof(optarg);
                if (g_copa_ai < 1.0) {
                    printf("option g_copa_ai must be greater than 1.0\n");
                    exit(0);
                } else {
                    printf("option g_copa_ai: %.4lf\n", g_copa_ai);
                }
                break;
            
            case 3:
                g_epoch_timeout = atoi(optarg);
                if (g_epoch_timeout <= 0) {
                    printf("invalid epoch_timeout!\n");
                    exit(0);
                } else {
                    printf("option g_epoch_timeout: %d\n", g_epoch_timeout);
                }
                break;

            case 4:
                g_dgram_qos_level = atoi(optarg);
                if (g_dgram_qos_level < XQC_DATA_QOS_HIGHEST || g_dgram_qos_level > XQC_DATA_QOS_LOWEST) {
                    printf("invalid qos level!\n");
                    exit(0);
                } else {
                    printf("option g_dgram_qos_level: %d\n", g_dgram_qos_level);
                }
                break;

            case 5:
                g_pmtud_on = atoi(optarg);
                printf("option g_pmtud_on: %d\n", g_pmtud_on);
                break;

            case 6:
                g_mp_ping_on = atoi(optarg);
                printf("option g_mp_ping_on: %d\n", g_mp_ping_on);
                break;

            case 7:
                rate_limit = atoi(optarg);
                printf("option rate_limit: %"PRIu64" Bps\n", rate_limit);
                break;

            default:
                break;
            }

            break;

        default:
            printf("other option :%c\n", ch);
            usage(argc, argv);
            exit(0);
        }

    }
    
    memset(g_header_key, 'k', sizeof(g_header_key));
    memset(g_header_value, 'v', sizeof(g_header_value));
    memset(&ctx, 0, sizeof(ctx));

    xqc_client_open_keylog_file(&ctx);
    xqc_client_open_log_file(&ctx);

    xqc_engine_ssl_config_t  engine_ssl_config;
    memset(&engine_ssl_config, 0, sizeof(engine_ssl_config));
    /* client does not need to fill in private_key_file & cert_file */
    engine_ssl_config.ciphers = XQC_TLS_CIPHERS;
    engine_ssl_config.groups = XQC_TLS_GROUPS;

    xqc_engine_callback_t callback = {
        .set_event_timer = xqc_client_set_event_timer, /* call xqc_engine_main_logic when the timer expires */
        .log_callbacks = {
            .xqc_log_write_err = xqc_client_write_log,
            .xqc_log_write_stat = xqc_client_write_log,
        },
        .keylog_cb = xqc_keylog_cb,
    };

    xqc_transport_callbacks_t tcbs = {
        .write_socket = xqc_client_write_socket,
        .write_socket_ex = xqc_client_write_socket_ex,
        .save_token = xqc_client_save_token,
        .save_session_cb = save_session_cb,
        .save_tp_cb = save_tp_cb,
        .cert_verify_cb = xqc_client_cert_verify,
        .conn_update_cid_notify = xqc_client_conn_update_cid_notify,
        .ready_to_create_path_notify = xqc_client_ready_to_create_path,
        .path_removed_notify = xqc_client_path_removed,
        .conn_closing = xqc_client_conn_closing_notify,
    };

    xqc_cong_ctrl_callback_t cong_ctrl;
    uint32_t cong_flags = 0;
    if (c_cong_ctl == 'b') {
        cong_ctrl = xqc_bbr_cb;
        cong_flags = XQC_BBR_FLAG_NONE;
#if XQC_BBR_RTTVAR_COMPENSATION_ENABLED
        if (c_cong_plus) {
            cong_flags |= XQC_BBR_FLAG_RTTVAR_COMPENSATION;
        }
#endif
    }
#ifdef XQC_ENABLE_RENO
    else if (c_cong_ctl == 'r') {
        cong_ctrl = xqc_reno_cb;
    }
#endif
    else if (c_cong_ctl == 'c') {
        cong_ctrl = xqc_cubic_cb;
    }
#ifdef XQC_ENABLE_BBR2
    else if (c_cong_ctl == 'B') {
        cong_ctrl = xqc_bbr2_cb;
        cong_flags = XQC_BBR2_FLAG_NONE;
#if XQC_BBR2_PLUS_ENABLED
        if (c_cong_plus) {
            cong_flags |= XQC_BBR2_FLAG_RTTVAR_COMPENSATION;
            cong_flags |= XQC_BBR2_FLAG_FAST_CONVERGENCE;
        }
#endif
    }
#endif
#ifdef XQC_ENABLE_UNLIMITED
    else if (c_cong_ctl == 'u') {
        cong_ctrl = xqc_unlimited_cc_cb;

    }
#endif
#ifdef XQC_ENABLE_COPA
    else if (c_cong_ctl == 'P') {
        cong_ctrl = xqc_copa_cb;

    } 
#endif
    else {
        printf("unknown cong_ctrl, option is b, r, c, B, bbr+, bbr2+, u\n");
        return -1;
    }
    printf("congestion control flags: %x\n", cong_flags);

    xqc_conn_settings_t conn_settings = {
        .pacing_on  =   pacing_on,
        .ping_on    =   0,
        .cong_ctrl_callback = cong_ctrl,
        .cc_params  =   {
            .customize_on = 1, 
            .init_cwnd = 32, 
            .cc_optimization_flags = cong_flags, 
            .copa_delta_ai_unit = g_copa_ai, 
            .copa_delta_base = g_copa_delta,
        },
        .spurious_loss_detect_on = 0,
        .keyupdate_pkt_threshold = 0,
        .max_datagram_frame_size = g_max_dgram_size,
        .enable_multipath = g_enable_multipath,
        .multipath_version = g_multipath_version,
        .marking_reinjection = 1,
        .mp_ping_on = g_mp_ping_on,
        .recv_rate_bytes_per_sec = rate_limit,
    };

    xqc_stream_settings_t stream_settings = { .recv_rate_bytes_per_sec = 0 };

    if (g_pmtud_on) {
        conn_settings.enable_pmtud = 1;
    }

    conn_settings.pacing_on = pacing_on;
    conn_settings.proto_version = XQC_VERSION_V1;
    conn_settings.max_datagram_frame_size = g_max_dgram_size;
    conn_settings.enable_multipath = g_enable_multipath;

    g_conn_settings = &conn_settings;

    xqc_config_t config;
    if (xqc_engine_get_default_config(&config, XQC_ENGINE_CLIENT) < 0) {
        return -1;
    }

    switch(c_log_level) {
        case 'e': config.cfg_log_level = XQC_LOG_ERROR; break;
        case 'i': config.cfg_log_level = XQC_LOG_INFO; break;
        case 'w': config.cfg_log_level = XQC_LOG_WARN; break;
        case 's': config.cfg_log_level = XQC_LOG_STATS; break;
        case 'd': config.cfg_log_level = XQC_LOG_DEBUG; break;
        default: config.cfg_log_level = XQC_LOG_DEBUG;
    }



    eb = event_base_new();

    ctx.ev_engine = event_new(eb, -1, 0, xqc_client_engine_callback, &ctx);

    ctx.engine = xqc_engine_create(XQC_ENGINE_CLIENT, &config, &engine_ssl_config,
                                   &callback, &tcbs, &ctx);
    if (ctx.engine == NULL) {
        printf("xqc_engine_create error\n");
        return -1;
    }

    int ret;

    /* register transport callbacks */
    xqc_app_proto_callbacks_t ap_cbs = {
        .conn_cbs = {
            .conn_create_notify = xqc_client_conn_create_notify,
            .conn_close_notify = xqc_client_conn_close_notify,
            .conn_handshake_finished = xqc_client_conn_handshake_finished,
            .conn_ping_acked = xqc_client_conn_ping_acked_notify,
        },
        .stream_cbs = {
            .stream_write_notify = xqc_client_stream_write_notify,
            .stream_read_notify = xqc_client_stream_read_notify,
            .stream_close_notify = xqc_client_stream_close_notify,
        }
    };

    xqc_engine_register_alpn(ctx.engine, XQC_ALPN_TRANSPORT, 9, &ap_cbs);

    user_conn_t *user_conn = xqc_client_user_conn_create(server_addr, server_port, transport);
    if (user_conn == NULL) {
        printf("xqc_client_user_conn_create error\n");
        return -1;
    }

    if (g_enable_multipath) {

        if (g_multi_interface_cnt < 1) {
            printf("Error: multi-path requires one path interfaces or more.\n");
            return -1;
        }

        conn_settings.enable_multipath = g_enable_multipath;
        for (int i = 0; i < g_multi_interface_cnt; ++i) {
            if (xqc_client_create_path(&g_client_path[i], g_multi_interface[i], user_conn) != XQC_OK) {
                printf("xqc_client_create_path %d error\n", i);
                return 0;
            }
        }

        // 权宜之计，，
        g_client_path[0].path_id = 0;
        g_client_path[0].is_in_used = 1;
        user_conn->fd = g_client_path[0].path_fd;
    }
    else {
        ret = xqc_client_create_conn_socket(user_conn);
        if (ret != XQC_OK) {
            printf("conn create socket error, ret: %d\n", ret);
            return -1;
        }
    }

    /* enable_reinjection */
    if (g_enable_reinjection == 1) {
        conn_settings.reinj_ctl_callback    = xqc_default_reinj_ctl_cb;
        conn_settings.mp_enable_reinjection = 1;

    } else if (g_enable_reinjection == 2) {
        conn_settings.reinj_ctl_callback    = xqc_deadline_reinj_ctl_cb;
        conn_settings.mp_enable_reinjection = 2;

    } else if (g_enable_reinjection == 3) {
        conn_settings.reinj_ctl_callback    = xqc_dgram_reinj_ctl_cb;
        conn_settings.mp_enable_reinjection = 4;
        conn_settings.scheduler_callback    = xqc_rap_scheduler_cb;
    } else if (g_enable_reinjection == 4) {
        conn_settings.reinj_ctl_callback    = xqc_dgram_reinj_ctl_cb;
        conn_settings.mp_enable_reinjection = 4;
        conn_settings.scheduler_callback    = xqc_rap_scheduler_cb;
        conn_settings.datagram_redundant_probe = 30000;
    }

    if (g_mp_backup_mode) {
        conn_settings.scheduler_callback  = xqc_backup_scheduler_cb;
    }

    unsigned char token[XQC_MAX_TOKEN_LEN];
    int token_len = XQC_MAX_TOKEN_LEN;
    token_len = xqc_client_read_token(token, token_len);
    if (token_len > 0) {
        user_conn->token = token;
        user_conn->token_len = token_len;
    }

    xqc_conn_ssl_config_t conn_ssl_config;
    memset(&conn_ssl_config, 0, sizeof(conn_ssl_config));

    if (g_verify_cert) {
        conn_ssl_config.cert_verify_flag |= XQC_TLS_CERT_FLAG_NEED_VERIFY;
        if (g_verify_cert_allow_self_sign) {
            conn_ssl_config.cert_verify_flag |= XQC_TLS_CERT_FLAG_ALLOW_SELF_SIGNED;
        }
    }

    char session_ticket_data[8192]={0};
    char tp_data[8192] = {0};

    int session_len = read_file_data(session_ticket_data, sizeof(session_ticket_data), "test_session");
    int tp_len = read_file_data(tp_data, sizeof(tp_data), "tp_localhost");

    if (session_len < 0 || tp_len < 0 || use_1rtt) {
        printf("sessoin data read error or use_1rtt\n");
        conn_ssl_config.session_ticket_data = NULL;
        conn_ssl_config.transport_parameter_data = NULL;

    } else {
        conn_ssl_config.session_ticket_data = session_ticket_data;
        conn_ssl_config.session_ticket_len = session_len;
        conn_ssl_config.transport_parameter_data = tp_data;
        conn_ssl_config.transport_parameter_data_len = tp_len;
    }


    const xqc_cid_t *cid;

    printf("conn type: %d\n", user_conn->h3);

    user_conn->ctx = &ctx;


    cid = xqc_connect(ctx.engine, &conn_settings, user_conn->token, user_conn->token_len,
                            server_addr, g_no_crypt, &conn_ssl_config, user_conn->peer_addr, 
                            user_conn->peer_addrlen, XQC_ALPN_TRANSPORT, user_conn);

    if (cid == NULL) {
        printf("xqc_connect error\n");
        xqc_engine_destroy(ctx.engine);
        return 0;
    }

    /* copy cid to its own memory space to prevent crashes caused by internal cid being freed */
    memcpy(&user_conn->cid, cid, sizeof(*cid));

    user_conn->dgram_blk = calloc(1, sizeof(user_dgram_blk_t));
    user_conn->dgram_blk->data_sent = 0;
    user_conn->dgram_blk->data_recv = 0;
    user_conn->dgram_blk->dgram_id = 1;
    if (user_conn->quic_conn) {
        printf("[dgram]|prepare_dgram_user_data|\n");
        xqc_datagram_set_user_data(user_conn->quic_conn, user_conn);
 
    }
    
    for (int i = 0; i < g_req_paral; i++) {
        g_req_cnt++;
        user_stream_t *user_stream = calloc(1, sizeof(user_stream_t));
        user_stream->user_conn = user_conn;
        user_stream->last_recv_log_time = now();
        user_stream->recv_log_bytes = 0;

        user_stream->stream = xqc_stream_create(ctx.engine, cid, NULL, user_stream);
        if (user_stream->stream == NULL) {
            printf("xqc_stream_create error\n");
            continue;
        }
        printf("[qperf]|ts:%"PRIu64"|test_start|\n", now());
        xqc_client_stream_send(user_stream->stream, user_stream);

    }

    last_recv_ts = now();

    event_base_dispatch(eb);

    // TODO
    // 如果支持多路径，socket由path管
    if (0 == g_enable_multipath) {
        event_free(user_conn->ev_socket);
    }
    event_free(user_conn->ev_timeout);

    if (user_conn->dgram_blk) {
        if (user_conn->dgram_blk->data) {
            free(user_conn->dgram_blk->data);
        }
        if (user_conn->dgram_blk->recv_data) {
            free(user_conn->dgram_blk->recv_data);
        }
        free(user_conn->dgram_blk);
    }

    free(user_conn->peer_addr);
    free(user_conn->local_addr);
    free(user_conn);

    if (ctx.ev_delay) {
        event_free(ctx.ev_delay);
    }

    xqc_engine_destroy(ctx.engine);
    xqc_client_close_keylog_file(&ctx);
    xqc_client_close_log_file(&ctx);

    return 0;
}
