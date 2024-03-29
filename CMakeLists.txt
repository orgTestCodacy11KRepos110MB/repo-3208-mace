# new CUDA support requires 3.8 for Linux/Mac, and 3.9 for Windows
cmake_minimum_required(VERSION 3.2 FATAL_ERROR)
message("CMAKE_VERSION: ${CMAKE_VERSION}")
project(mace C CXX)

option(MACE_ENABLE_CPU         "whether to enable CPU support"              OFF)
option(MACE_ENABLE_NEON        "whether to enable NEON support"             OFF)
option(MACE_ENABLE_QUANTIZE    "whether to enable NEON int8 support"        OFF)
option(MACE_ENABLE_OPENCL      "whether to enable OpenCL support"           OFF)
option(MACE_ENABLE_CUDA        "whether to enable CUDA support"             OFF)
option(MACE_ENABLE_HEXAGON_DSP "whether to enable Hexagon DSP support"      OFF)
option(MACE_ENABLE_HEXAGON_HTA "whether to enable Hexagon HTA support"      OFF)
option(MACE_ENABLE_MTK_APU     "whether to enable MTK APU support"          OFF)
option(MACE_ENABLE_BFLOAT16    "whether to enable bfloat16 support"         OFF)
option(MACE_ENABLE_FP16        "whether to enable armv8.2 fp16 support"     OFF)
option(MACE_ENABLE_TESTS       "whether to build c++ unit tests"            OFF)
option(MACE_ENABLE_BENCHMARKS  "whether to build c++ micro benchmarks"      OFF)
option(MACE_ENABLE_OPT_SIZE    "whether to build with optimized binary size" ON)
option(MACE_ENABLE_OBFUSCATE   "whether to build with code obfuscation"      ON)
option(MACE_ENABLE_CCACHE      "whether to build with ccache"                ON)
option(MACE_ENABLE_CODE_MODE   "whether to use code mode"                   OFF)

message("CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")

string(TOLOWER ${CMAKE_SYSTEM_NAME} MACE_OS)
string(TOLOWER ${CMAKE_SYSTEM_PROCESSOR} MACE_TARGET)

if(MACE_ENABLE_CCACHE)
  find_program(CCACHE_FOUND ccache)
  if(CCACHE_FOUND)
    set(CMAKE_CXX_COMPILER_LAUNCHER ccache)
    set(CMAKE_C_COMPILER_LAUNCHER   ccache)
  endif(CCACHE_FOUND)
endif(MACE_ENABLE_CCACHE)

# TODO make these flags well defined and organized
# TODO enable sanitizer
set(MACE_CC_FLAGS "${MACE_CC_FLAGS} -fPIC")
if(MACE_ENABLE_OPT_SIZE)
  if(APPLE)
    set(MACE_LINKER_FLAGS "${MACE_LINKER_FLAGS} -Wl,-dead_strip -Wl,-dead_strip_dylibs")
  else(APPLE)
    set(MACE_LINKER_FLAGS "${MACE_LINKER_FLAGS} -Wl,--strip-all -Wl,--gc-sections")
    set(MACE_CC_FLAGS "${MACE_CC_FLAGS} -ffunction-sections -fdata-sections")
  endif(APPLE)
  set(MACE_CC_FLAGS "${MACE_CC_FLAGS} -fvisibility=hidden -fvisibility-inlines-hidden")
  set(MACE_CODE_CC_FLAGS "${MACE_CODE_CC_FLAGS} -fno-rtti -fno-exceptions -DGOOGLE_PROTOBUF_NO_RTTI -DPROTOBUF_USE_EXCEPTIONS=0")
endif(MACE_ENABLE_OPT_SIZE)

if(MACE_ENABLE_CODE_MODE)
  set(MACE_CODE_CC_FLAGS "${MACE_CODE_CC_FLAGS} -DMODEL_GRAPH_FORMAT_CODE")
endif(MACE_ENABLE_CODE_MODE)

# flags apply only to mace code (third_party excluded)
# -Wno-error=unused-command-line-argument: official Android toolchain contains
# unsupported argument and will break ccache preprocessor
if(ANDROID)
  set(MACE_CODE_CC_FLAGS "${MACE_CODE_CC_FLAGS} -Wall -Werror -Wno-error=unused-command-line-argument -Wno-error=unevaluated-expression -Wno-error=tautological-compare")
else(ANDROID)
  set(MACE_CODE_CC_FLAGS "${MACE_CODE_CC_FLAGS} -Wall -Werror")
endif(ANDROID)
set(MACE_CODE_CC_FLAGS "${MACE_CODE_CC_FLAGS} -std=c++11 -D_GLIBCXX_USE_C99_MATH_TR1")

if(IOS)
  # TODO correct the code
  set(MACE_CODE_CC_FLAGS "${MACE_CODE_CC_FLAGS} -Wno-error=shorten-64-to-32")
endif(IOS)

