// ***********************************************************/
// packets_sync.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 数据结构与同步原语实现，参考自 ffplay.c：
//   - PacketQueue:  解复用线程 -> 解码线程 之间的 AVPacket 队列
//   - FrameQueue:   解码线程  -> 播放线程 之间的 AVFrame 队列
//   - Clock:        音视频同步用的"时钟"抽象
//   - Decoder:      单路流的解码器封装（视频/音频/字幕通用）
//   - 各种同步辅助函数（get_master_clock / compute_target_delay 等）
// ***********************************************************/

#include "packets_sync.h"

// 是否允许解码时丢帧
//   > 0 强制丢；= 0 非视频主时钟时丢；< 0 不丢
// 默认 -1（不丢）
int framedrop = -1;
// static int decoder_reorder_pts = -1;
// static int display_disable = 1;
// static int64_t audio_callback_time;

/**
 * @brief 初始化 PacketQueue
 *        - 内部用 av_fifo（FIFO 队列）存 packet
 *        - mutex/cond 用于多线程同步（put/get）
 *        - 默认 abort_request=1（需要 packet_queue_start 才允许取）
 */
//初始化队列和互斥锁、条件变量
int packet_queue_init(PacketQueue* q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
    if (!q->pkt_list)
        return AVERROR(ENOMEM);
    q->mutex = new QMutex();
    if (!q->mutex)
    {
        av_log(nullptr, AV_LOG_FATAL, "new QMutex() error.\n");
        return AVERROR(ENOMEM);
    }
    q->cond = new QWaitCondition();
    if (!q->cond)
    {
        av_log(nullptr, AV_LOG_FATAL, "new QWaitCondition() error.\n");
        return AVERROR(ENOMEM);
    }
    q->abort_request = 1;
    return 0;
}

/**
 * @brief 销毁 PacketQueue：清空 + 释放 FIFO + mutex + cond
 */
void packet_queue_destroy(PacketQueue* q)
{
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkt_list);
    delete q->mutex;
    delete q->cond;
}

/**
 * @brief 清空 PacketQueue 里所有 packet
 *        serial++ 是为了告诉消费方"这是新的一轮"，丢弃旧 packet
 */
void packet_queue_flush(PacketQueue* q)
{
    MyAVPacketList pkt1;

    q->mutex->lock();
    //丢弃队列中的所有包
    while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)
        av_packet_free(&pkt1.pkt);
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    q->serial++;
    q->mutex->unlock();
}

/**
 * @brief 启动队列：清掉 abort 标志，serial++ 让阻塞中的 get 重新检查
 */
void packet_queue_start(PacketQueue* q)
{
    q->mutex->lock();
    q->abort_request = 0;
    q->serial++;
    q->mutex->unlock();
}

/**
 * @brief 停止队列：设 abort 标志并唤醒所有等待者
 *        解码线程会立刻从 packet_queue_get 返回
 */
void packet_queue_abort(PacketQueue* q)
{
    q->mutex->lock();
    q->abort_request = 1;
    q->cond->wakeAll();
    q->mutex->unlock();
}

/**
 * @brief 从 PacketQueue 取一个 packet
 * @param block=1 阻塞等待；block=0 没数据立刻返回
 * @param serial  [out] 返回 packet 所属的 serial
 * @return  1=成功，0=无数据，-1=abort
 */
int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block, int* serial)
{
#if PRINT_PACKETQUEUE_INFO
    // packet_queue_print(q, pkt, "packet_queue_get");
#endif

    MyAVPacketList pkt1;
    int ret = 0;

    q->mutex->lock();

    for (;;)
    {
        // ★ 第一步：检查是否要求退出
        // abort_request = 1 表示"全部线程都要撤了"
        // 此时直接返回 -1，让调用方退出循环
        if (q->abort_request)
        {
            ret = -1;
            break;
        }

        // ★ 第二步：从 FIFO 队列里读一个元素
        // 这里的"读"是物理读：从队列里弹出一个 MyAVPacketList
        // av_fifo_read 返回值：>= 0 = 成功，负数 = 队列空
        if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)
        {
            // ★ 第三步：更新队列统计信息
            //   nb_packets：包数量减 1（不管包大小，每个包算 1）
            //   size：字节数减少（数据字节 + 链表节点本身的字节）
            //   duration：总时长减少（用 AV 包自带的 duration 字段）
            q->nb_packets--;
            q->size -= pkt1.pkt->size + sizeof(pkt1);
            q->duration -= pkt1.pkt->duration;

            // ★ 第四步：把 AVPacket 的"所有权"转移给调用方
            //   av_packet_move_ref 不是复制数据，而是把 pkt1.pkt 内部的指针
            //   转移给 pkt，然后把 pkt1.pkt 置空（避免 double free）
            //   性能上比 av_packet_ref 复制要快得多
            av_packet_move_ref(pkt, pkt1.pkt);

            // ★ 第五步：把 serial 传出去
            //   serial 用来区分"这一包是哪个播放代次"的（seek 后会变）
            //   调用方拿这个去比对 FrameQueue 是否过期
            if (serial)
                *serial = pkt1.serial;

            // ★ 第六步：释放 MyAVPacketList 自己的 AVPacket
            //   此时 pkt1.pkt 已经被 move_ref 置空了，av_packet_free 是 no-op
            //   但保留这个调用是为了代码清晰
            av_packet_free(&pkt1.pkt);
            ret = 1;
            break;
        }
        // 队列空的情况：
        else if (!block)
        {
            // block=0 表示"非阻塞"模式，没数据就立刻返回 0
            // 调用方拿到 0 可以去做别的事
            ret = 0;
            break;
        }
        else
        {
            // ★ 阻塞模式：睡在条件变量上
            // 释放 mutex，等 packet_queue_put 调 cond->wakeAll 再醒来
            // 醒来后会自动重新获取 mutex，然后回到 for(;;) 顶端重试
            q->cond->wait(q->mutex);
        }
    }
    q->mutex->unlock();
    return ret;
}

void packet_queue_print(const PacketQueue* q, const AVPacket* pkt, const QString& prefix)
{
    qDebug("[%s]Queue:[%p](nb_packets:%d, size:%d, dur:%d, serial:%d), "
           "pkt(pts:%lld,dts:%lld,size:%d,s_index:%d,dur:%lld,pos:%lld).",
           qUtf8Printable(prefix), q, q->nb_packets, q->size, q->duration,
           q->serial, pkt->pts, pkt->dts, pkt->size, pkt->stream_index,
           pkt->duration, pkt->pos);
}

/**
 * @brief 入队 packet（拷贝引用 + 重新分配）
 *        这是 PacketQueue 的"对外 put"，内部走 packet_queue_put_private
 */
int packet_queue_put(PacketQueue* q, AVPacket* pkt)
{
    AVPacket* pkt1;
    int ret = -1;

    pkt1 = av_packet_alloc();
    if (!pkt1)
    {
        av_packet_unref(pkt);
        return ret;
    }
    //pkt1内存申请成功，开始转移资源
    //将pkt中的资源权转给pkt1
    av_packet_move_ref(pkt1, pkt);

    q->mutex->lock();
    ret = packet_queue_put_private(q, pkt1);
    q->mutex->unlock();

    if (ret < 0)
        av_packet_free(&pkt1);

#if PRINT_PACKETQUEUE_INFO
        // packet_queue_print(q, pkt, "packet_queue_put");
#endif
    return ret;
}

/**
 * @brief 写入一个"空 packet"（用于 flush / 断开时让解码线程收到 EOF）
 */
