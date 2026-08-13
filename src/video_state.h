/**
 * @file video_state.h
 * @author lichong
 *
 * @brief VideoStateData —— VideoState 的"包装类"
 *
 * 【设计说明】前面 packets_sync.h 里定义的 VideoState 是一个纯 C 风格的结构体，
 * 字段都是公开的，初始化和销毁比较繁琐。
 * VideoStateData 是它的 C++ 包装，提供：
 *   - 构造函数 / 析构函数（自动管理资源）
 *   - 打开 / 关闭媒体文件的完整流程
 *   - 硬件解码支持（DXVA2）
 *   - 线程管理
 *
 * 可以把它理解成"播放会话管理器"，MainWindow 通过它来控制整个播放过程。
 */

#pragma once

#include "packets_sync.h"

/**
 * @brief 播放会话的 C++ 包装类
 *
 * 一个 VideoStateData 实例对应一个正在播放（或即将播放）的媒体。
 * 负责：
 *   1. stream_open()：打开文件、找流、找解码器
 *   2. 启动/停止所有工作线程（在 MainWindow 那一层调用）
 *   3. 硬件加速解码（可选）
 *   4. stream_close()：关闭时释放所有资源
 */
class VideoStateData
{
public:
    /**
     * @brief 构造函数
     * @param use_hardware 是否尝试用硬件解码（DXVA2 on Windows）
     * @param loop_play    是否循环播放
     */
    explicit VideoStateData(bool use_hardware = false, bool loop_play = false);
    virtual ~VideoStateData();

public:
    bool has_video() const;     // 是否有视频流
    bool has_audio() const;     // 是否有音频流
    bool has_subtitle() const;  // 是否有字幕流
    AVCodecContext* get_contex(AVMediaType type) const;  // 拿指定流的解码器上下文
    bool is_hardware_decode() const;  // 是否真的启用了硬件解码
    int create_video_state(const char* filename);   // 打开并初始化 VideoState
    void delete_video_state();                       // 销毁 VideoState
    VideoState* get_state() const;                   // 拿到底层的 VideoState 指针（给线程用）
    void print_state() const;                        // 打印 VideoState 当前状态（调试用）
    void threads_setting(VideoState* is, const Threads& threads);  // 把线程指针塞进 VideoState

private:
    // 打开一个媒体流，初始化 VideoState 内部的所有队列和解码器
    VideoState* stream_open(const char* filename, const AVInputFormat* iformat = NULL);
    // 关闭流，释放资源
    void stream_close(VideoState* is);
    // 打开某个流（视频/音频/字幕），找解码器、打开解码器
    int stream_component_open(VideoState* is, int stream_index);
    // 关闭某个流
    void stream_component_close(VideoState* is, int stream_index);
    // 打开媒体并填充 VideoState
    int open_media(VideoState* is);
    // 拿指定名字的硬件设备类型（dxva2、qsv、cuda 等）
    enum AVHWDeviceType get_hwdevice(const char* device) const;
    // 拿指定硬件设备支持的像素格式
    enum AVPixelFormat get_hwdevice_decoder(const AVCodec* decoder, enum AVHWDeviceType type) const;
    // 打开硬件解码器
    bool open_hardware(AVCodecContext* avctx, const AVCodec* codec, const char* device = "dxva2");
    void close_hardware();    // 关闭硬件解码器
    int hw_decoder_init(AVCodecContext* ctx, const enum AVHWDeviceType type);  // 初始化硬件解码器
    void read_thread_exit_wait(VideoState* is);  // 等待读包线程退出
    void threads_exit_wait(VideoState* is);      // 等待所有线程退出

private:
    VideoState* m_pState{nullptr};  // 真正的播放状态（前面 packets_sync.h 里定义的那个）

    // 当前文件是否包含视频/音频/字幕
    bool m_bHasVideo{false};
    bool m_bHasAudio{false};
    bool m_bHasSubtitle{false};

    // 各流的解码器上下文（用于 OpenCV 等模块直接访问解码后的格式）
    AVCodecContext* m_avctxVideo{nullptr};
    AVCodecContext* m_avctxAudio{nullptr};
    AVCodecContext* m_avctxSubtitle{nullptr};

    /** 硬件解码相关 **/
    bool m_bUseHardware{true};         // ★ 默认开启硬解（4K 软解太慢，DXVA2 失败时自动回退软解）
    bool m_bHardwareSuccess{false};    // 硬件解码是否真的初始化成功
    AVBufferRef* m_hw_device_ctx{nullptr};  // 硬件设备上下文（GPU 相关）

    bool m_bLoopPlay{false};  // 是否循环播放
};
