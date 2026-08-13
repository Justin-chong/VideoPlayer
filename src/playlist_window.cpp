// ***********************************************************/
// playlist_window.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 播放列表窗口：
//   - 显示所有待播放的文件（标题 / 时长 / 路径 三列）
//   - 双击某一行 -> 通知 MainWindow 播放该文件
//   - 支持拖入文件 / 右键菜单（删除、清空、保存为 .pl）
//   - 支持保存/加载 .pl 播放列表
//
// 内部数据：std::map<QString, PlayListLine> m_dataItems
//   key 是完整路径，value 是 {fileName, duration, file} 三元组
// ***********************************************************/

#include "playlist_window.h"
#include "mainwindow.h"
#include "packets_sync.h"
#include "play_control_window.h"
#include "common.h"

/**
 * @brief 构造函数：初始化 UI + 设置窗口属性 + 初始化表格
 *
 * 几个关键的设计决策：
 *   1. 窗口关闭时只 hide() 不销毁（closeEvent 中 event->ignore）
 *      —— 避免每次都重新 new，提升响应速度
 *   2. setAcceptDrops(true) + dragEnter/dropEvent
 *      —— 支持用户从资源管理器拖文件进来
 *   3. WindowStaysOnTopHint
 *      —— 让播放列表常驻在主窗口之上
 *   4. 表格右键菜单用 Qt::CustomContextMenu 策略
 *      —— 不弹 Qt 内置菜单，完全自定义
 *
 * @param parent 父窗口（MainWindow）
 */
PlayListWnd::PlayListWnd(QWidget* parent) : QWidget(parent), ui(std::make_unique<Ui::PlayList>())
{
    // ★ 必须先 setupUi，否则 ui->xxx 都是 nullptr
    ui->setupUi(this);
    // ★ 把 .ui 里的 gridLayout 设为 widget 的主布局
    setLayout(ui->gridLayout);

    auto flags = windowFlags();
    // ★ 把 widget 提升为"独立窗口"（不加 Qt::Window 它会被嵌进父窗口）
    flags |= Qt::Window;
    // ★ 始终置顶
    flags |= Qt::WindowStaysOnTopHint;
    // ★ 去掉标题栏右上角的"?"帮助按钮
    flags &= (~Qt::WindowContextHelpButtonHint);

    // ★ 一次性应用所有窗口标志
    setWindowFlags(flags);
    // ★ 非模态：不阻塞主窗口（用户可以一边看主窗口一边操作列表）
    setWindowModality(Qt::NonModal);

    // ★ 开启"接受拖放"，否则 dragEnter/dropEvent 根本不会触发
    setAcceptDrops(true);

    // ★ 预创建右键菜单（Delete / Clear / Save）
    create_temp_menu();
    // ★ 初始化表格（3 列、单选、双击播放等）
    init_list();
    // ★ 数据集合清空
    clear_data_files();
    // ★ 表格刷新一次（此时是空的，刷新出表头）
    update_table_list();
}

/**
 * @brief 初始化 QTableWidget 的外观和行为
 *
 * 关键配置：
 *   - 3 列：Title / Duration / Path
 *   - 高度固定 35，关闭竖直表头
 *   - 网格线关掉，编辑关掉
 *   - 单选整行（点击整行高亮，比"点单元格"更符合列表习惯）
 *   - 双击 cell -> cellSelected -> emit play_file
 *   - 右键 -> displayMenu
 */
void PlayListWnd::init_list()
{
    auto pTable = get_table();

    QStringList headerLabels;
    // headerLabels << "#" << "Title" << "Duration" << "Path";
    // ★ 表头：三列（如果以后需要"序号"列，把第一行注释打开即可）
    headerLabels << "Title"
                 << "Duration"
                 << "Path";
    // ★ 设置列数
    pTable->setColumnCount(headerLabels.size());
    // ★ 设置表头文字
    pTable->setHorizontalHeaderLabels(headerLabels);

    auto header = pTable->horizontalHeader();
    // ★ 表头行高固定 35，更易点
    header->setFixedHeight(35);
    // header->setSectionResizeMode(QHeaderView::Stretch);
    // header->setSectionResizeMode(1, QHeaderView::ResizeToContents); //column
    // duration.
    // ★ 最后一列（Path）自动拉伸占满剩余空间
    header->setStretchLastSection(true);

    // ★ 不显示行号（左侧 1,2,3 那列）
    pTable->verticalHeader()->setVisible(false);
    // ★ 关闭网格线（看起来更干净）
    pTable->setShowGrid(false);
    // pTable->setStyleSheet("QTableView {selection-background-color: red;}");
    // ★ 不允许双击编辑（避免用户改坏文件路径）
    pTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // ★ "选中整行" 而不是"选中单元格"，更符合列表操作习惯
    pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // ★ 一次只能选一行
    pTable->setSelectionMode(QAbstractItemView::SingleSelection);

    // ★ 双击某行 -> 触发 cellSelected（最终会 emit play_file）
    connect(pTable, SIGNAL(cellDoubleClicked(int, int)), this, SLOT(cellSelected(int, int)));

    // ★ 开启自定义右键菜单策略
    pTable->setContextMenuPolicy(Qt::CustomContextMenu);
    // ★ 右键点击 -> displayMenu（弹出 Delete/Clear/Save）
    connect(pTable, SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(displayMenu(const QPoint&)));
}

