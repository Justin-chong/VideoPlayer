// ***********************************************************/
// video_state.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 播放器全局状态管理 - 视频/音频/字幕流的打开与关闭
// 职责：
//   1. 用 avformat_open_input 打开媒体文件
//   2. 用 av_find_best_stream 找出最佳的视频/音频/字幕流
//   3. 打开对应的解码器（视频可能走 DXVA2 硬解）
//   4. 创建并管理 VideoState、PacketQueue、FrameQueue
//   5. 提供线程注册、关闭等待等接口
// ***********************************************************/
#include "video_state.h"

int infinite_buffer = -1;
int64_t start_time = AV_NOPTS_VALUE;
static enum AVPixelFormat hw_pix_fmt;

VideoStateData::VideoStateData(bool use_hardware, bool loop_play)
    : m_bUseHardware(use_hardware), m_bLoopPlay(loop_play)
{
}

/**
 * @brief 析构：先关流（让所有 avctx 释放 hw_device_ctx/hw_frames_ctx），再 unref 自己的 m_hw_device_ctx
 *        顺序很重要：必须先关流，再 unref 自己的 ref，否则可能 use-after-free
 */
VideoStateData::~VideoStateData()
{
    // ★ 修复（2026-08-04）：原代码 close_hardware() 在前，会先 unref m_hw_device_ctx
    //   但此时 avctx->hw_device_ctx 和 avctx->hw_frames_ctx 还在用这个 device_ctx
    //   万一 FFmpeg 7.x 内部用延迟释放，可能踩到已 unref 但还没真正释放的 ctx
    //   修法：先关流（stream_close 走完后所有 avctx 都被 free，device_ctx 引用减到 1）
    //         再 unref 自己这份，device_ctx 引用归 0，真正释放
    delete_video_state();   // 1) 先关流：内部 stream_close 会 unref avctx 上的 device_ctx/frames_ctx
    close_hardware();       // 2) 再 unref 自己持有的 m_hw_device_ctx
}

/**
 * @brief 释放 VideoState（其实是 stream_close 包装）
 *        调一次就够了，多次调是安全的（m_pState 置 nullptr 后直接返回）
 */
void VideoStateData::delete_video_state()
{
    if (m_pState)
    {
        stream_close(m_pState);
        m_pState = nullptr;
    }
}

/**
 * @brief 取裸的 VideoState*（主窗口和各线程都用它访问共享状态）
 */
VideoState* VideoStateData::get_state() const
{
    return m_pState;
}

/**
 * @brief 视频流是不是用了硬解（DXVA2）
 */
bool VideoStateData::is_hardware_decode() const
{
    return m_bHardwareSuccess;
}

/**
 * @brief 创建 VideoState 并打开文件
 * @return 0=成功，负数=失败
 *
 * 流程：stream_open() 分配所有结构 -> open_media() 真的 avformat_open_input
 */
int VideoStateData::create_video_state(const char* filename)
{
    int ret = -1;
    // ★ 防御性检查：filename 不能为空字符串或 nullptr
    // 这种参数错误不应该让程序崩，而是返回 -1 让 UI 给用户提示
    if (!filename || !filename[0])
    {
        qDebug("filename is invalid, please select a valid media file.");
        return ret;
    }

    // 第一步：分配 VideoState 结构 + 初始化队列/时钟等（不做实际解码）
    m_pState = stream_open(filename);
    if (!m_pState)
    {
        qDebug("stream_open failed!");
        return ret;
    }

    // 第二步：真正调用 FFmpeg 打开文件 + 找到流 + 开解码器
    return open_media(m_pState);
}

/**
 * @brief 打印 VideoState 里所有重要指针（调试用）
 */
void VideoStateData::print_state() const
{
    if (const auto is = m_pState)
    {
        qDebug("[VideoState]PacketQueue(v:%p,a:%p,s:%p)", &is->videoq, &is->audioq, &is->subtitleq);
        qDebug("[VideoState]FrameQueue(v:%p,a:%p,s:%p)", &is->pictq, &is->sampq, &is->subpq);
        qDebug("[VideoState]Decoder(v:%p,a:%p,s:%p)", &is->viddec, &is->auddec, &is->subdec);
        qDebug("[VideoState]Clock(v:%p,a:%p,s:%p)", &is->vidclk, &is->audclk, &is->extclk);
    }
}

/**
 * @brief 用 FFmpeg 打开媒体文件 + 找到最佳流 + 打开解码器
 *
 * 关键步骤：
 *   1. avformat_open_input：读取文件头
 *   2. avformat_find_stream_info：探测码流参数
 *   3. av_find_best_stream：选视频/音频/字幕的最佳流索引
 *   4. stream_component_open：开解码器（视频可能尝试硬解）
 *   5. 如果没视频也没音频 -> 失败
 */
