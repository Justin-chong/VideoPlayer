/**
 * @file audio_effect_gl.h
 * @brief OpenGL 音频可视化窗口
 *
 * 接收来自 AudioPlayThread 的音频数据，用 OpenGL 绘制频谱/波形/柱状图等。
 * 继承自 QOpenGLWidget（Qt 提供的 OpenGL 嵌入窗口）。
 *
 * 实际绘制工作委托给 BarHelper（audio_effect_helper.h），本类只负责：
 *   - 创建 OpenGL 上下文
 *   - 接收音频数据
 *   - 触发 paintEvent 重绘
 */

#pragma once

#include <QImage>
#include <QKeyEvent>
#include <QtOpenGLWidgets>
#include "audio_effect_helper.h"
#include "audio_play_thread.h"

class AudioEffectGL : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit AudioEffectGL(QWidget* parent = nullptr);
    virtual ~AudioEffectGL(){};

public:
    // 接收一段新的音频数据，存起来等重绘
    void paint_data(const AudioData& data);
    // 清空画面（停止播放时调用）
    void paint_clear()
    {
        m_data.len = 0;
        repaint();
    };
    // 设置绘制格式（柱状图/折线图/饼图 + 采样/频域）
    void set_draw_fmt(const BarHelper::VisualFormat& fmt) { m_helper.set_draw_fmt(fmt); }

signals:
    // 窗口被关闭时发出
    void hiden(bool bSend = false);

protected:
    void paintEvent(QPaintEvent* event) override;  // Qt 重绘事件
    void keyPressEvent(QKeyEvent* event) override; // ESC 关闭窗口
    void closeEvent(QCloseEvent* event) override;  // 关闭事件

private:
    QImage m_img;             // 离屏图像（暂未使用）
    BarHelper m_helper;       // 实际绘制逻辑
    AudioData m_data;         // 当前要绘制的音频数据
};
