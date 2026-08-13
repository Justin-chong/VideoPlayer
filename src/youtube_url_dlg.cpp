// ***********************************************************/
// youtube_url_dlg.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// YouTube URL 输入对话框
//   - 接受 YouTube 视频链接
//   - 让用户选下载质量（best / worst / bestvideo ...）
//   - MainWindow 用 get_data() 拿数据，再调 pytube 解析真实流地址
// ***********************************************************/

#include <QMessageBox>
#include "youtube_url_dlg.h"
#include "ui_youtube_url_dlg.h"

/* https://github.com/ytdl-org/youtube-dl  FORMAT SELECTION */
// ★ 这 6 个选项和 youtube-dl 工具的 "-f" 参数对齐，pytube 脚本也认这些 key
const QStringList YoutubeUrlDlg::m_options = {
    "best", "worst", "bestvideo", "worstvideo", "bestaudio", "worstaudio"};

/**
 * @brief 构造：装 UI + 设置窗口属性 + 绑定 OK/Cancel + 初始化下拉框
 *        OK -> accept()，Cancel -> reject()，由 exec() 返回值告诉调用方
 *
 * ★ 为什么要加 WindowStaysOnTopHint？
 *   模态对话框挂在最前面，避免被主窗口挡住。
 *
 * ★ 为什么要去掉 Min/Max 按钮和 Help 按钮？
 *   简单对话框不需要这些按钮，界面更干净。
 *
 * @param parent 父窗口（一般是 MainWindow）
 */
YoutubeUrlDlg::YoutubeUrlDlg(QWidget* parent) : QDialog(parent), ui(std::make_unique<Ui::YoutubeUrlDlg>())
{
    ui->setupUi(this);
    setLayout(ui->gridLayout);

    auto flags = windowFlags();
    // ★ 让对话框保持最前
    flags |= Qt::WindowStaysOnTopHint;
    // ★ 去掉 Min/Max 和 Help 按钮
    flags &= (~Qt::WindowMinMaxButtonsHint);
    flags &= (~Qt::WindowContextHelpButtonHint);

    setWindowFlags(flags);

    // ★ OK 按钮：触发 accept()，exec() 返回 QDialog::Accepted
    QObject::connect(ui->btn_Ok, SIGNAL(clicked()), this, SLOT(accept()));
    // ★ Cancel 按钮：触发 reject()，exec() 返回 QDialog::Rejected
    QObject::connect(ui->btn_Cancel, SIGNAL(clicked()), this, SLOT(reject()));

    // ★ 把 m_options 灌进 comboBox，默认选第 0 项
    init_options();
}

YoutubeUrlDlg::~YoutubeUrlDlg()
{
}

/**
 * @brief 把 m_options 写到下拉框 + 默认选第 0 项
 *
 * ★ 为什么要 if (auto pCombox = ui->comboBox)？
 *   防御性写法：万一 UI 里没这个控件，也不至于崩溃。
 */
void YoutubeUrlDlg::init_options()
{
    if (auto pCombox = ui->comboBox)
        pCombox->addItems(m_options);
    // ★ 默认选 "best"
    set_options_index(0);
}

/**
 * @brief 取当前选中的选项文字
 *
 * @return 当前 comboBox 文字（"best" / "worstvideo" ...）
 */
QString YoutubeUrlDlg::get_options() const
{
    return ui->comboBox->currentText();
}

/**
 * @brief 取用户输入的 YouTube URL
 *
 * @return lineEdit 里输入的字符串
 */
QString YoutubeUrlDlg::get_url() const
{
    return ui->lineEdit->text();
}

/**
 * @brief 取当前下拉框索引
 *
 * @return comboBox 当前索引（从 0 开始）
 */
int YoutubeUrlDlg::get_options_index() const
{
    return ui->comboBox->currentIndex();
}

/**
 * @brief 设置下拉框当前项
 *
 * ★ 越界时为什么要强制设 0？
 *   comboBox 设非法索引会触发 Qt 警告（甚至异常），
 *   兜底成 0 至少能正常显示一个选项。
 *
 * @param id 目标索引
 */
void YoutubeUrlDlg::set_options_index(int id)
{
    if (id < 0 || id >= m_options.size())
        id = 0;

    ui->comboBox->setCurrentIndex(id);
}

/**
 * @brief 一次性把 url/option/index 打包到 YoutubeUrlData
 *
 * ★ 为什么不校验 URL 合法性？
 *   校验放调用方（pytube 解析时自然会失败），这里只负责搬运数据。
 *
 * @param data [out] 输出 YoutubeUrlData
 * @return 始终 true（占位，预留校验扩展）
 */
bool YoutubeUrlDlg::get_data(YoutubeUrlData& data) const
{
    data.url = get_url();
    data.option = get_options();
    data.opt_index = get_options_index();
    return true;
}
