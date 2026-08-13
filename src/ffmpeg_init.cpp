// ***********************************************************/
// ffmpeg_init.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// FFmpeg 工具函数实现
// ***********************************************************/

#include "ffmpeg_init.h"

// Debug 模式下打开 FFmpeg 内部日志
#if !NDEBUG
#define OPEN_FFMPEG_LOG 1
#else
#define OPEN_FFMPEG_LOG 0
#endif

// 打印单个 FFmpeg 库版本信息的宏
// 输出格式：libavutil    xx.xxx.xxx / xx.xxx.xxx
#define PRINT_LIB_INFO(libname, LIBNAME)                                  \
    if (true)                                                             \
    {                                                                     \
        const char* indent = "  ";                                        \
        unsigned int version = libname##_version();                       \
        qInfo("%slib%-11s %2d.%3d.%3d / %2d.%3d.%3d", indent, #libname,   \
              LIB##LIBNAME##_VERSION_MAJOR, LIB##LIBNAME##_VERSION_MINOR, \
              LIB##LIBNAME##_VERSION_MICRO, AV_VERSION_MAJOR(version),    \
              AV_VERSION_MINOR(version), AV_VERSION_MICRO(version));      \
    }

#define BUFF_MAXLEN 256

/**
 * @brief FFmpeg 的日志回调
 * 把 av_log 的输出格式化成字符串，然后转发到 qInfo
 *
 * ★ 为什么要写这个回调？
 *   FFmpeg 默认把日志打到 stderr，会和 Qt 的消息混在一起。
 *   自定义回调后，FFmpeg 的日志会走 Qt 通道，统一格式输出。
 *
 * ★ 为什么用 va_copy？
 *   av_log_format_line 会消费 va_list，但 fmt 还可能再次被用
 *   （FFmpeg 会复用 fmt）。先 copy 一份给它，避免原 va_list 被破坏。
 *
 * @param ptr   FFmpeg 内部上下文（一般不用）
 * @param level 日志级别
 * @param fmt   格式化字符串
 * @param vl    可变参数列表
 */
static void log_callback(void* ptr, int level, const char* fmt, va_list vl)
{
    // ★ 等级过滤：比当前全局级别还低的直接丢，少打无用日志
    if (level > av_log_get_level())
        return;

    va_list vl2;
    char line[1024];
    static int print_prefix = 1;

    // ★ va_copy 保护原始 vl，让 av_log_format_line 用副本
    va_copy(vl2, vl);
    av_log_format_line(ptr, level, fmt, vl2, line, sizeof(line), &print_prefix);
    va_end(vl2);

#if OPEN_FFMPEG_LOG
    // ★ 调试期才打 FFMPEG 标签，便于在日志里区分来源
    qInfo("FFMPEG:%s", line);
#endif
}

/**
 * @brief 初始化 FFmpeg
 * 1. 设置日志回调
 * 2. 设置日志级别为 INFO
 * 3. 启用"跳过重复日志"选项
 *
 * ★ 为什么要设 AV_LOG_SKIP_REPEATED？
 *   FFmpeg 在解码循环里经常产生一模一样的日志，
 *   开启这个标志后，连续相同的日志会被去重，避免刷屏。
 *
 * ★ 为什么日志级别设 INFO？
 *   DEBUG 太啰嗦，WARNING 又看不到详细信息。
 *   INFO 是个平衡点：能看见关键流程，又不刷屏。
 *
 * @return 固定返回 0（占位，预留扩展）
 */
int ffmpeg_init()
{
    // ★ 开启"跳过重复日志"，避免同一条日志连刷 N 行
    av_log_set_flags(AV_LOG_SKIP_REPEATED);

    av_log_set_level(AV_LOG_INFO);
    // ★ 装上自定义回调，让 FFmpeg 日志走 Qt 通道
    av_log_set_callback(log_callback);

    // print_ffmpeg_info(AV_LOG_INFO);
    // ★ check_error(-22) 是为了演示 av_strerror 能解码出 EINVAL
    check_error(-22);
    return 0;
}