int VideoStateData::open_media(VideoState* is)
{
    assert(is);//如果is为nullptr，让程序安全崩溃
    int err;
    uint i;
    int ret = -1;
    // st_index[类型] 存每种类型流（视频/音频/字幕）选中的流索引
    // 先全部填 -1，表示"还没找到"
    int st_index[AVMEDIA_TYPE_NB];
    AVFormatContext* ic = nullptr;
    // wanted_stream_spec 用于按"用户指定的流描述符"过滤（比如用户想播第 2 路音频）
    // 当前项目没用到外层配置，全是 0
    const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};

    memset(st_index, -1, sizeof(st_index));

    // 重置 eof：开始新文件播放
    is->eof = 0;

    // 先分配一个 FormatContext，后面 avformat_open_input 会填充它
    ic = avformat_alloc_context();
    if (!ic)
    {
        av_log(nullptr, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    // interrupt_callback 允许用户打断耗时的读操作（比如打开网络流卡住时按取消）
    // 当前项目没注册具体回调，置空表示"不打断"
    // ★ 注意：opaque 字段存了 is 指针，方便回调内部访问状态
    ic->interrupt_callback.callback = nullptr; // decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;

    // ★ 关键步骤 1：用 FFmpeg 打开文件（读取文件头，识别封装格式）
    err = avformat_open_input(&ic, is->filename, is->iformat, nullptr);
    if (err < 0)
    {
        av_log(nullptr, AV_LOG_FATAL, "failed to open %s: %d", is->filename, err);
        ret = -1;
        goto fail;
    }

    // 把 ic 挂到 is 上（这样 stream_close 时能正常关闭）
    is->ic = ic;

    //ic->flags |= AVFMT_FLAG_GENPTS; // gen pts

    // 给 ic 注入全局 side data（一些元信息，比如旋转角度）
    // ★ FFmpeg 7.x：av_format_inject_global_side_data 已移除，7.x 中自动处理
    // av_format_inject_global_side_data(ic);

    //AVDictionary** opts = setup_find_stream_info_opts(ic, codec_opts);
    //int orig_nb_streams = ic->nb_streams;

    // ★ 关键步骤 2：探测码流参数
    // 这一步会读一些包来分析码流特征（比如视频宽高、音频采样率）
    err = avformat_find_stream_info(ic, nullptr);
    if (err < 0)
    {
        av_log(nullptr, AV_LOG_WARNING, "%s: could not find codec parameters\n", is->filename);
        ret = -1;
        goto fail;
    }

    if (ic->pb)
        // 强制把"文件结束"标志清零（之前可能用过这个 FormatContext）
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use
                                 // avio_feof() to test for the end

    // if (seek_by_bytes < 0)
    //	seek_by_bytes = (ic->iformat->flags & AVFMT_TS_DISCONT) &&
    //strcmp("ogg", ic->iformat->name);

    //is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
    // max_frame_duration：单帧最长允许的时长（秒）
    // 用于音视频同步时判断"这一帧是不是太久没出现"（可能卡了）
    is->max_frame_duration = 2.0;

    /* if seeking requested, we execute it */
    // 如果命令行指定了 start_time，跳到该位置
    if (start_time != AV_NOPTS_VALUE)
    {
        int64_t timestamp;

        timestamp = start_time;
        /* add the stream start time */
        // 加上 ic 的 start_time 偏移（某些容器的 PTS 起点不是 0）
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0)
        {
            av_log(nullptr, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n", is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    // is_realtime() 探测这个流是不是"实时流"（如直播、rtsp）
    // 实时流需要用无限缓冲策略，不能让播放器停下来等数据
    is->realtime = is_realtime(ic);

    // 打印文件格式信息（调试用：能在控制台看到时长、码率、流的元数据）
    av_dump_format(ic, 0, is->filename, 0);

    // 第一遍循环：按用户指定的流描述符（wanted_stream_spec）筛流
    // 比如用户想播第 2 路音频，这里就能识别
    for (i = 0; i < ic->nb_streams; i++)
    {
        AVStream* st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        // ★ 先把所有流标记为"全部丢弃"
        //   后面只有被选中的流会改回 AVDISCARD_DEFAULT
        //   这样能减少"误读包"——非选中流连 packet 都不会被读进 packet queue
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
                st_index[type] = i;
    }
    // 如果用户指定了某个流但找不到，给个错误提示
    for (i = 0; i < AVMEDIA_TYPE_NB; i++)
    {
        if (wanted_stream_spec[i] && st_index[i] == -1)
        {
            av_log(nullptr, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n",
                   wanted_stream_spec[i], av_get_media_type_string(AVMediaType(i)));
            // ★ 标 INT_MAX 表示"用户显式指定了但找不到"
            //   后面 av_find_best_stream 会跳过这种类型
            st_index[i] = INT_MAX;
        }
    }

    // ★ 关键步骤 3：选最佳流
    // av_find_best_stream 在所有流里挑"最适合播放"的那一路
    // 比如多音轨时选默认音轨；视频流的 related_stream 参数告诉它优先选跟音频相关的字幕流
    st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, st_index[AVMEDIA_TYPE_VIDEO], -1, nullptr, 0);
    st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, st_index[AVMEDIA_TYPE_AUDIO],
                                                       st_index[AVMEDIA_TYPE_VIDEO], nullptr, 0);
    st_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE, st_index[AVMEDIA_TYPE_SUBTITLE],
                                                          (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO] : st_index[AVMEDIA_TYPE_VIDEO]), nullptr, 0);

    /* open the streams */
    // ★ 关键步骤 4：开解码器（一个流一个流地开）
    // ★ 注意：stream_component_open 的返回值本来应检查，失败要 abort_request
    //   这里是历史代码遗留，会在 abort_request 检查那一行展开说明
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0)
    {
        int vret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
        if (vret < 0)
        {
            qWarning("[open_media] stream_component_open VIDEO failed: ret=%d (will still continue, may cause deadlock)", vret);
        }
    }

    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0)
    {
        int vret = stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
        if (vret < 0)
        {
            qWarning("[open_media] stream_component_open AUDIO failed: ret=%d (will still continue)", vret);
        }
    }

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0)
    {
        int vret = stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
        if (vret < 0)
        {
            qWarning("[open_media] stream_component_open SUBTITLE failed: ret=%d (will still continue)", vret);
        }
    }

    // 至少要有视频或音频之一，否则这个文件没意义
    if (is->video_stream < 0 && is->audio_stream < 0)
    {
        av_log(nullptr, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n", is->filename);
        ret = -1;
        goto fail;
    }

    // 实时流默认用无限缓冲（不能让播放器停下来等包，否则会卡顿）
    if (infinite_buffer < 0 && is->realtime)
        infinite_buffer = 1;

    return 0;

fail:
    // 注意：is->ic 已经被赋值的情况不要重复 close
    if (ic && !is->ic)
        avformat_close_input(&ic);
    return ret;
}

