/**
 * @file rgbd-slam-node.cpp
 * @brief Implementation of the RgbdSlamNode Wrapper class.
 * @author Suchetan R S (rssuchetan@gmail.com)
 */
#include "rgbd-slam-node.hpp"

#include <opencv2/core/core.hpp>

namespace ORB_SLAM3_Wrapper
{
    RgbdSlamNode::RgbdSlamNode(const std::string &strVocFile,
                               const std::string &strSettingsFile,
                               ORB_SLAM3::System::eSensor sensor)
        : Node("ORB_SLAM3_RGBD_ROS2")
    {
        // ROS Subscribers
        rgbSub_ = std::make_shared<message_filters::Subscriber<sensor_msgs::msg::Image>>(this, "camera/image_raw");
        depthSub_ = std::make_shared<message_filters::Subscriber<sensor_msgs::msg::Image>>(this, "camera/depth/image_raw");
        syncApproximate_ = std::make_shared<message_filters::Synchronizer<approximate_sync_policy>>(approximate_sync_policy(10), *rgbSub_, *depthSub_);
        syncApproximate_->registerCallback(&RgbdSlamNode::RGBDCallback, this);
        imuSub_ = this->create_subscription<sensor_msgs::msg::Imu>("imu", 1000, std::bind(&RgbdSlamNode::ImuCallback, this, std::placeholders::_1));
        odomSub_ = this->create_subscription<nav_msgs::msg::Odometry>("odom", 1000, std::bind(&RgbdSlamNode::OdomCallback, this, std::placeholders::_1));
        // ROS Publishers
        mapDataPub_ = this->create_publisher<slam_msgs::msg::MapData>("map_data", 10);
        mapPointsPub_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("map_points", 10);
#ifdef WITH_TRAVERSABILITY_MAP
        lidarSub_ = this->create_subscription<sensor_msgs::msg::PointCloud2>("velodyne_points", 1000, std::bind(&RgbdSlamNode::LidarCallback, this, std::placeholders::_1));
        gridmapPub_ = this->create_publisher<nav_msgs::msg::OccupancyGrid>("traversability_grid", 10);
        traversabilityPub_ = this->create_publisher<grid_map_msgs::msg::GridMap>("RTQuadtree_struct", rclcpp::QoS(1).transient_local());
#endif
        // Services
        getMapDataService_ = this->create_service<slam_msgs::srv::GetMap>("orb_slam3_get_map_data", std::bind(&RgbdSlamNode::getMapServer, this,
                                                                                                              std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        // Timers
        mapDataTimer_ = this->create_wall_timer(std::chrono::seconds(1), std::bind(&RgbdSlamNode::publishMapData, this));
#ifdef WITH_TRAVERSABILITY_MAP
        traversabilityTimer_ = this->create_wall_timer(std::chrono::milliseconds(800), std::bind(&RgbdSlamNode::publishTraversabilityData, this));
#endif
        // TF
        tfBroadcaster_ = std::make_shared<tf2_ros::TransformBroadcaster>(this);
        tfBuffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
        tfListener_ = std::make_shared<tf2_ros::TransformListener>(*tfBuffer_);

        bool bUseViewer;
        this->declare_parameter("visualization", rclcpp::ParameterValue(true));
        this->get_parameter("visualization", bUseViewer);

        this->declare_parameter("ros_visualization", rclcpp::ParameterValue(false));
        this->get_parameter("ros_visualization", rosViz_);

        this->declare_parameter("robot_base_frame", "base_link");
        this->get_parameter("robot_base_frame", robot_base_frame_id_);

        this->declare_parameter("global_frame", "map");
        this->get_parameter("global_frame", global_frame_);

        this->declare_parameter("odom_frame", "odom");
        this->get_parameter("odom_frame", odom_frame_id_);

        this->declare_parameter("robot_x", rclcpp::ParameterValue(1.0));
        this->get_parameter("robot_x", robot_x_);

        this->declare_parameter("robot_y", rclcpp::ParameterValue(1.0));
        this->get_parameter("robot_y", robot_y_);

        interface_ = std::make_shared<ORB_SLAM3_Wrapper::ORBSLAM3Interface>(strVocFile, strSettingsFile,
                                                                            sensor, bUseViewer, rosViz_, robot_x_,
                                                                            robot_y_, global_frame_, odom_frame_id_);
        RCLCPP_INFO(this->get_logger(), "CONSTRUCTOR END!");
    }

    RgbdSlamNode::~RgbdSlamNode()
    {
        rgbSub_.reset();
        depthSub_.reset();
        imuSub_.reset();
        odomSub_.reset();
        interface_.reset();
        RCLCPP_INFO(this->get_logger(), "DESTRUCTOR!");
    }

    void RgbdSlamNode::ImuCallback(const sensor_msgs::msg::Imu::SharedPtr msgIMU)
    {
        RCLCPP_DEBUG_STREAM(this->get_logger(), "ImuCallback");
        // push value to imu buffer.
        interface_->handleIMU(msgIMU);
    }

    void RgbdSlamNode::OdomCallback(const nav_msgs::msg::Odometry::SharedPtr msgOdom)
    {
        std::lock_guard<std::mutex> lock(latestTimeMutex_);
        latestTime_ = msgOdom->header.stamp;
        RCLCPP_DEBUG_STREAM(this->get_logger(), "OdomCallback");
        interface_->getMapToOdomTF(msgOdom, tfMapOdom_);
    }

    void RgbdSlamNode::RGBDCallback(const sensor_msgs::msg::Image::SharedPtr msgRGB, const sensor_msgs::msg::Image::SharedPtr msgD)
    {
        Sophus::SE3f Tcw;
        if (interface_->trackRGBDi(msgRGB, msgD, Tcw))
        {
            isTracked_ = true;
            tfBroadcaster_->sendTransform(tfMapOdom_);
            if (rosViz_)
            {
                publishMapPointCloud();
            }
        }
    }

#ifdef WITH_TRAVERSABILITY_MAP
    void RgbdSlamNode::LidarCallback(sensor_msgs::msg::PointCloud2 msgLidar)
    {
        // RCLCPP_INFO_STREAM(this->get_logger(), "PCLCallback");
        interface_->handleLidarPCL(msgLidar.header.stamp, msgLidar);
    }

    void RgbdSlamNode::publishTraversabilityData()
    {
        std::lock_guard<std::mutex> lock(latestTimeMutex_);
        auto map = interface_->getTraversabilityData();
        // publish the gridmap and occupancy map.
        map.first.info.origin.position.x = map.first.info.origin.position.x + robot_x_;
        map.first.info.origin.position.y = map.first.info.origin.position.y + robot_y_;
        map.first.header.frame_id = global_frame_;
        map.first.header.stamp = latestTime_;
        gridmapPub_->publish(map.first);
        traversabilityPub_->publish(map.second);
    }
#endif

    void RgbdSlamNode::publishMapPointCloud()
    {
        sensor_msgs::msg::PointCloud2 mapPCL;
        interface_->getCurrentMapPoints(mapPCL);
        // map_points_pub->publish(mapPCL);
    }

    void RgbdSlamNode::publishMapData()
    {
        if (isTracked_)
        {
            RCLCPP_INFO_STREAM(this->get_logger(), "Publishing map data");
            // publish the map data (current active keyframes etc)
            slam_msgs::msg::MapData mapDataMsg;
            interface_->mapDataToMsg(mapDataMsg, true, false);
            mapDataPub_->publish(mapDataMsg);
        }
    }

    void RgbdSlamNode::getMapServer(std::shared_ptr<rmw_request_id_t> request_header,
                                    std::shared_ptr<slam_msgs::srv::GetMap::Request> request,
                                    std::shared_ptr<slam_msgs::srv::GetMap::Response> response)
    {
        RCLCPP_INFO(this->get_logger(), "GetMap2 service called.");
        slam_msgs::msg::MapData mapDataMsg;
        interface_->mapDataToMsg(mapDataMsg, false, request->tracked_points, request->kf_id_for_landmarks);
        response->data = mapDataMsg;
    }
}
