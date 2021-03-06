#Copyright 2016 Ramakrishnan Kannan

cmake_minimum_required(VERSION 3.6 FATAL_ERROR)

set(ARMA_USE_WRAPPER true)

# the settings below will be automatically configured by the rest of this script

project(NMF CXX)
set(NMFLIB_COMMON ${NMF_SOURCE_DIR}/../common)
set(NNLS_SRC ${NMF_SOURCE_DIR}/../nnls/)
message(STATUS "NMF_SOURCE_DIR =  ${NMF_SOURCE_DIR}")
message(STATUS "NMFLIB_COMMON =  ${NMFLIB_COMMON}")

include(${NMFLIB_COMMON}/cmake_aux/NMFLIB_FindARMA.cmake)

message(STATUS "CMAKE_CXX_FLAGS           = ${CMAKE_CXX_FLAGS}"          )
message(STATUS "CMAKE_SHARED_LINKER_FLAGS = ${CMAKE_SHARED_LINKER_FLAGS}")
message(STATUS "CMAKE_REQUIRED_INCLUDES   = ${CMAKE_REQUIRED_INCLUDES}"  )

message(STATUS "*** libraries:")
message(STATUS "*** NMFLIB_LIBS = ${NMFLIB_LIBS}")

include_directories(
  ${ARMADILLO_INCLUDE_DIR}
  ${ARMADILLO_INCLUDE_DIRS}
  ${NMF_SOURCE_DIR}
  ${NMF_SOURCE_DIR}/../
  ${NMFLIB_COMMON}
  ${NNLS_SRC}
  ${Boost_INCLUDE_DIR}
  ${MKL_INCLUDE_DIR}
  ${OPENBLAS_INCLUDE_DIR}
)

if(CMAKE_BUILD_SPARSE)
  set(DENSE_OR_SPARSE sparse)
else()
  set(DENSE_OR_SPARSE dense)
endif()

add_executable(${DENSE_OR_SPARSE}_nmf nmf.cpp)
target_link_libraries(${DENSE_OR_SPARSE}_nmf ${NMFLIB_LIBS})
install(TARGETS ${DENSE_OR_SPARSE}_nmf
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} )