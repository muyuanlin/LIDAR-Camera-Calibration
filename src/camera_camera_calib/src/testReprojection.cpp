#include <iostream>

#include <opencv2/opencv.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/highgui/highgui.hpp>

#include "ros/ros.h"
#include "ceres/rotation.h"

#include "camera_camera_calib/loadBag.h"
#include "camera_camera_calib/loadSettings.h"
#include "camera_camera_calib/objectPose.h"
#include "camera_camera_calib/optimizer.h"
#include "camera_camera_calib/omniModel.h"
#include "camera_camera_calib/aprilTagsDetector.h"
#include "camera_camera_calib/ocamCalibModel.h"

using namespace std;
using namespace cv;

bool SYNTHTIC = false;

std::pair<double, double> getXYZ(double squareDist, int id, int m_tagRows, int m_tagCols){
  double x = ( id % (m_tagCols+1) ) * squareDist;
  double y = ( id / (m_tagCols+1) ) * squareDist;
  
  return std::pair<double, double>(x, y);
}

cv::Point3f pointTransform(const cv::Point3f& p0, const Eigen::Matrix4d& transform){
    Eigen::Vector4d eigen_p0;
    eigen_p0 << p0.x, p0.y, p0.z, 1;
    Eigen::Vector4d eigen_p1 = transform * eigen_p0;
    return cv::Point3f(eigen_p1(0), eigen_p1(1), eigen_p1(2));
}

cv::Point2f targetPoint2ImagePixel(const OCamCalibModel& cam, 
                                   const cv::Point3f& p0, 
                                   const Eigen::Matrix4d& target_pose){
    cv::Point3f p1 = pointTransform(p0, target_pose);

    double Ps[3] = {p1.x, p1.y, p1.z};
    double Ms[2];
    cam.world2cam(Ms, Ps);

    return cv::Point2f(Ms[0], Ms[1]);
}

/*
 * Camera-camera extrinsic parameter calibration
 */
