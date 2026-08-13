// ***********************************************************/
// read_thread.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 读包线程实现（流水线的第一站）
// ***********************************************************/

#include "read_thread.h"

// 外部全局变量，定义在 mainwindow.cpp 里
extern int infinite_buffer;       // 1=不限速（实时流），0=按队列大小限速
extern int64_t start_time;        // 用户指定的开始时间（微秒）
// 当前文件的总时长
static int64_t duration = AV_NOPTS_VALUE;

ReadThread::ReadThread(QObject* parent, VideoState* pState)
    : QThread(parent), m_pPlayData(pState)
{
}

ReadThread::~ReadThread()
{
}

void ReadThread::set_video_state(VideoState* pState)
{
    assert(pState);
    m_pPlayData = pState;//将外部传入的VideoState赋值给私有变量
}

/**
 * @brief 读包主循环
 *
 * 这个函数是整个读包线程的核心，无限循环地：
 *   1. 处理暂停/恢复
 *   2. 处理 seek 请求
 *   3. 处理附件请求（如封面）
 *   4. 检查队列是否已满（满了就等 10ms 避免内存爆炸）
 *   5. 调 av_read_frame() 读一个包
 *   6. 根据 stream_index 分发到 audioq / videoq / subtitleq
 *
 * 循环退出的条件：
 *   - 收到 abort_request
 *   - 读到了 EOF（非循环播放）
 *   - 网络流读错误
 */
