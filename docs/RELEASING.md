# Релизы и автообновление

## Версия

Единственный источник версии приложения — `TMC_APP_VERSION` в корневом `CMakeLists.txt`. CMake записывает её в `tmc-app-version.txt`; package scripts передают это значение Velopack, а release workflow сверяет его с git tag.

Stable tag имеет форму `vX.Y.Z` и обязан точно совпадать с `TMC_APP_VERSION`.

## Выпуск версии

1. Обновить `TMC_APP_VERSION` в `CMakeLists.txt`.
2. Проверить Windows/macOS build workflow.
3. Создать и отправить tag `vX.Y.Z`.
4. Release workflow собирает Windows Setup, Windows portable ZIP и macOS package.
5. `vpk pack` создаёт платформенные packages и release feeds.
6. После package smoke release assets получают GitHub provenance attestations.
7. GitHub Release публикуется только после успешной сборки обеих платформ.

Приложение использует штатный `Velopack::GithubSource` и запрашивает последний stable GitHub Release. Draft и prerelease игнорируются. Windows portable ZIP самостоятельно обновления не устанавливает и открывает страницу Releases.

## Модель доверия

Update-клиент доверяет публичному GitHub repository, HTTPS и штатной проверке целостности Velopack feed/package. Отдельного подписанного TinyMesh manifest и ключей Ed25519 нет.

Компрометация GitHub repository или release credentials находится за пределами текущей модели угроз. Для небольшого закрытого круга пользователей это осознанное упрощение; перед широким публичным распространением следует добавить Authenticode и Apple Developer ID/notarization.

## Откат

Ошибочный release нужно удалить из update feed и выпустить исправленную версию с большим номером. Уже скачанный pending update отозвать нельзя, поэтому исправленную версию следует публиковать как можно быстрее. Автоматический downgrade запрещён. При повреждённой локальной установке пользователь может установить актуальный Setup вручную со страницы Releases; пользовательский config хранится отдельно от директории приложения.
