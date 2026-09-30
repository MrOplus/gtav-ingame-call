# Usage: cmake -DSRC=<file> -DDST=<dir> -P copy_if_missing.cmake
get_filename_component(_name "${SRC}" NAME)
if(NOT EXISTS "${DST}/${_name}")
    file(COPY "${SRC}" DESTINATION "${DST}")
endif()