/**
 * @brief 分配并初始化一个 VideoState（不打开文件）
 *
 * - 3 个 PacketQueue、3 个 FrameQueue
 * - 3 个 Clock（vid/aud/ext）
 * - continue_read_thread 用来给读线程发"队列空了"信号
 * - 默认主时钟是音频（AV_SYNC_AUDIO_MASTER）
 */
VideoState* VideoStateData::stream_open(const char* filename, const AVInputFormat* iformat)
{
    VideoState* is = nullptr;

    int startup_volume = 100;
    // 默认用音频做主时钟（音视频同步的基准）
    int av_sync_type = AV_SYNC_AUDIO_MASTER;

    // av_mallocz = malloc + memset 0，所有字段默认清零
    is = (VideoState*)av_mallocz(sizeof(VideoState));
    if (!is)
        return nullptr;
    // 三个流的索引初始为 -1（表示"未选"）
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    // av_strdup 复制一份文件名（VideoState 自己持有，不会因为原字符串释放而悬空）
    is->filename = av_strdup(filename);
    if (!is->filename)
        goto fail;
    is->iformat = iformat;//输入格式
    is->ytop = 0;
    is->xleft = 0;

    /* start video display */
    // 初始化 3 个 FrameQueue
    // - pictq: 视频帧队列（解码线程→视频播放线程），keep_last=1 保留最后一帧防止画面闪烁
    // - subpq:  字幕帧队列（字幕解码→显示），keep_last=0
    // - sampq: 音频帧队列（音频解码→音频播放），keep_last=1
    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    // 初始化 3 个 PacketQueue（解复用→解码）
    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0 ||
        packet_queue_init(&is->subtitleq) < 0)
        goto fail;

    // continue_read_thread：当解码线程发现包队列空了，唤醒读线程
    // （这样读线程知道该努力读包了，不会傻等）
    if (!(is->continue_read_thread = new QWaitCondition()))
    {
        av_log(nullptr, AV_LOG_FATAL, "new QWaitCondition() failed!\n");
        goto fail;
    }

    // 初始化 3 个 Clock（视频时钟、音频时钟、外部时钟）
    // init_clock 第二个参数是指向 serial 的指针，当 serial 变化时 clock 自动失效
    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    // ★ extclk 用"自己的 serial"而不是 videoq/audioq 的
    //   原因：extclk 是"外部时钟"——不依赖任何流
    //   它有自己的失效机制（更新外部时钟时 serial++）
    init_clock(&is->extclk, &is->extclk.serial);
    // ★ 初始 -1 表示"还没收到第一个音频帧"
    //   音频时钟要等第一帧解码完成才能用 serial 校正
    is->audio_clock_serial = -1;
    // 音量范围 [0, 100] 限幅 + 转成 SDL 内部范围 [0, SDL_MIX_MAXVOLUME]
    if (startup_volume < 0)
        av_log(nullptr, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
    if (startup_volume > 100)
        av_log(nullptr, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
    startup_volume = av_clip(startup_volume, 0, 100);
    startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    is->audio_volume = startup_volume;
    is->muted = 0;  // 默认不静音
    is->av_sync_type = av_sync_type;//默认音频为主时钟
    // is->read_tid = m_pReadThreadId;
    // read_thread_exit: -1=还没退出，0=已退出
    is->read_thread_exit = -1;
    is->loop = int(m_bLoopPlay);

    is->threads = {nullptr};

#if USE_AVFILTER_AUDIO
    // 音频变速倍数（1.0=原速，2.0=2 倍速）
    is->audio_speed = 1.0;
#endif
    return is;
fail:
    // 出错时统一走 stream_close 释放已经分配的资源
    stream_close(is);
    return nullptr;
}

/**
 * @brief 注册所有播放线程到 VideoState
 *        关闭时用这些指针 join 线程
 *        将所有线程指针塞入VideoState
 */
void VideoStateData::threads_setting(VideoState* is, const Threads& threads)
{
    if (!is)
        return;

    // 防御：每个线程都应该是空的（之前没人注册过）
    // 重复设置可能意味着代码逻辑出错，用 assert 立刻暴露
    assert(!is->threads.read_tid);
    assert(!is->threads.video_decode_tid);
    assert(!is->threads.audio_decode_tid);
    assert(!is->threads.video_play_tid);
    assert(!is->threads.audio_play_tid);
    assert(!is->threads.subtitle_decode_tid);

    // 把外部创建的 5 个线程指针都收进来
    // 关闭播放器时要用这些指针 join 线程
    //Threads是个结构体，包含6个工作线程
    is->threads = threads;
}

/**
 * @brief 等待 ReadThread 退出
 *        ReadThread 退出后会自行设 is->read_thread_exit = 0
 */
void VideoStateData::read_thread_exit_wait(VideoState* is)
{
    if (!is)
        return;

    // read_thread_exit 状态检查：
    //  -1：线程 还没起来（异常，不等）
    //   0：正在跑
    //  >0：已经退  出
    if (is->read_thread_exit != 0)
        return;

    if (is->threads.read_tid)
    {
        av_log(nullptr, AV_LOG_INFO, "read thread wait before!\n");
        // Qt 的 wait() 会阻塞当前线程，直到目标线程 run() 返回
        is->threads.read_tid->wait();
        av_log(nullptr, AV_LOG_INFO, "read thread wait after!\n");
        // 回收后清空指针，避免重复 wait
        is->threads.read_tid = nullptr;
    }
}

/**
 * @brief join 5 个工作线程（视频/音频解码 + 视频/音频播放 + 字幕解码）
 */
//为什么先退出播放线程，再退出解码线程？
//因为播放线程是消费者对于FrameQueue来说，
//而解码线程是生产者对于FrameQueue，
//必须先停消费者（让需求归0），再停生产者（让供给停止），否者会卡住
void VideoStateData::threads_exit_wait(VideoState* is)
{
    if (!is)
        return;

    // 按顺序 join 每个线程
    // 注意：这里不保证一定能 join 成功（要靠 decoder_abort 唤醒）
    // 所以 stream_close 里要确保先 abort 再 join
    if (is->threads.video_play_tid)
    {
        is->threads.video_play_tid->wait();
        is->threads.video_play_tid = nullptr;
    }

    if (is->threads.audio_play_tid)
    {
        is->threads.audio_play_tid->wait();
        is->threads.audio_play_tid = nullptr;
    }

    if (is->threads.video_decode_tid)
    {
        is->threads.video_decode_tid->wait();
        is->threads.video_decode_tid = nullptr;
    }

    if (is->threads.audio_decode_tid)
    {
        is->threads.audio_decode_tid->wait();
        is->threads.audio_decode_tid = nullptr;
    }

    if (is->threads.subtitle_decode_tid)
    {
        is->threads.subtitle_decode_tid->wait();
        is->threads.subtitle_decode_tid = nullptr;
    }
}

/**
 * @brief 完整关闭流程：abort + join 所有线程 + 释放所有资源
 *
 * 顺序很重要：必须先 abort 让线程自己退出，才能 join
 * 然后再 destroy queue/frame queue 等
 */
void VideoStateData::stream_close(VideoState* is)
{
    assert(is);

    // ★ 步骤 1：先设置 abort 标志
    // 这一行会让所有线程的循环检查到 abort_request 后自己退出
    // ★ 顺序很关键：必须先 abort 再 join
    //   如果先 join，线程可能卡在条件变量上永远不退（死锁）
    is->abort_request = 1;

    // ★ 步骤 2：等读线程退出
    // 读线程是"源头"，要先关掉它，否则它会一直往队列里塞包
    //   解码线程会一直从空队列 sleep，没人叫醒它——但 abort 标志会叫醒它
    read_thread_exit_wait(is);

    // if (is->read_thread_exit == 0)
    //{
    //	// SDL_WaitThread(is->read_tid, nullptr);
    //	//((QThread*)(is->read_tid))->wait();
    //	/*if (m_pReadThreadId)
    //		m_pReadThreadId->wait();*/
    //}

    /* close each stream */
    // ★ 步骤 3：依次关音频/视频/字幕流
    // stream_component_close 内部会：decoder_abort（让解码线程退出）+ decoder_destroy（释放资源）
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(is, is->subtitle_stream);

    // ★ 步骤 4：join 所有工作线程，等所有线程手上的活干完
    // 注意：必须在 stream_component_close 之后 join，因为 decoder_abort 只是设了 flag
    // 真正让线程退出还需要在 stream_component_close 中断流的 PacketQueue
    threads_exit_wait(is); // read and decode threads exit here.

    // 关 FormatContext（释放文件读取相关资源）
    avformat_close_input(&is->ic);

    // 销毁 3 个 PacketQueue
    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
    // 销毁 3 个 FrameQueue
    frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);
    frame_queue_destory(&is->subpq);

    if (is->continue_read_thread)
    {
        delete is->continue_read_thread;
        is->continue_read_thread = nullptr;
    }

    // SDL_DestroyCond(is->continue_read_thread);
    // 释放 swscale 上下文（视频颜色空间转换用）
    sws_freeContext(is->img_convert_ctx);
    sws_freeContext(is->sub_convert_ctx);
    // 释放文件名字符串
    av_free(is->filename);
    /*if (is->vis_texture)
          SDL_DestroyTexture(is->vis_texture);
  if (is->vid_texture)
          SDL_DestroyTexture(is->vid_texture);
  if (is->sub_texture)
          SDL_DestroyTexture(is->sub_texture);*/

    // 最后释放 VideoState 本身
    av_free(is);
}

