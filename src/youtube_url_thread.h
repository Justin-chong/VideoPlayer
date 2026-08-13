#pragma once

#include <QDebug>
#include <QThread>
#include <QDir>
#include <QProcess>
#include "youtube_url_dlg.h"
#include "youtube_json.h"

/**
 * @brief YouTube 链接解析线程
 *
 * YouTube 不直接给视频文件 URL，而是给一个网页。
 * 要拿到能播放的 URL，需要：
 *   1. 下载并解析 YouTube 页面
 *   2. 提取出视频流的真实地址
 *
 * 业界常用的工具是 youtube-dl / yt-dlp（Python 写的）。
 * 本项目支持两种方式：
 *   - youtube_dl_exe():  调用 youtube-dl.exe
 *   - youtube_python():  调用 python + pytube 库
 *
 * 为什么单独开线程？
 * 因为 YouTube 页面下载 + 解析可能要 5~30 秒，绝不能在 UI 线程做。
 */
class YoutubeUrlThread : public QThread
{
    Q_OBJECT

public:
    explicit YoutubeUrlThread(const YoutubeUrlDlg::YoutubeUrlData& data, QObject* parent = Q_NULLPTR);
    ~YoutubeUrlThread();
signals:
    void resultReady(const QString& s);                                       // 解析成功，返回 URL
    void resultFailed(const QString& s);                                      // 解析失败，返回错误信息
    void resultYtReady(const YoutubeJsonParser::YtStreamData& st_data);       // 解析成功，返回视频信息（含清晰度等）

protected:
    void run() override;
    void youtube_dl_exe(); // using youtube-dl.exe
    void youtube_python(); // using python *.py
    bool python_install_pytube();
    bool excute_process(const QString& exec, const QStringList& params, QString& output);

private:
    YoutubeUrlDlg::YoutubeUrlData m_data;  // 用户输入的 URL 和选项
    static bool m_bInstalledpyTube;        // 是否已安装 pytube 库
};
