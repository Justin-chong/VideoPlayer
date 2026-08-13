// ***********************************************************/
// youtube_url_thread.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// youtube url parsing thread
// ***********************************************************/

#include "youtube_json.h"

/**
 * @brief YoutubeJsonParser 构造函数
 * 立刻把 JSON 文件解析进 m_data（不延迟）
 *
 * @param json_file pytube 脚本写出来的 JSON 文件路径
 */
YoutubeJsonParser::YoutubeJsonParser(const QString& json_file) : m_jsonFile(json_file)
{
    // ★ 构造时就解析，外部直接 get_streams() 拿结果
    parse();
}

/**
 * @brief 解析 JSON 文件内容到 m_data
 *
 * 解析流程：
 *   1. 读整个 JSON 文件
 *   2. 用 Qt 的 QJsonDocument 解析为树
 *   3. 取顶层字段：title / author / length / captions_length
 *   4. 遍历 streams 数组，每条流保存 progressive/type/resolution/abr/url
 *   5. 标记 m_valid = true
 *
 * ★ 为什么自己写 JSON 解析？
 *   不引入第三方 JSON 库（pytube 输出格式固定），
 *   用 Qt 自带的 QJsonDocument 就够用了。
 */
void YoutubeJsonParser::parse()
{
    // 1. 把整个 JSON 文件读成字符串
    QString all = read_file(m_jsonFile);

    // 2. 用 Qt 的 JSON 解析器
    // ★ fromJson 返回 doc，内部存的是结构化的树
    QJsonDocument doc = QJsonDocument::fromJson(all.toUtf8());

    // 3. 解析顶层字段
    QJsonObject json = doc.object();
    m_data.title = json["title"].toString();              // 视频标题
    m_data.author = json["author"].toString();            // 作者
    m_data.captions_len = json["captions_length"].toInt();// 字幕长度
    m_data.length = json["length"].toInt();               // 视频时长（秒）

    // 4. 解析所有可用的流（每种分辨率/码率是一个流）
    auto v = json["streams"].toArray();
    for (const auto& st : v)
    {
        YtStream stream;
        auto st_obj = st.toObject();
        stream.id = st_obj["id"].toInt();

        // ★ progressive 表示这个流是"音视频合一"（不用分别下载再合流）
        stream.progressive = st_obj["progressive"].toBool();
        // ★ 默认先按 audio 处理，如果是视频类型再覆盖
        stream.type = StreamType::Audio;
        if (st_obj["type"].toString().compare("video") == 0)
        {
            stream.type = StreamType::Video;
            // ★ 视频流带分辨率字段（"720p" 这种）
            stream.resolution = st_obj["resolution"].toString();
        }
        else
        {
            // ★ 音频流带码率字段（"128kbps" 这种）
            stream.abr = st_obj["abr"].toString();
        }

        stream.filesize = st_obj["filesize"].toInt();  // 文件大小
        stream.url = st_obj["url"].toString();        // 直链 URL
        m_data.streams.push_back(stream);
    }

    // ★ 标记解析成功
    m_data.m_valid = true;
}

/**
 * @brief 把整个文件读成 QString
 *
 * ★ 为什么要用 QIODevice::Text？
 *   Text 标志会让 Qt 自动把 Windows 的 \r\n 转换成 \n，
 *   避免后续 JSON 解析时被多余 \r 卡住。
 *
 * @param fileName 文件路径
 * @return 文件内容
 */
QString YoutubeJsonParser::read_file(const QString& fileName)
{
    QString val;
    QFile file;
    file.setFileName(fileName);
    // ★ ReadOnly: 只读；Text: 自动转 \r\n -> \n
    // ★ C4834: [[nodiscard]] 必须检查 open() 返回值
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        val = file.readAll();
        file.close();
    }
    return val;
}

/**
 * @brief 按 compression 类型筛选出对应的流列表
 *
 * @param streams [out] 输出流列表（追加，不清空）
 * @param type     压缩类型：Compressed=音视频合一 / VideoOnly=纯视频 / 其他=纯音频
 */
void YoutubeJsonParser::get_streams(std::vector<YtStream>& streams, CompressionType type) const
{
    if (!m_data.m_valid)
        return;

    // 按类型筛选流
    if (type == CompressionType::Compressed)
    {
        // ★ "压缩"= 音视频合一的流（最简单，不用 ffmpeg 合流）
        for (const auto& st : m_data.streams)
        {
            if (st.progressive)
                streams.push_back(st);
        }
    }
    else if (type == CompressionType::VideoOnly)
    {
        // ★ 纯视频流（高分辨率常用，需要单独下音频再合流）
        for (const auto& st : m_data.streams)
        {
            if (!st.progressive && st.type == StreamType::Video)
                streams.push_back(st);
        }
    }
    else
    {
        // ★ 纯音频流（高音质常用，要单独下视频再合流）
        for (const auto& st : m_data.streams)
        {
            if (!st.progressive && st.type == StreamType::Audio)
                streams.push_back(st);
        }
    }
}

