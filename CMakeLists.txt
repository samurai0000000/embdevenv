cmake_minimum_required(VERSION 3.16)
project(embdevenv VERSION 0.1.10 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -pedantic -Wno-unused-parameter)
endif()

# Build metadata & version header generation
set(WHOAMI_OUTPUT)
execute_process(
    COMMAND whoami
    OUTPUT_VARIABLE WHOAMI_OUTPUT
    RESULT_VARIABLE WHOAMI_RESULT
    ERROR_VARIABLE WHOAMI_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(HOSTNAME_OUTPUT)
execute_process(
    COMMAND hostname
    OUTPUT_VARIABLE HOSTNAME_OUTPUT
    RESULT_VARIABLE HOSTNAME_RESULT
    ERROR_VARIABLE HOSTNAME_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(DATE_OUTPUT)
execute_process(
    COMMAND date "+%Y-%m-%d %H:%M:%S"
    OUTPUT_VARIABLE DATE_OUTPUT
    RESULT_VARIABLE DATE_RESULT
    ERROR_VARIABLE DATE_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/Version.hxx.in
    ${CMAKE_CURRENT_BINARY_DIR}/Version.hxx
    @ONLY
)

# Dependencies
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CONFIGPP REQUIRED libconfig++)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# Source files
set(SOURCES
    src/Main.cxx
    src/SerialPort.cxx
    src/TelemetryStore.cxx
    src/UsageDatabase.cxx
    src/N1655Mcu.cxx
    src/Console.cxx
    src/TelnetServer.cxx
    src/Target.cxx
    src/N1655Target.cxx
    src/TargetFactory.cxx
    src/TargetManager.cxx
    src/Config.cxx
    src/WebServer.cxx
    src/ToolDispatcher.cxx
    src/UsbMatrixEmbedded.cxx
    src/FlashManager.cxx
    src/AimonGatewayClient.cxx
    src/StandaloneMcpServer.cxx
    src/InstanceLock.cxx
    src/XyModem.cxx
    src/TerminalSanitizer.cxx
)

add_executable(embdevenv ${SOURCES})

target_include_directories(embdevenv PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
    ${CONFIGPP_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
)

target_link_libraries(embdevenv PRIVATE
    Threads::Threads
    ${CONFIGPP_LIBRARIES}
    ${SQLITE3_LIBRARIES}
)

if(CONFIGPP_LIBRARY_DIRS)
    target_link_directories(embdevenv PRIVATE ${CONFIGPP_LIBRARY_DIRS})
endif()
if(SQLITE3_LIBRARY_DIRS)
    target_link_directories(embdevenv PRIVATE ${SQLITE3_LIBRARY_DIRS})
endif()

add_executable(test_xymodem
    test/TestXyModem.cxx
    src/XyModem.cxx
)
target_include_directories(test_xymodem PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(test_xymodem PRIVATE
    Threads::Threads
)

add_executable(test_terminal_sanitizer
    test/TestTerminalSanitizer.cxx
    src/TerminalSanitizer.cxx
)
target_include_directories(test_terminal_sanitizer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(test_terminal_sanitizer PRIVATE
    Threads::Threads
)

add_executable(test_mcu_polling
    test/TestMcuPolling.cxx
    src/N1655Mcu.cxx
    src/N1655Target.cxx
    src/Target.cxx
    src/TargetFactory.cxx
    src/Console.cxx
    src/SerialPort.cxx
    src/TelemetryStore.cxx
    src/TelnetServer.cxx
    src/TerminalSanitizer.cxx
    src/UsageDatabase.cxx
    src/Config.cxx
    src/XyModem.cxx
)
target_include_directories(test_mcu_polling PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${CONFIGPP_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
)
target_link_libraries(test_mcu_polling PRIVATE
    Threads::Threads
    ${CONFIGPP_LIBRARIES}
    ${SQLITE3_LIBRARIES}
)
if(CONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_mcu_polling PRIVATE ${CONFIGPP_LIBRARY_DIRS})
endif()
if(SQLITE3_LIBRARY_DIRS)
    target_link_directories(test_mcu_polling PRIVATE ${SQLITE3_LIBRARY_DIRS})
endif()

add_executable(test_serial_isolation
    test/TestSerialIsolation.cxx
    src/SerialPort.cxx
    src/Console.cxx
    src/TelnetServer.cxx
    src/TerminalSanitizer.cxx
    src/XyModem.cxx
    src/N1655Mcu.cxx
    src/TelemetryStore.cxx
    src/UsageDatabase.cxx
    src/Config.cxx
)
target_include_directories(test_serial_isolation PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${CONFIGPP_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
)
target_link_libraries(test_serial_isolation PRIVATE
    Threads::Threads
    ${CONFIGPP_LIBRARIES}
    ${SQLITE3_LIBRARIES}
)
if(CONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_serial_isolation PRIVATE ${CONFIGPP_LIBRARY_DIRS})
endif()
if(SQLITE3_LIBRARY_DIRS)
    target_link_directories(test_serial_isolation PRIVATE ${SQLITE3_LIBRARY_DIRS})
endif()


