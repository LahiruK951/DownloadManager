cmake_minimum_required(VERSION 3.25)
project(FluxDM VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Sql Qml Quick QuickControls2 Test LinguistTools)
find_package(CURL CONFIG REQUIRED)
find_package(OpenSSL REQUIRED) # Ed25519 release signatures (also used by libtorrent)

qt_standard_project_setup(REQUIRES 6.5)

# ---------------------------------------------------------------- core library
# Engine + persistence, no UI. Mirrors FDM's split of app core vs. download modules.
add_library(fluxcore STATIC
    src/core/Types.h
    src/core/DownloadRecord.h
    src/core/DownloadEngine.h
    src/core/Download.h               src/core/Download.cpp
    src/core/DownloadManager.h        src/core/DownloadManager.cpp
    src/core/DownloadManagerQueue.cpp src/core/DownloadManagerEvents.cpp
    src/core/DownloadManagerFiles.cpp src/core/DownloadManagerSettings.cpp
    src/core/Database.h               src/core/Database.cpp
    src/core/FileNamer.h              src/core/FileNamer.cpp
    src/core/LinkCheck.h              src/core/LinkCheck.cpp
    src/core/Secret.h                 src/core/Secret.cpp
    src/core/Tag.h
    src/core/ProxySettings.h
    src/core/BrowserHost.h
    src/core/AppLog.h                 src/core/AppLog.cpp
    src/core/RemoteServer.h           src/core/RemoteServer.cpp
    src/core/Updater.h                src/core/Updater.cpp
    src/core/AttachmentCheck.h        src/core/AttachmentCheck.cpp
    src/core/FdmImport.h              src/core/FdmImport.cpp
    src/core/MeteredGuard.h           src/core/MeteredGuard.cpp
    src/core/TorrentFolderWatcher.h   src/core/TorrentFolderWatcher.cpp
    src/core/MediaConvert.h           src/core/MediaConvert.cpp
    src/core/SleepGuard.h             src/core/SleepGuard.cpp
    src/core/DiskSpace.h              src/core/DiskSpace.cpp
    src/core/InstanceServer.h         src/core/InstanceServer.cpp
    src/core/Checksum.h               src/core/Checksum.cpp
    src/core/ReleaseSignature.h       src/core/ReleaseSignature.cpp
    src/core/Metalink.h               src/core/Metalink.cpp
    src/core/TrafficMode.h
    src/core/Schedule.h
    src/core/BatchLinks.h
    src/core/Instance.h
    src/core/FileMover.h
    # module "downloadswww": HTTP/HTTPS/FTP via libcurl
    src/engines/www/FileWriter.h      src/engines/www/FileWriter.cpp
    src/engines/www/HttpTask.h        src/engines/www/HttpTask.cpp
    src/engines/www/CurlWorker.h      src/engines/www/CurlWorker.cpp
    src/engines/www/WwwEngine.h       src/engines/www/WwwEngine.cpp
    # module "downloadsm3u": HLS streams
    src/engines/hls/M3u8.h            src/engines/hls/M3u8.cpp
    src/engines/hls/HlsEngine.h       src/engines/hls/HlsEngine.cpp
    src/engines/dash/Mpd.h            src/engines/dash/Mpd.cpp
    src/engines/dash/DashEngine.h     src/engines/dash/DashEngine.cpp
    src/engines/sites/SiteEngine.h    src/engines/sites/SiteEngine.cpp
    src/core/Aes128.h                 src/core/Aes128.cpp
    # developer-only simulated module (FLUXDM_DEV_DEMO)
    src/engines/demo/DemoEngine.h     src/engines/demo/DemoEngine.cpp
    src/dev/DevDemo.h
)
target_include_directories(fluxcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# The remote-control web page (served by RemoteServer from :/remote/index.html).
qt_add_resources(fluxcore "remote_ui" PREFIX "/remote" BASE src/remote FILES src/remote/index.html)
target_link_libraries(fluxcore PUBLIC Qt6::Core Qt6::Network Qt6::Sql CURL::libcurl OpenSSL::Crypto)
# Where "Check for updates" looks (latest.json from scripts\package.ps1 -Installer); empty = not set.
set(FLUXDM_UPDATE_URL "" CACHE STRING "Update manifest URL (https://.../latest.json)")
# Public key that must have signed update manifests (from "fluxdm-sign keygen"); empty = not required yet.
set(FLUXDM_UPDATE_PUBLIC_KEY "" CACHE STRING "Ed25519 release public key (base64)")
target_compile_definitions(fluxcore PUBLIC FLUXDM_VERSION="${PROJECT_VERSION}" FLUXDM_UPDATE_URL="${FLUXDM_UPDATE_URL}"
                           FLUXDM_UPDATE_PUBLIC_KEY="${FLUXDM_UPDATE_PUBLIC_KEY}")
if(MSVC)
    target_compile_options(fluxcore PUBLIC /utf-8 /W3 /bigobj)
    target_link_libraries(fluxcore PUBLIC bcrypt crypt32 dbghelp ole32)
endif()

# module "downloadsbt": BitTorrent via libtorrent-rasterbar (optional)
find_package(LibtorrentRasterbar CONFIG QUIET)
if(LibtorrentRasterbar_FOUND)
    message(STATUS "BitTorrent support: libtorrent ${LibtorrentRasterbar_VERSION}")
    target_sources(fluxcore PRIVATE src/engines/bt/BtEngine.h src/engines/bt/BtEngine.cpp)
    target_link_libraries(fluxcore PUBLIC LibtorrentRasterbar::torrent-rasterbar)
    target_compile_definitions(fluxcore PUBLIC FLUXDM_HAS_TORRENT)
else()
    message(STATUS "BitTorrent support: disabled (libtorrent not found)")
endif()

# ---------------------------------------------------------------- application
qt_add_executable(fluxdm WIN32
    src/main.cpp
    src/ui/AppIcon.h
    src/ui/DownloadsModel.h           src/ui/DownloadsModel.cpp
    src/ui/TrayIcon.h                 src/ui/TrayIcon.cpp
)

set(FLUXDM_QML_FILES
    qml/Main.qml
    qml/Theme.qml
    qml/IconButton.qml
    qml/Chip.qml
    qml/TopBar.qml
    qml/FilterBar.qml
    qml/DownloadsList.qml
    qml/DownloadRow.qml
    qml/ProgressTrack.qml
    qml/SegmentBar.qml
    qml/BottomPanel.qml
    qml/StatusBar.qml
    qml/AddDownloadDialog.qml
    qml/SettingsPage.qml
    qml/SettingsGeneral.qml
    qml/SettingsRemote.qml
    qml/SettingsNetwork.qml
    qml/SettingsBrowser.qml
    qml/SettingsCard.qml
    qml/SettingRow.qml
    qml/FSegmented.qml
    qml/FToggle.qml
    qml/StatePill.qml
    qml/Toast.qml
    qml/TrafficPopup.qml
    qml/SpeedLimitDialog.qml
    qml/RenameDialog.qml
    qml/LoginDialog.qml
    qml/FileExistsDialog.qml
    qml/TorrentFilesDialog.qml
    qml/TagDialog.qml
    qml/ConvertDialog.qml
    qml/ReportDialog.qml
    qml/CertificateDialog.qml
    qml/MeteredDialog.qml
    qml/MirrorsDialog.qml
    qml/DownloadWindow.qml
    qml/DeleteDialog.qml
    qml/AboutDialog.qml
    qml/ScheduleEditor.qml
    qml/ScheduleDialog.qml
    qml/WhenDoneDialog.qml
    qml/FButton.qml
    qml/FTextField.qml
    qml/FTextArea.qml
    qml/FCheckBox.qml
    qml/FSwitch.qml
    qml/FComboBox.qml
    qml/FSpinBox.qml
    qml/FMenu.qml
    qml/FMenuItem.qml
    qml/FMenuSeparator.qml
    qml/FDialog.qml
    qml/FieldLabel.qml
)
# Place every QML file at the module root so they all see the module's types (App, Theme, ...).
foreach(f IN LISTS FLUXDM_QML_FILES)
    get_filename_component(alias ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${alias})
endforeach()
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(fluxdm
    URI FluxDM
    VERSION 1.0
    QML_FILES ${FLUXDM_QML_FILES}
    SOURCES
        src/ui/AppController.h src/ui/AppController.cpp
        src/ui/AppControllerSettings.cpp src/ui/AppControllerActions.cpp src/ui/AppControllerIntegration.cpp
)

# ---------------------------------------------------------------- translations
# i18n/fluxdm_<lang>.ts are updated from the sources with the "update_translations" target
# (lupdate) and compiled into the app as :/i18n/fluxdm_<lang>.qm at every build.
qt_add_translations(TARGETS fluxdm
    SOURCE_TARGETS fluxdm fluxcore
    TS_FILES i18n/fluxdm_es.ts
    RESOURCE_PREFIX "/i18n"
    LUPDATE_OPTIONS -no-obsolete -locations none)

target_link_libraries(fluxdm PRIVATE fluxcore Qt6::Widgets Qt6::Quick Qt6::QuickControls2 Qt6::Qml)
target_include_directories(fluxdm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/ui) # for qmltyperegistrar
if(WIN32)
    target_link_libraries(fluxdm PRIVATE PowrProf Shell32 Winmm)
endif()
set_target_properties(fluxdm PROPERTIES OUTPUT_NAME "FluxDM")

# ---------------------------------------------------------------- browser integration
# Native messaging host (console app; the browser talks to it over stdio).
qt_add_executable(fluxdm-nativehost src/nativehost/main.cpp src/core/Instance.h)
target_include_directories(fluxdm-nativehost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fluxdm-nativehost PRIVATE Qt6::Core Qt6::Network)
target_compile_definitions(fluxdm-nativehost PRIVATE FLUXDM_VERSION="${PROJECT_VERSION}")
# Ship the unpacked browser extension next to the app (Settings → Browser → open folder).
# Firefox uses the same scripts with its own manifest (background scripts, add-on ID).
add_custom_command(TARGET fluxdm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/browser/chromium
            $<TARGET_FILE_DIR:fluxdm>/browser/chromium
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/browser/chromium
            $<TARGET_FILE_DIR:fluxdm>/browser/firefox
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/browser/firefox/manifest.json
            $<TARGET_FILE_DIR:fluxdm>/browser/firefox/manifest.json
    COMMAND ${CMAKE_COMMAND} -E rm -f $<TARGET_FILE_DIR:fluxdm>/browser/firefox/extension-id.txt)

# Release signing tool for scripts\package.ps1 (not shipped).
qt_add_executable(fluxdm-sign src/tools/sign.cpp)
target_link_libraries(fluxdm-sign PRIVATE fluxcore)

# ---------------------------------------------------------------- tests
enable_testing()
qt_add_executable(test_core tests/test_core.cpp tests/RangeServer.h tests/HlsServer.h
    src/ui/DownloadsModel.h src/ui/DownloadsModel.cpp) # list sorting/filtering is plain Qt Core
target_link_libraries(test_core PRIVATE fluxcore Qt6::Test Qt6::Network)
add_dependencies(test_core fluxdm-nativehost)
add_test(NAME test_core COMMAND test_core)
set_tests_properties(test_core PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/bin"
    TIMEOUT 180)