/**
 * @brief 硬解回调：FFmpeg 在尝试选择像素格式时会调它
 *        我们只允许它选 hw_pix_fmt（DXVA2 对应的格式，比如 AV_PIX_FMT_DXVA2_VLD）
 *        这样解码出来的 frame 就是 GPU 上的 frame
 */
static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts)
{
    // ★ pix_fmts 是 FFmpeg 给我们的候选像素格式列表
    //   它是"以 -1 结尾"的数组 — 遍历到 *p == -1 表示结束
    //   我们遍历找 hw_pix_fmt（DXVA2 对应的格式，比如 AV_PIX_FMT_DXVA2_VLD）
    for (const enum AVPixelFormat* p = pix_fmts; *p != -1; p++)
    {
        if (*p == hw_pix_fmt)
            return *p;  // ★ 命中：告诉 FFmpeg 用 GPU 格式
                        //   这样 frame->data 里的就是 GPU 显存指针
                        //   上层需要 av_hwframe_transfer_data 才能取到 CPU 数据
    }

    // ★ 兜底分支：没找到 hw_pix_fmt
    //   理论上不应该发生（前面 open_hardware 查过了）
    //   真发生就是配置/驱动有问题，给个明显错误提示
    fprintf(stderr, "Failed to get HW surface format, codec_id=%d\n", (int)ctx->codec_id);
    return AV_PIX_FMT_NONE;
}

