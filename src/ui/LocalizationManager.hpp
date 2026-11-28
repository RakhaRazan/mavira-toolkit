#pragma once

#include <QString>

#include <string>
#include <string_view>

namespace mavira::ui {

/// Lightweight runtime localization helper supporting:
/// - "en" (English)
/// - "id" (Bahasa Indonesia)
/// - "zh" (Chinese)
/// - "ja" (Japanese)
/// - "ko" (Korean)
class LocalizationManager {
public:
    static void setLanguage(std::string_view langCode);
    [[nodiscard]] static const std::string& currentLanguage() noexcept;

    /// Returns the localized string for `key` in the active language,
    /// falling back to English, or humanizing the trailing token if unmapped.
    [[nodiscard]] static QString tr(std::string_view key);
    [[nodiscard]] static QString translate(std::string_view key) { return tr(key); }
};

} // namespace mavira::ui
