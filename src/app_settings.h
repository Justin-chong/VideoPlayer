/**
 * @file app_settings.h
 * @brief 应用程序设置（基于 QSettings 封装）
 *
 * 用 QSettings 把播放器的设置保存到 VideoPlayer.ini 文件里。
 * 配置文件分成几个"组"（section）：
 *   - General:    通用设置（皮肤、循环播放、默认音量等）
 *   - Info:       一些状态信息
 *   - RecentFiles: 最近播放文件列表
 *   - SavedPlaylistFiles: 保存过的播放列表
 *
 * QSettings 默认存在 Windows 注册表里，这里强制用 INI 文件格式，便于调试。
 */

#pragma once

#include <QSettings>
#include <QDebug>
#include <QDir>
#include <QVariant>
#include <QMetaType>
#include <memory>
#include "common.h"

class AppSettings
{
public:
    /**
     * @brief 构造函数
     * @param file 配置文件名，默认 "VideoPlayer.ini"
     */
    explicit AppSettings(const QString& file = "VideoPlayer.ini");
    ~AppSettings(){};

public:
    // ===== 通用设置的读写 =====
    QVariant get_general(const QString& key) const;
    void set_general(const QString& key, const QVariant& value);

    // ===== Info 段 =====
    QVariant get_info(const QString& key) const;
    void set_info(const QString& key, const QVariant& value);

    // ===== 最近文件 =====
    QVariant get_recentfiles(const QString& key = "files") const;
    void set_recentfiles(const QVariant& value = {}, const QString& key = "files");

    // ===== 已保存的播放列表 =====
    QVariant get_savedplaylists(const QString& key = "files") const;
    void set_savedplaylists(const QVariant& value = {}, const QString& key = "files");

private:
    // 各个配置段的 ID
    enum SectionID
    {
        SECTION_ID_NONE = -1,
        SECTION_ID_GENERAL,
        SECTION_ID_INFO,
        SECTION_ID_RECENTFILES,
        SECTION_ID_SAVEDPLAYLISTFILES,
        SECTION_ID_MAX
    };

    // 段 ID 与段名（写入 INI 时用的字符串）的对应表
    typedef struct Section
    {
        SectionID id;
        const char* str;
    } Section;

private:
    void print_settings() const;  // 启动时把当前所有设置都打印到日志（调试用）
    void set_value(SectionID id, const QString& key, const QVariant& value);
    void set_value(const QString& group, const QString& key, const QVariant& value);
    QVariant get_value(const QString& group, const QString& key) const;
    QVariant get_value(SectionID id, const QString& key) const;
    // 把"段名/键名"拼成 QSettings 用的格式："段名/键名"
    inline static QString group_key(const QString& group, const QString& key);

private:
    std::unique_ptr<QSettings> m_pSettings;  // Qt 配置类
    static const Section m_sections[];       // 段名表
};
