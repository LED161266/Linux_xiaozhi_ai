#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <opus/opus.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

/* ==================== 常量定义 ==================== */
#define SAMPLE_RATE         16000      /* 修改：16000 → 24000 */
#define CHANNELS            1
#define FRAME_DURATION_MS   60
#define FRAME_SIZE          (SAMPLE_RATE * FRAME_DURATION_MS / 1000)  /* 240 采样点 */
#define MAX_PACKET_SIZE     8000
#define ALSA_DEVICE         "hw:audiocodec"
#define ALSA_DEVICE_ENV     "XIAOZHI_ALSA_DEVICE"
#define ALSA_CAPTURE_DEVICE_ENV  "XIAOZHI_ALSA_CAPTURE_DEVICE"
#define ALSA_PLAYBACK_DEVICE_ENV "XIAOZHI_ALSA_PLAYBACK_DEVICE"

/* 预缓冲队列 */
#define PRE_BUFFER_FRAMES   10        /* 修改：10 */

/* ==================== UDP 配置 ==================== */
#define UDP_SEND_PORT       9001        /* 发送端口 */
#define UDP_RECV_PORT       9002        /* 接收端口（与 xiaozhi_http.c 一致）*/
#define UDP_LOCAL_IP        "127.0.0.1"

/* ==================== 全局变量 ==================== */
/* 编码器相关 */
static OpusEncoder *g_encoder = NULL;
static int g_send_sockfd = -1;
static struct sockaddr_in g_send_addr;

/* 解码器相关 */
static OpusDecoder *g_decoder = NULL;
static int g_recv_sockfd = -1;
static snd_pcm_t *g_alsa_play_handle = NULL;
static struct sockaddr_in g_recv_addr;
static pthread_t g_recv_thread;

/* 采集相关 */
static snd_pcm_t *g_alsa_handle = NULL;
static const char *g_alsa_capture_device = ALSA_DEVICE;
static const char *g_alsa_playback_device = ALSA_DEVICE;
static volatile int g_running = 1;

/* 统计信息 */
static int g_frame_received = 0;
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ==================== 函数声明 ==================== */
static int init_opus_encoder(void);
static void cleanup_opus(void);
static int init_opus_decoder(void);
static void cleanup_opus_decoder(void);
static int init_udp_socket(void);
static void cleanup_udp(void);
static int init_udp_recv_socket(void);
static void cleanup_udp_recv(void);
static int init_alsa_capture(void);
static void cleanup_alsa(void);
static int init_alsa_playback(void);
static void cleanup_alsa_playback(void);
static const char *getenv_nonempty(const char *name);
static void configure_alsa_devices(void);
static void *udp_recv_decode_play_thread(void *arg);
static int audio_capture_and_encode(void);
static void graceful_shutdown(void);
static void signal_handler(int sig);

/* ==================== 信号处理 ==================== */
static void signal_handler(int sig)
{
    printf("\n⚠️  收到信号 %d，正在关闭...\n", sig);
    g_running = 0;
    
    /* 关闭 socket 唤醒阻塞的 recvfrom */
    if (g_recv_sockfd >= 0) {
        shutdown(g_recv_sockfd, SHUT_RD);
    }
    if (g_send_sockfd >= 0) {
        shutdown(g_send_sockfd, SHUT_WR);
    }
    if (g_alsa_handle) {
        snd_pcm_drop(g_alsa_handle);
    }
    if (g_alsa_play_handle) {
        snd_pcm_drop(g_alsa_play_handle);
    }
}

/* ==================== 优雅退出处理 ==================== */
static void graceful_shutdown(void)
{
    printf("\n[主程序] 开始优雅退出...\n");
    g_running = 0;
    
    if (g_recv_thread) {
        pthread_cancel(g_recv_thread);
        pthread_join(g_recv_thread, NULL);
    }
    
    cleanup_opus_decoder();
    cleanup_opus();
    cleanup_alsa_playback();
    cleanup_alsa();
    cleanup_udp_recv();
    cleanup_udp();
    
    printf("[主程序] 退出处理完成\n");
}

