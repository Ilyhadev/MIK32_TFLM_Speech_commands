## Документация на MIK32 АМУР
 * [HTML - mik32-amur.ru](https://mik32-amur.ru/)
 * [PDF - nc.mikron.ru](https://nc.mikron.ru/s/keWGji8jfmsPeDo/download)


## Установка
1. Скачать и установить [VS Code](https://code.visualstudio.com/)
2. Следуя инструкции [DevContainers Installation](https://code.visualstudio.com/docs/devcontainers/containers#_installation), установить Docker и DevContainer плагин для VS Code

> :warning: Если у вас Windows, вам так же необходимо выполнить пункт [Настройка отладчика](#настройка-отладчика)

## Настройка отладчика (Windows)
> :bulb: Пользователям Linux эти шаги **выполнять не нужно**
1. Открыть терминал от имени администратора
2. Ввести команду `winget install usbipd`
3. Выполнить все инструкции на экране
4. Подключить отладчик по USB
5. Ввести команду `usbipd list`
6. Найти в списке устройство с именем "Dual RS232-HS" (или с VID:PID "0403:6010") и скопировать его "BUSID" <br>
7. Ввести команду `usbipd bind --busid=<BUSID> --force`
8. Ввести команду `usbipd attach --auto-attach --wsl --busid <BUSID>` <br>
(:warning: данную команду необходимо выполнять каждый раз после переподключения отладчика) <br>
(:warning: если отладчик все еще не доступен внутри контейнера, то нужно перезапустить vs code)

## Запуск
> :warning: Если у вас Windows, то необходимо выполнить пункт 8 из [настройки отладчика](#настройка-отладчика)
1. Открыть папку с репозиторием в VS Code
2. Нажать сочетание клавиш "Ctrl + Shift + P"
3. Ввести "Dev Containers: Open Folder in Container"
4. Нажать Enter
5. Дождаться сборки контейнера и настройки VS Code

> :warning: Выше описанные шаги необходимо выполнить только первый раз.
В следующий раз достаточно в VS Code открыть проект `mik32_base_project @ desktop-linux [Dev Container]`

## Сборка
В терминале Dev Containers ввести команду `make`
*(для удаления артефактов сборки ввести `make clean`)*

## Загрузка прошивки
> :bulb: Для того чтобы избежать проблем при загрузке прошики, всегда **подключайте отладчик** и выполняйте пункт 8 из [настройки отладчика](#настройка-отладчика) (если у вас Windows) **до запуска VS Code**.

В терминале Dev Containers ввести команду `make flash`

## Serial monitor

1. Подключить плату через USB type-c провод.
2. Выполнить проброс в wsl устройства `USB-SERIAL CH340` аналогично [Настройка отладчика (Windows)](#Настройка-отладчика-(Windows))
3. В (опционально в новом) терминале Dev Containers ввести команду `make monitor`
4. Выход из терминала возможен по последовательному нажатию сочетаний `ctrl`+`a`,затем `ctrl`+`x`.

## Отладка

* Используется функциональность VS code и плагина [cortex-debug](https://github.com/Marus/cortex-debug).
* Перед запуском отладчика нужно выполнить сборку проекта и прошивку МК командой `make build_app flash`.
* gdb-команда `load` не поддерживается, загрузка в память возможна только с помощью скрипта `mik32_upload.py`. Либо командой `make flash`
* Иногда установка плагина [cortex-debug](https://github.com/Marus/cortex-debug) выполняется с ошибкой, тогда нужно переустановить его внутри контейнера вручную через UI VS code.