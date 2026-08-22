// ***********************************************************/
// audio_play_thread.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 音频播放线程实现（流水线的第三站 - 音频部分）
// 把解码好的音频 PCM 数据通过 Qt 的 QAudioSink 喂给声卡
// ***********************************************************/

#include "audio_play_thread.h"

// 调试开关
#if !NDEBUG
#define DEBUG_PLAYFILTER 0
#define WRITE_AUDIO_FILE 0
#else
#define DEBUG_PLAYFILTER 0
#define WRITE_AUDIO_FILE 0
#endif

#if WRITE_AUDIO_FILE
#include <fstream>  // 写入 PCM 到文件（调试用）
#endif

AudioPlayThread::AudioPlayThread(QObject* parent, VideoState* pState) : QThread(parent), m_pState(pState)
{
    // 启动时打印可用音频设备
    print_device();
    // 注册 AudioData 元类型，让信号槽能跨线程传自定义结构
    qRegisterMetaType<AudioData>("AudioData");
}

AudioPlayThread::~AudioPlayThread()
{
    // ★ 防御性修复（2026-08-04）：stop 按钮 abort 排查
    //   原代码只调 stop_device()，没先 stop_thread()
    //   隐患：m_pOutput->stop() 时如果音频线程的 run() 还在用 m_audioDevice->write()
    //         会导致 QAudioSink 内部状态不一致 → abort()
    //   修法：先 stop_thread()（设 m_bExitThread=true + wait），确保 run() 已退出
    //         再 stop_device() 才能安全
    stop_thread();            // 先让音频线程自己退出
    stop_device();            // 再停止音频设备
    final_resample_param();   // 最后释放重采样资源
}

/**
 * @brief 打印系统所有音频设备
 * 用于启动时诊断音频问题
 */
void AudioPlayThread::print_device() const
{
    // 1. 拿默认输出设备
    auto audioOutput = QMediaDevices::defaultAudioOutput();
    audioOutput.description();   // 拿名字（这里只取值，没打印）

    // 2. 遍历所有"输入设备"（麦克风等）
    for (const auto& device : QMediaDevices::audioInputs())
        audio_device_detail(device);

    // 3. 遍历所有"输出设备"（扬声器、耳机等）
    for (const auto& device : QMediaDevices::audioOutputs())
        audio_device_detail(device);
}

/**
 * @brief 打印一个音频设备的详细信息
 */
void AudioPlayThread::audio_device_detail(const QAudioDevice& device) const
{
    auto mode = device.mode();
    if (mode == QAudioDevice::Mode::Input)
    {
        qDebug() << "Input audio device: ";
    }
    else if (mode == QAudioDevice::Mode::Output)
    {
        qDebug() << "Output audio device: ";
    }
    else
    {
        qDebug() << "Warning, NULL audio device! ";
        return;
    }

    qDebug() << "desc: " << device.description();
    qDebug() << ", ID: " << device.id();
    qDebug() << ", Mode: " << device.mode();
    qDebug() << ", IsDefault: " << device.isDefault();

    QAudioFormat fmt = device.preferredFormat();
}

/**
 * @brief 初始化音频输出设备
 * @param sample_rate 采样率
 * @param channel 声道数
 * @param sample_fmt 采样格式（项目里固定用 S16）
 * @param default_vol 默认音量
 * @return true=成功
 */
bool AudioPlayThread::init_device(int sample_rate, int channel, AVSampleFormat sample_fmt, float default_vol)
{
    auto deviceInfo = QMediaDevices::defaultAudioOutput();

    QAudioFormat format;

    format.setSampleRate(sample_rate);
    format.setChannelCount(channel);
    // ★ Qt 音频设备固定要 Int16，和 swr_convert 目标格式保持一致
    format.setSampleFormat(QAudioFormat::Int16);

    // 检查设备是否支持这种格式
    // ★ Qt 设备可能不支持任意组合（采样率/声道数），
    //   不支持时 Qt 会返回一个最接近的替代格式
    if (!deviceInfo.isFormatSupported(format))
    {
        qWarning() << "Raw audio format not supported!";
        return false;
    }

    // ★ QAudioSink 是 Qt 的音频输出"门户"
    //   内部用 QIODevice 喂数据，配合 QAudioSink::start() 拿到写接口
    m_pOutput = std::make_unique<QAudioSink>(deviceInfo, format);
    set_device_volume(default_vol);

    // ★ start() 返回 QIODevice*，之后用 m_audioDevice->write() 喂 PCM
    m_audioDevice = m_pOutput->start();
    return true;
}

