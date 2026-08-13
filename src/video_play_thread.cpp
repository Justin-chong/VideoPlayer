// ***********************************************************/
// video_play_thread.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 视频播放线程实现（流水线的第三站 - 视频部分）
// 包含最核心的音视频同步逻辑（基于 PTS/DTS）以及字幕处理
// ***********************************************************/

#include "video_play_thread.h"

// 外部全局变量（在 mainwindow.cpp 定义），是否允许丢帧
extern int framedrop;

// ASS 字幕格式里"样式代码"用 {...} 包裹，这里用正则去掉
const QRegularExpression VideoPlayThread::m_assFilter = QRegularExpression("{\\\\.*?}");
// ASS 字幕里 \\N 或 \\n 表示换行
const QRegularExpression VideoPlayThread::m_assNewLineReplacer = QRegularExpression("\\\\n|\\\\N");

VideoPlayThread::VideoPlayThread(QObject* parent, VideoState* pState)
    : QThread(parent), m_pState(pState)
{
}

VideoPlayThread::~VideoPlayThread()
{
    stop_thread();
    final_resample_param();
}

void VideoPlayThread::run()
{
    // 父窗口必须有效，否则 run 没意义
    assert(m_pState);
    VideoState* is = m_pState;
    // remaining_time 是"还差多久该刷下一帧"，单位秒
    // 视频播放本质就是"按固定节奏刷新画面"
    double remaining_time = 0.0;

    for (;;)
    {
        // 1. 退出请求
        if (m_bExitThread)
            break;
        if (is->abort_request)
            break;

        // 2. 暂停时不要刷新画面，但也不能忙等
        if (is->paused)
        {
            msleep(10);  // 10ms 后再检查（避免 100% CPU）
            continue;
        }

        // 3. ★ 关键：等到该刷下一帧的时刻
        // video_refresh 里会算"距离下一帧还差多久"
        // 这里用 av_usleep 精确微秒级 sleep，比 msleep 更准
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));

        // 默认刷新周期（约 10ms，对应 100fps 的尝试频率）
        // 真正显示频率由 video_refresh 里根据 PTS 算出，REFRESH_RATE 只是上限
        remaining_time = REFRESH_RATE;
        // 4. 调 video_refresh 做实际工作：
        //    取一帧 → 算同步延迟 → 推到 UI 显示
        if ((!is->paused || is->force_refresh))
            video_refresh(is, &remaining_time);
    }

    qDebug("-------- Video play thread exit.");
}

