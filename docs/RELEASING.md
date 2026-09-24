# Релизы и автообновление

## Версия

Единственный источник версии приложения — `TMC_APP_VERSION` в корневом `CMakeLists.txt`. CMake записывает её в `tmc-app-version.txt`; package scripts передают это значение Velopack, а release workflow сверяет его с git tag.

Stable tag имеет форму `vX.Y.Z` и обязан точно совпадать с `TMC_APP_VERSION`.

## Выпуск версии

1. Обновить `TMC_APP_VERSION` в `CMakeLists.txt` в PR. Выбрать новый номер версии и убедиться, что соответствующий тег ещё не существует на GitHub.
2. Дождаться успешного Builds workflow для Windows, macOS и Linux-сервера. Проверить подключение, сообщения и голос на двух устройствах, включая подключение через TURN.
3. Влить PR в `main`, получить актуальный `main` и создать и отправить tag `vX.Y.Z` на релизном коммите.
4. Release workflow собирает Windows Setup, Windows portable ZIP, macOS package и архив Linux x64 signaling-сервера.
5. `vpk pack` создаёт платформенные packages и release feeds.
6. После package smoke release assets получают GitHub provenance attestations.
7. GitHub Release публикуется только после успешной сборки всех трёх заданий. Workflow сам создаёт релиз и описание изменений; заранее создавать релиз вручную не нужно.

Например, для версии `0.1.2` после слияния PR:

```bash
git switch main
git pull --ff-only origin main
git tag -a v0.1.2 -m "TinyMesh Chat v0.1.2"
git push origin v0.1.2
```

Номер тега должен точно совпадать с `TMC_APP_VERSION`. Отправка тега запускает публикацию stable-релиза. Ручной запуск Release workflow на ветке только собирает пакеты, но не публикует GitHub Release.

Публикация архива signaling-сервера не обновляет работающий сервер: его развёртывание выполняется отдельно.

Приложение использует штатный `Velopack::GithubSource` и запрашивает последний stable GitHub Release. Draft и prerelease игнорируются. Windows portable ZIP самостоятельно обновления не устанавливает и открывает страницу Releases.

## Модель доверия

Update-клиент доверяет публичному GitHub repository, HTTPS и штатной проверке целостности Velopack feed/package. Отдельного подписанного TinyMesh manifest и ключей Ed25519 нет.

Компрометация GitHub repository или release credentials находится за пределами текущей модели угроз. Для небольшого закрытого круга пользователей это осознанное упрощение; перед широким публичным распространением следует добавить Authenticode и Apple Developer ID/notarization.

## Откат

Ошибочный release нужно удалить из update feed и выпустить исправленную версию с большим номером. Уже скачанный pending update отозвать нельзя, поэтому исправленную версию следует публиковать как можно быстрее. Автоматический downgrade запрещён. При повреждённой локальной установке пользователь может установить актуальный Setup вручную со страницы Releases; пользовательский config хранится отдельно от директории приложения.
