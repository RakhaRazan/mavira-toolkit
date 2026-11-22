#include "app/ApplicationCore.hpp"

#include "data/fs/FileManager.hpp"

#include <Windows.h>
#include <Objbase.h>
#include <Propkey.h>
#include <Propvarutil.h>
#include <ShObjIdl.h>
#include <ShlObj.h>

#include <QFileInfo>
#include <QProcess>
#include <QStringList>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace mavira::app {

namespace {

using mavira::core::ErrorCode;
using mavira::core::Result;
using mavira::data::fs::FileManager;

template <typename T>
struct ComPtr {
    T* ptr{nullptr};

    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    void reset() noexcept {
        if (ptr != nullptr) {
            ptr->Release();
            ptr = nullptr;
        }
    }

    [[nodiscard]] T** put() noexcept {
        reset();
        return &ptr;
    }

    [[nodiscard]] T* get() const noexcept { return ptr; }
    [[nodiscard]] T* operator->() const noexcept { return ptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr != nullptr; }
};

struct ComInitGuard {
    bool shouldUninit{false};
    HRESULT hr{E_FAIL};

    ComInitGuard() noexcept {
        hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (hr == S_OK || hr == S_FALSE) {
            shouldUninit = true;
        }
    }

    ~ComInitGuard() {
        if (shouldUninit) {
            ::CoUninitialize();
        }
    }

    [[nodiscard]] bool ok() const noexcept {
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
};

[[nodiscard]] std::filesystem::path currentExecutablePath() {
    std::array<wchar_t, 32768> buf{};
    const DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) {
        return {};
    }
    return std::filesystem::path(std::wstring(buf.data(), len));
}

[[nodiscard]] std::wstring quoteForShellArg(const std::filesystem::path& path) {
    return L"\"" + path.native() + L"\"";
}

[[nodiscard]] HRESULT createShellLinkForComic(
    const std::filesystem::path& exePath,
    const JumpListEntry& entry,
    IShellLinkW** outLink) {
    ComPtr<IShellLinkW> link;
    HRESULT hr = ::CoCreateInstance(
        CLSID_ShellLink,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(link.put()));
    if (FAILED(hr)) {
        return hr;
    }

    const std::wstring exeNative = exePath.native();
    const std::wstring args = quoteForShellArg(entry.filePath);
    std::wstring titleWide = FileManager::utf8ToWide(entry.title);
    if (titleWide.empty()) {
        titleWide = entry.filePath.stem().native();
    }
    if (titleWide.empty()) {
        titleWide = entry.filePath.filename().native();
    }

    hr = link->SetPath(exeNative.c_str());
    if (FAILED(hr)) {
        return hr;
    }
    hr = link->SetArguments(args.c_str());
    if (FAILED(hr)) {
        return hr;
    }
    (void)link->SetIconLocation(exeNative.c_str(), 0);
    (void)link->SetDescription(titleWide.c_str());

    ComPtr<IPropertyStore> propStore;
    hr = link->QueryInterface(IID_PPV_ARGS(propStore.put()));
    if (SUCCEEDED(hr) && propStore) {
        PROPVARIANT pv;
        ::PropVariantInit(&pv);
        hr = ::InitPropVariantFromString(titleWide.c_str(), &pv);
        if (SUCCEEDED(hr)) {
            (void)propStore->SetValue(PKEY_Title, pv);
            (void)propStore->Commit();
            ::PropVariantClear(&pv);
        }
    }

    *outLink = link.ptr;
    link.ptr = nullptr;
    return S_OK;
}

} // namespace

ApplicationCore::ApplicationCore(
    mavira::data::db::DatabaseManager* db,
    QObject* parent)
    : QObject(parent),
      db_(db) {}

ApplicationCore::~ApplicationCore() {
    if (keepScreenActive_) {
        setKeepScreenActive(false);
    }
}

void ApplicationCore::ensureAppUserModelId() {
    (void)::SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
}

void ApplicationCore::setKeepScreenActive(bool active) {
    const EXECUTION_STATE desired = active
        ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)
        : ES_CONTINUOUS;

    (void)::SetThreadExecutionState(desired);
    lastExecutionState_ = static_cast<unsigned long>(desired);

    if (keepScreenActive_ != active) {
        keepScreenActive_ = active;
        emit keepScreenActiveChanged(keepScreenActive_);
    }
}

