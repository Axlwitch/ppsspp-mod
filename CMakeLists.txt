# vim:noexpandtab:
cmake_minimum_required(VERSION 3.16)
project(PPSSPP)
enable_testing()
if (ANDROID)
    add_compile_options(-O3 -flto)
    if(CMAKE_ANDROID_ARCH_ABI MATCHES "arm")
        add_compile_options(-mcpu=cortex-a53)
    endif()
    add_link_options(-flto)
endif()
		
#This is supposed to work but doesn't!
if(NOT ANDROID)
	set(CMAKE_CXX_STANDARD 17)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
