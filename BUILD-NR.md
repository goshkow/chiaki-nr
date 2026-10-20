# Сборка Chiaki NR под Windows x64

Нужны MSYS2 UCRT64, CMake/Ninja, GCC, Qt6 base/declarative/svg, FFmpeg, libplacebo >=7.349, SDL2, OpenSSL, opus, curl, protobuf/nanopb, speexdsp, shaderc и Vulkan. Точный список установки - .github/workflows/nr-portable.yml. Заголовки NVIDIA/AMD с лицензиями включены в third-party/nr-sdk; закрытые runtime не нужны для компиляции.

```sh
git clone --recursive git@github.com:goshkow/chiaki-nr.git
cd chiaki-nr
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIAKI_ENABLE_CLI=OFF -DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF -DCHIAKI_ENABLE_SETSU=OFF -DCHIAKI_ENABLE_TESTS=OFF -DCHIAKI_USE_SYSTEM_CURL=ON
cmake --build build --target chiaki -j4
python scripts/package-nr.py --prefix /ucrt64 --build build --output dist/Chiaki-NR
```

Упаковщик запускается в UCRT64; нужны windeployqt6 и objdump. Он собирает зависимости только из MSYS2, не из пользовательской установки. `scripts/source-archive.py` создаёт архив отслеживаемых исходников и submodules. Настройки/логи/закрытые DLL не входят в релиз.

Проверки:

- `QT_QPA_PLATFORM=windows chiaki.exe --portable-check` - запись настроек рядом с portable, без подключения к консоли. В публичном архиве Windows-плагин Qt; offscreen в него не входит.
- `QT_QPA_PLATFORM=windows chiaki.exe --nvidia-qml-check` - панель Insert, колесо, ограничения размеров и независимость настроек. Проводить на тестовой копии.
- `QT_QPA_PLATFORM=offscreen chiaki.exe --nvidia-self-test report.json` - GPU-тест; нужен NVIDIA RTX и совместимые runtime для внутренних SR/NR-проверок. Не запускается на обычном GitHub runner без GPU.

Локальные GPU-тесты не равны проверке живой трансляции PS5 или физического вывода 120 FPS. Изменения закрытых runtime/драйверов могут нарушить NR.
