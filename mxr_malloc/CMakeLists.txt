# ============================================================
#  1. Select source files based on integration mode
# ============================================================
# mxr_malloc.c is ALWAYS compiled (the core allocator).
# The integration layer files are mutually exclusive.

set(MXR_SRCS "mxr_malloc.c")

if(CONFIG_MXR_INTEGRATION_WRAP)
    # Wrap mode: original heap stays, linker redirects calls
    list(APPEND MXR_SRCS "mxr_heap_wrap.c")
elseif(CONFIG_MXR_INTEGRATION_COMPAT)
    # Compat mode: we REPLACE the original heap component
    list(APPEND MXR_SRCS "mxr_heap_compat.c")
elseif(CONFIG_MXR_INTEGRATION_PORT)
    # Port mode: we provide both heap_caps_* AND standard libc malloc/free
    list(APPEND MXR_SRCS "mxr_heap_compat.c" "mxr_heap_port.c")
endif()

idf_component_register(
    SRCS ${MXR_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        log
        newlib
        freertos
        esp8266
)

# ============================================================
#  2. Compatibility warning
# ============================================================
if(CONFIG_MXR_WARN_HEAP_TRACING AND CONFIG_HEAP_TRACING AND CONFIG_MXR_INTEGRATION_WRAP)
    message(WARNING
        "MxR-malloc: CONFIG_HEAP_TRACING is enabled. "
        "Original heap tracing is not compatible with MxR wrap mode. "
        "Disable CONFIG_HEAP_TRACING or disable this warning."
    )
endif()

# ============================================================
#  3. Validate MXR_REGION_CONFIG at configure time
# ============================================================
set(_MXR_CFG "${CONFIG_MXR_REGION_CONFIG}")
if(_MXR_CFG)
    string(REPLACE "," ";" _MXR_ENTRIES "${_MXR_CFG}")
    list(LENGTH _MXR_ENTRIES _MXR_COUNT)
    foreach(_entry ${_MXR_ENTRIES})
        string(STRIP "${_entry}" _entry)
        string(REGEX MATCH "^[0-9]+-[0-9]+%$" _VALID "${_entry}")
        if(NOT _VALID)
            message(FATAL_ERROR
                "MxR-malloc: Invalid region entry '${_entry}' in "
                "CONFIG_MXR_REGION_CONFIG='${CONFIG_MXR_REGION_CONFIG}'. "
                "Expected format: <bytes>-<percent>%")
        endif()
    endforeach()
    message(STATUS
        "MxR-malloc: ${_MXR_COUNT} region(s) from '${CONFIG_MXR_REGION_CONFIG}'")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        "MXR_PARSED_REGION_COUNT=${_MXR_COUNT}"
    )
else()
    message(WARNING "MxR-malloc: CONFIG_MXR_REGION_CONFIG is empty, using default")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        "MXR_PARSED_REGION_COUNT=3"
    )
endif()

# ============================================================
#  4. Linker wraps (ONLY applied in Wrap mode!)
# ============================================================
if(CONFIG_MXR_INTEGRATION_WRAP)
    set(MXR_WRAP_FLAGS
        "-Wl,--wrap=heap_caps_init"
        "-Wl,--wrap=_heap_caps_malloc"
        "-Wl,--wrap=_heap_caps_free"
        "-Wl,--wrap=_heap_caps_realloc"
        "-Wl,--wrap=_heap_caps_calloc"
        "-Wl,--wrap=_heap_caps_zalloc"
    )

    if(CONFIG_MXR_WRAP_HEAP_QUERY)
        list(APPEND MXR_WRAP_FLAGS
            "-Wl,--wrap=heap_caps_get_free_size"
            "-Wl,--wrap=heap_caps_get_minimum_free_size"
            "-Wl,--wrap=heap_caps_get_dram_free_size"
        )
    endif()

    if(CONFIG_MXR_WRAP_DEFAULT_POOL)
        list(APPEND MXR_WRAP_FLAGS
            "-Wl,--wrap=heap_caps_malloc_default"
            "-Wl,--wrap=heap_caps_realloc_default"
        )
    endif()

    if(CONFIG_MXR_WRAP_ESP_SYSTEM)
        list(APPEND MXR_WRAP_FLAGS
            "-Wl,--wrap=esp_get_free_heap_size"
            "-Wl,--wrap=esp_get_minimum_free_heap_size"
            "-Wl,--wrap=esp_get_free_internal_heap_size"
        )
    endif()

    if(CONFIG_MXR_WRAP_LIBC)
        list(APPEND MXR_WRAP_FLAGS
            "-Wl,--wrap=malloc"
            "-Wl,--wrap=free"
            "-Wl,--wrap=calloc"
            "-Wl,--wrap=realloc"
            "-Wl,--wrap=zalloc"
        )
    endif()

    target_link_libraries(${COMPONENT_LIB} INTERFACE ${MXR_WRAP_FLAGS})
endif()