if(MACE_ENABLE_NEON)
  add_definitions(-DMACE_ENABLE_NEON)
  if(ANDROID_ABI STREQUAL "armeabi-v7a")
    # Enable NEON fp16 support
    string(REPLACE "-mfpu=neon " "-mfpu=neon-fp16 " CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
  endif(ANDROID_ABI STREQUAL "armeabi-v7a")
endif(MACE_ENABLE_NEON)

if(MACE_ENABLE_QUANTIZE)
  add_definitions(-DMACE_ENABLE_QUANTIZE)
  add_definitions(-DGEMMLOWP_USE_MACE_THREAD_POOL)
  add_definitions(-DMACE_DEPTHWISE_U8_USE_MULTI_THREAD)
endif(MACE_ENABLE_QUANTIZE)

if(MACE_ENABLE_CPU OR MACE_ENABLE_OPENCL)
  add_definitions(-DMACE_ENABLE_CPU)
endif(MACE_ENABLE_CPU OR MACE_ENABLE_OPENCL)

if(MACE_ENABLE_OPENCL)
  if(IOS)
    message(FATAL_ERROR "OpenCL is not supported for iOS")
  endif(IOS)
  add_definitions(-DMACE_ENABLE_OPENCL)
endif(MACE_ENABLE_OPENCL)

if(MACE_ENABLE_CUDA)
  # new CUDA support requires 3.8 for Linux/Mac, and 3.9 for Windows
  cmake_minimum_required(VERSION 3.8 FATAL_ERROR)
  enable_language(CUDA)
endif(MACE_ENABLE_CUDA)

if(MACE_ENABLE_RPCMEM)
  add_definitions(-DMACE_ENABLE_RPCMEM)
endif(MACE_ENABLE_RPCMEM)

if(MACE_ENABLE_HEXAGON_DSP OR MACE_ENABLE_HEXAGON_HTA)
  if(ANDROID_ABI STREQUAL "arm64-v8a")
    # Use gold linker to avoid linking check of libcdsprpc.so
    set(MACE_LINKER_FLAGS "${MACE_LINKER_FLAGS} -fuse-ld=gold")
  endif(ANDROID_ABI STREQUAL "arm64-v8a")
endif(MACE_ENABLE_HEXAGON_DSP OR MACE_ENABLE_HEXAGON_HTA)

if(MACE_ENABLE_HEXAGON_DSP)
  if(NOT ANDROID)
    message(FATAL_ERROR "Hexagon DSP is only supported on Android")
  endif(NOT ANDROID)
  # TODO => -DMACE_ENABLE_HEXAGON_DSP
  add_definitions(-DMACE_ENABLE_HEXAGON)
endif(MACE_ENABLE_HEXAGON_DSP)

if(MACE_ENABLE_RPCMEM)
  add_definitions(-DMACE_ENABLE_RPCMEM)
endif(MACE_ENABLE_RPCMEM)

if(MACE_ENABLE_HEXAGON_HTA)
  if(NOT ANDROID)
    message(FATAL_ERROR "Hexagon HTA is only supported on Android")
  endif(NOT ANDROID)
  add_definitions(-DMACE_ENABLE_HEXAGON_HTA)
  add_definitions(-DMACE_ENABLE_HTA)
endif(MACE_ENABLE_HEXAGON_HTA)

if(MACE_ENABLE_MTK_APU)
  if(NOT ANDROID)
    message(FATAL_ERROR "MTK APU is only supported on Android")
  endif(NOT ANDROID)
  add_definitions(-DMACE_ENABLE_MTK_APU)
  # Use gold linker to avoid linking check of libapu-platform.so
  set(MACE_LINKER_FLAGS "${MACE_LINKER_FLAGS} -fuse-ld=gold")
endif(MACE_ENABLE_MTK_APU)

if(MACE_ENABLE_BFLOAT16)
  add_definitions(-DMACE_ENABLE_BFLOAT16)
endif(MACE_ENABLE_BFLOAT16)

if(MACE_ENABLE_FP16)
  add_definitions(-DMACE_ENABLE_FP16)
endif(MACE_ENABLE_FP16)

if(MACE_ENABLE_OBFUSCATE)
  add_definitions(-DMACE_OBFUSCATE_LITERALS)
endif(MACE_ENABLE_OBFUSCATE)

if(NOT MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${MACE_CODE_CC_FLAGS} ${MACE_CC_FLAGS}")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${MACE_LINKER_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${MACE_LINKER_FLAGS}")
endif(NOT MSVC)

include(${PROJECT_SOURCE_DIR}/third_party/third_party.cmake)

include_directories("${PROJECT_SOURCE_DIR}")
include_directories("${PROJECT_SOURCE_DIR}/include")
include_directories("${PROJECT_BINARY_DIR}") # proto

add_subdirectory(include)
add_subdirectory(mace)

if(MACE_ENABLE_TESTS OR MACE_ENABLE_BENCHMARKS)
  add_subdirectory(test)
endif(MACE_ENABLE_TESTS OR MACE_ENABLE_BENCHMARKS)

if(MACE_ENABLE_MICRO)
  add_subdirectory(micro)
endif(MACE_ENABLE_MICRO)