int ReadThread::loop_read()
{
    int ret = -1;
    VideoState* is = m_pPlayData;
    AVPacket* pkt = nullptr;
    int pkt_in_play_range = 0;
    int64_t stream_start_time = 0;
    int64_t pkt_ts = 0;
    // AVFormatContext* pFormatCtx = is->ic;
    assert(is);
    if (!is)
        return ret;

    // 分配一个 AVPacket，后续循环里反复复用
    pkt = av_packet_alloc();
    if (!pkt)
    {
        av_log(nullptr, AV_LOG_FATAL, "Could not allocate packet.\n");
        ret = AVERROR(ENOMEM);
        return ret;
    }

    is->read_thread_exit = 0;  // ★ 标记读包线程已启动（外部用来判断线程是否在跑）

    for (;;)
    {
        // 1. 退出请求（停止播放时会设置 abort_request=1）
        if (is->abort_request)
            break;  // ★ 跳出整个读包循环，触发后续清理

        // 2. 处理暂停/恢复
        // 直播流需要调 av_read_pause / av_read_play 来暂停网络拉流
        if (is->paused != is->last_paused)  // ★ 仅当状态发生变化时才处理，避免重复调
        {
            is->last_paused = is->paused;    // ★ 先更新旧状态，防止下一次循环重复进入
            if (is->paused)
                is->read_pause_return = av_read_pause(is->ic);  // ★ 通知底层协议栈暂停拉流
            else
                av_read_play(is->ic);                          // ★ 恢复拉流
        }

        // 3. 处理 seek 请求
        if (is->seek_req)//seek_req=1代表要处理seek,
        {
            int64_t seek_target = is->seek_pos;
            // ★ seek_min/max 是 FFmpeg 的容错窗口
            // 允许 demuxer 在 [min, max] 区间内选最接近的 I 帧（关键帧）
            // 向前 seek 时窗口往前扩 2，反之向后扩 2
            int64_t seek_min =
                is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
            int64_t seek_max =
                is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;

            // 调 FFmpeg 的 seek 函数，跳转到指定时间
            //avformat_seek_file = "快进 / 倒退到指定时间" ，
            //是 FFmpeg 提供的官方"拖进度条"接口
            /*
int avformat_seek_file(
    AVFormatContext *s,          // ① 文件上下文（哪个文件）
    int stream_index,            // ② 参考哪个流（-1 = 自动选）
    int64_t min_ts,              // ③ 最早能跳到哪
    int64_t ts,                  // ④ 目标时间（要跳到哪）
    int64_t max_ts,              // ⑤ 最晚能跳到哪
    int flags                    // ⑥ 标志（按字节/按关键帧...）
);
             */
            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max,
                                     is->seek_flags);

            if (ret < 0)
            {
                // ★ seek 失败常见原因：格式不支持 seek、流已结束
                // 不致命，仅记日志，继续播放
                av_log(nullptr, AV_LOG_ERROR, "%s: error while seeking\n", is->ic->url);
            }
            else
            {
                // seek 成功后，清空所有队列（因为旧数据已经过期了）
                // ★ 旧队列里的 packet 全部"过期"了，不清会导致音画不同步
                if (is->audio_stream >= 0)
                    packet_queue_flush(&is->audioq);
                if (is->subtitle_stream >= 0)
                    packet_queue_flush(&is->subtitleq);
                if (is->video_stream >= 0)
                    packet_queue_flush(&is->videoq);
                // ★ 同步外部时钟到 seek 目标
                // AVSEEK_FLAG_BYTE 表示按字节偏移而非时间，NAN 表示未知
                if (is->seek_flags & AVSEEK_FLAG_BYTE)
                {
                    set_clock(&is->extclk, NAN, 0);
                }
                else
                {
                    set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;            // ★ 消费完 seek 请求
            is->queue_attachments_req = 1; // ★ 请求重新入队附件（封面等）
            is->eof = 0;                  // ★ seek 后重置 EOF 标志
            if (is->paused)
                step_to_next_frame(is);   // ★ 暂停状态下，单步走到下一帧
        }

        // 4. 处理附件请求（比如 mp3 里的封面图）
        if (is->queue_attachments_req)
        {
            // ★ 仅当视频流有 attached_pic（封面）时才处理
            if (is->video_st &&
                is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)
            {
                // ★ av_packet_ref 浅拷贝 attached_pic 的数据到 pkt
                // 后面再入队，避免直接用 attached_pic 引用计数混乱
                ret = av_packet_ref(pkt, &is->video_st->attached_pic);
                if (ret < 0)
                    break;
                packet_queue_put(&is->videoq, pkt);
                // ★ 紧跟一个"空包"告诉解码器：封面已发完，接下来是真正的视频
                packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
            }
            is->queue_attachments_req = 0; // ★ 消费完附件请求
        }

        /* 5. 如果队列已满，等待 10ms 避免内存爆炸
         * 条件：累计大小 > 15MB 或所有流都缓存了足够包
         */
        // ★ 这是背压机制（back-pressure）：解码/播放线程消费不过来时，读包线程主动阻塞
        //   - MAX_QUEUE_SIZE 防内存爆炸
        //   - stream_has_enough_packets 给每个流单独一个合理缓冲
        if (infinite_buffer < 1 &&
            (is->audioq.size + is->videoq.size + is->subtitleq.size >
                 MAX_QUEUE_SIZE ||
             (stream_has_enough_packets(is->audio_st, is->audio_stream,
                                        &is->audioq) &&
              stream_has_enough_packets(is->video_st, is->video_stream,
                                        &is->videoq) &&
              stream_has_enough_packets(is->subtitle_st, is->subtitle_stream,
                                        &is->subtitleq))))
        {
            /* wait 10 ms */
            // ★ 用条件变量 wait 10ms，期间会释放锁，允许其他线程修改 continue_read_thread
            //   continue_read_thread 在 seek/暂停时会被 wake，可提前解除等待
            m_waitMutex.lock();
            is->continue_read_thread->wait(&m_waitMutex, 10);
            m_waitMutex.unlock();
            continue;  // ★ 重新从循环开头检查 abort/seek 等
        }

        // 6. 真正读一个 AVPacket
        ret = av_read_frame(is->ic, pkt);
        //错误。比如网络断了、文件损坏等。可以用 av_strerror() 拿到可读的错误信息
        if (ret < 0)
        {
            // 读到文件末尾的处理
            if ((ret == AVERROR_EOF || avio_feof(is->ic->pb)) && !is->eof)
            {
                // ★ 给每个流发"空包"（flush packet），告诉解码器可以退出了
                //   否则解码器会一直等下一帧，无法判定流结束
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq, pkt, is->subtitle_stream);

                // 循环播放：seek 回 0；否则退出
                if (is->loop)
                {
                    // ★ 循环模式：重新 seek 到开头，实现无缝循环
                    stream_seek(is, 0, 0, 0);
                }
                else
                {
                    is->eof = 1;  // ★ 标记已读到末尾，UI 线程据此显示"播放完毕"
                    break;
                }
            }
            // 网络错误
            // ★ avio_feof 之外的真错误（如网络断开、403），直接退出线程
            if (is->ic->pb && is->ic->pb->error)
            {
                break;
            }

            // ★ 非致命错误（如临时网络抖动）→ 等 10ms 再试
            //   与"队列满"时一样，用 continue_read_thread 等待
            m_waitMutex.lock();
            //等待前会先解锁，然后再加锁
            is->continue_read_thread->wait(&m_waitMutex, 10);
            m_waitMutex.unlock();
            continue;
        }
        else
        {
            is->eof = 0;  // ★ 成功读到包，重置 EOF（曾因 EOF 退出过，循环回 0 后必须清标志）
        }

        /* 7. 判断包是否在播放范围内（用户可能指定了起止时间）
         *   在范围内才放入队列；否则丢弃
         */
        // ★ 计算 pkt 的"相对文件头"的播放时间（秒）
        //   start_time：流的起始时间戳（某些 mkv/mp4 文件非 0）
        //   start_time：用户在 UI 指定的"开始播放的时间"
        stream_start_time = is->ic->streams[pkt->stream_index]->start_time;
        // ★ pts 没有时退化为 dts（解码时间戳）
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        // ★ pkt_in_play_range = 1 表示这个包位于用户指定的 [start, start+duration] 范围内
        //   公式：packet相对时间(秒) <= 持续时长(秒) → 在范围内
        pkt_in_play_range =
            duration == AV_NOPTS_VALUE ||
            (pkt_ts -
             (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                        av_q2d(is->ic->streams[pkt->stream_index]->time_base) -
                    (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) /
                        1000000 <=
                ((double)duration / 1000000);

        // 8. 根据 stream_index 分发到不同的队列，一共有3个PacketQueue和3个FrameQueue
        // ★ 关键分发逻辑：按 stream_index 决定 packet 送给谁
        //   - 命中对应的流 + 在播放范围内 → 入队
        //   - 命中视频但 disposition 是封面 → 跳过（封面在前面单独处理过）
        //   - 其它不感兴趣的流 → 立即 unref 释放，避免内存泄漏
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range)
        {
            packet_queue_put(&is->audioq, pkt);
        }
        else if (pkt->stream_index == is->video_stream && pkt_in_play_range &&
                 !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC))
        {
            packet_queue_put(&is->videoq, pkt);
        }
        else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range)
        {
            packet_queue_put(&is->subtitleq, pkt);
        }
        else
        {
            // 不感兴趣的包（其它流），直接释放
            // ★ 这里必须 unref，否则 av_read_frame 分配的 buffer 永远不释放，内存会涨
            av_packet_unref(pkt);
        }
    }

    // ★ 退出前标记 read_thread_exit = -1，外部可据此判断读包线程已彻底退出
    is->read_thread_exit = -1;
    av_packet_free(&pkt);  // ★ 释放循环中复用的 packet 容器
    return 0;
}

void ReadThread::run()
{
    int ret = loop_read();
    if (ret < 0)
    {
        qDebug("-------- Read packets thread exit, with error=%d\n", ret);
    }
    else
    {
        qDebug("-------- Read packets thread exit.");
    }
}