int packet_queue_put_nullpacket(PacketQueue* q, AVPacket* pkt, int stream_index)
{
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/**
 * @brief 入队 packet 的实际工作（要求 mutex 已锁）
 */
int packet_queue_put_private(PacketQueue* q, AVPacket* pkt)
{
    MyAVPacketList pkt1;
    int ret;

    // ★ 第一步：检查是否要退出
    // 如果 abort_request 已置位，就不接收新包了（避免线程卡死）
    if (q->abort_request)
        return -1;

    // ★ 第二步：把 AVPacket 和 serial 打包成 MyAVPacketList
    //   pkt1.pkt 存的是指针（不复制数据）
    //   pkt1.serial 是当前播放代次（用于 seek 后识别旧包）
    pkt1.pkt = pkt;
    pkt1.serial = q->serial;

    // ★ 第三步：把 pkt1 写入 FIFO 队列
    //   复制的是 MyAVPacketList 本身（小结构），不复制 AVPacket 数据
    //   数据通过指针共享 → 节省内存
    //   av_fifo_write 参数：目标队列、数据指针、元素个数
    ret = av_fifo_write(q->pkt_list, &pkt1, 1);
    if (ret < 0)
        return ret;

    // ★ 第四步：更新队列统计
    //   三个维度：包数量、字节数、总时长
    q->nb_packets++;
    q->size += pkt1.pkt->size + sizeof(pkt1);
    q->duration += pkt1.pkt->duration;

    // ★ 第五步：唤醒所有等待这个队列的线程
    //   wakeAll 唤醒所有在 cond->wait 上睡着的消费者
    //   注意：不要写成 wakeOne，多个消费者要并行解码
    q->cond->wakeAll();
    return 0;
}

/**
 * @brief 初始化 FrameQueue（环形数组）
 *        - max_size 队列容量，上限 FRAME_QUEUE_SIZE
 *        - keep_last=1 时队列会保留最后一帧（rindex_shown=1）
 *          防止"读指针追过写指针"导致画面闪烁
 */
int frame_queue_init(FrameQueue* f, PacketQueue* pktq, int max_size, int keep_last)
{
    int i;
    memset(f, 0, sizeof(FrameQueue));
    //创建互斥锁
    if (!(f->mutex = new QMutex()))
    {
        av_log(nullptr, AV_LOG_FATAL, "new QMutex() error!\n");
        return AVERROR(ENOMEM);
    }
    //创建条件变量
    if (!(f->cond = new QWaitCondition()))
    {
        av_log(nullptr, AV_LOG_FATAL, "new QWaitCondition() error\n");
        return AVERROR(ENOMEM);
    }
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))//创建AVFrame,存储原始数据
            return AVERROR(ENOMEM);
    return 0;
}

/**
 * @brief 销毁 FrameQueue：释放每一帧 + mutex + cond
 */
void frame_queue_destory(FrameQueue* f)
{
    int i;
    for (i = 0; i < f->max_size; i++)
    {
        Frame* vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }

    delete f->mutex;
    delete f->cond;
}

/**
 * @brief 释放一帧引用的数据（不解 av_frame 本身）
 */
void frame_queue_unref_item(Frame* vp)
{
    av_frame_unref(vp->frame); // frame reference number reduce 1
    avsubtitle_free(&vp->sub); // sub
}

/**
 * @brief 唤醒所有等待 FrameQueue 的线程
 */
void frame_queue_signal(FrameQueue* f)
{
    f->mutex->lock();
    f->cond->wakeAll();
    f->mutex->unlock();
}

/**
 * @brief 取当前要显示/消费的帧（peek，不推进）
 */