/**
 * @brief 把一个文件加进数据集合（不立即刷 UI）
 *
 * 不同来源的文件处理不同：
 *   - 本地文件：用 avformat 探测时长
 *   - 网络/YouTube：先尝试拿 YouTube 标题和时长
 *
 * @param file 完整文件路径（本地或网络 URL）
 * @return bool 始终 true（保留 bool 返回值是为了将来扩展）
 */
bool PlayListWnd::add_data_file(const QString& file)
{
    PlayListLine data;
    data.file = file;
    if (is_local(data.file))
    {
        // ★ 本地文件：取文件基名（去后缀）
        data.fileName = get_file_name(data.file);
        // ★ 用 avformat 探测时长（packets_sync.h 的 get_file_info）
        data.duration = get_file_duration(data.file);
    }
    else
    {
        // ★ 网络文件：先填占位符
        data.fileName = "Unknow"; // not handled
        data.duration = "--:--";

        // ★ 尝试让父窗口（MainWindow）去查 YouTube 链接的信息
        if (auto parent = (MainWindow*)parentWidget())
        {
            YoutubeJsonParser::YtStreamData st_data;
            if (parent->find_yt_list(data.file, st_data))
            {
                // ★ 拿到 YouTube 信息：填入标题和真实时长
                data.fileName = st_data.title;
                data.duration = get_file_duration(data.file);
            }
        }
    }

    // ★ 用 map::insert 自动去重，相同 key 不会覆盖
    m_dataItems.insert({file, data});
    return true;
}

/**
 * @brief 判断文件是否已经在播放列表里（避免重复添加）
 *
 * @param file 完整文件路径
 * @return bool true=已存在，false=不在
 */
inline bool PlayListWnd::already_in(const QString& file) const
{
    // ★ std::map::find 找不到时返回 end()，这是惯用判存在法
    return m_dataItems.find(file) != m_dataItems.end();
}

/**
 * @brief 从数据集合里删除一个文件
 *
 * @param file 完整文件路径
 * @return void
 */
void PlayListWnd::del_data_file(const QString& file)
{
    // ★ 先判存在，避免 erase 一个不存在的 key 抛异常
    if (already_in(file))
        m_dataItems.erase(file);
}

/**
 * @brief 按"行号"取出文件路径
 *
 * 注意：id 是表格行号，std::map 是按 key 排序的有序容器，
 * 所以可以"前进 N 步"到第 N 个元素。
 *
 * @param id 行号（0-based）
 * @return QString 该行的文件路径
 */
inline QString PlayListWnd::get_data_file(int id) const
{
    // ★ begin() 拿到第一个元素的迭代器
    auto it = m_dataItems.begin();
    // ★ std::advance 把迭代器前进 id 步
    std::advance(it, id);
    // ★ it->first 是 map 的 key（文件路径）
    return it->first;
}

/**
 * @brief 清空数据集合（不立即刷 UI，UI 由 update_table_list 负责）
 */
void PlayListWnd::clear_data_files()
{
    m_dataItems.clear();
}

/**
 * @brief 把一行数据追加到表格末尾
 *
 * @param data 一行的所有信息（fileName / duration / file）
 * @return void
 */
