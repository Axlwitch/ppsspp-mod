# vim:noexpandtab:
cmake_minimum_required(VERSION 3.16)
project(PPSSPP)
enable_testing()

if(NOT ANDROID)
	set(CMAKE_CXX_STANDARD 17)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

enable_language(ASM)

add_compile_definitions(__STDC_CONSTANT_MACROS)
# ===== AKTIVASI SHADER DUMP & REPLACE =====
# Aktif untuk semua build type
add_compile_definitions(__FRAGMENT_GLSL_FILE__)
add_compile_definitions(__VERTEXT_GLSL_FILE__)
message("Shader Dump & Replace: ENABLED for all builds")
# ==========================================

if(${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang" OR ${CMAKE_CXX_COMPILER_ID} STREQUAL "AppleClang")
	set(CLANG ON)
	message("Clang enabled")
else()
	message("Non-Clang compiler detected: ${CMAKE_CXX_COMPILER_ID}")
endif()

if(FORCED_CPU)
	message("Detected CPU (${CMAKE_SYSTEM_PROCESSOR}) overridden as: ${FORCED_CPU}")
	set(CMAKE_SYSTEM_PROCESSOR ${FORCED_CPU})
endif()

if(CMAKE_SYSTEM_PROCESSOR)
	if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "^aarch64" OR ${CMAKE_SYSTEM_PROCESSOR} MATCHES "^arm64")
		set(ARM64 ON)
	elseif(${CMAKE_SYSTEM_PROCESSOR} MATCHES "^arm")
		set(ARM_DEVICE ON)
		if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "^armv7")
			set(ARMV7_DEVICE ON)
			add_compile_options(-mfpu=neon)
			if(ANDROID)
				set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -target armv7a-none-linux-android")
			endif()
		endif()
	elseif(${CMAKE_SYSTEM_PROCESSOR} MATCHES "^amd64" OR ${CMAKE_SYSTEM_PROCESSOR} MATCHES "^x86_64" OR ${CMAKE_SYSTEM_PROCESSOR} MATCHES "^AMD64")
		set(X86_DEVICE ON)
		set(X86_64_DEVICE ON)
	elseif(${CMAKE_SYSTEM_PROCESSOR} MATCHES "^x86" OR ${CMAKE_SYSTEM_PROCESSOR} MATCHES "i.86")
		set(X86_DEVICE ON)
	endif()
endif()

set(ANDROID ON)
set(VULKAN ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/Modules)

include(CheckCXXSourceCompiles)
include(GNUInstallDirs)

add_compile_definitions(ASSETS_DIR="${CMAKE_INSTALL_FULL_DATADIR}/ppsspp/assets/")

if(OPENXR AND NOT ARMV7_DEVICE)
	add_compile_definitions(OPENXR)
	add_subdirectory(ext/OpenXR-SDK)
	message("OpenXR enabled")
endif()

if(GOLD)
	add_compile_definitions(GOLD)
	message("Gold Build")
else()
	message("Non-gold Build")
endif()

# ==============================================================================
# OPTIONS (PAKSA HANYA UNTUK TARGET ANDROID)
# ==============================================================================
option(ARMV7 "Set to ON if targeting an ARMv7 processor" ${ARMV7_DEVICE})
option(ARM "Set to ON if targeting an ARM processor" ${ARM_DEVICE})
option(X86 "Set to ON if targeting an X86 processor" ${X86_DEVICE})
option(X86_64 "Set to ON if targeting an X86_64 processor" ${X86_64_DEVICE})

option(USING_GLES2 "Set to ON if target device uses OpenGL ES 2.0" ON)
option(MOBILE_DEVICE "Set to ON when targeting a mobile device" ON)

# Matikan seluruh build eksternal & platform lain
set(HEADLESS OFF CACHE BOOL "" FORCE)
set(UNITTEST OFF CACHE BOOL "" FORCE)
set(ATLAS_TOOL OFF CACHE BOOL "" FORCE)
set(LIBRETRO OFF CACHE BOOL "" FORCE)
set(USE_DISCORD OFF CACHE BOOL "" FORCE)
set(USING_EGL OFF CACHE BOOL "" FORCE)
set(USING_FBDEV OFF CACHE BOOL "" FORCE)
set(USING_X11_VULKAN OFF CACHE BOOL "" FORCE)
set(USE_WAYLAND_WSI OFF CACHE BOOL "" FORCE)