// 计算下一帧应该什么时候显示（同步逻辑的核心）
void VideoPlayThread::video_refresh(VideoState* is, double* remaining_time)
{
    double time;
    Frame *sp, *sp2;

    // 用外部时钟做主时钟（且是实时流）时，要动态调速保证 buffer 稳定
    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        check_external_clock_speed(is);

    if (is->video_st)
    {
    retry:  // ★ 用 goto 实现"丢帧后重试"的简洁循环（避免深层嵌套）
        // 队列空：没东西可显示，先 sleep
        if (frame_queue_nb_remaining(&is->pictq) == 0)
        {
            // nothing to do, no picture to display in the queue
            // ★ 没帧时把 remaining_time 设成 REFRESH_RATE，让外层 sleep 10ms 再来
            *remaining_time = REFRESH_RATE;
        }
        else
        {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            // peek_last：上一帧（用于算 duration）
            // peek：当前要显示的帧
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            // serial 不匹配说明是 seek 后的旧帧，丢掉重试
            if (vp->serial != is->videoq.serial)
            {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            // serial 变了（新一轮播放开始），重置 frame_timer
            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;  // ★ 暂停时跳过同步计算，直接走 display（保持显示当前帧）

            /* compute nominal last_duration */
            // last_duration：上一帧的显示时长（用来推算下一帧该啥时候显示）
            last_duration = vp_duration(is, lastvp, vp);
            // delay：根据音视频同步算"这一帧应该延迟多久再显示"
            delay = compute_target_delay(last_duration, is);

            // 当前时间 < frame_timer+delay：还没到显示时间，再睡一会
            // ★ FFMIN 取"同步要求的等待"和"外层默认 10ms"的较小值，避免睡眠过长
            time = av_gettime_relative() / 1000000.0;
            if (time < is->frame_timer + delay)
            {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            // 推进 frame_timer
            is->frame_timer += delay;
            // 超过阈值则重置（防止长时间累积导致漂移）
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;

            // ★ 更新视频时钟（用当前帧的 pts）
            is->pictq.mutex->lock();
            if (!isnan(vp->pts))
                update_video_pts(is, vp->pts, vp->pos, vp->serial);
            is->pictq.mutex->unlock();

            // ★ 丢帧判断：
            //   如果当前帧显示完时，下一帧的 PTS 已经过了
            //   说明视频已经"落后"于音频了，要丢掉当前帧追上去
            if (frame_queue_nb_remaining(&is->pictq) > 1)
            {
                Frame* nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp);
                if (!is->step &&
                    (framedrop > 0 ||
                     (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) &&
                    time > is->frame_timer + duration)
                {
                    is->frame_drops_late++;
                    frame_queue_next(&is->pictq);  // 丢当前帧
                    goto retry;                     // 重试下一帧
                }
            }

            // ★ 字幕过期处理：检查字幕队列，丢过期的字幕
            if (is->subtitle_st)
            {
                while (frame_queue_nb_remaining(&is->subpq) > 0)
                {
                    sp = frame_queue_peek(&is->subpq);

                    if (frame_queue_nb_remaining(&is->subpq) > 1)
                        sp2 = frame_queue_peek_next(&is->subpq);
                    else
                        sp2 = nullptr;

                    // 字幕过期条件（任一即可）：
                    //   - serial 变了（新时间点）
                    //   - 视频时钟已经超过字幕的结束时间
                    //   - 下一条字幕的起始时间也过了
                    if (sp->serial != is->subtitleq.serial ||
                        (is->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000))) ||
                        (sp2 && is->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))
                    {
#if 0
						if (sp->uploaded) {
							int i;
							for (i = 0; i < sp->sub.num_rects; i++) {
								AVSubtitleRect* sub_rect = sp->sub.rects[i];

								/*uint8_t* pixels;
								int pitch, j;

								if (!SDL_LockTexture(is->sub_texture, (SDL_Rect*)sub_rect, (void**)&pixels, &pitch)) {
									for (j = 0; j < sub_rect->h; j++, pixels += pitch)
										memset(pixels, 0, sub_rect->w << 2);
									SDL_UnlockTexture(is->sub_texture);
								}*/
							}
						}
#endif
                        // 丢过期字幕
                        frame_queue_next(&is->subpq);
                    }
                    else
                    {
                        break;
                    }
                }
            }

            // 推进视频帧队列（这一帧播放完了）
            frame_queue_next(&is->pictq);
            // 标记"需要重绘"
            is->force_refresh = 1;

            // step 模式：单帧步进，播完一帧就暂停
            if (is->step && !is->paused)
                toggle_pause(is, !is->step);
        }

    display:
        /* display picture */
        // 实际显示：调 video_display -> video_image_display
        // ★ force_refresh：seek 后或显示完一帧时设为 1，强制重画
        // ★ rindex_shown：rindex 是"已显示的槽位"，只有它走到 valid 位置才需要显示
        if (is->force_refresh && is->pictq.rindex_shown)
            video_display(is);
    }

    is->force_refresh = 0;
}

/**
 * @brief 视频显示入口：决定走"音视频频谱显示"还是"图片显示"
 *        当前默认只走图片显示（video_image_display）
 */
void VideoPlayThread::video_display(VideoState* is)
{
    if (is->audio_st && false)
    {
        // video_audio_display(is);  // 频谱图模式，暂未启用
    }
    else if (is->video_st)
    {
        // 普通视频帧显示：YUV -> RGB -> QImage -> 发给主窗口
        video_image_display(is);
    }
}

