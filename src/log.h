/**
 * @file log.h
 * @brief 日志系统
 *
 * Qt 自带 qDebug/qWarning 等输出函数，本项目做了一点扩展：
 *   1. logOutput()：接管 Qt 的所有日志输出，加上时间戳、文件、行号
 *   2. Logger：单例类，把日志同时写入 "log.txt" 文件
 *
 * 用法：
 *   main.cpp 里调 qInstallMessageHandler(logOutput) 接管 Qt 日志
 *   任何地方用 Logger::instance().log("xxx") 即可写入日志
 */

#pragma once
#include<QtDebug>
#include<QFile>
#include<QTextStream>
#include<QFileInfo>
#include<QTime>
//#include<QTextCodec>
#include<memory>

/**
 * @brief Qt 日志回调函数
 * 由 qInstallMessageHandler 安装，会在每次 qDebug/qInfo/qWarning/qCritical 时调用。
 * 给消息加上时间戳、文件、行号后，输出到 stderr 并写入日志文件。
 */
void logOutput(const QtMsgType type, const QMessageLogContext& context, const QString& msg);

/**
 * @brief 日志类（单例）
 * 把日志同时输出到控制台和 log.txt 文件。
 * 单例模式：整个程序只有一个 Logger 实例。
 */
class Logger {
public:
    // 单例访问点
    static Logger& instance() {
        static Logger instance;
        return instance;
    }

    // 写入一条日志
    void log(const QString& str);

private:
    explicit Logger(const QString& file = "log.txt");
    virtual ~Logger();

private:
    std::unique_ptr<QFile> m_logfile;       // 日志文件
    std::unique_ptr<QTextStream> m_ts;     // 文件写入流
};