// static int hw_decoder_init(AVCodecContext* ctx, const enum AVHWDeviceType type)
//{
//	int err = 0;
//
//	if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type, nullptr, nullptr, 0)) < 0) {
//		fprintf(stderr, "Failed to create specified HW device.\n");
//		return err;
//	}
//
//	ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
//
//	return err;
//}

/**
 * @brief 真的创建一个硬件设备（DXVA2/d3d11va），并分配 frame pool
 *
 * 关键修复（2026-08-04）：
 *   - 原代码只设置 ctx->hw_device_ctx，没设置 ctx->hw_frames_ctx
 *   - 这会导致 av_hwframe_transfer_data() 失败或阻塞（用户机器表现为 0 帧输出）
 *   - 现在额外创建 hw_frames_ctx，让 FFmpeg 用固定尺寸的 GPU frame pool
 *
 * @param ctx    解码器上下文（会往它的 hw_device_ctx 和 hw_frames_ctx 字段挂引用）
 * @param type   硬解设备类型（AV_HWDEVICE_TYPE_DXVA2 / D3D11VA / ...）
 * @return 0=成功，负数=失败
 */
int VideoStateData::hw_decoder_init(AVCodecContext* ctx, const enum AVHWDeviceType type)
{
    int err = 0;

    // 1) 创建 D3D/DXVA 设备上下文（D3D9 或 D3D11 后端）
    if ((err = av_hwdevice_ctx_create(&m_hw_device_ctx, type, nullptr, nullptr, 0)) < 0)
    {
        fprintf(stderr, "Failed to create specified HW device.\n");
        return err;
    }

    // 2) 把设备 ctx 引用一份挂到 AVCodecContext 上
    //    avcodec_close 时会自动 unref
    ctx->hw_device_ctx = av_buffer_ref(m_hw_device_ctx);

    // 3) ★ 关键修复：创建 frame pool（hw_frames_ctx）
    //   没有这个的话，av_hwframe_transfer_data() 内部要临时创建 frame pool
    //   在某些 GPU 驱动上会失败或阻塞（AMD 核显上表现为 0 帧）
    //   显式分配一个固定尺寸的 frame pool 就能避免这个坑
    if (ctx->width > 0 && ctx->height > 0)
    {
        AVBufferRef* hw_frames_ref = av_hwframe_ctx_alloc(m_hw_device_ctx);
        if (!hw_frames_ref)
        {
            fprintf(stderr, "Failed to alloc HW frame context.\n");
            // 不算致命错误：fallback 到没 hw_frames_ctx 的旧路径
            return 0;
        }
        AVHWFramesContext* frames_ctx = (AVHWFramesContext*)(hw_frames_ref->data);
        // 硬解出来的 GPU 帧格式：DXVA2_VLD 或 D3D11VA_VLD
        // ★ 关键：用全局变量 hw_pix_fmt（open_hardware 里 get_hwdevice_decoder 设置的）
        //   绝对不要用 ctx->sw_pix_fmt——avcodec_open2 之前这个字段还是 0！
        //   用 0 去 av_hwframe_ctx_init 会立刻失败：Failed to init HW frame context
        //   然后整个 frame pool 创建被 fallback 0，av_hwframe_transfer_data 内部拿不到 frame
        //   → AMD 核显上 0 帧输出（"解码完成"但 transfer 不到 CPU）
        //   → 视频一直黑屏或卡死
        frames_ctx->format = hw_pix_fmt;
        // transfer 之后落到 CPU 的格式：NV12 是 D3D/DXVA 标准输出格式
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
        // 帧尺寸：必须和视频一致
        frames_ctx->width = ctx->width;
        frames_ctx->height = ctx->height;
        // 初始 pool 大小：3 帧够用（B 帧参考需要至少 4）
        frames_ctx->initial_pool_size = 4;
        // ★ 调试日志：打印 hw_pix_fmt 值，确认硬解 frame pool 正确初始化
        //   这个值必须是 DXVA2_VLD(51) 或 D3D11VA_VLD 等 GPU 格式
        //   如果是 0 或 -1，说明初始化失败，av_hwframe_transfer_data 会 0 帧
        qInfo("[hw_decoder_init] hw_frames_ctx allocated: %dx%d, hw_format=%s, sw_format=NV12",
              frames_ctx->width, frames_ctx->height,
              av_get_pix_fmt_name((AVPixelFormat)frames_ctx->format));
        if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0)
        {
            fprintf(stderr, "Failed to init HW frame context: %d\n", err);
            av_buffer_unref(&hw_frames_ref);
            // 失败不致命：fallback
            return 0;
        }
        // 挂到 avctx 上
        ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
        av_buffer_unref(&hw_frames_ref);
        qInfo("[hw_decoder_init] hw_frames_ctx allocated: %dx%d, sw_format=NV12",
              frames_ctx->width, frames_ctx->height);
    }

    return err;
}

/**
 * @brief 打开硬解的总入口
 *        1. get_hwdevice：把字符串 "dxva2" 转成 enum AVHWDeviceType
 *        2. get_hwdevice_decoder：查这个 codec 支不支持 dxva2，返回对应的像素格式
 *        3. 注册 get_hw_format 回调，强制 FFmpeg 选 GPU 格式
 *        4. hw_decoder_init 真的去拿设备
 */
