#include "ui/ReaderViewport.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace mavira::ui {

namespace {

using mavira::core::ImageFrame;
using mavira::core::rendering::FitMode;
using mavira::core::rendering::ReadingDirection;
using mavira::core::rendering::ReadingMode;
using mavira::core::rendering::RenderingEngine;
using mavira::core::rendering::SpreadLayoutResult;
using mavira::core::rendering::ViewportTransform;

constexpr float kQuadVertices[] = {
    // posX, posY,   u,    v
    0.0f, 0.0f,  0.0f, 0.0f,
    1.0f, 0.0f,  1.0f, 0.0f,
    0.0f, 1.0f,  0.0f, 1.0f,
    1.0f, 1.0f,  1.0f, 1.0f,
};

} // namespace

ReaderViewport::ReaderViewport(
    mavira::app::ReaderController* controller,
    QWidget* parent)
    : QOpenGLWidget(parent)
    , controller_(controller) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(320, 240);

    hudHideTimer_ = new QTimer(this);
    hudHideTimer_->setSingleShot(true);
    hudHideTimer_->setInterval(kHudAutoHideMs);
    connect(hudHideTimer_, &QTimer::timeout, this, [this]() {
        if (hudOverlay_ != nullptr && !hudOverlay_->underMouse()) {
            hudOverlay_->setVisible(false);
        }
    });

    setupHudOverlay();

    if (controller_ != nullptr) {
        connect(controller_, &mavira::app::ReaderController::pageReady,
                this, &ReaderViewport::onPageReady);
        connect(controller_, &mavira::app::ReaderController::comicOpened,
                this, &ReaderViewport::onComicOpened);
        connect(controller_, &mavira::app::ReaderController::comicClosed,
                this, &ReaderViewport::onComicClosed);
        connect(controller_, &mavira::app::ReaderController::activeComicClosed,
                this, &ReaderViewport::clearPage);
        connect(controller_, &mavira::app::ReaderController::viewStateChanged,
                this, &ReaderViewport::onViewStateChanged);
        connect(controller_, &mavira::app::ReaderController::bookmarksChanged,
                this, &ReaderViewport::onBookmarksChanged);
        connect(controller_, &mavira::app::ReaderController::pageChanged,
                this, [this](int, int) {
                    resetPanAndScroll();
                    syncHudFromController();
                });

        if (auto existing = controller_->currentLayout(); existing.has_value()) {
            onPageReady(static_cast<int>(controller_->currentPage()), *existing);
        }
    }

    syncHudFromController();
}

ReaderViewport::~ReaderViewport() {
    cleanupGpuResources();
}

void ReaderViewport::cleanupGpuResources() {
    if (!glInitialized_ || !isValid()) {
        gpuTiles_.clear();
        return;
    }
    makeCurrent();
    destroyTileTextures();
    quadShader_.reset();
    loupeShader_.reset();
    vbo_.destroy();
    vao_.destroy();
    doneCurrent();
    glInitialized_ = false;
}

void ReaderViewport::destroyTileTextures() {
    for (auto& tile : gpuTiles_) {
        if (tile.textureId != 0) {
            glDeleteTextures(1, &tile.textureId);
            tile.textureId = 0;
        }
    }
    gpuTiles_.clear();
}

QString ReaderViewport::resolveShaderPath(const QString& fileName) const {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/assets/shaders/") + fileName,
        QDir::currentPath() + QStringLiteral("/assets/shaders/") + fileName,
        appDir + QStringLiteral("/../../assets/shaders/") + fileName,
        appDir + QStringLiteral("/../../../assets/shaders/") + fileName,
    };

    for (const QString& path : candidates) {
        if (QFile::exists(path)) {
            return path;
        }
    }
    return candidates.first();
}

