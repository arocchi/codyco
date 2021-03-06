# Copyright: (C) 2013 RobotCub Consortium
# Authors: Silvio Traversaro
# CopyPolicy: Released under the terms of the GNU GPL v2.0 (or any later version).

cmake_minimum_required( VERSION 2.8 FATAL_ERROR )


SET(PROJECTNAME iDynTree)

PROJECT(${PROJECTNAME})

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")



find_package(YARP REQUIRED)
find_package(ICUB REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(Orocos-KDL REQUIRED)
find_package(kdl_codyco REQUIRED)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${YARP_MODULE_PATH})
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${ICUB_MODULE_PATH})

SET(folder_source src/DynTree.cpp
                  src/yarp_kdl.cpp
                  src/iCubTree.cpp
                  src/iDyn2KDL.cpp
                  src/idyn2kdl_icub.cpp)

SET(folder_header include/iCub/iDynTree/DynTree.h
                  include/iCub/iDynTree/yarp_kdl.h
                  include/iCub/iDynTree/iDyn2KDL.h
                  include/iCub/iDynTree/iCubTree.h
                  include/iCub/iDynTree/idyn2kdl_icub.h)

SOURCE_GROUP("Source Files" FILES ${folder_source})
SOURCE_GROUP("Header Files" FILES ${folder_header})


INCLUDE_DIRECTORIES(include
                    ${skinDynLib_INCLUDE_DIRS}
                    ${iDyn_INCLUDE_DIRS}
                    ${GSL_INCLUDE_DIRS}
                    ${YARP_INCLUDE_DIRS}
                    ${Orocos-KDL_INCLUDE_DIRS}
                    ${kdl_codyco_INCLUDE_DIRS}
                    ${EIGEN3_INCLUDE_DIR})

ADD_LIBRARY(${PROJECTNAME} ${folder_source} ${folder_header})

TARGET_LINK_LIBRARIES(${PROJECTNAME} iDyn
                                     ${ICUB_LIBRARIES}
                                     ${GSL_LIBRARIES}
                                     ${YARP_LIBRARIES}
                                     ${Orocos-KDL_LIBRARIES} 
                                     ${kdl_codyco_LIBRARIES})



INSTALL(TARGETS iDynTree EXPORT iDynTargets-install DESTINATION lib)
INSTALL(DIRECTORY include/iCub/iDynTree DESTINATION include/iCub)
#install(EXPORT iDynTargets-install DESTINATION share/${PROJECT_NAME}/cmake )
set(PKG_NAME ${PROJECT_NAME})
set(PKG_LIBRARIES iDynTree)
set(PKG_DEPENDS ICUB)
set(cmake_conf_file "${CMAKE_CURRENT_SOURCE_DIR}/cmake/iDynTreeConfig.cmake")
configure_file("${cmake_conf_file}.in" "${cmake_conf_file}" @ONLY)
install(FILES ${cmake_conf_file} DESTINATION share/${PROJECT_NAME}/cmake/ COMPONENT cmake)

## Make the package config file
#if (NOT MSVC)
#  set(PKG_DESC "iDynTree")
#  set(PKG_DEPENDS "Orocos-KDL eigen3") # make the list separated by spaces instead of ;
#  set(PKG_LIBS "")
#  set(pkg_conf_file "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pkgconfig/iDynTree.pc")
#  configure_file("${pkg_conf_file}.in" "${pkg_conf_file}" @ONLY)
#  install(FILES ${pkg_conf_file} DESTINATION lib/pkgconfig/ COMPONENT pkgconfig)
#endif()

icub_export_library(${PROJECTNAME} INTERNAL_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
                                   DESTINATION include/iCub/iDynTree
                                   FILES ${folder_header})


message(STATUS "Configuration successful. Type make to compile iDynTree")

