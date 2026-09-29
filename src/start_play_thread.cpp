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
StartPlayThread::StartPlayThread(QObject* parent)
    : QThread(parent), m_pMainWnd(qobject_cast<MainWindow*>(parent))
{
}

/**
 * @brief 析构函数
 * 资源由 QThread 自动回收，Qt 会保证 run() 退出后才析构子类
 */
StartPlayThread::~StartPlayThread()
{
    qInfo("[TRACE][~StartPlayThread] this=%p, isRunning=%d, isFinished=%d",
          (void*)this, (int)isRunning(), (int)isFinished());
}

/**
 * @brief 开始播放预处理线程主函数
 *
 * 工作流程：
 *   1. 检查音频解码上下文是否就绪（廉价的前置校验）
 *   2. 通过 audio_device_init 信号把结果丢回主窗口的 play_started()
 *   3. ★ 真正的音频设备初始化（new QAudioSink + start）在 play_started() 里做
 *
 * ★★ 2026-09-24 重要修正：为什么本线程不再创建 QAudioSink ★★
 *   原实现是在这里调 AudioPlayThread::init_device()，也就是在"本线程"里
 *   new QAudioSink(...) + start()。但本线程 run() 一返回就结束了，而这个
 *   QAudioSink 的生命周期远长于此：
 *       创建+start  → 本线程（几毫秒后线程死亡）
 *       write        → AudioPlayThread
 *       stop/reset   → GUI 线程
 *       析构         → GUI 线程
 *   QAudioSink 内部带 QTimer、绑定了创建线程的线程亲和性（QTBUG-108187），
 *   跨线程操作/销毁属于未定义行为 —— 这正是点 Stop 时 abort() 的最后来源。
 *   所以现在把"创建 + start + stop + 析构"全部收拢到 GUI 线程执行。
 *
 * 本线程现在只保留"异步预检"的作用（保留原有启动时序，不改调用方）。
 */
void StartPlayThread::run()
{
    // ★ 主窗口必须还在，否则什么都做不了
    //   ★ 用构造时保存的 QPointer，而不是 parent()：本线程"退休"时
    //     已从 MainWindow 的父子树上被摘下来（parent() 为 null），
    //     主窗口先销毁时它也会自动变 null。
    if (!m_pMainWnd)
    {
        qWarning("StartPlayThread: main window is gone, skip audio pre-check.");
        emit audio_device_init(false);
        return;
    }
    bool ret = false;

#if !NDEBUG
    // ★ 调试用：测一下整个预处理过程花了多久
    QElapsedTimer timer;
    timer.start();
#endif

    // ★ 只做廉价的前置校验：音频解码上下文是否就绪
    //   （真正耗时的 new QAudioSink + start + swr 初始化，见下方说明，
    //     已挪到 GUI 线程的 MainWindow::play_started() 里做）
    VideoStateData* pVideoStateData = m_pMainWnd->get_video_state_data();
    if (pVideoStateData)
    {
        AVCodecContext* pAudio = pVideoStateData->get_contex(AVMEDIA_TYPE_AUDIO);
        ret = (pAudio != nullptr);
        if (!ret)
        {
            qWarning("no audio codec context, skip audio device init.");
        }
    }

    // ★ 通知主窗口：后台预检完成（真正初始化在 play_started()，带成功/失败状态）
    emit audio_device_init(ret);
#if !NDEBUG
    // ★ 调试版打耗时，方便看出预处理是否过慢
    qDebug("Start play operation took %d milliseconds", timer.elapsed());
#endif
    qDebug("-------- start play thread(audio device initial) exit,ret=%d.", ret);
}