/**
 * @brief 把 FFmpeg 错误码转成可读字符串（调试用）
 *
 * ★ 怎么把负数变成可读字符串？
 *   FFmpeg 用负数表示错误（最高位置 1），需要 av_strerror 反查。
 *   这一步一般只在出错分支调用，不影响主流程性能。
 *
 * @param error FFmpeg 错误码（通常是 < 0 的数）
 */
void check_error(int error)
{
    char errorStr[256] = {0};
    // ★ 几个经典错误码的对照，打印出来方便后面比较
    qDebug("ENOMEM: %d", AVERROR(ENOMEM));
    qDebug("ENOMEM: %d", AVERROR(EINVAL));
    qDebug("ENOMEM: %d", AVERROR_OPTION_NOT_FOUND);
    // ★ av_strerror 会把 -22 翻译成 "Invalid argument"
    av_strerror(error, errorStr, sizeof(errorStr));
}

/**
 * @brief 打印 FFmpeg 各库的版本信息
 * 输出到 qInfo
 *
 * ★ 为什么要打版本？
 *   不同版本 FFmpeg 的 API 行为可能不一样。
 *   启动时打出来，出问题方便对照是不是版本差异。
 *
 * ★ PRINT_LIB_INFO 宏怎么用？
 *   第一个参数是库名（avutil），第二个是宏名前缀（AVUTIL），
 *   宏会自动去查 libxxx_version() 和 LIBxxx_VERSION_xxx。
 */
void print_ffmpeg_info()
{
    // ★ 先打 ffmpeg 整体版本（FFMPEG_VERSION 是宏，定义在 version.h）
    qInfo("%s version %s", "ffmpeg", FFMPEG_VERSION);

    // ★ 依次打各子库版本
    PRINT_LIB_INFO(avutil, AVUTIL);      // 通用工具
    PRINT_LIB_INFO(avcodec, AVCODEC);    // 编解码
    PRINT_LIB_INFO(avformat, AVFORMAT);  // 容器格式
    // PRINT_LIB_INFO(avdevice, AVDEVICE);  // 设备（用不到）
    // PRINT_LIB_INFO(avfilter, AVFILTER);  // 滤镜（用不到）
    PRINT_LIB_INFO(swscale, SWSCALE);    // 图像缩放/转换
    PRINT_LIB_INFO(swresample, SWRESAMPLE);  // 音频重采样
    qInfo("");
}

/**
 * @brief 把 AVFormatContext 格式化成可读字符串（参考 ffmpeg -i 的输出）
 *
 * 输出大致分 7 段：
 *   1. 文件头（Input/Output #0, 格式, 'url':）
 *   2. metadata（标题、艺术家等）
 *   3. 时长、起始时间、比特率
 *   4. 章节（DVD/Blu-ray）
 *   5. 节目（多节目流，如 MPEG-TS）
 *   6. 流列表（音视频/字幕）
 *   7. 剩余的流（没被节目包含的）
 *
 * ★ 为什么要自己实现一份 dump_format？
 *   FFmpeg 自带的 av_dump_format 只能打印到 stderr，
 *   自己实现的版本可以拼到 QString 里，方便在 UI 上展示。
 *
 * ★ printed[] 数组的用途？
 *   program 里的流打印过要打 1，避免在第 6 段重复打印。
 *
 * @param ic        格式上下文（要解封装或封装的目标）
 * @param index     文件编号（用于 "Input #0" 那种显示）
 * @param url       文件路径或网络 URL
 * @param is_output 0=输入 / 1=输出，决定走 oformat 还是 iformat
 * @return 拼好的多行字符串；出错时返回空串
 */
