#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <libwebsockets.h>
#include <cjson/cJSON.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* ==================== 配置定义 ==================== */
#define UDP_FORWARD_IP      "127.0.0.1"
#define UDP_FORWARD_PORT    9002
#define UDP_AUDIO_IP        "127.0.0.1"
#define UDP_AUDIO_PORT      9001
#define UDP_AUDIO_MAX_SIZE  4096
#define AUDIO_TX_QUEUE_DEPTH 64
#define AUDIO_LOG_INTERVAL  500
#define WS_TEXT_MAX_SIZE    4096
#define WS_TEXT_QUEUE_DEPTH 8
#define DEFAULT_PROTOCOL    "xiaozhi-protocol"

typedef enum {
    WS_TEXT_KIND_GENERIC = 0,
    WS_TEXT_KIND_LISTEN_START,
    WS_TEXT_KIND_LISTEN_STOP,
    WS_TEXT_KIND_MCP_INITIALIZE_RESULT,
    WS_TEXT_KIND_MCP_TOOLS_LIST_RESULT
} ws_text_kind_t;

/* ==================== 全局变量 ==================== */
static volatile sig_atomic_t interrupted = 0;
static struct lws *client_wsi = NULL;
static struct lws_context *g_lws_context = NULL;
static int hello_sent = 0;
static char g_session_id[128] = {0};
static int g_session_id_saved = 0;
static unsigned long g_listen_start_sent_count = 0;
static int g_listen_after_tools_list_sent = 0;
static char g_listen_mode[16] = "auto";
static int g_listen_stop_after_sec = 0;
static time_t g_after_tools_list_listen_sent_time = 0;
static int g_listen_stop_queued = 0;
static int g_listen_stop_sent = 0;
static int g_mcp_initialize_replied = 0;
static int g_mcp_initialized_notification_recv = 0;
static int g_mcp_tools_list_replied = 0;

/* UDP 转发 */
static int g_udp_forward_sockfd = -1;
static struct sockaddr_in g_udp_forward_addr;

/* 音频线程 */
static pthread_t g_audio_thread = 0;
static volatile int g_audio_thread_running = 0;
static int g_udp_audio_sockfd = -1;
static struct sockaddr_in g_udp_audio_addr;

typedef struct {
    size_t len;
    unsigned char data[UDP_AUDIO_MAX_SIZE];
} audio_tx_packet_t;

typedef struct {
    size_t len;
    ws_text_kind_t kind;
    char reason[32];
    char data[WS_TEXT_MAX_SIZE];
} ws_text_packet_t;

static pthread_mutex_t g_audio_tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static audio_tx_packet_t g_audio_tx_queue[AUDIO_TX_QUEUE_DEPTH];
static size_t g_audio_tx_head = 0;
static size_t g_audio_tx_tail = 0;
static size_t g_audio_tx_count = 0;
static unsigned long g_udp_audio_recv_count = 0;
static unsigned long g_audio_tx_enqueued_count = 0;
static unsigned long g_audio_tx_dropped_count = 0;
static unsigned long g_ws_audio_sent_count = 0;
static unsigned long g_server_binary_recv_count = 0;
static unsigned long g_udp_forward_sent_count = 0;
static unsigned long g_udp_forward_send_fail_count = 0;

static pthread_mutex_t g_ws_text_mutex = PTHREAD_MUTEX_INITIALIZER;
static ws_text_packet_t g_ws_text_queue[WS_TEXT_QUEUE_DEPTH];
static size_t g_ws_text_head = 0;
static size_t g_ws_text_tail = 0;
static size_t g_ws_text_count = 0;
static unsigned long g_ws_text_dropped_count = 0;
static unsigned long g_ws_text_sent_count = 0;
static unsigned long g_listen_stop_sent_count = 0;

/* 连接参数 */
struct connection_params {
    const char *access_token;
    const char *version;
    const char *device_id;
    const char *client_id;
};

static struct connection_params conn_params = {
    "test-token",
    "1",
    "ee:a4:ee:f8:cc:7c",
    "d90612cb-fd9c-4ddd-9981-a27b238983b7"
};

/* ==================== 函数声明 ==================== */
static int init_udp_forward(void);
static void cleanup_udp_forward(void);
static int init_udp_audio_recv(void);
static void cleanup_udp_audio_recv(void);
static void *AudioThread(void *arg);
static void start_audio_thread(void);
static void stop_audio_thread(void);
static int audio_tx_enqueue(const unsigned char *data, size_t len);
static int audio_tx_dequeue(unsigned char *data, size_t *len);
static size_t audio_tx_pending_count(void);
static void audio_tx_clear(void);
static void request_ws_audio_write(void);
static void PrintAudioStats(const char *tag);
static void LogProtocolState(const char *tag);
static int ws_text_enqueue_ex(const char *data, size_t len, ws_text_kind_t kind, const char *reason);
static int ws_text_dequeue(char *data, size_t *len, ws_text_kind_t *kind, char *reason, size_t reason_size);
static size_t ws_text_pending_count(void);
static void ws_text_clear(void);
static void request_ws_text_write(void);
static void LoadListenConfig(void);
static void StartListen(struct lws *wsi, const char *reason);
static void QueueListenStop(struct lws *wsi, const char *reason);
static void CheckListenStopTimer(void);
static void ProcessBinDataFrmServer(unsigned char *data, size_t len);
static void ProcessTxtDataFrmServer(const char *data, size_t len);
static void ProcessHello(cJSON *root);
static void ProcessMCP(cJSON *root);
static void SendMCPInitializeResult(cJSON *request_id, const char *protocol_version);
static void SendMCPToolsListResult(cJSON *request_id);
static void ProcessTTS(cJSON *root);
static void ProcessSTT(cJSON *root);

