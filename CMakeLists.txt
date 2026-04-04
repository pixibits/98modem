cmake_minimum_required(VERSION 3.20)
project(vmodem C)

set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-W -Wall -Wno-unknown-pragmas)

add_library(modem_core
    src/common/modem_core.c
)

target_include_directories(modem_core PUBLIC include)

add_executable(modem_core_tests
    tests/modem_core_tests.c
)

target_link_libraries(modem_core_tests modem_core)

add_executable(helper_net_tests
    tests/helper_net_tests.c
    src/helper/net_common.c
)

target_include_directories(helper_net_tests PRIVATE include src/helper)

add_executable(helper_telnet_tests
    tests/helper_telnet_tests.c
    src/helper/telnet_proto.c
)

target_include_directories(helper_telnet_tests PRIVATE include src/helper)

add_executable(helper_peer_tests
    tests/helper_peer_tests.c
    src/helper/vmodem_peer.c
)

target_include_directories(helper_peer_tests PRIVATE include src/helper)

add_executable(dos_uart_tests
    tests/dos_uart_tests.c
    src/vxd/dos_uart.c
)

target_include_directories(dos_uart_tests PRIVATE include src/vxd)
target_link_libraries(dos_uart_tests modem_core)

enable_testing()
if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME modem_core_tests COMMAND modem_core_tests)
    add_test(NAME helper_net_tests COMMAND helper_net_tests)
    add_test(NAME helper_telnet_tests COMMAND helper_telnet_tests)
    add_test(NAME helper_peer_tests COMMAND helper_peer_tests)
    add_test(NAME dos_uart_tests COMMAND dos_uart_tests)
endif()

if(WIN32 OR MINGW)
    target_compile_options(modem_core PRIVATE -m32)
    target_compile_options(modem_core_tests PRIVATE -m32)
    target_compile_options(helper_net_tests PRIVATE -m32)
    target_compile_options(helper_telnet_tests PRIVATE -m32)
    target_compile_options(helper_peer_tests PRIVATE -m32)
    target_compile_options(dos_uart_tests PRIVATE -m32)
    target_link_options(modem_core_tests PRIVATE -m32 -mconsole)
    target_link_options(helper_net_tests PRIVATE -m32 -mconsole)
    target_link_options(helper_telnet_tests PRIVATE -m32 -mconsole)
    target_link_options(helper_peer_tests PRIVATE -m32 -mconsole)
    target_link_options(dos_uart_tests PRIVATE -m32 -mconsole)

    add_executable(vmodem_helper
        src/helper/helper_main.c
        src/helper/ipc_helper.c
        src/helper/net_common.c
        src/helper/net_winsock.c
        src/helper/session_log.c
        src/helper/telnet_proto.c
        src/helper/vmodem_peer.c
    )

    target_include_directories(vmodem_helper PRIVATE include src/helper)
    target_compile_options(vmodem_helper PRIVATE -m32)
    target_link_options(vmodem_helper PRIVATE -m32 -mwindows)
    target_link_libraries(vmodem_helper kernel32 user32 gdi32 shell32 comctl32 wsock32)

    add_executable(vmodem_diag
        src/helper/diag_main.c
        src/helper/ipc_helper.c
    )

    target_include_directories(vmodem_diag PRIVATE include)
    target_compile_options(vmodem_diag PRIVATE -m32)
    target_link_options(vmodem_diag PRIVATE -m32 -mconsole)
    target_link_libraries(vmodem_diag kernel32 user32 advapi32)

    configure_file(src/helper/vmodem.ini
                   ${CMAKE_CURRENT_BINARY_DIR}/vmodem.ini
                   COPYONLY)
endif()
