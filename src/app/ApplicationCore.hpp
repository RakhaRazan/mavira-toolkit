#pragma once

#include "core/CommonTypes.hpp"
#include "data/db/DatabaseManager.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mavira::app {

/// Represents an item added to the Windows Taskbar Jump List ("Recent Comics" category).
struct JumpListEntry {
    std::filesystem::path filePath{};
    std::string title{};
};

/// Application-layer coordinator for native Windows 11 OS integration:
/// 1. **Keep Screen Active:** Win32 `SetThreadExecutionState` management while reading.
/// 2. **Windows Taskbar Jump Lists:** Recent comics via `ICustomDestinationList`.
/// 3. **Explorer CLI Open & External App Handoff:** Command-line file launch parsing
///    and `QProcess::startDetached` companion tool invocation.
class ApplicationCore : public QObject {
    Q_OBJECT

public:
    static constexpr const wchar_t* kAppUserModelId = L"MAVIRA.Desktop.Reader";

    explicit ApplicationCore(
        mavira::data::db::DatabaseManager* db = nullptr,
        QObject* parent = nullptr);

    ~ApplicationCore() override;

    ApplicationCore(const ApplicationCore&) = delete;
    ApplicationCore& operator=(const ApplicationCore&) = delete;

    // -------------------------------------------------------------------------
    // 1. Keep Screen Active (Win32 SetThreadExecutionState)
    // -------------------------------------------------------------------------
    /// Enables or disables display/system sleep prevention via Win32 `SetThreadExecutionState`.
    void setKeepScreenActive(bool active);

    [[nodiscard]] bool isKeepScreenActive() const noexcept { return keepScreenActive_; }
    [[nodiscard]] unsigned long lastExecutionStateFlags() const noexcept { return lastExecutionState_; }

    // -------------------------------------------------------------------------
    // 2. Windows Taskbar Jump Lists (ICustomDestinationList)
    // -------------------------------------------------------------------------
    /// Sets a deterministic AppUserModelID for the process so taskbar pinning and
    /// `ICustomDestinationList` entries group under the MAVIRA taskbar icon.
    static void ensureAppUserModelId();

    /// Updates the Windows Taskbar Jump List "Recent Comics" category with `recentEntries`.
    [[nodiscard]] mavira::core::Result<std::size_t> updateJumpList(
        const std::vector<JumpListEntry>& recentEntries,
        const std::filesystem::path& exePathOverride = {});

    /// Queries SQLite for recent/in-progress comics and updates the Windows Taskbar Jump List.
    [[nodiscard]] mavira::core::Result<std::size_t> refreshJumpListFromDatabase(
        std::size_t maxItems = 10,
        bool includeHidden = false);

    [[nodiscard]] std::size_t lastJumpListCount() const noexcept { return lastJumpListCount_; }

    // -------------------------------------------------------------------------
    // 3. Explorer CLI Open & External App Handoff
    // -------------------------------------------------------------------------
    /// Extracts a comic/document file path passed via Windows Explorer or CLI arguments.
    [[nodiscard]] static std::optional<std::filesystem::path> extractLaunchFilePath(
        const QStringList& arguments);

    /// Launches a user-configured external companion tool (e.g. Vellum) via
    /// `QProcess::startDetached` with `comicPath` as its argument.
    [[nodiscard]] static mavira::core::Result<qint64> launchExternalApp(
        const std::filesystem::path& comicPath,
        const std::filesystem::path& externalAppPath);

signals:
    void keepScreenActiveChanged(bool active);
    void jumpListUpdated(int itemCount);
    void externalAppLaunched(const QString& appPath, const QString& comicPath, qint64 pid);

private:
    mavira::data::db::DatabaseManager* db_{nullptr};
    bool keepScreenActive_{false};
    unsigned long lastExecutionState_{0};
    std::size_t lastJumpListCount_{0};
};

} // namespace mavira::app