Frame* frame_queue_peek(FrameQueue* f)
{
    //rindex_shown有两种状态
    //0代表当前帧还没显示 ，返回的就是当前帧
    //1 代表已经显示，返回的是下一帧
    //音视频同步核心，让播放线程"重复显示当前帧"而不推进读指针
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

/**
 * @brief peek 下一帧（用来预看 duration）
 */
Frame* frame_queue_peek_next(FrameQueue* f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

/**
 * @brief peek 最后一帧（已经显示过的）
 */
Frame* frame_queue_peek_last(FrameQueue* f) { return &f->queue[f->rindex]; }

/**
 * @brief 等一个可写槽位（解码线程往里放）
 * 让解码线程"取一个空位"来写新帧。如果队列满了，就阻塞等待；
 * 被叫醒后队列还没满，就返回这个空位的指针。
 */
Frame* frame_queue_peek_writable(FrameQueue* f)
{
    /* wait until we have space to put a new frame */
    f->mutex->lock();
    //队列满了但不是退出时，阻塞等待消费者消费
    while (f->size >= f->max_size && !f->pktq->abort_request)
    {
        f->cond->wait(f->mutex);//满了就睡
    }
    f->mutex->unlock();

    if (f->pktq->abort_request)
        return nullptr;

    return &f->queue[f->windex];
}

/**
 * @brief 等一个可读槽位（播放线程从这里取）
 */
Frame* frame_queue_peek_readable(FrameQueue* f)
{
    /* wait until we have a readable a new frame */
    f->mutex->lock();
    //队列中没有可读位置
    while (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request)
    {
        f->cond->wait(f->mutex);//点击stop停止产生循环等待死锁原因
    }
    f->mutex->unlock();

    if (f->pktq->abort_request)
        return nullptr;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

/**
 * @brief 提交刚写好的帧：推进写指针
 */
void frame_queue_push(FrameQueue* f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    f->mutex->lock();
    f->size++;
    f->cond->wakeAll();
    f->mutex->unlock();
}

/**
 * @brief 释放当前帧并推进读指针（播放完一帧后调用）
 */
void frame_queue_next(FrameQueue* f)
{
    if (f->keep_last && !f->rindex_shown)
    {
        f->rindex_shown = 1;// ★ 只标记 rindex 这一帧
        return;             // 不释放、不推进
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    f->mutex->lock();
    f->size--;
    f->cond->wakeAll();
    f->mutex->unlock();
}

/**
 * @brief 队列中"还没显示"的帧数（=size - rindex_shown）
 */
int frame_queue_nb_remaining(FrameQueue* f)
{
    //队里所有帧-当前是不是已经显示
    //因为任何时刻，队里最多只有1帧处于“已显示但保留状态”
    return f->size - f->rindex_shown;
}

/**
 * @brief 最近显示的那一帧的文件偏移（用于进度条）
 *        没用上，-1 表示无效
 */
int64_t frame_queue_last_pos(FrameQueue* f)
{
    Frame* fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}

/**
 * @brief 把一帧视频放到 pictq 队列
 *        解码线程每拿到一帧 AVFrame 就调一次
 *        - 等可写槽位
 *        - 拷引用（av_frame_move_ref）
 *        - 写 pts/duration/serial/pos
 *        - push（推进写指针）
 *
 *        queue_picture 是"把解码好的视频帧"放进 pictq（视频帧队列）的封装函数。
 *它把"申请位置 → 填数据 → 推进写指针"这三步打包在一起。
 */
int queue_picture(VideoState* is, AVFrame* src_frame, double pts, double duration, int64_t pos, int serial)
{
#if PRINT_PACKETQUEUE_INFO
    // 调试打印：把这一帧的所有关键信息都列出来
    // 包括宽高、格式、是否关键帧、PTS、duration、pos、serial
    qDebug("queue picture, w:%d, h:%d, nb:%d, ft:%d(%s), kf:%d, pic_t:%d(%c), "
           "pts:%lf, duration:%lf, pos:%lld, serial:%d",
           src_frame->width, src_frame->height, src_frame->nb_samples,
           src_frame->format,
           av_get_sample_fmt_name(AVSampleFormat(src_frame->format)),
           src_frame->key_frame, src_frame->pict_type,
           av_get_picture_type_char(src_frame->pict_type), pts, duration, pos,
           serial);
#endif

    Frame* vp;

    // ★ 第一步：从 pictq 申请一个"可写位置"
    //   frame_queue_peek_writable 内部会：
    //   1) 加锁
    //   2) 如果队列满 → 在 cond 上阻塞等待
    //   3) 有空位了 → 返回 Frame 指针
    //   返回 nullptr 说明收到 abort_request
    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    // ★ 第二步：填元数据（采样宽高比 + 标志位）
    vp->sar = src_frame->sample_aspect_ratio;  // SAR（像素宽高比）
    vp->uploaded = 0;                            // 是否已上传到 GPU（未上传）

    // ★ 第三步：填帧基本信息
    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    // ★ 第四步：填时间信息（★ 同步用的关键数据）
    //   pts：这一帧的显示时间戳（秒）
    //   duration：这一帧该显示多久（用于算下一帧时间）
    //   pos：这一帧在文件中的字节偏移（用于定位）
    //   serial：播放代次（seek 后会变）
    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    // ★ 第五步：把 AVFrame 的数据"搬"到队列槽里
    //   av_frame_move_ref：转移所有权（不复制数据）
    //   性能关键：避免深拷贝
    av_frame_move_ref(vp->frame, src_frame);

    // ★ 第六步：推进 FrameQueue 的写指针
    //   内部会更新 size 并 wakeAll 唤醒消费者
    frame_queue_push(&is->pictq);
    return 0;
}

/**
 * @brief 视频解码线程的主循环体
 *        1. decoder_decode_frame() 走"喂包 -> 取帧"完整流程
 *        2. 算 PTS
 *        3. 如果 framedrop 开启，且这帧比主时钟慢太多，丢掉
 *        4. 否则把这一帧推进 pictq
 *
 * @return  1=拿到一帧，0=暂时没帧（EAGAIN），-1=错误/EOF
 */
int get_video_frame(VideoState* is, AVFrame* frame)
{
    int got_picture = -1;

    // ★ 委托给通用解码器状态机
    //   返回值: 1=拿到一帧, 0=EAGAIN(暂没帧), -1=EOF/错误
    if ((got_picture = decoder_decode_frame(&is->viddec, frame, nullptr)) < 0)
        return -1;

    if (got_picture)
    {
        double dpts = NAN;

        // ★ 把 FFmpeg 给的整数 PTS 换算成"秒"
        //   公式：秒 = time_base.den / time_base.num * pts
        //   time_base 是流的时基（如 1/12800 表示 12800 ticks/秒）
        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        // ★ 自动猜测 SAR（Sample Aspect Ratio，像素宽高比）
        //   有些 MP4 容器没把 SAR 写在 stream 头里，需要根据 frame 内容猜
        //   SAR 影响最终画面比例：实际宽高比 = width * SAR
        frame->sample_aspect_ratio =
            av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        // ★ 丢帧策略：framedrop>0 强制丢；framedrop=0 仅当视频不是主时钟时丢
        //   framdop<0 完全不丢（默认）
        if (framedrop > 0 ||
            (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))
        {
            if (frame->pts != AV_NOPTS_VALUE)
            {
                // ★ diff = 视频帧的 PTS - 主时钟当前时间
                //   diff < 0 表示这帧已经"过期"了（比主时钟慢），可以丢
                double diff = dpts - get_master_clock(is);
                // ★ 4 个丢帧前置条件，缺一不可：
                //   1) diff 是有效数字（主时钟已被初始化）
                //   2) diff 在 ±10s 范围内（太大就别丢，等"重新对齐"）
                //   3) diff 比滤镜延迟还小（避免丢一帧马上又被滤镜"吐"出来）
                //   4) 视频 decoder 的 serial 跟视频 clock 一致（没在 seek 过程中）
                //   5) packet 队列还有包（不是"快没了"的最后一帧）
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets)
                {
                    is->frame_drops_early++;
                    // ★ 释放 frame 内部的 data 引用（不解 frame 本身）
                    av_frame_unref(frame);
                    got_picture = 0;  // 告诉调用方"这一帧我没要"
                }
            }
        }
    }

    return got_picture;
}

/**
 * @brief 初始化 Decoder
 *        - 分配 AVPacket
 *        - 绑定 解码器上下文 和 packet queue
 *        - empty_queue_cond 是"packet 队列空了"的通知（用来给读线程发信号）
 */
int decoder_init(Decoder* d, AVCodecContext* avctx, PacketQueue* queue, QWaitCondition* empty_queue_cond)
{
    memset(d, 0, sizeof(Decoder));
    d->pkt = av_packet_alloc();
    if (!d->pkt)
        return AVERROR(ENOMEM);
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
    return 0;
}

/**
 * @brief 启动 Decoder：把 packet_queue 解禁，记录线程指针
 */
int decoder_start(Decoder* d, void* thread, const char* thread_name)
{
    packet_queue_start(d->queue);//启动队列
    d->decoder_tid = thread;
    d->decoder_name = av_strdup(thread_name);
    return 0;
}

/**
 * @brief 销毁 Decoder：释放 AVPacket、AVCodecContext、线程名字
 */
void decoder_destroy(Decoder* d)
{
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->avctx);
    av_free(d->decoder_name);
}

/**
 * @brief 终止解码线程：abort queue + 唤醒 + 等待线程退出 + flush
 *        注意这里用 QThread::wait() 替代了原 ffplay 的 SDL_WaitThread
 */
//`decoder_abort` 要同时掐断解码线程**所有可能的阻塞点**（PacketQueue 的 get + FrameQueue 的 peek_writable/peek_readable）
void decoder_abort(Decoder* d, FrameQueue* fq)
{
    //两个阻塞点，唤醒阻塞在 （等包）/（等帧队列槽位）的解码线程
    packet_queue_abort(d->queue);//停止队列，唤醒所有消费者（解码线程）
    frame_queue_signal(fq);//唤醒所有等待 FrameQueue 的线程
    // SDL_WaitThread(d->decoder_tid, nullptr);

    //等待上面结束
    ((QThread*)(d->decoder_tid))->wait();
    d->decoder_tid = nullptr;
    packet_queue_flush(d->queue);
}

/**
 * @brief 通用解码主函数：同时处理音视频字幕
 *
 * 内部循环：
 *   1. 如果 queue serial 跟当前 pkt_serial 一致，调 avcodec_receive_frame 取一帧
 *      - 视频：处理 best_effort_timestamp / pkt_dts
 *      - 音频：算采样时间为 pts（next_pts 续帧）
 *   2. 拿不到帧就尝试从 packet queue 取一个 packet，send 给解码器
 *   3. 如果 serial 变化（seek 后），调 avcodec_flush_buffers 清掉解码器缓存
 *   4. 字幕走 avcodec_decode_subtitle2 单独路径
 */
int decoder_decode_frame(Decoder* d, AVFrame* frame, AVSubtitle* sub)
{
    int ret = AVERROR(EAGAIN);
    int decoder_reorder_pts = -1;
    for (;;)
    {
        // ★ 状态机入口 1：先看"解码器缓存里有没有攒好的帧可以取"
        //   只有当 packet 队列的 serial 跟 decoder 当前记录的 pkt_serial 一致
        //   才认为之前送进解码器的那些包"还作数"，否则该先 flush 再取帧
        if (d->queue->serial == d->pkt_serial)
        {
            do
            {
                // ★ 退出检查：用户可能随时点"停止播放"
                // 任何时候都要先看 abort，否则线程关不掉
                if (d->queue->abort_request)
                    return -1;

                switch (d->avctx->codec_type)
                {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0)
                        {
                            // ★ 视频帧的 PTS 选取策略
                            //   best_effort_timestamp：FFmpeg 自动从各种来源里挑最合理的 PTS
                            //   pkt_dts：仅当用户强制要求按 DTS 排序时才用
                            if (decoder_reorder_pts == -1)
                            {
                                frame->pts = frame->best_effort_timestamp;
                            }
                            else if (!decoder_reorder_pts)
                            {
                                frame->pts = frame->pkt_dts;
                            }
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0)
                        {
                            // ★ 音频帧的 PTS 换算
                            //   一帧音频的 PTS 单位是"采样数"，不是秒
                            //   这里把任意时间基换算成 {1, sample_rate}，方便后续用 sample 数累加
                            AVRational tb = AVRational{1, frame->sample_rate};
                            if (frame->pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                            else if (d->next_pts != AV_NOPTS_VALUE)
                                // ★ 兜底：用上一帧的 next_pts 推算本帧的 PTS
                                //   场景：个别音频帧没有 PTS 字段，靠"上一帧 + nb_samples"续帧
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                            if (frame->pts != AV_NOPTS_VALUE)
                            {
                                // ★ 记录"下一帧的起点 PTS"（=本帧 PTS + 本帧采样数）
                                //   这样下一帧没 PTS 时也能接着算
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb;
                            }
                        }
                        break;
                }
                // ★ 收到 EOF：packet 队列已经被塞了"空包"（flush packet）
                //   含义是"数据全送完了，把解码器里最后几帧也冲出来"
                if (ret == AVERROR_EOF)
                {
                    d->finished = d->pkt_serial;
                    // ★ flush 解码器：清掉内部缓存，避免下次重新打开时残留旧帧
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
                // ★ 拿到 EAGAIN：解码器缓存空了，需要再喂 packet 才能继续解
                //   内层 do-while 退出，外层 for(;;) 进入"喂包"分支
            } while (ret != AVERROR(EAGAIN));
        }

        do
        {
            // ★ 队列空了：通知"读线程"努力读包
            //   读线程会在 empty_queue_cond 上等待，这里叫醒它
            if (d->queue->nb_packets == 0)
                d->empty_queue_cond->wakeAll();

            // ★ packet_pending：处理"解码器返回 EAGAIN 但我们手上还有包没塞"的情况
            //   比如解码器缓存已满但还有包没送出去，先把这包"留着"，等下次 send
            if (d->packet_pending)
            {
                d->packet_pending = 0;
            }
            else
            {
                int old_serial = d->pkt_serial;
                // ★ 阻塞取一包 packet
                //   block=1 表示"没包就睡"，由 packet_queue_put 唤醒
                if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0)
                    return -1;
                // ★ serial 变化 = 用户触发了 seek
                //   之前所有 frame 都作废了，必须 flush 解码器才能继续
                if (old_serial != d->pkt_serial)
                {
                    avcodec_flush_buffers(d->avctx);
                    d->finished = 0;
                    // ★ 重置音频帧的续帧基线
                    //   seek 后第一帧的 PTS 用 start_pts（外部设定）
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            }
            // ★ 跟当前 packet 队列的 serial 匹配了才进 send 阶段
            //   不匹配说明中途被 seek 打断，丢掉这个包再等下一包
            if (d->queue->serial == d->pkt_serial)
                break;
            av_packet_unref(d->pkt);
        } while (1);

        if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE)
        {
            int got_frame = 0;
            ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, d->pkt);
            if (ret < 0)
            {
                ret = AVERROR(EAGAIN);
            }
            else
            {
                // ★ 字幕的 packet 可能"一个包产生多帧"（datas 数组）
                //   当 got_frame=1 但 pkt->data 已经被解码器消耗完
                //   标记 packet_pending，让下一轮再补喂一次
                if (got_frame && !d->pkt->data)
                {
                    d->packet_pending = 1;
                }
                ret = got_frame ? 0 : (d->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
            }
            av_packet_unref(d->pkt);
        }
        else
        {
            // ★ 音视频：把 packet 喂给解码器
            //   理论上 EAGAIN 不应该出现（我们已经 avcodec_receive_frame EAGAIN 过）
            //   出现就说明是 FFmpeg API 内部状态问题，标记 pending 留到下一轮
            if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN))
            {
                av_log(d->avctx, AV_LOG_ERROR,
                       "Receive_frame and send_packet both returned EAGAIN, which is "
                       "an API violation.\n");
                d->packet_pending = 1;
            }
            else
            {
                av_packet_unref(d->pkt);
            }
        }
    }
}