static int audio_tx_enqueue(const unsigned char *data, size_t len)
{
    int queued = 0;
    unsigned long recv_snapshot = 0;
    unsigned long enqueued_snapshot = 0;
    unsigned long dropped_snapshot = 0;
    size_t pending_snapshot = 0;

    if (data == NULL || len == 0 || len > UDP_AUDIO_MAX_SIZE) {
        return 0;
    }

    pthread_mutex_lock(&g_audio_tx_mutex);

    if (g_audio_tx_count == AUDIO_TX_QUEUE_DEPTH) {
        g_audio_tx_head = (g_audio_tx_head + 1) % AUDIO_TX_QUEUE_DEPTH;
        g_audio_tx_count--;
        g_audio_tx_dropped_count++;
    }

    g_audio_tx_queue[g_audio_tx_tail].len = len;
    memcpy(g_audio_tx_queue[g_audio_tx_tail].data, data, len);
    g_audio_tx_tail = (g_audio_tx_tail + 1) % AUDIO_TX_QUEUE_DEPTH;
    g_audio_tx_count++;
    g_audio_tx_enqueued_count++;
    queued = 1;

    recv_snapshot = g_udp_audio_recv_count;
    enqueued_snapshot = g_audio_tx_enqueued_count;
    dropped_snapshot = g_audio_tx_dropped_count;
    pending_snapshot = g_audio_tx_count;

    pthread_mutex_unlock(&g_audio_tx_mutex);

    if (recv_snapshot <= 5 || (recv_snapshot % AUDIO_LOG_INTERVAL) == 0 ||
        (dropped_snapshot > 0 && (dropped_snapshot <= 5 || (dropped_snapshot % 1000) == 0))) {
        printf("[audio] udp_recv=%lu queued=%lu dropped=%lu pending=%zu last_len=%zu\n",
               recv_snapshot, enqueued_snapshot, dropped_snapshot, pending_snapshot, len);
    }

    return queued;
}

static int audio_tx_dequeue(unsigned char *data, size_t *len)
{
    if (data == NULL || len == NULL) {
        return 0;
    }

    pthread_mutex_lock(&g_audio_tx_mutex);

    if (g_audio_tx_count == 0) {
        pthread_mutex_unlock(&g_audio_tx_mutex);
        return 0;
    }

    *len = g_audio_tx_queue[g_audio_tx_head].len;
    memcpy(data, g_audio_tx_queue[g_audio_tx_head].data, *len);
    g_audio_tx_head = (g_audio_tx_head + 1) % AUDIO_TX_QUEUE_DEPTH;
    g_audio_tx_count--;

    pthread_mutex_unlock(&g_audio_tx_mutex);
    return 1;
}

static size_t audio_tx_pending_count(void)
{
    size_t pending = 0;

    pthread_mutex_lock(&g_audio_tx_mutex);
    pending = g_audio_tx_count;
    pthread_mutex_unlock(&g_audio_tx_mutex);

    return pending;
}

static void audio_tx_clear(void)
{
    pthread_mutex_lock(&g_audio_tx_mutex);
    g_audio_tx_head = 0;
    g_audio_tx_tail = 0;
    g_audio_tx_count = 0;
    pthread_mutex_unlock(&g_audio_tx_mutex);
}

static void PrintAudioStats(const char *tag)
{
    unsigned long udp_recv = 0;
    unsigned long queued = 0;
    unsigned long dropped = 0;
    size_t pending = 0;

    pthread_mutex_lock(&g_audio_tx_mutex);
    udp_recv = g_udp_audio_recv_count;
    queued = g_audio_tx_enqueued_count;
    dropped = g_audio_tx_dropped_count;
    pending = g_audio_tx_count;
    pthread_mutex_unlock(&g_audio_tx_mutex);

    printf("[stats] %s udp9001_recv=%lu ws_binary_sent=%lu audio_queue_queued=%lu audio_queue_dropped=%lu audio_queue_pending=%zu server_binary_recv=%lu udp9002_sent=%lu udp9002_send_failed=%lu listen_start_sent=%lu listen_stop_sent=%lu listen_mode=%s stop_after_sec=%d ws_text_sent=%lu ws_text_dropped=%lu\n",
           tag != NULL ? tag : "final",
           udp_recv,
           g_ws_audio_sent_count,
           queued,
           dropped,
           pending,
           g_server_binary_recv_count,
           g_udp_forward_sent_count,
           g_udp_forward_send_fail_count,
           g_listen_start_sent_count,
           g_listen_stop_sent_count,
           g_listen_mode,
           g_listen_stop_after_sec,
           g_ws_text_sent_count,
           g_ws_text_dropped_count);
}

static void LogProtocolState(const char *tag)
{
    printf("[state] %s session_saved=%d listen_sent=%lu init_replied=%d initialized=%d tools_list_replied=%d after_tools_list_listen=%d mode=%s stop_after_sec=%d stop_queued=%d stop_sent=%d\n",
           tag != NULL ? tag : "unknown",
           g_session_id_saved,
           g_listen_start_sent_count,
           g_mcp_initialize_replied,
           g_mcp_initialized_notification_recv,
           g_mcp_tools_list_replied,
           g_listen_after_tools_list_sent,
           g_listen_mode,
           g_listen_stop_after_sec,
           g_listen_stop_queued,
           g_listen_stop_sent);
}

