// ***********************************************************/
// audio_effect_helper.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 音频可视化实际绘图实现 - BarHelper
//   - 把 PCM 数据 -> 频谱/采样 -> 柱状/折线/扇形
//   - 入口：paint(painter, event, audioData)
//
// 流程：
//   1. get_data       ：从 int16 PCM 数组取数据（取左声道或全部）
//   2. data_sample    ：把数据重采样到 N 个点（默认 128）
//   3. data_frequency ：或者算频谱直方图
//   4. normal_*       ：归一化到画布尺寸
//   5. draw_data_*    ：画成柱/线/扇形
// ***********************************************************/

#include <QPaintEvent>
#include <QPainter>
#include <QWidget>
#include <QtMath>
#include <algorithm>
#include "audio_effect_helper.h"

// 16bit PCM 音频的最大幅值（用于归一化）
// ★ unsigned 16bit PCM 的最大值是 0xFFFF = 65535
//   实际 signed int16 的范围是 -32768 ~ +32767，但这里用 unsigned 0xffff 做归一化上界
#define MAX_AUDIO_VAULE 0xffff // unsigned 16bit pcm audio max value

/**
 * @brief 构造：初始化画图用的画笔、字体、渐变刷
 *
 * 主要工作：
 *   - 初始化数据格式（16bit、单声道）
 *   - 创建渐变刷（白到绿色，从下到上）
 *   - 准备背景色（暗紫色）
 *   - 设置折线图用的画笔（绿色）
 *   - 设置文字字体（50px，没实际用上）
 */
BarHelper::BarHelper()
{
    m_datafmt.sample_fmt = 16;  // ★ 16 bit
    m_datafmt.channel = 1;      // ★ 单声道（数据归一化前会先 get_data 提一个声道）

    // ★ 线性渐变：从 (50,200) 到 (50,0) 纵向变化
    QLinearGradient gradient(QPointF(50, 200), QPointF(50, 0));
    gradient.setColorAt(0.0, Qt::white);               // 底部白色
    gradient.setColorAt(1.0, QColor(0xa6, 0xce, 0x39));// 顶部绿色

    // ★ 背景色：暗紫色 (R=64, G=32, B=64)
    m_background = QBrush(QColor(64, 32, 64));
    // ★ 前景刷：渐变刷（柱状图用）
    m_brush = QBrush(gradient);
    // ★ 折线图笔：绿色 2 像素
    m_pen = QPen(Qt::green, 2); // QPen(Qt::black);

    m_textPen = QPen(Qt::white);
    // ★ 文字字体（实际没用到，预留）
    m_textFont.setPixelSize(50);
}

/**
 * @brief 画柱状图：在画布上画 n 根柱
 *
 * @param painter  绘制器
 * @param data     高度数据（长度必须等于 n）
 * @param n        柱的根数
 * @param w        画布宽度
 * @param h        画布高度
 * @param h_inter  柱间距
 *
 * 算法：
 *   - 总宽 - 总间距 = 实际可分配给柱子的宽度
 *   - w_step = (w - (n-1) * h_inter) / n
 *   - 柱子从底部往上长（top 越大越高）
 */
void BarHelper::draw_data_bar(QPainter* painter, std::vector<int>& data, int n, int w, int h, int h_inter)
{
    assert(data.size() == n);

    // 算每根柱子的宽度：
    //   总宽 - 柱间距总数 = 实际可分配给柱子的宽度
    //   再除以 n 得到单柱宽度
    auto w_step = (w - (n - 1) * h_inter) * 1.0 / n;
    for (int i = 0; i < n; ++i)
    {
        auto top = data[i]; // rand() % height;
        // QRectF(x, y, width, height)
        // ★ y 用 h-top 是因为 Qt 坐标原点在左上角
        //   要让柱"从下往上长"，所以顶部 y 坐标 = h - top
        QRectF rt(i * (w_step + h_inter), h - top, w_step, top);
        painter->drawRect(rt);
    }
}

/**
 * @brief 画折线图：把相邻两点用直线连起来
 *
 * @param painter  绘制器
 * @param data     折线高度数据
 * @param n        数据点数
 * @param w        画布宽度
 * @param h        画布高度
 * @param h_inter  点间距
 *
 * 用 m_pen（绿色）描线
 */
