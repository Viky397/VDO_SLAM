#include "visualizer/OpenCvVisualizer3D.h"
#include "utils/UtilsOpenCv.h"
#include "utils/UtilsGTSAM.h"
#include "Converter.h"

#include <memory>
#include <gtsam/geometry/Pose3.h>
#include <Eigen/Core>

namespace VDO_SLAM {

OpenCvVisualizer3D::OpenCvVisualizer3D(Map* map_)
    : map(CHECK_NOTNULL(map_)) {

        window.setWindowSize(cv::Size(800, 600));
        window.setBackgroundColor(cv::viz::Color::white());

        drawWorldCoordinateSystem();
        setupModelViewMatrix();

    }

void OpenCvVisualizer3D::process() {
    WidgetsMap widgets;
    drawCurrentCameraPose(&widgets);

    addToTrajectory();
    drawTrajectory(&widgets);

    // followCurrentView();
    drawStaticPointCloud(&widgets);
    drawDynamicPointClouds(&widgets);

    for (auto it = widgets.begin(); it != widgets.end(); ++it) {
        CHECK(it->second);
        // / This is to go around opencv issue #10829, new opencv should have this
        // fixed. cv::viz::Widget3D::getPose segfaults.
        it->second->updatePose(cv::Affine3d());
        window.showWidget(
            it->first, *(it->second), it->second->getPose());
    }

    window.spinOnce(1, true);
    removeWidgets();
}

void OpenCvVisualizer3D::drawWorldCoordinateSystem() {
    //add coordinate system widget
    //can confirm axis colours at XYZ-> RGB
    std::unique_ptr<cv::viz::WCoordinateSystem> coor_system_widget_ptr 
        = VDO_SLAM::make_unique<cv::viz::WCoordinateSystem>();
    gtsam::Pose3 origin;
    coor_system_widget_ptr->setPose(utils::gtsamPose3ToCvAffine3d(origin));

    window.showWidget("Origin", *coor_system_widget_ptr, coor_system_widget_ptr->getPose());
}

void OpenCvVisualizer3D::setupModelViewMatrix() {
    //attempting to replicate the follow mode of ORB-SLAM3 viewwer.cc
    model_view_matrix =
        ModelViewLookAt(viewpointX, viewpointY, viewpointZ, 0,0,0,0.0,-1.0, 0.0);
}

void OpenCvVisualizer3D::followCurrentView() {
    cv::Mat view = model_view_matrix * getLatestPose();
    cv::Affine3d affine_view = utils::matPoseToCvAffine3d(view);
    window.setViewerPose(affine_view);

}

void OpenCvVisualizer3D::drawCurrentCameraPose(WidgetsMap* widgets_map) {
    //todo add posr to trajectory?
    const int N = map->vpFeatSta.size() - 1;
    cv::Mat camera_pose_mat = map->vmCameraPose[N];
    //put into camera pose with inverted already
    cv::Affine3d camera_pose  = utils::matPoseToCvAffine3d(camera_pose_mat);

    CHECK_NOTNULL(widgets_map);
    std::unique_ptr<cv::viz::WCameraPosition> cam_widget_ptr = nullptr;
    cam_widget_ptr = VDO_SLAM::make_unique<cv::viz::WCameraPosition>(
        K_, 1.0, cv::viz::Color::green());
        CHECK(cam_widget_ptr);
    cam_widget_ptr->setPose(camera_pose);
    (*widgets_map)["Camera_pose"] = std::move(cam_widget_ptr);

    markWidgetForRemoval("Camera_pose");
}

void OpenCvVisualizer3D::addToTrajectory() {
    //assume we call every step so we just take the last one
    const int N = map->vpFeatSta.size() - 1;
    cv::Mat camera_pose_mat = map->vmCameraPose[N];
    cv::Affine3d camera_pose  = utils::matPoseToCvAffine3d(camera_pose_mat);
    trajectory.push_back(camera_pose);
    //hardcoded size for now
    while (trajectory.size() > 20) {
        trajectory.pop_front();
    }
}


void OpenCvVisualizer3D::drawTrajectory(WidgetsMap* widgets_map) {
    CHECK_NOTNULL(widgets_map);

    if(trajectory.size()  == 0) {
        LOG(WARNING) << "Trajectory is empty";
        return;
    }


    // // Create a Trajectory widget. (argument can be PATH, FRAMES, BOTH).
    std::vector<cv::Affine3f> trajectory_viz;
    trajectory_viz.reserve(trajectory.size());
    for (const auto& pose : trajectory) {
        trajectory_viz.push_back(pose);
    }
    (*widgets_map)["Trajectory"] = VDO_SLAM::make_unique<cv::viz::WTrajectory>(
            trajectory_viz, cv::viz::WTrajectory::PATH, 1.0, cv::viz::Color::red());
}

void OpenCvVisualizer3D::markWidgetForRemoval(const std::string& widget_id) {
    //assume unique
    widgets_to_remove.push_back(widget_id);
}


