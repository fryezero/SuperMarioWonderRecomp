# iOS feasibility probe: address space, memory limit, Metal and MoltenVK features on the device.
# cmake -S tools/ios_probe -B private/ios_probe -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DNX_TEAM=<team> [-DNX_ENT=plain|mem|probe]
cmake_minimum_required(VERSION 3.28)
project(WonderProbe OBJCXX CXX)
set(CMAKE_OSX_DEPLOYMENT_TARGET 17.0)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(NX_TEAM "" CACHE STRING "Apple development team id")
string(TOLOWER "com.wonderprobe.${NX_TEAM}" NX_PROBE_ID_DEFAULT)
set(NX_PROBE_ID "${NX_PROBE_ID_DEFAULT}" CACHE STRING "Bundle id of the probe")
set(NX_ENT "plain" CACHE STRING "Entitlements file: plain, mem (increased-memory-limit), probe (+extended VA)")
set(NX_MOLTENVK "${CMAKE_SOURCE_DIR}/../../private/tools/moltenvk/MoltenVK/MoltenVK" CACHE PATH "MoltenVK release dir")
add_executable(WonderProbe MACOSX_BUNDLE probe.mm)
target_include_directories(WonderProbe PRIVATE "${NX_MOLTENVK}/include")
target_link_libraries(WonderProbe PRIVATE "${NX_MOLTENVK}/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a"
    "-framework Metal" "-framework QuartzCore" "-framework IOSurface" "-framework UIKit" "-framework Foundation"
    "-framework CoreGraphics" c++)
target_compile_options(WonderProbe PRIVATE -fobjc-arc)
set(ENT "${CMAKE_SOURCE_DIR}/${NX_ENT}.entitlements")
set_target_properties(WonderProbe PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${NX_PROBE_ID}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${NX_PROBE_ID}"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${NX_TEAM}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${ENT}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_ENABLE_BITCODE NO)
