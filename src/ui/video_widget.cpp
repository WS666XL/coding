#include "ui/video_widget.hpp"

#include <QDebug>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QRectF>
#include <QUrl>

#include <cstring>
#include <utility>

namespace lowlat {
namespace {

// GLSL 110 是为了在 compatibility profile 与老驱动上都能跑。
// 顶点着色器把 [-1,1] 的裁剪空间坐标映射到 [0,1] 纹理坐标，并翻转 Y：
// OpenGL 纹理原点在左下，而视频帧第一行是图像顶部。
const char* const kVertexShader = R"(
#version 110
attribute vec2 a_position;
varying vec2 v_texcoord;
void main()
{
    v_texcoord = a_position * 0.5 + 0.5;
    v_texcoord.y = 1.0 - v_texcoord.y;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

// BT.601 有限范围（limited range）转换。
// Y 在 [16,235]，Cb/Cr 在 [16,240]，必须先归一化到 [0,1] 再乘矩阵，
// 否则整幅画面会偏灰、对比度不足——这是最常见的 YUV 渲染错误。
const char* const kFragmentShader = R"(
#version 110
uniform sampler2D u_tex_y;
uniform sampler2D u_tex_u;
uniform sampler2D u_tex_v;
varying vec2 v_texcoord;
void main()
{
    float y = texture2D(u_tex_y, v_texcoord).r;
    float u = texture2D(u_tex_u, v_texcoord).r;
    float v = texture2D(u_tex_v, v_texcoord).r;

    y = (y - 16.0 / 255.0) * (255.0 / 219.0);
    u = (u - 128.0 / 255.0) * (255.0 / 224.0);
    v = (v - 128.0 / 255.0) * (255.0 / 224.0);

    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;

    gl_FragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
}
)";

}  // namespace

VideoWidget::VideoWidget(QWidget* parent) : QOpenGLWidget(parent)
{
    setMinimumSize(160, 90);
    setAcceptDrops(true);  // 允许把视频文件拖进来
}

void VideoWidget::set_file_drop_handler(std::function<void(const std::string&)> handler)
{
    drop_handler_ = std::move(handler);
}

void VideoWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData() != nullptr && event->mimeData()->hasUrls() &&
        drop_handler_ != nullptr) {
        event->acceptProposedAction();
        return;
    }
    QOpenGLWidget::dragEnterEvent(event);
}

void VideoWidget::dropEvent(QDropEvent* event)
{
    if (event->mimeData() == nullptr || !event->mimeData()->hasUrls() ||
        drop_handler_ == nullptr) {
        QOpenGLWidget::dropEvent(event);
        return;
    }
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty() || !urls.first().isLocalFile()) {
        return;
    }
    event->acceptProposedAction();
    drop_handler_(urls.first().toLocalFile().toStdString());
}

VideoWidget::~VideoWidget()
{
    if (!context()) {
        return;
    }
    makeCurrent();
    if (textures_ready_) {
        glDeleteTextures(3, textures_);
    }
    doneCurrent();
}

void VideoWidget::set_frame(const YuvPlanes& planes, std::shared_ptr<void> keepalive)
{
    if (!planes.valid()) {
        return;
    }
    pending_ = planes;
    keepalive_ = std::move(keepalive);
    has_frame_ = true;
    upload_pending_ = true;
    update();
}

void VideoWidget::initializeGL()
{
    initializeOpenGLFunctions();

    const auto text = [this](GLenum name) -> QString {
        const auto* value = reinterpret_cast<const char*>(glGetString(name));
        return value != nullptr ? QString::fromLatin1(value) : QStringLiteral("<null>");
    };
    gl_info_ = QStringLiteral("renderer=%1 version=%2").arg(text(GL_RENDERER), text(GL_VERSION));
    qInfo().noquote() << "[video] GL" << gl_info_;

    if (!program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)) {
        qCritical().noquote() << "[video] 顶点着色器编译失败:" << program_.log();
        return;
    }
    if (!program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)) {
        qCritical().noquote() << "[video] 片元着色器编译失败:" << program_.log();
        return;
    }
    program_.bindAttributeLocation("a_position", 0);
    if (!program_.link()) {
        qCritical().noquote() << "[video] 着色器链接失败:" << program_.log();
        return;
    }

    textures_ready_ = true;
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void VideoWidget::ensure_textures(int width, int height)
{
    if (!textures_ready_) {
        return;
    }
    if (texture_width_ == width && texture_height_ == height) {
        return;
    }

    if (textures_[0] == 0) {
        glGenTextures(3, textures_);
    }

    const int chroma_width = width / 2;
    const int chroma_height = height / 2;

    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(i));
        glBindTexture(GL_TEXTURE_2D, textures_[i]);
        // 解码输出的行宽经常大于图像宽度，必须按 1 字节对齐解包
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        const GLsizei w = (i == 0) ? width : chroma_width;
        const GLsizei h = (i == 0) ? height : chroma_height;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    }

    texture_width_ = width;
    texture_height_ = height;
}

