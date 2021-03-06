/*
 * Copyright (C) 2013 IIT - Istituto Italiano di Tecnologia
 * Author: Silvio Traversaro
 * CopyPolicy: Released under the terms of the GNU LGPL v2.0 (or any later version)
 *
 */

#include <iCub/iDynTree/DynTree.h>

#include <iCub/iDynTree/yarp_kdl.h>

//Loops from KDL_CoDyCo
#include <kdl_codyco/position_loops.hpp>
#include <kdl_codyco/rnea_loops.hpp>
#include <kdl_codyco/regressor_loops.hpp>
#include <kdl_codyco/jacobian_loops.hpp>
#include <kdl_codyco/com_loops.hpp>
#include <kdl_codyco/utils.hpp>

#ifndef NDEBUG
#include <kdl/frames_io.hpp>
#endif

#include <yarp/math/Math.h>
#include <yarp/math/SVD.h>

#include <vector>

using namespace yarp::sig;
using namespace yarp::math;

/**
 * 
 * 
 * 
 * 
 * \todo implement range checking and part handling
 * \todo iDynTreeContact solve dynamic allocation of regressor matrix for contacts
 * 
 */ 
 
namespace iCub {
namespace iDynTree {
    
DynTree::DynTree()
{
}

DynTree::DynTree(const KDL::Tree & _tree, 
                   const std::vector<std::string> & joint_sensor_names,
                   const std::string & imu_link_name,
                   KDL::CoDyCo::TreeSerialization serialization,
                   KDL::CoDyCo::TreePartition _partition
                   ):  tree_graph(_tree,serialization,_partition)
{constructor(_tree,joint_sensor_names,imu_link_name,serialization,_partition);
}

void DynTree::constructor(const KDL::Tree & _tree, 
                          const std::vector<std::string> & joint_sensor_names,
                      const std::string & imu_link_name,
                      KDL::CoDyCo::TreeSerialization serialization,
                      KDL::CoDyCo::TreePartition _partition,
                      std::vector<KDL::Frame> child_sensor_transforms
)
{
int ret;
    
    tree_graph = KDL::CoDyCo::TreeGraph(_tree,serialization,_partition);  
    partition = _partition;
    
    //Setting useful constants
    NrOfDOFs = _tree.getNrOfJoints();
    NrOfLinks = _tree.getNrOfSegments();
    NrOfFTSensors = joint_sensor_names.size();
    NrOfDynamicSubGraphs = NrOfFTSensors + 1;
    
    assert((int)tree_graph.getNrOfDOFs() == NrOfDOFs);
    assert((int)tree_graph.getNrOfLinks() == NrOfLinks);
    
    world_base_frame = KDL::Frame::Identity();
    
    q = KDL::JntArray(NrOfDOFs);
    
    is_X_dynamic_base_updated = false;
    
    dq = KDL::JntArray(NrOfDOFs);
    ddq = KDL::JntArray(NrOfDOFs);
    
    torques = KDL::JntArray(NrOfDOFs);
    
    q_min = KDL::JntArray(NrOfDOFs);
    q_max = KDL::JntArray(NrOfDOFs);
    constrained = std::vector<bool>(NrOfDOFs,false);
    constrained_count = 0;
    
    kinematic_traversal = KDL::CoDyCo::Traversal();
    dynamic_traversal = KDL::CoDyCo::Traversal();
    
    measured_wrenches =  std::vector< KDL::Wrench >(NrOfFTSensors);
    
    v = std::vector<KDL::Twist>(NrOfLinks);
    a = std::vector<KDL::Twist>(NrOfLinks);
    
    f = std::vector<KDL::Wrench>(NrOfLinks); /**< wrench trasmitted by a link to the predecessor (note that predecessor definition depends on the selected dynamic base */
        
    f_ext = std::vector<KDL::Wrench>(NrOfLinks,KDL::Wrench::Zero());     
        
    //Create default kinematic traversal (if imu_names is wrong, creating the default one)   
    ret = tree_graph.compute_traversal(kinematic_traversal,imu_link_name);
    
    if( ret < 0 ) { std::cerr << "iDynTree constructor: imu_link_name not found" << std::endl; }
    assert(ret == 0);
    
    //Create default dynamics traversal (the dynamical base is the default one of the KDL::Tree)
    //Note that the selected dynamical base affect only the "classical" use case, when unkown external
    //wrenches are not estimated and are assume acting on the base. 
    //If the external forces are properly estimated, the base link for dynamic loop should not 
    //affect the results (i.e. 
    ret = tree_graph.compute_traversal(dynamic_traversal);
    assert(ret == 0);
    
    //iDynTreeContact
    ret = buildSubGraphStructure(joint_sensor_names);
    if( ret != 0 ) { std::cerr << "iDynTree constructor: ft sensor specified not found" << std::endl; }
    
    //building matrix and vectors for each subgraph
    A_contacts.resize(NrOfDynamicSubGraphs);
    b_contacts.resize(NrOfDynamicSubGraphs);
    
    b_contacts_subtree.resize(NrOfLinks);
    
    //end iDynTreeContact

    
    assert(ret == 0);
}

DynTree::~DynTree() { } ;

double DynTree::setAng(const double q_in, const int i)
{
    is_X_dynamic_base_updated = false;
    
    if (constrained[i]) {
        q(i) = (q_in<q_min(i)) ? q_min(i) : ((q_in>q_max(i)) ? q_max(i) : q_in);
    } else {
        q(i) = q_in;
    }
    return q(i);
}


//====================================
//
//      iDynTreeContact methods
//
//====================================

bool DynTree::isFTsensor(const std::string & joint_name, const std::vector<std::string> & ft_sensors) const
{
    if (std::find(ft_sensors.begin(), ft_sensors.end(), joint_name) != ft_sensors.end())
    {
        return true;
    }
    //else 
    {
        return false;
    }
}

int DynTree::buildSubGraphStructure(const std::vector<std::string> & ft_names)
{
    link2subgraph_index.resize(NrOfLinks,-1);
    link_is_subgraph_root.resize(NrOfLinks,false);
    subgraph_index2root_link.resize(NrOfDynamicSubGraphs,-1);
    
    int next_id = 0;
    
    for(int i=0; i < (int)dynamic_traversal.order.size(); i++) {
        
        KDL::CoDyCo::LinkMap::const_iterator link_it = dynamic_traversal.order[i];
        int link_nmbr = link_it->link_nr; 
        
        if( i == 0 ) {
            
            //Starting with the dynamical base link, assign an index to the subgraph
            assert( dynamic_traversal.parent[link_nmbr] == tree_graph.getInvalidLinkIterator() );
            link2subgraph_index[link_nmbr] = next_id;
            
            //The dynamical base link is the root of its subgraph
            link_is_subgraph_root[link_nmbr] = true;
            subgraph_index2root_link[link2subgraph_index[link_nmbr]] = link_nmbr;
            
            next_id++;
            
        } else {
            //For every link, the subgraph is the same of its parent, unless it is connected to it by an FT sensor
            KDL::CoDyCo::LinkMap::const_iterator parent_it = dynamic_traversal.parent[link_it->link_nr];
            int parent_nmbr = parent_it->link_nr;
            
            if( isFTsensor(link_it->getAdjacentJoint(parent_it)->joint.getName(),ft_names) ) {
                //The FT sensor should be a fixed joint ? probably not
                //assert(link_it->getAdjacentJoint(parent_it)->joint.getType() == Joint::None);

                link2subgraph_index[link_nmbr] = next_id;
                
                //This link is a root of a dynamical subgraph, as its parent is in another subgraph
                link_is_subgraph_root[link_nmbr] = true;
                subgraph_index2root_link[link2subgraph_index[link_nmbr]] = link_nmbr;

                next_id++;
            } else {
                link2subgraph_index[link_nmbr] = link2subgraph_index[parent_nmbr];
                
                //This link is not a root of a dynamical subgraph
                link_is_subgraph_root[link_nmbr] = false;
            }
        }
    }
    
    //Building Force/Torque sensors data structures
    ft_list = KDL::CoDyCo::FTSensorList(tree_graph,ft_names);

    //The numbers of ids must be equal to the number of subgraphs
    if(next_id == NrOfDynamicSubGraphs) {
        return 0;
    } else {
        assert(false);
        return -1;
    }
}

KDL::Wrench DynTree::getMeasuredWrench(int link_id)
{
  return ft_list.getMeasuredWrench(link_id,measured_wrenches);
}

void DynTree::buildAb_contacts()
{
    //First calculate the known terms b related to inertial, gravitational and 
    //measured F/T 
    
    for(int l=dynamic_traversal.order.size()-1; l>=0; l-- ) {  
            KDL::CoDyCo::LinkMap::const_iterator link_it = dynamic_traversal.order[l];
            int link_nmbr = link_it->link_nr;
            //Collect RigidBodyInertia and external forces
            KDL::RigidBodyInertia Ii= link_it->I;
            //This calculation should be done one time in forward kineamtic loop and stored \todo
            b_contacts_subtree[link_nmbr] = Ii*a[link_nmbr]+v[link_nmbr]*(Ii*v[link_nmbr]) - getMeasuredWrench(link_nmbr);
    }
    

    for(int l=dynamic_traversal.order.size()-1; l>=0; l-- ) {
            
        KDL::CoDyCo::LinkMap::const_iterator link_it = dynamic_traversal.order[l];
        int link_nmbr = link_it->link_nr;

        if( l != 0 ) {

            KDL::CoDyCo::LinkMap::const_iterator parent_it = dynamic_traversal.parent[link_nmbr];
            const int parent_nmbr = parent_it->link_nr;
            //If this link is a subgraph root, store the result, otherwise project it to the parent
            if( !isSubGraphRoot(link_nmbr) ) {
                double joint_pos;

            KDL::CoDyCo::JunctionMap::const_iterator joint_it = link_it->getAdjacentJoint(parent_it);
                if( joint_it->joint.getType() == KDL::Joint::None ) {
                    joint_pos = 0.0;
                } else {
                    joint_pos = q(link_it->getAdjacentJoint(parent_it)->q_nr);
                }
             

                b_contacts_subtree[parent_nmbr] += link_it->pose(parent_it,joint_pos)*b_contacts_subtree[link_nmbr]; 
            } else {
                b_contacts[getSubGraphIndex(link_nmbr)].setSubvector(0,KDLtoYarp(b_contacts_subtree[link_nmbr].torque));
                b_contacts[getSubGraphIndex(link_nmbr)].setSubvector(3,KDLtoYarp(b_contacts_subtree[link_nmbr].force));
            }
       }
    }
    
    //Then calculate the A and b related to unknown contacts
    
    iCub::skinDynLib::dynContactList::const_iterator it;
    
    std::vector<int> unknowns(NrOfDynamicSubGraphs,0);
    
    //Calculate the number of unknowns for each subgraph
    for(int sg=0; sg < NrOfDynamicSubGraphs; sg++ ) {
        for(it = contacts[sg].begin(); it!=contacts[sg].end(); it++)
        {
            if(it->isMomentKnown())
            {
                if(it->isForceDirectionKnown()) 
                {
                    unknowns[sg]++;     // 1 unknown (force module)
                }
                else
                {
                    unknowns[sg]+=3;    // 3 unknowns (force)
                }
            }
            else
            {
                unknowns[sg]+=6;        // 6 unknowns (force and moment)
            }
        }
    }
    
    
    for(int sg=0; sg < NrOfDynamicSubGraphs; sg++ ) {
        //Resize the A matrices
        A_contacts[sg] = yarp::sig::Matrix(6,unknowns[sg]);
        
        //Calculate A and b related to contacts
        int colInd = 0;
        for( it = contacts[sg].begin(); it!=contacts[sg].end(); it++)
        {
            //get link index
            int body_part = it->getBodyPart();
            int local_link_index = it->getLinkNumber();
            int link_contact_index = partition.getGlobalLinkIndex(body_part,local_link_index);
            
            //Subgraph information
            int subgraph_index = link2subgraph_index[link_contact_index];
            
            assert(subgraph_index == sg);
            
            int subgraph_root = subgraph_index2root_link[subgraph_index];
            
            //Get Frame transform between contact link and subgraph root
            //Inefficient but leads to cleaner code, if necessary can be improved 
            KDL::Frame H_root_link = getFrameLoop(tree_graph,q,dynamic_traversal,subgraph_root,link_contact_index);
            
            KDL::Vector COP;
            YarptoKDL(it->getCoP(),COP); 
            
            KDL::Frame H_link_contact = KDL::Frame(COP);
            KDL::Frame H_root_contact = H_root_link*H_link_contact;
            
            if(it->isForceDirectionKnown())
            {   
                // 1 UNKNOWN: FORCE MODULE
                yarp::sig::Matrix un(6,1);
                un.zero();
                un.setSubcol(it->getForceDirection(),3,0); // force direction unit vector
                yarp::sig::Matrix H_adj_root_contact = KDLtoYarp_wrench(H_root_contact);
                yarp::sig::Matrix col = H_adj_root_contact*un;
                A_contacts[sg].setSubmatrix(col,0,colInd);
                colInd += 1;
               
            }
            else
            {   
                if( it->isMomentKnown() ) {
                    // 3 UNKNOWNS: FORCE
                    A_contacts[sg].setSubmatrix(KDLtoYarp_wrench(H_root_contact).submatrix(0,5,3,5),0,colInd);
                    colInd += 3;
                    
                } else {                       
                    // 6 UNKNOWNS: FORCE AND MOMENT
                    A_contacts[sg].setSubmatrix(KDLtoYarp_wrench(H_root_contact),0,colInd);
                    colInd += 6;
                }
                
            }
        }
    }


}

void DynTree::store_contacts_results()
{
    //Make sure that the external forces are equal to zero before storing the results
    for(int l=dynamic_traversal.order.size()-1; l>=0; l-- ) {  
        f_ext[dynamic_traversal.order[l]->link_nr] = KDL::Wrench::Zero();
    }

    for(int sg=0; sg < NrOfDynamicSubGraphs; sg++ ) {
        unsigned int unknownInd = 0;
        iCub::skinDynLib::dynContactList::iterator it;
        for(it = contacts[sg].begin(); it!=contacts[sg].end(); it++)
        {

            //Store the result in dynContactList, for output
            if(it->isForceDirectionKnown()) {
                //1 UNKNOWN
                it->setForceModule( x_contacts[sg](unknownInd++));
            }
            else
            {
                if(it->isMomentKnown())
                {
                    //3 UNKNOWN
                    it->setForce(x_contacts[sg].subVector(unknownInd, unknownInd+2));
                    unknownInd += 3;                
                } else {
                    //6 UNKNOWN
                    it->setMoment(x_contacts[sg].subVector(unknownInd, unknownInd+2));
                    unknownInd += 3;
                    it->setForce(x_contacts[sg].subVector(unknownInd, unknownInd+2));
                    unknownInd += 3;

                }
            }
            
            //Store the results in f_ext, for RNEA dynamic loop
            KDL::Vector COP, force, moment;
            YarptoKDL(it->getCoP(),COP);
            KDL::Frame H_link_contact = KDL::Frame(COP);
            YarptoKDL(it->getForce(),force);
            YarptoKDL(it->getMoment(),moment);
            
            KDL::Wrench f_contact = KDL::Wrench(force,moment);
            //Get global link index from part ID and local index
            int link_nmbr = partition.getGlobalLinkIndex(it->getBodyPart(),it->getLinkNumber());
            f_ext[link_nmbr] = H_link_contact*f_contact;
        }
    }
}

//====================================
//
//      Set/Get methods
//
//====================================
bool DynTree::setWorldBasePose(const yarp::sig::Matrix & H_w_b)
{
    if ((H_w_b.rows()==4) && (H_w_b.cols()==4))
    {
        return YarptoKDL(H_w_b,world_base_frame);
    }
    else
    {
        if (verbose)
            std::cerr << "Attempt to reference a wrong matrix H_w_p (not 4x4)" << std::endl;

        return false;
    }
}

yarp::sig::Matrix DynTree::getWorldBasePose()
{
    KDLtoYarp_position(world_base_frame,_H_w_b);
    return _H_w_b;
}


yarp::sig::Vector DynTree::setAng(const yarp::sig::Vector & _q, const std::string & part_name) 
{
    is_X_dynamic_base_updated = false;
    
    yarp::sig::Vector ret_q = _q;
    
    if( part_name.length() ==  0 ) 
    { 
        //No part specified
        if( (int)_q.size() != NrOfDOFs ) { std::cerr << "setAng: Input vector has a wrong number of elements" << std::endl; return yarp::sig::Vector(0); } 
        if( constrained_count == 0 ) {
            //if all are not constrained, use a quicker way to copy
            YarptoKDL(_q,q);
        } else {
            for(int i =0; i < NrOfDOFs; i++ ){
                ret_q[i] = setAng(_q[i],i);
            }
        }
        
    } else { // if part_name.length > 0 
        std::vector<int> dof_ids;
        dof_ids = partition.getPartDOFIDs(part_name);
        if( dof_ids.size() != _q.size() ) { std::cerr << "setAng: Input vector has a wrong number of elements (or part_name wrong)" << std::endl; return yarp::sig::Vector(0); }
        for(int i = 0; i < (int)dof_ids.size(); i++ ) {
            ret_q[i] = setAng(_q[i],dof_ids[i]);
        }
    }
    return ret_q;
}

yarp::sig::Vector DynTree::getAng(const std::string & part_name) const
{    
    yarp::sig::Vector ret;
    if( part_name.length() == 0 ) 
    {
        KDLtoYarp(q,ret);
    } else {
        const std::vector<int> & dof_ids = partition.getPartDOFIDs(part_name);
        if( dof_ids.size() ==0  ) { std::cerr << "getAng: wrong part_name (or part with 0 DOFs)" << std::endl; return yarp::sig::Vector(0); }
        ret.resize(dof_ids.size());
        for(int i = 0; i < (int)dof_ids.size(); i++ ) {
            ret[i] = q(dof_ids[i]);
        }
    }
    return ret;
}

yarp::sig::Vector DynTree::setDAng(const yarp::sig::Vector & _q, const std::string & part_name)
{
    if( part_name.length() == 0 ) {
        if( (int)_q.size() != NrOfDOFs  ) { std::cerr << "setDAng: Input vector has a wrong number of elements" << std::endl; return yarp::sig::Vector(0); } 
        YarptoKDL(_q,dq);
    } 
    else 
    {
        const std::vector<int> & dof_ids = partition.getPartDOFIDs(part_name);
        if( dof_ids.size() != _q.size() ) { std::cerr << "setDAng: Input vector has a wrong number of elements (or part_name wrong)" << std::endl; return yarp::sig::Vector(0); }
        for(int i = 0; i < (int)dof_ids.size(); i++ ) {
            dq(dof_ids[i]) = _q[i];
        }
    }
    return _q;
}

yarp::sig::Vector DynTree::getDAng(const std::string & part_name) const
{
    yarp::sig::Vector ret;
    if( part_name.length() == 0 ) 
    {
        KDLtoYarp(dq,ret);
    } else {
        const std::vector<int> & dof_ids = partition.getPartDOFIDs(part_name);
        if( dof_ids.size() ==0  ) { std::cerr << "getDAng: wrong part_name (or part with 0 DOFs)" << std::endl; return yarp::sig::Vector(0); }
        ret.resize(dof_ids.size());
        for(int i = 0; i < (int)dof_ids.size(); i++ ) {
            ret[i] = dq(dof_ids[i]);
        }
    }
    return ret;

}

yarp::sig::Vector DynTree::getDQ_fb() const
{
    /**
     * 
     * \todo checking it is possible to return something which have sense
     */
    return cat(getVel(dynamic_traversal.order[0]->getLinkIndex()),getDAng());
}

yarp::sig::Vector DynTree::setD2Ang(const yarp::sig::Vector & _q, const std::string & part_name)
{
    if( part_name.length() == 0 ) {
        if( (int)_q.size() != NrOfDOFs  ) { std::cerr << "setD2Ang: Input vector has a wrong number of elements" << std::endl; return yarp::sig::Vector(0); } 
        YarptoKDL(_q,ddq);
    } 
    else 
    {
        const std::vector<int> & dof_ids = partition.getPartDOFIDs(part_name);
        if( dof_ids.size() != _q.size() ) { std::cerr << "setD2Ang: Input vector has a wrong number of elements (or part_name wrong)" << std::endl; return yarp::sig::Vector(0); }
        for(int i = 0; i < (int)dof_ids.size(); i++ ) {
            ddq(dof_ids[i]) = _q[i];
        }
    }
    return _q;
}

yarp::sig::Vector DynTree::getD2Ang(const std::string & part_name) const
{
    yarp::sig::Vector ret;
    if( part_name.length() == 0 ) 
    {
        KDLtoYarp(ddq,ret);
    } else {
        const std::vector<int> & dof_ids = partition.getPartDOFIDs(part_name);
        if( dof_ids.size() ==0  ) { std::cerr << "getD2Ang: wrong part_name (or part with 0 DOFs)" << std::endl; return yarp::sig::Vector(0); }
        ret.resize(dof_ids.size());
        for(int i = 0; i < (int)dof_ids.size(); i++ ) {
            ret[i] = ddq(dof_ids[i]);
        }
    }
    return ret;
}

bool DynTree::setInertialMeasure(const yarp::sig::Vector &w0, const yarp::sig::Vector &dw0, const yarp::sig::Vector &ddp0)
{
    KDL::Vector w0_kdl, dw0_kdl, ddp0_kdl;
    YarptoKDL(w0,w0_kdl);
    YarptoKDL(dw0,dw0_kdl);
    YarptoKDL(ddp0,ddp0_kdl);
    imu_velocity.vel = KDL::Vector::Zero();
    imu_velocity.rot = w0_kdl;
    imu_acceleration.vel = ddp0_kdl;
    imu_acceleration.rot = dw0_kdl;
    return true;
}

bool DynTree::getInertialMeasure(yarp::sig::Vector &w0, yarp::sig::Vector &dw0, yarp::sig::Vector &ddp0) const
{
    //should care about returning the 3d acceleration instead of the spatial one? 
    //assuming the base linear velocity as 0, they are the same
    KDLtoYarp(imu_velocity.rot,w0);
    KDLtoYarp(imu_acceleration.vel,ddp0);
    KDLtoYarp(imu_acceleration.rot,dw0);
    return true;
}

bool DynTree::setSensorMeasurement(const int sensor_index, const yarp::sig::Vector &ftm)
{
    if( sensor_index < 0 || sensor_index > (int)measured_wrenches.size() ) { return false; }
    if( ftm.size() != 6 ) { return false; }
    YarptoKDL(ftm.subVector(0,2),measured_wrenches[sensor_index].force);
    YarptoKDL(ftm.subVector(3,5),measured_wrenches[sensor_index].torque);
    
    are_contact_estimated = false;
    
    return true;
}

bool DynTree::getSensorMeasurement(const int sensor_index, yarp::sig::Vector &ftm) const
{
    //\todo avoid unnecessary memory allocation
    yarp::sig::Vector force_yarp(3);
    yarp::sig::Vector torque_yarp(3);
    if( sensor_index < 0 || sensor_index > (int)measured_wrenches.size() ) { return false; }
    if( ftm.size() != 6 ) { ftm.resize(6); }
    KDLtoYarp(measured_wrenches[sensor_index].force,force_yarp);
    KDLtoYarp(measured_wrenches[sensor_index].torque,torque_yarp);
    ftm.setSubvector(0,force_yarp);
    ftm.setSubvector(3,torque_yarp);
    return true;
}

yarp::sig::Vector DynTree::getJointBoundMin(const std::string & part_name)
{
    yarp::sig::Vector ret;
    if( part_name.length() == 0 ) 
    {
        KDLtoYarp(q_min,ret);
    } else {
        const std::vector<int> & dof_ids = partition.getPartDOFIDs(part_name);
        if( dof_ids.size() ==0  ) { std::cerr << "getJointBoundMin: wrong part_name (or part with 0 DOFs)" << std::endl; return yarp::sig::Vector(0); }
        ret.resize(dof_ids.size());
        for(int i = 0; i < (int)dof_ids.size(); i++ ) {
            ret[i] = q_min(dof_ids[i]);
        }
    }
    return ret;
}
        
yarp::sig::Vector DynTree::getJointBoundMax(const std::string & part_name)
{
    yarp::sig::Vector ret;
    if( part_name.length() == 0 ) 
    {
        KDLtoYarp(q_max,ret);
    } else {
        const std::vector<int> & dof_ids = partition.getPartDOFIDs(part_name);
        if( dof_ids.size() ==0  ) { std::cerr << "getJointBoundMax: wrong part_name (or part with 0 DOFs)" << std::endl; return yarp::sig::Vector(0); }
        ret.resize(dof_ids.size());
        for(int i = 0; i < (int)dof_ids.size(); i++ ) {
            ret[i] = q_max(dof_ids[i]);
        }
    }
    return ret;
}

bool DynTree::setJointBoundMin(const yarp::sig::Vector & _q, const std::string & part_name)
{
    if( part_name.length() == 0 ) {
        if( (int)_q.size() != NrOfDOFs  ) { std::cerr << "setJointBoundMin error: input vector has size " << _q.size() <<  " while should have size " << NrOfDOFs << std::endl; return false; } 
        YarptoKDL(_q,q_min);
    } 
    else 
    {
        const std::vector<int> & dof_ids = partition.getPartDOFIDs(part_name);
        if( dof_ids.size() != _q.size() ) { std::cerr << "setJointBoundMax error: Input vector has a wrong number of elements (or part_name wrong)" << std::endl; return false; }
        for(int i = 0; i < (int)dof_ids.size(); i++ ) {
            q_min(dof_ids[i]) = _q[i];
        }
    }
    return true;
}

bool DynTree::setJointBoundMax(const yarp::sig::Vector & _q, const std::string & part_name)
{
    if( part_name.length() == 0 ) {
        if( (int)_q.size() != NrOfDOFs  ) { std::cerr << "setJointBoundMax error: input vector has size " << _q.size() <<  " while should have size " << NrOfDOFs << std::endl; return false; } 
        YarptoKDL(_q,q_max);
    } 
    else 
    {
        const std::vector<int> & dof_ids = partition.getPartDOFIDs(part_name);
        if( dof_ids.size() != _q.size() ) { std::cerr << "setJointBoundMax error: Input vector has a wrong number of elements (or part_name wrong)" << std::endl; return false; }
        for(int i = 0; i < (int)dof_ids.size(); i++ ) {
            q_max(dof_ids[i]) = _q[i];
        }
    }
    return true;
}

void DynTree::setAllConstraints(bool _constrained)
{
    if( _constrained ) {
        //all joints are now not constrained 
        for(int i=0; i < (int)constrained.size(); i++ ) constrained[i] = false;
        constrained_count = 0;
    } else {
        //all joints are now constrained
        for(int i=0; i < (int)constrained.size(); i++ ) constrained[i] = true;
        constrained_count = constrained.size();
    }
}

void DynTree::setConstraint(unsigned int i, bool _constrained) 
{
    //If a joint is constrained, add 1 to the number of constrained joints
    if( !constrained[i] && _constrained ) constrained_count++;
    //If a joint is liberated from its constraint, subtract 1 from the number of constrained joints
    if( constrained[i] && !_constrained ) constrained_count--;

    constrained[i] = _constrained;  
}

bool DynTree::getConstraint(unsigned int i) { return constrained[i]; }


yarp::sig::Matrix DynTree::getPosition(const int link_index,bool inverse) const
{
    if( link_index < 0 || link_index >= (int)tree_graph.getNrOfLinks() ) { std::cerr << "DynTree::getPosition: link index " << link_index <<  " out of bounds" << std::endl; return yarp::sig::Matrix(0,0); }
    computePositions();
    if( !inverse ) {
        return KDLtoYarp_position(world_base_frame*X_dynamic_base[link_index]);
    } else {
        return KDLtoYarp_position((world_base_frame*X_dynamic_base[link_index]).Inverse());
    }
}

yarp::sig::Matrix DynTree::getPosition(const int first_link, const int second_link) const
{
   if( first_link < 0 || first_link >= (int)tree_graph.getNrOfLinks() ) { std::cerr << "DynTree::getPosition: link index " << first_link <<  " out of bounds" << std::endl; return yarp::sig::Matrix(0,0); }
   if( second_link < 0 || second_link >= (int)tree_graph.getNrOfLinks() ) { std::cerr << "DynTree::getPosition: link index " << second_link <<  " out of bounds" << std::endl; return yarp::sig::Matrix(0,0); }
   computePositions();
   return KDLtoYarp_position(X_dynamic_base[first_link].Inverse()*X_dynamic_base[second_link]);
}

yarp::sig::Vector DynTree::getVel(const int link_index, bool local) const
{
    if( link_index < 0 || link_index >= (int)tree_graph.getNrOfLinks() ) { std::cerr << "DynTree::getVel: link index " << link_index <<  " out of bounds" << std::endl; return yarp::sig::Vector(0); }
    yarp::sig::Vector ret(6), lin_vel(3), ang_vel(3);
    KDL::Twist return_twist;
    
    if( !local ) {
        computePositions();
        return_twist = (world_base_frame*X_dynamic_base[link_index]).M*(v[link_index]);
    } else {
        return_twist = v[link_index];
    }
    
    KDLtoYarp(return_twist.vel,lin_vel);
    KDLtoYarp(return_twist.rot,ang_vel);
    ret.setSubvector(0,lin_vel);
    ret.setSubvector(3,ang_vel);
    return ret;
}

yarp::sig::Vector DynTree::getAcc(const int link_index) const
{
    if( link_index < 0 || link_index >= (int)tree_graph.getNrOfLinks() ) { std::cerr << "DynTree::getAcc: link index " << link_index <<  " out of bounds" << std::endl; return yarp::sig::Vector(0); }
    yarp::sig::Vector ret(6), classical_lin_acc(3), ang_acc(3);
    KDLtoYarp(a[link_index].vel+v[link_index].rot*v[link_index].vel,classical_lin_acc);
    KDLtoYarp(a[link_index].rot,ang_acc);
    ret.setSubvector(0,classical_lin_acc);
    ret.setSubvector(3,ang_acc);
    return ret;
}
    
yarp::sig::Vector DynTree::getTorques(const std::string & part_name) const
{
    #ifndef NDEBUG
    //std::cout << "DynTree::getTorques(" << part_name << ")" << std::endl;
    #endif
    if( part_name.length() == 0 ) {
        yarp::sig::Vector ret(NrOfDOFs);
        KDLtoYarp(torques,ret);
        return ret;
    } else {
        const std::vector<int> & dof_ids = partition.getPartDOFIDs(part_name);
        if( dof_ids.size() ==0  ) { std::cerr << "getTorques: " << part_name << " wrong part_name (or part with 0 DOFs)" << std::endl; return yarp::sig::Vector(0); }
        yarp::sig::Vector ret(dof_ids.size());
        #ifndef NDEBUG
        //std::cout << "dof_ids" << dof_ids.size() << std::endl;
        #endif
        for(int i = 0; i < (int)dof_ids.size(); i++ ) {
            #ifndef NDEBUG
            //std::cout << "ids " << dof_ids[i] << std::endl;
            #endif
            ret[i] = torques(dof_ids[i]);
        }
        return ret;
    }
}
    
bool DynTree::setContacts(const iCub::skinDynLib::dynContactList & contacts_list)
{
    for(int sg = 0; sg < NrOfDynamicSubGraphs; sg++ ) {
        contacts[sg].resize(0);
    }
    
    //Separate unknown contacts depending on their subgraph
    for(iCub::skinDynLib::dynContactList::const_iterator it = contacts_list.begin();
            it != contacts_list.end(); it++ ) 
    {
        //get link index
        int body_part = it->getBodyPart();
        int local_link_index = it->getLinkNumber();
        int link_contact_index = partition.getGlobalLinkIndex(body_part,local_link_index);
        
        int subgraph_id = getSubGraphIndex(link_contact_index);
        
        contacts[subgraph_id].push_back(*it);
    }
    
    are_contact_estimated = false;
    
    return true; 
}
    
const iCub::skinDynLib::dynContactList DynTree::getContacts() const
{
    iCub::skinDynLib::dynContactList all_contacts(0);
    
   
    for(int sg = 0; sg < NrOfDynamicSubGraphs; sg++ )
    {
        all_contacts.insert(all_contacts.end(),contacts[sg].begin(),contacts[sg].end());
    }
    
    return all_contacts;
}
    

//====================================
//
//      Computation methods
//
//====================================
bool DynTree::computePositions() const
{
    if( !is_X_dynamic_base_updated ) { 
        if(X_dynamic_base.size() != tree_graph.getNrOfLinks()) { X_dynamic_base.resize(tree_graph.getNrOfLinks()); }
        if( getFramesLoop(tree_graph,q,dynamic_traversal,X_dynamic_base) == 0 ) {
            is_X_dynamic_base_updated = true;
            return true;
        }
        //else
        return false;  
    } else {
        return true;
    }
}

bool DynTree::kinematicRNEA()
{
    int ret;
    ret = rneaKinematicLoop(tree_graph,q,dq,ddq,kinematic_traversal,imu_velocity,imu_acceleration,v,a);
    
    are_contact_estimated = false;
    
    if( ret < 0 ) return false;
    //else
    return true;
}

bool DynTree::estimateContactForces()
{
    double tol = 1e-7; /**< value extracted from old iDynContact */
    buildAb_contacts();
    for(int i=0; i < NrOfDynamicSubGraphs; i++ ) {
        x_contacts[i] = yarp::math::pinv(A_contacts[i],tol)*b_contacts[i];
    }
    store_contacts_results();
    are_contact_estimated = true;
    return true;
}
    
bool DynTree::dynamicRNEA()
{
    int ret;
    KDL::Wrench base_force;
    ret = rneaDynamicLoop(tree_graph,q,dynamic_traversal,v,a,f_ext,f,torques,base_force);
    //Check base force: if estimate contact was called, it should be zero
    if( are_contact_estimated == true ) {
        //If the force were estimated wright
        assert( base_force.force.Norm() < 1e-5 );
        assert( base_force.torque.Norm() < 1e-5 );
        //Note: this (that no residual appears happens only for the proper selection of the provided dynContactList
        for(int i=0; i < NrOfFTSensors; i++ ) {
            #ifndef NDEBUG
            KDL::Wrench residual = measured_wrenches[i] - ft_list.ft_sensors_vector[i]->getH_child_sensor().Inverse(f[ft_list.ft_sensors_vector[i]->getChild()]);
            assert( residual.force.Norm() < 1e-5 );
            assert( residual.torque.Norm() < 1e-5 );
            #endif //NDEBUG
        }
        
        
    } else {
        //In case contacts forces where not estimated, the sensor values have
        //to be calculated from the RNEA
        for(int i=0; i < NrOfFTSensors; i++ ) {
            //Todo add case that the force/wrench is the one of the parent ?
            #ifndef NDEBUG
            //std::cerr << "Project sensor " << i << "from link " << ft_list.ft_sensors_vector[i].getChild() << " to sensor " << std::endl;
            //std::cerr << "Original force " << KDLtoYarp(f[ft_list.ft_sensors_vector[i].getChild()].force).toString() << std::endl;
            #endif
            //measured_wrenches[i] = ft_list.ft_sensors_vector[i].getH_child_sensor().Inverse(f[ft_list.ft_sensors_vector[i].getChild()]);
            measured_wrenches[i] = ft_list.estimateSensorWrenchFromRNEA(i,dynamic_traversal,f);
        }
    }
    if( ret < 0 ) return false;
    return true;
}

////////////////////////////////////////////////////////////////////////
////// COM related methods
////////////////////////////////////////////////////////////////////////

yarp::sig::Vector DynTree::getCOM(const std::string & part_name, int link_index) 
{
    if( (link_index < 0 || link_index >= (int)tree_graph.getNrOfLinks()) && link_index != -1 ) { std::cerr << "DynTree::getCOM: link index " << link_index <<  " out of bounds" << std::endl; return yarp::sig::Vector(0); }
    if( com_yarp.size() != 3 ) { com_yarp.resize(3); }
    if( (int)subtree_COM.size() != getNrOfLinks() ) { subtree_COM.resize(getNrOfLinks()); }
    if( (int)subtree_mass.size() != getNrOfLinks() ) { subtree_mass.resize(getNrOfLinks()); }

    int part_id;
    if( part_name.length() == 0 ) {
         part_id = -1; 
    } else {
         part_id = partition.getPartIDfromPartName(part_name);
         if( part_id == -1 ) { std::cerr << "getCOM: Part name " << part_name << " not recognized " << std::endl; return yarp::sig::Vector(0); }
    } 
    
    KDL::Vector com;                   
    getCenterOfMassLoop(tree_graph,q,dynamic_traversal,subtree_COM,subtree_mass,com,part_id);
    
    KDL::Vector com_world, com_return;
    
    com_world = world_base_frame*com;
    
    if( link_index != -1 ) {
        com_return = X_dynamic_base[link_index].Inverse(com);
    } else {
        //if no reference frame for the return is specified, used the world reference frame
        com_return = com_world;
    }
    
    memcpy(com_yarp.data(),com_return.data,3*sizeof(double));

    return com_yarp;
}


bool DynTree::getCOMJacobian(yarp::sig::Matrix & jac, const std::string & part_name) 
{
    /*
    if( (int)com_jacobian.columns() != 6+getNrOfDOFs() ) { com_jacobian.resize(6+getNrOfDOFs()); }
    if( (int)com_jac_buffer.columns() != 6+getNrOfDOFs() ) { com_jac_buffer.resize(6+getNrOfDOFs()); }
    
    if( jac.rows() != (int)(6) || jac.cols() != (int)(6+tree_graph.getNrOfDOFs()) ) {
        jac.resize(6,6+tree_graph.getNrOfDOFs());
    }
    
    int part_id;
    if( part_name.length() == 0 ) {
        part_id = -1; 
    } else {
        part_id = partition.getPartIDfromPartName(part_name);
        if( part_id == -1 ) { std::cerr << "getCOMJacobian error: Part name " << part_name << " not recognized " << std::endl; return false; }
    } 
    
    computePositions();
    
    getCOMJacobianLoop(tree_graph,q,dynamic_traversal,X_dynamic_base,com_jacobian,com_jac_buffer,part_id);
    
    //Rotate the jacobian to the world 
    com_jacobian.changeBase(world_base_frame.M);
    
    Eigen::Map< Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> > mapped_jacobian(jac.data(),jac.rows(),jac.cols());

    mapped_jacobian = com_jacobian.data;
    
    return true;
    */
    yarp::sig::Matrix dummy;
    return getCOMJacobian(jac,dummy,part_name);
}

bool DynTree::getCOMJacobian(yarp::sig::Matrix & jac, yarp::sig::Matrix & momentum_jac, const std::string & part_name) 
{
    if( (int)com_jacobian.columns() != 6+getNrOfDOFs() ) { com_jacobian.resize(6+getNrOfDOFs()); }
    if( (int)momentum_jacobian.columns() != 6+getNrOfDOFs() ) { momentum_jacobian.resize(6+getNrOfDOFs()); }
    if( (int)com_jac_buffer.columns() != 6+getNrOfDOFs() ) { com_jac_buffer.resize(6+getNrOfDOFs()); }
    if( (int)momentum_jac_buffer.columns() != 6+getNrOfDOFs() ) { momentum_jac_buffer.resize(6+getNrOfDOFs()); }
    
    if( jac.rows() != (int)(6) || jac.cols() != (int)(6+tree_graph.getNrOfDOFs()) ) {
        jac.resize(6,6+tree_graph.getNrOfDOFs());
    }
    
    if( momentum_jac.rows() != (int)(6) || momentum_jac.cols() != (int)(6+tree_graph.getNrOfDOFs()) ) {
        momentum_jac.resize(6,6+tree_graph.getNrOfDOFs());
    }
    
    int part_id;
    if( part_name.length() == 0 ) {
        part_id = -1; 
    } else {
        part_id = partition.getPartIDfromPartName(part_name);
        if( part_id == -1 ) { std::cerr << "getCOMJacobian error: Part name " << part_name << " not recognized " << std::endl; return false; }
    } 
    
    computePositions();
    
    KDL::RigidBodyInertia base_total_inertia;
    
    getMomentumJacobianLoop(tree_graph,q,dynamic_traversal,X_dynamic_base,momentum_jacobian,com_jac_buffer,momentum_jac_buffer,base_total_inertia,part_id);

    
    momentum_jacobian.changeRefFrame(world_base_frame);
    
    total_inertia = world_base_frame*base_total_inertia;
    
    if( total_inertia.getMass() == 0 ) {  std::cerr << "getCOMJacobian error: Tree has no mass " << std::endl; return false; }
    
    momentum_jacobian.changeRefPoint(total_inertia.getCOG());
    
    /** \todo add a meaniful transformation for the rotational part of the jacobian */
    //KDL::CoDyCo::divideJacobianInertia(momentum_jacobian,total_inertia,com_jacobian);
    com_jacobian.data = momentum_jacobian.data/total_inertia.getMass();
    

    //As in iDynTree the base twist is expressed in the world frame, the first six columns are always the identity
    com_jacobian.setColumn(0,KDL::Twist(KDL::Vector(1,0,0),KDL::Vector(0,0,0)).RefPoint(total_inertia.getCOG()));
    com_jacobian.setColumn(1,KDL::Twist(KDL::Vector(0,1,0),KDL::Vector(0,0,0)).RefPoint(total_inertia.getCOG()));
    com_jacobian.setColumn(2,KDL::Twist(KDL::Vector(0,0,1),KDL::Vector(0,0,0)).RefPoint(total_inertia.getCOG()));
    com_jacobian.setColumn(3,KDL::Twist(KDL::Vector(0,0,0),KDL::Vector(1,0,0)).RefPoint(total_inertia.getCOG()));
    com_jacobian.setColumn(4,KDL::Twist(KDL::Vector(0,0,0),KDL::Vector(0,1,0)).RefPoint(total_inertia.getCOG()));
    com_jacobian.setColumn(5,KDL::Twist(KDL::Vector(0,0,0),KDL::Vector(0,0,1)).RefPoint(total_inertia.getCOG()));
    
    momentum_jacobian.changeRefPoint(-total_inertia.getCOG());

    Eigen::Map< Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> > mapped_jacobian(jac.data(),jac.rows(),jac.cols());

    mapped_jacobian = com_jacobian.data;
    
    Eigen::Map< Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> > mapped_momentum_jacobian(momentum_jac.data(),momentum_jac.rows(),momentum_jac.cols());

    mapped_momentum_jacobian = momentum_jacobian.data;

    return true;
}

yarp::sig::Vector DynTree::getVelCOM() 
{
    if( com_yarp.size() != 3 ) { com_yarp.resize(3); }
    
    /** \todo add controls like for computePositions() */
    kinematicRNEA();
    computePositions();
    
    double m = 0;
    KDL::Vector mdcom;
    KDL::Vector mdcom_world;
    KDL::Vector dcom_world;
    for(int i=0; i < getNrOfLinks(); i++ ) {
        double m_i = tree_graph.getLink(i)->getInertia().getMass();
        KDL::Vector com_i = tree_graph.getLink(i)->getInertia().getCOG();
        mdcom += X_dynamic_base[i].M*(m_i*(v[i].RefPoint(com_i)).vel);
        m += m_i;
    }
       
    mdcom_world = world_base_frame.M*mdcom;
    
    dcom_world = mdcom_world/m;
    
    memcpy(com_yarp.data(),dcom_world.data,3*sizeof(double));

    return com_yarp;
}

yarp::sig::Vector DynTree::getMomentum() 
{
    yarp::sig::Vector momentum_yarp(6);
    yarp::sig::Vector lin_vel(3), ang_vel(3);
    /** \todo add controls like for computePositions() */
    kinematicRNEA();
    computePositions();
    
    double m = 0;
    KDL::Wrench mom;
    KDL::Wrench mom_world;
    for(int i=0; i < getNrOfLinks(); i++ ) {
        double m_i = tree_graph.getLink(i)->getInertia().getMass();
        mom += (X_dynamic_base[i]*(tree_graph.getLink(i)->getInertia()*v[i]));
        m += m_i;
    }
    
    mom_world = world_base_frame*mom;
    
    KDLtoYarp(mom_world.force,lin_vel);
    KDLtoYarp(mom_world.torque,ang_vel);
    momentum_yarp.setSubvector(0,lin_vel);
    momentum_yarp.setSubvector(3,ang_vel);
    return momentum_yarp;
}

////////////////////////////////////////////////////////////////////////
////// Jacobian related methods
////////////////////////////////////////////////////////////////////////
bool DynTree::getJacobian(const int link_index, yarp::sig::Matrix & jac, bool local)
{
    if( link_index < 0 || link_index >= (int)tree_graph.getNrOfLinks() ) { std::cerr << "DynTree::getJacobian: link index " << link_index <<  " out of bounds" << std::endl; return false; }
    if( jac.rows() != (int)(6) || jac.cols() != (int)(6+tree_graph.getNrOfDOFs()) ) {
        jac.resize(6,6+tree_graph.getNrOfDOFs());
    }
    
    if( abs_jacobian.rows() != 6 || abs_jacobian.columns() != 6+tree_graph.getNrOfDOFs() ) { abs_jacobian.resize(6+tree_graph.getNrOfDOFs()); } 
    
    getFloatingBaseJacobianLoop(tree_graph,q,dynamic_traversal,link_index,abs_jacobian);
    
    /** \todo compute only the needed rototranslation */
    computePositions();
    
    if( !local ) {
        //Compute the position of the world n

        abs_jacobian.changeBase((world_base_frame*X_dynamic_base[link_index]).M);
        
        KDL::Vector dist_world_link = (world_base_frame*X_dynamic_base[link_index]).p;
        
        //As in iDynTree the base twist is expressed in the world frame, the first six columns are always the identity
        abs_jacobian.setColumn(0,KDL::Twist(KDL::Vector(1,0,0),KDL::Vector(0,0,0)).RefPoint(dist_world_link));
        abs_jacobian.setColumn(1,KDL::Twist(KDL::Vector(0,1,0),KDL::Vector(0,0,0)).RefPoint(dist_world_link));
        abs_jacobian.setColumn(2,KDL::Twist(KDL::Vector(0,0,1),KDL::Vector(0,0,0)).RefPoint(dist_world_link));
        abs_jacobian.setColumn(3,KDL::Twist(KDL::Vector(0,0,0),KDL::Vector(1,0,0)).RefPoint(dist_world_link));
        abs_jacobian.setColumn(4,KDL::Twist(KDL::Vector(0,0,0),KDL::Vector(0,1,0)).RefPoint(dist_world_link));
        abs_jacobian.setColumn(5,KDL::Twist(KDL::Vector(0,0,0),KDL::Vector(0,0,1)).RefPoint(dist_world_link));
    } else {
        //The first 6 columns should be the transformation between world and the local frame
        //in kdl_codyco the velocity of the base twist is expressed in the base frame
        KDL::Frame H_link_world = (world_base_frame*X_dynamic_base[link_index]).Inverse();
        
        abs_jacobian.setColumn(0,H_link_world*KDL::Twist(KDL::Vector(1,0,0),KDL::Vector(0,0,0)));
        abs_jacobian.setColumn(1,H_link_world*KDL::Twist(KDL::Vector(0,1,0),KDL::Vector(0,0,0)));
        abs_jacobian.setColumn(2,H_link_world*KDL::Twist(KDL::Vector(0,0,1),KDL::Vector(0,0,0)));
        abs_jacobian.setColumn(3,H_link_world*KDL::Twist(KDL::Vector(0,0,0),KDL::Vector(1,0,0)));
        abs_jacobian.setColumn(4,H_link_world*KDL::Twist(KDL::Vector(0,0,0),KDL::Vector(0,1,0)));
        abs_jacobian.setColumn(5,H_link_world*KDL::Twist(KDL::Vector(0,0,0),KDL::Vector(0,0,1)));
    }
    
    Eigen::Map< Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> > mapped_jacobian(jac.data(),jac.rows(),jac.cols());
    
    mapped_jacobian = abs_jacobian.data;
    
    return true;
}
    
bool DynTree::getRelativeJacobian(const int jacobian_distal_link, const int jacobian_base_link, yarp::sig::Matrix & jac, bool global)
{    
    if( jac.rows() != (int)(6) || jac.cols() != (int)(tree_graph.getNrOfDOFs()) ) {
        jac.resize(6,tree_graph.getNrOfDOFs());
    }
    
    if( rel_jacobian.rows() != 6 || rel_jacobian.columns() != tree_graph.getNrOfDOFs() ) { rel_jacobian.resize(tree_graph.getNrOfDOFs()); } 
    
    /*
     if the specified jacobian_base_link is the base in dynamic_traversal, kinematic_traversal or rel_jacobian_traversal
     use the traversal already available, otherwise overwrite the rel_jacobian_traversal with the traversal with base at jacobian_base_link
    */
    KDL::CoDyCo::Traversal * p_traversal;
   
    if( dynamic_traversal.order[0]->getLinkIndex() == jacobian_base_link ) {
        p_traversal = &dynamic_traversal;
    } else if ( kinematic_traversal.order[0]->getLinkIndex() == jacobian_base_link ) {
        p_traversal = &kinematic_traversal;
    } else {
       if( rel_jacobian_traversal.order.size() != tree_graph.getNrOfLinks() ||  rel_jacobian_traversal.order[0]->getLinkIndex() == jacobian_base_link  ) {
            tree_graph.compute_traversal(rel_jacobian_traversal,jacobian_base_link);
       }
       p_traversal = &rel_jacobian_traversal;
    }
    
    assert( p_traversal->order[0]->getLinkIndex() == jacobian_base_link );
    
    getRelativeJacobianLoop(tree_graph,q,*p_traversal,jacobian_distal_link,rel_jacobian);
    
    if( global ) {
        computePositions();
        rel_jacobian.changeRefFrame(world_base_frame*X_dynamic_base[jacobian_distal_link]);
    }
    
    Eigen::Map< Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> > mapped_jacobian(jac.data(),jac.rows(),jac.cols());

    mapped_jacobian = rel_jacobian.data;
    
    return true;
}


////////////////////////////////////////////////////////////////////////
////// Regressor related methods
////////////////////////////////////////////////////////////////////////
bool DynTree::getDynamicsRegressor(yarp::sig::Matrix & mat)
{
    //If the incoming matrix have the wrong number of rows/colums, resize it
    if( mat.rows() != (int)(6+tree_graph.getNrOfDOFs()) || mat.cols() != (int)(10*tree_graph.getNrOfLinks()) ) {
        mat.resize(6+tree_graph.getNrOfDOFs(),10*tree_graph.getNrOfLinks());
    }
    
    //Calculate the result directly in the output matrix
    /**
     * \todo check that X_b,v and are computed
     */
    Eigen::Map< Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> > mapped_dynamics_regressor(mat.data(),mat.rows(),mat.cols());
    
    Eigen::MatrixXd dynamics_regressor;
    dynamics_regressor.resize(6+tree_graph.getNrOfDOFs(),10*tree_graph.getNrOfLinks());
    
    computePositions();
    dynamicsRegressorLoop(tree_graph,q,dynamic_traversal,X_dynamic_base,v,a,dynamics_regressor);
    
    mapped_dynamics_regressor = dynamics_regressor;
    
    return true;
}
    
bool DynTree::getDynamicsParameters(yarp::sig::Vector & vec)
{
    if( vec.size() != 10*tree_graph.getNrOfLinks() ) {
        vec.resize(10*tree_graph.getNrOfLinks());
    }
    
    Eigen::Map< Eigen::VectorXd > mapped_vector(vec.data(),10*tree_graph.getNrOfLinks());
    Eigen::VectorXd inertial_parameters;
    inertial_parameters.resize(10*tree_graph.getNrOfLinks());
    
    inertialParametersVectorLoop(tree_graph,inertial_parameters);
    
    mapped_vector = inertial_parameters;
    
    return true;
}

int DynTree::getNrOfDOFs(const std::string & part_name)
{
    
    if( part_name.length() ==  0 ) 
    { 
        //No part specified
        return tree_graph.getNrOfDOFs();        
    } else { // if part_name.length > 0 
        std::vector<int> dof_ids;
        dof_ids = partition.getPartDOFIDs(part_name);
        std::cout << "DynTree::getNrOfDOFs " << part_name << " has " << dof_ids.size() << std::endl;
        return dof_ids.size();
    }
}
        
int DynTree::getNrOfLinks()
{
    return tree_graph.getNrOfLinks();
}

int DynTree::getLinkIndex(const std::string & link_name)
{
    KDL::CoDyCo::LinkMap::const_iterator link_it = tree_graph.getLink(link_name);
    if( link_it == tree_graph.getInvalidLinkIterator() ) { std::cerr << "DynTree::getLinkIndex : link " << link_name << " not found" << std::endl; return -1; }
    return link_it->getLinkIndex();
}
       
int DynTree::getDOFIndex(const std::string & dof_name)
{
    KDL::CoDyCo::JunctionMap::const_iterator junction_it = tree_graph.getJunction(dof_name);
    if( junction_it == tree_graph.getInvalidJunctionIterator() || junction_it->getNrOfDOFs() != 1 ) { std::cerr << "DynTree::getDOFIndex : DOF " << dof_name << " not found" << std::endl; return -1; }
    return junction_it->getDOFIndex();
}

int DynTree::getLinkIndex(const int part_id, const int local_link_index)
{
    int ret =  partition.getGlobalLinkIndex(part_id,local_link_index);
    if( ret < 0 ) { std::cerr << "DynTree::getLinkIndex : link " << local_link_index << " of part " << part_id << " not found" << std::endl; return -1; }
    return ret;
}
       
int DynTree::getDOFIndex(const int part_id, const int local_DOF_index)
{
    int ret = partition.getGlobalDOFIndex(part_id,local_DOF_index);
    if( ret < 0 ) { std::cerr << "DynTree::getDOFIndex : DOF " << local_DOF_index << " of part " << part_id << " not found" << std::endl; return -1; }
    return ret;
}
        
int DynTree::getLinkIndex(const std::string & part_name, const int local_link_index)
{
    return getLinkIndex(partition.getPartIDfromPartName(part_name),local_link_index);
}
       
int DynTree::getDOFIndex(const std::string & part_name, const int local_DOF_index)
{
    return getDOFIndex(partition.getPartIDfromPartName(part_name),local_DOF_index);
}

}
}
