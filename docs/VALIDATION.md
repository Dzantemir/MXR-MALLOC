# Validation / воспроизведение

## Что действительно выполнено

Проверенный SDK: ESP8266_RTOS_SDK `858c7c2eb9004691f2c736c64a23715f6ea900c4`.
Компилятор: Espressif xtensa-lx106-elf GCC 8.4.0 (esp-2020r3).
Toolchain archive SHA256: `0a1804b5e2231c6db8b72af6bc2a0f9a5b6994cfba29956d412651109f13fe7e`.

| Проверка | Результат |
|---|---|
| Полная WRAP ESP8266 сборка validation, static descriptors, IRAM-BSS state, ALLOC_FAMILY, canary/DFD | ELF/BIN PASS |
| Та же сборка с включённым cache-off test | Link PASS; **не исполнялась** |
| 8 compile-only вариантов, все 4 C source файла, GCC `-O2` | PASS, `verification/compile-matrix.txt` |
| Host isolated tests из актуальных функций: copy/move/clear, fallback tail/interior limit, failed-search exact largest, early-exit, decimal overflow | PASS |
| Force-out-of-line memory helpers, GCC `-O2` | IRAM, l32i/s32i, без byte/halfword и calls, PASS |
| Status-copy в linked ELF | Цикл l32i/s32i, без libc memcpy; дизассемблирование приложено |
| Linker map: s_stats, regions, descriptor arrays | В startup-zeroed IRAM BSS, до `_iram_end` |
| Alloc-family resolved call closure | 47 функций; без flash code/literal-pool fetch в нормальном пути |
| Cache-off на плате; runtime dynamic growth/concurrency; latency | **Не выполнены** |

Compile matrix — не восемь полных linked firmware и не runtime tests. PORT/COMPAT проверены на компиляцию, но их интеграция без конфликтов с конкретным SDK требует отдельной сборки проекта. Полная связанная прошивка проверена в WRAP.

ELF checker исключает SDK `ets_printf`, достижимый через аварийную ветку unbalanced `vPortExitCritical`. Он разрешает direct calls и literal-loaded callx; это проверка заданного компилятора, а не формальное доказательство. Не проверяет произвольные pointer-derived data references, ROM internals, NMI/ISR окружение. Факт IRAM attribute сам по себе не даёт cache-off гарантию.

## Подготовка

Установить SDK указанного commit, его зависимости и рекомендованный toolchain. Инициализировать SDK submodules (для этого примера нужны cJSON, lwip, mbedtls, mqtt). Python зависимости — из SDK requirements; в проверенном окружении использованы `pyparsing==2.3.1`, `cryptography<35`, CMake `<4`, Ninja, pyelftools. Не обновлять старый SDK до произвольных несовместимых Python пакетов.

```sh
export IDF_PATH=/path/to/ESP8266_RTOS_SDK
export PATH=/path/to/xtensa-lx106-elf/bin:$PATH
cmake -S examples/validation -B /tmp/mxr-validation -G Ninja \
  -DPYTHON="$(command -v python)"
cmake --build /tmp/mxr-validation -j4
python3 tests/regressions.py
python3 tests/target_matrix.py /tmp/mxr-validation
python3 tests/check_words.py
python3 tests/check_elf.py /tmp/mxr-validation/mxr_validation.elf
```

После изменения sdkconfig.defaults удалите ранее сгенерированный `examples/validation/sdkconfig` и используйте чистую build directory, либо изменяйте конфигурацию через menuconfig. Снимок фактического проверенного sdkconfig лежит в `verification/sdkconfig-tested`.

## На плате

1. Использовать development board без ценных данных; задать flash size/mode и UART под свою плату. Сначала собрать/прошить обычный validation через SDK `idf.py flash monitor` из `examples/validation`. Предварительно проверить partition table. SDK и toolchain в ZIP не включены.
2. Обычный smoke проверяет alloc/realloc content, calloc/zalloc, status и dump. Он должен вывести `MXR on-device smoke PASS`. До реального запуска эта строка — **ожидаемый**, не полученный результат.
3. В menuconfig включить `MxR validation → Run destructive cache-off smoke test`. Требуются ALLOC_FAMILY, canary и DFD. Тест работает после init, маскирует NMI, входит в critical section, отключает cache через SDK primitives. Внутри нет logger/assert/flash-строк. Намеренно повреждает canary, восстанавливает его, освобождает и повторяет free. После восстановления cache проверить result=0 и рост обоих deferred counters. IRAM preference не гарантирует IRAM при исчерпании — отдельно проверить адрес и повторить с доступной IRAM. Этот smoke не тестирует обычные asynchronous NMI/ISR нагрузки.
4. Проверить static, IRAM-only dynamic, DRAM-only dynamic, both dynamic; для динамических DRAM таблиц выбрать descriptor/state placement DRAM. Использовать маленький INIT/CHUNK и неравные MAX. Создавать/освобождать сотни объектов вперемешку, добиваться нескольких grow/shrink; после освобождения сравнивать status/free bytes и canary counters с baseline.
5. Stress: две FreeRTOS задачи с **разными объектами**, одна realloc с checksum до/после и принудительными migrations, другая alloc/free до grow/shrink таблиц. Добавить taskYIELD между итерациями. Проверить неизменность данных первой задачи при движении таблиц второй. Не делать free/realloc общего указателя без внешней синхронизации.
6. Измерить циклы ccount/GPIO между lock/unlock (включая worst-case 256/128 descriptors), p50/p99/max; отдельно warm/cold largest-cache, MINIMAL/NORMAL/FULL dump, fragmentированный realloc. Не логировать внутри измеряемого critical section. Повторить с Wi-Fi и штатными ISR. Численных обещаний latency в этой версии нет.

## Архив

`artifacts/validation/` содержит ELF/BIN/map, bootloader и partition binary проверенной **обычной**, не destructive, конфигурации. Это evidence сборки, не универсальная готовая прошивка для любой flash layout. Для платы пересобирайте из исходников. `docs/verification` содержит результаты и snippets дизассемблирования, `docs/audit-fixes.patch` — изменения отслеживаемых исходных файлов относительно baseline (новые examples/tests/docs добавлены отдельно в ZIP).
