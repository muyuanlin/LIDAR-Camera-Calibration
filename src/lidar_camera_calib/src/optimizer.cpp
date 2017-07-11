/**
 * This file is part of vo.
 *
 * Muyuan Lin, 2016
 * For more information see <https://github.com/muyuanlin/vo>
 */

#include "lidar_camera_calib/optimizer.h"

double max(double array[], int size){
    double res = array[0];
    for (size_t i=1; i<size; i++){
        if (array[i] > res) res = array[i];
    }
    return res;
}

double min(double array[], int size){
    double res = array[0];
    for (size_t i=1; i<size; i++){
        if (array[i] < res) res = array[i];
    }
    return res;
}


/*
 * optimize transform between LIDAR and camera
 * input: 
        object_points:
        lidar_scan:
        object_poses: a list of object poses of each frame
        parameter: 6 elements array which stores the transform between LIDAR and camera
        
 */
void optimizer::bundleAdjustment(const std::vector<std::vector<cv::Point3f> > lidar_scan,
                                 const std::vector<Eigen::Matrix4d> object_poses,
                                 const cv::Size patternsize,
                                 const double square_size,
                                 const double cube_depth,
                                 double* parameter,
                                 cv::Mat init_rvec,
                                 cv::Mat init_tvec
                                 )
{   /* 
     * debug
    // std::cout << "Lidar size" << lidar_scan.size() << " "  << lidar_scan[0].size() << std::endl;
    // std::cout << "Object pose size" << object_poses.size() << std::endl;
    // std::cout << patternsize.width << " " << patternsize.height << std::endl;
    // std::cout << square_size << std::endl;
    // for (size_t i=0; i<6; i++)
    //     std::cout << parameter[i] << " ";
    // std::cout << std::endl;
    */
    // initial guess of transform
    Eigen::Quaternionf r_lidar_frame( init_rvec.at<double>(3,0),
                                              init_rvec.at<double>(0,0),
                                              init_rvec.at<double>(1,0),
                                              init_rvec.at<double>(2,0));
    Eigen::Vector3f trans_vec_E(init_tvec.at<double>(0,0),
                                init_tvec.at<double>(1,0),
                                init_tvec.at<double>(2,0));
    Eigen::Translation<float,3> t_lidar_frame(trans_vec_E);

    // set inital estimation, checkerboad dimension
    initial_transform_guess = new double[6];
    initial_transform_guess[0] = parameter[0];
    initial_transform_guess[1] = parameter[1];
    initial_transform_guess[2] = parameter[2];
    initial_transform_guess[3] = parameter[3];
    initial_transform_guess[4] = parameter[4];
    initial_transform_guess[5] = parameter[5];

    // cull out points of lidar scan that are within checkerboard
    std::vector<std::vector<Eigen::Vector3f> > onplane_scan_all;

    double width = square_size * (patternsize.width + 2); // dimension of the checkerboard
    double height = square_size * (patternsize.height + 2);
    int n_frame = lidar_scan.size();
    for (size_t i=0; i < n_frame; i++){
        Eigen::Vector3f p1(0, 0, cube_depth), p2(width, 0, cube_depth), p3(width, height, cube_depth), p4(0, height, cube_depth);
        Eigen::Vector3f p5(0, 0, -cube_depth), p6(width, 0, -cube_depth), p7(width, height, -cube_depth), p8(0, height, -cube_depth);

        Eigen::Matrix4d pose = object_poses[i];
        Eigen::Matrix3d R = pose.block<3,3>(0,0);
        Eigen::Quaternionf r_pose(R.cast<float>());
        Eigen::Vector3f t_vec_pose(pose(0,3), pose(1,3), pose(2,3));
        Eigen::Translation<float, 3> t_pose(t_vec_pose);

        Eigen::Transform<float,3, Eigen::Affine> combined = t_lidar_frame * r_lidar_frame * t_pose * r_pose;
        
        p1 = combined * p1;
        p2 = combined * p2;
        p3 = combined * p3;
        p4 = combined * p4;
        p5 = combined * p5;
        p6 = combined * p6;
        p7 = combined * p7;
        p8 = combined * p8;
        
        double tmp1[] = {p1(0), p2(0), p3(0), p4(0), p5(0), p6(0), p7(0), p8(0)};
        double max_x = max(tmp1, 8);
        double tmp2[] = {p1(1), p2(1), p3(1), p4(1), p5(1), p6(1), p7(1), p8(1)};
        double max_y = max(tmp2, 8);
        double tmp3[] = {p1(2), p2(2), p3(2), p4(2), p5(2), p6(2), p7(2), p8(2)};
        double max_z = max(tmp3, 8);

        double tmp4[] = {p1(0), p2(0), p3(0), p4(0), p5(0), p6(0), p7(0), p8(0)};
        double min_x = min(tmp4, 8);
        double tmp5[] = {p1(1), p2(1), p3(1), p4(1), p5(1), p6(1), p7(1), p8(1)};
        double min_y = min(tmp5, 8);
        double tmp6[] = {p1(2), p2(2), p3(2), p4(2), p5(2), p6(2), p7(2), p8(2)};
        double min_z = min(tmp6, 8);
        
        std::vector<Eigen::Vector3f> onplane_scan;
        size_t n_scan = lidar_scan[i].size();
        for (size_t j=0; j < n_scan; j++){
            
            double p[3];
            p[0] = lidar_scan[i][j].x;
            p[1] = lidar_scan[i][j].y;
            p[2] = lidar_scan[i][j].z;
            if (p[0] > min_x && p[0] < max_x 
                && p[1] > min_y && p[1] < max_y 
                && p[2] > min_z && p[2] < max_z ){
                Eigen::Vector3f pt(p[0], p[1], p[2]); // in LIDAR frame
                onplane_scan.push_back(pt);
            }
        }

        onplane_scan_all.push_back(onplane_scan);
    }

    // Create residuals for each observation in the bundle adjustment problem. The
    // parameters for cameras and points are added automatically.
    ceres::Problem problem; 
    for (size_t i = 0; i < n_frame; ++i) {
        size_t n_scan = onplane_scan_all[i].size();
        for (size_t j=0; j < n_scan; j++){
        // Each Residual block takes a point and a camera as input and outputs a 1
        // dimensional residual. 
            ceres::CostFunction* cs = SnavelyReprojectionError::Create(onplane_scan_all[i][j], object_poses[i]);
            problem.AddResidualBlock(cs, NULL /* squared loss */, &parameter[0]);
        }
    }

    // Make Ceres automatically detect the bundle structure. Note that the
    // standard solver, SPARSE_NORMAL_CHOLESKY, also works fine but it is slower
    // for standard bundle adjustment problems. 
    ceres::Solver::Options options;
    options.linear_solver_type = ceres::DENSE_SCHUR;
    // If solve small to medium sized problems, consider setting
    // use_explicit_schur_complement as true
    // options.use_explicit_schur_complement = true;
    options.minimizer_progress_to_stdout = true;
    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);
    std::cout << summary.FullReport() << "\n";
}

    
