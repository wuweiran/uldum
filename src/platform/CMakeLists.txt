set(PLATFORM_SOURCES "")

if(WIN32)
    list(APPEND PLATFORM_SOURCES windows/win32_platform.cpp)
elseif(ANDROID)
    list(APPEND PLATFORM_SOURCES android/android_platform.cpp)
endif()

add_library(uldum_platform STATIC ${PLATFORM_SOURCES})

target_include_directories(uldum_platform PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_platform PUBLIC uldum_core)

if(WIN32)
    target_link_libraries(uldum_platform PRIVATE user32 gdi32 shcore)
elseif(ANDROID)
    # `android` for ANativeWindow, `log` for __android_log_print.
    # game-activity provides the native_app_glue headers we include from
    # android_platform.cpp; link as PUBLIC so transitive consumers also see
    # the include paths.
    target_link_libraries(uldum_platform
        PUBLIC  game-activity::game-activity_static
        PRIVATE android log)
endif()
