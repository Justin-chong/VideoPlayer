/**
 * @file playlist_window.h
 * @brief 播放列表窗口
 *
 * 菜单 Media -> PlayList 弹出的窗口。
 * 功能：
 *   - 显示当前播放列表（文件名 + 时长）
 *   - 双击某行播放该文件
 *   - 增删文件、清空列表
 *   - 保存为播放列表文件（*.m3u 类）
 *   - 支持拖入文件
 *
 * UI 在 playlist_window.ui 里定义，主体是一个 QTableWidget。
 */

#pragma once

#include <QDebug>
#include <QDialog>
#include <QDropEvent>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMimeDatabase>
#include <memory>
#include <set>
#include "ui_playlist_window.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
class PlayList;  // 由 playlist_window.ui 生成的 UI 类
}
QT_END_NAMESPACE

// 播放列表文件里的分隔符
#define PLAYLIST_SEPERATE_CHAR "\n"

class PlayListWnd : public QWidget
{
    Q_OBJECT

public:
    explicit PlayListWnd(QWidget* parent = Q_NULLPTR);
    ~PlayListWnd(){};

public:
    // 列表里的一行数据
    typedef struct
    {
        QString fileName;  // 文件名（不含路径）
        QString file;      // 完整路径
        QString duration;  // 时长字符串
    } PlayListLine;

public:
    // ===== 增删改查 =====
    void add_file(const QString& file);
    void add_files(const QStringList& files);
    void get_files(QStringList& files) const;
    void update_files(const QStringList& files);
    void set_cur_palyingfile();  // 高亮当前正在播放的文件

signals:
    void play_file(const QString& file);              // 双击某行时发出
    void save_playlist_signal(const QStringList& files);  // 保存播放列表
    void hiden();                                    // 窗口被隐藏
    void playlist_file_saved(const QString& file);   // 播放列表已保存

public slots:
    void cellSelected(int row, int col);       // 选中某行
    void deleteBtn_clicked();                  // 点击"删除"按钮
    void clearBtn_clicked();                   // 点击"清空"按钮
    bool saveBtn_clicked();                    // 点击"保存"按钮
    void displayMenu(const QPoint& pos);       // 右键菜单

protected:
    void closeEvent(QCloseEvent* event) override;
    void dropEvent(QDropEvent* event) override;          // 拖放文件
    void dragEnterEvent(QDragEnterEvent* event) override; // 拖入检测
    void keyPressEvent(QKeyEvent* event) override;        // 键盘事件

private:
    inline QTableWidget* get_table() const { return ui->tableWidget; }
    static QString get_file_name(const QString& path);
    inline bool already_in(const QString& file) const;
    QString get_cursel_file() const;
    QString get_row_file(int row) const;
    void init_list();
    void add_table_line(const PlayListLine& data);
    void update_table_list();
    void clear_table_list();
    bool add_data_file(const QString& file);
    void del_data_file(const QString& file);
    inline QString get_data_file(int id) const;
    void clear_data_files();
    void set_sel_file(const QString& file);
    QString get_file_duration(const QString& file) const;       // 拿文件时长（通过 FFmpeg）
    QString get_file_duration(int64_t duration) const;          // 把微秒转成 "HH:MM:SS" 字符串
    void create_temp_menu();
    static QString mimeType(const QString& filePath);
    static bool is_local(const QString& file);
    bool is_media(const QString& file) const;  // 判断是否是媒体文件

private:
    std::unique_ptr<Ui::PlayList> ui;
    std::unique_ptr<QMenu> m_tmpMenu;  // 右键菜单
    // 文件路径 -> 该行的数据（用 map 便于查重和按文件名查找）
    std::map<QString, PlayListLine> m_dataItems;
};
