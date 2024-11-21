message(STATUS "Looking for 3ds tools...")

################
## BANNERTOOL ##
################
if(NOT BANNERTOOL)
    # message(STATUS "Looking for bannertool...")
    find_program(BANNERTOOL bannertool ${DEVKITARM}/bin)
    if(BANNERTOOL)
        message(STATUS "bannertool: ${BANNERTOOL} - found")
    else()
        message(WARNING "bannertool - not found")
    endif()
endif()

#############
## MAKEROM ##
#############
if(NOT MAKEROM)
    # message(STATUS "Looking for makerom...")
    find_program(MAKEROM makerom ${DEVKITARM}/bin)
    if(MAKEROM)
        message(STATUS "makerom: ${MAKEROM} - found")
    else()
        message(WARNING "makerom - not found")
    endif()
endif()

function(__add_ncch_banner target IMAGE SOUND)
    if(IMAGE MATCHES ".*\\.png$")
        set(IMG_PARAM -i ${IMAGE})
    else()
        set(IMG_PARAM -ci ${IMAGE})
    endif()
    if(SOUND MATCHES ".*\\.wav$")
        set(SND_PARAM -a ${SOUND})
    else()
        set(SND_PARAM -ca ${SOUND})
    endif()
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target}
                        COMMAND ${BANNERTOOL} makebanner -o ${CMAKE_CURRENT_BINARY_DIR}/${target} ${IMG_PARAM} ${SND_PARAM}
                        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                        DEPENDS ${IMAGE} ${SOUND}
                        VERBATIM
    )
endfunction()

function(add_cia_target target RSF IMAGE SOUND )
    get_filename_component(target_we ${target} NAME_WE)
    if(${ARGC} EQUAL 5)
        string(REPLACE "." ";" VERSION_PARTS ${ARGV4})
        list(GET VERSION_PARTS 0 VERSION_MAJOR)
        list(GET VERSION_PARTS 1 VERSION_MINOR)
        list(GET VERSION_PARTS 2 VERSION_MICRO)
        math(EXPR VERSION_VALUE "${VERSION_MAJOR} * 1024 + ${VERSION_MINOR} * 16 + ${VERSION_MICRO}")
    endif()
    if(NOT VERSION_VALUE)
        set(VERSION_VALUE 0)
    endif()

    __add_ncch_banner(${target_we}.bnr ${IMAGE} ${SOUND})
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target_we}.cia
                        COMMAND ${MAKEROM}  -f cia
                                            -target t
                                            -exefslogo
                                            -o ${CMAKE_CURRENT_BINARY_DIR}/${target_we}.cia
                                            -elf ${CMAKE_CURRENT_BINARY_DIR}/${target_we}.elf
                                            -rsf ${RSF}
                                            -ver ${VERSION_VALUE}
                                            -banner ${CMAKE_CURRENT_BINARY_DIR}/${target_we}.bnr
                                            -icon ${CMAKE_CURRENT_BINARY_DIR}/${target_we}.smdh
                        DEPENDS ${target} ${RSF} ${CMAKE_CURRENT_BINARY_DIR}/${target_we}.bnr ${CMAKE_CURRENT_BINARY_DIR}/${target_we}.smdh
                        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                        VERBATIM
    )

    add_custom_target(${target_we}_cia ALL SOURCES ${CMAKE_CURRENT_BINARY_DIR}/${target_we}.cia)
endfunction()
