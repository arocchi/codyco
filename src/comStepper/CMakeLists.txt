# Copyright (C) 2010 RobotCub Consortium
# Author: Jorhabib Eljaik, Marco Randazzo
# CopyPolicy: Released under the terms of the GNU GPL v2.0.

cmake_minimum_required(VERSION 2.8)
SET(PROJECTNAME comStepper)
PROJECT(${PROJECTNAME})

find_package(YARP)
find_package(ICUB)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${YARP_MODULE_PATH})
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${ICUB_MODULE_PATH})


FILE(GLOB folder_source ./src/main.cpp ./src/comStepper.cpp ./src/rangeCheck.cpp ./src/util.cpp)
FILE(GLOB folder_header ./src/comStepper.h ./src/rangeCheck.h ./src/util.h)

SOURCE_GROUP("Source Files" FILES ${folder_source})
SOURCE_GROUP("Header Files" FILES ${folder_header})

include(iCubHelpers)
include(iCubOptions)

SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${ICUB_LINK_FLAGS}")

INCLUDE_DIRECTORIES(${iDyn_INCLUDE_DIRS}
                    ${ICUB_INCLUDE_DIRS}
                    ${YARP_INCLUDE_DIRS})

ADD_EXECUTABLE(${PROJECTNAME} ${folder_source} ${folder_header})

IF(EXPERIMENTAL_COMSTEPPER_HQP)
ADD_DEFINITIONS(-DEXPERIMENTAL_COMSTEPPER_HQP)
TARGET_LINK_LIBRARIES(${PROJECTNAME} iDyn qpOASES
	                             ${YARP_LIBRARIES})
ELSE(EXPERIMENTAL_COMSTEPPER_HQP)
TARGET_LINK_LIBRARIES(${PROJECTNAME} iDyn
                                     ${YARP_LIBRARIES})
ENDIF(EXPERIMENTAL_COMSTEPPER_HQP)


IF(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT) 
SET(CMAKE_INSTALL_PREFIX $ENV{ICUB_DIR} CACHE PATH "installation prefix" FORCE)
ENDIF(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
if(WIN32)
	INSTALL_TARGETS(/bin/Release ${PROJECTNAME})	
else(WIN32)
	INSTALL_TARGETS(/bin ${PROJECTNAME})
endif(WIN32)

add_subdirectory(app)