QString dump_format(AVFormatContext* ic, int index, const char* url, int is_output)
{
    QString str;
    char tmp[BUFF_MAXLEN];
    const char* indent = "  ";
    // printed[i] 标记第 i 路流是否已经被打印过（避免 program 内重复打）
    uint8_t* printed = nullptr;

    // 防御：参数检查
    if (!ic)
    {
        qErrnoWarning("invalid parameter!");
        goto fail;
    }

    if (!url || !url[0])
    {
        qErrnoWarning("url is invalid!");
        goto fail;
    }

    // 为每路流分配 1 字节做"是否已打印"标记
    printed = ic->nb_streams ? (uint8_t*)av_mallocz(ic->nb_streams) : nullptr;
    if (ic->nb_streams && !printed)
        goto fail;

    // ★ 第 1 段：文件头信息
    //  例：Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'test.mp4':
    snprintf(tmp, sizeof(tmp), "%s #%d, %s, %s '%s':",
             is_output ? "Output" : "Input", index,
             is_output ? ic->oformat->name : ic->iformat->name,
             is_output ? "to" : "from", url);
    str += tmp;
    str += "\n";

    // 打印全局 metadata（标题、艺术家等）
    str += dump_metadata(ic->metadata, indent);

    if (!is_output)
    {
        // ★ 第 2 段：时长
        // 格式 HH:MM:SS.cc（cc = 厘秒）
        snprintf(tmp, sizeof(tmp), "%sDuration: ", indent);
        str += tmp;

        if (ic->duration != AV_NOPTS_VALUE)
        {
            int64_t hours, mins, secs, us;
            // +5000 是为了四舍五入（5 毫秒）
            int64_t duration =
                ic->duration + (ic->duration <= INT64_MAX - 5000 ? 5000 : 0);
            secs = duration / AV_TIME_BASE;  // 总秒数
            us = duration % AV_TIME_BASE;    // 剩余微秒
            mins = secs / 60;                // 分
            secs %= 60;                      // 秒
            hours = mins / 60;               // 时
            mins %= 60;                      // 分

            snprintf(tmp, sizeof(tmp), "%02lld:%02lld:%02lld.%02lld", hours, mins,
                     secs, (100 * us) / AV_TIME_BASE);
            str += tmp;
        }
        else
        {
            // 直播流等没有总时长
            str += "N/A";
        }

        // ★ 第 3 段：起始时间
        if (ic->start_time != AV_NOPTS_VALUE)
        {
            int secs, us;
            secs = llabs(ic->start_time / AV_TIME_BASE);
            us = llabs(ic->start_time % AV_TIME_BASE);
            // av_log(nullptr, AV_LOG_INFO, ", start: %s%d.%06d",
            // ic->start_time >= 0 ? "" : "-",	secs, (int)av_rescale(us, 1000000,
            // AV_TIME_BASE));
            snprintf(tmp, sizeof(tmp), ", start: %s%d.%06d",
                     ic->start_time >= 0 ? "" : "-", secs,
                     (int)av_rescale(us, 1000000, AV_TIME_BASE));
            str += tmp;
        }

        // ★ 第 4 段：比特率
        str += ", bitrate: ";
        if (ic->bit_rate)
        {
            snprintf(tmp, sizeof(tmp), "%lld kb/s", ic->bit_rate / 1000);
            str += tmp;
        }
        else
            str += "N/A";

        str += "\n";
    }

    // ★ 第 5 段：章节（DVD/Blu-ray 那种分章节的内容）
    if (ic->nb_chapters)
    {
        snprintf(tmp, sizeof(tmp), "%sChapters:\n", indent);
        str += tmp;
    }

    for (unsigned int i = 0; i < ic->nb_chapters; i++)
    {
        const AVChapter* ch = ic->chapters[i];
        snprintf(tmp, sizeof(tmp), "    Chapter #%d:%d: start %f, end %f\n", index,
                 i, ch->start * av_q2d(ch->time_base),
                 ch->end * av_q2d(ch->time_base));
        str += tmp;

        str += dump_metadata(ch->metadata, "      ");
    }

    // ★ 第 6 段：节目（Program，多节目流才用到，比如 MPEG-TS）
    if (ic->nb_programs)
    {
        unsigned int j, k, total = 0;
        for (j = 0; j < ic->nb_programs; j++)
        {
            const AVProgram* program = ic->programs[j];
            const AVDictionaryEntry* name =
                av_dict_get(program->metadata, "name", nullptr, 0);
            snprintf(tmp, sizeof(tmp), "  Program %d %s\n", program->id,
                     name ? name->value : "");
            str += tmp;

            str += dump_metadata(program->metadata, "    ");

            // 打印 program 里的流
            for (k = 0; k < program->nb_stream_indexes; k++)
            {
                str +=
                    dump_stream_format(ic, program->stream_index[k], index, is_output);
                printed[program->stream_index[k]] = 1;  // 标记已打
            }
            total += program->nb_stream_indexes;
        }
        if (total < ic->nb_streams)
        {
            str += "  No Program\n";
        }
    }

    str += "\n";
    // ★ 第 7 段：剩余的流（没被 program 包含的）
    for (unsigned int i = 0; i < ic->nb_streams; i++)
    {
        if (!printed[i])
        {
            str += dump_stream_format(ic, i, index, is_output);
            str += "\n";
        }
    }

    // 释放标记数组
    if (printed)
        av_free(printed);
    return str;

fail:
    if (printed)
        av_free(printed);
    return QString("");
}

