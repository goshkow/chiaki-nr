# Chiaki NR

Форк chiaki-ng 1.10.0 для Windows x64. Upstream: chiaki-ng/chiaki-ng, разработчик streetpea и участники; оригинальный Chiaki - thestr4ng3r. Лицензия AGPL-3.0 с сохранением лицензий компонентов.

Стек: C++/Qt Quick, FFmpeg, libplacebo/Vulkan, SDL2, MSYS2 UCRT64/CMake. Основной видеопуть - gui/src/qmlmainwindow.cpp, обработка - gui/src/nvidiavideo.cpp, панель Insert - gui/src/qml/Main.qml. NR вызывается через Vulkan feature 18 и caller shim chiaki_nvngx.dll. Закрытая пользовательская nvngx_dlssnr.dll отдельно; право её публикации не подтверждено. SR DLSS/FSR не доступны в меню. FG - собственная экспериментальная optical-flow интерполяция, не NVIDIA MFG.

С v0.1.3 все обычные запуски используют исходное хранилище HKCU/Software/Chiaki/Chiaki через QSettings NativeFormat. Именованные профили и pl_render_params совместимы с оригиналом. portable.txt больше не меняет backend. Старый config v0.1.2 сохраняется, но не читается автоматически. Пользовательские регистрации не входят в публичные файлы. Тесты QML/GPU используют отдельное имя организации; portable-check только проверяет backend без записи.

Публичная сборка: scripts/package-nr.py берёт runtime только из MSYS2 и собственных сборочных DLL, проверяет зависимости PE и исключает закрытые NVIDIA runtime. Заголовки AMD с MIT-лицензией - third-party/nr-sdk. Для NGX - собственный gui/include/ngx_abi.h; NVIDIA SDK в актуальном дереве не поставляется. CI Windows x64 - .github/workflows/nr-portable.yml; tag v* создаёт релиз с portable и архивом соответствующих исходников. README.md русский, README.upstream.md сохраняет оригинал.

NR принимает только известный SHA256 локально проверенной DLL или runtime с проверенной Windows-подписью NVIDIA Corporation. Подпись не гарантирует совместимость, ошибка инициализации оставляет обычный стриминг. Для portable используется QT_QPA_PLATFORM=windows; offscreen не входит в публичную упаковку.

GitHub: git по SSH, коммиты goshkow <goshkow@users.noreply.github.com>. История upstream и лицензии сохраняются; новые коммиты без дополнительных подписей авторства.

v0.1.2 опубликован через Actions #5 (39450972907), успешная сборка подтверждена. Скачанный архив прошёл проверку сумм, запуска, portable-настроек и QML; GPU-тест CI-бинарника с отдельно добавленным проверенным runtime прошёл на RTX 3080. Публичный архив не включает закрытые DLL. Страница Actions ранее показывала устаревший результат, отключение Actions не было причиной.

Выпущен v0.1.3: Actions #7 (39477615895), успешная сборка 4м28с. Исправлена несовместимость portable с регистрациями оригинала: backend снова общий NativeFormat Chiaki/Chiaki. Опубликованный бинарник проверен на совместимое хранилище и QML; живой стрим PS5 не запускался. Старые отдельные INI не удаляются.
