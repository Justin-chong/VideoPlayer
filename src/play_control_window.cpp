// ***********************************************************/
// play_control_window.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 播放控制面板：
//   - 进度条 / 音量条 / 倍速条
//   - 播放、暂停、停止、上一首、下一首按钮
//   - 静音复选框
//   - 当前/总时长显示
//
// 这个面板不直接处理播放逻辑，所有按钮都转给 MainWindow
// （这样可以把"控件"和"业务"解耦，控件只管 UI）
// ***********************************************************/

#include "play_control_window.h"
#include "mainwindow.h"
#include "ui_play_control_window.h"
#include "clickable_slider.h"  // ★ 修复：ClickableSlider 未声明错误

// 倍速步长：0.25 一档
// ★ 用宏而不是常量：编译期就替换，零运行时开销
#define PLAY_SPEED_STEP 0.25
// 最小倍速：0.5x
// ★ 设为 0.5 是因为 0.25x 听起来太"卡"，0.5x 是视频网站常见的最慢档
#define PLAY_SPEED_START 0.5
// 最大倍速：2.0x
// ★ 设为 2.0 是因为 2x 之后音频/视频明显失真，再大没意义
#define PLAY_SPEED_STOP 2 // 4 speed multiple from start to stop in step

/**
 * @brief 构造函数：加载 .ui 布局并把所有控件的信号接到 MainWindow
 *
 * 这里的核心思想是"控件类不直接做业务"——所有点击/拖动/数值变化
 * 都通过信号槽转给 MainWindow，由 MainWindow 调用真正的播放/暂停/seek。
 * 这种"控件/业务分离"的好处是：
 *   - 控件可独立测试
 *   - 改 UI 不影响业务
 *   - 多个窗口可以共享同一份业务逻辑
 *
 * 信号槽的三个关键约定：
 *   1. 进度条拖动开始 -> 暂停（避免拖动期间视频还在跳，画面闪）
 *   2. 进度条拖动结束 -> 真正 seek 到目标位置
 *   3. 单击进度条 -> 直接 seek（自定义 ClickableSlider 的 onClick 信号）
 *
 * @param parent 父窗口（MainWindow），所有信号都转发到它
 */
PlayControlWnd::PlayControlWnd(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::play_control_window>())
{
    // ★ 必须先 setupUi，否则 ui->xxx 都是空指针
    ui->setupUi(this);

    // ★ 把 .ui 里默认的 gridLayout 装到 widget 上
    setLayout(ui->gridLayout);
    // ★ 把外边距清零，让控件条紧贴主窗口底边（更紧凑）
    ui->gridLayout->setContentsMargins(0, 0, 0, 0);

    // ★ 音量条 valueChanged -> 把数字同步显示到 label_vol（"50"）
    connect(ui->slider_vol, SIGNAL(valueChanged(int)), ui->label_vol, SLOT(setNum(int)));
    // ★ 静音复选框状态变化 -> 本类的 volume_muted（更新"灰显"效果）
    // ★ 静音复选框状态变化 -> 切换静音（Qt 5 写法，Qt 6 用 checkStateChanged）
    connect(ui->check_mute, &QCheckBox::stateChanged, this, &PlayControlWnd::volume_muted);
    // ★ 静音复选框状态变化 -> MainWindow 真正切换静音
    connect(ui->check_mute, &QCheckBox::stateChanged, (MainWindow*)parent, &MainWindow::play_mute);
    // ★ "停止"按钮 -> MainWindow::stop_play
    connect(ui->btn_stop, &QPushButton::clicked, (MainWindow*)parent, &MainWindow::stop_play);
    // ★ "播放/暂停"按钮 -> MainWindow::pause_play（命名是 pause_play，因为按钮本身就在两个状态间切换）
    connect(ui->btn_play, &QPushButton::clicked, (MainWindow*)parent, &MainWindow::pause_play);
    // ★ 音量条 valueChanged -> MainWindow::set_volume
    connect(ui->slider_vol, &QSlider::valueChanged, (MainWindow*)parent, &MainWindow::set_volume);
    // ★ "上一首"按钮按下 -> MainWindow::play_seek_pre
    connect(ui->btn_pre, &QPushButton::pressed, (MainWindow*)parent, &MainWindow::play_seek_pre);
    // ★ "下一首"按钮按下 -> MainWindow::play_seek_next
    connect(ui->btn_next, &QPushButton::pressed, (MainWindow*)parent, &MainWindow::play_seek_next);
    // ★ 进度条拖动结束 -> MainWindow::play_start_seek（真正执行 seek）
    connect(ui->progress_slider, &QSlider::sliderReleased, (MainWindow*)parent, &MainWindow::play_start_seek);
    // ★ 进度条拖动开始 -> MainWindow::pause_play（先暂停，避免视频跳帧）
    connect(ui->progress_slider, &QSlider::sliderPressed, (MainWindow*)parent, &MainWindow::pause_play);
    // ★ 自定义 ClickableSlider 的 onClick 信号（单击进度条跳转） -> MainWindow::play_seek
    connect(ui->progress_slider, &ClickableSlider::onClick, (MainWindow*)parent, &MainWindow::play_seek);
    // ★ 倍速条变化 -> 本类先更新"1.00x"文字
    connect(ui->slider_speed, &QSlider::valueChanged, this, &PlayControlWnd::speed_changed);
    // connect(ui->slider_speed, &QSlider::sliderReleased, (MainWindow*)parent,&MainWindow::set_play_speed);
    // ★ 同时把新倍速也转给 MainWindow（边拖边生效）
    connect(ui->slider_speed, &QSlider::valueChanged, (MainWindow*)parent, &MainWindow::set_play_speed);

    // ★ 初始状态：所有控件置灰、显示"--:--"
    clear_all();

    // ★ 把所有控件设为 NoFocus（详见 set_focus_policy 注释）
    set_focus_policy();
}

