# Участие в разработке

## Перед изменениями

- используйте C++20 и существующее разделение слоёв;
- не переносите аудио/RTP-кадры через GUI event loop;
- не меняйте сетевой JSON без явной миграции protocol version;
- не добавляйте приватные identity/config, ключи, build или dist в git.

## Стиль

Формат C++ задаётся `.clang-format`, общие окончания строк и отступы — `.editorconfig`. Не запускайте массовое форматирование несвязанных файлов.

## Проверка

Минимально требуются соответствующая Debug/Release сборка и `--qml-smoke`. Отдельной test infrastructure пока нет; новые test targets добавляются отдельной задачей.

Изменения packaging проверяются на GitHub-hosted Windows x64 и macOS arm64 runners. Изменения updater дополнительно требуют package smoke и проверки созданного Velopack feed.
