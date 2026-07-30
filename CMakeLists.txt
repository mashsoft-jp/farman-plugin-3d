cmake_minimum_required(VERSION 3.21)
project(farman-plugin-3d VERSION 0.1.0 LANGUAGES CXX)

# farman 本体とは独立してビルドする、farman 用の外部ビュアープラグイン。
# 成果物 ModelViewerPlugin.(dylib|so|dll) を farman の外部プラグインディレクトリ
# <AppData>/plugins/viewers/ に置くと、farman が起動時にロードして .fbx を 3D
# 表示する。詳細は README.md 参照。

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ── 依存: Qt6 と Assimp ───────────────────────────────────────────
# farman と同じ Qt を指すよう、macOS の Homebrew では
#   cmake -B build -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/qt;/opt/homebrew/opt/assimp"
# のように渡す。
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

# Homebrew の assimp は keg-only なので明示的にパスを通す。
if(APPLE)
  if(EXISTS /opt/homebrew/opt/assimp)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/assimp)
  elseif(EXISTS /usr/local/opt/assimp)
    list(APPEND CMAKE_PREFIX_PATH /usr/local/opt/assimp)
  endif()
endif()
find_package(assimp CONFIG QUIET)
if(NOT assimp_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(ASSIMP REQUIRED IMPORTED_TARGET assimp)
endif()

# ── プラグイン本体 ────────────────────────────────────────────────
# farman-sdk/ は farman 本体から vendoring した ABI ヘッダ (IViewerPlugin.h)。
# ModelView は farman 本体と共有する描画実装 (Assimp + QOpenGLWidget)。
qt_add_plugin(ModelViewerPlugin
  CLASS_NAME ModelViewerPlugin
  src/ModelViewerPlugin.cpp
  src/ModelView.cpp
  farman-sdk/viewer/IViewerPlugin.cpp
)

set_target_properties(ModelViewerPlugin PROPERTIES
  PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dist"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dist"
)

target_include_directories(ModelViewerPlugin PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/farman-sdk   # "viewer/IViewerPlugin.h" を解決
)

# project() の VERSION をプラグインの version() として埋め込む。
target_compile_definitions(ModelViewerPlugin PRIVATE
  THREED_PLUGIN_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(ModelViewerPlugin PRIVATE
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::OpenGL
  Qt6::OpenGLWidgets
)
if(assimp_FOUND)
  target_link_libraries(ModelViewerPlugin PRIVATE assimp::assimp)
else()
  target_link_libraries(ModelViewerPlugin PRIVATE PkgConfig::ASSIMP)
endif()
