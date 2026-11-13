#include "App.h"
#include "Win32IoCancellation.h"
#include "AppInternals.h"
#include "AppLayout.h"
#include "CollectionAlgorithms.h"
#include "IpcProtocol.h"
#include "LibraryQuery.h"
#include "MediaFoundation.h"
#include "OnlineSubtitles.h"
#include "Reorder.h"
#include "Updater.h"
#include "ChannelPolicy.h"
#include "Localization.h"
#include "SettingsLayout.h"
#include "Presentation.h"
#include "WindowGeometry.h"
#if defined(_WIN32)
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <imm.h>
#endif
#include <numeric>
#if !defined(_WIN32)
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Aperture {


void App::OpenFileDialog() { OpenFileDialogForTarget("", true); }


// Shared one-file picker for the Listen batch-import flows. Returns the
// chosen path, or an empty string when the dialog was cancelled. Callers own
// the systemDialogActive guard and the focus snapshot.
namespace {
std::wstring PickListenListFile(HWND owner, const wchar_t* title, const wchar_t* patterns) {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) && dialog) {
        const COMDLG_FILTERSPEC filters[] = {
            { L"List Files", patterns },
            { L"All Files", L"*.*" },
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetFileTypeIndex(1);
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR);
        dialog->SetTitle(title);
        if (dialog->Show(owner) == S_OK) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    return result;
}

std::wstring LowerFileExtension(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return std::wstring();
    std::wstring ext = path.substr(dot);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(::towlower(c));
    return ext;
}
} // namespace

// Batch podcast import: an OPML subscription export or a plain URL-per-line
// text file. Feeds are queued and fetched one at a time through the shared
// add-preview pipeline, so each subscription is validated before it lands.
void App::ImportPodcastListDialog() {
    if (systemDialogActive) return;
    if (playbackSession.IsPrivateMode()) {
        ShowOSD(IconType::VolumeMuted, L"Podcast import unavailable in private mode", 4.0f);
        return;
    }
    struct DialogGuard { bool& flag; ~DialogGuard() { flag = false; } } dialogGuard{systemDialogActive};
    systemDialogActive = true;
    PushFocusSnapshot(TransientSurfaceType::SystemDialog);
    const std::wstring path = PickListenListFile(hWnd, L"Import podcast list", L"*.opml;*.xml;*.txt");
    PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
    if (path.empty()) return;
    std::string content;
    if (!ReadTextFile(path, content).is_ok()) {
        ShowOSD(IconType::VolumeMuted, L"Could not read the list file", 4.0f);
        return;
    }
    std::vector<std::wstring> urls;
    const std::wstring ext = LowerFileExtension(path);
    if (ext == L".opml" || ext == L".xml") {
        for (const auto& entry : ParseOpmlFeeds(content)) urls.push_back(entry.url);
    } else {
        urls = ParseUrlListText(content);
    }
    // Skip feeds already subscribed (normalized comparison, like the panel).
    std::vector<std::wstring> fresh;
    const auto& subs = podcastSubscriptions.GetFeeds();
    for (const std::wstring& url : urls) {
        bool exists = false;
        for (const auto& sub : subs) {
            const std::wstring normSub = NormalizePodcastFeedUrl(sub.url);
            if (sub.url == url || (!normSub.empty() && normSub == url)) { exists = true; break; }
        }
        if (!exists) fresh.push_back(url);
    }
    if (fresh.empty()) {
        ShowOSD(IconType::Checkmark, L"No new podcast feeds in the list");
        return;
    }
    podcastImportQueue.assign(fresh.begin() + 1, fresh.end());
    podcastImportTotal = fresh.size();
    podcastJobs.EnsureAddPreview(fresh.front());
    ShowOSD(IconType::MicPodcast, L"Importing podcasts\u2026 1 of " + std::to_wstring(podcastImportTotal), 2.0f);
}