/**
 * @brief 析构函数
 *
 * 空的，因为 ui 用 unique_ptr 持有，会自动释放。
 */
PlayControlWnd::~PlayControlWnd()
{
}

/**
 * @brief 把所有控件的焦点策略设为 NoFocus
 *
 * 为什么必须这样？
 *   - 如果 slider 拿到焦点，按方向键会被 slider 消费掉
 *     （方向键本意是让 slider 的值 ±1）
 *   - 但本项目希望方向键去控制 MainWindow 的"前进/后退 2 秒"
 *   - 所以把焦点策略设为 NoFocus，让焦点停留在主窗口上，
 *     方向键就一定能被 MainWindow 的 keyPressEvent 收到
 */
void PlayControlWnd::set_focus_policy()
{
    /*
   * Disable widgets in this window to accept foucs,
   * so all key event will be transfered to the
   * parent widget. The mainwindow will handle these keyevent
   * to control the playing.
   */

    // setFocusPolicy(Qt::NoFocus); // 旧代码，全局设置（已弃用，改为逐个设置）
    // ★ 进度条：不抢焦点，方向键由 MainWindow 处理
    ui->progress_slider->setFocusPolicy(Qt::NoFocus);
    // ★ 倍速条：同上
    ui->slider_speed->setFocusPolicy(Qt::NoFocus);
    // ★ 音量条：同上
    ui->slider_vol->setFocusPolicy(Qt::NoFocus);

    // ★ 4 个按钮：按空格/回车也不会被按钮"截胡"
    ui->btn_pre->setFocusPolicy(Qt::NoFocus);
    ui->btn_play->setFocusPolicy(Qt::NoFocus);
    ui->btn_next->setFocusPolicy(Qt::NoFocus);
    ui->btn_stop->setFocusPolicy(Qt::NoFocus);
    // ★ 静音复选框同理
    ui->check_mute->setFocusPolicy(Qt::NoFocus);
}

/**
 * @brief 静音/解除静音时的 UI 同步
 *
 * 设计要点：
 *   - 静音时把"音量条 + 数字"灰显出来（视觉上提示用户现在是静音）
 *   - 同步把静音复选框自身的 checked 状态翻转
 *   - 真正切换音频静音通过信号槽去通知 MainWindow（不在这里做）
 *
 * @param state Qt::CheckState 枚举值（Checked=已勾选=静音, Unchecked=未选=有声音）
 * @return void
 */
void PlayControlWnd::volume_muted(int state)
{
    // ★ state 来自 QCheckBox::stateChanged 信号
    //   Qt::Checked=2（已勾选=静音）→ enable=false（变灰）
    //   非 Checked → enable=true（正常）
    bool enable = (state != Qt::Checked);
    // ★ 数字标签变灰
    ui->label_vol->setEnabled(enable);
    // ★ 滑块变灰（变灰后用户就拖不动了，避免和真实静音状态错乱）
    ui->slider_vol->setEnabled(enable);
    // ★ 复选框勾选状态同步：enable=false（静音）时复选框应被勾上
    ui->check_mute->setChecked(!enable);
}

