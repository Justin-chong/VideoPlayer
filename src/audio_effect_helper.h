/**
 * @file audio_effect_helper.h
 * @brief 音频可视化绘制助手
 *
 * 把音频 PCM 数据转换成可视化所需的数值，并绘制成图形。
 *
 * 两种数据来源：
 *   - Sampleing（采样）：直接显示音频波形
 *   - Frequency（频域）：用 FFT 算出来后画频谱
 *
 * 三种图形：
 *   - Bar（柱状图）
 *   - Line（折线图）
 *   - Pie（饼图/极坐标）
 */

#pragma once
#include <QBrush>
#include <QFont>
#include <QPen>
#include <QWidget>
#include "audio_play_thread.h"

class BarHelper
{
public:
    explicit BarHelper();
    virtual ~BarHelper(){};

public:
    // 图形类型
    enum GraphicType
    {
        e_GtBar,   // 柱状图
        e_GtLine,  // 折线图
        e_GtPie    // 饼图
    };
    // 数据来源类型
    enum VisualType
    {
        e_VtSampleing,   // 采样波形
        e_VtFrequency    // 频域（FFT 后的频谱）
    };
    // 上面两个组合起来的"显示格式"
    typedef struct VisualFormat
    {
        GraphicType gType;
        VisualType vType;

        VisualFormat() : gType(e_GtBar), vType(e_VtSampleing) {}  // 默认：采样柱状图
    } VisualFormat;

public:
    // 主入口：绘制音频可视化
    void paint(QPainter* painter, QPaintEvent* event, const AudioData& data);
    // 按指定格式绘制
    void draw_data_style(QPainter* painter, const QRect& rt, const AudioData& data);
    // 设置显示格式
    void set_draw_fmt(const VisualFormat& fmt) { m_visualFmt = fmt; }

private:
    // 从原始 PCM 数据里提取要显示的数值
    void get_data(const AudioData& data, std::vector<int>& v, bool left = true) const;
    // 数据归一化（缩放到 [0, height]）
    void normal_data(std::vector<int>& v, const int height);
    void normal_overzero(std::vector<int>& v);  // 中心化
    void normal_audio_to_size(std::vector<int>& v, const int size);
    void normal_to_size(std::vector<int>& v, const int size);
    void data_sample_old(std::vector<int>& v, const uint32_t num);  // 老式采样
    void data_sample(std::vector<int>& v, const uint32_t num);      // 采样
    void binary_data(std::vector<int>& v);
    void data_frequency(std::vector<int>& v, const uint32_t num);   // 频域（FFT）

    // 各种图形的具体绘制
    void draw_data_bar(QPainter* painter, std::vector<int>& data, int n, int w, int h, int h_inter);
    void draw_data_line(QPainter* painter, std::vector<int>& data, int n, int w, int h, int h_inter);
    void draw_data_arc(QPainter* painter, std::vector<int>& data, int n, int w, int h);
    void draw_data_polygon(QPainter* painter, std::vector<int>& data, int n, int w, int h, int r_offset = 50);

private:
    QBrush m_background;  // 背景刷
    QBrush m_brush;       // 前景刷
    QFont m_textFont;     // 字体
    QPen m_pen;           // 画笔
    QPen m_textPen;       // 文字画笔
    AudioFrameFmt m_datafmt;  // 数据格式（采样率等）
    VisualFormat m_visualFmt; // 当前显示格式
};
