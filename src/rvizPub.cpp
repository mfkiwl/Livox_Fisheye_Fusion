// basic
#include <string>
#include <sstream>
// ros 
#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <ros/package.h>
// pcl library
#include <pcl/point_cloud.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/io/pcd_io.h>

using namespace std;

string getDataPath(){
    std::string currPkgDir = ros::package::getPath("data_process");
    std::string dataPath = currPkgDir + "/data/runYangIn";
    return dataPath;
}
const string dataPath = getDataPath();

main (int argc, char **argv){
    ros::init (argc, argv, "rvizPub");
    ros::NodeHandle nh;

    ros::Publisher orgPub = nh.advertise<sensor_msgs::PointCloud2> ("rvizOrgTopic", 1);
    // ros::Publisher fltPub = nh.advertise<sensor_msgs::PointCloud2> ("rvizFltTopic", 1);
    pcl::PointCloud<pcl::PointXYZI> orgCloud;
    // pcl::PointCloud<pcl::PointXYZI> fltCloud;
    pcl::io::loadPCDFile (dataPath + "/outputs/ryOutLidDense.pcd", orgCloud);
    sensor_msgs::PointCloud2 orgMsg;
    // sensor_msgs::PointCloud2 fltMsg;
    pcl::toROSMsg(orgCloud, orgMsg);
    // pcl::toROSMsg(fltCloud, fltMsg);

    orgMsg.header.frame_id = "org"; //this has been done in order to be able to visualize our PointCloud2 message on the RViz visualizer
    // fltMsg.header.frame_id = "flt";
    
    ros::Rate loop_rate(1);
    while (ros::ok())
    {
        orgPub.publish(orgMsg);
        // fltPub.publish(fltMsg);
        ros::spinOnce();
        loop_rate.sleep();
    }
    return 0;
}