/**
 * @brief 倍速条 value 变化时，把"1.00x"这样的字符串显示到 label_speed
 *
 * 倍速公式：
 *     speed = (value - 1) * STEP + START
 * 例如 STEP=0.25, START=0.5：
 *     value=1 -> 0.5x；value=2 -> 0.75x；...；value=7 -> 2.0x
 * value 范围对应 0.5x ~ 2.0x，步长 0.25。
 *
 * @param value 滑块当前值（int）
 * @return void
 */
void PlayControlWnd::speed_changed(int value)
{
    int max = ui->slider_speed->maximum();
    // ★ 用 value %= (max + 1) 防止越界（理论上不会触发，但保险）
    value %= (max + 1);
    // ★ 关键换算公式：把整数 value 映射到 0.5~2.0 之间的浮点倍速
    double speed = (value - 1) * PLAY_SPEED_STEP + PLAY_SPEED_START;
    // ★ 'f' + 2 表示固定 2 位小数
    QString str = QString::number(speed, 'f', 2) + "x";
    // qDebug() << speed << max << "str=" << str;
    // ★ 把格式化好的串写到"1.00x"那个标签上
    ui->label_speed->setText(str);
}

/**
 * @brief 读取当前倍速值（外部，如 MainWindow 设置播放速度时用）
 *
 * @return double 当前倍速（例如 1.0 表示正常速度，2.0 表示 2 倍速）
 */
double PlayControlWnd::get_speed() const
{
    int value = ui->slider_speed->value();
    // ★ 和 speed_changed 里的换算公式完全对应
    return (value - 1) * PLAY_SPEED_STEP + PLAY_SPEED_START;
}

/**
 * @brief 倍速 ±1 档（用于快捷键 , 和 .）
 *
 * 流程：读当前 value -> ±1 -> 钳制到 [0, max] -> setValue
 * 钳制是必须的，否则按 "." 超出 max 会被滑块"吃掉"
 *
 * @param up true 表示 +1 档（变快），false 表示 -1 档（变慢）
 * @return void
 */
void PlayControlWnd::speed_adjust(bool up)
{
    int value = ui->slider_speed->value();
    int max = ui->slider_speed->maximum();
    if (up)
    {
        // ★ 加速
        value += 1;
    }
    else
    {
        // ★ 减速
        value -= 1;
    }

    // ★ 上限钳制
    value = value > max ? max : value;
    // ★ 下限钳制（最小到 0，对应 0.5x）
    value = value < 0 ? 0 : value;
    // ★ setValue 会触发 valueChanged 信号，从而调用 speed_changed 更新 label
    ui->slider_speed->setValue(value);
}

/**
 * @brief 初始化倍速条：设置最大值 + 把当前值设到"1.0x"那一档
 *
 * 数学推导：
 *   - 最大倍速 = STOP = 2.0
 *   - 一共有 (STOP - START) / STEP = (2.0 - 0.5) / 0.25 = 6 个档位
 *   - 滑块 range = [0, 6]，但 value=1 才是 0.5x（因为公式 value-1）
 *   - 所以 max 应该 = 档位数 + 1 = 7？实际代码算了 8，保留余量
 *   - "1.0x"对应的 value = (1 + 0.25 - 0.5) / 0.25 = 0.75/0.25 = 3
 */
void PlayControlWnd::init_slider_speed()
{
    // ★ maxSpeed = (STOP + STEP - START) / STEP = 8，给滑块留一点余量
    int maxSpeed = (PLAY_SPEED_STOP + PLAY_SPEED_STEP - PLAY_SPEED_START) / PLAY_SPEED_STEP; // 8
    ui->slider_speed->setMaximum(maxSpeed);
    // ★ 默认停在 1.0x 这一档（value=3）
    ui->slider_speed->setValue((1 + PLAY_SPEED_STEP - PLAY_SPEED_START) / PLAY_SPEED_STEP); // set 1x speed
}

/**
 * @brief 从外部（如加载保存的设置）设置音量条位置
 *
 * @param volume 音量值，范围 0.0~1.0（与音频 API 一致）
 * @return void
 */
void PlayControlWnd::set_volume_slider(float volume)
{
    // ★ 先把音量条解禁（清空状态时是灰的）
    enable_slider_vol(true);
    auto max = ui->slider_vol->maximum();
    // ★ volume * max 把 [0, 1] 映射到 [0, max]
    ui->slider_vol->setValue(int(volume * max));
}