/**
 * @brief 把 AVDictionary 格式化成可读字符串
 *
 * 输出格式：
 *   Metadata:
 *     key1          : value1
 *     key2          : value2（多行会换行对齐）
 *
 * ★ 为什么要单独判断 "language 字段只有 1 个"？
 *   大多数视频只有一个 language 标签，没啥信息量。
 *   这种情况下干脆不显示 "Metadata:" 头，让日志更紧凑。
 *
 * ★ 为什么要拆 \r\n？
 *   metadata 里有的字段是带换行的（比如 comment），
 *   不拆的话后面的行会贴在一起很难看。
 *
 * @param m      metadata 字典
 * @param indent 缩进字符串
 * @return 拼好的字符串（无内容时为空串）
 */
QString dump_metadata(const AVDictionary* m, const char* indent)
{
    QString str;
    char tmp[BUFF_MAXLEN];

    // 跳过单条 language 字段（特判：单独只有 language 时不显示"Metadata:"头）
    if (m && !(av_dict_count(m) == 1 && av_dict_get(m, "language", nullptr, 0)))
    {
        const AVDictionaryEntry* tag = nullptr;

        // 输出 "Metadata:" 标题
        snprintf(tmp, sizeof(tmp), "%sMetadata:\n", indent);
        str += tmp;

        // 遍历所有 metadata 字段
        // av_dict_get 第 4 个参数 AV_DICT_IGNORE_SUFFIX 让 "xx-yy" 和 "xx_zz" 都能匹配
        while ((tag = av_dict_get(m, "", tag, AV_DICT_IGNORE_SUFFIX)))
        {
            // 跳过 language 字段（已经在外层特殊处理）
            if (strcmp("language", tag->key))
            {
                const char* p = tag->value;
                // av_log(ctx, AV_LOG_INFO,"%s  %-16s: ", indent, tag->key);
                snprintf(tmp, sizeof(tmp), "%s  %-16s: ", indent, tag->key);
                str += tmp;

                // 处理多行字符串：metadata 可能含 \r\n，要拆开显示
                // 这些控制字符：\x8\xa\xb\xc\xd
                while (*p)
                {
                    char tmp_str[256];
                    // 找第一个控制字符的位置
                    size_t len = strcspn(p, "\x8\xa\xb\xc\xd");
                    av_strlcpy(tmp_str, p, FFMIN(sizeof(tmp_str), len + 1));
                    // str += "%s", tmp);
                    snprintf(tmp, sizeof(tmp), "%s", tmp_str);
                    str += tmp;

                    p += len;
                    if (*p == 0xd)  // \r：空格代替
                        str += " ";
                    if (*p == 0xa)  // \n：换行
                        str += "\n%s  %-16s: ";
                    if (*p)
                        p++;
                }
                str += "\n";
            }
        }
    }
    return str;
}

