cmake_minimum_required(VERSION 3.0.0)
project(firesnes VERSION 0.1.0)

include(CTest)
enable_testing()



add_executable(firesnes
    main.cpp
    m65816.cpp
    m65816_addressing.cpp
    m65816_emitter.cpp
    m65816_utils.cpp
    ir_interpreter.cpp
)

set_property(TARGET firesnes PROPERTY CXX_STANDARD 17)
set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
