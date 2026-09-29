// ***********************************************************/
// video_decode_thread.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 视频解码线程实现（流水线的第二站 - 视频部分）
// 包含包队列处理与 DXVA2 硬件解码后的帧转移
// ***********************************************************/

#include "video_decode_thread.h"

VideoDecodeThread::VideoDecodeThread(QObject* parent, VideoState* pState)
    : QThread(parent), m_pState(pState)
{
}

VideoDecodeThread::~VideoDecodeThread()
{
    qInfo("[TRACE][~VideoDecodeThread] this=%p, isRunning=%d, isFinished=%d",
          (void*)this, (int)isRunning(), (int)isFinished());
}

/**
 * @brief 视频解码线程主函数
 *
 * 工作流程：
 *   1. 循环调 get_video_frame() 从解码器拿一帧
 *   2. 如果启用了视频滤镜，过一遍滤镜图
 *   3. 如果是硬件解码（DXVA2），把 GPU 上的帧转移到系统内存
 *   4. 计算 PTS 和帧时长
 *   5. 调 queue_picture() 把帧推进 pictq（视频帧队列）
 *
 * 退出条件：get_video_frame() 返回负数（出错或退出）
 */
void VideoDecodeThread::run()
{
    // 进入时 m_pState 必须有效（外部用 assert 守门）
    assert(m_pState);
    VideoState* is = m_pState;
    // frame: 接收解码器输出的帧（可能是 GPU 上的帧）
    AVFrame* frame = av_frame_alloc();
    // sw_frame: 硬件解码时，从 GPU 转移到 CPU 的帧
    AVFrame* sw_frame = av_frame_alloc();
    // tmp_frame: 指向最终要送入队列的帧
    // 软解时 = frame；硬解时 = sw_frame（GPU→CPU 转移之后的）
    AVFrame* tmp_frame = nullptr;
    double pts;        // 帧的显示时间戳（秒）
    double duration;   // 帧的显示时长（秒），用于计算下一帧的显示时间
    int ret;
    // tb: 视频流的时间基（每 1 个 pts 单位代表多少秒）
    AVRational tb = is->video_st->time_base;
    // frame_rate: 猜测的视频帧率（如 25/1 = 25 fps）
    // 用于推算"这帧该显示多久"
    //AVRational = "用分数表示的小数" ，
    //专门用来精确表示"帧率""宽高比""时间戳单位"等不容易用浮点精确表达的值。
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, nullptr);

#if USE_AVFILTER_VIDEO
    // 视频滤镜（未启用）
    // 这里的变量用于追踪"上一帧的输入参数"，
    // 当参数变化时需要重新构建滤镜图
    AVFilterContext *filt_out = nullptr, *filt_in = nullptr;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = AV_PIX_FMT_NONE;
    int last_serial = -1;
    int last_vfilter_idx = 0;
