#pragma once

#include <QThread>
#include "packets_sync.h"

/**
 * @brief 字幕解码线程
 *
 * 流水线的"第二站"——字幕部分。
 * 工作流程（与视频/音频解码线程类似，但更轻量）：
 *   1. 从 subtitleq 取一个 AVPacket
 *   2. 用字幕解码器解码（libavcodec 支持 SRT、ASS、SSA 等）
 *   3. 拿到 AVSubtitle 后推入 subpq（字幕帧队列）
 *
 * 视频播放线程会从 subpq 取字幕，叠加到画面上。
 */
class SubtitleDecodeThread : public QThread
{
    Q_OBJECT

public:
    explicit SubtitleDecodeThread(QObject* parent = nullptr, VideoState* pState = nullptr);
    ~SubtitleDecodeThread();

protected:
    void run() override;  // Qt 线程入口

private:
    VideoState* m_pState;  // 共享播放状态（含 subtitleq 和 subpq）
};