bool VideoStateData::open_hardware(AVCodecContext* avctx, const AVCodec* codec, const char* device)
{
    // ★ 步骤 1：字符串 -> 枚举
    //   "dxva2" / "d3d11va" / "vaapi" / "vdpau" → enum AVHWDeviceType
    enum AVHWDeviceType type = get_hwdevice(device);
    // ★ 步骤 2：查 codec + device 的组合对应的"硬解像素格式"
    //   例：h264 + dxva2 → AV_PIX_FMT_DXVA2_VLD
    //   get_hwdevice_decoder 内部把结果存到全局 hw_pix_fmt，get_hw_format 用
    //   返回 AV_PIX_FMT_NONE 表示这个 codec 不支持这个 device（需要回落到软解）
    hw_pix_fmt = get_hwdevice_decoder(codec, type);

    // ★ 步骤 3：注册 get_format 回调
    //   当 FFmpeg 内部要选"解码输出 frame 的像素格式"时，会调 get_hw_format
    //   我们强制它选 hw_pix_fmt（GPU 格式）— 这样 frame->data 里就是 GPU 显存指针
    //   注意：这之后 frame 必须用 av_frame_copy 或 av_hwframe_transfer_data 才能传到 CPU
    avctx->get_format = get_hw_format;

    // ★ 步骤 4：真的去拿硬件设备
    //   内部会调 D3D9/DXVA2 初始化 GPU
    //   失败时 m_hw_device_ctx 仍是 nullptr，close_hardware 不会崩
    if (hw_decoder_init(avctx, type) < 0)
        return false;

    return true;
}

/**
 * @brief 关闭硬解：只 unref m_hw_device_ctx 一次（引用计数）
 *        因为 avctx->hw_device_ctx 是 ref 过来的，avcodec_close 时会自动 unref
 */
void VideoStateData::close_hardware()
{
    // ★ av_buffer_unref 减引用计数，计数到 0 时真正释放
    // 这里只 unref 我们自己持有的那一份引用
    // avctx 那一份由 avcodec_close 负责（因为它是 av_buffer_ref 出来的）
    // ★ 关键：m_hw_device_ctx 必须 unref 两次才会真的释放
    //   1) close_hardware 解一次（这里）
    //   2) avcodec_close 时解第二次
    //   少解一次 → 显存泄漏
    av_buffer_unref(&m_hw_device_ctx);
}

/**
 * @brief 打开一个流（视频/音频/字幕），包括：
 *        1. 创建 AVCodecContext 并填好参数
 *        2. 找解码器
 *        3. 视频分支尝试开硬解（DXVA2）
 *        4. avcodec_open2 真打开
 *        5. 把 avctx 挂到 is->video_st/audio_st/subtitle_st
 *        6. 设置 eof 标志、取消 AVDISCARD_ALL
 *        7. 音频分支额外配置 avfilter（如果开了 USE_AVFILTER_AUDIO）
 *
 * @return 0=成功，负数=失败
 */
