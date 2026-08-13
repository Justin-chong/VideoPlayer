// ***********************************************************/
// clickable_slider.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 可点击跳转的进度条实现
// ***********************************************************/

#include <QDebug>
#include <QStyleOptionSlider>
#include "clickable_slider.h"

/**
 * @brief 构造函数：直接复用 QSlider 的默认构造
 *
 * @param parent 父窗口指针（用于 Qt 对象树管理生命周期）
 */
ClickableSlider::ClickableSlider(QWidget* parent) : QSlider(parent)
{
}

/**
 * @brief 重写鼠标按下事件：让用户可以点击进度条任意位置跳转
 *
 * 默认 QSlider 只响应"按住拖动"，点击空白处不响应。
 * 这里算出点击位置对应的进度值，然后调 setValue() 立即更新，
 * 并通过 onClick 信号通知主窗口。
 *
 * @param event 鼠标按下事件（含点击坐标和按键信息）
 */
void ClickableSlider::mousePressEvent(QMouseEvent* event)
{
    // ★ 拿到当前样式选项，主要为了取得滑块（handle）的几何信息
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    // ★ sr 是滑块把手在进度条上的矩形区域（用于判断用户点的是把手还是空白处）
    auto sr = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);

    // ★ 条件1：必须是左键；条件2：点击位置不能落在滑块把手上
    // 这样用户拖动滑块时走默认逻辑，点空白处时走我们的跳转逻辑
    if (event->button() == Qt::LeftButton && !sr.contains(event->pos()))
    {
        int newVal = 0;
        double normalizedPosition = 0;
        if (orientation() == Qt::Vertical)
        {
            // ★ 垂直进度条：从下往上计算进度（Qt 垂直 slider 默认下大上小）
            // halfHandleHeight 用来把点击位置"夹"在有效范围内，避免点太靠边算出越界值
            auto halfHandleHeight = (0.5 * sr.height()) + 0.5;
            int adaptedPosY = height() - event->pos().y();
            if (adaptedPosY < halfHandleHeight)
                adaptedPosY = halfHandleHeight;
            if (adaptedPosY > height() - halfHandleHeight)
                adaptedPosY = height() - halfHandleHeight;
            auto newHeight = (height() - halfHandleHeight) - halfHandleHeight;
            // 归一化到 0~1 的小数，表示点击处在有效滑动范围内的比例
            normalizedPosition = (adaptedPosY - halfHandleHeight) / newHeight;
        }
        else
        {
            // ★ 水平进度条：从左往右计算进度（最常见的情况）
            // halfHandleWidth 用来把点击位置"夹"在有效范围内
            auto halfHandleWidth = (0.5 * sr.width()) + 0.5;
            int adaptedPosX = event->pos().x();
            if (adaptedPosX < halfHandleWidth)
                adaptedPosX = halfHandleWidth;
            if (adaptedPosX > width() - halfHandleWidth)
                adaptedPosX = width() - halfHandleWidth;
            auto newWidth = (width() - halfHandleWidth) - halfHandleWidth;
            normalizedPosition = (adaptedPosX - halfHandleWidth) / newWidth;
        }

        // ★ 把 0~1 的归一化位置映射回 slider 的 min~max 整数范围
        newVal = minimum() + ((maximum() - minimum()) * normalizedPosition);
        // ★ 如果 slider 设置了反向显示（比如右到左），值要取反
        if (invertedAppearance())
            newVal = maximum() - newVal;

        // ★ 立即更新 slider 当前位置（会触发 valueChanged 信号）
        setValue(newVal);

        // 标记事件已被处理，不再向上传播
        event->accept();

        // ★ 通知主窗口：用户点击了进度条
        // 主窗口里会通过 onClick 信号算出对应时间点，再调 video_seek() 跳转
        emit onClick(this->value());
    }
    else
    {
        // ★ 点击的是滑块把手本身，让 QSlider 走默认的拖动逻辑
        // 这样既能点击跳转，又能拖动滑块
        QSlider::mousePressEvent(event);
    }
}