#if 0
void VideoPlayThread::video_audio_display(VideoState* s)
{
	int64_t audio_callback_time = 0;
	int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
	int ch, channels, h, h2;
	int64_t time_diff;
	int rdft_bits, nb_freq;

	for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++)
		;
	nb_freq = 1 << (rdft_bits - 1);

	/* compute display index : center on currently output samples */
	channels = s->audio_tgt.channels;
	nb_display_channels = channels;
	if (!s->paused) {
		int data_used = (2 * nb_freq);
		n = 2 * channels;
		delay = s->audio_write_buf_size;
		delay /= n;

		/* to be more precise, we take into account the time spent since
		   the last buffer computation */
		if (audio_callback_time) {
			time_diff = av_gettime_relative() - audio_callback_time;
			delay -= (time_diff * s->audio_tgt.freq) / 1000000;
		}

		delay += 2 * data_used;
		if (delay < data_used)
			delay = data_used;

		i_start = x = compute_mod(s->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
		/*if (s->show_mode == SHOW_MODE_WAVES) {
			h = INT_MIN;
			for (i = 0; i < 1000; i += channels) {
				int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
				int a = s->sample_array[idx];
				int b = s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
				int c = s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
				int d = s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
				int score = a - d;
				if (h < score && (b ^ c) < 0) {
					h = score;
					i_start = idx;
				}
			}
		}*/

		s->last_i_start = i_start;
	}
	else {
		i_start = s->last_i_start;
	}

#if 0
	if (s->show_mode == SHOW_MODE_WAVES) {
		SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

		/* total height for one channel */
		h = s->height / nb_display_channels;
		/* graph height / 2 */
		h2 = (h * 9) / 20;
		for (ch = 0; ch < nb_display_channels; ch++) {
			i = i_start + ch;
			y1 = s->ytop + ch * h + (h / 2); /* position of center line */
			for (x = 0; x < s->width; x++) {
				y = (s->sample_array[i] * h2) >> 15;
				if (y < 0) {
					y = -y;
					ys = y1 - y;
				}
				else {
					ys = y1;
				}
				fill_rectangle(s->xleft + x, ys, 1, y);
				i += channels;
				if (i >= SAMPLE_ARRAY_SIZE)
					i -= SAMPLE_ARRAY_SIZE;
			}
		}

		SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);

		for (ch = 1; ch < nb_display_channels; ch++) {
			y = s->ytop + ch * h;
			fill_rectangle(s->xleft, y, s->width, 1);
		}
	}
	else {
		if (realloc_texture(&s->vis_texture, SDL_PIXELFORMAT_ARGB8888, s->width, s->height, SDL_BLENDMODE_NONE, 1) < 0)
			return;

		if (s->xpos >= s->width)
			s->xpos = 0;
		nb_display_channels = FFMIN(nb_display_channels, 2);
		if (rdft_bits != s->rdft_bits) {
			av_rdft_end(s->rdft);
			av_free(s->rdft_data);
			s->rdft = av_rdft_init(rdft_bits, DFT_R2C);
			s->rdft_bits = rdft_bits;
			s->rdft_data = av_malloc_array(nb_freq, 4 * sizeof(*s->rdft_data));
		}
		if (!s->rdft || !s->rdft_data) {
			av_log(nullptr, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
			s->show_mode = SHOW_MODE_WAVES;
		}
		else {
			FFTSample* data[2];
			SDL_Rect rect = { .x = s->xpos, .y = 0, .w = 1, .h = s->height };
			uint32_t* pixels;
			int pitch;
			for (ch = 0; ch < nb_display_channels; ch++) {
				data[ch] = s->rdft_data + 2 * nb_freq * ch;
				i = i_start + ch;
				for (x = 0; x < 2 * nb_freq; x++) {
					double w = (x - nb_freq) * (1.0 / nb_freq);
					data[ch][x] = s->sample_array[i] * (1.0 - w * w);
					i += channels;
					if (i >= SAMPLE_ARRAY_SIZE)
						i -= SAMPLE_ARRAY_SIZE;
				}
				av_rdft_calc(s->rdft, data[ch]);
			}
			/* Least efficient way to do this, we should of course
			 * directly access it but it is more than fast enough. */
			if (!SDL_LockTexture(s->vis_texture, &rect, (void**)&pixels, &pitch)) {
				pitch >>= 2;
				pixels += pitch * s->height;
				for (y = 0; y < s->height; y++) {
					double w = 1 / sqrt(nb_freq);
					int a = sqrt(w * sqrt(data[0][2 * y + 0] * data[0][2 * y + 0] + data[0][2 * y + 1] * data[0][2 * y + 1]));
					int b = (nb_display_channels == 2) ? sqrt(w * hypot(data[1][2 * y + 0], data[1][2 * y + 1]))
						: a;
					a = FFMIN(a, 255);
					b = FFMIN(b, 255);
					pixels -= pitch;
					*pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
				}
				SDL_UnlockTexture(s->vis_texture);
			}
			SDL_RenderCopy(renderer, s->vis_texture, nullptr, nullptr);
		}
		if (!s->paused)
			s->xpos++;
	}
#endif
}
#endif

