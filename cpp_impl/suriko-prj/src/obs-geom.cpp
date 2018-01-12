#include <cmath> // std::sqrt
#include <iostream> // std::cerr
#include <Eigen/Cholesky>
#include "suriko/approx-alg.h"
#include "suriko/obs-geom.h"

namespace suriko
{
auto ToPoint(const Eigen::Matrix<Scalar,2,1>& m) -> suriko::Point2 { return suriko::Point2(m); }
auto ToPoint(const Eigen::Matrix<Scalar,3,1>& m) -> suriko::Point3 { return suriko::Point3(m); }

auto SE3Inv(const SE3Transform& rt) -> SE3Transform {
    SE3Transform result;
    result.R = rt.R.transpose();
    result.T = - result.R * rt.T;
    return result;
}

auto SE3Apply(const SE3Transform& rt, const suriko::Point3& x) -> suriko::Point3
{
    // 0-copy
    suriko::Point3 result(0,0,0);
    result.Mat() = rt.R * x.Mat() + rt.T;
    return result;
    // 1-copy
//    Eigen::Matrix<Scalar,3,1> result= rt.R * x.Mat() + rt.T;
//    return ToPoint(result);
}

auto SE3Compose(const SE3Transform& rt1, const SE3Transform& rt2) -> suriko::SE3Transform
{
    SE3Transform result;
    result.R = rt1.R * rt2.R;
    result.T = rt1.R * rt2.T + rt1.T;
    return result;
}

auto SE3AFromB(const SE3Transform& a_from_world, const SE3Transform& b_from_world) -> suriko::SE3Transform
{
    return SE3Compose(a_from_world, SE3Inv(b_from_world));
}


void FragmentMap::AddSalientPoint(size_t point_track_id, const std::optional<suriko::Point3> &value)
{
    if (point_track_id >= salient_points.size())
        salient_points.resize(point_track_id+1);
    if (value.has_value())
        SetSalientPoint(point_track_id, value.value());
    point_track_count += 1;
}
void FragmentMap::SetSalientPoint(size_t point_track_id, const suriko::Point3 &value)
{
    assert(point_track_id < salient_points.size());
    salient_points[point_track_id] = value;
}
suriko::Point3 FragmentMap::GetSalientPoint(size_t point_track_id) const
{
    assert(point_track_id < salient_points.size());
    std::optional<suriko::Point3> sal_pnt = salient_points[point_track_id];
    SRK_ASSERT(sal_pnt.has_value());
    return sal_pnt.value();
}


bool CornerTrack::HasCorners() const {
    return StartFrameInd != -1;
}

void CornerTrack::AddCorner(size_t frame_ind, const suriko::Point2& value)
{
    if (StartFrameInd == -1)
        StartFrameInd = frame_ind;
    else
        SRK_ASSERT(frame_ind >= StartFrameInd && "Can insert points later than the initial (start) frame");

    CoordPerFramePixels.push_back(std::optional<suriko::Point2>(value));
    CheckConsistent();
}

std::optional<suriko::Point2> CornerTrack::GetCorner(size_t frame_ind) const
{
    SRK_ASSERT(StartFrameInd != -1);
    ptrdiff_t local_ind = frame_ind - StartFrameInd;
    if (local_ind < 0 || local_ind >= CoordPerFramePixels.size())
        return std::optional<suriko::Point2>();
    return CoordPerFramePixels[local_ind];
}

void CornerTrack::CheckConsistent()
{
    if (StartFrameInd != -1)
        SRK_ASSERT(!CoordPerFramePixels.empty());
    else
        SRK_ASSERT(CoordPerFramePixels.empty());
}


suriko::CornerTrack& CornerTrackRepository::GetByPointId(size_t point_id)
{
    size_t pnt_ind = point_id;
    return CornerTracks[pnt_ind];
}

void CornerTrackRepository::PopulatePointTrackIds(std::vector<size_t> *result) {
    for (size_t pnt_ind=0;pnt_ind<CornerTracks.size(); ++pnt_ind)
        result->push_back(pnt_ind);
}

bool IsSpecialOrthogonal(const Eigen::Matrix<Scalar,3,3>& R, std::string* msg) {
    Scalar rtol = 1.0e-3;
    Scalar atol = 1.0e-3;
    bool is_ident = (R.transpose() * R).isIdentity(atol);
    if (!is_ident) {
        if (msg != nullptr) {
            std::stringstream ss;
            ss << "failed Rt.R=I, R=\n" << R;
            *msg = ss.str();
        }
        return false;
    }
    Scalar rdet = R.determinant();
    bool is_one = IsClose(1, rdet, rtol, atol);
    if (!is_one) {
        if (msg != nullptr) {
            std::stringstream ss;
            ss << "failed det(R)=1, actual detR=" << rdet << " R=\n" << R;
            *msg = ss.str();
        }
        return false;
    }
    return true;
}

auto DecomposeProjMat(const Eigen::Matrix<Scalar, 3, 4> &proj_mat, bool check_post_cond)
-> std::tuple<Scalar, Eigen::Matrix<Scalar, 3, 3>, SE3Transform>
{
    using namespace Eigen;
    typedef Matrix<Scalar,3,3> Mat33;

    // copy the input, because we may flip sign later
    Mat33 Q = proj_mat.leftCols(3);
    Matrix<Scalar,3,1> q = proj_mat.rightCols(1);

    // ensure that R will have positive determinant
    int P_sign = 1;
    Scalar Q_det = Q.determinant();
    if (Q_det < 0) {
        P_sign = -1;
        Q *= -1;
        q *= -1;
    }

    // find translation T
    Mat33 Q_inv = Q.inverse();
    Matrix<Scalar,3,1> t = -Q_inv * q;

    // find rotation R
    Mat33 QQt = Q * Q.transpose();

    // QQt is inverted to allow further use Cholesky decomposition to find K
    Mat33 QQt_inv = QQt.inverse();
    LLT<Mat33> llt(QQt_inv); // Cholesky decomposition
    Mat33 C = llt.matrixL();

    // we need upper triangular matrix, but Eigen::LLT returns lower triangular
    C.transposeInPlace();

    Mat33 R = (C * Q).transpose();

    if (check_post_cond)
    {
        std::string err_msg;
        if (!IsSpecialOrthogonal(R, &err_msg))
            std::cerr <<err_msg <<std::endl;
    }

    // find intrinsic parameters K
    Mat33 C_inv = C.inverse();
    Scalar c_last = C_inv(2, 2);
    //assert not np.isclose(0, c_last), "division by zero, c_last={}".format(c_last)
    Mat33 K = C_inv * (1/c_last);

    Scalar scale_factor = P_sign * c_last;

    if (check_post_cond)
    {
        Eigen::Matrix<Scalar, 3, 4> right;
        right <<Mat33::Identity(), -t;
        Eigen::Matrix<Scalar, 3, 4> P_back = scale_factor * K * R.transpose() * right;
        auto diff = (proj_mat - P_back).norm();
        assert(diff < 1e-2 && "Failed to decompose P[3x4]->R,T,K"); // (diff={})
    }

    SE3Transform direct_orient_cam(R,t);
    return std::make_tuple(scale_factor, K, direct_orient_cam);
}

auto Triangulate3DPointByLeastSquares(const std::vector<suriko::Point2> &xs2D,
                                      const std::vector<Eigen::Matrix<Scalar,3,4>> &proj_mat_list, Scalar f0, int debug)
-> suriko::Point3
{
    size_t frames_count_P = proj_mat_list.size();
    size_t frames_count_xs = xs2D.size();
    SRK_ASSERT(frames_count_P == frames_count_xs && "Provide two lists of 2D coordinates and projection matrices of the same length");

    size_t frames_count = frames_count_P;
    SRK_ASSERT(frames_count >= 2 && "Provide 2 or more projections of a 3D point");

    // populate matrices A and B to solve for least squares
    Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic> A(frames_count * 2, 3);
    Eigen::Matrix<Scalar,Eigen::Dynamic,1> B(frames_count * 2);

    for (size_t frame_ind = 0; frame_ind < frames_count; ++frame_ind) {
        const auto &x2D = xs2D[frame_ind];
        auto x = x2D[0];
        auto y = x2D[1];
        const auto &P = proj_mat_list[frame_ind];
        A(frame_ind * 2 + 0, 0) = x * P(2, 0) - f0 * P(0, 0);
        A(frame_ind * 2 + 0, 1) = x * P(2, 1) - f0 * P(0, 1);
        A(frame_ind * 2 + 0, 2) = x * P(2, 2) - f0 * P(0, 2);
        A(frame_ind * 2 + 1, 0) = y * P(2, 0) - f0 * P(1, 0);
        A(frame_ind * 2 + 1, 1) = y * P(2, 1) - f0 * P(1, 1);
        A(frame_ind * 2 + 1, 2) = y * P(2, 2) - f0 * P(1, 2);
        B(frame_ind * 2 + 0) = -(x * P(2, 3) - f0 * P(0, 3));
        B(frame_ind * 2 + 1) = -(y * P(2, 3) - f0 * P(1, 3));
    }

#define LEAST_SQ 2
#if LEAST_SQ == 1
    const auto& jacobi_svd = A.jacobiSvd(Eigen::ComputeThinU|Eigen::ComputeThinV);
Eigen::Matrix<Scalar, Eigen::Dynamic, 1> sol = jacobi_svd.solve(B);
#elif LEAST_SQ == 2
    const auto& householder_qr = A.colPivHouseholderQr();
    Eigen::Matrix<Scalar, Eigen::Dynamic, 1> sol = householder_qr.solve(B);
#endif

    if (debug >= 4) {
        const Eigen::Matrix<Scalar,Eigen::Dynamic,1> diff_vec = A * sol - B;
        Scalar diff = diff_vec.norm();
        if (diff > 0.1) {
            std::cout << "warn: big diff=" << diff << " frames_count=" << frames_count << std::endl;
        }
    }
    suriko::Point3 x3D(sol(0), sol(1), sol(2));
    return x3D;
}
}