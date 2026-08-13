/**
 * @file packets_sync.h
 * @brief 播放器核心数据结构与线程同步原语定义
 *
 * 【小白必读】这个头文件是整个播放器的"骨架"。
 * 播放器使用多线程流水线（读包 → 解码 → 播放），而不同线程之间需要交换数据：
 *   - 读包线程把 AVPacket 放进 PacketQueue
 *   - 解码线程从 PacketQueue 取 AVPacket，解码出 AVFrame 放进 FrameQueue
 *   - 播放线程从 FrameQueue 取 AVFrame 拿去显示
 * 此外，还要解决"音视频同步"（唇同步）的问题，所以这里定义了 Clock 时钟。
 *
 * 本文件结构：
 *   1. 一些调优用的宏（队列大小、同步阈值等）
 *   2. PacketQueue —— 线程安全的包队列
 *   3. FrameQueue  —— 线程安全的帧队列
 *   4. Decoder    —— 单路解码器的状态
 *   5. Clock      —— 播放时钟（用于音视频同步）
 *   6. VideoState —— 整个播放会话的全量状态（所有线程共享它）
 *   7. 一堆操作这些数据结构的函数声明（实现在 packets_sync.cpp）
 */

#pragma once

#include <QMutex>
#include <QThread>
#include <QWaitCondition>

// 是否启用 FFmpeg 滤镜系统开关：
// 音频滤镜：用于倍速播放（atempo 滤镜）、音量调整等
// 视频滤镜：这里关闭，因为视频同步主要由音频时钟控制
#define USE_AVFILTER_AUDIO 1
#define USE_AVFILTER_VIDEO 0

// 下面这一段是 FFmpeg 的 C 接口头文件，必须用 extern "C" 包裹，
// 因为 FFmpeg 是 C 写的，C++ 直接 include 会出现链接错误。
extern "C"
{
#include <libavcodec/avcodec.h>      // 编解码相关
#include <libavformat/avformat.h>    // 容器格式（mp4、flv 等）
#include <libavutil/bprint.h>
#include <libavutil/fifo.h>          // FFmpeg 自带的 FIFO 队列
#include <libavutil/imgutils.h>      // 图像工具（分配图像 buffer）
#include <libavutil/samplefmt.h>     // 音频采样格式
#include <libavutil/time.h>          // 时间相关
// ★ 7.x 已移除：avfft.h 被合并到 libavutil/tx.h
// #include <libavcodec/avfft.h>        // 快速傅里叶变换（用于音频可视化）
#include <libswresample/swresample.h>// 音频重采样（把任意采样率/格式转成统一格式）
#include <libswscale/swscale.h>      // 视频像素格式转换与缩放

// 是否使用 FFmpeg 的滤镜系统（滤镜可以做变速、混音、特效等）
// 这里只对音频开启滤镜（用来实现倍速播放），视频暂时不用
#if USE_AVFILTER_AUDIO
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avstring.h>
#include <libavutil/macros.h>
#include <libavutil/opt.h>
#endif
}

// 下面这堆宏是各种调优参数，大部分是从 FFplay 抄过来的。
// 不用深究每个数字的意思，先有个印象即可，等需要调性能时再来翻。

// PacketQueue 队列允许占用的最大字节数（15MB），超过就暂停读包
#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
// 队列里最少要缓存多少帧，避免播放线程饿死
#define MIN_FRAMES 25
// 外部时钟模式下，队列最少/最多帧数（用于动态调整播放速度）
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

// 下面的 SDL_xxx 宏其实是 FFplay 时代遗留的，SDL 是 FFplay 用的窗口库。
// 这个项目换成了 Qt，但保留了这些宏名。
#define SDL_MIX_MAXVOLUME 128                                  // 音量最大值
#define SDL_AUDIO_MIN_BUFFER_SIZE 512                          // 最小音频缓冲（采样数）
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30                     // 每秒最多回调次数
#define SDL_VOLUME_STEP (0.75)                                 // 每次音量调节的步长（dB）

// ===== 音视频同步阈值（单位：秒）=====
// 当音视频差值小于 0.04s 时，不做同步校正（容忍范围内）
#define AV_SYNC_THRESHOLD_MIN 0.04
// 超过 0.1s 时，进行同步校正（重复或丢帧）
#define AV_SYNC_THRESHOLD_MAX 0.1
// 帧时长超过 0.1s 时，不通过重复帧来补偿同步
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
// 音视频差超过 10s 时，认为是异常，不再同步
#define AV_NOSYNC_THRESHOLD 10.0

