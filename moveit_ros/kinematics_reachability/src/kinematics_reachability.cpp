/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2012, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author: Sachin Chitta
*********************************************************************/

#include <kinematics_reachability/kinematics_reachability.h>

namespace kinematics_reachability
{

KinematicsReachability::KinematicsReachability():node_handle_("~")
{
}

bool KinematicsReachability::initialize()
{
  visualization_publisher_ = node_handle_.advertise<visualization_msgs::MarkerArray>("workspace_markers",0,true);
  workspace_publisher_ = node_handle_.advertise<kinematics_reachability::WorkspacePoints>("workspace",0,true);
  robot_trajectory_publisher_ = node_handle_.advertise<moveit_msgs::DisplayTrajectory>("display_state",0,true);
  tool_offset_.setIdentity();
  tool_offset_inverse_.setIdentity();
  if(!kinematics_solver_.initialize())
  {
    ROS_ERROR("Could not initialize solver");  
    return false;    
  }

  while(!kinematics_solver_.isActive())
  {
    ros::Duration sleep_wait(1.0);
    sleep_wait.sleep();
  }
  
  node_handle_.param("cache_origin/x", default_cache_options_.origin.x, 0.0);
  node_handle_.param("cache_origin/y", default_cache_options_.origin.y, 0.0);
  node_handle_.param("cache_origin/z", default_cache_options_.origin.z, 0.0);

  node_handle_.param("cache_workspace_size/x", default_cache_options_.workspace_size[0], 2.0);
  node_handle_.param("cache_workspace_size/y", default_cache_options_.workspace_size[1], 2.0);
  node_handle_.param("cache_workspace_size/z", default_cache_options_.workspace_size[2], 2.0);
  node_handle_.param("cache_workspace_resolution/x", default_cache_options_.resolution[0], 0.01);
  node_handle_.param("cache_workspace_resolution/y", default_cache_options_.resolution[1], 0.01);
  node_handle_.param("cache_workspace_resolution/z", default_cache_options_.resolution[2], 0.01);
  int tmp;  
  node_handle_.param("cache_num_solutions_per_point", tmp, 1);
  default_cache_options_.max_solutions_per_grid_location = (unsigned int) tmp;
  
  if(!node_handle_.getParam("cache_filename", cache_filename_))
  {
    ROS_ERROR("Must specify cache_filename");
    return false;
  }  
  node_handle_.param<double>("cache_timeout",default_cache_timeout_,60.0);  

  // Visualization
  node_handle_.param("arrow_marker_scale/x", arrow_marker_scale_.x, 0.10);
  node_handle_.param("arrow_marker_scale/y", arrow_marker_scale_.y, 0.04);
  node_handle_.param("arrow_marker_scale/z", arrow_marker_scale_.z, 0.04);

  double sphere_marker_radius;
  node_handle_.param("sphere_marker_radius", sphere_marker_radius, 0.02);
  sphere_marker_scale_.x = sphere_marker_radius;
  sphere_marker_scale_.y = sphere_marker_radius;
  sphere_marker_scale_.z = sphere_marker_radius;

  initializeColor("reachable_color",reachable_color_,0.0,1.0,0.0);
  initializeColor("unreachable_color",unreachable_color_,1.0,0.0,0.0);
  initializeColor("evaluating_color",evaluating_color_,0.0,0.0,1.0);

  first_time_ = true;  
  use_cache_ = false;  
  ROS_INFO("Initialized: Waiting for request");  
  return true;  
}

void KinematicsReachability::initializeColor(const std::string &color_name,
                                             std_msgs::ColorRGBA &color_msg,
                                             double default_r,
                                             double default_g,
                                             double default_b)
{
  double color;  
  node_handle_.param(color_name+"/r", color, default_r);
  color_msg.r = color;  
  node_handle_.param(color_name+"/g", color, default_g);
  color_msg.g = color;  
  node_handle_.param(color_name+"/b", color, default_b);
  color_msg.b = color; 
  color_msg.a = 1.0;  
}


/////////////////////////////////////////////////////////////////////////////////////////////
// Public API Workspace Functions ///////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////


bool KinematicsReachability::computeWorkspace(kinematics_reachability::WorkspacePoints &workspace, 
                                              bool visualize)
{
  if(first_time_)
  {  
    if(generateCache(workspace.group_name,default_cache_timeout_,default_cache_options_,cache_filename_))
      use_cache_ = true;    
    first_time_ = false;    
  }
  
  setToolFrameOffset(workspace.tool_frame_offset);
  if(!sampleUniform(workspace))
    return false;
  if(visualize)
    visualizeWorkspaceSamples(workspace);
  
  findIKSolutions(workspace,visualize);
  return true;
}

bool KinematicsReachability::computeWorkspaceFK(kinematics_reachability::WorkspacePoints &workspace,
                                                double timeout)
{
  if(!kinematics_solver_.getKinematicsSolver(workspace.group_name))
  {
    ROS_ERROR("Could not find group: %s",workspace.group_name.c_str());
    return false;    
  }
  
  std::map<std::string,kinematics::KinematicsBaseConstPtr> my_solver_map = kinematics_solver_.getKinematicsSolver(workspace.group_name)->getKinematicsSolverMap();
  kinematics::KinematicsBaseConstPtr my_solver = my_solver_map.find(workspace.group_name)->second;  
  ros::WallTime start_time = ros::WallTime::now();  
  std::vector<std::string> fk_names;
  std::vector<double> fk_values;  
  std::vector<geometry_msgs::Pose> poses;    

  fk_names.push_back(my_solver->getTipFrame());
  fk_values.resize(my_solver->getJointNames().size(),0.0);
  poses.resize(1);    

  planning_models::KinematicState kinematic_state = kinematics_solver_.getPlanningSceneMonitor()->getPlanningScene()->getCurrentState();
  planning_models::KinematicState::JointStateGroup* joint_state_group = kinematic_state.getJointStateGroup(workspace.group_name);  
  moveit_msgs::MoveItErrorCodes error_code;
  
  while((ros::WallTime::now()-start_time).toSec() <= timeout)
  {
    joint_state_group->setToRandomValues();
    joint_state_group->getGroupStateValues(fk_values);    
    if(!my_solver->getPositionFK(fk_names,fk_values,poses))
    {
      ROS_ERROR("Fk failed");      
      return false;    
    }    
    kinematics_reachability::WorkspacePoint point;
    point.pose = poses[0];    
    point.robot_state.joint_state.position = fk_values;
    point.robot_state.joint_state.name = my_solver->getJointNames();
    point.solution_code.val = point.solution_code.SUCCESS;
    if(!kinematics_solver_.getKinematicsSolver(workspace.group_name)->isValid(kinematic_state,kinematics_solver_.getPlanningSceneMonitor()->getPlanningScene(),error_code))
    {
      point.solution_code.val = point.solution_code.NO_IK_SOLUTION;
    }    
    workspace.points.push_back(point);    
  }
  return true;  
}

bool KinematicsReachability::getOnlyReachableWorkspace(kinematics_reachability::WorkspacePoints &workspace, 
                                                       bool visualize)
{
  if(!computeWorkspace(workspace, visualize))
    return false;
  removeUnreachableWorkspace(workspace);
  return true;
}


/////////////////////////////////////////////////////////////////////////////////////////////
// IK Functions //////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////

kinematics_reachability::WorkspacePoints KinematicsReachability::computeRedundantSolutions(const std::string &group_name,
                                                                                           const geometry_msgs::PoseStamped &pose_stamped,
                                                                                           double timeout,
                                                                                           bool visualize_workspace)
{
    kinematics_reachability::WorkspacePoints workspace;
    workspace.header = pose_stamped.header;
    workspace.group_name = group_name;    
    setToolFrameOffset(workspace.tool_frame_offset);

    bool use_cache_old_value = use_cache_;
    use_cache_ = false;    
    ros::WallTime start_time = ros::WallTime::now();    
    while(ros::WallTime::now()-start_time <= ros::WallDuration(timeout) && ros::ok())
    {
      moveit_msgs::MoveItErrorCodes error_code;
      moveit_msgs::RobotState solution;
      kinematics_reachability::WorkspacePoint point;
      geometry_msgs::PoseStamped desired_pose = pose_stamped;    
      findIK(group_name,pose_stamped,error_code,solution);
      point.pose = pose_stamped.pose;
      point.solution_code = error_code;
      if(error_code.val == error_code.SUCCESS)
      {
        ROS_INFO("Succeeded");        
        point.robot_state = solution;
        if(visualize_workspace)
        {
          visualize(workspace,"");        
        }      
      }
      workspace.points.push_back(point);      
    }    
    use_cache_ = use_cache_old_value;    
    return workspace;    
}


void KinematicsReachability::findIKSolutions(kinematics_reachability::WorkspacePoints &workspace,
                                             bool visualize_workspace)
{  
  for(unsigned int i=0; i < workspace.points.size(); ++i)
  {
    geometry_msgs::PoseStamped ik_pose;
    ik_pose.pose = workspace.points[i].pose;
    ik_pose.header = workspace.header;
    moveit_msgs::MoveItErrorCodes error_code;
    moveit_msgs::RobotState solution;
    
    findIK(workspace.group_name,ik_pose,error_code,solution);
    workspace.points[i].solution_code = error_code;

    if(error_code.val == error_code.SUCCESS)
    {      
      ROS_DEBUG("Solution   : Point %d of %d",(int) i,(int) workspace.points.size());
      workspace.points[i].robot_state = solution;
      kinematics_cache_->addToCache(workspace.points[i].pose,solution.joint_state.position,true);    
    }
    else
    {
      ROS_ERROR("No Solution: Point %d of %d",(int) i,(int) workspace.points.size());
    }
    if(visualize_workspace)
    {
      visualize(workspace,"online");
      animateWorkspace(workspace,i);      
    }
    
    if(i%1000 == 0 || workspace.points.size() <= 100)
      ROS_INFO("At sample %d, (%f,%f,%f)",i,workspace.points[i].pose.position.x,workspace.points[i].pose.position.y,workspace.points[i].pose.position.z);
  }

  if(!kinematics_cache_->writeToFile(cache_filename_))
  {
    ROS_WARN("Could not write cache to file");
  }    
}

void KinematicsReachability::findIK(const std::string &group_name,
                                    const geometry_msgs::PoseStamped &pose_stamped,
                                    moveit_msgs::MoveItErrorCodes &error_code,
                                    moveit_msgs::RobotState &robot_state)
{
  kinematics_msgs::GetConstraintAwarePositionIK::Request request;
  kinematics_msgs::GetConstraintAwarePositionIK::Response response;
  getDefaultIKRequest(group_name,request);
  tf::Pose tmp_pose;
  geometry_msgs::PoseStamped transformed_pose = pose_stamped;  
  tf::poseMsgToTF(pose_stamped.pose,tmp_pose);
  tmp_pose = tmp_pose * tool_offset_inverse_;
  tf::poseTFToMsg(tmp_pose,transformed_pose.pose);  
  request.ik_request.pose_stamped = transformed_pose;
  if(use_cache_)
  {      
    if(!updateFromCache(request))
    {
      error_code.val = error_code.PLANNING_FAILED;
      return;      
    }       
  }        
  kinematics_solver_.getIK(request,response);
  error_code = response.error_code;
  robot_state = response.solution;  
}

                                        
void KinematicsReachability::getDefaultIKRequest(const std::string &group_name,
                                                 kinematics_msgs::GetConstraintAwarePositionIK::Request &req)
{
  kinematics_msgs::GetKinematicSolverInfo::Request request;
  kinematics_msgs::GetKinematicSolverInfo::Response response;

  planning_models::KinematicModelConstPtr kinematic_model = kinematics_solver_.getKinematicModel();
  planning_models::KinematicState kinematic_state(kinematic_model);
  const planning_models::KinematicModel::JointModelGroup* joint_model_group = kinematic_model->getJointModelGroup(group_name);
  planning_models::KinematicState::JointStateGroup joint_state_group(&kinematic_state,(const planning_models::KinematicModel::JointModelGroup*) joint_model_group);
  joint_state_group.setToRandomValues();
  
  req.timeout = ros::Duration(5.0);
  req.ik_request.ik_link_name = joint_model_group->getLinkModelNames().back();
  req.ik_request.ik_seed_state.joint_state.name = joint_model_group->getJointModelNames();
  joint_state_group.getGroupStateValues(req.ik_request.ik_seed_state.joint_state.position);  
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Cache Functions //////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////

bool KinematicsReachability::updateFromCache(kinematics_msgs::GetConstraintAwarePositionIK::Request &request)
{
  geometry_msgs::Pose pose = request.ik_request.pose_stamped.pose;
  double distance_squared = (pose.position.x*pose.position.x + pose.position.y*pose.position.y + pose.position.z*pose.position.z);
  std::pair<double,double> distances;
  distances = kinematics_cache_->getMinMaxSquaredDistance(); 
  if(distance_squared >= distances.second)
    return false;  
  kinematics_cache_->getSolution(request.ik_request.pose_stamped.pose,
                                 0,
                                 request.ik_request.ik_seed_state.joint_state.position);
  return true;  
}

bool KinematicsReachability::generateCache(const std::string &group_name,
                                           double timeout,
                                           const kinematics_cache::KinematicsCache::Options &options,
                                           const std::string &cache_filename)
{
  if(!kinematics_cache_ || kinematics_cache_->getGroupName()!= group_name)
  {    
    std::map<std::string,kinematics::KinematicsBasePtr> kinematics_solver_map = kinematics_solver_.getPlanningSceneMonitor()->getKinematicModelLoader()->generateKinematicsSolversMap();
    if(kinematics_solver_map.find(group_name) == kinematics_solver_map.end())
    {
      ROS_ERROR("Group name: %s incorrect",group_name.c_str());      
      return false;
    }    
    kinematics::KinematicsBaseConstPtr kinematics_solver_local = kinematics_solver_map.find(group_name)->second;    
    kinematics_cache_.reset(new kinematics_cache::KinematicsCache());
    kinematics_cache_->initialize(kinematics_solver_local,
                                  kinematics_solver_.getKinematicModel(),
                                  options);    
  }  
  if(!kinematics_cache_->readFromFile(cache_filename))
  {
    ROS_INFO("Generating cache map online");    
    if(!kinematics_cache_->generateCacheMap(timeout))
    {
      return false;
    }          
    if(!kinematics_cache_->writeToFile(cache_filename))
    {
      ROS_ERROR("Could not write to file");
      return false;      
    }    
  }
  return true;  
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Helper Functions //////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////

bool KinematicsReachability::isEqual(const geometry_msgs::Quaternion &orientation_1, 
                                     const geometry_msgs::Quaternion &orientation_2)
{
  tf::Quaternion quat_1,quat_2;
  tf::quaternionMsgToTF(orientation_1,quat_1);
  tf::quaternionMsgToTF(orientation_2,quat_2);
  if(quat_1.angleShortestPath(quat_2) < 0.001)
    return true;
  return false;
}

void KinematicsReachability::publishWorkspace(const kinematics_reachability::WorkspacePoints &workspace)
{
  workspace_publisher_.publish(workspace); 
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Workspace Functions //////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////

void KinematicsReachability::getPositionIndex(const kinematics_reachability::WorkspacePoints &workspace,
                                              std::vector<unsigned int> &reachable_workspace,
                                              std::vector<unsigned int> &unreachable_workspace)
{
  unsigned int x_num_points,y_num_points,z_num_points;
  getNumPoints(workspace,x_num_points,y_num_points,z_num_points);
  unsigned int num_workspace_points = x_num_points*y_num_points*z_num_points*workspace.orientations.size();
  for(unsigned int i=0; i < num_workspace_points; ++i)
  {
    if(workspace.points[i].solution_code.val == workspace.points[i].solution_code.SUCCESS)
      reachable_workspace.push_back(i);
    else
      unreachable_workspace.push_back(i);
  }
}

void KinematicsReachability::removeUnreachableWorkspace(kinematics_reachability::WorkspacePoints &workspace)
{
  unsigned int remove_counter = 0;
  moveit_msgs::MoveItErrorCodes error_code;
  std::vector<kinematics_reachability::WorkspacePoint>::iterator it = workspace.points.begin();
  while(it != workspace.points.end())
  {
    if(it->solution_code.val != it->solution_code.SUCCESS)
    {
      it = workspace.points.erase(it);
      remove_counter++;
    }
    else
      ++it;
  }
  if(remove_counter)
    ROS_DEBUG("Removed %d points from workspace",remove_counter);
}

std::vector<unsigned int> KinematicsReachability::getPointsAtOrientation(const kinematics_reachability::WorkspacePoints &workspace,
                                                                                                           const geometry_msgs::Quaternion &orientation)
{
  std::vector<unsigned int> wp;
  for(unsigned int i = 0; i < workspace.points.size(); ++i)
  {
    if(isEqual(workspace.points[i].pose.orientation,orientation))
      wp.push_back(i);
  }
  return wp;
}

std::vector<unsigned int> KinematicsReachability::getPointsWithinRange(const kinematics_reachability::WorkspacePoints &workspace,
                                                                       const double min_radius,
                                                                       const double max_radius)
{
  std::vector<unsigned int> wp;
  for(unsigned int i = 0; i < workspace.points.size(); ++i)
  {
    tf::Vector3 vector;
    tf::pointMsgToTF(workspace.points[i].pose.position,vector);
    if(vector.length() >= min_radius && vector.length() <= max_radius)
      wp.push_back(i);
  }
  return wp;
}

void KinematicsReachability::getNumPoints(const kinematics_reachability::WorkspacePoints &workspace,
                                          unsigned int &x_num_points,
                                          unsigned int &y_num_points,
                                          unsigned int &z_num_points)
{
  double position_resolution = workspace.position_resolution;
  double x_dim = std::fabs(workspace.parameters.min_corner.x - workspace.parameters.max_corner.x);
  double y_dim = std::fabs(workspace.parameters.min_corner.y - workspace.parameters.max_corner.y);
  double z_dim = std::fabs(workspace.parameters.min_corner.z - workspace.parameters.max_corner.z);

  x_num_points = (unsigned int) (x_dim/position_resolution) + 1;
  y_num_points = (unsigned int) (y_dim/position_resolution) + 1;
  z_num_points = (unsigned int) (z_dim/position_resolution) + 1;

  ROS_DEBUG("Cache dimension (num grid points) in (x,y,z): %d %d %d",x_num_points,y_num_points,z_num_points);
}



bool KinematicsReachability::sampleUniform(kinematics_reachability::WorkspacePoints &workspace)
{
  if(workspace.orientations.empty())
  {
    ROS_ERROR("Must specify at least one orientation");
    return false;
  }
  workspace.ordered = true;  
  double position_resolution = workspace.position_resolution;
  double x_min = workspace.parameters.min_corner.x;
  double y_min = workspace.parameters.min_corner.y;
  double z_min = workspace.parameters.min_corner.z;

  unsigned int x_num_points,y_num_points,z_num_points;
  getNumPoints(workspace,x_num_points,y_num_points,z_num_points);

  unsigned int num_rotations = workspace.orientations.size();
  kinematics_reachability::WorkspacePoint ws_point;
  geometry_msgs::Pose pose;

  for(unsigned int i=0; i < x_num_points; ++i)
  {
    pose.position.x = x_min + i * position_resolution;
    for(unsigned int j=0; j < y_num_points; ++j)
    {
      pose.position.y = y_min + j * position_resolution;
      for(unsigned int k=0; k < z_num_points; ++k)
      {
        pose.position.z = z_min + k * position_resolution;
        for(unsigned int m=0; m < num_rotations; ++m)
        {
          tf::Vector3 point(pose.position.x,pose.position.y,pose.position.z);
          tf::pointTFToMsg(point,ws_point.pose.position);
          ws_point.pose.orientation = workspace.orientations[m];
          workspace.points.push_back(ws_point);
        }
      }
    }
  }
  ROS_DEBUG("Generated %d samples for workspace points",(int) workspace.points.size());
  return true;
}

void KinematicsReachability::setToolFrameOffset(const geometry_msgs::Pose &pose)
{
  tf::poseMsgToTF(pose,tool_offset_);
  tool_offset_inverse_ = tool_offset_.inverse();
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Visualization functions //////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////

void KinematicsReachability::getMarkers(const kinematics_reachability::WorkspacePoints &workspace,
                                        const std::string &marker_namespace,
                                        const std::vector<unsigned int> &points,
                                        visualization_msgs::MarkerArray &marker_array)
{
  std::vector<moveit_msgs::MoveItErrorCodes> error_codes(3);
  error_codes[0].val = error_codes[0].SUCCESS;  
  error_codes[1].val = error_codes[1].PLANNING_FAILED;  
  error_codes[2].val = error_codes[2].NO_IK_SOLUTION;  

  std::vector<unsigned int> marker_ids(3);
  marker_ids[0] = 0;
  marker_ids[1] = 1;
  marker_ids[2] = 2;

  std::vector<std_msgs::ColorRGBA> colors(3);
  colors[0] = reachable_color_;
  colors[1] = evaluating_color_;
  colors[2] = unreachable_color_;
  
  std::vector<visualization_msgs::Marker> markers = getSphereMarker(workspace,marker_namespace,points,colors,error_codes,marker_ids);    
  for(unsigned int i=0; i < markers.size(); ++i)
    marker_array.markers.push_back(markers[i]);  
}

std::vector<visualization_msgs::Marker> KinematicsReachability::getSphereMarker(const kinematics_reachability::WorkspacePoints &workspace,
                                                                                const std::string &marker_namespace,
                                                                                const std::vector<unsigned int> &indices,
                                                                                const std::vector<std_msgs::ColorRGBA> &colors,
                                                                                const std::vector<moveit_msgs::MoveItErrorCodes> &error_codes,
                                                                                const std::vector<unsigned int> &marker_id)
{
  std::vector<visualization_msgs::Marker> markers;
  if(marker_id.size() != error_codes.size() || colors.size() != error_codes.size())
    return markers;
  markers.resize(marker_id.size());
  
  for(unsigned int i=0; i < markers.size(); ++i)
  {
    markers[i].type = markers[i].SPHERE_LIST;
    markers[i].action = 0;
    markers[i].pose.orientation.w = 1.0;  
    markers[i].ns = marker_namespace;
    markers[i].header = workspace.header;
    markers[i].scale = sphere_marker_scale_;
    markers[i].id = marker_id[i];
    markers[i].color = colors[i];
  }
  
  if(indices.empty())
  {
    for(unsigned int i=0; i < workspace.points.size(); ++i)
    {
      geometry_msgs::Point point = workspace.points[i].pose.position;
      for(unsigned int j=0; j < error_codes.size(); ++j)
      {
        if(workspace.points[i].solution_code.val == error_codes[j].val)
        {
          markers[j].colors.push_back(colors[j]);
          markers[j].points.push_back(point);        
        }
      }      
    }    
  }
  else
  {
    for(unsigned int i=0; i < indices.size(); ++i)
    {
      if(indices[i] >= workspace.points.size())
      {
        ROS_WARN("Invalid point: %d",indices[i]);
        continue;        
      }
      geometry_msgs::Point point = workspace.points[indices[i]].pose.position;
      for(unsigned int j=0; j < error_codes.size(); ++j)
      {
        if(workspace.points[indices[i]].solution_code.val == error_codes[j].val)
        {
          markers[j].colors.push_back(colors[j]);
          markers[j].points.push_back(point);        
        }
      }      
    }    
  }  
  return markers;  
}

std_msgs::ColorRGBA KinematicsReachability::getMarkerColor(const kinematics_reachability::WorkspacePoint &workspace_point)
{
  if(workspace_point.solution_code.val == workspace_point.solution_code.SUCCESS)
  {
    return reachable_color_;    
  }
  else if(workspace_point.solution_code.val == workspace_point.solution_code.NO_IK_SOLUTION)
  {
    return unreachable_color_;    
  }
  else
  {
    return evaluating_color_;    
  }    
}

void KinematicsReachability::getArrowMarkers(const kinematics_reachability::WorkspacePoints &workspace,
                                             const std::string &marker_namespace,
                                             const std::vector<unsigned int> &points,
                                             visualization_msgs::MarkerArray &marker_array)
{
  visualization_msgs::Marker marker;
  
  marker.type = marker.ARROW;
  marker.action = 0;
  marker.ns = marker_namespace;
  marker.header = workspace.header;
  marker.scale = arrow_marker_scale_;
  
  if(points.empty())
  {
    for(unsigned int i=0; i < workspace.points.size(); ++i)
    {
      marker.pose = workspace.points[i].pose;    
      marker.id = i+4;    
      marker.color = getMarkerColor(workspace.points[i]);    
      marker_array.markers.push_back(marker);    
    }    
  }
  else
  {
    for(unsigned int i=0; i < points.size(); ++i)
    {
      if(points[i] >= workspace.points.size())
      {
        ROS_WARN("Invalid point: %d",points[i]);
        continue;        
      }
      marker.pose = workspace.points[points[i]].pose;    
      marker.id = points[i];    
      marker.color = getMarkerColor(workspace.points[points[i]]);    
      marker_array.markers.push_back(marker);    
    }    
  }
}

bool KinematicsReachability::getDisplayTrajectory(const kinematics_reachability::WorkspacePoints &workspace,
                                                  moveit_msgs::DisplayTrajectory &display_trajectory)
{
  if(workspace.points.empty())
     return false;

  std::vector<unsigned int> reachable_workspace, unreachable_workspace;
  getPositionIndex(workspace,reachable_workspace,unreachable_workspace);
  ros::Duration time_from_start(0.0);
  bool first_time(true);  
  for(unsigned int i=0; i < reachable_workspace.size(); ++i)
  {
    if(first_time)
    {
        display_trajectory.trajectory.joint_trajectory.joint_names = workspace.points[reachable_workspace[i]].robot_state.joint_state.name;
        first_time = false;
    }    
    trajectory_msgs::JointTrajectoryPoint point;
    point.positions = workspace.points[reachable_workspace[i]].robot_state.joint_state.position;
    point.time_from_start = time_from_start;
    display_trajectory.trajectory.joint_trajectory.points.push_back(point);
  }
  return true;
}

bool KinematicsReachability::getDisplayTrajectory(const kinematics_reachability::WorkspacePoint &workspace_point,
                                                  moveit_msgs::DisplayTrajectory &display_trajectory)
{
  trajectory_msgs::JointTrajectoryPoint point;
  display_trajectory.trajectory.joint_trajectory.joint_names = workspace_point.robot_state.joint_state.name;
  point.positions = workspace_point.robot_state.joint_state.position;
  display_trajectory.trajectory.joint_trajectory.points.push_back(point);
  display_trajectory.trajectory.joint_trajectory.points.push_back(point);
  return true;
}

void KinematicsReachability::animateWorkspace(const kinematics_reachability::WorkspacePoints &workspace)
{
  moveit_msgs::DisplayTrajectory display_trajectory;
  if (!getDisplayTrajectory(workspace,display_trajectory))
  {
    ROS_WARN("No trajectory to display");
    return;
  }  
  robot_trajectory_publisher_.publish(display_trajectory);
  ROS_INFO("Animating trajectory");  
}

void KinematicsReachability::animateWorkspace(const kinematics_reachability::WorkspacePoints &workspace,
                                              unsigned int index)
{
  moveit_msgs::DisplayTrajectory display_trajectory;
  if (index >= workspace.points.size() ||   
      workspace.points[index].solution_code.val != workspace.points[index].solution_code.SUCCESS ||
      !getDisplayTrajectory(workspace.points[index],display_trajectory))
  {
    ROS_DEBUG("No trajectory to display");
    return;
  }  
  robot_trajectory_publisher_.publish(display_trajectory);
}

void KinematicsReachability::visualize(const kinematics_reachability::WorkspacePoints &workspace,
                                       const std::string &marker_namespace)
{
  visualization_msgs::MarkerArray marker_array;
  std::vector<unsigned int> points;  
  getMarkers(workspace,marker_namespace,points,marker_array);
  getArrowMarkers(workspace,marker_namespace,points,marker_array);
  visualization_publisher_.publish(marker_array);
}

void KinematicsReachability::visualize(const kinematics_reachability::WorkspacePoints &workspace,
                                       const std::string &marker_namespace,
                                       const std::vector<geometry_msgs::Quaternion> &orientations)
{
  visualization_msgs::MarkerArray marker_array;
  for(unsigned int i=0; i < orientations.size(); ++i)
  {
    std::vector<unsigned int> points = getPointsAtOrientation(workspace,orientations[i]);
    std::ostringstream name;
    name << "orientation_" << i;
    std::string marker_name = marker_namespace+name.str();
    getMarkers(workspace,marker_name,points,marker_array);
  }
  visualization_publisher_.publish(marker_array);
}

void KinematicsReachability::visualizeWithArrows(const kinematics_reachability::WorkspacePoints &workspace,
                                                 const std::string &marker_namespace)
{
  visualization_msgs::MarkerArray marker_array;
  std::vector<unsigned int> points;  
  getArrowMarkers(workspace,marker_namespace,points,marker_array);
  visualization_publisher_.publish(marker_array);
}

void KinematicsReachability::visualizeWorkspaceSamples(const kinematics_reachability::WorkspacePoints &workspace_in)
{
  kinematics_reachability::WorkspacePoints workspace = workspace_in;
  
  visualization_msgs::MarkerArray marker_array;
  visualization_msgs::Marker marker;
  marker.type = marker.CUBE;
  marker.action = 0;

  marker.color.r = 0.5;
  marker.color.g = 0.5;
  marker.color.b = 0.5;
  marker.color.a = 0.2;
  
  marker.ns = "samples";
  marker.header = workspace.header;

  marker.pose.position.x = (workspace.parameters.min_corner.x + workspace.parameters.max_corner.x)/2.0;
  marker.pose.position.y = (workspace.parameters.min_corner.y + workspace.parameters.max_corner.y)/2.0;
  marker.pose.position.z = (workspace.parameters.min_corner.z + workspace.parameters.max_corner.z)/2.0;
  marker.pose.orientation.w = 1.0;
  
  marker.scale.x = std::fabs(workspace.parameters.min_corner.x - workspace.parameters.max_corner.x);
  marker.scale.y = std::fabs(workspace.parameters.min_corner.y - workspace.parameters.max_corner.y);
  marker.scale.z = std::fabs(workspace.parameters.min_corner.z - workspace.parameters.max_corner.z);
  marker.id = 3; 
  marker_array.markers.push_back(marker);
  
  if(workspace.points.empty())
    sampleUniform(workspace);

  std::vector<unsigned int> indices;  
  std::vector<moveit_msgs::MoveItErrorCodes> error_code(1); 
  std::vector<std_msgs::ColorRGBA> colors;
  colors.push_back(evaluating_color_);
  std::vector<unsigned int> marker_ids;
  marker_ids.push_back(1);  
 
  std::vector<visualization_msgs::Marker> marker_points = getSphereMarker(workspace,"samples",indices,colors,error_code,marker_ids);      
  marker_array.markers.push_back(marker_points[0]);
  
  ROS_INFO("Publishing initial set of markers");  
  visualization_publisher_.publish(marker_array);  
}

} // namespace
