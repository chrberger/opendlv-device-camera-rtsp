# Copyright (C) 2020  Christian Berger
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.2)
 
project (opendlv-device-camera-rtsp)

################################################################################
# Defining the relevant versions of OpenDLV Standard Message Set and libcluon.
set(OPENDLV_STANDARD_MESSAGE_SET opendlv-standard-message-set-v0.9.7.odvd)
set(CLUON_COMPLETE cluon-complete-v0.0.140.hpp)

################################################################################
# Set the search path for .cmake files.
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" ${CMAKE_MODULE_PATH})

################################################################################
# This project requires C++14 or newer.
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
IF(NOT(APPLE))
  # Build a static binary.
  set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")
ENDIF()

if("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin")
  set(CMAKE_CXX_FLAGS "-D_DARWIN_C_SOURCE=1 ")
  set(CMAKE_OSX_SYSROOT /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk)
endif()
# Add further warning levels.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} \
    -D_XOPEN_SOURCE=700 \
    -D_FORTIFY_SOURCE=2 \
    -DNO_OPENSSL=1 -DBSD=1 -DSOCKLEN_T=socklen_t -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE=1 \
    -Os \
    -fstack-protector \
    -fomit-frame-pointer \
    -ffunction-sections -fdata-sections \
    -pipe \
    -Wall -Wextra -Wshadow -Wdeprecated \
    -Wdiv-by-zero -Wfloat-equal -Wfloat-conversion -Wsign-compare -Wpointer-arith \
    -Wuninitialized -Wunreachable-code")

IF(APPLE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} \
      -DNEED_XLOCALE_H")
ENDIF()
 
################################################################################
# Create symbolic link to cluon-complete.hpp.
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/cluon-complete.hpp
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_CURRENT_SOURCE_DIR}/src/${CLUON_COMPLETE} ${CMAKE_BINARY_DIR}/cluon-complete.hpp
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${CLUON_COMPLETE})

# Add current build directory as include directory as it contains generated files.
include_directories(SYSTEM ${CMAKE_BINARY_DIR})

################################################################################
# Extract cluon-msc from cluon-complete.hpp.
IF(APPLE)
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/cluon-msc
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_CURRENT_SOURCE_DIR}/src/${CLUON_COMPLETE} ${CMAKE_BINARY_DIR}/cluon-complete.hpp
      COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/cluon-complete.hpp ${CMAKE_BINARY_DIR}/cluon-complete.cpp
      COMMAND ${CMAKE_CXX_COMPILER} -o ${CMAKE_BINARY_DIR}/cluon-msc ${CMAKE_BINARY_DIR}/cluon-complete.cpp -isysroot ${CMAKE_OSX_SYSROOT} -std=c++14 -pthread -D_DARWIN_C_SOURCE=1 -D HAVE_CLUON_MSC
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${CLUON_COMPLETE})
ELSE()
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/cluon-msc
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_CURRENT_SOURCE_DIR}/src/${CLUON_COMPLETE} ${CMAKE_BINARY_DIR}/cluon-complete.hpp
      COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/cluon-complete.hpp ${CMAKE_BINARY_DIR}/cluon-complete.cpp
      COMMAND ${CMAKE_CXX_COMPILER} -o ${CMAKE_BINARY_DIR}/cluon-msc ${CMAKE_BINARY_DIR}/cluon-complete.cpp -std=c++14 -pthread -D HAVE_CLUON_MSC
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${CLUON_COMPLETE})
ENDIF()

################################################################################
# Generate opendlv-standard-message-set.hpp from ${OPENDLV_STANDARD_MESSAGE_SET} file.
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/opendlv-standard-message-set.hpp
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMAND ${CMAKE_BINARY_DIR}/cluon-msc --cpp --out=${CMAKE_BINARY_DIR}/opendlv-standard-message-set.hpp ${CMAKE_CURRENT_SOURCE_DIR}/src/${OPENDLV_STANDARD_MESSAGE_SET}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${OPENDLV_STANDARD_MESSAGE_SET} ${CMAKE_BINARY_DIR}/cluon-msc)
# Add current build directory as include directory as it contains generated files.
include_directories(SYSTEM ${CMAKE_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

################################################################################
# Threads are necessary for linking the resulting binaries as UDPReceiver is running in parallel.
set(THREADS_PREFER_PTHREAD_FLAG ON)
IF(APPLE)
    set(CMAKE_THREAD_LIBS_INIT "-lpthread")
    set(CMAKE_HAVE_THREADS_LIBRARY 1)
    set(CMAKE_USE_WIN32_THREADS_INIT 0)
    set(CMAKE_USE_PTHREADS_INIT 1)
ELSE()
    find_package(Threads REQUIRED)
    set(LIBRARIES ${LIBRARIES} Threads::Threads)
ENDIF()

if(UNIX)
    if(NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin")
        find_package(LibRT REQUIRED)
        set(LIBRARIES ${LIBRARIES} ${LIBRT_LIBRARIES})
        include_directories(SYSTEM ${LIBRT_INCLUDE_DIR})
    endif()
endif()


################################################################################
# Build static library from live555.
set(LIVE ${CMAKE_BINARY_DIR}/live)
if (NOT EXISTS ${LIVE})
    file (DOWNLOAD https://download.videolan.org/pub/contrib/live555/live.2022.01.17.tar.gz ${CMAKE_BINARY_DIR}/live555.tar.gz)
    EXECUTE_PROCESS(COMMAND ${CMAKE_COMMAND} -E tar xvf ${CMAKE_BINARY_DIR}/live555.tar.gz  RESULT_VARIABLE unpack_result)
    if(NOT unpack_result STREQUAL "0")
        message(FATAL_ERROR "Failed to get live555 source tarball!")
    endif()
endif() 
FILE(GLOB LIVEHELPERSOURCE ${LIVE}/groupsock/*.c* ${LIVE}/liveMedia/*.c* ${LIVE}/UsageEnvironment/*.c* ${LIVE}/BasicUsageEnvironment/*.c*)
add_library (live555 STATIC ${LIVEHELPERSOURCE})	
target_include_directories(live555 PUBLIC ${LIVE}/groupsock/include ${LIVE}/liveMedia/include ${LIVE}/UsageEnvironment/include ${LIVE}/BasicUsageEnvironment/include src)
set(LIBRARIES ${LIBRARIES} live555)

# Create executable.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
add_executable(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/src/${PROJECT_NAME}.cpp ${CMAKE_BINARY_DIR}/cluon-complete.hpp ${CMAKE_BINARY_DIR}/opendlv-standard-message-set.hpp)
target_link_libraries(${PROJECT_NAME} ${LIBRARIES})

################################################################################
# Install executable.
install(TARGETS ${PROJECT_NAME} DESTINATION bin COMPONENT ${PROJECT_NAME})
