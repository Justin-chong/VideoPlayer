/**
 * @file version.h
 * @brief 播放器版本号定义
 *
 * 【小白必读】这个文件用来定义软件的版本号。
 * 用了几个"宏的宏"的小技巧，主要是为了让编译器把数字拼成字符串。
 *
 * 实际使用：
 *   - PLAYER_VERSION  -> 一个字符串，例如 "2.2.3"，用于显示在"关于"对话框
 *   - PLAYER_VERSION_NUMBER -> 一个数字（实际是 2.2.3 这种 token），用于宏比较
 *
 * 修改版本号：只要改 VERSION_MAJOR / MINOR / MICRO 三个宏即可。
 */
#pragma once

// 主版本号：大改架构、API 不兼容时 +1
#define VERSION_MAJOR 2
// 次版本号：加新功能时 +1
#define VERSION_MINOR 2
// 修订号：修 bug 时 +1
#define VERSION_MICRO 3

// 用来把 a.b.c 拼成 a.b.c 这样的 token（不是字符串）
// 宏替换：VERSION_DOT(2,2,3) -> 2.2.3（注意：这是数字不是字符串）
#define VERSION_DOT(a, b, c) a.b.c

// 把三个版本号拼成 2.2.3 这样的 token
#define PLAYER_VERSION_NUMBER \
    VERSION_DOT(VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO)

// ===== 字符串化的小技巧 =====
// STR_HELPER(x) 把 x 变成字符串（先 #x 转字符串）
// STR(x) 是为了展开宏后再转字符串
//   否则 #PLAYER_VERSION_NUMBER 会变成 "PLAYER_VERSION_NUMBER" 而不是 "2.2.3"
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

// 最终给 UI 用的版本字符串，例如 "2.2.3"
#define PLAYER_VERSION STR(PLAYER_VERSION_NUMBER)