static int ws_text_enqueue_ex(const char *data, size_t len, ws_text_kind_t kind, const char *reason)
{
    if (data == NULL || len == 0 || len >= WS_TEXT_MAX_SIZE) {
        printf("[ws-text] warning: skip invalid text frame len=%zu\n", len);
        return 0;
    }

    pthread_mutex_lock(&g_ws_text_mutex);

    if (g_ws_text_count == WS_TEXT_QUEUE_DEPTH) {
        g_ws_text_head = (g_ws_text_head + 1) % WS_TEXT_QUEUE_DEPTH;
        g_ws_text_count--;
        g_ws_text_dropped_count++;
        printf("[ws-text] warning: text queue full, dropped oldest count=%lu\n",
               g_ws_text_dropped_count);
    }

    g_ws_text_queue[g_ws_text_tail].len = len;
    g_ws_text_queue[g_ws_text_tail].kind = kind;
    snprintf(g_ws_text_queue[g_ws_text_tail].reason,
             sizeof(g_ws_text_queue[g_ws_text_tail].reason),
             "%s", reason != NULL ? reason : "");
    memcpy(g_ws_text_queue[g_ws_text_tail].data, data, len);
    g_ws_text_queue[g_ws_text_tail].data[len] = '\0';
    g_ws_text_tail = (g_ws_text_tail + 1) % WS_TEXT_QUEUE_DEPTH;
    g_ws_text_count++;

    pthread_mutex_unlock(&g_ws_text_mutex);
    return 1;
}

static int ws_text_dequeue(char *data, size_t *len, ws_text_kind_t *kind, char *reason, size_t reason_size)
{
    if (data == NULL || len == NULL) {
        return 0;
    }

    pthread_mutex_lock(&g_ws_text_mutex);

    if (g_ws_text_count == 0) {
        pthread_mutex_unlock(&g_ws_text_mutex);
        return 0;
    }

    *len = g_ws_text_queue[g_ws_text_head].len;
    if (kind != NULL) {
        *kind = g_ws_text_queue[g_ws_text_head].kind;
    }
    if (reason != NULL && reason_size > 0) {
        snprintf(reason, reason_size, "%s", g_ws_text_queue[g_ws_text_head].reason);
    }
    memcpy(data, g_ws_text_queue[g_ws_text_head].data, *len);
    data[*len] = '\0';
    g_ws_text_head = (g_ws_text_head + 1) % WS_TEXT_QUEUE_DEPTH;
    g_ws_text_count--;

    pthread_mutex_unlock(&g_ws_text_mutex);
    return 1;
}

static size_t ws_text_pending_count(void)
{
    size_t pending = 0;

    pthread_mutex_lock(&g_ws_text_mutex);
    pending = g_ws_text_count;
    pthread_mutex_unlock(&g_ws_text_mutex);

    return pending;
}

static void ws_text_clear(void)
{
    pthread_mutex_lock(&g_ws_text_mutex);
    g_ws_text_head = 0;
    g_ws_text_tail = 0;
    g_ws_text_count = 0;
    pthread_mutex_unlock(&g_ws_text_mutex);
}

static void request_ws_audio_write(void)
{
    struct lws *wsi = client_wsi;

    if (wsi != NULL && !interrupted) {
        lws_callback_on_writable(wsi);
    }

    if (g_lws_context != NULL) {
        lws_cancel_service(g_lws_context);
    }
}

static void request_ws_text_write(void)
{
    request_ws_audio_write();
}

/* ==================== UDP 转发初始化 ==================== */
static int init_udp_forward(void)
{
    g_udp_forward_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_forward_sockfd < 0) {
        perror("❌ 创建 UDP 转发 Socket 失败");
        return -1;
    }
    
    memset(&g_udp_forward_addr, 0, sizeof(g_udp_forward_addr));
    g_udp_forward_addr.sin_family = AF_INET;
    g_udp_forward_addr.sin_port = htons(UDP_FORWARD_PORT);
    g_udp_forward_addr.sin_addr.s_addr = inet_addr(UDP_FORWARD_IP);
    
    printf("✓ UDP 转发初始化成功，目标：%s:%d\n", UDP_FORWARD_IP, UDP_FORWARD_PORT);
    return 0;
}

/* ==================== UDP 转发清理 ==================== */
static void cleanup_udp_forward(void)
{
    if (g_udp_forward_sockfd >= 0) {
        close(g_udp_forward_sockfd);
        g_udp_forward_sockfd = -1;
    }
}

/* ==================== UDP 音频接收初始化 ==================== */
static int init_udp_audio_recv(void)
{
    int optval = 1;
    
    g_udp_audio_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_audio_sockfd < 0) {
        perror("❌ 创建 UDP 音频接收 Socket 失败");
        return -1;
    }
    
    setsockopt(g_udp_audio_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(g_udp_audio_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    memset(&g_udp_audio_addr, 0, sizeof(g_udp_audio_addr));
    g_udp_audio_addr.sin_family = AF_INET;
    g_udp_audio_addr.sin_port = htons(UDP_AUDIO_PORT);
    g_udp_audio_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(g_udp_audio_sockfd, (struct sockaddr *)&g_udp_audio_addr, sizeof(g_udp_audio_addr)) < 0) {
        perror("❌ 绑定 UDP 音频接收端口失败");
        close(g_udp_audio_sockfd);
        g_udp_audio_sockfd = -1;
        return -1;
    }
    
    printf("✓ UDP 音频接收初始化成功，监听：%s:%d\n", UDP_AUDIO_IP, UDP_AUDIO_PORT);
    return 0;
}

/* ==================== UDP 音频接收清理 ==================== */
static void cleanup_udp_audio_recv(void)
{
    if (g_udp_audio_sockfd >= 0) {
        shutdown(g_udp_audio_sockfd, SHUT_RD);
        close(g_udp_audio_sockfd);
        g_udp_audio_sockfd = -1;
    }
}

