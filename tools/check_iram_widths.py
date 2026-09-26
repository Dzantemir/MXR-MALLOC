#!/usr/bin/env python3
"""
check_iram_widths.py — статический аудит правила ESP8266:
"данные, размещённые в IRAM, можно читать/писать ТОЛЬКО 32-битными словами".

Почему это нужно: ESP8266 (LX106) не умеет 8/16-битные доступы к IRAM
(0x40100000..). SDK ставит LoadStoreErrorHandler (xtensa_vectors.S), который
ЭМУЛИРУЕТ l8ui/l16ui/l16si/s8i/s16i — то есть код "работает", но каждое такое
обращение = вход в исключение (фиксированный 28-байтный стек, один слот
повтора). Любая другая инструкция к IRAM (в т.ч. невыровненная 32-битная)
уходит в _xt_ext_panic().

Что делает скрипт:
  1) находит объекты, которые реально попадут в IRAM:
       - объявленные с MXR_IRAM_DATA_ATTR;
       - объявленные с MXR_STATE_DATA_ATTR (универсальное дерево: они в IRAM
         только в режиме MXR_STATE_PLACEMENT = IRAM, т.е. в режиме B);
  2) определяет тип каждого объекта и ширину его полей — с учётом режима
     (CONFIG_MXR_COMPACT_TYPES и MXR_FIELD_* из заголовка);
  3) находит ВСЕ обращения к полям этих объектов (в т.ч. через псевдонимы
     вида `mxr_region_t *r = &s_region[0]`);
  4) помечает, какие из них в горячем пути (malloc/free/realloc).

Универсальное дерево (с MXR_STATE_PLACEMENT) проверяется в ОБОИХ режимах:
  режим A (состояние в DRAM) и режим B (состояние в IRAM, поля 32 бита).
Код возврата: 0 — чисто во всех проверенных режимах, 1 — есть находки.

Использование:
    python3 check_iram_widths.py [путь_к_компоненту]     # по умолчанию "."
"""

import re
import sys
import os
from collections import defaultdict

COMPACT = True      # CONFIG_MXR_COMPACT_TYPES (значение по умолчанию на ESP8266)

W_BASE = {          # ширина базового типа в байтах
    'bool': 1, 'uint8_t': 1, 'int8_t': 1, 'char': 1,
    'uint16_t': 2, 'int16_t': 2,
    'uint32_t': 4, 'int32_t': 4, 'size_t': 4, 'unsigned': 4, 'int': 4,
}

HOT = {  # функции горячего пути (вызов на каждый malloc/free/realloc)
    'mxr_malloc_caps_locked', 'mxr_malloc_locked', 'mxr_free_locked',
    'mxr_realloc_caps', 'mxr_calloc_caps', 'mxr_zalloc_caps',
    'mxr_dram_desc_insert', 'mxr_dram_desc_remove', 'mxr_dram_desc_shift_left',
    'mxr_dram_desc_shift_right', 'mxr_region_for_size', 'mxr_region_by_off',
    'mxr_region_caps_ok', 'mxr_region_size_ok', 'mxr_region_allocated',
    'mxr_region_released', 'mxr_region_invalidate_cache', 'mxr_region_set_cache',
    'mxr_find_free_and_largest', 'mxr_hint_try', 'mxr_bins_try',
    'mxr_iram_fb_find_free_in_region', 'mxr_iram_fb_desc_insert',
    'mxr_iram_fb_desc_remove', 'mxr_try_iram_fallback',
    'mxr_dram_cross_try', 'mxr_region_hint_after_free',
    'mxr_region_hint_after_alloc', 'mxr_region_hint_after_free_tail',
}


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def _define_table(txt):
    return {m.group(1): m.group(2)
            for m in re.finditer(r'#define\s+(MXR_FIELD_\w+)\s+(\w+)', txt)}


def field_halves(htext):
    """(#if-ветка, #else-ветка) для блока #define MXR_FIELD_BOOL...
    Возвращает (narrow_txt, wide_txt) или (None, None), если блока нет."""
    i = htext.find('#define MXR_FIELD_BOOL')
    if i < 0:
        return None, None
    start = htext.rfind('#if', 0, i)
    els = htext.find('#else', i)
    endif = htext.find('#endif', i)
    if start < 0 or els < 0 or endif < 0:
        return None, None
    # ветка #if (обычно "в IRAM -> 32 бита") и ветка #else (обычные ширины)
    return _define_table(htext[els:endif]), None


