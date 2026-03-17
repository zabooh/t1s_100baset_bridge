set(DEPENDENT_MP_BIN2HEXT1S_100BaseT_Bridge_default_YP2gdPv0 "c:/Program Files/Microchip/xc32/v4.60/bin/xc32-bin2hex.exe")
set(DEPENDENT_DEPENDENT_TARGET_ELFT1S_100BaseT_Bridge_default_YP2gdPv0 ${CMAKE_CURRENT_LIST_DIR}/../../../../out/T1S_100BaseT_Bridge/default.elf)
set(DEPENDENT_TARGET_DIRT1S_100BaseT_Bridge_default_YP2gdPv0 ${CMAKE_CURRENT_LIST_DIR}/../../../../out/T1S_100BaseT_Bridge)
set(DEPENDENT_BYPRODUCTST1S_100BaseT_Bridge_default_YP2gdPv0 ${DEPENDENT_TARGET_DIRT1S_100BaseT_Bridge_default_YP2gdPv0}/${sourceFileNameT1S_100BaseT_Bridge_default_YP2gdPv0}.c)
add_custom_command(
    OUTPUT ${DEPENDENT_TARGET_DIRT1S_100BaseT_Bridge_default_YP2gdPv0}/${sourceFileNameT1S_100BaseT_Bridge_default_YP2gdPv0}.c
    COMMAND ${DEPENDENT_MP_BIN2HEXT1S_100BaseT_Bridge_default_YP2gdPv0} --image ${DEPENDENT_DEPENDENT_TARGET_ELFT1S_100BaseT_Bridge_default_YP2gdPv0} --image-generated-c ${sourceFileNameT1S_100BaseT_Bridge_default_YP2gdPv0}.c --image-generated-h ${sourceFileNameT1S_100BaseT_Bridge_default_YP2gdPv0}.h --image-copy-mode ${modeT1S_100BaseT_Bridge_default_YP2gdPv0} --image-offset ${addressT1S_100BaseT_Bridge_default_YP2gdPv0} 
    WORKING_DIRECTORY ${DEPENDENT_TARGET_DIRT1S_100BaseT_Bridge_default_YP2gdPv0}
    DEPENDS ${DEPENDENT_DEPENDENT_TARGET_ELFT1S_100BaseT_Bridge_default_YP2gdPv0})
add_custom_target(
    dependent_produced_source_artifactT1S_100BaseT_Bridge_default_YP2gdPv0 
    DEPENDS ${DEPENDENT_TARGET_DIRT1S_100BaseT_Bridge_default_YP2gdPv0}/${sourceFileNameT1S_100BaseT_Bridge_default_YP2gdPv0}.c
    )
