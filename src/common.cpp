// ***********************************************************/
// common.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// 通用 Qt 工具函数
//   - appendPath: 安全地拼接两个路径
//   - toNativePath: 把 Qt 风格的 "/" 路径转成系统原生（Windows 下是 "\\"）

#include "common.h"

/**
 * @brief 拼接路径并清理（去掉多余的分隔符、处理 ".." 之类）
 *
 * 例：appendPath("a/b", "/c") -> "a/b/c"
 * 例：appendPath("a/", "b")  -> "a/b"（不会变 "a//b"）
 *
 * ★ 为什么要先用 separator 拼再用 cleanPath？
 *   cleanPath 能识别分隔符并去掉重复段，
 *   跨平台时 separator 会自动变 Windows 的 "\\" 或 Linux 的 "/"。
 *
 * @param path     父路径
 * @param sub_path 子路径
 * @return 清理后的完整路径
 */
QString appendPath(const QString& path, const QString& sub_path)
{
    return QDir::cleanPath(path + QDir::separator() + sub_path);
}

/**
 * @brief 把 Qt 通用分隔符 "/" 转换成系统原生
 * Windows 下变 "\\"，Linux 下保持 "/"
 *
 * 例："/tmp/test" -> Windows: "\\tmp\\test", Linux: "/tmp/test"
 *
 * ★ 什么时候需要这个？
 *   Qt 内部 API 用 "/"，但调系统 API（CreateFile 等）需要 "\\"，
 *   所以跨 Qt / 系统 API 时一定要做这个转换。
 *
 * @param path Qt 风格路径
 * @return 系统原生路径
 */
QString toNativePath(const QString& path)
{
    return QDir::toNativeSeparators(path);
}
