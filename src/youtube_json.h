/**
 * @file youtube_json.h
 * @brief YouTube JSON 数据解析
 *
 * youtube-dl / yt-dlp 可以把视频信息输出成 JSON 文件。
 * 本类负责解析这种 JSON，把里面的视频流信息提取出来。
 *
 * 一个 YouTube 视频通常有多个流（不同清晰度、视频/音频分离），
 * 用户可以选择"画质最好"、"最省流量"等。
 */

#pragma once

#include <QString>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

class YoutubeJsonParser
{
public:
    // 流的压缩类型
    typedef enum class CompressionType
    {
        Compressed,   // 普通流（音视频在一起）
        AudioOnly,    // 仅音频
        VideoOnly     // 仅视频（无音轨）
    } CompressionType;

    // 流的类型
    typedef enum class StreamType
    {
        Audio,
        Video
    } StreamType;

    // 单个流的信息
    typedef struct YtStream
    {
        int id{-1};
        bool progressive{false};      // 是否音视频合一的"渐进式"流
        StreamType type{StreamType::Audio};
        QString resolution;           // 视频分辨率（如 "1280x720"）
        QString abr;                  // 音频比特率（Average Bitrate）
        int filesize{0};
        QString url;                  // 实际播放地址
    } YtStream;

    // 整个视频的信息
    typedef struct YoutubeData
    {
        bool m_valid{false};
        QString title;       // 标题
        QString author;      // 作者
        uint captions_len{0};  // 字幕数量
        uint length{0};      // 时长（秒）
        std::vector<YtStream> streams;  // 所有可选流
    } YoutubeData;

    // 用户最终选择的流（含视频元信息）
    typedef struct YtStreamData
    {
        QString title;
        uint length{0};
        YtStream stream;
    } YtStreamData;

public:
    explicit YoutubeJsonParser(const QString& json_file);
    ~YoutubeJsonParser() { QFile::remove(m_jsonFile); };  // 析构时删除临时 JSON 文件

public:
    // ===== 拿各种"最佳/最差"的 URL =====
    QString get_yt_url(CompressionType type, bool order = true) const;  // 按类型拿
    QString get_best_url() const;        // 总体最佳
    QString get_worst_url() const;       // 总体最差
    QString get_bestvideo_url() const;   // 最佳视频
    QString get_worstvideo_url() const;  // 最差视频
    QString get_bestaudio_url() const;   // 最佳音频
    QString get_worstaudio_url() const;  // 最差音频

    // 拿所有指定类型的流
    void get_streams(std::vector<YtStream>& streams, CompressionType type) const;
    // 各种"最佳/最差"流的版本（返回详细信息）
    bool get_yt_stream(CompressionType type, YtStream& stream, bool order = true) const;
    bool get_best_stream(YtStream& st) const;
    bool get_worst_stream(YtStream& st) const;
    bool get_bestvideo_stream(YtStream& st) const;
    bool get_worstvideo_stream(YtStream& st) const;
    bool get_bestaudio_stream(YtStream& st) const;
    bool get_worstaudio_stream(YtStream& st) const;

private:
    // 真正解析 JSON 的函数
    void parse();
    QString read_file(const QString& file);

public:
    QString m_jsonFile;     // JSON 文件路径
    YoutubeData m_data;     // 解析后的数据
};
