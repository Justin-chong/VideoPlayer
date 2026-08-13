// ***********************************************************/
// start_play_thread.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// "开始播放" 预处理线程实现
// 在后台做音频设备初始化等耗时操作，避免 GUI 卡顿
// ***********************************************************/

#include "start_play_thread.h"
#include "mainwindow.h"

/**
 * @brief 构造函数
 * 单纯把 parent 透传给 QThread，本身不做事
 *
 * @param parent 父 QObject（一般是 MainWindow）
 */
StartPlayThread::StartPlayThread(QObject* parent) : QThread(parent)
{
}

/**
 * @brief 析构函数
 * 资源由 QThread 自动回收，Qt 会保证 run() 退出后才析构子类
 */
StartPlayThread::~StartPlayThread()
{
}

/**
 * @brief 开始播放预处理线程主函数
 *
 * 工作流程：
 *   1. 拿到主窗口的当前设置（音量、VideoState 等）
 *   2. 调 AudioPlayThread::init_device() 初始化音频设备
 *   3. 调 AudioPlayThread::init_resample_param() 初始化音频重采样
 *   4. 完成后通过 audio_device_init 信号通知主窗口
 *
 * 为什么单独开线程？
 *   - 枚举音频设备可能耗时几百 ms
 *   - 初始化重采样上下文（SwrContext）也要几十 ms
 *   - 这些操作如果放主线程会让"打开文件"按钮卡顿
 */
void StartPlayThread::run()
{
    // ★ 父对象必须存在，否则什么都做不了
    MainWindow* pParent = (MainWindow*)parent();
    assert(pParent);
    bool ret = false;

#if !NDEBUG
    // ★ 调试用：测一下整个预处理过程花了多久
    QElapsedTimer timer;
    timer.start();
#endif

    // ★ 拿到用户当前设置的音量（false=不触发 mute 状态）
    float vol = pParent->volume_settings(false);
    // ★ 输出音频固定用 S16 格式（Qt 6 QAudioSink 推荐格式）
    AVSampleFormat sample_fmt = AV_SAMPLE_FMT_S16;

    VideoStateData* pVideoStateData = pParent->get_video_state_data();
    if (pVideoStateData)
    {
        AVCodecContext* pAudio = pVideoStateData->get_contex(AVMEDIA_TYPE_AUDIO);
        VideoState* pState = pVideoStateData->get_state();
        if (pAudio)
        {
            AudioPlayThread* pThread = pParent->get_audio_play_thread();
            if (pThread)
            {
                // ★ 1. 初始化音频设备（按 FFmpeg 解码出来的参数）
                ret = pThread->init_device(pAudio->sample_rate,
                                           pAudio->ch_layout.nb_channels, sample_fmt,
                                           vol);
                if (!ret)
                {
                    qWarning("audio play init_device failed.");
                }

                if (ret)
                {
                    // ★ 2. 初始化重采样（把 FFmpeg 解码的任意音频格式转成 S16）
                    ret = pThread->init_resample_param(pAudio, sample_fmt, pState);
                    if (!ret)
                    {
                        qWarning("audio play init resample param failed.");
                    }
                }
            }
        }
    }

    // ★ 通知主窗口：音频设备初始化完成（带成功/失败状态）
    emit audio_device_init(ret);
#if !NDEBUG
    // ★ 调试版打耗时，方便看出预处理是否过慢
    qDebug("Start play operation took %d milliseconds", timer.elapsed());
#endif
    qDebug("-------- start play thread(audio device initial) exit,ret=%d.", ret);
}
