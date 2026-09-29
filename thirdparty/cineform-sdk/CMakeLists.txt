# CMakeLists.txt
cmake_minimum_required (VERSION 3.5.1)
project (CineFormSDK)

# Build settings
# Float contraction, off, on every target.
#
# clang contracts a*b+c into a fused multiply-add where the hardware has one. That is a
# single rounding instead of two, so the result is *more* accurate -- and different. ARM
# has FMA and takes it; x86-64 without -mfma does not. The codec's scalar float paths
# therefore produced different numbers on the two architectures, and a wavelet accumulates
# the difference rather than dropping it.
#
# Measured with a differential harness against the same source built for both: the SSE
# intrinsics agreed exactly, the scalar a*b+c did not, and -ffp-contract=off made all of
# them agree. The MMX and SSE shims in Codec/simd_compat.h were not at fault.
#
# Set for every target rather than only ARM, so the flag is what fixes reproducibility
# rather than the accident of which architecture lacks the instruction.
if(NOT MSVC)
  # -ffp-contract=off: clang fuses a*b+c into a single-rounding FMA where the hardware
  # has one, so the same source gives different numbers on machines that differ in
  # whether it does.
  #
  # -fno-strict-aliasing: this codec reads the same memory through int, short, char and
  # vector types constantly -- WarpLib/sse_types.h is a union of five of them -- and the
  # standard says a compiler may assume that never happens. gcc and clang exploit that
  # assumption differently, which is measurable: with it on, gcc and clang builds of this
  # tree, same OS and same ISA, disagree on all 400 frames of TestCFHD -d by up to 30 KB,
  # while two ISAs under one compiler agree exactly. The difference is the compiler
  # reading undefined behaviour, not the architecture.
  add_compile_options(-ffp-contract=off -fno-strict-aliasing)
endif()

# GCC on aarch64 needs the SIMD features named explicitly. sse2neon reaches for CRC and
# crypto instructions, and where clang enables them by default for the host, GCC does not
# and fails in the header rather than at the call. Apple clang never hits this, so it shows
# up only on an arm64 Linux builder.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-march=armv8-a+crc+crypto -flax-vector-conversions)
  endif()
endif()

option(BUILD_LIBS "Build codec libraries" ON)
option(BUILD_TOOLS "Build test tools" ON)
option(BUILD_STATIC "Build static component" ON)
option(BUILD_SEPARATED "Build a separate de/encoder libraries" OFF)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
add_definitions(-D_ALLOCATOR=1 -DWARPSTUFF=1)

find_package(OpenMP)

if (WIN32)
	SET(COMPILER_FLAGS "")
endif (WIN32)

if (UNIX)
	SET(COMPILER_FLAGS -fPIC)
	SET(ADDITIONAL_LIBS "-luuid -lpthread")
	SET(TOY_LIBS "-lm")
endif (UNIX)

if (APPLE)
	SET(COMPILER_FLAGS -fvisibility=hidden)
	SET(ADDITIONAL_LIBS "-lpthread")
endif (APPLE)

if (OPENMP_FOUND)
	add_definitions(-DHAVE_OPENMP=1)
	SET(ADDITIONAL_LIBS "${ADDITIONAL_LIBS} ${OpenMP_CXX_FLAGS}")
endif (OPENMP_FOUND)
string(STRIP "${ADDITIONAL_LIBS}" ADDITIONAL_LIBS)

include_directories("Common" "Tables" "Codec" "ConvertLib" "WarpLib" "Example")
file(GLOB CODEC_SOURCES "Codec/*.c" "Codec/*.h" "Codec/*.cpp" "WarpLib/*.c" "WarpLib/*.h" )
file(GLOB ENCODER_SOURCES "EncoderSDK/*.cpp" "Common/*.h")
file(GLOB DECODER_SOURCES "DecoderSDK/*.cpp" "Common/*.h" "WarpLib/*.c" "WarpLib/*.h" "ConvertLib/*.cpp" "ConvertLib/*.h" )
file(GLOB EXAMPLE_SOURCE "Example/*.cpp" "Example/*.h" )
file(GLOB WAVELETDEMO_SOURCE "Example/WaveletDemo/*.c" "Example/WaveletDemo/*.h" )
file(GLOB PUBLIC_HEADERS "Common/*.h")

