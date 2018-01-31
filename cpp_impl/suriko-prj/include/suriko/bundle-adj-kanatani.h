﻿#pragma once
#include <string>
#include <array>
#include <vector>
#include <optional>
#include <cmath> // std::isnan
#include <iostream>
#include <Eigen/Dense>
#include "suriko/obs-geom.h"

namespace suriko
{

// forward declaration for friend
//template <typename Scalar> class SceneNormalizer;

auto NormalizeSceneInplace(FragmentMap* map, std::vector<SE3Transform>* inverse_orient_cams,
                                   Scalar t1y_dist, int unity_comp_ind, bool* success);

/// Performs normalization of world points , so that (R0,T0) is the identity rotation plus zero translation and T1y=1.
    /// Changes the and camera positions/orientations so that
class SceneNormalizer
{
    FragmentMap* map_ = nullptr;
    std::vector<SE3Transform>* inverse_orient_cams_ = nullptr;
    Scalar normalized_t1y_dist_ = Scalar(); // expected T1y (or T1x) distance after normalization
    int unity_comp_ind_ = 1; // index of 3-element T1(x,y,z) to normalize (0 to use T1x; 1 to use T1y)

    // store pre-normalized state
    SE3Transform prenorm_rt0_;
    Scalar world_scale_ = 0; // world, scaled with this multiplier, is transformed into normalized world

    friend auto NormalizeSceneInplace(FragmentMap* map, std::vector<SE3Transform>* inverse_orient_cams,
                                              Scalar t1y_norm, int unity_comp_ind, bool* success);

    SceneNormalizer(FragmentMap* map, std::vector<SE3Transform>* inverse_orient_cams, Scalar t1y, int unity_comp_ind);

    enum class NormalizeAction
    {
        Normalize,
        Revert
    };

    static auto Opposite(NormalizeAction action);

    static SE3Transform NormalizeOrRevertRT(const SE3Transform& inverse_orient_camk,
        const SE3Transform& inverse_orient_cam0, Scalar world_scale, NormalizeAction action, bool check_back_conv=true);

    // TODO: back conversion check can be moved to unit testing
    static suriko::Point3 NormalizeOrRevertPoint(const suriko::Point3& x3D,
        const SE3Transform& inverse_orient_cam0, Scalar world_scale, NormalizeAction action, bool check_back_conv=true);

    /// Modify structure so that it becomes 'normalized'.
    /// The structure is updated in-place, because a copy of salient points and orientations of a camera can be too expensive to make.
    bool NormalizeWorldInplaceInternal();
public:
    SceneNormalizer() = default; // TODO: fragile

    void RevertNormalization();
};

/// Normalizes the salient points and orientations of the camera so that R0=Identity, T0=zeros(3), T1[unity_comp_ind]=t1y_dist.
/// Usually unity_comp_ind=1 (that is, y-component of T1) and t1y_dist=1 (unity).
auto NormalizeSceneInplace(FragmentMap* map, std::vector<SE3Transform>* inverse_orient_cams,
        Scalar t1y_dist, int unity_comp_ind, bool* success);

bool CheckWorldIsNormalized(const std::vector<SE3Transform>& inverse_orient_cams, Scalar t1y, int unity_comp_ind,
    std::string* err_msg = nullptr);

/// Performs Bundle adjustment (BA) inplace. Iteratively shifts world points and cameras position and orientation so
/// that the reprojection error is minimized.
/// TODO: think about it: the synthetic scene, corrupted with noise, probably will not be 'repaired' (adjusted) to zero reprojection error.
/// source: "Bundle adjustment for 3-d reconstruction" Kanatani Sugaya 2010
class BundleAdjustmentKanatani
{
    FragmentMap* map_ = nullptr;
    std::vector<SE3Transform>* inverse_orient_cams_ = nullptr;
    const CornerTrackRepository* track_rep_ = nullptr;
    const Eigen::Matrix<Scalar, 3, 3>* shared_intrinsic_cam_mat_ = nullptr;
    const std::vector<Eigen::Matrix<Scalar, 3, 3>> * intrinsic_cam_mats_ = nullptr;
    SceneNormalizer scene_normalizer_;

    Scalar t1y_ = 1.0; // const, y-component of the first camera shift, usually T1y==1
    int unity_comp_ind_ = 1; // 0 for X, 1 for Y; index of T1 to be set to unity

    typedef Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic> EigenDynMat;

    // cache gradients
    std::vector<Scalar> gradE_finite_diff;
    EigenDynMat deriv_second_point_finite_diff;
    EigenDynMat deriv_second_frame_finite_diff;
    EigenDynMat deriv_second_pointframe_finite_diff;

    static const size_t kPointVarsCount = 3; // number of variables in 3D point [X,Y,Z]

    static const size_t kIntrinsicVarsCount = 4; // count({fx,fy,u0,v0})=4
    static const size_t kFxFyCount = 2; // count({fx, fy})=2
    static const size_t kU0V0Count = 2; // count({u0, v0})=2
    static const size_t kTVarsCount = 3; // count({Tx, Ty, Tz})=3
    static const size_t kWVarsCount = 3; // count({Wx, Wy, Wz})=3

    size_t frame_vars_count_ = 0; // number of variables to parameterize a camera orientation [[fx fy u0 v0] T1 T2 T3 W1 W2 W3]
    