/**
 * @brief 把单路流（音/视频/字幕）格式化成可读字符串
 *
 * 输出格式示例：
 *   Stream #0:0[0x1](eng): Video: h264, 1920x1080, 30 fps, default
 *
 * ★ 为什么要先 alloc 一个临时 AVCodecContext？
 *   st->codecpar 只存了"解码器参数"（宽高、采样率等），
 *   没有解码器名字、profile 等信息。
 *   avcodec_parameters_to_context 把参数灌进一个完整 ctx，
 *   再调 avcodec_string 才能打出像 "h264 (High)" 这种完整描述。
 *
 * ★ 为什么要 free 那个临时 avctx？
 *   avcodec_alloc_context3 申请的内存必须手动 free，
 *   不然反复 dump 流会泄漏内存。
 *
 * @param ic        格式上下文
 * @param i         流索引
 * @param index     文件编号
 * @param is_output 0=输入 / 1=输出
 * @return 拼好的字符串；失败返回空串
 */
QString dump_stream_format(const AVFormatContext* ic, int i, int index, int is_output)
{
    QString str;
    char tmp[BUFF_MAXLEN];

    char buf[256];
    int flags = (is_output ? ic->oformat->flags : ic->iformat->flags);
    const AVStream* st = ic->streams[i];
    // const FFStream* const sti = cffstream(st);
    const AVDictionaryEntry* lang =
        av_dict_get(st->metadata, "language", nullptr, 0);
    const char* separator = (const char*)ic->dump_separator;
    AVCodecContext* avctx;
    // AVCodecContext* st_avctx = (AVCodecContext*)st->priv_data;
    int ret;

    avctx = avcodec_alloc_context3(nullptr);
    if (!avctx)
        goto fail;

    ret = avcodec_parameters_to_context(avctx, st->codecpar);
    if (ret < 0)
    {
        avcodec_free_context(&avctx);
        goto fail;
    }

    // Fields which are missing from AVCodecParameters need to be taken from the
    // AVCodecContext
    /*
      avctx->properties = st_avctx->properties;
      avctx->codec = st_avctx->codec;
      avctx->qmin = st_avctx->qmin;
      avctx->qmax = st_avctx->qmax;
      avctx->coded_width = st_avctx->coded_width;
      avctx->coded_height = st_avctx->coded_height;

      if (separator)
              av_opt_set(avctx, "dump_separator", separator, 0);*/
    avcodec_string(buf, sizeof(buf), avctx, is_output);
    avcodec_free_context(&avctx);

    snprintf(tmp, sizeof(tmp), "  Stream #%d:%d", index, i);
    str += tmp;

    if (flags & AVFMT_SHOW_IDS)
    {
        snprintf(tmp, sizeof(tmp), "[0x%x]", st->id);
        str += tmp;
    }

    if (lang)
    {
        snprintf(tmp, sizeof(tmp), "(%s)", lang->value);
        str += tmp;
    }

    snprintf(tmp, sizeof(tmp), ": %s", buf);
    str += tmp;

    if (st->sample_aspect_ratio.num &&
        av_cmp_q(st->sample_aspect_ratio, st->codecpar->sample_aspect_ratio))
    {
        AVRational display_aspect_ratio;
        av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
                  st->codecpar->width * (int64_t)st->sample_aspect_ratio.num,
                  st->codecpar->height * (int64_t)st->sample_aspect_ratio.den,
                  1024 * 1024);

        snprintf(tmp, sizeof(tmp), ", SAR %d:%d DAR %d:%d",
                 st->sample_aspect_ratio.num, st->sample_aspect_ratio.den,
                 display_aspect_ratio.num, display_aspect_ratio.den);
        str += tmp;
    }

    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
    {
        int fps = st->avg_frame_rate.den && st->avg_frame_rate.num;
        int tbr = st->r_frame_rate.den && st->r_frame_rate.num;
        int tbn = st->time_base.den && st->time_base.num;

        if (fps || tbr || tbn)
        {
            snprintf(tmp, sizeof(tmp), "%s", separator);
            str += tmp;
        }

        if (fps)
            str +=
                print_fps(av_q2d(st->avg_frame_rate), tbr || tbn ? "fps, " : "fps");
        if (tbr)
            str += print_fps(av_q2d(st->r_frame_rate), tbn ? "tbr, " : "tbr");
        if (tbn)
            str += print_fps(1 / av_q2d(st->time_base), "tbn");
    }

    if (st->disposition & AV_DISPOSITION_DEFAULT)
        str += " (default)";
    if (st->disposition & AV_DISPOSITION_DUB)
        str += " (dub)";
    if (st->disposition & AV_DISPOSITION_ORIGINAL)
        str += " (original)";
    if (st->disposition & AV_DISPOSITION_COMMENT)
        str += " (comment)";
    if (st->disposition & AV_DISPOSITION_LYRICS)
        str += " (lyrics)";
    if (st->disposition & AV_DISPOSITION_KARAOKE)
        str += " (karaoke)";
    if (st->disposition & AV_DISPOSITION_FORCED)
        str += " (forced)";
    if (st->disposition & AV_DISPOSITION_HEARING_IMPAIRED)
        str += " (hearing impaired)";
    if (st->disposition & AV_DISPOSITION_VISUAL_IMPAIRED)
        str += " (visual impaired)";
    if (st->disposition & AV_DISPOSITION_CLEAN_EFFECTS)
        str += " (clean effects)";
    if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
        str += " (attached pic)";
    if (st->disposition & AV_DISPOSITION_TIMED_THUMBNAILS)
        str += " (timed thumbnails)";
    if (st->disposition & AV_DISPOSITION_CAPTIONS)
        str += " (captions)";
    if (st->disposition & AV_DISPOSITION_DESCRIPTIONS)
        str += " (descriptions)";
    if (st->disposition & AV_DISPOSITION_METADATA)
        str += " (metadata)";
    if (st->disposition & AV_DISPOSITION_DEPENDENT)
        str += " (dependent)";
    if (st->disposition & AV_DISPOSITION_STILL_IMAGE)
        str += " (still image)";
    str += "\n";

    str += dump_metadata(st->metadata, "    ");
    str += dump_sidedata(st, "    ");
    return str;