bool ReaderViewport::loadShaderPrograms() {
    const QString vertPath = resolveShaderPath(QStringLiteral("reader_quad.vert"));
    const QString loupeFragPath = resolveShaderPath(QStringLiteral("loupe_magnifier.frag"));

    static constexpr const char* kQuadFragSource =
        "#version 330 core\n"
        "in vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    fragColor = texture(uTexture, vTexCoord);\n"
        "}\n";

    quadShader_ = std::make_unique<QOpenGLShaderProgram>();
    if (!quadShader_->addShaderFromSourceFile(QOpenGLShader::Vertex, vertPath) ||
        !quadShader_->addShaderFromSourceCode(QOpenGLShader::Fragment, kQuadFragSource) ||
        !quadShader_->link()) {
        spdlog::warn("ReaderViewport: failed to compile/link quad shader: {}",
                     quadShader_->log().toStdString());
        quadShader_.reset();
        return false;
    }

    loupeShader_ = std::make_unique<QOpenGLShaderProgram>();
    if (!loupeShader_->addShaderFromSourceFile(QOpenGLShader::Vertex, vertPath) ||
        !loupeShader_->addShaderFromSourceFile(QOpenGLShader::Fragment, loupeFragPath) ||
        !loupeShader_->link()) {
        spdlog::warn("ReaderViewport: failed to compile/link loupe_magnifier shader: {}",
                     loupeShader_->log().toStdString());
        loupeShader_.reset();
        return false;
    }

    return true;
}

void ReaderViewport::initializeGL() {
    initializeOpenGLFunctions();
    glInitialized_ = true;

    vao_.create();
    vao_.bind();

    vbo_.create();
    vbo_.bind();
    vbo_.allocate(kQuadVertices, sizeof(kQuadVertices));

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<const void*>(2 * sizeof(float)));

    vbo_.release();
    vao_.release();

    shadersReady_ = loadShaderPrograms();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void ReaderViewport::resizeGL(int w, int h) {
    glViewport(0, 0, std::max(1, w), std::max(1, h));
}

void ReaderViewport::uploadPendingLayout() {
    if (!layoutDirty_ || !glInitialized_) {
        return;
    }
    layoutDirty_ = false;
    destroyTileTextures();
    activeLayout_ = pendingLayout_;
    pendingLayout_.reset();

    if (!activeLayout_.has_value() || !activeLayout_->isValid()) {
        return;
    }

    gpuTiles_.reserve(activeLayout_->tiles.size());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    for (const auto& tile : activeLayout_->tiles) {
        const ImageFrame& frame = tile.frame;
        if (!frame.isValid()) {
            continue;
        }

        GLuint texId = 0;
        glGenTextures(1, &texId);
        glBindTexture(GL_TEXTURE_2D, texId);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        const std::uint32_t rowPixels = frame.effectiveStride() / ImageFrame::kBytesPerPixel;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(rowPixels));
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA8,
            static_cast<GLsizei>(frame.width),
            static_cast<GLsizei>(frame.height),
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            frame.pixels.data());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glGenerateMipmap(GL_TEXTURE_2D);

        gpuTiles_.push_back(GpuTile{texId, tile.offsetY, frame.width, frame.height});
    }

    glBindTexture(GL_TEXTURE_2D, 0);
}