option(USE_FFMPEG "Build with FFMPEG support" ON)
option(USE_MINIUPNPC "Build with miniUPnPc support" ON)
option(USE_CCACHE "Use ccache if detected" ON)
option(GOLD "Gold build" OFF)

if(USE_CCACHE)
	include(ccache)
endif()

add_compile_definitions(HTTPS_NOT_AVAILABLE)

include_directories(ext/glslang)
include_directories(ext/glew)

if(OPENXR AND NOT ARMV7_DEVICE)
	set(OPENGL_LIBRARIES GLESv3 EGL)
else()
	set(OPENGL_LIBRARIES GLESv2 EGL)
endif()

include(FindThreads)
include_directories("${CMAKE_SOURCE_DIR}")

add_compile_definitions(USING_GLES2)
add_compile_definitions(MOBILE_DEVICE)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release")
endif()

# Compiler Flags Optimization
add_compile_definitions(PNG_ARM_NEON_OPT=0)
add_compile_options(-Wall -Werror=return-type -Wno-unused-function -Wno-sign-compare -Wno-unused-but-set-variable "$<$<COMPILE_LANGUAGE:CXX>:-Wno-reorder>" -Wno-unknown-pragmas -Wno-unused-value -Wno-unused-variable -Wno-error=incompatible-pointer-types)
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-std=c++17>")
add_compile_options(-fsigned-char -fno-strict-aliasing -fno-math-errno)

check_symbol_exists(getauxval sys/auxv.h HAVE_GETAUXVAL)
if(HAVE_GETAUXVAL)
	add_definitions(-DHAVE_GETAUXVAL)
endif()

check_symbol_exists(elf_aux_info sys/auxv.h HAVE_ELF_AUX_INFO)
if(HAVE_ELF_AUX_INFO)
	add_definitions(-DHAVE_ELF_AUX_INFO)
endif()

# Subdirectories External Libraries
add_subdirectory(ext)
include_directories(ext/OpenXR-SDK/include)
include_directories(ext)
include_directories(Common)

if(USE_FFMPEG)
	if(NOT FFMPEG_DIR)
		if(ARMV7)
			set(PLATFORM_ARCH "android/armv7")
		elseif(ARM64)
			set(PLATFORM_ARCH "android/arm64")
		elseif(X86_64)
			set(PLATFORM_ARCH "android/x86_64")
		elseif(X86)
			set(PLATFORM_ARCH "android/x86")
		endif()
		set(FFMPEG_DIR "ffmpeg/${PLATFORM_ARCH}")
	endif()
	find_package(FFmpeg REQUIRED avcodec avformat avutil swresample swscale)
endif()

# Embedded ZLIB
add_library(zlib STATIC
	ext/zlib/adler32.c
	ext/zlib/compress.c
	ext/zlib/crc32.c
	ext/zlib/crc32.h
	ext/zlib/deflate.c
	ext/zlib/deflate.h
	ext/zlib/gzclose.c
	ext/zlib/gzguts.h
	ext/zlib/gzlib.c
	ext/zlib/gzread.c
	ext/zlib/gzwrite.c
	ext/zlib/infback.c
	ext/zlib/inffast.c
	ext/zlib/inffast.h
	ext/zlib/inffixed.h
	ext/zlib/inflate.c
	ext/zlib/inflate.h
	ext/zlib/inftrees.c
	ext/zlib/inftrees.h
	ext/zlib/trees.c
	ext/zlib/trees.h
	ext/zlib/uncompr.c
	ext/zlib/zconf.h
	ext/zlib/zlib.h
	ext/zlib/zutil.c
	ext/zlib/zutil.h
)
include_directories(ext/zlib)
set(ZLIB_LIBRARY zlib)