void BarHelper::draw_data_line(QPainter* painter, std::vector<int>& data, int n, int w, int h, int h_inter)
{
    // ★ 折线图必须先 setPen，不然画出来是默认颜色
    painter->setPen(m_pen);

    auto w_step = (w - (n - 1) * h_inter) * 1.0 / n;
    for (size_t i = 0; i < n - 1; ++i)
    {
        qreal x1 = i * (w_step + h_inter);
        qreal y1 = h - data[i];  // ★ y 翻转：数据大 -> 离画布顶近
        qreal x2 = x1 + (w_step + h_inter);
        qreal y2 = h - data[i + 1];
        // painter->drawRect(rt);
        // ★ 画一根从 (x1,y1) 到 (x2,y2) 的线
        QLine line = QLine(QPoint(x1, y1), QPoint(x2, y2));
        painter->drawLine(line);
    }
}

/**
 * @brief 画扇形图（极坐标下画 n 个扇形）
 *        半径 = data[i] + 固定半径，半径越大扇形越大
 *        圆心在画布中心
 *
 * @param painter 绘制器
 * @param data    每个扇形的数据（决定半径）
 * @param n       扇形数量
 * @param w       画布宽度
 * @param h       画布高度
 *
 * 注意：内部用 painter->translate(center) 把圆心移到画布中心
 *       调用前不要画其它东西，否则 translate 会影响后续
 */
void BarHelper::draw_data_arc(QPainter* painter, std::vector<int>& data, int n, int w, int h)
{
    assert(data.size() == n);
    // ★ 基础半径：所有扇形都至少有 50 像素，避免太靠近圆心
    const qreal s_radius = 50.0;

    // ★ 圆心 = 画布中心
    QPointF center(w * 1.0 / 2, h * 1.0 / 2);
    // ★ 移动坐标系原点到圆心（之后所有坐标都相对圆心）
    painter->translate(center);
    // ★ 扇形填充红色
    painter->setBrush(QBrush(Qt::red));

    // ★ 每个扇形跨度：360 / n 度（QPainter 的角度单位是 1/16 度，所以乘 16）
    const int spanAngle = 360 / n * 16;
    for (int i = 0; i < n; ++i)
    {
        // ★ data[i] 越大，半径越大，扇形越大
        qreal radius = data[i] + s_radius;
        // ★ 扇形外接矩形：圆心在 (0,0)，所以 (-r, -r) 到 (r, r)
        QRect bounds(-radius, -radius, 2 * radius, 2 * radius);
        // ★ 起始角度 = 360 * i / n
        int startAngle = 360 * i / n * 16;  // 同样 *16 是 QtPainter 的特殊单位
        painter->drawArc(bounds, startAngle, spanAngle);
    }
}

/**
 * @brief 画饼图（极坐标下画 n 个梯形）
 *        每个梯形的形状 = (r_offset, r_offset+data) 之间的扇环
 *        4 个顶点用 drawPolygon 画
 *
 * @param painter  绘制器
 * @param data     每个扇环的外半径（决定厚度）
 * @param n        扇环数量
 * @param w        画布宽度
 * @param h        画布高度
 * @param r_offset 内半径（所有扇环共享）
 *
 * 实现：用三角函数把"极坐标"转成"直角坐标"画多边形
 *       qCos/sin + qDegreesToRadians 实现角度 -> 弧度转换
 */
void BarHelper::draw_data_polygon(QPainter* painter, std::vector<int>& data, int n, int w, int h, int r_offset)
{
    assert(data.size() == n);

    // ★ 圆心在画布中心
    QPointF center(w * 1.0 / 2, h * 1.0 / 2);
    // ★ 移动坐标系到圆心
    painter->translate(center);

    // ★ 每个梯形 4 个顶点
    const int pt_size = 4;
    // ★ 扇环之间的角度间隙（0 = 没间隙，扇环紧贴）
    const int inter_a = 0;
    // ★ 内半径 = r_offset
    const qreal s_radius = r_offset;
    QPointF points[pt_size] = {};

    for (int i = 0; i < n; ++i)
    {
        // ★ 外半径 = 内半径 + data[i]
        qreal radius = data[i] + s_radius;

        // ★ 起始角 / 终止角（角度，0~360）
        float startAngle = 360 * i / n;
        float stopAngle = 360 * (i + 1) / n - inter_a;

        // ★ 角度转弧度（三角函数用弧度）
        float start_d = qDegreesToRadians(startAngle);
        float stop_d = qDegreesToRadians(stopAngle);

        // ★ 4 个顶点（顺时针：内弧起点 -> 外弧起点 -> 外弧终点 -> 内弧终点）
        points[0] = QPointF(qCos(start_d) * s_radius, qSin(start_d) * s_radius);
        points[1] = QPointF(qCos(start_d) * radius, qSin(start_d) * radius);
        points[2] = QPointF(qCos(stop_d) * radius, qSin(stop_d) * radius);
        points[3] = QPointF(qCos(stop_d) * s_radius, qSin(stop_d) * s_radius);

        // ★ 用多边形画 4 个顶点（自动闭合 + 填充）
        painter->drawPolygon(points, 4);
    }
}

