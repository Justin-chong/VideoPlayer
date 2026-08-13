#pragma once

#include <QThread>
#include "packets_sync.h"

/**
 * @brief 读包线程（解复用线程）
 *
 * 这是播放流水线的"第一站"，负责：
 *   1. 调用 avformat_open_input() 打开本地文件 / 网络流
 *   2. 查找流信息（视频流、音频流、字幕流）
 *   3. 打开对应的解码器
 *   4. 循环调用 av_read_frame() 读取 AVPacket
 *   5. 根据 packet 的 stream_index 分别放入 videoq / audioq / subtitleq
 *
 * 整个播放过程只有这一个线程负责"读数据"，其它线程（解码、播放）都从队列取数据。
 */
class ReadThread : public QThread
{
    Q_OBJECT

public:
    explicit ReadThread(QObject* parent = nullptr, VideoState* pState = nullptr);
    ~ReadThread();

public:
    void set_video_state(VideoState* pState = nullptr); // call before start thread
    // 启动线程前必须调用，把要处理的 VideoState 传进来

protected:
    // 打开某个流（视频/音频/字幕），包括找解码器、打开解码器、启动对应解码线程
    int stream_component_open(int stream_index);
    // 关闭某个流，释放资源
    void stream_component_close(VideoState* is, int stream_index);
    // 循环读包的逻辑主体（被 run() 调用）
    int loop_read();

protected:
    void run() override;  // Qt 线程入口函数，启动后会执行这里面的代码

private:
    VideoState* m_pPlayData;   // 共享的播放状态
    QMutex m_waitMutex;        // 等待用的互斥锁
};