// Batch radio import: an M3U/M3U8/PLS playlist or URL-per-line text file.
// Entries are validated and merged into the favorites store in one pass.
void App::ImportRadioStationsDialog() {
    if (systemDialogActive) return;
    if (playbackSession.IsPrivateMode()) {
        ShowOSD(IconType::VolumeMuted, L"Station import unavailable in private mode", 4.0f);
        return;
    }
    struct DialogGuard { bool& flag; ~DialogGuard() { flag = false; } } dialogGuard{systemDialogActive};
    systemDialogActive = true;
    PushFocusSnapshot(TransientSurfaceType::SystemDialog);
    const std::wstring path = PickListenListFile(hWnd, L"Import station list", L"*.m3u;*.m3u8;*.pls;*.txt");
    PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
    if (path.empty()) return;
    std::string content;
    if (!ReadTextFile(path, content).is_ok()) {
        ShowOSD(IconType::VolumeMuted, L"Could not read the list file", 4.0f);
        return;
    }
    const auto parsed = ParseRadioPlaylistText(content);
    size_t added = 0, skipped = 0;
    for (const auto& station : parsed) {
        if (radioFavorites.Contains(station.url)) { ++skipped; continue; }
        if (radioFavorites.Add(station)) ++added; else ++skipped;
    }
    if (added == 0) {
        ShowOSD(IconType::VolumeMuted, L"No new stations found in the list", 4.0f);
        return;
    }
    radioFavorites.Save(GetDataDir());
    std::wstring message = L"Imported " + std::to_wstring(added) + L" stations";
    if (skipped > 0) message += L" (" + std::to_wstring(skipped) + L" skipped)";
    ShowOSD(IconType::Star, message);
}


void App::OpenFileDialogForPlaylist(const std::string& playlistId) {
    if (playlistId.empty()) return;
    Playlist* playlist = library.FindPlaylist(playlistId);
    if (!playlist || playlist->IsSmart()) return;
    OpenFileDialogForTarget(playlistId, false);
}


void App::OpenFileDialogForTarget(const std::string& targetPlaylistId, bool launchSingle) {
    // Same modal-pump guard as the watched-folder picker: no nested dialogs.
    if (systemDialogActive) return;
    struct DialogGuard { bool& flag; ~DialogGuard() { flag = false; } } dialogGuard{systemDialogActive};
    systemDialogActive = true;
    PushFocusSnapshot(TransientSurfaceType::SystemDialog);
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) || !dialog) {
        ShowOSD(IconType::VolumeMuted, L"Could not open file picker");
        PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
        return;
    }
    const COMDLG_FILTERSPEC filters[] = {
        { L"Media Files", L"*.mp4;*.mkv;*.avi;*.mov;*.webm;*.wmv;*.flv;*.m4v;*.3gp;*.ogv;*.ts;*.vob;*.mpg;*.mpeg;*.m2ts;*.divx;*.asf;*.rm;*.rmvb;*.3g2;*.f4v;*.m2v;*.mpe;*.mpv;*.m4p;*.mp3;*.wav;*.ogg;*.flac;*.aac;*.m4a;*.opus;*.wma;*.alac;*.aiff;*.pcm;*.mka" },
        { L"All Files", L"*.*" }
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR);
    dialog->SetTitle(targetPlaylistId.empty() ? L"Open media files" : L"Add media to playlist");
    const HRESULT shown = dialog->Show(hWnd);
    if (shown == S_OK) {
        IShellItemArray* results = nullptr;
        std::vector<std::wstring> selectedFiles;
        if (SUCCEEDED(dialog->GetResults(&results)) && results) {
            DWORD count = 0;
            if (SUCCEEDED(results->GetCount(&count))) {
                selectedFiles.reserve(count);
                for (DWORD i = 0; i < count; ++i) {
                    IShellItem* item = nullptr;
                    PWSTR selectedPath = nullptr;
                    if (SUCCEEDED(results->GetItemAt(i, &item)) && item &&
                        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath)) && selectedPath) {
                        selectedFiles.push_back(Ipc::ResolveFullPathLong(selectedPath));
                    }
                    if (selectedPath) CoTaskMemFree(selectedPath);
                    if (item) item->Release();
                }
            }
            results->Release();
        }
        if (!selectedFiles.empty()) { StartImportJob(selectedFiles, targetPlaylistId, launchSingle && selectedFiles.size() == 1); }
    }
    dialog->Release();
    PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
}


