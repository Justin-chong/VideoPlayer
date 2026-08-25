// ***********************************************************/
// mainwindow.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 主窗口实现 - 整个播放器的控制中枢
// 职责：
//   1. 装载 UI（视频标签、播放控件、菜单、播放列表）
//   2. 创建并维护播放流水线（read/decode/play 多线程）
//   3. 把 FFmpeg 解码线程和 Qt UI 事件循环串起来（信号/槽）
//   4. 处理键盘快捷键、拖放、皮肤切换、保存设置等
// ***********************************************************/

#include "mainwindow.h"
#include "common.h"
#include "about.h"
#include "ffmpeg_init.h"
// #include "imagecv_operations.h"   // ★ 暂时禁用：OpenCV 4.9 objdetect 头文件依赖 aruco_contrib
// #include "qimage_convert_mat.h"   // ★ 暂时禁用：依赖 OpenCV 头文件
#include "qimage_operation.h"
#include "start_play_thread.h"
#include "ui_mainwindow.h"
#include "youtube_url_dlg.h"

#if NDEBUG
#define AUTO_HIDE_PLAYCONTROL 1 // release version
#else
#define AUTO_HIDE_PLAYCONTROL 0
#endif

/**
 * @brief MainWindow 构造函数 - 整个 UI 的初始化入口
 *
 * 1. 加载 .ui 描述文件
 * 2. 创建子窗口：视频标签、播放控件、皮肤菜单、最近文件菜单、CV 滤镜组、音频可视化、播放列表
 * 3. 安装全局事件过滤器（捕获鼠标移动用于自动隐藏播放控件）
 * 4. 接受文件拖放
 * 5. 初始化 FFmpeg
 * 6. 启动自动隐藏播放控件的定时器（仅 release 版）
 * 7. 读取上次保存的设置（窗口大小、皮肤、音量等）
 *
 * @param parent 父窗口指针（顶层窗口通常传 nullptr）
 */
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), ui(std::make_unique<Ui::MainWindow>())
{
    // ★ 加载 .ui XML 描述文件，建出所有 Qt 控件
    ui->setupUi(this);

    // ★ 确保窗口有最小化、最大化、关闭按钮（部分平台默认会少）
    setWindowFlags(windowFlags() | Qt::WindowMinimizeButtonHint |
                   Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);

    // ★ 按顺序创建所有子窗口和菜单
    create_video_label();
    create_play_control();
    create_style_menu();
    create_recentfiles_menu();
    create_cv_action_group();
    create_audio_effect();
    create_avisual_action_group();
    create_playlist_wnd();
    create_savedPlaylists_menu();

    setWindowTitle(tr("视频播放器"));
    // ★ 显示默认背景图（刚启动时画面不是黑的）
    set_default_bkground();

    // ★ 安装全局事件过滤器，自己捕获所有对象的鼠标移动事件
    qApp->installEventFilter(this);
    // ★ 允许拖放文件到窗口上
    setAcceptDrops(true);

    // ★ 初始化 FFmpeg 库（注册编解码器等）
    ffmpeg_init();

#if AUTO_HIDE_PLAYCONTROL
    // ★ release 模式才启用自动隐藏播放控件（debug 时关闭方便调试）
    // set mouse moving detection timer
    setMouseTracking(true);
    m_timer.setInterval(3 * 1000);  // ★ 3 秒无操作就隐藏
    m_timer.setSingleShot(false);   // ★ 重复触发
    connect(&m_timer, &QTimer::timeout, this, &MainWindow::check_hide_play_control);
    m_timer.start();
#endif

    // ★ 恢复上次的窗口大小、皮肤、音量等
    resize_window();
    read_settings();
    update_menus();

    // ★ "关于 Qt" 标准菜单项直接连到 QApplication 自带的 aboutQt 槽
    connect(ui->actionAbout_QT, SIGNAL(triggered()), qApp, SLOT(aboutQt()));
    // ★ 5 个音频可视化菜单项都连接到同一个 popup_audio_effect 槽
    connect(ui->actionSampling, &QAction::triggered, this, &MainWindow::popup_audio_effect);
    connect(ui->actionFrequency, &QAction::triggered, this, &MainWindow::popup_audio_effect);
    connect(ui->actionLine, &QAction::triggered, this, &MainWindow::popup_audio_effect);
    connect(ui->actionBar, &QAction::triggered, this, &MainWindow::popup_audio_effect);
    connect(ui->actionPie, &QAction::triggered, this, &MainWindow::popup_audio_effect);
#if !NDEBUG
    // ★ debug 模式下打印屏幕参数，方便排查分辨率/DPI 问题
    print_screen();
#endif
}

/**
 * @brief 析构：关掉正在播放的内容并保存设置
 *
 * 退出时调用 stop_play() 释放所有解码/播放线程，
 * 再调 save_settings() 把窗口/皮肤/音量等写回 QSettings。
 */
MainWindow::~MainWindow()
{
    stop_play();
    save_settings();
}

/**
 * @brief 获取主屏幕指针
 *
 * @return QScreen* 主屏幕对象（用于查询屏幕大小、缩放比等）
 */
QScreen* MainWindow::screen() const
{
    return QApplication::primaryScreen();
}

/**
 * @brief 获取主屏幕的可用区域（不含任务栏）
 *
 * @return QRect 屏幕矩形
 */
QRect MainWindow::screen_rect() const
{
    //auto pScreen = screen();
    //auto scale = pScreen->devicePixelRatio();
    //return QRect(0, 0, rt.width() * scale, rt.height() * scale);
    return screen()->geometry();
}

/**
 * @brief 获取屏幕缩放比（HiDPI 屏通常是 1.5 或 2.0）
 *
 * @return qreal 缩放比，比如 1.0（普通屏）或 2.0（Retina 屏）
 */
qreal MainWindow::screen_scale() const
{
    return screen()->devicePixelRatio();
}

/**
 * @brief 算出视频在屏幕上"逻辑像素"的尺寸
 *
 * 视频原始宽高是物理像素，HiDPI 屏需要除以缩放比得到逻辑像素，
 * 否则窗口/控件大小会算错。
 *
 * @param pVideo 视频解码上下文（从中取 width/height）
 * @return QSize 视频在屏幕上的逻辑尺寸
 */
QSize MainWindow::display_video_size(AVCodecContext* pVideo) const
{
    auto scale = screen_scale(); //screen display scale
    if (pVideo && scale != 0)
        return QSize(pVideo->width / scale, pVideo->height / scale);

    return QSize(0, 0);
}

/**
 * @brief 创建一个 VideoLabel 当成画布，放在 centralWidget 上
 *        视频播放线程通过 frame_ready 信号把 QImage 贴到这里
 */
void MainWindow::create_video_label()
{
    // ★ centralWidget 是 QMainWindow 默认的中央区域
    m_video_label = std::make_unique<VideoLabel>(centralWidget());
    m_video_label->setObjectName(QString::fromUtf8("label_Video"));
    // ★ setScaledContents(true) 让 pixmap 跟着 label 大小缩放（视频画面能自动填满）
    m_video_label->setScaledContents(true);
    // ★ 加 SubWindow 标志，让它表现为 centralWidget 的子窗口
    m_video_label->setWindowFlags(m_video_label->windowFlags() | Qt::SubWindow);
    m_video_label->show();
}

/**
 * @brief 创建音频可视化窗口（默认隐藏）
 *        收到 hiden 信号后重新启动音频采样数据推送
 */
void MainWindow::create_audio_effect()
{
    m_audio_effect_wnd = std::make_unique<AudioEffectGL>(centralWidget());
    m_audio_effect_wnd->setObjectName(QString::fromUtf8("audio_effect"));
    // ★ 默认隐藏，用户点菜单时才显示
    m_audio_effect_wnd->hide();

    // ★ 当窗口被隐藏时，关闭音频采样数据推送（省 CPU）
    connect(m_audio_effect_wnd.get(), &AudioEffectGL::hiden, this, &MainWindow::start_send_data);
}

/**
 * @brief 显示/隐藏音频可视化窗口
 *
 * 显示前会先把它移到主窗口正中央，并清空画布。
 *
 * @param bShow true=显示；false=隐藏
 */
void MainWindow::show_audio_effect(bool bShow)
{
    if (!m_audio_effect_wnd)
        return;

    // ★ 把窗口几何中心对齐到主窗口几何中心
    auto pt = frameGeometry().center() - m_audio_effect_wnd->rect().center();
    m_audio_effect_wnd->move(pt);
    // ★ 每次显示前清空 OpenGL 画布（避免上一帧残留）
    m_audio_effect_wnd->paint_clear();

    if (bShow)
    {
        m_audio_effect_wnd->show();
    }
    else
    {
        m_audio_effect_wnd->hide();
    }
}

/**
 * @brief 动态扫描皮肤文件，构建"系统主题"和"自定义 QSS"菜单
 *        系统主题：枚举在 PlayerSkin 里写死的几个 Qt 风格名
 *        自定义主题：扫 res/QSS/ 下的 .qss 文件
 */
void MainWindow::create_style_menu()
{
    auto pMenu = ui->menuStyle;

    // ★ 第一个分隔符：标识下面的菜单是"系统主题"
    pMenu->addSeparator()->setText("系统主题");

    // ★ QActionGroup：让多个 action 互斥（只能选一个）
    m_styleActsGroup = std::make_unique<QActionGroup>(this);
    uint id = 0;

    // ★ 遍历 PlayerSkin 提供的所有内置系统主题
    for (const auto& style : m_skin.get_style())
    {
        qDebug("style:%s", qUtf8Printable(style));
        QString name = "action" + style;

        // ★ 超出数组容量就停止（防御性编程）
        if (id >= MaxSkinStlyes)
            break;

        m_styleActions[id] = std::make_unique<QAction>(this);
        const auto& action = m_styleActions[id];

        action->setCheckable(true);   // ★ 让它能显示选中状态
        action->setData(style);        // ★ 把风格名存到 data 里，触发时取出
        if (id == 0)
        {
            // ★ 第一个默认选中
            action->setChecked(true);
        }
        action->setObjectName(QString::fromUtf8(name.toStdString().c_str()));
        action->setText(QApplication::translate("MainWindow", style.toStdString().c_str(), nullptr));
        pMenu->addAction(action.get());

        connect(action.get(), &QAction::triggered, this, &MainWindow::on_actionSystemStyle);
        // ★ 加入 ActionGroup 实现单选
        m_styleActsGroup->addAction(action.get());

        id++;
    }

    // ★ 第二个分隔符：标识下面的菜单是"自定义主题"
    pMenu->addSeparator()->setText("自定义主题");

    // ★ 遍历 res/QSS/ 下所有自定义 .qss 文件
    for (const auto& path : m_skin.get_custom_styles())
    {
        QFileInfo fileInfo(path);
        QString filename = fileInfo.baseName();  // ★ 取不带后缀的文件名作为主题名

        qDebug("path:%s, %s", qUtf8Printable(path), qUtf8Printable(filename));
        QString name = "action" + filename;

        if (id >= MaxSkinStlyes)
            break;

        m_styleActions[id] = std::make_unique<QAction>(this);
        const auto& action = m_styleActions[id];

        action->setData(filename);
        action->setCheckable(true);
        action->setObjectName(QString::fromUtf8(name.toStdString().c_str()));
        action->setText(QApplication::translate("MainWindow", filename.toStdString().c_str(), nullptr));
        pMenu->addAction(action.get());

        connect(action.get(), &QAction::triggered, this, &MainWindow::on_actionCustomStyle);
        m_styleActsGroup->addAction(action.get());

        id++;
    }
}

/**
 * @brief 构建 OpenCV 滤镜的 ActionGroup（互斥单选）
 *        actionRemoveCV 是默认选中的（即不处理）
 */
void MainWindow::create_cv_action_group()
{
    auto pMenuCV = ui->menuCV;
    // ★ 显示工具提示（hover 时能看到说明文字）
    pMenuCV->setToolTipsVisible(true);

    // ★ QActionGroup 让所有滤镜互斥（只能选一个）
    m_CvActsGroup = std::make_unique<QActionGroup>(this);
    // ★ 把所有 CV 滤镜 action 加进 group
    m_CvActsGroup->addAction(ui->actionRotate);
    m_CvActsGroup->addAction(ui->actionRepeat);
    m_CvActsGroup->addAction(ui->actionEqualizeHist);
    m_CvActsGroup->addAction(ui->actionThreshold);
    m_CvActsGroup->addAction(ui->actionThreshold_Adaptive);
    m_CvActsGroup->addAction(ui->actionReverse);
    m_CvActsGroup->addAction(ui->actionColorReduce);
    m_CvActsGroup->addAction(ui->actionGamma);
    m_CvActsGroup->addAction(ui->actionContrastBright);
    m_CvActsGroup->addAction(ui->actionCanny);
    m_CvActsGroup->addAction(ui->actionBlur);
    m_CvActsGroup->addAction(ui->actionSobel);
    m_CvActsGroup->addAction(ui->actionLaplacian);
    m_CvActsGroup->addAction(ui->actionScharr);
    m_CvActsGroup->addAction(ui->actionPrewitt);
    m_CvActsGroup->addAction(ui->actionRemoveCV);   // ★ "无"选项

#if NDEBUG
    // ★ release 版本隐藏测试菜单项
    ui->actionTest_CV->setVisible(false);
#endif

    // ★ 默认选中"无"（actionRemoveCV = 不过任何 CV 滤镜）
    ui->actionRemoveCV->setChecked(true);
    QString tips =
        "Please be careful to enable these features, some of them may "
        "cause this program to freezing if your CPU is not real-time capable. "
        "But you can select a low-resolution video for testing these features.";
    // ★ 警告用户：低性能 CPU 上开启 CV 滤镜可能让播放器卡死
    ui->actionRemoveCV->setToolTip(tips);
}

/**
 * @brief 初始化"最近文件"菜单的 Action 列表
 *        Action 默认隐藏，由 update_recentfile_actions() 同步
 */
void MainWindow::create_recentfiles_menu()
{
    // ★ 预创建 MaxRecentFiles 个 Action（Qt 设计：菜单项个数是固定的）
    for (int i = 0; i < MaxRecentFiles; ++i)
    {
        m_recentFileActs[i] = std::make_unique<QAction>(this);
        m_recentFileActs[i]->setVisible(false);  // ★ 默认隐藏
        connect(m_recentFileActs[i].get(), SIGNAL(triggered()), this, SLOT(open_recentFile()));
    }

    // ★ 单独的"清空最近文件"项
    m_recentClear = std::make_unique<QAction>(this);
    m_recentClear->setText(QApplication::translate("MainWindow", "清空", nullptr));
    connect(m_recentClear.get(), SIGNAL(triggered()), this, SLOT(clear_recentfiles()));

    auto pMenu = ui->menuRecent_Files;
    // pMenu->addSeparator();
    for (int i = 0; i < MaxRecentFiles; ++i)
        pMenu->addAction(m_recentFileActs[i].get());
    pMenu->addSeparator();
    pMenu->addAction(m_recentClear.get());

    // ★ 启动时按已有记录刷新菜单显示
    update_recentfile_actions();
}

