cmake_minimum_required(VERSION 3.0)
project (dapmodvpn)
  
set(VPN_SRCS dap_stream_ch_vpn.c)

add_library(${PROJECT_NAME} STATIC ${VPN_SRCS})

target_link_libraries(dapmodvpn dap_core
    dap_crypto dap_core_server dap_stream)

target_include_directories(dapmodvpn INTERFACE .)
