/**
 * @file ffmpeg_init.h
 * @brief FFmpeg 初始化与日志工具
 *
 * 这个文件很小，但提供了一些"全局性"的功能：
 *   1. ffmpeg_init()：把 FFmpeg 的网络层（avio）和日志注册到 Qt 的日志系统
 *   2. print_ffmpeg_info()：打印 FFmpeg 版本和编译时启用的编解码器
 *   3. check_error()：把 FFmpeg 的错误码转成可读字符串
 *   4. dump_format / dump_metadata 等：把媒体信息格式化输出（用于"媒体信息"对话框）
 */

#pragma once

#include <QDebug>

// FFmpeg 是 C 库，必须用 extern "C" 包起来，否则 C++ 编译器会按 C++ 的
// 名字修饰规则找符号，导致链接失败
extern "C"
{
#include <libavutil/log.h>          // FFmpeg 日志
#include <libavutil/ffversion.h>     // FFmpeg 版本号
#include <libavutil/version.h>
#include <libavutil/avutil.h>        // 通用工具
#include <libavcodec/avcodec.h>      // 编解码
#include <libavformat/avformat.h>    // 容器格式
#include <libswscale/swscale.h>      // 视频像素转换
#include <libswresample/swresample.h>// 音频重采样
#include <libavutil/avstring.h>      // 字符串工具
#include <libavutil/opt.h>           // 选项解析
#include <libavutil/display.h>       // 显示矩阵（用于旋转视频）
}

/**
 * @brief 初始化 FFmpeg
 * 主要工作：
 *   1. 注册所有编解码器、格式、滤镜（新版 FFmpeg 不需要显式注册，但兼容老代码）
 *   2. 接管 FFmpeg 的日志回调，让 av_log 的输出能进 Qt 的 qDebug
 *   3. 初始化 avio 网络协议（用于播放网络流）
 * @return 0=成功
 */
int ffmpeg_init();

/**
 * @brief 打印 FFmpeg 版本信息
 * 程序启动时调用，让用户知道用的是哪个版本的 FFmpeg、启用了哪些编解码器。
 */
void print_ffmpeg_info();

/**
 * @brief 检查 FFmpeg 错误码
 * @param error av_xxx 函数的返回值
 * 把负数错误码转成可读字符串，方便调试。
 */
void check_error(int error);

/**
 * @brief 把 AVFormatContext 的流信息格式化为字符串
 * 类似 ffprobe 的输出，用于"媒体信息"对话框。
 */
QString dump_format(AVFormatContext* ic, int index, const char* url, int is_output = 0);
QString dump_metadata(const AVDictionary* m, const char* indent = "  ");  // 格式化元数据
QString dump_stream_format(const AVFormatContext* ic, int i, int index, int is_output);  // 格式化单个流
QString print_fps(double d, const char* postfix);                          // 把小数帧率格式化为分数（如 29.97 -> "30000/1001"）
QString dump_sidedata(const AVStream* st, const char* indent);             // 格式化流的附加数据