/**
 * @brief 记录当前播放的文件名
 *        - 加到最近文件列表（去重，最新优先）
 *        - 触发 update_recentfile_actions() 同步菜单
 *
 * @param fileName 完整文件路径
 */
void MainWindow::set_current_file(const QString& fileName)
{
    // ★ Qt 的任务栏/标题栏会用这个显示"当前文档"名
    setWindowFilePath(fileName);

    // ★ 读出当前最近文件列表
    auto files = m_settings.get_recentfiles().toStringList();
    // ★ removeAll + prepend 模式：把已存在的项移到最前（去重 + 最新优先）
    files.removeAll(fileName);
    files.prepend(fileName);
    // ★ 超出 MaxRecentFiles 限制就把最旧的丢掉
    while (files.size() > MaxRecentFiles)
        files.removeLast();

    m_settings.set_recentfiles(files);

    // ★ 同步刷新菜单 UI
    update_recentfile_actions();
}

/**
 * @brief 清空最近文件列表
 */
void MainWindow::clear_recentfiles()
{
    auto files = m_settings.get_recentfiles().toStringList();
    files.clear();
    m_settings.set_recentfiles(files);

    update_recentfile_actions();
}

/**
 * @brief 从最近文件列表里移除指定文件
 *
 * @param fileName 要移除的完整文件路径
 */
void MainWindow::remove_recentfiles(const QString& fileName)
{
    auto files = m_settings.get_recentfiles().toStringList();
    files.removeAll(fileName);
    m_settings.set_recentfiles(files);

    update_recentfile_actions();
}

/**
 * @brief 用 QSettings 里的列表刷新最近文件菜单的显示
 */
void MainWindow::update_recentfile_actions()
{
    auto files = m_settings.get_recentfiles().toStringList();

    // ★ qMin 防止 QSettings 里被外部改成超过上限的数量
    int numRecentFiles = qMin(files.size(), (int)MaxRecentFiles);

    // ★ 一个文件都没有就把整个菜单 disable 掉
    ui->menuRecent_Files->setEnabled(numRecentFiles > 0);

    // ★ 把前 numRecentFiles 个 action 显示出来，标号 1~N
    for (int i = 0; i < numRecentFiles; ++i)
    {
        QString text = tr("%1 %2").arg(i + 1).arg(stripped_name(files[i]));
        m_recentFileActs[i]->setText(QApplication::translate("MainWindow", text.toStdString().c_str(), nullptr));
        m_recentFileActs[i]->setData(files[i]);
        m_recentFileActs[i]->setVisible(true);
    }
    // ★ 剩下的 action 隐藏掉
    for (int j = numRecentFiles; j < MaxRecentFiles; ++j)
        m_recentFileActs[j]->setVisible(false);
}

/**
 * @brief 取文件名的"显示名"（带扩展名，不带路径）
 *
 * @param fullFileName 完整文件路径
 * @return QString 比如 "C:/a/b/c.mp4" -> "c.mp4"
 */
QString MainWindow::stripped_name(const QString& fullFileName) const
{
    return QFileInfo(fullFileName).fileName();
}

/**
 * @brief 最近文件菜单被点击时的槽函数
 *        拿到 action 里存的 data（即完整文件路径），开始播放
 */
void MainWindow::open_recentFile()
{
    // ★ qobject_cast + sender() 是 Qt 经典模式：获取"是谁发的信号"
    if (auto action = qobject_cast<QAction*>(sender()))
        start_to_play(action->data().toString());
}

/**
 * @brief 打开一个 dump_format 格式化的媒体信息对话框
 *        显示码率、时长、流信息（视频/音频/字幕）等
 */
void MainWindow::about_media_info()
{
    if (!m_pVideoState)
        return;

    auto pState = m_pVideoState->get_state();
    if (!pState)
        return;

    if (auto ic = pState->ic)
    {
        // ★ FFmpeg 自带的 dump_format 会把容器/流/编解码信息整理成字符串
        auto str = dump_format(ic, 0, pState->filename);
        // ★ 用 min-width 760px 的样式让信息框不会太窄
        show_msg_dlg(str, "Media information", "QLabel{min-width: 760px;}");
    }
}

/**
 * @brief 创建底部播放控制条（PlayControlWnd）
 *        高度固定 65，无边框，作为工具窗口浮在视频上方
 */
void MainWindow::create_play_control()
{
    m_play_control_wnd = std::make_unique<PlayControlWnd>(this);
    m_play_control_wnd->setObjectName(QString::fromUtf8("play_control"));
    // ★ 高度固定 65，宽度先设 0（后面 update_play_control 会再调整）
    m_play_control_wnd->setGeometry(0, 0, 0, 65);
    // ★ 宽度可被外层拉伸、高度固定
    m_play_control_wnd->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    // ★ Tool 标志：浮动在主窗口之上；Frameless：无标题栏边框
    m_play_control_wnd->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
}

/**
 * @brief 把视频标签 resize 到占满 centralWidget
 */
void MainWindow::update_video_label()
{
    auto sizeCenter = centralWidget()->size();
    if (auto pLabel = get_video_label())
        pLabel->resize(sizeCenter.width(), sizeCenter.height());
}

/**
 * @brief 弹一个模态消息框（默认在主窗口中央）
 *
 * @param message 要显示的文本
 * @param windowTitle 对话框标题
 * @param styleSheet 可选的样式表（用于控制对话框内控件样式）
 */
void MainWindow::show_msg_dlg(const QString& message, const QString& windowTitle, const QString& styleSheet)
{
    QMessageBox msgBox;

    msgBox.setText(message);
    msgBox.setWindowTitle(windowTitle);
    msgBox.setStyleSheet(styleSheet);

    // ★ 先 show() 再 move() 才能让 rect() 拿到真实大小
    msgBox.show();
    msgBox.move(frameGeometry().center() - msgBox.rect().center());
    // ★ Dialog 标志让它有标准对话框外观
    msgBox.setWindowFlags(msgBox.windowFlags() | Qt::Dialog /*| Qt::WindowStaysOnTopHint*/);
    // ★ 模态：阻塞主窗口事件循环直到用户关闭
    msgBox.setModal(true);
    msgBox.exec();
}

/**
 * @brief 调整底部播放控制条的位置和宽度
 *        紧贴主窗口的客户区底部，宽度撑满
 */
void MainWindow::update_play_control()
{
    if (auto pPlayControl = get_play_control())
    {
        // ★ 跟 centralWidget 同宽（撑满播放区）
        auto sizeCenter = centralWidget()->size();

        pPlayControl->resize(sizeCenter.width(), pPlayControl->size().height());

        auto frameGeoRt = frameGeometry();
        auto geoRt = geometry();

        // QPoint pt = ui->statusbar->pos();
        // ★ frameGeometry 和 geometry 差值 = 标题栏 + 边框的高度
        //   borderH 暂时没在用（下方用的是 size().height() - 1），但保留注释方便未来调
        //int borderH = frameGeoRt.height() - (geoRt.y() - frameGeoRt.y()) - geoRt.height();
        //int borderH = frameGeoRt.height() - geoRt.height();
        // int borderw = frameGeoRt.width() - geoRt.width();
        // int borderSelf = pPlayControl->frameGeometry().height() - (pPlayControl->geometry().y() - pPlayControl->frameGeometry().y()) - pPlayControl->geometry().height();

        // ★ 让控件底部贴齐客户区底部（-1 是为了避免 1px 重叠让边框出现双线）
        auto pt = geoRt.bottomLeft() - QPoint(0, pPlayControl->size().height() - 1);
        pPlayControl->move(pt);
    }
}

/**
 * @brief 把默认背景图（res/music.png）贴到视频标签上
 *        用在：刚启动时、播放停止时
 */
void MainWindow::set_default_bkground()
{
    // ★ ":/images/" 是 Qt 资源路径前缀，图片在编译时打包到 exe 里
    QImage img(":/images/res/music.png");
    update_image(img);
}

/**
 * @brief 打印当前窗口/控件的几何信息（debug 用）
 */
void MainWindow::print_size() const
{
    auto rt = geometry();
    qDebug("geometry rt:(x:%d, y:%d, w:%d, h:%d)", rt.x(), rt.y(), rt.width(), rt.height());
    rt = frameGeometry();
    qDebug("frameGeometry rt:(x:%d, y:%d, w:%d, h:%d)", rt.x(), rt.y(), rt.width(), rt.height());

    auto size = this->size();
    qDebug("window size:(%d,%d)", size.width(), size.height());
    /*size = event->size();
    qDebug("event size:(%d,%d)", size.width(), size.height());*/
    size = ui->centralwidget->size();
    qDebug("centralwidget size:(%d,%d)", size.width(), size.height());
    size = ui->menubar->size();
    qDebug("menubar size:(%d,%d)", size.width(), size.height());
    /*size = ui->statusbar->size();
    qDebug("statusbar size:(%d,%d)", size.width(), size.height());*/

    //auto pt = ui->statusbar->pos();
    // pt = ui->statusbar->mapToParent(pt);
    //qDebug("statusbar pt (x:%d, y:%d)", pt.x(), pt.y());
    // pt = menuBar()->mapToParent(QPoint(0, 0));
    auto pt = ui->menubar->pos();
    qDebug("menuBar pt (x:%d, y:%d)", pt.x(), pt.y());
}

void MainWindow::print_screen() const
{
    auto screen = QApplication::primaryScreen();
    auto rt = screen->availableGeometry();
    qDebug("availableGeometry rt (x:%d, y:%d, width:%d, height:%d)", rt.x(), rt.y(), rt.width(), rt.height());

    auto sz = screen->availableSize();
    qDebug("availableSize sz (width:%d, height:%d)", sz.width(), sz.height());

    sz = screen->size();
    qDebug("size sz (width:%d, height:%d)", sz.width(), sz.height());

    rt = screen->virtualGeometry();
    qDebug("virtualGeometry rt (x:%d, y:%d, width:%d, height:%d)", rt.x(), rt.y(), rt.width(), rt.height());

    sz = screen->virtualSize();
    qDebug("virtualSize sz (width:%d, height:%d)", sz.width(), sz.height());

    rt = screen->availableVirtualGeometry();
    qDebug("availableVirtualGeometry rt (x:%d, y:%d, width:%d, height:%d)", rt.x(), rt.y(), rt.width(), rt.height());

    sz = screen->availableVirtualSize();
    qDebug("availableVirtualSize sz (width:%d, height:%d)", sz.width(), sz.height());

    rt = screen->geometry();
    qDebug("geometry rt (x:%d, y:%d, width:%d, height:%d)", rt.x(), rt.y(), rt.width(), rt.height());

    auto depth = screen->depth();
    qDebug() << "depth :" << depth;

    auto ratio = screen->devicePixelRatio();
    qDebug() << "devicePixelRatio :" << ratio;

    auto dot_per_inch = screen->logicalDotsPerInch();
    qDebug() << "logicalDotsPerInch :" << dot_per_inch;

    auto x = screen->logicalDotsPerInchX();
    qDebug() << "logicalDotsPerInchX :" << x;

    auto y = screen->logicalDotsPerInchY();
    qDebug() << "logicalDotsPerInchY :" << y;

    auto str = screen->manufacturer();
    qDebug() << "manufacturer :" << str;

    str = screen->model();
    qDebug() << "model :" << str;

    str = screen->name();
    qDebug() << "name :" << str;

    str = screen->serialNumber();
    qDebug() << "serialNumber :" << str;

    auto o = screen->nativeOrientation();
    qDebug() << "nativeOrientation :" << o;

    o = screen->orientation();
    qDebug() << "orientation :" << o;

    o = screen->primaryOrientation();
    qDebug() << "primaryOrientation :" << o;

    auto ph_d = screen->physicalDotsPerInch();
    qDebug() << "physicalDotsPerInch :" << ph_d;

    ph_d = screen->physicalDotsPerInchX();
    qDebug() << "physicalDotsPerInchX :" << ph_d;

    ph_d = screen->physicalDotsPerInchY();
    qDebug() << "physicalDotsPerInchY :" << ph_d;

    auto sz_f = screen->physicalSize();
    qDebug() << "physicalSize :" << sz_f;

    auto fr = screen->refreshRate();
    qDebug() << "refreshRate :" << fr;
}

/**
 * @brief 窗口尺寸变化事件：同步更新视频标签和播放控件
 *
 * @param event Qt 框架传入的尺寸变化事件
 */
void MainWindow::resizeEvent(QResizeEvent* event)
{
    // 窗口大小变化时同步视频标签和播放控件
    update_video_label();
    update_play_control();

    QMainWindow::resizeEvent(event);
}

/**
 * @brief 窗口位置变化事件：同步更新播放控件位置
 *
 * 因为播放控件是独立的 Tool 窗口，主窗口移动时它不会自动跟随，
 * 所以这里手动调 update_play_control() 让它跟着对齐。
 *
 * @param event Qt 框架传入的移动事件
 */
void MainWindow::moveEvent(QMoveEvent* event)
{
    // 窗口位置变化时同步播放控件（播放控件是独立 Tool 窗口）
    update_play_control();
    QMainWindow::moveEvent(event);
}

/**
 * @brief 键盘快捷键集中处理入口
 *
 * | 键              | 动作            |
 * |-----------------|----------------|
 * | Space           | 暂停/继续       |
 * | Up/Down         | 音量增减        |
 * | Left/Right      | 后退/前进 2 秒  |
 * | M               | 静音/解除       |
 * | , / .           | 减速/加速       |
 * | A               | 保持宽高比      |
 * | O               | 原始尺寸        |
 * | L               | 显示播放列表     |
 * | F               | 切换全屏        |
 * | Esc             | 退出全屏        |
 * | H               | 弹出快捷键帮助   |
 */
