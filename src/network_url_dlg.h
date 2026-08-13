/**
 * @file network_url_dlg.h
 * @brief "打开网络 URL" 对话框
 *
 * 菜单 Media -> Open Network Url 弹出的对话框，
 * 让用户输入 HTTP/RTSP 等网络流地址。
 *
 * 极简：一个文本框 + 确定/取消按钮。
 */

#pragma once

#include <QDialog>
#include <memory>
#include "ui_network_url_dlg.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
class NetworkUrlDlg;  // 由 network_url_dlg.ui 生成的 UI 类
};
QT_END_NAMESPACE

class NetworkUrlDlg : public QDialog
{
    Q_OBJECT

public:
    explicit NetworkUrlDlg(QWidget* parent = Q_NULLPTR);
    ~NetworkUrlDlg(){};

public:
    /**
     * @brief 获取用户输入的 URL
     */
    QString get_url() const;

private:
    std::unique_ptr<Ui::NetworkUrlDlg> ui;
};
