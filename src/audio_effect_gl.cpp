// ***********************************************************/
// audio_effect_gl.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 音频可视化窗口 - 继承自 QOpenGLWidget（用 OpenGL 加速）
//   - 显示当前音频的"频谱"或"采样"可视化效果
//   - 支持多种图形：柱状图、折线图、扇形图、采样点
//   - 实际绘制逻辑在 audio_effect_helper.cpp 里
//   - 音频数据从 MainWindow 定时推过来（push_data）
// ***********************************************************/

#include <QApplication>
#include <QDebug>
#include <QPainter>
#include "audio_effect_gl.h"

/**
 * @brief 构造：创建音频可视化窗口
 *
 * @param parent 父窗口（一般是主窗口 MainWindow）
 *
 * 主要工作：
 *   1. 设置窗口标志（置顶、不可拉伸太小）
 *   2. 设置最小尺寸 480x280（太小画不下）
 *   3. 加载背景图（资源 :images/res/music.png）
 *
 * 父类 QOpenGLWidget 会自动创建 OpenGL 上下文，无需手动管
 */
AudioEffectGL::AudioEffectGL(QWidget* parent) : QOpenGLWidget(parent)
{
    auto flags = windowFlags();
    // ★ Qt::Window：作为独立窗口（不是内嵌的子控件）
    flags |= Qt::Window;
    // ★ WindowStaysOnTopHint：窗口永远置顶
    flags |= Qt::WindowStaysOnTopHint;
    // ★ 去掉帮助按钮（窗口右上角的 ? 按钮）
    flags &= (~Qt::WindowContextHelpButtonHint);
    // flags &= (~Qt::WindowMinMaxButtonsHint);

    setWindowFlags(flags);

    int width = 480;
    int height = 280;

    // setFixedSize(width, height);
    // ★ 只设最小尺寸，不设固定，让用户可以拉大
    setMinimumWidth(width);
    setMinimumHeight(height);

    setWindowTitle("Audio visualization");
    // ★ 自动用背景色填充，避免 OpenGL 初始化前出现黑屏
    setAutoFillBackground(true);

    // ★ 从 Qt 资源系统加载背景图（:是 Qt 资源前缀）
    m_img = QImage(":/images/res/music.png");
}

/**
 * @brief 窗口被关闭时：只隐藏，不要销毁
 *        同时发 hiden 信号，让 MainWindow 知道可以重启数据推送
 *
 * @param event 关闭事件
 *
 * 设计意图：这样下次再"显示"时不用重新分配 OpenGL 上下文，性能更好
 */
void AudioEffectGL::closeEvent(QCloseEvent* event)
{
    // ★ 隐藏而不是关闭（区别：close 会触发析构）
    hide();
    // ★ 发信号通知外面（MainWindow 收到后会停止推数据或重置状态）
    emit hiden();
    // ★ 接受事件，阻止默认的"关闭窗口"行为
    event->accept();
}

/**
 * @brief 重绘事件（Qt 自动调用）
 *        这里只是简单地把绘制委托给 BarHelper（m_helper）
 *
 * @param event 绘制事件（包含需要重绘的区域）
 *
 * 流程：
 *   1. 创建 QPainter
 *   2. 开启抗锯齿（让线条更平滑）
 *   3. 调 m_helper.paint 让 BarHelper 实际画图
 *   4. 结束 QPainter
 */
void AudioEffectGL::paintEvent(QPaintEvent* event)
{
    QPainter painter;
    // ★ 必须 begin/end 配对使用
    painter.begin(this);
    // ★ Antialiasing 让柱状图/折线图的边缘更平滑（代价：稍慢）
    painter.setRenderHint(QPainter::Antialiasing);
#if 0
    // 旧版：直接画背景图，没用上
	painter.drawImage(rect(), m_img);
#else
    // ★ 实际绘制委托给 helper（m_data 是当前要画的音频数据）
    m_helper.paint(&painter, event, m_data);
#endif
    painter.end();
}

/**
 * @brief 按键事件：按 Esc 关闭窗口
 *
 * @param event 按键事件
 *
 * 设计意图：方便用户快速关闭浮窗
 */
void AudioEffectGL::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
    {
        // ★ Esc 键 -> 隐藏窗口
        hide();
        event->accept();
    }
    else
    {
        // ★ 其它键交给父类（不然其它键可能不响应）
        QOpenGLWidget::keyPressEvent(event);
    }
}

/**
 * @brief 外部推数据进来：存到 m_data + 立即重绘
 *
 * @param data 一段 PCM 音频数据（来自 AudioPlayThread 回调）
 *
 * 调用者：MainWindow 收到音频线程的回调后会调本函数
 */
void AudioEffectGL::paint_data(const AudioData& data)
{
    // ★ 先存数据（注意：AudioData 内部 buffer 是浅拷贝，需保证 data 生命周期）
    m_data = data;
    // qDebug() << "p=" << &data << "datalen:" << data.len;
    // ★ 触发 paintEvent
    repaint();
}
