#include "camera_direction_determinator/camera_direction_determinator.h"

CameraDirectionDeterminator::CameraDirectionDeterminator()
    : private_nh_("~"), tf_listener_(tf_buffer_), start_time_(ros::Time::now()) {
    private_nh_.param("HZ", HZ, 10);
    private_nh_.param("MIN_CLUSTER", MIN_CLUSTER, 300);
    private_nh_.param("MOTION_NOISE", MOTION_NOISE, 0.03);
    private_nh_.param("MEASUREMENT_NOISE", MEASUREMENT_NOISE, 0.1);
    private_nh_.param("LIFETIME_THRESHOLD", LIFETIME_THRESHOLD, 0.1);

    std::vector<color_detector_params_hsv::ThresholdHSV> _;
    color_detector_params_hsv::init(colors_, _);
    dynamixel_pubs_.resize(colors_.size());
    position_subs_.resize(colors_.size());
    angle_subs_.resize(colors_.size());
    color_enable_clients_.resize(colors_.size());
    color_enables_.resize(colors_.size());
    for (size_t i = 0; i < colors_.size(); i++) {
        std::string roomba = "roomba" + std::to_string(i + 1);
        dynamixel_pubs_[i] = nh_.advertise<dynamixel_angle_msgs::DynamixelAngle>(roomba + "/dynamixel/angle", 1);
        position_subs_[i] =
            nh_.subscribe(roomba + "/target/position", 1, &CameraDirectionDeterminator::position_callback, this);
        angle_subs_[i] = nh_.subscribe(roomba + "/target/angle", 1, &CameraDirectionDeterminator::angle_callback, this);
        color_enable_clients_[i] = nh_.serviceClient<color_detector_srvs::ColorEnable>(roomba + "/color_enable");
        for (const auto &color : colors_) {
            color_enables_[i][color] = false;
        }
    }

    ellipse_pub_ = private_nh_.advertise<visualization_msgs::MarkerArray>("ellipses", 1);
    set_color_map();
}

void CameraDirectionDeterminator::update_kalman_filter(size_t idx,
                                                       const color_detector_msgs::TargetPositionConstPtr &pos) {
    geometry_msgs::TransformStamped transform_stamped;
    std::string roomba = "roomba" + std::to_string(idx + 1);
    try {
        transform_stamped = tf_buffer_.lookupTransform("map", roomba + "/camera_link", ros::Time(0));
    } catch (tf2::TransformException &ex) {
        ROS_WARN_STREAM(ex.what());
        ros::Duration(1.0).sleep();
        return;
    }
    geometry_msgs::PoseStamped target_pose;
    calc_target_pose_on_world(roomba, pos, transform_stamped, &target_pose);
    std::string color = colors_[idx];
    if (kalman_filters_.count(color) == 0) {
        kalman_filters_[color].set_motion_noise(MOTION_NOISE);
        kalman_filters_[color].set_measurement_noise(MEASUREMENT_NOISE);
    }
    kalman_filters_[color].update(target_pose.pose.position.x, target_pose.pose.position.y,
                                  (ros::Time::now() - start_time_).toSec());
}

void CameraDirectionDeterminator::calc_target_pose_on_world(std::string roomba,
                                                            const color_detector_msgs::TargetPositionConstPtr &target,
                                                            const geometry_msgs::TransformStamped &transform,
                                                            geometry_msgs::PoseStamped *output_pose) {
    geometry_msgs::PoseStamped target_pose;
    target_pose.header = target->header;
    target_pose.header.frame_id = roomba + "/camera_link";
    target_pose.pose.position.x = target->z;
    target_pose.pose.position.y = -target->x;
    target_pose.pose.position.z = target->y;
    target_pose.pose.orientation.w = 1;
    target_pose.pose.orientation.x = 0;
    target_pose.pose.orientation.y = 0;
    target_pose.pose.orientation.z = 0;

    tf2::doTransform(target_pose, *output_pose, transform);
    return;
}

void CameraDirectionDeterminator::angle_callback(const color_detector_msgs::TargetAngleListConstPtr &angles) {
    if (angles->data.empty()) {
        ROS_WARN("angle list is empty.");
        publish_angle(0, angles->my_number);
        return;
    }
    color_detector_msgs::TargetAngle angle;
    double min_likelihood = 1e5;
    for (const auto &agl : angles->data) {
        if (agl.cluster_num < MIN_CLUSTER) continue;
        kalman_filters_[agl.color].estimate_update((ros::Time::now() - start_time_).toSec());
        double likelihood = kalman_filters_[agl.color].get_likelihood();
        if (likelihood < min_likelihood) {
            angle = agl;
            min_likelihood = likelihood;
        }
    }
    if (min_likelihood == 1e5) {
        ROS_WARN_STREAM("cannnot find roomba");
        publish_angle(0, angles->my_number);
        return;
    }
    if (!isfinite(angle.radian)) {
        ROS_WARN_STREAM(angle.color << "'s radian is " << angle.radian);
        publish_angle(0, angles->my_number);
        return;
    }
    publish_angle(angle.radian, angles->my_number);
    ROS_INFO_STREAM("camera direction to " << angle.color);
    int roomba_idx = angles->my_number;
    call_color_enable_service(&color_enable_clients_[roomba_idx - 1], &color_enables_[roomba_idx - 1], angle.color);
}

