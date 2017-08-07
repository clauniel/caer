IF (NOT LOCALFILTDOWNSAMPLE)
	SET(LOCALFILTDOWNSAMPLE 0 CACHE BOOL "Enable the local support and downsampling FPGA filter")
ENDIF()

IF (LOCALFILTDOWNSAMPLE)
    ADD_LIBRARY(localfiltdownsample SHARED localfiltdownsample.c)

    TARGET_LINK_LIBRARIES(localfiltdownsample ${CAER_C_LIBS})

ENDIF()