/* ==================== 音频线程（参考 websocket.c 结构） ==================== */
static void *AudioThread(void *arg)
{
    unsigned char buffer[UDP_AUDIO_MAX_SIZE];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    printf("🎤 [音频线程] 启动，监听 UDP %s:%d\n", UDP_AUDIO_IP, UDP_AUDIO_PORT);
    
    while (g_audio_thread_running && !interrupted) {
        ssize_t recv_len = recvfrom(g_udp_audio_sockfd, buffer, sizeof(buffer), 0,
                                    (struct sockaddr *)&client_addr, &addr_len);
        
        if (recv_len < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("⚠️  UDP 音频接收失败");
            usleep(10000);
            continue;
        }
        
        // 检查结束标志（4 字节 int，值为 0）
        if (recv_len == sizeof(int)) {
            int flag = 0;
            memcpy(&flag, buffer, sizeof(int));
            if (flag == 0) {
                printf("🎤 [音频线程] 收到结束标志，等待新数据...\n");
                usleep(100000);
                continue;
            }
        }
        
        pthread_mutex_lock(&g_audio_tx_mutex);
        g_udp_audio_recv_count++;
        pthread_mutex_unlock(&g_audio_tx_mutex);

        if (client_wsi != NULL && g_audio_thread_running && !interrupted) {
            if (audio_tx_enqueue(buffer, (size_t)recv_len)) {
                request_ws_audio_write();
            }
        }
    }
    
    printf("🎤 [音频线程] 退出\n");
    pthread_exit(NULL);
}

/* ==================== 启动音频线程 ==================== */
static void start_audio_thread(void)
{
    if (g_audio_thread_running) {
        printf("⚠️  音频线程已在运行\n");
        return;
    }
    
    if (init_udp_audio_recv() != 0) {
        fprintf(stderr, "❌ UDP 音频接收初始化失败\n");
        return;
    }
    
    g_audio_thread_running = 1;
    
    if (pthread_create(&g_audio_thread, NULL, AudioThread, NULL) != 0) {
        perror("❌ 创建音频线程失败");
        g_audio_thread_running = 0;
        cleanup_udp_audio_recv();
        return;
    }
    
    printf("✓ 音频线程已启动\n");
}

/* ==================== 停止音频线程 ==================== */
static void stop_audio_thread(void)
{
    if (!g_audio_thread_running) {
        return;
    }
    
    printf("🛑 停止音频线程...\n");
    g_audio_thread_running = 0;
    
    cleanup_udp_audio_recv();
    
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    
    if (pthread_timedjoin_np(g_audio_thread, NULL, &ts) != 0) {
        printf("⚠️  音频线程等待超时，强制取消\n");
        pthread_cancel(g_audio_thread);
        pthread_join(g_audio_thread, NULL);
    }
    
    g_audio_thread = 0;
    audio_tx_clear();
    printf("✓ 音频线程已停止\n");
}

/* ==================== Listen config ==================== */
static void LoadListenConfig(void)
{
    const char *mode = getenv("XIAOZHI_LISTEN_MODE");
    const char *stop_after = getenv("XIAOZHI_LISTEN_STOP_AFTER_SEC");

    snprintf(g_listen_mode, sizeof(g_listen_mode), "auto");
    if (mode != NULL && mode[0] != '\0') {
        if (strcmp(mode, "manual") == 0) {
            snprintf(g_listen_mode, sizeof(g_listen_mode), "manual");
        } else if (strcmp(mode, "auto") == 0) {
            snprintf(g_listen_mode, sizeof(g_listen_mode), "auto");
        } else {
            printf("[listen] warning: unsupported XIAOZHI_LISTEN_MODE=%s, use auto\n", mode);
        }
    }

    g_listen_stop_after_sec = 0;
    if (stop_after != NULL && stop_after[0] != '\0') {
        char *end = NULL;
        long value = strtol(stop_after, &end, 10);
        if (end != stop_after && value > 0 && value <= 3600) {
            g_listen_stop_after_sec = (int)value;
        } else {
            printf("[listen] warning: invalid XIAOZHI_LISTEN_STOP_AFTER_SEC=%s, use 0\n",
                   stop_after);
        }
    }

    printf("[listen] config mode=%s stop_after_sec=%d\n",
           g_listen_mode, g_listen_stop_after_sec);
}

/* ==================== StartListen ==================== */
static void StartListen(struct lws *wsi, const char *reason)
{
    if (wsi == NULL || interrupted) {
        return;
    }

    if (g_session_id[0] == '\0') {
        printf("[listen] warning: skip start reason=%s because session_id is empty\n",
               reason != NULL ? reason : "unknown");
        return;
    }

    const char *listen_reason = reason != NULL ? reason : "unknown";
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", g_session_id);
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "start");
    cJSON_AddStringToObject(root, "mode", g_listen_mode);
    
    char *msg = cJSON_PrintUnformatted(root);
    if (msg == NULL) {
        cJSON_Delete(root);
        return;
    }
    
    printf("[listen] queue start reason=%s session_id=%s mode=%s\n",
           listen_reason, g_session_id, g_listen_mode);
    printf("[listen] start JSON: %s\n", msg);
    printf("📤 [发送] listen 消息：%s\n", msg);
    if (ws_text_enqueue_ex(msg, strlen(msg), WS_TEXT_KIND_LISTEN_START, listen_reason)) {
        request_ws_text_write();
    } else {
        printf("[listen] error: failed to queue start reason=%s\n", listen_reason);
    }

    cJSON_Delete(root);
    free(msg);
    return;
}

/* ==================== Queue listen stop ==================== */
static void QueueListenStop(struct lws *wsi, const char *reason)
{
    if (wsi == NULL || interrupted) {
        return;
    }

    if (g_session_id[0] == '\0') {
        printf("[listen] warning: skip stop reason=%s because session_id is empty\n",
               reason != NULL ? reason : "unknown");
        return;
    }

    const char *listen_reason = reason != NULL ? reason : "unknown";

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", g_session_id);
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "stop");

    char *msg = cJSON_PrintUnformatted(root);
    if (msg == NULL) {
        cJSON_Delete(root);
        return;
    }

    printf("[listen] queue stop reason=%s session_id=%s\n", listen_reason, g_session_id);
    printf("[listen] stop JSON: %s\n", msg);
    if (ws_text_enqueue_ex(msg, strlen(msg), WS_TEXT_KIND_LISTEN_STOP, listen_reason)) {
        g_listen_stop_queued = 1;
        request_ws_text_write();
    } else {
        printf("[listen] error: failed to queue stop reason=%s\n", listen_reason);
    }

    cJSON_Delete(root);
    free(msg);
}