/**
 * @brief 把"时分秒"换算成总秒数
 *
 * @param hours 小时
 * @param mins  分钟
 * @param secs  秒
 * @return double 总秒数
 */
inline double PlayControlWnd::get_time_secs(int64_t hours, int64_t mins, int64_t secs)
{
    // ★ 标准换算：1h = 3600s, 1m = 60s
    return hours * 60 * 60 + mins * 60 + secs;
}

/**
 * @brief 取进度条指针（给 MainWindow 用，比如直接更新进度条位置）
 *
 * @return QSlider* 进度条指针
 */
inline QSlider* PlayControlWnd::get_progress_slider() const
{
    return ui->progress_slider;
}

/**
 * @brief 取音量条指针
 *
 * @return QSlider* 音量条指针
 */
inline QSlider* PlayControlWnd::get_volume_slider() const
{
    return ui->slider_vol;
}

/**
 * @brief 取倍速条指针
 *
 * @return QSlider* 倍速条指针
 */
inline QSlider* PlayControlWnd::get_speed_slider() const
{
    return ui->slider_speed;
}

/**
 * @brief 取音量条的最大值
 *
 * .ui 里通常设成 100，所以调用方可以直接认为 max=100
 *
 * @return int 音量条最大值
 */
int PlayControlWnd::get_volum_slider_max()
{
    return get_volume_slider()->maximum();
}

/**
 * @brief 取进度条的最大值（实际 = 总时长秒数，由 set_total_time 设入）
 *
 * @return int 进度条最大值
 */
int PlayControlWnd::get_progress_slider_max()
{
    return get_progress_slider()->maximum();
}

/**
 * @brief 取进度条当前值
 *
 * @return int 进度条当前值（= 当前播放秒数）
 */
int PlayControlWnd::get_progress_slider_value()
{
    return get_progress_slider()->value();
}

/**
 * @brief 启用/禁用进度条
 *
 * @param enable true=启用，false=禁用（灰显）
 * @return void
 */
void PlayControlWnd::enable_progressbar(bool enable)
{
    get_progress_slider()->setEnabled(enable);
}

/**
 * @brief 启用/禁用音量条
 *
 * @param enable true=启用，false=禁用（灰显）
 * @return void
 */
void PlayControlWnd::enable_slider_vol(bool enable)
{
    ui->slider_vol->setEnabled(enable);
}

/**
 * @brief 启用/禁用倍速条
 *
 * @param enable true=启用，false=禁用（灰显）
 * @return void
 */
void PlayControlWnd::enable_slider_speed(bool enable)
{
    ui->slider_speed->setEnabled(enable);
}

/**
 * @brief 把"总秒数"拆成时分秒
 *
 * 实现思路（高效版）：
 *   1. 先算分钟 = total / 60，秒 = total % 60
 *   2. 再算小时 = mins / 60，mins %= 60
 * 这样只做两次除法，效率高于"小时 = total/3600"那种除三次的写法。
 *
 * @param total_secs 总秒数
 * @param hours      [out] 小时
 * @param mins       [out] 分钟
 * @param secs       [out] 秒
 * @return void
 */
void PlayControlWnd::get_play_time_params(int64_t total_secs, int64_t& hours, int64_t& mins, int64_t& secs)
{
#if 1
    // ★ 第一步：算分钟和秒
    mins = total_secs / 60;
    secs = total_secs % 60;
    // ★ 第二步：分钟里再拆出小时，并更新 mins 为余数
    hours = mins / 60;
    mins %= 60;
#else
    // 备选实现（更直观但多了一次除法）
    hours = int(total_secs / 3600);
    mins = (total_secs - hours * 3600) / 60;
    secs = (total_secs - hours * 3600 - mins * 60);
#endif
}

/**
 * @brief 用"时分秒"更新当前时间显示 + 进度条位置
 *
 * 三件事：
 *   1. 格式化成 "mm:ss" / "hh:mm:ss" 字符串
 *   2. 写到 label_curTime
 *   3. 把当前秒数换算成百分比，setValue 到 progress_slider
 *
 * @param hours 当前小时
 * @param mins  当前分钟
 * @param secs  当前秒
 * @return void
 */