/* ==================== Opus 编码器初始化 ==================== */
static int init_opus_encoder(void)
{
    int error;
    g_encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK || g_encoder == NULL) {
        fprintf(stderr, "创建 Opus 编码器失败：%s\n", opus_strerror(error));
        return -1;
    }
    printf("✓ Opus 编码器初始化成功\n");
    return 0;
}

static void cleanup_opus(void)
{
    if (g_encoder) {
        opus_encoder_destroy(g_encoder);
        g_encoder = NULL;
    }
}

/* ==================== Opus 解码器初始化 ==================== */
static int init_opus_decoder(void)
{
    int error;
    g_decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);
    if (error != OPUS_OK || g_decoder == NULL) {
        fprintf(stderr, "创建 Opus 解码器失败：%s\n", opus_strerror(error));
        return -1;
    }
    printf("✓ Opus 解码器初始化成功\n");
    return 0;
}

static void cleanup_opus_decoder(void)
{
    if (g_decoder) {
        opus_decoder_destroy(g_decoder);
        g_decoder = NULL;
    }
}

/* ==================== ALSA 采集初始化 ==================== */
static const char *getenv_nonempty(const char *name)
{
    const char *value = getenv(name);
    return (value && value[0] != '\0') ? value : NULL;
}

static void configure_alsa_devices(void)
{
    const char *common_device = getenv_nonempty(ALSA_DEVICE_ENV);
    const char *capture_device = getenv_nonempty(ALSA_CAPTURE_DEVICE_ENV);
    const char *playback_device = getenv_nonempty(ALSA_PLAYBACK_DEVICE_ENV);

    if (common_device) {
        g_alsa_capture_device = common_device;
        g_alsa_playback_device = common_device;
    }
    if (capture_device) {
        g_alsa_capture_device = capture_device;
    }
    if (playback_device) {
        g_alsa_playback_device = playback_device;
    }

    printf("ALSA capture device: %s\n", g_alsa_capture_device);
    printf("ALSA playback device: %s\n", g_alsa_playback_device);
}

