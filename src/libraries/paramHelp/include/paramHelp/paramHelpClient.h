/**
 * \defgroup paramHelp paramHelp
 *
 * @ingroup codyco_libraries
 *
 * Classes for simplifying the management of the module parameters.
 *
 * \section dep_sec Dependencies
 * YARP
 *
 * \section intro_sec Description
 *
 *
 * \section tested_os_sec Tested OS
 *
 * Windows
 *
 * \author Andrea Del Prete - andrea.delprete@iit.it
 *
 * Copyright (C) 2013-.. CODYCO
 * CopyPolicy: Released under the terms of the GNU GPL v2.0.
 **/

#ifndef __PARAMHELPCLIENT_H__
#define __PARAMHELPCLIENT_H__

#include <yarp/sig/Vector.h>
#include <yarp/os/Semaphore.h>
#include <yarp/os/BufferedPort.h>
#include <string>
#include <sstream>
#include <iostream>
#include <iterator>
#include <map>
#include <vector>
#include <paramHelp/paramHelp.h>


namespace paramHelp
{

// *************************************************************************************************
// Parameter helper, client side.
// To use this class you need an initialization phase, which consists of the following steps:
// 1) Instantiate an object of this class specifying the parameters and the commands of the module
// 2) Link each parameter to a variable, calling the method 'linkParam'
// 3) Call the method 'init' (actually it can be called at any moment)
//
// After the initializion, you can use this class in these ways:
// 1) To set a parameter through an rpc message call the method 'setRpcParam'
// 2) To get a parameter through an rpc message call the method 'getRpcParam'
// 3) To send an rpc command call the method 'sendRpcCommand'
// 4) To send the input streaming parameters call the method 'sendStreamParams'
// 5) To read the output streaming parameters call the method 'readStreamParams'
// 6) To read sporadic info messages (about the module status) call the method 'readInfoMessage'
// *************************************************************************************************
class ParamHelperClient: public ParamHelperBase
{
protected:
    yarp::os::Port                              portRpc;        // port for rpc messages

public:
    /** Constructor.
      * @param pdList List of the module parameters
      * @param pdListSize
      * @param cdList List of the module commands
      * @param cdListSize
      */
    ParamHelperClient(const ParamDescription *pdList=0, int pdListSize=0, const CommandDescription *cdList=0, int cdListSize=0);

    // Destructor
    ~ParamHelperClient();

    /** Open 4 ports:
      * - "/localName/remoteName/rpc": Rpc Port for synchronous set/get operations on module parameters
      * - "/localName/remoteName/info:i": Input Port for sporadic message regarding the module status
      * - "/localName/remoteName/stream:o": Output BufferedPort<Bottle> for asynchronous streaming data
      * - "/localName/remoteName/stream:i": Input BufferedPort<Bottle> for asynchronous streaming data 
      * Then it tries to connect these ports to the corresponding remote ports.
      * @param localName Name of this module, used as stem for all the port names 
      * @param remoteName Name of the remote module, with which you want to communicate
      * @param reply Output bottle containing error messages, if any
      * @return True if the operation succeeded, false otherwise. */
    bool init(std::string localName, std::string remoteName, yarp::os::Bottle &reply);

    /** Close the ports opened during the initialization phase (see init method). */
    bool close();

    /** Send an rpc command to set the value of the specified parameter
     * @param paramId Id of the parameter to set
     * @return bool True if the operation succeeded, false otherwise. */
    bool setRpcParam(int paramId);

    /** Send an rpc command to get the value of the specified parameter
     * @param paramId Id of the parameter to get
     * @return bool True if the operation succeeded, false otherwise. */
    bool getRpcParam(int paramId);

    bool sendRpcCommand(int cmdId);

    /** Read a message from the info port.
      * @param b Message read
      * @return True if the operation succeeded, false otherwise */
    bool readInfoMessage(yarp::os::Bottle &b, bool blockingRead=false);

    /** Send the input streaming parameters.
      * @return True if the operation succeeded, false otherwise */
    bool sendStreamParams();

    /** Read the output streaming parameters.
      * @param blockingRead If true the reading is blocking (it waits until data arrive), otherwise it is not
      * @return True if the operation succeeded, false otherwise */
    bool readStreamParams(bool blockingRead=false);
};
    
}//end namespace paramHelp


#endif



