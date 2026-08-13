// ***********************************************************/
// audio_decode_thread.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 音频解码线程实现（流水线的第二站 - 音频部分）
// ***********************************************************/

#include "audio_decode_thread.h"

AudioDecodeThread::AudioDecodeThread(QObject* parent, VideoState* pState)
    : QThread(parent), m_pState(pState)
{
}

AudioDecodeThread::~AudioDecodeThread()
{
}

/**
 * @brief 音频解码线程主函数
 *
 * 工作流程（与视频解码类似，但更复杂，因为有滤镜）：
 *   1. 调 decoder_decode_frame() 从解码器拿一帧音频
 *   2. 检查音频参数是否变化（采样率、声道、格式），
 *      如果变了就重新配置音频滤镜（configure_audio_filters）
 *   3. 把帧推进音频滤镜（用于倍速播放等）
 *   4. 从滤镜出口拿帧，计算 PTS 和时长
 *   5. 推入 sampq（音频采样队列），给音频播放线程用
 *
 * 退出条件：decoder_decode_frame() 返回负数
 */
void AudioDecodeThread::run()
{
    assert(m_pState);
    VideoState* is = m_pState;
    // frame: 接收解码器输出的音频帧
    AVFrame* frame = av_frame_alloc();
    // af: 即将送入 sampq 的 Frame 包装
    Frame* af;
#if USE_AVFILTER_AUDIO
    int last_serial = -1;            // 上一次的包序号
    AVChannelLayout dec_channel_layout; // 声道布局
    int reconfigure;                 // 标记是否需要重建滤镜链
#endif
    int got_frame = 0;
    // tb: 时间基
    //   普通情况 = {1, sample_rate}，即 pts 单位是 1/sample_rate 秒
    //   走滤镜时 = 滤镜出口的时间基（可能不同）
    // ★ audio_play_speed（变速播放）由 atempo 滤镜实现，变速不会改 sample_rate，
    //   但 atempo 输出帧的 pts 不再是原 {1, sample_rate} 关系，必须用滤镜出口 tb
    AVRational tb;
    int ret = 0;

    if (!frame)
        return;  // ★ 分配失败直接退出，绝不解引用空指针

    // ★ 用 do-while 而非 for：因为内部 goto the_end 后要保证清理代码执行
    //   do-while 至少执行一次，条件放在末尾便于把 EAGAIN/EOF 也视为"继续"
    do
    {
        // 1. ★ 从解码器拿一帧音频
        //    decoder_decode_frame 内部会：包队列取包 → send_packet → receive_frame
        //    （audio_decode_frame 是 decoder_decode_frame 内部调用的核心解码函数）
        // ★ 返回值 < 0 = 致命错误或 abort，goto the_end 进入清理
        if ((got_frame = decoder_decode_frame(&is->auddec, frame, nullptr)) < 0)
            goto the_end;

        if (got_frame)
        {
            // 普通情况：时间基 = {1, sample_rate}
            // （即 1 个 pts 单位 = 1/44100 秒）
            // ★ 默认假设：未经滤镜时音频帧的 pts 单位 = 1/sample_rate 秒
            //   一旦下方走滤镜，这个值会被覆盖为滤镜出口 tb
            tb = AVRational{1, frame->sample_rate};

#if USE_AVFILTER_AUDIO
            // 2. ★ 检查是否需要重新配置音频滤镜
            // 触发条件（任一即可）：
            //   - 采样率变了（不同流切换）
            //   - 声道数变了
            //   - 采样格式变了
            //   - 包序号变了（seek 之后）
            // ★ audio_resampling（重采样）也由滤镜链负责（aformat/aresample）
            //   任何输入参数变化都必须重建滤镜，否则输出参数不匹配
            dec_channel_layout = frame->ch_layout;

            // cmp_audio_fmts 自定义比较函数
            // 其它 3 个是采样率/声道/serial 的对比
            reconfigure = cmp_audio_fmts(is->audio_filter_src.fmt,
                                         is->audio_filter_src.ch_layout.nb_channels,
                                         AVSampleFormat(frame->format),
                                         frame->ch_layout.nb_channels) ||
                          is->audio_filter_src.ch_layout.nb_channels !=
                              dec_channel_layout.nb_channels ||
                          is->audio_filter_src.freq != frame->sample_rate ||
                          is->auddec.pkt_serial != last_serial;  // ★ serial 变 = seek 之后，旧滤镜图失效

            // 重新配置滤镜链
            if (reconfigure || is->req_afilter_reconfigure)
            {
                // 先更新 src 参数记录
                // ★ 必须在调 configure_audio_filters 之前更新，否则配置函数看不到新参数
                is->audio_filter_src.fmt = (AVSampleFormat)frame->format;
                ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &frame->ch_layout);
                if (ret < 0)
                    goto the_end;
                is->audio_filter_src.freq = frame->sample_rate;
                last_serial = is->auddec.pkt_serial;  // ★ 记录"这次的 serial"，下轮对比

                // 重建滤镜图（atepo、volume、aformat、aresample 等）
                // ★ 包含 audio_play_speed（atempo）、audio_resampling（aresample）、音量等
                ret = configure_audio_filters(is, is->afilters, 1);
                if (ret < 0)
                    goto the_end;
                is->req_afilter_reconfigure = 0;  // ★ 消费外部"强制重建"请求
            }

            // 3. ★ 把帧送入滤镜链的入口
            //    滤镜会做：变速（atempo）、变调、重采样（aresample）、音量调整等
            // ★ av_buffersrc_add_frame 会"消费" frame 的引用；之后 frame 仍可继续用但内容已被掏空
            ret = av_buffersrc_add_frame(is->in_audio_filter, frame);
            if (ret < 0)
                goto the_end;

            // 4. 从滤镜链的出口拿帧
            //    一次解码帧可能产生多个输出帧（如 atempo 拆分）
            //    0 = 非阻塞，没有就立刻返回 EAGAIN
            // ★ 必须用循环拉取：1 个输入帧在 atempo 等滤镜下可能拆成多个输出帧
            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0)
            {
                // 用滤镜出口的时间基算 PTS
                // ★ 关键：atepo 改变了播放时长，但滤镜会同步调整 pts 对应的新 tb
                //   这里用滤镜出口 tb 才能算出正确的显示时间
                tb = av_buffersink_get_time_base(is->out_audio_filter);
#endif

                // 5. 拿一个可写的 Frame 槽位（满了就阻塞）
                // ★ frame_queue_peek_writable 内部会 wait，所以这里是天然背压点
                //   解码速度 > 播放速度时自动阻塞在 sampq 满
                if (!(af = frame_queue_peek_writable(&is->sampq)))
                    goto the_end;

                // 6. 填充元信息
                //   pts: 帧的显示时间（秒）
                //   duration: 这一帧代表多少秒的音频
                // ★ NAN 哨兵：pts 不可用时给 NAN，下游播放器据此跳过同步（按节奏直接放）
                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                // ★ FFmpeg 7.x：frame->pkt_pos 字段已移除，用 -1 表示位置未知
                af->pos = -1;                                       // 在文件中的字节偏移
                af->serial = is->auddec.pkt_serial;                // 包序号（用于 seek 后识别）
                // ★ duration = nb_samples / sample_rate
                //   走 atempo 后虽然播放速度变，但 sample_rate 不变，所以这个值不需要改
                af->duration = av_q2d(AVRational{frame->nb_samples, frame->sample_rate});  // 时长 = 采样数 / 采样率

                // 7. ★ 转移 frame 所有权给队列
                //    move_ref 之后 frame 内部指针被掏空，但 frame 本身还能继续用
                // ★ 必须用 move_ref 而非 ref：避免 frame 和 af->frame 引用同一 buffer，
                //   否则下一次 av_buffersink_get_frame 重新填充 frame 时会破坏队列里的数据
                av_frame_move_ref(af->frame, frame);
                // 提交到队列（推进写指针，唤醒等待的播放线程）
                // ★ frame_queue_push 内部会 signal 唤醒被 peek_writable 阻塞的播放线程
                frame_queue_push(&is->sampq);

#if USE_AVFILTER_AUDIO
                // seek 后旧的 serial 已变，丢弃剩余数据
                // 防止旧时间点的帧被送到新时间点的播放线程
                // ★ serial 检查：seek 后 auddec.pkt_serial 已经变化，
                //   此时滤镜出口可能还有"旧播放段"的残留帧，统统丢，避免音画错位
                if (is->audioq.serial != is->auddec.pkt_serial)
                    break;
            }
            // 滤镜出口返回 EOF，说明所有数据都消费完了
            // ★ 标记 finished，让上游 packet 队列知道这一段已结束
            if (ret == AVERROR_EOF)
                is->auddec.finished = is->auddec.pkt_serial;
#endif

#if PRINT_PACKETQUEUE_AUDIO_INFO
            // 调试用：打印入队的音频采样信息
            qDebug("queue audio sample, pts:%lf, duration:%lf, pos:%lld, serial:%d",
                   af->pts, af->duration, af->pos, af->serial);
#endif
        }
        // 循环条件：成功 / EAGAIN（暂时没帧） / EOF（流末尾）都继续
        // ★ do-while 条件：ret >= 0 OR ret == EAGAIN OR ret == EOF
        //   其它错误（如 ENOMEM）会跳出循环，goto the_end 清理
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);

the_end:

#if USE_AVFILTER_AUDIO
    // 释放滤镜图资源
    // ★ 释放顺序：先 graph（包含所有滤镜实例）再 afilters 字符串，
    //   反之会出现 graph 内部还在用字符串的悬空引用
    avfilter_graph_free(&is->agraph);
    if (is->afilters)
    {
        av_free(is->afilters);
        is->afilters = nullptr;
    }
#endif

    // 释放 frame
    // ★ 必须先 av_frame_free 再退出线程，否则解码器内部 buffer 会泄露
    av_frame_free(&frame);
    qDebug("-------- audio decode thread exit.");
    return;
}