// 音频最大变速百分比（用于同步时微调音频速度）
#define SAMPLE_CORRECTION_PERCENT_MAX 10

// 外部时钟模式下，根据队列缓冲动态调整播放速度的范围
#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

// 音视频差值平均窗口大小（用最近 20 次差值求平均）
#define AUDIO_DIFF_AVG_NB 20

// 视频刷新轮询间隔（秒），应小于 1/fps
#define REFRESH_RATE 0.01

// 音频采样数组大小（用于音频可视化时存储历史采样点）
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000  // 鼠标自动隐藏延迟（微秒）

#define USE_ONEPASS_SUBTITLE_RENDER 1  // 是否单次渲染字幕

/**
 * @brief PacketQueue 中的一个节点
 *
 * 队列里不是直接放 AVPacket，而是包一层 MyAVPacketList，
 * 多保存一个 serial 序号，用来识别"这一包属于哪一次播放"，
 * seek 之后会换新 serial，旧包就可以丢弃。
 */
typedef struct MyAVPacketList
{
    AVPacket* pkt;     // 真正的数据包
    int serial;        // 包所属的播放序号（seek 时会 +1）
} MyAVPacketList;

/**
 * @brief 包队列（线程安全）
 *
 * 用于在不同线程之间传递 AVPacket：
 *   生产者：read_thread 把 av_read_frame 读到的包放进来
 *   消费者：video/audio/subtitle decode_thread 取出去解码
 *
 * 字段解释：
 *   - pkt_list: FFmpeg 的 AVFifo 队列
 *   - nb_packets: 队列里的包数量
 *   - size: 队列里所有包的字节总和（用于判断是否超过 MAX_QUEUE_SIZE，限制队列大小）
 *   - duration: 所有包的累计时长，入队时长增加，出队时长减少；假设没有duration限制，
 *   如果 生产者太快 （读包很快）， 消费者太慢 （解码卡），队列就会无限制增长。
 *   需要size和duation同时限制是为了防止不同视频码率
 *      4K 高码率：1 秒 50 MB
 *      360p 低码率：1 秒 0.1 MB，，
 *   只限 15MB
 *      4K：15MB / 50MB/s = 0.3 秒就满了 → 限流很严
 *      360p：15MB / 0.1MB/s = 150 秒才满 → 几乎不限流
 *   - abort_request: 1 请求所有线程中止运行（用于让阻塞中的线程退出）
 *   - serial: 当前播放的序号
 *   - mutex/cond: Qt 的互斥锁和条件变量，实现线程同步
 */
typedef struct PacketQueue
{
    AVFifo* pkt_list;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    QMutex* mutex;//上面6个变量是多线程共享
    QWaitCondition* cond;//条件变量
} PacketQueue;