void ReaderViewport::paintGL() {
    if (darkTheme_) {
        glClearColor(0.067f, 0.075f, 0.090f, 1.0f); // #111317
    } else {
        glClearColor(0.957f, 0.961f, 0.969f, 1.0f); // #F4F5F7
    }
    glClear(GL_COLOR_BUFFER_BIT);

    if (layoutDirty_) {
        uploadPendingLayout();
    }

    if (!shadersReady_ || !activeLayout_.has_value() || gpuTiles_.empty()) {
        return;
    }

    const float viewW = static_cast<float>(std::max(1, width()));
    const float viewH = static_cast<float>(std::max(1, height()));

    const FitMode fitMode = (controller_ != nullptr) ? controller_->fitMode() : FitMode::FitScreen;
    const float zoom = (controller_ != nullptr) ? controller_->zoomFactor() : 1.0f;
    const int rotation = (controller_ != nullptr) ? controller_->rotationDegrees() : 0;
    const ReadingMode mode = (controller_ != nullptr)
        ? controller_->readingMode()
        : ReadingMode::SinglePage;

    const auto& composed = activeLayout_->composedFrame;
    const ViewportTransform xf = RenderingEngine::computeViewportTransform(
        composed.width,
        composed.height,
        static_cast<std::uint32_t>(viewW),
        static_cast<std::uint32_t>(viewH),
        (mode == ReadingMode::VerticalWebtoon && fitMode == FitMode::FitScreen)
            ? FitMode::FitWidth
            : fitMode,
        zoom,
        rotation);

    clampPanOffset(xf.contentWidth, xf.contentHeight);

    const float centerX = (viewW * 0.5f) + static_cast<float>(panOffset_.x());
    float centerY = (viewH * 0.5f) + static_cast<float>(panOffset_.y());
    if (mode == ReadingMode::VerticalWebtoon && xf.contentHeight > viewH) {
        // Align top of Webtoon strip with top of viewport when scrolled to 0.
        centerY = (xf.contentHeight * 0.5f) + static_cast<float>(panOffset_.y());
    }

    QMatrix4x4 proj;
    proj.ortho(0.0f, viewW, viewH, 0.0f, -1.0f, 1.0f);

    QMatrix4x4 baseModel;
    baseModel.translate(centerX, centerY, 0.0f);
    baseModel.rotate(static_cast<float>(xf.rotationDegrees), 0.0f, 0.0f, 1.0f);

    const bool quarterTurn = (xf.rotationDegrees == 90 || xf.rotationDegrees == 270);
    const float unrotatedW = quarterTurn ? xf.contentHeight : xf.contentWidth;
    const float unrotatedH = quarterTurn ? xf.contentWidth : xf.contentHeight;
    const float totalImgH = static_cast<float>(std::max<std::uint32_t>(1, composed.height));

    vao_.bind();
    quadShader_->bind();
    quadShader_->setUniformValue("uTexture", 0);

    glActiveTexture(GL_TEXTURE0);
    for (const auto& tile : gpuTiles_) {
        const float tileFracY = static_cast<float>(tile.offsetY) / totalImgH;
        const float tileFracH = static_cast<float>(tile.height) / totalImgH;
        const float tileLeft = -unrotatedW * 0.5f;
        const float tileTop = -unrotatedH * 0.5f + tileFracY * unrotatedH;
        const float tileDrawH = tileFracH * unrotatedH;

        QMatrix4x4 tileModel = baseModel;
        tileModel.translate(tileLeft, tileTop, 0.0f);
        tileModel.scale(unrotatedW, tileDrawH, 1.0f);

        quadShader_->setUniformValue("uMvp", proj * tileModel);

        glBindTexture(GL_TEXTURE_2D, tile.textureId);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    quadShader_->release();

    // Hold-to-Zoom Circular Loupe Magnifier Pass
    if (loupeActive_ && loupeFeatureEnabled_ && loupeShader_ != nullptr) {
        loupeShader_->bind();
        loupeShader_->setUniformValue("uTexture", 0);
        loupeShader_->setUniformValue("uLoupeRadiusPx", kDefaultLoupeRadiusPx);
        loupeShader_->setUniformValue("uLoupeZoom", loupeZoom_);

        for (const auto& tile : gpuTiles_) {
            const float tileFracY = static_cast<float>(tile.offsetY) / totalImgH;
            const float tileFracH = static_cast<float>(tile.height) / totalImgH;
            const float tileLeft = centerX - unrotatedW * 0.5f;
            const float tileTop = centerY - unrotatedH * 0.5f + tileFracY * unrotatedH;
            const float tileDrawH = tileFracH * unrotatedH;

            // Check if cursor is within vertical range of this tile (+ lens radius)
            if (cursorPos_.y() < tileTop - kDefaultLoupeRadiusPx - 4.0f ||
                cursorPos_.y() > tileTop + tileDrawH + kDefaultLoupeRadiusPx + 4.0f) {
                continue;
            }

            const QPointF cursorUv = screenToImageUv(
                cursorPos_, tileLeft, tileTop, unrotatedW, tileDrawH);

            QMatrix4x4 tileModel = baseModel;
            tileModel.translate(-unrotatedW * 0.5f, -unrotatedH * 0.5f + tileFracY * unrotatedH, 0.0f);
            tileModel.scale(unrotatedW, tileDrawH, 1.0f);

            loupeShader_->setUniformValue("uMvp", proj * tileModel);
            loupeShader_->setUniformValue(
                "uTexelSize",
                QVector2D(1.0f / static_cast<float>(std::max<std::uint32_t>(1, tile.width)),
                          1.0f / static_cast<float>(std::max<std::uint32_t>(1, tile.height))));
            loupeShader_->setUniformValue(
                "uCursorUv",
                QVector2D(static_cast<float>(cursorUv.x()), static_cast<float>(cursorUv.y())));
            loupeShader_->setUniformValue("uQuadSizePx", QVector2D(unrotatedW, tileDrawH));

            glBindTexture(GL_TEXTURE_2D, tile.textureId);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        loupeShader_->release();
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    vao_.release();
}

QPointF ReaderViewport::screenToImageUv(
    const QPointF& pos,
    float quadLeft,
    float quadTop,
    float quadWidth,
    float quadHeight) const noexcept {
    const float u = (quadWidth > 0.0f) ? (static_cast<float>(pos.x()) - quadLeft) / quadWidth : 0.5f;
    const float v = (quadHeight > 0.0f) ? (static_cast<float>(pos.y()) - quadTop) / quadHeight : 0.5f;
    return QPointF(u, v);
}

void ReaderViewport::clampPanOffset(float contentW, float contentH) {
    const float viewW = static_cast<float>(std::max(1, width()));
    const float viewH = static_cast<float>(std::max(1, height()));

    const float maxPanX = std::max(0.0f, (contentW - viewW) * 0.5f);
    panOffset_.setX(std::clamp(panOffset_.x(), static_cast<qreal>(-maxPanX), static_cast<qreal>(maxPanX)));

    const ReadingMode mode = (controller_ != nullptr)
        ? controller_->readingMode()
        : ReadingMode::SinglePage;

    if (mode == ReadingMode::VerticalWebtoon && contentH > viewH) {
        const float minY = -(contentH - viewH);
        panOffset_.setY(std::clamp(panOffset_.y(), static_cast<qreal>(minY), 0.0));
    } else {
        const float maxPanY = std::max(0.0f, (contentH - viewH) * 0.5f);
        panOffset_.setY(std::clamp(panOffset_.y(), static_cast<qreal>(-maxPanY), static_cast<qreal>(maxPanY)));
    }
}

void ReaderViewport::resetPanAndScroll() {
    panOffset_ = QPointF(0.0, 0.0);
    update();
}

void ReaderViewport::applySettings(const mavira::data::config::AppSettings& settings) {
    setLoupeEnabled(settings.loupe_magnifier_enabled);
    const bool isLight =
        (settings.theme == "light" || settings.theme == "Light" ||
         settings.theme == "warm_parchment" || settings.theme == "nordic_glacier" ||
         settings.theme == "sakura_mist");
    setDarkTheme(!isLight);
    syncHudFromController();
}

void ReaderViewport::setDarkTheme(bool dark) {
    darkTheme_ = dark;
    update();
}

void ReaderViewport::setLoupeEnabled(bool enabled) {
    loupeFeatureEnabled_ = enabled;
    if (!enabled && loupeActive_) {
        loupeActive_ = false;
        update();
    }
}

void ReaderViewport::setLoupeZoom(float zoom) {
    loupeZoom_ = std::clamp(zoom, kMinLoupeZoom, kMaxLoupeZoom);
    if (loupeActive_) {
        update();
    }
}

void ReaderViewport::showHud() {
    if (hudOverlay_ != nullptr) {
        layoutHudOverlay();
        hudOverlay_->setVisible(true);
        hudOverlay_->raise();
    }
    if (hudHideTimer_ != nullptr) {
        hudHideTimer_->start();
    }
}

void ReaderViewport::hideHud() {
    if (hudHideTimer_ != nullptr) {
        hudHideTimer_->stop();
    }
    if (hudOverlay_ != nullptr) {
        hudOverlay_->setVisible(false);
    }
}

bool ReaderViewport::isHudVisible() const {
    return hudOverlay_ != nullptr && hudOverlay_->isVisible();
}

void ReaderViewport::onPageReady(int, const SpreadLayoutResult& layout) {
    pendingLayout_ = layout;
    layoutDirty_ = true;
    syncHudFromController();
    update();
}

void ReaderViewport::onComicOpened(qint64, int) {
    resetPanAndScroll();
    syncHudFromController();
    showHud();
}

void ReaderViewport::clearPage() {
    onComicClosed();
}

void ReaderViewport::onComicClosed() {
    pendingLayout_.reset();
    activeLayout_.reset();
    layoutDirty_ = true;
    resetPanAndScroll();
    if (glInitialized_ && isValid()) {
        makeCurrent();
        destroyTileTextures();
        doneCurrent();
    } else {
        gpuTiles_.clear();
    }
    syncHudFromController();
    update();
}

void ReaderViewport::onViewStateChanged() {
    syncHudFromController();
    update();
}

void ReaderViewport::onBookmarksChanged() {
    updateBookmarkButtonState();
}

void ReaderViewport::resizeEvent(QResizeEvent* event) {
    QOpenGLWidget::resizeEvent(event);
    layoutHudOverlay();
}

void ReaderViewport::keyPressEvent(QKeyEvent* event) {
    if (controller_ == nullptr) {
        QOpenGLWidget::keyPressEvent(event);
        return;
    }

    const bool rtl = (controller_->readingDirection() == ReadingDirection::RTL);
    const Qt::KeyboardModifiers mods = event->modifiers();

    switch (event->key()) {
        case Qt::Key_Escape:
            emit backToLibraryRequested();
            return;
        case Qt::Key_Right:
            if (rtl) {
                controller_->previousPage();
            } else {
                controller_->nextPage();
            }
            return;
        case Qt::Key_Left:
            if (rtl) {
                controller_->nextPage();
            } else {
                controller_->previousPage();
            }
            return;
        case Qt::Key_Space:
        case Qt::Key_PageDown:
            controller_->nextPage();
            return;
        case Qt::Key_Backspace:
        case Qt::Key_PageUp:
            controller_->previousPage();
            return;
        case Qt::Key_Home:
            controller_->firstPage();
            return;
        case Qt::Key_End:
            controller_->lastPage();
            return;
        case Qt::Key_M:
            controller_->setReadingDirection(rtl ? ReadingDirection::LTR : ReadingDirection::RTL);
            return;
        case Qt::Key_Z:
            setLoupeEnabled(!loupeFeatureEnabled_);
            return;
        case Qt::Key_R:
            controller_->rotateClockwise();
            return;
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            if (mods.testFlag(Qt::ControlModifier)) {
                controller_->zoomIn();
                return;
            }
            break;
        case Qt::Key_Minus:
            if (mods.testFlag(Qt::ControlModifier)) {
                controller_->zoomOut();
                return;
            }
            break;
        case Qt::Key_0:
            if (mods.testFlag(Qt::ControlModifier)) {
                controller_->resetZoom();
                resetPanAndScroll();
                return;
            }
            break;
        case Qt::Key_B:
            if (mods.testFlag(Qt::ControlModifier) && btnBookmark_ != nullptr) {
                btnBookmark_->click();
                return;
            }
            break;
        default:
            break;
    }

    QOpenGLWidget::keyPressEvent(event);
}

void ReaderViewport::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    cursorPos_ = event->position();

    if (event->button() == Qt::LeftButton) {
        panning_ = true;
        lastMousePos_ = event->position();
        setCursor(Qt::ClosedHandCursor);
    } else if (event->button() == Qt::RightButton && loupeFeatureEnabled_) {
        loupeActive_ = true;
        update();
    }

    showHud();
    QOpenGLWidget::mousePressEvent(event);
}

void ReaderViewport::mouseMoveEvent(QMouseEvent* event) {
    cursorPos_ = event->position();

    if (panning_) {
        const QPointF delta = event->position() - lastMousePos_;
        lastMousePos_ = event->position();
        panOffset_ += delta;
        update();
    } else if (loupeActive_) {
        update();
    }

    showHud();
    QOpenGLWidget::mouseMoveEvent(event);
}

void ReaderViewport::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        panning_ = false;
        unsetCursor();
    } else if (event->button() == Qt::RightButton && loupeActive_) {
        loupeActive_ = false;
        update();
    }

    QOpenGLWidget::mouseReleaseEvent(event);
}

