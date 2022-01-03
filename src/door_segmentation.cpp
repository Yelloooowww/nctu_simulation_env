#include <stdio.h>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <ros/ros.h>
#include <tf/tf.h>
#include <tf/transform_listener.h>
#include <tf/transform_broadcaster.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/LaserScan.h>
#include <pcl/point_types.h>
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>
#include <geometry_msgs/Vector3.h>
#include <geometry_msgs/PoseStamped.h>
#include <gazebo_msgs/SetModelState.h>
#include <gazebo_msgs/GetModelState.h>
#include <gazebo_msgs/GetJointProperties.h>
// #include "Quat.h"
#include <Eigen/Geometry>
#include <Eigen/Dense>
#include <cmath>

using namespace ros;
using namespace std;
using namespace pcl;
using namespace Eigen;


class Door_segmentation
{
private:
    tf::TransformBroadcaster tf_br;
    tf::TransformListener tf_listener;
    tf::StampedTransform tf_pose;
    tf::Transform transform;
    tf::Quaternion q;
    Eigen::Matrix4f pose_matrix;

    float door_length, door_width, door_height, modelpose_x, modelpose_y, modelpose_z, modelpose_row, modelpose_yaw, modelpose_pitch;
    sensor_msgs::PointCloud2 pc_msg;
    PointCloud<PointXYZ> pc_raw;
    PointCloud<PointXYZRGB> pc_label;

    Publisher pub_map, pub_scan_label;
    Subscriber sub_map, sub_scan;
    ServiceClient ser_client, joint_client;

    gazebo_msgs::GetModelState getmodelstate;
    gazebo_msgs::GetJointProperties getjointproperties;

    float rotation_rad, rotate_x, rotate_y;

public:
    Door_segmentation(NodeHandle &nh);
    void pc_cb(const sensor_msgs::PointCloud2 msg);
    void scan_cb(const sensor_msgs::LaserScan msg);
    bool get_tf();
    ~Door_segmentation();
};

Door_segmentation::Door_segmentation(NodeHandle &nh)
{
    param::get("~door_length", door_length );
    param::get("~door_width", door_width );
    param::get("~door_height", door_height );
    param::get("~modelpose_x", modelpose_x );
    param::get("~modelpose_y", modelpose_y );
    param::get("~modelpose_z", modelpose_z );
    param::get("~modelpose_row", modelpose_row );
    param::get("~modelpose_yaw", modelpose_yaw );
    param::get("~modelpose_pitch", modelpose_pitch );


    pub_map = nh.advertise<sensor_msgs::PointCloud2>("door_detection", 1);
    pub_scan_label = nh.advertise<sensor_msgs::LaserScan>("/RL/scan_label", 1);
    sub_map = nh.subscribe("points", 1, &Door_segmentation::pc_cb, this);
    sub_scan = nh.subscribe("/RL/scan", 1, &Door_segmentation::scan_cb, this);
    ser_client = nh.serviceClient<gazebo_msgs::GetModelState>("/gazebo/get_model_state");
    joint_client = nh.serviceClient<gazebo_msgs::GetJointProperties>("/gazebo/get_joint_properties");

    ROS_INFO("door detection initialized");
}

Door_segmentation::~Door_segmentation()
{
}