# Embedded LIBZIP
add_library(libzip STATIC
	ext/libzip/zip_add.c
	ext/libzip/zip_add_dir.c
	ext/libzip/zip_add_entry.c
	ext/libzip/zip_algorithm_deflate.c
	ext/libzip/zip_buffer.c
	ext/libzip/zip_close.c
	ext/libzip/zip_delete.c
	ext/libzip/zip_dir_add.c
	ext/libzip/zip_dirent.c
	ext/libzip/zip_discard.c
	ext/libzip/zip_entry.c
	ext/libzip/zip_error.c
	ext/libzip/zip_error_clear.c
	ext/libzip/zip_error_get.c
	ext/libzip/zip_error_get_sys_type.c
	ext/libzip/zip_error_strerror.c
	ext/libzip/zip_error_to_str.c
	ext/libzip/zip_extra_field.c
	ext/libzip/zip_extra_field_api.c
	ext/libzip/zip_fclose.c
	ext/libzip/zip_fdopen.c
	ext/libzip/zip_file_add.c
	ext/libzip/zip_file_error_clear.c
	ext/libzip/zip_file_error_get.c
	ext/libzip/zip_file_get_comment.c
	ext/libzip/zip_file_get_external_attributes.c
	ext/libzip/zip_file_get_offset.c
	ext/libzip/zip_file_rename.c
	ext/libzip/zip_file_replace.c
	ext/libzip/zip_file_set_comment.c
	ext/libzip/zip_file_set_encryption.c
	ext/libzip/zip_file_set_external_attributes.c
	ext/libzip/zip_file_set_mtime.c
	ext/libzip/zip_file_strerror.c
	ext/libzip/zip_fopen.c
	ext/libzip/zip_fopen_encrypted.c
	ext/libzip/zip_fopen_index.c
	ext/libzip/zip_fopen_index_encrypted.c
	ext/libzip/zip_fread.c
	ext/libzip/zip_fseek.c
	ext/libzip/zip_ftell.c
	ext/libzip/zip_get_archive_comment.c
	ext/libzip/zip_get_archive_flag.c
	ext/libzip/zip_get_encryption_implementation.c
	ext/libzip/zip_get_file_comment.c
	ext/libzip/zip_get_name.c
	ext/libzip/zip_get_num_entries.c
	ext/libzip/zip_get_num_files.c
	ext/libzip/zip_hash.c
	ext/libzip/zip_io_util.c
	ext/libzip/zip_libzip_version.c
	ext/libzip/zip_memdup.c
	ext/libzip/zip_name_locate.c
	ext/libzip/zip_new.c
	ext/libzip/zip_open.c
	ext/libzip/zip_pkware.c
	ext/libzip/zip_progress.c
	ext/libzip/zip_rename.c
	ext/libzip/zip_replace.c
	ext/libzip/zip_set_archive_comment.c
	ext/libzip/zip_set_archive_flag.c
	ext/libzip/zip_set_default_password.c
	ext/libzip/zip_set_file_comment.c
	ext/libzip/zip_set_file_compression.c
	ext/libzip/zip_set_name.c
	ext/libzip/zip_source_accept_empty.c
	ext/libzip/zip_source_begin_write.c
	ext/libzip/zip_source_begin_write_cloning.c
	ext/libzip/zip_source_buffer.c
	ext/libzip/zip_source_call.c
	ext/libzip/zip_source_close.c
	ext/libzip/zip_source_commit_write.c
	ext/libzip/zip_source_compress.c
	ext/libzip/zip_source_crc.c
	ext/libzip/zip_source_error.c
	ext/libzip/zip_source_file_common.c
	ext/libzip/zip_source_file_stdio.c
	ext/libzip/zip_source_free.c
	ext/libzip/zip_source_function.c
	ext/libzip/zip_source_get_file_attributes.c
	ext/libzip/zip_source_is_deleted.c
	ext/libzip/zip_source_layered.c
	ext/libzip/zip_source_open.c
	ext/libzip/zip_source_pkware_decode.c
	ext/libzip/zip_source_pkware_encode.c
	ext/libzip/zip_source_read.c
	ext/libzip/zip_source_remove.c
	ext/libzip/zip_source_rollback_write.c
	ext/libzip/zip_source_seek.c
	ext/libzip/zip_source_seek_write.c
	ext/libzip/zip_source_stat.c
	ext/libzip/zip_source_supports.c
	ext/libzip/zip_source_tell.c
	ext/libzip/zip_source_tell_write.c
	ext/libzip/zip_source_window.c
	ext/libzip/zip_source_write.c
	ext/libzip/zip_source_zip.c
	ext/libzip/zip_source_zip_new.c
	ext/libzip/zip_stat.c
	ext/libzip/zip_stat_index.c
	ext/libzip/zip_stat_init.c
	ext/libzip/zip_strerror.c
	ext/libzip/zip_string.c
	ext/libzip/zip_unchange.c
	ext/libzip/zip_unchange_all.c
	ext/libzip/zip_unchange_archive.c
	ext/libzip/zip_unchange_data.c
	ext/libzip/zip_utf-8.c
	ext/libzip/zip_err_str.c
	ext/libzip/zip_mkstempm.c
	ext/libzip/zip_source_file_stdio_named.c
	ext/libzip/zip_random_unix.c
)
target_compile_definitions(libzip PRIVATE HAVE_STDBOOL_H)
include_directories(ext/libzip)
set(LIBZIP_LIBRARY libzip)