void MainWindow::keyPressEvent(QKeyEvent* event)
{
    // ★ 调试日志：记录每个按键（方便排查快捷键问题）
    qDebug() << "Mainwindow key event, event:" << event->text()
             << "key:" << event->key()
             << "key_str:" << QKeySequence(event->key()).toString();

    switch (event->key())
    {
        // ★ 8 个播放相关快捷键都丢给 play_control_key 统一处理
        case Qt::Key_Space:  // pause/continue
        case Qt::Key_Up:     // volume up
        case Qt::Key_Down:   // volume down
        case Qt::Key_Left:   // play back
        case Qt::Key_Right:  // play forward
        case Qt::Key_M:      // mute
        case Qt::Key_Comma:  // speed down
        case Qt::Key_Period: // speed up
            play_control_key((Qt::Key)event->key());
            break;

        case Qt::Key_A: // aspect ratio
            on_actionAspect_Ratio_triggered();
            break;

        case Qt::Key_O: // keep orginal size
            on_actionOriginalSize_triggered();
            break;

        case Qt::Key_L: // show play list wnd
        {
            show_playlist();
            // ★ 同步勾选菜单项，让 UI 状态保持一致
            ui->actionPlayList->setChecked(true);
        }
        break;

        case Qt::Key_F: // full screen
        {
            bool bFullscreen = label_fullscreen();
            show_fullscreen(!bFullscreen);
            // ★ 同步勾选菜单项
            ui->actionFullscreen->setChecked(!bFullscreen);
        }
        break;

        case Qt::Key_Escape:
        {
            show_fullscreen(false);
            // ★ 同步勾选菜单项
            ui->actionFullscreen->setChecked(false);
        }
        break;

        case Qt::Key_H:
            on_actionKeyboard_Usage_triggered();
            break;

        default:
            qDebug("Not handled key event, key:%s(%d) pressed!\n", qUtf8Printable(event->text()), event->key());
            // ★ 没处理的键交给基类（实现 Tab 焦点切换等默认行为）
            QWidget::keyPressEvent(event);
            break;
    }
}

/**
 * @brief 全局事件过滤器：主要处理鼠标移动
 *        - 全屏时根据 y 坐标决定显示/隐藏菜单栏
 *        - 鼠标在播放控件内时停止自动隐藏计时器
 *        - release 模式下进入播放控件时强制显示光标
 */
bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseMove)
    {
        auto mouseEvent = static_cast<QMouseEvent*>(event);
        // displayStatusMessage(QString("Mouse move
        // (%1,%2)").arg(mouseEvent->pos().x()).arg(mouseEvent->pos().y()));

        // ★ 全屏时根据鼠标 y 坐标决定菜单栏是否显示（悬浮顶部才显示）
        check_hide_menubar(mouseEvent->pos());

#if AUTO_HIDE_PLAYCONTROL
        // ★ 条件1：用户没主动选择隐藏控件；条件2：当前不是全屏
        if (!(ui->actionHide_Play_Ctronl->isChecked() || label_fullscreen()))
        {
            if (cursor_in_window(get_play_control()))
            {
                // ★ 鼠标在播放控件内：停掉自动隐藏计时器、强制显示
                m_timer.stop();
                auto_hide_play_control(false);
            }
            else
            {
                // ★ 鼠标在控件外：重新启动 3 秒计时
                m_timer.start();
            }
        }

        // ★ 鼠标一移动就显示光标（不管之前是不是被隐藏了）
        hide_cursor(false);
        setCursor(Qt::ArrowCursor);
#endif
    }
    return QMainWindow::eventFilter(obj, event);
}

/**
 * @brief 拖放文件支持：把第一个 URL 转成本地路径开始播放
 *
 * @param event Qt 拖放事件（包含拖进来的文件信息）
 */
void MainWindow::dropEvent(QDropEvent* event)
{
    auto mimeData = event->mimeData();

    // ★ 没拖文件就什么都不做
    if (!mimeData->hasUrls())
        return;

    // ★ 只处理第一个 URL（多文件拖入只播第一个）
    if (auto urlList = mimeData->urls(); urlList.size() > 0)
        start_to_play(urlList.at(0).toLocalFile());
}

/**
 * @brief 拖拽进入事件：必须有 URL 才接受拖放
 *
 * @param event Qt 拖拽进入事件
 */
void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (auto mimeData = event->mimeData(); mimeData->hasUrls())
        // ★ acceptProposedAction() 告诉 Qt"我能处理这个拖放"
        event->acceptProposedAction();
    event->accept();
}

/**
 * @brief 根据鼠标 y 坐标决定是否隐藏菜单栏
 *
 * 全屏时：鼠标在顶部几像素内显示菜单栏，移开后自动隐藏。
 * 非全屏时：什么都不做。
 *
 * @param pt 当前鼠标位置（窗口坐标）
 */
void MainWindow::check_hide_menubar(const QPoint& pt)
{
    if (isFullScreen())
        // ★ y 大于菜单栏高度 = 鼠标不在菜单区，隐藏菜单栏
        hide_menubar(pt.y() > menuBar()->geometry().height());
}

/**
 * @brief 定时器回调：3 秒无操作就自动隐藏播放控件
 */
void MainWindow::check_hide_play_control()
{
    // ★ 没在播放就什么都不做
    if (!is_playing())
        return;

    // ★ 非全屏且鼠标在控件上：不隐藏（让用户能继续操作）
    if (!isFullScreen() && cursor_in_window(get_play_control()))
    {
        qDebug() << "Cursor is in PlayControl window, don't hide it.";
        return;
    }

    // ★ 时间到，隐藏控件 + 隐藏光标
    auto_hide_play_control();
    hide_cursor();
}

/**
 * @brief 自动隐藏播放控件的统一入口
 *
 * 一连串早返回检查各种条件，简化上层调用代码。
 *
 * @param bHide true=隐藏；false=显示
 */
void MainWindow::auto_hide_play_control(bool bHide)
{
    if (!get_play_control())
        return;

    if (!m_pVideoState)
        return;

    if (!m_pVideoState->get_state())
        return;

    // ★ 用户在菜单里勾选了"隐藏控件"，就保持隐藏，不自动切回显示
    if (ui->actionHide_Play_Ctronl->isChecked())
        return;

    hide_play_control(bHide);
}

/**
 * @brief 菜单 File -> Open... 槽
 *        弹文件选择对话框，把选中的文件丢给 start_to_play
 */
void MainWindow::on_actionOpen_triggered()
{
    // ★ 三种过滤器让用户能快速选常见格式，也能选所有文件
    const QStringList filters({"Videos (*.mp4 *.avi *.mkv)",
                               "Audios (*.mp3 *.wav *.wma)", "Any files (*)"});

    QFileDialog dialog(this);
    // ★ ExistingFile 模式：只能选已存在的文件，不允许输入新路径
    dialog.setFileMode(QFileDialog::ExistingFile);
    // dialog.setNameFilter(tr("Videos (*.mp4 *.avi *.mp3)"));
    dialog.setNameFilters(filters);
    // ★ 列表视图（另一种是 Detail 视图，更详细但占空间）
    dialog.setViewMode(QFileDialog::List);

    if (dialog.exec())
    {
        // ★ 只取选中的第一个文件
        start_to_play(dialog.selectedFiles()[0]);
    }
}

/**
 * @brief 菜单 File -> Open Youtube URL 槽
 *        弹 YoutubeUrlDlg 拿 URL，然后丢给 YoutubeUrlThread 去解析
 *        解析成功后会回调 start_yt_play -> start_to_play
 */
void MainWindow::on_actionYoutube_triggered()
{
    YoutubeUrlDlg dialog(this);

    // ★ 恢复上次用户选过的解析选项
    dialog.set_options_index(get_youtube_optionid());

    auto result = dialog.exec();
    if (result == QDialog::Accepted)
    {
        YoutubeUrlDlg::YoutubeUrlData data;
        dialog.get_data(data);

        // ★ 必须是以 https://www.youtube.com/ 开头的合法 URL
        if (data.url.isEmpty() || (!data.url.startsWith("https://www.youtube.com/", Qt::CaseInsensitive)))
        {
            show_msg_dlg("Please input a valid youtube url. ");
            return;
        }

        // ★ 把 & 后面的查询参数砍掉（yt 解析用不到，避免被当成新 URL）
        if (auto pos = data.url.indexOf("&"); pos != -1) // remove url parameters, all chars after '&'
            data.url.truncate(pos);

        start_youtube_url_thread(data);

        // ★ 保存用户选的解析选项，下次自动恢复
        set_youtube_optionid(dialog.get_options_index());
    }
}

/**
 * @brief 菜单 View -> Aspect Ratio 触发的槽（保持视频宽高比）
 */
void MainWindow::on_actionAspect_Ratio_triggered()
{
    keep_aspect_ratio();
}

/**
 * @brief 菜单 View -> System Style 触发的槽
 *        从 action->data() 里取风格名，丢给 PlayerSkin 应用
 */
void MainWindow::on_actionSystemStyle()
{
    if (auto act = qobject_cast<QAction*>(sender()))
    {
        auto str = act->data().toString();
        qDebug("style menu clicked:%s", qUtf8Printable(str));
        m_skin.set_system_style(str);
    }
}

/**
 * @brief 菜单 View -> Custom Style 触发的槽
 *        从 action->data() 里取 QSS 文件名，丢给 PlayerSkin 应用
 */
void MainWindow::on_actionCustomStyle()
{
    if (auto act = qobject_cast<QAction*>(sender()))
    {
        auto str = act->data().toString();
        qDebug("custom style menu clicked:%s", qUtf8Printable(str));
        m_skin.set_custom_style(str);
    }
}

/**
 * @brief 菜单 File -> Quit 触发的槽
 */
void MainWindow::on_actionQuit_triggered()
{
    // ★ 直接调基类 close()，会触发 closeEvent 走正常退出流程
    QMainWindow::close();
}

/**
 * @brief 菜单 Help 触发的槽（目前是空实现，预留）
 */
void MainWindow::on_actionHelp_triggered()
{
}

/**
 * @brief 菜单 Playback -> Stop 触发的槽
 */
void MainWindow::on_actionStop_triggered()
{
    stop_play();
}

/**
 * @brief 菜单 View -> Hide Play Control 触发的槽
 */
void MainWindow::on_actionHide_Play_Ctronl_triggered()
{
    // ★ 勾选时隐藏，取消勾选时显示
    hide_play_control(ui->actionHide_Play_Ctronl->isChecked());
}

/**
 * @brief 菜单 View -> Fullscreen 触发的槽
 */
void MainWindow::on_actionFullscreen_triggered()
{
    show_fullscreen(ui->actionFullscreen->isChecked());
}

/**
 * @brief 菜单 Playback -> Loop 触发的槽
 *        把 loop 标志写到 VideoState，循环逻辑在读流线程里
 */
void MainWindow::on_actionLoop_Play_triggered()
{
    if (!m_pVideoState)
        return;

    if (auto pState = m_pVideoState->get_state())
        pState->loop = int(ui->actionLoop_Play->isChecked());
}

/**
 * @brief 菜单 Tools -> Media Info 触发的槽
 *        没在播放就什么都不做
 */
void MainWindow::on_actionMedia_Info_triggered()
{
    if (is_playing())
        about_media_info();
}

/**
 * @brief 显示按键帮助对话框
 */
void MainWindow::on_actionKeyboard_Usage_triggered()
{
    QString str;
    // ★ 用 tab 缩进，让两列对齐
    QString indent = "		";
    str += "A" + indent + "视频宽高比\n";
    str += "F" + indent + "全屏/退出全屏\n";
    str += "H" + indent + "显示帮助\n";
    str += "L" + indent + "显示播放列表\n";
    str += "M" + indent + "静音/取消静音\n";
    str += "O" + indent + "保持视频原始大小\n";
    str += "Space" + indent + "暂停/播放\n";
    str += "Up" + indent + "音量增加\n";
    str += "Down" + indent + "音量减小\n";
    str += "Left" + indent + "快退\n";
    str += "Right" + indent + "快进\n";
    str += "<" + indent + "减速\n";
    str += ">" + indent + "加速\n";

    show_msg_dlg(str, "键盘控制");
}

/**
 * @brief 把可视化样式发给 AudioEffectGL 窗口
 *
 * @param fmt 可视化样式（柱状/线/饼；采样/频域）
 */
void MainWindow::set_audio_effect_format(const BarHelper::VisualFormat& fmt)
{
    if (m_audio_effect_wnd)
        m_audio_effect_wnd->set_draw_fmt(fmt);
}

/**
 * @brief 菜单 View -> Audio Effect 槽
 *        把当前可视化样式发给 AudioEffectGL，然后居中弹窗 + 启动音频采样数据
 */
//弹出一个音频可视化窗口 （比如频谱条、波形图），把当前播放的音频数据"推"给它显示。
void MainWindow::popup_audio_effect()
{
    if (is_playing())
    {
        // ★ 从菜单项状态构造 VisualFormat
        BarHelper::VisualFormat fmt;
        get_avisual_format(fmt);
        set_audio_effect_format(fmt);
        show_audio_effect();
        // ★ 通知音频播放线程开始往可视化窗口推数据
        start_send_data();
    }
}

/**
 * @brief 重载版本：直接接受 QSize 调用下面那个
 *
 * @param size 目标窗口尺寸
 */
void MainWindow::resize_window(const QSize& size)
{
    resize_window(size.width(), size.height());
}

/**
 * @brief 调整窗口大小（带边界检查）
 *        - 超过屏幕：自动最大化
 *        - 小于最小尺寸：夹紧
 *        - 移出屏幕：自动居中
 *
 * @param width  目标宽度（逻辑像素）
 * @param height 目标高度（逻辑像素）
 */
void MainWindow::resize_window(int width, int height)
{
    auto pt = this->pos();
    auto screen_rec = screen_rect();

#if !NDEBUG
    QString msg = QString("resize window: w:%1, h:%2, screenW:%3,screenH:%4").arg(width).arg(height).arg(screen_rec.width()).arg(screen_rec.height());
    qInfo("%s", qUtf8Printable(msg));
#endif

    // ★ 比屏幕还大就直接最大化
    if (width > screen_rec.width() || height > screen_rec.height())
    {
        showMaximized();
        return;
    }

    // ★ 夹紧到最小尺寸
    width = width < minimumWidth() ? minimumWidth() : width;
    height = height < minimumHeight() ? minimumHeight() : height;

    resize(width, height);

    if (width != screen_rec.width() || height != screen_rec.height())
    {
        // ★ 不是全屏大小就强制退出全屏（避免 resize 不生效）
        show_fullscreen(false);
    }

    // ★ resize 之后如果新位置会出屏幕，就自动居中
    if (pt.x() + width > screen_rec.width() || pt.y() + height > screen_rec.height())
    {
        center_window(screen_rec);
    }
}

/**
 * @brief 把窗口移到屏幕正中央并显示
 *
 * @param screen_rec 屏幕矩形
 */
void MainWindow::center_window(QRect screen_rec)
{
    auto x = (screen_rec.width() - width()) / 2;
    auto y = (screen_rec.height() - height()) / 2;
    move(x, y);
    show();
}

/**
 * @brief 切换视频标签的全屏状态
 *        全屏时同时隐藏光标
 *
 * @param bFullscreen true=进入全屏；false=退出全屏
 */
void MainWindow::show_fullscreen(bool bFullscreen)
{
    if (auto pLabel = get_video_label())
        pLabel->show_fullscreen(bFullscreen);

    // ★ 退出全屏后要重新让视频标签占满 centralWidget
    if (!bFullscreen)
        update_video_label();

    // ★ 全屏时隐藏鼠标，让观影更沉浸
    hide_cursor(bFullscreen);
}

