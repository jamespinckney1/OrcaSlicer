# Builds shop_check, the failure check's comparison tool, for the checker service (Linux; see README.md).
# Only the engine is needed: src/slic3r/GUI/FailureCheck.* has no GUI or libslic3r dependencies.
cmake_minimum_required(VERSION 3.16)
project(shop_check CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Eigen3 REQUIRED NO_MODULE)
find_package(nlohmann_json REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TURBOJPEG REQUIRED libturbojpeg)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_executable(shop_check shop_check.cpp ${ROOT}/src/slic3r/GUI/FailureCheck.cpp)
target_include_directories(shop_check PRIVATE ${ROOT}/src ${TURBOJPEG_INCLUDE_DIRS})
target_link_libraries(shop_check PRIVATE Eigen3::Eigen nlohmann_json::nlohmann_json Boost::filesystem ${TURBOJPEG_LINK_LIBRARIES})