float AudioPlayThread::get_device_volume() const
{
    if (m_pOutput)
        return m_pOutput->volume();

    return 0;
}

void AudioPlayThread::set_device_volume(float volume)
{
    if (m_pOutput)
        m_pOutput->setVolume(volume);
}

void AudioPlayThread::stop_device()
{
    // ★ 防止二次调用：析构函数 + *_stopped 槽 + 任何其他路径都可能走到这里
    //   QAudioSink::stop() 二次调用会触发 Qt 内部 abort
    if (m_bDeviceStopped.exchange(true))
        return;
    if (m_pOutput)
    {
        m_pOutput->stop();
        m_pOutput->reset();
    }
}
// 播放本地文件（调试用）
void AudioPlayThread::play_file(const QString& file)
{
    /*play pcm file directly*/
    QFile audioFile;
    audioFile.setFileName(file);
    // ★ C4834: [[nodiscard]] 必须检查返回值
    if (audioFile.open(QIODevice::ReadOnly))
    {
        m_pOutput->start(&audioFile);
    }
}
// 播放一段原始 PCM
void AudioPlayThread::play_buf(const uint8_t* buf, int datasize)
{
    if (!m_audioDevice)
        return;

    uint8_t* data = (uint8_t*)buf;
    // ★ QAudioSink::write 不一定一次能写完所有数据（设备 buffer 满了）
    //   用 while 循环分批写，直到写完为止，避免漏音
    while (datasize > 0)
    {
        qint64 len = m_audioDevice->write((const char*)data, datasize);
        if (len < 0)
            break;  // 写入失败（设备异常），跳出避免死循环
        if (len > 0)
        {
            data = data + len;
            datasize -= len;
        }
        // qDebug("play buf:reslen:%d, write len:%d", len, datasize);
    }
}

