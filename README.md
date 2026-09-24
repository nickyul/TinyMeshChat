# TinyMesh Chat

[![Desktop builds](https://github.com/nickyul/TinyMeshChat/actions/workflows/builds.yml/badge.svg)](https://github.com/nickyul/TinyMeshChat/actions/workflows/builds.yml)
[![Desktop release](https://github.com/nickyul/TinyMeshChat/actions/workflows/release.yml/badge.svg)](https://github.com/nickyul/TinyMeshChat/actions/workflows/release.yml)
[![Latest release](https://img.shields.io/github/v/release/nickyul/TinyMeshChat)](https://github.com/nickyul/TinyMeshChat/releases/latest)
[![License](https://img.shields.io/github/license/nickyul/TinyMeshChat)](LICENSE)

TinyMesh Chat — настольный P2P-чат с групповыми голосовыми звонками для небольших динамических mesh-сетей. Участники устанавливают WebRTC-соединения через настраиваемый signaling-сервер или ручной обмен приглашениями. При недоступности прямого соединения поддерживается передача через TURN. Центрального сервера хранения сообщений нет.

## Возможности

- прямой текстовый чат по надёжным WebRTC DataChannel;
- группы до шести участников с автоматическим построением full mesh;
- приглашения через signaling-сервер, онлайн-статус знакомых и ручной обмен приглашениями без сервера;
- поддержка TURN с временными учётными данными от signaling-сервера;
- голос Opus 48 кГц mono, кадры RTP по 20 мс;
- WebRTC APM: AEC, high-pass filter, noise suppression и AGC;
- VAD и системный Push-to-Talk;
- адаптивный jitter buffer, Opus FEC/PLC и статистика настоящих RTP-потерь;
- mute, deafen, индивидуальная громкость и живой мониторинг микрофона;
- системный tray, сохраняющий активную mesh-сессию при закрытии окна;
- автообновление устанавливаемых сборок через Velopack и GitHub Releases.

Сообщения и roster существуют только в памяти текущей mesh-сессии. История, облачная синхронизация и запись звонков не используются.

## Поддерживаемые платформы

| Платформа | Основной пакет | Обновления |
|---|---|---|
| Windows 10/11 x64 | Velopack Setup | автоматические |
| Windows 10/11 x64 | portable ZIP | вручную через Releases |
| macOS 12+ arm64 | Velopack package | автоматические |
| Linux x64 | архив signaling-сервера | вручную |

До появления Authenticode и Apple Developer ID публичные сборки могут показывать предупреждения SmartScreen или Gatekeeper. Текущий update-канал доверяет GitHub Releases проекта и штатной проверке целостности Velopack.

## Быстрый старт

1. Скачайте пакет со страницы [Releases](https://github.com/nickyul/TinyMeshChat/releases/latest).
2. При первом запуске задайте отображаемое имя.
3. Для подключения через сервер получите ссылку доступа от допущенного пользователя или разрешение от владельца и импортируйте его в окне `Доступ к серверу`.
4. Создайте mesh и передайте приглашение другому участнику либо примите полученное приглашение. При использовании сервера обмен описаниями соединения выполняется автоматически.
5. Для голоса нажмите `Начать звонок` и разрешите доступ к микрофону.

Без сервера можно создать mesh и обменяться ручными приглашениями `tmc2:`/`.tmcinvite`: получатель передаёт ответный код пригласившему участнику.

Пока пользователь находится в mesh, закрытие главного окна скрывает приложение в tray. Полностью завершить процесс можно через пункт `Завершить TinyMesh Chat`.

## Документация

- [Сборка и packaging](docs/BUILDING.md)
- [Архитектура и потоки данных](docs/ARCHITECTURE.md)
- [Релизы и автообновление](docs/RELEASING.md)
- [Политика безопасности](SECURITY.md)
- [Участие в разработке](CONTRIBUTING.md)

## Конфигурация

Пользовательские `identity.json` и `config.json` находятся в `QStandardPaths::AppDataLocation`. Конфиг сохраняется атомарно через `QSaveFile`; старые конфиги дополняются актуальными defaults только при следующем успешном пользовательском сохранении.

## Лицензия

[MIT](LICENSE)
