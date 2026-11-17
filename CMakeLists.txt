cmake_minimum_required(VERSION 3.25)
project(FluxDM VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Sql Qml Quick QuickControls2 Test)
find_package(CURL CONFIG REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

# ---------------------------------------------------------------- core library
# Engine + persistence, no UI. Mirrors FDM's split of app core vs. download modules.
add_library(fluxcore STATIC
    src/core/Types.h
    src/core/DownloadRecord.h
    src/core/DownloadEngine.h
    src/core/Download.h               src/core/Download.cpp
    src/core/DownloadManager.h        src/core/DownloadManager.cpp
    src/core/Database.h               src/core/Database.cpp
    src/core/FileNamer.h              src/core/FileNamer.cpp
    src/core/TrafficMode.h
    src/core/Schedule.h
    src/core/BatchLinks.h
    # module "downloadswww": HTTP/HTTPS/FTP via libcurl
    src/engines/www/FileWriter.h      src/engines/www/FileWriter.cpp
    src/engines/www/HttpTask.h        src/engines/www/HttpTask.cpp
    src/engines/www/CurlWorker.h      src/engines/www/CurlWorker.cpp
    src/engines/www/WwwEngine.h       src/engines/www/WwwEngine.cpp
    # developer-only simulated module (FLUXDM_DEV_DEMO)
    src/engines/demo/DemoEngine.h     src/engines/demo/DemoEngine.cpp
    src/dev/DevDemo.h
)
target_include_directories(fluxcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fluxcore PUBLIC Qt6::Core Qt6::Network Qt6::Sql CURL::libcurl)
target_compile_definitions(fluxcore PUBLIC FLUXDM_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(fluxcore PUBLIC /utf-8 /W3 /bigobj)
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
    qml/SettingsDialog.qml
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
)

target_link_libraries(fluxdm PRIVATE fluxcore Qt6::Widgets Qt6::Quick Qt6::QuickControls2 Qt6::Qml)
target_include_directories(fluxdm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/ui) # for qmltyperegistrar
if(WIN32)
    target_link_libraries(fluxdm PRIVATE PowrProf Shell32)
endif()
set_target_properties(fluxdm PROPERTIES OUTPUT_NAME "FluxDM")

# ---------------------------------------------------------------- tests
enable_testing()
qt_add_executable(test_core tests/test_core.cpp tests/RangeServer.h)
target_link_libraries(test_core PRIVATE fluxcore Qt6::Test Qt6::Network)
add_test(NAME test_core COMMAND test_core)
set_tests_properties(test_core PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/bin"
    TIMEOUT 180)
