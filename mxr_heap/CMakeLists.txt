# MxR-malloc component (WRAP MODE).

idf_component_register(
    SRCS
        "mxr_malloc.c"
        "mxr_heap_wrap.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        log
        newlib
        freertos
        esp8266
)


if(CONFIG_MXR_WARN_HEAP_TRACING AND CONFIG_HEAP_TRACING)
    message(WARNING
        "MxR-malloc: CONFIG_HEAP_TRACING is enabled. "
        "Original heap tracing is not compatible with MxR wrap mode. "
        "Disable CONFIG_HEAP_TRACING or disable this warning."
    )
endif()

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