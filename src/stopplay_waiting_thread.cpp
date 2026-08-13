// ***********************************************************/
// stopplay_waiting_thread.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 等待播放停止的线程实现
// ***********************************************************/
#include "stopplay_waiting_thread.h"
#include "mainwindow.h"

/**
 * @brief 构造函数
 * 把 parent 透传给 QThread，并保存下一个要播放的文件路径
 *
 * @param parent 父 QObject
 * @param file   下一个要播放的文件（空 = 不继续播放）
 */
StopWaitingThread::StopWaitingThread(QObject* parent, const QString& file)
    : QThread(parent), m_file(file)
{
}

/**
 * @brief 析构函数
 * 资源由 QThread 自动回收
 */
StopWaitingThread::~StopWaitingThread()
{
}

/**
 * @brief 停止等待线程主函数
 *
 * 工作流程：
 *   1. 发 stopPlay 信号通知主窗口开始停止
 *   2. 循环等待，每 2ms 轮询一次 is_playing()，直到主窗口报告"已停止"
 *   3. 如果 m_file 不为空，紧接着发 startPlay 信号，开始播放下一个文件
 *
 * 这个线程的作用：
 *   让"停止 + 播放下一个"的流程在后台完成，避免 UI 卡顿
 *   （停止可能需要等几百 ms 等子线程退出）
 */
void StopWaitingThread::run()
{
    MainWindow* pMainWnd = (MainWindow*)parent();
    // ★ 先发信号让主窗口开始停止流程
    emit stopPlay();

    // ★ 轮询等待主窗口停止完毕（每 2ms 查一次）
    //    不能用 wait 死等，因为主窗口必须有机会处理事件循环
    while (pMainWnd && pMainWnd->is_playing())
    {
        msleep(2);
    }

    // ★ 如果指定了下一个文件，就接着发 startPlay 让它开始播
    emit startPlay(m_file);
    qDebug("-------- stopplay waiting thread exit.");
    return;
}
