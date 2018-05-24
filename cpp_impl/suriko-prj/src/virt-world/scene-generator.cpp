#include "suriko/approx-alg.h"
#include "suriko/virt-world/scene-generator.h"

namespace suriko { namespace virt_world {

using namespace suriko::internals;

void GenerateCircleCameraShots(const suriko::Point3& circle_center, Scalar circle_radius, Scalar ascentZ, gsl::span<const Scalar> rot_angles, std::vector<SE3Transform>* inverse_orient_cams)
{
    for (gsl::span<const Scalar>::index_type ang_ind = 0; ang_ind < rot_angles.size(); ++ang_ind)
    {
        Scalar ang = rot_angles[ang_ind];

        // X is directed to the right, Y - to up
        Eigen::Matrix<Scalar, 4, 4> cam_from_world = Eigen::Matrix<Scalar, 4, 4>::Identity();

        // translate to circle center
        Eigen::Matrix<Scalar, 3, 1> shift = circle_center.Mat();

        // translate to camera position at the circumference of the circle
        Eigen::Matrix<Scalar, 3, 1> center_to_cam_pos(
            circle_radius * std::cos(ang),
            circle_radius * std::sin(ang),
            ascentZ
        );
        shift += center_to_cam_pos;

        // minus due to inverse camera orientation (conversion from world to camera)
        cam_from_world = SE3Mat(Eigen::Matrix<Scalar, 3, 1>(-shift)) * cam_from_world;

        // rotate OY around OZ so that OY points towards center in horizontal plane OZ=ascentZ
        Eigen::Matrix<Scalar, 3, 1> cam_pos_to_center_circle = -Eigen::Matrix<Scalar, 3, 1>(shift[0], shift[1], 0); // the direction towards center O
        cam_pos_to_center_circle.normalize();
        Eigen::Matrix<Scalar, 3, 1> oy(0, 1, 0);
        Scalar ang_yawOY = std::acos(oy.dot(cam_pos_to_center_circle)); // rotate OY 'towards' (parallel to XOY plane) center

        // correct sign so that OY is rotated towards center by shortest angle
        Eigen::Matrix<Scalar, 3, 1> oz(0, 0, 1);
        int ang_yawOY_sign = Sign(oy.cross(cam_pos_to_center_circle).dot(oz));
        ang_yawOY *= ang_yawOY_sign;

        cam_from_world = SE3Mat(RotMat(oz, -ang_yawOY)) * cam_from_world;

        // look down towards the center
        Scalar look_down_ang = std::atan2(center_to_cam_pos[2], Eigen::Matrix<Scalar, 3, 1>(center_to_cam_pos[0], center_to_cam_pos[1], 0).norm());

        // +pi/2 to direct not y-forward and z-up but z-forward and y-bottom
        cam_from_world = SE3Mat(RotMat(1, 0, 0, look_down_ang + (Scalar)(M_PI / 2))) * cam_from_world;
        SE3Transform RT(cam_from_world.topLeftCorner(3, 3), cam_from_world.topRightCorner(3, 1));

        // now camera is directed x-right, y-bottom, z-forward
        inverse_orient_cams->push_back(RT);
    }
}

}}