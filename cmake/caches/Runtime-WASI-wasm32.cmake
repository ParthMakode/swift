set(SWIFT_PRIMARY_VARIANT_SDK WASI CACHE STRING "")
set(SWIFT_PRIMARY_VARIANT_ARCH wasm32 CACHE STRING "")
set(SWIFT_HOST_VARIANT_SDK NONE CACHE STRING "")
set(SWIFT_HOST_VARIANT_ARCH NONE CACHE STRING "")

set(SWIFT_SDKS WASI CACHE STRING "")

set(SWIFT_INCLUDE_TOOLS NO CACHE BOOL "")
set(SWIFT_INCLUDE_TESTS TRUE CACHE BOOL "")
set(SWIFT_INCLUDE_DOCS NO CACHE BOOL "")

set(SWIFT_BUILD_SOURCEKIT NO CACHE BOOL "")
set(SWIFT_ENABLE_SOURCEKIT_TESTS NO CACHE BOOL "")

set(SWIFTWASM_DISABLE_REFLECTION_TEST YES CACHE BOOL "")

# stdlib configurations
set(SWIFT_BUILD_STATIC_STDLIB YES CACHE BOOL "")
set(SWIFT_BUILD_DYNAMIC_STDLIB NO CACHE BOOL "")
set(SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY YES CACHE BOOL "")

# build with the host compiler
set(SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER YES CACHE BOOL "")

set(SWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY TRUE CACHE BOOL "")
