#pragma once

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

namespace lowlat {

// 传给渲染层的 YUV 平面视图。
// 注意 stride 通常大于 width（解码器按 16/32 字节对齐），不能假设紧凑布局；
// 这也是外面不能直接用 width 做行偏移的原因。
struct YuvPlanes {
    const std::uint8_t* y{nullptr};
    const std::uint8_t* u{nullptr};
    const std::uint8_t* v{nullptr};
    int width{0};
    int height{0};
    int stride_y{0};
    int stride_u{0};
    int stride_v{0};

    bool valid() const noexcept
    {
        return y != nullptr && u != nullptr && v != nullptr && width > 0 && height > 0 &&
               stride_y >= width && stride_u >= width / 2 && stride_v >= width / 2;
    }
};

// 用三个 GL_R8 纹理承载 Y/U/V，由 fragment shader 做 BT.601 色彩空间转换。
//
// 为什么不在 CPU 侧用 sws_scale 转 RGB 再上传：
//   1. 720p 每帧 sws_scale 约 3~5ms，占满 33ms 预算的 15%
//   2. 转成 RGB 后上传量从 1.4MB 涨到 2.8MB
//   3. 色彩矩阵放在 shader 里可以一处修改、无需重编
class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions {
public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget() override;

    // keepalive 用于延长平面内存的生命周期（通常是解码器帧的持有者）。
    // 传进来之后本类零拷贝直接上传纹理，直到下一帧替换。
    void set_frame(const YuvPlanes& planes, std::shared_ptr<void> keepalive);

    bool has_frame() const noexcept { return has_frame_; }
    QString gl_info() const { return gl_info_; }

    // 已上传的纹理尺寸（用于外部核对是否与视频一致）
    int texture_width() const noexcept { return texture_width_; }
    int texture_height() const noexcept { return texture_height_; }

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    void ensure_textures(int width, int height);
    void upload_plane(int index, const std::uint8_t* data, int width, int height, int stride);
    QRectF fit_rect() const;

    QOpenGLShaderProgram program_;
    unsigned int textures_[3]{0, 0, 0};
    int texture_width_{0};
    int texture_height_{0};
    bool textures_ready_{false};

    YuvPlanes pending_;
    std::shared_ptr<void> keepalive_;
    bool has_frame_{false};
    bool upload_pending_{false};

    std::vector<std::uint8_t> staging_;
    QString gl_info_;
};

}  // namespace lowlat