int VideoStateData::stream_component_open(VideoState* is, int stream_index)
{
    assert(is);
    AVFormatContext* ic = is->ic;//解复用上下文
    AVCodecContext* avctx;//解码器上下文
    const AVCodec* codec;//解码器
    //FFmpeg 的"键值对配置表"，用来给 API 传可选参数
    AVDictionary* opts = nullptr;
    // const AVDictionaryEntry* t = nullptr;
    int sample_rate, nb_channels;
    //AVChannelLayout描述 音频的声道布局 ——也就是"声音从哪几个喇叭出来"
    AVChannelLayout ch_layout={};
    // int64_t
    int format;
    int ret = 0;
    int stream_lowres = 0;

    // 边界检查：流索引不能越界
    if (stream_index < 0 || ((unsigned int)stream_index) >= ic->nb_streams)
    {
        qWarning("[stream_component_open] FAIL: stream_index %d out of range [0,%d)", stream_index, ic->nb_streams);
        return -1;
    }

    // 分配解码器上下文
    avctx = avcodec_alloc_context3(nullptr);
    if (!avctx)
    {
        qWarning("[stream_component_open] FAIL: avcodec_alloc_context3 returned NULL");
        return AVERROR(ENOMEM);
    }

    // 把流的 codecpar 拷贝到 avctx（codecpar 里是流的元信息：宽高、采样率等）
    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
    {
        char errbuf[128] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        qWarning("[stream_component_open] FAIL: avcodec_parameters_to_context: %d (%s)", ret, errbuf);
        goto fail;
    }
    // 记下时间基（用于 PTS 换算）
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    // 根据 codec_id 找 FFmpeg 自带的解码器（H.264/AAC/...)
    codec = avcodec_find_decoder(avctx->codec_id);

    // 按流类型分别处理
    switch (avctx->codec_type)
    {
        case AVMEDIA_TYPE_AUDIO:
            is->last_audio_stream = stream_index;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->last_subtitle_stream = stream_index;
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->last_video_stream = stream_index;

            // ★ 视频流特殊处理：尝试开启硬解（DXVA2）
            if (m_bUseHardware)
            {
                // 先重置硬解标志，open_hardware 成功才会置 true
                m_bHardwareSuccess = false;
                // Windows 上用 dxva2（也可以改成 d3d11va）
                const char* hardware_device = "dxva2"; // device = <vaapi|vdpau|dxva2|d3d11va>
                ret = open_hardware(avctx, codec, hardware_device);
                // ★ 关键修复：硬解失败不再 goto fail，而是回退软解
                //   原因：4K 软解卡死体验更差，让用户用软解看低清，或开硬解看高清
                //   失败时打印警告，清掉 get_format 回调，硬件 ctx 由 close_hardware 释放
                if (!ret)
                {
                    qWarning("hardware-accelerated open failed, fallback to software decoding, device:%s", hardware_device);
                    // ★ 回退清理：取消 get_format 回调，让解码器走默认像素格式
                    avctx->get_format = nullptr;
                    // 硬件 ctx 由 close_hardware 在析构时统一清理（m_hw_device_ctx 是 nullptr）
                    // 继续走下面的 avcodec_open2 走软解
                }
                else
                {
                    qInfo("hardware-accelerated opened, device:%s", hardware_device);
                    m_bHardwareSuccess = true;
                }
            }
            break;
    }

    // 没找到解码器说明这个编码格式 FFmpeg 不支持
    if (!codec)
    {
        av_log(nullptr, AV_LOG_WARNING, "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    // ★ 重新写回 codec_id：保证 avctx 用的就是 avcodec_find_decoder 返回的那个
    //   理论上 codecpar 里的 codec_id 跟 find_decoder 出来的应该一致
    //   但某些少见情况（私有 codec 别名）可能不同，统一一下
    avctx->codec_id = codec->id;
    //max_lowres这个解码器最多支持到几级低分辨率
    if (stream_lowres > codec->max_lowres)
    {
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n", codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;

    // avctx->flags2 |= AV_CODEC_FLAG2_FAST;
    /*opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);*/

    // ★ 真正打开解码器
    // 从这里开始 avctx 就可以用了，avcodec_send_packet / avcodec_receive_frame 可以正常工作
    //第 3 个参数是**"解码器的个性化配置字典"
    //   avcodec_open2 内部会消耗 opts（av_dict_free 不需要再 free 这个指针）
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0)
    {
        char errbuf[128] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        qWarning("[stream_component_open] FAIL: avcodec_open2 (codec=%s) returned %d (%s)",
                 avcodec_get_name(avctx->codec_id), ret, errbuf);
        goto fail;
    }
    qInfo("[stream_component_open] OK: stream_index=%d codec=%s (type=%d)",
          stream_index, avcodec_get_name(avctx->codec_id), (int)avctx->codec_type);

    // ★ 标记流为"默认"（不再丢包）
    //   之前在 open_media 里所有流都设了 AVDISCARD_ALL
    //   成功打开解码器后改回 AVDISCARD_DEFAULT，让读线程开始往这个流的 queue 塞包
    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    // 按流类型配置额外信息
    switch (avctx->codec_type)
    {
        case AVMEDIA_TYPE_AUDIO:
#if USE_AVFILTER_AUDIO
        {
            // 音频滤镜（变速、音调调整等）
            AVFilterContext* sink;
            // const char* afilters =
            // "aresample=8000,aformat=sample_fmts=s16:channel_layouts=mono"; //
            // "atempo=2"; const char* afilters = nullptr; const char* afilters =
            // "atempo=2.0";
            is->audio_filter_src.freq = avctx->sample_rate;
            is->audio_filter_src.ch_layout.nb_channels = avctx->ch_layout.nb_channels; // avctx->channels;
            is->audio_filter_src.ch_layout = avctx->ch_layout;                         //  avctx->channel_layout
            is->audio_filter_src.fmt = avctx->sample_fmt;
            // configure_audio_filters 在内部构建 atempo/aresample 滤镜链
            if ((ret = configure_audio_filters(is, is->afilters, 0)) < 0)
                goto fail;

            sink = is->out_audio_filter;
            // 从滤镜输出端拿参数
            sample_rate = av_buffersink_get_sample_rate(sink);
            nb_channels = av_buffersink_get_channels(sink);
            // channel_layout = av_buffersink_get_channel_layout(sink);
            format = av_buffersink_get_format(sink);
            AVChannelLayout chn_layout;
            av_buffersink_get_ch_layout(sink, &chn_layout);
            qDebug("afilter sink: sample rate:%d, chn:%d, fmt:%d, chn_layout:%d", sample_rate, nb_channels, format, chn_layout.u);
        }
#else
            // 不走滤镜的话，直接用解码器的原始参数
            sample_rate = avctx->sample_rate;
            ret = av_channel_layout_copy(&ch_layout, &avctx->ch_layout);
            if (ret < 0)
                goto fail;

#endif
            /* prepare audio output */
            /*if ((ret = audio_open(is, chn_layout, nb_channels, sample_rate,
    &is->audio_tgt)) < 0) goto fail;

    is->audio_src = is->audio_tgt;*/

            // 记下音频流索引和 AVStream
            is->audio_stream = stream_index;
            is->audio_st = ic->streams[stream_index];

            // 某些容器（no binsearch）需要手动记下 start_pts
            if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)))
            {
                is->auddec.start_pts = is->audio_st->start_time;
                is->auddec.start_pts_tb = is->audio_st->time_base;
            }

            m_bHasAudio = true;
            m_avctxAudio = avctx;//音频流解码上下文
            break;

        case AVMEDIA_TYPE_VIDEO:
            is->video_stream = stream_index;
            is->video_st = ic->streams[stream_index];

            m_bHasVideo = true;
            m_avctxVideo = avctx;//视频流解码上下文
            break;

        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_stream = stream_index;
            is->subtitle_st = ic->streams[stream_index];

            m_bHasSubtitle = true;
            m_avctxSubtitle = avctx;//字幕流解码上下文
            break;

        default:
            break;
    }

    // 成功时直接跳到 out（跳过 fail 标签）
    goto out;

fail:
    // 失败：释放 avctx
    avcodec_free_context(&avctx);
out:
    // 无论成功失败都释放 opts（avcodec_open2 会消耗 opts 中的选项）
    av_dict_free(&opts);
    return ret;
}

/**
 * @brief 关闭一个流（stream_component_open 的反操作）
 *        1. decoder_abort：让该流的解码线程退出
 *        2. decoder_destroy：释放 Decoder 内部资源（avpacket 等）
 *        3. 标记流被丢弃（AVDISCARD_ALL）
 *        4. 把 VideoState 里对应的 stream_index / stream ptr 置为 -1/nullptr
 */
