#pragma once

#include <QDebug>
#include <QImage>             // Qt 的图像类（最终显示给用户看的）
#include <QRegularExpression>
#include <QThread>
#include <atomic>             // std::atomic：m_bExitThread 跨线程读写
#include "packets_sync.h"

// 调试开关：是否打印视频缓冲信息
#define PRINT_VIDEO_BUFFER_INFO 0

/**
 * @brief 视频缩放/重采样的上下文
 * FFmpeg 解码出的视频帧一般是 YUV 格式，不能直接显示，
 * 需要通过 sws_scale 转成 RGB 才能给 QImage 用。
 *
 * ## 为什么需要它？
*FFmpeg 解码出来的视频是 YUV 格式 （一种节省带宽的颜色编码方式），
*但 Qt 的 QImage 只认识 RGB 格式 → 中间必须转一次。
 */
typedef struct Video_Resample
{
    AVFrame* pFrameRGB{nullptr};        // 转换后的 RGB 帧
    uint8_t* buffer_RGB{nullptr};       // RGB 数据缓冲区
    struct SwsContext* sws_ctx{nullptr};// sws 转换上下文（保存转换参数）
    int dst_width{0};                   // 缩放后宽度（4K 自动降到 1920）
    int dst_height{0};                  // 缩放后高度
} Video_Resample;

/**
 * @brief 视频播放线程
 *
 * 流水线的"第三站"——视频部分。
 * 视频播放最复杂，主要做三件事：
 *   1. 从 pictq 取出解码好的 AVFrame
 *   2. sws_scale 把 YUV 转成 RGB，并用 OpenCV 做可选的图像特效
 *   3. 封装成 QImage 通过 frame_ready 信号发给主窗口显示
 *
 * 还要处理：
 *   - 音视频同步：每帧该显示多久（用主时钟计算）
 *   - 字幕解析：从 subpq 取字幕，处理 ASS 格式
 *   - 倍速处理：把视频按当前播放速度调整
 */
class VideoPlayThread : public QThread
{
    Q_OBJECT
public:
    explicit VideoPlayThread(QObject* parent = nullptr, VideoState* pState = nullptr);
    ~VideoPlayThread();

public:
    // 初始化 sws 重采样参数（视频格式、尺寸变化时需要重新初始化）
    bool init_resample_param(AVCodecContext* pVideo, bool bHardware = false);

public slots:
    void stop_thread();  // 外部调用，让线程退出

signals:
    void frame_ready(const QImage&);         // 一帧图像准备好，发送给主窗口显示
    void subtitle_ready(const QString&);     // 一段字幕准备好，发送给主窗口显示
    void playback_finished();                // ★ 视频自然播放完毕（读到 EOF），通知主线程自动停止

protected:
    void run() override;  // Qt 线程入口

private:
    // 计算下一帧应该什么时候显示（同步逻辑的核心）
    void video_refresh(VideoState* is, double* remaining_time);
    // 把当前帧显示到屏幕（真正发出 frame_ready 信号）
    void video_image_display(VideoState* is);
    // 视频显示的封装（含字幕叠加）
    void video_display(VideoState* is);
    void final_resample_param();  // 释放 sws 相关资源
    inline int compute_mod(int a, int b) { return a < 0 ? (a % b + b) : (a % b); }
    // 解析 ASS 格式的字幕（去掉样式标签，提取纯文本）
    void parse_subtitle_ass(const QString& text);

private:
    VideoState* m_pState{nullptr};   // 共享播放状态
    Video_Resample m_Resample;       // 缩放/转换上下文
    std::atomic<bool> m_bExitThread{false};  // 退出标志（由主线程写、本线程读，必须原子）

    // ASS 字幕解析用的正则表达式
    const static QRegularExpression m_assFilter;
    const static QRegularExpression m_assNewLineReplacer;
};