// 各类帧队列的容量（最多缓存几帧）
#define VIDEO_PICTURE_QUEUE_SIZE 3   // 视频帧队列容量
#define SUBPICTURE_QUEUE_SIZE 16     // 字幕帧队列容量
#define SAMPLE_QUEUE_SIZE 20        // 音频采样队列容量
// FrameQueue 数组总大小：取三者最大值
#define FRAME_QUEUE_SIZE     \
    FFMAX(SAMPLE_QUEUE_SIZE, \
          FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

/**
 * @brief 音频参数
 * 描述一段音频的基本属性，用于重采样时知道源格式和目标格式。
 */
typedef struct AudioParams
{
    int freq;                       // 采样率（如 44100 Hz）
    AVChannelLayout ch_layout;      // 声道布局（立体声/5.1 等）
    enum AVSampleFormat fmt;        // 采样格式（S16、FLTP 等）
    int frame_size;                 // 一帧的采样数
    int bytes_per_sec;              // 每秒字节数
} AudioParams;

/**
 * @brief 播放时钟（音视频同步的核心）
 *
 * 播放器有三种时钟：视频时钟、音频时钟、外部时钟。
 * 同步时以某个时钟为主时钟，其它流跟上。
 *
 * 字段解释：
 *   - pts: 时钟当前指向的 PTS（presentation time stamp，显示时间戳）
 *   - pts_drift: 漂移量（用于校准系统时间带来的误差）
 *   - last_updated: 上次更新时钟的真实时间
 *   - speed: 倍速（1.0 为正常速度）
 *   - serial: 时钟所属的播放序号（seek 后会变）
 *   - paused: 是否暂停
 *   - queue_serial: 指向对应 PacketQueue 的 serial，用来判断时钟是否过期
 */
typedef struct Clock
{
    double pts;       /* clock base */
    double pts_drift; /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial; /* clock is based on a packet with this serial */
    int paused;
    int* queue_serial; /* pointer to the current packet queue serial, used for
                        obsolete clock detection */
} Clock;

typedef struct FrameData
{
    int64_t pkt_pos;
} FrameData;

/**
 * @brief 帧（解码后的数据单元）
 * 视频帧、音频帧、字幕帧都用这个结构。
 * 通过 union 思路节省空间：frame 字段给视频/音频用，sub 字段给字幕用。
 */
typedef struct Frame
{
    AVFrame* frame;       // 视频/音频的解码结果
    AVSubtitle sub;       // 字幕的解码结果
    int serial;           // 该帧所属的播放序号
    double pts;           // 显示时间戳（单位：秒）
    double duration;      // 该帧应显示多久（视频一帧通常 1/fps 秒）
    int64_t pos;          // 该帧在文件中的字节位置
    int width;
    int height;
    int format;           // 像素格式 / 采样格式
    AVRational sar;       // 像素宽高比（Sample Aspect Ratio）
    int uploaded;         // 是否已上传到 GPU（已弃用，本项目用 QImage）
    int flip_v;           // 是否需要垂直翻转
} Frame;

/**
 * @brief 帧队列（线程安全，循环数组实现）
 *
 * 用一个固定大小的数组 + 读写指针模拟环形队列，比链表更省内存。
 * 写指针 windex 不断后移，读指针 rindex 不断后移，超过 max_size 就回绕。
 *
 * 字段解释：
 *   - queue: 帧数组
 *   - rindex: 读位置（消费者从这里取）
 *   - windex: 写位置（生产者往这里放）
 *   - size: 当前已有多少帧
 *   - max_size: 最多能放多少帧
 *   - keep_last: 是否保留最后一帧（防止 seek 后黑屏）
 *   - rindex_shown: 当前 rindex 位置的帧是否已被显示过
 *   - pktq: 反向指向对应的 PacketQueue
 */
typedef struct FrameQueue
{
    //FRAME_QUEUE_SIZE大小为20
    Frame queue[FRAME_QUEUE_SIZE]; // array queue model, loop queue
    int rindex;                    // read pointer
    int windex;                    // write pointer
    int size;                      // current frame num
    int max_size;                  // max frame num
    int keep_last;                 // keep last frame
    int rindex_shown;              // current frame is shown
    QMutex* mutex;//互斥锁
    QWaitCondition* cond;//条件变量，与互斥锁实现线程同步互斥
    PacketQueue* pktq;
} FrameQueue;

/**
 * @brief 音视频同步模式
 *   - AV_SYNC_AUDIO_MASTER: 默认模式，以音频时钟为准（因为音频卡顿耳朵能听出来）
 *   - AV_SYNC_VIDEO_MASTER: 以视频时钟为准（纯视频或音频无意义时使用）
 *   - AV_SYNC_EXTERNAL_CLOCK: 以外部时钟为准（实时流用）
 */
enum
{
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

/**
 * @brief 单路解码器的状态
 * 每个流（视频/音频/字幕）一个 Decoder 实例。
 */
typedef struct Decoder
{
    AVPacket* pkt;                     // 当前正在解码的包
    PacketQueue* queue;                // 输入的包队列
    AVCodecContext* avctx;             // FFmpeg 解码器上下文
    int pkt_serial;                    // 当前包的 serial
    int finished;                      // 是否已结束（读到末尾）
    int packet_pending;                // 是否有待解码的包
    QWaitCondition* empty_queue_cond;  // 队列空时，解码线程在这里等待
    int64_t start_pts;                 // 起始 PTS（处理 seek 后的连续性）
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    void* decoder_tid;                 // 解码线程的指针（实际是 QThread*）
    char* decoder_name;                // 解码器名称（用于日志）
} Decoder;

/**
 * @brief 所有工作线程的指针集合
 * 方便 VideoState 统一管理线程的生命周期。
 */
typedef struct Threads
{
    QThread* read_tid{nullptr};
    QThread* video_decode_tid{nullptr};
    QThread* audio_decode_tid{nullptr};
    QThread* video_play_tid{nullptr};
    QThread* audio_play_tid{nullptr};
    QThread* subtitle_decode_tid{nullptr};
} Threads;

/**
 * @brief 整个播放会话的全量状态
 *
 * 这是项目里最大、最重要的结构体。
 * 几乎所有线程都要读写它，所以访问时必须加锁（VideoState 自己内部不带锁，
 * 使用它的人根据字段加锁）。
 *
 * 可以把它想象成"播放器的大脑"：
 *   - 包含三个流（视频/音频/字幕）的包队列、帧队列、解码器
 *   - 包含三个时钟用于同步
 *   - 包含 AVFormatContext（解复用器）
 *   - 包含滤镜图（用于倍速播放等）
 *   - 包含所有线程指针
 */
typedef struct VideoState
{
    const AVInputFormat* iformat;        // 输入格式（可强制指定）
    int abort_request;                   // 1 表示请求退出所有线程
    int force_refresh;                   // 强制刷新画面（用于窗口尺寸变化）
    int paused;                          // 当前是否暂停
    int last_paused;                     // 上一次的暂停状态
    int queue_attachments_req;           // 请求处理附件（封面、章节等）
    int seek_req;                        // 收到 seek 请求的标志
    int seek_flags;                      // seek 标志（前后退、关键帧等）
    int64_t seek_pos;                    // seek 目标位置
    int64_t seek_rel;                    // 相对 seek 偏移  大于0就是向前快进，小于0就是后退
    int read_pause_return;               // 读线程暂停时是否立即返回
    AVFormatContext* ic;                 // FFmpeg 解复用器上下文（相当于"打开的文件"）
    int realtime;                        // 是否是实时流（直播）

    Clock vidclk;                        // 视频时钟
    Clock audclk;                        // 音频时钟
    Clock extclk;                        // 外部时钟

    PacketQueue videoq;                  // 视频包队列
    PacketQueue audioq;                  // 音频包队列
    PacketQueue subtitleq;               // 字幕包队列

    FrameQueue pictq;                    // 视频帧队列
    FrameQueue sampq;                    // 音频采样队列
    FrameQueue subpq;                    // 字幕帧队列

    Decoder viddec;                      // 视频解码器
    Decoder auddec;                      // 音频解码器
    Decoder subdec;                      // 字幕解码器

    int audio_stream;                    // 选中的音频流索引
    int av_sync_type;                    // 当前用的同步模式

    double audio_clock;                  // 音频时钟缓存
    int audio_clock_serial;              // 音频时钟序号

    double audio_clock_old;              // 变速后用于更新时钟

    AVStream* audio_st;                  // 音频流

    int audio_volume;                    // 音量（0~SDL_MIX_MAXVOLUME）
    int muted;                           // 是否静音

    int frame_drops_early;               // 解码阶段丢帧数
    int frame_drops_late;                // 显示阶段丢帧数

#if 0
    // 以下是早期 SDL 版本遗留字段，已不使用，但保留注释方便查阅
    double audio_diff_avg_coef;          // 音视频差值平均系数
    int audio_diff_avg_count;
    double audio_diff_cum;               // 累计差值
    double audio_diff_threshold;
    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    uint8_t* audio_buf;
    uint8_t* audio_buf1;
    unsigned int audio_buf_size;
    unsigned int audio_buf1_size;
    int audio_buf_index;
    int audio_write_buf_size;
    int audio_hw_buf_size;
    struct SwrContext* swr_ctx;
    enum ShowMode {
        SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
    } show_mode;
#endif

    int last_i_start;                    // 上一帧 I 帧位置
    int rdft_bits;                       // FFT 位数（用于音频可视化）
    int xpos;                            // 频谱绘制 X 坐标
    double last_vis_time;                // 上次绘制可视化的时间

    int subtitle_stream;                 // 选中的字幕流索引
    AVStream* subtitle_st;               // 字幕流

    double frame_timer;                  // 上一帧显示时间（用于同步计算）
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;                    // 选中的视频流索引
    AVStream* video_st;                  // 视频流

    double max_frame_duration;           // 帧最大间隔（用于检测时间戳跳跃）
    struct SwsContext* img_convert_ctx;  // 视频像素转换上下文（YUV → RGB）
    struct SwsContext* sub_convert_ctx;  // 字幕转换上下文
    int eof;                             // 是否已到文件末尾
    int loop;                            // 是否循环播放

    char* filename;                      // 当前播放的文件名
    int width, height, xleft, ytop;      // 视频原始尺寸与显示位置
    int step;                            // 是否处于"逐帧步进"模式

#if USE_AVFILTER_AUDIO
    struct AudioParams audio_src;        // 音频源参数
    struct AudioParams audio_tgt;        // 音频目标参数（重采样目标）

    double audio_speed;                  // 当前播放速度
    char* afilters;                      // 音频滤镜描述字符串（如 "atempo=2.0"）
    int req_afilter_reconfigure;         // 请求重新配置音频滤镜
    char* vfilters;                      // 视频滤镜描述字符串
    int req_vfilter_reconfigure;

    struct AudioParams audio_filter_src; // 滤镜前的音频参数
    int vfilter_idx;
    AVFilterContext* in_video_filter;    // 视频滤镜链入口
    AVFilterContext* out_video_filter;   // 视频滤镜链出口
    AVFilterContext* in_audio_filter;    // 音频滤镜链入口
    AVFilterContext* out_audio_filter;   // 音频滤镜链出口
    AVFilterGraph* agraph;               // 音频滤镜图
    AVFilterGraph* vgraph;               // 视频滤镜图
#endif

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    QWaitCondition* continue_read_thread;  // 读线程等待条件（用于暂停/限流）
    int read_thread_exit;                  // 读线程退出标志

    Threads threads;                       // 所有工作线程指针
} VideoState;

// 调试开关：是否打印包队列状态信息（0=关闭，1=开启）
#if !NDEBUG
#define PRINT_PACKETQUEUE_INFO 0
#define PRINT_PACKETQUEUE_AUDIO_INFO 0
#else
#define PRINT_PACKETQUEUE_INFO 0
#define PRINT_PACKETQUEUE_AUDIO_INFO 0
#endif

/*************** PacketQueue 操作 *****************/
// 下面这一组函数是包队列的操作接口，定义在 packets_sync.cpp 里。
// 名字都很直白：init=初始化，destroy=销毁，get=取出，put=放入。
int packet_queue_init(PacketQueue* q);                             // 初始化队列
void packet_queue_destroy(PacketQueue* q);                        // 销毁队列（释放内存）
void packet_queue_flush(PacketQueue* q);                           // 清空队列（seek 时用）
void packet_queue_start(PacketQueue* q);                           // 启动队列（serial++，开启新一轮）
void packet_queue_abort(PacketQueue* q);                           // 终止队列（让阻塞线程退出）
int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block, int* serial); // 取一个包，block=是否阻塞等待
int packet_queue_put(PacketQueue* q, AVPacket* pkt);               // 放入一个包
int packet_queue_put_nullpacket(PacketQueue* q, AVPacket* pkt, int stream_index); // 放一个空包（用于冲刷解码器）
int packet_queue_put_private(PacketQueue* q, AVPacket* pkt);       // put 的内部实现
void packet_queue_print(const PacketQueue* q, const AVPacket* pkt, const QString& prefix); // 打印队列状态

/*************** FrameQueue 操作 *****************/
// peek 系列：只看不取（不会移动读指针）
// peek_writable: 取一个可写位置（用于生产者放新帧）
// peek_readable: 取一个可读位置（用于消费者取帧）
int frame_queue_init(FrameQueue* f, PacketQueue* pktq, int max_size, int keep_last);
void frame_queue_destory(FrameQueue* f);
void frame_queue_unref_item(Frame* vp);                            // 释放帧里 AVFrame 的引用
void frame_queue_signal(FrameQueue* f);                            // 唤醒等待中的线程
Frame* frame_queue_peek_writable(FrameQueue* f);                   // 拿到可写位置
Frame* frame_queue_peek(FrameQueue* f);                            // 拿到当前帧
Frame* frame_queue_peek_next(FrameQueue* f);                       // 拿到下一帧
Frame* frame_queue_peek_last(FrameQueue* f);                       // 拿到最后一帧
Frame* frame_queue_peek_readable(FrameQueue* f);                   // 拿到可读帧
void frame_queue_push(FrameQueue* f);                              // 推入（写指针前移）
void frame_queue_next(FrameQueue* f);                              // 前进一帧（读指针前移）
int frame_queue_nb_remaining(FrameQueue* f);                       // 队列剩余帧数
int64_t frame_queue_last_pos(FrameQueue* f);                       // 最后一帧的位置

// 把解码好的视频帧推入 FrameQueue
int queue_picture(VideoState* is, AVFrame* src_frame, double pts, double duration, int64_t pos, int serial);
// 从 PacketQueue 取包、解码、放入 FrameQueue
int get_video_frame(VideoState* is, AVFrame* frame);

/*************** Decoder 操作 *****************/
int decoder_init(Decoder* d, AVCodecContext* avctx, PacketQueue* queue, QWaitCondition* empty_queue_cond);
int decoder_decode_frame(Decoder* d, AVFrame* frame, AVSubtitle* sub);  // 解码一帧
void decoder_destroy(Decoder* d);
int decoder_start(Decoder* d, void* thread, const char* thread_name);   // 启动解码
void decoder_abort(Decoder* d, FrameQueue* fq);                         // 中止解码
void get_file_info(const char* filename, int64_t& duration);            // 获取文件时长
void get_duration_time(const int64_t duration_us, int64_t& hours, int64_t& mins, int64_t& secs, int64_t& us); // 把微秒转时分秒

/*************** Clock 操作（音视频同步关键）*****************/
double get_clock(Clock* c);                  // 获取时钟的当前 PTS（实时计算）
void set_clock_at(Clock* c, double pts, int serial, double time);  // 在指定系统时间设置时钟
void set_clock(Clock* c, double pts, int serial);                   // 设置时钟（用当前时间）
void set_clock_speed(Clock* c, double speed);                       // 设置倍速
void init_clock(Clock* c, int* queue_serial);                       // 初始化时钟
void sync_clock_to_slave(Clock* c, Clock* slave);                   // 把 c 同步到 slave

/*************** VideoState 操作 *****************/
int get_master_sync_type(VideoState* is);                  // 获取当前主时钟类型
double get_master_clock(VideoState* is);                   // 获取主时钟的当前 PTS
void check_external_clock_speed(VideoState* is);           // 检查并调整外部时钟速度
void stream_seek(VideoState* is, int64_t pos, int64_t rel, int seek_by_bytes); // 跳转到指定位置
void toggle_pause(VideoState* is, bool pause = true);      // 切换暂停状态
void toggle_mute(VideoState* is, bool mute = true);        // 切换静音
void update_volume(VideoState* is, int sign, double step); // 调整音量
void step_to_next_frame(VideoState* is);                   // 步进到下一帧（暂停时按单帧）
double compute_target_delay(double delay, VideoState* is); // 计算同步后的目标延迟
double vp_duration(VideoState* is, Frame* vp, Frame* nextvp); // 计算两帧之间的显示时长
void update_video_pts(VideoState* is, double pts, int64_t pos, int serial); // 更新视频 PTS

#if PRINT_PACKETQUEUE_INFO
void print_state_info(VideoState* is);  // 打印全局状态（调试用）
#endif

/****************************************/
int is_realtime(AVFormatContext* s);     // 判断是否是实时流
int stream_has_enough_packets(AVStream* st, int stream_id, PacketQueue* queue); // 判断流是否已缓存足够包

#if USE_AVFILTER_AUDIO
// ===== 滤镜相关函数（用于倍速播放、特效等）=====
void set_audio_playspeed(VideoState* is, double value);    // 设置音频倍速（通过 atempo 滤镜）

int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2);  // 比较两个音频格式是否一致

int configure_audio_filters(VideoState* is, const char* afilters, int force_output_format); // 配置音频滤镜
int configure_filtergraph(AVFilterGraph* graph, const char* filtergraph, AVFilterContext* source_ctx, AVFilterContext* sink_ctx); // 通用滤镜图配置

void set_video_playspeed(VideoState* is);                  // 调整视频播放速度（视频同步音频）
int configure_video_filters(AVFilterGraph* graph, VideoState* is, const char* vfilters, AVFrame* frame);
#endif
