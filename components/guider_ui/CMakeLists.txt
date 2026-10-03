set(GUIDER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../guider")

file(GLOB_RECURSE GUIDER_SRCS
    "${GUIDER_ROOT}/generated/*.c"
    "${GUIDER_ROOT}/generated/screens/*.c"
    "${GUIDER_ROOT}/generated/events/*.c"
    "${GUIDER_ROOT}/custom/*.c"
)

idf_component_register(
    SRCS        ${GUIDER_SRCS}
    INCLUDE_DIRS
        "${GUIDER_ROOT}/generated"
        "${GUIDER_ROOT}/generated/screens"
        "${GUIDER_ROOT}/generated/events"
        "${GUIDER_ROOT}/generated/assets/fonts"
        "${GUIDER_ROOT}/generated/assets/images"
        "${GUIDER_ROOT}/custom"
    REQUIRES
        lvgl__lvgl
        lvgl_init
)