def widths(htext, state_placed_in_iram):
    """Возвращает (W, TPDEF) для указанного режима размещения состояния."""
    W = dict(W_BASE)
    tp = {'mxr_caps_t': 2 if COMPACT else 4,
          'mxr_class_t': 2 if COMPACT else 4,
          'mxr_count_t': 2 if COMPACT else 4,
          'mxr_arena_id_t': 4}

    # MXR_FIELD_*: берём ту ветку #if / #else, которая соответствует режиму
    i = htext.find('#define MXR_FIELD_BOOL')
    if i >= 0:
        start = htext.rfind('#if', 0, i)
        els = htext.find('#else', i)
        endif = htext.find('#endif', i)
        if start >= 0 and els >= 0 and endif >= 0:
            branch = htext[start:els] if state_placed_in_iram else htext[els:endif]
            for name, base in _define_table(branch).items():
                W[name] = W.get(base, 4)

    # typedef-ы size-class: при состоянии в IRAM они принудительно 32-битные
    m = re.search(r'#if defined\(CONFIG_MXR_COMPACT_TYPES\)[^\n]*\n(.*?)#else\n(.*?)#endif',
                  htext, re.S)
    if m:
        branch = m.group(2) if state_placed_in_iram else m.group(1)
        for tm in re.finditer(r'typedef\s+(\w+)\s+(mxr_\w+_t)\s*;', branch):
            tp[tm.group(2)] = W.get(tm.group(1), 4)
    return W, tp