/**
 * @brief 把一帧视频从 YUV 转换为 QImage（RGB888），并通过 frame_ready 信号发给主窗口
 *
 * 处理流程：
 *   1. 看字幕队列里有没有当前帧需要显示的字幕
 *      - ASS 字幕：解析文本后通过 subtitle_ready 信号发给主窗口
 *      - 图形字幕（暂未启用）：sws_scale 转成 BGRA 贴上去
 *   2. 取出当前视频帧 vp，sws_scale 把它从原始格式转成 RGB24
 *   3. 把 RGB24 数据按行拷到 QImage
 *   4. emit frame_ready(img) —— 主窗口会把它贴到 VideoLabel 上
 *
 * @param is 播放器全局状态（含视频/字幕队列）
 */
void VideoPlayThread::video_image_display(VideoState* is)
{
    Frame* sp = nullptr;        // 字幕帧
    Frame* vp = frame_queue_peek_last(&is->pictq);  // 当前要显示的视频帧
    Video_Resample* pResample = &m_Resample;

    // ---- 字幕处理 ----
    if (frame_queue_nb_remaining(&is->subpq) > 0)
    {
        sp = frame_queue_peek(&is->subpq);

        // 视频时钟已"过"字幕开始时间，说明这条字幕该显示了
        if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000))
        {
            if (!sp->uploaded)
            {
                if (!sp->width || !sp->height)
                {
                    // 字幕帧没带尺寸，借用视频帧的
                    sp->width = vp->width;
                    sp->height = vp->height;
                }

#if 1
                // 当前只支持 ASS 文本字幕
                for (unsigned int i = 0; i < sp->sub.num_rects; i++)
                {
                    AVSubtitleRect* sub_rect = sp->sub.rects[i];
                    if (sub_rect->type == SUBTITLE_ASS)
                    {
                        // ASS 字幕格式举例：Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,你好\\N世界
                        // 用逗号切分后，第 9 段（index 8）就是真正的文本
                        QString ass = QString::fromLocal8Bit(QString::fromStdString(sub_rect->ass).toUtf8());
                        QStringList assList = ass.split(",");
                        if (assList.size() > 8)
                        {
                            ass = assList[8];
                            parse_subtitle_ass(ass);
                        }
                    }
                    else
                    {
                        // 图形字幕（如 PGS）暂未实现
                        qWarning("not handled yet, type:%d", sub_rect->type);
                    }
                }
#else
                // 图形字幕路径，sws_scale PAL8 -> BGRA（已注释）
#endif
                sp->uploaded = 1;
            }
        }
        else
        {
            // 还没到字幕显示时间，这一帧不显示字幕
            sp = nullptr;
        }
    }

    // ---- 视频帧转 QImage ----
    AVFrame* pFrameRGB = pResample->pFrameRGB;        // sws_scale 的目标帧（RGB）
    AVCodecContext* pVideoCtx = is->viddec.avctx;
    AVFrame* pFrame = vp->frame;                       // 解码出来的原始帧

    // ★ 用缩放后尺寸（4K 自动降到 1920×1080，避免主线程渲染卡顿）
    const int out_w = pResample->dst_width  > 0 ? pResample->dst_width  : pVideoCtx->width;
    const int out_h = pResample->dst_height > 0 ? pResample->dst_height : pVideoCtx->height;

    // 把任意像素格式（YUV420P/NV12/...）转换为 RGB24（可缩放）
    // ★ sws_scale 是高度优化的色彩空间转换库，内部用 SIMD 指令加速
    //    传 0 作为 sliceY 表示处理整帧，返回值是输出行高
    // ★ 源/目标尺寸都用 ctx 的尺寸（sws_ctx 在 init 时已经按这两个尺寸配好转换系数）
    sws_scale(pResample->sws_ctx, (uint8_t const* const*)pFrame->data, pFrame->linesize, 0,
              pVideoCtx->height, pFrameRGB->data, pFrameRGB->linesize);

    // 把 RGB24 buffer 拷到 QImage（一行一行拷，因为 linesize 可能对齐）
    // ★ QImage::Format_RGB888 是按 3 字节像素存的，但 FFmpeg 的 linesize 可能有 padding 对齐
    //    所以必须按行 memcpy，不能整块 memcpy（否则会把对齐 padding 也拷进去）
    // ★ QImage 用缩放后尺寸（out_w × out_h），不是 4K 原尺寸
    QImage img(out_w, out_h, QImage::Format_RGB888);
    for (int y = 0; y < out_h; ++y)
    {
        memcpy(img.scanLine(y), pFrameRGB->data[0] + y * pFrameRGB->linesize[0], out_w * 3);
    }

    // 通知主窗口：有一帧新画面
    // ★ 跨线程信号，由 Qt::QueuedConnection 投递到主线程的 VideoLabel 上贴图
    emit frame_ready(img);
}

