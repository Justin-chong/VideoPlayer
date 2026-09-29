// ***********************************************************/
// subtitle_decode_thread.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 字幕解码线程实现（流水线的第二站 - 字幕部分）
// ***********************************************************/

#include "subtitle_decode_thread.h"

SubtitleDecodeThread::SubtitleDecodeThread(QObject* parent, VideoState* pState)
    : QThread(parent), m_pState(pState)
{
}

SubtitleDecodeThread::~SubtitleDecodeThread()
{
    qInfo("[TRACE][~SubtitleDecodeThread] this=%p, isRunning=%d, isFinished=%d",
          (void*)this, (int)isRunning(), (int)isFinished());
}

/**
 * @brief 字幕解码线程主函数
 *
 * 工作流程：
 *   1. 拿一个可写的 Frame 槽位
 *   2. 调 decoder_decode_frame() 解码出一个 AVSubtitle
 *   3. 计算 PTS（AVSubtitle 的 pts 单位是微秒，要除以 AV_TIME_BASE）
 *   4. 推入 subpq（字幕帧队列）
 *
 * 只处理 format==1 的字幕（位图字幕，如 PGS、DVD 字幕）。
 * 文本字幕（SRT、ASS）目前是视频播放线程里单独解析的，不走这里。
 */
void SubtitleDecodeThread::run()
{
    assert(m_pState);
    VideoState* is = m_pState;
    // sp: 字幕帧包装（包含 AVSubtitle 数据）
    Frame* sp;
    // got_subtitle: decoder_decode_frame 返回值
    //   1 = 解出字幕，0 = 没字幕（继续），-1 = 出错
    int got_subtitle;
    double pts = 0;

    for (;;)
    {
        // 1. ★ 拿一个可写槽位
        //    字幕队列一般只有几帧大小（字幕不会像视频帧那么多）
        //    peek_writable 会阻塞等队列有空位，避免无限增长占用内存
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return;

        // 2. 解码一个字幕
        //    第 2 个参数传 nullptr（不需要 AVFrame）
        //    第 3 个参数传 &sp->sub（让 decoder_decode_frame 把字幕填到 sp->sub）
        if ((got_subtitle = decoder_decode_frame(&is->subdec, nullptr, &sp->sub)) < 0)
            break;  // ★ 返回负数：解码失败或流结束，退出线程

        pts = 0;

        // 3. ★ 只处理 format==1 的字幕（位图字幕）
        //    比如蓝光 PGS、DVD 字幕（一张 PNG 那种）
        //    文本字幕（SRT/ASS）走文本解析，不在这里
        if (got_subtitle && sp->sub.format == 1)
        {
            // AVSubtitle.pts 单位是微秒（AV_TIME_BASE = 1,000,000）
            // 转成秒
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts;
            // serial 用于 seek 后识别"这是新时间点的字幕"
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            // uploaded=0：还没上传到 GPU，让播放线程知道该走上传逻辑
            sp->uploaded = 0;

            // 4. 推入字幕帧队列
            //    视频播放线程会从这里拿字幕并叠加到画面上
            frame_queue_push(&is->subpq);
        }
        else if (got_subtitle)
        {
            // 其它格式（如文本字幕）暂时不处理
            // 释放 sp->sub 避免泄漏（如果不释放 AVSubtitle 内部的 rects 会泄漏）
            qWarning("Not handled subtitle type:%d", sp->sub.format);
            avsubtitle_free(&sp->sub);
        }
    }

    qDebug("-------- subtitle decode thread exit.");
    return;
}