 void OpenCvVisualizer3D::drawStaticPointCloud(WidgetsMap* widgets_map) {
    const int N = map->vpFeatSta.size();
    const std::vector<std::vector<std::pair<int, int>>>& StaTracks = map->TrackletSta;
    std::vector<gtsam::Point3> points;
    if(N < 2) {
        return;
    }
    //copied from process in optimizer
    // mark each feature if it is satisfied (valid) for usage
    // here we use track length as threshold, for static >=3, dynamic >=3.
    // label each feature of the position in TrackLets: -1(invalid) or >=0(TrackID);
    // size: static: (N)xM_1, M_1 is the size of features in each frame
    std::vector<std::vector<int> > vnFeaLabSta(N);
    for (int i = 0; i < N; ++i) {
        std::vector<int>  vnFLS_tmp(map->vpFeatSta[i].size(),-1);
        vnFeaLabSta[i] = vnFLS_tmp;
    }

    // label static feature
    for (int i = 0; i < StaTracks.size(); ++i) {
        // filter the tracklets via threshold
        if (StaTracks[i].size() < 3) { // 3 the length of track on background.
            continue;
        }
        // label them
        for (int j = 0; j < StaTracks[i].size(); ++j) {
            //first -> frameId, second -> feature Id
            if(StaTracks[i][j].first >= vnFeaLabSta.size()) {
                // LOG(WARNING) << "Static feature tracks longer than frames";
                continue;
            }

            if(StaTracks[i][j].second >= vnFeaLabSta[StaTracks[i][j].first].size()) {
                // LOG(WARNING) << "Static feature tracks greater than feature Id for frame " << StaTracks[i][j].first;
                continue;
            }  


            vnFeaLabSta[StaTracks[i][j].first][StaTracks[i][j].second] = i;
        }
    }

    //loop over everything many times?
    for(int i = 0; i < N; i++) {
        // loop for static features
        for (int j = 0; j < vnFeaLabSta[i].size(); j++) {

              // check feature validation
            if (vnFeaLabSta[i][j]==-1) {
                continue;
            }
            gtsam::Point3 X_w = utils::cvMatToGtsamPoint3(map->vp3DPointSta[i][j]);
            points.push_back(X_w);
        }
    }

    if(points.size() == 0) {
        LOG(WARNING) << "Point Cloud empty. Not vizualising";
        return;
    }

    LOG(INFO) << "Making point cloud of size " << points.size();

    // Populate cloud structure with 3D points.
    cv::Mat point_cloud(1, points.size(), CV_32FC3);
    // cv::Mat point_cloud_color(
    //     1, point_cloud_color.size(), CV_8UC3, cloud_color_);

    cv::Point3f* data = point_cloud.ptr<cv::Point3f>();
    size_t i = 0;
    for(const gtsam::Point3& point : points) {
        data[i].x = static_cast<float>(point.x());
        data[i].y = static_cast<float>(point.y());
        data[i].z = static_cast<float>(point.z());
        i++;
    }

    // Create a cloud widget.
    std::unique_ptr<cv::viz::WCloud> cloud_widget =
                VDO_SLAM::make_unique<cv::viz::WCloud>(point_cloud, cv::viz::Color::black());
    cloud_widget->setRenderingProperty(cv::viz::POINT_SIZE, 2);

    (*widgets_map)["Point cloud"] = std::move(cloud_widget);

    markWidgetForRemoval("Point cloud");


}

