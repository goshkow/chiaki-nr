# Chiaki NR

Форк chiaki-ng 1.10.0 для Windows x64. Upstream: chiaki-ng/chiaki-ng, разработчик streetpea и участники; оригинальный Chiaki - thestr4ng3r. Лицензия AGPL-3.0 с сохранением лицензий компонентов.

Стек: C++/Qt Quick, FFmpeg, libplacebo/Vulkan, SDL2, MSYS2 UCRT64/CMake. Основной видеопуть - gui/src/qmlmainwindow.cpp, обработка - gui/src/nvidiavideo.cpp, панель Insert - gui/src/qml/Main.qml. NR вызывается через Vulkan feature 18 и caller shim chiaki_nvngx.dll. Закрытая пользовательская nvngx_dlssnr.dll отдельно; право её публикации не подтверждено. SR DLSS/FSR не доступны в меню. FG - собственная экспериментальная optical-flow интерполяция, не NVIDIA MFG.

Без portable.txt сохраняется совместимость с HKCU/Software/Chiaki/Chiaki. При portable.txt QSettings использует INI в config/ChiakiNR рядом с программой. Пользовательские регистрации никогда не включать в публичные файлы. Тесты используют изолированные настройки. Существующая установка вне репозитория не заменяется при подготовке публичной сборки.

Публичная сборка: scripts/package-nr.py берёт runtime только из MSYS2 и собственных сборочных DLL, проверяет зависимости PE и исключает закрытые NVIDIA runtime. Исходники SDK с лицензиями - third-party/nr-sdk. CI Windows x64 - .github/workflows/nr-portable.yml; tag v* создаёт релиз с portable и архивом соответствующих исходников. README.md русский, README.upstream.md сохраняет оригинал.

NR принимает только известный SHA256 локально проверенной DLL или runtime с проверенной Windows-подписью NVIDIA Corporation. Подпись не гарантирует совместимость, ошибка инициализации оставляет обычный стриминг. Для portable используется QT_QPA_PLATFORM=windows; offscreen не входит в публичную упаковку.

GitHub: git по SSH, коммиты goshkow <goshkow@users.noreply.github.com>. История upstream и лицензии сохраняются; новые коммиты без дополнительных подписей авторства.