void AudioPlayThread::run()
{
    assert(m_pState);
    VideoState* is = m_pState;
    int audio_size;

    for (;;)
    {
        if (m_bExitThread)
            break;

        if (is->abort_request)
            break;

        if (is->paused)
        {
            // ★ 暂停时也不忙等，10ms 后再检查退出标志
            msleep(10);
            continue;
        }

        // ★ 拿一帧解码好的音频样本（从 sampq 队列里取，详见 audio_decode_frame）
        audio_size = audio_decode_frame(is);
        if (audio_size < 0)
            break;  // 队列空且已到 EOF，退出线程

        if (!isnan(is->audio_clock))    // 音频时钟有效？（某些流可能没 PTS）
        {
            AVCodecContext* pAudioCodex = is->auddec.avctx;
            if (pAudioCodex)
            {
                // 1. 算"每秒多少字节"（= 采样率 × 声道数 × 单样本字节数）
                // ★ 用 av_samples_get_buffer_size 而不是手算，避免声道布局对齐的坑
                int bytes_per_sec = av_samples_get_buffer_size(
                    nullptr,
                    pAudioCodex->ch_layout.nb_channels,
                    pAudioCodex->sample_rate,
                    AV_SAMPLE_FMT_S16,
                    1
                    );
                // 例：44100 × 2 × 2 = 176400 字节/秒

                // 2. 记录"当前真实时间"（用相对时间避免受系统时钟影响）
                // ★ 相对时间从 0 开始单调递增，不受用户调系统时间影响
                int64_t audio_callback_time = av_gettime_relative();

                // 3. ★ 设置音频时钟
                //  - 音频播放进度 = 当前 PTS - 这一帧还要播多久
                //  - 因为这一帧刚要开始播，所以"现在"对应 PTS
                //  - audio_clock_serial 用于识别"seek 后"的时钟，避免用错时间的旧数据
                set_clock_at(&is->audclk,
                             is->audio_clock - (double)audio_size / bytes_per_sec,  // 帧起始 PTS
                             is->audio_clock_serial,                                // 序列号
                             audio_callback_time / 1000000.0);                     // 真实时间

                // 4. 同步"外部时钟"到"音频时钟"（保持对齐）
                // ★ extclk 是从外部（网络流、用户拖动）来的时钟，这里让它跟着 audclk 走
                sync_clock_to_slave(&is->extclk, &is->audclk);
            }
        }
    }

    qDebug("-------- Audio play thread exit.");
}
// 从 sampq 取一帧音频，做重采样，写入 QAudioSink
int AudioPlayThread::audio_decode_frame(VideoState* is)
{
    int data_size;
    Frame* af;

    do
    {
        // ★ sampq 队列空时，1ms 一查：等解码线程塞数据进来
        //   满了 eof 就退出；abort_request 来了也走（用户主动停止）
        while (frame_queue_nb_remaining(&is->sampq) == 0)
        {
            if (is->eof)
            {
                // break;
                return -1;  // 队列空 + 已到文件尾，结束播放
            }
            else
            {
                av_usleep(1000);  // 1ms 后再试（避免空转 CPU 100%）
                // return -1;
            }

            if (is->abort_request)
                break;
        }
        //等一个可读槽位（播放线程从这里取）
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;
        // ★ 立刻 next 把这个槽位让出来，下一帧可以往里写
        //   但不释放 af 本身，af 还在这一帧的 audio 数据
        frame_queue_next(&is->sampq);
        // ★ serial 不一致 = seek 之后队列里残留的旧帧，丢掉重试
        //   直到找到 seek 后的第一个新帧为止
    } while (af->serial != is->audioq.serial);

    /*data_size = av_samples_get_buffer_size(nullptr, af->frame->channels,
          af->frame->nb_samples,
          AVSampleFormat(af->frame->format), 1);*/

#if USE_AVFILTER_AUDIO
    // 走 avfilter 路径：直接拷贝（已经滤过波了）
    data_size = av_samples_get_buffer_size(nullptr, af->frame->ch_layout.nb_channels,
                                           af->frame->nb_samples, AV_SAMPLE_FMT_S16, 1);
    uint8_t* const buffer_audio = (uint8_t*)av_malloc(data_size * sizeof(uint8_t));

    memcpy(buffer_audio, af->frame->data[0], data_size);
#else
    // ★ 走 swr_convert 路径：把任意采样格式转成 S16（Qt 音频设备要 S16）
    //   比如 FLTP/FLT -> S16，或者重排声道布局
    struct SwrContext* swrCtx = m_audioResample.swrCtx;
    data_size = av_samples_get_buffer_size(
        nullptr, af->frame->channels, af->frame->nb_samples, AV_SAMPLE_FMT_S16,
        0); // AVSampleFormat(af->frame->format)
    uint8_t* buffer_audio = (uint8_t*)av_malloc(data_size * sizeof(uint8_t));

    int ret =
        swr_convert(swrCtx, &buffer_audio, af->frame->nb_samples,
                    (const uint8_t**)(af->frame->data), af->frame->nb_samples);
    if (ret < 0)
    {
        // ★ 转换失败也不能让线程挂掉，返回 0 让外层继续循环
        return 0;
    }
#endif

    // ★ 静音不是停止解码，而是把已经解码好的数据清零
    //   这样不会卡顿（已经在队列里了），同时又听不到声音
    if (is->muted && data_size > 0)
        memset(buffer_audio, 0, data_size); // mute

#if WRITE_AUDIO_FILE
    std::ofstream myfile;
    myfile.open("audio.pcm", std::ios::out | std::ios::app | std::ios::binary);
    if (myfile.is_open())
    {
        myfile.write((char*)buffer_audio, data_size);
    }
#endif

    if (m_bSendToVisual)
    {
        AudioData data;
        if (data_size > BUFFER_LEN)
        {
            qDebug() << "audio frame is too long,data_size:" << data_size
                     << ", buffer_len:" << BUFFER_LEN << "\n";
        }

        int len = std::min(data_size, BUFFER_LEN);
        memcpy(data.buffer, buffer_audio, len);
        data.len = len;
        // ★ 可视化用的是同一份 S16 数据（如频谱图），不能传原始的 FLTP
        emit data_visual_ready(data);
    }

    play_buf(buffer_audio, data_size);

    av_free((void*)buffer_audio);

    /* update the audio clock with the pts */
    if (!isnan(af->pts))
    {
        // is->audio_clock = af->pts + (double)af->frame->nb_samples /
        // af->frame->sample_rate;
        // ★ 这一帧的"时长" = 样本数 / 采样率，加到 pts 上得到这一帧播完时的"结束时间"
        double frame = (double)af->frame->nb_samples / af->frame->sample_rate;
        // frame = frame * is->audio_speed;
        is->audio_clock = af->pts + frame;

#if USE_AVFILTER_AUDIO
        is->audio_clock = is->audio_clock_old + (is->audio_clock - is->audio_clock_old) * is->audio_speed;
        // is->audio_clock = is->audio_clock * is->audio_speed;
#endif

#if DEBUG_PLAYFILTER
        static int pks_num = 0;
        pks_num++;

        qDebug("[%d]audio: clock=%0.3f pts=%0.3f, (nb:%d, sr:%d)frame:%0.3f\n",
               pks_num, is->audio_clock, af->pts, af->frame->nb_samples,
               af->frame->sample_rate, frame);

        // qDebug("audio: clock=%0.3f pts=%0.3f, (nb:%d, sr:%d)frame:%0.3f\n",
        // is->audio_clock, af->pts, af->frame->nb_samples, af->frame->sample_rate,
        // frame);
#endif
    }
    else
    {
        // ★ 这一帧没 PTS（比如直播流的某些情况），把时钟置为无效
        //   让上层音视频同步逻辑跳过
        is->audio_clock = NAN;
    }
    // ★ serial 跟着 af 走，下一帧如果 serial 变了说明发生过 seek，时钟要重建
    is->audio_clock_serial = af->serial;

    // 通知主线程更新进度条
    // ★ update_play_time 信号带的是 audio_clock，UI 上拿它画进度
    emit update_play_time();

#if (!NDEBUG && PRINT_PACKETQUEUE_AUDIO_INFO)
    {
        static double last_clock;
        qDebug("audio: delay=%0.3f clock=%0.3f\n", is->audio_clock - last_clock, is->audio_clock);
        last_clock = is->audio_clock;
    }
#endif

    return data_size;
}
// 初始化音频重采样参数（把解码出的音频格式转成 Qt 设备要求的格式）
bool AudioPlayThread::init_resample_param(AVCodecContext* pAudio, AVSampleFormat sample_fmt, VideoState* is)
{
    if (pAudio)
    {
        int ret = -1;
        struct SwrContext* swrCtx = nullptr;
#if USE_AVFILTER_AUDIO
        if (is)
        {
            // ★ 走 avfilter 路径：从 out_audio_filter sink 拿最终格式
            //   适用于需要变速/均衡器/重采样的复杂场景
            AVFilterContext* sink = is->out_audio_filter;
            // int sample_rate = av_buffersink_get_sample_rate(sink);
            // int nb_channels = av_buffersink_get_channels(sink);

            AVChannelLayout channel_layout;
            av_buffersink_get_ch_layout(sink, &channel_layout);
            // int64_t channel_layout = av_buffersink_get_channel_layout(sink);
            int format = av_buffersink_get_format(sink);

            // ★ 输入：解码器原始格式；输出：Qt 设备要求的 sample_fmt（一般是 S16）
            ret = swr_alloc_set_opts2(&swrCtx, &pAudio->ch_layout, sample_fmt,
                                      pAudio->sample_rate, &channel_layout,
                                      (AVSampleFormat)format, pAudio->sample_rate, 0,
                                      nullptr);

            /*m_audioResample.channel_layout = channel_layout;
      m_audioResample.sample_fmt = sample_fmt;
      m_audioResample.sample_rate = pAudio->sample_rate;*/
        }
#else
        // ★ 普通路径：源 = 解码器格式 / 目标 = Qt 设备要的 S16
        //   swr_alloc_set_opts2 比旧的 swr_alloc_set_opts 多支持 AVChannelLayout
        ret = swr_alloc_set_opts2(&swrCtx, &pAudio->ch_layout, sample_fmt,
                                  pAudio->sample_rate, &pAudio->ch_layout,
                                  pAudio->sample_fmt, pAudio->sample_rate, 0,
                                  nullptr);
#endif

        if (!(ret < 0))
        {
            // ★ swr_init 之后才能用，分配 FIFO 和转换系数
            swr_init(swrCtx);
            m_audioResample.swrCtx = swrCtx;
            return true;
        }
    }
    return false;
}

void AudioPlayThread::final_resample_param()
{
    swr_free(&m_audioResample.swrCtx);
}

void AudioPlayThread::stop_thread()
{
    m_bExitThread = true;
    wait();
}
