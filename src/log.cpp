// ***********************************************************/
// log.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 日志模块实现
// 接管 Qt 的 qDebug/qInfo/qWarning 输出，加上时间戳和文件信息
// ***********************************************************/

#include <Windows.h>
#include <iostream>
#include "log.h"

#define BUFF_LEN (1024)

/**
 * @brief Windows API 风格的可变参数打印（同时写到 OutputDebugString）
 *
 * ★ 为什么是 Windows 专用？
 *   用了 OutputDebugString + Windows 字符集宏，只有 Windows 平台支持。
 *   配合 #ifdef 也能支持 UNICODE 模式。
 *
 * ★ 为什么要再封一层？
 *   把 printf 风格串一行就转发到 OutputDebugString，
 *   VS 调试时不用挂 console 也能在输出窗口看日志。
 *
 * @param szFormat printf 风格的格式串
 * @param ...       可变参数
 */
#ifdef UNICODE
void Output(LPCWSTR szFormat, ...)
#else
void Output(const char* szFormat, ...)
#endif
{
#ifdef UNICODE
    WCHAR szBuff[BUFF_LEN + 1];

    va_list arg;
    va_start(arg, szFormat);
    // ★ _TRUNCATE 表示缓冲区不够就截断，不让它溢出
    _vsnwprintf_s(szBuff, _countof(szBuff), _TRUNCATE, szFormat, arg);
    va_end(arg);
#else
    char szBuff[BUFF_LEN];

    va_list arg;
    va_start(arg, szFormat);
    _vsnprintf(szBuff, BUFF_LEN, szFormat, arg);
    va_end(arg);
#endif

    // ★ 把字符串扔给 OutputDebugString，VS 立即看到
    OutputDebugString(szBuff);
}

/**
 * @brief Qt 日志回调（qInstallMessageHandler 安装）
 *
 * 给所有 qDebug / qInfo / qWarning / qCritical 输出加上时间戳、文件、行号。
 * 调试版（!NDEBUG）输出到 OutputDebugString（VS 里能看到）
 * 发布版只把 warning 及以上写入 log.txt
 *
 * @param type    Qt 消息类型
 * @param context 上下文（文件名/行号/函数名）
 * @param msg     实际消息
 */
void logOutput(const QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    QString txt, type_str;
    // ★ 时间戳用 "dd/MM/yy hh:mm:ss.zzz"（毫秒精度，方便对齐多线程日志）
    QString time = QDateTime::currentDateTime().toString("dd/MM/yy hh:mm:ss.zzz");

    QFileInfo file(context.file);

    // 把消息类型转成字符串
    switch (type)
    {
        case QtDebugMsg:
            type_str = "Debug";
            break;
        case QtInfoMsg:
            type_str = "Info";
            break;
        case QtWarningMsg:
            type_str = "Warning";
            break;
        case QtCriticalMsg:
            type_str = "Critical";
            break;
        case QtFatalMsg:
            type_str = "Fatal";
            // ★ 致命错误：日志记完后直接 abort()，避免进程留异常状态
            abort();
        default:
            break;
    }

    // ★ 过滤 Qt 内部定时器的频繁消息（startTimer / killTimer 在 hot path）
    if (msg.startsWith("QObject::startTimer:") || msg.startsWith("QObject::killTimer:"))
        return;

#if !NDEBUG // 调试版：输出到 VS 的 Output 窗口 + 写日志文件
    txt = QString("[%1][%2]%3 (file:%4:%5, fun:%6)")
              .arg(time)
              .arg(type_str)
              .arg(msg)
              .arg(file.fileName())
              .arg(context.line)
              .arg(context.function);

    Output(txt.toStdWString().c_str());
    // ★ Debug 模式也写文件，方便事后分析（不只是 VS 里看）
    {
        Logger& logger = Logger::instance();
        logger.log(txt);
    }
#else // 发布版：只把 warning 及以上写入日志文件
    if (type <= QtDebugMsg)
        return;

    txt = QString("[%1][%2]%3").arg(time).arg(type_str).arg(msg);

    // ★ Logger 是单例，整个进程共用一个 log.txt
    Logger& logger = Logger::instance();
    logger.log(txt);
#endif
}

/**
 * @brief Logger 构造函数
 * 打开 log.txt 文件（追加模式），创建 QTextStream
 *
 * ★ 为什么要 make_unique 包装 QFile？
 *   QFile 是 RAII 类型，用智能指针可以自动 close，
 *   析构里再 close 一次保险。
 *
 * ★ 为什么用 Append 而不是 WriteOnly？
 *   Append 表示追加写，多次启动程序不会清空旧日志，
 *   方便回溯历史问题。
 *
 * @param file 日志文件路径
 */
Logger::Logger(const QString& file) : m_logfile(nullptr), m_ts(nullptr)
{
    m_logfile = std::make_unique<QFile>(file);
    if (m_logfile)
    {
        // ★ 追加模式打开，UTF-8 编码
        if (m_logfile->open(QIODevice::WriteOnly | QIODevice::Append))
        {
            m_ts = std::make_unique<QTextStream>(m_logfile.get());
            // ★ 强制 UTF-8 写盘，避免 Windows 默认 GBK 乱码
            m_ts->setEncoding(QStringConverter::Utf8);
        }
    }
}

/**
 * @brief 析构函数
 * 关闭日志文件，刷新缓冲区
 */
Logger::~Logger()
{
    if (m_logfile)
        m_logfile->close();
}

/**
 * @brief 写入一条日志（自动加换行）
 *
 * ★ 为什么要判 m_ts 是否有效？
 *   构造时 open 失败 m_ts 会是 nullptr，
 *   后续写操作要跳过，避免空指针。
 *
 * @param str 日志内容
 */
void Logger::log(const QString& str)
{
    if (m_ts)
        // ★ Qt::endl 会刷缓冲 + 加平台换行
        (*m_ts) << str << Qt::endl;
}