void VideoWidget::upload_plane(int index, const std::uint8_t* plane, int width, int height,
                               int stride)
{
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(index));
    glBindTexture(GL_TEXTURE_2D, textures_[index]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (stride == width) {
        // 紧凑布局，直接上传，零拷贝
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, plane);
        return;
    }

    // 有行填充：先拷贝成紧凑缓冲。GL_UNPACK_ROW_LENGTH 在 ES 子集里不可用，
    // 与其依赖扩展不如老老实实拷一次（720p 约 0.2ms）。
    staging_.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int row = 0; row < height; ++row) {
        std::memcpy(staging_.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width),
                    plane + static_cast<std::ptrdiff_t>(row) * stride,
                    static_cast<std::size_t>(width));
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE,
                    staging_.data());
}

QRectF VideoWidget::fit_rect() const
{
    if (texture_width_ <= 0 || texture_height_ <= 0) {
        return QRectF(0, 0, width(), height());
    }
    const double view_w = width();
    const double view_h = height();
    const double video_aspect =
        static_cast<double>(texture_width_) / static_cast<double>(texture_height_);
    const double view_aspect = view_w / view_h;

    double draw_w = view_w;
    double draw_h = view_h;
    if (view_aspect > video_aspect) {
        // 窗口比视频更宽：锁高，左右留黑边
        draw_w = view_h * video_aspect;
    } else {
        draw_h = view_w / video_aspect;
    }
    return QRectF((view_w - draw_w) / 2.0, (view_h - draw_h) / 2.0, draw_w, draw_h);
}

void VideoWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);

    if (!textures_ready_ || !has_frame_) {
        return;
    }

    ensure_textures(pending_.width, pending_.height);
    if (texture_width_ != pending_.width || texture_height_ != pending_.height) {
        return;
    }

    if (upload_pending_) {
        upload_plane(0, pending_.y, pending_.width, pending_.height, pending_.stride_y);
        upload_plane(1, pending_.u, pending_.width / 2, pending_.height / 2, pending_.stride_u);
        upload_plane(2, pending_.v, pending_.width / 2, pending_.height / 2, pending_.stride_v);
        upload_pending_ = false;
    }

    // 保持宽高比，避免画面被拉伸（这一步在真实播放器里也必须做）
    // 注意：用 glViewport 会同时影响清屏区域，所以先清屏再设视口，
    // 最后恢复全窗口视口，保证黑边区域是干净的。
    const double dpr = devicePixelRatioF();
    const QRectF target = fit_rect();
    glViewport(static_cast<int>(target.x() * dpr), static_cast<int>(target.y() * dpr),
               static_cast<int>(target.width() * dpr), static_cast<int>(target.height() * dpr));

    program_.bind();
    program_.setUniformValue("u_tex_y", 0);
    program_.setUniformValue("u_tex_u", 1);
    program_.setUniformValue("u_tex_v", 2);

    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(i));
        glBindTexture(GL_TEXTURE_2D, textures_[i]);
    }

    static const float kQuad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    program_.enableAttributeArray(0);
    program_.setAttributeArray(0, kQuad, 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    program_.disableAttributeArray(0);
    program_.release();

    glViewport(0, 0, static_cast<int>(width() * dpr), static_cast<int>(height() * dpr));
}

void VideoWidget::resizeGL(int width, int height)
{
    const double dpr = devicePixelRatioF();
    glViewport(0, 0, static_cast<int>(width * dpr), static_cast<int>(height * dpr));
}

}  // namespace lowlat