void PlayControlWnd::update_play_time(int64_t hours, int64_t mins, int64_t secs)
{
    // ★ 第一步：格式化时间字符串
    auto time_str = get_play_time(hours, mins, secs);
    // ★ 第二步：写到"当前时间"标签
    ui->label_curTime->setText(time_str);

    int percent = 0;
    // ★ 取总时长（秒）
    auto total = get_total_time();
    // ★ 把当前时分秒也算成秒
    auto cur = get_time_secs(hours, mins, secs);
    // ★ 钳制：cur 不能超过 total（防呆，避免进度条溢出）
    cur = cur > total ? total : cur;
    if (total > 0)
    {
        // ★ 算百分比：cur / total * max
        percent = cur * get_progress_slider()->maximum() / total;
    }
    // ★ 更新进度条
    get_progress_slider()->setValue(percent);
}

/**
 * @brief 用"总秒数"重载版本：常被视频解码线程周期性调用
 *
 * 增加了边界保护：
 *   - 小于 0 钳到 0
 *   - 大于 total 钳到 total
 * 然后把总秒数拆成时分秒，转交给三参数版本。
 *
 * @param total_secs 当前播放秒数
 * @return void
 */
void PlayControlWnd::update_play_time(int64_t total_secs)
{
    int64_t hours = 0, mins = 0, secs = 0;
    double total = get_total_time();

    // ★ 边界保护：负数视为 0
    total_secs = total_secs < 0 ? 0 : total_secs;
    // ★ 边界保护：超过总时长就钳到总时长
    total_secs = total_secs > total ? total : total_secs;

    // ★ 拆成时分秒
    get_play_time_params(total_secs, hours, mins, secs);
    // qDebug() << "total:" << total_secs << "h:" << hours << "m:" << mins <<
    // "ses:" << secs;
    // ★ 复用三参数版本
    update_play_time(hours, mins, secs);
}

/**
 * @brief 把保存的 m_hours/m_mins/m_secs 拼成总秒数
 *
 * @return double 总时长（秒）
 */
double PlayControlWnd::get_total_time() const
{
    return get_time_secs(m_hours, m_mins, m_secs);
}

/**
 * @brief 设置总时长（一般在视频打开成功后调用一次）
 *
 * 一连串动作：
 *   1. 解禁进度条 / 播放按钮 / 音量条 / 倍速条 / 静音框
 *   2. 初始化倍速条（重新算 max）
 *   3. 把 h/m/s 缓存到成员变量
 *   4. 设置进度条最大值 = 总秒数
 *   5. 更新"总时长"标签
 *
 * @param hours 总时长小时
 * @param mins  总时长分钟
 * @param secs  总时长秒
 * @return void
 */
void PlayControlWnd::set_total_time(int64_t hours, int64_t mins, int64_t secs)
{
    // ★ 解禁所有控件（之前可能是灰的）
    enable_progressbar();
    enable_play_buttons();
    enable_slider_vol();
    enable_slider_speed();
    ui->check_mute->setEnabled(true);
    // ★ 倍速条重新初始化（避免之前残留的 max 不对）
    init_slider_speed();

    // ★ 缓存到成员，供 get_total_time() 用
    m_hours = hours;
    m_mins = mins;
    m_secs = secs;

    // ★ 进度条最大值 = 总秒数，这样 setValue(秒) 就直接对应"百分比"
    set_progress_bar(get_total_time());
    // ★ 显示"总时长"字符串
    ui->label_totalTime->setText(get_play_time(hours, mins, secs));
}

/**
 * @brief 设置进度条的最大值（= 总秒数）
 *
 * @param total_secs 总秒数
 * @return void
 */
void PlayControlWnd::set_progress_bar(double total_secs)
{
    get_progress_slider()->setMaximum(total_secs);
}

/**
 * @brief 把"时分秒"格式化为"mm:ss"或"hh:mm:ss"字符串
 *
 * 规则：
 *   - hours = 0 时只输出 "mm:ss"（视频通常不会上小时）
 *   - hours > 0 时输出 "hh:mm:ss"
 * - 不足两位的数字用 '0' 左填充（rightJustified）
 *
 * @param hours 小时
 * @param mins  分钟
 * @param secs  秒
 * @return QString 格式化好的时间字符串
 */
QString PlayControlWnd::get_play_time(int64_t hours, int64_t mins, int64_t secs)
{
    QString str;

    if (hours == 0)
    {
        // ★ 短格式：mm:ss
        str = QString("%1:%2")
                  .arg(QString::number(mins).rightJustified(2, '0'))
                  .arg(QString::number(secs).rightJustified(2, '0'));
    }
    else
    {
        // ★ 长格式：hh:mm:ss
        str = QString("%1:%2:%3")
                  .arg(QString::number(hours).rightJustified(2, '0'))
                  .arg(QString::number(mins).rightJustified(2, '0'))
                  .arg(QString::number(secs).rightJustified(2, '0'));
    }
    return str;
}

