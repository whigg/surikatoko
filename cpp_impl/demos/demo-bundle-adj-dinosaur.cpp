#include <iostream>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <utility>
#include <cassert>
#include <tuple>
//#include <filesystem>
//#include <experimental/filesystem>
#include <boost/filesystem.hpp>
#include <Eigen/Dense>
#include "suriko/bundle-adj-kanatani.hpp"
#include "suriko/obs-geom.hpp"
#include "suriko/mat-serialization.hpp"
namespace suriko_demos
{
using namespace std;
//using namespace std::experimental::filesystem;
using namespace boost::filesystem;
using Eigen::MatrixXd;
using namespace suriko;

template <typename Scalar>
void PopulateCornersPerFrame(const vector<Scalar>& viff_data_by_row, size_t viff_num_rows, size_t viff_num_cols,
                             CornerTrackRepository<Scalar> *track_rep)
{
    size_t points_count = viff_num_rows;
    size_t frames_count = viff_num_cols/2;
    size_t next_track_id = 0;
    for (size_t pnt_ind = 0; pnt_ind < points_count; ++pnt_ind)
    {
        CornerTrack<Scalar> track;

        for (size_t frame_ind = 0; frame_ind < frames_count; ++frame_ind)
        {
            size_t i = pnt_ind*viff_num_cols+frame_ind*2;
            auto x = viff_data_by_row[i];
            auto y = viff_data_by_row[i+1];
            if (x == -1 || y == -1) continue;

            track.AddCorner(frame_ind, suriko::Point2<Scalar>(x,y));
        }
        if (!track.HasCorners()) continue; // track without registered corners

        track.SyntheticSalientPointId = 10000 + pnt_ind;
        track.TrackId = next_track_id++;
        track_rep->CornerTracks.push_back(track);
    }
}

int DinoDemo(int argc, char* argv[])
{
    const char* test_data = "";
    if (argc >= 2)
        test_data = argv[1];

    cout <<"test_data=" <<test_data <<endl;

    int debug = 3;

    typedef double Scalar;

    MatrixXd m(2,2);
    m(0,0) = 3;
    m(1,0) = 2.5;
    m(0,1) = -1;
    m(1,1) = m(1,0) + m(0,1);
    std::cout << m << std::endl;

    Eigen::Matrix<Scalar, 2, 2> m2;
    m2(0,0) = 3;
    m2(1,0) = 2.5;
    m2(0,1) = -1;
    m2(1,1) = m2(1,0) + m2(0,1);
    std::cout << m2 << std::endl;

    auto proj_mats_file_path = (current_path() / test_data /
            "oxfvisgeom/dinosaur/dinoPs_as_mat108x4.txt").normalize();

    vector<Scalar> P_data_by_row;
    size_t P_num_rows, P_num_cols;
    string err_msg;
    bool op= ReadMatrixFromFile(proj_mats_file_path, '\t', &P_data_by_row, &P_num_rows, &P_num_cols, &err_msg);
    if (!op)
    {
        std::cerr << err_msg;
        return 1;
    }

    size_t frames_count = P_num_rows / 3;
    cout <<"frames_count=" <<frames_count <<endl;

    vector<Eigen::Matrix<Scalar,3,4>> proj_mat_per_frame;
    vector<SE3Transform<Scalar>> inverse_orient_cam_per_frame;
    vector<Eigen::Matrix<Scalar, 3, 3>> intrinsic_cam_mat_per_frame; // K

    for (size_t frame_ind = 0; frame_ind < frames_count; ++frame_ind)
    {
        Eigen::Map<Eigen::Matrix<Scalar,3,4,Eigen::RowMajor>> proj_mat_row_major(P_data_by_row.data()+frame_ind*12, 3, 4);
        Eigen::Matrix<Scalar,3,4> proj_mat = proj_mat_row_major;
        proj_mat_per_frame.push_back(proj_mat);

        Scalar scale_factor;
        Eigen::Matrix<Scalar,3,3> K;
        SE3Transform<Scalar> direct_orient_cam;
        std::tie(scale_factor, K, direct_orient_cam) = DecomposeProjMat(proj_mat);

        intrinsic_cam_mat_per_frame.push_back(K);

        auto inverse_orient_cam = SE3Inv(direct_orient_cam);
        inverse_orient_cam_per_frame.push_back(inverse_orient_cam);
    }

    auto viff_mats_file_path = (current_path() / test_data / "oxfvisgeom/dinosaur/viff.xy").normalize();
    vector<Scalar> viff_data_by_row;
    size_t viff_num_rows, viff_num_cols;
    op= ReadMatrixFromFile(viff_mats_file_path, ' ', &viff_data_by_row, &viff_num_rows, &viff_num_cols, &err_msg);
    if (!op)
    {
        cerr << err_msg;
        return 1;
    }

    if (frames_count != viff_num_cols / 2)
    {
        cerr << "Inconsistent frames_count";
        return 1;
    }

    CornerTrackRepository<Scalar> track_rep;
    PopulateCornersPerFrame(viff_data_by_row, viff_num_rows, viff_num_cols, &track_rep);

    vector<size_t> point_track_ids;
    track_rep.PopulatePointTrackIds(&point_track_ids);

    // triangulate 3D points
    vector<Point2<Scalar>> one_pnt_corner_per_frame;
    vector<Eigen::Matrix<Scalar,3,4>> one_pnt_proj_mat_per_frame;
    FragmentMap<Scalar> map;
    for (size_t pnt_track_id : point_track_ids)
    {
        const auto& corner_track = track_rep.GetByPointId(pnt_track_id);

        one_pnt_corner_per_frame.clear();
        one_pnt_proj_mat_per_frame.clear();
        for (size_t frame_ind = 0; frame_ind < frames_count; ++frame_ind)
        {
            optional<Point2<Scalar>> corner = corner_track.GetCorner(frame_ind);
            if (!corner.has_value())
                continue;
            one_pnt_corner_per_frame.push_back(corner.value());
            one_pnt_proj_mat_per_frame.push_back(proj_mat_per_frame[frame_ind]);
        }
        Scalar f0 = 1;
        Point3<Scalar> x3D = Triangulate3DPointByLeastSquares(one_pnt_corner_per_frame, one_pnt_proj_mat_per_frame, f0, debug);
        map.AddSalientPoint(pnt_track_id, x3D);
    }

    auto err_initial = BundleAdjustmentKanatani<Scalar>::ReprojError(map, inverse_orient_cam_per_frame, track_rep, nullptr, &intrinsic_cam_mat_per_frame);
    cout <<"err_initial=" <<err_initial <<endl;

    bool debug_reproj_err = false;
    if (debug_reproj_err)
    {
        for (size_t point_track_id : point_track_ids) {
            const auto &point_track = track_rep.GetByPointId(point_track_id);

            Point3<Scalar> x3D = map.GetSalientPoint(point_track_id);
            Eigen::Matrix<Scalar,4,1> x3D_homog(x3D[0], x3D[1], x3D[2], 1);

            for (size_t frame_ind = 0; frame_ind < frames_count; ++frame_ind) {
                const auto &corner = point_track.GetCorner(frame_ind);
                if (!corner.has_value())
                    continue;

                const auto& cor = corner.value();

                const auto &P = proj_mat_per_frame[frame_ind];
                auto x2D_homog = P * x3D_homog;
                auto pix_x = x2D_homog[0] / x2D_homog[2];
                auto pix_y = x2D_homog[1] / x2D_homog[2];
                auto err = (cor.Mat() - Eigen::Matrix<Scalar,2,1>(pix_x, pix_y)).norm();
                if (err > 1)
                    cout << "repr err=" <<err <<" for point_track_id=" << point_track_id <<endl;
            }
        }
    }

    bool check_derivatives = true;

    BundleAdjustmentKanatani<Scalar> ba;

    cout << "start bundle adjustment..." <<endl;
    op = ba.ComputeInplace(map, inverse_orient_cam_per_frame, track_rep, nullptr, &intrinsic_cam_mat_per_frame, check_derivatives);

    return 0;
}
}