/**
 * @brief 从某类型流里取一个（按 order 决定取 best 还是 worst）
 *
 * @param type   流类型
 * @param stream [out] 取到的流
 * @param order  true=取最后一个（pytube 把最佳放最后），false=取第一个
 * @return 是否取到（流列表非空才算成功）
 */
bool YoutubeJsonParser::get_yt_stream(CompressionType type, YtStream& stream, bool order) const
{
    std::vector<YtStream> streams;
    get_streams(streams, type);
    if (streams.size())
    {
        if (order)
        {
            // ★ pytube 把最清晰的流放最后，取 back 就是"最佳"
            stream = streams.back();
        }
        else
        {
            // ★ 取 front 就是"最差"
            stream = streams.front();
        }
        return true;
    }
    return false;
}

/**
 * @brief 取 Compressed 流的最佳一项
 *
 * @param st [out] 取到的流
 * @return 是否取到
 */
bool YoutubeJsonParser::get_best_stream(YtStream& st) const
{
    return get_yt_stream(CompressionType::Compressed, st);
}

/**
 * @brief 取 Compressed 流的最低质量一项
 *
 * @param st [out] 取到的流
 * @return 是否取到
 */
bool YoutubeJsonParser::get_worst_stream(YtStream& st) const
{
    return get_yt_stream(CompressionType::Compressed, st, false);
}

/**
 * @brief 取纯视频流的最高画质一项
 *
 * @param st [out] 取到的流
 * @return 是否取到
 */
bool YoutubeJsonParser::get_bestvideo_stream(YtStream& st) const
{
    return get_yt_stream(CompressionType::VideoOnly, st);
}

/**
 * @brief 取纯视频流的最低画质一项
 *
 * @param st [out] 取到的流
 * @return 是否取到
 */
bool YoutubeJsonParser::get_worstvideo_stream(YtStream& st) const
{
    return get_yt_stream(CompressionType::VideoOnly, st, false);
}

/**
 * @brief 取纯音频流的最高音质一项
 *
 * @param st [out] 取到的流
 * @return 是否取到
 */
bool YoutubeJsonParser::get_bestaudio_stream(YtStream& st) const
{
    return get_yt_stream(CompressionType::AudioOnly, st);
}

/**
 * @brief 取纯音频流的最低音质一项
 *
 * @param st [out] 取到的流
 * @return 是否取到
 */
bool YoutubeJsonParser::get_worstaudio_stream(YtStream& st) const
{
    return get_yt_stream(CompressionType::AudioOnly, st, false);
}

/**
 * @brief 一次性取某类型流的 URL
 *
 * @param type  流类型
 * @param order true=最佳 / false=最差
 * @return 直链 URL；取不到返回空串
 */
QString YoutubeJsonParser::get_yt_url(CompressionType type, bool order) const
{
    YtStream st;
    if (get_yt_stream(type, st, order))
        return st.url;
    return QString("");
}

/**
 * @brief 最佳 Compressed 流的 URL
 *
 * @return URL；取不到返回空串
 */
QString YoutubeJsonParser::get_best_url() const
{
    return get_yt_url(CompressionType::Compressed);
}

/**
 * @brief 最差 Compressed 流的 URL
 *
 * @return URL；取不到返回空串
 */
QString YoutubeJsonParser::get_worst_url() const
{
    return get_yt_url(CompressionType::Compressed, false);
}

/**
 * @brief 最佳视频流的 URL
 *
 * @return URL；取不到返回空串
 */
QString YoutubeJsonParser::get_bestvideo_url() const
{
    return get_yt_url(CompressionType::VideoOnly);
}

/**
 * @brief 最差视频流的 URL
 *
 * @return URL；取不到返回空串
 */
QString YoutubeJsonParser::get_worstvideo_url() const
{
    return get_yt_url(CompressionType::VideoOnly, false);
}

/**
 * @brief 最佳音频流的 URL
 *
 * @return URL；取不到返回空串
 */
QString YoutubeJsonParser::get_bestaudio_url() const
{
    return get_yt_url(CompressionType::AudioOnly);
}

/**
 * @brief 最差音频流的 URL
 *
 * @return URL；取不到返回空串
 */
QString YoutubeJsonParser::get_worstaudio_url() const
{
    return get_yt_url(CompressionType::AudioOnly, false);
}