void ReaderViewport::wheelEvent(QWheelEvent* event) {
    if (controller_ == nullptr) {
        QOpenGLWidget::wheelEvent(event);
        return;
    }

    const int deltaY = event->angleDelta().y();
    if (deltaY == 0) {
        return;
    }

    if (loupeActive_) {
        const float step = (deltaY > 0) ? 0.2f : -0.2f;
        setLoupeZoom(loupeZoom_ + step);
        event->accept();
        return;
    }

    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        if (deltaY > 0) {
            controller_->zoomIn();
        } else {
            controller_->zoomOut();
        }
        event->accept();
        return;
    }

    if (controller_->readingMode() == ReadingMode::VerticalWebtoon) {
        panOffset_.setY(panOffset_.y() + (deltaY > 0 ? 90.0 : -90.0));
        update();
        event->accept();
        return;
    }

    if (deltaY > 0) {
        controller_->previousPage();
    } else {
        controller_->nextPage();
    }
    event->accept();
}

void ReaderViewport::leaveEvent(QEvent* event) {
    if (loupeActive_) {
        loupeActive_ = false;
        update();
    }
    QOpenGLWidget::leaveEvent(event);
}

void ReaderViewport::setupHudOverlay() {
    hudOverlay_ = new QWidget(this);
    hudOverlay_->setObjectName(QStringLiteral("ReaderHudOverlay"));
    hudOverlay_->setStyleSheet(QStringLiteral(R"qss(
        QWidget#ReaderHudOverlay {
            background-color: rgba(17, 19, 23, 228);
            border: 1px solid #2B303B;
            border-radius: 0px;
        }
        QWidget#ReaderHudOverlay QLabel {
            color: #EAECEF;
            font-weight: 600;
            background: transparent;
        }
        QWidget#ReaderHudOverlay QPushButton {
            background-color: #21252D;
            color: #EAECEF;
            border: 1px solid #353B48;
            border-radius: 0px;
            padding: 3px 8px;
            font-weight: 600;
        }
        QWidget#ReaderHudOverlay QPushButton:hover {
            border-color: #4D8BF5;
            background-color: #2C323D;
        }
        QWidget#ReaderHudOverlay QPushButton:checked {
            background-color: #2A62C8;
            border-color: #4D8BF5;
            color: #FFFFFF;
        }
        QWidget#ReaderHudOverlay QComboBox {
            background-color: #21252D;
            color: #EAECEF;
            border: 1px solid #353B48;
            border-radius: 0px;
            padding: 2px 6px;
        }
    )qss"));

    auto* hudLayout = new QVBoxLayout(hudOverlay_);
    hudLayout->setContentsMargins(10, 6, 10, 8);
    hudLayout->setSpacing(6);

    // Row 1: Page slider + page counter
    auto* sliderRow = new QHBoxLayout();
    sliderRow->setSpacing(8);

    pageSlider_ = new QSlider(Qt::Horizontal, hudOverlay_);
    pageSlider_->setObjectName(QStringLiteral("HudPageSlider"));
    pageSlider_->setMinimum(0);
    pageSlider_->setMaximum(0);

    pageCounterLabel_ = new QLabel(QStringLiteral("0 / 0"), hudOverlay_);
    pageCounterLabel_->setObjectName(QStringLiteral("HudPageCounter"));
    pageCounterLabel_->setMinimumWidth(78);
    pageCounterLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    sliderRow->addWidget(pageSlider_, 1);
    sliderRow->addWidget(pageCounterLabel_);

    // Row 2: Back, Bookmark, Layout Toggles, Shader & Auto-Scroll
    auto* ctrlRow = new QHBoxLayout();
    ctrlRow->setSpacing(6);

    btnHudBack_ = new QPushButton(QStringLiteral("Library"), hudOverlay_);
    btnHudBack_->setObjectName(QStringLiteral("HudBackBtn"));

    btnBookmark_ = new QPushButton(QStringLiteral("Bookmark"), hudOverlay_);
    btnBookmark_->setObjectName(QStringLiteral("HudBookmarkBtn"));
    btnBookmark_->setCheckable(true);

    cmbReadingMode_ = new QComboBox(hudOverlay_);
    cmbReadingMode_->setObjectName(QStringLiteral("HudReadingModeCombo"));
    cmbReadingMode_->addItem(QStringLiteral("Single"), static_cast<int>(ReadingMode::SinglePage));
    cmbReadingMode_->addItem(QStringLiteral("Dual"), static_cast<int>(ReadingMode::DualPage));
    cmbReadingMode_->addItem(QStringLiteral("Webtoon"), static_cast<int>(ReadingMode::VerticalWebtoon));

    btnDirection_ = new QPushButton(QStringLiteral("RTL"), hudOverlay_);
    btnDirection_->setObjectName(QStringLiteral("HudDirectionBtn"));
    btnDirection_->setCheckable(true);

    cmbFitMode_ = new QComboBox(hudOverlay_);
    cmbFitMode_->setObjectName(QStringLiteral("HudFitModeCombo"));
    cmbFitMode_->addItem(QStringLiteral("Fit Screen"), static_cast<int>(FitMode::FitScreen));
    cmbFitMode_->addItem(QStringLiteral("Fit Width"), static_cast<int>(FitMode::FitWidth));
    cmbFitMode_->addItem(QStringLiteral("Fit Height"), static_cast<int>(FitMode::FitHeight));
    cmbFitMode_->addItem(QStringLiteral("Original"), static_cast<int>(FitMode::Original));

    btnAutoCrop_ = new QPushButton(QStringLiteral("Auto-Crop"), hudOverlay_);
    btnAutoCrop_->setObjectName(QStringLiteral("HudAutoCropBtn"));
    btnAutoCrop_->setCheckable(true);

    ctrlRow->addWidget(btnHudBack_);
    ctrlRow->addWidget(btnBookmark_);
    ctrlRow->addWidget(cmbReadingMode_);
    ctrlRow->addWidget(btnDirection_);
    ctrlRow->addWidget(cmbFitMode_);
    ctrlRow->addWidget(btnAutoCrop_);
    ctrlRow->addStretch(1);

    hudLayout->addLayout(sliderRow);
    hudLayout->addLayout(ctrlRow);

    // Wire HUD controls
    connect(btnHudBack_, &QPushButton::clicked, this, &ReaderViewport::backToLibraryRequested);

    connect(pageSlider_, &QSlider::valueChanged, this, [this](int val) {
        if (updatingHudControls_ || controller_ == nullptr || !controller_->isOpen()) {
            return;
        }
        controller_->goToPage(static_cast<std::size_t>(val));
    });

    connect(btnBookmark_, &QPushButton::clicked, this, [this]() {
        if (updatingHudControls_ || controller_ == nullptr || !controller_->isOpen()) {
            return;
        }
        const std::size_t page = controller_->currentPage();
        for (const auto& bm : controller_->bookmarks()) {
            if (bm.pageIndex == page) {
                (void)controller_->removeBookmark(bm.id);
                return;
            }
        }
        const std::string label = "Page " + std::to_string(page + 1);
        (void)controller_->addBookmark(label, page);
    });

    connect(cmbReadingMode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (updatingHudControls_ || controller_ == nullptr || idx < 0) {
            return;
        }
        const auto mode = static_cast<ReadingMode>(cmbReadingMode_->itemData(idx).toInt());
        controller_->setReadingMode(mode);
    });

    connect(btnDirection_, &QPushButton::toggled, this, [this](bool rtlChecked) {
        if (updatingHudControls_ || controller_ == nullptr) {
            return;
        }
        controller_->setReadingDirection(rtlChecked ? ReadingDirection::RTL : ReadingDirection::LTR);
    });

    connect(cmbFitMode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (updatingHudControls_ || controller_ == nullptr || idx < 0) {
            return;
        }
        const auto fit = static_cast<FitMode>(cmbFitMode_->itemData(idx).toInt());
        controller_->setFitMode(fit);
        resetPanAndScroll();
    });

    connect(btnAutoCrop_, &QPushButton::toggled, this, [this](bool checked) {
        if (updatingHudControls_ || controller_ == nullptr) {
            return;
        }
        controller_->setAutoCropBorders(checked);
    });

    hudOverlay_->setVisible(false);
}