def struct_fields(text, name, W, TPDEF):
    """Поля структуры <name> как {field: bytes}; None, если структура не найдена.
    Ищем именованные 'typedef struct { ... } NAME;' со СБАЛАНСИРОВАННЫМИ скобками,
    иначе регулярка склеивает соседние структуры."""
    body = None
    for m in re.finditer(r'typedef\s+struct\s*\{', text):
        i = m.end() - 1
        depth = 0
        while i < len(text):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        tail = text[i + 1:i + 200]
        nm = re.match(r'\s*([A-Za-z_][A-Za-z0-9_]*)\s*;', tail)
        if nm and nm.group(1) == name:
            body = text[m.end():i]
            break
    if body is None:
        return None
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)
    fields = {}
    for line in body.split(';'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        mm = re.match(r'((?:const\s+)?[A-Za-z_][A-Za-z0-9_]*)\s+\**\s*([A-Za-z_][A-Za-z0-9_]*)'
                      r'(\s*\[[^\]]*\])?$', line)
        if not mm:
            continue
        typ, fld = mm.group(1), mm.group(2)
        fields[fld] = W.get(typ, TPDEF.get(typ, 4))
    return fields


def function_of(lines, idx):
    """Имя функции, в теле которой находится строка idx.

    В этом коде сигнатура и '{' стоят на разных строках, поэтому смотрим
    на строку в колонке 0 без ';' и проверяем, что далее идёт '{'."""
    for i in range(idx, -1, -1):
        line = lines[i]
        if not line or line[0] in ' \t*/#':
            continue
        if line.rstrip().endswith(';') or '(' not in line:
            continue
        j = i + 1
        while j < len(lines) and not lines[j].strip():
            j += 1
        nxt = lines[j].lstrip() if j < len(lines) else ''
        if '{' in line or nxt.startswith('{'):
            m = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*\(', line)
            if m:
                return m.group(1)
    return '?'


def audit(stext, htext, state_placed_in_iram):
    """Один проход аудита. Возвращает (objs, structs, findings).

    state_placed_in_iram: True — моделируем режим, когда состояние лежит в IRAM
    (объекты с MXR_STATE_DATA_ATTR считаются IRAM-объектами, поля расширены);
    False — состояние в DRAM (такие объекты пропускаем, ширины обычные)."""
    W, TPDEF = widths(htext, state_placed_in_iram)

    attrs = ['MXR_IRAM_DATA_ATTR']
    if state_placed_in_iram:
        attrs.append('MXR_STATE_DATA_ATTR')
    attr_re = '|'.join(map(re.escape, attrs))

    objs = {}
    for m in re.finditer(r'^\s*static\s+([A-Za-z_][A-Za-z0-9_]*)\s*'
                         r'([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*(' + attr_re + r')\s*;',
                         stext, re.M):
        objs[m.group(2)] = m.group(1)

    structs = {}
    for t in set(objs.values()):
        f = struct_fields(htext, t, W, TPDEF)
        if f is not None:
            structs[t] = f

    alias = {}
    if objs:
        for m in re.finditer(r'([A-Za-z_][A-Za-z0-9_]*)\s*=\s*&?\s*(' +
                             '|'.join(map(re.escape, objs)) + r')\b', stext):
            alias[m.group(1)] = m.group(2)

    lines = stext.splitlines()
    findings = []
    for name, typ in objs.items():
        small = {f: w for f, w in structs.get(typ, {}).items() if w < 4}
        if not small:
            continue
        names = '|'.join([re.escape(name)] +
                         [re.escape(a) for a, o in alias.items() if o == name])
        pat = re.compile(r'\b(?:' + names + r')\s*(?:\[[^\]]*\])?\s*(?:\.|->)\s*'
                         r'([A-Za-z_][A-Za-z0-9_]*)')
        for i, line in enumerate(lines):
            if line.lstrip().startswith('*') or line.lstrip().startswith('#define'):
                continue
            for fld in pat.findall(line):
                if fld in small:
                    fn = function_of(lines, i)
                    findings.append((name, typ, fld, small[fld], i + 1, fn,
                                     fn in HOT, line.strip()))
    return objs, structs, findings


def report_mode(title, objs, structs, findings, guarded):
    print('-' * 78)
    print(f'РЕЖИМ {title}')
    print(f'  заголовок с барьером ширин: {"ДА" if guarded else "НЕТ"}')
    print('  IRAM-объекты:')
    for n, t in objs.items():
        sm = {f: w for f, w in structs.get(t, {}).items() if w < 4}
        flag = 'ОПАСНО' if sm else ('безопасно (все поля 32-бит)' if t in structs
                                    else 'скаляр/указатель')
        print(f'    {n:22} : {t:14} -> {flag}')
        if sm:
            print('        поля < 32 бит: ' + ', '.join(f'{f}({w}B)' for f, w in sm.items()))
    if not findings:
        print('  Нарушений не найдено.')
        return 0
    hot = [f for f in findings if f[6]]
    cold = [f for f in findings if not f[6]]
    print(f'  Найдено обращений к sub-32-битным полям IRAM-объектов: {len(findings)}')
    print(f'    из них в горячем пути (malloc/free/realloc): {len(hot)}')
    print(f'    в холодных путях (init/dump/status):         {len(cold)}')
    print('  --- горячий путь (каждое = LoadStoreError на устройстве) ---')
    per_func = defaultdict(list)
    for f in hot:
        per_func[f[5]].append(f)
    for fn, items in sorted(per_func.items(), key=lambda kv: -len(kv[1])):
        print(f'    {fn}  ({len(items)} обр.)')
        for _, _, fld, w, ln, _, _, code in items:
            print(f'        строка {ln:5d}  {w}B  {fld:22} {code[:66]}')
    print('  --- холодные пути ---')
    for f in cold:
        print(f'    строка {f[4]:5d}  {f[3]}B  {f[0]}.{f[2]:24} в {f[5]}')
    return 1


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else '.'
    hdr = os.path.join(root, 'include', 'mxr_malloc.h')
    src = os.path.join(root, 'mxr_malloc.c')
    if not os.path.exists(src):
        print(f'не найден {src}', file=sys.stderr)
        return 2
    htext, stext = read(hdr), read(src)

    print('=' * 78)
    print('IRAM width audit (ESP8266): доступ к IRAM только 32-битными словами')
    print('=' * 78)

    unified = 'MXR_STATE_DATA_ATTR' in htext
    guarded = 'MXR_ASSERT_WORD_FIELD' in htext

    rc = 0
    if unified:
        print('\nЗаголовок поддерживает выбор размещения состояния '
              '(MXR_STATE_PLACEMENT) — проверяются оба режима.\n')
        for title, in_iram in (('B (MXR_STATE_PLACEMENT = IRAM, поля 32 бита)', True),
                               ('A (MXR_STATE_PLACEMENT = DRAM, состояние в .bss)', False)):
            objs, structs, findings = audit(stext, htext, in_iram)
            rc |= report_mode(title, objs, structs, findings, guarded)
    else:
        in_iram = 'MXR_IRAM_PLACEMENT_ACTIVE' in htext
        objs, structs, findings = audit(stext, htext, in_iram)
        title = ('как в заголовке (состояние в IRAM)' if in_iram
                 else 'как в заголовке (IRAM-объекты с MXR_IRAM_DATA_ATTR)')
        rc = report_mode(title, objs, structs, findings, guarded)

    if rc:
        print('\nВЫВОД: sub-32-битные обращения к IRAM есть -> на ESP8266 они эмулируются')
        print('        LoadStoreErrorHandler (медленно, нереентерабельно), а не работают')
        print('        как обычная запись. Исправить: 32-битные поля ИЛИ убрать атрибут.')
    else:
        print('\nИТОГ: нарушений нет ни в одном проверенном режиме.')
    return rc


if __name__ == '__main__':
    sys.exit(main())
