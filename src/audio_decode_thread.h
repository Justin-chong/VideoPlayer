#pragma once

#include <QThread>
#include "packets_sync.h"

/**
 * @brief 音频解码线程
 *
 * 流水线的"第二站"——音频部分。
 * 工作循环（与视频解码线程类似）：
 *   1. 从 audioq 取一个 AVPacket
 *   2. 送给音频解码器（avcodec_send_packet / avcodec_receive_frame）
 *   3. 拿到 AVFrame 后会经过音频滤镜（实现倍速等）
 *   4. 把处理好的 AVFrame 推入 sampq（音频采样队列），给播放线程用
 */
class AudioDecodeThread : public QThread
{
    Q_OBJECT

public:
    explicit AudioDecodeThread(QObject* parent = nullptr, VideoState* pState = nullptr);
    ~AudioDecodeThread();

protected:
    void run() override;  // Qt 线程入口

private:
    VideoState* m_pState;  // 共享播放状态（含 audioq、sampq、音频滤镜等）
};
