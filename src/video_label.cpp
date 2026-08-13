// ***********************************************************/
// video_label.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 视频显示标签实现（双击全屏、ESC 退出全屏）
// ***********************************************************/

#include <QApplication>
#include "video_label.h"
#include "mainwindow.h"

/**
 * @brief 构造函数：直接复用 QLabel 的默认构造
 *
 * @param parent 父控件指针（通常是 MainWindow 的 centralWidget）
 */
VideoLabel::VideoLabel(QWidget* parent) : QLabel(parent)
{
}

/**
 * @brief 析构函数：空实现，没有需要手动释放的资源
 */
VideoLabel::~VideoLabel()
{
}

/**
 * @brief 键盘事件：把 ESC/F 转发给主窗口处理
 *
 * VideoLabel 自己没有全屏切换逻辑，捕获到这些键后
 * 通过 sendEvent 转给祖父窗口（即 MainWindow）做统一处理。
 *
 * @param event 键盘事件对象
 */
void VideoLabel::keyPressEvent(QKeyEvent* event)
{
    switch (event->key())
    {
        case Qt::Key_Escape:
        case Qt::Key_F:
        {
            // ★ 把事件转发给祖父窗口（即主窗口）
            // parent() 是 centralWidget，parent()->parent() 才是 MainWindow
            QApplication::sendEvent(parent()->parent(), event);
        }
        break;

        default:
            // 其他键按 QLabel 默认行为处理（比如焦点切换）
            QWidget::keyPressEvent(event);
            break;
    }
}

/**
 * @brief 鼠标双击事件：切换全屏/退出全屏
 *
 * 通过 parent()->parent() 找到主窗口，调用它的 show_fullscreen()。
 *
 * @param event 鼠标双击事件对象
 */
void VideoLabel::mouseDoubleClickEvent(QMouseEvent* event)
{
    // ★ 通过父级的父级（祖父窗口）拿到主窗口指针
    if (auto mainWnd = (MainWindow*)(parent()->parent()))
    {
        // ★ 翻转当前全屏状态
        mainWnd->show_fullscreen(!isFullScreen());
    }
}

/**
 * @brief 全屏/退出全屏切换
 *
 * 设计点：进/出全屏时改 windowFlags，是为了确保从子窗口（SubWindow）
 * 变成独立窗口（Window），布局/任务栏行为才会正确。
 *
 * @param bFullscreen true=进入全屏；false=退出全屏
 */
void VideoLabel::show_fullscreen(bool bFullscreen)
{
    if (bFullscreen)
    {
        // ★ 把窗口标志加上 Qt::Window，使它从子窗口变成顶级窗口
        setWindowFlags(windowFlags() | Qt::Window);
        // ★ 先 maximize 再 fullScreen 是一种常见做法，确保首次进入全屏布局正确
        showMaximized();
        showFullScreen();
    }
    else
    {
        // ★ 去掉 Qt::Window 标志，恢复成 centralWidget 里的子窗口
        setWindowFlags(windowFlags() & ~Qt::Window);
        showNormal();
    }
}
