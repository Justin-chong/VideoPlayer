/**
 * @file common.h
 * @brief 项目里的通用工具函数
 *
 * 当前只有两个简单的工具函数。
 */

#pragma once

#include <QString>
#include <QDir>

/**
 * @brief 拼接路径
 * 自动处理分隔符（Windows 用 "\\"，Linux/Mac 用 "/"）
 * 例如 appendPath("/a/b", "c") -> "/a/b/c"
 */
QString appendPath(const QString& path, const QString& sub_path);

/**
 * @brief 把 Qt 风格的路径转成系统原生路径
 * 例如把 "/" 转成 "\\"（Windows 下）
 */
QString toNativePath(const QString& path);