 void OpenCvVisualizer3D::drawDynamicPointClouds(WidgetsMap* widgets_map) {
    const int N = map->vpFeatSta.size();
    const std::vector<std::vector<std::pair<int, int>>>& DynTracks = map->TrackletDyn;
    std::vector<gtsam::Point3> points;
    std::vector<cv::Scalar> colours;
    if(N < 2) {
        return;
    }
    //copied from process in optimizer
    // mark each feature if it is satisfied (valid) for usage
    // here we use track length as threshold, for static >=3, dynamic >=3.
    // label each feature of the position in TrackLets: -1(invalid) or >=0(TrackID);
    // size: static: (N)xM_1, M_1 is the size of features in each frame
    // std::vector<std::vector<int> > vnFeaLabDyn(N);
    // for (int i = 0; i < N; ++i) {
    //     std::vector<int>  vnFLS_tmp(map->vpFeatDyn[i].size(),-1);
    //     vnFeaLabDyn[i] = vnFLS_tmp;
    // }

    //  // label dynamic feature
    // for (int i = 0; i < DynTracks.size(); ++i) {
    //     // filter the tracklets via threshold
    //     if (DynTracks[i].size() < 3) { // 3 the length of track on objects.
    //         continue;
    //     }
    //     // label them
    //     for (int j = 0; j < DynTracks[i].size(); ++j){
    //         //first -> frameId, second -> feature Id
    //         if(DynTracks[i][j].first >= vnFeaLabDyn.size()) {
    //             // LOG(WARNING) << "Static feature tracks longer than frames";
    //             continue;
    //         }

    //         if(DynTracks[i][j].second >= vnFeaLabDyn[DynTracks[i][j].first].size()) {
    //             // LOG(WARNING) << "Static feature tracks greater than feature Id for frame " << StaTracks[i][j].first;
    //             continue;
    //         }  
    //         vnFeaLabDyn[DynTracks[i][j].first][DynTracks[i][j].second] = i;
    //     }
    // }

    //do previous frame if not tracked yet?
    for(int i = N -2; i < N - 1; i++) {
        for(int j = 0; j < map->vnFeatLabel[i].size(); j++) {
            int dynamic_label = map->vnFeatLabel[i][j];
            if(dynamic_label == -1 || dynamic_label == 0) {
                //log warning?
                continue;
            }

            gtsam::Point3 X_w = utils::cvMatToGtsamPoint3(map->vp3DPointDyn[i][j]);
            // cv::Scalar colour = getObjectColour(dynamic_label);
        }
    }

    // //actually just want the last one... for now
    //  //loop over everything many times?
    // for(int i = N-1; i < N; i++) {
    //     // loop for static features

    //     for (int j = 0; j < vnFeaLabDyn[i].size(); j++) {
    //         int dynamic_label = vnFeaLabDyn[i][j];
    //         //is outlier
    //         if(dynamic_label == -1) {
    //             continue;
    //         }
    //         if(dynamic_label == 0) {
    //             LOG(WARNING) << "Dynamic label shoudlnt be -1 (outlier) or static 0";
    //             continue;
    //         }
    //         gtsam::Point3 X_w = utils::cvMatToGtsamPoint3(map->vp3DPointDyn[i][j]);
    //         LOG(INFO) << "dynamic label " << map->vnFeatLabel[i][j];
    //         cv::Scalar colour = getObjectColour(dynamic_label);
    //         // points.push_back(X_w);
    //         // colours.push_back(colour);
    //     }
    // }

     if(points.size() == 0) {
        LOG(WARNING) << "Dynamic Point Cloud empty. Not vizualising";
        return;
    }

    LOG(INFO) << "Making dynamic point cloud of size " << points.size();
    CHECK_EQ(points.size(), colours.size());

    // Populate cloud structure with 3D points.
    cv::Mat point_cloud(1, points.size(), CV_32FC3);
    //default dynamic object?
    cv::Mat point_cloud_color(
        1, colours.size(), CV_8UC3);

    cv::Point3f* data = point_cloud.ptr<cv::Point3f>();
    size_t i = 0;
    for(const gtsam::Point3& point : points) {
        data[i].x = static_cast<float>(point.x());
        data[i].y = static_cast<float>(point.y());
        data[i].z = static_cast<float>(point.z());

        point_cloud_color.col(i) = colours.at(i);

        i++;
    }

    // Create a cloud widget.
    std::unique_ptr<cv::viz::WCloud> cloud_widget =
                VDO_SLAM::make_unique<cv::viz::WCloud>(point_cloud, point_cloud_color);
    cloud_widget->setRenderingProperty(cv::viz::POINT_SIZE, 2);

    (*widgets_map)["Dynamic Point cloud"] = std::move(cloud_widget);

    markWidgetForRemoval("Dynamic Point cloud");

 }

void OpenCvVisualizer3D::removeWidgets() {
    for(const std::string& widget_id : widgets_to_remove) {
        window.removeWidget(widget_id);
    }

    widgets_to_remove.clear();
}

cv::Mat OpenCvVisualizer3D::getLatestPose() {
    const int N = map->vpFeatSta.size() - 1;
    cv::Mat camera_pose_mat = map->vmCameraPose[N];
    return Converter::toInvMatrix(camera_pose_mat);
}

//taken from pangolin opengl render state
cv::Mat OpenCvVisualizer3D::ModelViewLookAt(
                                            double ex, 
                                            double ey, 
                                            double ez, 
                                            double lx, 
                                            double ly, 
                                            double lz, 
                                            double ux, 
                                            double uy, 
                                            double uz) {
    Eigen::Matrix4d mat;
    double* m = mat.data();
    const Eigen::Vector3d u_o(ux,uy,uz);

    Eigen::Vector3d z(ex - lx, ey - ly, ez - lz);
    z.normalize();

    Eigen::Vector3d x = u_o.cross(z);
    Eigen::Vector3d y = z.cross(x);

    const double len_x = x.norm();
    const double len_y = y.norm();

    if( len_x > 0 && len_y > 0) {
        for(size_t r = 0; r < 3; ++r ) {
            x[r] /= len_x;
            y[r] /= len_y;
        }

    #define M(row,col)  m[col*4+row]
        M(0,0) = x[0];
        M(0,1) = x[1];
        M(0,2) = x[2];
        M(1,0) = y[0];
        M(1,1) = y[1];
        M(1,2) = y[2];
        M(2,0) = z[0];
        M(2,1) = z[1];
        M(2,2) = z[2];
        M(3,0) = 0.0;
        M(3,1) = 0.0;
        M(3,2) = 0.0;
        M(0,3) = -(M(0,0)*ex + M(0,1)*ey + M(0,2)*ez);
        M(1,3) = -(M(1,0)*ex + M(1,1)*ey + M(1,2)*ez);
        M(2,3) = -(M(2,0)*ex + M(2,1)*ey + M(2,2)*ez);
        M(3,3) = 1.0;
    #undef M        
        return Converter::toCvMat(mat);
    }
    else {
        throw std::invalid_argument("'Look' and 'up' vectors cannot be parallel when calling ModelViewLookAt.");
    }

}


} //VDO_SLAM