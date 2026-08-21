# small3dlib is a single header containing its own implementations, so it has
# no source files of its own - it gets compiled into whichever component
# includes it (here: main).
idf_component_register(INCLUDE_DIRS "include")