void App::OpenFolderDialog() {
    PushFocusSnapshot(TransientSurfaceType::SystemDialog);
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) || !dialog) {
        ShowOSD(IconType::VolumeMuted, L"Could not open folder picker");
        PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    dialog->SetTitle(L"Open a folder of media");
    const HRESULT shown = dialog->Show(hWnd);
    if (shown != S_OK) {
        dialog->Release();
        PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
        return;
    }
    IShellItem* selectedItem = nullptr;
    PWSTR selectedPath = nullptr;
    if (FAILED(dialog->GetResult(&selectedItem)) || !selectedItem ||
        FAILED(selectedItem->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath)) || !selectedPath) {
        if (selectedItem) selectedItem->Release();
        dialog->Release();
        ShowOSD(IconType::VolumeMuted, L"Could not read the selected folder");
        PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
        return;
    }
    const std::wstring folderPath = Ipc::ResolveFullPathLong(selectedPath);
    CoTaskMemFree(selectedPath);
    selectedItem->Release();
    dialog->Release();
    // ImportPaths recursively accepts only player-compatible media; every
    // other file inside the folder is ignored and reported in the summary.
    StartImportJob({ folderPath }, "", false);
    PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
}


void App::OpenSubtitleFileDialog() {
    PushFocusSnapshot(TransientSurfaceType::SystemDialog);
    std::vector<wchar_t> szFile(Ipc::MAX_IPC_PATH_CHARS, L'\0');
    OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = szFile.data();
    ofn.nMaxFile = static_cast<DWORD>(szFile.size());
    ofn.lpstrFilter = L"Subtitle Files (*.srt;*.vtt;*.ass;*.ssa;*.smi;*.sub;*.pjs)\0*.srt;*.vtt;*.ass;*.ssa;*.smi;*.sub;*.pjs\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;
    if (DelaySafeGetOpenFileNameW(&ofn)) {
        std::wstring subtitlePath = Ipc::ResolveFullPathLong(szFile.data());
        bool loaded = false;
        if (playbackSession.IsOpen()) {
            // Once mpv is active, let it own every supported external subtitle format so tracks are refreshed and styled consistently. Do
            // not clear the custom fallback until the backend accepts the file.
            loaded = player.LoadExternalSubtitle(subtitlePath);
            if (loaded) subtitles.Clear();
        } else {
            // Before playback starts the lightweight parser remains useful as the pre-player fallback subtitle source.
            loaded = subtitles.LoadSubtitleFile(subtitlePath);
        }
        if (loaded) { ShowOSD(IconType::Subtitles, L"Loaded Subtitle: " + GetFileNameFromPath(subtitlePath)); }
    }
    PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
}

// Cover art: copy the picked image under <data>\covers\<fileIndex>_<volumeSerial>.<ext>
// (identity fields the library already persists) and register it as the item's
// static-thumbnail override. Re-picks overwrite the stored file.
void App::SetCoverArtFromFile(const MediaItem& item) {
    PushFocusSnapshot(TransientSurfaceType::SystemDialog);
    std::vector<wchar_t> szFile(Ipc::MAX_IPC_PATH_CHARS, L'\0');
    OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = szFile.data();
    ofn.nMaxFile = static_cast<DWORD>(szFile.size());
    ofn.lpstrFilter = L"Image Files (*.png;*.jpg;*.jpeg;*.webp)\0*.png;*.jpg;*.jpeg;*.webp\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;
    if (DelaySafeGetOpenFileNameW(&ofn)) {
        const std::wstring sourcePath = Ipc::ResolveFullPathLong(szFile.data());
        if (MediaMetadataOverrides::NormalizeCoverArtExtension(sourcePath).empty()) {
            ShowOSD(IconType::VolumeMuted, L"Unsupported image type");
        } else {
            const std::wstring fileName =
                MediaMetadataOverrides::CoverArtFileName(item.fileIndex, item.volumeSerialNumber, sourcePath);
#if defined(_WIN32)
            const std::wstring coversDir = GetDataDir() + L"\\covers";
            SHCreateDirectoryExW(NULL, coversDir.c_str(), NULL);
            const std::wstring storedPath = coversDir + L"\\" + fileName;
#else
            const std::wstring coversDir = GetDataDir() + L"/covers";
            SHCreateDirectoryExW(NULL, coversDir.c_str(), NULL);
            const std::wstring storedPath = coversDir + L"/" + fileName;
#endif
            // CopyFileW overwrites the stored file on re-pick; the renderer
            // evicts its cached copy when the new path is registered below.
            if (fileName.empty() || !CopyFileW(sourcePath.c_str(), storedPath.c_str(), FALSE)) {
                ShowOSD(IconType::VolumeMuted, L"Could not copy cover art");
            } else {
                MediaMetadataOverrides overrides;
                if (item.overrides) overrides = *item.overrides;
                overrides.coverArtPath = storedPath;
                library.SetMetadataOverrides(item.filePath, overrides);
                SaveLibrary();
                InvalidateMediaLayoutCache();
                renderer.SetCoverArtPath(item.filePath, storedPath);
                ShowOSD(IconType::Checkmark, L"Cover Art Updated");
                InvalidateRect(hWnd, NULL, FALSE);
            }
        }
    }
    PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
}


