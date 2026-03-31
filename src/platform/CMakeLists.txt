set(PLATFORM_SOURCES "")

if(WIN32)
    list(APPEND PLATFORM_SOURCES windows/win32_platform.cpp)
elseif(ANDROID)
    # list(APPEND PLATFORM_SOURCES android/android_platform.cpp)
endif()

add_library(uldum_platform STATIC ${PLATFORM_SOURCES})

target_include_directories(uldum_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_platform PUBLIC uldum_core)

if(WIN32)
    target_link_libraries(uldum_platform PRIVATE user32 gdi32)
endif()
