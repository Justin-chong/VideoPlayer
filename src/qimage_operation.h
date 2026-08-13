/**
 * @file qimage_operation.h
 * @brief QImage 基础图像处理操作
 *
 * 一组轻量级图像处理函数（基于 QPainter，不依赖 OpenCV）。
 * 提供：文字绘制、灰度化、随机化、通道分离、反色、镜像、缩放、伽马变换等。
 *
 * 如果要做更复杂的图像处理（边缘检测、人脸识别等），用 imagecv_operations.h。
 */

#ifndef QIMAGE_OPERATION_H
#define QIMAGE_OPERATION_H
#include <QImage>
#include <QRandomGenerator>  // 随机数
#include <QPainter>
#include <QPen>
#include <QFont>
#include <QGraphicsEffect>      // Qt 图像特效
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QGraphicsColorizeEffect>  // 颜色化特效

// ===== 基础信息 =====
void image_info(const QImage& img);                                // 打印 QImage 信息（尺寸、格式）
void create_image(QImage& img);                                   // 创建一个测试图

// ===== 绘制 =====
bool draw_img_text(QImage& img, const QString& str, const QRect rt,
                   QPen pen = QPen(Qt::red), QFont font = QFont("Times", 48, QFont::Bold));  // 在图上画文字
bool draw_img_rect(QImage& img, const QRect rt, QPen pen = QPen(Qt::red));  // 画矩形

// ===== 像素变换 =====
void grey_image(QImage& img);               // 灰度化
void random_image(QImage& img);             // 随机化（用于测试）
void split_image(QImage& img, QImage& r_img, QImage& b_img, QImage& g_img);  // 分离 R/G/B 通道
void invert_image(QImage& img);             // 反色
void mirro_image(QImage& img, bool horizontal = true, bool vertical = false);  // 镜像翻转
void swap_image(QImage& img);              // RGB ↔ BGR 通道交换

// ===== 几何变换 =====
void scale_image(QImage& img, int width, int height);  // 缩放
void transform_image(QImage& img, const QTransform& matrix, Qt::TransformationMode mode = Qt::FastTransformation);  // 任意变换矩阵
QImage gamma_image(const QImage& img, double exp = 1 / 2.0);  // 伽马变换

/*
 * 下面是一些更高级的特效（用 QGraphicsEffect 实现），
 * 当前代码里被注释掉了，需要时取消注释。
QImage applyEffectToImage(QImage& src, QGraphicsEffect* effect, int extent = 0);
QImage blur_img(QImage& img, int radius = 5, int extent = 0);     // 模糊
QImage dropshadow_img(QImage& img, ...);                          // 投影
QImage colorize_img(QImage& img, ...);                            // 颜色化
QImage opacity_img(QImage& img, double opacity = 0.5);            // 透明度
*/
#endif // QIMAGE_OPERATION_H