static void CheckListenStopTimer(void)
{
    if (client_wsi == NULL || interrupted) {
        return;
    }

    if (strcmp(g_listen_mode, "manual") != 0 || g_listen_stop_after_sec <= 0) {
        return;
    }

    if (!g_listen_after_tools_list_sent || g_after_tools_list_listen_sent_time == 0 ||
        g_listen_stop_queued || g_listen_stop_sent) {
        return;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1) {
        return;
    }

    if ((int)(now - g_after_tools_list_listen_sent_time) >= g_listen_stop_after_sec) {
        printf("[listen] stop timer expired elapsed=%ld stop_after_sec=%d\n",
               (long)(now - g_after_tools_list_listen_sent_time),
               g_listen_stop_after_sec);
        QueueListenStop(client_wsi, "manual_stop_after_tools_list");
    }
}

/* ==================== 二进制数据处理 ==================== */
static void ProcessBinDataFrmServer(unsigned char *data, size_t len)
{
    if (g_udp_forward_sockfd >= 0 && !interrupted && len > 0) {
        g_server_binary_recv_count++;
        ssize_t sent = sendto(g_udp_forward_sockfd, data, len, 0,
                              (struct sockaddr *)&g_udp_forward_addr, 
                              sizeof(g_udp_forward_addr));
        if (sent < 0) {
            g_udp_forward_send_fail_count++;
            perror("❌ UDP 转发失败");
        } else {
            g_udp_forward_sent_count++;
            if (g_server_binary_recv_count == 1 ||
                g_server_binary_recv_count == 10 ||
                g_server_binary_recv_count == 50 ||
                g_server_binary_recv_count == 100 ||
                (g_server_binary_recv_count % AUDIO_LOG_INTERVAL) == 0) {
                printf("[audio] server_binary_recv=%lu len=%zu udp9002_sent=%lu udp9002_failed=%lu sent_len=%zd\n",
                       g_server_binary_recv_count, len, g_udp_forward_sent_count,
                       g_udp_forward_send_fail_count, sent);
            }
        }
    }
}

/* ==================== 文本数据处理 ==================== */
static void ProcessTxtDataFrmServer(const char *data, size_t len)
{
    printf("📝 [文本数据] 收到：%.*s\n", (int)len, data);
    
    cJSON *root = cJSON_Parse(data);
    if (root == NULL) {
        printf("❌ JSON 解析失败\n");
        return;
    }
    
    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_item) || type_item->valuestring == NULL) {
        cJSON_Delete(root);
        return;
    }
    
    const char *msg_type = type_item->valuestring;
    printf("📋 消息类型：%s\n", msg_type);
    
    if (strcmp(msg_type, "hello") == 0) {
        ProcessHello(root);
    } else if (strcmp(msg_type, "mcp") == 0) {
        ProcessMCP(root);
    } else if (strcmp(msg_type, "tts") == 0) {
        ProcessTTS(root);
    } else if (strcmp(msg_type, "stt") == 0) {
        ProcessSTT(root);
    }
    
    cJSON_Delete(root);
}

/* ==================== Hello 消息处理 ==================== */
static void ProcessHello(cJSON *root)
{
    printf("✅ [Hello] 服务器握手响应\n");
    cJSON *session_item = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (cJSON_IsString(session_item) && session_item->valuestring != NULL &&
        session_item->valuestring[0] != '\0') {
        snprintf(g_session_id, sizeof(g_session_id), "%s", session_item->valuestring);
        g_session_id_saved = 1;
        printf("[session] saved session_id=%s\n", g_session_id);
    } else {
        g_session_id[0] = '\0';
        g_session_id_saved = 0;
        printf("[session] warning: hello response has no session_id\n");
    }

    cJSON *audio_params = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        cJSON *sample_rate = cJSON_GetObjectItemCaseSensitive(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            printf("[protocol] server audio sample_rate=%d\n", sample_rate->valueint);
        }
    }

    if (client_wsi) {
        StartListen(client_wsi, "after_hello");
    }
    LogProtocolState("hello_processed");
}

/* ==================== MCP 消息处理 ==================== */
static void ProcessMCP(cJSON *root)
{
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!cJSON_IsObject(payload)) {
        printf("[mcp] warning: mcp message has no payload object\n");
        return;
    }

    cJSON *method = cJSON_GetObjectItemCaseSensitive(payload, "method");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(payload, "id");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(payload, "params");
    cJSON *protocol_version = NULL;
    const char *protocol_version_text = "2024-11-05";

    char *payload_text = cJSON_PrintUnformatted(payload);
    if (payload_text != NULL) {
        printf("[mcp] payload=%s\n", payload_text);
        free(payload_text);
    }

    if (!cJSON_IsString(method) || method->valuestring == NULL) {
        printf("[mcp] warning: missing method\n");
        return;
    }

    if (cJSON_IsObject(params)) {
        protocol_version = cJSON_GetObjectItemCaseSensitive(params, "protocolVersion");
        if (cJSON_IsString(protocol_version) && protocol_version->valuestring != NULL &&
            protocol_version->valuestring[0] != '\0') {
            protocol_version_text = protocol_version->valuestring;
        }
    }

    if (cJSON_IsNumber(id)) {
        printf("[mcp] method=%s id=%g protocolVersion=%s\n",
               method->valuestring, id->valuedouble, protocol_version_text);
    } else if (cJSON_IsString(id) && id->valuestring != NULL) {
        printf("[mcp] method=%s id=%s protocolVersion=%s\n",
               method->valuestring, id->valuestring, protocol_version_text);
    } else {
        printf("[mcp] method=%s id=<missing> protocolVersion=%s\n",
               method->valuestring, protocol_version_text);
    }

    if (strcmp(method->valuestring, "initialize") == 0) {
        printf("[mcp] received initialize request\n");
        SendMCPInitializeResult(id, protocol_version_text);
    } else if (strcmp(method->valuestring, "tools/list") == 0) {
        printf("[mcp] received tools/list request\n");
        SendMCPToolsListResult(id);
    } else if (strcmp(method->valuestring, "notifications/initialized") == 0) {
        g_mcp_initialized_notification_recv = 1;
        printf("[mcp] received MCP notification: notifications/initialized\n");
        printf("[mcp] no response required\n");
        LogProtocolState("mcp_initialized_notification");
    } else if (id == NULL) {
        printf("[mcp] received MCP notification: %s\n", method->valuestring);
        printf("[mcp] no response required\n");
    }
}

