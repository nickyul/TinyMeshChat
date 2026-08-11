# Сборка и packaging

## Зависимости

- CMake 3.25+;
- C++20 и Visual Studio 2022 на Windows либо Xcode/Clang на macOS;
- Qt 6.5+ с Core, Gui, Network, Qml, Quick, QuickControls2 и Widgets;
- vcpkg на baseline из `vcpkg.json`;
- Ninja и NASM на macOS.

Qt и vcpkg передаются через `QT_ROOT` и `VCPKG_ROOT`. Локальные абсолютные пути храните только в игнорируемом `CMakeUserPresets.json`.

## Windows

```powershell
$env:VCPKG_ROOT = 'C:\src\vcpkg'
$env:QT_ROOT = 'C:\Qt\6.9.3\msvc2022_64'
cmake --preset windows-debug
cmake --build --preset windows-debug

cmake --preset windows-release
cmake --build --preset windows-release
```

Portable package:

```powershell
./scripts/package-windows.ps1
```

Скрипт собирает Release, выполняет `windeployqt`, проверяет runtime closure, запускает console/QML smoke и создаёт `dist/TinyMeshChat.zip`.

## macOS arm64

```bash
brew install cmake ninja nasm
cmake --preset macos-debug
cmake --build --preset macos-debug
cmake --preset macos-release
cmake --build --preset macos-release
bash scripts/package-macos.sh
```

Package script использует deployment target 12, выполняет `macdeployqt`, ad-hoc codesign, проверку arm64 и оба smoke-сценария.

## Сборка с updater

Velopack зафиксирован на версии `1.2.0`. SDK не хранится в git. Его можно заранее загрузить отдельным скриптом с проверкой SHA-256:

```powershell
./scripts/bootstrap-velopack.ps1 -Destination C:/tools/velopack
```

```bash
bash scripts/bootstrap-velopack.sh /tmp/velopack
```

Для CMake нужны:

```text
-DTMC_ENABLE_UPDATER=ON
-DTMC_VELOPACK_ROOT=<распакованный SDK>
```

Package scripts могут выполнить bootstrap автоматически, если updater включён, а `TMC_VELOPACK_ROOT` не задан. SDK попадёт в `build/tools/velopack`.

Полная локальная Windows-упаковка:

```powershell
$env:TMC_ENABLE_UPDATER = '1'
$env:TMC_BUILD_VELOPACK = '1'
./scripts/package-windows.ps1
```

На macOS используются те же переменные и `bash scripts/package-macos.sh`. Для `TMC_BUILD_VELOPACK=1` требуется .NET SDK: package script восстанавливает закреплённый локальный tool `vpk` и создаёт installer/update packages. Если оставить только `TMC_ENABLE_UPDATER=1`, будет собран и проверен runtime с Velopack, но команда `vpk pack` не запустится.

## Иконки

До финальной интеграции branding tray использует системную fallback-иконку. Ожидаемые файлы:

- Windows: `TinyMeshChat.ico` с размерами 16–256 px;
- macOS: `TinyMeshChat.icns` с размерами 16–1024 px;
- Windows tray: прозрачный RGBA PNG 32×32;
- macOS template: 18×18 и `@2x` 36×36, чёрный силуэт с прозрачностью.

## Частые проблемы

### WebRTC требует NASM

На macOS порт WebRTC/AOM не конфигурируется без NASM:

```bash
brew install nasm
```

### AOM долго собирается

vcpkg собирает Debug и Release варианты зависимостей. Холодная сборка AOM/WebRTC может долго не печатать вывод. После успешного job binary cache ускоряет следующие сборки.

### `atomic<shared_ptr>` на libc++

Transport endpoint использует mutex-защищённый snapshot `shared_ptr`, а не `std::atomic<std::shared_ptr<T>>`, поэтому сборка не зависит от различий MSVC STL и libc++ Xcode 26.
