#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <filesystem>
#include <string>

namespace {

[[nodiscard]] std::filesystem::path getSelfDirectory() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size()) {
            buf.resize(len);
            return std::filesystem::path(buf).parent_path();
        }
        buf.resize(buf.size() * 2);
    }
}

void prependRuntimeToPath(const std::wstring& runtimeDir) {
    ::SetDllDirectoryW(runtimeDir.c_str());

    const DWORD currentLen = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring newPath = runtimeDir;
    if (currentLen > 0) {
        std::wstring currentPath(currentLen, L'\0');
        const DWORD written = ::GetEnvironmentVariableW(L"PATH", currentPath.data(), currentLen);
        if (written > 0) {
            currentPath.resize(written);
            newPath.push_back(L';');
            newPath.append(currentPath);
        }
    }
    ::SetEnvironmentVariableW(L"PATH", newPath.c_str());
}

} // namespace

int WINAPI wWinMain(
    _In_ HINSTANCE /*hInstance*/,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPWSTR lpCmdLine,
    _In_ int /*nCmdShow*/) {
    const std::filesystem::path exeDir = getSelfDirectory();
    if (exeDir.empty()) {
        ::MessageBoxW(
            nullptr,
            L"Unable to resolve MAVIRA installation directory.",
            L"MAVIRA",
            MB_ICONERROR | MB_OK);
        return 1;
    }

    const std::filesystem::path runtimeDir = exeDir / L"runtime";
    const std::filesystem::path coreExe = runtimeDir / L"MAVIRA_Core.exe";

    if (!std::filesystem::exists(coreExe)) {
        const std::wstring msg =
            L"Could not locate MAVIRA runtime binary:\n" + coreExe.native();
        ::MessageBoxW(nullptr, msg.c_str(), L"MAVIRA", MB_ICONERROR | MB_OK);
        return 1;
    }

    prependRuntimeToPath(runtimeDir.native());

    std::wstring cmdLine = L"\"" + coreExe.native() + L"\"";
    if (lpCmdLine != nullptr && *lpCmdLine != L'\0') {
        cmdLine.push_back(L' ');
        cmdLine.append(lpCmdLine);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!::CreateProcessW(
            coreExe.c_str(),
            cmdLine.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        const std::wstring msg =
            L"Failed to launch MAVIRA_Core.exe (Win32 error " +
            std::to_wstring(::GetLastError()) + L").";
        ::MessageBoxW(nullptr, msg.c_str(), L"MAVIRA", MB_ICONERROR | MB_OK);
        return 1;
    }

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return 0;
}