/**
 * @brief 查询当前视频标签是否在全屏
 *
 * @return bool true=全屏中
 */
bool MainWindow::label_fullscreen()
{
    if (auto pLabel = get_video_label())
        return pLabel->isFullScreen();
    return false;
}

/**
 * @brief 调整窗口大小以保持视频宽高比（默认以宽度为基准）
 *        如果新尺寸超过屏幕会等比例缩小到屏幕内
 *
 * @param bWidth true=以宽为基准算高；false=以高为基准算宽
 */
void MainWindow::keep_aspect_ratio(bool bWidth)
{
    if (!m_pVideoState)
        return;

    auto pVideoCtx = m_pVideoState->get_contex(AVMEDIA_TYPE_VIDEO);
    auto pLabel = get_video_label();
    if (!pVideoCtx || !pLabel)
        return;

    auto sizeLabel = pLabel->size();
    auto sz = size();
    auto screen_rt = screen_rect();
    auto video_sz = display_video_size(pVideoCtx);

    int h_change = 0;
    int w_change = 0;
    if (bWidth)
    {
        // ★ 以宽度为基准算高度 = 标签宽 × 视频高/视频宽
        h_change = sizeLabel.width() * video_sz.height() / video_sz.width() - sizeLabel.height();
        sz += QSize(0, h_change);
    }
    else
    {
        // ★ 以高度为基准算宽度
        w_change = sizeLabel.height() * video_sz.width() / video_sz.height() - sizeLabel.width();
        sz += QSize(w_change, 0);
    }

    // size greater than screen size
    // ★ 等比缩到屏幕内
    if (sz.width() > screen_rt.width())
    {
        w_change = screen_rt.width() - sz.width();
        h_change = w_change * video_sz.height() / video_sz.width();
        sz += QSize(w_change, h_change);
    }

    if (sz.height() > screen_rt.height())
    {
        h_change = screen_rt.height() - sz.height();
        w_change = h_change * video_sz.width() / video_sz.height();
        sz += QSize(w_change, h_change);
    }

    resize_window(sz);
}

/**
 * @brief 切换原始视频尺寸（按视频实际分辨率 resize 窗口）
 *        同样受最小尺寸和屏幕尺寸限制
 */
void MainWindow::on_actionOriginalSize_triggered()
{
    if (!m_pVideoState)
        return;

    auto pVideoCtx = m_pVideoState->get_contex(AVMEDIA_TYPE_VIDEO);
    auto pLabel = get_video_label();
    if (!pVideoCtx || !pLabel)
        return;

    auto sizeLabel = pLabel->size();
    auto sz = size();

    // ★ 视频原始尺寸
    auto video_sz = display_video_size(pVideoCtx);
    int new_width = video_sz.width();
    int new_height = video_sz.height();

    // ★ 宽度太小就按比例放大（保持原宽高比）
    if (new_width < minimumWidth())
    {
        new_height = minimumWidth() * new_height / new_width;
        new_width = minimumWidth();
    }

    if (new_height < minimumHeight())
    {
        new_width = minimumHeight() * new_width / new_height;
        new_height = minimumHeight();
    }

    int w_change = new_width - sizeLabel.width();
    int h_change = new_height - sizeLabel.height();

    sz += QSize(w_change, h_change);
    resize_window(sz);
}

/**
 * @brief 显示/隐藏播放控件条
 *
 * @param bHide true=隐藏；false=显示
 */
void MainWindow::hide_play_control(bool bHide)
{
    if (auto pPlayControl = get_play_control())
    {
        if (pPlayControl->isVisible() == bHide)
        {
            // ★ 状态变了才真正调 setVisible（避免无效的窗口系统调用）
            pPlayControl->setVisible(!bHide);
        }
    }
}

/**
 * @brief 初始化播放控件
 *        set=true：把文件总时长写到进度条上
 *        set=false：清空（停止播放时调用）
 *
 * @param set true=绑定当前文件；false=清空控件
 */
void MainWindow::set_paly_control_wnd(bool set)
{
    auto pPlayControl = get_play_control();
    if (!pPlayControl)
        return;

    if (set)
    {
        if (!m_pVideoState)
            return;

        auto pState = m_pVideoState->get_state();
        if (!pState)
            return;

        if (auto ic = pState->ic)
        {
            int64_t hours, mins, secs, us;
            // ★ 从 FFmpeg 拿到总时长，拆成时:分:秒显示
            get_duration_time(ic->duration, hours, mins, secs, us);
            pPlayControl->set_total_time(hours, mins, secs);
        }
    }
    else
    {
        pPlayControl->clear_all();
    }
}

/**
 * @brief 调音量（步进 unit），并写到设置文件
 *        超出范围会 beep 提示
 *
 * @param bUp   true=音量+；false=音量-
 * @param unit  步进值（默认 0.05，可省略）
 */
void MainWindow::set_volume_updown(bool bUp, float unit)
{
    if (!m_pAudioPlayThread)
        return;

    auto volume = m_pAudioPlayThread->get_device_volume();
    auto n_volume = volume;
    if (bUp)
    {
        n_volume += unit;
    }
    else
    {
        n_volume -= unit;
    }

    // ★ 超出范围就 beep 一下（系统音效）
    if (n_volume > 1.0 || n_volume < 0)
    {
        QApplication::beep();
    }

    // ★ 夹紧到合法范围
    n_volume = n_volume > 1.0 ? 1.0 : n_volume;
    n_volume = n_volume < 0 ? 0 : n_volume;

    // ★ 写实际值 + 同步 UI 滑块
    set_volume(int(n_volume * 100));
    update_paly_control_volume();
}

/**
 * @brief 把音频设备的当前音量同步到 UI 滑块
 */
void MainWindow::update_paly_control_volume()
{
    if (!m_pAudioPlayThread)
        return;

    if (auto pPlayControl = get_play_control())
        pPlayControl->set_volume_slider(m_pAudioPlayThread->get_device_volume());
}

/**
 * @brief 把 VideoState 里的静音状态同步到 UI 控件
 */
void MainWindow::update_paly_control_muted()
{
    if (!m_pVideoState)
        return;

    if (auto pPlayControl = get_play_control())
    {
        if (auto pState = m_pVideoState->get_state())
            // ★ pState->muted 是 int（0/1），但 volume_muted 槽函数要 Qt::CheckState 枚举
            //   static_cast 把 0 → Unchecked, 非0 → Checked
            pPlayControl->volume_muted(static_cast<Qt::CheckState>(pState->muted));
    }
}

/**
 * @brief 把 VideoState 里的暂停状态同步到 UI 控件（播放/暂停按钮图标）
 */
void MainWindow::update_paly_control_status()
{
    if (!m_pVideoState)
        return;

    if (auto pPlayControl = get_play_control())
    {
        if (auto pState = m_pVideoState->get_state())
            // ★ !! 把 int 转成 bool（暂停时显示"播放"图标）
            pPlayControl->update_btn_play(!!pState->paused);
    }
}

/**
 * @brief 把当前播放时间同步到 UI 控件（进度条 + 时间标签）
 */
void MainWindow::update_play_time()
{
    if (!m_pVideoState)
        return;

    if (auto pPlayControl = get_play_control())
    {
        if (auto pState = m_pVideoState->get_state())
            pPlayControl->update_play_time(pState->audio_clock);
    }
}

/**
 * @brief 相对当前位置前进/后退 incr 秒
 *        比如按 Left/Right 键时调用
 *
 * @param incr 增量秒数（负数=后退，正数=前进）
 */
void MainWindow::video_seek_inc(double incr) // incr seconds
{
    if (!m_pVideoState)
        return;

    auto pState = m_pVideoState->get_state();
    if (!pState)
        return;

    // ★ 取主时钟当前位置
    auto pos = get_master_clock(pState);
    if (isnan(pos))
        // ★ 还没起时钟就拿上次 seek 位置兜底
        pos = (double)pState->seek_pos / AV_TIME_BASE;

    pos += incr;
    video_seek(pos, incr);
}

/**
 * @brief 跳转到 pos 秒（相对于文件开头）
 *        会触发 stream_seek -> 各个队列的 serial++，丢弃旧帧
 *
 * @param pos  目标位置（秒，相对文件开头）
 * @param incr 跳转步长（秒，给 FFmpeg 做参考）
 */
