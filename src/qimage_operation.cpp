// ***********************************************************/
// qimage_operation.cpp
//
//      Copy Right @ lichong. All rights reserved.
//
// QImage 处理函数 - 给 CV 效果用
//   - 提供"无依赖 OpenCV"的纯 QImage 操作（灰度、随机像素、split、镜像 ...）
//   - 以及一些 demo（create_image / draw_img_text / gamma_image）
//   - 底部有一段被注释的 QGraphicsEffect 版本（blur / shadow / colorize / opacity）
//     留作参考
// ***********************************************************/

#include "qimage_operation.h"

/**
 * @brief 打印 QImage 所有关键参数（仅用于调试）
 *        包含：format、alpha、灰度、宽高、字节数、DPI、像素比 ...
 *
 * @param img 要打印的 QImage
 *
 * 输出：通过 qDebug 输出到控制台，调用一次打印一长串
 */
void image_info(const QImage& img)
{
    qDebug("QImage: w:%d, h:%d, (format:%d,alpha:%d,grey:%d,null:%d),"
           "color count:%d, size(%d,%d), sizeinbytes:%lld,"
           "depth:%d, devicePixelRatio:%f, dotsX:%d, dotsY:%d,"
           "offset(%d,%d), rect(%d,%d,%d,%d),"
           "(hMM:%d,wMM:%d,logDpiX:%d,logDpiY:%d,phsDpiX:%d,phsDpiY:%d)",
           img.width(), img.height(), img.format(), img.hasAlphaChannel(),
           img.isGrayscale(), img.isNull(), img.colorCount(), img.size().width(),
           img.size().height(), img.sizeInBytes(), img.depth(),
           img.devicePixelRatio(), img.dotsPerMeterX(), img.dotsPerMeterY(),
           img.offset().x(), img.offset().y(), img.rect().x(), img.rect().y(),
           img.rect().width(), img.rect().height(), img.heightMM(), img.widthMM(),
           img.logicalDpiX(), img.logicalDpiY(), img.physicalDpiX(),
           img.physicalDpiY());
}

/**
 * @brief demo：填一张测试图（实际就是 random_image 一下）
 *        函数名是 create_image 但内部只是打日志 + 随机填充，是早期 demo
 *
 * @param img 被填充的 QImage（要求 QImage::Format_RGB32 格式）
 */
void create_image(QImage& img)
{
    image_info(img);

    /*int w = img.width();
  int h = img.height();*/

    // QRgb value;
    // value = qRgb(237, 187, 51); // 0xffedba31
    // image.setPixel(2, 1, value);

    int n = 50;
    QRgb value[50];
    // ★ 准备 50 个颜色（黑->白渐变）；实际循环没用到，保留作 demo
    for (int i = 0; i < n; i++)
        value[i] = int(0xff000000 + 0xffffff * i / n);

    // ★ assert 要求 QImage 是 RGB32 格式（4 字节 / 像素）
    Q_ASSERT(img.format() == QImage::Format_RGB32);
    // ★ 调 random_image 把图填成随机彩色
    random_image(img);
}

/**
 * @brief 在图上居中画一段文字
 *        用 QPainter（性能远高于逐像素 setPixel）
 *
 * @param img 目标 QImage
 * @param str 要绘制的文字
 * @param rt  文字所在的矩形（Qt::AlignCenter 让文字居中）
 * @param pen 文字颜色和粗细
 * @param font 字体
 * @return 成功 true；QPainter::begin 失败返回 false
 */
bool draw_img_text(QImage& img, const QString& str, const QRect rt, QPen pen, QFont font)
{
    QPainter p;
    // ★ QPainter::begin 必须先调用，失败说明 img 不是有效的绘图设备
    if (!p.begin(&img))
        return false;

    p.setPen(pen);
    p.setFont(font);
    // ★ Qt::AlignCenter 表示水平+垂直都居中
    p.drawText(rt, Qt::AlignCenter, str);
    return p.end();
}