fail:
    return QString("");
}

/**
 * @brief 格式化帧率数字（自动选最简洁的写法）
 *
 * 转换规则：
 *   - 整数 30      -> "30 fps"
 *   - 带小数 29.97 -> "29.97 fps"
 *   - 太大 60000   -> "60k fps"（除以 1000）
 *   - 极小 0.0001  -> "0.0001 fps"（保留 4 位小数）
 *
 * ★ 为什么要做这种处理？
 *   ffprobe 的输出是稳定的格式：能找到最简表达就找最简。
 *   这样日志可读性最好。
 *
 * @param d       帧率（fps）
 * @param postfix 单位字符串（"fps" / "tbr" / "tbn"）
 * @return 格式化好的字符串
 */
QString print_fps(double d, const char* postfix)
{
    char tmp[BUFF_MAXLEN];

    uint64_t v = lrintf(d * 100);
    if (!v)
    {
        snprintf(tmp, sizeof(tmp), "%1.4f %s", d, postfix);
    }
    else if (v % 100)
    {
        snprintf(tmp, sizeof(tmp), "%3.2f %s", d, postfix);
    }
    else if (v % (100 * 1000))
    {
        snprintf(tmp, sizeof(tmp), "%1.0f %s", d, postfix);
    }
    else
    {
        snprintf(tmp, sizeof(tmp), "%1.0fk %s", d / 1000, postfix);
    }

    return QString(tmp);
}