/**
 * @brief 初始化 sws_scale 重采样参数 + 分配 RGB 输出 buffer
 *
 * - 源格式：硬件解码时统一当作 NV12（GPU 出来的格式），软解时用 pVideo->pix_fmt
 * - 目标格式：RGB24（QImage 最常吃的格式）
 * - 同时分配一个 pFrameRGB 和 buffer_RGB 用来存转换结果
 *
 * @param pVideo   视频解码器上下文
 * @param bHardware 是否硬件解码（DXVA2 出来是 NV12）
 * @return true=成功
 */
bool VideoPlayThread::init_resample_param(AVCodecContext* pVideo, bool bHardware)
{
    Video_Resample* pResample = &m_Resample;
    if (pVideo)
    {
        enum AVPixelFormat pix_fmt = pVideo->pix_fmt; // 软解的原始格式
        // ★ 硬解（DXVA2/VAAPI 等）出来的帧统一是 NV12 格式
        //    即使解码器 ctx 里的 pix_fmt 是别的，硬解路径下也以 NV12 为准
        if (bHardware)
            pix_fmt = AV_PIX_FMT_NV12;                // 硬解出来统一是 NV12

        // ★★★ 关键优化：4K 视频在主线程渲染会卡顿（QImage copy + QPixmap 上传要 1-2 秒）
        //   解决：4K 视频缩放到 1920×1080 显示，10MB → 6MB，主线程渲染快 3-4 倍
        //   用户体验：流畅播放，画质略降（但 4K 缩到 1080p 在普通屏幕看不出差别）
        // ★ 输出尺寸计算：宽超过 1920 时按比例缩放
        int src_w = pVideo->width;
        int src_h = pVideo->height;
        int dst_w = src_w;
        int dst_h = src_h;
        const int kMaxDisplayWidth = 1920;  // 1920 宽度上限（1080p 横向分辨率）
        if (dst_w > kMaxDisplayWidth)
        {
            dst_w = kMaxDisplayWidth;
            // 按比例缩放高度（保持宽高比）
            dst_h = (int)((double)src_h * dst_w / src_w);
            // 高度取偶数（YUV/RGB 通常要求偶数）
            dst_h = (dst_h + 1) & ~1;
        }
        qInfo("[init_resample_param] src=%dx%d, dst=%dx%d, hw=%d, fmt=%d",
              src_w, src_h, dst_w, dst_h, bHardware, pix_fmt);

        // 创建 sws 上下文：源 -> RGB24（可缩放）
        // ★ sws_getContext 会缓存转换系数，多次调用同一参数很高效
        //    SWS_BILINEAR 双线性插值，质量足够，速度比 BICUBIC 快
        struct SwsContext* sws_ctx = sws_getContext(src_w, src_h,
                                                    pix_fmt, // AV_PIX_FMT_YUV420P
                                                    dst_w, dst_h,
                                                    AV_PIX_FMT_RGB24, // sws_scale destination color scheme
                                                    SWS_BILINEAR, nullptr, nullptr, nullptr);

        AVFrame* pFrameRGB = av_frame_alloc();
        if (!pFrameRGB)
        {
            printf("Could not allocate rgb frame.\n");
            return false;
        }

        // ★ 32 是字节对齐值（保证 linesize 是 32 的倍数，SIMD 优化用）
        //    缓冲区按缩放后尺寸分配
        int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, dst_w, dst_h, 32);
        uint8_t* const buffer_RGB = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
        if (!buffer_RGB)
        {
            printf("Could not allocate buffer.\n");
            return false;
        }

        // 把 buffer_RGB 绑到 pFrameRGB 的 data/linesize 上
        // ★ av_image_fill_arrays 不复制数据，只是设置指针，buffer_RGB 才是真正存数据的地方
        //    pFrameRGB 只是个"壳"，方便传给 sws_scale
        av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer_RGB, AV_PIX_FMT_RGB24, dst_w, dst_h, 32);

        pResample->sws_ctx = sws_ctx;
        pResample->pFrameRGB = pFrameRGB;
        pResample->buffer_RGB = buffer_RGB;
        // ★ 存缩放后尺寸，给 sws_scale 调用和 QImage 创建用
        pResample->dst_width = dst_w;
        pResample->dst_height = dst_h;
        return true;
    }
    return false;
}