/**
 * @brief 在图上画一个矩形（用 pen 描边，不是填充）
 *
 * @param img 目标 QImage
 * @param rt  矩形区域
 * @param pen 描边笔
 * @return 成功 true；QPainter::begin 失败返回 false
 */
bool draw_img_rect(QImage& img, const QRect rt, QPen pen)
{
    QPainter p;
    if (!p.begin(&img))
        return false;

    p.setPen(pen);
    p.drawRect(rt);
    return p.end();
}

/**
 * @brief 转灰度图
 *        用 Qt 自带的 convertToFormat(Format_Grayscale8) 一行搞定
 *        注释掉的是手写循环版本（演示用，效率低）
 *
 * @param img 输入 + 输出图（in-place 改写）
 *
 * 性能对比：convertToFormat 内部用 SIMD 优化，比手写循环快 5-10 倍
 */
void grey_image(QImage& img)
{
#if 1
    // ★ 推荐：Qt 自带的灰度化（内部用 SIMD 优化）
    img = img.convertToFormat(QImage::Format_Grayscale8);
#else
    // 备用：手写循环（演示用）
    int depth = sizeof(QRgb); // 4
    for (int i = 0; i < img.height(); i++)
    {
        // ★ scanLine(i) 拿到第 i 行的裸指针，效率比 setPixel 高很多
        uchar* scan = img.scanLine(i);
        for (int j = 0; j < img.width(); j++)
        {
            // ★ reinterpret_cast 把字节流当成 QRgb* 看待（每 4 字节一个像素）
            QRgb* rgbpixel = reinterpret_cast<QRgb*>(scan + j * depth);
            // ★ qGray 内部按 ITU-R BT.601 公式算灰度
            int gray = qGray(*rgbpixel);
            *rgbpixel = QColor(gray, gray, gray).rgb();
        }
    }
#endif
}

/**
 * @brief 把图填成纯随机彩色（test 用）
 *
 * @param img 目标 QImage
 *
 * 性能优化技巧：
 *   - 用 scanLine 直接拿行指针，比 setPixel 快 10 倍以上
 *   - 连续 reinterpret_cast<QRgb*> 直接写 4 字节
 */
void random_image(QImage& img)
{
#if 1
    // ★ depth = 4（Format_RGB32 每像素 4 字节）
    int depth = img.depth() / 8;
    for (int i = 0; i < img.height(); i++)
    {
        // ★ scanLine(i) 直接拿第 i 行首地址（不拷贝）
        uchar* scan = img.scanLine(i);
        for (int j = 0; j < img.width(); j++)
        {
            // ★ 把字节流当 QRgb*（4 字节 = 1 像素）写
            QRgb* rgbpixel = reinterpret_cast<QRgb*>(scan + j * depth);

            // ★ QRandomGenerator::global() 拿全局随机数生成器
            int r = QRandomGenerator::global()->generate();
            int g = QRandomGenerator::global()->generate();
            int b = QRandomGenerator::global()->generate();

            //*rgbpixel = QColor(r % 255, g % 255, b % 255).rgb();
            // ★ qRgb 是 Qt 的宏，把 R/G/B 三个字节打包成 0xFFRRGGBB
            *rgbpixel = qRgb(r, g, b);
        }
    }
#else
    // 备用：setPixel 版本（慢，演示用）
    for (int i = 0; i < img.height(); i++)
    {
        for (int j = 0; j < img.width(); j++)
        {
            // img.setPixel(j, i, value[w % n]);
            int r = QRandomGenerator::global()->generate();
            int g = QRandomGenerator::global()->generate();
            int b = QRandomGenerator::global()->generate();
            img.setPixel(j, i, qRgb(r % 255, g % 255, b % 255));
        }
    }
#endif
}

/**
 * @brief 拆分成 R / G / B 三张单色图
 *        调试用：可以分别查看每个通道的内容
 *
 * @param img   源图（3 通道彩色图）
 * @param r_img 输出：只保留 R 通道的图
 * @param b_img 输出：只保留 B 通道的图
 * @param g_img 输出：只保留 G 通道的图
 *
 * 实现技巧：用 scanLine 同时拿 4 张图的行指针，一起遍历
 *           qRed/qGreen/qBlue 从 QRgb 里提取单个通道
 */
