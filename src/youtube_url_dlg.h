/**
 * @file youtube_url_dlg.h
 * @brief "打开 YouTube" 对话框
 *
 * 菜单 Media -> Open Youtube 弹出的对话框。
 * 用户输入 YouTube 链接，并选择下载选项（画质、音视频分离等）。
 *
 * 选项通过 YoutubeUrlData 结构体传递给 YoutubeUrlThread。
 */

#pragma once

#include <QDialog>
#include <memory>

QT_BEGIN_NAMESPACE
namespace Ui
{
class YoutubeUrlDlg;  // 由 youtube_url_dlg.ui 生成的 UI 类
};
QT_END_NAMESPACE

class YoutubeUrlDlg : public QDialog
{
    Q_OBJECT

public:
    explicit YoutubeUrlDlg(QWidget* parent = Q_NULLPTR);
    ~YoutubeUrlDlg();

public:
    // 用户输入的数据
    typedef struct YoutubeUrlData
    {
        QString url;       // YouTube 视频 URL
        QString option;    // 选中的选项文本
        int opt_index{0};  // 选中的选项索引
    } YoutubeUrlData;

public:
    void set_options_index(int id);  // 预设选中的选项
    bool get_data(YoutubeUrlData& data) const;  // 取出用户输入
    int get_options_index() const;  // 当前选中的选项

private:
    // 初始化下拉框的选项（"最佳画质"、"仅音频"等）
    void init_options();
    QString get_options() const;
    QString get_url() const;

private:
    std::unique_ptr<Ui::YoutubeUrlDlg> ui;
    // 可选的所有下载选项
    const static QStringList m_options;
};