void MainWindow::video_seek(double pos, double incr)
{
    if (!m_pVideoState)
        return;

    auto pState = m_pVideoState->get_state();
    if (!pState)
        return;

#if USE_AVFILTER_AUDIO
        // pos /= pState->audio_speed;
#endif

    // ★ 防止跳到负数（某些流 start_time > 0）
    if (pState->ic->start_time != AV_NOPTS_VALUE && pos < pState->ic->start_time / (double)AV_TIME_BASE)
    {
        // qDebug("!seek_by_bytes pos=%lf, start_time=%lf, %lf", pos,
        // pState->ic->start_time, pState->ic->start_time / (double)AV_TIME_BASE);
        pos = pState->ic->start_time / (double)AV_TIME_BASE;
    }

    // ★ AV_TIME_BASE = 1_000_000，转成微秒丢给底层
    stream_seek(pState, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
}

/**
 * @brief 进度条拖动/点击后跳转到对应位置
 *        进度条的值 0..max 对应 0..total_time 秒
 */
void MainWindow::play_seek()
{
    if (auto pPlayControl = get_play_control())
    {
        auto maxValue = pPlayControl->get_progress_slider_max();
        auto total_time = pPlayControl->get_total_time();
        auto value = pPlayControl->get_progress_slider_value();

        double seek_time = 0;
        if (maxValue > 0)
            // ★ 线性映射：value / max = seek_time / total_time
            seek_time = value * total_time * 1.0 / maxValue;

        qDebug() << "val:" << value << ",maxVal:" << maxValue << ",total time" << total_time << ",seek time:" << seek_time;
        video_seek(seek_time);
    }

    update_paly_control_status();
}

/**
 * @brief 快捷后退 2 秒（Left 键）
 */
void MainWindow::play_seek_pre()
{
    video_seek_inc(-2);
}

/**
 * @brief 快捷前进 2 秒（Right 键）
 */
void MainWindow::play_seek_next()
{
    video_seek_inc(2);
}

/**
 * @brief 切换静音状态
 *
 * @param state Qt::CheckState 枚举（Checked=已勾选=静音, Unchecked=未选=有声音）
 */
void MainWindow::play_mute(bool muted)
{
    if (!m_pVideoState)
        return;

    // ★ bool 直接喂给底层 toggle_mute
    //   true = 静音，false = 有声音
    auto pState = m_pVideoState->get_state();
    if (pState)
        toggle_mute(pState, muted);
}

/**
 * @brief 设置系统音量（0~max 整数），并写入设置
 *
 * @param volume 音量值（0~100 范围）
 */
void MainWindow::set_volume(int volume)
{
    auto pPlayControl = get_play_control();
    if (!pPlayControl)
        return;

    if (!m_pAudioPlayThread)
        return;

    auto max_val = pPlayControl->get_volum_slider_max();
    // ★ 整数百分比 → 0~1 浮点
    auto vol = volume * 1.0 / max_val;
    m_pAudioPlayThread->set_device_volume(vol);

    // ★ 同步保存到 QSettings
    volume_settings(true, vol);
}

/**
 * @brief 把 PlayControl 上的速度滑块值同步到 VideoState
 */
void MainWindow::set_play_speed()
{
    auto pPlayControl = get_play_control();
    if (!pPlayControl)
        return;

    auto speed = pPlayControl->get_speed();

    // qDebug("set_play_spped, speed control changed, speed:%d", speed);
    if (m_pVideoState)
    {
        if (auto pState = m_pVideoState->get_state())
        {
#if USE_AVFILTER_AUDIO
            // ★ 启用了音频变速滤镜才设置
            set_audio_playspeed(pState, speed);
#endif
        }
    }
}

/**
 * @brief 速度加/减一档，同时同步 VideoState
 *
 * @param up true=加速；false=减速
 */
void MainWindow::play_speed_adjust(bool up)
{
    if (auto pPlayControl = get_play_control())
        pPlayControl->speed_adjust(up);

    set_play_speed();
}

/**
 * @brief 显示/隐藏状态栏（同时调整主窗口高度补偿）
 *
 * @param bHide true=隐藏状态栏
 */
void MainWindow::hide_statusbar(bool bHide)
{
    statusBar()->setVisible(!bHide);

    auto sz_status = statusBar()->size();

    if (isFullScreen())
    {
        // ★ 全屏时让 centralWidget 重排即可
        centralWidget()->resize(centralWidget()->size());
    }
    else
    {
        auto sz = size();
        if (statusBar()->isVisible())
        {
            // ★ 多了一个状态栏高度要补回去
            sz += QSize(0, sz_status.height());
        }
        else
        {
            // ★ 没了状态栏要缩回去
            sz -= QSize(0, sz_status.height());
        }

        resize(sz);
    }
}

/**
 * @brief 显示/隐藏菜单栏
 *
 * @param bHide true=隐藏菜单栏
 */
void MainWindow::hide_menubar(bool bHide)
{
    menuBar()->setVisible(!bHide);

    // qDebug("is full screen:%d, menu is visible:%d", isFullScreen(), bVisible);
    if (isFullScreen())
    {
        // ★ 全屏时强制让 centralWidget 重排
        centralWidget()->resize(centralWidget()->size());
    }

    // ★ 菜单栏状态变了，底部播放控件的位置也要重新算
    update_play_control();
}

/**
 * @brief 菜单 Help -> About 槽：弹出关于对话框
 */
void MainWindow::on_actionAbout_triggered()
{
    About dlg;
    // ★ 把对话框移到主窗口正中央
    dlg.move(frameGeometry().center() - dlg.rect().center());
    dlg.setModal(true);
    dlg.exec();
}

/**
 * @brief StartPlayThread 完成后回调：所有解码/播放线程一起启动
 *
 * 设计原因：音频设备初始化可能要几百毫秒，所以放在独立线程做。
 * 这里用 play_started() 保证所有线程"同时"开始工作。
 */
void MainWindow::play_started(bool ret)
{
    // ★ ret=false 表示音频设备初始化失败（但视频依然能播）
    if (!ret)
    {
        qWarning("audio device init failed!");
    }

    // ★ 启动所有线程 + 把线程指针注册到 VideoState
    all_thread_start();
    set_threads();
}

/**
 * @brief 把所有线程对象打包到 Threads 结构体，丢给 VideoState
 *        VideoState 内部用这些指针发 abort 信号、join 线程
 */
void MainWindow::set_threads()
{
    if (m_pVideoState)
    {
        // ★ Threads 是 FFmpeg 风格的纯数据结构，存一堆线程指针
        Threads threads;
        threads.read_tid = m_pPacketReadThread.get();
        threads.video_decode_tid = m_pDecodeVideoThread.get();
        threads.audio_decode_tid = m_pDecodeAudioThread.get();
        threads.video_play_tid = m_pVideoPlayThread.get();
        threads.audio_play_tid = m_pAudioPlayThread.get();
        threads.subtitle_decode_tid = m_pDecodeSubtitleThread.get();

        m_pVideoState->threads_setting(m_pVideoState->get_state(), threads);
    }
}

/**
 * @brief 解析成功回调：把 yt 流数据加到本地缓存，然后开始播放
 *
 * @param st_data 解析得到的 yt 流信息（含 URL）
 */
void MainWindow::start_yt_play(const YoutubeJsonParser::YtStreamData& st_data)
{
    remove_yt_list(st_data.stream.url);
    insert_yt_list(st_data.stream.url, st_data);
    start_to_play(st_data.stream.url);
}

/**
 * @brief "开始播放" 入口（点 Open/拖文件/最近文件都会调到这里）
 *
 * 流程：
 *   1. 如果当前正在播放同一个文件，直接返回
 *   2. 如果正在播其他文件，等旧文件停完再播新文件（StopWaitingThread）
 *   3. 记录文件路径，调 start_play() 创建所有播放线程
 *   4. 成功则加入最近文件、播放列表
 *
 * @param file 媒体文件路径（本地或网络 URL）
 */
void MainWindow::start_to_play(const QString& file)
{
    if (is_playing())
    {
#if 1
        // ★ 正在播同一个文件就不重播
        if (m_videoFile == file)
            return;

        // ★ 正在播别的文件就排队等待
        wait_stop_play(file);
#else
        QString str = QString("File(%1) is playing, please stop it first. ").arg(m_videoFile);
        show_msg_dlg(str);
#endif
        return;
    }

    m_videoFile = file;

    if (!start_play())
    {
        // ★ 启动失败：从最近文件里移除 + 弹错误
        remove_recentfiles(file);
        play_failed(m_videoFile);
        return;
    }

    // ★ 启动成功：同步最近文件、播放列表、菜单
    set_current_file(file);
    add_to_playlist(file);
    update_menus();
}

/**
 * @brief 创建 StopWaitingThread，等正在播放的文件停完再播新文件
 *
 * @param file 想要播放的新文件
 */
void MainWindow::wait_stop_play(const QString& file)
{
    m_pStopplayWaitingThread = std::make_unique<StopWaitingThread>(this, file);
    // ★ 旧文件停完后，发 stopPlay 通知主窗口
    connect(m_pStopplayWaitingThread.get(), &StopWaitingThread::stopPlay, this, &MainWindow::stop_play);
    // ★ 旧文件停完后，发 startPlay 开始播新文件
    connect(m_pStopplayWaitingThread.get(), &StopWaitingThread::startPlay, this, &MainWindow::start_to_play);

    m_pStopplayWaitingThread->start();
    qDebug("++++++++++ stopplay waiting thread started.");
}

/**
 * @brief 播放失败：弹错误框，并从最近文件里移除
 *
 * @param file 失败的文件路径
 */
void MainWindow::play_failed(const QString& file)
{
    show_msg_dlg(QString("File play failed, file: %1").arg(toNativePath(file)));
}

/**
 * @brief 是否有任何线程在播放
 *        通过检查所有线程指针是否非空
 *
 * @return bool true=正在播放
 */
bool MainWindow::is_playing() const
{
    if (m_pVideoState || m_pPacketReadThread || m_pDecodeVideoThread ||
        m_pDecodeAudioThread || m_pAudioPlayThread || m_pVideoPlayThread ||
        m_pDecodeSubtitleThread)
    {
        qDebug("Now file is playing, please wait or stop the current playing.\n");
        qDebug("VideoState=%p, PacketRead=%p\n", m_pVideoState.get(), m_pPacketReadThread.get());
        qDebug("VideoDecode=%p, AudioDecode=%p, SubtitleDecode=%p\n", m_pDecodeVideoThread.get(), m_pDecodeAudioThread.get(), m_pDecodeSubtitleThread.get());
        qDebug("VideoPlay=%p, AudioPlay=%p\n", m_pVideoPlayThread.get(), m_pAudioPlayThread.get());

        return true;
    }
    return false;
}

/**
 * @brief 当前媒体是否包含视频流
 *
 * @return bool true=有视频
 */
bool MainWindow::playing_has_video()
{
    if (m_pVideoState)
        return m_pVideoState->has_video();
    return false;
}

/**
 * @brief 当前媒体是否包含音频流
 *
 * @return bool true=有音频
 */
bool MainWindow::playing_has_audio()
{
    if (m_pVideoState)
        return m_pVideoState->has_audio();
    return false;
}

/**
 * @brief 当前媒体是否包含字幕流
 *
 * @return bool true=有字幕
 */
bool MainWindow::playing_has_subtitle()
{
    if (m_pVideoState)
        return m_pVideoState->has_subtitle();
    return false;
}

bool MainWindow::start_play()
{
#if !NDEBUG
    // ★ debug 模式下用计时器记录每个阶段耗时（方便优化）
    QElapsedTimer timer;
    timer.start();
#endif

    bool ret = false;

    QString msg = QString("Start to play file: %1").arg(toNativePath(m_videoFile));
    qInfo("");
    qInfo("%s", qUtf8Printable(msg)); // qPrintable(msg)
    // qInfo("%s", msg);
    // displayStatusMessage(msg);

    // ★ 1) 校验文件名
    if (m_videoFile.isEmpty())
    {
        qWarning("filename is invalid, please select a valid media file.");
        return ret;
    }

#if !NDEBUG
    qDebug("---------------------------------%d milliseconds", timer.elapsed());
#endif

    // ★ 2) 先创建读线程（state 后面再注入）
    // create read thread (video state need read-thread id)
    ret = create_read_thread();
    if (!ret)
    {
        qWarning("packet read thread create failed.\n");
        return ret;
    }

#if !NDEBUG
    qDebug("---------------------------------%d milliseconds", timer.elapsed());
#endif

    // ★ 3) 创建 VideoStateData（这一步 open 文件最耗时，特别是网络流）
    ret = create_video_state(m_videoFile); // time-consuming for open of network url
    if (!ret)
    {
        qWarning("video state create failed.\n");
        read_packet_stopped();
        return ret;
    }

#if !NDEBUG
    qDebug("---------------------------------%d milliseconds", timer.elapsed());
#endif

    assert(m_pVideoState);
    if (!m_pVideoState)
    {
        qWarning("video state error!\n");
        return false;
    }

    // ★ 4) 把 state 注入读线程
    m_pPacketReadThread->set_video_state(m_pVideoState->get_state());

    // ★ 5) 判断文件里有哪些流
    bool bVideo = playing_has_video();
    bool bAudio = playing_has_audio();
    bool bSubtitle = playing_has_subtitle();

    if (bVideo)
    {
        // ★ 先按视频原始尺寸调整窗口
        play_window_size();

        ret = create_decode_video_thread();//创建视频解码线程
        if (!ret)
        {
            qWarning("video decode thread create failed.\n");
            return ret;
        }

        ret = create_video_play_thread();//创建视频播放线程
        if (!ret)
        {
            qWarning("video play thread create failed.\n");
            return ret;
        }
    }

#if !NDEBUG
    qDebug("---------------------------------%d milliseconds", timer.elapsed());
#endif

    if (bAudio)
    {
        ret = create_decode_audio_thread();
        if (!ret)
        {
            qWarning("audio decode thread create failed.\n");
            return ret;
        }

#if !NDEBUG
        qDebug("---------------------------------%d milliseconds", timer.elapsed());
#endif

        ret = create_audio_play_thread();
        if (!ret)
        {
            qWarning("audio play thread create failed.\n");
            return ret;
        }
    }

#if !NDEBUG
    qDebug("---------------------------------%d milliseconds", timer.elapsed());
#endif

    if (bSubtitle)
    {
        ret = create_decode_subtitle_thread();
        if (!ret)
        {
            qWarning("subtitle decode thread create failed.\n");
            return ret;
        }
    }

    if (bAudio)
    {
        // ★ 音频设备初始化很耗时，所以开独立线程做
        // start a thread for time-consuming(audio device init) task
        // play_started would be called after thread
        start_play_thread();
    }
    else
    {
        // ★ 没音频就直接全部启动（不需要等音频设备）
        // if no audio stream but video stream, start all thread
        play_started();
    }

#if !NDEBUG
    qDebug("---------------------------------%d milliseconds", timer.elapsed());
#endif

    return true;
}

/**
 * @brief 启动所有已创建的播放线程
 *        - 音频线程设置为 HighPriority（人耳对音频卡顿更敏感）
 *        - 其他线程用默认优先级
 */
void MainWindow::all_thread_start()
{
    // ★ 同步播放控件状态（按菜单里的"隐藏控件"选项来决定）
    hide_play_control(ui->actionHide_Play_Ctronl->isChecked());

    set_paly_control_wnd();
    update_paly_control_volume();
    update_paly_control_status();

    // start all threads
    if (m_pPacketReadThread)
    {
        m_pPacketReadThread->start(); // QThread::Priority::HighPriority
        qDebug("++++++++++ Read  packets thread started.");
    }

    if (m_pDecodeVideoThread)
    {
        m_pDecodeVideoThread->start();
        qDebug("++++++++++ Video decode thread started.");
    }

    if (m_pDecodeAudioThread)
    {
        // ★ 音频解码设高优先级：人耳对音频卡顿最敏感
        m_pDecodeAudioThread->start(QThread::Priority::HighPriority);
        qDebug("++++++++++ Audio decode thread started.");
    }

    if (m_pDecodeSubtitleThread)
    {
        m_pDecodeSubtitleThread->start();
        qDebug("++++++++++ Subtitle decode thread started.");
    }

    if (m_pVideoPlayThread)
    {
        m_pVideoPlayThread->start();
        qDebug("++++++++++ Video play thread started.");
    }

    if (m_pAudioPlayThread)
    {
        m_pAudioPlayThread->start();
        qDebug("++++++++++ Audio play thread started.");
    }
}

/**
 * @brief 停止播放
 *        1. 释放 VideoStateData（析构里会给所有线程发 abort 信号）
 *        2. 清空播放控件
 *        3. 清空字幕
 *        各线程在 finished 信号里被 reset 掉
 */
/**
 * @brief 停止播放（菜单 Stop / 关窗口 / 播放完成都会走到这里）
 *
 * 流程：
 *   1. emit stop_audio_play_thread()  —— 让音频线程干净退出
 *   2. delete_video_state()           —— 释放 VideoStateData（析构会 abort+join 所有线程）
 *   3. set_paly_control_wnd(false)   —— 恢复未播放状态 UI
 *   4. clear_subtitle_str()          —— 清空字幕
 *
 * 整个函数包了 try/catch 防止某处意外抛 C++ 异常导致 abort()
 */
void MainWindow::stop_play()
{
    try
    {
        // ★ 防重入：read_packet_stopped 和 on_playback_finished 可能同时触发 stop_play
        //   如果所有 unique_ptr 都为 null，说明已经 stop 过了，直接 return
        if (!m_pVideoState && !m_pVideoPlayThread && !m_pAudioPlayThread &&
            !m_pPacketReadThread && !m_pDecodeVideoThread && !m_pDecodeAudioThread)
        {
            qInfo("[stop_play] already stopped, skip");
            return;
        }

        qInfo("[stop_play] start, video_state=%p, audio_play=%p, video_play=%p",
              m_pVideoState.get(), m_pAudioPlayThread.get(), m_pVideoPlayThread.get());

        // ★ 关键修复（2026-08-17 二次修复）：play thread（视频/音频）没有 exec() 事件循环，
        //   emit signal() 是 QueuedConnection，永远收不到 → 永远不退 → use-after-free → abort()
        //   正确做法：直接调 stop_thread()（设 m_bExitThread + wait），同步等线程退出
        //
        //   ★ 重要：这里只能 stop_thread()，不能 reset()！
        //     原因：set_threads() 时把 m_pVideoPlayThread.get() 存进了 is->threads.video_play_tid
        //           如果这里 reset()，unique_ptr 析构 → play thread 对象被 delete
        //           但 is->threads.video_play_tid 仍然指着已 delete 的对象（野指针）
        //           后面 delete_video_state() → ~VideoStateData() → stream_close() → threads_exit_wait()
        //           会执行 is->threads.video_play_tid->wait()  →  use-after-free  →  abort()!
        //   正确做法：stop_thread() 后保留 unique_ptr，让 stream_close() 通过 is->threads.video_play_tid
        //           安全 wait（线程已退出，wait 立即返回）。
        //           等 stream_close() 走完，事件循环处理 queued finished 信号时，
        //           video_play_stopped()/audio_play_stopped() 槽函数才会安全 reset unique_ptr。
        if (m_pVideoPlayThread)
        {
            qInfo("[stop_play] stopping video play thread...");
            m_pVideoPlayThread->stop_thread();   // 直接同步等它退出
            // ★ 故意不 reset：让 stream_close() 安全 wait，槽函数最后再 reset
            qInfo("[stop_play] video play thread stopped");
        }
        if (m_pAudioPlayThread)
        {
            qInfo("[stop_play] stopping audio play thread...");
            m_pAudioPlayThread->stop_thread();   // 直接同步等它退出
            // ★ 故意不 reset：让 stream_close() 安全 wait，槽函数最后再 reset
            qInfo("[stop_play] audio play thread stopped");
        }

        // ★ 步骤：释放 VideoStateData（其析构会发 abort 给 read/decode 线程 + free is）
        //   此时两个 play thread 都已退出，is 不会被访问
        delete_video_state();
        // ★ 清空播放控件（恢复未播放状态）
        set_paly_control_wnd(false);
        // ★ 清空字幕
        clear_subtitle_str();

        qInfo("[stop_play] done");
    }
    catch (const std::exception& e)
    {
        qWarning("[stop_play] caught std::exception: %s", e.what());
    }
    catch (...)
    {
        qWarning("[stop_play] caught unknown exception");
    }
}

/**
 * @brief 暂停/恢复播放
 *        切换 VideoState 里的 paused 标志
 */
void MainWindow::pause_play()
{
    if (!m_pVideoState)
        return;

    if (auto pState = m_pVideoState->get_state())
        // ★ 翻转 paused 标志
        toggle_pause(pState, !pState->paused);

    update_paly_control_status();
}

/**
 * @brief 拖动进度条时调用：先跳转再暂停（拖动过程中视频静止）
 */
void MainWindow::play_start_seek()
{
    play_seek();
    pause_play();
}

/**
 * @brief 集中处理播放相关的快捷键
 *        包括：Space/Up/Down/Left/Right/M/,/.
 */
void MainWindow::play_control_key(Qt::Key key)
{
    if (!m_pVideoState)
        return;

    auto pState = m_pVideoState->get_state();
    if (!pState)
        return;

    switch (key)
    {
        case Qt::Key_Space: // pause/continue
            pause_play();
            break;

        case Qt::Key_M:
            // ★ 翻转 muted
            toggle_mute(pState, !pState->muted);
            // ★ 同步 UI 上的喇叭图标
            update_paly_control_muted();
            break;

        case Qt::Key_Up: // volume
            set_volume_updown(true);
            break;

        case Qt::Key_Down: // volume
            set_volume_updown(false);
            break;

        case Qt::Key_Left:
            play_seek_pre();
            break;

        case Qt::Key_Right:
            play_seek_next();
            break;

        case Qt::Key_Comma:
            play_speed_adjust(false);
            break;

        case Qt::Key_Period:
            play_speed_adjust(true);
            break;

        default:
            qDebug("key:(%d) pressed, not handled!\n", key);
            break;
    }
}

/**
 * @brief 创建 VideoStateData，打开媒体文件
 *        这是 open 文件最耗时的一步（特别是网络流）
 *
 * @param file 媒体文件路径
 * @return true=成功，state 可用
 */
bool MainWindow::create_video_state(const QString& file)
{
    // ★ 读菜单里用户选的是否硬解、是否循环
    // ★ 关键：硬解默认开启（4K 软解极慢，DXVA2 失败时 VideoStateData 会自动回退软解）
    //   UI 菜单可以手动取消勾选关闭硬解
    bool use_hardware = ui->actionHardware_decode->isChecked() || true;  // 强制默认开
    bool loop = ui->actionLoop_Play->isChecked();
    assert(!m_pVideoState);
    if (!m_pVideoState)
    {
        m_pVideoState = std::make_unique<VideoStateData>(use_hardware, loop);
        auto ret = m_pVideoState->create_video_state(file.toStdString().c_str());
        // ★ 打印流的元信息（debug 用）
        m_pVideoState->print_state();
        if (ret < 0)
        {
            delete_video_state();
            qWarning("---------- VideoState data create failed.");
            return false;
        }

        return true;
    }
    return false;
}

/**
 * @brief 释放 VideoStateData（触发所有线程的退出）
 */
void MainWindow::delete_video_state()
{
    //reset() = "把智能指针里现在的对象丢掉，换一个新的（或者清空）" 。
    //旧的对象会被自动 delete
    //无参数则是清空
    //有参数则是换一个新的
    m_pVideoState.reset();
}

/**
 * @brief 创建 ReadThread（解复用线程）
 *        state 后面会通过 set_video_state() 注入
 *
 * @return true=成功
 */
bool MainWindow::create_read_thread()
{
    assert(!m_pPacketReadThread);
    if (!m_pPacketReadThread)
    {
        // ★ state 先传 nullptr，后面再注入
        m_pPacketReadThread = std::make_unique<ReadThread>(this, nullptr);
        // ★ 线程退出时自动清空指针 + 走 stop_play 流程
        connect(m_pPacketReadThread.get(), &ReadThread::finished, this, &MainWindow::read_packet_stopped);
        return true;
    }
    return false;
}

/**
 * @brief 创建视频解码线程 + 初始化对应的 Decoder
 *        Decoder 负责 packet 队列和解码回调的绑定
 *
 * @return true=成功
 */
bool MainWindow::create_decode_video_thread()
{
    assert(!m_pDecodeVideoThread);
    if (!m_pDecodeVideoThread && m_pVideoState)
    {
        if (auto pState = m_pVideoState->get_state())
        {
            m_pDecodeVideoThread = std::make_unique<VideoDecodeThread>(this, pState);
            //当视频解码线程跑完时，自动调用&MainWindow::decode_video_stopped
            connect(m_pDecodeVideoThread.get(), &VideoDecodeThread::finished, this, &MainWindow::decode_video_stopped);

            auto avctx = m_pVideoState->get_contex(AVMEDIA_TYPE_VIDEO);

            // ★ 把队列和继续读信号绑到 decoder
            int ret = decoder_init(&pState->viddec, avctx, &pState->videoq, pState->continue_read_thread);
            if (ret < 0)
            {
                qWarning("decode video thread decoder_init failed.");
                return false;
            }

            // ★ 启动 decoder 线程
            ret = decoder_start(&pState->viddec, m_pDecodeVideoThread.get(), "video_decoder_thread");
            if (ret < 0)
            {
                qWarning("decode video thread decoder_start failed.");
                return false;
            }

            // ★ 告诉读线程：视频解码器需要 attachment 数据
            pState->queue_attachments_req = 1;
            return true;
        }
    }
    return false;
}

/**
 * @brief 创建音频解码线程 + 初始化 Decoder
 *
 * @return true=成功
 */
bool MainWindow::create_decode_audio_thread()
{
    assert(!m_pDecodeAudioThread);
    if (!m_pDecodeAudioThread && m_pVideoState)
    {
        if (auto pState = m_pVideoState->get_state())
        {
            m_pDecodeAudioThread = std::make_unique<AudioDecodeThread>(this, pState);

            connect(m_pDecodeAudioThread.get(), &AudioDecodeThread::finished, this, &MainWindow::decode_audio_stopped);

            auto avctx = m_pVideoState->get_contex(AVMEDIA_TYPE_AUDIO);
            int ret = decoder_init(&pState->auddec, avctx, &pState->audioq, pState->continue_read_thread);
            if (ret < 0)
            {
                qWarning("decode audio thread decoder_init failed.");
                return false;
            }

            ret = decoder_start(&pState->auddec, m_pDecodeAudioThread.get(), "audio_decoder_thread");
            if (ret < 0)
            {
                qWarning("decode audio thread decoder_init failed.");
                return false;
            }

            return true;
        }
    }
    return false;
}

/**
 * @brief 创建字幕解码线程 + 初始化 Decoder
 *
 * @return true=成功
 */
bool MainWindow::create_decode_subtitle_thread() // decode subtitle thread
{
    assert(!m_pDecodeSubtitleThread);
    if (!m_pDecodeSubtitleThread && m_pVideoState)
    {
        if (auto pState = m_pVideoState->get_state())
        {
            m_pDecodeSubtitleThread = std::make_unique<SubtitleDecodeThread>(this, pState);

            connect(m_pDecodeSubtitleThread.get(), &SubtitleDecodeThread::finished, this, &MainWindow::decode_subtitle_stopped);

            auto avctx = m_pVideoState->get_contex(AVMEDIA_TYPE_SUBTITLE);
            int ret = decoder_init(&pState->subdec, avctx, &pState->subtitleq, pState->continue_read_thread);
            if (ret < 0)
            {
                qWarning("decode subtitle thread decoder_init failed.");
                return false;
            }

            ret = decoder_start(&pState->subdec, m_pDecodeSubtitleThread.get(), "subtitle_decoder_thread");
            if (ret < 0)
            {
                qWarning("decode subtitle thread decoder_init failed.");
                return false;
            }

            return true;
        }
    }
    return false;
}

/**
 * @brief 创建视频播放线程（消费视频帧队列）
 *        这里会做 sws_scale 参数初始化（YUV -> RGB）
 *        绑定 frame_ready/subtitle_ready 信号到主窗口
 *
 * @return true=成功
 */
bool MainWindow::create_video_play_thread() // video play thread
{
    assert(!m_pVideoPlayThread);
    bool ret = false;
    if (!m_pVideoPlayThread && m_pVideoState)
    {
        if (auto pState = m_pVideoState->get_state())
        {
            m_pVideoPlayThread = std::make_unique<VideoPlayThread>(this, pState);

            // ★ 三种信号都接好：线程退出 / 帧 ready / 字幕 ready
            connect(m_pVideoPlayThread.get(), &VideoPlayThread::finished, this, &MainWindow::video_play_stopped);
            connect(m_pVideoPlayThread.get(), &VideoPlayThread::frame_ready, this, &MainWindow::image_ready);
            connect(m_pVideoPlayThread.get(), &VideoPlayThread::subtitle_ready, this, &MainWindow::subtitle_ready);
            // ★ 关键修复：视频自然播放完后由 video_play_thread 通知，自动安全停止
            //   避免老 bug：视频播完后 video_play_thread 永远空转，UI 点 stop 时 use-after-free → abort
            connect(m_pVideoPlayThread.get(), &VideoPlayThread::playback_finished, this, &MainWindow::on_playback_finished);
            connect(this, &MainWindow::stop_video_play_thread, m_pVideoPlayThread.get(), &VideoPlayThread::stop_thread);

            auto pVideo = m_pVideoState->get_contex(AVMEDIA_TYPE_VIDEO);
            bool bHardware = m_pVideoState->is_hardware_decode();
            // ★ 初始化色彩空间转换参数（YUV→RGB）
            ret = m_pVideoPlayThread->init_resample_param(pVideo, bHardware);
            if (!ret)
            {
                qWarning("init_resample_param failed.");
            }
        }
    }
    return ret;
}

/**
 * @brief 创建音频播放线程（消费音频帧队列 -> QAudioSink）
 *        音频设备初始化放在 StartPlayThread 里异步做（耗时）
 *
 * @return true=成功
 */
bool MainWindow::create_audio_play_thread()
{
    assert(!m_pAudioPlayThread);
    if (!m_pAudioPlayThread && m_pVideoState)
    {
        if (auto pState = m_pVideoState->get_state())
        {
            m_pAudioPlayThread = std::make_unique<AudioPlayThread>(this, pState);

            // ★ 三个信号：线程退出、停止命令、播放时间更新、可视化数据
            connect(m_pAudioPlayThread.get(), &AudioPlayThread::finished, this, &MainWindow::audio_play_stopped);
            connect(this, &MainWindow::stop_audio_play_thread, m_pAudioPlayThread.get(), &AudioPlayThread::stop_thread);
            connect(m_pAudioPlayThread.get(), &AudioPlayThread::update_play_time, this, &MainWindow::update_play_time);
            connect(m_pAudioPlayThread.get(), &AudioPlayThread::data_visual_ready, this, &MainWindow::audio_data);

            auto pAudio = m_pVideoState->get_contex(AVMEDIA_TYPE_AUDIO);
            print_decodeContext(pAudio, false);

#if 0 // this part is time-consuming, already using thread instead
			ret = m_pAudioPlayThread->init_device(pAudio->sample_rate, pAudio->channels); //pAudio->sample_fmt
			if (!ret) {
				qWarning("audio play init_device failed.");
				return false;
			}

			ret = m_pAudioPlayThread->init_resample_param(pAudio);
			if (!ret) {
				qWarning("audio play init resample param failed.");
				return false;
			}
#endif
            return true;
        }
    }
    return false;
}

/**
 * @brief 调整主窗口到视频原始分辨率大小
 *        如果新尺寸超过屏幕会按比例缩小
 */
void MainWindow::play_window_size()
{
    if (!m_pVideoState)
        return;

    if (auto pVideo = m_pVideoState->get_contex(AVMEDIA_TYPE_VIDEO))
    {
        print_decodeContext(pVideo);
        auto size_center = centralWidget()->size();
        // ★ 公式：原窗口大小 + 视频尺寸 - 中央控件大小 = 新窗口大小
        auto n_size = size() + display_video_size(pVideo) - size_center;
        // ★ 防止超出屏幕
        adjust_window_size(n_size);

        resize_window(n_size); // Adjust window size

        auto sz = minimumSize();
        // ★ 如果新窗口小于最小尺寸就强制按宽高比调整
        if (n_size.width() < sz.width() || n_size.height() < sz.height())
        {
            keep_aspect_ratio();
        }
    }
}

/**
 * @brief 等比例调整 size 到不超出屏幕范围
 *
 * @param size 输入输出参数：被调整到不超出屏幕的尺寸
 */
void MainWindow::adjust_window_size(QSize& size)
{
    auto rect = screen_rect();

    // ★ 宽度超了就按比例缩
    if (size.width() > rect.width())
    {
        auto w = size.width();
        size.setWidth(rect.width());
        size.setHeight(size.width() * size.height() / w);
    }

    // ★ 高度超了也按比例缩
    if (size.height() > rect.height())
    {
        auto h = size.height();
        size.setHeight(rect.height());
        size.setWidth(size.height() * size.width() / h);
    }
}

/**
 * @brief 启动 StartPlayThread（后台初始化音频设备）
 *        初始化完成后会发 audio_device_init 信号触发 play_started
 *
 * @return true=成功
 */
bool MainWindow::start_play_thread()
{
    m_pBeforePlayThread.reset();

    m_pBeforePlayThread = std::make_unique<StartPlayThread>(this);
    // connect(m_pBeforePlayThread.get(), &StartPlayThread::finished, m_pBeforePlayThread.get(), &QObject::deleteLater);
    connect(m_pBeforePlayThread.get(), &StartPlayThread::audio_device_init, this, &MainWindow::play_started);
    m_pBeforePlayThread->start();
    qDebug("++++++++++ start play thread(audio device initial) started.");
    return true;
}

/**
 * @brief 视频帧 ready 槽（来自 VideoPlayThread）
 *
 * 1. 拷贝一份 QImage（避免后台线程修改时主线程使用）
 * 2. 叠加字幕文字（黑底白字）
 * 3. 走一遍 OpenCV 滤镜（如果菜单里选了）
 * 4. 贴到 VideoLabel 上
 */
void MainWindow::image_ready(const QImage& img)
{
    // ★ 拷贝一份 QImage（避免后台线程继续修改时主线程用着出问题）
    QImage image = img.copy();

    if (!m_subtitle.isEmpty())
    {
        int height = 90;
        // QPen pen = QPen(Qt::white);
        QFont font = QFont("Times", 15, QFont::Bold);
        // ★ 字幕显示在画面底部
        QRect rt(0, image.height() - height, image.width(), height);

        // ★ 先画一层黑色阴影，再画白色文字（描边效果，让字幕在任何背景上都能看清）
        draw_img_text(image, m_subtitle, rt, QPen(Qt::black), font); // black shadow
        rt.adjust(-1, -1, -1, -1);
        draw_img_text(image, m_subtitle, rt, QPen(Qt::white), font);
    }

    // QElapsedTimer timer;
    // timer.start();
    // ★ 走一遍 OpenCV 滤镜（如果菜单里选了）—— ★ 暂时禁用：OpenCV 编译问题
    // image_cv(image); // cv handling
    // qDebug("------------image_cv---------------------%d milliseconds",
    // timer.elapsed());

    // ★ 把处理完的画面贴到 VideoLabel
    update_image(image);
}

/**
 * @brief 几何变换：灰度/镜像/旋转等简单效果
 *
 * @param image 输入输出参数：被修改的 QImage
 */
void MainWindow::image_cv_geo(QImage& image)
{
    // ★ 暂时禁用：依赖已移除的 OpenCV 头文件
    (void)image;
}

/**
 * @brief 主流程：把 QImage 转 Mat -> 走选中的 OpenCV 滤镜 -> Mat 转回 QImage
 *        image_cv_geo 还会再做一次几何变换（灰度/镜像/旋转）
 *
 * @param image 输入输出参数：被修改的 QImage
 */
void MainWindow::image_cv(QImage& image)
{
    // ★ 暂时禁用：依赖已移除的 OpenCV 头文件
    (void)image;
#if 0
    cv::Mat matImg;
    qimage_to_mat(image, matImg);

    if (ui->actionTest_CV->isChecked())
    {
        // mat_to_qimage(grey_img(matImg), image); //23 millsecs
        // mat_to_qimage(rotate_img(matImg), image);	//10
        // mat_to_qimage(repeat_img(matImg, 3, 3), image); //48
        // mat_to_qimage(histgram_img(matImg), image);	//20
        // mat_to_qimage(equalized_hist_img(matImg), image);	//30
        // mat_to_qimage(threshold_img(matImg), image);	//30
        // mat_to_qimage(thresholdAdaptive_img(matImg), image);	//78
        // mat_to_qimage(reverse_img(matImg), image);	//6

        /*
        int divideWith = 20;
        uchar table[256];
        gen_color_table(table, sizeof(table), divideWith);
        Mat table_mat = cv::Mat(1, 256, CV_8UC1, table);
        Mat res = scane_img_LUT(matImg, table_mat);
        mat_to_qimage(res, image);	//10
        */

        // mat_to_qimage(lighter_img(matImg, 1.2), image);	//20
        // mat_to_qimage(exposure_img(matImg), image);	//15
        // mat_to_qimage(gamma_img(matImg, 1.2), image);	//10
        // mat_to_qimage(contrast_bright_img(matImg, 1.2, 30), image);	//90
        // mat_to_qimage(canny_img(matImg), image);	//440

        // mat_to_qimage(blur_img(matImg), image);	//120
        // mat_to_qimage(sobel_img_XY(matImg), image);	//240
        // mat_to_qimage(laplacian_img(matImg), image);	//80
        // mat_to_qimage(scharr_img_XY(matImg), image);	//350
        // mat_to_qimage(prewitt_img_XY(matImg), image);	//270
    }

    // ★ 大量 if-else if：每个分支对应一个 CV 滤镜
    //  因为互斥（ActionGroup），所以只会有一个命中
    if (ui->actionRotate->isChecked())
    {
        mat_to_qimage(rotate_img(matImg), image);
    }
    else if (ui->actionRepeat->isChecked())
    {
        mat_to_qimage(repeat_img(matImg, 3, 3), image);
    }
    else if (ui->actionEqualizeHist->isChecked())
    {
        // ★ 直方图均衡化
        equalized_hist_img(matImg);
        mat_to_qimage(matImg, image);
    }
    else if (ui->actionThreshold->isChecked())
    {
        mat_to_qimage(threshold_img(matImg), image);
    }
    else if (ui->actionThreshold_Adaptive->isChecked())
    {
        mat_to_qimage(thresholdAdaptive_img(matImg), image);
    }
    else if (ui->actionReverse->isChecked())
    {
        // ★ 反相（颜色翻转）
        reverse_img(matImg);
        mat_to_qimage(matImg, image);
    }
    else if (ui->actionColorReduce->isChecked())
    {
        // ★ 颜色降阶：用 LUT 把 256 级颜色降到 256/20=12.8 级
        int divideWith = 20;
        uchar table[256];
        gen_color_table(table, sizeof(table), divideWith);
        Mat table_mat = cv::Mat(1, 256, CV_8UC1, table);
        scane_img_LUT(matImg, table_mat);
        mat_to_qimage(matImg, image); // 10
    }
    else if (ui->actionGamma->isChecked())
    {
        gamma_img(matImg, 1.2f);
        mat_to_qimage(matImg, image);
    }
    else if (ui->actionContrastBright->isChecked())
    {
        contrast_bright_img(matImg, 1.2, 30);
        mat_to_qimage(matImg, image);
    }
    else if (ui->actionCanny->isChecked())
    {
        mat_to_qimage(canny_img(matImg), image);

        /*const char* haar = "E:\\projects\\c++\\myProject\\vc\\video_player\\VideoPlayer_CMake\\src\\opencv\\build\\data\\haarcascades\\haarcascade_frontalface_alt.xml";
        face_detect(matImg, haar);
        mat_to_qimage(matImg, image);*/
    }
    else if (ui->actionBlur->isChecked())
    {
        mat_to_qimage(blur_img(matImg), image);
    }
    else if (ui->actionSobel->isChecked())
    {
        mat_to_qimage(sobel_img_XY(matImg), image);
    }
    else if (ui->actionLaplacian->isChecked())
    {
        mat_to_qimage(laplacian_img(matImg), image);
    }
    else if (ui->actionScharr->isChecked())
    {
        mat_to_qimage(scharr_img_XY(matImg), image);
    }
    else if (ui->actionPrewitt->isChecked())
    {
        mat_to_qimage(prewitt_img_XY(matImg), image);
    }

    // ★ 再做一遍几何变换（独立于上面的 CV 滤镜）
    image_cv_geo(image);
#endif  // ★ 暂时禁用：依赖已移除的 OpenCV 头文件
}

/**
 * @brief 字幕 ready 槽（来自 VideoPlayThread）
 *
 * @param text 当前字幕文字
 */
void MainWindow::subtitle_ready(const QString& text)
{
    set_subtitle(text);
}

/**
 * @brief 缓存当前字幕文本，image_ready 时会把它画到画面上
 *
 * @param str 字幕文本
 */
void MainWindow::set_subtitle(const QString& str)
{
    m_subtitle = str;
    qDebug() << "subtitle received:" << m_subtitle;
}

/**
 * @brief 清空当前字幕
 */
void MainWindow::clear_subtitle_str()
{
    set_subtitle("");
}

/**
 * @brief 把 QImage 转成 QPixmap 并贴到 VideoLabel
 *
 * @param img 要显示的图像
 */
void MainWindow::update_image(const QImage& img)
{
    auto pLabel = get_video_label();
    // ★ 空图就不贴（避免覆盖默认背景）
    if (!img.isNull() && pLabel)
        pLabel->setPixmap(QPixmap::fromImage(img));
}

/**
 * @brief 各种线程退出后的清理槽
 *        每个 *_stopped 都会 reset 对应的 unique_ptr
 *        视频/音频播放线程退出时还会恢复默认背景、隐藏音频可视化窗口
 */
void MainWindow::read_packet_stopped()
{
    // ★ 防重入：stop_play 中可能已 reset 过
    if (!m_pPacketReadThread)
        return;
    m_pPacketReadThread.reset();
    qDebug("************* Read  packets thread stopped.");

    // ★ 读线程退出意味着播放已结束
    stop_play();
}

/**
 * @brief 视频解码线程退出清理
 */
void MainWindow::decode_video_stopped()
{
    // ★ 防重入
    if (!m_pDecodeVideoThread)
        return;
    m_pDecodeVideoThread.reset();
    // ★ 线程变了，菜单可用状态也要刷新
    update_menus();
    qDebug("************* Video decode thread stopped.");
}

/**
 * @brief 音频解码线程退出清理
 */
void MainWindow::decode_audio_stopped()
{
    if (!m_pDecodeAudioThread)
        return;
    m_pDecodeAudioThread.reset();
    update_menus();
    qDebug("************* Audio decode thread stopped.");
}

/**
 * @brief 字幕解码线程退出清理
 */
void MainWindow::decode_subtitle_stopped()
{
    if (!m_pDecodeSubtitleThread)
        return;
    m_pDecodeSubtitleThread.reset();
    update_menus();
    qDebug("************* Subtitle decode thread stopped.");
}

/**
 * @brief 音频播放线程退出清理
 */
void MainWindow::audio_play_stopped()
{
    // ★ 防重入：stop_play 中已 reset
    if (!m_pAudioPlayThread)
        return;
    m_pAudioPlayThread.reset();
    qDebug("************* Audio play thread stopped.");

    // ★ 恢复默认背景
    set_default_bkground();

    // ★ 关闭音频可视化窗口
    show_audio_effect(false);
    update_menus();
}

/**
 * @brief 视频播放线程退出清理
 */
void MainWindow::video_play_stopped()
{
    // ★ 防重入：stop_play 中已 reset
    if (!m_pVideoPlayThread)
        return;
    m_pVideoPlayThread.reset();
    qDebug("************* Video play thread stopped.");

    // ★ 恢复默认背景
    set_default_bkground();
    update_menus();
}

/**
 * @brief 视频自然播放完毕（read_thread 读到 EOF，video_play_thread 队列空）
 *
 * 这是关键修复：之前 video_play_thread 不会在 EOF 时退出，
 * 永远在 video_refresh 里 10ms 空转，UI 后续点 stop 时会出现
 * video_play_thread 访问已 free 的 VideoState → abort() 崩溃。
 *
 * 现在：video_play_thread 主动 emit playback_finished，主线程
 * 自动调 stop_play 走正常释放流程。
 */
void MainWindow::on_playback_finished()
{
    qInfo("[on_playback_finished] video reached EOF, auto-stopping");
    // ★ 调现有的 stop_play，里面包了 try/catch，保证异常不传播
    stop_play();
}

/**
 * @brief 在状态栏显示一条消息
 *
 * @param message 要显示的消息
 */
void MainWindow::displayStatusMessage(const QString& message)
{
    int timeout = 0; // 5000: 5 second timeout
    if (auto bar = statusBar())
        bar->showMessage(message, timeout);
}

/**
 * @brief 打印解码器上下文信息（调试用）
 *        bVideo=true 打印视频相关字段，否则打印音频
 *
 * @param pDecodeCtx 解码器上下文
 * @param bVideo     true=视频；false=音频
 */
void MainWindow::print_decodeContext(const AVCodecContext* pDecodeCtx, bool bVideo) const
{
    if (!pDecodeCtx)
        return;

    if (bVideo)
    {
        qInfo("video codec_name: %s", pDecodeCtx->codec->name);
        qInfo("codec_type: %d, codec_id: %d, codec_tag: %d", pDecodeCtx->codec_type, pDecodeCtx->codec_id, pDecodeCtx->codec_tag);
        qInfo("width: %d, height: %d, codec_tag: %d", pDecodeCtx->width, pDecodeCtx->height);
    }
    else
    {
        qInfo("audio codec_name: %s", pDecodeCtx->codec->name);
        qInfo("codec_type: %d, codec_id: %d, codec_tag: %d", pDecodeCtx->codec_type, pDecodeCtx->codec_id, pDecodeCtx->codec_tag);
        qInfo("sample_rate: %d, channels: %d, sample_fmt: %d", pDecodeCtx->sample_rate, pDecodeCtx->ch_layout.nb_channels, pDecodeCtx->sample_fmt);
        qInfo("frame_size: %d, frame_number: %d, block_align: %d", pDecodeCtx->frame_size, pDecodeCtx->frame_num, pDecodeCtx->block_align);
    }
}

/**
 * @brief 把用户设置写入 QSettings
 *        - 隐藏播放控件、全屏、硬解、循环播放、皮肤
 *        - 软件信息（用于 About 对话框）
 */
void MainWindow::save_settings()
{
    auto res = ui->actionHide_Play_Ctronl->isChecked();
    m_settings.set_general("hidePlayContrl", int(res));
    res = ui->actionFullscreen->isChecked();
    m_settings.set_general("fullScreen", int(res));

    res = ui->actionHardware_decode->isChecked();
    m_settings.set_general("openDXVA2", int(res));
    res = ui->actionLoop_Play->isChecked();
    m_settings.set_general("loopPlay", int(res));

    m_settings.set_general("style", get_selected_style());

    m_settings.set_info("software", "Video player");
    m_settings.set_info("version", PLAYER_VERSION);
    m_settings.set_info("author", "lichong");
}

/**
 * @brief 从 QSettings 读取上次的设置，应用到 UI
 */
void MainWindow::read_settings()
{
    int value;
    auto values = m_settings.get_general("hidePlayContrl");
    if (values.isValid())
    {
        value = values.toInt();
        ui->actionHide_Play_Ctronl->setChecked(!!value);
        // ★ 同步应用：菜单勾上 + 控件立刻隐藏
        hide_play_control(value);
    }

    values = m_settings.get_general("fullScreen");
    if (values.isValid())
    {
        value = values.toInt();
        ui->actionFullscreen->setChecked(!!value);
        show_fullscreen(value);
    }

    values = m_settings.get_general("openDXVA2");
    if (values.isValid())
    {
        value = values.toInt();
        ui->actionHardware_decode->setChecked(!!value);
    }

    values = m_settings.get_general("loopPlay");
    if (values.isValid())
    {
        value = values.toInt();
        ui->actionLoop_Play->setChecked(!!value);
    }

    values = m_settings.get_general("style");
    if (values.isValid())
    {
        auto style = values.toString();
        // ★ 把对应菜单项勾上
        set_style_action(style);

        // ★ 判断是系统风格还是自定义风格，分别走两条路
        if (m_skin.get_style().contains(style))
        {
            m_skin.set_system_style(style);
        }
        else
        {
            m_skin.set_custom_style(style);
        }
    }
}

/**
 * @brief 读写音量设置（getter/setter 合体）
 *        set=true：写入；set=false：读取（默认 0.2）
 *
 * @param set true=写入；false=读取
 * @param vol 写入时的音量值（0~1）
 * @return float 读取时的音量值（0~1）
 */
float MainWindow::volume_settings(bool set, float vol)
{
    if (set)
    {
        // ★ 保留 1 位小数存储
        m_settings.set_general("volume", QString::number(float(vol), 'f', 1));
    }
    else
    {
        auto value = 0.2f; // default sound volume
        auto values = m_settings.get_general("volume");
        if (values.isValid())
            value = values.toFloat();
        return value;
    }
    return 0;
}

/**
 * @brief 找到菜单里当前被选中的风格名
 *
 * @return QString 选中的风格名（没选就是空串）
 */
QString MainWindow::get_selected_style() const
{
    auto pMenu = ui->menuStyle;
    for (auto action : pMenu->actions())
    {
        // ★ 跳过分隔符和子菜单
        if (!(action->isSeparator() || action->menu()))
        {
            qDebug("action: %s", qUtf8Printable(action->text()));
            if (action->isChecked())
                return action->data().toString();
        }
    }
    return QString("");
}

/**
 * @brief 按风格名勾上对应菜单项
 *
 * @param style 风格名
 */
void MainWindow::set_style_action(const QString& style)
{
    auto pMenu = ui->menuStyle;
    for (auto action : pMenu->actions())
    {
        if (!(action->isSeparator() || action->menu()))
        {
            if (action->data().toString() == style)
                action->setChecked(true);
        }
    }
}

/**
 * @brief 收到音频采样数据 -> 喂给音频可视化窗口
 *
 * @param data 音频采样数据
 */
void MainWindow::audio_data(const AudioData& data)
{
    if (m_audio_effect_wnd)
        m_audio_effect_wnd->paint_data(data);
}

/**
 * @brief 启动/停止音频可视化数据推送
 *
 * @param bSend true=开始推；false=停止推
 */
void MainWindow::start_send_data(bool bSend)
{
    if (m_pAudioPlayThread)
        m_pAudioPlayThread->send_visual_open(bSend);
}

/**
 * @brief 根据当前是否在播放更新所有菜单的可用状态
 */
void MainWindow::update_menus()
{
    qDebug() << "update_menus: " << is_playing();
    enable_menus(is_playing());
    enable_v_menus(playing_has_video());
    enable_a_menus(playing_has_audio());
}

/**
 * @brief 启用/禁用"通用"菜单（停止、媒体信息）
 *
 * @param enable true=启用
 */
void MainWindow::enable_menus(bool enable)
{
    ui->actionStop->setEnabled(enable);
    ui->actionMedia_Info->setEnabled(enable);
}

/**
 * @brief 启用/禁用"视频相关"菜单（宽高比、原始尺寸、硬解、CV 滤镜）
 *
 * @param enable true=启用，有视频流则启用
 */
void MainWindow::enable_v_menus(bool enable)
{
    ui->actionAspect_Ratio->setEnabled(enable);
    ui->actionOriginalSize->setEnabled(enable);
    ui->actionHardware_decode->setEnabled(enable);

    // ★ CV 滤镜菜单全部启用/禁用
    for (auto& pAction : ui->menuCV->actions())
    {
        if (pAction)
            pAction->setEnabled(enable);
    }
}

/**
 * @brief 启用/禁用"音频可视化"菜单
 *
 * @param enable true=启用
 */
void MainWindow::enable_a_menus(bool enable)
{
    ui->menuAudio_visualize->setEnabled(enable);
}

/**
 * @brief 读上次保存的 youtube 解析选项 id
 *
 * @return int 选项 id（默认 0）
 */
int MainWindow::get_youtube_optionid() const
{
    int option = 0;
    auto values = m_settings.get_general("youtube_option");
    if (values.isValid())
        option = values.toInt();
    return option;
}

/**
 * @brief 写入 youtube 解析选项 id
 *
 * @param id 选项 id
 */
void MainWindow::set_youtube_optionid(int id)
{
    m_settings.set_general("youtube_option", id);
}

/**
 * @brief 创建音频可视化的两个 ActionGroup（图形类型 + 数据类型）
 */
void MainWindow::create_avisual_action_group()
{
    // ★ 图形类型：线/柱/饼（互斥）
    m_AVisualGrapicTypeActsGroup = std::make_unique<QActionGroup>(this);
    m_AVisualGrapicTypeActsGroup->addAction(ui->actionLine);
    m_AVisualGrapicTypeActsGroup->addAction(ui->actionBar);
    m_AVisualGrapicTypeActsGroup->addAction(ui->actionPie);

    // ★ 数据类型：采样/频域（互斥）
    m_AVisualTypeActsGroup = std::make_unique<QActionGroup>(this);
    m_AVisualTypeActsGroup->addAction(ui->actionSampling);
    m_AVisualTypeActsGroup->addAction(ui->actionFrequency);
}

/**
 * @brief 从菜单状态读出当前的可视化格式
 *
 * @param fmt 输出参数：被填充的 VisualFormat
 * @return true=成功
 */
bool MainWindow::get_avisual_format(BarHelper::VisualFormat& fmt) const
{
    if (ui->actionLine->isChecked())
    {
        fmt.gType = BarHelper::e_GtLine;
    }
    else if (ui->actionBar->isChecked())
    {
        fmt.gType = BarHelper::e_GtBar;
    }
    else if (ui->actionPie->isChecked())
    {
        fmt.gType = BarHelper::e_GtPie;
    }

    if (ui->actionSampling->isChecked())
    {
        fmt.vType = BarHelper::e_VtSampleing;
    }
    else if (ui->actionFrequency->isChecked())
    {
        fmt.vType = BarHelper::e_VtFrequency;
    }

    return true;
}

/**
 * @brief 启动 YoutubeUrlThread 解析 yt URL
 *        解析完成后通过信号回调 start_to_play 或 start_yt_play
 *
 * @param data YoutubeUrlDlg 拿到的 URL 和选项
 * @return true=成功
 */
bool MainWindow::start_youtube_url_thread(const YoutubeUrlDlg::YoutubeUrlData& data)
{
    m_pYoutubeUrlThread.reset();
    m_pYoutubeUrlThread = std::make_unique<YoutubeUrlThread>(data, this);
    connect(m_pYoutubeUrlThread.get(), &YoutubeUrlThread::resultReady, this, &MainWindow::start_to_play);
    connect(m_pYoutubeUrlThread.get(), &YoutubeUrlThread::resultYtReady, this, &MainWindow::start_yt_play);
    connect(m_pYoutubeUrlThread.get(), &YoutubeUrlThread::resultFailed, this, &MainWindow::play_failed);
    m_pYoutubeUrlThread->start();
    qDebug("++++++++++ youtube url parsing thread started.");
    return true;
}

/**
 * @brief 创建播放列表窗口
 */
void MainWindow::create_playlist_wnd()
{
    m_playListWnd = std::make_unique<PlayListWnd>(this);
    // ★ 列表里双击文件 -> 开始播放
    connect(m_playListWnd.get(), &PlayListWnd::play_file, this, &MainWindow::start_to_play);
    // connect(m_playListWnd.get(), &PlayListWnd::save_playlist_signal, this, &MainWindow::save_playlist);
    // ★ 列表窗口隐藏时同步菜单勾选状态
    connect(m_playListWnd.get(), &PlayListWnd::hiden, this, &MainWindow::playlist_hiden);
    connect(m_playListWnd.get(), &PlayListWnd::playlist_file_saved, this, &MainWindow::playlist_file_saved);
}

/**
 * @brief 菜单 View -> PlayList 触发的槽
 */
void MainWindow::on_actionPlayList_triggered()
{
    show_playlist(ui->actionPlayList->isChecked());
}

/**
 * @brief 显示/隐藏播放列表窗口
 *
 * @param show true=显示
 */
void MainWindow::show_playlist(bool show)
{
    if (!m_playListWnd)
        return;

    if (show)
    {
        m_playListWnd->show();
        // ★ 高亮当前正在播放的文件
        m_playListWnd->set_cur_palyingfile();
    }
    else
    {
        m_playListWnd->hide();
    }
}

/**
 * @brief 播放列表窗口被隐藏时同步菜单
 */
void MainWindow::playlist_hiden()
{
    // ★ 取消勾选菜单，让 UI 状态和实际窗口状态一致
    ui->actionPlayList->setChecked(false);
}

/**
 * @brief 把文件加到播放列表
 *
 * @param file 文件路径
 */
void MainWindow::add_to_playlist(const QString& file)
{
    if (m_playListWnd)
        m_playListWnd->add_file(file);
}

/**
 * @brief 取得当前正在播放的文件路径
 *
 * @return QString 文件路径（没在播放就返回空）
 */
QString MainWindow::get_playingfile() const
{
    if (is_playing())
        return m_videoFile;
    return QString("");
}

/**
 * @brief 菜单 File -> Open Network URL 槽
 *        弹 NetworkUrlDlg 拿 URL，丢给 start_to_play
 */
void MainWindow::on_actionOpenNetworkUrl_triggered()
{
    NetworkUrlDlg dialog(this);

    if (dialog.exec() == QDialog::Accepted)
    {
        if (auto url = dialog.get_url(); !url.isEmpty())
        {
            start_to_play(url);
        }
        else
        {
            show_msg_dlg("Please input a valid youtube url. ");
        }
    }
}

/**
 * @brief 隐藏/恢复光标
 *
 * @param bHide true=隐藏；false=恢复
 */
void MainWindow::hide_cursor(bool bHide)
{
    if (bHide)
    {
        // ★ BlankCursor = 透明光标
        QApplication::setOverrideCursor(Qt::BlankCursor);
    }
    else
    {
        // ★ restoreOverrideCursor 配对使用
        QGuiApplication::restoreOverrideCursor();
    }
}

/**
 * @brief 判断当前鼠标位置是否在某个窗口内
 *
 * @param pWnd 目标窗口
 * @return bool true=在窗口内
 */
bool MainWindow::cursor_in_window(QWidget* pWnd)
{
    if (!pWnd)
        return false;

    auto rt = pWnd->rect();
    // ★ mapFromGlobal 把全局屏幕坐标转成窗口内坐标
    return rt.contains(pWnd->mapFromGlobal(QCursor::pos()));
}

/**
 * @brief 创建"已保存播放列表"菜单
 */
void MainWindow::create_savedPlaylists_menu()
{
    for (int i = 0; i < MaxPlaylist; ++i)
    {
        m_savedPlaylists[i] = std::make_unique<QAction>(this);
        // ★ 默认隐藏，按需显示
        m_savedPlaylists[i]->setVisible(false);
        connect(m_savedPlaylists[i].get(), SIGNAL(triggered()), this, SLOT(open_playlist()));
    }

    // ★ "清空"项
    m_PlaylistsClear = std::make_unique<QAction>(this);
    m_PlaylistsClear->setText(QApplication::translate("MainWindow", "清空", nullptr));
    connect(m_PlaylistsClear.get(), SIGNAL(triggered()), this, SLOT(clear_savedPlaylists()));

    auto pMenu = ui->menuSavedPlaylist;
    for (int i = 0; i < MaxPlaylist; ++i)
        pMenu->addAction(m_savedPlaylists[i].get());
    pMenu->addSeparator();
    pMenu->addAction(m_PlaylistsClear.get());

    update_savedPlaylists_actions();
}

/**
 * @brief 从已保存播放列表里移除一项
 *
 * @param fileName 列表文件名
 */
void MainWindow::remove_playlist_file(const QString& fileName)
{
    auto files = m_settings.get_savedplaylists().toStringList();
    files.removeAll(fileName);
    m_settings.set_savedplaylists(files);

    update_savedPlaylists_actions();
}

/**
 * @brief 用 QSettings 里的列表刷新"已保存播放列表"菜单
 */
void MainWindow::update_savedPlaylists_actions()
{
    auto files = m_settings.get_savedplaylists().toStringList();

    int num = qMin(files.size(), (int)MaxPlaylist);

    // ★ 一个都没有就 disable 整个菜单
    ui->menuSavedPlaylist->setEnabled(num > 0);

    for (int i = 0; i < num; ++i)
    {
        QString text = tr("%1 %2").arg(i + 1).arg(stripped_name(files[i]));
        m_savedPlaylists[i]->setText(QApplication::translate("MainWindow", text.toStdString().c_str(), nullptr));
        m_savedPlaylists[i]->setData(files[i]);
        m_savedPlaylists[i]->setVisible(true);
    }

    for (int j = num; j < MaxPlaylist; ++j)
        m_savedPlaylists[j]->setVisible(false);
}

/**
 * @brief 清空所有已保存的播放列表
 *        同时删除磁盘上的文件
 */
void MainWindow::clear_savedPlaylists()
{
    auto files = m_settings.get_savedplaylists().toStringList();

    // ★ 删文件
    for (const auto& i : files)
    {
        QFile file(i);
        file.remove();
    }

    files.clear();
    m_settings.set_savedplaylists(files);

    update_savedPlaylists_actions();
}

/**
 * @brief 打开一个已保存的播放列表
 */
void MainWindow::open_playlist()
{
    if (!m_playListWnd)
        return;

    // ★ 拿到是哪个菜单项被点了
    auto action = qobject_cast<QAction*>(sender());
    if (!action)
        return;

    auto file = action->data().toString();

    QStringList files;
    if (read_playlist(file, files))
    {
        // ★ 成功：把列表塞进窗口 + 显示窗口 + 勾选菜单
        m_playListWnd->update_files(files);
        show_playlist();
        ui->actionPlayList->setChecked(true);
    }
    else
    {
        // ★ 读失败：把这一项从保存列表里移除（可能文件被删了）
        remove_playlist_file(file);
    }
}

/**
 * @brief 用户保存了一个播放列表文件
 *        把文件加到"已保存列表"菜单
 *
 * @param file 新保存的列表文件路径
 */
void MainWindow::playlist_file_saved(const QString& file)
{
    auto files = m_settings.get_savedplaylists().toStringList();
    // ★ 移到最前
    files.removeAll(file);
    files.prepend(file);

    if (files.size() > MaxPlaylist)
    {
        // ★ 超过上限就提示用户
        show_msg_dlg(QString("You can only save %1 playlist files!").arg(MaxPlaylist));
    }

    // ★ 砍掉超出部分
    while (files.size() > MaxPlaylist)
        files.removeLast();

    m_settings.set_savedplaylists(files);
    update_savedPlaylists_actions();
}

/**
 * @brief 从磁盘读一个播放列表文件
 *
 * @param playlist_file 列表文件路径
 * @param files         输出参数：读出的文件列表
 * @return bool true=成功
 */
bool MainWindow::read_playlist(const QString& playlist_file, QStringList& files) const
{
    QFile file(playlist_file);
    if (file.open(QIODevice::ReadOnly))
    {
        QTextStream stream(&file);
        // ★ 列表文件按 UTF-8 编码读
        stream.setEncoding(QStringConverter::Utf8);

        // ★ 用 PLAYLIST_SEPERATE_CHAR 分割
        files = stream.readAll().split(PLAYLIST_SEPERATE_CHAR);
        // ★ 清掉末尾的空行
        files.removeAll(QString(""));
        file.close();
        return true;
    }
    return false;
}

/**
 * @brief 清空 yt 流缓存
 */
void MainWindow::clear_yt_list()
{
    m_playYtList.clear();
}

/**
 * @brief 从 yt 缓存里移除一项
 *
 * @param url 要移除的 URL
 */
void MainWindow::remove_yt_list(const QString& url)
{
    if (m_playYtList.find(url) != m_playYtList.end())
        m_playYtList.erase(url);
}

/**
 * @brief 往 yt 缓存里插一项
 *
 * @param url     流 URL
 * @param st_data 流的额外信息
 */
void MainWindow::insert_yt_list(const QString& url, const YoutubeJsonParser::YtStreamData& st_data)
{
    m_playYtList[url] = st_data;
}

/**
 * @brief 从 yt 缓存里查一项
 *
 * @param url     要查的 URL
 * @param st_data 输出参数：查到的流数据
 * @return bool true=找到了
 */
bool MainWindow::find_yt_list(const QString& url, YoutubeJsonParser::YtStreamData& st_data)
{
    if (auto it = m_playYtList.find(url); it != m_playYtList.end())
    {
        st_data = it->second;
        return true;
    }
    return false;
}
