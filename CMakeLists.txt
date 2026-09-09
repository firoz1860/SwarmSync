cmake_minimum_required(VERSION 3.20)

project(SwarmSync VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

file(GLOB SWARMSYNC_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB SWARMSYNC_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)

add_library(swarmsync_core ${SWARMSYNC_CORE_SOURCES})
target_include_directories(swarmsync_core PUBLIC include)
target_compile_options(swarmsync_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(swarmsync_core PUBLIC OpenSSL::Crypto Threads::Threads)

add_executable(swarmsync apps/swarmsync_main.cpp)
target_link_libraries(swarmsync PRIVATE swarmsync_core)
target_compile_options(swarmsync PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(swarmsync-tracker apps/tracker_main.cpp)
target_link_libraries(swarmsync-tracker PRIVATE swarmsync_core)
target_compile_options(swarmsync-tracker PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(swarmsync-tests ${SWARMSYNC_TEST_SOURCES})
target_include_directories(swarmsync-tests PRIVATE tests)
target_link_libraries(swarmsync-tests PRIVATE swarmsync_core)
target_compile_options(swarmsync-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()
add_test(NAME swarmsync-tests COMMAND swarmsync-tests)
add_test(NAME swarmsync-local-demo COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_demo.sh)
set_tests_properties(swarmsync-local-demo PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