void PlayListWnd::add_table_line(const PlayListLine& data)
{
    const auto pTable = get_table();
    // ★ 当前行数
    const int count = pTable->rowCount();
    // ★ 追加一行（先 setRowCount + 1）
    pTable->setRowCount(count + 1);

    int col = 0;
    // ★ 第 0 列：文件名
    pTable->setItem(count, col++, new QTableWidgetItem(data.fileName));
    // ★ 第 1 列：时长字符串
    pTable->setItem(count, col++, new QTableWidgetItem(data.duration));

    // ★ 把路径转成系统原生分隔符（Windows 上 "\"），显示更友好
    auto file = toNativePath(data.file);
    // ★ 第 2 列：完整路径
    pTable->setItem(count, col++, new QTableWidgetItem(file));
    // ★ 行高固定 16，让每行看起来更紧凑
    pTable->setRowHeight(count, 16);
}

/**
 * @brief 删掉表格里所有行（保留表头）
 */
void PlayListWnd::clear_table_list()
{
    const auto pTable = get_table();
    // ★ 循环 removeRow(0) 直到行数 == 0
    while (pTable->rowCount() > 0)
        pTable->removeRow(0);
}

/**
 * @brief 用 m_dataItems 重新生成整个表格
 *
 * 流程：
 *   1. clear_table_list 清空旧行
 *   2. for-each 遍历 m_dataItems，每行 add_table_line
 *   3. set_cur_palyingfile 高亮当前正在播放的文件
 *
 * 调用时机：add/del/clear 任何会改变数据集合的动作之后
 */
void PlayListWnd::update_table_list()
{
    // ★ 先清空
    clear_table_list();

    // ★ 再用数据集合的当前内容重画
    for (auto const& i : m_dataItems)
        add_table_line(i.second);

    // ★ 把"当前正在播放"那一行高亮起来
    set_cur_palyingfile();
}

/**
 * @brief 双击某行：发射 play_file 信号通知 MainWindow
 *
 * @param row 表格行号
 * @param col 表格列号（本函数没用，但 Qt 槽签名需要）
 */
void PlayListWnd::cellSelected(int row, int col)
{
    // ★ 把行号转回文件路径
    auto file = get_data_file(row);
    qDebug() << "file clicked:"
             << "row:" << row << "col:" << col << "file:" << file;
    // ★ 发信号，MainWindow 收到后调用 play() 真正开始播放
    emit play_file(file);
}

/**
 * @brief 取当前选中行的文件路径
 *
 * @return QString 当前选中行的文件路径；如果没选则返回 map 第一个（潜在 BUG 但目前没问题）
 */
QString PlayListWnd::get_cursel_file() const
{
    const auto pTable = get_table();
    // ★ currentRow() 在没选中时返回 -1，get_data_file(-1) 行为未定义；
    //   不过实际调用前都先判了 rowCount > 0，所以一般安全
    return get_data_file(pTable->currentRow());
}

/**
 * @brief 取指定行的文件路径
 *
 * @param row 表格行号
 * @return QString 该行的文件路径
 */
QString PlayListWnd::get_row_file(int row) const
{
    return get_data_file(row);
}

/**
 * @brief 批量添加文件（拖入多个文件时用）
 *
 * @param files 多个文件路径
 * @return void
 */
void PlayListWnd::add_files(const QStringList& files)
{
    // ★ 逐个调用 add_file，复用单文件逻辑（含"非媒体拒绝"等判断）
    for (int i = 0; i < files.size(); i++)
        add_file(files[i]);
}

/**
 * @brief 添加单个文件
 *
 * 三步校验：
 *   1. 空 -> 警告并返回
 *   2. 非媒体文件 -> 警告并返回
 *   3. 通过 -> add_data_file + update_table_list
 *
 * @param file 完整文件路径
 * @return void
 */
void PlayListWnd::add_file(const QString& file)
{
    if (file.isEmpty())
    {
        // ★ 空字符串直接拒绝
        qWarning() << "File is empty!\n";
        return;
    }

    if (!is_media(file))
    {
        // ★ 非媒体文件警告并返回（拖入 .txt 等不应该加进列表）
        qWarning() << "This is not media file, file:" << file;
        return;
    }

    // ★ 通过校验：写入数据集合
    add_data_file(file);
    // ★ 刷新 UI
    update_table_list();
}

/**
 * @brief 窗口关闭事件：只隐藏，不销毁
 *
 * 重要：
 *   - hide() + event->ignore() 配合，Qt 就不会真销毁 widget
 *   - 同时发 hiden 信号让 MainWindow 知道（拼写是 hiden 而非 hidden，沿用项目原拼写）
 *   - 这样下次"显示播放列表"时直接 show() 即可，秒开
 *
 * @param event 关闭事件
 * @return void
 */
