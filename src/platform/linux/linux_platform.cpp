// Linux platform stub. `windows/win32_platform.cpp` and
// `android/android_platform.cpp` carry the real window + input code
// for their desktops; this file is the placeholder for the eventual
// Linux desktop implementation. Today only the headless `uldum_server`
// build needs `uldum_platform` to link on Linux — and the server never
// calls Platform methods, so an empty translation unit is enough to
// give CMake a source for the STATIC lib. `platform::InputState` is a
// POD struct in platform.h and needs no definition here.

namespace uldum::platform {
// Intentionally empty — fill in window / input handling when a real
// Linux desktop build target lands.
}