/**
 * @brief 释放 init_resample_param 分配的所有资源
 *        析构时调用，避免内存泄漏
 */
void VideoPlayThread::final_resample_param()
{
    Video_Resample* pResample = &m_Resample;
    // Free video resample context
    sws_freeContext(pResample->sws_ctx);

    // Free the RGB image
    av_free(pResample->buffer_RGB);
    av_frame_free(&pResample->pFrameRGB);
    av_free(pResample->pFrameRGB);
}

/**
 * @brief 安全停止线程：置退出标志 + 等待 run() 退出
 */
void VideoPlayThread::stop_thread()
{
    m_bExitThread = true;
    wait();
}

/**
 * @brief 解析 ASS 字幕文本，去掉样式代码、转换换行符
 * 然后通过 subtitle_ready 信号发给主窗口
 *
 * ASS 里会有很多 {\b1} {\fs20} 这种样式代码，用正则一次性删除
 * \\N / \\n 表示换行，转成 \n
 *
 * @param text 原始 ASS 文本（含样式代码）
 */
void VideoPlayThread::parse_subtitle_ass(const QString& text)
{
    QString str = text;

    str.remove(m_assFilter);                          // 去掉 {xxx} 样式
    str.replace(m_assNewLineReplacer, "\n");          // \\N 换行
    str = str.trimmed();

    emit subtitle_ready(str);
}