void PlayListWnd::closeEvent(QCloseEvent* event)
{
    // ★ 只隐藏
    hide();
    // ★ ignore 告诉 Qt 不要继续默认的关闭流程
    event->ignore();
    // ★ 通知主窗口
    emit hiden();
}

/**
 * @brief 处理拖入文件放下事件
 *
 * Qt 的拖放机制：dragEnter 决定光标样式，dropEvent 才真正接收数据
 *
 * @param event 拖放事件
 * @return void
 */
void PlayListWnd::dropEvent(QDropEvent* event)
{
    // ★ QMimeData 是 Qt 拖放/剪贴板统一的数据载体
    const auto mimeData = event->mimeData();

    // ★ 没有 URL 就不处理（用户拖的可能不是文件）
    if (!mimeData->hasUrls())
        return;

    // ★ 取出所有 URL
    auto urlList = mimeData->urls();

    QStringList files;
    // ★ 每个 URL 转成本地路径（toLocalFile 还会去掉 "file:///" 前缀）
    for (int i = 0; i < urlList.size(); i++)
        files.append(urlList.at(i).toLocalFile().trimmed());

    // ★ 批量添加
    add_files(files);
}

/**
 * @brief 取文件名（不含路径和后缀）
 *
 * 例如 "C:/a/b/c.mp4" -> "c"
 *
 * @param path 完整路径
 * @return QString 文件基名
 */
QString PlayListWnd::get_file_name(const QString& path)
{
    // ★ QFileInfo 用来拆解路径很方便
    QFileInfo fileInfo(path);
    // ★ baseName = 不含后缀的文件名
    return fileInfo.baseName();
}

/**
 * @brief 用 avformat 探测文件时长并格式化成 "mm:ss"
 *
 * @param file 文件路径
 * @return QString 形如 "03:21" 的时长字符串
 */
QString PlayListWnd::get_file_duration(const QString& file) const
{
    int64_t duration = 0;
    // ★ get_file_info 在 packets_sync.h，封装了 avformat_open_input 等
    get_file_info(file.toStdString().c_str(), duration);
    // ★ 调下面那个 int64_t 重载去格式化
    return get_file_duration(duration);
}

/**
 * @brief 把 avformat 返回的"微秒时长"格式化为 "mm:ss" 字符串
 *
 * @param duration avformat 返回的时长（单位是微秒 AV_TIME_BASE）
 * @return QString "mm:ss" 字符串
 */
QString PlayListWnd::get_file_duration(int64_t duration) const
{
    int64_t hours = 0, mins = 0, secs = 0, us = 0;
    // ★ 公共时间拆分函数
    get_duration_time(duration, hours, mins, secs, us);

    // ★ 复用 PlayControlWnd 的格式化（保持项目内时间字符串风格一致）
    return PlayControlWnd::get_play_time(hours, mins, secs);
}

/**
 * @brief 拖入开始时显示"可放下"光标
 *
 * 如果没有这一步，用户拖文件进来时鼠标会显示"禁止"图标，体验很差。
 *
 * @param event 拖入事件
 * @return void
 */
void PlayListWnd::dragEnterEvent(QDragEnterEvent* event)
{
    const auto mimeData = event->mimeData();
    if (mimeData->hasUrls())
        // ★ acceptProposedAction 让 Qt 显示"可放下"光标
        event->acceptProposedAction();
    // ★ 兼容老版本 Qt 的 accept 写法
    event->accept();
}

/**
 * @brief 处理 Esc 键关闭窗口
 *
 * @param event 按键事件
 * @return void
 */
void PlayListWnd::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
    {
        // ★ Esc = 关闭窗口（其实是 hide）
        hide();
        // ★ 标记事件已处理
        event->accept();
    }
    else
    {
        // ★ 其他按键交给 Qt 默认处理
        QWidget::keyPressEvent(event);
    }
}

/**
 * @brief 把"传入的文件"对应的行高亮
 *
 * 用于：
 *   - 加载 .pl 后同步当前播放高亮
 *   - 切换文件时同步
 *
 * @param file 要高亮的文件路径
 * @return void
 */
void PlayListWnd::set_sel_file(const QString& file)
{
    if (const auto pTable = get_table())
    {
        // ★ 默认选中第一行（兜底，文件不在列表里也至少有一行可选）
        pTable->selectRow(0); // default
        // ★ 遍历所有行，找到匹配的那行 selectRow
        for (int i = 0; i < pTable->rowCount(); i++)
        {
            if (get_row_file(i) == file)
                pTable->selectRow(i);
        }
    }
}