void VideoStateData::stream_component_close(VideoState* is, int stream_index)
{
    assert(is);
    AVFormatContext* ic = is->ic;
    AVCodecParameters* codecpar;//流的元信息只读描述符

    // 边界检查
    if (stream_index < 0 || ((unsigned int)stream_index) >= ic->nb_streams)
        return;

    codecpar = ic->streams[stream_index]->codecpar;

    // 按流类型分别清理
    switch (codecpar->codec_type)
    {
        case AVMEDIA_TYPE_AUDIO:
            // decoder_abort 设置 abort 标志并唤醒等待队列的解码线程
            decoder_abort(&is->auddec, &is->sampq);
            // SDL_CloseAudioDevice(audio_dev);
            // decoder_destroy 释放 Decoder 内部的 AVPacket 等
            decoder_destroy(&is->auddec);

            // swr_free(&is->swr_ctx);
            // av_freep(&is->audio_buf1);
            // is->audio_buf1_size = 0;
            // is->audio_buf = nullptr;

            /*if (is->rdft) {
            av_rdft_end(is->rdft);
            av_freep(&is->rdft_data);
            is->rdft = nullptr;
            is->rdft_bits = 0;
            }*/
            break;

        case AVMEDIA_TYPE_VIDEO:
            decoder_abort(&is->viddec, &is->pictq);
            decoder_destroy(&is->viddec);
            break;

        case AVMEDIA_TYPE_SUBTITLE:
            decoder_abort(&is->subdec, &is->subpq);
            decoder_destroy(&is->subdec);
            break;

        default:
            qDebug("Not handled yet.......code type:%d", codecpar->codec_type);
            break;
    }

    // 标记流为"全部丢弃"，FFmpeg 不再给这个流发包
    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    // 清空 VideoState 里对应流的指针
    switch (codecpar->codec_type)
    {
        case AVMEDIA_TYPE_AUDIO:
            is->audio_st = nullptr;
            is->audio_stream = -1;
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->video_st = nullptr;
            is->video_stream = -1;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_st = nullptr;
            is->subtitle_stream = -1;
            break;
        default:
            break;
    }
}

/** @brief 是否有视频流 */
bool VideoStateData::has_video() const
{
    return m_bHasVideo;
}

/** @brief 是否有音频流 */
bool VideoStateData::has_audio() const
{
    return m_bHasAudio;
}

/** @brief 是否有字幕流 */
bool VideoStateData::has_subtitle() const
{
    return m_bHasSubtitle;
}

/**
 * @brief 拿指定类型流的 AVCodecContext解码器上下文
 *        - 主窗口用它在播放前查 width/height 等参数
 *        - 视频播放线程也会用这个 avctx 做色彩空间转换
 */
AVCodecContext* VideoStateData::get_contex(AVMediaType type) const
{
    AVCodecContext* pCtx = nullptr;
    switch (type)
    {
        case AVMEDIA_TYPE_AUDIO:
            pCtx = m_avctxAudio;
            break;
        case AVMEDIA_TYPE_VIDEO:
            pCtx = m_avctxVideo;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            pCtx = m_avctxSubtitle;
            break;
        default:
            break;
    }
    return pCtx;
}

/**
 * @brief 通过传入字符串 返回 硬件设备类型
 *        合法值：vaapi / vdpau / dxva2 / d3d11va
 *        不认识会打印所有支持的类型
 */
enum AVHWDeviceType VideoStateData::get_hwdevice(const char* device) const
{
    // device = <vaapi|vdpau|dxva2|d3d11va>
    // 通过字符串找对应的硬件设备类型
    enum AVHWDeviceType type = av_hwdevice_find_type_by_name(device);
    if (type == AV_HWDEVICE_TYPE_NONE)
    {
        // 不认识这个设备，打印警告
        av_log(nullptr, AV_LOG_WARNING, "Device type %s is not supported.\n", device);

        // 顺便打印所有支持的类型，方便用户排查
        av_log(nullptr, AV_LOG_INFO, "Available device types:");
        while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
            av_log(nullptr, AV_LOG_INFO, " %s", av_hwdevice_get_type_name(type));
        av_log(nullptr, AV_LOG_INFO, "\n");
        return AV_HWDEVICE_TYPE_NONE;
    }
    return type;
}

/**
 * @brief 查询 decoder + device 的组合对应的硬件像素格式
 *        例：h264 + dxva2 -> AV_PIX_FMT_DXVA2_VLD
 *        找不到就返回 NONE，调用方会回落到软解
 */
enum AVPixelFormat VideoStateData::get_hwdevice_decoder(const AVCodec* decoder, enum AVHWDeviceType type) const
{
    // 防御：参数无效就返回 NONE
    if (!decoder || AV_HWDEVICE_TYPE_NONE == type)
        return AV_PIX_FMT_NONE;

    // 遍历该 decoder 的所有硬件配置
    // avcodec_get_hw_config 返回 NULL 表示遍历完
    for (int i = 0;; i++)
    {
        const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, i);
        if (!config)
        {
            // 遍历完所有配置都没找到 → 这个 codec 不支持这个 device
            av_log(nullptr, AV_LOG_WARNING, "Decoder %s does not support device type %s.\n", decoder->name, av_hwdevice_get_type_name(type));
            return AV_PIX_FMT_NONE;
        }
        // 找到了：必须是"支持独立 device ctx" + device 类型匹配
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == type)
        {
            return config->pix_fmt;
        }
    }
    return AV_PIX_FMT_NONE;
}
