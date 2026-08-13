// ***********************************************************/
// youtube_url_thread.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// YouTube URL 解析线程实现
// 通过调用 Python 脚本（pytube）来获取 YouTube 视频的直链
// ***********************************************************/

#include "youtube_url_thread.h"
#include "common.h"

// 是否已安装 pytube（静态变量，所有实例共享）
bool YoutubeUrlThread::m_bInstalledpyTube = false;

/**
 * @brief 构造函数
 * 把 YouTube 链接 + 画质选项存到 m_data
 *
 * @param data  来自 YoutubeUrlDlg 的 url/option/index
 * @param parent 父 QObject
 */
YoutubeUrlThread::YoutubeUrlThread(const YoutubeUrlDlg::YoutubeUrlData& data, QObject* parent)
    : QThread(parent), m_data(data)
{
}

/**
 * @brief 析构函数
 * 资源由 QThread 自动回收
 */
YoutubeUrlThread::~YoutubeUrlThread()
{
}

/**
 * @brief 启动一个外部进程并获取输出
 * @param exec 可执行文件路径或命令名
 * @param params 命令行参数
 * @param output [out] 进程的 stdout 输出
 * @return true=成功（NormalExit）
 */
bool YoutubeUrlThread::excute_process(const QString& exec, const QStringList& params, QString& output)
{
    bool success = false;
    output = "";
    // ★ QProcess: Qt 自带的"启动外部进程"工具
    QProcess process;

    // ★ 默认假设是 CrashExit（只有确认正常退出才改 success）
    QProcess::ExitStatus status = QProcess::ExitStatus::CrashExit;
    // ★ 启动进程（exec 是可执行文件，params 是命令行参数）
    process.start(exec, params);

    qDebug() << "Exe: " << exec << "params:" << params;
    // ★ 阻塞等待进程结束（要等到调用方有 timeout 或有事件循环才返回）
    process.waitForFinished();

    status = process.exitStatus();
    // ★ 只在"正常退出"时算成功（崩溃/被杀都算失败）
    if (status == QProcess::ExitStatus::NormalExit)
    {
        // ★ 读进程的标准输出（注意：要 NormalExit 之后才读得到）
        output = QString(process.readAllStandardOutput());
        qDebug() << "output: " << output;
        success = true;
    }

    return success;
}

/**
 * @brief 用 youtube-dl.exe 解析（方式 1）
 * 走老式 youtube-dl 命令行工具
 * 命令形如：youtube-dl.exe -f best -g <url>
 */
void YoutubeUrlThread::youtube_dl_exe()
{
    QString output;
    // ★ youtube-dl.exe 放在 tools 目录下，相对当前工作目录
    QString exec = appendPath(QDir::currentPath(), "tools/youtube-dl.exe");

    QStringList params;
    // ★ 防御：exe 不存在就直接退出（避免 QProcess 报错刷日志）
    if (!QFile::exists(exec))
        return;

    if (m_data.url.isEmpty())
        return;

    // ★ -f 选格式 -g 只输出直链
    params << "-f" << m_data.option << "-g" << m_data.url;
    if (excute_process(exec, params, output) && !output.isEmpty())
    {
        // ★ 旧版接口：直接把 url 字符串发出去
        emit resultReady(output);
        return;
    }

    qWarning() << "Parsing url failed, url:" << m_data.url << "options:" << m_data.option;
    emit resultFailed(m_data.url);
}

/**
 * @brief 线程主函数
 * 选一种解析方式去拿 YouTube 直链（当前用 youtube_python()）
 */
void YoutubeUrlThread::run()
{
    //youtube_dl_exe();  // 旧方式，已弃用
    // ★ 新方式：python + pytube，更稳定
    youtube_python();
}

