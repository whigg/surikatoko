#include <random>
#include <numeric> // accumulate
#include "suriko/davison-mono-slam.h"
#include <glog/logging.h>
#include "suriko/approx-alg.h"
#include "suriko/quat.h"
#include "suriko/eigen-helpers.hpp"
#include "suriko/templ-match.h"
#include "suriko/rand-stuff.h"
#include <opencv2/imgproc.hpp>

namespace suriko
{
template <typename EigenMat>
auto Span(EigenMat& m) -> gsl::span<Scalar>
{
    return gsl::make_span<Scalar>(m.data(), static_cast<gsl::span<Scalar>::index_type>(m.size()));
}

template <typename EigenMat>
auto Span(const EigenMat& m) -> gsl::span<const Scalar>
{
    return gsl::make_span<const Scalar>(m.data(), static_cast<gsl::span<const Scalar>::index_type>(m.size()));
}

template <typename EigenMat>
auto Span(EigenMat& m, size_t count) -> gsl::span<Scalar>
{
    return gsl::make_span<Scalar>(m.data(), static_cast<gsl::span<Scalar>::index_type>(count));
}

template <typename EigenMat>
auto Span(const EigenMat& m, size_t count) -> gsl::span<const Scalar>
{
    return gsl::make_span<const Scalar>(m.data(), static_cast<gsl::span<Scalar>::index_type>(count));
}

template <typename EigenMat>
auto SpanAu(EigenMat& m, size_t count) -> gsl::span<typename EigenMat::Scalar>
{
    typedef typename EigenMat::Scalar S;
    // fails: m.data()=const double* and it doesn't match gsl::span<double>
    return gsl::make_span<S>(m.data(), static_cast<typename gsl::span<S>::index_type>(count));
}

SE3Transform CamWfc(const CameraStateVars& cam_state)
{
    Eigen::Matrix<Scalar, kEucl3, kEucl3> Rwfc;
    RotMatFromQuat(Span(cam_state.orientation_wfc), &Rwfc);

    return SE3Transform{ Rwfc, cam_state.pos_w };
}

suriko::Point2i TemplateTopLeftInt(const suriko::Point2f& center, suriko::Sizei templ_size)
{
    int rad_x = templ_size.width / 2;
    int rad_y = templ_size.height / 2;
    return suriko::Point2i{
        static_cast<int>(center[0] - rad_x),
        static_cast<int>(center[1] - rad_y) };
}

DavisonMonoSlamInternalsLogger::DavisonMonoSlamInternalsLogger(DavisonMonoSlam* mono_slam)
    :mono_slam_(mono_slam)
{
}

void DavisonMonoSlamInternalsLogger::StartNewFrameStats()
{
    cur_stats_ = DavisonMonoSlamTrackerInternalsSlice{};

    // the last operation is to start a timer to reduce overhead of logger
    frame_start_time_point_ = std::chrono::high_resolution_clock::now();
}

/// Finds median of position uncertainty of all salient points
std::optional<Eigen::Matrix<Scalar, 3, 3>> GetRepresentiveSalientPointUncertainty(DavisonMonoSlam* mono_slam_)
{
    // make the matrix row-major because the number of columns is fixed
    Eigen::Matrix<Scalar, Eigen::Dynamic, kEucl3*kEucl3, Eigen::RowMajor> sal_pnt_uncert_hist;
    sal_pnt_uncert_hist.resize(mono_slam_->SalientPointsCount(), Eigen::NoChange);

    int points_count = 0;
    for (SalPntId sal_pnt_id : mono_slam_->GetSalientPoints())
    {
        Eigen::Matrix<Scalar, kEucl3, 1> pos;
        Eigen::Matrix<Scalar, kEucl3, kEucl3> pos_uncert;
        bool got_3d_pos = mono_slam_->GetSalientPointEstimated3DPosWithUncertaintyNew(sal_pnt_id, &pos, &pos_uncert);
        if (!got_3d_pos)
            continue;

        for (decltype(sal_pnt_uncert_hist.cols()) i = 0; i < sal_pnt_uncert_hist.cols(); ++i)
            sal_pnt_uncert_hist(points_count,i) = pos_uncert.data()[i];
        points_count++;
    }

    if (points_count == 0)
        return std::nullopt;

    sal_pnt_uncert_hist.conservativeResize(points_count, Eigen::NoChange);

    // sort each column separately
    for (decltype(sal_pnt_uncert_hist.cols()) i = 0; i < sal_pnt_uncert_hist.cols(); ++i)
    {
        auto all_covs = gsl::make_span<Scalar>(&sal_pnt_uncert_hist(0, i), sal_pnt_uncert_hist.rows());
        std::sort(all_covs.begin(), all_covs.end());
    }

    size_t median_ind = sal_pnt_uncert_hist.rows() / 2;

    Eigen::Matrix<Scalar, kEucl3, kEucl3, 1> sal_pnt_uncert{};
    Eigen::Map<Eigen::Matrix<Scalar, kEucl3*kEucl3, 1>> sal_pnt_uncert_stacked(sal_pnt_uncert.data());
    for (decltype(sal_pnt_uncert_hist.cols()) i = 0; i < sal_pnt_uncert_hist.cols(); ++i)
        sal_pnt_uncert_stacked[i] = sal_pnt_uncert_hist(median_ind, i);

    return sal_pnt_uncert;
}

void DavisonMonoSlamInternalsLogger::RecordFrameFinishTime()
{
    frame_finish_time_point_ = std::chrono::high_resolution_clock::now();
    cur_stats_.frame_processing_dur = frame_finish_time_point_ - frame_start_time_point_;
}

void DavisonMonoSlamInternalsLogger::PushCurFrameStats()
{
    hist_.state_samples.push_back(cur_stats_);
}

void DavisonMonoSlamInternalsLogger::NotifyNewComDelSalPnts(size_t new_count, size_t common_count, size_t deleted_count)
{
    cur_stats_.new_sal_pnts = new_count;
    cur_stats_.common_sal_pnts = common_count;
    cur_stats_.deleted_sal_pnts = deleted_count;
}

void DavisonMonoSlamInternalsLogger::NotifyEstimatedSalPnts(size_t estimated_sal_pnts_count)
{
    cur_stats_.estimated_sal_pnts = estimated_sal_pnts_count;
}

void CalcAggregateStatisticsInplace(DavisonMonoSlamTrackerInternalsHist* hist)
{
    size_t frames_count = hist->state_samples.size();

    using DurT = decltype(hist->avg_frame_processing_dur);
    DurT dur = std::accumulate(hist->state_samples.begin(), hist->state_samples.end(), DurT::zero(),
        [](const auto& acc, const auto& i) { return acc + i.frame_processing_dur; });
    hist->avg_frame_processing_dur = dur / frames_count;
}

DavisonMonoSlamTrackerInternalsHist& DavisonMonoSlamInternalsLogger::BuildStats()
{
    CalcAggregateStatisticsInplace(&hist_);
    return hist_;
}

// static
DavisonMonoSlam::DebugPathEnum DavisonMonoSlam::s_debug_path_ = DebugPathEnum::DebugNone;

static constexpr Scalar estim_var_init_std = 1;

DavisonMonoSlam::DavisonMonoSlam()
{
    SetProcessNoiseStd(process_noise_std_);

    ResetCamera(estim_var_init_std);
}

void DavisonMonoSlam::ResetCamera(Scalar estim_var_init_std, bool init_estim_vars)
{
    // state vector
    size_t n = kCamStateComps;
    if (init_estim_vars)
        estim_vars_.setZero(n, 1);
    gsl::span<Scalar> state_span = gsl::make_span(estim_vars_.data(), n);

    // camera position
    DependsOnCameraPosPackOrder();
    state_span[0] = 0;
    state_span[1] = 0;
    state_span[2] = 0;

    // camera orientation
    state_span[3] = 1;
    state_span[4] = 0;
    state_span[5] = 0;
    state_span[6] = 0;

    // camera velocity; at each iteration is increased by acceleration in the form of the gaussian noise
    state_span[7] = 0;
    state_span[8] = 0;
    state_span[9] = 0;

    // camera angular velocity; at each iteration is increased by acceleration in the form of the gaussian noise
    state_span[10] = 0;
    state_span[11] = 0;
    state_span[12] = 0;

    Scalar estim_var_init_variance = suriko::Sqr(estim_var_init_std);

    // state uncertainty matrix
    if (init_estim_vars)
        estim_vars_covar_.setZero(n, n);
    else
        estim_vars_covar_.topLeftCorner< kCamStateComps, kCamStateComps>().setZero();

    // camera position
    estim_vars_covar_(0, 0) = estim_var_init_variance;
    estim_vars_covar_(1, 1) = estim_var_init_variance;
    estim_vars_covar_(2, 2) = estim_var_init_variance;
    // camera orientation (quaternion)
    estim_vars_covar_(3, 3) = estim_var_init_variance;
    estim_vars_covar_(4, 4) = estim_var_init_variance;
    estim_vars_covar_(5, 5) = estim_var_init_variance;
    estim_vars_covar_(6, 6) = estim_var_init_variance;
    // camera speed
    estim_vars_covar_(7, 7) = estim_var_init_variance;
    estim_vars_covar_(8, 8) = estim_var_init_variance;
    estim_vars_covar_(9, 9) = estim_var_init_variance;
    // camera angular speed
    estim_vars_covar_(10, 10) = estim_var_init_variance;
    estim_vars_covar_(11, 11) = estim_var_init_variance;
    estim_vars_covar_(12, 12) = estim_var_init_variance;
}

void DavisonMonoSlam::SetCamera(const SE3Transform& cam_pos_cfw)
{
    auto cam_pos_wfc = SE3Inv(cam_pos_cfw);

    // state vector
    size_t n = kCamStateComps;
    estim_vars_.setZero(n, 1);
    gsl::span<Scalar> state_span = gsl::make_span(estim_vars_.data(), n);

    // camera position
    DependsOnCameraPosPackOrder();
    state_span[0] = cam_pos_wfc.T[0];
    state_span[1] = cam_pos_wfc.T[1];
    state_span[2] = cam_pos_wfc.T[2];

    // camera orientation
    gsl::span<Scalar> cam_pos_wfc_quat = gsl::make_span(estim_vars_.data() + kEucl3, kQuat4);
    bool op = QuatFromRotationMat(cam_pos_wfc.R, cam_pos_wfc_quat);
    SRK_ASSERT(op);

    Scalar qlen = std::sqrt(
        suriko::Sqr(cam_pos_wfc_quat[0]) +
        suriko::Sqr(cam_pos_wfc_quat[1]) +
        suriko::Sqr(cam_pos_wfc_quat[2]) +
        suriko::Sqr(cam_pos_wfc_quat[3]));

    state_span[3] = cam_pos_wfc_quat[0];
    state_span[4] = cam_pos_wfc_quat[1];
    state_span[5] = cam_pos_wfc_quat[2];
    state_span[6] = cam_pos_wfc_quat[3];

    // camera velocity; at each iteration is increased by acceleration in the form of the gaussian noise
    state_span[7] = 0;
    state_span[8] = 0;
    state_span[9] = 0;

    // camera angular velocity; at each iteration is increased by acceleration in the form of the gaussian noise
    state_span[10] = 0;
    state_span[11] = 0;
    state_span[12] = 0;

    Scalar cam_orient_q_comp_var = suriko::Sqr(cam_orient_q_comp_std_);
    Scalar cam_vel_var = suriko::Sqr(cam_vel_std_);
    Scalar cam_ang_vel_var = suriko::Sqr(cam_ang_vel_std_);

    // state uncertainty matrix
    estim_vars_covar_.setZero(n, n);
    // camera position
    estim_vars_covar_(0, 0) = suriko::Sqr(cam_pos_x_std_m_);;
    estim_vars_covar_(1, 1) = suriko::Sqr(cam_pos_y_std_m_);;
    estim_vars_covar_(2, 2) = suriko::Sqr(cam_pos_z_std_m_);;
    // camera orientation (quaternion)
    estim_vars_covar_(3, 3) = cam_orient_q_comp_var;
    estim_vars_covar_(4, 4) = cam_orient_q_comp_var;
    estim_vars_covar_(5, 5) = cam_orient_q_comp_var;
    estim_vars_covar_(6, 6) = cam_orient_q_comp_var;
    // camera speed
    estim_vars_covar_(7, 7) = cam_vel_var;
    estim_vars_covar_(8, 8) = cam_vel_var;
    estim_vars_covar_(9, 9) = cam_vel_var;
    // camera angular speed
    estim_vars_covar_(10, 10) = cam_ang_vel_var;
    estim_vars_covar_(11, 11) = cam_ang_vel_var;
    estim_vars_covar_(12, 12) = cam_ang_vel_var;
}

void DavisonMonoSlam::SetProcessNoiseStd(Scalar process_noise_std)
{
    process_noise_std_ = process_noise_std;
    
    Scalar process_noise_std_variance = suriko::Sqr(process_noise_std);

    process_noise_covar_.setZero();
    for (int i=0; i < process_noise_covar_.rows(); ++i)
        process_noise_covar_(i, i) = process_noise_std_variance;
}

void AzimElevFromEuclidCoords(suriko::Point3 hw, Scalar* azim_theta, Scalar* elev_phi)
{
    *azim_theta = std::atan2(hw[0], hw[2]);
    *elev_phi = std::atan2(-hw[1], std::sqrt(suriko::Sqr(hw[0]) + suriko::Sqr(hw[2])));
}

bool DavisonMonoSlam::ConvertXyzFromSphericalSalientPoint(const SphericalSalientPoint& sal_pnt_vars, Eigen::Matrix<Scalar, kEucl3, 1>* pos_mean)
{
    Eigen::Matrix<Scalar, kEucl3, 1> m;
    CameraCoordinatesEuclidUnityDirFromPolarAngles(sal_pnt_vars.azimuth_theta_w, sal_pnt_vars.elevation_phi_w, &m[0], &m[1], &m[2]);

    if (IsClose(0, sal_pnt_vars.inverse_dist_rho))
        return false;

    // the expression below is derived from A.21 when camera position r=[0 0 0]
    // salient point in world coordinates = position of the first camera + position of the salient point in the first camera
    *pos_mean = sal_pnt_vars.first_cam_pos_w + (1 / sal_pnt_vars.inverse_dist_rho) * m;  // this fails for salient points in infinity, when dist=inf and rho=0
    return true;
}

// Converts 3D world salient point into inverse depth 6x1 salient point representation.
bool DavisonMonoSlam::ConvertSphericalFromXyzSalientPoint(const Eigen::Matrix <Scalar, kXyzSalientPointComps, 1>& sal_pnt_pos_w,
    const SE3Transform& first_cam_tfc,
    SphericalSalientPoint* spher_sal_pnt)
{
    // A.22 hc_xyz=Rcw(y3-first_cam_pos)
    Eigen::Matrix <Scalar, kXyzSalientPointComps, 1> xc = first_cam_tfc.R.transpose() * (sal_pnt_pos_w - first_cam_tfc.T);

    auto first_cam_cft = SE3Inv(first_cam_tfc);
    auto xc2 = SE3Apply(first_cam_cft, suriko::Point3{ sal_pnt_pos_w });

    Eigen::Matrix <Scalar, kXyzSalientPointComps, 1> x_cam_pos_world_orient = sal_pnt_pos_w - first_cam_tfc.T;
    Scalar dist = x_cam_pos_world_orient.norm();  // in the first camera
    if (IsClose(0, dist))
        return false;

    Scalar azim_theta_w;
    Scalar elev_phi_w;
    AzimElevFromEuclidCoords(suriko::Point3{ x_cam_pos_world_orient }, &azim_theta_w, &elev_phi_w);

    spher_sal_pnt->first_cam_pos_w = first_cam_tfc.T;
    spher_sal_pnt->azimuth_theta_w = azim_theta_w;
    spher_sal_pnt->elevation_phi_w = elev_phi_w;
    spher_sal_pnt->inverse_dist_rho = 1 / dist;
    return true;
}

void DavisonMonoSlam::ConvertMorphableFromSphericalSalientPoints(
    const std::vector<SphericalSalientPointWithBuildInfo>& sal_pnt_build_infos,
    SalPntComps sal_pnt_repres,
    std::vector<MorphableSalientPoint>* sal_pnts)
{
    for (const auto& sbi : sal_pnt_build_infos)
    {
        MorphableSalientPoint sp;
        if (sal_pnt_repres == SalPntComps::kXyz)
        {
            bool op = ConvertXyzFromSphericalSalientPoint(sbi.spher_sal_pnt, &sp.pos_w);
            SRK_ASSERT(op);
        }
        else if (sal_pnt_repres == SalPntComps::kSphericalFirstCamInvDist)
        {
            sp.first_cam_pos_w = sbi.spher_sal_pnt.first_cam_pos_w;
            sp.azimuth_theta_w = sbi.spher_sal_pnt.azimuth_theta_w;
            sp.elevation_phi_w = sbi.spher_sal_pnt.elevation_phi_w;
            sp.inverse_dist_rho = sbi.spher_sal_pnt.inverse_dist_rho;
        }
        sal_pnts->push_back(sp);
    }
}

void DavisonMonoSlam::CameraCoordinatesEuclidUnityDirFromPolarAngles(Scalar azimuth_theta, Scalar elevation_phi, Scalar* hx, Scalar* hy, Scalar* hz)
{
    // polar -> euclidean position of salient point in world frame with center in camera position
    // theta=azimuth
    Scalar cos_th = std::cos(azimuth_theta);
    Scalar sin_th = std::sin(azimuth_theta);

    // phi=elevation
    Scalar cos_ph = std::cos(elevation_phi);
    Scalar sin_ph = std::sin(elevation_phi);

    *hx =  cos_ph * sin_th;
    *hy = -sin_ph;
    *hz =  cos_ph * cos_th;
}

template <typename EigenMat>
bool CheckUncertCovMat(const EigenMat& pos_uncert, bool can_throw)
{
    constexpr Scalar ellipse_cut_thr = 0.05f;

    Eigen::Matrix<Scalar, kEucl3, 1> pnt_pos{ 0, 0, 0 };

    auto [op, rot_ellipsoid] = GetRotatedUncertaintyEllipsoidFromCovMat(pos_uncert, pnt_pos, ellipse_cut_thr);
    if (can_throw) SRK_ASSERT(op);
    return op;
}

template <typename EigenMat>
void CheckEllipseIsExtractableFrom2DCovarMat(const EigenMat& covar_2D)
{
    constexpr Scalar ellipse_cut_thr = 0.05f;

    Eigen::Matrix<Scalar, kPixPosComps, 1> pnt_pos{ 0, 0 };

    auto [op, rot_ellipse] = Get2DRotatedEllipseFromCovMat(covar_2D, pnt_pos, ellipse_cut_thr);
    SRK_ASSERT(op);
}

void DavisonMonoSlam::CheckCameraAndSalientPointsCovs(
    const EigenDynVec& src_estim_vars,
    const EigenDynMat& src_estim_vars_covar) const
{
    Eigen::Matrix<Scalar, kEucl3, kEucl3> cam_pos_cov = src_estim_vars_covar.block<kEucl3, kEucl3>(0, 0);
    CheckUncertCovMat(cam_pos_cov, true);

    // check camera orientation quaternion is normalized
    CameraStateVars cam_state_vars;
    LoadCameraStateVarsFromArray(Span(src_estim_vars, kCamStateComps), &cam_state_vars);
    Scalar q_norm = cam_state_vars.orientation_wfc.norm();
    SRK_ASSERT(IsCloseAbs(1, q_norm, 0.001)) << "Camera orientation quaternion must be normalized";

    // check there are nonnegative numbers on diagonal of error covariance matrix
    Eigen::Index min_index = -1;
    auto state_err_covar_diag = src_estim_vars_covar.diagonal().eval();
    Scalar min_value = state_err_covar_diag.minCoeff(&min_index);
    SRK_ASSERT(min_value >= 0) << "Error covariance has nonnegative numbers on diagonal";

    for (SalPntId sal_pnt_id : GetSalientPoints())
    {
        const TrackedSalientPoint& sal_pnt = GetSalientPoint(sal_pnt_id);

        CheckSalientPoint(src_estim_vars, src_estim_vars_covar, sal_pnt, true);
    }
}

void DavisonMonoSlam::RemoveSalientPointsWithNonextractableUncertEllipsoid(EigenDynVec *src_estim_vars,
    EigenDynMat* src_estim_vars_covar)
{
    std::vector<size_t> bad_sal_pnt_inds;
    for (SalPntId sal_pnt_id : GetSalientPoints())
    {
        const TrackedSalientPoint& sal_pnt = GetSalientPoint(sal_pnt_id);

        // TODO: need quick way to find out if position uncertainty ellipsoid of salient point is extractable from covariance matrix
        if (!CheckSalientPoint(*src_estim_vars, *src_estim_vars_covar, sal_pnt, false))
            bad_sal_pnt_inds.push_back(sal_pnt.sal_pnt_ind);
    }
    if (bad_sal_pnt_inds.empty())
        return;
    RemoveSalientPoints(bad_sal_pnt_inds);
}

void DavisonMonoSlam::FillRk2x2(Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps>* Rk) const
{
    Scalar measurm_noise_variance = suriko::Sqr(static_cast<Scalar>(measurm_noise_std_pix_));
    *Rk << measurm_noise_variance, 0, 0, measurm_noise_variance;
}

void DavisonMonoSlam::FillRk(size_t obs_sal_pnt_count, EigenDynMat* Rk) const
{
    Rk->setZero(obs_sal_pnt_count * kPixPosComps, obs_sal_pnt_count * kPixPosComps);

    Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps> noise_one;
    FillRk2x2(&noise_one);
    
    for (size_t i = 0; i < obs_sal_pnt_count; ++i)
    {
        size_t off = i * kPixPosComps;
        Rk->block<kPixPosComps, kPixPosComps>(off, off) = noise_one;
    }
}

void DavisonMonoSlam::PredictCameraMotionByKinematicModel(gsl::span<const Scalar> cam_state,
    gsl::span<Scalar> new_cam_state,
    const Eigen::Matrix<Scalar, kProcessNoiseComps, 1>* noise_state) const
{
    Eigen::Map<const Eigen::Matrix<Scalar, kCamStateComps, 1>> cam_state_mat(cam_state.data());

    //Eigen::Map<const Eigen::Matrix<Scalar, kEucl3, 1>> cam_pos(&cam_state[0]);
    //Eigen::Map<const Eigen::Matrix<Scalar, kQuat4, 1>> cam_orient_quat(&cam_state[kEucl3]);
    //Eigen::Map<const Eigen::Matrix<Scalar, kVelocComps, 1>> cam_vel(&cam_state[kEucl3 + kQuat4]);
    //Eigen::Map<const Eigen::Matrix<Scalar, kAngVelocComps, 1>> cam_ang_vel(&cam_state[kEucl3 + kQuat4 + kVelocComps]);
    Eigen::Matrix<Scalar, kEucl3, 1> cam_pos = cam_state_mat.middleRows<kEucl3>(0);
    Eigen::Matrix<Scalar, kQuat4, 1> cam_orient_quat = cam_state_mat.middleRows<kQuat4>(kEucl3);
    Eigen::Matrix<Scalar, kVelocComps, 1> cam_vel = cam_state_mat.middleRows< kVelocComps>(kEucl3 + kQuat4);
    Eigen::Matrix<Scalar, kAngVelocComps, 1> cam_ang_vel = cam_state_mat.middleRows<kAngVelocComps>(kEucl3 + kQuat4 + kVelocComps);

    Eigen::Map<Eigen::Matrix<Scalar, kEucl3, 1>> new_cam_pos(&new_cam_state[0]);
    Eigen::Map<Eigen::Matrix<Scalar, kQuat4, 1>> new_cam_orient_quat(&new_cam_state[kEucl3]);
    Eigen::Map<Eigen::Matrix<Scalar, kVelocComps, 1>> new_cam_vel(&new_cam_state[kEucl3 + kQuat4]);
    Eigen::Map<Eigen::Matrix<Scalar, kAngVelocComps, 1>> new_cam_ang_vel(&new_cam_state[kEucl3 + kQuat4 + kVelocComps]);

    // camera position
    Scalar dT = between_frames_period_;
    new_cam_pos = cam_pos + cam_vel * dT;

    DependsOnInputNoisePackOrder();
    if (noise_state != nullptr)
        new_cam_pos += noise_state->topRows<kAccelComps>() * dT;

    // camera orientation
    Eigen::Matrix<Scalar, kVelocComps, 1> cam_orient_delta = cam_ang_vel * dT;
    if (noise_state != nullptr)
        cam_orient_delta += noise_state->middleRows<kAngAccelComps>(kAccelComps) * dT;

    Eigen::Matrix<Scalar, kQuat4, 1> cam_orient_delta_quat{};
    QuatFromAxisAngle(cam_orient_delta, &cam_orient_delta_quat);

    Eigen::Matrix<Scalar, kQuat4, 1> new_cam_orient_quat_tmp;
    QuatMult(cam_orient_quat, cam_orient_delta_quat, &new_cam_orient_quat_tmp);

    Scalar q_len_normed = new_cam_orient_quat_tmp.norm();
    SRK_ASSERT(IsCloseAbs(1, q_len_normed, 0.001)) << "quaternion must have unity length";

    new_cam_orient_quat = new_cam_orient_quat_tmp;

    // camera velocity is unchanged
    new_cam_vel = cam_vel;
    if (noise_state != nullptr)
        new_cam_vel += noise_state->middleRows<kAccelComps>(0);

    // camera angular velocity is unchanged
    new_cam_ang_vel = cam_ang_vel;
    if (noise_state != nullptr)
        new_cam_ang_vel += noise_state->middleRows<kAngAccelComps>(kAccelComps);
}

void DavisonMonoSlam::PredictEstimVars(
    const EigenDynVec& src_estim_vars, const EigenDynMat& src_estim_vars_covar,
    EigenDynVec* predicted_estim_vars, EigenDynMat* predicted_estim_vars_covar) const
{
    // estimated vars
    std::array<Scalar, kCamStateComps> new_cam{};
    PredictCameraMotionByKinematicModel(Span(src_estim_vars, kCamStateComps), new_cam);

    //
    Eigen::Matrix<Scalar, kCamStateComps, kCamStateComps> F;
    Deriv_cam_state_by_cam_state(&F);

    Eigen::Matrix<Scalar, kCamStateComps, kProcessNoiseComps> G;
    Deriv_cam_state_by_process_noise(&G);

    static bool debug_F_G_derivatives = false;
    if (debug_F_G_derivatives)
    {
        Eigen::Matrix<Scalar, kCamStateComps, kCamStateComps> finite_diff_F;
        FiniteDiff_cam_state_by_cam_state(Span(src_estim_vars, kCamStateComps), kFiniteDiffEpsDebug, &finite_diff_F);

        Scalar diff1 = (finite_diff_F - F).norm();

        Eigen::Matrix<Scalar, kCamStateComps, kProcessNoiseComps> finite_diff_G;
        FiniteDiff_cam_state_by_process_noise(kFiniteDiffEpsDebug, &finite_diff_G);

        Scalar diff2 = (finite_diff_G - G).norm();
        SRK_ASSERT(true);
    }

    // Pvv = F*Pvv*Ft+G*Q*Gt
    Eigen::Matrix<Scalar, kCamStateComps, kCamStateComps> Pvv_new =
        F * src_estim_vars_covar.topLeftCorner<kCamStateComps, kCamStateComps>() * F.transpose() +
        G * process_noise_covar_ * G.transpose();
    
    // Pvm = F*Pvm
    DependsOnOverallPackOrder();
    size_t sal_pnts_vars_count = SalientPointsCount() * kSalientPointComps;
    Eigen::Matrix<Scalar, kCamStateComps, Eigen::Dynamic> Pvm_new = 
        F * src_estim_vars_covar.topRightCorner(kCamStateComps, sal_pnts_vars_count);

    // Pmm is unchanged

    // update x
    *predicted_estim_vars = src_estim_vars;
    predicted_estim_vars->topRows<kCamStateComps>() = Eigen::Map<const Eigen::Matrix<Scalar, kCamStateComps, 1>>(new_cam.data(), kCamStateComps);

    // update P
    *predicted_estim_vars_covar = src_estim_vars_covar;
    predicted_estim_vars_covar->topLeftCorner<kCamStateComps, kCamStateComps>() = Pvv_new;
    predicted_estim_vars_covar->topRightCorner(kCamStateComps, sal_pnts_vars_count) = Pvm_new;
    predicted_estim_vars_covar->bottomLeftCorner(sal_pnts_vars_count, kCamStateComps) = Pvm_new.transpose();

    if (fix_estim_vars_covar_symmetry_)
        FixSymmetricMat(predicted_estim_vars_covar);
}

void DavisonMonoSlam::PredictEstimVarsHelper()
{
    // make predictions
    PredictEstimVars(estim_vars_, estim_vars_covar_, &predicted_estim_vars_, &predicted_estim_vars_covar_);
}

void DavisonMonoSlam::RemoveSalientPoints(gsl::span<size_t> sal_pnt_inds_to_delete_desc)
{
    size_t last_sal_pnt_ind = SalientPointsCount();
    size_t last_sal_pnt_var_ind = SalientPointOffset(last_sal_pnt_ind);
    for (auto remove_sal_pnt_ind : sal_pnt_inds_to_delete_desc)
    {
        if (kSurikoDebug)
        {
            auto remove_sp_id = GetSalientPointIdByOrderInEstimCovMat(remove_sal_pnt_ind);
            auto remove_sp = GetSalientPoint(remove_sp_id);
            std::stringstream ss;
            ss << "Removing SPind=" << remove_sal_pnt_ind;
#if defined(SRK_DEBUG)
            ss << " inited at #" << remove_sp.initial_frame_ind_synthetic_only_ 
                << "[" << remove_sp.initial_templ_center_pix_debug_.X() << "," << remove_sp.initial_templ_center_pix_debug_.Y() << "]";
#endif
            VLOG(4) << ss.str();
        }

        last_sal_pnt_ind--;
        last_sal_pnt_var_ind -= kSalientPointComps;

        // collect all salient points, which are marked for removing, in the back of corresponding array by
        // swapping each removing salient point with the back salient point
        // then all salient points in the back may be swept in one pass

        auto remove_sal_pnt_id = GetSalientPointIdByOrderInEstimCovMat(remove_sal_pnt_ind);
        auto last_sal_pnt_id = GetSalientPointIdByOrderInEstimCovMat(last_sal_pnt_ind);
        TrackedSalientPoint& last_sal_pnt = GetSalientPoint(last_sal_pnt_id);

        sal_pnts_as_ids_.erase(remove_sal_pnt_id);
        latest_frame_sal_pnts_.erase(remove_sal_pnt_id);

        bool need_moving = remove_sal_pnt_ind != last_sal_pnt_ind;  // otherwise it is already in the end and ready to be truncated
        if (need_moving)
        {
            // process indices in the order from high to low
            size_t remove_var_ind = SalientPointOffset(remove_sal_pnt_ind);

            auto move_estim_vars_back = [](size_t rem_ind, size_t back_ind, EigenDynVec* src_estim_vars)
            {
                // move last salient point into the place of deleting salient point
                src_estim_vars->middleRows<kSalientPointComps>(rem_ind) =
                    src_estim_vars->middleRows<kSalientPointComps>(back_ind);
            };
            move_estim_vars_back(remove_var_ind, last_sal_pnt_var_ind, &estim_vars_);
            move_estim_vars_back(remove_var_ind, last_sal_pnt_var_ind, &predicted_estim_vars_);

            auto move_estim_vars_covar_back = [](size_t rem_ind, size_t back_ind, EigenDynMat* src_estim_vars_covar)
            {
                src_estim_vars_covar->middleRows<kSalientPointComps>(rem_ind) =
                    src_estim_vars_covar->middleRows<kSalientPointComps>(back_ind);
                src_estim_vars_covar->middleCols<kSalientPointComps>(rem_ind) =
                    src_estim_vars_covar->middleCols<kSalientPointComps>(back_ind);
            };
            move_estim_vars_covar_back(remove_var_ind, last_sal_pnt_var_ind, &estim_vars_covar_);
            move_estim_vars_covar_back(remove_var_ind, last_sal_pnt_var_ind, &predicted_estim_vars_covar_);

            std::swap(sal_pnts_[remove_sal_pnt_ind], sal_pnts_[last_sal_pnt_ind]);

            // update maintaining indices
            SRK_ASSERT(last_sal_pnt.sal_pnt_ind == last_sal_pnt_ind);
            last_sal_pnt.sal_pnt_ind = remove_sal_pnt_ind;

            SRK_ASSERT(last_sal_pnt.estim_vars_ind == last_sal_pnt_var_ind);
            last_sal_pnt.estim_vars_ind = remove_var_ind;
        }
    }

    // truncate salient points in the back
    if (!sal_pnt_inds_to_delete_desc.empty())
    {
        auto new_sal_pnt_count = sal_pnts_.size() - sal_pnt_inds_to_delete_desc.size();
        sal_pnts_.resize(new_sal_pnt_count);

        estim_vars_.conservativeResize(last_sal_pnt_var_ind);
        predicted_estim_vars_.conservativeResize(last_sal_pnt_var_ind);

        estim_vars_covar_.conservativeResize(last_sal_pnt_var_ind, last_sal_pnt_var_ind);
        predicted_estim_vars_covar_.conservativeResize(last_sal_pnt_var_ind, last_sal_pnt_var_ind);
    }

    if (kSurikoDebug) CheckSalientPointsConsistency();
}

void DavisonMonoSlam::RemoveObsoleteSalientPoints()
{
    if (!sal_pnt_max_undetected_frames_count_.has_value())
        return;

    // update 'unobserved' counter
    std::vector<size_t> sal_pnt_inds_to_delete;
    for (auto sal_pnt_id : sal_pnts_as_ids_)
    {
        auto& sal_pnt = GetSalientPoint(sal_pnt_id);
        if (sal_pnt.track_status == SalPntTrackStatus::Unobserved)
            ++sal_pnt.undetected_frames_count;
        else
            sal_pnt.undetected_frames_count = 0;  // reset counter

        if (sal_pnt_max_undetected_frames_count_.has_value() &&
            sal_pnt.undetected_frames_count > sal_pnt_max_undetected_frames_count_.value())
        {
            sal_pnt_inds_to_delete.push_back(sal_pnt.sal_pnt_ind);
        }
    }

    // init this during debugging to force removing the salient point
    static int sal_pnt_dummy_ind_to_remove = -1;
    if (sal_pnt_dummy_ind_to_remove != -1)
    {
        sal_pnt_inds_to_delete.push_back(sal_pnt_dummy_ind_to_remove);
        auto remove_sal_pnt_id = GetSalientPointIdByOrderInEstimCovMat(sal_pnt_dummy_ind_to_remove);
        latest_frame_sal_pnts_.erase(remove_sal_pnt_id);
    }

    std::sort(sal_pnt_inds_to_delete.begin(), sal_pnt_inds_to_delete.end(),
        [](auto x, auto y) { return x > y; });

    RemoveSalientPoints(sal_pnt_inds_to_delete);
}

void DavisonMonoSlam::ProcessFrame(size_t frame_ind, const Picture& image)
{
    if (stats_logger_ != nullptr) stats_logger_->StartNewFrameStats();

    // initial status of a salient point is 'not observed'
    // later we will overwrite status for the matched salient points as 'matched'
    for (auto sal_pnt_id : sal_pnts_as_ids_)
    {
        auto& sal_pnt = GetSalientPoint(sal_pnt_id);
        sal_pnt.SetUndetected();
    }

    corners_matcher_->AnalyzeFrame(frame_ind, image);

    std::vector<std::pair<SalPntId, CornersMatcherBlobId>> matched_sal_pnts;
    corners_matcher_->MatchSalientPoints(frame_ind, image, sal_pnts_as_ids_, &matched_sal_pnts);

    latest_frame_sal_pnts_.clear();
    for (auto[sal_pnt_id, blob_id] : matched_sal_pnts)
    {
        Point2f templ_center = corners_matcher_->GetBlobCoord(blob_id);
        TrackedSalientPoint& sal_pnt = GetSalientPoint(sal_pnt_id);
        sal_pnt.track_status = SalPntTrackStatus::Matched;
        sal_pnt.SetTemplCenterPix(templ_center, sal_pnt_templ_size_);
        latest_frame_sal_pnts_.insert(sal_pnt_id);
    }

    RemoveObsoleteSalientPoints();

    std::unique_lock<std::shared_mutex> lk(predicted_estim_vars_mutex_, std::defer_lock);
    if (in_multi_threaded_mode_)
        lk.lock();

    switch (mono_slam_update_impl_)
    {
    default:
    case 1:
        ProcessFrame_StackedObservationsPerUpdate(frame_ind);
        break;
    case 2:
        ProcessFrame_OneObservationPerUpdate(frame_ind);
        break;
    case 3:
        ProcessFrame_OneComponentOfOneObservationPerUpdate(frame_ind);
        break;
    }

    // eagerly try allocate new salient points
    std::vector<CornersMatcherBlobId> new_blobs;
    this->corners_matcher_->RecruitNewSalientPoints(frame_ind, image, sal_pnts_as_ids_, matched_sal_pnts, &new_blobs);
    if (!new_blobs.empty())
    {
        CameraStateVars cam_state;
        LoadCameraStateVarsFromArray(Span(estim_vars_, kCamStateComps), &cam_state);

        for (auto blob_id : new_blobs)
        {
            if (debug_max_sal_pnt_coun_.has_value() && 
                SalientPointsCount() >= debug_max_sal_pnt_coun_.value()) break;

            Point2f coord = corners_matcher_->GetBlobCoord(blob_id);

            std::optional<Scalar> pnt_inv_dist_gt;
            if (fake_sal_pnt_initial_inv_dist_)
            {
                pnt_inv_dist_gt = corners_matcher_->GetSalientPointGroundTruthInvDepth(blob_id);
            }

            TemplMatchStats templ_stats{};
            Picture templ_img = corners_matcher_->GetBlobTemplate(blob_id, image);
            if (!templ_img.gray.empty())
            {
                // calculate the statistics of this template (mean and variance), used for matching templates
                auto templ_roi = Recti{ 0, 0, sal_pnt_templ_size_.width, sal_pnt_templ_size_.height };
                Scalar templ_mean = GetGrayImageMean(templ_img.gray, templ_roi);
                Scalar templ_sum_sqr_diff = GetGrayImageSumSqrDiff(templ_img.gray, templ_roi, templ_mean);

                // correlation coefficient is undefined for templates with zero variance (because variance goes into the denominator of corr coef)
                if (IsClose(0, templ_sum_sqr_diff))
                    continue;

                templ_stats.templ_mean_ = templ_mean;
                templ_stats.templ_sqrt_sum_sqr_diff_ = std::sqrt(templ_sum_sqr_diff);
            }

            // current camera frame is the 'first' camera where a salient point is seen the first time: first_cam=cur_cam
            SalPntId sal_pnt_id = AddSalientPoint(frame_ind, cam_state, coord, templ_img, templ_stats, pnt_inv_dist_gt);
            latest_frame_sal_pnts_.insert(sal_pnt_id);
            corners_matcher_->OnSalientPointIsAssignedToBlobId(sal_pnt_id, blob_id, image);
        }

        // now the estimated variables are changed, the dependent predicted variables must be updated too
        predicted_estim_vars_.resizeLike(estim_vars_);
        predicted_estim_vars_covar_.resizeLike(estim_vars_covar_);
    }

    if (stats_logger_ != nullptr)
    {
        stats_logger_->NotifyNewComDelSalPnts(new_blobs.size(), matched_sal_pnts.size(), 0);
        stats_logger_->NotifyEstimatedSalPnts(SalientPointsCount());
    }

    PredictStateAndCovariance();
    EnsureNonnegativeStateVariance(&predicted_estim_vars_covar_);

    if (in_multi_threaded_mode_)
        lk.unlock();

    static bool debug_predicted_vars = false;
    if (debug_predicted_vars || DebugPath(DebugPathEnum::DebugPredictedVarsCov))
    {
        CheckCameraAndSalientPointsCovs(predicted_estim_vars_, predicted_estim_vars_covar_);
    }

    ProcessFrameOnExit_UpdateSalientPoint(frame_ind);

    FinishFrameStats(frame_ind);
}

/// Only portion of the salient points is observed in each frame. Thus index of a salient point in the estimated variables vector is different from 
/// ordering of observed salient points (sal_pnt_ind != obs_sal_pnt_ind)
/// Ordering of observed salient points may be arbitrary.
void MarkOrderingOfObservedSalientPoints() {}

void DavisonMonoSlam::ProcessFrame_StackedObservationsPerUpdate(size_t frame_ind)
{
    if (!latest_frame_sal_pnts_.empty())
    {
        // improve predicted estimation with the info from observations
        std::swap(estim_vars_, predicted_estim_vars_);
        std::swap(estim_vars_covar_, predicted_estim_vars_covar_);
        //estim_vars_ = predicted_estim_vars_;
        //estim_vars_covar_ = predicted_estim_vars_covar_;

        if (kSurikoDebug)
        {
            // iventially these will be set up later in the prediction step
            predicted_estim_vars_.setConstant(kNan);
            predicted_estim_vars_covar_.setConstant(kNan);
            //predicted_estim_vars_ = estim_vars_;
            //predicted_estim_vars_covar_ = estim_vars_covar_;
        }

        const auto& derive_at_pnt = estim_vars_;
        const auto& Pprev = estim_vars_covar_;

        CameraStateVars cam_state;
        LoadCameraStateVarsFromArray(Span(derive_at_pnt, kCamStateComps), &cam_state);

        Eigen::Matrix<Scalar, kEucl3, kEucl3> cam_orient_wfc;
        RotMatFromQuat(gsl::make_span<const Scalar>(cam_state.orientation_wfc.data(), kQuat4), &cam_orient_wfc);

        auto& cache = stacked_update_cache_;
        
        //
        //EigenDynMat Hk; // [2m,13+6n]
        auto& Hk = cache.H;
        Deriv_H_by_estim_vars(cam_state, cam_orient_wfc, derive_at_pnt, &Hk);

        // evaluate filter gain
        //EigenDynMat Rk;
        auto& Rk = cache.R;
        size_t obs_sal_pnt_count = latest_frame_sal_pnts_.size();
        FillRk(obs_sal_pnt_count, &Rk);

        // innovation variance S=H*P*Ht
        //auto innov_var = Hk * Pprev * Hk.transpose() + Rk; // [2m,2m]
        cache.H_P.noalias() = Hk * Pprev;
        auto& innov_var = cache.innov_var;
        innov_var.noalias() = cache.H_P * Hk.transpose(); // [2m,2m]
        innov_var.noalias() += Rk;

        if (stats_logger_ != nullptr)
        {
            auto diag = innov_var.diagonal().array().eval();
            stats_logger_->CurStats().meas_residual_std = diag.sqrt();
        }
        
        //EigenDynMat innov_var_inv = innov_var.inverse();
        auto& innov_var_inv = cache.innov_var_inv;
        static int innov_var_inv_impl = 1;
        if (innov_var_inv_impl == 1)
            innov_var_inv.noalias() = innov_var.inverse();
        else if (innov_var_inv_impl == 2)
        {
            Eigen::FullPivLU<EigenDynMat> llt_of_innov_var(innov_var);
            innov_var_inv.noalias() = llt_of_innov_var.inverse();
        }

        // K=P*Ht*inv(S)
        //EigenDynMat Knew = Pprev * Hk.transpose() * innov_var_inv; // [13+6n,2m]
        auto& Knew = cache.Knew;
        Knew.noalias() = cache.H_P.transpose() * innov_var_inv; // [13+6n,2m]

        //
        //Eigen::Matrix<Scalar, Eigen::Dynamic, 1> zk;
        auto& zk = cache.zk;
        zk.resize(obs_sal_pnt_count * kPixPosComps, 1);

        //Eigen::Matrix<Scalar, Eigen::Dynamic, 1> projected_sal_pnts;
        auto& projected_sal_pnts = cache.projected_sal_pnts;
        projected_sal_pnts.resizeLike(zk);

        size_t obs_sal_pnt_ind = -1;
        for (SalPntId obs_sal_pnt_id : latest_frame_sal_pnts_)
        {
            MarkOrderingOfObservedSalientPoints();
            ++obs_sal_pnt_ind;

            const TrackedSalientPoint& sal_pnt = GetSalientPoint(obs_sal_pnt_id);
            SRK_ASSERT(sal_pnt.IsDetected());

            Point2f corner_pix = sal_pnt.templ_center_pix_.value();
            zk.middleRows<kPixPosComps>(obs_sal_pnt_ind * kPixPosComps) = corner_pix.Mat();

            // project salient point into current camera

            MorphableSalientPoint sal_pnt_vars;
            LoadSalientPointDataFromArray(Span(derive_at_pnt).subspan(sal_pnt.estim_vars_ind, kSalientPointComps), &sal_pnt_vars);

            Eigen::Matrix<Scalar, kPixPosComps, 1> hd = ProjectInternalSalientPoint(cam_state, sal_pnt_vars, nullptr);
            projected_sal_pnts.middleRows<kPixPosComps>(obs_sal_pnt_ind * kPixPosComps) = hd;
        }

        if (stats_logger_ != nullptr)
        {
            auto residual = (zk - projected_sal_pnts).eval();
            stats_logger_->CurStats().meas_residual = residual;
        }

        // Xnew=Xold+K(z-obs)
        // update estimated variables
        if (kSurikoDebug)
        {
            EigenDynVec estim_vars_delta = Knew * (zk - projected_sal_pnts);
            Eigen::Map<Eigen::Matrix<Scalar, kQuat4, 1>> cam_quat(estim_vars_delta.data() + kEucl3);
            Scalar cam_quat_len = cam_quat.norm();
            bool change = cam_quat_len > 0.1;
            if (change)
                VLOG(5) << "estim_vars cam_q_len=" << cam_quat_len;
            estim_vars_.noalias() += estim_vars_delta;
        }
        else
        {
            estim_vars_.noalias() += Knew * (zk - projected_sal_pnts);
        }

        // update covariance matrix
        //estim_vars_covar_.noalias() = (ident - Knew * Hk) * Pprev; // way1
        //estim_vars_covar_.noalias() = Pprev - Knew * innov_var * Knew.transpose(); // way2, 10% faster than way1

        static int upd_cov_mat_impl = 2;
        if (upd_cov_mat_impl == 1)
        {
            // way1, impl of Pnew=(I-K*H)Pold=Pold-K*H*Pold
            size_t n = EstimatedVarsCount();
            auto ident = EigenDynMat::Identity(n, n);
            stacked_update_cache_.K_H_minus_I.noalias() = Knew * Hk;
            stacked_update_cache_.K_H_minus_I -= ident;
#if defined(SRK_DEBUG)
            EigenDynMat K_H_P = Knew * Hk * estim_vars_covar_;
#endif
            predicted_estim_vars_covar_.noalias() = -stacked_update_cache_.K_H_minus_I * estim_vars_covar_;
            std::swap(predicted_estim_vars_covar_, estim_vars_covar_);
            // now, estim_vars_covar_ has valid data
        }
        else if (upd_cov_mat_impl == 2)
        {
            // way2, impl of Pnew=Pold-K*innov_var*Kt
            cache.K_S.noalias() = Knew * innov_var;

#if defined(SRK_DEBUG)
            EigenDynMat K_S_Kt = cache.K_S * Knew.transpose();
#endif
            estim_vars_covar_.noalias() -= cache.K_S * Knew.transpose();
        }

        NormalizeCameraOrientationQuaternionAndCovariances(&estim_vars_, &estim_vars_covar_);

        if (fix_estim_vars_covar_symmetry_)
            FixSymmetricMat(&estim_vars_covar_);

        EnsureNonnegativeStateVariance(&estim_vars_covar_);

        RemoveSalientPointsWithNonextractableUncertEllipsoid(&estim_vars_, &estim_vars_covar_);

        static bool debug_cam_pos = false;
        if (kSurikoDebug && debug_cam_pos && gt_cami_from_tracker_fun_ != nullptr) // ground truth
        {
            SE3Transform cam_orient_cfw_gt = gt_cami_from_tracker_fun_(frame_ind);
            SE3Transform cam_orient_wfc_gt = SE3Inv(cam_orient_cfw_gt);

            Eigen::Matrix<Scalar, kQuat4, 1> cam_orient_wfc_quat;
            QuatFromRotationMatNoRChecks(cam_orient_wfc_gt.R, gsl::make_span<Scalar>(cam_orient_wfc_quat.data(), kQuat4));

            // print norm of delta with gt (pos,orient)
            CameraStateVars cam_state_new;
            LoadCameraStateVarsFromArray(gsl::make_span<const Scalar>(estim_vars_.data(), kCamStateComps), &cam_state_new);
            Scalar d1 = (cam_orient_wfc_gt.T - cam_state_new.pos_w).norm();
            Scalar d2 = (cam_orient_wfc_quat - cam_state_new.orientation_wfc).norm();
            Scalar diff_gt = d1 + d2;
            Scalar estim_change = (zk - projected_sal_pnts).norm();
            VLOG(4) << "diff_gt=" << diff_gt << " zk-obs=" << estim_change;
        }

        static bool debug_estim_vars = false;
        if (debug_estim_vars || DebugPath(DebugPathEnum::DebugEstimVarsCov))
        {
            CheckCameraAndSalientPointsCovs(estim_vars_, estim_vars_covar_);
        }
    }
    else
    {
        // we have no observations => current state <- prediction
        std::swap(estim_vars_, predicted_estim_vars_);
        std::swap(estim_vars_covar_, predicted_estim_vars_covar_);
    }

    OnEstimVarsChanged(frame_ind);
}

void DavisonMonoSlam::ProcessFrame_OneObservationPerUpdate(size_t frame_ind)
{
    if (!latest_frame_sal_pnts_.empty())
    {
        // improve predicted estimation with the info from observations
        std::swap(estim_vars_, predicted_estim_vars_);
        std::swap(estim_vars_covar_, predicted_estim_vars_covar_);
        
        if (kSurikoDebug)
        {
            //predicted_estim_vars_.setConstant(kNan);
            //predicted_estim_vars_covar_.setConstant(kNan);
            // TODO: fix me; initialize predicted, because UI reads it without sync!
            predicted_estim_vars_ = estim_vars_;
            predicted_estim_vars_covar_ = estim_vars_covar_;
        }

        Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps> Rk;
        FillRk2x2(&Rk);

        Scalar diff_vars_total = 0;
        Scalar diff_cov_total = 0;
        for (SalPntId obs_sal_pnt_id : latest_frame_sal_pnts_)
        {
            const TrackedSalientPoint& sal_pnt = GetSalientPoint(obs_sal_pnt_id);

            // the point where derivatives are calculated at
            const EigenDynVec& derive_at_pnt = estim_vars_;
            const EigenDynMat& Pprev = estim_vars_covar_;

            CameraStateVars cam_state;
            LoadCameraStateVarsFromArray(Span(derive_at_pnt, kCamStateComps), &cam_state);

            Eigen::Matrix<Scalar, kEucl3, kEucl3> cam_orient_wfc;
            RotMatFromQuat(gsl::make_span<const Scalar>(cam_state.orientation_wfc.data(), kQuat4), &cam_orient_wfc);

            DependsOnOverallPackOrder();
            const Eigen::Matrix<Scalar, kCamStateComps, kCamStateComps>& Pxx =
                Pprev.topLeftCorner<kCamStateComps, kCamStateComps>(); // camera-camera covariance

            MorphableSalientPoint sal_pnt_vars;
            LoadSalientPointDataFromArray(Span(derive_at_pnt).subspan(sal_pnt.estim_vars_ind, kSalientPointComps), &sal_pnt_vars);

            Eigen::Matrix<Scalar, kPixPosComps, kCamStateComps> hd_by_cam_state;
            Eigen::Matrix<Scalar, kPixPosComps, kSalientPointComps> hd_by_sal_pnt;
            Deriv_hd_by_cam_state_and_sal_pnt(derive_at_pnt, cam_state, cam_orient_wfc, sal_pnt, sal_pnt_vars, &hd_by_cam_state, &hd_by_sal_pnt);

            // 1. innovation variance S[2,2]

            size_t off = sal_pnt.estim_vars_ind;
            const Eigen::Matrix<Scalar, kCamStateComps, kSalientPointComps>& Pxy = 
                Pprev.block<kCamStateComps, kSalientPointComps>(0, off); // camera-sal_pnt covariance
            const Eigen::Matrix<Scalar, kSalientPointComps, kSalientPointComps>& Pyy =
                Pprev.block<kSalientPointComps, kSalientPointComps>(off, off); // sal_pnt-sal_pnt covariance

            Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps> mid = 
                hd_by_cam_state * Pxy * hd_by_sal_pnt.transpose();

            Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps> innov_var_2x2 =
                hd_by_cam_state * Pxx * hd_by_cam_state.transpose() +
                mid + mid.transpose() +
                hd_by_sal_pnt * Pyy * hd_by_sal_pnt.transpose() +
                Rk;
            Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps> innov_var_inv_2x2 = innov_var_2x2.inverse();

            // 2. filter gain [13+6n, 2]: K=(Px*Hx+Py*Hy)*inv(S)

            one_obs_per_update_cache_.P_Hxy.noalias() = Pprev.leftCols<kCamStateComps>() * hd_by_cam_state.transpose(); // P*Hx
            one_obs_per_update_cache_.P_Hxy.noalias() += Pprev.middleCols<kSalientPointComps>(off) * hd_by_sal_pnt.transpose(); // P*Hy

            auto& Knew = one_obs_per_update_cache_.Knew;
            Knew.noalias() = one_obs_per_update_cache_.P_Hxy * innov_var_inv_2x2;

            // 3. update X and P using info derived from salient point observation
            SRK_ASSERT(sal_pnt.IsDetected());
            suriko::Point2f corner_pix = sal_pnt.templ_center_pix_.value();
            
            // project salient point into current camera
            Eigen::Matrix<Scalar, kPixPosComps, 1> hd = ProjectInternalSalientPoint(cam_state, sal_pnt_vars, nullptr);

            //
            auto estim_vars_delta = Knew * (corner_pix.Mat() - hd);
            auto estim_vars_delta_debug = estim_vars_delta.eval();

            one_obs_per_update_cache_.K_S.noalias() = Knew * innov_var_2x2; // cache
            auto estim_vars_covar_delta = one_obs_per_update_cache_.K_S * Knew.transpose();
            auto estim_vars_covar_delta_debug = estim_vars_covar_delta.eval();

            if (kSurikoDebug)
            {
                Scalar estim_vars_delta_norm = estim_vars_delta.norm();
                diff_vars_total += estim_vars_delta_norm;
                
                Scalar estim_vars_covar_delta_norm = estim_vars_covar_delta.norm();
                diff_cov_total += estim_vars_covar_delta_norm;
            }

            //
            estim_vars_.noalias() += estim_vars_delta;
            estim_vars_covar_.noalias() -= estim_vars_covar_delta;

            NormalizeCameraOrientationQuaternionAndCovariances(&estim_vars_, &estim_vars_covar_);
            
            if (fix_estim_vars_covar_symmetry_)
                FixSymmetricMat(&estim_vars_covar_);
        }

        if (kSurikoDebug)
        {
            VLOG(4) << "diff_vars=" << diff_vars_total << " diff_cov=" << diff_cov_total;
        }
    }
    else
    {
        // we have no observations => current state <- prediction
        std::swap(estim_vars_, predicted_estim_vars_);
        std::swap(estim_vars_covar_, predicted_estim_vars_covar_);
    }

    static bool debug_estim_vars = false;
    if (debug_estim_vars || DebugPath(DebugPathEnum::DebugEstimVarsCov))
    {
        CheckCameraAndSalientPointsCovs(estim_vars_, estim_vars_covar_);
    }

    OnEstimVarsChanged(frame_ind);
}

void DavisonMonoSlam::ProcessFrame_OneComponentOfOneObservationPerUpdate(size_t frame_ind)
{
    if (!latest_frame_sal_pnts_.empty())
    {
        // improve predicted estimation with the info from observations
        std::swap(estim_vars_, predicted_estim_vars_);
        std::swap(estim_vars_covar_, predicted_estim_vars_covar_);
        
        if (kSurikoDebug)
        {
            //predicted_estim_vars_.setConstant(kNan);
            //predicted_estim_vars_covar_.setConstant(kNan);
            // TODO: fix me; initialize predicted, because UI reads it without sync!
            predicted_estim_vars_ = estim_vars_;
            predicted_estim_vars_covar_ = estim_vars_covar_;
        }

        Scalar diff_vars_total = 0;
        Scalar diff_cov_total = 0;
        Scalar measurm_noise_variance = suriko::Sqr(static_cast<Scalar>(measurm_noise_std_pix_)); // R[1,1]

        for (SalPntId obs_sal_pnt_id : latest_frame_sal_pnts_)
        {
            const TrackedSalientPoint& sal_pnt = GetSalientPoint(obs_sal_pnt_id);

            // get observation corner
            SRK_ASSERT(sal_pnt.IsDetected());
            Point2f corner_pix = sal_pnt.templ_center_pix_.value();

            for (size_t obs_comp_ind = 0; obs_comp_ind < kPixPosComps; ++obs_comp_ind)
            {
                // the point where derivatives are calculated at
                // attach to the latest state and P
                const EigenDynVec& derive_at_pnt = estim_vars_;
                const EigenDynMat& Pprev = estim_vars_covar_;

                CameraStateVars cam_state;
                LoadCameraStateVarsFromArray(Span(derive_at_pnt, kCamStateComps), &cam_state);

                Eigen::Matrix<Scalar, kEucl3, kEucl3> cam_orient_wfc;
                RotMatFromQuat(gsl::make_span<const Scalar>(cam_state.orientation_wfc.data(), kQuat4), &cam_orient_wfc);

                DependsOnOverallPackOrder();
                const Eigen::Matrix<Scalar, kCamStateComps, kCamStateComps>& Pxx =
                    Pprev.topLeftCorner<kCamStateComps, kCamStateComps>(); // camera-camera covariance

                size_t off = sal_pnt.estim_vars_ind;
                const Eigen::Matrix<Scalar, kCamStateComps, kSalientPointComps>& Pxy =
                    Pprev.block<kCamStateComps, kSalientPointComps>(0, off); // camera-sal_pnt covariance
                const Eigen::Matrix<Scalar, kSalientPointComps, kSalientPointComps>& Pyy =
                    Pprev.block<kSalientPointComps, kSalientPointComps>(off, off); // sal_pnt-sal_pnt covariance

                MorphableSalientPoint sal_pnt_vars;
                LoadSalientPointDataFromArray(Span(derive_at_pnt).subspan(off, kSalientPointComps), &sal_pnt_vars);

                Eigen::Matrix<Scalar, kPixPosComps, kCamStateComps> hd_by_cam_state;
                Eigen::Matrix<Scalar, kPixPosComps, kSalientPointComps> hd_by_sal_pnt;
                Deriv_hd_by_cam_state_and_sal_pnt(derive_at_pnt, cam_state, cam_orient_wfc, sal_pnt, sal_pnt_vars, &hd_by_cam_state, &hd_by_sal_pnt);

                // 1. innovation variance is a scalar (one element matrix S[1,1])
                auto obs_comp_by_cam_state = hd_by_cam_state.middleRows<1>(obs_comp_ind); // [1,13]
                auto obs_comp_by_sal_pnt = hd_by_sal_pnt.middleRows<1>(obs_comp_ind); // [1,6]

                typedef Eigen::Matrix<Scalar, 1, 1> EigenMat11;

                EigenMat11 mid_1x1 = obs_comp_by_cam_state * Pxy * obs_comp_by_sal_pnt.transpose();

                EigenMat11 innov_var_1x1 =
                    obs_comp_by_cam_state * Pxx * obs_comp_by_cam_state.transpose() +
                    obs_comp_by_sal_pnt * Pyy * obs_comp_by_sal_pnt.transpose();

                Scalar innov_var = innov_var_1x1[0] + 2 * mid_1x1[0] + measurm_noise_variance;

                Scalar innov_var_inv = 1 / innov_var;

                // 2. filter gain [13+6n, 1]: K=(Px*Hx+Py*Hy)*inv(S)
                auto& Knew = one_comp_of_obs_per_update_cache_.Knew;
                Knew.noalias() = innov_var_inv * Pprev.leftCols<kCamStateComps>() * obs_comp_by_cam_state.transpose();
                Knew.noalias() += innov_var_inv * Pprev.middleCols<kSalientPointComps>(off) * obs_comp_by_sal_pnt.transpose();

                //
                // project salient point into current camera
                Eigen::Matrix<Scalar, kPixPosComps, 1> hd = ProjectInternalSalientPoint(cam_state, sal_pnt_vars, nullptr);

                // 3. update X and P using info derived from salient point observation

                auto estim_vars_delta = Knew * (corner_pix[obs_comp_ind] - hd[obs_comp_ind]);

                // keep outer product K*Kt lazy ([13+6n,1]*[1,13+6n]=[13+6n,13+6n])
                
                auto estim_vars_covar_delta = innov_var * (Knew * Knew.transpose()); // [13+6n,13+6n]

                // NOTE: (K*Kt)S is 3 times slower than S*(K*Kt) or (S*K)Kt, S=scalar. Why?
                //auto estim_vars_covar_delta = (Knew * Knew.transpose()) * innov_var; // [13+6n,13+6n] slow!!!
                
                if (kSurikoDebug)
                {
                    Scalar estim_vars_delta_norm = estim_vars_delta.norm();
                    diff_vars_total += estim_vars_delta_norm;

                    Scalar estim_vars_covar_delta_norm = estim_vars_covar_delta.norm();
                    diff_cov_total += estim_vars_covar_delta_norm;
                }

                //
                estim_vars_.noalias() += estim_vars_delta;
                estim_vars_covar_.noalias() -= estim_vars_covar_delta;

                NormalizeCameraOrientationQuaternionAndCovariances(&estim_vars_, &estim_vars_covar_);
                
                if (fix_estim_vars_covar_symmetry_)
                    FixSymmetricMat(&estim_vars_covar_);
            }
        }

        if (kSurikoDebug)
        {
            VLOG(4) << "diff_vars=" << diff_vars_total << " diff_cov=" << diff_cov_total;
        }
    }
    else
    {
        // we have no observations => current state <- prediction
        std::swap(estim_vars_, predicted_estim_vars_);
        std::swap(estim_vars_covar_, predicted_estim_vars_covar_);
    }

    static bool debug_estim_vars = false;
    if (debug_estim_vars || DebugPath(DebugPathEnum::DebugEstimVarsCov))
    {
        CheckCameraAndSalientPointsCovs(estim_vars_, estim_vars_covar_);
    }
    
    OnEstimVarsChanged(frame_ind);
}

void DavisonMonoSlam::NormalizeCameraOrientationQuaternionAndCovariances(EigenDynVec* src_estim_vars, EigenDynMat* src_estim_vars_covar)
{
    CameraStateVars cam_state_vars;
    LoadCameraStateVarsFromArray(Span(*src_estim_vars, kCamStateComps), &cam_state_vars);

    auto q = cam_state_vars.orientation_wfc; // cam_orient_quat
    Scalar q_len = q.norm();

    static bool always_normalize = false;
    bool do_normalize = always_normalize || !IsClose(1, q_len);
    if (!do_normalize)
        return;

    // normalize quaternion (formula A.141)
    Eigen::Matrix<Scalar, kQuat4, 1> new_cam_orient_quat = q / q_len;
    auto& est_vars = *src_estim_vars;
    DependsOnCameraPosPackOrder();
    est_vars[kEucl3 + 0] = new_cam_orient_quat[0];
    est_vars[kEucl3 + 1] = new_cam_orient_quat[1];
    est_vars[kEucl3 + 2] = new_cam_orient_quat[2];
    est_vars[kEucl3 + 3] = new_cam_orient_quat[3];

    // normalize quaternion
    Eigen::Matrix<Scalar, kQuat4, kQuat4> dq4x4;
    using suriko::Sqr;
    dq4x4(0, 0) = Sqr(q[1]) + Sqr(q[2]) + Sqr(q[3]);
    dq4x4(1, 1) = Sqr(q[0]) + Sqr(q[2]) + Sqr(q[3]);
    dq4x4(2, 2) = Sqr(q[0]) + Sqr(q[1]) + Sqr(q[3]);
    dq4x4(3, 3) = Sqr(q[0]) + Sqr(q[1]) + Sqr(q[2]);
    dq4x4(0, 1) = dq4x4(1, 0) = -q[0] * q[1];
    dq4x4(0, 2) = dq4x4(2, 0) = -q[0] * q[2];
    dq4x4(0, 3) = dq4x4(3, 0) = -q[0] * q[3];
    dq4x4(1, 2) = dq4x4(2, 1) = -q[1] * q[2];
    dq4x4(1, 3) = dq4x4(3, 1) = -q[1] * q[3];
    dq4x4(2, 3) = dq4x4(3, 2) = -q[2] * q[3];

    Scalar q_mult = std::pow(Sqr(q[0]) + Sqr(q[1]) + Sqr(q[2]) + Sqr(q[3]), (Scalar)-1.5);
    dq4x4 *= q_mult;

    auto& est_vars_covar = *src_estim_vars_covar;

    auto q_up = (est_vars_covar.block<kEucl3, kQuat4>(0, kEucl3) * dq4x4.transpose()).eval();

    auto q_down_size = est_vars_covar.rows() - kEucl3 - kQuat4;
    auto q_down = est_vars_covar.block<Eigen::Dynamic, kQuat4>(kEucl3 + kQuat4, kEucl3, q_down_size, kQuat4) * dq4x4.transpose();

    est_vars_covar.block<kEucl3, kQuat4>(0, kEucl3) = q_up;  // to the up
    est_vars_covar.block<kQuat4, kEucl3>(kEucl3, 0) = q_up.transpose();  // to the left

    // the central block is not mutated yet
    est_vars_covar.block<kQuat4, kQuat4>(kEucl3, kEucl3) =
        dq4x4 *
        est_vars_covar.block<kQuat4, kQuat4>(kEucl3, kEucl3) *
        dq4x4.transpose();

    est_vars_covar.block<Eigen::Dynamic, kQuat4>(kEucl3 + kQuat4, kEucl3, q_down_size, kQuat4) = q_down;  // to the down
    est_vars_covar.block<kQuat4, Eigen::Dynamic>(kEucl3, kEucl3 + kQuat4, kQuat4, q_down_size) = q_down.transpose();  // to the right
}

void DavisonMonoSlam::EnsureNonnegativeStateVariance(EigenDynMat* src_estim_vars_covar)
{
    // zeroize tiny negative numbers on diagonal of error covariance (may appear when subtracting tiny numbers)
    // doing it after all updates to the error covariance matrix have completed
    auto estim_err_diag = src_estim_vars_covar->diagonal().array();
    if (kSurikoDebug)
    {
        Eigen::Index min_index = -1;
        Scalar min_value = estim_err_diag.minCoeff(&min_index);
        SRK_ASSERT(min_value > -0.1) << "Got big negative numbers on diagonal of error covariance";
    }
    estim_err_diag = estim_err_diag.max(0);
}

void DavisonMonoSlam::OnEstimVarsChanged(size_t frame_ind)
{
}

void DavisonMonoSlam::FinishFrameStats(size_t frame_ind)
{
    if (stats_logger_ == nullptr) return;

    // the first operation is to stop a timer to reduce overhead of logger
    stats_logger_->RecordFrameFinishTime();

    auto& cur_stats = stats_logger_->CurStats();
    
    constexpr Scalar kNanInJson = -1;  // nan is not represented in json
    cur_stats.cur_reproj_err_meas = CurrentFrameReprojError(FilterStageType::Estimated).value_or(kNanInJson);
    cur_stats.cur_reproj_err_pred = CurrentFrameReprojError(FilterStageType::Predicted).value_or(kNanInJson);

    //
    CameraStateVars cam_state = GetCameraEstimatedVars();

    constexpr auto kCam = DavisonMonoSlam::kCamStateComps;
    Eigen::Matrix<Scalar, kCam, kCam> cam_state_covar;
    GetCameraEstimatedVarsUncertainty(&cam_state_covar);

    cur_stats.cam_state = estim_vars_.topRows<kCamStateComps>();
    cur_stats.cam_state_gt = estim_vars_covar_.topLeftCorner<kCamStateComps, kCamStateComps>().diagonal().array().sqrt();

    cur_stats.sal_pnts_uncert_median = GetRepresentiveSalientPointUncertainty(this);

    cur_stats.estim_err_std = estim_vars_covar_.diagonal().array().sqrt();
    SRK_ASSERT(AllFiniteNotMax(cur_stats.estim_err_std));

    // estimation error is available only when ground truth is available
    if (gt_cami_from_world_fun_ != nullptr)
    {
        CameraStateVars gt_cam_state;
        std::vector<SphericalSalientPointWithBuildInfo> gt_sal_pnt_build_infos;
        GetGroundTruthEstimVars(frame_ind, &gt_cam_state, &gt_sal_pnt_build_infos);

        std::vector<MorphableSalientPoint> gt_sal_pnts;
        ConvertMorphableFromSphericalSalientPoints(gt_sal_pnt_build_infos, kSalPntRepres, &gt_sal_pnts);

        // estimated variables
        EigenDynVec gt_measured_estim_vars;
        gt_measured_estim_vars.resizeLike(estim_vars_);
        SaveEstimVars(gt_cam_state, gt_sal_pnts, &gt_measured_estim_vars);

        cur_stats.cam_state_gt = gt_measured_estim_vars.topRows<kCamStateComps>();

        EigenDynVec estim_errs = estim_vars_ - gt_measured_estim_vars;
        cur_stats.estim_err = estim_errs;

        // check that optimal estimate and its error are uncorrelated E[x_hat*x_err']=0
        Scalar est_mul_err = (estim_vars_ * estim_errs.transpose()).norm();
        cur_stats.optimal_estim_mul_err = est_mul_err;
    }

    stats_logger_->PushCurFrameStats();
}

void DavisonMonoSlam::PredictStateAndCovariance()
{
    // make predictions
    PredictEstimVars(estim_vars_, estim_vars_covar_, &predicted_estim_vars_, &predicted_estim_vars_covar_);
}

void DavisonMonoSlam::GetGroundTruthEstimVars(size_t frame_ind,
    CameraStateVars* cam_state,
    std::vector<SphericalSalientPointWithBuildInfo>* sal_pnt_build_infos) const
{
    SE3Transform tracker_from_world = gt_cami_from_world_fun_(kTrackerOriginCamInd); // =cam0 from world

    SE3Transform cam_cft = gt_cami_from_tracker_new_(tracker_from_world, frame_ind).value();  // cft=camera from tracker
    SE3Transform cam_tfc = SE3Inv(cam_cft);  // tfc=tracker from camera

    std::array<Scalar, kQuat4> cam_tfc_quat;
    QuatFromRotationMatNoRChecks(cam_tfc.R, cam_tfc_quat);

    // camera position
    cam_state->pos_w = cam_tfc.T;

    // camera orientation
    cam_state->orientation_wfc = Eigen::Map<Eigen::Matrix<Scalar, kQuat4, 1>>(cam_tfc_quat.data());

    // default values
    cam_state->velocity_w.setZero();
    cam_state->angular_velocity_c.setZero();

    // for the last frame there is no 'next' frame, thus velocity and angular velocity can't be computed - keep them zero
    std::optional<SE3Transform> next_cam_cft = gt_cami_from_tracker_new_(tracker_from_world, frame_ind + 1);
    if (next_cam_cft.has_value())
    {
        SE3Transform next_cam_tfc = SE3Inv(next_cam_cft.value());

        // camera velocity
        auto cam_vel = (next_cam_tfc.T - cam_tfc.T).eval();
        cam_state->velocity_w = cam_vel;

        // camera angular velocity
        // R * Rdelta = Rnext => Rdelta = inv(R)*Rnext
        auto Rdelta = (cam_tfc.R.transpose() * next_cam_tfc.R).eval();
        Eigen::Matrix<Scalar, kEucl3, 1> cam_ang_vel;
        bool op = AxisAngleFromRotMat(Rdelta, &cam_ang_vel);  // false for identity matrix
        if (op)
        {
            cam_state->angular_velocity_c = cam_ang_vel;
        }
    }

    auto sal_pnt_count = SalientPointsCount();
    sal_pnt_build_infos->resize(sal_pnt_count);

    for (size_t sal_pnt_ind = 0; sal_pnt_ind < sal_pnt_count; ++sal_pnt_ind)
    {
        SalPntId sal_pnt_id = GetSalientPointIdByOrderInEstimCovMat(sal_pnt_ind);
        const TrackedSalientPoint& sal_pnt = GetSalientPoint(sal_pnt_id);

        // The frame where a salient point was first seen.
        // It is used in 6x1 representation of a salient point.
        // For 3x1 representation of a salient point, the first_cam_frame_ind can be used to construct
        // dummy lobe-shape uncertainties of a salient point.
        // So populate it regardless of the type of used salient point's representation
        size_t first_cam_frame_ind = sal_pnt.initial_frame_ind_synthetic_only_;

        static size_t overwrite_first_cam_frame_ind = (size_t)-1;
        if (overwrite_first_cam_frame_ind != (size_t)-1)
            first_cam_frame_ind = overwrite_first_cam_frame_ind;

        SphericalSalientPointWithBuildInfo& sal_pnt_build_info = (*sal_pnt_build_infos)[sal_pnt_ind];
        sal_pnt_build_info.first_cam_frame_ind = first_cam_frame_ind;
        
        //
        SE3Transform first_cam_cft = gt_cami_from_tracker_new_(tracker_from_world, first_cam_frame_ind).value();  // cft=camera from tracker
        SE3Transform first_cam_tfc = SE3Inv(first_cam_cft);
        suriko::Point3 first_cam_pos_in_tracker = suriko::Point3{ first_cam_tfc.T };

        Dir3DAndDistance sal_pnt_in_camera = gt_sal_pnt_in_camera_fun_(tracker_from_world, first_cam_cft, sal_pnt_id);

        // when dist=inf (or unavailable) the rho(or inv_dist)=0
        Scalar inv_dist_rho = sal_pnt_in_camera.dist.has_value() ? 1 / sal_pnt_in_camera.dist.value() : 0;

        Eigen::Matrix<Scalar, kEucl3, 1> hc;
        if (sal_pnt_in_camera.dist.has_value())
        {
            hc = sal_pnt_in_camera.unity_dir * sal_pnt_in_camera.dist.value();
        }
        else
        {
            // point is in infinity => depth is infinity
            // make the last component unity, as in A.58
            hc = sal_pnt_in_camera.unity_dir / sal_pnt_in_camera.unity_dir.z();  // A.58;
        }

        // we are interested in a point, centered in the first camera, where the salient point was first seen,
        // but oriented parallel to the world frame
        suriko::Point3 dir_pos_cam_orient_tracker_old{ first_cam_tfc.R * sal_pnt_in_camera.unity_dir };

        auto hw = suriko::Point3{ first_cam_tfc.R * hc };

        Scalar azim_theta_w = kNan;
        Scalar elev_phi_w = kNan;
        AzimElevFromEuclidCoords(hw, &azim_theta_w, &elev_phi_w);

        sal_pnt_build_info.spher_sal_pnt.first_cam_pos_w = first_cam_pos_in_tracker.Mat();
        sal_pnt_build_info.spher_sal_pnt.azimuth_theta_w = azim_theta_w;
        sal_pnt_build_info.spher_sal_pnt.elevation_phi_w = elev_phi_w;
        sal_pnt_build_info.spher_sal_pnt.inverse_dist_rho = inv_dist_rho;

        sal_pnt_build_info.proj_interm_vars.hc = hc;
        sal_pnt_build_info.proj_interm_vars.Rwfc = first_cam_tfc.R;
        sal_pnt_build_info.proj_interm_vars.hw = hw.Mat();
    }
}

void DavisonMonoSlam::SetCamStateCovarToGroundTruth(EigenDynMat* src_estim_vars_covar) const
{
    auto& est_covar = *src_estim_vars_covar;
    const Scalar cam_orient_q_variance = suriko::Sqr(cam_orient_q_comp_std_);
    const Scalar cam_vel_variance = suriko::Sqr(cam_vel_std_);
    const Scalar cam_ang_vel_variance = suriko::Sqr(cam_ang_vel_std_);
    est_covar(0, 0) = suriko::Sqr(cam_pos_x_std_m_);
    est_covar(1, 1) = suriko::Sqr(cam_pos_y_std_m_);
    est_covar(2, 2) = suriko::Sqr(cam_pos_z_std_m_);
    est_covar(3, 3) = cam_orient_q_variance;
    est_covar(4, 4) = cam_orient_q_variance;
    est_covar(5, 5) = cam_orient_q_variance;
    est_covar(6, 6) = cam_vel_variance;
    est_covar(7, 7) = cam_vel_variance;
    est_covar(8, 8) = cam_vel_variance;
    est_covar(6, 6) = cam_ang_vel_variance;
    est_covar(7, 7) = cam_ang_vel_variance;
    est_covar(8, 8) = cam_ang_vel_variance;
}

Eigen::Matrix<Scalar, kEucl3, kEucl3> DavisonMonoSlam::GetDefaultXyzSalientPointCovar() const
{
    const Scalar sal_pnt_pos_x_variance = suriko::Sqr(sal_pnt_pos_x_std_);
    const Scalar sal_pnt_pos_y_variance = suriko::Sqr(sal_pnt_pos_y_std_);
    const Scalar sal_pnt_pos_z_variance = suriko::Sqr(sal_pnt_pos_z_std_);

    Eigen::Matrix<Scalar, kEucl3, kEucl3> m{};
    m.setZero();
    m(0, 0) = sal_pnt_pos_x_variance;
    m(1, 1) = sal_pnt_pos_y_variance;
    m(2, 2) = sal_pnt_pos_z_variance;
    return m;
}

void DavisonMonoSlam::SetEstimStateCovarInEstimSpace(size_t frame_ind)
{
    // covariances of estimated variables
    estim_vars_covar_.setZero();
    SetCamStateCovarToGroundTruth(&estim_vars_covar_);

    const Scalar sal_pnt_first_cam_pos_variance = suriko::Sqr(sal_pnt_first_cam_pos_std_);
    const Scalar sal_pnt_azimuth_variance = suriko::Sqr(sal_pnt_azimuth_std_);
    const Scalar sal_pnt_elevation_variance = suriko::Sqr(sal_pnt_elevation_std_);

    for (size_t sal_pnt_ind = 0; sal_pnt_ind < SalientPointsCount(); ++sal_pnt_ind)
    {
        size_t sal_pnt_offset = SalientPointOffset(sal_pnt_ind);

        if (kSalPntRepres == SalPntComps::kXyz)
        {
            auto xyz_sal_pnt_covar = GetDefaultXyzSalientPointCovar();
            estim_vars_covar_.block<kEucl3, kEucl3>(sal_pnt_offset, sal_pnt_offset) = xyz_sal_pnt_covar;
        }
        else if (kSalPntRepres == SalPntComps::kSphericalFirstCamInvDist)
        {
            auto sal_pnt_covar = estim_vars_covar_.block<kSphericalSalientPointComps, kSphericalSalientPointComps>(sal_pnt_offset, sal_pnt_offset);
            sal_pnt_covar(0, 0) = sal_pnt_first_cam_pos_variance;
            sal_pnt_covar(1, 1) = sal_pnt_first_cam_pos_variance;
            sal_pnt_covar(2, 2) = sal_pnt_first_cam_pos_variance;
            sal_pnt_covar(3, 3) = sal_pnt_azimuth_variance;
            sal_pnt_covar(4, 4) = sal_pnt_elevation_variance;
            sal_pnt_covar(5, 5) = suriko::Sqr(sal_pnt_init_inv_dist_std_);
        }
    }
}

void DavisonMonoSlam::SetEstimStateCovarLikeInAddNewSalPnt(size_t frame_ind,
    const std::vector<SphericalSalientPointWithBuildInfo>& sal_pnt_build_infos)
{
    // covariances of estimated variables
    estim_vars_covar_.setZero();
    SetCamStateCovarToGroundTruth(&estim_vars_covar_);

    SE3Transform tracker_from_world = gt_cami_from_world_fun_(kTrackerOriginCamInd); // =cam0 from world

    // current camera frame
    SE3Transform cur_cam_cft = gt_cami_from_tracker_new_(tracker_from_world, frame_ind).value();  // cft=camera from tracker
    SE3Transform cur_cam_tfc = SE3Inv(cur_cam_cft);  // tfc=tracker from camera

    CameraStateVars cur_cam_state{};
    cur_cam_state.pos_w = cur_cam_tfc.T;
    QuatFromRotationMatNoRChecks(cur_cam_tfc.R, Span(cur_cam_state.orientation_wfc));

    size_t sal_pnt_count = SalientPointsCount();
    for (size_t sal_pnt_ind = 0; sal_pnt_ind < sal_pnt_count; ++sal_pnt_ind)
    {
        size_t sal_pnt_offset = SalientPointOffset(sal_pnt_ind);
        const SphericalSalientPointWithBuildInfo& sal_pnt_build_info = sal_pnt_build_infos[sal_pnt_ind];

        // the camera frame, which observes a salient point for the first time, is specific for each salient point
        size_t first_cam_frame_ind = sal_pnt_build_info.first_cam_frame_ind;
        SE3Transform first_cam_cft = gt_cami_from_tracker_new_(tracker_from_world, first_cam_frame_ind).value();  // cft=camera from tracker
        SE3Transform first_cam_tfc = SE3Inv(first_cam_cft);  // tfc=tracker from camera

        CameraStateVars first_cam_state{};
        first_cam_state.pos_w = first_cam_tfc.T;
        QuatFromRotationMatNoRChecks(first_cam_tfc.R, Span(first_cam_state.orientation_wfc));

        Eigen::Matrix<Scalar, kPixPosComps, 1> first_cam_corner_pix = ProjectCameraSalientPoint(sal_pnt_build_info.proj_interm_vars.hc, nullptr);

        // P

        size_t take_vars_count = sal_pnt_offset;

        // the state is taken from ground truth data, but covariance is calculated in the same way as for the new salient point
        // use covariances for 'inverse depth' salient point representation
        Eigen::Matrix<Scalar, kSphericalSalientPointComps, kSphericalSalientPointComps> spher_sal_pnt_autocovar;
        Eigen::Matrix<Scalar, kSphericalSalientPointComps, Eigen::Dynamic> spher_sal_pnt_to_other_covar;
        GetNewSphericalSalientPointCovar(first_cam_state, suriko::Point2f{ first_cam_corner_pix }, sal_pnt_build_info.proj_interm_vars, take_vars_count, &spher_sal_pnt_autocovar, &spher_sal_pnt_to_other_covar);

        SalPntComps sal_pnt_repres = kSalPntRepres;
        if (sal_pnt_repres == SalPntComps::kXyz)
        {
            Eigen::Matrix<Scalar, kXyzSalientPointComps, kXyzSalientPointComps> xyz_sal_pnt_autocovar;
            Eigen::Matrix<Scalar, kXyzSalientPointComps, Eigen::Dynamic> xyz_sal_pnt_to_other_covar;
            GetDefaultXyzSalientPointCovarOrConvertFromSpherical(sal_pnt_build_info.spher_sal_pnt, take_vars_count,
                spher_sal_pnt_autocovar,
                spher_sal_pnt_to_other_covar,
                &xyz_sal_pnt_autocovar, &xyz_sal_pnt_to_other_covar);

            estim_vars_covar_.block(take_vars_count, 0, kXyzSalientPointComps, take_vars_count) = xyz_sal_pnt_to_other_covar;
            estim_vars_covar_.block(0, take_vars_count, take_vars_count, kXyzSalientPointComps) = xyz_sal_pnt_to_other_covar.transpose();

            estim_vars_covar_.block(take_vars_count, take_vars_count, kXyzSalientPointComps, kXyzSalientPointComps) = xyz_sal_pnt_autocovar;
        }
        else if (sal_pnt_repres == SalPntComps::kSphericalFirstCamInvDist)
        {
            estim_vars_covar_.block(take_vars_count, 0, kSphericalSalientPointComps, take_vars_count) = spher_sal_pnt_to_other_covar;
            estim_vars_covar_.block(0, take_vars_count, take_vars_count, kSphericalSalientPointComps) = spher_sal_pnt_to_other_covar.transpose();

            estim_vars_covar_.block(take_vars_count, take_vars_count, kSphericalSalientPointComps, kSphericalSalientPointComps) = spher_sal_pnt_autocovar;
        }
    }
}

void DavisonMonoSlam::SetEstimStateAndCovarToGroundTruth(size_t frame_ind)
{
    CameraStateVars cam_state;
    std::vector<SphericalSalientPointWithBuildInfo> sal_pnt_build_infos;
    GetGroundTruthEstimVars(frame_ind, &cam_state, &sal_pnt_build_infos);

    std::vector<MorphableSalientPoint> sal_pnts;
    ConvertMorphableFromSphericalSalientPoints(sal_pnt_build_infos, kSalPntRepres, &sal_pnts);

    // estimated variables
    EigenDynVec gt_measured_estim_vars;
    gt_measured_estim_vars.resizeLike(estim_vars_);
    SaveEstimVars(cam_state, sal_pnts, &gt_measured_estim_vars);

    estim_vars_ = gt_measured_estim_vars;

    // covariance
    if (set_estim_state_covar_to_gt_impl_ == 1)
        SetEstimStateCovarInEstimSpace(frame_ind);
    else if (set_estim_state_covar_to_gt_impl_ == 2)
        SetEstimStateCovarLikeInAddNewSalPnt(frame_ind, sal_pnt_build_infos);

    PredictStateAndCovariance();  // recalculate the predictions
}

template <typename EigenVec>
std::ostringstream& FormatVec(std::ostringstream& os, const EigenVec& v)
{
    os << "[";
    if (v.rows() > 0)
        os << v[0];

    for (int i=1; i<v.rows(); ++i)
    {
        // truncate close to zero value to exact zero
        auto d = v[i];
        if (IsClose(0, d))
            d = 0;

        os << " " << d;
    }
    os << "]";
    return os;
}

void DavisonMonoSlam::DumpTrackerState(std::ostringstream& os) const
{
    auto filter_state = FilterStageType::Estimated;

    os << "Estimated state:" << std::endl;
    CameraStateVars cam_vars = GetCameraStateVars(filter_state);
    os << "cam.pos: ";
    FormatVec(os, cam_vars.pos_w) << std::endl;
    os << "cam.orient_wfc: ";
    FormatVec(os, cam_vars.orientation_wfc) << std::endl;

    Eigen::Matrix<Scalar, 3, 3> Rwfc;
    RotMatFromQuat(Span(cam_vars.orientation_wfc), &Rwfc);
    Eigen::Map<Eigen::Matrix<Scalar, 9, 1>, Eigen::ColMajor> Rwfc_s(Rwfc.data());
    os << "cam.Rwfc_s: ";
    FormatVec(os, Rwfc_s) <<std::endl;

    os << "cam.vel: ";
    FormatVec(os, cam_vars.velocity_w) << std::endl;
    os << "cam.angvel: ";
    FormatVec(os, cam_vars.angular_velocity_c) << std::endl;

    const EigenDynVec* p_src_estim_vars;
    const EigenDynMat* p_src_estim_vars_covar;
    std::tie(p_src_estim_vars, p_src_estim_vars_covar) = GetFilterStage(filter_state);

    // camera covariance
    DependsOnCameraPosPackOrder();
    auto cam_pos_covar = p_src_estim_vars_covar->topLeftCorner<kEucl3, kEucl3>().eval();
    Eigen::Matrix<Scalar, kEucl3, 1> cam_pos_covar_diag = cam_pos_covar.diagonal();
    os << "cam.pos.covar.diag: ";
    FormatVec(os, cam_pos_covar_diag) << std::endl;

    auto cam_orient_covar = p_src_estim_vars_covar->block<kQuat4, kQuat4>(kEucl3, kEucl3).eval();
    Eigen::Matrix<Scalar, kQuat4, 1> cam_orient_covar_diag = cam_orient_covar.diagonal();
    os << "cam.orient.covar.diag: ";
    FormatVec(os, cam_orient_covar_diag) << std::endl;

    auto cam_vel_covar = p_src_estim_vars_covar->block<kEucl3, kEucl3>(kEucl3 + kQuat4, kEucl3 + kQuat4).eval();
    Eigen::Matrix<Scalar, kEucl3, 1> cam_vel_covar_diag = cam_vel_covar.diagonal();
    os << "cam.vel.covar.diag: ";
    FormatVec(os, cam_vel_covar_diag) << std::endl;

    auto cam_ang_vel_covar = p_src_estim_vars_covar->block<kEucl3, kEucl3>(kEucl3 + kQuat4 + kEucl3, kEucl3 + kQuat4 + kEucl3).eval();
    Eigen::Matrix<Scalar, kEucl3, 1> cam_ang_vel_covar_diag = cam_ang_vel_covar.diagonal();
    os << "cam.angvel.covar.diag: ";
    FormatVec(os, cam_ang_vel_covar_diag) << std::endl;

    //
    for (SalPntId sal_pnt_id : latest_frame_sal_pnts_)
    {
        auto& sal_pnt = GetSalientPoint(sal_pnt_id);
        os << "SP[ind=" << sal_pnt.sal_pnt_ind << "] center=";
        if (sal_pnt.templ_center_pix_.has_value())
            FormatVec(os, sal_pnt.templ_center_pix_.value().Mat());
        else
            os << "none";
        
        // get 3D position in tracker (usually =cam0) coordinates
        os << " estim3D=";
        Eigen::Matrix<Scalar, kEucl3, 1> pos_mean;
        bool op = GetSalientPoint3DPosWithUncertaintyHelper(filter_state, sal_pnt_id, &pos_mean, nullptr);
        if (op)
            FormatVec(os, pos_mean);
        else
            os << "NA";
        os << std::endl;

        auto sal_pnt_vars_span = Span(*p_src_estim_vars).subspan(sal_pnt.estim_vars_ind, kSalientPointComps);
        MorphableSalientPoint sal_pnt_vars;
        LoadSalientPointDataFromArray(sal_pnt_vars_span, &sal_pnt_vars);

        if constexpr (kSalPntRepres == SalPntComps::kXyz)
        {
#if defined(XYZ_SAL_PNT_REPRES)
            os << "SP.pos: ";
            FormatVec(os, sal_pnt_vars.pos_w) << std::endl;
#endif
        }
        else if constexpr (kSalPntRepres == SalPntComps::kSphericalFirstCamInvDist)
        {
#if defined(SPHER_SAL_PNT_REPRES)
            os << "SP.firstcam: ";
            FormatVec(os, sal_pnt_vars.first_cam_pos_w) << std::endl;
            os << "SP.azim_theta: " << sal_pnt_vars.azimuth_theta_w << std::endl;
            os << "SP.elev_phi: " << sal_pnt_vars.elevation_phi_w << std::endl;
            os << "SP.inv_d_rho: " << sal_pnt_vars.inverse_dist_rho;

            os << " d: ";
            if (!IsClose(0, sal_pnt_vars.inverse_dist_rho))
                os << 1 / sal_pnt_vars.inverse_dist_rho;
            else
                os << "NA";
            os << std::endl;
#endif
        }

        // salient point covariance
         auto sal_pnt_covar = p_src_estim_vars_covar->block<kSalientPointComps, kSalientPointComps>(sal_pnt.estim_vars_ind, sal_pnt.estim_vars_ind).eval();
        Eigen::Matrix<Scalar, kSalientPointComps, 1> sal_pnt_covar_diag = sal_pnt_covar.diagonal();
        os << "SP.covar.diag: ";
        FormatVec(os, sal_pnt_covar_diag) << std::endl;
    }
}

void DavisonMonoSlam::ProcessFrameOnExit_UpdateSalientPoint(size_t frame_ind)
{
#if defined(SRK_DEBUG)
    for (SalPntId sal_pnt_id : latest_frame_sal_pnts_)
    {
        auto& sal_pnt = GetSalientPoint(sal_pnt_id);
        if (!sal_pnt.IsDetected()) continue;
        sal_pnt.prev_detection_frame_ind_debug_ = frame_ind;
        sal_pnt.prev_detection_templ_center_pix_debug_ = sal_pnt.templ_center_pix_.value();
    }
#endif
}

void DavisonMonoSlam::BackprojectPixelIntoCameraPlane(const Eigen::Matrix<Scalar, kPixPosComps, 1>& hu, Eigen::Matrix<Scalar, kEucl3, 1>* pos_camera) const
{
    std::array<Scalar, 2> f_pix = cam_intrinsics_.FocalLengthPix();
    Scalar Cx = cam_intrinsics_.principal_point_pix[0];
    Scalar Cy = cam_intrinsics_.principal_point_pix[1];

    // A.58
    Eigen::Matrix<Scalar, kEucl3, 1>& hc = *pos_camera;;
    hc[0] = -(hu[0] - Cx) / f_pix[0];
    hc[1] = -(hu[1] - Cy) / f_pix[1];
    hc[2] = 1;
}

void DavisonMonoSlam::AllocateAndInitStateForNewSalientPoint(size_t new_sal_pnt_var_ind,
    const CameraStateVars& cam_state, suriko::Point2f corner_pix,
    std::optional<Scalar> pnt_inv_dist_gt)
{
    SphericalSalientPointIntermProjVars interm_proj_vars;
    SphericalSalientPoint spher_sal_pnt = GetNewSphericalSalientPointState(cam_state, corner_pix, pnt_inv_dist_gt, &interm_proj_vars);

    size_t vars_count_before = EstimatedVarsCount();
    size_t new_vars_count = kSalientPointComps;
    size_t vars_count_after = vars_count_before + new_vars_count;

    // internal salient point state is either in XYZ or Spherical format
    // allocate both to switch between them at runtime

    Eigen::Matrix<Scalar, kSphericalSalientPointComps, 1> spher_sal_pnt_vars;
    Eigen::Matrix<Scalar, kSphericalSalientPointComps, kSphericalSalientPointComps> spher_sal_pnt_autocovar;
    Eigen::Matrix<Scalar, kSphericalSalientPointComps, Eigen::Dynamic> spher_sal_pnt_to_other_covar;
    GetNewSphericalSalientPointCovar(cam_state, corner_pix, interm_proj_vars, vars_count_before, &spher_sal_pnt_autocovar, &spher_sal_pnt_to_other_covar);

    Eigen::Matrix<Scalar, kXyzSalientPointComps, 1> xyz_sal_pnt_vars;
    Eigen::Matrix<Scalar, kXyzSalientPointComps, kXyzSalientPointComps> xyz_sal_pnt_autocovar;
    Eigen::Matrix<Scalar, kXyzSalientPointComps, Eigen::Dynamic> xyz_sal_pnt_to_other_covar;

    if (kSalPntRepres == SalPntComps::kXyz)
    {
        // convert spherical [6x1] to Euclidean XYZ [3x1] format
        bool op = ConvertXyzFromSphericalSalientPoint(spher_sal_pnt, &xyz_sal_pnt_vars);
        SRK_ASSERT(op) << "Can't init Euclidean 3D salient point";

        GetDefaultXyzSalientPointCovarOrConvertFromSpherical(spher_sal_pnt, vars_count_before,
            spher_sal_pnt_autocovar,
            spher_sal_pnt_to_other_covar,
            &xyz_sal_pnt_autocovar, &xyz_sal_pnt_to_other_covar);
    }
    else if (kSalPntRepres == SalPntComps::kSphericalFirstCamInvDist)
    {
        SaveSalientPointDataToArray(spher_sal_pnt, Span(spher_sal_pnt_vars));
    }

    // allocate space for estimated variables
    estim_vars_.conservativeResize(vars_count_after);

    if (kSalPntRepres == SalPntComps::kXyz)
    {
        Eigen::Map<Eigen::Matrix<Scalar, kXyzSalientPointComps, 1>> dst_sal_pnt_vars(&estim_vars_[new_sal_pnt_var_ind]);
        dst_sal_pnt_vars = xyz_sal_pnt_vars;
    }
    else if (kSalPntRepres == SalPntComps::kSphericalFirstCamInvDist)
    {
        Eigen::Map<Eigen::Matrix<Scalar, kSphericalSalientPointComps, 1>> dst_sal_pnt_vars(&estim_vars_[new_sal_pnt_var_ind]);
        dst_sal_pnt_vars = spher_sal_pnt_vars;
    }

    // P

    // Pold is augmented with 6 rows and columns corresponding to how a new salient point interact with all other
    // variables and itself. So Pnew=Pold+6rowscols. The values of Pold itself are unchanged.
    // the Eigen's conservative resize uses temporary to resize and copy matrix, slow
    estim_vars_covar_.conservativeResize(vars_count_after, vars_count_after);

    if (kSalPntRepres == SalPntComps::kXyz)
    {
        estim_vars_covar_.bottomLeftCorner(kXyzSalientPointComps, vars_count_before) = xyz_sal_pnt_to_other_covar;
        estim_vars_covar_.topRightCorner(vars_count_before, kXyzSalientPointComps) = xyz_sal_pnt_to_other_covar.transpose();

        estim_vars_covar_.bottomRightCorner(kXyzSalientPointComps, kXyzSalientPointComps) = xyz_sal_pnt_autocovar;
    }
    else if (kSalPntRepres == SalPntComps::kSphericalFirstCamInvDist)
    {
        estim_vars_covar_.bottomLeftCorner(kSphericalSalientPointComps, vars_count_before) = spher_sal_pnt_to_other_covar;
        estim_vars_covar_.topRightCorner(vars_count_before, kSphericalSalientPointComps) = spher_sal_pnt_to_other_covar.transpose();

        estim_vars_covar_.bottomRightCorner(kSphericalSalientPointComps, kSphericalSalientPointComps) = spher_sal_pnt_autocovar;
    }
}

DavisonMonoSlam::SphericalSalientPoint DavisonMonoSlam::GetNewSphericalSalientPointState(
    const CameraStateVars& first_cam_state,
    suriko::Point2f first_cam_corner_pix,
    std::optional<Scalar> first_cam_sal_pnt_inv_dist_gt,
    SphericalSalientPointIntermProjVars* interm_proj_vars) const
{
    // undistort 2D image coordinate
    Eigen::Matrix<Scalar, kPixPosComps, 1> hd = first_cam_corner_pix.Mat(); // distorted
    Eigen::Matrix<Scalar, kPixPosComps, 1> hu = hd; // undistorted

    // A.58
    Eigen::Matrix<Scalar, kEucl3, 1> hc;
    BackprojectPixelIntoCameraPlane(hu, &hc);
    interm_proj_vars->hc = hc;

    Eigen::Matrix<Scalar, kEucl3, kEucl3> first_cam_Rwfc;
    RotMatFromQuat(Span(first_cam_state.orientation_wfc), &first_cam_Rwfc);
    interm_proj_vars->Rwfc = first_cam_Rwfc;

    Eigen::Matrix<Scalar, kEucl3, 1> hw = first_cam_Rwfc * hc;
    interm_proj_vars->hw = hw;

    SphericalSalientPoint sal_pnt_vars;
    //auto dst_vars = gsl::make_span<Scalar>(sal_pnt_vars->data(), kSalientPointComps);
    //DependsOnSalientPointPackOrder();
////    if constexpr (kSalPntRepres == SalPntComps::kEucl3D)
////    {
////#if defined(XYZ_SAL_PNT_REPRES)
////        Eigen::Matrix<Scalar, kEucl3, 1> sal_pnt_pos = hw + cam_state.pos_w;
////        dst_vars[0] = sal_pnt_pos[0];
////        dst_vars[1] = sal_pnt_pos[1];
////        dst_vars[2] = sal_pnt_pos[2];
////#endif
////    }
////    else if constexpr (kSalPntRepres == SalPntComps::kFirstCamPolarInvDepth)
    {
#if defined(SPHER_SAL_PNT_REPRES)
        sal_pnt_vars.first_cam_pos_w = first_cam_state.pos_w;

        AzimElevFromEuclidCoords(suriko::Point3{ hw }, &sal_pnt_vars.azimuth_theta_w, &sal_pnt_vars.elevation_phi_w);

        // NOTE: initial inverse depth is constant because the first image can't provide the depth information
        sal_pnt_vars.inverse_dist_rho = first_cam_sal_pnt_inv_dist_gt.value_or(sal_pnt_init_inv_dist_);
#endif
    }
    return sal_pnt_vars;
}

void DavisonMonoSlam::GetNewSphericalSalientPointCovar(
    const CameraStateVars& first_cam_state,
    suriko::Point2f first_cam_corner_pix,
    const SphericalSalientPointIntermProjVars& proj_side_effect_vars,
    size_t take_estim_vars_count,
    Eigen::Matrix<Scalar, kSphericalSalientPointComps, kSphericalSalientPointComps>* spher_sal_pnt_autocovar,
    Eigen::Matrix<Scalar, kSphericalSalientPointComps, Eigen::Dynamic>* spher_sal_pnt_to_other_covar) const
{
    //Eigen::Matrix<Scalar, kPixPosComps, 1> hd = proj_side_effect_vars.corner_pix.Mat(); // distorted
    //Eigen::Matrix<Scalar, kPixPosComps, 1> hu = hd; // undistorted
    const auto& hc = proj_side_effect_vars.hc;
    const auto& first_cam_Rwfc = proj_side_effect_vars.Rwfc;
    const auto& hw = proj_side_effect_vars.hw;
    SRK_ASSERT(AllFiniteNotMax(first_cam_Rwfc));

    // A.67
    Eigen::Matrix<Scalar, kSphericalSalientPointComps, kEucl3> sal_pnt_by_cam_r;
    sal_pnt_by_cam_r.topRows<3>().setIdentity();
    sal_pnt_by_cam_r.bottomRows<3>().setZero();

    Eigen::Matrix<Scalar, kEucl3, kEucl3> dR_by_dq0;
    Eigen::Matrix<Scalar, kEucl3, kEucl3> dR_by_dq1;
    Eigen::Matrix<Scalar, kEucl3, kEucl3> dR_by_dq2;
    Eigen::Matrix<Scalar, kEucl3, kEucl3> dR_by_dq3;
    Deriv_R_by_q(first_cam_state.orientation_wfc, &dR_by_dq0, &dR_by_dq1, &dR_by_dq2, &dR_by_dq3);

    // A.73
    Eigen::Matrix<Scalar, kEucl3, kQuat4> hw_by_qwfc;
    hw_by_qwfc.middleCols<1>(0) = dR_by_dq0 * hc;
    hw_by_qwfc.middleCols<1>(1) = dR_by_dq1 * hc;
    hw_by_qwfc.middleCols<1>(2) = dR_by_dq2 * hc;
    hw_by_qwfc.middleCols<1>(3) = dR_by_dq3 * hc;

    // salient point by camera quaternion
    Eigen::Matrix<Scalar, kSphericalSalientPointComps, kQuat4> sal_pnt_by_cam_q;
    Eigen::Matrix<Scalar, 1, kEucl3> azim_theta_by_hw;
    Eigen::Matrix<Scalar, 1, kEucl3> elev_phi_by_hw;
    Deriv_azim_theta_elev_phi_by_hw(hw, &azim_theta_by_hw, &elev_phi_by_hw);

    // A.68
    sal_pnt_by_cam_q.topRows<kEucl3>().setZero();
    sal_pnt_by_cam_q.bottomRows<kRho>().setZero();
    sal_pnt_by_cam_q.middleRows<1>(kEucl3) = azim_theta_by_hw * hw_by_qwfc;
    sal_pnt_by_cam_q.middleRows<1>(kEucl3 + 1) = elev_phi_by_hw * hw_by_qwfc; // +1 for azimuth component

    constexpr size_t kCamPQ = kEucl3 + kQuat4;
    Eigen::Matrix<Scalar, kSphericalSalientPointComps, kCamPQ> sal_pnt_by_cam;
    sal_pnt_by_cam.block<kSphericalSalientPointComps, kEucl3>(0, 0) = sal_pnt_by_cam_r;
    sal_pnt_by_cam.block<kSphericalSalientPointComps, kQuat4>(0, kEucl3) = sal_pnt_by_cam_q;

    // A.78
    const Eigen::Matrix<Scalar, kEucl3, kEucl3>& hw_by_hc = first_cam_Rwfc;

    Eigen::Matrix<Scalar, kEucl3, kPixPosComps> hc_by_hu;
    hc_by_hu.setZero();
    std::array<Scalar, 2> f_pix = cam_intrinsics_.FocalLengthPix();
    hc_by_hu(0, 0) = -1 / f_pix[0];
    hc_by_hu(1, 1) = -1 / f_pix[1];

    Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps> hu_by_hd;
    Deriv_hu_by_hd(first_cam_corner_pix, &hu_by_hd);

    Eigen::Matrix <Scalar, kPixPosComps, kPixPosComps> R;
    FillRk2x2(&R);

    // the bottom left horizontal stripe of Pnew
    spher_sal_pnt_to_other_covar->resize(Eigen::NoChange, take_estim_vars_count);
    spher_sal_pnt_to_other_covar->noalias() = sal_pnt_by_cam * estim_vars_covar_.topLeftCorner<kCamPQ, Eigen::Dynamic>(kCamPQ, take_estim_vars_count);

    // A.76-A.79
    Eigen::Matrix<Scalar, kSphericalSalientPointComps - kRho, kEucl3> sal_pnt_by_hw;
    sal_pnt_by_hw.topRows<kEucl3>().setZero();
    sal_pnt_by_hw.middleRows<1>(kEucl3) = azim_theta_by_hw;
    sal_pnt_by_hw.middleRows<1>(kEucl3 + 1) = elev_phi_by_hw; // +1 for azimuth component

    // A.75
    Eigen::Matrix<Scalar, kSphericalSalientPointComps, kPixPosComps + kRho> sal_pnt_by_h_rho;
    sal_pnt_by_h_rho.topLeftCorner<kSphericalSalientPointComps - kRho, kPixPosComps>() = sal_pnt_by_hw * hw_by_hc * hc_by_hu * hu_by_hd;
    sal_pnt_by_h_rho.bottomLeftCorner<kRho, kPixPosComps>().setZero();
    sal_pnt_by_h_rho.rightCols<kRho>().setZero();
    sal_pnt_by_h_rho.bottomRightCorner<kRho, kRho>().setOnes(); // single element

    Scalar rho_init_var = suriko::Sqr(sal_pnt_init_inv_dist_std_);

    // P bottom right corner
    spher_sal_pnt_autocovar->noalias() = spher_sal_pnt_to_other_covar->leftCols<kCamPQ>() * sal_pnt_by_cam.transpose();
    *spher_sal_pnt_autocovar +=
        sal_pnt_by_h_rho.leftCols<kPixPosComps>() * R * sal_pnt_by_h_rho.leftCols<kPixPosComps>().transpose() +
        sal_pnt_by_h_rho.rightCols<kRho>() * rho_init_var * sal_pnt_by_h_rho.rightCols<kRho>().transpose();

    if (kSurikoDebug)
    {
        auto Pyy_1 = (spher_sal_pnt_to_other_covar->leftCols<kCamPQ>() * sal_pnt_by_cam.transpose()).eval();
        auto Pyy_2 = (sal_pnt_by_h_rho.leftCols<kPixPosComps>() * R * sal_pnt_by_h_rho.leftCols<kPixPosComps>().transpose()).eval();
        auto Pyy_3 = (sal_pnt_by_h_rho.rightCols<kRho>() * rho_init_var * sal_pnt_by_h_rho.rightCols<kRho>().transpose()).eval();

        std::array<Scalar, 3> Pyy_norms = { Pyy_1.norm(), Pyy_2.norm(), Pyy_3.norm() };
        auto norm_sum = Pyy_norms[0] + Pyy_norms[1] + Pyy_norms[2];
        auto s_perc = { Pyy_norms[0] / norm_sum, Pyy_norms[1] / norm_sum, Pyy_norms[2] / norm_sum };
        SRK_ASSERT(true);
    }

    //
    if (kSurikoDebug)
    {
        auto Pnew_bottom_left_debug = spher_sal_pnt_to_other_covar->eval();

        // check components of Pyy
        auto Pyy_tmp = spher_sal_pnt_autocovar->eval();
        Scalar Pyy_norm = Pyy_tmp.norm();
        SRK_ASSERT(true);
    }
}

void DavisonMonoSlam::GetDefaultXyzSalientPointCovarOrConvertFromSpherical(
    const SphericalSalientPoint& spher_sal_pnt_vars,
    size_t take_estim_vars_count,
    const Eigen::Matrix<Scalar, kSphericalSalientPointComps, kSphericalSalientPointComps>& spher_sal_pnt_autocovar,
    const Eigen::Matrix<Scalar, kSphericalSalientPointComps, Eigen::Dynamic>& spher_sal_pnt_to_other_covar,
    Eigen::Matrix<Scalar, kXyzSalientPointComps, kXyzSalientPointComps>* xyz_sal_pnt_autocovar,
    Eigen::Matrix<Scalar, kXyzSalientPointComps, Eigen::Dynamic>* xyz_sal_pnt_to_other_covar) const
{
    if (force_xyz_sal_pnt_pos_diagonal_uncert_)
    {
        *xyz_sal_pnt_autocovar = GetDefaultXyzSalientPointCovar();;
        xyz_sal_pnt_to_other_covar->resize(Eigen::NoChange, take_estim_vars_count);
        xyz_sal_pnt_to_other_covar->setZero();
    }
    else
    {
        // derive xyz uncertainty from spherical uncertainty
        Eigen::Matrix<Scalar, kXyzSalientPointComps, kSphericalSalientPointComps> deriv_sal_pnt_xyz_by_spher;
        DerivSalPnt_xyz_by_spher(spher_sal_pnt_vars, &deriv_sal_pnt_xyz_by_spher);

        *xyz_sal_pnt_autocovar = deriv_sal_pnt_xyz_by_spher * spher_sal_pnt_autocovar * deriv_sal_pnt_xyz_by_spher.transpose();
        *xyz_sal_pnt_to_other_covar = deriv_sal_pnt_xyz_by_spher * spher_sal_pnt_to_other_covar;
    }

}


DavisonMonoSlam::SalPntId DavisonMonoSlam::AddSalientPoint(size_t frame_ind, const CameraStateVars& cam_state, suriko::Point2f corner_pix,
    Picture templ_img, TemplMatchStats templ_stats,
    std::optional<Scalar> pnt_inv_dist_gt)
{
    size_t old_sal_pnts_count = SalientPointsCount();
    size_t sal_pnt_var_ind = SalientPointOffset(old_sal_pnts_count);

    AllocateAndInitStateForNewSalientPoint(sal_pnt_var_ind, cam_state, corner_pix, pnt_inv_dist_gt);

    //
    suriko::Point2i top_left = TemplateTopLeftInt(corner_pix);

    // ID of new salient point
    sal_pnts_.push_back(std::make_unique<TrackedSalientPoint>());
    TrackedSalientPoint& sal_pnt = *sal_pnts_.back().get();
    sal_pnt.estim_vars_ind = sal_pnt_var_ind;
    sal_pnt.sal_pnt_ind = old_sal_pnts_count;
    sal_pnt.track_status = SalPntTrackStatus::New;
    sal_pnt.SetTemplCenterPix(corner_pix, sal_pnt_templ_size_);
    sal_pnt.offset_from_top_left_ = suriko::Point2f{ corner_pix.X() - top_left.x, corner_pix.Y() - top_left.y };
    sal_pnt.initial_templ_gray_ = std::move(templ_img.gray);
    sal_pnt.initial_frame_ind_synthetic_only_ = frame_ind;
#if defined(SRK_DEBUG)
    sal_pnt.initial_templ_center_pix_debug_ = corner_pix;
    sal_pnt.initial_templ_top_left_pix_debug_ = top_left;
    sal_pnt.initial_templ_bgr_debug = std::move(templ_img.bgr_debug);
#endif
    sal_pnt.templ_stats = templ_stats;
    //
    SalPntId sal_pnt_id = SalPntId(sal_pnts_.back().get());
    sal_pnts_as_ids_.insert(sal_pnt_id);
    //
    static bool debug_estimated_vars = false;
    if (debug_estimated_vars || DebugPath(DebugPathEnum::DebugEstimVarsCov))
    {
        // check uncertainty ellipsoid can be extracted from covariance matrix
        CheckSalientPoint(estim_vars_, estim_vars_covar_, sal_pnt, true);
    }
    return sal_pnt_id;
}

std::optional<suriko::Point2f> DavisonMonoSlam::GetDetectedSalientTemplCenter(SalPntId sal_pnt_id) const
{
    const auto& sal_pnt = GetSalientPoint(sal_pnt_id);
    if (!sal_pnt.IsDetected())
        return std::nullopt;
    return sal_pnt.templ_center_pix_.value();
}

void DavisonMonoSlam::Deriv_hu_by_hd(suriko::Point2f corner_pix, Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps>* hu_by_hd) const
{
    Scalar ud = corner_pix[0];
    Scalar vd = corner_pix[1];
    Scalar Cx = cam_intrinsics_.principal_point_pix[0];
    Scalar Cy = cam_intrinsics_.principal_point_pix[1];
    Scalar dx = cam_intrinsics_.pixel_size_mm[0];
    Scalar dy = cam_intrinsics_.pixel_size_mm[1];
    Scalar k1 = cam_distort_params_.k1;
    Scalar k2 = cam_distort_params_.k2;

    Scalar rd = std::sqrt(
        suriko::Sqr(dx * (ud - Cx)) +
        suriko::Sqr(dy * (vd - Cy)));

    Scalar tort = 1 + k1 * suriko::Sqr(rd) + k2 * suriko::Pow4(rd);

    Scalar p2 = (k1 + 2 * k2 * suriko::Sqr(rd));

    auto& r = *hu_by_hd;
    r(0, 0) = tort + 2 * suriko::Sqr(dx * (ud - Cx))*p2;
    r(1, 1) = tort + 2 * suriko::Sqr(dy * (vd - Cy))*p2;
    r(1, 0) = 2 * suriko::Sqr(dx) * (vd - Cy)*(ud - Cx)*p2;
    r(0, 1) = 2 * suriko::Sqr(dy) * (vd - Cy)*(ud - Cx)*p2;
}

void DavisonMonoSlam::Deriv_hd_by_hu(suriko::Point2f corner_pix, Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps>* hd_by_hu) const
{
    Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps> hu_by_hd;
    Deriv_hu_by_hd(corner_pix, &hu_by_hd);

    *hd_by_hu = hu_by_hd.inverse(); // A.33
    hd_by_hu->setIdentity(); // TODO: for now, suppport only 'no distortion' model
}

void DavisonMonoSlam::Deriv_hu_by_hc(const SalPntProjectionIntermidVars& proj_hist, Eigen::Matrix<Scalar, kPixPosComps, kEucl3>* hu_by_hc) const
{
    Scalar hcx = proj_hist.hc[0];
    Scalar hcy = proj_hist.hc[1];
    Scalar hcz = proj_hist.hc[2];

    std::array<Scalar, 2> f_pix = cam_intrinsics_.FocalLengthPix();

    auto& m = *hu_by_hc;
    m(0, 0) = -f_pix[0] / hcz;
    m(1, 0) = 0;
    m(0, 1) = 0;
    m(1, 1) = -f_pix[1] / hcz;
    m(0, 2) = f_pix[0] * hcx / (hcz * hcz);
    m(1, 2) = f_pix[1] * hcy / (hcz * hcz);
}

void DavisonMonoSlam::Deriv_hc_by_hu(Eigen::Matrix<Scalar, kEucl3, kPixPosComps>* hc_by_hu) const
{
    auto& m = *hc_by_hu;
    // A.79
    m.setZero();
    std::array<Scalar, 2> f_pix = cam_intrinsics_.FocalLengthPix();
    m(0, 0) = -1 / f_pix[0];
    m(1, 1) = -1 / f_pix[1];
}

void DavisonMonoSlam::Deriv_R_by_q(const Eigen::Matrix<Scalar, kQuat4, 1>& q,
    Eigen::Matrix<Scalar, 3, 3>* dR_by_dq0,
    Eigen::Matrix<Scalar, 3, 3>* dR_by_dq1,
    Eigen::Matrix<Scalar, 3, 3>* dR_by_dq2,
    Eigen::Matrix<Scalar, 3, 3>* dR_by_dq3) const
{
    // A.46-A.49
    *dR_by_dq0 <<
        2 * q[0], -2 * q[3], 2 * q[2],
        2 * q[3], 2 * q[0], -2 * q[1],
        -2 * q[2], 2 * q[1], 2 * q[0];

    *dR_by_dq1 <<
        2 * q[1], 2 * q[2], 2 * q[3],
        2 * q[2], -2 * q[1], -2 * q[0],
        2 * q[3], 2 * q[0], -2 * q[1];

    *dR_by_dq2 <<
        -2 * q[2], 2 * q[1], 2 * q[0],
        2 * q[1], 2 * q[2], 2 * q[3],
        -2 * q[0], 2 * q[3], -2 * q[2];

    *dR_by_dq3 <<
        -2 * q[3], -2 * q[0], 2 * q[1],
        2 * q[0], -2 * q[3], 2 * q[2],
        2 * q[1], 2 * q[2], 2 * q[3];
}

void DavisonMonoSlam::Deriv_hd_by_camera_state(const MorphableSalientPoint& sal_pnt,
    const CameraStateVars& cam_state,
    const Eigen::Matrix<Scalar, kEucl3, kEucl3>& cam_orient_wfc,
    const SalPntProjectionIntermidVars& proj_hist,
    const Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps>& hd_by_hu,
    const Eigen::Matrix<Scalar, kPixPosComps, kEucl3>& hu_by_hc,
    Eigen::Matrix<Scalar, kPixPosComps, kCamStateComps>* hd_by_xc) const
{
    hd_by_xc->setZero();

    //
    Eigen::Matrix<Scalar, kEucl3, kEucl3> Rcw = cam_orient_wfc.transpose();

    Eigen::Matrix<Scalar, kEucl3, kEucl3> dhc_by_dr;
    if constexpr (kSalPntRepres == SalPntComps::kXyz)
    {
        dhc_by_dr = -Rcw;  // A.36
    }
    else if constexpr (kSalPntRepres == SalPntComps::kSphericalFirstCamInvDist)
    {
#if defined(SPHER_SAL_PNT_REPRES)
        dhc_by_dr = -sal_pnt.inverse_dist_rho * Rcw;  // A.35
#endif
    }

    Eigen::Matrix<Scalar, kPixPosComps, kEucl3> dh_by_dr = hd_by_hu * hu_by_hc * dhc_by_dr; // A.31
    hd_by_xc->middleCols<kEucl3>(0) = dh_by_dr;


    //
    Eigen::Matrix<Scalar, kQuat4, 1> cam_orient_cfw = QuatInverse(cam_state.orientation_wfc);
    const auto& q = cam_orient_cfw;

    Eigen::Matrix<Scalar, kEucl3, kEucl3> dR_by_dq0;
    Eigen::Matrix<Scalar, kEucl3, kEucl3> dR_by_dq1;
    Eigen::Matrix<Scalar, kEucl3, kEucl3> dR_by_dq2;
    Eigen::Matrix<Scalar, kEucl3, kEucl3> dR_by_dq3;
    Deriv_R_by_q(q, &dR_by_dq0, &dR_by_dq1, &dR_by_dq2, &dR_by_dq3);

    Eigen::Matrix<Scalar, kEucl3, 1> part2;
    if constexpr (kSalPntRepres == SalPntComps::kXyz)
    {
#if defined(XYZ_SAL_PNT_REPRES)
        part2 = sal_pnt.pos_w - cam_state.pos_w;
#endif
    }
    else if constexpr (kSalPntRepres == SalPntComps::kSphericalFirstCamInvDist)
    {
#if defined(SPHER_SAL_PNT_REPRES)
        part2 = sal_pnt.inverse_dist_rho * (sal_pnt.first_cam_pos_w - cam_state.pos_w) + proj_hist.first_cam_sal_pnt_unity_dir;
#endif
    }

    Eigen::Matrix<Scalar, kEucl3, kQuat4> dhc_by_dqcw; // A.40
    dhc_by_dqcw.middleCols<1>(0) = dR_by_dq0 * part2;
    dhc_by_dqcw.middleCols<1>(1) = dR_by_dq1 * part2;
    dhc_by_dqcw.middleCols<1>(2) = dR_by_dq2 * part2;
    dhc_by_dqcw.middleCols<1>(3) = dR_by_dq3 * part2;

    Eigen::Matrix<Scalar, kQuat4, kQuat4> dqcw_by_dqwc; // A.39
    dqcw_by_dqwc.setIdentity();
    dqcw_by_dqwc(1, 1) = -1;
    dqcw_by_dqwc(2, 2) = -1;
    dqcw_by_dqwc(3, 3) = -1;

    Eigen::Matrix<Scalar, kEucl3, kQuat4> dhc_by_dqwc = dhc_by_dqcw * dqcw_by_dqwc;

    //
    Eigen::Matrix<Scalar, kPixPosComps, kQuat4> dh_by_dqwc = hd_by_hu * hu_by_hc * dhc_by_dqwc;
    hd_by_xc->middleCols<kQuat4>(kEucl3) = dh_by_dqwc;
}

void DavisonMonoSlam::Deriv_hd_by_sal_pnt(const MorphableSalientPoint& sal_pnt,
    const CameraStateVars& cam_state,
    const Eigen::Matrix<Scalar, kEucl3, kEucl3>& cam_orient_wfc,
    const Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps>& hd_by_hu,
    const Eigen::Matrix<Scalar, kPixPosComps, kEucl3>& hu_by_hc,
    Eigen::Matrix<Scalar, kPixPosComps, kSalientPointComps>* hd_by_sal_pnt) const
{
    Eigen::Matrix<Scalar, kEucl3, kEucl3> Rcw = cam_orient_wfc.transpose();

    Eigen::Matrix<Scalar, kEucl3, kSalientPointComps> dhc_by_dy;
    if constexpr (kSalPntRepres == SalPntComps::kXyz)
    {
        // A.55
        dhc_by_dy = Rcw;
    }
    else if constexpr (kSalPntRepres == SalPntComps::kSphericalFirstCamInvDist)
    {
#if defined(SPHER_SAL_PNT_REPRES)
        Scalar cos_phi = std::cos(sal_pnt.elevation_phi_w);
        Scalar sin_phi = std::sin(sal_pnt.elevation_phi_w);
        Scalar cos_theta = std::cos(sal_pnt.azimuth_theta_w);
        Scalar sin_theta = std::sin(sal_pnt.azimuth_theta_w);

        // A.53
        Eigen::Matrix<Scalar, kEucl3, 1> dm_by_dtheta; // azimuth
        dm_by_dtheta[0] = cos_phi * cos_theta;
        dm_by_dtheta[1] = 0;
        dm_by_dtheta[2] = -cos_phi * sin_theta;

        // A.54
        Eigen::Matrix<Scalar, kEucl3, 1> dm_by_dphi; // elevation
        dm_by_dphi[0] = -sin_phi * sin_theta;
        dm_by_dphi[1] = -cos_phi;
        dm_by_dphi[2] = -sin_phi * cos_theta;

        // A.52
        DependsOnSalientPointPackOrder();
        dhc_by_dy.middleCols<kEucl3>(0) = sal_pnt.inverse_dist_rho * Rcw;
        dhc_by_dy.middleCols<1>(kEucl3+0) = Rcw * dm_by_dtheta;
        dhc_by_dy.middleCols<1>(kEucl3+1) = Rcw * dm_by_dphi;
        dhc_by_dy.middleCols<1>(kEucl3+2) = Rcw * (sal_pnt.first_cam_pos_w - cam_state.pos_w);
#endif
    }

    // A.51
    *hd_by_sal_pnt = hd_by_hu * hu_by_hc * dhc_by_dy;
}

void DavisonMonoSlam::Deriv_azim_theta_elev_phi_by_hw(
    const Eigen::Matrix<Scalar, kEucl3, 1>& hw,
    Eigen::Matrix<Scalar, 1, kEucl3>* azim_theta_by_hw,
    Eigen::Matrix<Scalar, 1, kEucl3>* elev_phi_by_hw) const
{
    Scalar dist_xz_sqr = suriko::Sqr(hw[0]) + suriko::Sqr(hw[2]);

    (*azim_theta_by_hw)[0] = hw[2] / dist_xz_sqr;
    (*azim_theta_by_hw)[1] = 0;
    (*azim_theta_by_hw)[2] = -hw[0] / dist_xz_sqr;

    Scalar dist_sqr = dist_xz_sqr + suriko::Sqr(hw[1]);
    Scalar dist_xz = std::sqrt(dist_xz_sqr);
    Scalar s = hw[1] / (dist_sqr * dist_xz);

    (*elev_phi_by_hw)[0] = hw[0] * s;
    (*elev_phi_by_hw)[1] = -dist_xz / dist_sqr;
    (*elev_phi_by_hw)[2] = hw[2] * s;
}

std::optional<Eigen::Matrix<Scalar, kEucl3, 1>> DavisonMonoSlam::InternalSalientPointToCamera(
    const MorphableSalientPoint& sal_pnt_vars,
    const CameraStateVars& cam_state,
    bool scaled_by_inv_dist,
    SalPntProjectionIntermidVars *proj_hist) const
{
    Eigen::Matrix<Scalar, kEucl3, kEucl3> camk_orient_wfc;
    RotMatFromQuat(gsl::make_span<const Scalar>(cam_state.orientation_wfc.data(), kQuat4), &camk_orient_wfc);

    Eigen::Matrix<Scalar, kEucl3, kEucl3> camk_orient_cfw33 = camk_orient_wfc.transpose();

    SE3Transform camk_wfc(camk_orient_wfc, cam_state.pos_w);
    SE3Transform camk_orient_cfw = SE3Inv(camk_wfc);

    std::optional<Eigen::Matrix<Scalar, kEucl3, 1>> sal_pnt_cam;
    if constexpr (kSalPntRepres == SalPntComps::kXyz)
    {
#if defined(XYZ_SAL_PNT_REPRES)
        // A.22
        Eigen::Matrix<Scalar, kEucl3, 1> sal_pnt_camk_eucl = camk_orient_cfw33 * (sal_pnt_vars.pos_w - cam_state.pos_w);
        auto sal_pnt_in_cam_debug = SE3Apply(camk_orient_cfw, sal_pnt_vars.pos_w);
        sal_pnt_cam = sal_pnt_camk_eucl;
#endif
    }
    else if constexpr (kSalPntRepres == SalPntComps::kSphericalFirstCamInvDist)
    {
#if defined(SPHER_SAL_PNT_REPRES)
        if (IsClose(0, sal_pnt_vars.inverse_dist_rho) && !scaled_by_inv_dist)
        {
            // the 3D pos is requested, but it can't be realized because it is in infinity
            return std::nullopt;
        }

        Eigen::Matrix<Scalar, kEucl3, 1> m;
        CameraCoordinatesEuclidUnityDirFromPolarAngles(sal_pnt_vars.azimuth_theta_w, sal_pnt_vars.elevation_phi_w, &m[0], &m[1], &m[2]);

        if (proj_hist != nullptr)
        {
            proj_hist->first_cam_sal_pnt_unity_dir = m;
        }

        // salient point in the world
        Eigen::Matrix<Scalar, kEucl3, 1> p_world = sal_pnt_vars.first_cam_pos_w + (1 / sal_pnt_vars.inverse_dist_rho) * m;

        // salient point in the cam-k (naive way)
        Eigen::Matrix<Scalar, kEucl3, 1> sal_pnt_world_cam_orig = sal_pnt_vars.first_cam_pos_w - cam_state.pos_w + (1 / sal_pnt_vars.inverse_dist_rho)*m;
        Eigen::Matrix<Scalar, kEucl3, 1> sal_pnt_camk_simple1 = camk_orient_cfw33 * (sal_pnt_vars.first_cam_pos_w - cam_state.pos_w + (1 / sal_pnt_vars.inverse_dist_rho)*m);
        suriko::Point3 p_world_back_simple1 = SE3Apply(camk_wfc, suriko::Point3(sal_pnt_camk_simple1));

        // direction to the salient point in the cam-k divided by distance to the feature from the first camera
        // A.21
        Eigen::Matrix<Scalar, kEucl3, 1> sal_pnt_camk_scaled = camk_orient_cfw33 * (sal_pnt_vars.inverse_dist_rho * (sal_pnt_vars.first_cam_pos_w - cam_state.pos_w) + m);
        Eigen::Matrix<Scalar, kEucl3, 1> sal_pnt_camk = sal_pnt_camk_scaled / sal_pnt_vars.inverse_dist_rho;

        sal_pnt_cam = scaled_by_inv_dist ? sal_pnt_camk_scaled : sal_pnt_camk_simple1;
#endif
    }

    return sal_pnt_cam;
}

Eigen::Matrix<Scalar, kPixPosComps,1> DavisonMonoSlam::ProjectInternalSalientPoint(const CameraStateVars& cam_state,
    const MorphableSalientPoint& sal_pnt_vars,
    SalPntProjectionIntermidVars *proj_hist) const
{
    bool scaled_by_inv_dist = true;  // projection must handle points in infinity
    std::optional<Eigen::Matrix<Scalar, kEucl3, 1>> sal_pnt_cam_opt = InternalSalientPointToCamera(sal_pnt_vars, cam_state, scaled_by_inv_dist, proj_hist);
    SRK_ASSERT(sal_pnt_cam_opt.has_value());
    
    Eigen::Matrix<Scalar, kEucl3, 1> sal_pnt_cam = sal_pnt_cam_opt.value();
    Eigen::Matrix<Scalar, kPixPosComps, 1> hd = ProjectCameraSalientPoint(sal_pnt_cam, proj_hist);
    return hd;
}

Eigen::Matrix<Scalar, kPixPosComps,1> DavisonMonoSlam::ProjectCameraSalientPoint(
    const Eigen::Matrix<Scalar, kEucl3, 1>& pnt_camera,
    SalPntProjectionIntermidVars *proj_hist) const
{
    Scalar Cx = cam_intrinsics_.principal_point_pix[0];
    Scalar Cy = cam_intrinsics_.principal_point_pix[1];
    std::array<Scalar, 2> f_pix = cam_intrinsics_.FocalLengthPix();

    // hc(X,Y,Z)->hu(uu,vu): project 3D salient point in camera-k onto image (pixels)
    //const auto& sal_pnt_cam = sal_pnt_camk_scaled;
    const auto& sal_pnt_cam = pnt_camera;
    
    Eigen::Matrix<Scalar, kPixPosComps, 1> hu;
    hu[0] = Cx - f_pix[0] * sal_pnt_cam[0] / sal_pnt_cam[2];
    hu[1] = Cy - f_pix[1] * sal_pnt_cam[1] / sal_pnt_cam[2];

    // hu->hd: distort image coordinates
    Scalar ru = std::sqrt(
        suriko::Sqr(cam_intrinsics_.pixel_size_mm[0] * (hu[0] - Cx)) +
        suriko::Sqr(cam_intrinsics_.pixel_size_mm[1] * (hu[1] - Cy)));
    
    // solve polynomial fun(rd)=k2*rd^5+k1*rd^3+rd-ru=0
    Scalar rd = ru; // TODO:

    Eigen::Matrix<Scalar, kPixPosComps, 1> hd;
    hd[0] = hu[0]; // TODO: not impl
    hd[1] = hu[1];
    
    if (proj_hist != nullptr)
    {
        proj_hist->hc = sal_pnt_cam;
    }
    return hd;
}

suriko::Point2f DavisonMonoSlam::ProjectCameraPoint(const suriko::Point3& pnt_camera) const
{
    suriko::Point2f result{};
    result.Mat() = ProjectCameraSalientPoint(pnt_camera.Mat(), nullptr);
    return result;
}

RotatedEllipse2D DavisonMonoSlam::ApproxProjectEllipsoidOnCameraByBeaconPoints(const Ellipsoid3DWithCenter& ellipsoid,
    const CameraStateVars& cam_state) const
{
    RotatedEllipsoid3D rot_ellipsoid = GetRotatedEllipsoid(ellipsoid);
    return ApproxProjectEllipsoidOnCameraByBeaconPoints(rot_ellipsoid, cam_state);
}

RotatedEllipse2D DavisonMonoSlam::ApproxProjectEllipsoidOnCameraByBeaconPoints(const RotatedEllipsoid3D& rot_ellipsoid,
    const CameraStateVars& cam_state) const
{
    Eigen::Matrix<Scalar, kEucl3, kEucl3> cam_wfc;
    RotMatFromQuat(Span(cam_state.orientation_wfc), &cam_wfc);

    SE3Transform se3_cam_wfc{ cam_wfc, cam_state.pos_w };
    SE3Transform se3_cam_cfw = SE3Inv(se3_cam_wfc);

    // approximate ellipsoid projection by projection of couple of points

    auto& mono_slam = *this;
    auto project_on_cam = [&rot_ellipsoid, &se3_cam_cfw, &mono_slam](suriko::Point3 p_ellipse) ->suriko::Point2f
    {
        suriko::Point3 p_tracker = SE3Apply(rot_ellipsoid.world_from_ellipse, p_ellipse);
        suriko::Point3 p_cam = SE3Apply(se3_cam_cfw, p_tracker);
        
        // project point onto camera plane Z=1, but do NOT convert into image pixel coordinates
        suriko::Point2f result{ p_cam[0] / p_cam[2], p_cam[1] / p_cam[2] };
        return result;
    };

    std::array<suriko::Point3, 6> beacon_points = {
        suriko::Point3 {rot_ellipsoid.semi_axes[0], 0, 0},
        suriko::Point3 {0, rot_ellipsoid.semi_axes[1], 0},
        suriko::Point3 {-rot_ellipsoid.semi_axes[0], 0, 0},
        suriko::Point3 {0, -rot_ellipsoid.semi_axes[1], 0},
        suriko::Point3 {0, 0, rot_ellipsoid.semi_axes[2]},
        suriko::Point3 {0, 0, -rot_ellipsoid.semi_axes[2]}
    };

    // for now, just form a circle which encompass all projected points
    suriko::Point2f ellip_center = project_on_cam(suriko::Point3{ 0, 0, 0 });
    Scalar rad = 0;

    for (const auto& p : beacon_points)
    {
        suriko::Point2f corner = project_on_cam(p);
        Scalar dist = (corner.Mat() - ellip_center.Mat()).norm();
        rad = std::max(rad, dist);
    }

    RotatedEllipse2D result;
    result.semi_axes = Eigen::Matrix<Scalar, 2, 1>{ rad, rad };
    // axes of circle are always may be treated as aligned with the world axes
    result.world_from_ellipse = SE2Transform(Eigen::Matrix<Scalar, 2, 2>::Identity(), ellip_center.Mat());
    return result;
}

RotatedEllipse2D DavisonMonoSlam::ProjectEllipsoidOnCameraOrApprox(const RotatedEllipsoid3D& rot_ellip, const CameraStateVars& cam_state,
    int* impl_with) const
{
    Eigen::Matrix<Scalar, 3, 3> cam_wfc;
    RotMatFromQuat(gsl::span<const Scalar>(cam_state.orientation_wfc.data(), 4), &cam_wfc);

    Eigen::Matrix<Scalar, 3, 1> eye = cam_state.pos_w;
    Eigen::Matrix<Scalar, 3, 1> u_world = cam_wfc.leftCols<1>();
    Eigen::Matrix<Scalar, 3, 1> v_world = cam_wfc.middleCols<1>(1);

    // convert camera plane n*x=lam into world coordinates

    // n_world=Rwc*n_cam=col<3rd>(Rwc), because n_cam=OZ=(0 0 1)
    Eigen::Matrix<Scalar, 3, 1> n_world = cam_wfc.rightCols<1>();

    Scalar lam_cam = suriko::kCamPlaneZ;
    Scalar lam_world = cam_state.pos_w.transpose() * n_world + lam_cam;

    // check if eye is inside the ellipsoid
    SE3Transform ellip_from_world = SE3Inv(rot_ellip.world_from_ellipse);
    suriko::Point3 eye_in_ellip = SE3Apply(ellip_from_world, eye);
    Scalar ellip_pnt =
        suriko::Sqr(eye_in_ellip[0]) / suriko::Sqr(rot_ellip.semi_axes[0]) +
        suriko::Sqr(eye_in_ellip[1]) / suriko::Sqr(rot_ellip.semi_axes[1]) +
        suriko::Sqr(eye_in_ellip[2]) / suriko::Sqr(rot_ellip.semi_axes[2]);
    bool eye_inside_ellip = ellip_pnt <= RotatedEllipsoid3D::kRightSide;

    Ellipse2DWithCenter ellipse_on_cam_plane;
    bool found_ellipse_with_center = false;

    int impl = -1;

    if (eye_inside_ellip)
    {
        bool op2 = IntersectRotEllipsoidAndPlane(rot_ellip, eye, u_world, v_world, n_world, lam_world, &ellipse_on_cam_plane);
        if (op2)
        {
            found_ellipse_with_center = true;
            impl = 2;
        }
        else
        {
            // Eye _inside_ the uncertainty ellipsoid, behind the camera plane. Ellipsoid is so short, that it doesn't cross the camera plane.
            // This feature should not be tracked.
            // for now, return approximate ellipse
        }
    }
    else
    {
        bool op = ProjectEllipsoidOnCamera(rot_ellip, eye, u_world, v_world, n_world, lam_world, &ellipse_on_cam_plane);
        if (op)
        {
            found_ellipse_with_center = true;
            impl = 1;
        }
    }

    RotatedEllipse2D result_ellipse;
    if (found_ellipse_with_center)
    {
        result_ellipse = GetRotatedEllipse2D(ellipse_on_cam_plane);
    }
    else
    {
        result_ellipse = ApproxProjectEllipsoidOnCameraByBeaconPoints(rot_ellip, cam_state);
        impl = 3;
    }

    SRK_ASSERT(impl != -1);
    if (impl_with != nullptr)
        *impl_with = impl;

    return result_ellipse;
}

void DavisonMonoSlam::Deriv_hd_by_cam_state_and_sal_pnt(
    const EigenDynVec& derive_at_pnt,
    const CameraStateVars& cam_state,
    const Eigen::Matrix<Scalar, kEucl3, kEucl3>& cam_orient_wfc,
    const TrackedSalientPoint& sal_pnt,
    const MorphableSalientPoint& sal_pnt_vars,
    Eigen::Matrix<Scalar, kPixPosComps, kCamStateComps>* hd_by_cam_state,
    Eigen::Matrix<Scalar, kPixPosComps, kSalientPointComps>* hd_by_sal_pnt,
    Eigen::Matrix<Scalar, kPixPosComps, 1>* hd) const
{
    // project salient point into current camera
    SalPntProjectionIntermidVars proj_hist{};
    Eigen::Matrix<Scalar, kPixPosComps, 1> h_distorted = ProjectInternalSalientPoint(cam_state, sal_pnt_vars, &proj_hist);

    if (hd != nullptr)
        *hd = h_distorted;

    // calculate derivatives

    // distorted pixels' coordinates depend on undistorted pixels' coordinates
    Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps> hd_by_hu;
    Point2f corner_pix = suriko::Point2f{ h_distorted[0], h_distorted[1] };
    Deriv_hd_by_hu(corner_pix, &hd_by_hu);

    // A.34 how undistorted pixels coordinates hu=[uu,vu] depend on salient point (in camera) 3D meter coordinates [hcx,hcy,hcz] (A.23)
    Eigen::Matrix<Scalar, kPixPosComps, kEucl3> hu_by_hc;
    Deriv_hu_by_hc(proj_hist, &hu_by_hc);

    Deriv_hd_by_camera_state(sal_pnt_vars, cam_state, cam_orient_wfc, proj_hist, hd_by_hu, hu_by_hc, hd_by_cam_state);

    Deriv_hd_by_sal_pnt(sal_pnt_vars, cam_state, cam_orient_wfc, hd_by_hu, hu_by_hc, hd_by_sal_pnt);

    static bool debug_corner_coord_derivatives = false;
    if (debug_corner_coord_derivatives)
    {
        Eigen::Matrix<Scalar, kPixPosComps, kCamStateComps> finite_diff_hd_by_xc;
        FiniteDiff_hd_by_camera_state(derive_at_pnt, sal_pnt_vars, kFiniteDiffEpsDebug, &finite_diff_hd_by_xc);

        Scalar diff1 = (finite_diff_hd_by_xc - *hd_by_cam_state).norm();

        Eigen::Matrix<Scalar, kPixPosComps, kSalientPointComps> finite_diff_hd_by_y;
        FiniteDiff_hd_by_sal_pnt_state(cam_state, sal_pnt, derive_at_pnt, kFiniteDiffEpsDebug, &finite_diff_hd_by_y);

        Scalar diff2 = (finite_diff_hd_by_y - *hd_by_sal_pnt).norm();
        SRK_ASSERT(true);
    }
}

void DavisonMonoSlam::Deriv_H_by_estim_vars(const CameraStateVars& cam_state,
    const Eigen::Matrix<Scalar, kEucl3, kEucl3>& cam_orient_wfc,
    const EigenDynVec& derive_at_pnt,
    EigenDynMat* H_by_estim_vars) const
{
    EigenDynMat& H = *H_by_estim_vars;

    size_t n = EstimatedVarsCount();
    size_t matched_corners = latest_frame_sal_pnts_.size();
    H.resize(kPixPosComps * matched_corners, n);
    H.setZero();

    //
    size_t obs_sal_pnt_ind = -1;
    for (SalPntId obs_sal_pnt_id : latest_frame_sal_pnts_)
    {
        MarkOrderingOfObservedSalientPoints();
        ++obs_sal_pnt_ind;

        const TrackedSalientPoint& sal_pnt = GetSalientPoint(obs_sal_pnt_id);

        MorphableSalientPoint sal_pnt_vars;
        size_t off = sal_pnt.estim_vars_ind;
        LoadSalientPointDataFromArray(Span(derive_at_pnt).subspan(off, kSalientPointComps), &sal_pnt_vars);

        Eigen::Matrix<Scalar, kPixPosComps, kCamStateComps> hd_by_cam_state;
        Eigen::Matrix<Scalar, kPixPosComps, kSalientPointComps> hd_by_sal_pnt;
        Deriv_hd_by_cam_state_and_sal_pnt(derive_at_pnt, cam_state, cam_orient_wfc, sal_pnt, sal_pnt_vars, &hd_by_cam_state, &hd_by_sal_pnt);

        //
        Eigen::Matrix<Scalar, kPixPosComps, Eigen::Dynamic> Hrowblock;
        Hrowblock.resize(Eigen::NoChange, n);
        Hrowblock.setZero();

        // by camera variables
        Hrowblock.middleCols<kCamStateComps>(0) = hd_by_cam_state;

        // by salient point variables
        // observed corner position (hd) depends only on the position of corresponding salient point (and not on any other salient point)
        Hrowblock.middleCols<kSalientPointComps>(off) = hd_by_sal_pnt;

        H.middleRows<kPixPosComps>(obs_sal_pnt_ind*kPixPosComps) = Hrowblock;
    }
}

void DavisonMonoSlam::FiniteDiff_hd_by_camera_state(const EigenDynVec& derive_at_pnt, 
    const MorphableSalientPoint& sal_pnt,
    Scalar finite_diff_eps,
    Eigen::Matrix<Scalar, kPixPosComps, kCamStateComps>* hd_by_xc) const
{
    for (size_t var_ind = 0; var_ind < kCamStateComps; ++var_ind)
    {
        // copy cam_state
        Eigen::Matrix<Scalar, kCamStateComps, 1> cam_state = derive_at_pnt.topRows<kCamStateComps>();
        cam_state[var_ind] += finite_diff_eps;

        CameraStateVars cam_state_right;
        LoadCameraStateVarsFromArray(gsl::make_span<const Scalar>(cam_state.data(), kCamStateComps), &cam_state_right);

        Eigen::Matrix<Scalar, kPixPosComps, 1> hd_right = ProjectInternalSalientPoint(cam_state_right, sal_pnt, nullptr);
        
        //
        cam_state[var_ind] -= 2 * finite_diff_eps;

        CameraStateVars cam_state_left;
        LoadCameraStateVarsFromArray(gsl::make_span<const Scalar>(cam_state.data(), kCamStateComps), &cam_state_left);

        Eigen::Matrix<Scalar, kPixPosComps, 1> hd_left = ProjectInternalSalientPoint(cam_state_left, sal_pnt, nullptr);
        hd_by_xc->middleCols<1>(var_ind) = (hd_right - hd_left) / (2 * finite_diff_eps);
    }
}

void DavisonMonoSlam::FiniteDiff_hd_by_sal_pnt_state(const CameraStateVars& cam_state, 
    const TrackedSalientPoint& sal_pnt,
    const EigenDynVec& derive_at_pnt,
    Scalar finite_diff_eps,
    Eigen::Matrix<Scalar, kPixPosComps, kSalientPointComps>* hd_by_y) const
{
    size_t off = sal_pnt.estim_vars_ind;
    for (size_t var_ind = 0; var_ind < kSalientPointComps; ++var_ind)
    {
        // copy cam_state
        Eigen::Matrix<Scalar, kSalientPointComps, 1> sal_pnt_state = derive_at_pnt.middleRows<kSalientPointComps>(off);
        sal_pnt_state[var_ind] += finite_diff_eps;

        MorphableSalientPoint sal_pnt_right;
        LoadSalientPointDataFromArray(gsl::make_span<const Scalar>(sal_pnt_state.data(), kSalientPointComps), &sal_pnt_right);

        Eigen::Matrix<Scalar, kPixPosComps, 1> hd_right = ProjectInternalSalientPoint(cam_state, sal_pnt_right, nullptr);
        
        //
        sal_pnt_state[var_ind] -= 2 * finite_diff_eps;

        MorphableSalientPoint sal_pnt_left;
        LoadSalientPointDataFromArray(gsl::make_span<const Scalar>(sal_pnt_state.data(), kSalientPointComps), &sal_pnt_left);

        Eigen::Matrix<Scalar, kPixPosComps, 1> hd_left = ProjectInternalSalientPoint(cam_state, sal_pnt_left, nullptr);
        hd_by_y->middleCols<1>(var_ind) = (hd_right - hd_left) / (2 * finite_diff_eps);
    }
}

size_t DavisonMonoSlam::SalientPointsCount() const
{
    return sal_pnts_.size();
}

const std::set<SalPntId>& DavisonMonoSlam::GetSalientPoints() const
{
    return sal_pnts_as_ids_;
}

size_t DavisonMonoSlam::EstimatedVarsCount() const
{
    return estim_vars_.size();
}

void DavisonMonoSlam::Deriv_cam_state_by_cam_state(Eigen::Matrix<Scalar, kCamStateComps, kCamStateComps>* result) const
{
    Scalar dT = between_frames_period_;

    auto& m = *result;
    m.setIdentity();

    auto id33 = Eigen::Matrix<Scalar, kEucl3, kEucl3>::Identity();

    // derivative of speed v by speed v
    DependsOnCameraPosPackOrder();
    m.block<kEucl3, kEucl3>(0, kEucl3 + kQuat4) = id33 * dT;

    // derivative of qk+1 with respect to qk

    Eigen::Matrix<Scalar, kAngVelocComps, 1> w = EstimVarsCamAngularVelocity();
    Eigen::Matrix<Scalar, kAngVelocComps, 1> delta_orient = w * dT;

    Eigen::Matrix<Scalar, kQuat4, 1> q1;
    QuatFromAxisAngle(delta_orient, &q1);

    // formula A.12
    Eigen::Matrix<Scalar, kQuat4, kQuat4> q3_by_q2;
    q3_by_q2 <<
        q1[0], -q1[1], -q1[2], -q1[3],
        q1[1],  q1[0],  q1[3], -q1[2],
        q1[2], -q1[3],  q1[0],  q1[1],
        q1[3],  q1[2], -q1[1],  q1[0];
    m.block<kQuat4, kQuat4>(kEucl3, kEucl3) = q3_by_q2;

    //
    Eigen::Matrix<Scalar, kQuat4, kAngVelocComps> q3_by_w;
    Deriv_q3_by_w(dT, &q3_by_w);

    m.block<kQuat4, kAngVelocComps>(kEucl3, kEucl3 + kQuat4 +  kVelocComps) = q3_by_w;
    SRK_ASSERT(m.allFinite());
}

void DavisonMonoSlam::FiniteDiff_cam_state_by_cam_state(gsl::span<const Scalar> cam_state, Scalar finite_diff_eps,
    Eigen::Matrix<Scalar, kCamStateComps, kCamStateComps>* result) const
{
    for (size_t var_ind = 0; var_ind < kCamStateComps; ++var_ind)
    {
        Eigen::Matrix<Scalar, kCamStateComps, 1> mut_state = Eigen::Map<const Eigen::Matrix<Scalar, kCamStateComps, 1>>(cam_state.data()); // copy state
        mut_state[var_ind] += finite_diff_eps;

        CameraStateVars cam_right;
        auto cam_right_array = Span(mut_state);
        LoadCameraStateVarsFromArray(cam_right_array, &cam_right);

        // note, we do  not normalize quaternion after applying motion model because the finite difference increment which
        // has been applied to the state vector, breaks unity of quaternions. Then normalization of a quaternion propagates
        // modifications to other it's components. This diverges the finite difference result from close form of a derivative.
        Eigen::Matrix<Scalar, kCamStateComps, 1> value_right;
        PredictCameraMotionByKinematicModel(cam_right_array, Span(value_right), nullptr);

        //
        mut_state[var_ind] -= 2 * finite_diff_eps;

        CameraStateVars cam_left;
        auto cam_left_array = Span(mut_state);
        LoadCameraStateVarsFromArray(cam_left_array, &cam_left);

        Eigen::Matrix<Scalar, kCamStateComps, 1> value_left;
        PredictCameraMotionByKinematicModel(cam_left_array,  Span(value_left), nullptr);

        Eigen::Matrix<Scalar, kCamStateComps, 1> col_diff = value_right -value_left;
        Eigen::Matrix<Scalar, kCamStateComps, 1> col = col_diff / (2 * finite_diff_eps);
        result->middleCols<1>(var_ind) = col;
    }
}

void DavisonMonoSlam::Deriv_cam_state_by_process_noise(Eigen::Matrix<Scalar, kCamStateComps, kProcessNoiseComps>* result) const
{
    Scalar dT = between_frames_period_;

    auto& m = *result;
    m.setZero();
    const auto id3x3 = Eigen::Matrix<Scalar, kEucl3, kEucl3>::Identity();
    m.block<kEucl3, kEucl3>(0, 0) = dT * id3x3;
    m.block<kEucl3, kEucl3>(kEucl3 + kQuat4, 0) = id3x3;
    m.block<kEucl3, kEucl3>(kEucl3 + kQuat4 + kVelocComps, kEucl3) = id3x3;

    // derivative of q3 with respect to capital omega is the same as the little omega
    // because in A.9 small omega and capital omega are interchangable
    Eigen::Matrix<Scalar, kQuat4, kAngVelocComps> q3_by_cap_omega;
    Deriv_q3_by_w(dT, &q3_by_cap_omega);

    m.block<kQuat4, kAngVelocComps>(kEucl3, kEucl3) = q3_by_cap_omega;
    SRK_ASSERT(m.allFinite());
}

void DavisonMonoSlam::FiniteDiff_cam_state_by_process_noise(Scalar finite_diff_eps,
    Eigen::Matrix<Scalar, kCamStateComps, kProcessNoiseComps>* result) const
{
    typedef Eigen::Matrix<Scalar, kProcessNoiseComps, 1> NoiseVec;
    const NoiseVec noise = NoiseVec::Zero(); // calculate finite differences at zero noise

    gsl::span<const Scalar> cam_state_array = gsl::make_span<const Scalar>(estim_vars_.data(), kCamStateComps);

    for (size_t var_ind = 0; var_ind < kProcessNoiseComps; ++var_ind)
    {
        // copy state
        NoiseVec mut_state = Eigen::Map<const NoiseVec>(noise.data());
        mut_state[var_ind] += finite_diff_eps;

        Eigen::Matrix<Scalar, kCamStateComps, 1> value_right;
        PredictCameraMotionByKinematicModel(cam_state_array, Span(value_right), &mut_state);

        //
        mut_state[var_ind] -= 2 * finite_diff_eps;

        Eigen::Matrix<Scalar, kCamStateComps, 1> value_left;
        PredictCameraMotionByKinematicModel(cam_state_array, Span(value_left), &mut_state);

        Eigen::Matrix<Scalar, kCamStateComps, 1> col_diff = value_right - value_left;
        Eigen::Matrix<Scalar, kCamStateComps, 1> col = col_diff / (2 * finite_diff_eps);
        result->middleCols<1>(var_ind) = col;
    }
}

void DavisonMonoSlam::Deriv_q3_by_w(Scalar deltaT, Eigen::Matrix<Scalar, kQuat4, kEucl3>* result) const
{
    Eigen::Matrix<Scalar, kQuat4, 1> q2 = EstimVarsCamQuat();

    // formula A.14
    Eigen::Matrix<Scalar, kQuat4, kQuat4> q3_by_q1;
    q3_by_q1 <<
        q2[0], -q2[1], -q2[2], -q2[3],
        q2[1], q2[0], -q2[3], q2[2],
        q2[2], q2[3], q2[0], -q2[1],
        q2[3], -q2[2], q2[1], q2[0];

    //
    Eigen::Matrix<Scalar, kQuat4, kAngVelocComps> q1_by_wk;
    Deriv_q1_by_w(deltaT, &q1_by_wk);

    // A.13
    *result = q3_by_q1 * q1_by_wk;
}

void DavisonMonoSlam::Deriv_q1_by_w(Scalar deltaT, Eigen::Matrix<Scalar, kQuat4, kEucl3>* result) const
{
    auto& q1_by_wk = *result;

    Eigen::Matrix<Scalar, kAngVelocComps, 1> w = EstimVarsCamAngularVelocity();
    Scalar len_w = w.norm();
    if (IsClose(0, len_w))
    {
        // when len(w)->0, the limit of formula A.19 gets deltaT/2 value; other values in the matrix are zero
        q1_by_wk.setZero();
        q1_by_wk(1, 0) = deltaT / 2;
        q1_by_wk(2, 1) = deltaT / 2;
        q1_by_wk(3, 2) = deltaT / 2;
        return;
    }

    Scalar c = std::cos(0.5f*len_w*deltaT);
    Scalar s = std::sin(0.5f*len_w*deltaT);

    // top row
    for (size_t i = 0; i < kAngVelocComps; ++i)
    {
        q1_by_wk(0, i) = -0.5f*deltaT*w[i] / len_w * s;
    }

    // next 3 rows
    for (size_t i = 0; i < kAngVelocComps; ++i)
        for (size_t j = 0; j < kAngVelocComps; ++j)
        {
            if (i == j) // on 'diagonal'
            {
                Scalar rat = w[i] / len_w;
                q1_by_wk(1 + i, i) = 0.5f*deltaT*rat*rat*c + (1 / len_w)*s*(1 - rat * rat);
            }
            else // off 'diagonal'
            {
                q1_by_wk(1 + i, j) = w[i] * w[j] / (len_w*len_w)*(0.5f*deltaT*c - (1 / len_w)*s);
            }
        }
}

void DavisonMonoSlam::LoadSalientPointDataFromArray(gsl::span<const Scalar> src, MorphableSalientPoint* result) const
{
    DependsOnSalientPointPackOrder();
    if constexpr (kSalPntRepres == SalPntComps::kXyz)
    {
#if defined(XYZ_SAL_PNT_REPRES)
        result->pos_w[0] = src[0];
        result->pos_w[1] = src[1];
        result->pos_w[2] = src[2];
#endif
    }
    else if constexpr (kSalPntRepres == SalPntComps::kSphericalFirstCamInvDist)
    {
#if defined(SPHER_SAL_PNT_REPRES)
        result->first_cam_pos_w[0] = src[0];
        result->first_cam_pos_w[1] = src[1];
        result->first_cam_pos_w[2] = src[2];
        result->azimuth_theta_w = src[3];
        result->elevation_phi_w = src[4];
        result->inverse_dist_rho = src[5];
#endif
    }
}

DavisonMonoSlam::MorphableSalientPoint DavisonMonoSlam::LoadSalientPointDataFromSrcEstimVars(const EigenDynVec& src_estim_vars, const TrackedSalientPoint& sal_pnt) const
{
    DavisonMonoSlam::MorphableSalientPoint result;
    LoadSalientPointDataFromArray(Span(src_estim_vars).subspan(sal_pnt.estim_vars_ind, kSalientPointComps), &result);
    return result;
}

void DavisonMonoSlam::SaveSalientPointDataToArray(const SphericalSalientPoint& sal_pnt_vars, gsl::span<Scalar> dst) const
{
    dst[0] = sal_pnt_vars.first_cam_pos_w[0];
    dst[1] = sal_pnt_vars.first_cam_pos_w[1];
    dst[2] = sal_pnt_vars.first_cam_pos_w[2];
    dst[3] = sal_pnt_vars.azimuth_theta_w;
    dst[4] = sal_pnt_vars.elevation_phi_w;
    dst[5] = sal_pnt_vars.inverse_dist_rho;
}

void DavisonMonoSlam::SaveSalientPointDataToArray(const MorphableSalientPoint& sal_pnt_vars, gsl::span<Scalar> dst) const
{
    DependsOnSalientPointPackOrder();
    if constexpr (kSalPntRepres == SalPntComps::kXyz)
    {
#if defined(XYZ_SAL_PNT_REPRES)
        dst[0] = sal_pnt_vars.pos_w[0];
        dst[1] = sal_pnt_vars.pos_w[1];
        dst[2] = sal_pnt_vars.pos_w[2];
#endif
    }
    else if constexpr (kSalPntRepres == SalPntComps::kSphericalFirstCamInvDist)
    {
#if defined(SPHER_SAL_PNT_REPRES)
        dst[0] = sal_pnt_vars.first_cam_pos_w[0];
        dst[1] = sal_pnt_vars.first_cam_pos_w[1];
        dst[2] = sal_pnt_vars.first_cam_pos_w[2];
        dst[3] = sal_pnt_vars.azimuth_theta_w;
        dst[4] = sal_pnt_vars.elevation_phi_w;
        dst[5] = sal_pnt_vars.inverse_dist_rho;
#endif
    }
}

void DavisonMonoSlam::SaveCameraStateToArray(const CameraStateVars& cam_state, gsl::span<Scalar> dst) const
{
    // camera position
    DependsOnCameraPosPackOrder();
    dst[0] = cam_state.pos_w[0];
    dst[1] = cam_state.pos_w[1];
    dst[2] = cam_state.pos_w[2];

    // camera orientation
    dst[3] = cam_state.orientation_wfc[0];
    dst[4] = cam_state.orientation_wfc[1];
    dst[5] = cam_state.orientation_wfc[2];
    dst[6] = cam_state.orientation_wfc[3];

    // camera velocity
    dst[7] = cam_state.velocity_w[0];
    dst[8] = cam_state.velocity_w[1];
    dst[9] = cam_state.velocity_w[2];

    // camera angular velocity
    dst[10] = cam_state.angular_velocity_c[0];
    dst[11] = cam_state.angular_velocity_c[1];
    dst[12] = cam_state.angular_velocity_c[2];
}

void DavisonMonoSlam::SaveEstimVars(const CameraStateVars& cam_state,
    const std::vector<MorphableSalientPoint>& sal_pnts,
    EigenDynVec* dst_estim_vars)
{
    auto& est = *dst_estim_vars;
    est.setZero();

    DependsOnOverallPackOrder();

    gsl::span<Scalar> dst = Span(est);
    SaveCameraStateToArray(cam_state, dst.subspan(0, kCamStateComps));

    for (size_t sal_pnt_ind = 0; sal_pnt_ind < sal_pnts.size(); ++sal_pnt_ind)
    {
        size_t sal_pnt_offset = SalientPointOffset(sal_pnt_ind);
        const auto& sal_pnt_state = sal_pnts[sal_pnt_ind];

        gsl::span<Scalar> sal_pnt_dst = dst.subspan(sal_pnt_offset, kSalientPointComps);
        SaveSalientPointDataToArray(sal_pnt_state, sal_pnt_dst);
    }
}

size_t DavisonMonoSlam::SalientPointOffset(size_t sal_pnt_ind) const
{
    DependsOnOverallPackOrder();
    return kCamStateComps + sal_pnt_ind * kSalientPointComps;
}

TrackedSalientPoint& DavisonMonoSlam::GetSalientPoint(SalPntId id)
{
    return *id.sal_pnt_internal;
}

const TrackedSalientPoint& DavisonMonoSlam::GetSalientPoint(SalPntId id) const
{
    return const_cast<DavisonMonoSlam*>(this)->GetSalientPoint(id);
}

SalPntId DavisonMonoSlam::GetSalientPointIdByOrderInEstimCovMat(size_t sal_pnt_ind) const
{
    TrackedSalientPoint *sp = sal_pnts_[sal_pnt_ind].get();
    return SalPntId{ sp };
}

void DavisonMonoSlam::CheckSalientPointsConsistency() const
{
    auto sal_pnts_count = sal_pnts_.size();
    SRK_ASSERT(sal_pnts_as_ids_.size() == sal_pnts_count);
    SRK_ASSERT(latest_frame_sal_pnts_.size() <= sal_pnts_count);

    for (size_t sal_pnt_ind = 0; sal_pnt_ind < sal_pnts_count; ++sal_pnt_ind)
    {
        const auto& p_sal_pnt = sal_pnts_[sal_pnt_ind];
        const TrackedSalientPoint& sal_pnt = *p_sal_pnt;

        // estimated state index
        SRK_ASSERT(sal_pnt_ind == sal_pnt.sal_pnt_ind);

        // id
        auto sal_pnt_id = SalPntId{ p_sal_pnt.get() };
        SalPntId sal_pnt_id_by_ind = GetSalientPointIdByOrderInEstimCovMat(sal_pnt_ind);
        SRK_ASSERT(sal_pnt_id == sal_pnt_id_by_ind);

        // offset
        size_t offset = SalientPointOffset(sal_pnt_ind);
        SRK_ASSERT(offset == sal_pnt.estim_vars_ind);
    }
}

std::optional<SalPntRectFacet> DavisonMonoSlam::ProtrudeSalientPointTemplIntoWorld(const EigenDynVec& src_estim_vars, const TrackedSalientPoint& sal_pnt) const
{
    // Let B be a plane through the salient point, orthogonal to direction from camera to this salient point.
    // Emit four rays from the camera center to four corners of the image template.
    // These rays intersect plane B in four 3D points, which is the 3D boundary for the salient point's image template.

    const MorphableSalientPoint sal_pnt_vars = LoadSalientPointDataFromSrcEstimVars(src_estim_vars, sal_pnt);

//    // the Euclidean 3D salient point will always have a 3D rectangle template;
//    // the 'inverse depth' salient point has no 3D rectangle template when point is in infinity
//    if constexpr (kSalPntRepres == SalPntComps::kFirstCamPolarInvDepth)
//    {
//#if defined(SPHER_SAL_PNT_REPRES)
//        if (IsClose(sal_pnt_vars.inverse_dist_rho, 0))
//            return std::nullopt;
//#endif
//    }

    CameraStateVars cam_state;
    LoadCameraStateVarsFromArray(Span(src_estim_vars), &cam_state);

    Eigen::Matrix<Scalar, kEucl3, kEucl3> cam_wfc;
    RotMatFromQuat(Span(cam_state.orientation_wfc), &cam_wfc);

    // the direction towards the center of the feature
    bool scaled_by_inv_dist = false;  // request 3D point
    const std::optional<Eigen::Matrix<Scalar, kEucl3, 1>> sal_pnt_cam_opt = InternalSalientPointToCamera(sal_pnt_vars, cam_state, scaled_by_inv_dist, nullptr);
    if (!sal_pnt_cam_opt.has_value())
        return std::nullopt;

    Eigen::Matrix<Scalar, kEucl3, 1> sal_pnt_cam = sal_pnt_cam_opt.value();
    Eigen::Matrix<Scalar, kEucl3, 1> sal_pnt_dir = sal_pnt_cam.normalized();
    const Scalar sal_pnt_dist = sal_pnt_cam.norm();

    SRK_ASSERT(sal_pnt.IsDetected());
    suriko::Point2i top_left_int = TemplateTopLeftInt(sal_pnt.templ_center_pix_.value());

    // select integer 2D boundary of a template on the image.
    using RealCorner = Eigen::Matrix<Scalar, 2, 1>;
    auto top_left = RealCorner{ top_left_int.x, top_left_int.y };
    auto bot_right = RealCorner{
        top_left[0] + sal_pnt_templ_size_.width,
        top_left[1] + sal_pnt_templ_size_.height
    };

    std::array< RealCorner, 4> corner_dirs;
    corner_dirs[SalPntRectFacet::kTopLeftInd] = top_left;
    corner_dirs[SalPntRectFacet::kTopRightInd] = RealCorner{ bot_right[0], top_left[1] };
    corner_dirs[SalPntRectFacet::kBotLeftInd] = RealCorner{ top_left[0], bot_right[1] };
    corner_dirs[SalPntRectFacet::kBotRightInd] = bot_right;

    SalPntRectFacet rect_3d;
    for (decltype(corner_dirs.size()) i = 0; i < corner_dirs.size(); ++i)
    {
        const RealCorner& corner = corner_dirs[i];

        Eigen::Matrix<Scalar, kEucl3, 1> corner_in_camera_dir;
        BackprojectPixelIntoCameraPlane(corner, &corner_in_camera_dir);
        corner_in_camera_dir.normalize();

        Scalar cos_ang = sal_pnt_dir.dot(corner_in_camera_dir);
        Scalar rect_vertex_dist_3D = sal_pnt_dist / cos_ang;
        
        Eigen::Matrix<Scalar, kEucl3, 1> vertex_in_camera = corner_in_camera_dir * rect_vertex_dist_3D;
        
        Eigen::Matrix<Scalar, kEucl3, 1> vertex_in_world = cam_state.pos_w + cam_wfc * vertex_in_camera;
        rect_3d.points[i] = suriko::Point3(vertex_in_world);
    }
    
    if (kSurikoDebug)
    {
        Eigen::Matrix<Scalar, 3, 1> pos;
        bool got_3d_pos = GetSalientPoint3DPosWithUncertainty(src_estim_vars, predicted_estim_vars_covar_, sal_pnt, true, &pos, nullptr);
        if (got_3d_pos)
        {
            SalPntRectFacet r = rect_3d;
            Eigen::Matrix<Scalar, 3, 1> cand1 = (r.TopLeft().Mat() + r.BotRight().Mat()) / 2;
            Eigen::Matrix<Scalar, 3, 1> cand2 = (r.TopRight().Mat() + r.BotLeft().Mat()) / 2;

            Scalar d1 = (pos - cand1).norm();
            Scalar d2 = (pos - cand2).norm();

            suriko::Point2f p0 = ProjectCameraPoint(suriko::Point3(pos));
            suriko::Point2f p1 = ProjectCameraPoint(suriko::Point3(cand1));
            suriko::Point2f p2 = ProjectCameraPoint(suriko::Point3(cand2));
            SRK_ASSERT(true);
        }
    }

    return rect_3d;
}

std::optional<SalPntRectFacet> DavisonMonoSlam::ProtrudeSalientTemplateIntoWorld(SalPntId sal_pnt_id) const
{
    const TrackedSalientPoint& sal_pnt = GetSalientPoint(sal_pnt_id);
    // unobserved salient point has no associated template of image
    if (!sal_pnt.IsDetected())
        return std::nullopt;

    const auto& src_estim_vars = estim_vars_;
    return ProtrudeSalientPointTemplIntoWorld(src_estim_vars, sal_pnt);
}

void DavisonMonoSlam::LoadCameraStateVarsFromArray(gsl::span<const Scalar> src, CameraStateVars* result) const
{
    DependsOnCameraPosPackOrder();
    CameraStateVars& c = *result;
    c.pos_w[0] = src[0];
    c.pos_w[1] = src[1];
    c.pos_w[2] = src[2];
    c.orientation_wfc[0] = src[3];
    c.orientation_wfc[1] = src[4];
    c.orientation_wfc[2] = src[5];
    c.orientation_wfc[3] = src[6];
    c.velocity_w[0] = src[7];
    c.velocity_w[1] = src[8];
    c.velocity_w[2] = src[9];
    c.angular_velocity_c[0] = src[10];
    c.angular_velocity_c[1] = src[11];
    c.angular_velocity_c[2] = src[12];
}

CameraStateVars DavisonMonoSlam::GetCameraStateVars(FilterStageType filter_step)
{
    EigenDynVec* src_estim_vars;
    EigenDynMat* src_estim_vars_covar;
    std::tie(src_estim_vars, src_estim_vars_covar) = GetFilterStage(filter_step);

    CameraStateVars result;
    LoadCameraStateVarsFromArray(gsl::make_span<const Scalar>(src_estim_vars->data(), kCamStateComps), &result);
    return result;
}

CameraStateVars DavisonMonoSlam::GetCameraStateVars(FilterStageType filter_step) const
{
    return const_cast<DavisonMonoSlam*>(this)->GetCameraStateVars(filter_step);
}

CameraStateVars DavisonMonoSlam::GetCameraEstimatedVars()
{
    return GetCameraStateVars(FilterStageType::Estimated);
}

CameraStateVars DavisonMonoSlam::GetCameraEstimatedVars() const
{
    return const_cast<DavisonMonoSlam*>(this)->GetCameraEstimatedVars();
}

CameraStateVars DavisonMonoSlam::GetCameraPredictedVars()
{
    return GetCameraStateVars(FilterStageType::Predicted);
}

void DavisonMonoSlam::GetCameraPosAndOrientationWithUncertainty(FilterStageType filter_stage,
    Eigen::Matrix<Scalar, kEucl3, 1>* cam_pos,
    Eigen::Matrix<Scalar, kEucl3, kEucl3>* cam_pos_uncert,
    Eigen::Matrix<Scalar, kQuat4, 1>* cam_orient_quat) const
{
    const EigenDynVec* p_src_estim_vars;
    const EigenDynMat* p_src_estim_vars_covar;
    std::tie(p_src_estim_vars, p_src_estim_vars_covar) = GetFilterStage(filter_stage);
    auto& src_estim_vars = *p_src_estim_vars;
    auto& src_estim_vars_covar = *p_src_estim_vars_covar;

    DependsOnCameraPosPackOrder();

    // mean of camera position
    auto& m = *cam_pos;
    m[0] = src_estim_vars[0];
    m[1] = src_estim_vars[1];
    m[2] = src_estim_vars[2];
    SRK_ASSERT(m.allFinite());

    // uncertainty of camera position
    const auto& orig_uncert = src_estim_vars_covar.block<kEucl3, kEucl3>(0, 0);

    auto& unc = *cam_pos_uncert;
    unc = orig_uncert;
    SRK_ASSERT(unc.allFinite());

    auto& q = *cam_orient_quat;
    q[0] = src_estim_vars[3];
    q[1] = src_estim_vars[4];
    q[2] = src_estim_vars[5];
    q[3] = src_estim_vars[6];
    SRK_ASSERT(q.allFinite());
}

void DavisonMonoSlam::GetCameraEstimatedPosAndOrientationWithUncertainty(
    Eigen::Matrix<Scalar, kEucl3, 1>* cam_pos,
    Eigen::Matrix<Scalar, kEucl3, kEucl3>* cam_pos_uncert,
    Eigen::Matrix<Scalar, kQuat4, 1>* cam_orient_quat) const
{
    GetCameraPosAndOrientationWithUncertainty(FilterStageType::Estimated, cam_pos, cam_pos_uncert, cam_orient_quat);
}

void DavisonMonoSlam::GetCameraEstimatedVarsUncertainty(Eigen::Matrix<Scalar, kCamStateComps, kCamStateComps>* cam_covar) const
{
    *cam_covar = estim_vars_covar_.topLeftCorner<kCamStateComps, kCamStateComps>();
}

void DavisonMonoSlam::DerivSalPnt_xyz_by_spher(const SphericalSalientPoint& sal_pnt_vars,
    Eigen::Matrix<Scalar, kXyzSalientPointComps, kSphericalSalientPointComps>* sal_pnt_3_by_6) const
{
    // 3D position (X,Y,Z) of a salient point in the world frame depends on 6x1 variables of the salient point
    // (Xf, Yf, Zf,theta-azimuth, phi-elevation,rho) and doesn't depend on pos of any camera (Xc,Yc,Zc)
    // sal_pnt(X,Y,Z)=fun(sal_pnt_6x1)
    auto& jacob = *sal_pnt_3_by_6;
    jacob.setZero();
    jacob.leftCols<kEucl3>() = Eigen::Matrix < Scalar, kEucl3, kEucl3>::Identity();

    Scalar cos_theta = std::cos(sal_pnt_vars.azimuth_theta_w);
    Scalar sin_theta = std::sin(sal_pnt_vars.azimuth_theta_w);
    Scalar cos_phi = std::cos(sal_pnt_vars.elevation_phi_w);
    Scalar sin_phi = std::sin(sal_pnt_vars.elevation_phi_w);

    SRK_ASSERT(!IsClose(0, sal_pnt_vars.inverse_dist_rho));  // fails when dist=inf and rho=0

    Scalar dist = 1 / sal_pnt_vars.inverse_dist_rho;

    // deriv of (Xc,Yc,Zc) by theta-azimuth
    jacob(0, kEucl3) = dist * cos_phi * cos_theta;
    jacob(1, kEucl3) = 0;
    jacob(2, kEucl3) = -dist * cos_phi * sin_theta;

    // deriv of (Xc,Yc,Zc) by phi-elevation
    jacob(0, kEucl3 + 1) = -dist * sin_phi * sin_theta;
    jacob(1, kEucl3 + 1) = -dist * cos_phi;
    jacob(2, kEucl3 + 1) = -dist * sin_phi * cos_theta;

    // deriv of (Xc,Yc,Zc) by rho
    Scalar dist2 = 1 / suriko::Sqr(sal_pnt_vars.inverse_dist_rho);
    jacob(0, kEucl3 + 2) = -dist2 * cos_phi * sin_theta;
    jacob(1, kEucl3 + 2) = dist2 * sin_phi;
    jacob(2, kEucl3 + 2) = -dist2 * cos_phi * cos_theta;

    SRK_ASSERT(jacob.allFinite());
}

#if defined(SPHER_SAL_PNT_REPRES)
void DavisonMonoSlam::PropagateSalPntPosUncertainty(const SphericalSalientPoint& sal_pnt_vars,
    const Eigen::Matrix<Scalar, kSphericalSalientPointComps, kSphericalSalientPointComps>& sal_pnt_covar,
    Eigen::Matrix<Scalar, kEucl3, kEucl3>* sal_pnt_pos_uncert) const
{
    // 3D position (X,Y,Z) of a salient point in the world frame depends on 6x1 variables of the salient point
    // (Xf, Yf, Zf,theta-azimuth, phi-elevation,rho) and doesn't depend on pos of any camera (Xc,Yc,Zc)
    // sal_pnt(X,Y,Z)=fun(sal_pnt_6x1)
    Eigen::Matrix<Scalar, kXyzSalientPointComps, kSphericalSalientPointComps> jacob;
    DerivSalPnt_xyz_by_spher(sal_pnt_vars, &jacob);

    // original uncertainty
    const auto& orig_uncert = sal_pnt_covar;

    // propagate uncertainty
    auto& unc = *sal_pnt_pos_uncert;
    unc = jacob * orig_uncert * jacob.transpose();
    SRK_ASSERT(unc.allFinite());
}
#endif

bool DavisonMonoSlam::GetSalientPointPositionUncertainty(
    const EigenDynMat& src_estim_vars_covar,
    const TrackedSalientPoint& sal_pnt,
    const MorphableSalientPoint& sal_pnt_vars,
    bool can_throw,
    Eigen::Matrix<Scalar, kEucl3, kEucl3>* sal_pnt_pos_covar) const
{
    Eigen::Matrix<Scalar, kSalientPointComps, kSalientPointComps> sal_pnt_covar =
        src_estim_vars_covar.block<kSalientPointComps, kSalientPointComps>(sal_pnt.estim_vars_ind, sal_pnt.estim_vars_ind);

    auto sal_pnt_covar_sym = (sal_pnt_covar + sal_pnt_covar.transpose()) / 2;
    auto sal_pnt_covar_diff = (sal_pnt_covar_sym - sal_pnt_covar).norm();

    if constexpr (kSalPntRepres == SalPntComps::kXyz)
    {
#if defined(XYZ_SAL_PNT_REPRES)
        *sal_pnt_pos_covar = sal_pnt_covar;
#endif
    }
    else if constexpr (kSalPntRepres == SalPntComps::kSphericalFirstCamInvDist)
    {
#if defined(SPHER_SAL_PNT_REPRES)
        SphericalSalientPoint spher_sal_pnt;
        CopyFrom(&spher_sal_pnt, sal_pnt_vars);
        // use first order approximation 
        PropagateSalPntPosUncertainty(spher_sal_pnt, sal_pnt_covar, sal_pnt_pos_covar);
        
        FixAlmostSymmetricMat(sal_pnt_pos_covar);

        bool op = CheckUncertCovMat(*sal_pnt_pos_covar, can_throw);
        if (!op)
        {
            if (can_throw) SRK_ASSERT(op);
            return false;
        }
#endif
    }
    return true;
}

auto DavisonMonoSlam::GetSalientPointProjected2DPosWithUncertainty(FilterStageType filter_stage, SalPntId sal_pnt_id) const
    -> MeanAndCov2D
{
    const EigenDynVec* src_estim_vars;
    const EigenDynMat* src_estim_vars_covar;
    std::tie(src_estim_vars, src_estim_vars_covar) = GetFilterStage(filter_stage);
    
    const TrackedSalientPoint& sal_pnt = GetSalientPoint(sal_pnt_id);
    
    return GetSalientPointProjected2DPosWithUncertainty(*src_estim_vars, *src_estim_vars_covar, sal_pnt);
}

auto DavisonMonoSlam::GetSalientPointProjected2DPosWithUncertainty(
    const EigenDynVec& src_estim_vars,
    const EigenDynMat& src_estim_vars_covar,
    const TrackedSalientPoint& sal_pnt) const ->MeanAndCov2D
{
    // fun(camera frame, salient point) -> pixel_coord, 13->2
    // collect 13x13 covariance matrix for
    // 3x1 rwc = camera position
    // 4x1 qwc = camera orientation
    // 6x1 yrho = salient point
    constexpr size_t kInSigmaSize = kEucl3 + kQuat4 + kSalientPointComps;
    Eigen::Matrix<Scalar, kInSigmaSize, kInSigmaSize> input_covar;

    // 1. Populate input covariance (of the camera frame (3+4) and the salient point (6))

    constexpr static size_t kRQ = kEucl3 + kQuat4;

    input_covar.topLeftCorner<kRQ, kRQ>() = src_estim_vars_covar.topLeftCorner<kRQ, kRQ>(); // cam pos and quaternion
    auto sal_pnt_cov_debug = src_estim_vars_covar.block<kSalientPointComps, kSalientPointComps>(sal_pnt.estim_vars_ind, sal_pnt.estim_vars_ind).eval();
    input_covar.bottomRightCorner<kSalientPointComps, kSalientPointComps>() =
        src_estim_vars_covar.block<kSalientPointComps, kSalientPointComps>(sal_pnt.estim_vars_ind, sal_pnt.estim_vars_ind); // salient point

    // 3x6 dr by dy
    auto dr_by_dy = src_estim_vars_covar.block<kEucl3, kSalientPointComps>(0, sal_pnt.estim_vars_ind);
    input_covar.block<kEucl3, kSalientPointComps>(0, kRQ) = dr_by_dy;
    input_covar.block<kSalientPointComps, kEucl3>(kRQ, 0) = dr_by_dy.transpose();

    // 4x6 dq by dy
    auto dq_by_dy = src_estim_vars_covar.block< kQuat4, kSalientPointComps>(kEucl3, sal_pnt.estim_vars_ind);
    input_covar.block<kQuat4, kSalientPointComps>(kEucl3, kRQ) = dq_by_dy;
    input_covar.block<kSalientPointComps, kQuat4>(kRQ, kEucl3) = dq_by_dy.transpose();

    // 2. Populate Jacobian

    CameraStateVars cam_state;
    LoadCameraStateVarsFromArray(Span(src_estim_vars, kCamStateComps), &cam_state);

    Eigen::Matrix<Scalar, kEucl3, kEucl3> cam_orient_wfc;
    RotMatFromQuat(gsl::make_span<const Scalar>(cam_state.orientation_wfc.data(), kQuat4), &cam_orient_wfc);

    MorphableSalientPoint sal_pnt_vars;
    size_t off = sal_pnt.estim_vars_ind;
    LoadSalientPointDataFromArray(Span(src_estim_vars).subspan(off, kSalientPointComps), &sal_pnt_vars);

    Eigen::Matrix<Scalar, kPixPosComps, kCamStateComps> hd_by_cam_state;
    Eigen::Matrix<Scalar, kPixPosComps, kSalientPointComps> hd_by_sal_pnt;
    Eigen::Matrix<Scalar, kPixPosComps, 1> hd;
    Deriv_hd_by_cam_state_and_sal_pnt(src_estim_vars, cam_state, cam_orient_wfc, sal_pnt, sal_pnt_vars, &hd_by_cam_state, &hd_by_sal_pnt, &hd);

    // Jacobian [2x13] of fun(camera frame, salient point) -> pixel_coord, 13->2
    Eigen::Matrix <Scalar, kPixPosComps, kInSigmaSize> J;
    J.middleCols<kEucl3>(0) = hd_by_cam_state.middleCols<kEucl3>(0);
    J.middleCols<kQuat4>(kEucl3) = hd_by_cam_state.middleCols<kQuat4>(kEucl3);
    J.middleCols<kSalientPointComps>(kEucl3 + kQuat4) = hd_by_sal_pnt;

    //
    static bool check_J = false;
    static bool use_finite_deriv_J = false;
    if (check_J)
    {
        Eigen::Matrix<Scalar, kInSigmaSize, 1> y_mean;
        y_mean.topRows<kRQ>() = src_estim_vars.topRows<kRQ>();
        y_mean.bottomRows<kEucl3>() = src_estim_vars.middleRows<kEucl3>(sal_pnt.estim_vars_ind);

        static Scalar eps = kFiniteDiffEpsDebug;
        Eigen::Matrix <Scalar, kPixPosComps, kInSigmaSize> finite_estim_J;
        for (size_t i = 0; i < kInSigmaSize; ++i)
        {
            auto y_mean1 = y_mean.eval();
            y_mean1[i] -= eps;
            CameraStateVars cam_state1;
            LoadCameraStateVarsFromArray(Span(y_mean1, kCamStateComps), &cam_state1);

            MorphableSalientPoint sal_pnt_vars1;
            LoadSalientPointDataFromArray(Span(y_mean1).subspan(kRQ, kSalientPointComps), &sal_pnt_vars1);

            Eigen::Matrix<Scalar, kPixPosComps, 1> h1 = ProjectInternalSalientPoint(cam_state1, sal_pnt_vars1, nullptr);

            //
            auto y_mean2 = y_mean.eval();
            y_mean2[i] += eps;
            CameraStateVars cam_state2;
            LoadCameraStateVarsFromArray(Span(y_mean2, kCamStateComps), &cam_state2);

            MorphableSalientPoint sal_pnt_vars2;
            LoadSalientPointDataFromArray(Span(y_mean2).subspan(kRQ, kSalientPointComps), &sal_pnt_vars2);

            Eigen::Matrix<Scalar, kPixPosComps, 1> h2 = ProjectInternalSalientPoint(cam_state2, sal_pnt_vars2, nullptr);
            finite_estim_J.middleCols<1>(i) = (h2 - h1) / (2 * eps);
        }
        
        if (use_finite_deriv_J)
            J = finite_estim_J;
    }

    // derive covariance of projected coord (in pixels)
    Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps> covar2D = J * input_covar * J.transpose();

    static bool b2 = true;
    if (b2)
        FixAlmostSymmetricMat(&covar2D);

    CheckEllipseIsExtractableFrom2DCovarMat(covar2D);

    // Pnew=J*P*Jt gives approximate 2D covariance of salient point's position uncertainty.
    // This works unsatisfactory because the size and eccentricity of the output ellipse are wrong.
    // TODO: fix 2D pos uncertainty ellipses for salient points

    //
    static bool use_simul = false;
    static bool simulate_propagation = false;
    if (simulate_propagation)
    {
        auto propag_fun = [this](const auto& in_mat, auto* out_mat) -> bool
        {
            CameraStateVars cam_state;
            gsl::span<const Scalar> cam_state_span = Span(in_mat, kCamStateComps);
            LoadCameraStateVarsFromArray(cam_state_span, &cam_state);

            MorphableSalientPoint sal_pnt_vars;
            LoadSalientPointDataFromArray(Span(in_mat).subspan(kRQ, kSalientPointComps), &sal_pnt_vars);

            Eigen::Matrix<Scalar, kPixPosComps, 1> h_distorted = ProjectInternalSalientPoint(cam_state, sal_pnt_vars, nullptr);

            auto& r = *out_mat;
            r = h_distorted;
            return true;
        };

        Eigen::Matrix<Scalar, kInSigmaSize, 1> input_mean;
        input_mean.topRows<kRQ>() = src_estim_vars.topRows<kRQ>();
        input_mean.bottomRows<kEucl3>() = src_estim_vars.middleRows<kEucl3>(sal_pnt.estim_vars_ind);

        Eigen::Matrix<Scalar, kInSigmaSize, kInSigmaSize> input_uncert = input_covar.eval();

        static size_t gen_samples_count = 100;
        static std::mt19937 gen{ 811 };
        Eigen::Matrix<Scalar, kPixPosComps, kPixPosComps> simul_uncert;
        PropagateUncertaintyUsingSimulation(input_mean, input_uncert, propag_fun, gen_samples_count, &gen, &simul_uncert);
        if (use_simul && simul_uncert.allFinite())
            covar2D = simul_uncert;
        SRK_ASSERT(true);
    }
    return MeanAndCov2D{hd, covar2D };
}

bool DavisonMonoSlam::GetSalientPoint3DPosWithUncertainty(
    const EigenDynVec& src_estim_vars,
    const EigenDynMat& src_estim_vars_covar,
    const TrackedSalientPoint& sal_pnt,
    bool can_throw,
    Eigen::Matrix<Scalar, kEucl3, 1>* pos_mean,
    Eigen::Matrix<Scalar, kEucl3, kEucl3>* pos_uncert) const
{
    MorphableSalientPoint sal_pnt_vars;
    LoadSalientPointDataFromArray(Span(src_estim_vars).subspan(sal_pnt.estim_vars_ind, kSalientPointComps), &sal_pnt_vars);

    if constexpr (kSalPntRepres == SalPntComps::kXyz)
    {
#if defined(XYZ_SAL_PNT_REPRES)
        *pos_mean = sal_pnt_vars.pos_w;
#endif
    }
    else if constexpr (kSalPntRepres == SalPntComps::kSphericalFirstCamInvDist)
    {
#if defined(SPHER_SAL_PNT_REPRES)
        Eigen::Matrix<Scalar, kEucl3, 1> m;
        CameraCoordinatesEuclidUnityDirFromPolarAngles(sal_pnt_vars.azimuth_theta_w, sal_pnt_vars.elevation_phi_w, &m[0], &m[1], &m[2]);

        if (IsClose(0, sal_pnt_vars.inverse_dist_rho))
            return false;

        // the expression below is derived from A.21 when camera position r=[0 0 0]
        // salient point in world coordinates = position of the first camera + position of the salient point in the first camera
        *pos_mean = sal_pnt_vars.first_cam_pos_w + (1 / sal_pnt_vars.inverse_dist_rho) * m;  // this fails for salient points in infinity, when dist=inf and rho=0
#endif
    }

    if (pos_uncert == nullptr)
        return true;

    bool op = GetSalientPointPositionUncertainty(src_estim_vars_covar, sal_pnt, sal_pnt_vars, can_throw, pos_uncert);
    if (!op)
    {
        if (can_throw) SRK_ASSERT(op);
        return false;
    }

    static bool simulate_propagation = false;
    static bool use_simulated_results = true;
    if (simulate_propagation)
    {
        auto propag_fun = [](const auto& in_mat, auto* out_mat) -> bool
        {
            Eigen::Matrix<Scalar, kEucl3, 1> first_cam_pos{ in_mat[0],in_mat[1],in_mat[2] };

            Scalar theta = in_mat[3];
            Scalar phi = in_mat[4];
            Scalar rho = in_mat[5];
            if (IsClose(0, rho))
                return false;

            Eigen::Matrix<Scalar, kEucl3, 1> m;
            CameraCoordinatesEuclidUnityDirFromPolarAngles(theta, phi, &m[0], &m[1], &m[2]);

            auto& r = *out_mat;
            r = first_cam_pos + (1 / rho) * m;
            return true;
        };

        Eigen::Map<const Eigen::Matrix<Scalar, kSalientPointComps, 1>> y_mean_mat(&src_estim_vars[sal_pnt.estim_vars_ind]);
        Eigen::Matrix<Scalar, kSalientPointComps, 1> y_mean = y_mean_mat;

        Eigen::Matrix<Scalar, kSalientPointComps, kSalientPointComps> orig_uncert =
            src_estim_vars_covar.block<kSalientPointComps, kSalientPointComps>(sal_pnt.estim_vars_ind, sal_pnt.estim_vars_ind);
        Eigen::Matrix<Scalar, kSalientPointComps, kSalientPointComps> y_uncert = orig_uncert.eval();

        static size_t gen_samples_count = 100000;
        static std::mt19937 gen{ 811 };
        Eigen::Matrix<Scalar, kEucl3, kEucl3> simul_uncert;
        PropagateUncertaintyUsingSimulation(y_mean, y_uncert, propag_fun, gen_samples_count, &gen, &simul_uncert);
        if (use_simulated_results)
            *pos_uncert = simul_uncert;
        SRK_ASSERT(true);
    }
    return true;
}

bool DavisonMonoSlam::CheckSalientPoint(
    const EigenDynVec& src_estim_vars,
    const EigenDynMat& src_estim_vars_covar,
    const TrackedSalientPoint& sal_pnt,
    bool can_throw) const
{
    Eigen::Matrix<Scalar, kEucl3, 1> sal_pnt_pos;
    Eigen::Matrix<Scalar, kEucl3, kEucl3> sal_pnt_pos_uncert;
    bool got_3d_pos = GetSalientPoint3DPosWithUncertainty(src_estim_vars, src_estim_vars_covar, sal_pnt, can_throw, &sal_pnt_pos, &sal_pnt_pos_uncert);
    if (!got_3d_pos)
    {
        if (can_throw) SRK_ASSERT(got_3d_pos);
        return false;
    }
    bool op = CheckUncertCovMat(sal_pnt_pos_uncert, can_throw);
    if (can_throw) SRK_ASSERT(op);
    return op;
}

auto DavisonMonoSlam::GetFilterStage(FilterStageType filter_stage)
-> std::tuple<EigenDynVec*, EigenDynMat*>
{
    switch (filter_stage)
    {
    case FilterStageType::Estimated:
        return std::make_tuple(&estim_vars_, &estim_vars_covar_);
    case FilterStageType::Predicted:
        return std::make_tuple(&predicted_estim_vars_, &predicted_estim_vars_covar_);
    }
    AssertFalse();
}

auto DavisonMonoSlam::GetFilterStage(FilterStageType filter_stage) const
-> std::tuple<const EigenDynVec*, const EigenDynMat*>
{
    return const_cast<DavisonMonoSlam*>(this)->GetFilterStage(filter_stage);
}

bool DavisonMonoSlam::GetSalientPoint3DPosWithUncertaintyHelper(FilterStageType filter_stage, SalPntId sal_pnt_id,
    Eigen::Matrix<Scalar, kEucl3, 1>* pos_mean,
    Eigen::Matrix<Scalar, kEucl3, kEucl3>* pos_uncert) const
{
    const EigenDynVec* src_estim_vars;
    const EigenDynMat* src_estim_vars_covar;
    std::tie(src_estim_vars, src_estim_vars_covar) = GetFilterStage(filter_stage);

    const TrackedSalientPoint& sal_pnt = GetSalientPoint(sal_pnt_id);

    return GetSalientPoint3DPosWithUncertainty(*src_estim_vars, *src_estim_vars_covar, sal_pnt, false, pos_mean, pos_uncert);
}

bool DavisonMonoSlam::GetSalientPointEstimated3DPosWithUncertaintyNew(SalPntId sal_pnt_id,
    Eigen::Matrix<Scalar, kEucl3, 1>* pos_mean,
    Eigen::Matrix<Scalar, kEucl3, kEucl3>* pos_uncert) const
{
    return GetSalientPoint3DPosWithUncertaintyHelper(FilterStageType::Estimated, sal_pnt_id, pos_mean, pos_uncert);
}

bool DavisonMonoSlam::GetSalientPointPredicted3DPosWithUncertaintyNew(SalPntId sal_pnt_id,
    Eigen::Matrix<Scalar, kEucl3, 1>* pos_mean,
    Eigen::Matrix<Scalar, kEucl3, kEucl3>* pos_uncert) const
{
    return GetSalientPoint3DPosWithUncertaintyHelper(FilterStageType::Predicted, sal_pnt_id, pos_mean, pos_uncert);
}

gsl::span<Scalar> DavisonMonoSlam::EstimVarsCamPosW()
{
    return gsl::make_span<Scalar>(estim_vars_.data(), kEucl3);
}

Eigen::Matrix<Scalar, kQuat4, 1> DavisonMonoSlam::EstimVarsCamQuat() const
{
    DependsOnCameraPosPackOrder();
    return estim_vars_.middleRows<kQuat4>(kEucl3);
}

Eigen::Matrix<Scalar, kAngVelocComps, 1> DavisonMonoSlam::EstimVarsCamAngularVelocity() const
{
    DependsOnCameraPosPackOrder();
    return estim_vars_.middleRows< kAngVelocComps>(kEucl3 + kQuat4 + kVelocComps);
}

void DavisonMonoSlam::SetCornersMatcher(std::unique_ptr<CornersMatcherBase> corners_matcher)
{
    corners_matcher_.swap(corners_matcher);
}

CornersMatcherBase& DavisonMonoSlam::CornersMatcher()
{
    return *corners_matcher_.get();
}

void DavisonMonoSlam::SetStatsLogger(std::unique_ptr<DavisonMonoSlamInternalsLogger> stats_logger)
{
    stats_logger_.swap(stats_logger);
}

DavisonMonoSlamInternalsLogger* DavisonMonoSlam::StatsLogger() const
{
    return stats_logger_.get();
}

std::optional<Scalar> DavisonMonoSlam::CurrentFrameReprojError(FilterStageType filter_stage) const
{
    if (latest_frame_sal_pnts_.empty()) return std::nullopt;

    // specify state the reprojection error is based on
    const auto& src_estim_vars = filter_stage == FilterStageType::Estimated ? estim_vars_ : predicted_estim_vars_;

    CameraStateVars cam_state;
    LoadCameraStateVarsFromArray(Span(src_estim_vars, kCamStateComps), &cam_state);

    Scalar err_sum = 0;
    for (SalPntId obs_sal_pnt_id : latest_frame_sal_pnts_)
    {
        const TrackedSalientPoint& sal_pnt = GetSalientPoint(obs_sal_pnt_id);

        MorphableSalientPoint sal_pnt_vars;
        LoadSalientPointDataFromArray(Span(src_estim_vars).subspan(sal_pnt.estim_vars_ind, kSalientPointComps), &sal_pnt_vars);

        Eigen::Matrix<Scalar, kPixPosComps, 1> pix = ProjectInternalSalientPoint(cam_state, sal_pnt_vars, nullptr);

        SRK_ASSERT(sal_pnt.IsDetected());
        suriko::Point2f corner_pix = sal_pnt.templ_center_pix_.value();
        Scalar err_one = (corner_pix.Mat() - pix).squaredNorm();
        err_sum += err_one;
    }

    return err_sum / latest_frame_sal_pnts_.size();
}

suriko::Point2i DavisonMonoSlam::TemplateTopLeftInt(const suriko::Point2f& center) const
{
    return suriko::TemplateTopLeftInt(center, sal_pnt_templ_size_);
}

Scalar DavisonMonoSlam::ClosestSalientPointTemplateMinDistance() const
{
    if (closest_sal_pnt_templ_min_dist_pix_.has_value())
        return closest_sal_pnt_templ_min_dist_pix_.value();

    // when two salient points touch each other, the distance between them is 2R, R='radius of a template'
    const Scalar touch_dist = std::sqrt(
        suriko::Sqr(static_cast<Scalar>(sal_pnt_templ_size_.width)) + 
        suriko::Sqr(static_cast<Scalar>(sal_pnt_templ_size_.height)));
    return touch_dist;
}

void DavisonMonoSlam::FixSymmetricMat(EigenDynMat* sym_mat) const
{
    FixAlmostSymmetricMat(sym_mat);
}

void DavisonMonoSlam::SetDebugPath(DebugPathEnum debug_path)
{
    s_debug_path_ = debug_path;
}
bool DavisonMonoSlam::DebugPath(DebugPathEnum debug_path)
{
    return (s_debug_path_ & debug_path) != DebugPathEnum::DebugNone;
}

}
