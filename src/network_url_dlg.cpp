// ***********************************************************/
// network_url_dlg.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 网络流媒体 URL 输入对话框
//   - 让用户输入 http/rtsp 等网络视频地址
//   - MainWindow 用 get_url() 取出后丢给 FFmpeg 播放
// ***********************************************************/

#include <QMessageBox>
#include "network_url_dlg.h"

/* some public test stream video urls
 * http://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4
 * http://commondatastorage.googleapis.com/gtv-videos-bucket/sample/ForBiggerBlazes.mp4
 * http://commondatastorage.googleapis.com/gtv-videos-bucket/sample/ForBiggerEscapes.mp4
 * http://commondatastorage.googleapis.com/gtv-videos-bucket/sample/ForBiggerFun.mp4
 * http://commondatastorage.googleapis.com/gtv-videos-bucket/sample/ForBiggerJoyrides.mp4
 * rtsp://rtsp.stream/pattern
 * rtsp://rtsp.stream/movie
 */

/**
 * @brief 构造函数：初始化 UI 并设置窗口属性
 *
 * 负责加载 .ui 布局文件，并对窗口做"置顶 + 去掉最大化最小化和帮助按钮"的设置。
 * 这些设置是为了让对话框更像一个"工具型小窗口"：始终浮在主窗口之上，
 * 不会被最小化/最大化按钮干扰，避免遮挡主窗口的操作。
 *
 * @param parent 父窗口指针（通常是 MainWindow），用于在父窗口上居中显示
 *               以及在父窗口被销毁时一起释放。本对话框不一定要在父窗口上
 *               显示（用 setWindowFlags 把它提升为独立窗口），但保留父指针
 *               是 Qt 推荐做法，方便 Qt 自动管理内存。
 */
NetworkUrlDlg::NetworkUrlDlg(QWidget* parent)
    : QDialog(parent), ui(std::make_unique<Ui::NetworkUrlDlg>())
{
    // ★ 必须先 setupUi，否则 ui->xxx 全部为 nullptr
    ui->setupUi(this);
    // ★ 把 .ui 里默认的 gridLayout 设为对话框的主布局，
    //   这样 setLayout 之后窗口大小会按内容自动调整
    setLayout(ui->gridLayout);

    auto flags = windowFlags();
    // ★ Qt::WindowStaysOnTopHint：让对话框始终浮在主窗口之上，
    //   用户在主窗口上看视频时仍能看到这个输入框
    flags |= Qt::WindowStaysOnTopHint;
    // ★ 隐藏"最小化/最大化"按钮——对话框不应该被最小化或最大化
    flags &= (~Qt::WindowMinMaxButtonsHint);
    // ★ 隐藏标题栏右上角的"?"帮助按钮，保持界面简洁
    flags &= (~Qt::WindowContextHelpButtonHint);

    // ★ 一次性把上面所有标志位应用给窗口
    setWindowFlags(flags);
}

/**
 * @brief 获取用户在输入框中填写的网络 URL
 *
 * 调用时机：MainWindow 在 exec() 返回 QDialog::Accepted（用户点了"确定"）
 * 之后才调这个函数去取 URL；如果用户点了"取消"，通常不会调用，
 * 直接丢弃整个对话框。
 *
 * @return QString 用户输入的 URL 字符串（不做 trim/校验，调用方自行处理）
 */
QString NetworkUrlDlg::get_url() const
{
    // ★ 直接返回 lineEdit 的原始 text，不做 trim/校验，
    //   因为同一个 URL 输入框可能要兼容 http/rtsp/rtmp 等多种协议，
    //   校验交给后面的 FFmpeg 层更合适（FFmpeg 内部能给出明确错误信息）
    return ui->lineEdit->text();
}