static int init_alsa_capture(void)
{
    int err;
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;
    
    err = snd_pcm_open(&g_alsa_handle, g_alsa_capture_device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "无法打开音频设备 %s: %s\n", g_alsa_capture_device, snd_strerror(err));
        return -1;
    }
    
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_sw_params_alloca(&sw_params);
    
    err = snd_pcm_hw_params_any(g_alsa_handle, hw_params);
    if (err < 0) {
        fprintf(stderr, "无法初始化硬件参数：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_hw_params_set_access(g_alsa_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        fprintf(stderr, "无法设置访问类型：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_hw_params_set_format(g_alsa_handle, hw_params, SND_PCM_FORMAT_S16_LE);
    if (err < 0) {
        fprintf(stderr, "无法设置采样格式：%s\n", snd_strerror(err));
        return -1;
    }
    
    unsigned int rate = SAMPLE_RATE;
    err = snd_pcm_hw_params_set_rate_near(g_alsa_handle, hw_params, &rate, 0);
    if (err < 0) {
        fprintf(stderr, "无法设置采样率：%s\n", snd_strerror(err));
        return -1;
    }
    
    unsigned int channels = CHANNELS;
    err = snd_pcm_hw_params_set_channels(g_alsa_handle, hw_params, channels);
    if (err < 0) {
        fprintf(stderr, "无法设置通道数：%s\n", snd_strerror(err));
        return -1;
    }
    
    snd_pcm_uframes_t period_size = FRAME_SIZE;
    err = snd_pcm_hw_params_set_period_size_near(g_alsa_handle, hw_params, &period_size, 0);
    if (err < 0) {
        fprintf(stderr, "无法设置周期大小：%s\n", snd_strerror(err));
        return -1;
    }
    
    snd_pcm_uframes_t buffer_size = period_size * 4;
    err = snd_pcm_hw_params_set_buffer_size_near(g_alsa_handle, hw_params, &buffer_size);
    if (err < 0) {
        fprintf(stderr, "无法设置缓冲区大小：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_hw_params(g_alsa_handle, hw_params);
    if (err < 0) {
        fprintf(stderr, "无法应用硬件参数：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_sw_params_current(g_alsa_handle, sw_params);
    if (err < 0) {
        fprintf(stderr, "无法获取软件参数：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_sw_params_set_start_threshold(g_alsa_handle, sw_params, 1);
    if (err < 0) {
        fprintf(stderr, "无法设置开始阈值：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_sw_params_set_stop_threshold(g_alsa_handle, sw_params, buffer_size);
    if (err < 0) {
        fprintf(stderr, "无法设置停止阈值：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_sw_params_set_avail_min(g_alsa_handle, sw_params, period_size);
    if (err < 0) {
        fprintf(stderr, "无法设置可用最小空间：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_sw_params(g_alsa_handle, sw_params);
    if (err < 0) {
        fprintf(stderr, "无法应用软件参数：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_prepare(g_alsa_handle);
    if (err < 0) {
        fprintf(stderr, "无法准备音频设备：%s\n", snd_strerror(err));
        return -1;
    }
    
    printf("✓ ALSA 采集设备初始化成功\n");
    printf("  period 大小：%lu 帧，buffer 大小：%lu 帧（约 %lu ms）\n", 
           period_size, buffer_size, buffer_size * 1000 / rate);
    return 0;
}

static void cleanup_alsa(void)
{
    if (g_alsa_handle) {
        snd_pcm_drain(g_alsa_handle);
        snd_pcm_close(g_alsa_handle);
        g_alsa_handle = NULL;
    }
}

/* ==================== ALSA 播放初始化 ==================== */
static int init_alsa_playback(void)
{
    int err;
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;
    unsigned int rate = SAMPLE_RATE;
    unsigned int channels = CHANNELS;
    
    err = snd_pcm_open(&g_alsa_play_handle, g_alsa_playback_device, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "无法打开播放设备 %s: %s\n", g_alsa_playback_device, snd_strerror(err));
        return -1;
    }
    
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_sw_params_alloca(&sw_params);
    
    err = snd_pcm_hw_params_any(g_alsa_play_handle, hw_params);
    if (err < 0) {
        fprintf(stderr, "无法初始化硬件参数：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_hw_params_set_access(g_alsa_play_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        fprintf(stderr, "无法设置访问类型：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_hw_params_set_format(g_alsa_play_handle, hw_params, SND_PCM_FORMAT_S16_LE);
    if (err < 0) {
        fprintf(stderr, "无法设置采样格式：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_hw_params_set_rate_near(g_alsa_play_handle, hw_params, &rate, 0);
    if (err < 0) {
        fprintf(stderr, "无法设置采样率：%s\n", snd_strerror(err));
        return -1;
    }
    
    /* 验证实际采样率 */
    if (rate != SAMPLE_RATE) {
        fprintf(stderr, "⚠️  警告：请求 %dHz，实际设置 %dHz\n", SAMPLE_RATE, rate);
    }

    err = snd_pcm_hw_params_set_channels(g_alsa_play_handle, hw_params, channels);
    if (err < 0) {
        fprintf(stderr, "无法设置通道数：%s\n", snd_strerror(err));
        return -1;
    }
    
    /* 使用 FRAME_SIZE 设置 period */
    snd_pcm_uframes_t period_size = FRAME_SIZE;
    err = snd_pcm_hw_params_set_period_size_near(g_alsa_play_handle, hw_params, &period_size, 0);
    if (err < 0) {
        fprintf(stderr, "无法设置周期大小：%s\n", snd_strerror(err));
        return -1;
    }
    
    /* 缓冲区大小 = period * 8 */
    snd_pcm_uframes_t buffer_size = period_size * 8;
    err = snd_pcm_hw_params_set_buffer_size_near(g_alsa_play_handle, hw_params, &buffer_size);
    if (err < 0) {
        fprintf(stderr, "无法设置缓冲区大小：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_hw_params(g_alsa_play_handle, hw_params);
    if (err < 0) {
        fprintf(stderr, "无法应用硬件参数：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_sw_params_current(g_alsa_play_handle, sw_params);
    if (err < 0) {
        fprintf(stderr, "无法获取软件参数：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_sw_params_set_start_threshold(g_alsa_play_handle, sw_params, buffer_size / 2);
    if (err < 0) {
        fprintf(stderr, "无法设置开始阈值：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_sw_params_set_stop_threshold(g_alsa_play_handle, sw_params, buffer_size);
    if (err < 0) {
        fprintf(stderr, "无法设置停止阈值：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_sw_params_set_avail_min(g_alsa_play_handle, sw_params, period_size);
    if (err < 0) {
        fprintf(stderr, "无法设置可用最小帧数：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_sw_params(g_alsa_play_handle, sw_params);
    if (err < 0) {
        fprintf(stderr, "无法应用软件参数：%s\n", snd_strerror(err));
        return -1;
    }
    
    err = snd_pcm_prepare(g_alsa_play_handle);
    if (err < 0) {
        fprintf(stderr, "无法准备播放设备：%s\n", snd_strerror(err));
        return -1;
    }
    
    printf("✓ ALSA 播放设备初始化成功：%uHz, %u 通道\n", rate, channels);
    printf("  周期大小：%lu 帧，缓冲区大小：%lu 帧（约 %lu ms）\n", 
           period_size, buffer_size, buffer_size * 1000 / rate);
    return 0;
}

static void cleanup_alsa_playback(void)
{
    if (g_alsa_play_handle) {
        snd_pcm_drain(g_alsa_play_handle);
        snd_pcm_close(g_alsa_play_handle);
        g_alsa_play_handle = NULL;
    }
}

/* ==================== UDP 发送 Socket 初始化 ==================== */
static int init_udp_socket(void)
{
    g_send_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_send_sockfd < 0) {
        perror("创建发送 Socket 失败");
        return -1;
    }
    
    memset(&g_send_addr, 0, sizeof(g_send_addr));
    g_send_addr.sin_family = AF_INET;
    g_send_addr.sin_port = htons(UDP_SEND_PORT);
    inet_pton(AF_INET, UDP_LOCAL_IP, &g_send_addr.sin_addr);
    
    printf("✓ UDP 发送 Socket 初始化成功，发送地址：%s:%d\n", UDP_LOCAL_IP, UDP_SEND_PORT);
    return 0;
}

static void cleanup_udp(void)
{
    if (g_send_sockfd >= 0) {
        close(g_send_sockfd);
        g_send_sockfd = -1;
    }
}

/* ==================== UDP 接收 Socket 初始化 ==================== */
static int init_udp_recv_socket(void)
{
    int optval = 1;
    
    g_recv_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_recv_sockfd < 0) {
        perror("创建接收 Socket 失败");
        return -1;
    }
    
    setsockopt(g_recv_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    
    /* 设置接收超时，避免阻塞 */
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(g_recv_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    memset(&g_recv_addr, 0, sizeof(g_recv_addr));
    g_recv_addr.sin_family = AF_INET;
    g_recv_addr.sin_port = htons(UDP_RECV_PORT);
    g_recv_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(g_recv_sockfd, (struct sockaddr *)&g_recv_addr, sizeof(g_recv_addr)) < 0) {
        perror("绑定接收端口失败");
        close(g_recv_sockfd);
        g_recv_sockfd = -1;
        return -1;
    }
    
    printf("✓ UDP 接收 Socket 初始化成功，监听端口：%d\n", UDP_RECV_PORT);
    return 0;
}
static void cleanup_udp_recv(void)
{
    if (g_recv_sockfd >= 0) {
        close(g_recv_sockfd);
        g_recv_sockfd = -1;
    }
}

/* ==================== UDP 接收解码播放线程（参考 xiaozhi_http.c）==================== */
static void *udp_recv_decode_play_thread(void *arg)
{
    unsigned char packet[MAX_PACKET_SIZE];
    opus_int16 pcm[FRAME_SIZE * CHANNELS];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    opus_int16 pre_buffer[PRE_BUFFER_FRAMES][FRAME_SIZE * CHANNELS];
    int pre_buffer_count = 0;
    
    printf("\n[接收线程] 音频接收线程启动\n");
    printf("  监听端口：%d (UDP)\n", UDP_RECV_PORT);
    
    if (init_opus_decoder() != 0) {
        pthread_exit(NULL);
    }
    
    if (init_alsa_playback() != 0) {
        cleanup_opus_decoder();
        pthread_exit(NULL);
    }
    
    printf("  ✓ 等待音频数据...\n");
    
    /* 预缓冲阶段 */
    printf("  [预缓冲] 正在缓冲 %d 帧...\n", PRE_BUFFER_FRAMES);
    while (g_running && pre_buffer_count < PRE_BUFFER_FRAMES) {
        int read_len = recvfrom(g_recv_sockfd, packet, MAX_PACKET_SIZE, 0,
                                (struct sockaddr *)&client_addr, &addr_len);
        if (read_len < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!g_running) break;
                continue;
            }
            break;
        }
        
        if (read_len == sizeof(int)) {
            int flag;
            memcpy(&flag, packet, sizeof(int));
            if (flag == 0) {
                pre_buffer_count = 0;
                continue;
            }
        }
        
        if (read_len <= 0 || read_len > MAX_PACKET_SIZE) {
            continue;
        }
        
        int samples = opus_decode(g_decoder, packet, read_len, 
                                  pre_buffer[pre_buffer_count], FRAME_SIZE, 0);
        if (samples < 0) {
            samples = FRAME_SIZE;
            memset(pre_buffer[pre_buffer_count], 0, sizeof(pcm));
        }
        
        pre_buffer_count++;
        printf("\r  [预缓冲] 已缓冲 %d/%d 帧", pre_buffer_count, PRE_BUFFER_FRAMES);
        fflush(stdout);
    }
    printf("\n  [预缓冲] 完成，开始播放\n");
    
    if (pre_buffer_count > 0) {
        /* 逐帧写入预缓冲数据 */
        for (int i = 0; i < pre_buffer_count; i++) {
            snd_pcm_writei(g_alsa_play_handle, pre_buffer[i], FRAME_SIZE);
        }
    }
    
    /* 正常播放阶段 */
    while (g_running) {
        int read_len = recvfrom(g_recv_sockfd, packet, MAX_PACKET_SIZE, 0,
                                (struct sockaddr *)&client_addr, &addr_len);
        if (read_len < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            usleep(10000);
            continue;
        }
        
        if (read_len == sizeof(int)) {
            int flag;
            memcpy(&flag, packet, sizeof(int));
            if (flag == 0) {
                usleep(100000);
                continue;
            }
        }
        
        if (read_len <= 0 || read_len > MAX_PACKET_SIZE) {
            continue;
        }
        
        int samples = opus_decode(g_decoder, packet, read_len, pcm, FRAME_SIZE, 0);
        if (samples < 0) {
            printf("⚠️  Opus 解码失败：%s (数据长度：%d)\n", opus_strerror(samples), read_len);
            samples = FRAME_SIZE;
            memset(pcm, 0, sizeof(pcm));
        } else {
            /* 添加解码样本数验证 */
            if (samples != FRAME_SIZE) {
                printf("⚠️  解码样本数异常：期望 %d，实际 %d\n", FRAME_SIZE, samples);
            }
        }
        
        int write_frames = snd_pcm_writei(g_alsa_play_handle, pcm, samples);
        if (write_frames < 0) {
            if (write_frames == -EPIPE) {
                snd_pcm_prepare(g_alsa_play_handle);
            }
            continue;
        }
        
        pthread_mutex_lock(&g_stats_mutex);
        g_frame_received++;
        pthread_mutex_unlock(&g_stats_mutex);
        
        printf("\r  [接收线程] 帧长度：%d 字节，解码样本：%d，已播放 %d 帧", 
            read_len, samples, g_frame_received);
        fflush(stdout);
    }
    
    printf("\n  [接收线程] 播放线程退出，共播放 %d 帧\n", g_frame_received);
    
    cleanup_opus_decoder();
    cleanup_alsa_playback();
    pthread_exit(NULL);
}


/* ==================== 音频采集与编码 ==================== */
static int audio_capture_and_encode(void)
{
    opus_int16 pcm[FRAME_SIZE * CHANNELS];
    unsigned char packet[MAX_PACKET_SIZE];
    int total_frames = 0;
    
    printf("\n[采集线程] 开始持续采集并编码，通过 UDP 发送...\n");
    
    /* 设置 ALSA 非阻塞模式 */
    /* Keep capture in blocking mode so ALSA provides natural audio pacing. */
    
    while (g_running) {
        int read_count = snd_pcm_readi(g_alsa_handle, pcm, FRAME_SIZE);
        
        if (read_count <= 0) {
            if (read_count == -EAGAIN || read_count == -EINTR) {
                usleep(1000);
                continue;
            }
            if (read_count == -EPIPE) {
                snd_pcm_prepare(g_alsa_handle);
                continue;
            }
            if (read_count == 0) {
                continue;
            }
            break;
        }
        
        if (read_count < FRAME_SIZE) {
            memset(pcm + read_count, 0, sizeof(opus_int16) * (FRAME_SIZE - read_count));
        }
        
        int packet_size = opus_encode(g_encoder, pcm, FRAME_SIZE, packet, MAX_PACKET_SIZE);
        if (packet_size < 0) {
            break;
        }
        
        sendto(g_send_sockfd, packet, packet_size, 0,
               (struct sockaddr *)&g_send_addr, sizeof(g_send_addr));
        
        total_frames++;
    }
    
    int end_flag = 0;
    sendto(g_send_sockfd, &end_flag, sizeof(int), 0,
           (struct sockaddr *)&g_send_addr, sizeof(g_send_addr));
    
    printf("\n  [采集线程] 采集编码完成，共 %d 帧\n", total_frames);
    return 0;
}

/* ==================== 主函数 ==================== */
int main(int argc, char *argv[])
{
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║         小智 Opus 音频处理 (UDP 收发版)              ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    configure_alsa_devices();
    
    if (init_udp_socket() != 0 || init_udp_recv_socket() != 0) {
        return -1;
    }
    
    if (init_alsa_capture() != 0 || init_opus_encoder() != 0) {
        return -1;
    }
    
    if (pthread_create(&g_recv_thread, NULL, udp_recv_decode_play_thread, NULL) != 0) {
        perror("创建接收线程失败");
        return -1;
    }
    printf("✓ 接收线程已启动\n");
    
    usleep(200000);
    printf("\n=== 运行中 (Ctrl+C 退出) ===\n\n");
    
    audio_capture_and_encode();
    
    g_running = 0;
    usleep(500000);
    
    /* 带超时保护的线程等待 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    
    if (pthread_timedjoin_np(g_recv_thread, NULL, &ts) != 0) {
        printf("⚠️  接收线程等待超时，强制取消\n");
        pthread_cancel(g_recv_thread);
        pthread_join(g_recv_thread, NULL);
    }
    
    cleanup_opus_decoder();
    cleanup_opus();
    cleanup_alsa_playback();
    cleanup_alsa();
    cleanup_udp_recv();
    cleanup_udp();
    
    printf("=== 程序正常退出 ===\n");
    return 0;
}
