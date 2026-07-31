# CMake toolchain for cross-compiling to Linux aarch64 using zig cc as a
# drop-in cross C/C++ compiler (no glibc sysroot management needed).
# Requires: zig on PATH (`brew install zig`).
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-aarch64-linux-gnu-zig.cmake
#   -DCROSS_DEPS_INSTALL_DIR=/path/to/prebuilt/libusb+libftdi+zlib ..

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(ZIG_CC_DIR "${CMAKE_CURRENT_LIST_DIR}/zig-cc")
set(CMAKE_C_COMPILER "${ZIG_CC_DIR}/aarch64-linux-gnu-gcc-zig")
set(CMAKE_CXX_COMPILER "${ZIG_CC_DIR}/aarch64-linux-gnu-g++-zig")
set(CMAKE_AR "${ZIG_CC_DIR}/zig-ar")
set(CMAKE_RANLIB "${ZIG_CC_DIR}/zig-ranlib")

# Not fatal here: CMake re-invokes this toolchain file for its own early
# compiler-ABI detection sub-build, before command-line -D cache vars are
# necessarily visible as DEFINED. Real usage (find_package/pkg-config) only
# happens in the main configure, by which point this is set.
if(DEFINED CROSS_DEPS_INSTALL_DIR)
	set(CMAKE_FIND_ROOT_PATH ${CROSS_DEPS_INSTALL_DIR})
	set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
	set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
	set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
	set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

	# Isolate pkg-config from the host's own .pc files during cross-compilation.
	set(ENV{PKG_CONFIG_LIBDIR} "${CROSS_DEPS_INSTALL_DIR}/lib/pkgconfig")
	set(ENV{PKG_CONFIG_PATH} "")
endif()

# hidapi is complex to cross-compile and wasn't cross-built here.
set(ENABLE_CMSISDAP OFF CACHE BOOL "hidapi not cross-built" FORCE)
set(BUILD_STATIC OFF CACHE BOOL "" FORCE)