/**
 * @brief 用单独的 FormatContext open 文件，只取总时长（不影响正在播放的 state）
 *        一般在设置窗口里显示"媒体信息"用
 */
//获取媒体文件总时长，只打开文件读取元数据
void get_file_info(const char* filename, int64_t& duration)
{
    AVFormatContext* pFormatCtx = avformat_alloc_context();
    if (avformat_open_input(&pFormatCtx, filename, NULL, NULL) == 0)
    {
        if (pFormatCtx->duration < 0)
            avformat_find_stream_info(pFormatCtx, NULL);
        duration = pFormatCtx->duration;
    }
    // etc
    avformat_close_input(&pFormatCtx);
    avformat_free_context(pFormatCtx);
}

/**
 * @brief 把 AVFormatContext 的微秒时长拆成 时:分:秒.厘秒
 *        +5000 微秒是四舍五入用的
 */
//把 FFmpeg 的 duration （微秒）拆成"小时:分钟:秒:厘秒"四部分 ，专用于显示进度条或时间标签
void get_duration_time(const int64_t duration_us, int64_t& hours, int64_t& mins, int64_t& secs, int64_t& us)
{
    int64_t duration = duration_us + (duration_us <= INT64_MAX - 5000 ? 5000 : 0);
    duration = duration < 0 ? 0 : duration;
    secs = duration / AV_TIME_BASE;
    us = duration % AV_TIME_BASE;
    us = (100 * us) / AV_TIME_BASE;
    mins = secs / 60;
    secs %= 60;
    hours = mins / 60;
    mins %= 60;
}

/**
 * @brief 取 Clock 当前值（秒）
 *        暂停时直接返回 pts；播放时用 pts_drift + (now - last_updated) 推算
 *        还要判断 serial 是否还匹配，不匹配返回 NaN
 */