    // maximum count of ([fx fy u0 v0 T1 T2 T3 W1 W2 W3])
    // camera intrinsics: 0 if K is shared for all frames; 3 for [f u0 v0] if fx=fy; 4 for [fx fy u0 v0]
    // hence max count of camera intrinsics variables is 4
    // camera translation: 3 for [T1 T2 T3]
    // camera axis angle: 3 for [W1 W2 W3]
    static const size_t kMaxFrameVarsCount = 10;

public:
    static Scalar ReprojError(const FragmentMap& map,
                            const std::vector<SE3Transform>& inverse_orient_cams,
                            const CornerTrackRepository& track_rep,
                            const Eigen::Matrix<Scalar, 3, 3>* shared_intrinsic_cam_mat = nullptr,
                            const std::vector<Eigen::Matrix<Scalar, 3, 3>>* intrinsic_cam_mats = nullptr);

    /// :return: True if optimization converges successfully.
    /// Stop conditions:
    /// 1) If a change of error function slows down and becomes less than self.min_err_change
    /// NOTE: There is no sense to provide absolute threshold on error function because when noise exist, the error will
    /// not get close to zero.
    bool ComputeInplace(FragmentMap& map,
                        std::vector<SE3Transform>& inverse_orient_cams,
                        const CornerTrackRepository& track_rep,
                        const Eigen::Matrix<Scalar, 3, 3>* shared_intrinsic_cam_mat = nullptr,
                        const std::vector<Eigen::Matrix<Scalar, 3, 3>>* intrinsic_cam_mats = nullptr,
                        bool check_derivatives=false);
private:
    bool ComputeOnNormalizedWorld();

    /// Computes finite difference approximation of derivatives.
    /// finitdiff_eps: finite difference step to approximate derivative
    void ComputeDerivativesFiniteDifference(Scalar finite_diff_eps,
                                            std::vector<Scalar>* gradE,
                                            EigenDynMat* deriv_second_point,
                                            EigenDynMat* deriv_second_frame,
                                            EigenDynMat* deriv_second_pointframe);

public:
    /// Represents a camera orientation data for some frame. There are packed and unpacked data, keeped in synced state.
    class FrameFromOptVarsUpdater
    {
        size_t frame_ind_;
        Eigen::Matrix<Scalar, 3, 3> cam_intrinsics_mat_;
        SE3Transform direct_orient_cam_;
        SE3Transform inverse_orient_cam_;
        bool direct_orient_cam_valid_ = false;
        bool inverse_orient_cam_valid_ = false;

        std::array<Scalar, kMaxFrameVarsCount> frame_vars_;
    public:
        FrameFromOptVarsUpdater(size_t frame_ind, const Eigen::Matrix<Scalar, 3, 3>& cam_intrinsics_mat, const SE3Transform& direct_orient_cam);
            
        void AddDelta(size_t var_ind, Scalar value);

        size_t FrameInd() const { return frame_ind_; }

        const Eigen::Matrix<Scalar, 3, 3>& CamIntrinsicsMat() const {
            return cam_intrinsics_mat_;
        }
        const SE3Transform& InverseOrientCam() {
            EnsureCameraOrientaionValid();
            return inverse_orient_cam_;
        }
    private:
        void UpdatePackedVars();
        void EnsureCameraOrientaionValid();
    };

private:
    auto GetFiniteDiffFirstPartialDerivPoint(size_t point_track_id, const suriko::Point3& pnt3D_world, size_t var1, Scalar finite_diff_eps) const -> Scalar;
    auto GetFiniteDiffSecondPartialDerivPoint(size_t point_track_id, const suriko::Point3& pnt3D_world, size_t var1, size_t var2, Scalar finite_diff_eps) const -> Scalar;
    auto GetFiniteDiffFirstPartialDerivFocalLengthFxFy(size_t frame_ind, const Eigen::Matrix<Scalar, 3, 3>& K, size_t fxfy_ind, Scalar finite_diff_eps) const -> Scalar;
    auto GetFiniteDiffFirstDerivPrincipalPoint(size_t frame_ind, const Eigen::Matrix<Scalar, 3, 3>& K, size_t u0v0_ind, Scalar finite_diff_eps) const -> Scalar;
    auto GetFiniteDiffFirstPartialDerivTranslationDirect(size_t frame_ind, const SE3Transform& direct_orient_cam, size_t tind, Scalar finite_diff_eps) const -> Scalar;
    auto GetFiniteDiffFirstPartialDerivRotation(size_t frame_ind, const SE3Transform& direct_orient_cam,
        const Eigen::Matrix<Scalar, 3, 1>& w_direct, size_t wind, Scalar finite_diff_eps) const->Scalar;

    auto GetFiniteDiffSecondPartialDerivFrameFrame(const FrameFromOptVarsUpdater& frame_vars_updater, size_t frame_var_ind1, size_t frame_var_ind2, Scalar finite_diff_eps) const->Scalar;

    auto GetFiniteDiffSecondPartialDerivPointFrame(size_t point_track_id, const suriko::Point3& pnt3D_world,
        const FrameFromOptVarsUpdater& frame_vars_updater, size_t point_var_ind, size_t frame_var_ind, Scalar finite_diff_eps) const->Scalar;

    /// Declares code dependency on the order of variables [[fx fy u0 v0] Tx Ty Tz Wx Wy Wz]
    static void MarkOptVarsOrderDependency() {}
};
}