void split_image(QImage& img, QImage& r_img, QImage& b_img, QImage& g_img)
{
    // ★ 三张输出图必须和源图同尺寸
    Q_ASSERT(img.size() == r_img.size());
    Q_ASSERT(r_img.size() == b_img.size());
    Q_ASSERT(r_img.size() == g_img.size());

    int depth = img.depth() / 8;
    for (int i = 0; i < img.height(); i++)
    {
        // ★ 4 张图同时拿第 i 行的指针（避免在循环里重复算）
        uchar* scan = img.scanLine(i);

        uchar* r_scan = r_img.scanLine(i);
        uchar* b_scan = b_img.scanLine(i);
        uchar* g_scan = g_img.scanLine(i);

        for (int j = 0; j < img.width(); j++)
        {
            // ★ 把行指针当成 QRgb 流看待
            QRgb* all_rgbpixel = reinterpret_cast<QRgb*>(scan + j * depth);

            QRgb* r_rgbpixel = reinterpret_cast<QRgb*>(r_scan + j * depth);
            QRgb* g_rgbpixel = reinterpret_cast<QRgb*>(g_scan + j * depth);
            QRgb* b_rgbpixel = reinterpret_cast<QRgb*>(b_scan + j * depth);

            // ★ qRed/qGreen/qBlue 分别提取 QRgb 的 R/G/B 字节
            uint r = qRed(*all_rgbpixel);
            uint g = qGreen(*all_rgbpixel);
            uint b = qBlue(*all_rgbpixel);

            // ★ 输出图：单通道图，只保留对应通道的值
            *r_rgbpixel = qRgb(r, 0, 0);
            *g_rgbpixel = qRgb(0, g, 0);
            *b_rgbpixel = qRgb(0, 0, b);
        }
    }
}

/**
 * @brief 反色（in-place 改写）
 *        每个像素的 R/G/B 都变为 255 - 原值
 *
 * @param img 输入 + 输出图
 */
void invert_image(QImage& img)
{
    // ★ QImage 自带 invertPixels，内部已优化
    img.invertPixels();
}

/**
 * @brief 镜像翻转
 *
 * @param img        输入 + 输出图（in-place 改写，因为 mirrored 返回新图）
 * @param horizontal true=左右翻转，false=保持水平
 * @param vertical   true=上下翻转，false=保持垂直
 *
 * 用法：mirro_image(img, true, false)  // 水平镜像
 *       mirro_image(img, true, true)   // 水平+垂直（旋转 180°）
 */
void mirro_image(QImage& img, bool horizontal, bool vertical)
{
    // ★ Qt 6 推荐用 flipped()，mirrored() 标了 deprecated
    //   Orientations: Horizontal=水平翻转, Vertical=垂直翻转
    Qt::Orientations orient = Qt::Orientations();
    if (horizontal) orient |= Qt::Horizontal;
    if (vertical) orient |= Qt::Vertical;
    img = img.flipped(orient);
}

/**
 * @brief R 和 B 通道交换（用于 OpenCV/其它库的颜色顺序校正）
 *        例如 RGB 顺序的图片变成 BGR
 *
 * @param img 输入 + 输出图
 */
void swap_image(QImage& img)
{
    // ★ Qt 自带 rgbSwapped 一行搞定
    img = img.rgbSwapped();
}

/**
 * @brief 缩放图像
 *
 * @param img    输入 + 输出图
 * @param width  目标宽度
 * @param height 目标高度
 *
 * 参数说明：
 *   - Qt::IgnoreAspectRatio : 不保持长宽比，直接拉伸（可能变形）
 *   - Qt::FastTransformation : 算法快（最近邻），质量低
 *     若想要更好画质，可改用 Qt::SmoothTransformation（双线性插值）
 */
