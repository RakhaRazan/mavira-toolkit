#include "ui/SectionCardWidget.hpp"

#include <QHBoxLayout>
#include <QVBoxLayout>

namespace mavira::ui {

SectionCardWidget::SectionCardWidget(const QString& tabTitle, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("SectionCardWidget"));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* tabRow = new QHBoxLayout();
    tabRow->setContentsMargins(0, 0, 0, 0);
    tabRow->setSpacing(0);

    tabBadge_ = new QLabel(tabTitle.toUpper(), this);
    tabBadge_->setObjectName(QStringLiteral("SectionCardTabBadge"));
    tabRow->addWidget(tabBadge_);
    tabRow->addStretch(1);

    cardBody_ = new QFrame(this);
    cardBody_->setObjectName(QStringLiteral("SectionCardBody"));
    cardBody_->setFrameShape(QFrame::NoFrame);

    contentLayout_ = new QVBoxLayout(cardBody_);
    contentLayout_->setContentsMargins(16, 14, 16, 14);
    contentLayout_->setSpacing(10);

    rootLayout->addLayout(tabRow);
    rootLayout->addWidget(cardBody_);
}

void SectionCardWidget::setTabTitle(const QString& title) {
    if (tabBadge_ != nullptr) {
        tabBadge_->setText(title.toUpper());
    }
}

QString SectionCardWidget::tabTitle() const {
    return tabBadge_ != nullptr ? tabBadge_->text() : QString{};
}

} // namespace mavira::ui