void ReaderViewport::layoutHudOverlay() {
    if (hudOverlay_ == nullptr) {
        return;
    }
    const int margin = 16;
    const int hudW = std::max(280, width() - margin * 2);
    const int hudH = hudOverlay_->sizeHint().height();
    hudOverlay_->setGeometry(margin, std::max(8, height() - hudH - margin), hudW, hudH);
}

void ReaderViewport::syncHudFromController() {
    updatingHudControls_ = true;

    if (controller_ != nullptr) {
        const bool hasVolume = controller_->isOpen();
        const int total = static_cast<int>(controller_->totalPages());
        const int current = static_cast<int>(controller_->currentPage());

        pageSlider_->setEnabled(hasVolume && total > 1);
        pageSlider_->setRange(0, std::max(0, total - 1));
        pageSlider_->setValue(hasVolume ? current : 0);
        pageCounterLabel_->setText(
            hasVolume ? QStringLiteral("%1 / %2").arg(current + 1).arg(total) : QStringLiteral("0 / 0"));

        btnBookmark_->setEnabled(hasVolume);

        const int modeIdx = cmbReadingMode_->findData(static_cast<int>(controller_->readingMode()));
        if (modeIdx >= 0) {
            cmbReadingMode_->setCurrentIndex(modeIdx);
        }

        const bool isRtl = (controller_->readingDirection() == ReadingDirection::RTL);
        btnDirection_->setChecked(isRtl);
        btnDirection_->setText(isRtl ? QStringLiteral("RTL") : QStringLiteral("LTR"));

        const int fitIdx = cmbFitMode_->findData(static_cast<int>(controller_->fitMode()));
        if (fitIdx >= 0) {
            cmbFitMode_->setCurrentIndex(fitIdx);
        }

        btnAutoCrop_->setChecked(controller_->autoCropBorders());
    }

    updatingHudControls_ = false;
    updateBookmarkButtonState();
}

void ReaderViewport::updateBookmarkButtonState() {
    if (btnBookmark_ == nullptr || controller_ == nullptr || !controller_->isOpen()) {
        return;
    }
    const std::size_t page = controller_->currentPage();
    bool bookmarked = false;
    for (const auto& bm : controller_->bookmarks()) {
        if (bm.pageIndex == page) {
            bookmarked = true;
            break;
        }
    }
    updatingHudControls_ = true;
    btnBookmark_->setChecked(bookmarked);
    btnBookmark_->setText(bookmarked ? QStringLiteral("Bookmarked") : QStringLiteral("Bookmark"));
    updatingHudControls_ = false;
}

} // namespace mavira::ui
