cmake_minimum_required(VERSION 3.0)
project (dap_stream_ch_vpn)
  
set(VPN_SRCS dap_stream_ch_vpn.c)

add_library(${PROJECT_NAME} STATIC ${VPN_SRCS})

target_link_libraries(dap_stream_ch_vpn dap_core
    dap_crypto dap_stream)

target_include_directories(dap_stream_ch_vpn INTERFACE .)
