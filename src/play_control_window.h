/**
 * @file play_control_window.h
 * @brief 底部播放控制条窗口
 *
 * 位于主窗口底部的控件条，包含：
 *   - 播放/暂停/停止按钮
 *   - 进度条（用 ClickableSlider，可点击跳转）
 *   - 当前时间/总时间显示
 *   - 音量条
 *   - 播放速度条
 *
 * UI 在 play_control_window.ui 里定义。
 */

#pragma once

#include <QSlider>
#include <QWidget>
#include <memory>

QT_BEGIN_NAMESPACE
namespace Ui
{
class play_control_window;  // 由 .ui 文件生成的 UI 类
};
QT_END_NAMESPACE

class PlayControlWnd : public QWidget
{
    Q_OBJECT

public:
    explicit PlayControlWnd(QWidget* parent = Q_NULLPTR);
    ~PlayControlWnd();

public:
    // ===== UI 状态更新 =====
    void update_play_time(int64_t total_secs);                            // 更新当前播放时间显示
    void set_total_time(int64_t hours, int64_t mins, int64_t secs);      // 设置总时长

    // 拿控件的指针（给 MainWindow 用）
    inline QSlider* get_progress_slider() const;
    inline QSlider* get_volume_slider() const;
    inline QSlider* get_speed_slider() const;

    int get_volum_slider_max();         // 音量条最大值
    int get_progress_slider_max();      // 进度条最大值
    int get_progress_slider_value();    // 进度条当前值

    void set_volume_slider(float volume);   // 设置音量条位置
    void clear_all();                       // 清空所有显示（停止时）
    void update_btn_play(bool bPause = true);  // 更新"播放/暂停"按钮图标

    double get_total_time() const;  // 拿到总时长（秒）
    double get_speed() const;       // 拿到当前播放速度
    void speed_adjust(bool up = true);  // 速度 +/-

public:
    // ===== 静态工具函数（与时间格式相关）=====
    static void get_play_time_params(int64_t total_secs, int64_t& hours, int64_t& mins, int64_t& secs);  // 秒数 → 时分秒
    static QString get_play_time(int64_t hours, int64_t mins, int64_t secs);  // 时分秒 → "HH:MM:SS" 字符串
    static inline double get_time_secs(int64_t hours, int64_t mins, int64_t secs);  // 时分秒 → 秒

public slots:
    void volume_muted(int state);  // 音量条被静音
    void speed_changed(int speed);  // 速度条变化

private:
    void enable_progressbar(bool enable = true);
    void enable_slider_vol(bool enable = true);
    void enable_slider_speed(bool enable = true);
    void init_slider_speed();  // 初始化速度条范围
    void clear_time();          // 清空时间显示
    void enable_play_buttons(bool enable = true);
    void update_play_time(int64_t hours, int64_t mins, int64_t secs);
    void set_progress_bar(double total_secs);
    void set_focus_policy();
    void keyPressEvent(QKeyEvent* event) override;  // 响应键盘

private:
    std::unique_ptr<Ui::play_control_window> ui;
    int64_t m_hours{0};   // 当前播放小时
    int64_t m_mins{0};    // 当前播放分钟
    int64_t m_secs{0};    // 当前播放秒
};
