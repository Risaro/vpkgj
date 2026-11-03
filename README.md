# VPKGJ

Магазин для PS Vita: игры, порты, переводы, хоумбрю и плагины в одном приложении. Своих серверов нет. Каталог лежит в GitHub репозитории [vpkgj-db](https://github.com/Risaro/vpkgj-db), файлы скачиваются напрямую с GitHub Releases или Google Drive.

## Что умеет

- Вкладки: Игры, Порты, Переводы, Хоумбрю, Плагины, Загрузки.
- Игры и DLC берутся из тех же списков, что у pkgj: приложение читает `ux0:pkgj/config.txt` (`url_games`, `url_dlcs`). Обложки, скриншоты и описания к ним лежат в каталоге.
- Фоновая загрузка игр через системную очередь Vita, как в pkgj: приложение можно закрыть, игра поставится из уведомлений.
- Обновления: при запуске VPKGJ сравнивает установленное с каталогом и патчи игр со списком `url_updates` из конфига pkgj, присылает системное уведомление и даёт обновить всё одной кнопкой во вкладке Загрузки.
- Фоновая проверка (Настройки): демон `vpkgj_daemon.suprx` в `*main` раз в 6 часов проверяет обновления, даже когда приложение закрыто, и показывает уведомление на LiveArea. Работает на 3.60 и 3.65.
- Зависимости: перед установкой перевода или порта проверяется, стоят ли нужные плагины (rePatch, kubridge, fd_fix и другие). Если нет, они ставятся сами.
- Сетка карточек с обложками, карточка игры со скриншотами, описанием и размером.
- Поиск по названию, titleid и автору.
- Очередь загрузок с докачкой, прогрессом и скоростью.
- Установка:
  - игры, патчи и DLC из PKG по zRIF (распаковка на лету, как в pkgj, нужен NoNpDrm);
  - vpk без VitaShell;
  - data портов в нужную папку;
  - переводы в `ux0:rePatch/TITLEID`, плагины перевода сразу прописываются в `config.txt`;
  - плагины в `ux0:tai` или `ur0:tai` с записью в нужный раздел `config.txt`.
- Понимает обычные ссылки Google Drive, включая большие файлы со страницей подтверждения. Если Drive упёрся в лимит, качает с зеркала.
- Отмечает установленное и показывает, когда в каталоге вышла новая версия.
- Можно подключить свой каталог: Настройки, Репозиторий каталога, `user/repo`.

## Установка

Скачай `VPKGJ.vpk` из [релизов](https://github.com/Risaro/vpkgj/releases) и поставь через VitaShell. Для работы нужен HENkaku/Ensō, для PKG игр ещё NoNpDrm.

Управление:

| Кнопка | Действие |
|---|---|
| L / R | вкладки |
| ✕ | открыть, установить, запустить |
| ○ | назад |
| △ | поиск, в карточке игры загрузка в фоне |
| □ | обновить каталог, в карточке переустановить |
| START | настройки |

Работает и тачскрин: тап по вкладке или карточке, свайп по сетке и по скриншотам.

## Где что лежит на консоли

| Путь | Что там |
|---|---|
| `ux0:data/VPKGJ/config.txt` | адрес каталога и ветка |
| `ux0:data/VPKGJ/index.json` | последний скачанный каталог |
| `ux0:data/VPKGJ/installed.txt` | что и какой версии поставлено через VPKGJ |
| `ux0:data/VPKGJ/cache` | кеш картинок |
| `ux0:data/VPKGJ/dl` | незаконченные загрузки |
| `ux0:data/VPKGJ/log.txt` | лог ошибок |

Перед первой правкой `config.txt` taiHEN приложение сохраняет копию `config.txt.vpkgj.bak` рядом с ним.

## Сборка

Нужен [VitaSDK](https://vitasdk.org) и пакеты:

```
vdpm libvita2d curl-mbedtls jsoncpp minizip libpng libjpeg-turbo freetype zlib taipool libk
cmake -S . -B build
cmake --build build
```

Готовый `build/VPKGJ.vpk`.

## Лицензия и благодарности

GPL-3.0. Шрифт Inter, лицензия OFL (`assets/Inter-LICENSE.txt`).

- Распаковка PKG, zRIF и системная фоновая загрузка по устройству [pkgj](https://github.com/blastrock/pkgj) (фоновая загрузка: dots-tb и команда NPS).
- Обложки и описания игр: база [LaunchBox Games Database](https://gamesdb.launchbox-app.com).
- Уведомления на LiveArea из демона по образцу VitaDB Downloader и [SceShell-Notice-PoC](https://github.com/Princess-of-Sleeping/SceShell-Notice-PoC).
- Генерация `head.bin` для установки vpk по образцу [VitaShell](https://github.com/TheOfficialFloW/VitaShell), VHBB и [VitaDB Downloader](https://github.com/Rinnegatamante/VitaDB-Downloader).
