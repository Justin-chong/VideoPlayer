/**
 * @file video_label.h
 * @brief 视频显示标签
 *
 * 继承自 QLabel，专门用来显示视频帧（QImage）。
 * 主要扩展：
 *   1. 双击切换全屏
 *   2. 全屏时再双击退出
 *   3. 按 ESC 退出全屏
 *
 * 整个播放窗口里就这一个 Label 用来显示视频画面。
 */

#pragma once

#include <QKeyEvent>
#include <QLabel>

class VideoLabel : public QLabel
{
    Q_OBJECT

public:
    explicit VideoLabel(QWidget* parent = Q_NULLPTR);
    virtual ~VideoLabel();

public:
    /**
     * @brief 切换全屏状态
     * @param bFullscreen true=进入全屏，false=退出全屏
     */
    void show_fullscreen(bool bFullscreen = true);

private:
    // 响应键盘按键（ESC 退出全屏等）
    void keyPressEvent(QKeyEvent* event) override;
    // 响应鼠标双击（双击切换全屏）
    void mouseDoubleClickEvent(QMouseEvent* event) override;
};
