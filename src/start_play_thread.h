#pragma once

#include <QPointer>
#include <QThread>

class MainWindow;

/**
 * @brief "开始播放" 预处理线程
 *
 * 为什么不直接在主线程里 start？
 * 因为开始播放要做很多"耗时的准备工作"：
 *   1. 打开文件（avformat_open_input）
 *   2. 探测流信息（avformat_find_stream_info）
 *   3. 找解码器、打开解码器
 *   4. 初始化音频设备（QAudioSink）—— 这一步可能要枚举系统音频设备，比较慢
 *   5. 启动读包、解码、播放三个子线程
 * 如果在主线程（UI 线程）做这些，点击"打开"按钮时界面就会卡死。
 *
 * 所以单独开一个线程来做，做完后通过 audio_device_init 信号通知主线程。
 */
class StartPlayThread : public QThread
{
    Q_OBJECT

public:
    explicit StartPlayThread(QObject* parent = Q_NULLPTR);
    ~StartPlayThread();
signals:
    void audio_device_init(bool ret);  // 音频设备初始化完成（true=成功）

protected:
    void run() override;  // Qt 线程入口

private:
    // ★ 构造时保存主窗口指针，不再依赖 QObject::parent()。
    //   原因：线程"退休"时会把它从 MainWindow 的父子树上摘下来
    //   （见 mainwindow.cpp 的 retire_worker_thread），那时 parent() 已经是 null。
    //   用 QPointer 还多一层保护：主窗口先销毁时它自动变 null。
    QPointer<MainWindow> m_pMainWnd;
};
