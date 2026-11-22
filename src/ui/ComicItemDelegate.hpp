#pragma once

#include <QColor>
#include <QSize>
#include <QStyledItemDelegate>

#include <string_view>

namespace mavira::ui {

/// Custom model data roles shared between `LibraryModel` and `ComicItemDelegate`.
enum ComicItemRole : int {
    RoleComicId = Qt::UserRole + 1,
    RoleFormatText,
    RolePageCount,
    RoleCurrentPage,
    RoleIsRead,
    RoleIsOfflinePinned,
    RoleIsHidden
};

/// High-contrast native item delegate for `QListView` IconMode.
/// Paints cover thumbnails, circular progress rings, read/unread status badges,
/// offline-pinned badges, and Smart Rename cleaned titles at 60-120 FPS.
class ComicItemDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    static constexpr int kCardWidth = 185;
    static constexpr int kCardHeight = 275;
    static constexpr int kCoverHeight = 220;

    explicit ComicItemDelegate(QObject* parent = nullptr);

    void setCardDensity(std::string_view density) noexcept;
    [[nodiscard]] int cardWidth() const noexcept { return cardWidth_; }
    [[nodiscard]] int cardHeight() const noexcept { return cardHeight_; }
    [[nodiscard]] int coverHeight() const noexcept { return coverHeight_; }

    void setShowCircularProgress(bool enabled) noexcept { showCircularProgress_ = enabled; }
    [[nodiscard]] bool showCircularProgress() const noexcept { return showCircularProgress_; }

    void setDarkTheme(bool dark) noexcept { darkTheme_ = dark; useCustomPalette_ = false; }
    [[nodiscard]] bool isDarkTheme() const noexcept { return darkTheme_; }

    void setThemePalette(
        bool dark,
        const QColor& cardBg,
        const QColor& coverBg,
        const QColor& accentCol,
        const QColor& borderCol,
        const QColor& hoverBorderCol,
        const QColor& textCol,
        const QColor& mutedTextCol) noexcept {
        darkTheme_ = dark;
        useCustomPalette_ = true;
        cardBg_ = cardBg;
        coverBg_ = coverBg;
        accentCol_ = accentCol;
        borderCol_ = borderCol;
        hoverBorderCol_ = hoverBorderCol;
        textCol_ = textCol;
        mutedTextCol_ = mutedTextCol;
    }

    void paint(
        QPainter* painter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;

    [[nodiscard]] QSize sizeHint(
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;

private:
    int cardWidth_{kCardWidth};
    int cardHeight_{kCardHeight};
    int coverHeight_{kCoverHeight};
    bool showCircularProgress_{true};
    bool darkTheme_{true};
    bool useCustomPalette_{false};
    QColor cardBg_{};
    QColor coverBg_{};
    QColor accentCol_{};
    QColor borderCol_{};
    QColor hoverBorderCol_{};
    QColor textCol_{};
    QColor mutedTextCol_{};
};

} // namespace mavira::ui
