idf_component_register(SRCS "hooks.c")

# Force the hook symbols into the bootloader link.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")
