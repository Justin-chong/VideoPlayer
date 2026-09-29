/**
 * @file mainwindow.h
 * @brief 主窗口类声明
 *
 * MainWindow 是整个播放器最大、最核心的类（约 200+ 成员函数）。
 * 它承担了：
 *   1. 整个播放流水线（6 个工作线程）的生命周期管理
 *   2. UI 编排（菜单、工具栏、播放控件、子窗口）
 *   3. 用户交互响应（键盘、鼠标、拖放文件）
 *   4. 设置持久化（QSettings）
 *
 * 阅读建议：
 *   先看类成员变量（m_xxx 那一片）——能猜到它有哪些子模块；
 *   再看 public slots —— 它响应哪些外部事件；
 *   最后看 private 函数 —— 这些是内部的实现细节。
 */

#pragma once

#include <QActionGroup>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QMainWindow>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QMutex>
#include <QSettings>
#include <QSizePolicy>
#include <QThread>
#include <QTimer>

// 各种子模块的头文件
#include "app_settings.h"               // 应用程序设置（窗口大小、皮肤等）
#include "audio_decode_thread.h"        // 音频解码线程
#include "audio_effect_gl.h"            // OpenGL 音频可视化
#include "audio_play_thread.h"          // 音频播放线程
#include "network_url_dlg.h"            // "打开网络 URL" 对话框
#include "play_control_window.h"        // 底部播放控制条
#include "player_skin.h"                // 皮肤（QSS 样式）
#include "playlist_window.h"            // 播放列表窗口
#include "read_thread.h"                // 读包线程
#include "start_play_thread.h"          // "开始播放"预处理线程
#include "stopplay_waiting_thread.h"    // "停止播放"等待线程
#include "subtitle_decode_thread.h"     // 字幕解码线程
#include "video_decode_thread.h"        // 视频解码线程
#include "video_label.h"                // 视频显示标签（继承 QLabel）
#include "video_play_thread.h"          // 视频播放线程
#include "video_state.h"                // 播放会话管理
#include "youtube_url_thread.h"         // YouTube 链接解析线程

QT_BEGIN_NAMESPACE
namespace Ui
{
class MainWindow;  // 由 .ui 文件生成的 UI 类（Qt Designer）
}
QT_END_NAMESPACE

// 各种"上限"宏
#define MaxRecentFiles 20   // "最近播放"最多保存 20 个
#define MaxSkinStlyes  20   // 最多 20 种皮肤可选
#define MaxPlaylist    5    // 播放列表最多 5 个槽位