static void SendMCPInitializeResult(cJSON *request_id, const char *protocol_version)
{
    if (request_id == NULL || (!cJSON_IsNumber(request_id) && !cJSON_IsString(request_id))) {
        printf("[mcp] warning: skip initialize result because request id is missing\n");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    cJSON *result = cJSON_CreateObject();
    cJSON *capabilities = cJSON_CreateObject();
    cJSON *client_info = cJSON_CreateObject();
    cJSON *id_copy = cJSON_Duplicate(request_id, 1);

    if (root == NULL || payload == NULL || result == NULL ||
        capabilities == NULL || client_info == NULL || id_copy == NULL) {
        printf("[mcp] error: failed to allocate initialize result JSON\n");
        cJSON_Delete(root);
        cJSON_Delete(payload);
        cJSON_Delete(result);
        cJSON_Delete(capabilities);
        cJSON_Delete(client_info);
        cJSON_Delete(id_copy);
        return;
    }

    if (g_session_id[0] != '\0') {
        cJSON_AddStringToObject(root, "session_id", g_session_id);
    }
    cJSON_AddStringToObject(root, "type", "mcp");

    cJSON_AddStringToObject(payload, "jsonrpc", "2.0");
    cJSON_AddItemToObject(payload, "id", id_copy);

    cJSON_AddStringToObject(result, "protocolVersion",
                            protocol_version != NULL ? protocol_version : "2024-11-05");
    cJSON_AddItemToObject(result, "capabilities", capabilities);
    cJSON_AddStringToObject(client_info, "name", "rk3568-websocket-test");
    cJSON_AddStringToObject(client_info, "version", "0.1.0");
    cJSON_AddItemToObject(result, "clientInfo", client_info);
    cJSON_AddItemToObject(payload, "result", result);
    cJSON_AddItemToObject(root, "payload", payload);

    char *msg = cJSON_PrintUnformatted(root);
    if (msg == NULL) {
        printf("[mcp] error: failed to print initialize result JSON\n");
        cJSON_Delete(root);
        return;
    }

    printf("[mcp] queue initialize result: %s\n", msg);
    if (ws_text_enqueue_ex(msg, strlen(msg), WS_TEXT_KIND_MCP_INITIALIZE_RESULT, "mcp_initialize")) {
        request_ws_text_write();
    } else {
        printf("[mcp] error: failed to queue initialize result\n");
    }

    free(msg);
    cJSON_Delete(root);
}

/* ==================== MCP tools/list 响应 ==================== */
static void SendMCPToolsListResult(cJSON *request_id)
{
    if (request_id == NULL || (!cJSON_IsNumber(request_id) && !cJSON_IsString(request_id))) {
        printf("[mcp] warning: skip tools/list result because request id is missing\n");
        return;
    }

    if (cJSON_IsNumber(request_id)) {
        printf("[mcp] tools/list request id=%g\n", request_id->valuedouble);
    } else {
        printf("[mcp] tools/list request id=%s\n", request_id->valuestring);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();
    cJSON *id_copy = cJSON_Duplicate(request_id, 1);

    if (root == NULL || payload == NULL || result == NULL ||
        tools == NULL || id_copy == NULL) {
        printf("[mcp] error: failed to allocate tools/list result JSON\n");
        cJSON_Delete(root);
        cJSON_Delete(payload);
        cJSON_Delete(result);
        cJSON_Delete(tools);
        cJSON_Delete(id_copy);
        return;
    }

    if (g_session_id[0] != '\0') {
        cJSON_AddStringToObject(root, "session_id", g_session_id);
    }
    cJSON_AddStringToObject(root, "type", "mcp");

    cJSON_AddStringToObject(payload, "jsonrpc", "2.0");
    cJSON_AddItemToObject(payload, "id", id_copy);
    cJSON_AddItemToObject(result, "tools", tools);
    cJSON_AddItemToObject(payload, "result", result);
    cJSON_AddItemToObject(root, "payload", payload);

    char *msg = cJSON_PrintUnformatted(root);
    if (msg == NULL) {
        printf("[mcp] error: failed to print tools/list result JSON\n");
        cJSON_Delete(root);
        return;
    }

    printf("[mcp] queue tools/list result: %s\n", msg);
    if (ws_text_enqueue_ex(msg, strlen(msg), WS_TEXT_KIND_MCP_TOOLS_LIST_RESULT, "mcp_tools_list")) {
        printf("[mcp] tools/list result queued for writeable callback\n");
        request_ws_text_write();
    } else {
        printf("[mcp] error: failed to queue tools/list result\n");
    }

    free(msg);
    cJSON_Delete(root);
}

/* ==================== TTS 消息处理 ==================== */
static void ProcessTTS(cJSON *root)
{
    printf("🔊 [TTS] 文本转语音消息\n");
    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsString(state) && state->valuestring != NULL) {
        printf("  状态：%s\n", state->valuestring);
        
        if (strcmp(state->valuestring, "stop") == 0 && !interrupted) {
            printf("🔄 TTS 播放结束，自动开始监听...\n");
            if (client_wsi) {
                StartListen(client_wsi, "after_tts_stop");
            }
        }
    }
}

/* ==================== STT 消息处理 ==================== */
static void ProcessSTT(cJSON *root)
{
    printf("🎤 [STT] 语音转文本消息\n");
    
    cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(text) && text->valuestring != NULL) {
        printf("  识别文本：%s\n", text->valuestring);
    }
}

