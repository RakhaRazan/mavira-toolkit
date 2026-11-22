#include "app/ApplicationCore.hpp"
#include "app/LibraryManager.hpp"
#include "core/CommonTypes.hpp"
#include "core/cache/CacheManager.hpp"
#include "data/config/SettingsManager.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"
#include "ui/MainWindow.hpp"

#include <Windows.h>

#include <QApplication>
#include <QTimer>

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace {

constexpr wchar_t kSingleInstanceMutexName[] = L"Global\\MAVIRA_SingleInstance_Mutex";
constexpr std::size_t kMaxLogFileSizeBytes = 5 * 1024 * 1024; // 5 MB
constexpr std::size_t kMaxLogFiles = 3;
constexpr std::size_t kAsyncLogQueueSize = 8192;

class SingleInstanceGuard {
public:
    explicit SingleInstanceGuard(const wchar_t* mutexName) noexcept {
        handle_ = ::CreateMutexW(nullptr, TRUE, mutexName);
        if (handle_ != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS) {
            alreadyRunning_ = true;
            ::CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    ~SingleInstanceGuard() {
        if (handle_ != nullptr) {
            ::ReleaseMutex(handle_);
            ::CloseHandle(handle_);
        }
    }

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    [[nodiscard]] bool isAlreadyRunning() const noexcept {
        return alreadyRunning_;
    }

private:
    HANDLE handle_{nullptr};
    bool alreadyRunning_{false};
};

bool initializeAsyncLogging(const std::filesystem::path& logsDir) {
    try {
        std::error_code ec;
        std::filesystem::create_directories(logsDir, ec);
        if (ec) {
            return false;
        }

        const std::filesystem::path logFile = logsDir / L"mavira.log";

        spdlog::init_thread_pool(kAsyncLogQueueSize, 1);
        auto rotatingSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            logFile.string(), kMaxLogFileSizeBytes, kMaxLogFiles);

        auto logger = std::make_shared<spdlog::async_logger>(
            "mavira",
            std::move(rotatingSink),
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);

        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [tid %t] %v");
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::warn);

        spdlog::set_default_logger(std::move(logger));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    const auto startupBegin = std::chrono::steady_clock::now();

    SingleInstanceGuard instanceGuard(kSingleInstanceMutexName);
    if (instanceGuard.isAlreadyRunning()) {
        return 0;
    }

    const auto portablePaths = mavira::data::fs::FileManager::resolvePortablePaths();
    (void)mavira::data::fs::FileManager::ensurePortableDirectories(portablePaths);

    const bool loggingReady = initializeAsyncLogging(portablePaths.logsDir);
    if (loggingReady) {
        spdlog::info("MAVIRA starting up (portable data dir: {})", portablePaths.dataDir.string());
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("MAVIRA"));
    app.setOrganizationName(QStringLiteral("MAVIRA"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    mavira::app::ApplicationCore::ensureAppUserModelId();

    const auto shellInitBegin = std::chrono::steady_clock::now();

    mavira::data::config::SettingsManager settingsManager(portablePaths.settingsPath);
    mavira::data::db::DatabaseManager dbManager;
    (void)dbManager.open(portablePaths.databasePath);

    mavira::core::cache::CacheManager cacheManager;
    mavira::app::LibraryManager libraryManager(&dbManager, &cacheManager, portablePaths);

    mavira::ui::MainWindow mainWindow(&dbManager, &cacheManager, &libraryManager, &settingsManager);

    const auto startupEnd = std::chrono::steady_clock::now();
    const auto coldStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        startupEnd - shellInitBegin).count();

    mainWindow.show();

    if (const auto launchPath = mavira::app::ApplicationCore::extractLaunchFilePath(app.arguments())) {
        mainWindow.openComicPath(*launchPath);
    }

    if (loggingReady) {
        spdlog::info("MAVIRA MainWindow initialized (cold startup: {} ms)", coldStartMs);
    }

    // Start background directory watchers after the initial frame is shown.
    QTimer::singleShot(0, &libraryManager, &mavira::app::LibraryManager::startWatching);

    // Optional smoke-test auto-exit when MAVIRA_SMOKE_TEST_MS is set in environment.
    wchar_t smokeBuf[32]{};
    const DWORD smokeLen = ::GetEnvironmentVariableW(
        L"MAVIRA_SMOKE_TEST_MS", smokeBuf, static_cast<DWORD>(std::size(smokeBuf)));
    if (smokeLen > 0 && smokeLen < std::size(smokeBuf)) {
        const int smokeMs = std::max(50, static_cast<int>(std::wcstol(smokeBuf, nullptr, 10)));
        QTimer::singleShot(smokeMs, &app, &QCoreApplication::quit);
    }

    const int exitCode = app.exec();

    libraryManager.shutdown();
    dbManager.close();

    if (loggingReady) {
        spdlog::info("MAVIRA shutting down (exit code: {})", exitCode);
        spdlog::shutdown();
    }

    return exitCode;
}
