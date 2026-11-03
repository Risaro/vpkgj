# VPKGJ

Магазин для PS Vita: игры, порты, переводы, хоумбрю и плагины в одном приложении. Своих серверов нет. Каталог лежит в GitHub репозитории [vpkgj-db](https://github.com/Risaro/vpkgj-db), файлы скачиваются напрямую с GitHub Releases или Google Drive.

## Что умеет

- Вкладки: Игры, Порты, Переводы, Хоумбрю, Плагины, Загрузки.
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
| △ | поиск |
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
vdpm libvita2d curl-mbedtls jsoncpp minizip libpng libjpeg-turbo freetype zlib
cmake -S . -B build
cmake --build build
```

Готовый `build/VPKGJ.vpk`.

## Лицензия и благодарности

GPL-3.0. Шрифт Inter, лицензия OFL (`assets/Inter-LICENSE.txt`).

- Распаковка PKG и zRIF по устройству [pkgj](https://github.com/blastrock/pkgj).
- Генерация `head.bin` для установки vpk по образцу [VitaShell](https://github.com/TheOfficialFloW/VitaShell), VHBB и [VitaDB Downloader](https://github.com/Rinnegatamante/VitaDB-Downloader).
