# User CMake configuration to fix portable include paths
# This file overrides the generated CMake configuration to ensure 
# include paths work regardless of checkout location

# Determine the project source root relative to this CMake file
# This CMake file is located at: cmake/T1S_100BaseT_Bridge/default/user.cmake
# The project root is 3 levels up from here
get_filename_component(PROJECT_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# Override the compile rule function to fix include paths
function(T1S_100BaseT_Bridge_default_default_XC32_compile_rule target)
    set(options
        "-g"
        "${CC_PRE}"
        "-x"
        "c"
        "-c"
        "-mprocessor=ATSAME54P20A"
        "-ffunction-sections"
        "-fdata-sections"
        "-O1"
        "-fno-common"
        "-Wall"
        "-mdfp=${PACK_REPO_PATH}/Microchip/SAME54_DFP/3.8.234")
    list(REMOVE_ITEM options "")
    target_compile_options(${target} PRIVATE "${options}")
    target_compile_definitions(${target}
        PRIVATE "__DEBUG"
        PRIVATE "HAVE_CONFIG_H"
        PRIVATE "WOLFSSL_IGNORE_FILE_WARN"
        PRIVATE "XPRJ_default=default")
    
    # Fixed include directories using absolute paths from project source root
    target_include_directories(${target}
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src"
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src/config/default"
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src/config/default/library"
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src/config/default/library/tcpip/src"
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src/config/default/library/tcpip/src/common"
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src/packs/ATSAME54P20A_DFP"
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src/packs/CMSIS"
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src/packs/CMSIS/CMSIS/Core/Include"
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src/third_party/wolfssl"
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src/third_party/wolfssl/wolfssl"
        PRIVATE "${PROJECT_SOURCE_ROOT}"
        PRIVATE "${PACK_REPO_PATH}/ARM/CMSIS/5.8.0/CMSIS/Core/Include")
endfunction()

# Override the C++ compile rule function as well
function(T1S_100BaseT_Bridge_default_default_XC32_compile_cpp_rule target)
    set(options
        "-g"
        "${CC_PRE}"
        "${DEBUGGER_NAME_AS_MACRO}"
        "-mprocessor=ATSAME54P20A"
        "-frtti"
        "-fexceptions"
        "-fno-check-new"
        "-fenforce-eh-specs"
        "-ffunction-sections"
        "-O1"
        "-fno-common"
        "-mdfp=${PACK_REPO_PATH}/Microchip/SAME54_DFP/3.8.234")
    list(REMOVE_ITEM options "")
    target_compile_options(${target} PRIVATE "${options}")
    target_compile_definitions(${target}
        PRIVATE "__DEBUG"
        PRIVATE "XPRJ_default=default")
    
    # Fixed include directories using absolute paths from project source root  
    target_include_directories(${target}
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src"
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src/config/default"
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src/packs/ATSAME54P20A_DFP"
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src/packs/CMSIS"
        PRIVATE "${PROJECT_SOURCE_ROOT}/../src/packs/CMSIS/CMSIS/Core/Include"
        PRIVATE "${PROJECT_SOURCE_ROOT}"
        PRIVATE "${PACK_REPO_PATH}/ARM/CMSIS/5.8.0/CMSIS/Core/Include")
endfunction()

message(STATUS "Using portable include paths from user.cmake")
message(STATUS "Project source root: ${PROJECT_SOURCE_ROOT}")