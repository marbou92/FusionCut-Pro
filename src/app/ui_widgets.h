#pragma once

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

#include "ui_theme.h"

namespace fc {

// Reusable inline surfaces for the cross-cutting suggestions:
//   #60 error surfaces - styled inline banners with a recovery action
//   #61 empty states  - illustrated hint with the next action
// Both are Q_OBJECT-free (no signals of their own; actions arrive as
// std::function), so they are header-only and need no AUTOMOC run.

// Inline error banner (suggestion #60): a danger-tinted rounded strip
// with the message, an optional recovery action button ("Reveal
// file...", "Reconnect drive..."), and a dismiss X. Hidden until
// showError(); clear() hides it again. Panels embed it above their
// content; it never steals focus.
class ErrorBanner : public QFrame {
public:
    explicit ErrorBanner(QWidget *parent = nullptr) : QFrame(parent) {
        setObjectName(QStringLiteral("fcErrorBanner"));
        setStyleSheet(QStringLiteral("QFrame#fcErrorBanner{background:%1;border:1px solid %2;"
                                     "border-radius:4px;}")
                          .arg(ui::tint(ui::kDanger).name(), ui::color(ui::kDanger).name()));
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 6, 8, 6);
        layout->setSpacing(8);
        message_ = new QLabel(this);
        message_->setWordWrap(true);
        message_->setStyleSheet(
            QStringLiteral("color:%1;border:none;").arg(ui::color(ui::kText).name()));
        action_ = new QPushButton(this);
        action_->setFlat(true);
        action_->setCursor(Qt::PointingHandCursor);
        action_->setStyleSheet(
            QStringLiteral(
                "QPushButton{color:%1;border:1px solid %1;border-radius:3px;padding:2px 8px;}"
                "QPushButton:hover{background:%2;}")
                .arg(ui::color(ui::kDanger).name(), ui::withAlpha(ui::kDanger, 40).name()));
        dismiss_ = new QPushButton(this);
        dismiss_->setFlat(true);
        dismiss_->setCursor(Qt::PointingHandCursor);
        dismiss_->setText(QStringLiteral("\u00D7")); // multiplication sign
        dismiss_->setToolTip(tr("Dismiss"));
        dismiss_->setFixedSize(18, 18);
        dismiss_->setStyleSheet(
            QStringLiteral("QPushButton{color:%1;border:none;font-weight:bold;}")
                .arg(ui::color(ui::kTextDim).name()));
        layout->addWidget(message_, 1);
        layout->addWidget(action_);
        layout->addWidget(dismiss_);
        hide();
        connect(dismiss_, &QPushButton::clicked, this, [this] { clear(); });
        connect(action_, &QPushButton::clicked, this, [this] {
            if (handler_) {
                handler_();
            }
        });
    }

    void showError(const QString &message, const QString &actionText,
                   std::function<void()> onAction) {
        message_->setText(message);
        handler_ = std::move(onAction);
        action_->setVisible(!actionText.isEmpty());
        action_->setText(actionText);
        setToolTip(message);
        show();
    }

    void showError(const QString &message) { showError(message, QString(), {}); }

    void clear() {
        handler_ = {};
        hide();
    }

private:
    QLabel *message_ = nullptr;
    QPushButton *action_ = nullptr;
    QPushButton *dismiss_ = nullptr;
    std::function<void()> handler_;
};

// Empty-state placeholder (suggestion #61): a centered glyph + title +
// hint column for panels with nothing to show yet ("Import media to
// begin - or drop files here"). Call bindVisibility(host, isEmpty) or
// drive show()/hide() from the panel's refresh path.
class EmptyState : public QWidget {
public:
    explicit EmptyState(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(6);
        glyph_ = new QLabel(this);
        glyph_->setAlignment(Qt::AlignCenter);
        title_ = new QLabel(this);
        title_->setAlignment(Qt::AlignCenter);
        title_->setWordWrap(true);
        hint_ = new QLabel(this);
        hint_->setAlignment(Qt::AlignCenter);
        hint_->setWordWrap(true);
        layout->addStretch(1);
        layout->addWidget(glyph_);
        layout->addWidget(title_);
        layout->addWidget(hint_);
        layout->addStretch(2);
        restyle();
    }

    void setGlyph(const QString &glyph) { glyph_->setText(glyph); }

    void setTitle(const QString &title) { title_->setText(title); }

    void setHint(const QString &hint) { hint_->setText(hint); }

    // Keeps the empty state visible exactly while `condition` holds by
    // watching the host widget's resize (cheap refresh point panels
    // already have) AND every explicit refresh() call.
    void refresh(bool isEmpty) { setVisible(isEmpty); }

private:
    void restyle() {
        glyph_->setStyleSheet(
            QStringLiteral("font-size:30px;color:%1;").arg(ui::color(ui::kTextDisabled).name()));
        title_->setStyleSheet(
            QStringLiteral("font-size:13px;color:%1;").arg(ui::color(ui::kText).name()));
        hint_->setStyleSheet(
            QStringLiteral("font-size:11px;color:%1;").arg(ui::color(ui::kTextDim).name()));
    }

    QLabel *glyph_ = nullptr;
    QLabel *title_ = nullptr;
    QLabel *hint_ = nullptr;
};

} // namespace fc
