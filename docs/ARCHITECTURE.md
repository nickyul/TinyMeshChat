# Архитектура

## Слои

- `core`, `identity`, `protocol`, `messaging` — данные, конфигурация и wire-format;
- `network` — libdatachannel, peer connections и RTP transport worker;
- `audio` — WebRTC APM, VAD/PTT, Opus, jitter buffer и playback;
- `app` — mesh, signaling, сообщения, голосовая сессия и updater;
- `ui/bootstrap` — ViewModel, QML, app links и системный tray.

Сетевой JSON не переносит аудиокадры. Для аудио используется отдельный RTP media track с payload type 111.

## Текстовые сообщения

```text
QML → AppViewModel → NetworkSession → MessagingService → chat DataChannel
                                                    ↘ локальная строка модели
chat DataChannel → PacketCodec → MessagingService → MessagesModel → QML
```

DataChannel сохраняет порядок сообщений внутри соединения, а `messageId` обеспечивает дедупликацию. Lamport clock остаётся частью протокола, но UI является append-only: строки показываются в порядке локального наблюдения и никогда не переставляются задним числом. При конкурентной отправке полностью P2P-клиенты могут увидеть разный допустимый порядок.

## Аудиотракт

```text
capture → ring → WebRTC APM 10 мс → meter/VAD/PTT
        → Opus 20 мс → AudioTransportWorker → RTP track

RTP callback → AudioTransportWorker → RemoteAudioStream
             → jitter/FEC/PLC → mix/limiter/volume
             → playback → APM reverse stream
```

DSP worker обрабатывает capture/playback и Opus. Отдельный `AudioTransportWorker` принимает RTP callbacks и рассылает готовые Opus-кадры. GUI получает только редкие переходы состояния, meter по таймеру и двухсекундную статистику.

Потери считаются по RTP sequence number. Пустая очередь после VAD/PTT означает паузу, а не потерю. PLC/FEC применяется только при подтверждённом sequence gap, когда уже получен более новый пакет.

## Жизненный цикл окна

Иконка tray показывается только в активном mesh. Событие `Close` перехватывается до уничтожения окна, окно скрывается, а `NetworkSession`, audio workers и DataChannel продолжают работу. Вне mesh событие закрытия обрабатывается Qt обычно. Явный Quit сначала вызывает `leaveMesh()`, затем завершает event loop.

Single-instance activation и `tinymesh://` восстанавливают скрытое окно.

## Обновления

Update worker использует штатный `Velopack::GithubSource` для проверки stable GitHub Release, загрузки feed и package. Установка запускается лишь вне mesh и выполняется внешним updater-процессом после штатного завершения TinyMesh Chat. Portable ZIP только сравнивает версию через GitHub API и открывает страницу Releases.
