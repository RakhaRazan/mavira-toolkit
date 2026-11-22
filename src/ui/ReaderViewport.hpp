#pragma once

#include "app/ReaderController.hpp"
#include "core/rendering/RenderingEngine.hpp"
#include "data/config/SettingsManager.hpp"

#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPointF>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;
class QWidget;

namespace mavira::ui {

/// Hardware-accelerated OpenGL reader viewport (`QOpenGLWidget`).
///
/// Features:
/// - `GL_LINEAR_MIPMAP_LINEAR` texture filtering for smooth 60-120 FPS zoom/pan
/// - Multi-tile Webtoon texture rendering (`> 4096 px` strips sliced into `2048 px` GPU tiles)
/// - Hold-to-Zoom Loupe Magnifier GLSL shader (`assets/shaders/loupe_magnifier.frag`, `2.5x-3.5x`)
/// - Sleek auto-hiding HUD overlay (page slider, bookmark toggle, layout toggles)
class ReaderViewport : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    static constexpr float kMinLoupeZoom = 2.5f;
    static constexpr float kMaxLoupeZoom = 3.5f;
    static constexpr float kDefaultLoupeZoom = 3.0f;
    static constexpr float kDefaultLoupeRadiusPx = 115.0f;
    static constexpr int kHudAutoHideMs = 2500;

    explicit ReaderViewport(
        mavira::app::ReaderController* controller,
        QWidget* parent = nullptr);

    ~ReaderViewport() override;

    void applySettings(const mavira::data::config::AppSettings& settings);
    void setDarkTheme(bool dark);

    // -------------------------------------------------------------------------
    // Shader & Tool Controls
    // -------------------------------------------------------------------------
    void setLoupeEnabled(bool enabled);
    [[nodiscard]] bool isLoupeEnabled() const noexcept { return loupeFeatureEnabled_; }
    [[nodiscard]] bool isLoupeActive() const noexcept { return loupeActive_; }

    void setLoupeZoom(float zoom);
    [[nodiscard]] float loupeZoom() const noexcept { return loupeZoom_; }

    void showHud();
    void hideHud();
    [[nodiscard]] bool isHudVisible() const;

    [[nodiscard]] bool shadersCompiled() const noexcept { return shadersReady_; }
    [[nodiscard]] std::size_t uploadedTileCount() const noexcept { return gpuTiles_.size(); }

    // Programmatic pan/scroll & lifecycle helpers
    void resetPanAndScroll();
    [[nodiscard]] QPointF panOffset() const noexcept { return panOffset_; }

public slots:
    void clearPage();

signals:
    void backToLibraryRequested();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;

private slots:
    void onPageReady(int pageIndex, const mavira::core::rendering::SpreadLayoutResult& layout);
    void onComicOpened(qint64 comicId, int totalPages);
    void onComicClosed();
    void onViewStateChanged();
    void onBookmarksChanged();

private:
    struct GpuTile {
        GLuint textureId{0};
        std::uint32_t offsetY{0};
        std::uint32_t width{0};
        std::uint32_t height{0};
    };

    void setupHudOverlay();
    void layoutHudOverlay();
    void syncHudFromController();
    void updateBookmarkButtonState();
    void cleanupGpuResources();
    void destroyTileTextures();
    void uploadPendingLayout();
    [[nodiscard]] bool loadShaderPrograms();
    [[nodiscard]] QString resolveShaderPath(const QString& fileName) const;

    void clampPanOffset(float contentW, float contentH);
    [[nodiscard]] QPointF screenToImageUv(
        const QPointF& pos,
        float quadLeft,
        float quadTop,
        float quadWidth,
        float quadHeight) const noexcept;

    mavira::app::ReaderController* controller_{nullptr};

    // OpenGL resources
    bool glInitialized_{false};
    bool shadersReady_{false};
    QOpenGLVertexArrayObject vao_{};
    QOpenGLBuffer vbo_{QOpenGLBuffer::VertexBuffer};
    std::unique_ptr<QOpenGLShaderProgram> quadShader_{};
    std::unique_ptr<QOpenGLShaderProgram> loupeShader_{};
    std::vector<GpuTile> gpuTiles_{};

    std::optional<mavira::core::rendering::SpreadLayoutResult> pendingLayout_{};
    std::optional<mavira::core::rendering::SpreadLayoutResult> activeLayout_{};
    bool layoutDirty_{false};

    // Viewport interaction & tool state
    bool darkTheme_{true};
    bool loupeFeatureEnabled_{true};
    bool loupeActive_{false};
    float loupeZoom_{kDefaultLoupeZoom};
    QPointF cursorPos_{0.0, 0.0};

    bool panning_{false};
    QPointF lastMousePos_{0.0, 0.0};
    QPointF panOffset_{0.0, 0.0};

    // Auto-hiding HUD overlay widgets
    QTimer* hudHideTimer_{nullptr};
    QWidget* hudOverlay_{nullptr};
    QPushButton* btnHudBack_{nullptr};
    QSlider* pageSlider_{nullptr};
    QLabel* pageCounterLabel_{nullptr};
    QPushButton* btnBookmark_{nullptr};
    QComboBox* cmbReadingMode_{nullptr};
    QPushButton* btnDirection_{nullptr};
    QComboBox* cmbFitMode_{nullptr};
    QPushButton* btnAutoCrop_{nullptr};
    bool updatingHudControls_{false};
};

} // namespace mavira::ui
