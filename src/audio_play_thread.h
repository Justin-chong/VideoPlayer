#pragma once

#include <QAudio>
//#include <QAudioDeviceInfo>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QAudioSink>      // Qt 6 的音频输出类
#include <QDebug>
#include <QFile>
#include <QIODevice>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>
#include <atomic>          // std::atomic：m_bExitThread 跨线程读写
#include <memory>
#include "packets_sync.h"

// 音频缓冲区大小（字节）。8192 = 8KB，约对应 44.1kHz 立体声 23ms 的音频
#define BUFFER_LEN 8192 // 1024

/**
 * @brief 播放线程和 UI 之间的音频数据
 * 用于把音频数据发给音频可视化窗口（频谱、波形等）
 */
typedef struct AudioData
{
    uint16_t len = 0;          // 数据长度
    char buffer[BUFFER_LEN] = {0}; // 数据内容
} AudioData;

/**
 * @brief 音频帧的格式信息
 * 告诉 Qt 音频设备要按什么格式打开。
 */
typedef struct AudioFrameFmt
{
    uint sample_rate;     // 采样率
    uint sample_fmt;      // 采样格式（AV_SAMPLE_FMT_S16 等）
    uint channel;         // 声道数
    int byte_order;       // 字节序（QAudioFormat::LittleEndian）
    int sample_type;      // 采样类型（QAudioFormat::SignedInt）
} AudioFrameFmt;

/**
 * @brief 音频播放线程
 *
 * 流水线的"第三站"——音频部分。
 * 工作流程：
 *   1. 初始化 Qt 的 QAudioSink（指定采样率、声道、格式）
 *   2. 从 sampq 取出解码好的 AVFrame
 *   3. 用 SwrContext（音频重采样）把任意格式转成 QAudioSink 要求的格式
 *   4. 把 PCM 数据写入 QIODevice，Qt 自动喂给声卡播放
 *   5. 可选：把音频数据发给音频可视化窗口
 */
class AudioPlayThread : public QThread
{
    Q_OBJECT
public:
    explicit AudioPlayThread(QObject* parent = nullptr, VideoState* pState = nullptr);
    virtual ~AudioPlayThread();

public:
    void print_device() const;  // 打印当前音频设备信息
    void audio_device_detail(const QAudioDevice& device) const;  // 打印指定设备的详细信息
    // 初始化音频设备（采样率、声道、采样格式、音量）
    bool init_device(int sample_rate = 8000, int channel = 1, AVSampleFormat sample_fmt = AV_SAMPLE_FMT_S16, float default_vol = 0.8);
    void stop_device();          // 停止音频设备
    // ★ 音频设备是否已初始化（m_pOutput 是否已创建）
    //   用途：QAudioSink 必须在 GUI 线程创建（线程亲和性），
    //         所以把它从 StartPlayThread 挪到 MainWindow::play_started() 里，
    //         该函数用来判断"是否还需要初始化"。
    bool device_ready() const { return m_pOutput != nullptr; }
    void play_file(const QString& file);  // 播放本地文件（调试用）
    void play_buf(const uint8_t* buf, int datasize);  // 播放一段原始 PCM
    // 初始化音频重采样参数（把解码出的音频格式转成 Qt 设备要求的格式）
    bool init_resample_param(AVCodecContext* pAudio, AVSampleFormat sample_fmt, VideoState* is);
    void final_resample_param(); // 释放重采样资源
    float get_device_volume() const;
    void set_device_volume(float volume);
    void send_visual_open(bool bSend = true) { m_bSendToVisual = bSend; };  // 是否把音频数据发给可视化窗口

signals:
    void update_play_time();                          // 通知 UI 更新播放进度
    void data_visual_ready(const AudioData& data);    // 通知可视化窗口有新音频数据

public slots:
    void stop_thread();  // 外部调用，让线程退出

protected:
    void run() override;  // Qt 线程入口

private:
    // 从 sampq 取一帧音频，做重采样，写入 QAudioSink
    int audio_decode_frame(VideoState* is);

private:
    // 音频重采样上下文
    typedef struct Audio_Resample
    {
        struct SwrContext* swrCtx{nullptr};  // FFmpeg 音频重采样上下文
    } Audio_Resample;

private:
    std::unique_ptr<QAudioSink> m_pOutput;  // Qt 音频输出设备
    QIODevice* m_audioDevice{nullptr};      // 实际写入音频数据的 IO 设备
    VideoState* m_pState{nullptr};          // 共享播放状态
    Audio_Resample m_audioResample;         // 重采样上下文
    std::atomic<bool> m_bExitThread{false}; // 退出标志（由主线程写、本线程读，必须原子）
    bool m_bSendToVisual{false};            // 是否发送数据给可视化窗口
    // ★ 防止 stop_device / 析构函数二次调用 m_pOutput->stop() 导致 QAudioSink 状态崩溃
    std::atomic<bool> m_bDeviceStopped{false};
};