/**
 * @brief 把所有文件路径导出到 QStringList（用于保存 .pl）
 *
 * @param files [out] 输出的文件路径列表
 * @return void
 */
void PlayListWnd::get_files(QStringList& files) const
{
    // ★ 遍历 map 把所有 key（文件路径）追加出去
    for (auto const& i : m_dataItems)
        files.append(i.first);
}

/**
 * @brief 右键菜单 "Delete" 槽：删掉当前选中行
 */
void PlayListWnd::deleteBtn_clicked()
{
    // ★ if-with-init：if 里声明 + 初始化 + 判空，一行搞定（C++17）
    if (auto sel = get_cursel_file(); !sel.isEmpty())
    {
        // ★ 从数据集合删
        del_data_file(sel);
        // ★ 刷新 UI
        update_table_list();
    }
}

/**
 * @brief 右键菜单 "Clear" 槽：清空整个列表
 */
void PlayListWnd::clearBtn_clicked()
{
    // ★ 清空数据 + 刷新 UI 两步走
    clear_data_files();
    update_table_list();
}

/**
 * @brief 右键菜单 "Save" 槽：把当前列表保存为 .pl 文件
 *
 * .pl 文件格式非常简单：所有文件路径用 PLAYLIST_SEPERATE_CHAR
 * （这里定义为 "\n"）连接，最后再加一个换行。
 *
 * @return bool true=保存成功，false=用户取消或列表为空或打开文件失败
 */
bool PlayListWnd::saveBtn_clicked()
{
    QStringList files;
    // ★ 先收集所有文件路径
    get_files(files);
    if (files.size() <= 0)
    {
        // ★ 列表是空的，没必要弹保存对话框
        qWarning() << "Nothing in playlist!";
        return false;
    }

    // ★ 默认目录：当前工作目录
    auto dir = QDir::currentPath();
    // ★ 弹出"另存为"对话框，限定扩展名是 .pl
    auto fileName = QFileDialog::getSaveFileName(this, tr("Save Playlist File"), dir, tr("Playlist (*.pl)"));
    if (fileName.isEmpty())
        // ★ 用户点了取消
        return false;

    // ★ 取绝对路径（虽然后面没用到，但保留以便将来调试）
    dir = QDir(fileName).absolutePath();

    QFile file(fileName);
    if (file.open(QIODevice::WriteOnly))
    {
        // ★ 用 QTextStream 写文本，自动处理编码
        QTextStream stream(&file);
        // ★ 强制 UTF-8，避免中文路径在不同 OS 上乱码
        stream.setEncoding(QStringConverter::Utf8);
        // ★ 用分隔符合并所有路径 + 末尾换行
        stream << files.join(PLAYLIST_SEPERATE_CHAR) << Qt::endl;
        stream.flush();

        file.close();

        // ★ 发信号告诉 MainWindow：列表已保存
        emit playlist_file_saved(fileName);
        return true;
    }
    return false;
}

/**
 * @brief 用一组文件整体替换当前列表
 *
 * 典型用途：加载 .pl 文件时，把文件里读出的所有路径灌进来。
 *
 * @param files 一组文件路径
 * @return void
 */
void PlayListWnd::update_files(const QStringList& files)
{
    // ★ 先清空旧数据
    clear_data_files();
    // ★ 再批量加新的
    add_files(files);
}

/**
 * @brief 显示右键菜单
 *
 * 注意：Qt 的 QMenu::exec 需要"屏幕坐标"，
 * 所以要先 mapToGlobal 把表格的相对坐标转到屏幕坐标。
 *
 * @param pos 鼠标点击位置（表格坐标系）
 * @return void
 */
void PlayListWnd::displayMenu(const QPoint& pos)
{
    if (auto pTable = get_table())
    {
        // ★ 表格是空的，不弹菜单（菜单里点啥都没意义）
        if (pTable->rowCount() <= 0)
            return;

        if (m_tmpMenu)
            // ★ viewport()->mapToGlobal 把坐标转到全屏
            m_tmpMenu->exec(pTable->viewport()->mapToGlobal(pos));
    }
}

/**
 * @brief 预创建右键菜单（Delete / Clear / Save）
 *
 * 用 unique_ptr 持有 QMenu，避免内存泄漏。
 * 之所以"预创建"而不是"每次右键都 new"，是为了省分配 + 加快弹出速度。
 */
