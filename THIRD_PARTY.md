# Лицензии и происхождение

Форк сохраняет AGPL-3.0 и исходное исключение OpenSSL: COPYING и LICENSES/AGPL-3.0-only-OpenSSL.txt. Существующая история и авторские уведомления upstream не удаляются.

| Компонент | Происхождение | Лицензия / примечание |
| --- | --- | --- |
| Chiaki / chiaki-ng | chiaki-ng/chiaki-ng, upstream c8bea2642d0e95402f373af55584a3b98cd89417 | AGPL-3.0 с существующим исключением OpenSSL |
| NR caller shim | wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass, forwarder | GPL-3.0, LICENSE-OptiScaler-forwarder.txt; используется вместе с AGPL-приложением |
| NVIDIA NIS | NVIDIAGameWorks/NVIDIAImageScaling, 35e13ba316c98eeecf16f37eae70ce88019911f6 | LICENSE-NVIDIA-NIS.txt |
| Интерфейс NGX | собственные минимальные объявления ABI в gui/include/ngx_abi.h | без реализации/заголовков NVIDIA SDK |
| FidelityFX headers | GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, c6efa6bf7f2027b3ec94f28578bb5965eabb9e55 | MIT, third-party/nr-sdk/fidelityfx/LICENSE.txt; старые внутренние SR-тесты |
| Qt, FFmpeg, libplacebo, SDL2 и runtime | пакеты MSYS2 UCRT64 | лицензии в portable/Licenses/MSYS2, версии в BUILD-DEPENDENCIES.txt |

Закрытые NVIDIA runtime DLL и пользовательский ShortFuse runtime в публичный архив **не включаются**. Проверка интерфейса не даёт права распространять стороннюю модификацию runtime. Caller shim не является самой моделью NR.

Архив исходников рядом с каждым релизом содержит исходники форка и доступных submodules. Сборочные зависимости берутся из MSYS2: [PKGBUILD и патчи](https://github.com/msys2/MINGW-packages), [исходные архивы пакетов](https://sources.msys2.org/). Точный состав установленного окружения записывается при упаковке. Для самостоятельной пересборки зависимостей используйте соответствующую версию PKGBUILD, указанную версию пакета и патчи MSYS2. Приложение динамически связывается с LGPL-библиотеками; их DLL можно заменить совместимой собственной сборкой.
