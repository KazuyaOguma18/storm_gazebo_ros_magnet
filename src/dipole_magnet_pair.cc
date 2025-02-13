/*
 * Copyright (c) 2016, Vanderbilt University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Addisu Z. Taddese
 */

#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>

#include <ros/callback_queue.h>
#include <ros/advertise_options.h>
#include <ros/ros.h>

#include <iostream>
#include <vector>
#include <cstdint>
#include <functional>

#include "storm_gazebo_ros_magnet/dipole_magnet_pair.h"

namespace gazebo {

DipoleMagnetPair::DipoleMagnetPair(): ModelPlugin() {
  this->connect_count = 0;
}

DipoleMagnetPair::~DipoleMagnetPair() {
  this->update_connection.reset();
  if (this->should_publish) {
    this->queue.clear();
    this->queue.disable();
    this->rosnode->shutdown();
    this->callback_queue_thread.join();
    delete this->rosnode;
  }
  // if (this->mag.first && this->mag.second) {
  //   DipoleMagnetContainer::Get().Remove(this->mag);
  // }
}

void DipoleMagnetPair::Load(physics::ModelPtr _parent, sdf::ElementPtr _sdf) {
  // Store the pointer to the model
  this->model = _parent;
  this->world = _parent->GetWorld();
  gzdbg << "Loading DipoleMagnetPair plugin" << std::endl;

  this->mag = std::make_pair(std::make_shared<DipoleMagnetContainer::Magnet>(),
                             std::make_shared<DipoleMagnetContainer::Magnet>());

  // load parameters
  this->robot_namespace = "";

  if(!_sdf->HasElement("parentBodyName")) {
    gzerr << "DipoleMagnetPair plugin missing <parentBodyName>, cannot proceed" << std::endl;
    return;
  }else {
    this->link_name.first = _sdf->GetElement("parentBodyName")->Get<std::string>();
  }

  if(!_sdf->HasElement("childBodyName")) {
    gzerr << "DipoleMagnetPair plugin missing <childBodyName>, cannot proceed" << std::endl;
    return;
  }else {
    this->link_name.second = _sdf->GetElement("childBodyName")->Get<std::string>();
  }

  this->link.first = this->model->GetLink(this->link_name.first);
  if(!this->link.first){
    gzerr << "Error: link named " << this->link_name.first << " does not exist" << std::endl;
    return;
  }
  this->link.second = this->model->GetLink(this->link_name.second);
  if(!this->link.second){
    gzerr << "Error: link named " << this->link_name.second << " does not exist" << std::endl;
    return;
  }

  this->should_publish = false;
  if (_sdf->HasElement("shouldPublish"))
  {
    this->should_publish = _sdf->GetElement("shouldPublish")->Get<bool>();
  }

  if (!_sdf->HasElement("updateRate"))
  {
    gzmsg << "DipoleMagnetPair plugin missing <updateRate>, defaults to 0.0"
        " (as fast as possible)" << std::endl;
    this->update_rate = 0;
  }
  else
    this->update_rate = _sdf->GetElement("updateRate")->Get<double>();

  if (_sdf->HasElement("parent_dipole_moment")){
    this->mag.first->moment = _sdf->Get<ignition::math::Vector3d>("parent_dipole_moment");
  }

  if (_sdf->HasElement("child_dipole_moment")){
    this->mag.second->moment = _sdf->Get<ignition::math::Vector3d>("child_dipole_moment");
  }

  if (_sdf->HasElement("parentxyzOffset")){
    this->mag.first->offset.Pos() = _sdf->Get<ignition::math::Vector3d>("xyzOffset");
  }

  if (_sdf->HasElement("childxyzOffset")){
    this->mag.first->offset.Pos() = _sdf->Get<ignition::math::Vector3d>("xyzOffset");
  }

  if (_sdf->HasElement("parentrpyOffset")){
    ignition::math::Vector3d rpy_offset = _sdf->Get<ignition::math::Vector3d>("rpyOffset");
    this->mag.first->offset.Rot() = ignition::math::Quaterniond(rpy_offset);
  }

  if (_sdf->HasElement("childrpyOffset")){
    ignition::math::Vector3d rpy_offset = _sdf->Get<ignition::math::Vector3d>("rpyOffset");
    this->mag.second->offset.Rot() = ignition::math::Quaterniond(rpy_offset);
  }

  if (this->should_publish) {
    if (!_sdf->HasElement("topicNs"))
    {
      gzmsg << "DipoleMagnetPair plugin missing <topicNs>," 
          "will publish on namespace " << this->link_name.first << std::endl;
    }
    else {
      this->topic_ns = _sdf->GetElement("topicNs")->Get<std::string>();
    }

    if (!ros::isInitialized())
    {
      gzerr << "A ROS node for Gazebo has not been initialized, unable to load "
        "plugin. Load the Gazebo system plugin 'libgazebo_ros_api_plugin.so' in "
        "the gazebo_ros package. If you want to use this plugin without ROS, "
        "set <shouldPublish> to false" << std::endl;
      return;
    }

    this->rosnode = new ros::NodeHandle(this->robot_namespace);
    this->rosnode->setCallbackQueue(&this->queue);

    this->wrench_pub = this->rosnode->advertise<geometry_msgs::WrenchStamped>(
        this->topic_ns + "/wrench", 1,
        boost::bind( &DipoleMagnetPair::Connect,this),
        boost::bind( &DipoleMagnetPair::Disconnect,this), ros::VoidPtr(), &this->queue);
    this->mfs_pub = this->rosnode->advertise<sensor_msgs::MagneticField>(
        this->topic_ns + "/mfs", 1,
        boost::bind( &DipoleMagnetPair::Connect,this),
        boost::bind( &DipoleMagnetPair::Disconnect,this), ros::VoidPtr(), &this->queue);

    // Custom Callback Queue
    this->callback_queue_thread = boost::thread( boost::bind( &DipoleMagnetPair::QueueThread,this ) );
  }

  gzmsg << "Loaded Gazebo dipole magnet plugin on " << this->model->GetName() << std::endl;

  // Listen to the update event. This event is broadcast every
  // simulation iteration.
  this->update_connection = event::Events::ConnectWorldUpdateBegin(
      boost::bind(&DipoleMagnetPair::OnUpdate, this, _1));
}

void DipoleMagnetPair::Connect() {
  this->connect_count++;
}

void DipoleMagnetPair::Disconnect() {
  this->connect_count--;
}

void DipoleMagnetPair::QueueThread() {
  static const double timeout = 0.01;

  while (this->rosnode->ok())
  {
    this->queue.callAvailable(ros::WallDuration(timeout));
  }
}

// Called by the world update start event
void DipoleMagnetPair::OnUpdate(const common::UpdateInfo & /*_info*/) {

  // Calculate the force from all other magnets
  ignition::math::Pose3d p_self = this->link.first->WorldCoGPose();
  p_self.Pos() += -p_self.Rot().RotateVector(this->mag.first->offset.Pos());
  p_self.Rot() *= this->mag.first->offset.Rot().Inverse();

  ignition::math::Pose3d p_other = this->link.second->WorldCoGPose();
  p_other.Pos() += -p_other.Rot().RotateVector(this->mag.second->offset.Pos());
  p_other.Rot() *= this->mag.second->offset.Rot().Inverse();  

  ignition::math::Vector3d moment_world = p_self.Rot().RotateVector(this->mag.first->moment);

  ignition::math::Vector3d force(0, 0, 0);
  ignition::math::Vector3d torque(0, 0, 0);
  ignition::math::Vector3d mfs(0, 0, 0);
  
  ignition::math::Vector3d m_other = p_other.Rot().RotateVector(this->mag.second->moment);

  ignition::math::Vector3d force_tmp;
  ignition::math::Vector3d torque_tmp;
  GetForceTorque(p_self, moment_world, p_other, m_other, force_tmp, torque_tmp);

  force += force_tmp;
  torque += torque_tmp;

  ignition::math::Vector3<double> mfs_tmp;
  GetMFS(p_self, p_other, m_other, mfs_tmp);

  mfs += mfs_tmp;

  this->link.first->AddForce(force_tmp);
  this->link.first->AddTorque(torque_tmp);
  this->link.second->AddForce(force_tmp * (-1));
  this->link.second->AddTorque(torque_tmp * (-1));

  this->PublishData(force, torque, mfs);
}

void DipoleMagnetPair::PublishData(
    const ignition::math::Vector3d& force,
    const ignition::math::Vector3d& torque,
    const ignition::math::Vector3d& mfs){
  if(this->should_publish && this->connect_count > 0) {
    // Rate control
    common::Time cur_time = this->world->SimTime();
    if (this->update_rate > 0 &&
        (cur_time-this->last_time).Double() < (1.0/this->update_rate))
      return;

    this->lock.lock();
    // copy data into wrench message
    this->wrench_msg.header.frame_id = "world";
    this->wrench_msg.header.stamp.sec = cur_time.sec;
    this->wrench_msg.header.stamp.nsec = cur_time.nsec;

    this->wrench_msg.wrench.force.x    = force[0];
    this->wrench_msg.wrench.force.y    = force[1];
    this->wrench_msg.wrench.force.z    = force[2];
    this->wrench_msg.wrench.torque.x   = torque[0];
    this->wrench_msg.wrench.torque.y   = torque[1];
    this->wrench_msg.wrench.torque.z   = torque[2];


    // now mfs
    this->mfs_msg.header.frame_id = this->link_name.first;
    this->mfs_msg.header.stamp.sec = cur_time.sec;
    this->mfs_msg.header.stamp.nsec = cur_time.nsec;

    this->mfs_msg.magnetic_field.x = mfs[0];
    this->mfs_msg.magnetic_field.y = mfs[1];
    this->mfs_msg.magnetic_field.z = mfs[2];


    this->wrench_pub.publish(this->wrench_msg);
    this->mfs_pub.publish(this->mfs_msg);

    this->lock.unlock();
  }
}


void DipoleMagnetPair::GetForceTorque(const ignition::math::Pose3d& p_self,
    const ignition::math::Vector3d& m_self,
    const ignition::math::Pose3d& p_other,
    const ignition::math::Vector3d& m_other,
    ignition::math::Vector3d& force,
    ignition::math::Vector3d& torque) {

  bool debug = false;
  ignition::math::Vector3d p = p_self.Pos() - p_other.Pos();
  ignition::math::Vector3d p_unit = p/p.Length();

  ignition::math::Vector3d m1 = m_other;
  ignition::math::Vector3d m2 = m_self;
  if (debug)
    std::cout << "p: " << p << " m1: " << m1 << " m2: " << m2 << std::endl;

  double K = 3.0*1e-7/pow(p.Length(), 4);
  force = K * (m2 * (m1.Dot(p_unit)) +  m1 * (m2.Dot(p_unit)) +
      p_unit*(m1.Dot(m2)) - 5*p_unit*(m1.Dot(p_unit))*(m2.Dot(p_unit)));

  double Ktorque = 1e-7/pow(p.Length(), 3);
  ignition::math::Vector3d B1 = Ktorque*(3*(m1.Dot(p_unit))*p_unit - m1);
  torque = m2.Cross(B1);
  if (debug)
    std::cout << "B: " << B1 << " K: " << Ktorque << " t: " << torque << std::endl;
}

void DipoleMagnetPair::GetMFS(const ignition::math::Pose3d& p_self,
    const ignition::math::Pose3d& p_other,
    const ignition::math::Vector3d& m_other,
    ignition::math::Vector3d& mfs) {

  // sensor location
  ignition::math::Vector3d p = p_self.Pos() - p_other.Pos();
  ignition::math::Vector3d p_unit = p/p.Length();

  // Get the field at the sensor location
  double K = 1e-7/pow(p.Length(), 3);
  ignition::math::Vector3d B = K*(3*(m_other.Dot(p_unit))*p_unit - m_other);

  // Rotate the B vector into the capsule/body frame
  ignition::math::Vector3d B_body = p_self.Rot().RotateVectorReverse(B);

  // Assign vector
  mfs[0] = B_body[0];
  mfs[1] = B_body[1];
  mfs[2] = B_body[2];
}

// Register this plugin with the simulator
GZ_REGISTER_MODEL_PLUGIN(DipoleMagnetPair)

}