int main(int argc, char **argv)
{
    /* 
     * Load image, convert from ROS image format to OpenCV Mat
     */
    ros::init(argc, argv, "camera_camera_calib");    
    string bag_file("/home/audren/Documents/data/small_drone_v2/ufo_2017-08-01-19-58-02.bag");

    vector<Mat> im0_seq, im1_seq;
    string topic0 = string("/synthetic_gimbal/cam0") + "/image_raw";
    string topic1 = string("/synthetic_gimbal/cam1") + "/image_raw";
    size_t sample_num = 10;
    size_t max_im_num = 500;
    loadBag(bag_file, topic0, topic1, im0_seq, im1_seq, sample_num, max_im_num);

    if (im0_seq.size() != im1_seq.size() || im0_seq.size() < 10){
        cout << "Inconsistent image numbers or too few images!" << endl;
        return 0;
    }
    cout << "---------------------------------------------" << endl;
    cout << "Number of left images:   " << im0_seq.size() << endl;
    cout << "Number of right images:  " << im1_seq.size() << endl;
    cout << "---------------------------------------------" << endl;
    /*
     * Read setting files
     */
    Settings s;
    string inputSettingsFile("/home/audren/Documents/lidar-camera-calib/src/camera_camera_calib/settings/settings.xml");
    FileStorage fs(inputSettingsFile, FileStorage::READ); // Read the settings
    if (!fs.isOpened())
    {
        cout << "Could not open the configuration file: \"" << inputSettingsFile << "\"" << endl;
        return -1;
    }
    fs["Settings"] >> s;
    fs.release();                                         // close Settings file

    if (!s.goodInput)
    {
        cout << "Invalid input detected. Application stopping. " << endl;
        return -1;
    }

    double width = 1050;//526;
    double height = 1050; //526;
    int tagRows = 10, tagCols = 7;
    double tagSize = 0.088; // unit: m
    double tagSpacing = 0.25; // unit: %

    // // Camera intrinsics
    std::vector<double> cam0_ss;
    double temp[] = {-2.575876e+02, 0.000000e+00, 2.283578e-04, 8.908668e-06, -2.621133e-08, 3.037693e-11 };
    for (size_t i=0; i<6; i++)
        cam0_ss.push_back(temp[i]);

    double cam0_u0 = 532.425687;
    double cam0_v0 = 517.382409;
    double cam0_c = 1.000805;
    double cam0_d = 0.000125;
    double cam0_e = 2.5200e-04;


    std::vector<double> cam1_ss;
    double temp1[] = {-2.593081e+02, 0.000000e+00, 4.238530e-04, 7.434385e-06, -2.212919e-08, 2.642407e-11};
    for (size_t i=0; i<6; i++)
        cam1_ss.push_back(temp1[i]);

    double cam1_u0 = 512.560101;
    double cam1_v0 = 523.645938;
    double cam1_c = 1.001192;
    double cam1_d = 0.000227;
    double cam1_e = 0.000224;

    Mat cam0_proj = s.intrinsics0;
    Mat cam0_dist = s.distortion0;
    double cam0_xi =  s.xi0;

    Mat cam1_proj = s.intrinsics1;
    Mat cam1_dist = s.distortion1;
    double cam1_xi =  s.xi1;
   
    OmniModel cam0(cam0_proj, cam0_dist, cam0_xi, cam0_u0, cam0_v0, cam0_ss, cam0_c, cam0_d, cam0_e);
    OmniModel cam1(cam1_proj, cam1_dist, cam1_xi, cam1_u0, cam1_v0, cam1_ss, cam1_c, cam1_d, cam1_e);
   
    
    AprilTagsDetector apriltags0(cam0_u0, cam0_v0, 
                                 cam0_proj.at<double>(0,0), cam0_proj.at<double>(1,1),  
                                 width, height, 
                                 tagRows, tagCols,
                                 tagSize, tagSpacing,
                                 string("cam0_apriltags_detection"));

    AprilTagsDetector apriltags1(cam1_u0, cam1_v0, 
                                 cam1_proj.at<double>(0,0), cam1_proj.at<double>(1,1),  
                                 width, height, 
                                 tagRows, tagCols,
                                 tagSize, tagSpacing,
                                 string("cam1_apriltags_detection"));

    OCamCalibModel ocamcalib_cam0;
    char ocamfile0[] = "/home/audren/Documents/data/small_drone_v2/Dart_21905596_high_res/calib_results_dart_21905596_high_res.txt";
    bool bopen0 = ocamcalib_cam0.get_ocam_model(ocamfile0);

    OCamCalibModel ocamcalib_cam1;
    char ocamfile1[] = "/home/audren/Documents/data/small_drone_v2/Dart_21905597_high_res/calib_results_dart_21905597_high_res.txt";
    bool bopen1 = ocamcalib_cam1.get_ocam_model(ocamfile1);

    if (!SYNTHTIC){
    vector<cv::Mat> &im_seq = im1_seq;
    AprilTagsDetector &apriltags = apriltags1;
    OCamCalibModel &cam = ocamcalib_cam1;
    for (size_t i=0; i<im_seq.size(); i++){
        /*
         * Step 1: Find out camera extrinsic parameter using PnP
         */
        Mat im = im_seq[i];
        if (im.channels() == 3)
            cvtColor(im, im, COLOR_RGB2GRAY);

        Eigen::Matrix4d target_pose;
        vector<AprilTags::TagDetection> detections;
        vector<cv::Point3f> objPts;
        vector<cv::Point2f> imgPts;
        vector<std::pair<bool, int> >tagid_found;
        
        Mat reproj_im = im.clone();
        cv::cvtColor(im, reproj_im, cv::COLOR_GRAY2BGR);

        bool bfind = apriltags.getDetections(im, detections, objPts, imgPts, tagid_found);
        bool good_estimation = cam.findCamPose(imgPts, objPts, target_pose);
        cout << target_pose << endl;
        waitKey(10);
        cout << "objPts number: " << objPts.size() << endl;
        /*
         * test 1: cam2world and world2cam
         */
        int TESTNO = 2;
        if (TESTNO == 1){
        for (size_t j=0; j<objPts.size(); j++){
            double Ms[2] = {imgPts[j].x, imgPts[j].y};
            double Ps[3];
            cam.cam2world(Ps, Ms);
            cam.world2cam(Ms, Ps);
            
            cv::circle(reproj_im, cv::Point2f(imgPts[j].x, imgPts[j].y), 1, cv::Scalar(0,255,14,0), 1);
            cv::circle(reproj_im, cv::Point2f(Ms[0], Ms[1]), 5, cv::Scalar(255,0,0,0), 1);
        }
        }else{
        /*
         * test 2: pose estimation
         */
        for (size_t j=0; j<objPts.size(); j++){
            cv::Point2f pt = targetPoint2ImagePixel(cam, objPts[j], target_pose);
    
            cv::circle(reproj_im, cv::Point2f(imgPts[j].x, imgPts[j].y), 1, cv::Scalar(0,255,14,0), 1);
            cv::circle(reproj_im, cv::Point2f(pt.x, pt.y), 8, cv::Scalar(255,0,0,0), 2);
        }
        }
        imshow("Reprojection", reproj_im);
        waitKey(0);

    }
    }else{
    /*
     * test: using synthetic data 
     * test pass
     */
    Eigen::Matrix4d pose_ground_truth;
    // pose_ground_truth << 1, 0, 0, -3,
    //                      0, 0.866, 0.5, 0,
    //                      0, -0.5, 0.866, 0.5,
    //                      0, 0, 0, 1;
    pose_ground_truth << 1, 0, 0, -1,
                         0, 1, 0, 0,
                         0, 0, 1,   -0.1,
                         0, 0, 0, 1;


    double squareDist = tagSize + tagSize * tagSpacing;
    double halfSquare = tagSize / 2.0;
    vector<cv::Point3f> objPts;
    vector<std::pair<double, double> > vec_center;
    Mat img(width, height, CV_8UC3, Scalar(0,0,0));
    for (size_t i=0; i<80; i++){
        std::pair<double, double> center = getXYZ(squareDist, i, 10, 7);
        vec_center.push_back(center);

        double cx = center.first;
        double cy = center.second;

        /*
         * draw ID
         */
        cv::Point2f center_i = targetPoint2ImagePixel(ocamcalib_cam1, cv::Point3f(cx, cy, 0), 
                                                      pose_ground_truth);
        std::ostringstream strSt;
        strSt << "#" << i;
        cv::putText(img, strSt.str(),
                  center_i,
                  cv::FONT_HERSHEY_PLAIN, 1, cv::Scalar(0,0,255));


        /* pass
        * cout << "-------------------debug------------------" << endl;
        * cout << cx << " " << cy << endl;
        */
        objPts.push_back(cv::Point3f(cx - halfSquare, cy - halfSquare, 0));
        objPts.push_back(cv::Point3f(cx + halfSquare, cy - halfSquare, 0));
        objPts.push_back(cv::Point3f(cx + halfSquare, cy + halfSquare, 0));
        objPts.push_back(cv::Point3f(cx - halfSquare, cy + halfSquare, 0));
    }

    vector<cv::Point2f> imgPts;
    for (size_t i=0; i<objPts.size(); i++){
        cv::Point2f ipt = targetPoint2ImagePixel(ocamcalib_cam1, objPts[i], pose_ground_truth);
        imgPts.push_back(ipt);
        /*
         * orientation is correct
         */
        cv::circle(img, cv::Point2f(ipt.x, ipt.y), 1, cv::Scalar(255,255,255,0), 1);
    }

    imshow("Test", img);
    waitKey(0);

    // from synthetic image poinsts and object points recover pose of target
    /*
     * test pass
     */
    Eigen::Matrix4d pose_estimated;
    bool good_estimation1 = ocamcalib_cam1.findCamPose(imgPts, objPts, pose_estimated);
    cout << "---------------target pose in camera frame-----------------" << endl;
    cout << pose_estimated << endl;
    }
    
    return 0;
}