# Build CFHDCodec library (static and shared rules)
if (BUILD_LIBS)
	if (BUILD_STATIC)
		if (BUILD_SEPARATED)
			add_library(CFHDEncoderStatic STATIC ${CODEC_SOURCES} ${ENCODER_SOURCES})
			add_library(CFHDDecoderStatic STATIC ${CODEC_SOURCES} ${DECODER_SOURCES})
			target_compile_options(CFHDEncoderStatic PUBLIC ${COMPILER_FLAGS})
			target_compile_options(CFHDDecoderStatic PUBLIC ${COMPILER_FLAGS})		
			target_link_libraries (CFHDEncoderStatic)
			target_link_libraries (CFHDDecoderStatic)
		else (BUILD_SEPARATED)
			add_library(CFHDCodecStatic STATIC ${CODEC_SOURCES} ${ENCODER_SOURCES} ${DECODER_SOURCES})
			target_compile_options(CFHDCodecStatic PUBLIC ${COMPILER_FLAGS})
		
			set_target_properties(CFHDCodecStatic PROPERTIES POSITION_INDEPENDENT_CODE ON)
			if (UNIX)
				set_target_properties(CFHDCodecStatic PROPERTIES OUTPUT_NAME CFHDCodec)
			endif (UNIX)
		
			target_link_libraries(CFHDCodecStatic)
		endif (BUILD_SEPARATED)
	else (BUILD_STATIC)
		if (BUILD_SEPARATED)
			add_library(CFHDEncoder SHARED ${CODEC_SOURCES} ${ENCODER_SOURCES})
			add_library(CFHDDecoder SHARED ${CODEC_SOURCES} ${DECODER_SOURCES})
			target_compile_options(CFHDEncoder PUBLIC ${COMPILER_FLAGS})
			target_compile_options(CFHDDecoder PUBLIC ${COMPILER_FLAGS})
			target_compile_definitions(CFHDEncoder PUBLIC -DDYNAMICLIB=1)
			target_compile_definitions(CFHDDecoder PUBLIC -DDYNAMICLIB=1)
			target_link_libraries (CFHDEncoder)
			target_link_libraries (CFHDDecoder)
		else (BUILD_SEPARATED)
			add_library(CFHDCodecShared SHARED ${CODEC_SOURCES} ${ENCODER_SOURCES} ${DECODER_SOURCES})
			target_compile_options(CFHDCodecShared PUBLIC ${COMPILER_FLAGS})
			target_compile_definitions(CFHDCodecShared PUBLIC -DDYNAMICLIB=1 -DCODECCOMBINED=1)
			target_link_libraries(CFHDCodecShared)
			set_target_properties(CFHDCodecShared PROPERTIES POSITION_INDEPENDENT_CODE ON)
			set_target_properties(CFHDCodecShared PROPERTIES OUTPUT_NAME CFHDCodec)
		endif (BUILD_SEPARATED)
	endif (BUILD_STATIC)
endif (BUILD_LIBS)

# Build tools
if (BUILD_TOOLS)
    # TestCFHD
	add_executable(TestCFHD ${EXAMPLE_SOURCE})
	if (OPENMP_FOUND)
		target_compile_options(TestCFHD PRIVATE ${OpenMP_CXX_FLAGS})
	endif ()


    if (BUILD_STATIC)
		if (BUILD_SEPARATED)
			target_link_libraries(TestCFHD CFHDEncoderStatic CFHDDecoderStatic ${INTERNAL_LIBS} ${ADDITIONAL_LIBS})
		else (BUILD_SEPARATED)
			target_link_libraries(TestCFHD CFHDCodecStatic ${INTERNAL_LIBS} ${ADDITIONAL_LIBS})
		endif (BUILD_SEPARATED)
    else (BUILD_STATIC)
		if (BUILD_SEPARATED)
			target_link_libraries(TestCFHD CFHDEncoder CFHDDecoder  ${INTERNAL_LIBS} ${ADDITIONAL_LIBS})
		else (BUILD_SEPARATED)
			target_link_libraries(TestCFHD CFHDCodecShared ${INTERNAL_LIBS} ${ADDITIONAL_LIBS})
		endif (BUILD_SEPARATED)
    endif (BUILD_STATIC)

    # WaveletDemo
    add_executable(WaveletDemo ${WAVELETDEMO_SOURCE})
    target_link_libraries(WaveletDemo ${TOY_LIBS})
endif (BUILD_TOOLS)


# pkg-config integration
set(PROJECT_VERSION "10.0.2")
set(LIB_SUFFIX "" CACHE STRING "Define suffix of directory name (32/64)")
set(EXEC_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX} CACHE PATH "Installation prefix for executables and object code libraries" FORCE)
set(BIN_INSTALL_DIR ${EXEC_INSTALL_PREFIX}/bin CACHE PATH "Installation prefix for user executables" FORCE)
set(LIB_INSTALL_DIR ${EXEC_INSTALL_PREFIX}/lib${LIB_SUFFIX} CACHE PATH "Installation prefix for object code libraries" FORCE)
set(INCLUDE_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/include/cineformsdk CACHE PATH "Installation prefix for header files" FORCE)

# System wide installation
if (BUILD_STATIC)
	if (BUILD_SEPARATED)
		set(TARGET_NAMES CFHDEncoderStatic CFHDDecoderStatic)
	else (BUILD_SEPARATED)
		set(TARGET_NAMES CFHDCodecStatic)
	endif (BUILD_SEPARATED)
else (BUILD_STATIC)
	if (BUILD_SEPARATED)
		set(TARGET_NAMES CFHDEncoder CFHDDecoder)
	else (BUILD_SEPARATED)
		set(TARGET_NAMES CFHDCodecShared)
	endif (BUILD_SEPARATED)
endif (BUILD_STATIC)
foreach(TARGET_NAME ${TARGET_NAMES})
    set(LIB_INSTALL_NAMES ${LIB_INSTALL_NAMES} -l${TARGET_NAME})
endforeach()
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/libcineformsdk.pc.cmake ${CMAKE_CURRENT_BINARY_DIR}/libcineformsdk.pc)

install(TARGETS ${TARGET_NAMES} DESTINATION lib/)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libcineformsdk.pc DESTINATION lib/pkgconfig)
install(FILES ${PUBLIC_HEADERS} DESTINATION ${INCLUDE_INSTALL_DIR})