void CameraDirectionDeterminator::publish_angle(double radian, int roomba_number) {
    dynamixel_angle_msgs::DynamixelAngle msg;
    msg.theta = radian;
    dynamixel_pubs_[roomba_number - 1].publish(msg);
}

void CameraDirectionDeterminator::call_color_enable_service(ros::ServiceClient *client,
                                                            std::map<std::string, bool> *color_enable,
                                                            std::string color) {
    for (auto itr = color_enable->begin(); itr != color_enable->end(); itr++) {
        if (itr->first == color && itr->second == false) {
            color_detector_srvs::ColorEnable srv;
            srv.request.color = itr->first;
            srv.request.color = true;
            if (client->call(srv)) {
                itr->second = true;
            } else {
                ROS_ERROR_STREAM("Failed to call service color_enable. Couldn't activate " << itr->first << ".");
            }
        }
        if (itr->first != color && itr->second == true) {
            color_detector_srvs::ColorEnable srv;
            srv.request.color = itr->first;
            srv.request.color = false;
            if (client->call(srv)) {
                itr->second = false;
            } else {
                ROS_ERROR_STREAM("Failed to call service color_enable. Couldn't deactivate " << itr->first << ".");
            }
        }
    }
}

void CameraDirectionDeterminator::position_callback(const color_detector_msgs::TargetPositionConstPtr &position) {
    for (size_t i = 0; i < colors_.size(); i++) {
        if (colors_[i] == position->color) {
            update_kalman_filter(i, position);
        }
    }
}

void CameraDirectionDeterminator::set_color_map() {
    color_map_["green"].r = 0.0f;
    color_map_["green"].g = 0.5f;
    color_map_["green"].b = 0.0f;
    color_map_["green"].a = 0.3f;
    color_map_["yellow"].r = 1.0f;
    color_map_["yellow"].g = 1.0f;
    color_map_["yellow"].b = 0.0f;
    color_map_["yellow"].a = 0.3f;
    color_map_["blue"].r = 0.0f;
    color_map_["blue"].g = 0.0f;
    color_map_["blue"].b = 1.0f;
    color_map_["blue"].a = 0.3f;
    color_map_["orange"].r = 1.0f;
    color_map_["orange"].g = 0.6f;
    color_map_["orange"].b = 0.0f;
    color_map_["orange"].a = 0.3f;
    color_map_["purple"].r = 0.5f;
    color_map_["purple"].g = 0.0f;
    color_map_["purple"].b = 0.5f;
    color_map_["purple"].a = 0.3f;
    color_map_["red"].r = 1.0f;
    color_map_["red"].g = 0.0f;
    color_map_["red"].b = 0.0f;
    color_map_["red"].a = 0.3f;
}

void CameraDirectionDeterminator::timer_callback(const ros::TimerEvent &event) {
    visualization_msgs::MarkerArray markers;
    for (size_t i = 0; i < colors_.size(); i++) {
        ros::Time now = ros::Time::now();
        kalman_filters_[colors_[i]].estimate_update((now - start_time_).toSec());
        std::string roomba = "roomba" + std::to_string(i + 1);
        visualization_msgs::Marker marker;
        marker.header.frame_id = "map";
        marker.header.stamp = now;
        marker.ns = roomba + "/kf";
        marker.id = i;

        marker.type = visualization_msgs::Marker::CYLINDER;
        marker.lifetime = ros::Duration();
        if (kalman_filters_[colors_[i]].get_likelihood() < LIFETIME_THRESHOLD) {
            marker.action = visualization_msgs::Marker::DELETE;
            markers.markers.push_back(marker);
            continue;
        }
        marker.action = visualization_msgs::Marker::ADD;

        std::vector<double> ellipse = kalman_filters_[colors_[i]].get_ellipse();
        marker.scale.x = ellipse[0];
        marker.scale.y = ellipse[1];
        marker.scale.z = 0.2;
        marker.pose.position.x = kalman_filters_[colors_[i]].get_x();
        marker.pose.position.y = kalman_filters_[colors_[i]].get_y();
        marker.pose.position.z = 0.2;
        double theta = std::acos(ellipse[1] / ellipse[0]);
        marker.pose.orientation.w = std::cos(theta / 2);
        marker.pose.orientation.x = std::cos(ellipse[2]) * std::sin(theta / 2);
        marker.pose.orientation.y = std::sin(ellipse[2]) * std::sin(theta / 2);
        marker.pose.orientation.z = 0.0;
        marker.color = color_map_[colors_[i]];

        markers.markers.push_back(marker);
    }
    ellipse_pub_.publish(markers);
}

void CameraDirectionDeterminator::process() {
    timer_ = nh_.createTimer(ros::Duration(1.0 / HZ), &CameraDirectionDeterminator::timer_callback, this);
    ros::spin();
}

int main(int argc, char **argv) {
    ros::init(argc, argv, "camera_direction_deteminator");
    CameraDirectionDeterminator cdd;
    cdd.process();
    return 0;
}
