cmake_minimum_required(VERSION 2.6.0)

project(south-plugin-python-interface)

set(CMAKE_CXX_FLAGS_DEBUG "-O0 -ggdb")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
set(DLLIB -ldl)
set(COMMON_LIB common-lib)
set(SERVICE_COMMON_LIB services-common-lib)

# Find source files
file(GLOB SOURCES *.cpp)

# Find Python.h 3.5 header file
set(_PYTHON_3.5_INCLUDES /usr/include/python3.5m /usr/include/python3.5m)
list(APPEND _PYTHON_3.5_INCLUDES /usr/include/python3.5 /usr/include/python3.5)
find_path(Python3.5_INCLUDE NAMES Python.h PATHS ${_PYTHON_3.5_INCLUDES})
if (NOT Python3.5_INCLUDE)
        message(WARNING
        " Python 3.5 header file 'Python.h' not found in /usr/include. \n"
        " Target '${PROJECT_NAME}' filter is not built.")
        return()
else()
        message(STATUS "Found Python 3.5 header file 'Python.h' in " ${Python3.5_INCLUDE})
endif()

# Find Python 3.5 library
find_library(FoundPython_3.5 NAMES python3.5m python35m python3.5 python35)
if (NOT FoundPython_3.5)
        message(FATAL_ERROR "Python 3.5 library not found.")
        return()
else()
        message(STATUS "Found Python 3.5 library in " ${FoundPython_3.5})
endif()

# Include header files
include_directories(include ../../../common/include ../../../services/common/include ../../../thirdparty/rapidjson/include)

# Add Python 3.5 header files
include_directories(${Python3.5_INCLUDE})

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/../../../lib)

# Create shared library
add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_link_libraries(${PROJECT_NAME} ${DLLIB})
target_link_libraries(${PROJECT_NAME} ${COMMON_LIB})
target_link_libraries(${PROJECT_NAME} ${SERVICE_COMMON_LIB})

# Add Python 3.5 library
target_link_libraries(${PROJECT_NAME} python3.5m)

set_target_properties(${PROJECT_NAME} PROPERTIES SOVERSION 1)

# Install library
install(TARGETS ${PROJECT_NAME} DESTINATION foglamp/lib)