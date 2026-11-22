#include "ui/ComicItemDelegate.hpp"

#include <QApplication>
#include <QColor>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QRect>
#include <QStyle>

#include <algorithm>

namespace mavira::ui {

ComicItemDelegate::ComicItemDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {}

void ComicItemDelegate::setCardDensity(std::string_view density) noexcept {
    if (density == "compact") {
        cardWidth_ = 150;
        cardHeight_ = 225;
        coverHeight_ = 174;
    } else if (density == "spacious") {
        cardWidth_ = 225;
        cardHeight_ = 330;
        coverHeight_ = 268;
    } else {
        cardWidth_ = kCardWidth;
        cardHeight_ = kCardHeight;
        coverHeight_ = kCoverHeight;
    }
}

QSize ComicItemDelegate::sizeHint(
    const QStyleOptionViewItem& /*option*/,
    const QModelIndex& /*index*/) const {
    return QSize(cardWidth_, cardHeight_);
}

void ComicItemDelegate::paint(
    QPainter* painter,
    const QStyleOptionViewItem& option,
    const QModelIndex& index) const {
    if (!index.isValid() || painter == nullptr) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    const bool isSelected = (option.state & QStyle::State_Selected) != 0;
    const bool isHovered = (option.state & QStyle::State_MouseOver) != 0;

    // Theme palette (supports all 5 curated themes + legacy dark/light fallback)
    const QColor cardBg = useCustomPalette_ ? cardBg_ : (darkTheme_ ? QColor(0x16, 0x1E, 0x2B) : QColor(0xFF, 0xFF, 0xFF));
    const QColor coverBg = useCustomPalette_ ? coverBg_ : (darkTheme_ ? QColor(0x0A, 0x0D, 0x12) : QColor(0xE8, 0xEE, 0xF5));
    const QColor accentCol = useCustomPalette_ ? accentCol_ : (darkTheme_ ? QColor(0x38, 0xBD, 0xF8) : QColor(0x02, 0x84, 0xC7));
    const QColor normalBorder = useCustomPalette_ ? borderCol_ : (darkTheme_ ? QColor(0x26, 0x31, 0x42) : QColor(0xCB, 0xD5, 0xE1));
    const QColor hoverBorder = useCustomPalette_ ? hoverBorderCol_ : (darkTheme_ ? QColor(0x47, 0x55, 0x69) : QColor(0x94, 0xA3, 0xB8));
    const QColor borderCol = isSelected ? accentCol : (isHovered ? hoverBorder : normalBorder);
    const QColor textCol = useCustomPalette_ ? textCol_ : (darkTheme_ ? QColor(0xF1, 0xF5, 0xF9) : QColor(0x0F, 0x17, 0x2A));
    const QColor mutedTextCol = useCustomPalette_ ? mutedTextCol_ : (darkTheme_ ? QColor(0x94, 0xA3, 0xB8) : QColor(0x47, 0x55, 0x69));

    // Outer rounded card bounds (10px radius)
    const QRect cardRect = option.rect.adjusted(5, 5, -5, -5);
    QPainterPath cardPath;
    cardPath.addRoundedRect(QRectF(cardRect), 10.0, 10.0);
    painter->fillPath(cardPath, cardBg);

    const QRect coverRect(
        cardRect.left() + 2,
        cardRect.top() + 2,
        cardRect.width() - 4,
        coverHeight_ - 2);
    QPainterPath coverClip;
    coverClip.addRoundedRect(QRectF(coverRect), 8.0, 8.0);
    painter->save();
    painter->setClipPath(coverClip);
    painter->fillRect(coverRect, coverBg);

    // 1. Cover Thumbnail or Format Placeholder
    const QVariant deco = index.data(Qt::DecorationRole);
    const QPixmap pix = deco.value<QPixmap>();
    if (!pix.isNull()) {
        const QSize scaledSize = pix.size().scaled(coverRect.size(), Qt::KeepAspectRatio);
        const QRect targetRect(
            coverRect.left() + (coverRect.width() - scaledSize.width()) / 2,
            coverRect.top() + (coverRect.height() - scaledSize.height()) / 2,
            scaledSize.width(),
            scaledSize.height());
        painter->drawPixmap(targetRect, pix);
    } else {
        const QString fmt = index.data(RoleFormatText).toString();
        QFont fmtFont = option.font;
        fmtFont.setBold(true);
        fmtFont.setPointSize(11);
        painter->setFont(fmtFont);
        painter->setPen(mutedTextCol);
        painter->drawText(coverRect, Qt::AlignCenter, fmt.isEmpty() ? QStringLiteral("COMIC") : fmt);
    }
    painter->restore();

    const quint32 pageCount = index.data(RolePageCount).toUInt();
    const quint32 currentPage = index.data(RoleCurrentPage).toUInt();
    const bool isRead = index.data(RoleIsRead).toBool();
    const bool isPinned = index.data(RoleIsOfflinePinned).toBool();
    const bool isHidden = index.data(RoleIsHidden).toBool();

    // 2. Read / Unread Status Badge (Top-Left of cover)
    QString statusText;
    QColor badgeBg;
    if (isRead) {
        statusText = QStringLiteral("READ");
        badgeBg = QColor(0x23, 0x86, 0x36, 230);
    } else if (currentPage > 0) {
        statusText = QStringLiteral("%1/%2")
                         .arg(currentPage + 1)
                         .arg(std::max<quint32>(1u, pageCount));
        badgeBg = QColor(0x1F, 0x6F, 0xEB, 230);
    } else {
        statusText = QStringLiteral("UNREAD");
        badgeBg = QColor(0x24, 0x29, 0x33, 220);
    }

    QFont badgeFont = option.font;
    badgeFont.setBold(true);
    badgeFont.setPointSize(7);
    painter->setFont(badgeFont);
    const QFontMetrics badgeFm(badgeFont);
    const int badgeW = badgeFm.horizontalAdvance(statusText) + 12;
    const QRect statusRect(coverRect.left() + 6, coverRect.top() + 6, badgeW, 18);
    painter->setPen(Qt::NoPen);
    painter->setBrush(badgeBg);
    painter->drawRoundedRect(statusRect, 6.0, 6.0);
    painter->setPen(Qt::white);
    painter->drawText(statusRect, Qt::AlignCenter, statusText);

    // 3. Offline-Pinned Badge (Bottom-Left of cover)
    if (isPinned) {
        const QString pinText = QStringLiteral("OFFLINE");
        const int pinW = badgeFm.horizontalAdvance(pinText) + 12;
        const QRect pinRect(coverRect.left() + 6, coverRect.bottom() - 24, pinW, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0xD9, 0x77, 0x06, 235));
        painter->drawRoundedRect(pinRect, 6.0, 6.0);
        painter->setPen(Qt::white);
        painter->drawText(pinRect, Qt::AlignCenter, pinText);
    }