/**
 * @brief 清空所有时间显示（停止播放 / 切换文件时调用）
 *
 * 三件事：
 *   1. 内部 h/m/s 全部清零
 *   2. 进度条指针归 0
 *   3. 两个时间标签显示 "--:--"
 */
void PlayControlWnd::clear_time()
{
    // ★ 内部时间戳清零
    m_hours = 0;
    m_mins = 0;
    m_secs = 0;

    // ★ 进度条归 0
    get_progress_slider()->setValue(0);
    // ★ "总时长"标签显示占位符
    ui->label_totalTime->setText("--:--");
    // ★ "当前时间"标签显示占位符
    ui->label_curTime->setText("--:--");
}

/**
 * @brief 重置整个控制面板
 *
 * 典型调用时机：
 *   - 程序刚启动（构造完先 clear_all 一次）
 *   - 关闭播放后
 *
 * 流程：
 *   1. clear_time 清空时间显示
 *   2. 灰显进度条
 *   3. 切换"播放"按钮文字
 *   4. 灰显倍速条 / 音量条 / 播放按钮 / 静音框
 *   5. 重新初始化倍速条（确保下次能正常用）
 */
void PlayControlWnd::clear_all()
{
    // ★ 先清空时间
    clear_time();
    // ★ 进度条灰显
    enable_progressbar(false);
    // ★ 把按钮文字还原为"Play"（初始 / 停止后）
    update_btn_play();

    // ★ 倍速条灰显
    enable_slider_speed(false);
    // ★ 音量条灰显
    enable_slider_vol(false);
    // ★ 4 个播放按钮灰显
    enable_play_buttons(false);
    // ★ 静音框灰显
    ui->check_mute->setEnabled(false);

    // ★ 倍速条重新初始化（这样下次打开视频时是 1.0x 起步）
    init_slider_speed();
}

/**
 * @brief 切换"播放/暂停"按钮上的文字
 *
 * 文字含义要小心：
 *   - 显示 "Play" 表示"现在处于暂停状态，按下会变成播放"
 *   - 显示 "Pause" 表示"现在正在播放，按下会变成暂停"
 *
 * @param bPause true=当前是暂停（按钮显示 Play）；false=当前是播放（按钮显示 Pause）
 * @return void
 */
void PlayControlWnd::update_btn_play(bool bPause)
{
    if (bPause)
    {
        // ★ 当前是暂停状态，按钮文字显示"Play"
        ui->btn_play->setText("Play");
    }
    else
    {
        // ★ 当前是播放状态，按钮文字显示"Pause"
        ui->btn_play->setText("Pause");
    }
}

/**
 * @brief 启用/禁用上一首/播放/下一首/停止 4 个按钮
 *
 * 把这 4 个按钮看成一组：要么全亮，要么全灰，
 * 因为"正在加载"或"播放结束"时这些动作都不该被点。
 *
 * @param enable true=全部启用，false=全部灰显
 * @return void
 */
void PlayControlWnd::enable_play_buttons(bool enable)
{
    // ★ 4 个按钮一起 setEnabled
    ui->btn_next->setEnabled(enable);
    ui->btn_pre->setEnabled(enable);
    ui->btn_play->setEnabled(enable);
    ui->btn_stop->setEnabled(enable);
}

/**
 * @brief 拦截所有按键事件并转发给 MainWindow
 *
 * 为什么这样做？
 *   - 控件虽然设了 NoFocus，但某些场景下还是可能拿到按键
 *   - 真正的快捷键处理（方向键前进/后退、空格暂停等）都在 MainWindow 里
 *   - 这里用 QApplication::sendEvent 把事件"原封不动"地转给父窗口
 *   - 最后 event->ignore() 告诉 Qt"这个事件我没处理"，避免双重处理
 *
 * @param event 按键事件指针
 * @return void
 */
void PlayControlWnd::keyPressEvent(QKeyEvent* event)
{
    if (auto pParent = (MainWindow*)parent())
    {
        // ★ 把事件转发给父窗口
        QApplication::sendEvent(pParent, event);
        // ★ 标记为"未处理"，避免 Qt 又把这个事件派回给本控件
        event->ignore();
    }
    else
    {
        // ★ 父窗口不在（极端情况），按默认方式处理
        QWidget::keyPressEvent(event);
    }
}
