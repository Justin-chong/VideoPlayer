// ***********************************************************/
// player_skin.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 播放器皮肤模块 - 负责切换"系统风格"和"自定义 QSS 皮肤"
// 系统风格：Qt 自带的 QStyle（Fusion、Windows、macOS 等）
// 自定义风格：res/qss/ 目录下的 .qss 文件
// ***********************************************************/

#include "player_skin.h"
#include "common.h"

// 自定义 QSS 文件夹路径
// ★ 写成 const static 字符串，所有实例共享一份，节省内存
// ★ 路径用相对路径（"./res/qss"），让程序可以"带目录整体拷走"运行，
//   不依赖安装时的绝对路径
const QString PlayerSkin::m_qss_path = "./res/qss";

/**
 * @brief 清除当前应用程序全局样式表
 *
 * 在切换风格之前必须先调用本函数：把 QApplication 的 setStyleSheet
 * 清空。否则上一次残留的 QSS 会和新的 QSS 叠加，导致风格混乱。
 *
 * @return void
 */
void PlayerSkin::clear_skin()
{
    // ★ 传空字符串 = 清掉整个应用程序的全局样式
    qApp->setStyleSheet("");
}

/**
 * @brief 枚举当前 Qt 内置支持的所有系统风格名
 *
 * Qt 把"系统主题"做成 QStyle 插件形式，运行时会扫描有哪些可用的风格。
 * 返回的 QStringList 形如 {"windowsvista", "Fusion", "macOS"}。
 * MainWindow 用这个列表来动态构建"系统主题"菜单的子菜单项。
 *
 * @return QStringList 所有可用 QStyle 的 key 列表
 */
QStringList PlayerSkin::get_style() const
{
    // ★ QStyleFactory::keys() 是 Qt 自带的"枚举所有内置 QStyle"的接口
    return QStyleFactory::keys();
}

/**
 * @brief 切换到 Qt 内置的某个系统风格（例如 "Fusion"）
 *
 * 切换顺序很重要：
 *   1. 先清掉 QSS（避免和系统风格叠加）
 *   2. 再 setStyle() 应用新的 QStyle
 *   3. 最后把当前 QStyle 的标准调色板 setPalette（不同风格下颜色看着才协调）
 *
 * 调用方：MainWindow::on_actionSystemStyle 槽函数。
 *
 * @param style Qt 风格名（必须是 QStyleFactory::keys() 里的一个）
 * @return void
 */
void PlayerSkin::set_system_style(const QString& style)
{
    // ★ 第一步：清掉自定义 QSS，避免和系统风格打架
    clear_skin();
    // ★ 第二步：用 QStyleFactory::create 创建并应用新的 QStyle
    qApp->setStyle(QStyleFactory::create(style));
    // ★ 第三步：把当前风格的"标准调色板"应用到 QApplication，
    //   这样窗口背景、文字颜色等都会和所选风格匹配
    qApp->setPalette(QApplication::style()->standardPalette());
}

/**
 * @brief 切换到某个自定义 QSS 皮肤（从 res/qss/ 目录读取）
 *
 * 步骤：
 *   1. 用 appendPath 拼出 .qss 文件的完整路径
 *   2. 用 QFileInfo::exists 判存在性
 *   3. 存在 -> clear_skin + setStyleSheet(readAll())
 *   4. 不存在 -> 静默返回（不抛错，方便用户加新皮肤时不用改代码）
 *
 * @param name QSS 文件名（不含后缀，例如 "dark" -> dark.qss）
 * @return void
 */
void PlayerSkin::set_custom_style(const QString& name)
{
    // ★ appendPath 来自 common.h，作用是拼接 "目录/文件名"
    auto file = appendPath(m_qss_path, name + ".qss");
    QFile qss(file);

    // ★ 用 QFileInfo::exists 而不是先 open 再判断，效率更高、语义更清晰
    if (QFileInfo::exists(file))
    {
        // ★ 先清掉系统风格或上一次 QSS，避免叠加
        clear_skin();
        // ★ ReadOnly 模式打开，只读不写
        // ★ C4834: [[nodiscard]] 必须检查返回值
        if (qss.open(QFile::ReadOnly))
        {
            // ★ readAll() 一次读完整个 QSS 文本，直接喂给 setStyleSheet
            qApp->setStyleSheet(qss.readAll());
        }
    }
}

/**
 * @brief 枚举 res/qss/ 目录下所有 .qss 自定义皮肤
 *
 * MainWindow 用这个返回值来动态构建"自定义主题"菜单项。
 * 列表里只包含文件名（含后缀），调用方通常需要再剥掉后缀。
 *
 * @return QStringList 例如 {"dark.qss", "light.qss"}
 */
QStringList PlayerSkin::get_custom_styles() const
{
    // ★ QDir 封装一个目录对象
    QDir directory(m_qss_path);
    // ★ entryList + 过滤器是 QDir 的经典用法：
    //   - 第一个参数 QStringList() << "*.qss" 表示"只匹配 .qss 文件"
    //   - 第二个参数 QDir::Files 表示"只看文件，不看子目录"
    return directory.entryList(QStringList() << "*.qss", QDir::Files);
}