# Embedded PNG17
if(ARM)
	set(PNG_ARM_INCLUDES
		ext/libpng17/arm/arm_init.c
		ext/libpng17/arm/filter_neon.S
		ext/libpng17/arm/filter_neon_intrinsics.c
	)
elseif(ARM64)
	set(PNG_ARM_INCLUDES
		ext/libpng17/arm/arm_init.c
		ext/libpng17/arm/filter_neon_intrinsics.c
	)
endif()

add_library(png17 STATIC
	ext/libpng17/pngconf.h
	ext/libpng17/pngdebug.h
	ext/libpng17/png.c
	ext/libpng17/png.h
	ext/libpng17/pngerror.c
	ext/libpng17/pngget.c
	ext/libpng17/pnginfo.h
	ext/libpng17/pnglibconf.h
	ext/libpng17/pngmem.c
	ext/libpng17/pngpread.c
	ext/libpng17/pngpriv.h
	ext/libpng17/pngread.c
	ext/libpng17/pngrio.c
	ext/libpng17/pngrtran.c
	ext/libpng17/pngrutil.c
	ext/libpng17/pngset.c
	ext/libpng17/pngstruct.h
	ext/libpng17/pngtrans.c
	ext/libpng17/pngwio.c
	ext/libpng17/pngwrite.c
	ext/libpng17/pngwtran.c
	ext/libpng17/pngwutil.c
	${PNG_ARM_INCLUDES}
)
set(PNG_LIBRARIES png17)
include_directories(ext/libpng17)

# Android JNI Sources
list(APPEND AndroidJniSource
	android/jni/app-android.cpp
	android/jni/AndroidAudio.cpp
	android/jni/AndroidAudio.h
	android/jni/OpenSLContext.cpp
	android/jni/OpenSLContext.h
)

set(ATOMIC_LIB atomic)
set(GlslangLibs glslang OGLCompiler OSDependent SPIRV spirv-cross-glsl)

add_subdirectory(Common)
add_subdirectory(ext/zstd-build)
include_directories(ext/zstd/lib)
include_directories(ext/libchdr/include)

add_subdirectory(GPU)
add_subdirectory(Core)
add_subdirectory(UI)

if(ARM)
	list(APPEND NativeAppSource android/jni/ArmEmitterTest.cpp)
elseif(ARM64)
	list(APPEND NativeAppSource android/jni/Arm64EmitterTest.cpp)
endif()

# Target Library Utama Android (.so)
add_library(ppsspp_jni SHARED ${AndroidJniSource} ${NativeAppSource})
target_link_libraries(ppsspp_jni Core log EGL OpenSLES)

if(TARGET ppsspp_ui)
	target_link_libraries(ppsspp_jni ppsspp_ui)
endif()

if(OPENXR AND NOT ARMV7_DEVICE)
	target_link_libraries(ppsspp_jni openxr_loader)
endif()

if(ARM64)
	# Support 16kb page size pada Android
	target_link_options(ppsspp_jni PRIVATE "-Wl,-z,max-page-size=16384")
endif()