bool Door_segmentation::get_tf(){
  //map frame tf
      getmodelstate.request.model_name = "X1";
      if (ser_client.call(getmodelstate)) ;
      else{
        ROS_ERROR("Failed to call service (door_1)");
        return 0;
      }
      transform.setOrigin(tf::Vector3(getmodelstate.response.pose.position.x,
                                      getmodelstate.response.pose.position.y,
                                      getmodelstate.response.pose.position.z));
      transform.setRotation(tf::Quaternion(getmodelstate.response.pose.orientation.x,
                                            getmodelstate.response.pose.orientation.y,
                                            getmodelstate.response.pose.orientation.z,
                                            getmodelstate.response.pose.orientation.w));
      tf_br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), "map", "base_link"));


      // door frame TF
      getmodelstate.request.model_name = "door_1";
      if (ser_client.call(getmodelstate)) ;
      else{
        ROS_ERROR("Failed to call service (door_1)");
        return 0;
      }
      getjointproperties.request.joint_name = "door_1::door_hinge";
      if (joint_client.call(getjointproperties)) ;
      else{
        ROS_ERROR("Failed to call service (door_hinge)");
        return 0;
      }
      rotation_rad = getjointproperties.response.position[0];
      rotate_x = getmodelstate.response.pose.position.x - modelpose_y*cos(modelpose_pitch)-modelpose_x*sin(modelpose_pitch) ;
      rotate_y = getmodelstate.response.pose.position.y - modelpose_y*sin(modelpose_pitch)-modelpose_x*cos(modelpose_pitch) ;
      // TransformBroadcaster
      transform.setOrigin( tf::Vector3(rotate_x, rotate_y, 0) ); // z = 0
      q.setRPY(0, 0, rotation_rad - modelpose_pitch);//tf::Quaternion
      transform.setRotation(q);//tf::Transform transform;
      tf_br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), "map", "model_door"));


      try
      {
          tf_listener.waitForTransform("model_door", "front_laser", ros::Time(0), ros::Duration(0.2));
          tf_listener.lookupTransform("model_door", "front_laser", ros::Time(0), tf_pose);
      }
      catch (tf::TransformException ex)
      {
          ROS_ERROR("%s", ex.what());
          return 0;
      }

      return 1;
}
void Door_segmentation::scan_cb(const sensor_msgs::LaserScan msg){
    if(!get_tf()) return;

    sensor_msgs::LaserScan output_scan = msg;
    output_scan.ranges.assign(msg.ranges.size(), std::numeric_limits<double>::infinity());
    output_scan.intensities.assign(msg.ranges.size(), std::numeric_limits<double>::infinity());

    if(msg.ranges.size()>0){
        float o_t_min, o_t_max, o_t_inc;
        o_t_min = msg.angle_min;
        o_t_max = msg.angle_max;
        o_t_inc = msg.angle_increment;

        int count = 0;
        for(int i=0;i<msg.ranges.size();i++){
          float theta = o_t_min+i*o_t_inc;
          float r = msg.ranges[i];
          geometry_msgs::PointStamped pt;
          pt.header.frame_id = "front_laser";
          pt.point.x = r * cos(theta);
          pt.point.y = r * sin(theta);
          try {
            tf_listener.transformPoint("model_door", pt, pt);
          }catch (tf::TransformException ex)
          {
              ROS_ERROR("%s", ex.what());
              return;
          }

          output_scan.ranges[i] = msg.ranges[i];
          if((pt.point.x<door_width*1.5) && (pt.point.x>-door_width*1.5) && (pt.point.y<(door_length+door_width)*1.05) && (pt.point.y>door_width)){
            output_scan.intensities[i] = 255;
            count++;
          }else output_scan.intensities[i] = 0;
          cout<<"total scanpoints:"<<msg.ranges.size()<<" segmentation:"<<count<<endl;
        }
    }
    output_scan.header = msg.header;
    output_scan.angle_min = msg.angle_min;
    output_scan.angle_max = msg.angle_max;
    output_scan.angle_increment = msg.angle_increment;
    output_scan.time_increment = msg.time_increment;
    output_scan.scan_time = msg.scan_time;
    output_scan.range_min = msg.range_min;
    output_scan.range_max = msg.range_max;
    pub_scan_label.publish(output_scan);
}

void Door_segmentation::pc_cb(const sensor_msgs::PointCloud2 msg){
    if(! get_tf()) return;
    // PointCloud Transform
    fromROSMsg(msg, pc_raw);
    pcl_ros::transformAsMatrix(tf_pose, pose_matrix);
    pcl::transformPointCloud(pc_raw, pc_raw, pose_matrix);
    pc_label.clear();
    if(pc_raw.size()>0){
        int count = 0;
        for(int i=0;i<pc_raw.size();i++){
          PointXYZRGB pt;
          pt.x = pc_raw[i].x;
          pt.y = pc_raw[i].y;
          pt.z = pc_raw[i].z;
          pt.r = 255;
          pt.g = 255;
          pt.b = 255;

          if((pt.x<door_width*1.5) && (pt.x>-door_width*1.5) && (pt.y<(door_length+door_width)*1.05) && (pt.y>door_width) && (pt.z<door_height*1.1) && (pt.z>0.05))
            pt.r = 0, pt.g = 0, count++;

          pc_label.push_back(pt);
          // cout<<"total points:"<<pc_raw.size()<<" segmentation:"<<count<<endl;
        }
    }

    try
    {
        tf_listener.waitForTransform("front_laser","model_door", ros::Time(0), ros::Duration(0.2));
        tf_listener.lookupTransform("front_laser","model_door", ros::Time(0), tf_pose);
    }
    catch (tf::TransformException ex)
    {
        ROS_ERROR("%s", ex.what());
        return;
    }
    pcl_ros::transformAsMatrix(tf_pose, pose_matrix);
    pcl::transformPointCloud(pc_label, pc_label, pose_matrix);
    toROSMsg(pc_label, pc_msg);
    pc_msg.header.frame_id = "front_laser";
    pub_map.publish(pc_msg);
}

int main(int argc, char **argv)
{
    init(argc, argv, "door_segmentation");
    NodeHandle nh;

    Door_segmentation door_segmentation(nh);

    spin();

    return 0;
}