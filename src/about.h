/**
 * @file about.h
 * @brief "关于"对话框
 *
 * 菜单 Help -> About 弹出来的窗口，显示版本号、Qt/FFmpeg/OpenCV 版本、版权信息。
 */

#pragma once

#include <QDialog>
#include <memory>
#include "version.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
class About;  // 由 about.ui 生成的 UI 类
};
QT_END_NAMESPACE

class About : public QDialog
{
    Q_OBJECT

public:
    explicit About(QWidget* parent = Q_NULLPTR);
    virtual ~About();

private:
    // 初始化"关于"对话框里的内容（版本号、链接等）
    void init_label();

private:
    std::unique_ptr<Ui::About> ui;  // 由 .ui 文件生成的 UI 控件
};
