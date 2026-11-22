#pragma once

#include <QFrame>
#include <QLabel>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

namespace mavira::ui {

/// Reusable tabbed section card with a top-left header tab badge seamlessly
/// attached to a rounded content card (`border-radius: 10px`).
class SectionCardWidget : public QWidget {
    Q_OBJECT

public:
    explicit SectionCardWidget(const QString& tabTitle, QWidget* parent = nullptr);

    void setTabTitle(const QString& title);
    [[nodiscard]] QString tabTitle() const;

    [[nodiscard]] QVBoxLayout* contentLayout() const noexcept { return contentLayout_; }
    [[nodiscard]] QFrame* cardBody() const noexcept { return cardBody_; }

private:
    QLabel* tabBadge_{nullptr};
    QFrame* cardBody_{nullptr};
    QVBoxLayout* contentLayout_{nullptr};
};

} // namespace mavira::ui