/**
 * @brief 实际"画数据"的入口（按 m_visualFmt 选择画什么）
 *
 * @param painter 绘制器
 * @param rt      画图区域
 * @param data    音频 PCM 数据
 *
 * 流程：
 *   1. get_data       拿 int16 PCM（提一个声道）
 *   2. normal_overzero 中心化（负值平移到非负）
 *   3. data_sample    或者 data_frequency 选采样/频谱
 *   4. 按 gType 选柱/线/饼
 *
 * 默认 n=128（柱的根数或频谱区间数）
 */
void BarHelper::draw_data_style(QPainter* painter, const QRect& rt, const AudioData& data)
{
    int width = rt.width();
    int height = rt.height();
    // ★ 柱间距 2 像素
    int h_inter = 2;

    // ★ 防御性检查：data.len 不能超过 buffer 容量
    assert(data.len <= BUFFER_LEN);
    std::vector<int> v_data;
    // ★ 1. 从原始 PCM 提一个声道
    get_data(data, v_data);
    if (v_data.size() == 0)
        return;

    // ★ 默认画 128 根柱（或 128 个频谱点）
    int n = 128;

    // ★ 2. 中心化（让最小值 = 0）
    normal_overzero(v_data);
    if (m_visualFmt.vType == e_VtSampleing)
    {
        // ★ 3a. 采样模式：重采样到 n 个点
        data_sample(v_data, n);
        // ★ 把数值缩放到画布高度
        normal_audio_to_size(v_data, height);
    }
    else
    {
        // ★ 3b. 频谱模式：算频谱直方图
        data_frequency(v_data, n);
    }

    // ★ 4. 按图形类型分派
    if (m_visualFmt.gType == e_GtBar)
    {
        draw_data_bar(painter, v_data, n, width, height, h_inter);
    }
    else if (m_visualFmt.gType == e_GtLine)
    {
        draw_data_line(painter, v_data, n, width, height, h_inter);
    }
    else if (m_visualFmt.gType == e_GtPie)
    {
        // ★ 饼图模式：内半径 30，最大半径 = min(w/2-30, h/2-30)
        int r_offset = 30;
        // ★ normal_to_size 缩放到画布（因为极坐标图的最大半径就是 min(w/2, h/2)）
        normal_to_size(v_data, qMin(width / 2 - r_offset, height / 2 - r_offset));
        draw_data_polygon(painter, v_data, n, width, height, r_offset);
    }
    else
    {
        qDebug() << "Not handled yet.\n";
    }
}

/**
 * @brief QWidget::paintEvent 的回调（被 AudioEffectGL::paintEvent 调用）
 *
 * @param painter 绘制器
 * @param event   绘制事件
 * @param data    音频 PCM 数据
 *
 * 流程：
 *   1. 用背景色填充整张画布
 *   2. 调 draw_data_style 画数据
 *
 * save/restore 用来保护 painter 状态：避免改 brush 后影响外部
 */
void BarHelper::paint(QPainter* painter, QPaintEvent* event, const AudioData& data)
{
    auto rt = event->rect();
    // ★ 第一步：填背景色（暗紫色）
    painter->fillRect(rt, m_background);
    // painter->translate(0, -1 * rt.height() / 2);

    // ★ save/restore 配对：保存当前 painter 状态（brush/pen/transform）
    //   防止下面改 brush 后影响外面
    painter->save();
    // ★ 设置柱状图用的渐变刷
    painter->setBrush(m_brush);
    // ★ 实际画图
    draw_data_style(painter, rt, data);
    // ★ 恢复 painter 状态
    painter->restore();
}

/**
 * @brief 从 PCM buffer 取出一个声道的数据
 *
 * @param data 音频数据
 * @param v    输出：提取出的采样数组
 * @param left 是否只取左声道
 *             true（默认）：只取左声道（偶数位 0、2、4...）
 *             false：取所有采样（左+右）
 *
 * PCM 立体声 int16 排布：LRLRLRLR...（左右交错）
 *                     字节：[L0低,L0高, R0低,R0高, L1低,L1高, R1低,R1高, ...]
 */