/* ==================== 信号处理 ==================== */
static void sigint_handler(int sig) {
    printf("\n⚠️  收到信号 %d，正在关闭...\n", sig);
    interrupted = 1;
}

/* ==================== WebSocket 回调 ==================== */
static int callback_echo(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user, void *in, size_t len) {

    unsigned char buf[LWS_PRE + 2048];
    unsigned char *p = &buf[LWS_PRE];
    
    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
    {
        unsigned char **hdr_ptr = (unsigned char **)in;
        unsigned char *end = (*hdr_ptr) + len;
        
        lws_add_http_header_by_name(wsi, (unsigned char *)"Authorization:", 
            (unsigned char *)conn_params.access_token, strlen(conn_params.access_token), hdr_ptr, end);
        lws_add_http_header_by_name(wsi, (unsigned char *)"Protocol-Version:", 
            (unsigned char *)conn_params.version, strlen(conn_params.version), hdr_ptr, end);    
        lws_add_http_header_by_name(wsi, (unsigned char *)"Device-Id:", 
            (unsigned char *)conn_params.device_id, strlen(conn_params.device_id), hdr_ptr, end);
        lws_add_http_header_by_name(wsi, (unsigned char *)"Client-Id:", 
            (unsigned char *)conn_params.client_id, strlen(conn_params.client_id), hdr_ptr, end);
        break;
    }
    
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        printf("✅ 连接成功！\n");
        client_wsi = wsi;
        hello_sent = 0;
        g_session_id[0] = '\0';
        g_session_id_saved = 0;
        g_listen_start_sent_count = 0;
        g_listen_after_tools_list_sent = 0;
        g_after_tools_list_listen_sent_time = 0;
        g_listen_stop_queued = 0;
        g_listen_stop_sent = 0;
        g_listen_stop_sent_count = 0;
        g_mcp_initialize_replied = 0;
        g_mcp_initialized_notification_recv = 0;
        g_mcp_tools_list_replied = 0;
        LogProtocolState("ws_established");
        start_audio_thread();
        lws_callback_on_writable(wsi);
        break;
        
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        printf("[ws] connection error: %s\n", in ? (char *)in : "unknown");
        LogProtocolState("ws_connection_error");
        printf("❌ 连接错误：%s\n", in ? (char *)in : "未知错误");
        interrupted = 1;
        break;
        
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (!hello_sent) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "type", "hello");
            cJSON_AddNumberToObject(root, "version", 1);
            cJSON_AddStringToObject(root, "transport", "websocket");
            
            cJSON *features = cJSON_CreateObject();
            cJSON_AddBoolToObject(features, "mcp", true);
            cJSON_AddItemToObject(root, "features", features);

            cJSON *audio_params = cJSON_CreateObject();
            cJSON_AddStringToObject(audio_params, "format", "opus");
            cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
            cJSON_AddNumberToObject(audio_params, "channels", 1);
            cJSON_AddNumberToObject(audio_params, "frame_duration", 60);
            cJSON_AddItemToObject(root, "audio_params", audio_params);
            
            char *msg = cJSON_PrintUnformatted(root);

            size_t msg_len = strlen(msg);
            memcpy(&buf[LWS_PRE], msg, msg_len);
            printf("📝 发送：%.*s\n", (int)msg_len, &buf[LWS_PRE]);
            int n = lws_write(wsi, &buf[LWS_PRE], msg_len, LWS_WRITE_TEXT);
            if (n < (int)msg_len) {
                printf("❌ 发送失败\n");
            }
            
            cJSON_Delete(root);
            free(msg);
            hello_sent = 1;

            if (audio_tx_pending_count() > 0) {
                lws_callback_on_writable(wsi);
            }
            break;
        }

        unsigned char text_out[LWS_PRE + WS_TEXT_MAX_SIZE];
        size_t text_len = 0;
        ws_text_kind_t text_kind = WS_TEXT_KIND_GENERIC;
        char text_reason[32] = {0};
        if (ws_text_dequeue((char *)&text_out[LWS_PRE], &text_len,
                            &text_kind, text_reason, sizeof(text_reason))) {
            int n = lws_write(wsi, &text_out[LWS_PRE], text_len, LWS_WRITE_TEXT);
            g_ws_text_sent_count++;
            printf("[ws-text] lws_write text returned %d/%zu sent_count=%lu\n",
                   n, text_len, g_ws_text_sent_count);
            if (n < 0) {
                fprintf(stderr, "[ws-text] lws_write text failed\n");
            } else if (n < (int)text_len) {
                fprintf(stderr, "[ws-text] lws_write text incomplete: %d/%zu\n", n, text_len);
            } else if (text_kind == WS_TEXT_KIND_LISTEN_START) {
                g_listen_start_sent_count++;
                if (strcmp(text_reason, "after_tools_list") == 0) {
                    g_listen_after_tools_list_sent = 1;
                    g_after_tools_list_listen_sent_time = time(NULL);
                    printf("[listen] after_tools_list sent_time=%ld stop_after_sec=%d\n",
                           (long)g_after_tools_list_listen_sent_time,
                           g_listen_stop_after_sec);
                }
                printf("[listen] sent start reason=%s ret=%d/%zu\n",
                       text_reason[0] != '\0' ? text_reason : "unknown", n, text_len);
                LogProtocolState("listen_start_sent");
            } else if (text_kind == WS_TEXT_KIND_LISTEN_STOP) {
                g_listen_stop_sent = 1;
                g_listen_stop_sent_count++;
                printf("[listen] sent stop reason=%s ret=%d/%zu\n",
                       text_reason[0] != '\0' ? text_reason : "unknown", n, text_len);
                LogProtocolState("listen_stop_sent");
            } else if (text_kind == WS_TEXT_KIND_MCP_INITIALIZE_RESULT) {
                g_mcp_initialize_replied = 1;
                printf("[mcp] initialize result sent ret=%d/%zu\n", n, text_len);
                LogProtocolState("mcp_initialize_result_sent");
            } else if (text_kind == WS_TEXT_KIND_MCP_TOOLS_LIST_RESULT) {
                g_mcp_tools_list_replied = 1;
                printf("[mcp] tools/list result sent ret=%d/%zu\n", n, text_len);
                LogProtocolState("mcp_tools_list_result_sent");
                if (g_session_id_saved && !g_listen_after_tools_list_sent) {
                    StartListen(wsi, "after_tools_list");
                }
            }

            if (ws_text_pending_count() > 0 || audio_tx_pending_count() > 0) {
                lws_callback_on_writable(wsi);
            }
            break;
        }

        unsigned char out[LWS_PRE + UDP_AUDIO_MAX_SIZE];
        size_t payload_len = 0;
        if (audio_tx_dequeue(&out[LWS_PRE], &payload_len)) {
            int n = lws_write(wsi, &out[LWS_PRE], payload_len, LWS_WRITE_BINARY);
            if (n < 0) {
                fprintf(stderr, "[audio] lws_write binary failed\n");
            } else if (n < (int)payload_len) {
                fprintf(stderr, "[audio] lws_write binary incomplete: %d/%zu\n", n, payload_len);
            } else {
                g_ws_audio_sent_count++;
                if (g_ws_audio_sent_count <= 5 ||
                    (g_ws_audio_sent_count % AUDIO_LOG_INTERVAL) == 0) {
                    printf("[audio] ws_binary_sent=%lu len=%zu pending=%zu dropped=%lu\n",
                           g_ws_audio_sent_count, payload_len,
                           audio_tx_pending_count(), g_audio_tx_dropped_count);
                }
            }

            if (audio_tx_pending_count() > 0) {
                lws_callback_on_writable(wsi);
            }
        }
        break;
    
    case LWS_CALLBACK_CLIENT_RECEIVE:
        printf("[ws] receive %s len=%zu\n", lws_frame_is_binary(wsi) ? "binary" : "text", len);
        if (lws_frame_is_binary(wsi)) {
            ProcessBinDataFrmServer((unsigned char *)in, len);
        } else {
            ProcessTxtDataFrmServer((const char *)in, len);
        }
        break;
    
    case LWS_CALLBACK_CLIENT_CLOSED:
        printf("[ws] client closed\n");
        LogProtocolState("ws_closed");
        printf("🔒 连接关闭\n");
        stop_audio_thread();
        ws_text_clear();
        client_wsi = NULL;
        interrupted = 1;
        break;
        
    case LWS_CALLBACK_WSI_DESTROY:
        printf("[ws] wsi destroy\n");
        LogProtocolState("ws_wsi_destroy");
        break;

    default:
        break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    { DEFAULT_PROTOCOL, callback_echo, 4096, 0, NULL, 0 },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

