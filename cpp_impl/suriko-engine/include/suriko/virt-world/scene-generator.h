#pragma once
#include <vector>
#include <gsl/span>
#include "suriko/rt-config.h"
#include "suriko/obs-geom.h"

namespace suriko { namespace virt_world {

struct WorldBounds
{
    Scalar x_min;
    Scalar x_max;
    Scalar y_min;
    Scalar y_max;
    Scalar z_min;
    Scalar z_max;
};

void GenerateCircleCameraShots(const suriko::Point3& circle_center, Scalar circle_radius, Scalar ascentZ,
    gsl::span<const Scalar> rot_angles, std::vector<SE3Transform>* inverse_orient_cams);

void GenerateCameraShotsRightAndLeft(const WorldBounds& wb,
    const Point3& eye_offset,
    const Point3& center_offset,
    const Point3& up,
    Scalar offset_dist,
    int num_steps,
    std::vector<SE3Transform>* inverse_orient_cams);

void GenerateCameraShotsOscilateRightAndLeft(const WorldBounds& wb,
    const Point3& eye_offset,
    const Point3& center_offset,
    const Point3& up,
    Scalar max_deviation,
    int periods_count,
    int shots_per_period,
    bool const_view_dir,
    std::vector<SE3Transform>* inverse_orient_cams);

void GenerateCameraShotsRotateLeftAndRight(const WorldBounds& wb,
    const Point3& eye,
    const Point3& up,
    Scalar min_ang, Scalar max_ang,
    int periods_count,
    int shots_per_period,
    std::vector<SE3Transform>* inverse_orient_cams);

struct LookAtComponents
{
    suriko::Point3 eye;
    suriko::Point3 center;
    suriko::Point3 up;
};

void GenerateCameraShots3DPath(const WorldBounds& wb,
    const std::vector<LookAtComponents>& cam_poses, int periods_count,
    std::vector<SE3Transform>* inverse_orient_cams);

}}