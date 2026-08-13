/**
 * @file clickable_slider.h
 * @brief 可点击跳转的进度条
 *
 * 标准的 QSlider 必须拖动滑块才能改变值。本类重写 mousePressEvent，
 * 让用户可以"点击进度条任意位置"直接跳转到对应时间（类似 YouTube 的进度条）。
 */

#pragma once

#include <QMouseEvent>
#include <QSlider>

class ClickableSlider : public QSlider
{
    Q_OBJECT

public:
    explicit ClickableSlider(QWidget* parent = nullptr);
    virtual ~ClickableSlider(){};

signals:
    // 用户点击进度条任意位置时发出，参数是点击位置对应的进度值
    void onClick(int value);

protected:
    // 重写鼠标按下事件，实现"点击跳转"
    void mousePressEvent(QMouseEvent* event) override;
};
