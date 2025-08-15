cmake_minimum_required(VERSION 3.14)
project(hako C)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
      -O3
      -march=native
      -mtune=native
      -fomit-frame-pointer
      -fno-stack-protector
      -funroll-loops
      -flto)

    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
      add_compile_options(-fgraphite-identity -floop-nest-optimize
                          -ftree-vectorize -fipa-pta)
      set(CMAKE_EXE_LINKER_FLAGS
          "${CMAKE_EXE_LINKER_FLAGS} -flto=auto -fuse-linker-plugin")
    endif()

    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
      add_compile_options(-fvectorize -fslp-vectorize)
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto=thin")
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      add_compile_options(-fno-plt)
      set(CMAKE_EXE_LINKER_FLAGS
          "${CMAKE_EXE_LINKER_FLAGS} -Wl,-O3 -Wl,--as-needed")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-dead_strip")
    endif()

  elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(
      /O2
      /Ob3
      /GL
      /Oi
      /Ot
      /Oy
      /GF
      /Gw
      /GS-
      /fp:precise)

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
      add_compile_options(/arch:AVX2)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
      add_compile_options(/arch:armv8.0)
    endif()

    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} /LTCG /OPT:REF /OPT:ICF")
    add_compile_options(/MP)
  endif()

  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
      add_compile_options(-msse4.2 -mavx2 -mfma)
    endif()
  endif()
endif()

set(WAMR_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasm-micro-runtime)


if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(WAMR_BUILD_PLATFORM "linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(WAMR_BUILD_PLATFORM "darwin")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(WAMR_BUILD_PLATFORM "windows")
endif()


if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(WAMR_BUILD_TARGET "X86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(WAMR_BUILD_TARGET "AARCH64")
else()
  set(WAMR_BUILD_TARGET "X86_64")
endif()


if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(WAMR_BUILD_JIT 1)
  set(WAMR_BUILD_FAST_JIT 0)
  set(WAMR_BUILD_INTERP 0)
  set(WAMR_BUILD_FAST_INTERP 0)
  message(STATUS "Using LLVM JIT for Linux x86_64")
else()
  set(WAMR_BUILD_JIT 0)
  set(WAMR_BUILD_FAST_JIT 0)
  set(WAMR_BUILD_INTERP 1)
  set(WAMR_BUILD_FAST_INTERP 1)
  message(STATUS "Using Fast Interpreter for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_LIBC_WASI 1)
set(WAMR_BUILD_SIMD 1)
set(WAMR_BUILD_TAIL_CALL 1)
set(WAMR_BUILD_REF_TYPES 1)
set(WAMR_BUILD_BULK_MEMORY 1)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(WAMR_DISABLE_HW_BOUND_CHECK 1)
  set(WAMR_DISABLE_STACK_OVERFLOW_CHECK 1)
endif()


if(WAMR_BUILD_JIT)
  find_package(LLVM REQUIRED CONFIG)
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
  message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
  
  
  separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
  
  
  llvm_map_components_to_libnames(llvm_libs support core orcjit native)
endif()

include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_include_directories(vmlib PUBLIC ${WAMR_ROOT_DIR}/core/iwasm/include)

if(WAMR_BUILD_JIT)
  target_include_directories(vmlib PUBLIC ${LLVM_INCLUDE_DIRS})
  target_compile_definitions(vmlib PUBLIC ${LLVM_DEFINITIONS_LIST})
  target_link_libraries(vmlib PUBLIC ${llvm_libs})
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(vmlib PRIVATE ${CMAKE_C_COMPILE_FLAGS})
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(vmlib PUBLIC m pthread dl)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(vmlib PUBLIC -Wl,-O3 -Wl,--gc-sections)
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_link_libraries(vmlib PUBLIC m pthread)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(vmlib PUBLIC -Wl,-dead_strip)
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  target_link_libraries(vmlib PUBLIC ws2_32)
endif()

add_executable(hako src/cli.c)
target_link_libraries(hako PRIVATE vmlib)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_custom_command(
      TARGET hako
      POST_BUILD
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:hako>
      COMMENT "Stripping hako binary")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_custom_command(
      TARGET hako
      POST_BUILD
      COMMAND strip -x $<TARGET_FILE:hako>
      COMMENT "Stripping hako binary")
  endif()
endif()

set_target_properties(hako PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                      ${CMAKE_BINARY_DIR}/bin)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  message(STATUS "Building in Release mode with maximum optimizations")
  message(STATUS "Compiler: ${CMAKE_C_COMPILER_ID}")
  message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")
  message(STATUS "Architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()