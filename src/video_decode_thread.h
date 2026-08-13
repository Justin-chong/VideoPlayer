#pragma once

#include <QThread>
#include "packets_sync.h"

/**
 * @brief 视频解码线程
 *
 * 流水线的"第二站"——视频部分。
 * 工作循环：
 *   1. 从 videoq（视频包队列）取一个 AVPacket
 *   2. 调 avcodec_send_packet() 送给解码器
 *   3. 调 avcodec_receive_frame() 拿到解码后的 AVFrame
 *   4. 把 AVFrame 推入 pictq（视频帧队列），给播放线程用
 *
 * 注意：解码一帧视频可能要消耗很多 CPU，所以单独放一个线程里跑，
 *       不会卡住读包线程和播放线程。
 */
class VideoDecodeThread : public QThread
{
    Q_OBJECT

public:
    explicit VideoDecodeThread(QObject* parent = nullptr, VideoState* pState = nullptr);
    ~VideoDecodeThread();

protected:
    void run() override;  // Qt 线程入口

private:
    VideoState* m_pState;  // 共享的播放状态（里面包含 videoq 和 pictq）
};
