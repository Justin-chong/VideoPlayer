// ***********************************************************/
// about.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// About dialog
// ★ 暂时移除 #include <opencv2/opencv.hpp>：OpenCV 4.9 总头会强制 include objdetect
//   改用 ffmpeg_init.h 里手动定义的 OPENCV_VERSION
// ***********************************************************/

// #include <opencv2/opencv.hpp>   // ★ 暂时禁用：OpenCV 4.9 objdetect 头文件依赖 aruco_contrib
#include "about.h"
#include "ffmpeg_init.h"
#include "ui_about.h"

/**
 * @brief About 对话框构造函数
 *
 * 1. 加载 .ui 描述文件
 * 2. 调整窗口标志（常驻顶层 + 去掉最小/最大化 + 去掉帮助按钮）
 * 3. 调用 init_label() 填充"关于"信息
 *
 * @param parent 父窗口指针（通常是 MainWindow）
 */
About::About(QWidget* parent) : QDialog(parent), ui(std::make_unique<Ui::About>())
{
    ui->setupUi(this);
    setLayout(ui->verticalLayout);

    // ★ 调整窗口标志，让"关于"对话框在所有窗口之上，方便用户随时看到
    auto flags = windowFlags();
    flags |= Qt::WindowStaysOnTopHint;   // ★ 始终在最上层
    flags &= (~Qt::WindowMinMaxButtonsHint);  // ★ 去掉最小化/最大化按钮（关于窗口不需要）
    flags &= (~Qt::WindowContextHelpButtonHint);  // ★ 去掉标题栏的 ? 帮助按钮

    setWindowFlags(flags);

    init_label();
}

/**
 * @brief 析构函数：空实现（unique_ptr 会自动释放 ui）
 */
About::~About()
{
}

/**
 * @brief 填充"关于"对话框里的两个 label
 *
 * 第一个 label 显示版本号 + 第三方库版本（带超链接）
 * 第二个 label 显示作者署名
 *
 * 设计点：用 HTML 富文本，让 QT/FFmpeg/OpenCV 等字样能点击直达官网
 */
void About::init_label()
{
    // ★ 文本左对齐（用左对齐更便于扫读多行信息）
    auto align = Qt::AlignLeft; //Qt::AlignCenter;

    // ★ 用 HTML 格式拼接"关于"信息
    QString str;
    // 第一行：播放器版本（粗体）
    str += "<b>Video Player v";
    str += PLAYER_VERSION;
    str += " (x64)</b>";
    str += "<br>";
    str += "<br>";

    // ★ 第三方库版本（用 HTML 链接让用户能直接点开官网）
    str += QString("<a href=\"https://www.qt.io/\">QT</a> Version: %1<br>").arg(qVersion());
    str += QString("<a href=\"https://www.ffmpeg.org/\">FFmpeg</a> Version: %1<br>").arg(FFMPEG_VERSION);
    // ★ 暂时禁用 OpenCV 版本：opencv.hpp 头文件依赖 aruco_contrib
    //   如需恢复，请装带 contrib 的 OpenCV 并取消上面的 include 注释
    // str += QString("<a href=\"https://opencv.org/\">OpenCV</a> Version: %1<br>").arg(CV_VERSION);
    // ★ 编译时间戳（从编译器宏 __TIMESTAMP__ 取得）
    str += QString("Datetime: %1<br>").arg(__TIMESTAMP__);

    // ★ 用 QApplication::translate 让这段字符串也能被翻译
    str = QApplication::translate("about", str.toStdString().c_str(), Q_NULLPTR);
    // ★ 设置 label 为富文本模式（这样 HTML 链接才能点）
    ui->label->setTextFormat(Qt::RichText);
    // ★ 设置文本交互标志：允许点击链接
    ui->label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    // ★ 允许自动打开外部链接
    ui->label->setOpenExternalLinks(true);
    ui->label->setAlignment(align);
    ui->label->setText(str);

    // ★ 第二行：作者署名（当前不附带项目主页链接，需要时再补）
    str = "\nCopy Right @ lichong\n";
    str = QApplication::translate("about", str.toStdString().c_str(), Q_NULLPTR);
    ui->label_name->setTextFormat(Qt::RichText);
    ui->label_name->setTextInteractionFlags(Qt::TextBrowserInteraction);
    ui->label_name->setOpenExternalLinks(true);
    ui->label_name->setAlignment(align);
    ui->label_name->setText(str);
}
