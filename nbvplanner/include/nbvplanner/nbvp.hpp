    /*
 * Copyright 2015 Andreas Bircher, ASL, ETH Zurich, Switzerland
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef NBVP_HPP_
#define NBVP_HPP_

#include <fstream>
#include <ctime>
#include <eigen3/Eigen/Dense>

#include <visualization_msgs/Marker.h>

#include <nbvplanner/nbvp.h>
#include <nbvplanner/rrt.h>
#include <nbvplanner/tree.h>

// Convenience macro to get the absolute yaw difference
#define ANGABS(x) (fmod(fabs(x),2.0*M_PI)<M_PI?fmod(fabs(x),2.0*M_PI):2.0*M_PI-fmod(fabs(x),2.0*M_PI))

using namespace Eigen;

template<typename stateVec>
nbvInspection::nbvPlanner<stateVec>::nbvPlanner(const ros::NodeHandle& nh,
                                                const ros::NodeHandle& nh_private)
    : nh_(nh),
      nh_private_(nh_private)
{

  manager_ = new volumetric_mapping::OctomapManager(nh_, nh_private_);

  // Set up the topics and services
  params_.inspectionPath_ = nh_.advertise<visualization_msgs::Marker>("inspectionPath", 1000);
  peerPosPub_ = nh_.advertise<visualization_msgs::Marker>("peerPoses", 100);
  peerRrtPub_ = nh_.advertise<multiagent_collision_check::Node>("/peerRrts", 1000);
  evadePub_ = nh_.advertise<multiagent_collision_check::Segment>("/evasionSegment", 100);

  bool rh_mode = true;
  std::cout << "rh_mode is " << rh_mode << std::endl;
  if (rh_mode) {
    std::cout << "Receding horizon mode" << std::endl;
    plannerService_ = nh_.advertiseService("nbvplanner", &nbvInspection::nbvPlanner<stateVec>::plannerCallback, this);
  } else {
    std::cout << "NBV / NF mode" << std::endl;
    plannerService_ = nh_.advertiseService("nbvplanner", &nbvInspection::nbvPlanner<stateVec>::VRRT__plannerCallback, this);
  }

  pub_path = nh_.advertise<sensor_msgs::PointCloud2>("path", 1000);

  posClient_ = nh_.subscribe("pose", 10, &nbvInspection::nbvPlanner<stateVec>::posCallback, this);
  odomClient_ = nh_.subscribe("odometry", 10, &nbvInspection::nbvPlanner<stateVec>::odomCallback, this);

  pointcloud_sub_ = nh_.subscribe(
          "pointcloud_throttled", 1, &nbvInspection::nbvPlanner<stateVec>::insertPointcloudWithTf, this);
  pointcloud_sub_cam_up_ = nh_.subscribe(
          "pointcloud_throttled_up", 1, &nbvInspection::nbvPlanner<stateVec>::insertPointcloudWithTfCamUp, this);
  pointcloud_sub_cam_down_ = nh_.subscribe(
      "pointcloud_throttled_down", 1, &nbvInspection::nbvPlanner<stateVec>::insertPointcloudWithTfCamDown, this);

  if (!setParams()) {
    ROS_ERROR("Could not start the planner. Parameters missing!");
  }

  // Precompute the camera field of view boundaries. The normals of the separating hyperplanes are stored
  for (int i = 0; i < params_.camPitch_.size(); i++) {
    double pitch = M_PI * params_.camPitch_[i] / 180.0;
    double camTop = (pitch - M_PI * params_.camVertical_[i] / 360.0) + M_PI / 2.0;
    double camBottom = (pitch + M_PI * params_.camVertical_[i] / 360.0) - M_PI / 2.0;
    double side = M_PI * (params_.camHorizontal_[i]) / 360.0 - M_PI / 2.0;
    Vector3d bottom(cos(camBottom), 0.0, -sin(camBottom));
    Vector3d top(cos(camTop), 0.0, -sin(camTop));
    Vector3d right(cos(side), sin(side), 0.0);
    Vector3d left(cos(side), -sin(side), 0.0);
    AngleAxisd m = AngleAxisd(pitch, Vector3d::UnitY());
    Vector3d rightR = m * right;
    Vector3d leftR = m * left;
    rightR.normalize();
    leftR.normalize();
    std::vector<Eigen::Vector3d> camBoundNormals;
    camBoundNormals.push_back(bottom);
    // ROS_INFO("bottom: (%2.2f, %2.2f, %2.2f)", bottom[0], bottom[1], bottom[2]);
    camBoundNormals.push_back(top);
    // ROS_INFO("top: (%2.2f, %2.2f, %2.2f)", top[0], top[1], top[2]);
    camBoundNormals.push_back(rightR);
    // ROS_INFO("rightR: (%2.2f, %2.2f, %2.2f)", rightR[0], rightR[1], rightR[2]);
    camBoundNormals.push_back(leftR);
    // ROS_INFO("leftR: (%2.2f, %2.2f, %2.2f)", leftR[0], leftR[1], leftR[2]);
    params_.camBoundNormals_.push_back(camBoundNormals);
  }

  // Load mesh from STL file if provided.
  std::string ns = ros::this_node::getName();
  std::string stlPath = "";
  mesh_ = NULL;
  if (ros::param::get(ns + "/stl_file_path", stlPath)) {
    if (stlPath.length() > 0) {
      if (ros::param::get(ns + "/mesh_resolution", params_.meshResolution_)) {
        std::fstream stlFile;
        stlFile.open(stlPath.c_str());
        if (stlFile.is_open()) {
          mesh_ = new mesh::StlMesh(stlFile);
          mesh_->setResolution(params_.meshResolution_);
          mesh_->setOctomapManager(manager_);
          mesh_->setCameraParams(params_.camPitch_, params_.camHorizontal_, params_.camVertical_, params_.gainRange_);
        } else {
          ROS_WARN("Unable to open STL file");
        }
      } else {
        ROS_WARN("STL mesh file path specified but mesh resolution parameter missing!");
      }
    }
  }
  // Initialize the tree instance.
  tree_ = new RrtTree(mesh_, manager_);
  tree_->setParams(params_);
  peerPosClient1_ = nh_.subscribe("peer_pose_1", 10,
                                  &nbvInspection::RrtTree::setPeerStateFromPoseMsg1, tree_);
  peerPosClient2_ = nh_.subscribe("peer_pose_2", 10,
                                  &nbvInspection::RrtTree::setPeerStateFromPoseMsg2, tree_);
  peerPosClient3_ = nh_.subscribe("peer_pose_3", 10,
                                  &nbvInspection::RrtTree::setPeerStateFromPoseMsg3, tree_);
  // Subscribe to topic used for the collaborative collision avoidance (don't hit your peer).
  evadeClient_ = nh_.subscribe("/evasionSegment", 10, &nbvInspection::TreeBase<stateVec>::evade, tree_);
  peerRrtClient_ = nh_.subscribe("/peerRrts", 10, &nbvInspection::nbvPlanner<stateVec>::addRrts, this);
  // Not yet ready. Needs a position message first.
  ready_ = false;
}

template<typename stateVec>
nbvInspection::nbvPlanner<stateVec>::~nbvPlanner()
{
  if (manager_) {
    delete manager_;
  }
  if (mesh_) {
    delete mesh_;
  }
}

template<typename stateVec>
void nbvInspection::nbvPlanner<stateVec>::posCallback(
    const geometry_msgs::PoseWithCovarianceStamped& pose)
{
  tree_->setStateFromPoseMsg(pose);
  // Planner is now ready to plan.
  ready_ = true;
}

template<typename stateVec>
void nbvInspection::nbvPlanner<stateVec>::odomCallback(
    const nav_msgs::Odometry& pose)
{
  tree_->setStateFromOdometryMsg(pose);
  // Planner is now ready to plan.
  ready_ = true;
}

template<typename stateVec>
bool nbvInspection::nbvPlanner<stateVec>::plannerCallback(nbvplanner::nbvp_srv::Request& req,
                                                          nbvplanner::nbvp_srv::Response& res)
{
  double rate;
  ros::Time computationTime = ros::Time::now();
  // Check that planner is ready to compute path.
  if (!ros::ok()) {
    ROS_INFO_THROTTLE(1, "Exploration finished. Not planning any further moves.");
    return true;
  }

  if (!ready_) {
    ROS_ERROR_THROTTLE(1, "Planner not set up: Planner not ready!");
    return true;
  }
  if (manager_ == NULL) {
    ROS_ERROR_THROTTLE(1, "Planner not set up: No octomap available!");
    return true;
  }
  if (manager_->getMapSize().norm() <= 0.0) {
    ROS_ERROR_THROTTLE(1, "Planner not set up: Octomap is empty!");
    return true;
  }
  res.path.clear();

  // Clear old tree and reinitialize.
  Eigen::Vector3d prev_state;
  for (int i = 0; i < 3; i++)
    prev_state[i] = 0.0;
  if (cnt > 0) {
    Eigen::Vector4d prev_target = tree_->getBest();
    for (int i = 0; i < 3; i++)
      prev_state[i] = prev_target[i];
  }
  if (rrts_.size() != 3) {
    peer_target.push_back(Eigen::Vector4d(0,0,0,0));
    peer_target.push_back(Eigen::Vector4d(0,0,0,0));
  } else {
    for (int i=0; i<3; i++) {
      bool flag = false;
      if (rrts_[i]->size() > 0) {
        flag = (*rrts_[i])[1][0] == prev_state[0]
               && (*rrts_[i])[1][1] == prev_state[1] && (*rrts_[i])[1][2] == prev_state[2];
        if (not flag) {
          peer_target.push_back(Eigen::Vector4d((*rrts_[i])[1][0], (*rrts_[i])[1][1], (*rrts_[i])[1][2], 1));
        }
      } else {
        peer_target.push_back(Eigen::Vector4d(0,0,0,0));
      }
    }
  }
  tree_->clear();
  tree_->initialize(peer_target);
  cnt++;
  vector_t path;
  // Iterate the tree construction method.
  int loopCount = 0;
  while ((!tree_->gainFound() || tree_->getCounter() < params_.initIterations_) && ros::ok()) {
    if (tree_->getCounter() > params_.cuttoffIterations_) {
      ROS_INFO("No gain found, shutting down");

      std::cout << "Exploration finished, time elapsed: " << ros::Time::now().toSec() << std::endl;

      rate = manager_->explorationRate(1);
      std::cout << "Exploration Rate: " << rate << std::endl;
      ros::shutdown();
      return true;
    }
    if (loopCount > 1000 * (tree_->getCounter() + 1)) {
      ROS_INFO_THROTTLE(1, "Exceeding maximum failed iterations, return to previous point!");
      res.path = tree_->getPathBackToPrevious(req.header.frame_id);
      return true;
    }
    peer_target.clear();
    if (rrts_.size() != 3) {
      peer_target.push_back(Eigen::Vector4d(0,0,0,0));
      peer_target.push_back(Eigen::Vector4d(0,0,0,0));
    } else {
      for (int i=0; i<3; i++) {
        bool flag = false;
        if (rrts_[i]->size() > 0) {
          flag = (*rrts_[i])[1][0] == prev_state[0]
                 && (*rrts_[i])[1][1] == prev_state[1] && (*rrts_[i])[1][2] == prev_state[2];
          if (not flag) {
            peer_target.push_back(Eigen::Vector4d((*rrts_[i])[1][0], (*rrts_[i])[1][1], (*rrts_[i])[1][2], 1));
          }
        } else {
          peer_target.push_back(Eigen::Vector4d(0,0,0,0));
        }
      }
    }
    tree_->iterate(peer_target);
    loopCount++;
  }

  Eigen::Vector4d target_node = tree_->getBest();
  multiagent_collision_check::Node target_msg;
  target_msg.prev_state.x = prev_state[0];
  target_msg.prev_state.y = prev_state[1];
  target_msg.prev_state.z = prev_state[2];
  target_msg.target.x = target_node[0];
  target_msg.target.y = target_node[1];
  target_msg.target.z = target_node[2];
  peerRrtPub_.publish(target_msg);

  tree_->getLeafNode(1);

  std::vector<nbvInspection::Node<stateVec>*> candidates = tree_->getCandidates();

  // Extract the best edge.
  res.path = tree_->getBestEdge(req.header.frame_id);

  tree_->memorizeBestBranch();
  // Publish path to block for other agents (multi agent only).
  multiagent_collision_check::Segment segment;
  segment.header.stamp = ros::Time::now();
  segment.header.frame_id = params_.navigationFrame_;
  if (!res.path.empty()) {
    segment.poses.push_back(res.path.front());
    segment.poses.push_back(res.path.back());
  }
  evadePub_.publish(segment);

  ROS_INFO("Path computation lasted %2.3fs", (ros::Time::now() - computationTime).toSec());

  std::ofstream outFile("Data.txt", std::ios_base::out|std::ios_base::app);

//  std::string sentence = "time: " + std::to_string(ros::Time::now().toSec()) + "s, Exploration Rate: " + std::to_string(rate*100);

  outFile << "time: " << ros::Time::now().toSec() << "s, Exploration Rate: " << rate*100 << "%" << std::endl;
  outFile.close();
  return true;
}

template<typename stateVec>
bool nbvInspection::nbvPlanner<stateVec>::setParams()
{
  std::string ns = ros::this_node::getName();
  bool ret = true;
  params_.v_max_ = 0.25;
  if (!ros::param::get(ns + "/system/v_max", params_.v_max_)) {
    ROS_WARN("No maximal system speed specified. Looking for %s. Default is 0.25.",
             (ns + "/system/v_max").c_str());
  }
  params_.dyaw_max_ = 0.5;
  if (!ros::param::get(ns + "/system/dyaw_max", params_.dyaw_max_)) {
    ROS_WARN("No maximal yaw speed specified. Looking for %s. Default is 0.5.",
             (ns + "/system/yaw_max").c_str());
  }
  params_.camPitch_ = {15.0};
  if (!ros::param::get(ns + "/system/camera/pitch", params_.camPitch_)) {
    ROS_WARN("No camera pitch specified. Looking for %s. Default is 15deg.",
             (ns + "/system/camera/pitch").c_str());
  }
  params_.camHorizontal_ = {90.0};
  if (!ros::param::get(ns + "/system/camera/horizontal", params_.camHorizontal_)) {
    ROS_WARN("No camera horizontal opening specified. Looking for %s. Default is 90deg.",
             (ns + "/system/camera/horizontal").c_str());
  }
  params_.camVertical_ = {60.0};
  if (!ros::param::get(ns + "/system/camera/vertical", params_.camVertical_)) {
    ROS_WARN("No camera vertical opening specified. Looking for %s. Default is 60deg.",
             (ns + "/system/camera/vertical").c_str());
  }
  if (params_.camPitch_.size() != params_.camHorizontal_.size()
      || params_.camPitch_.size() != params_.camVertical_.size()) {
    ROS_WARN("Specified camera fields of view unclear: Not all parameter vectors have same length! Setting to default.");
    params_.camPitch_.clear();
    params_.camPitch_ = {15.0};
    params_.camHorizontal_.clear();
    params_.camHorizontal_ = {90.0};
    params_.camVertical_.clear();
    params_.camVertical_ = {60.0};
  }
  params_.igProbabilistic_ = 0.0;
  if (!ros::param::get(ns + "/nbvp/gain/probabilistic", params_.igProbabilistic_)) {
    ROS_WARN(
        "No gain coefficient for probability of cells specified. Looking for %s. Default is 0.0.",
        (ns + "/nbvp/gain/probabilistic").c_str());
  }
  params_.igFree_ = 0.0;
  if (!ros::param::get(ns + "/nbvp/gain/free", params_.igFree_)) {
    ROS_WARN("No gain coefficient for free cells specified. Looking for %s. Default is 0.0.",
             (ns + "/nbvp/gain/free").c_str());
  }
  params_.igOccupied_ = 0.0;
  if (!ros::param::get(ns + "/nbvp/gain/occupied", params_.igOccupied_)) {
    ROS_WARN("No gain coefficient for occupied cells specified. Looking for %s. Default is 0.0.",
             (ns + "/nbvp/gain/occupied").c_str());
  }
  params_.igUnmapped_ = 1.0;
  if (!ros::param::get(ns + "/nbvp/gain/unmapped", params_.igUnmapped_)) {
    ROS_WARN("No gain coefficient for unmapped cells specified. Looking for %s. Default is 1.0.",
             (ns + "/nbvp/gain/unmapped").c_str());
  }
  params_.igArea_ = 1.0;
  if (!ros::param::get(ns + "/nbvp/gain/area", params_.igArea_)) {
    ROS_WARN("No gain coefficient for mesh area specified. Looking for %s. Default is 1.0.",
             (ns + "/nbvp/gain/area").c_str());
  }
  params_.degressiveCoeff_ = 0.25;
  if (!ros::param::get(ns + "/nbvp/gain/degressive_coeff", params_.degressiveCoeff_)) {
    ROS_WARN(
        "No degressive factor for gain accumulation specified. Looking for %s. Default is 0.25.",
        (ns + "/nbvp/gain/degressive_coeff").c_str());
  }
  params_.cuCoeff_ = 0.5;
  if (!ros::param::get(ns + "/nbvp/gain/cu_coeff", params_.cuCoeff_)) {
    ROS_WARN(
            "No cost-utility factor for gain accumulation specified. Looking for %s. Default is 0.5.",
            (ns + "/nbvp/gain/cu_coeff").c_str());
  }
  params_.radiusInfluence_ = 8.0;
  if (!ros::param::get(ns + "/nbvp/gain/radius_influence", params_.radiusInfluence_)) {
    ROS_WARN("No radius of influence specified. Looking for %s. Default is 8.0.",
             (ns + "/nbvp/gain/radius_influence").c_str());
  }
  params_.voronoiBias_ = 0.95;
  if (!ros::param::get(ns + "/nbvp/voronoi_bias", params_.voronoiBias_)) {
    ROS_WARN("No Voronoi bias specified. Looking for %s. Default is 0.95.",
             (ns + "/nbvp/vornoi_bias").c_str());
  }
  params_.extensionRange_ = 1.0;
  if (!ros::param::get(ns + "/nbvp/tree/extension_range", params_.extensionRange_)) {
    ROS_WARN("No value for maximal extension range specified. Looking for %s. Default is 1.0m.",
             (ns + "/nbvp/tree/extension_range").c_str());
  }
  params_.initIterations_ = 15;
  if (!ros::param::get(ns + "/nbvp/tree/initial_iterations", params_.initIterations_)) {
    ROS_WARN("No number of initial tree iterations specified. Looking for %s. Default is 15.",
             (ns + "/nbvp/tree/initial_iterations").c_str());
  }
  params_.dt_ = 0.1;
  if (!ros::param::get(ns + "/nbvp/dt", params_.dt_)) {
    ROS_WARN("No sampling time step specified. Looking for %s. Default is 0.1s.",
             (ns + "/nbvp/dt").c_str());
  }
  params_.gainRange_ = 1.0;
  if (!ros::param::get(ns + "/nbvp/gain/range", params_.gainRange_)) {
    ROS_WARN("No gain range specified. Looking for %s. Default is 1.0m.",
             (ns + "/nbvp/gain/range").c_str());
  }
  if (!ros::param::get(ns + "/bbx/minX", params_.minX_)) {
    ROS_WARN("No x-min value specified. Looking for %s", (ns + "/bbx/minX").c_str());
    ret = false;
  }
  if (!ros::param::get(ns + "/bbx/minY", params_.minY_)) {
    ROS_WARN("No y-min value specified. Looking for %s", (ns + "/bbx/minY").c_str());
    ret = false;
  }
  if (!ros::param::get(ns + "/bbx/minZ", params_.minZ_)) {
    ROS_WARN("No z-min value specified. Looking for %s", (ns + "/bbx/minZ").c_str());
    ret = false;
  }
  if (!ros::param::get(ns + "/bbx/maxX", params_.maxX_)) {
    ROS_WARN("No x-max value specified. Looking for %s", (ns + "/bbx/maxX").c_str());
    ret = false;
  }
  if (!ros::param::get(ns + "/bbx/maxY", params_.maxY_)) {
    ROS_WARN("No y-max value specified. Looking for %s", (ns + "/bbx/maxY").c_str());
    ret = false;
  }
  if (!ros::param::get(ns + "/bbx/maxZ", params_.maxZ_)) {
    ROS_WARN("No z-max value specified. Looking for %s", (ns + "/bbx/maxZ").c_str());
    ret = false;
  }
  params_.softBounds_ = false;
  if (!ros::param::get(ns + "/bbx/softBounds", params_.softBounds_)) {
    ROS_WARN(
        "Not specified whether scenario bounds are soft or hard. Looking for %s. Default is false",
        (ns + "/bbx/softBounds").c_str());
  }
  params_.boundingBox_[0] = 0.5;
  if (!ros::param::get(ns + "/system/bbx/x", params_.boundingBox_[0])) {
    ROS_WARN("No x size value specified. Looking for %s. Default is 0.5m.",
             (ns + "/system/bbx/x").c_str());
  }
  params_.boundingBox_[1] = 0.5;
  if (!ros::param::get(ns + "/system/bbx/y", params_.boundingBox_[1])) {
    ROS_WARN("No y size value specified. Looking for %s. Default is 0.5m.",
             (ns + "/system/bbx/y").c_str());
  }
  params_.boundingBox_[2] = 0.3;
  if (!ros::param::get(ns + "/system/bbx/z", params_.boundingBox_[2])) {
    ROS_WARN("No z size value specified. Looking for %s. Default is 0.3m.",
             (ns + "/system/bbx/z").c_str());
  }
  params_.cuttoffIterations_ = 200;
  if (!ros::param::get(ns + "/nbvp/tree/cuttoff_iterations", params_.cuttoffIterations_)) {
    ROS_WARN("No cuttoff iterations value specified. Looking for %s. Default is 200.",
             (ns + "/nbvp/tree/cuttoff_iterations").c_str());
  }
  params_.zero_gain_ = 0.0;
  if (!ros::param::get(ns + "/nbvp/gain/zero", params_.zero_gain_)) {
    ROS_WARN("No zero gain value specified. Looking for %s. Default is 0.0.",
             (ns + "/nbvp/gain/zero").c_str());
  }
  params_.dOvershoot_ = 0.5;
  if (!ros::param::get(ns + "/system/bbx/overshoot", params_.dOvershoot_)) {
    ROS_WARN(
        "No estimated overshoot value for collision avoidance specified. Looking for %s. Default is 0.5m.",
        (ns + "/system/bbx/overshoot").c_str());
  }
  params_.log_ = false;
  if (!ros::param::get(ns + "/nbvp/log/on", params_.log_)) {
    ROS_WARN("Logging is off by default. Turn on with %s: true", (ns + "/nbvp/log/on").c_str());
  }
  params_.log_throttle_ = 0.5;
  if (!ros::param::get(ns + "/nbvp/log/throttle", params_.log_throttle_)) {
    ROS_WARN("No throttle time for logging specified. Looking for %s. Default is 0.5s.",
             (ns + "/nbvp/log/throttle").c_str());
  }
  params_.navigationFrame_ = "world";
  if (!ros::param::get(ns + "/tf_frame", params_.navigationFrame_)) {
    ROS_WARN("No navigation frame specified. Looking for %s. Default is 'world'.",
             (ns + "/tf_frame").c_str());
  }
  params_.pcl_throttle_ = 0.333;
  if (!ros::param::get(ns + "/pcl_throttle", params_.pcl_throttle_)) {
    ROS_WARN(
        "No throttle time constant for the point cloud insertion specified. Looking for %s. Default is 0.333.",
        (ns + "/pcl_throttle").c_str());
  }
  params_.inspection_throttle_ = 0.25;
  if (!ros::param::get(ns + "/inspection_throttle", params_.inspection_throttle_)) {
    ROS_WARN(
        "No throttle time constant for the inspection view insertion specified. Looking for %s. Default is 0.1.",
        (ns + "/inspection_throttle").c_str());
  }
  params_.exact_root_ = true;
  if (!ros::param::get(ns + "/nbvp/tree/exact_root", params_.exact_root_)) {
    ROS_WARN("No option for exact root selection specified. Looking for %s. Default is true.",
             (ns + "/nbvp/tree/exact_root").c_str());
  }
  params_.explr_mode_ = 0;
  if (!ros::param::get(ns + "/nbvp/mode", params_.explr_mode_)) {
    ROS_WARN("No mode for exploration specified. Looking for %s. Default is NBVP without coordination.",
             (ns + "/nbvp/mode").c_str());
  }
  return ret;
}

template<typename stateVec>
void nbvInspection::nbvPlanner<stateVec>::insertPointcloudWithTf(
    const sensor_msgs::PointCloud2::ConstPtr& pointcloud)
{
  static double last = ros::Time::now().toSec();
  if (last + params_.pcl_throttle_ < ros::Time::now().toSec()) {
    tree_->insertPointcloudWithTf(pointcloud);
    last += params_.pcl_throttle_;
  }
}

template<typename stateVec>
void nbvInspection::nbvPlanner<stateVec>::insertPointcloudWithTfCamUp(
    const sensor_msgs::PointCloud2::ConstPtr& pointcloud)
{
  static double last = ros::Time::now().toSec();
  if (last + params_.pcl_throttle_ < ros::Time::now().toSec()) {
    tree_->insertPointcloudWithTf(pointcloud);
    last += params_.pcl_throttle_;
  }
}

template<typename stateVec>
void nbvInspection::nbvPlanner<stateVec>::insertPointcloudWithTfCamDown(
    const sensor_msgs::PointCloud2::ConstPtr& pointcloud)
{
  static double last = ros::Time::now().toSec();
  if (last + params_.pcl_throttle_ < ros::Time::now().toSec()) {
    tree_->insertPointcloudWithTf(pointcloud);
    last += params_.pcl_throttle_;
  }
}

template<typename stateVec>
void nbvInspection::nbvPlanner<stateVec>::evasionCallback(
    const multiagent_collision_check::Segment& segmentMsg)
{
  tree_->evade(segmentMsg);
}

template<typename stateVec>
void nbvInspection::nbvPlanner<stateVec>::addRrts(const multiagent_collision_check::Node& rrtMsg) {
  if (rrts_.size() != 3) {
    for (int i = 0; i < 3; i++)
      rrts_.push_back(new std::vector<Eigen::Vector3d>);
  }

  for (int i = 0; i < 3; i++) {
    if (rrts_[i]->size() <= 0) {
      rrts_[i]->push_back(Eigen::Vector3d(rrtMsg.prev_state.x, rrtMsg.prev_state.y, rrtMsg.prev_state.z));
      rrts_[i]->push_back(Eigen::Vector3d(rrtMsg.target.x, rrtMsg.target.y, rrtMsg.target.z));
      return;
    }

    bool flag;
    flag = (*rrts_[i])[1][0] == rrtMsg.prev_state.x
           && (*rrts_[i])[1][1] == rrtMsg.prev_state.y && (*rrts_[i])[1][2] == rrtMsg.prev_state.z;
    if (flag) {
      rrts_[i]->clear();
      rrts_[i]->push_back(Eigen::Vector3d(rrtMsg.prev_state.x, rrtMsg.prev_state.y, rrtMsg.prev_state.z));
      rrts_[i]->push_back(Eigen::Vector3d(rrtMsg.target.x, rrtMsg.target.y, rrtMsg.target.z));
      return;
    }
  }
}

/*---------------- Volumetric RRT Method ----------------------*/
template<typename stateVec>
bool nbvInspection::nbvPlanner<stateVec>::VRRT__plannerCallback(nbvplanner::nbvp_srv::Request& req, nbvplanner::nbvp_srv::Response& res)
{
  double rate;
  ros::Time computationTime = ros::Time::now();
  // Check that planner is ready to compute path.
  if (!ros::ok()) {
    ROS_INFO_THROTTLE(1, "Exploration finished. Not planning any further moves.");
    return true;
  }

  if (!ready_) {
    ROS_ERROR_THROTTLE(1, "Planner not set up: Planner not ready!");
    return true;
  }
  if (manager_ == NULL) {
    ROS_ERROR_THROTTLE(1, "Planner not set up: No octomap available!");
    return true;
  }
  if (manager_->getMapSize().norm() <= 0.0) {
    ROS_ERROR_THROTTLE(1, "Planner not set up: Octomap is empty!");
    return true;
  }
  res.path.clear();

  pcl::PointCloud<pcl::PointXYZRGB>::Ptr points(new pcl::PointCloud<pcl::PointXYZRGB>);

  Eigen::Vector4d r1 = tree_->getRoot();
  Eigen::Vector4d r2 = tree_->getRoot();

  // Clear old tree and reinitialize.
  Eigen::Vector3d prev_state;
  for (int i = 0; i < 3; i++)
    prev_state[i] = 0.0;
  if (cnt > 0) {
    Eigen::Vector4d prev_target = tree_->getBest();
    for (int i = 0; i < 3; i++)
      prev_state[i] = prev_target[i];
  }
  if (rrts_.size() != 3) {
    peer_target.push_back(Eigen::Vector4d(0,0,0,0));
    peer_target.push_back(Eigen::Vector4d(0,0,0,0));
  } else {
    for (int i=0; i<3; i++) {
      bool flag = false;
      if (rrts_[i]->size() > 0) {
        flag = (*rrts_[i])[1][0] == prev_state[0]
               && (*rrts_[i])[1][1] == prev_state[1] && (*rrts_[i])[1][2] == prev_state[2];
        if (not flag) {
          peer_target.push_back(Eigen::Vector4d((*rrts_[i])[1][0], (*rrts_[i])[1][1], (*rrts_[i])[1][2], 1));
        }
      } else {
        peer_target.push_back(Eigen::Vector4d(0,0,0,0));
      }
    }
  }
  tree_->clear();
  tree_->VRRT_initialize();
  cnt++;
  vector_t path;
  // Iterate the tree construction method.
  int loopCount = 0;
  while ((!tree_->gainFound() || tree_->getCounter() < params_.initIterations_) && ros::ok()) {
    if (tree_->getCounter() > params_.cuttoffIterations_) {
      ROS_INFO("No gain found, shutting down");

      std::cout << "Exploration finished, time elapsed: " << ros::Time::now().toSec() << std::endl;

      rate = manager_->explorationRate(1);
      std::cout << "Exploration Rate: " << rate << std::endl;
      ros::shutdown();
      return true;
    }
    if (loopCount > 1000 * (tree_->getCounter() + 1)) {
      ROS_INFO_THROTTLE(1, "Exceeding maximum failed iterations, return to previous point!");
      res.path = tree_->getPathBackToPrevious(req.header.frame_id);
      return true;
    }
    peer_target.clear();
    if (rrts_.size() != 3) {
      peer_target.push_back(Eigen::Vector4d(0,0,0,0));
      peer_target.push_back(Eigen::Vector4d(0,0,0,0));
    } else {
      for (int i=0; i<3; i++) {
        bool flag = false;
        if (rrts_[i]->size() > 0) {
          flag = (*rrts_[i])[1][0] == prev_state[0]
                 && (*rrts_[i])[1][1] == prev_state[1] && (*rrts_[i])[1][2] == prev_state[2];
          if (not flag) {
            peer_target.push_back(Eigen::Vector4d((*rrts_[i])[1][0], (*rrts_[i])[1][1], (*rrts_[i])[1][2], 1));
          }
        } else {
          peer_target.push_back(Eigen::Vector4d(0,0,0,0));
        }
      }
    }
    tree_->VRRT_iterate(peer_target);
    loopCount++;
  }

  Eigen::Vector4d target_node = tree_->getBest();
  multiagent_collision_check::Node target_msg;
  target_msg.prev_state.x = prev_state[0];
  target_msg.prev_state.y = prev_state[1];
  target_msg.prev_state.z = prev_state[2];
  target_msg.target.x = target_node[0];
  target_msg.target.y = target_node[1];
  target_msg.target.z = target_node[2];
  peerRrtPub_.publish(target_msg);

  tree_->getLeafNode(1);

  std::vector<nbvInspection::Node<stateVec>*> candidates = tree_->getCandidates();

  // Extract the best edge.
  res.path = tree_->VRRT_getBestEdge(req.header.frame_id);

  for(int i =0; i<res.path.size(); i++ ){
    geometry_msgs::Pose ret = res.path[i];
    pcl::PointXYZRGB point;
    point.x = ret.position.x;
    point.y = ret.position.y;
    point.z = ret.position.z;
    point.r = 200; point.g = 200; point.b = 200;
    points->push_back(point);
  }

  pcl::PointXYZRGB point1;
  point1.x = r1[0];
  point1.y = r1[1];
  point1.z = r1[2];
  point1.r = 200; point1.g = 0; point1.b = 0;
  points->push_back(point1);

  pcl::PointXYZRGB point2;
  point2.x = r2[0];
  point2.y = r2[1];
  point2.z = r2[2];
  point2.r = 0; point2.g = 200; point2.b = 0;
  points->push_back(point2);

  // Plot points
  sensor_msgs::PointCloud2 pc2;
  pcl::toROSMsg(*points, pc2);
  pc2.header.frame_id = "world";
  pub_path.publish(pc2);

  tree_->memorizeBestBranch();
  // Publish path to block for other agents (multi agent only).
  multiagent_collision_check::Segment segment;
  segment.header.stamp = ros::Time::now();
  segment.header.frame_id = params_.navigationFrame_;
  if (!res.path.empty()) {
    segment.poses.push_back(res.path.front());
    segment.poses.push_back(res.path.back());
  }
  evadePub_.publish(segment);

  ROS_INFO("Path computation lasted %2.3fs", (ros::Time::now() - computationTime).toSec());

  std::ofstream outFile("Data.txt", std::ios_base::out|std::ios_base::app);

//  std::string sentence = "time: " + std::to_string(ros::Time::now().toSec()) + "s, Exploration Rate: " + std::to_string(rate*100);

      outFile << "time: " << ros::Time::now().toSec() << "s, Exploration Rate: " << rate*100 << "%" << std::endl;
      outFile.close();
  return true;
}
#endif // NBVP_HPP_