void scale_image(QImage& img, int width, int height)
{
    img =
        img.scaled(width, height, Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

/**
 * @brief 通用 QTransform 变换（旋转 + 缩放 + 倾斜 ...）
 *
 * @param img    输入 + 输出图
 * @param matrix QTransform 变换矩阵
 * @param mode   变换模式：FastTransformation（快） / SmoothTransformation（平滑）
 */
void transform_image(QImage& img, const QTransform& matrix, Qt::TransformationMode mode)
{
    // ★ transformed 返回新图，赋回 img
    img = img.transformed(matrix, mode);
}

/**
 * @brief Gamma 校正
 *        公式：out = (in/255)^exp * 255
 *
 * @param img 源图（不修改）
 * @param exp 指数：
 *            > 1 图像变暗
 *            < 1 图像变亮
 * @return 校正后的新图（原图不变）
 *
 * 实现：不修改原图，先 copy 一份再处理（对比 gamma_img（OpenCV 版）in-place）
 */
QImage gamma_image(const QImage& img, double exp)
{
    // ★ 复制一份输出图
    QImage retImg(img);

    int depth = img.depth() / 8;
    for (int i = 0; i < img.height(); i++)
    {
        // ★ 源图（const）和目标图（非 const）分别拿行指针
        const uchar* scan = img.scanLine(i);
        uchar* ret_scan = retImg.scanLine(i);

        for (int j = 0; j < img.width(); j++)
        {
            const QRgb* rgbpixel = reinterpret_cast<const QRgb*>(scan + j * depth);

            QRgb* ret_rgbpixel = reinterpret_cast<QRgb*>(ret_scan + j * depth);

            // ★ 把 0~255 归一化到 0.0~1.0 才能做 pow
            const double r = qRed(*rgbpixel) / 255.0;
            const double g = qGreen(*rgbpixel) / 255.0;
            const double b = qBlue(*rgbpixel) / 255.0;

            // ★ 伽马公式：255 * pow(in/255, exp)，QColor 自动 clamp 到 0~255
            *ret_rgbpixel = QColor(255 * std::pow(r, exp), 255 * std::pow(g, exp),
                                   255 * std::pow(b, exp))
                                .rgb();
        }
    }

    return retImg;
}

/*
QImage applyEffectToImage(QImage& src, QGraphicsEffect* effect, int extent)
{
        QGraphicsScene scene;
        QGraphicsPixmapItem item;
        item.setPixmap(QPixmap::fromImage(src));
        item.setGraphicsEffect(effect);
        scene.addItem(&item);
        QImage res(src.size() + QSize(extent * 2, extent * 2),
QImage::Format_ARGB32); res.fill(Qt::transparent); QPainter ptr(&res);
        scene.render(&ptr, QRectF(), QRectF(-extent, -extent, src.width() +
extent * 2, src.height() + extent * 2)); return res;
}

QImage blur_img(QImage& img, int radius, int extent)
{
        QGraphicsBlurEffect* e = new QGraphicsBlurEffect();
        e->setBlurRadius(radius);
        return applyEffectToImage(img, e, extent);
}

QImage dropshadow_img(QImage& img, int radius, int offsetX, int offsetY, QColor
color, int extent)
{
        QGraphicsDropShadowEffect* e = new QGraphicsDropShadowEffect();
        e->setColor(color);
        e->setOffset(offsetX, offsetY);
        e->setBlurRadius(radius);
        return applyEffectToImage(img, e, extent);
}

QImage colorize_img(QImage& img, QColor color, double strength)
{
        QGraphicsColorizeEffect* e = new QGraphicsColorizeEffect();
        e->setColor(color);
        e->setStrength(strength);
        return applyEffectToImage(img, e);
}

QImage opacity_img(QImage& img, double opacity)
{
        QGraphicsOpacityEffect* e = new QGraphicsOpacityEffect();
        e->setOpacity(opacity);
        //e->setOpacityMask();
        return applyEffectToImage(img, e);
}
*/