void BarHelper::get_data(const AudioData& data, std::vector<int>& v, bool left) const
{
    // ★ 先清空输出数组
    v.clear();

    // ★ 把 byte buffer 强转成 int16* 看待（每 2 字节一个采样）
    // unsigned 16bit
    const int16_t* p = (int16_t*)data.buffer;
    // ★ len 按 byte 计，除以 2 拿到 int16 个数
    uint32_t len = data.len / sizeof(int16_t);

    // ★ left=true 时从 0 开始（偶数位 = 左声道），否则从 1 开始
    int start = left ? 0 : 1;

    // ★ 每 2 个 int16 跳一个（左/右声道都取时 i+=1 即可；只取左时 i+=2 跳右声道）
    for (uint32_t i = start; i < len; i += 2)
        v.push_back(*(p + i));

    // qDebug() << "data size: " << v.size();
}

/**
 * @brief 旧版重采样（线性取点）
 *        现在用 binary_data 替代，留作参考
 *
 * @param v   输入 + 输出数组
 * @param num 目标点数
 *
 * 算法：每隔 numItems 个采样取一个（i=0, numItems, 2*numItems, ...）
 *       缺点：只取"前面"的采样，丢掉了"后面"的细节
 */
void BarHelper::data_sample_old(std::vector<int>& v, const uint32_t num)
{
    auto size = v.size();
    if (num <= 0 || size < num)
    {
        // ★ 源数据不够，末尾补 0 到 num 个
        v.insert(v.end(), num - size, 0);
        return;
    }

    auto numItems = size / num;
    if (numItems <= 1)
    {
        // ★ 源数据已经够少，直接截断
        v.erase(v.begin() + num, v.end());
        return;
    }

    for (size_t i = 0; i < num; i++)
    {
        auto& value = v[i];
        // ★ 每 numItems 个采样取一个
        value = v[i * numItems];

        /*for (int j = 1; j < numItems; j++) {
            value += v[i * numItems + j];
        }*/
    }

    // ★ 截掉 num 之后的多余数据
    v.erase(v.begin() + num, v.end());
}

/**
 * @brief 重采样 v 到 num 个点（用"二分合并"实现，比线性取点更平滑）
 *
 * @param v   输入 + 输出数组
 * @param num 目标点数
 *
 * 算法：
 *   - 如果 v.size() / num >= 2，就调用 binary_data 把 v 减半
 *   - 重复到 v.size() / num == 1 为止
 *   - 截断到 num
 *
 *   优点：每次合并相邻两个采样（保留"两边的信息"）
 *        缺点：实现稍复杂
 */
void BarHelper::data_sample(std::vector<int>& v, const uint32_t num)
{
    auto size = v.size();
    if (num <= 0 || size < num)
    {
        // ★ 源数据不够，末尾补 0
        v.insert(v.end(), num - size, 0);
        return;
    }

    auto numItems = size / num;
    if (numItems <= 1)
    {
        // ★ 源数据已经够少，直接截断
        v.erase(v.begin() + num, v.end());
        return;
    }

    // ★ 反复二分直到 size/num == 1
    while (v.size() / num >= 2)
    {
        // ★ binary_data 内部把 v 的相邻两点合并成一点
        binary_data(v);
    }

    // ★ 截掉 num 之后的多余数据
    v.erase(v.begin() + num, v.end());
}

/**
 * @brief 二分合并：每两个相邻采样取前一个
 *        [0,1,2,3,4,5] -> [0,2,4]
 *        size 减半
 *
 * @param v 输入 + 输出数组（in-place 改写）
 *
 * 注意：只能从前往后写，否则会覆盖还没读的数据
 */
void BarHelper::binary_data(std::vector<int>& v)
{
    auto j = 0;
    // ★ i 步长为 2：取 v[0], v[2], v[4]...
    //   写到 v[0], v[1], v[2]...（从前向后写，安全）
    for (size_t i = 0; i < v.size(); i += 2)
        v[j++] = v[i];

    // ★ 截掉后半部分
    v.erase(v.begin() + j, v.end());
}

/**
 * @brief 把 v 里所有值整体上移，使最小值 = 0
 *        适合 PCM（有正有负）转柱状图（要非负）
 *
 * @param v 输入 + 输出数组（in-place 改写）
 *
 * 算法：
 *   - 找最小值 min
 *   - 如果 min < 0，把 v 里所有值都加上 |min|，最小值变成 0
 */
void BarHelper::normal_overzero(std::vector<int>& v)
{
    int min = *std::min_element(v.begin(), v.end());
    // int max = *std::max_element(v.begin(), v.end());
    // qDebug() << "before size: " << v.size() << ",Max value: " << max << ",Min
    // value: " << min;

    if (min < 0)
    {
        // ★ 整体平移：min = 0
        min *= -1;
        for (int& x : v)
            x += min;
    }
}