/**
 * @brief 用 Python + pytube 解析（方式 2，当前在用）
 *
 * 命令形如：python get_yt_url.py <url> <option_index>
 *
 * 流程：
 *   1. 第一次跑先装 pytube
 *   2. 调 python 跑 get_yt_url.py
 *   3. 脚本会下载视频信息并写一个 JSON 文件，路径在 stdout 最后一行
 *   4. 用 YoutubeJsonParser 解析 JSON
 *   5. 根据用户选项（opt_index）选最合适的流
 *   6. 发 resultYtReady 信号通知主窗口
 *
 * ★ 为什么要用 Python 子进程而不是直接调 pytube？
 *   pytube 经常因 YouTube 改 API 失效，独立成脚本可以单独升级，
 *   C++ 端不重新编译就能用上最新版 pytube。
 *
 * ★ 选流为什么用 switch 而不是数组索引？
 *   0~5 分别对应 best/worst/bestvideo/worstvideo/bestaudio/worstaudio
 *   顺序和 YoutubeUrlDlg 的 m_options 保持一致。
 */
void YoutubeUrlThread::youtube_python()
{
    // ★ 第一次运行时先装 pytube 库
    //    m_bInstalledpyTube 是静态成员，整个进程只装一次
    if (!m_bInstalledpyTube)
        python_install_pytube();

    QString output;
    // ★ 调系统环境变量里的 python（依赖 PATH 里有 python）
    QString exec = "python";
    QString script = appendPath(QDir::currentPath(), "tools/get_yt_url.py");

    QStringList params;
    if (m_data.url.isEmpty())
        return;

    // ★ 参数：脚本路径 + URL + 选项索引
    //    脚本会下载视频信息并写入一个 JSON 文件
    params << script << m_data.url << QString::number(m_data.opt_index);

    if (excute_process(exec, params, output) && !output.isEmpty())
    {
#if _DEBUG
        qInfo() << "Yutube Json: " << output.trimmed();
#endif
        // ★ Python 脚本最后会输出 JSON 文件的路径
        //    用 \r\n 分割（Windows 的换行）
        auto parts = output.split("\r\n");
        // ★ 删掉空行（脚本可能输出多余的空行）
        parts.removeAll(QString(""));

        // ★ 最后一行就是 JSON 文件路径
        YoutubeJsonParser parser(parts.takeLast());

        // 解析选中的流
        YoutubeJsonParser::YtStreamData st;
        st.title = parser.m_data.title;
        st.length = parser.m_data.length;
        // ★ 根据用户选项（画质/音质优先级）选不同的流
        switch (m_data.opt_index)
        {
            case 0:
                parser.get_best_stream(st.stream);        // 最佳
                break;
            case 1:
                parser.get_worst_stream(st.stream);      // 最差
                break;
            case 2:
                parser.get_bestvideo_stream(st.stream);  // 最佳视频
                break;
            case 3:
                parser.get_worstvideo_stream(st.stream); // 最差视频
                break;
            case 4:
                parser.get_bestaudio_stream(st.stream);  // 最佳音频
                break;
            case 5:
                parser.get_worstaudio_stream(st.stream); // 最差音频
                break;
        }
        // ★ 通知主窗口：解析完毕，可以播放了
        emit resultYtReady(st);
        return;
    }

    qWarning() << "Parsing url failed, url:" << m_data.url << "options:" << m_data.option;
    emit resultFailed(m_data.url);
}

/**
 * @brief 用 pip 安装 pytube 库（如果没装过）
 *
 * ★ 为什么要独立成一个函数？
 *   pip install 可能要 5-10 秒（还要下载包），要单独处理
 *   失败的情况（pip 不在 PATH、网络断等）。
 *
 * @return true=安装成功（或已安装）；false=pip 调不起来
 */
bool YoutubeUrlThread::python_install_pytube()
{
    QString output;
    // ★ 调系统环境变量里的 pip
    QString exec = "pip";

    QStringList params;

    // ★ install pytube -q -U
    //    -q 安静模式（少打 pip 自家的 log）
    //    -U 升级到最新版（pytube 经常因 YouTube 改 API 失效，要保持最新）
    params << "install"
           << "pytube"
           << "-q"
           << "-U";

    if (!excute_process(exec, params, output))
    {
        qWarning() << "pip install pytube failed!";
        return false;
    }

    // ★ 标记已安装，下次不用再装
    m_bInstalledpyTube = true;
    return true;
}
