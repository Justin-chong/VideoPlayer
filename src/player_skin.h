/**
 * @file player_skin.h
 * @brief 播放器皮肤（QSS 样式）管理
 *
 * Qt 皮肤其实就是 QSS（Qt Style Sheet，类似于网页的 CSS）。
 * 本类负责：
 *   1. 列出所有可用的内置 QSS
 *   2. 加载 "res/skin/" 目录下的用户自定义 QSS
 *   3. 切换皮肤（应用到整个应用程序）
 *
 * 皮肤文件来自 GTRONICK/QSS 这个 GitHub 项目。
 */

#pragma once
#include <QApplication>
#include <QStyleFactory>
#include <QStyle>
#include <QFile>
#include <QDir>

class PlayerSkin {
public:
	explicit PlayerSkin() {};
	~PlayerSkin() {};

public:
	// 列出所有可用的皮肤（系统样式 + 自定义 QSS）
	QStringList get_style() const;
	QStringList get_custom_styles() const;
	// 切换到系统内置样式（Fusion、Windows 等）
	void set_system_style(const QString& style);
	// 切换到自定义 QSS 皮肤（从文件加载）
	void set_custom_style(const QString& filename);

private:
	void clear_skin();  // 清理当前 QSS

private:
	// QSS 文件所在目录（相对路径）
	const static QString m_qss_path;
};