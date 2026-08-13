// ***********************************************************/
// app_settings.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// app settings load and save
// ***********************************************************/

#include "app_settings.h"

// 整个 app 的"分区"映射表
//  - id  : 枚举值（业务侧用）
//  - str : 配置文件里的 [Section] 名字
// 这样业务侧用 enum 即可，不用记字符串
const AppSettings::Section AppSettings::m_sections[] = {
    {SECTION_ID_GENERAL, "General"},
    {SECTION_ID_INFO, "Info"},
    {SECTION_ID_RECENTFILES, "RecentFiles"},
    {SECTION_ID_SAVEDPLAYLISTFILES, "SavedPlaylistFiles"},
};

/**
 * @brief AppSettings 构造函数
 * 用 QSettings 包装传入的 .ini 文件路径，并打印所有当前配置（调试用）。
 *
 * ★ 为什么要用 unique_ptr<QSettings>？
 *   QSettings 内部需要稳定地址，unique_ptr 把它放在堆上，
 *   避免拷贝导致内部状态错乱。
 *
 * ★ 为什么要 print_settings()？
 *   调试时方便确认配置文件加载没加载到、值对不对。
 *
 * @param file 配置文件路径（通常是 settings.ini）
 */
AppSettings::AppSettings(const QString& file)
{
    // ★ IniFormat：人类可读、跨平台。普通 IniFormat 比 NativeFormat 兼容性更好
    m_pSettings = std::make_unique<QSettings>(file, QSettings::IniFormat);

    // 调试用：把配置内容全打出来
    print_settings();
}

/**
 * @brief 把当前 QSettings 里的所有内容打印到 qDebug
 *
 * ★ 为什么用 beginGroup / endGroup？
 *   QSettings 的 childKeys 只能在某个 group 内查，
 *   必须先进入 group 才能列内部 key。
 *
 * ★ 为什么打印两次 org/app？
 *   排查路径问题时方便看 QSettings 内部到底定位到哪个文件。
 */
void AppSettings::print_settings() const
{
    if (m_pSettings)
    {
        // ★ toNativePath 把 "/" 变成系统分隔符（Windows 下 "\")
        qDebug() << "videoplayer configure file:" << toNativePath(m_pSettings->fileName());
        qDebug() << "organizationName:" << m_pSettings->organizationName();
        qDebug() << "applicationName:" << m_pSettings->applicationName();

        // ★ 遍历所有分组（[] 括起来的）
        for (const auto& group : m_pSettings->childGroups())
        {
            // ★ 进入分组后才能读 key
            m_pSettings->beginGroup(group);
            qDebug() << "group:" << group;
            // ★ 遍历分组内所有 key
            for (const auto& key : m_pSettings->childKeys())
            {
                qDebug() << QString("key:%1, value:%2").arg(key).arg(m_pSettings->value(key).toString());
            }
            m_pSettings->endGroup();
        }
    }
}

/**
 * @brief 用枚举分区 ID 存值（业务侧首选）
 *
 * ★ 为什么要做这个薄封装？
 *   业务侧用 SECTION_ID_GENERAL 这种 enum 比直接写 "General" 字符串稳，
 *   改 ini 分区名只要改 m_sections 一处。
 *
 * @param id    分区 ID（enum）
 * @param key   分区内 key
 * @param value 要写入的值
 */
void AppSettings::set_value(SectionID id, const QString& key, const QVariant& value)
{
    // ★ 范围检查：超过 NONE 或等于 MAX 都是非法 enum
    if (id > SECTION_ID_NONE && id < SECTION_ID_MAX)
        set_value(QString(m_sections[id].str), key, value);
}

/**
 * @brief 用枚举分区 ID 取值
 *
 * @param id  分区 ID
 * @param key 分区内 key
 * @return 取到的值；id 非法或 key 不存在时返回默认 QVariant
 */
QVariant AppSettings::get_value(SectionID id, const QString& key) const
{
    if (id > SECTION_ID_NONE && id < SECTION_ID_MAX)
    {
        return get_value(QString(m_sections[id].str), key);
    }
    // ★ 非法 id 返回默认 QVariant，让上层用 .isValid() 判断
    return {};
}

/**
 * @brief 把 group 和 key 拼成 QSettings 路径格式 "group/key"
 *
 * ★ QSettings 内部用 "/" 分层，所以直接拼字符串就行，
 *   不需要考虑 Windows 路径分隔符。
 *
 * @param group 分区名
 * @param key   key 名
 * @return "group/key"
 */
inline QString AppSettings::group_key(const QString& group, const QString& key)
{
    return group + "/" + key;
}

/**
 * @brief 用字符串分区名存值（底层）
 *
 * @param group 分区字符串
 * @param key   key
 * @param value 要写的值
 */
void AppSettings::set_value(const QString& group, const QString& key, const QVariant& value)
{
    m_pSettings->setValue(group_key(group, key), value);
}

/**
 * @brief 用字符串分区名取值（底层）
 *
 * @param group 分区字符串
 * @param key   key
 * @return 取到的值；不存在时返回默认 QVariant
 */
QVariant AppSettings::get_value(const QString& group, const QString& key) const
{
    return m_pSettings->value(group_key(group, key));
}

/**
 * @brief 快捷方法：写到 General 分区
 *
 * @param key   key 名
 * @param value 值
 */
void AppSettings::set_general(const QString& key, const QVariant& value)
{
    set_value(SECTION_ID_GENERAL, key, value);
}

/**
 * @brief 快捷方法：读 General 分区
 *
 * @param key key 名
 * @return 值
 */
QVariant AppSettings::get_general(const QString& key) const
{
    return get_value(SECTION_ID_GENERAL, key);
}

/**
 * @brief 快捷方法：写到 Info 分区
 *
 * @param key   key 名
 * @param value 值
 */
void AppSettings::set_info(const QString& key, const QVariant& value)
{
    set_value(SECTION_ID_INFO, key, value);
}

/**
 * @brief 快捷方法：读 Info 分区
 *
 * @param key key 名
 * @return 值
 */
QVariant AppSettings::get_info(const QString& key) const
{
    return get_value(SECTION_ID_INFO, key);
}

/**
 * @brief 快捷方法：写到 RecentFiles 分区（最近打开文件列表）
 *
 * @param value 值（一般是文件路径列表）
 * @param key   key 名
 */
void AppSettings::set_recentfiles(const QVariant& value, const QString& key)
{
    set_value(SECTION_ID_RECENTFILES, key, value);
}

/**
 * @brief 快捷方法：读 RecentFiles 分区
 *
 * @param key key 名
 * @return 值
 */
QVariant AppSettings::get_recentfiles(const QString& key) const
{
    return get_value(SECTION_ID_RECENTFILES, key);
}

/**
 * @brief 快捷方法：读 SavedPlaylistFiles 分区（已保存的播放列表）
 *
 * @param key key 名
 * @return 值
 */
QVariant AppSettings::get_savedplaylists(const QString& key) const
{
    return get_value(SECTION_ID_SAVEDPLAYLISTFILES, key);
}

/**
 * @brief 快捷方法：写到 SavedPlaylistFiles 分区
 *
 * @param value 值
 * @param key   key 名
 */
void AppSettings::set_savedplaylists(const QVariant& value, const QString& key)
{
    set_value(SECTION_ID_SAVEDPLAYLISTFILES, key, value);
}
