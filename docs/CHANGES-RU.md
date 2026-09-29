# Исправления по аудиту — 29 сентября 2026

База MXR-MALLOC: `8df0101875e38628bd2d3bb9a195044e8f383856`.
Исходный аудит приложен как `docs/AUDIT-BASELINE.md` и описывает **старый** код.
Это не сертификация всех комбинаций аллокатора.

## Выполнено по «Рекомендуемому порядку действий»

1. **Независимые dynamic tables (#21).** INIT/CHUNK доступны при включении только IRAM dynamic. Эффективный INIT ограничивается отдельно потолком DRAM и IRAM. Общий INIT больше не переопределяется вторым пулом.
2. **Anti-sliver и parser (#9,17).** Расширение fallback-блока ограничено глобальным nonzero MAX_BYTES и max класса, с учётом служебных байтов. Если весь gap слишком велик, выделяется исходный размер. Decimal parser проверяет overflow до умножения и отвергает такую конфигурацию.
3. **Счётчики realloc (#13).** Оба ранних счётчика отказов защищены allocator lock.
4. **Размещение и ошибки (#3,7,8).** Входная `.bss.mxriram` через `LDFRAGMENTS` попадает в SDK `.iram0.bss`, обнуляется startup и учитывается в `_iram_end`. Второй полный linker script не подключается. Memory helpers имеют core IRAM attribute и volatile 32-битные обращения. Canary/double-free пути увеличивают счётчики, без flash-строк и logger; `mxr_dump()` сообщает накопленные ошибки после unlock. Адрес конкретного нарушения больше не печатается немедленно — смотреть счётчики/отладчик.
5. **Целевые проверки (#6).** Получены ESP8266 ELF/BIN, проверены map, status-copy и отдельные helpers в машинном коде, нормальное замыкание вызовов alloc family. Добавлен отключённый по умолчанию cache-off smoke test. **Физический запуск cache-off не выполнен: платы нет.** Подробные границы проверки — VALIDATION.md.
6. **Проходы и latency (#12,14,15,16).** DRAM status собирает gaps/largest/fragmentation одним проходом по отсортированным таблицам. MINIMAL dump не собирает региональные snapshots/неиспользуемую подробную статистику. Региональные largest запросы используют raw-gap cache. IRAM primary search возвращает largest и признак точности, не требует второго scan при failed search. DRAM cross использует ту же search policy, что primary, и не помечает незавершённый largest scan точным. Payload-copy realloc выполняется вне lock; оба блока остаются allocated, старый повторно находится по адресу после reacquire. При grow/shrink копируются только живые descriptors, а не свободные slots. Время блокировки на плате не измерялось.
7. **Политика malloc.** По умолчанию сохранена SDK-compatible маска `MALLOC_CAP_32BIT`. Новая `CONFIG_MXR_DEFAULT_DRAM_ONLY=y` задаёт обычной malloc-family `8BIT|32BIT`. Явные caps и SDK heap queries не изменены. Пример validation включает DRAM-only явно, библиотека по умолчанию — нет.

Дополнительно: parsed region counts экспортируются как PUBLIC compile definitions (#11); COMPAT выводит CMake warning о конфликте со штатным heap (#10); PORT получил необходимые объявления (#27); отрицательный BIG_GAP_MIN запрещён (#19).

## Совместимость и ограничения

- Используйте компонентный CMake build; в собственном build system нужно повторить linker mapping и экспорт definitions.
- `mxr_malloc/ld/esp8266.project.ld.in` сохранён как **исторический** шаблон; не подменяйте им SDK script при использовании нового fragment.
- Диагностика/status/dump и init остаются flash-on API. Core-only не обещает cache-off для всей calloc/realloc family; для неё нужен ALLOC_FAMILY.
- SDK аварийный путь некорректного critical nesting всё ещё может вызвать flash logger. Это не исправлялось подменой SDK.
- Одновременный realloc/free одного и того же объекта недопустим; пользователь обязан синхронизировать его lifetime.
- Dynamic growth требует свободного arena tail и полного CHUNK до потолка. Частичный последний chunk не добавлен.
- Аппаратные многозадачные stress, cache-off, Wi-Fi/NMI нагрузки и p99/max lock latency остаются обязательными перед production.
