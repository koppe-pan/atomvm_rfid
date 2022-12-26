set(ATOMVM_RFID_COMPONENT_SRCS
    "ports/atomvm_rfid.c"
)

idf_component_register(
    SRCS ${ATOMVM_RFID_COMPONENT_SRCS}
    INCLUDE_DIRS "ports/include"
    PRIV_REQUIRES "libatomvm" "avm_sys" "rc522"
)

idf_build_set_property(
    LINK_OPTIONS "-Wl,--whole-archive ${CMAKE_CURRENT_BINARY_DIR}/lib${COMPONENT_NAME}.a -Wl,--no-whole-archive"
    APPEND
)