void PlayListWnd::create_temp_menu()
{
    // ★ unique_ptr 自动 delete，避免泄漏
    m_tmpMenu = std::make_unique<QMenu>(this);
    // ★ 3 个菜单项
    auto del_act = m_tmpMenu->addAction("Delete");
    auto clear_act = m_tmpMenu->addAction("Clear");
    auto save_act = m_tmpMenu->addAction("Save");

    // ★ 3 个 connect：菜单项 triggered -> 对应槽
    connect(del_act, &QAction::triggered, this, &PlayListWnd::deleteBtn_clicked);
    connect(clear_act, &QAction::triggered, this, &PlayListWnd::clearBtn_clicked);
    connect(save_act, &QAction::triggered, this, &PlayListWnd::saveBtn_clicked);
}

/**
 * @brief 把"当前正在播放"的文件高亮
 *
 * 实现：问 MainWindow 当前在播什么，再调用 set_sel_file。
 */
void PlayListWnd::set_cur_palyingfile()
{
    if (auto pParent = (MainWindow*)parent())
        // ★ 委托给 set_sel_file 去做高亮
        set_sel_file(pParent->get_playingfile());
}

/**
 * @brief 判断一个字符串是不是本地文件路径
 *
 * 用 QUrl 解析用户输入：
 *   - "C:/a/b.mp4" -> isLocalFile() == true
 *   - "http://..." -> isLocalFile() == false
 *
 * @param file 待判断的字符串
 * @return bool true=本地文件，false=网络 URL
 */
bool PlayListWnd::is_local(const QString& file)
{
    return QUrl::fromUserInput(file).isLocalFile();
}

/**
 * @brief 调试用：打印一个文件的所有 MIME 信息
 *
 * 实际项目里只在 is_media 里被调用，留着是为了方便以后
 * 排查"为什么这个文件被识别为非媒体"之类的问题。
 *
 * @param file 文件路径
 * @return QString MIME 类型字符串（例如 "video/mp4"）
 */
QString PlayListWnd::mimeType(const QString& file)
{
    auto mimeType = QMimeDatabase().mimeTypeForFile(file);
    qDebug() << file << ", MIME info:";
    qDebug() << "name:" << mimeType.name();
    qDebug() << "comment:" << mimeType.comment();
    qDebug() << "genericIconName:" << mimeType.genericIconName();
    qDebug() << "iconName:" << mimeType.iconName();
    qDebug() << "globPatterns:" << mimeType.globPatterns();
    qDebug() << "parentMimeTypes:" << mimeType.parentMimeTypes();
    qDebug() << "allAncestors:" << mimeType.allAncestors();
    qDebug() << "aliases:" << mimeType.aliases();
    qDebug() << "suffixes:" << mimeType.suffixes();
    qDebug() << "preferredSuffix:" << mimeType.preferredSuffix();
    qDebug() << "filterString:" << mimeType.filterString();

    return QMimeDatabase().mimeTypeForFile(file).name();
}

/**
 * @brief 判断一个文件是不是"媒体文件"
 *
 * 规则：
 *   1. 网络 URL 一律视为媒体（无法用本地 MIME 探测）
 *   2. 本地文件用 MIME 主类型判断：主类型是 "video" 或 "audio" 即认为是媒体
 *
 * @param file 文件路径（本地或网络 URL）
 * @return bool true=媒体文件，false=非媒体
 */
bool PlayListWnd::is_media(const QString& file) const
{
    if (!is_local(file)) // assume all network url are media files
        // ★ 网络 URL 不做 MIME 判断，全部放行（FFmpeg 会自己报错）
        return true;

    auto mimetype = mimeType(file);
    // ★ 按 "/" 拆出主类型：例如 "video/mp4" -> ["video", "mp4"]
    auto mimetypes = mimetype.split("/");
    // ★ 只看主类型：video/* 或 audio/* 都算媒体
    if (mimetypes[0] == "video" || mimetypes[0] == "audio")
        return true;

#if 0
    // 备选：只允许白名单里的几个 MIME（已弃用，留作参考）
    QStringList mimes = { "video/mp4","video/x-matroska","video/webm","audio/x-wav" };
    if (mimes.contains(mimetype, Qt::CaseInsensitive)) {
        return true;
    }
    else {
        qWarning() << "Not handled, MIME type:" << mimetype;
    }
#endif
    return false;
}