/**
 * @brief 把 v 归一化到 [0, size]
 *        用 16bit PCM 上限 0xffff 作为参考最大值
 *
 * @param v    输入 + 输出数组
 * @param size 目标最大值
 *
 * 算法：v[i] = v[i] * size / maxValue，先 clamp 再缩放
 *       maxValue 选 0xffff 是因为 16bit PCM 的理论最大幅度就是 0xffff
 */
void BarHelper::normal_audio_to_size(std::vector<int>& v, const int size)
{
    // ★ 防御性检查：size <= 0 直接返回（避免除 0）
    if (size <= 0)
        return;

#if 0
    // 备用方案：动态记录 maxValue（学习真实音频的最大值）
    static int maxValue = 0xff;

    int max = *std::max_element(v.begin(), v.end());
    if (maxValue < max)
        maxValue = max;
#else
    // ★ 推荐：用 16bit PCM 理论最大值
    int maxValue = MAX_AUDIO_VAULE;
#endif

    for (auto& x : v)
    {
        // ★ 先 clamp 到 maxValue（防溢出）
        x = (x > maxValue) ? maxValue : x;
        // ★ 线性缩放：v[i] * size / maxValue
        x = (x * size) / maxValue;
    }
}

/**
 * @brief 频谱模式下用的归一化（maxValue 用实际最大值，不用 0xffff）
 *
 * @param v    输入 + 输出数组
 * @param size 目标最大值
 *
 * 和 normal_audio_to_size 的区别：
 *   - 这里 maxValue = 实际 v 里的最大值（更小）
 *   - 适合频谱直方图（data_frequency 输出），让最大值正好顶到画布
 */
void BarHelper::normal_to_size(std::vector<int>& v, const int size)
{
    if (size <= 0)
        return;

    // ★ maxValue 用 v 的实际最大值
    int maxValue = *std::max_element(v.begin(), v.end());
    // ★ 如果 size 已经 >= maxValue，v 不用缩放
    if (size >= maxValue)
        return;

    for (auto& x : v)
    {
        x = (x > size) ? size : x;
        x = (x * size) / maxValue;
    }
}

/**
 * @brief 串联 normal_overzero + normal_audio_to_size
 *        一步完成"中心化 + 缩放"
 *
 * @param v      输入 + 输出数组
 * @param height 目标画布高度（缩放上限）
 */
void BarHelper::normal_data(std::vector<int>& v, const int height)
{
    // ★ 先平移最小值到 0（处理 PCM 的负值）
    normal_overzero(v);
    // ★ 再缩放到 [0, height]
    normal_audio_to_size(v, height);
}

/**
 * @brief 算"频谱直方图"：把每个采样按值大小归到 num 个桶里
 *        桶号 = (sample % maxValue) / step
 *        每个桶里有多少个采样 -> 那根柱子的高度
 *
 * @param v   输入 + 输出数组（in-place：原数据被替换为频谱）
 * @param num 频谱区间数（默认 128）
 *
 * 实现细节：
 *   - step = ceil((maxValue + 1) / num)  // 桶宽度
 *   - 对每个采样：index = (sample % maxValue) / step
 *   - res[index]++ （该桶计数 +1）
 *   - 最后 v = res
 *
 * 注意：这不是真正的 FFT，只是按值大小做的"直方图分布"
 *       能粗略展示音频的"响度分布"
 */
void BarHelper::data_frequency(std::vector<int>& v, const uint32_t num)
{
    auto size = v.size();
    if (num <= 0 || size < num)
    {
        // ★ 源数据不够，末尾补 0
        v.insert(v.end(), num - size, 0);
        return;
    }

    // ★ num 个桶，初始为 0
    std::vector<int> res(num, 0);
#if 0
    // 备用方案：动态记录 maxValue
    static uint32_t maxValue = 0xff;

    int max = *std::max_element(v.begin(), v.end());
    if (maxValue < max)
        maxValue = max;
#else
    // ★ 推荐：用 16bit PCM 理论最大值（0xffff = 65535）
    int maxValue = MAX_AUDIO_VAULE;
#endif
    // ★ 每个桶的宽度（向上取整，保证最后一桶也能装下）
    const uint16_t step = ceil((maxValue + 1) * 1.0 / num);
    for (const auto& i : v)
    {
        // ★ 算桶号：先 % 拿范围 [0, maxValue)，再 /step 拿桶号
        uint32_t index = (i % maxValue) / step;
        assert(index < num);
        res[index % num]++;  // 累加计数
    }

    // ★ 把频谱结果赋回 v
    v = res;
}