void App::ClearCoverArt(const MediaItem& item) {
    std::wstring storedPath;
    if (item.overrides) storedPath = item.overrides->coverArtPath;
    MediaMetadataOverrides overrides;
    if (item.overrides) overrides = *item.overrides;
    overrides.coverArtPath.clear();
    library.SetMetadataOverrides(item.filePath, overrides);
    // Best-effort: the copied file is a cache; a locked file must not block the override reset.
    if (!storedPath.empty()) DeleteFileW(storedPath.c_str());
    SaveLibrary();
    InvalidateMediaLayoutCache();
    renderer.ClearCoverArtPath(item.filePath);
    ShowOSD(IconType::Checkmark, L"Cover Art Cleared");
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::SyncCoverArtThumbnails() {
    renderer.ClearAllCoverArtPaths();
    for (const auto& item : library.mediaItems) {
        if (item.overrides && !item.overrides->coverArtPath.empty()) {
            renderer.SetCoverArtPath(item.filePath, item.overrides->coverArtPath);
        }
    }
}


void App::OpenOnlineSubtitlesDialog() {
    float dp = overlayRenderer.GetDpiScale();
    RECT rc;
    GetClientRect(hWnd, &rc);
    float x = ((rc.right - rc.left) - 300.0f * dp) / 2.0f;
    float y = ((rc.bottom - rc.top) - 240.0f * dp) / 2.0f;
    OpenOnlineSubtitlesContextMenu(std::max(8.0f * dp, x), std::max(8.0f * dp, y));
}


void App::OpenInExplorer(const std::wstring& filePath) {
    if (filePath.empty()) return;
#if defined(_WIN32)
    std::wstring param = L"/select,\"" + filePath + L"\"";
    ShellExecuteW(NULL, L"open", L"explorer.exe", param.c_str(), NULL, SW_SHOWNORMAL);
#else
    const std::string utf8Path = Utf16ToUtf8(filePath);
    if (utf8Path.empty()) return;
    struct stat st{};
    const bool isDir = (::stat(utf8Path.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
    // For files, prefer org.freedesktop.FileManager1.ShowItems so the item is highlighted.
    if (!isDir) {
        static void* dbusLib = []() -> void* {
            void* lib = ::dlopen("libdbus-1.so.3", RTLD_LAZY);
            return lib ? lib : ::dlopen("libdbus-1.so", RTLD_LAZY);
        }();
        if (dbusLib) {
            using FnThreadsInit = int (*)();
            using FnBusGet = void* (*)(int, void*);
            using FnMsgNew = void* (*)(const char*, const char*, const char*, const char*);
            using FnIterInitAppend = void (*)(void*, void*);
            using FnIterOpenContainer = int (*)(void*, int, const char*, void*);
            using FnIterAppendBasic = int (*)(void*, int, const void*);
            using FnIterCloseContainer = int (*)(void*, void*);
            using FnSendReply = void* (*)(void*, void*, int, void*);
            using FnMsgUnref = void (*)(void*);
            using FnConnUnref = void (*)(void*);
            static auto pThreadsInit = reinterpret_cast<FnThreadsInit>(::dlsym(dbusLib, "dbus_threads_init_default"));
            static auto pBusGet = reinterpret_cast<FnBusGet>(::dlsym(dbusLib, "dbus_bus_get"));
            static auto pMsgNew = reinterpret_cast<FnMsgNew>(::dlsym(dbusLib, "dbus_message_new_method_call"));
            static auto pIterInit = reinterpret_cast<FnIterInitAppend>(::dlsym(dbusLib, "dbus_message_iter_init_append"));
            static auto pIterOpen = reinterpret_cast<FnIterOpenContainer>(::dlsym(dbusLib, "dbus_message_iter_open_container"));
            static auto pIterAppend = reinterpret_cast<FnIterAppendBasic>(::dlsym(dbusLib, "dbus_message_iter_append_basic"));
            static auto pIterClose = reinterpret_cast<FnIterCloseContainer>(::dlsym(dbusLib, "dbus_message_iter_close_container"));
            static auto pSendReply = reinterpret_cast<FnSendReply>(::dlsym(dbusLib, "dbus_connection_send_with_reply_and_block"));
            static auto pMsgUnref = reinterpret_cast<FnMsgUnref>(::dlsym(dbusLib, "dbus_message_unref"));
            static auto pConnUnref = reinterpret_cast<FnConnUnref>(::dlsym(dbusLib, "dbus_connection_unref"));
            bool dbusOk = false;
            if (pBusGet && pMsgNew && pIterInit && pIterOpen && pIterAppend && pIterClose && pSendReply && pMsgUnref) {
                static std::once_flag dbusThreadsOnce;
                std::call_once(dbusThreadsOnce, []() {
                    if (pThreadsInit) pThreadsInit();
                });
                std::string fileUri = "file://";
                for (unsigned char c : utf8Path) {
                    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
                        fileUri.push_back(static_cast<char>(c));
                    } else {
                        static constexpr char kHex[] = "0123456789ABCDEF";
                        fileUri.push_back('%');
                        fileUri.push_back(kHex[(c >> 4) & 0xF]);
                        fileUri.push_back(kHex[c & 0xF]);
                    }
                }
                void* conn = pBusGet(0 /* DBUS_BUS_SESSION */, nullptr);
                if (conn) {
                    void* msg = pMsgNew("org.freedesktop.FileManager1",
                                        "/org/freedesktop/FileManager1",
                                        "org.freedesktop.FileManager1",
                                        "ShowItems");
                    if (msg) {
                        struct RawIter { uintptr_t words[16]; } iter{}, subIter{};
                        pIterInit(msg, &iter);
                        if (pIterOpen(&iter, 'a', "s", &subIter)) {
                            const char* uriPtr = fileUri.c_str();
                            const char* startupId = "";
                            if (pIterAppend(&subIter, 's', &uriPtr) &&
                                pIterClose(&iter, &subIter) &&
                                pIterAppend(&iter, 's', &startupId)) {
                                void* reply = pSendReply(conn, msg, 1500, nullptr);
                                if (reply) {
                                    dbusOk = true;
                                    pMsgUnref(reply);
                                }
                            }
                        }
                        pMsgUnref(msg);
                    }
                    if (pConnUnref) pConnUnref(conn);
                }
            }
            if (dbusOk) return;
        }
    }
    std::string targetDir = utf8Path;
    if (!isDir) {
        const size_t slash = targetDir.find_last_of('/');
        targetDir = (slash == std::string::npos) ? "." : (slash == 0 ? "/" : targetDir.substr(0, slash));
    }
    // Double-fork so xdg-open runs detached without creating a zombie child.
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        const pid_t grand = ::fork();
        if (grand == 0) {
            const int devNull = ::open("/dev/null", O_RDWR);
            if (devNull >= 0) {
                ::dup2(devNull, STDIN_FILENO);
                ::dup2(devNull, STDOUT_FILENO);
                ::dup2(devNull, STDERR_FILENO);
                if (devNull > STDERR_FILENO) ::close(devNull);
            }
            char* const argv[] = {
                const_cast<char*>("xdg-open"),
                const_cast<char*>(targetDir.c_str()),
                nullptr
            };
            ::execvp("xdg-open", argv);
            ::_exit(127);
        }
        ::_exit(0);
    } else if (pid > 0) {
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
#endif
}

bool App::TakeScreenshot() {
    std::lock_guard<std::mutex> lock(screenshotMutex);
    if (shutdownComplete) return false;
    if (!playbackSession.IsOpen()) { ShowOSD(IconType::VolumeMuted, L"Screenshot unavailable: no media loaded", 2.0f); return false; }
    std::wstring mediaPath = playbackSession.GetCurrentFilePath();
    if (mediaPath.empty()) { mediaPath = player.GetCurrentFilePath(); }
    if (mediaPath.empty()) { ShowOSD(IconType::VolumeMuted, L"Screenshot unavailable: no media loaded", 2.0f); return false; }
    std::wstring root = GetExeDir();
    if (root.empty()) {
        StorageStatus err = StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, L"", "Application root directory path is empty");
        ReportStorageFailure(err, L"Screenshots");
        return false;
    }
    // 1. Ensure screenshots directory is created only on first capture
    StorageStatus dirStatus = EnsureScreenshotsDirectoryCreated(root);
    if (!dirStatus.is_ok()) { ReportStorageFailure(dirStatus, L"Screenshots"); return false; }
    // 2. Generate collision-safe deterministic screenshot file path
    // mpv infers the screenshot encoder from the file extension.
    const std::wstring extension = settings.screenshotFormat == "jpg" ? L".jpg" : L".png";
    std::wstring targetPath = GenerateScreenshotFilePath(mediaPath, root, nullptr, nullptr, extension);
    if (targetPath.empty() || !IsScreenshotPathSafe(targetPath, root)) {
        AppError err = AppError::Storage( static_cast<int>(StorageErrorCode::InvalidPath), ERROR_ACCESS_DENIED,
            L"Save failed: Invalid or unsafe screenshot path.", L"Path escape or invalid screenshot path detected: " + targetPath,
            targetPath);
        ReportAppError(err);
        return false;
    }
    // 3. Delegate decoded video frame capture to player
    StorageStatus captureStatus = player.TakeScreenshot(targetPath);
    if (!captureStatus.is_ok()) { ReportStorageFailure(captureStatus, L"Screenshots"); return false; }
    // 4. Update last screenshot path and project root-relative path to user
    lastScreenshotPath = targetPath;
    std::wstring relPath = ToRelocatablePath(targetPath, root);
    ShowOSD(IconType::Checkmark, L"Screenshot saved: " + relPath, 3.0f);
    RequestRender();
    return true;
}

void App::PruneOrphanedHubThumbnailCache() {
    std::vector<std::wstring> livePaths;
    livePaths.reserve(library.mediaItems.size());
    for (const auto& item : library.mediaItems) { livePaths.push_back(item.filePath); }
    PruneOrphanedHubThumbnailDiskCache(livePaths);
}


void App::RemoveSelectedFolders() {
    if (selectedFolderKeys.empty()) return;
    ArmLibraryUndo(L"Folder removal");
    if (libraryGroupMode == LibraryGroupMode::Series) {
        const auto& proj = GetCurrentMediaProjection(true);
        std::vector<std::string> idsToRemove;
        size_t seriesCount = 0;
        for (const auto& g : proj.groups) {
            if (selectedFolderKeys.count(g.key) > 0) {
                seriesCount++;
                for (size_t i = 0; i < g.count; ++i) {
                    size_t pIdx = g.startIndex + i;
                    if (pIdx < proj.indices.size()) { idsToRemove.push_back(library.mediaItems[proj.indices[pIdx]].id); }
                }
            }
        }
        size_t removed = 0;
        if (!idsToRemove.empty()) { removed = library.RemoveMedia(idsToRemove, nullptr, true, /*keepPlaylistEntries=*/true); }
        selectedFolderKeys.clear();
        m_mediaQueryEngine.InvalidateCache();
        InvalidateMediaLayoutCache();
        ClampScrollOffsets();
        InvalidateRect(hWnd, NULL, FALSE);
        std::wstring msg = L"Removed " + std::to_wstring(seriesCount) + L" series (" + std::to_wstring(removed) + L" episodes) • Ctrl+Z to undo";
        ShowOSD(IconType::Trash, msg);
        AnnounceStatus(msg);
        return;
    }
    std::vector<std::wstring> foldersToRemove(selectedFolderKeys.begin(), selectedFolderKeys.end());
    // Media-only removal: playlists keep their entries (restore via the
    // background menu or Ctrl+Z).
    size_t removed = library.RemoveFolders(foldersToRemove, true, /*keepPlaylistEntries=*/true);
    selectedFolderKeys.clear();
    m_mediaQueryEngine.InvalidateCache();
    InvalidateMediaLayoutCache();
    ClampScrollOffsets();
    InvalidateRect(hWnd, NULL, FALSE);
    std::wstring msg = L"Removed " + std::to_wstring(foldersToRemove.size()) + L" folder(s) (" + std::to_wstring(removed) + L" videos) • Ctrl+Z to undo";
    ShowOSD(IconType::Trash, msg);
    AnnounceStatus(msg);
}

// Re-imports playlist entries that no longer have a library record (e.g.
// removed from the Media tab while playlists kept them).
void App::RestorePlaylistItemsToLibrary() {
    std::vector<std::wstring> missing;
    std::set<std::wstring> seen;
    for (const auto& pl : library.playlists) {
        for (const auto& path : pl.itemPaths) {
            if (path.empty()) continue;
            std::wstring key = NormalizePathKey(path);
            if (!seen.insert(key).second) continue;
            if (!library.FindByPath(path)) missing.push_back(path);
        }
    }
    if (missing.empty()) { ShowOSD(IconType::Checkmark, L"All playlist items are already in the library"); return; }
    StartImportJob(missing, "", false);
    ShowOSD(IconType::Refresh, L"Restoring " + std::to_wstring(missing.size()) + L" playlist item(s)");
}


void App::RunFolderRediscovery(bool showFeedback) {
    if (!libraryReady || shutdownComplete || playerCallbacksDisabled.load(std::memory_order_acquire)) return;
    folderRediscoveryFeedback |= showFeedback;
    if (folderRediscoveryActive) { folderRediscoveryAgain = true; return; }
    try {
        auto snapshot = FolderRediscoverySnapshot::Capture(library);
        folderRediscoveryActive = folderRediscoveryJob.Start([snapshot = std::move(snapshot)](const auto& cancellation) {
            Win32IoCancellation interrupt(cancellation);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            return PrepareFolderRediscovery(snapshot, cancellation);
        }) != 0;
        if (folderRediscoveryActive && showFeedback) ShowOSD(IconType::Refresh, L"Looking for moved folders");
    } catch (...) {
        folderRediscoveryActive = false;
        folderRediscoveryFeedback = false;
        ReportStorageFailure(StorageStatus::Error(StorageErrorCode::FileReadFailed, ERROR_NOT_ENOUGH_MEMORY,
            L"", "Could not prepare folder rediscovery"), L"folder rediscovery");
    }
}


void App::FinishFolderRediscovery() {
    if (!folderRediscoveryActive || shutdownComplete || playerCallbacksDisabled.load(std::memory_order_acquire)) return;
    auto completion = folderRediscoveryJob.TakeResult();
    if (!completion) return;
    folderRediscoveryActive = false;
    if (!completion->result) {
        folderRediscoveryFeedback = folderRediscoveryAgain = false;
        ReportStorageFailure(StorageStatus::Error(StorageErrorCode::FileReadFailed, ERROR_READ_FAULT,
            L"", "Folder rediscovery worker failed"), L"folder rediscovery");
        return;
    }
    const auto previousWatches = library.watchedFolders;
    const auto res = library.ApplyFolderRediscovery(*completion->result, true);
    if (!res.status.is_ok()) ReportStorageFailure(res.status, L"folder rediscovery");
    if (res.renamedFoldersCount > 0 || res.mergedItemsCount > 0 || res.relocatedItemsCount > 0 || res.removedGhostCount > 0) {
        folderWatcher.SetWatchedFoldersAsync(library.GetWatchedFolders());
        for (const auto& rename : res.renamedFolders) {
            const auto oldKey = NormalizePathKey(rename.first);
            if (std::any_of(previousWatches.begin(), previousWatches.end(), [&](const auto& root) { return NormalizePathKey(root) == oldKey; })) {
                watchReconcileWorker.Queue({true, rename.second, {}});
            }
        }
        m_mediaQueryEngine.InvalidateCache();
        InvalidateMediaLayoutCache();
        ClampScrollOffsets();
        InvalidateRect(hWnd, NULL, FALSE);
        ScheduleMetadataBackfill();
        std::wstring msg = L"Rediscovered " + std::to_wstring(res.renamedFoldersCount) + L" renamed folder(s)";
        if (res.mergedItemsCount > 0) { msg += L" (" + std::to_wstring(res.mergedItemsCount) + L" items reconciled)"; }
        ShowOSD(IconType::Checkmark, msg);
        AnnounceStatus(msg);
    } else if (folderRediscoveryFeedback && res.status.is_ok() && !res.needsRescan && !folderRediscoveryAgain) {
        ShowOSD(IconType::Checkmark, L"No folder changes found");
    }
    const bool again = folderRediscoveryAgain || res.needsRescan;
    folderRediscoveryAgain = false;
    if (again) RunFolderRediscovery(false);
    else folderRediscoveryFeedback = false;
}

} // namespace Aperture