// 自定义连接信息
typedef struct {
    const char *hostname;
    int port;
    const char *path;
    int use_ssl;
} xiaozhi_server_t;

xiaozhi_server_t xiaozhi_server = {
    "api.tenclass.net",  // 目标服务器
    443,                 // 端口
    "/xiaozhi/v1/",      // 路径
    1                    // 使用SSL
};

/* ==================== 主函数 ==================== */
int main(void) {
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);
    
    struct lws_context *context;
    struct lws_context_creation_info ctx_info;
    struct lws_client_connect_info conn_info;
    
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);
    LoadListenConfig();
    
    printf("========================================\n");
    printf("小智 WebSocket 客户端 (websocket_test)\n");
    printf("版本：%s\n", lws_get_library_version());
    printf("服务器：api.tenclass.net:443/xiaozhi/v1/\n");
    printf("========================================\n\n");
    
    if (init_udp_forward() != 0) {
        return 1;
    }
    memset(&ctx_info, 0, sizeof(ctx_info));
    ctx_info.port = CONTEXT_PORT_NO_LISTEN;
    ctx_info.protocols = protocols;
    ctx_info.gid = -1;
    ctx_info.uid = -1;
    ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    ctx_info.ssl_ca_filepath = "/etc/ssl/certs/ca-certificates.crt";
    
    context = lws_create_context(&ctx_info);
    if (!context) {
        fprintf(stderr, "创建上下文失败\n");
        cleanup_udp_forward();
        return 1;
    }
    g_lws_context = context;
    
    memset(&conn_info, 0, sizeof(conn_info));
    conn_info.context = context;
    conn_info.address = xiaozhi_server.hostname;
    conn_info.port = xiaozhi_server.port;
    conn_info.path = xiaozhi_server.path;
    conn_info.host = xiaozhi_server.hostname;
    conn_info.origin = xiaozhi_server.hostname;
    conn_info.protocol = "xiaozhi-protocol";
    conn_info.ssl_connection = xiaozhi_server.use_ssl ? LCCSCF_USE_SSL : 0;
    conn_info.local_protocol_name = "xiaozhi-protocol";
    
    printf("正在连接 wss://api.tenclass.net/xiaozhi/v1/ ...\n");
    
    struct lws *wsi = lws_client_connect_via_info(&conn_info);
    if (!wsi) {
        fprintf(stderr, "连接失败\n");
        g_lws_context = NULL;
        lws_context_destroy(context);
        cleanup_udp_forward();
        return 1;
    }
    
    printf("[信息] 进入事件循环 (按 Ctrl+C 退出)\n");
    
    while (!interrupted) {
        lws_service(context, 50);
        CheckListenStopTimer();
    }
    
    printf("正在清理资源...\n");
    
    if (g_audio_thread_running) {
        stop_audio_thread();
    }
    PrintAudioStats("final");
    
    g_lws_context = NULL;
    lws_context_destroy(context);
    cleanup_udp_forward();
    
    printf("程序结束\n");
    return 0;
}