//暂停时返回上一次记录点，不暂停返回上一次记录点与流逝的时间之和。
double get_clock(Clock* c)
{
    // ★ serial 校验：Clock 跟 PacketQueue 强绑定
    //   队列换 serial = 发生过 seek = 旧 PTS 无效
    //   此时必须返回 NaN，让上层"重新等同步"
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused)
    {
        // ★ 暂停时直接返回上次记录的 pts
        //   期间不会调用 set_clock，所以 pts 不会变 → 时间"冻结"
        return c->pts;
    }
    else
    {
        // ★ 正常播放：根据"漂移 + 流逝时间"推算当前 PTS
        //   time - (time - last_updated) * (1.0 - speed) 是"考虑倍速"的修正
        //   speed=1.0 时这一项退化为 time，所以 speed=1.0 时就是最朴素的 drift+elapsed
        //   speed=2.0 时时间走得比 wall time 快一倍
        double time = av_gettime_relative() / 1000000.0;//**从系统某个参考点开始，到现在一共走过了多少秒**。
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

/**
 * @brief 把 Clock 设到 pts（同时记录"漂移 = pts - time"）
 *        time 一般是 av_gettime_relative()/1e6
 */
void set_clock_at(Clock* c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;//记录时刻的 pts 减去记录时刻的墙钟
    c->serial = serial;
}

/**
 * @brief set_clock 的便捷版本，自动取当前时间
 */
void set_clock(Clock* c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

/**
 * @brief 改 Clock 的"播放速度"（用于倍速播放）
 *        通过把当前 pts 重新放回 drift 里 + 改 speed 字段实现
 */
void set_clock_speed(Clock* c, double speed)
{
    //设置倍速之前先获取当前时钟时间，固定当前位置
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

/**
 * @brief 初始化 Clock：速度 1.0、不暂停、pts=NaN
 */
void init_clock(Clock* c, int* queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

/**
 * @brief 把主时钟 c 跟从时钟 slave 同步
 *        如果主时钟是 NaN 或偏差太大，就 copy 从时钟的值
 */
void sync_clock_to_slave(Clock* c, Clock* slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    // ★ 同步触发条件（两个满足任一即可）：
    //   1) 从时钟还没初始化（isnan）— 这种情况没必要同步，但代码把"主未初始化+从已初始化"也当成"用从的值"
    //   2) 两者偏差超过 AV_NOSYNC_THRESHOLD（默认 10s）
    //      比如 seek 后时钟错位，需要尽快"跳"到新位置
    // ★ 注意：把"偏差大"用 set_clock 处理，相当于"硬同步"
    //   偏差小则不动，保留平滑的时钟增长
    if (!isnan(slave_clock) &&
        (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

/**
 * @brief 判断流对应的 PacketQueue 是否已经攒够了数据
 *        用来决定 ReadThread 是不是该睡一会儿（不要把文件读爆）
 */
int stream_has_enough_packets(AVStream* st, int stream_id, PacketQueue* queue)
{
    return stream_id < 0 || queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES &&
               (!queue->duration ||
                av_q2d(st->time_base) * queue->duration > 1.0);
}

/**
 * @brief 判断是不是实时流（rtp/rtsp/udp）
 *        实时流用外部时钟更合适
 */
int is_realtime(AVFormatContext* s)
{
    if (!strcmp(s->iformat->name, "rtp") || !strcmp(s->iformat->name, "rtsp") ||
        !strcmp(s->iformat->name, "sdp"))
        return 1;

    if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4)))
        return 1;
    return 0;
}

/**
 * @brief 根据用户设置和流的可用性，决定"主时钟"到底用 视频/音频/外部时钟
 *        优先选用户指定的；用户选视频但没视频流就降级到音频，依此类推
 */
int get_master_sync_type(VideoState* is)
{
    // ★ 同步策略的"降级链"——按用户指定 → 流可用性 来选主时钟
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER)
    {
        // ★ 用户要求"以视频为主时钟"
        //   但如果这个文件根本没视频流（比如纯音频），就降级到音频
        //   不能让系统去找一个不存在的时钟
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    }
    else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER)
    {
        // ★ 默认情况：以音频为主时钟（音频最连续，最适合做基准）
        //   同样：没音频就降级到外部时钟
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    }
    else
    {
        // ★ 用户显式选了外部时钟，或者上面都降级失败
        //   外部时钟 = 用 wall time 推算，不依赖任何流
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/**
 * @brief 取主时钟的当前值（秒），由 get_master_sync_type 决定取哪个 Clock
 */
double get_master_clock(VideoState* is)
{
    double val;

    switch (get_master_sync_type(is))
    {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}

/**
 * @brief 当没有音频流时，根据包队列长度动态调外部时钟的 speed
 *        队列快空了 -> 降速（重复）；队列太多了 -> 升速（丢帧）
 *        用来让外部时钟尽量追播放进度
 */
void check_external_clock_speed(VideoState* is)
{
    // ★ 场景 1：某路流的 packet 队列快空了
    //   触发条件：视频/音频（只要存在的话）的 nb_packets ≤ 最小帧数
    //   含义：数据流供应不上来了
    //   对策：把外部时钟的 speed 调慢一点（重复显示）— 让 wall time 跑得比播放进度慢
    //   看起来像"播放停顿了一会儿"，实际是给读包留时间
    if ((is->video_stream >= 0 &&
         is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) ||
        (is->audio_stream >= 0 &&
         is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES))
    {
        // ★ FFMAX 限速不低于 EXTERNAL_CLOCK_SPEED_MIN（默认 0.5，即最低半速）
        //   用减法步进 - 步长 = EXTERNAL_CLOCK_SPEED_STEP（默认 0.01）
        set_clock_speed(&is->extclk,
                        FFMAX(EXTERNAL_CLOCK_SPEED_MIN,
                              is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    }
    else if ((is->video_stream < 0 ||
              is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
             (is->audio_stream < 0 ||
              is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES))
    {
        // ★ 场景 2：所有流的包都太多了（远超正常水位）
        //   含义：解码器来不及消化，读线程读得太快
        //   对策：把外部时钟的 speed 调快一点 — 让"未来时间"提前，迫使用户端"跳到匹配点"
        //   实际上播放器会丢帧来追
        set_clock_speed(&is->extclk,
                        FFMIN(EXTERNAL_CLOCK_SPEED_MAX,
                              is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    }
    else
    {
        // ★ 场景 3：包数量在合理范围
        //   含义：暂时不需要干预
        //   但 speed 已经被调过（不等于 1.0），需要慢慢"拉回来"
        //   用一个非线性公式：离 1.0 越远，回得越快；接近 1.0 时回得越慢
        //   避免震荡
        double speed = is->extclk.speed;
        if (speed != 1.0)
            set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP *
                                                     (1.0 - speed) /
                                                     fabs(1.0 - speed));
    }
}

/**
 * @brief 发起 seek 请求：把目标位置存到 is->seek_pos，唤醒读线程
 *        读线程会真的去 av_seek_frame，然后清空队列
 */
void stream_seek(VideoState* is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    // ★ 防重入：如果已经有一个 seek 请求还没被读线程处理，就忽略这次的
    //   避免读线程被频繁打断（每次都重新 seek 会卡顿）
    if (!is->seek_req)
    {
        // ★ 把"要 seek 到哪"存到 VideoState 上
        //   seek_pos: 绝对位置（时间戳或字节偏移，取决于 seek_by_bytes）
        //   seek_rel: 相对偏移（UI 拖进度条时用）
        is->seek_pos = pos;
        is->seek_rel = rel;
        // ★ 清掉"按字节 seek"标志，再根据本次入参决定要不要设
        //   不能直接 |=，因为用户可能从"按时间"切到"按字节"
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        // ★ 标记有 seek 请求，等读线程在主循环里看到
        is->seek_req = 1;
        // ★ 唤醒读线程
        //   读线程会在 continue_read_thread 上睡，等有包就起来读
        //   这里是"有 seek 要处理"，也叫醒它
        is->continue_read_thread->wakeAll();
    }
}

/* pause or resume the video */
// void stream_toggle_pause(VideoState* is, bool pause)
//{
//	if (is->paused) {
//		is->frame_timer += av_gettime_relative() / 1000000.0 -
//is->vidclk.last_updated; 		if (is->read_pause_return != AVERROR(ENOSYS)) {
//			is->vidclk.paused = 0;
//		}
//		set_clock(&is->vidclk, get_clock(&is->vidclk),
//is->vidclk.serial);
//	}
//	set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
//	is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused =
//pause; // !is->paused; 	is->step = 0;
//}

/**
 * @brief 切换暂停状态
 *        - 从暂停恢复时，需要补偿 frame_timer（暂停期间流逝的 wall time）
 *        - 同步所有 4 个 Clock（vid/aud/ext）的 paused 标志
 *        - is->paused 是统一开关
 */
void toggle_pause(VideoState* is, bool pause)
{
    // ★ 正在"从暂停恢复"——需要补偿时间
    if (is->paused)
    {
        // ★ frame_timer 记录"上一帧视频该显示的时刻"
        //   暂停期间 wall time 继续走，但时钟没走
        //   恢复时要把"暂停期间流逝的 wall time"加回到 frame_timer
        //   否则下一帧会被认为"早该显示了"，立刻刷新
        is->frame_timer +=
            av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        // ★ read_pause_return != ENOSYS 表示读线程支持"暂停时也读包"
        //   （实时流特性，需要提前把包攒好）
        //   这种情况下视频时钟需要先"跳回"原值，再 unpause
        if (is->read_pause_return != AVERROR(ENOSYS))
        {
            is->vidclk.paused = 0;
        }
        // ★ 重新 set 一下视频时钟
        //   目的：让 pts_drift 重新基于"现在"计算（避免恢复时跳一大段）
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    // ★ 外部时钟也重新 set
    //   原因同上：恢复时让它基于"现在的 wall time"
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    // ★ 4 个时钟的 paused 标志 + 统一开关 is->paused 一起更新
    //   任何一路流"暂停"都要 4 个 clock 一起停
    //   （暂停/恢复本质是"时间流不流"，所有流共享这个开关）
    //刷新暂停/播放标记
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused =
        pause; // !is->paused;
    is->step = 0;
}

/**
 * @brief 切换静音状态
 */
void toggle_mute(VideoState* is, bool mute)
{
    bool muted = !!is->muted;//int->bool
    if (muted != mute)
    {
        is->muted = mute;
    }
}

/**
 * @brief 调音量（dB 步进方式），通过 SDL_MIX_MAXVOLUME 折算
 *        实际项目里没用到，保留是为了完整性
 */
void update_volume(VideoState* is, int sign, double step)
{
    double volume_level =
        is->audio_volume
            ? (20 * log(is->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10))
            : -1000.0;
    int new_volume =
        lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    is->audio_volume = av_clip(
        is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume,
        0, SDL_MIX_MAXVOLUME);
}

/**
 * @brief 单步播放：先取消暂停，设 step=1
 *        video_refresh 检测到 step=1 后只显示一帧就自动暂停
 */
void step_to_next_frame(VideoState* is)
{
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        toggle_pause(is, !is->paused);
    is->step = 1;
}

/**
 * @brief 音视频同步的核心：算出"这一帧该延迟多久显示"
 *
 * 思路（视频以音频为基准时）：
 *   diff = 视频时钟 - 主时钟
 *   - diff <= -threshold ：视频超前了，要把 delay 缩短（让视频晚一点）
 *   - diff >= +threshold ：视频落后了
 *       - delay 超过 AV_SYNC_FRAMEDUP_THRESHOLD：直接延迟 delay+diff（重复一帧）
 *       - 否则 delay 翻倍（让视频等一会）
 *   否则 diff 在合理范围，delay 不变
 *   delay相当于这一帧应该显示多久
 *  **delay 输入** = "这帧天然该显示多久"（帧率的倒数）
    **delay 返回值** = "考虑到音视频差，这帧实际该显示多久"
 */
double compute_target_delay(double delay, VideoState* is)
{
    double sync_threshold, diff = 0;

    // ★ 只有在"视频是从时钟"时才需要校正（视频主时钟时视频就是基准，无需校正）
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)
    {
        // ★ diff = 视频时钟 - 主时钟（正=视频超前，负=视频落后）  偏差
        diff = get_clock(&is->vidclk) - get_master_clock(is);//主时钟是音频

        // ★ 同步阈值：取 [0.04, 0.1] 之间，且不高于 delay
        sync_threshold =
            FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        //修正方向
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration)
        {
            if (diff <= -sync_threshold)// ① 视频落后太多
                delay = FFMAX(0, delay + diff);//    缩短 delay → 视频追上去
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;// ② 视频超前太多，且帧长>0.1s
                                      //    直接延迟 delay+diff，
            //视频超前时恰恰要 `delay + diff`（增大 delay）让视频慢下来，而不是缩短

            else if (diff >= sync_threshold)// ③ 视频超前太多，正常帧长
                delay = 2 * delay;//    delay 翻倍 → 等音频
        }
    }

    av_log(nullptr, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);
    return delay;
}

/**
 * @brief 计算两帧之间的 duration
 *        - 优先用 pts 差
 *        - 异常值回退用 vp->duration（来自 packet 的 duration 字段）
 *        - 跨 serial 视为 0
 */
//返回的就是 delay 的 "初始值"
double vp_duration(VideoState* is, Frame* vp, Frame* nextvp)
{
    if (vp->serial == nextvp->serial)// 同一播放段才可比
    {
        double duration = nextvp->pts - vp->pts;// 优先用 PTS 差
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;// 异常值回退用帧自带 duration
        else
            return duration;
    }

    return 0.0;
}

/**
 * @brief 更新视频时钟，并把外部时钟跟它同步
 *        调用时机：刚显示完一帧视频
 */
void update_video_pts(VideoState* is, double pts, int64_t pos, int serial)
{
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

#if PRINT_PACKETQUEUE_INFO
void print_state_info(VideoState* is)
{
    if (is)
    {
        PacketQueue* pPacket = &is->videoq;
        qDebug("[VideoState] V PacketQueue[%p](nb_packets:%d,size:%d,dur:%lld, "
               "abort:%d, serial:%d)",
               pPacket, pPacket->nb_packets, pPacket->size, pPacket->duration,
               pPacket->abort_request, pPacket->serial);

        pPacket = &is->audioq;
        qDebug("[VideoState] A PacketQueue[%p](nb_packets:%d,size:%d,dur:%lld, "
               "abort:%d, serial:%d)",
               pPacket, pPacket->nb_packets, pPacket->size, pPacket->duration,
               pPacket->abort_request, pPacket->serial);

        pPacket = &is->subtitleq;
        qDebug("[VideoState] S PacketQueue[%p](nb_packets:%d,size:%d,dur:%lld, "
               "abort:%d, serial:%d)",
               pPacket, pPacket->nb_packets, pPacket->size, pPacket->duration,
               pPacket->abort_request, pPacket->serial);

        /*qDebug("[VideoState]FrameQueue(v:%p,a:%p,s:%p)",
            &is->pictq, &is->sampq, &is->subpq);
    qDebug("[VideoState]Decoder(v:%p,a:%p,s:%p)",
            &is->viddec, &is->auddec, &is->subdec);
    qDebug("[VideoState]Clock(v:%p,a:%p,s:%p)",
            &is->vidclk, &is->audclk, &is->extclk);*/
    }
}
#endif


//用户改了倍速，把倍速"翻译"成 FFmpeg 看得懂的滤镜字符串。
#if USE_AVFILTER_AUDIO
void set_audio_playspeed(VideoState* is, double value)
{
    // ★ 限速：<0.25 倍速的 atempo 滤镜不支持（FFmpeg 限制 0.5~2.0）
    //   4.0 是另一个边界（单 atempo 滤镜最多 2x，必须叠加）
    if (value < 0 || value > 4)
        return;

    // ★ 速度没变就不重配滤镜（避免不必要的 avfilter 重启）
    if (is->audio_speed == value)
        return;

    is->audio_speed = value;

    const size_t len = 32;
    if (!is->afilters)
        is->afilters = (char*)av_malloc(len);

    if (value <= 0.5)
    {
        // ★ 倍速 0~0.5：单个 atempo 滤镜最低 0.5
        //   必须叠加两个：第一级固定 0.5，第二级用 value/0.5 把"剩余减速"补上
        //   举例：value=0.25 → "atempo=0.5,atempo=0.5" → 0.5*0.5=0.25
        snprintf(is->afilters, len, "atempo=0.5,");
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "atempo=%lf", value / 0.5);

        // strncat(is->afilters, tmp, len - strlen(is->afilters) - 1);
        strncat_s(is->afilters, len, tmp, len - strlen(is->afilters) - 1);
    }
    else if (value <= 2.0)
    {
        // ★ 倍速 0.5~2.0：单 atempo 滤镜就够
        //   FFmpeg 内部会自动选最佳重采样算法
        snprintf(is->afilters, len, "atempo=%lf", value);
    }
    else
    {
        // ★ 倍速 2.0~4.0：必须叠加两个 atempo
        //   原理同减速版：第一级固定 2.0，第二级用 value/2.0 补"剩余加速"
        //   举例：value=3.0 → "atempo=2.0,atempo=1.5" → 2*1.5=3
        snprintf(is->afilters, len, "atempo=2.0,");
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "atempo=%lf", value / 2.0);
        // strncat(is->afilters, tmp, len - strlen(is->afilters) - 1);
        strncat_s(is->afilters, len, tmp, len - strlen(is->afilters) - 1);
    }

    qDebug("changing audio filters to :%s", is->afilters);

#if USE_AVFILTER_VIDEO
    // ★ 视频也要同步变倍速（用 setpts 滤镜改 PTS 缩放系数）
    //   音频 1.0x 对应视频 1.0x；音频 2.0x 对应视频 2.0x
    set_video_playspeed(is);
#endif

    // ★ 记住"变倍速前的音频时钟值"
    //   等新滤镜生效后，用这个值 + 已播放时长推算"新基线"
    is->audio_clock_old = is->audio_clock;
    // ★ 标记"音频滤镜需要重配"
    //   上层在合适的时机（一般是下一次 audio_thread 调 configure_audio_filters）会重配
    is->req_afilter_reconfigure = 1;
}

void set_video_playspeed(VideoState* is)
{
    double speed = is->audio_speed;

    size_t len = 32;
    if (!is->vfilters)
        is->vfilters = (char*)av_malloc(len);

    snprintf(is->vfilters, len, "setpts=%.4lf*PTS", 1.0 / speed);

    is->req_vfilter_reconfigure = 1;

    qDebug("changing video filters to :%s", is->vfilters);
}

//比较两个音频格式"是否相同" ——返回 0 就是相同，非 0 就是不同。
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

// int64_t get_valid_channel_layout(int64_t channel_layout, int channels)
//{
//	if (channel_layout && av_get_channel_layout_nb_channels(channel_layout)
//== channels) 		return channel_layout; 	else 		return 0;
//}
//把"滤镜字符串描述"真正搭建成一条可运行的滤镜链。
int configure_filtergraph(AVFilterGraph* graph, const char* filtergraph, AVFilterContext* source_ctx, AVFilterContext* sink_ctx)
{
    int ret;
    // ★ nb_filters 记下"调用本函数前 graph 里已有的滤镜数"
    //   avfilter_graph_parse_ptr 会把新滤镜追加到 graph 尾部
    //   后面那个 FFSWAP 循环就是要把"新追加的滤镜"挪到最前面
    //   （让自定义滤镜先执行，再接到源/汇）
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = nullptr, *inputs = nullptr;

    if (filtergraph)
    {
        // ★ 分配两个"虚拟节点"用于描述滤镜链的入口和出口
        //   outputs 描述"输出端"（连接到 source_ctx）
        //   inputs 描述"输入端"（连接到 sink_ctx）
        outputs = avfilter_inout_alloc();
        inputs = avfilter_inout_alloc();
        if (!outputs || !inputs)
        {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        // ★ outputs 是"滤镜图的最左端"，名字叫"in"
        //   接到 source_ctx（abuffer/buffer）的 pad 0
        outputs->name = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx = 0;
        outputs->next = nullptr;

        // ★ inputs 是"滤镜图的最右端"，名字叫"out"
        //   接到 sink_ctx（abuffersink/buffersink）的 pad 0
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_ctx;
        inputs->pad_idx = 0;
        inputs->next = nullptr;

        // ★ 真正解析滤镜描述字符串（如 "atempo=2.0,volume=0.5"）
        //   解析成功后会：
        //   1) 创建所有滤镜实例
        //   2) 按字符串顺序连成链
        //   3) 把首尾接到 source_ctx / sink_ctx
        //   失败返回 <0（比如滤镜名写错）
        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs,
                                            nullptr)) < 0)
            goto fail;
    }
    else
    {
        // ★ 没指定滤镜描述：直接 source -> sink 一根线
        //   等于"直通"，不经过任何处理
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged
   * first */
    // ★ 滤镜顺序调整：把"用户自定义的滤镜"挪到"系统自带的 source/sink 滤镜"前面
    //   原因：avfilter_graph_config 要求"输入先 ready，输出才能 link"
    //   自定义滤镜作为中间节点，排在前面才能被正确 link
    for (unsigned int i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i],
               graph->filters[i + nb_filters]);

    // ★ 真正"让滤镜图跑起来"
    //   内部会：检查格式协商、分配缓冲、建 link
    //   失败说明滤镜参数冲突（比如采样率不匹配）
    ret = avfilter_graph_config(graph, nullptr);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

//建一个"音频滤镜工厂"：造好入口/出口，规定出口格式，然后让装配工按配方搭流水线。
int configure_audio_filters(VideoState* is, const char* afilters, int force_output_format)
{
    // ★ 输出格式候选：S16（16bit 整数）— SDL 默认接受的就是这个
    //   7.x 中 abuffersink 的 sample_fmts 是 init-time option（不再是 runtime）
    //   所以必须通过 avfilter_graph_create_filter 的 args 字符串传递
    int sample_rates[2] = {0, -1};
    AVFilterContext *filt_asrc = nullptr, *filt_asink = nullptr;
    // char aresample_swr_opts[512] = "";
    // const AVDictionaryEntry* e = nullptr;
    char asrc_args[256];
    char asink_args[256]; // ★ 7.x 新增：abuffersink 的初始化参数（sample_fmts 等）
    int ret;
    AVBPrint bp;

    // ★ 每次重配都先释放旧的 graph，再 alloc 新的
    //   防止"上一次的滤镜链还在内存里"
    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    // ★ 0 = 自动选线程数（一般等于 CPU 核数）
    //   滤镜内部会自动并行处理（前提是滤镜本身支持）
    is->agraph->nb_threads = 0;

    // ★ 把"声道布局"格式化成可读字符串
    //   比如 5.1 → "5.1"，立体声 → "stereo"
    //   后面要塞给 abuffer 当参数
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &bp);

    // ★ 构造 abuffer 的初始化参数
    //   包含：采样率、采样格式、时间基、声道布局
    //   这些参数必须跟"喂给滤镜图的原始 AVFrame"完全匹配
    //   时间基用 1/freq（每个采样 = 1/freq 秒）— 跟 frame->pts 单位一致
    snprintf(asrc_args, sizeof(asrc_args),
             "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
             is->audio_filter_src.freq,
             av_get_sample_fmt_name(is->audio_filter_src.fmt), 1,
             is->audio_filter_src.freq, bp.str);

    // ★ 创建"入口滤镜" abuffer
    //   它的作用是接收"原始音频帧"（来自解码器），喂给后面的滤镜链
    ret = avfilter_graph_create_filter(
        &filt_asrc, avfilter_get_by_name("abuffer"), "ffplay_abuffer", asrc_args,
        nullptr, is->agraph);
    if (ret < 0)
        goto end;

    // ★★★ FFmpeg 7.x 关键修复 ★★★
    // 7.x 中 abuffersink 的选项名和类型都变了（参考 FFmpeg/libavfilter/buffersink.c）：
    //   - 选项名：sample_fmts → sample_formats（注意多个 s）
    //              sample_rates → samplerates
    //              ch_layouts   → channel_layouts（旧的 ch_layouts 已废弃）
    //   - 类型：  AV_OPT_TYPE_FLAG_ARRAY（数组）
    //            字符串中用 '|' 分隔多个值，如 "s16|s32"、"stereo|mono"
    //   - 属性：  变成 init-time option，必须通过 avfilter_graph_create_filter 的 args 字符串传递
    //   - 移除：  all_channel_counts 在 7.x 中已删除（用 channel_layouts 替代）
    if (force_output_format)
    {
        // 强制出口参数 = 入口参数
        // ★ 7.x：用 channel_layouts 字符串描述（"stereo"/"mono"/"5.1"），bp.str 已经是这个格式
        snprintf(asink_args, sizeof(asink_args),
                 "sample_formats=%s:samplerates=%d:channel_layouts=%s",
                 av_get_sample_fmt_name(AV_SAMPLE_FMT_S16),
                 is->audio_filter_src.freq,
                 bp.str);
    }
    else
    {
        // 限定为 S16（SDL 默认只支持 S16）
        //   不传 channel_layouts → 接受所有声道布局（5.1 自动 downmix 到 stereo）
        snprintf(asink_args, sizeof(asink_args),
                 "sample_formats=%s",
                 av_get_sample_fmt_name(AV_SAMPLE_FMT_S16));
    }

    // ★ 创建"出口滤镜" abuffersink
    //   它的作用是从滤镜链拿"处理过的音频帧"，交给 SDL 播放
    //   7.x：必须用 args 字符串设置 init-time options
    ret = avfilter_graph_create_filter(
        &filt_asink, avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
        asink_args, nullptr, is->agraph);
    if (ret < 0)
        goto end;

    // ★ 把入口、出口、滤镜描述串起来，搭成完整滤镜图
    if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc,
                                     filt_asink)) < 0)
        goto end;

    // ★ 保存入口/出口指针：解码线程往 in 喂帧，SDL 从 out 拿帧
    is->in_audio_filter = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    av_bprint_finalize(&bp, NULL);
    return ret;
}

//和音频版几乎一样，只是建的是"视频滤镜工厂"——处理的是 AVFrame 图像。
int configure_video_filters(AVFilterGraph* graph, VideoState* is, const char* vfilters, AVFrame* frame)
{
    enum AVPixelFormat pix_fmts[1]; // FF_ARRAY_ELEMS(sdl_texture_format_map)
    // char sws_flags_str[512] = "";
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = nullptr, *filt_out = nullptr,
                    *last_filter = nullptr;
    AVCodecParameters* codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, nullptr);
    // const AVDictionaryEntry* e = nullptr;
    int nb_pix_fmts = 0;

    /*
  int i, j;
  for (i = 0; i < renderer_info.num_texture_formats; i++) {
          for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
                  if (renderer_info.texture_formats[i] ==
  sdl_texture_format_map[j].texture_fmt) { pix_fmts[nb_pix_fmts++] =
  sdl_texture_format_map[j].format; break;
                  }
          }
  }*/
    pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;

    /*while ((e = av_dict_get(sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
          if (!strcmp(e->key, "sws_flags")) {
                  av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:",
  "flags", e->value);
          }
          else
                  av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:",
  e->key, e->value);
  }
  if (strlen(sws_flags_str))
          sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

  graph->scale_sws_opts = av_strdup(sws_flags_str);*/

    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format,
             is->video_st->time_base.num, is->video_st->time_base.den,
             codecpar->sample_aspect_ratio.num,
             FFMAX(codecpar->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d",
                    fr.num, fr.den);

    if ((ret = avfilter_graph_create_filter(
             &filt_src, avfilter_get_by_name("buffer"), "ffplay_buffer",
             buffersrc_args, nullptr, graph)) < 0)
        goto fail;

    ret = avfilter_graph_create_filter(
        &filt_out, avfilter_get_by_name("buffersink"), "ffplay_buffersink",
        nullptr, nullptr, graph);
    if (ret < 0)
        goto fail;

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts,
                                   AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto fail;

    last_filter = filt_out;

#if 0
	/* Note: this macro adds a filter before the lastly added filter, so the
	 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg)                                                    \
    do                                                                            \
    {                                                                             \
        AVFilterContext* filt_ctx;                                                \
                                                                                  \
        ret = avfilter_graph_create_filter(&filt_ctx, avfilter_get_by_name(name), \
                                           "ffplay_" name, arg, nullptr, graph);  \
        if (ret < 0)                                                              \
            goto fail;                                                            \
                                                                                  \
        ret = avfilter_link(filt_ctx, 0, last_filter, 0);                         \
        if (ret < 0)                                                              \
            goto fail;                                                            \
                                                                                  \
        last_filter = filt_ctx;                                                   \
    } while (0)

	if (autorotate) {
		int32_t* displaymatrix = (int32_t*)av_stream_get_side_data(is->video_st, AV_PKT_DATA_DISPLAYMATRIX, nullptr);
		double theta = get_rotation(displaymatrix);

		if (fabs(theta - 90) < 1.0) {
			INSERT_FILT("transpose", "clock");
		}
		else if (fabs(theta - 180) < 1.0) {
			INSERT_FILT("hflip", nullptr);
			INSERT_FILT("vflip", nullptr);
		}
		else if (fabs(theta - 270) < 1.0) {
			INSERT_FILT("transpose", "cclock");
		}
		else if (fabs(theta) > 1.0) {
			char rotate_buf[64];
			snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
			INSERT_FILT("rotate", rotate_buf);
		}
	}
#endif

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    is->in_video_filter = filt_src;
    is->out_video_filter = filt_out;

fail:
    return ret;
}

#if 0
int audio_open(void* opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams* audio_hw_params)
{
	//SDL_AudioSpec wanted_spec, spec;
	//const char* env;
	static const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
	static const int next_sample_rates[] = { 0, 44100, 48000, 96000, 192000 };
	//int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

	/*env = SDL_getenv("SDL_AUDIO_CHANNELS");
	if (env) {
		wanted_nb_channels = atoi(env);
		wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
	}*/

	if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
		wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
		wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
	}

	wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
	/*
	wanted_spec.channels = wanted_nb_channels;
	wanted_spec.freq = wanted_sample_rate;
	if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
		av_log(nullptr, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
		return -1;
	}
	while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
		next_sample_rate_idx--;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.silence = 0;
	wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
	wanted_spec.callback = sdl_audio_callback;
	wanted_spec.userdata = opaque;
	while (!(audio_dev = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
		av_log(nullptr, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
			wanted_spec.channels, wanted_spec.freq, SDL_GetError());
		wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
		if (!wanted_spec.channels) {
			wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
			wanted_spec.channels = wanted_nb_channels;
			if (!wanted_spec.freq) {
				av_log(nullptr, AV_LOG_ERROR,
					"No more combinations to try, audio open failed\n");
				return -1;
			}
		}
		wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
	}
	if (spec.format != AUDIO_S16SYS) {
		av_log(nullptr, AV_LOG_ERROR,
			"SDL advised audio format %d is not supported!\n", spec.format);
		return -1;
	}
	if (spec.channels != wanted_spec.channels) {
		wanted_channel_layout = av_get_default_channel_layout(spec.channels);
		if (!wanted_channel_layout) {
			av_log(nullptr, AV_LOG_ERROR,
				"SDL advised channel count %d is not supported!\n", spec.channels);
			return -1;
		}
	}*/

	audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
	audio_hw_params->freq = wanted_sample_rate; // spec.freq;
	audio_hw_params->channel_layout.nb_channels = wanted_channel_layout;
	audio_hw_params->channels = wanted_nb_channels; // spec.channels;

	audio_hw_params->frame_size = av_samples_get_buffer_size(nullptr, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
	audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(nullptr, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
	if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
		av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
		return -1;
	}
	return 0;// spec.size;
}
#endif

#endif