/**
 * @brief 把流的 side data 格式化成可读字符串
 *
 * ★ 什么是 side data？
 *   编解码器参数之外的"附加信息"，比如旋转角度、HDR 元数据、ICC 颜色配置等。
 *   容器里一般以独立字段存（不在 codecpar 主结构里）。
 *
 * ★ 哪些 type 不打印细节？
 *   很多类型（replaygain/spherical/3d 等）需要专门的解码函数，
 *   这里留了占位 TODO，正式产品里再实现。
 *
 * @param st     要打印的流
 * @param indent 缩进字符串
 * @return 拼好的字符串（无 side data 时返回空串）
 */
QString dump_sidedata(const AVStream* st, const char* indent)
{
    QString str;
    char tmp[BUFF_MAXLEN];

    if (!st->codecpar)
        return str;

    if (st->codecpar->nb_coded_side_data)
    {
        snprintf(tmp, sizeof(tmp), "%sSide data:\n", indent);
        str += tmp;
    }

    for (int i = 0; i < st->codecpar->nb_coded_side_data; i++)
    {
        const AVPacketSideData* sd = &st->codecpar->coded_side_data[i];
        snprintf(tmp, sizeof(tmp), "%s  ", indent);
        str += tmp;

        switch (sd->type)
        {
            case AV_PKT_DATA_PALETTE:
                str += "palette";
                break;
            case AV_PKT_DATA_NEW_EXTRADATA:
                str += "new extradata";
                break;
            case AV_PKT_DATA_PARAM_CHANGE:
                str += "paramchange: ";
                // dump_paramchange(ctx, sd);
                break;
            case AV_PKT_DATA_H263_MB_INFO:
                str += "H.263 macroblock info";
                break;
            case AV_PKT_DATA_REPLAYGAIN:
                str += "replaygain: ";
                // dump_replaygain(ctx, sd);
                break;
            case AV_PKT_DATA_DISPLAYMATRIX:
            {
                snprintf(tmp, sizeof(tmp), "displaymatrix: rotation of %.2f degrees",
                         av_display_rotation_get((const int32_t*)sd->data));
                str += tmp;
            }
            break;
            case AV_PKT_DATA_STEREO3D:
                str += "stereo3d: ";
                // dump_stereo3d(ctx, sd);
                break;
            case AV_PKT_DATA_AUDIO_SERVICE_TYPE:
                str += "audio service type: ";
                // dump_audioservicetype(ctx, sd);
                break;
            case AV_PKT_DATA_QUALITY_STATS:
            {
                snprintf(tmp, sizeof(tmp), "quality factor: %p, pict_type: %c", sd->data,
                         av_get_picture_type_char(AVPictureType(sd->data[4])));
                str += tmp;
            }
            break;
            case AV_PKT_DATA_CPB_PROPERTIES:
                str += "cpb: ";
                // dump_cpb(ctx, sd);
                break;
            case AV_PKT_DATA_MASTERING_DISPLAY_METADATA:
                // dump_mastering_display_metadata(ctx, sd);
                break;
            case AV_PKT_DATA_SPHERICAL:
                str += "spherical: ";
                // dump_spherical(ctx, st->codecpar, sd);
                break;
            case AV_PKT_DATA_CONTENT_LIGHT_LEVEL:
                // dump_content_light_metadata(ctx, sd);
                break;
            case AV_PKT_DATA_ICC_PROFILE:
                str += "ICC Profile";
                break;
            case AV_PKT_DATA_DOVI_CONF:
                str += "DOVI configuration record: ";
                // dump_dovi_conf(ctx, sd);
                break;
            case AV_PKT_DATA_S12M_TIMECODE:
                str += "SMPTE ST 12-1:2014: ";
                // dump_s12m_timecode(ctx, st, sd);
                break;
            default:
            {
                snprintf(tmp, sizeof(tmp), "unknown side data type %d, size:%llu bytes",
                         sd->type, sd->size);
                str += tmp;
            }
            break;
        }

        str += "\n";
    }

    return str;
}