/**
 * @class MainWindow
 * @brief 播放器主窗口
 *
 * 继承自 QMainWindow（带菜单栏、工具栏、状态栏的标准窗口）。
 * 通过 signals/slots 与各子模块通信。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = Q_NULLPTR);
    ~MainWindow();

public:
    // ===== 对外接口（外部代码可调用）=====
    //里面实际是调用内部实现函数
    void start_to_play(const QString& file);  // 播放一个文件（启动整个流水线）
    void stop_play();                          // 停止播放
    void pause_play();                         // 暂停/恢复
    float volume_settings(bool set = true, float vol = 0);  // 设置/读取音量
    inline AudioPlayThread* get_audio_play_thread() const { return m_pAudioPlayThread.get(); }
    inline VideoPlayThread* get_video_play_thread() const { return m_pVideoPlayThread.get(); }
    inline VideoStateData* get_video_state_data() const { return m_pVideoState.get(); }
    void play_mute(bool muted);  // 静音
    void play_seek();                   // seek（拖动进度条）
    void play_start_seek();             // seek 到开始
    void play_seek_pre();               // 上一个
    void play_seek_next();              // 下一个
    void set_volume(int volume);        // 设置音量
    void set_play_speed();              // 设置播放速度
    void show_msg_dlg(const QString& message, const QString& windowTitle = "Warning", const QString& styleSheet = "");  // 显示提示框
    bool is_playing() const;            // 是否正在播放
    QString get_playingfile() const;    // 当前播放的文件名
    void start_yt_play(const YoutubeJsonParser::YtStreamData& st_data);  // 播放 YouTube 视频
    void show_fullscreen(bool bFullscreen = true);  // 全屏/退出全屏

public slots:
    // ===== 响应各子模块信号的槽函数 =====
    void image_ready(const QImage&);            // 视频播放线程：新的一帧准备好了
    void subtitle_ready(const QString&);       // 视频播放线程：一段字幕准备好了
    void audio_data(const AudioData& data);     // 音频播放线程：音频数据（用于可视化）
    void open_recentFile();                     // "最近文件"菜单点击
    void clear_recentfiles();                   // 清空最近文件
    void read_packet_stopped();                 // 读包线程已停止
    void decode_video_stopped();                // 视频解码线程已停止
    void decode_audio_stopped();                // 音频解码线程已停止
    void decode_subtitle_stopped();             // 字幕解码线程已停止
    void audio_play_stopped();                  // 音频播放线程已停止
    void video_play_stopped();                  // 视频播放线程已停止
    void update_play_time();                    // 定时器触发，更新播放进度条
    void play_started(bool ret = true);         // 开始播放成功/失败
    void play_failed(const QString& file);      // 播放失败
    void playlist_file_saved(const QString& file);  // 播放列表已保存
    void on_playback_finished();               // ★ 视频自然播放完毕（video_play_thread 发出）
    void set_threads();                         // 线程集合设置（用于停止时统一退出）

signals:
    // ===== 发给各子模块的信号 =====
    void stop_audio_play_thread();          // 停止音频播放
    void stop_video_play_thread();          // 停止视频播放
    void stop_decode_thread();              // 停止解码
    void stop_read_packet_thread();         // 停止读包
    void wait_stop_audio_play_thread();     // 等待音频播放退出
    void wait_stop_video_play_thread();     // 等待视频播放退出

private:
    // ===== Qt 事件重写 =====
    void keyPressEvent(QKeyEvent* event) override;     // 键盘按键
    void resizeEvent(QResizeEvent* event) override;    // 窗口尺寸变化
    void moveEvent(QMoveEvent* event) override;        // 窗口移动
    bool eventFilter(QObject* obj, QEvent* event) override;  // 全局事件过滤
    void dropEvent(QDropEvent* event) override;        // 拖放文件
    void dragEnterEvent(QDragEnterEvent* event) override;      // 拖入文件

private slots:
    // ===== 菜单项的响应槽 =====
    //不需要connect，直接在ui页面右击控件“转为槽”，就会生成on_xxx_triggered()模板
    void on_actionOpen_triggered();                 // 菜单：打开文件
    void on_actionQuit_triggered();                 // 菜单：退出
    void on_actionHelp_triggered();                 // 菜单：帮助
    void on_actionAbout_triggered();                // 菜单：关于
    void on_actionStop_triggered();                 // 菜单：停止
    void on_actionFullscreen_triggered();           // 菜单：全屏
    void on_actionHide_Play_Ctronl_triggered();     // 菜单：隐藏/显示播放控制条
    void on_actionYoutube_triggered();              // 菜单：打开 YouTube
    void on_actionAspect_Ratio_triggered();         // 菜单：画面比例
    void on_actionSystemStyle();                    // 菜单：使用系统样式
    void on_actionCustomStyle();                    // 菜单：使用自定义皮肤
    void on_actionLoop_Play_triggered();            // 菜单：循环播放
    void on_actionMedia_Info_triggered();           // 菜单：媒体信息
    void on_actionKeyboard_Usage_triggered();       // 菜单：键盘快捷键
    void on_actionPlayList_triggered();             // 菜单：播放列表
    void on_actionOpenNetworkUrl_triggered();       // 菜单：打开网络 URL
    void on_actionOriginalSize_triggered();         // 菜单：原始尺寸

private:
    // ===== 内部实现函数（按功能分组）=====

    // --- 启动/状态 ---
    bool start_play();                                // 启动播放（创建 VideoState 和线程）
    bool playing_has_video();                         // 当前文件是否有视频
    bool playing_has_audio();                         // 当前文件是否有音频
    bool playing_has_subtitle();                      // 当前文件是否有字幕
    void update_image(const QImage&);                 // 把一帧图像显示到 video_label
    void print_decodeContext(const AVCodecContext* pVideo, bool bVideo = true) const;  // 打印解码器信息

    // --- 媒体信息 / 特效 ---
    void about_media_info();                          // 显示媒体信息对话框
    void image_cv(QImage&);                           // 对视频帧应用 OpenCV 滤镜
    void image_cv_geo(QImage&);                       // 几何变换（旋转、缩放等）

    // --- 窗口 / 控件 ---
    void resize_window(int width = 800, int height = 480);  // 调整窗口大小
    void resize_window(const QSize& size);
    void center_window(QRect screen_rec);             // 窗口居中
    bool label_fullscreen();                          // video_label 全屏
    void hide_statusbar(bool bHide = true);           // 隐藏/显示状态栏
    void hide_menubar(bool bHide = true);             // 隐藏/显示菜单栏
    void check_hide_menubar(const QPoint& pt);        // 检查是否要自动隐藏菜单栏
    void check_hide_play_control();                   // 检查是否要自动隐藏控制条
    void auto_hide_play_control(bool bHide = true);   // 自动隐藏控制条
    void displayStatusMessage(const QString& message);  // 在状态栏显示消息
    void hide_play_control(bool bHide = true);
    void set_paly_control_wnd(bool set = true);
    void update_paly_control_volume();                // 更新控制条的音量显示
    void update_paly_control_status();                // 更新控制条的状态（播放/暂停）
    void update_paly_control_muted();                 // 更新控制条的静音状态
    void print_size() const;                          // 打印窗口尺寸（调试用）
    void print_screen() const;                        // 打印屏幕尺寸
    void keep_aspect_ratio(bool bWidth = true);       // 保持画面宽高比

    // --- 皮肤 / 样式 ---
    void create_style_menu();                         // 创建"皮肤"菜单
    inline QScreen* screen() const;
    QRect screen_rect() const;
    qreal screen_scale() const;
    QSize display_video_size(AVCodecContext* pCtxVideo) const;  // 计算视频显示尺寸
    inline VideoLabel* get_video_label() const { return m_video_label.get(); }
    inline PlayControlWnd* get_play_control() const { return m_play_control_wnd.get(); }
    inline QObject* get_object(const QString& name) const { return findChild<QObject*>(name); }
    void create_play_control();
    void update_play_control();
    void set_volume_updown(bool bUp = true, float unit = 0.05);  // 音量+/-（按上下键）
    void create_recentfiles_menu();                   // 创建"最近文件"菜单
    void set_current_file(const QString& fileName);   // 设置当前文件（更新窗口标题）
    void remove_recentfiles(const QString& fileName);
    void update_recentfile_actions();
    QString stripped_name(const QString& fullFileName) const;  // 从完整路径提取文件名

    // --- 设置持久化 ---
    void save_settings();   // 程序退出时调用，把窗口大小、皮肤等存到 QSettings
    void read_settings();   // 程序启动时调用，恢复上次的设置
    QString get_selected_style() const;
    void set_style_action(const QString& style);

    // --- 字幕 ---
    void clear_subtitle_str();
    void set_subtitle(const QString& str);

    // --- OpenCV / 音频可视化 ---
    void create_cv_action_group();                    // 创建"OpenCV 滤镜"菜单组
    void play_speed_adjust(bool up = true);           // 调整播放速度
    void create_video_label();
    void update_video_label();
    void create_audio_effect();                       // 创建 OpenGL 音频可视化窗口
    void start_send_data(bool bSend = true);
    void show_audio_effect(bool bShow = true);
    void play_control_key(Qt::Key key);
    void set_default_bkground();                      // 设置默认背景图（无视频时显示）

    // --- 线程管理（核心）---
    bool create_video_state(const QString& file);     // 创建 VideoState（打开文件）
    void delete_video_state();                        // 销毁 VideoState
    bool create_read_thread();            // 创建读包线程
    bool create_decode_video_thread();    // 创建视频解码线程
    bool create_decode_audio_thread();    // 创建音频解码线程
    bool create_decode_subtitle_thread(); // 创建字幕解码线程
    bool create_video_play_thread();      // 创建视频播放线程
    bool create_audio_play_thread();      // 创建音频播放线程
    bool start_play_thread();             // 创建"开始播放"预处理线程
    void all_thread_start();              // 启动所有线程
    void video_seek_inc(double incr);     // 相对 seek
    void video_seek(double pos = 0, double incr = 0);  // 绝对 seek
    void update_menus();                  // 根据播放状态启用/禁用菜单项
    void enable_menus(bool enable = true);
    void enable_v_menus(bool enable = true);  // 视频相关菜单
    void enable_a_menus(bool enable = true);  // 音频相关菜单

    // --- YouTube ---
    int get_youtube_optionid() const;
    void set_youtube_optionid(int id);
    void create_avisual_action_group();
    bool get_avisual_format(BarHelper::VisualFormat& fmt) const;
    void popup_audio_effect();
    void set_audio_effect_format(const BarHelper::VisualFormat& fmt);
    bool start_youtube_url_thread(const YoutubeUrlDlg::YoutubeUrlData& data);
    void wait_stop_play(const QString& file);

    // --- 播放列表 ---
    void create_playlist_wnd();
    void add_to_playlist(const QString& file);
    void show_playlist(bool show = true);
    void playlist_hiden();
    void hide_cursor(bool bHide = true);
    bool cursor_in_window(QWidget* pWnd);

    void create_savedPlaylists_menu();
    void remove_playlist_file(const QString& fileName);
    void update_savedPlaylists_actions();
    bool read_playlist(const QString& playlist_file, QStringList& files) const;
    void play_window_size();
    void adjust_window_size(QSize& size);
    void clear_yt_list();
    void remove_yt_list(const QString& url);
    void insert_yt_list(const QString& url, const YoutubeJsonParser::YtStreamData& st);

public:
    bool find_yt_list(const QString& url, YoutubeJsonParser::YtStreamData& st);

public slots:
    void clear_savedPlaylists();
    void open_playlist();

private:
    std::unique_ptr<Ui::MainWindow> ui;  // 由 .ui 文件生成的 UI 控件

    // ===== 6 个工作线程（播放流水线的核心）=====
    std::unique_ptr<ReadThread> m_pPacketReadThread;               // 读包线程
    std::unique_ptr<VideoDecodeThread> m_pDecodeVideoThread;       // 视频解码线程
    std::unique_ptr<AudioDecodeThread> m_pDecodeAudioThread;       // 音频解码线程
    std::unique_ptr<SubtitleDecodeThread> m_pDecodeSubtitleThread; // 字幕解码线程
    std::unique_ptr<AudioPlayThread> m_pAudioPlayThread;           // 音频播放线程
    std::unique_ptr<VideoPlayThread> m_pVideoPlayThread;           // 视频播放线程

    // ===== 辅助线程（解决 UI 卡顿问题）=====
    std::unique_ptr<VideoStateData> m_pVideoState;                 // 播放会话管理
    std::unique_ptr<StartPlayThread> m_pBeforePlayThread;          // 开始播放前的预处理
    std::unique_ptr<YoutubeUrlThread> m_pYoutubeUrlThread;         // YouTube 链接解析
    std::unique_ptr<StopWaitingThread> m_pStopplayWaitingThread;   // 等待停止完成

    // ===== 状态变量 =====
    QString m_videoFile;       // 当前播放的文件
    QTimer m_timer;            // 定时器（用于自动隐藏控制条）
    AppSettings m_settings;     // 应用设置
    PlayerSkin m_skin;         // 当前皮肤
    QString m_subtitle;         // 当前字幕文本

    // ===== 子窗口/控件 =====
    std::unique_ptr<VideoLabel> m_video_label;         // 显示视频的 QLabel
    std::unique_ptr<PlayControlWnd> m_play_control_wnd; // 底部播放控制条
    std::unique_ptr<AudioEffectGL> m_audio_effect_wnd; // OpenGL 音频可视化窗口
    std::unique_ptr<PlayListWnd> m_playListWnd;        // 播放列表窗口

private:
    // ===== 各种菜单项的 Action（用智能指针管理）=====
    std::unique_ptr<QAction> m_recentFileActs[MaxRecentFiles];   // 最近文件菜单项
    std::unique_ptr<QAction> m_recentClear;                       // "清空最近文件"
    std::unique_ptr<QActionGroup> m_styleActsGroup;               // 皮肤菜单组（互斥）
    std::unique_ptr<QAction> m_styleActions[MaxSkinStlyes];        // 各种皮肤
    std::unique_ptr<QActionGroup> m_CvActsGroup;                  // OpenCV 滤镜菜单组
    std::unique_ptr<QActionGroup> m_AVisualTypeActsGroup;         // 音频可视化类型
    std::unique_ptr<QActionGroup> m_AVisualGrapicTypeActsGroup;   // 音频可视化图形
    std::unique_ptr<QAction> m_savedPlaylists[MaxPlaylist];       // 已保存的播放列表
    std::unique_ptr<QAction> m_PlaylistsClear;                    // "清空播放列表"

    // 已解析过的 YouTube 链接缓存（URL -> 视频信息）
    std::map<QString, YoutubeJsonParser::YtStreamData> m_playYtList;
};