#endif

    if (!frame)
        return;  // ★ 分配失败直接退出线程，绝不解引用空指针

    // ★ 主循环：不停地"取一帧 → 转换 → 推进队列"
    // 调试计数器：记录 EAGAIN 次数和成功拿到帧的次数
    static int s_heartbeat = 0;
    static int s_got_frame = 0;
    for (;;)
    {
        // 1. 从解码器拿一帧（内部会处理 send_packet/receive_frame）
        // ★ get_video_frame 内部会从 packet 队列取包 → avcodec_send_packet → avcodec_receive_frame
        //   返回值语义：
        //     < 0 → 致命错误或用户请求退出，goto the_end
        //     = 0 → EAGAIN（暂时没帧，packet 还没到），跳过本轮等下次
        //     > 0 → 成功拿到一帧
        ret = get_video_frame(is, frame);
        if (ret < 0)
        {
            qWarning("[video_decode] get_video_frame returned %d, exit loop", ret);
            goto the_end;   // 负数 = 出错或退出
        }
        if (!ret)
        {
            // 心跳：每 5000 次 EAGAIN 打印一次，验证线程没卡死
            if ((++s_heartbeat % 5000) == 0)
            {
                qInfo("[video_decode] HEARTBEAT: still waiting (count=%d), videoq.nb_packets=%d, abort=%d",
                      s_heartbeat, is->videoq.nb_packets, is->videoq.abort_request);
            }
            continue;       // 0 = 暂时没帧（EAGAIN），继续等
        }
        // 成功拿到一帧：前 3 帧打印详细信息（验证硬解/格式正确）
        if (s_got_frame < 3)
        {
            // ★ FFmpeg 7.x：key_frame 字段已移除，用 flags 字段检查
            int is_key = (frame->flags & AV_FRAME_FLAG_KEY) ? 1 : 0;
            qInfo("[video_decode] GOT FRAME #%d: format=%s, %dx%d, pts=%lld, key=%d",
                  s_got_frame + 1,
                  av_get_pix_fmt_name((AVPixelFormat)frame->format),
                  frame->width, frame->height, (long long)frame->pts, is_key);
        }
        s_got_frame++;

#if USE_AVFILTER_VIDEO
        // 视频滤镜链未启用，相关代码被条件编译屏蔽
        // 当分辨率/格式/serial 变化时重建滤镜
        if (last_w != frame->width || last_h != frame->height ||
            last_format != frame->format || last_serial != is->viddec.pkt_serial ||
            last_vfilter_idx != is->vfilter_idx || is->req_vfilter_reconfigure)
        {
            ...
        }
#endif

        // 2. ★ 硬件解码特殊处理（关键：GPU→CPU 数据转移）
        // 硬解出来的 frame 还在 GPU 显存里，sws_scale / QImage 没法直接读
        // 必须用 av_hwframe_transfer_data 把数据搬到系统内存
        tmp_frame = frame;  // ★ 默认软解：tmp_frame 指向原 frame，不复制指针更高效
        if (frame->format == AV_PIX_FMT_DXVA2_VLD)  // ★ 判定硬解的标志性 format
        {
            // av_hwframe_transfer_data：GPU → CPU 的数据拷贝
            // 这步是同步的，会等 GPU 完成解码
            /*
int av_hwframe_transfer_data(
    AVFrame *dst,         // 目标：CPU 内存的新帧→ sw_frame
    AVFrame *src,         // 源：GPU 显存里的硬解帧→ frame
    int flags             // 一般传 0
);
             */
//检查源是硬件帧 → 准备 CPU 目标帧 → 调用 DXVA2 的 `GetRenderTargetData`
//把 GPU 显存的 D3D surface 拷贝到 CPU 系统内存 → 设置元数据（宽高 / NV12 格式），
//但不复制 PTS/DTS
            ret = av_hwframe_transfer_data(sw_frame, frame, 0);
            if (ret < 0)
            {
                // ★ 转移失败常见原因：GPU 驱动异常、显存不足
                // 警告而非致命错误：可以选择丢帧继续，但本项目直接结束线程
                av_log(nullptr, AV_LOG_WARNING, "Error transferring the data to system memory\n");
                goto the_end;
            }
            // 转移后 sw_frame 没有时间戳，从原 frame 拷贝过来
            // ★ av_hwframe_transfer_data 不会复制 pts/dts，必须手动迁移
            //   否则后续同步和 clock 计算全部出错
            sw_frame->pts = frame->pts;
            sw_frame->pkt_dts = frame->pkt_dts;
            // 后面 queue_picture 用的就是 sw_frame
            tmp_frame = sw_frame;  // ★ 硬解路径：把"系统内存版本"送入帧队列
        }

        // 3. 计算 PTS（秒）和帧时长
        // frame_rate = 25 fps → duration = 1/25 = 0.04 秒
        // ★ frame_rate 是"分数"，例如 25/1；用 {den, num} 翻转得到 1/25 即每帧秒数
        duration = (frame_rate.num && frame_rate.den ? av_q2d({frame_rate.den, frame_rate.num}) : 0);
        // pts 没有时给 NAN（后面会跳过同步）
        // ★ time_base 把"时间基单位"换算成秒（乘以 tb 的浮点值）
        pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);



        // 4. ★ 把这一帧推进 pictq
        // queue_picture 内部会：找空槽 → 填 pts/duration/serial → move_ref
        // ★ pkt_serial 用于跨 seek 识别"是否是同一个播放段"，后续同步判等会用到
        // ★ FFmpeg 7.x：frame->pkt_pos 字段已移除，传 -1 表示位置未知
        ret = queue_picture(is, tmp_frame, pts, duration, -1, is->viddec.pkt_serial);
        // 5. 释放 frame 引用计数（解码器会复用内部 buffer）
        // 这里 unref tmp_frame（硬解时是 sw_frame，软解时是 frame）
        // ★ 关键：必须 unref tmp_frame 而非 frame，路径要对齐
        //   解码器解码下一帧时才会安全地复用 frame 内部的 buffer
        av_frame_unref(tmp_frame);

        if (ret < 0)
            goto the_end;  // ★ 入队失败（队列已 abort 等），跳到末尾清理
    }

the_end:
#if USE_AVFILTER_VIDEO
    // 释放滤镜图资源
    avfilter_graph_free(&is->vgraph);
    if (is->vfilters)
    {
        av_free(is->vfilters);
        is->vfilters = nullptr;
    }
#endif
    // 释放 frame 和 sw_frame
    // ★ sw_frame 即使没被硬解用上，也已经 av_frame_alloc，必须对应 av_frame_free
    av_frame_free(&frame);
    av_frame_free(&sw_frame);
    qDebug("-------- video decode thread exit.");
    return;
}
