// ***********************************************************/
// main.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 程序入口文件
// 这个文件非常短，但包含了 Qt 程序启动的几个必备步骤：
//   1. 创建 QApplication（每个 Qt GUI 程序必须有，且只能有一个）
//   2. 安装自定义日志 handler
//   3. 根据系统语言加载翻译文件（i18n，国际化）
//   4. 创建主窗口并显示
//   5. 进入 Qt 事件循环
// ***********************************************************/

#include <QApplication>    // Qt GUI 应用程序类
#include <QLocale>         // 区域设置（用于根据系统语言选择翻译）
#include <QTranslator>     // Qt 翻译器
#include "log.h"           // 自定义日志模块
// #include "vld.h"        // ★ 暂时禁用：Visual Leak Detector 仅 Debug 生效，但 vld.lib 不在构建系统里

#include "mainwindow.h"    // 主窗口类

/**
 * @brief Qt GUI 程序的统一入口
 *
 * 整个播放器从这里启动，按顺序完成：
 *   1. 构造 QApplication（Qt GUI 程序的"操作系统"）
 *   2. 安装自定义日志 handler（把 qDebug 等输出重定向到我们的 logOutput）
 *   3. 根据系统语言尝试加载 .qm 翻译文件
 *   4. 创建主窗口并显示
 *   5. 进入 Qt 事件循环（程序主循环）
 *
 * @param argc 命令行参数个数（Qt 会用到比如 -style 这种标准参数）
 * @param argv 命令行参数数组
 * @return int 程序退出码（exec() 返回的，通常是 0 表示正常退出）
 */
int main(int argc, char* argv[])
{
    // ★ 第 1 步：创建 QApplication
    // QApplication 是所有 Qt GUI 程序必须的对象，全局只能有一个
    // 它在构造时会完成很多底层工作：解析命令行参数、初始化字体、注册事件类型等
    QApplication a(argc, argv);

    // ★ 第 2 步：接管 Qt 的日志输出
    // 默认 qDebug/qWarning 走 stderr；装上自定义 handler 后，可以重定向到文件、加时间戳等
    qInstallMessageHandler(logOutput); // log

    // ★ 第 3 步：国际化（i18n）
    // 准备一个翻译器；后面会尝试加载对应语言的 .qm 文件
    QTranslator translator;

    // 遍历系统所有首选语言（按优先级排），找到第一个能加载成功的就用它
    // 比如 zh_CN -> 加载 VideoPlayer_zh_CN.qm
    for (const auto& locale : QLocale::system().uiLanguages())
    {
        // 构造翻译文件名，比如 "VideoPlayer_zh_CN"
        auto baseName = "VideoPlayer_" + QLocale(locale).name();
        // ":/i18n/" 是 Qt 资源系统路径，编译时 .qm 被嵌入到 exe 里
        if (translator.load(":/i18n/" + baseName))
        {
            // 安装到 QApplication，UI 里所有 tr() 字符串会走翻译
            a.installTranslator(&translator);
            break;  // 找到一个能用的就退出循环
        }
    }

    // ★ 第 4 步：构造主窗口并显示
    // 构造里会做大量初始化：建子窗口、连信号槽、初始化 FFmpeg、读设置
    MainWindow w;
    w.show();

    // ★ 调试便利：如果命令行传入了文件路径，直接打开它（不用弹对话框）
    //   用法: VideoPlayer.exe <文件路径>
    //   没有参数就什么都不做（由用户点菜单打开）
    if (argc >= 2)
    {
        QString filePath = QString::fromLocal8Bit(argv[1]);
        qInfo("[main] Command-line file argument: %s", qUtf8Printable(filePath));
        // 异步打开（避免阻塞 Qt 事件循环）
        QMetaObject::invokeMethod(&w, [&w, filePath]() {
            w.start_to_play(filePath);
        }, Qt::QueuedConnection);
    }

    // ★ 第 5 步：进入 Qt 事件循环
    // exec() 内部是个死循环，处理所有 Qt 事件（鼠标、键盘、定时器、信号槽等）
    // 直到调用 QApplication::quit() 或所有顶层窗口都关闭
    return a.exec();
}