    // Hidden indicator (Bottom-Right if shown)
    if (isHidden) {
        const QString hidText = QStringLiteral("HIDDEN");
        const int hidW = badgeFm.horizontalAdvance(hidText) + 12;
        const QRect hidRect(coverRect.right() - hidW - 6, coverRect.bottom() - 24, hidW, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x99, 0x1B, 0x1B, 230));
        painter->drawRoundedRect(hidRect, 6.0, 6.0);
        painter->setPen(Qt::white);
        painter->drawText(hidRect, Qt::AlignCenter, hidText);
    }

    // 4. Circular Progress Ring (Top-Right of cover)
    if (showCircularProgress_ && (isRead || (currentPage > 0 && pageCount > 0))) {
        const double ratio = isRead
            ? 1.0
            : std::clamp(
                  static_cast<double>(currentPage + 1) / static_cast<double>(std::max<quint32>(1u, pageCount)),
                  0.0,
                  1.0);

        constexpr int kRingSize = 26;
        const QRect ringOuter(
            coverRect.right() - kRingSize - 5,
            coverRect.top() + 5,
            kRingSize,
            kRingSize);
        const QRect ringArcRect = ringOuter.adjusted(4, 4, -4, -4);

        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x11, 0x13, 0x17, 210));
        painter->drawEllipse(ringOuter);

        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QColor(0x45, 0x4D, 0x5C), 2.5));
        painter->drawEllipse(ringArcRect);

        const QColor arcColor = isRead ? QColor(0x22, 0xC5, 0x5E) : accentCol;
        painter->setPen(QPen(arcColor, 2.5, Qt::SolidLine, Qt::RoundCap));
        const int spanAngle = -static_cast<int>(ratio * 360.0 * 16.0);
        painter->drawArc(ringArcRect, 90 * 16, spanAngle);
    }

    // 5. Title Area (Smart Rename or Raw Title, 2-line elided)
    const QRect titleRect(
        cardRect.left() + 8,
        coverRect.bottom() + 5,
        cardRect.width() - 16,
        cardRect.bottom() - coverRect.bottom() - 9);

    QFont titleFont = option.font;
    titleFont.setBold(isSelected);
    titleFont.setPointSize(8);
    painter->setFont(titleFont);
    painter->setPen(textCol);

    const QString title = index.data(Qt::DisplayRole).toString();
    painter->drawText(
        titleRect,
        Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
        title);

    // 6. Rounded Card Border (2px on selected, 1px hairline otherwise)
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(borderCol, isSelected ? 2.0 : 1.0));
    painter->drawPath(cardPath);

    painter->restore();
}

} // namespace mavira::ui
