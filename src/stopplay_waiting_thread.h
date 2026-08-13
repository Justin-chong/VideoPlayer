#pragma once

#include <QDebug>
#include <QThread>

/**
 * @brief "停止播放" 等待线程
 *
 * 为什么需要这个线程？
 * 停止播放时，要做：
 *   1. 通知读包、解码、播放三个子线程退出
 *   2. 等待它们真的退出（不能强制杀线程，会泄漏资源）
 *   3. 释放 VideoState、关闭文件、关闭音频设备
 *   4. 然后再开始播放新文件
 *
 * 这些操作可能需要几百毫秒到几秒（取决于文件大小、缓存清理）。
 * 如果在 UI 线程里做，界面会卡住。
 *
 * 所以单独开一个线程：等所有子线程退出后，发 stopPlay 信号通知 UI；
 * 用户也可以选择紧接着播放下一个文件，发 startPlay 信号即可。
 */
class StopWaitingThread : public QThread
{
    Q_OBJECT

public:
    explicit StopWaitingThread(QObject* parent = Q_NULLPTR, const QString& file = "");
    ~StopWaitingThread();
signals:
    void stopPlay();                 // 停止完成通知
    void startPlay(const QString& file);  // 紧接着播放下一个文件

protected:
    void run() override;  // Qt 线程入口

private:
    QString m_file;  // 紧接着要播放的文件（可为空）
};