Result<std::size_t> ApplicationCore::updateJumpList(
    const std::vector<JumpListEntry>& recentEntries,
    const std::filesystem::path& exePathOverride) {
    const std::filesystem::path exePath = exePathOverride.empty()
        ? currentExecutablePath()
        : exePathOverride;
    if (exePath.empty()) {
        return Result<std::size_t>::err(
            ErrorCode::IoError, "Failed to resolve MAVIRA executable path for Jump List");
    }

    ComInitGuard com;
    if (!com.ok()) {
        return Result<std::size_t>::err(
            ErrorCode::IoError, "COM initialization failed for Windows Jump List");
    }

    ComPtr<ICustomDestinationList> destList;
    HRESULT hr = ::CoCreateInstance(
        CLSID_DestinationList,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(destList.put()));
    if (FAILED(hr) || !destList) {
        return Result<std::size_t>::err(
            ErrorCode::IoError, "Failed to create ICustomDestinationList");
    }

    (void)destList->SetAppID(kAppUserModelId);

    UINT minSlots = 0;
    ComPtr<IObjectArray> removedItems;
    hr = destList->BeginList(&minSlots, IID_PPV_ARGS(removedItems.put()));
    if (FAILED(hr)) {
        return Result<std::size_t>::err(
            ErrorCode::IoError, "ICustomDestinationList::BeginList failed");
    }

    const std::size_t maxItems = std::min<std::size_t>(
        recentEntries.size(),
        minSlots > 0 ? static_cast<std::size_t>(minSlots) : 10u);

    std::size_t addedCount = 0;
    if (maxItems > 0) {
        ComPtr<IObjectCollection> collection;
        hr = ::CoCreateInstance(
            CLSID_EnumerableObjectCollection,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(collection.put()));
        if (FAILED(hr) || !collection) {
            (void)destList->AbortList();
            return Result<std::size_t>::err(
                ErrorCode::IoError, "Failed to create Jump List IObjectCollection");
        }

        for (std::size_t i = 0; i < maxItems; ++i) {
            if (recentEntries[i].filePath.empty()) {
                continue;
            }
            ComPtr<IShellLinkW> link;
            if (SUCCEEDED(createShellLinkForComic(exePath, recentEntries[i], link.put())) && link) {
                if (SUCCEEDED(collection->AddObject(link.get()))) {
                    ++addedCount;
                }
            }
        }

        if (addedCount > 0) {
            ComPtr<IObjectArray> objectArray;
            hr = collection->QueryInterface(IID_PPV_ARGS(objectArray.put()));
            if (SUCCEEDED(hr) && objectArray) {
                hr = destList->AppendCategory(L"Recent Comics", objectArray.get());
                if (FAILED(hr)) {
                    // Portable apps without HKCU/HKLM ProgID file associations cannot use
                    // AppendCategory (DESTS_E_NO_MATCHING_ASSOC_HANDLER); fall back to AddUserTasks.
                    hr = destList->AddUserTasks(objectArray.get());
                }
                if (FAILED(hr)) {
                    (void)destList->AbortList();
                    return Result<std::size_t>::err(
                        ErrorCode::IoError,
                        "ICustomDestinationList failed to attach Jump List items (HRESULT 0x" +
                            QString::number(static_cast<quint32>(hr), 16).toStdString() + ")");
                }
            }
        }
    }

    hr = destList->CommitList();
    if (FAILED(hr)) {
        (void)destList->AbortList();
        return Result<std::size_t>::err(
            ErrorCode::IoError, "ICustomDestinationList::CommitList failed");
    }

    lastJumpListCount_ = addedCount;
    emit jumpListUpdated(static_cast<int>(addedCount));
    return Result<std::size_t>::ok(addedCount);
}

Result<std::size_t> ApplicationCore::refreshJumpListFromDatabase(
    std::size_t maxItems,
    bool includeHidden) {
    if (db_ == nullptr || !db_->isOpen()) {
        return Result<std::size_t>::err(
            ErrorCode::DatabaseError, "Database is not open for Jump List refresh");
    }

    std::vector<JumpListEntry> entries;
    if (auto crRes = db_->getContinueReading(maxItems, includeHidden); crRes) {
        for (const auto& c : crRes.value()) {
            if (!c.filePath.empty()) {
                entries.push_back(JumpListEntry{
                    c.filePath,
                    c.title.empty()
                        ? FileManager::wideToUtf8(c.filePath.stem().native())
                        : c.title
                });
            }
        }
    }

    return updateJumpList(entries);
}

std::optional<std::filesystem::path> ApplicationCore::extractLaunchFilePath(
    const QStringList& arguments) {
    // Skip argv[0] (the executable path).
    for (int i = 1; i < arguments.size(); ++i) {
        QString arg = arguments.at(i).trimmed();
        if (arg.isEmpty()) {
            continue;
        }
        if (arg.startsWith(QLatin1Char('-')) || arg.startsWith(QLatin1Char('/'))) {
            continue;
        }
        if (arg.size() >= 2 && arg.startsWith(QLatin1Char('"')) && arg.endsWith(QLatin1Char('"'))) {
            arg = arg.mid(1, arg.size() - 2);
        }
        if (!arg.isEmpty()) {
            return std::filesystem::path(arg.toStdWString());
        }
    }
    return std::nullopt;
}

Result<qint64> ApplicationCore::launchExternalApp(
    const std::filesystem::path& comicPath,
    const std::filesystem::path& externalAppPath) {
    if (comicPath.empty()) {
        return Result<qint64>::err(ErrorCode::InvalidArgument, "Comic path is empty");
    }
    if (externalAppPath.empty()) {
        return Result<qint64>::err(
            ErrorCode::InvalidArgument, "External companion app path is not configured");
    }

    const QString appQStr = QString::fromStdWString(externalAppPath.native());
    const QString comicQStr = QString::fromStdWString(comicPath.native());
    const QString workingDir = QFileInfo(comicQStr).absolutePath();

    qint64 pid = 0;
    const bool started = QProcess::startDetached(appQStr, {comicQStr}, workingDir, &pid);
    if (!started) {
        return Result<qint64>::err(
            ErrorCode::IoError,
            "Failed to launch external app: " + FileManager::wideToUtf8(externalAppPath.native()));
    }

    return Result<qint64>::ok(pid);
}

} // namespace mavira::app
