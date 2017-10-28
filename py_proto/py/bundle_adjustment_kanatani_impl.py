import numpy as np

from py.obs_geom import *


class BundleAdjustmentKanatani:
    """
    Performs Bundle adjustment (BA) inplace.
    source: "Bundle adjustment for 3-d reconstruction" Kanatani Sugaya 2010
    """
    def __init__(self, err_fun_min_relative_change = 1e-3):
        self.points_life = None
        self.bundle_pnt_ids = None # ids of points for BA processing
        self.cam_mat_pixel_from_meter = None
        self.world_pnts = None
        self.framei_from_world_RT_list = None
        self.elem_type = None
        self.err_fun_min_relative_change = err_fun_min_relative_change
        # const
        self.T1y = 1 # const, y-component of the first camera shift, usually T1y==1
        self.unity_comp_ind = 1 # 0 for X, 1 for Y; index of T1 to be set to unity

    def BundleAdjustmentComputeInplace(self, pnt_track_list, cam_mat_pixel_from_meter, salient_points, camera_frames, check_derivatives=False, debug=False):
        self.points_life = pnt_track_list
        self.cam_mat_pixel_from_meter = cam_mat_pixel_from_meter
        self.world_pnts = salient_points
        self.framei_from_world_RT_list = camera_frames

        self.elem_type = type(salient_points[0][0])

        # select tracks (salient 3D points) to use for bundle adjustment
        # NOTE: furter the pnt_ind refers to pnt_id in the array of bundle points
        bundle_pnt_ids = []
        for pnt_life in self.points_life:
            pnt3D_world = self.world_pnts[pnt_life.track_id]
            if pnt3D_world is None: continue
            pnt_id = pnt_life.track_id
            assert not pnt_id is None, "Required point track id to identif the track"
            bundle_pnt_ids.append(pnt_id)
        self.bundle_pnt_ids = bundle_pnt_ids

        world_pnts_prenorm = self.world_pnts.copy()
        framei_from_world_RT_list_prenorm = self.framei_from_world_RT_list.copy()

        norm_params = self.BundleAdjustmentNormalizeWorld()

        result = self.BundleAdjustmentComputeOnNormalizedWorld(check_derivatives, debug)

        self.BundleAdjustmentRevertNormalization(bundle_pnt_ids, norm_params)

        check_normalization_is_reversed = True
        if False and check_normalization_is_reversed: # !!! this doesn't make sense if world is changed inplace
            x1expect = world_pnts_prenorm[1]
            x1actual = self.world_pnts[1]
            assert np.isclose(x1expect[0], x1actual[0], atol=1e-5), "Normalization side-effects must be reversed"
            t1expect = framei_from_world_RT_list_prenorm[1][1]
            t1actual = self.framei_from_world_RT_list[1][1]
            assert np.isclose(t1expect[0], t1actual[0], atol=1e-5), "Normalization side-effects must be reversed"

        return result

    def NormalizeOrRevertRT(self, rt, R0, T0, world_scale, normalize_or_revert, check_back_conv=True):
        Rk,Tk = rt
        if normalize_or_revert == True:
            newRk = Rk.dot(R0.T)
            newTk = world_scale * (Tk - Rk.dot(R0.T).dot(T0))
            result_rt = (newRk, newTk)
        else:
            revertRk = Rk.dot(R0)
            Tktmp = Tk / world_scale
            revertTk = Tktmp + Rk.dot(T0)
            result_rt = (revertRk, revertTk)

        if check_back_conv:
            back_rt = self.NormalizeOrRevertRT(result_rt, R0, T0, world_scale, not normalize_or_revert, check_back_conv=False)
            assert np.allclose(rt[0], back_rt[0], atol=1e-3), "Error in normalization or reverting"
            assert np.allclose(rt[1], back_rt[1], atol=1e-3), "Error in normalization or reverting"
        return result_rt

    def NormalizeOrRevertPoint(self, X3D, R0, T0, world_scale, normalize_or_revert, check_back_conv=True):
        if normalize_or_revert == True:
            newX = R0.dot(X3D) + T0
            newX *= world_scale
            result_x = newX
        else:
            X3Dtmp = X3D / world_scale
            result_x = R0.T.dot(X3Dtmp - T0)

        if check_back_conv:
            back_x = self.NormalizeOrRevertPoint(result_x, R0, T0, world_scale, not normalize_or_revert, check_back_conv=False)
            assert np.allclose(X3D, back_x, atol=1e-3), "Error in normalization or reverting"
        return result_x

    # unity_comp_ind component of T1 to make unity [0,1,2] for [T1X,T1Y,T1Z]
    def BundleAdjustmentNormalizeWorld(self):
        check_post_cond = True
        if check_post_cond:
            framei_from_world_RT_list_prenorm = self.framei_from_world_RT_list.copy()
            world_pnts_prenorm = self.world_pnts.copy()

        pnt_id0 = self.bundle_pnt_ids[0]
        pnt_id1 = self.bundle_pnt_ids[1]

        R0,T0 = self.framei_from_world_RT_list[pnt_id0]
        R1,T1 = self.framei_from_world_RT_list[pnt_id1]

        R0, T0 = R0.copy(), T0.copy() # initial values of R0,T0

        # translation vector from frame0 to frame1
        initial_camera_shift = T0 - R0.dot(R1.T).dot(T1)

        # make y-component of the first camera shift a unity (formula 27) T1y==1
        world_scale = self.T1y / initial_camera_shift[self.unity_comp_ind]

        for pnt_id in self.bundle_pnt_ids:
            pnt_ind = pnt_id
            X3D = self.world_pnts[pnt_ind]
            newX = self.NormalizeOrRevertPoint(X3D, R0, T0, world_scale, normalize_or_revert=True)
            self.world_pnts[pnt_ind] = newX

        for frame_ind in range(0, len(self.framei_from_world_RT_list)):
            rt = self.framei_from_world_RT_list[frame_ind]
            new_rt = self.NormalizeOrRevertRT(rt, R0, T0, world_scale, normalize_or_revert=True)
            self.framei_from_world_RT_list[frame_ind] = new_rt

        if check_post_cond:
            pmsg = ['']
            assert self.CheckWorldIsNormalized(pmsg), pmsg[0]

        norm_params = (R0, T0, world_scale)
        return norm_params

    def CheckWorldIsNormalized(self, msg):
        pnt_id0 = self.bundle_pnt_ids[0]
        pnt_id1 = self.bundle_pnt_ids[1]

        # the first frame is the identity
        rt0 = self.framei_from_world_RT_list[pnt_id0]
        if not np.allclose(np.identity(3), rt0[0], atol=1e-3):
            msg[0] = "R0=Identity"
            return False
        if not np.allclose(np.zeros(3), rt0[1], atol=1e-3):
            msg[0] = "T0=zeros(3)"
            return False

        # the second frame has translation of unity length
        t2 = SE3Inv(self.framei_from_world_RT_list[pnt_id1])[1]
        if not np.allclose(self.T1y, t2[1], atol=1e-3):
            msg[0] = "expected T1y=1 but T1 was {}".format(t2)
            return False
        return True

    def BundleAdjustmentRevertNormalization(self, bundle_pnt_ids, norm_params):
        R0, T0, world_scale = norm_params
        # revert unity transformation
        for pnt_id in bundle_pnt_ids:
            pnt_ind = pnt_id
            X3D = self.world_pnts[pnt_ind]
            revertX = self.NormalizeOrRevertPoint(X3D, R0, T0, world_scale, normalize_or_revert=False)
            self.world_pnts[pnt_ind] = revertX

        for frame_ind in range(0, len(self.framei_from_world_RT_list)):
            rt = self.framei_from_world_RT_list[frame_ind]
            revert_rt = self.NormalizeOrRevertRT(rt, R0, T0, world_scale, normalize_or_revert=False)
            self.framei_from_world_RT_list[frame_ind] = revert_rt

    def BundleAdjustmentComputeOnNormalizedWorld(self, check_derivatives, debug):
        result = self.BundleAdjustmentComputeOnNormalizedWorldCore(check_derivatives, debug)
        # check world is still normalized after optimization
        pmsg = ['']
        assert self.CheckWorldIsNormalized(pmsg), pmsg[0]
        return result

    def BundleAdjustmentComputeOnNormalizedWorldCore(self, check_derivatives, debug):
        el_type = self.elem_type
        frames_count = len(self.framei_from_world_RT_list)
        points_count = len(self.bundle_pnt_ids)

        # +3 for R0=Identity[3x3]
        # +3 for T0=[0 0 0]
        # +1 for T1y=1
        KNOWN_FRAME_VARS_COUNT = 7

        check_data_is_normalized = False
        if check_data_is_normalized:
            R0,T0 = self.framei_from_world_RT_list[0]
            assert np.isclose(R0[0,0], 1)
            assert np.isclose(T0[0], 0)
            R1, T1 = self.framei_from_world_RT_list[1]
            assert np.isclose(T1[self.unity_comp_ind], 1)

        # derivatives data
        gradE  = np.zeros(3*points_count + 6*frames_count, dtype=el_type) # derivative of vars
        gradE2_onlyW = np.zeros(3*points_count + 6*frames_count, dtype=el_type)
        deriv_second_point = np.zeros((3*points_count, 3), dtype=el_type) # rows=[X1 Y1 Z1 X2 Y2 Z2...] cols=[X Y Z]
        deriv_second_frame = np.zeros((6*frames_count, 6), dtype=el_type) # rows=[T1 T2 T3 W1 W2 W3...for each frame] cols=[T1 T2 T3 W1 W2 W3]
        # rows=[X1 Y1 Z1 X2 Y2 Z2...for each point] cols=[T1 T2 T3 W1 W2 W3...for each frame]
        deriv_second_pointframe = np.zeros((3*points_count, 6*frames_count), dtype=el_type)
        corrections = np.zeros(3 * points_count + 6 * frames_count, dtype=el_type) # corrections of vars

        # data to solve linear equations
        left_side1 = np.zeros((6 * frames_count - KNOWN_FRAME_VARS_COUNT, 6 * frames_count - KNOWN_FRAME_VARS_COUNT), dtype=el_type)
        right_side = np.zeros(6 * frames_count - KNOWN_FRAME_VARS_COUNT, dtype=el_type)
        matG = np.zeros((6 * frames_count - KNOWN_FRAME_VARS_COUNT, 6 * frames_count - KNOWN_FRAME_VARS_COUNT), dtype=el_type)

        MAX_ABS_DIST = 0.1
        MAX_REL_DIST = 0.1 # 14327.78415796-14328.10677215=0.32261419 => rtol=2.2e-5
        def IsClose(a, b):
            return np.allclose(a, b, atol=MAX_ABS_DIST, rtol=MAX_REL_DIST)

        world_pnts_revert_copy = self.world_pnts.copy()
        framei_from_world_RT_list_revert_copy = self.framei_from_world_RT_list.copy()

        err_value_initial,_ = self.BundleAdjustmentReprojError(None, None, None, None)
        err_value = err_value_initial

        # TODO: if abs(opt_fun_value)<eps then success

        hessian_scalar = 0.0001  # hessian's diagonal multiplier
        while True:
            self.BundleAdjustmentComputeDerivatives(points_count, frames_count, check_derivatives, gradE, gradE2_onlyW, deriv_second_point, deriv_second_frame, deriv_second_pointframe, debug)

            # backup current state (world points and camera orientations)
            world_pnts_revert_copy[:] = self.world_pnts[:]
            framei_from_world_RT_list_revert_copy[:] = self.framei_from_world_RT_list[:]

            # loop to find a hessian scalar which decreases the target optimization function
            err_value_change = None
            while True:
                self.BundleAdjustmentEstimateCorrections(points_count, frames_count, hessian_scalar, gradE, deriv_second_point, deriv_second_frame, deriv_second_pointframe, matG, left_side1, right_side, corrections, debug)

                self.BundleAdjustmentApplyCorrections(points_count, frames_count, corrections)

                err_value_new,_ = self.BundleAdjustmentReprojError(None, None, None, None)

                err_value_change = err_value_new - err_value

                target_fun_decreased = err_value_change < 0
                if target_fun_decreased:
                    break

                # now, the value of target minimization function increases, try again with different params

                # restore saved state
                self.world_pnts[:] = world_pnts_revert_copy[:]
                self.framei_from_world_RT_list[:] = framei_from_world_RT_list_revert_copy[:]

                debug_successfull_revertion = True
                if debug_successfull_revertion:
                    opt_fun_value_reverted,_ = self.BundleAdjustmentReprojError(None, None, None, None)
                    assert np.isclose(err_value, opt_fun_value_reverted)

                if hessian_scalar > 1e6:
                    # prevent overflow for too big factors
                    break

                hessian_scalar *= 10  # prefer more the Steepest descent

            target_fun_decreased = err_value_change < 0
            if not target_fun_decreased:
                # imporovement ceased
                err_change_ratio = err_value / err_value_initial
                return False, err_change_ratio # failed

            if math.fabs(err_value_change) < self.err_fun_min_relative_change:
                err_change_ratio = err_value / err_value_initial
                return True, err_change_ratio  # success

            err_value = err_value_new
            hessian_scalar /= 10  # prefer more the Gauss-Newton

        assert False

    def BundleAdjustmentComputeDerivatives(self, points_count, frames_count, check_derivatives, gradE, gradE2_onlyW, deriv_second_point, deriv_second_frame, deriv_second_pointframe, debug):
        MAX_ABS_DIST = 0.1
        MAX_REL_DIST = 0.1 # 14327.78415796-14328.10677215=0.32261419 => rtol=2.2e-5
        POINT_COMPS = 3 # [X Y Z]
        FRAME_COMPS = 6 # [T1 T2 T3 W1 W2 W3], w=rotation axis in 'direct' SO3 frame
        cam_mat_pixel_from_meter = self.cam_mat_pixel_from_meter

        f1 = cam_mat_pixel_from_meter[0,0]
        f2 = cam_mat_pixel_from_meter[1,1]
        u0,v0,f0 = cam_mat_pixel_from_meter[0:3,2]

        el_type = self.elem_type

        eps = 1e-5 # finite difference step to approximate derivative

        def IsClose(a, b):
            return np.allclose(a, b, atol=MAX_ABS_DIST, rtol=MAX_REL_DIST)

        # Estimate the first derivative of a world point [X Y Z] around the given component index (0=X,1=Y,2=Z)
        def EstimateFirstPartialDerivPoint(pnt_id, pnt0, xyz_ind):
            pnt3D_left = pnt0.copy()
            pnt3D_left[xyz_ind] -= eps
            x1_err_sum, x1_err_per_point = self.BundleAdjustmentReprojError(pnt_id, pnt3D_left, None, None)

            pnt3D_right = pnt0.copy()
            pnt3D_right[xyz_ind] += eps
            x2_err_sum, x2_err_per_point = self.BundleAdjustmentReprojError(pnt_id, pnt3D_right, None, None)
            return (x2_err_sum - x1_err_sum) / (2 * eps)

        def EstimateFirstPartialDerivTranslation(frame_ind, T_direct, R_direct, w_direct, t_ind):
            R_direct_tmp = R_direct
            if R_direct_tmp is None:
                assert not w_direct is None
                suc, R_direct_tmp = RotMatFromAxisAngle(w_direct)
                assert suc

            t1dir = T_direct.copy()
            t1dir[t_ind] -= eps
            r1inv, t1inv = SE3Inv((R_direct_tmp, t1dir))
            t1_err_sum, t1_err_per_point = self.BundleAdjustmentReprojError(None, None, frame_ind, (r1inv, t1inv))

            t2dir = T_direct.copy()
            t2dir[t_ind] += eps
            r2inv, t2inv = SE3Inv((R_direct_tmp, t2dir))
            t2_err_sum, t2_err_per_point = self.BundleAdjustmentReprojError(None, None, frame_ind, (r2inv, t2inv))
            return (t2_err_sum - t1_err_sum) / (2 * eps)

        def EstimateFirstPartialDerivRotation(frame_ind, t_direct, w_direct, w_ind):
            w1 = w_direct.copy()
            w1[w_ind] -= eps
            suc, R1_direct = RotMatFromAxisAngle(w1)
            assert suc
            R1inv, T1inv = SE3Inv((R1_direct, t_direct))
            R1_err_sum, t1_err_per_point = self.BundleAdjustmentReprojError(None, None, frame_ind, (R1inv, T1inv))

            w2 = w_direct.copy()
            w2[w_ind] += eps
            suc, R2 = RotMatFromAxisAngle(w2)
            assert suc
            R2inv, T2inv = SE3Inv((R2, t_direct))
            R2_err_sum, R2_err_per_point = self.BundleAdjustmentReprojError(None, None, frame_ind, (R2inv, T2inv))
            return (R2_err_sum - R1_err_sum) / (2 * eps)

        def ErrAtXVec(xvec, pnt_id, frame_ind):
            assert len(xvec) == 9, "len(x)==9, [X Y Z T1 T2 T3 W1 W2 W3]"
            point3D = xvec[0:3]
            t_direct = xvec[3:6]
            w_direct = xvec[6:9]

            suc, r_direct = RotMatFromAxisAngle(w_direct)
            assert suc
            r_inv, t_inv = SE3Inv((r_direct, t_direct))

            result, _ = self.BundleAdjustmentReprojError(pnt_id, point3D, frame_ind, (r_inv, t_inv))
            return result

        xvec = np.zeros(9, dtype=el_type)
        xvec_tmp = np.zeros_like(xvec)

        def EstimateSecondPartialDeriv(pnt_id, x3D, var1, frame_ind, t_direct, w_direct, var2):
            xvec[0:3] = x3D
            xvec[3:6] = t_direct
            xvec[6:9] = w_direct

            np.copyto(xvec_tmp, src=xvec)
            xvec_tmp[var1] += eps
            xvec_tmp[var2] += eps
            f1 = ErrAtXVec(xvec_tmp, pnt_id, frame_ind)

            np.copyto(xvec_tmp, src=xvec)
            xvec_tmp[var1] += eps
            xvec_tmp[var2] -= eps
            f2 = ErrAtXVec(xvec_tmp, pnt_id, frame_ind)

            np.copyto(xvec_tmp, src=xvec)
            xvec_tmp[var1] -= eps
            xvec_tmp[var2] += eps
            f3 = ErrAtXVec(xvec_tmp, pnt_id, frame_ind)

            np.copyto(xvec_tmp, src=xvec)
            xvec_tmp[var1] -= eps
            xvec_tmp[var2] -= eps
            f4 = ErrAtXVec(xvec_tmp, pnt_id, frame_ind)

            # second order central finite difference formula
            # https://en.wikipedia.org/wiki/Finite_difference
            deriv2_value = (f1 - f2 - f3 + f4) / (4 * eps * eps)
            return deriv2_value

        for pnt_ind, pnt_id in enumerate(self.bundle_pnt_ids):
            pnt_life = self.points_life[pnt_id]
            pnt3D_world = self.world_pnts[pnt_life.track_id]
            assert not pnt3D_world is None

            for frame_ind in range(0, frames_count):
                corner_pix = pnt_life.points_list_pixel[frame_ind]
                if corner_pix is None: continue

                R_inverse,T_inverse = self.framei_from_world_RT_list[frame_ind]
                P = np.zeros((3,4), dtype=el_type)
                P[0:3, 0:3] = np.dot(cam_mat_pixel_from_meter,R_inverse)
                P[0:3, 3] = np.dot(cam_mat_pixel_from_meter,T_inverse)

                x3D_cam = SE3Apply((R_inverse,T_inverse), pnt3D_world)
                x3D_pix = np.dot(cam_mat_pixel_from_meter, x3D_cam)
                pqr = x3D_pix

                # dX,dY,dZ
                for xyz_ind in range(0,3):
                    ax = (pqr[0] / pqr[2] - corner_pix[0] / f0) * (pqr[2] * P[0, xyz_ind] - pqr[0] * P[2, xyz_ind]) + \
                         (pqr[1] / pqr[2] - corner_pix[1] / f0) * (pqr[2] * P[1, xyz_ind] - pqr[1] * P[2, xyz_ind])
                    ax *= 2 / pqr[2]**2
                    gradE[pnt_ind*3+xyz_ind] += ax

                # second derivative Point-Point
                # derivative(p) = [P[0,0] P[0,1] P[0,2]]
                # derivative(q) = [P[1,0] P[1,1] P[1,2]]
                # derivative(r) = [P[2,0] P[2,1] P[2,2]]
                for deriv1 in range(0,3): # [X Y Z]
                    for deriv2 in range(0,3): # [X Y Z]
                        ax = (pqr[2] * P[0, deriv1] - pqr[0] * P[2, deriv1]) * (pqr[2] * P[0, deriv2] - pqr[0] * P[2, deriv2]) + \
                             (pqr[2] * P[1, deriv1] - pqr[1] * P[2, deriv1]) * (pqr[2] * P[1, deriv2] - pqr[1] * P[2, deriv2])
                        ax *= 2 / pqr[2]**4
                        deriv_second_point[pnt_ind*3+deriv1, deriv2] += ax

            if check_derivatives:
                # estimate dX,dY,dZ
                gradPoint_estim = np.zeros(3, dtype=el_type)
                for xyz_ind in range(0, 3):
                    gradPoint_estim[xyz_ind] = EstimateFirstPartialDerivPoint(pnt_life.track_id, pnt3D_world, xyz_ind)

                print("gradXYZ_estim={} gradXYZ_exact={}".format(gradPoint_estim, gradE[pnt_ind*3:(pnt_ind+1)*3]))

                close = IsClose(gradPoint_estim, gradE[pnt_ind*3:(pnt_ind+1)*3])
                if not close:
                    assert False, "ERROR"

                # 2nd derivative Point-Point
                deriv_second_point_estim = np.zeros((3,3), dtype=el_type)
                R_direct, T_direct = SE3Inv((R_inverse, T_inverse))
                suc, w_norm, w_ang = LogSO3New(R_direct)
                assert suc
                w_direct = w_norm * w_ang
                for var1 in range(0,3):
                    for var2 in range(0,3):
                        deriv_second_point_estim[var1,var2] = EstimateSecondPartialDeriv(pnt_life.track_id, pnt3D_world, var1, frame_ind, T_direct, w_direct, var2)
                print("deriv2nd_estim deriv2nd_exact\n{}\n{}".format(deriv_second_point_estim, deriv_second_point[pnt_ind*3:(pnt_ind+1)*3,0:3]))

                close = IsClose(deriv_second_point_estim, deriv_second_point[pnt_ind*3:(pnt_ind+1)*3,0:3])
                if not close:
                    assert False, "ERROR"

            check_point_hessian_is_invertible = True
            if check_point_hessian_is_invertible:
                point_hessian = deriv_second_point[pnt_ind * 3 : (pnt_ind+1)*3, 0:3]
                is_inverted = True
                try:
                    point_hessian_inv = LA.inv(point_hessian)
                except LA.LinAlgError:
                    print("ERROR: inverting 3x3 E, pnt_ind={}".format(pnt_ind))
                    is_inverted = False
                assert is_inverted, "Can't invert point hessian for pnt_id={}".format(pnt_id)

        # dT
        grad_frames_section = points_count * 3 # frames goes after points
        pvec = np.zeros(3, dtype=el_type)
        qvec = np.zeros(3, dtype=el_type)
        rvec = np.zeros(3, dtype=el_type)
        for frame_ind in range(0, frames_count):
            R_inverse, T_inverse = self.framei_from_world_RT_list[frame_ind]
            R_direct, T_direct = SE3Inv((R_inverse,T_inverse))

            pvec[:] = -(f1*R_direct[:,0] + u0*R_direct[:,2])
            qvec[:] = -(f2*R_direct[:,1] + v0*R_direct[:,2])
            rvec[:] = -(               f0*R_direct[:,2])

            grad_frame_offset = grad_frames_section + frame_ind * 6

            for pnt_ind, pnt_id in enumerate(self.bundle_pnt_ids):
                pnt_life =  self.points_life[pnt_id]
                pnt3D_world = self.world_pnts[pnt_life.track_id]
                assert not pnt3D_world is None

                corner_pix = pnt_life.points_list_pixel[frame_ind]
                if corner_pix is None: continue

                x3D_cam = SE3Apply((R_inverse, T_inverse), pnt3D_world)
                x3D_pix = np.dot(cam_mat_pixel_from_meter, x3D_cam)
                pqr = x3D_pix
                P = np.dot(cam_mat_pixel_from_meter, np.hstack((R_inverse, T_inverse.reshape(3,1))))

                # translation gradient
                gradT_vec = (pqr[0] / pqr[2] - corner_pix[0] / f0) * (pqr[2] * pvec - pqr[0] * rvec) + \
                            (pqr[1] / pqr[2] - corner_pix[1] / f0) * (pqr[2] * qvec - pqr[1] * rvec)
                gradT_vec *= 2 / pqr[2] ** 2
                gradE[grad_frame_offset:grad_frame_offset+3] += gradT_vec

                # rotation gradient
                gradp_byw = np.cross(-pvec, pnt3D_world-T_direct)
                gradq_byw = np.cross(-qvec, pnt3D_world-T_direct)
                gradr_byw = np.cross(-rvec, pnt3D_world-T_direct)
                gradW_vec = (pqr[0] / pqr[2] - corner_pix[0] / f0) * (pqr[2] * gradp_byw - pqr[0] * gradr_byw) + \
                            (pqr[1] / pqr[2] - corner_pix[1] / f0) * (pqr[2] * gradq_byw - pqr[1] * gradr_byw)
                gradW_vec *= 2 / pqr[2] ** 2
                gradE[grad_frame_offset+3:grad_frame_offset+6] += gradW_vec

                rotation_gradient_via_quaternions = False
                if rotation_gradient_via_quaternions:
                    suc, w_direct_norm, w_ang = LogSO3New(R_direct)
                    if not suc:
                        # TODO: when this case happens?
                        continue

                    q = QuatFromRotationMat(R_direct)
                    #w_norm, w_ang = AxisPlusAngleFromQuat(q)
                    w_direct = w_direct_norm * w_ang

                    # source: "A Recipe on the Parameterization of Rotation Matrices", Terzakis, 2012
                    # partial derivatives of [3x3] rotation matrix with respect to quaternion components, formulas (33-36)
                    F = np.zeros((4, 3,3), dtype=el_type) # 4 matrices [3x3]
                    F[0,0,0:3] = [q[0], -q[3], q[2]]
                    F[0,1,0:3] = [q[3], q[0], -q[1]]
                    F[0,2,0:3] = [-q[2], q[1], q[0]]

                    F[1,0,0:3] = [q[1], q[2], q[3]]
                    F[1,1,0:3] = [q[2], -q[1], -q[0]]
                    F[1,2,0:3] = [q[3], q[0], -q[1]]

                    F[2,0,0:3] = [-q[2], q[1], q[0]]
                    F[2,1,0:3] = [q[1], q[2], q[3]]
                    F[2,2,0:3] = [-q[0], q[3], -q[2]]

                    F[3,0,0:3] = [-q[3], -q[0], q[1]]
                    F[3,1,0:3] = [q[0], -q[3], q[2]]
                    F[3,2,0:3] = [q[1], q[2], q[3]]
                    F *= 2

                    # partial derivatives of [4x1] quaternion components with respect to [3x1] axis-angle components, formulas (38-40)
                    G = np.zeros((4,3), dtype=el_type) # 3 vectors [4x1]
                    ang = w_ang
                    sin_ang2 = math.sin(ang/2)
                    cos_ang2 = math.cos(ang/2)
                    cos_diff = (0.5 * cos_ang2 - sin_ang2 / ang) / ang ** 2
                    G[0, 0] = -0.5 * w_direct[0] * sin_ang2 / ang # dq0/du1
                    G[1, 0] = sin_ang2 / ang + w_direct[0] * w_direct[0] * cos_diff  # dq1/du1
                    G[2, 0] = w_direct[0] * w_direct[1] * cos_diff # dq2/du1
                    G[3, 0] = w_direct[0] * w_direct[2] * cos_diff # dq2/du1

                    G[0, 1] = -0.5 * w_direct[1] * sin_ang2 / ang
                    G[1, 1] = w_direct[0] * w_direct[1] * cos_diff
                    G[2, 1] = sin_ang2 / ang + w_direct[1] * w_direct[1] * cos_diff
                    G[3, 1] = w_direct[1] * w_direct[2] * cos_diff

                    G[0, 2] = -0.5 * w_direct[2] * sin_ang2 / ang
                    G[1, 2] = w_direct[0] * w_direct[2] * cos_diff
                    G[2, 2] = w_direct[1] * w_direct[2] * cos_diff
                    G[3, 2] = sin_ang2 / ang + w_direct[2] * w_direct[2] * cos_diff

                    # formula 41
                    dR = np.zeros((3, 3,3), dtype=el_type) # 3 matrices [3x3]
                    for deriv1 in range(0,3): # each component of axis-angle representation
                        for deriv2 in range(0, 4):
                            mat33 = F[deriv2,:,:]
                            scalar = G[deriv2,deriv1]
                            dR[deriv1,:,:] += mat33 * scalar

                    #print("gradp_byw={}".format(gradp_byw))
                    #print("gradq_byw={}".format(gradq_byw))
                    #print("gradr_byw={}".format(gradr_byw))
                    pqr_bywi = np.zeros((3,3),dtype=el_type)
                    for wi in range(0,3):
                        pqr_bywi[0:3,wi] = cam_mat_pixel_from_meter.dot(dR[wi,:,:].T).dot(np.hstack((np.eye(3,3),-T_direct.reshape((3,1))))).dot(np.hstack((pnt3D_world,1)))
                    gradp_byw = pqr_bywi[0,0:3]
                    gradq_byw = pqr_bywi[1,0:3]
                    gradr_byw = pqr_bywi[2,0:3]
                    #print("gradp_byw={}".format(gradp_byw))
                    #print("gradq_byw={}".format(gradq_byw))
                    #print("gradr_byw={}".format(gradr_byw))
                    #print()
                    gradW_vec2 = (pqr[0] / pqr[2] - corner_pix[0] / f0) * (pqr[2] * gradp_byw - pqr[0] * gradr_byw) + \
                                (pqr[1] / pqr[2] - corner_pix[1] / f0) * (pqr[2] * gradq_byw - pqr[1] * gradr_byw)
                    gradW_vec2 *= 2 / pqr[2] ** 2
                    gradE2_onlyW[grad_frame_offset+3:grad_frame_offset+6] += gradW_vec2

                # partial second derivative of the Frame components [T1 T2 T3 W1 W2 W3]
                deriv = np.zeros((6,3), dtype=el_type) # rows=[T1 T2 T3 W1 W2 W3] cols=[dp/di dq/di dr/di]
                deriv[0:3,0] = pvec[0:3]
                deriv[0:3,1] = qvec[0:3]
                deriv[0:3,2] = rvec[0:3]
                deriv[3:6,0] = gradp_byw[0:3]
                deriv[3:6,1] = gradq_byw[0:3]
                deriv[3:6,2] = gradr_byw[0:3]
                for deriv1 in range(0,6):
                    for deriv2 in range(0,6):
                        ax = (pqr[2] * deriv[deriv1, 0] - pqr[0] * deriv[deriv1 ,2]) * (pqr[2] * deriv[deriv2, 0] - pqr[0] * deriv[deriv2, 2]) + \
                             (pqr[2] * deriv[deriv1, 1] - pqr[1] * deriv[deriv1, 2]) * (pqr[2] * deriv[deriv2, 1] - pqr[1] * deriv[deriv2, 2])
                        ax *= 2 / pqr[2]**4
                        deriv_second_frame[frame_ind*6+deriv1, deriv2] += ax

                # partial second derivative of the Point-Frame components [T1 T2 T3 W1 W2 W3]
                deriv = np.zeros((9,3), dtype=el_type) # rows=[X Y Z T1 T2 T3 W1 W2 W3] cols=[dp/di dq/di dr/di]
                deriv[0:3,0] = P[0, 0:3] # dp/d(X,Y,Z)
                deriv[0:3,1] = P[1, 0:3] # dq/d(X,Y,Z)
                deriv[0:3,2] = P[2, 0:3] # dr/d(X,Y,Z)
                deriv[3:6,0] = pvec[0:3] # dp/d(T1,T2,T3)
                deriv[3:6,1] = qvec[0:3] # dq/d(T1,T2,T3)
                deriv[3:6,2] = rvec[0:3] # dr/d(T1,T2,T3)
                deriv[6:9,0] = gradp_byw[0:3] # dp/d(W1,W2,W3)
                deriv[6:9,1] = gradq_byw[0:3] # dq/d(W1,W2,W3)
                deriv[6:9,2] = gradr_byw[0:3] # dr/d(W1,W2,W3)
                for deriv1 in range(0,3): # X Y Z
                    for deriv2 in range(0,6): # T1 T2 T3 W1 W2 W3
                        # deriv[+3,] to offset XYZ derivative components
                        ax = (pqr[2] * deriv[deriv1, 0] - pqr[0] * deriv[deriv1 ,2]) * (pqr[2] * deriv[3+deriv2, 0] - pqr[0] * deriv[3+deriv2, 2]) + \
                             (pqr[2] * deriv[deriv1, 1] - pqr[1] * deriv[deriv1, 2]) * (pqr[2] * deriv[3+deriv2, 1] - pqr[1] * deriv[3+deriv2, 2])
                        ax *= 2 / pqr[2]**4
                        deriv_second_pointframe[pnt_ind*3+deriv1, frame_ind*6+deriv2] += ax

            if check_derivatives:
                # approximation of translation gradient
                gradT_estim = np.zeros(3, dtype=el_type)
                for t_ind in range(0, 3):
                    gradT_estim[t_ind] = EstimateFirstPartialDerivTranslation(frame_ind, T_direct, R_direct, None, t_ind)

                print("gradT_estim={} gradT_exact={}".format(gradT_estim, gradE[grad_frame_offset:grad_frame_offset+3]))

                close = IsClose(gradT_estim, gradE[grad_frame_offset:grad_frame_offset+3])
                if not close:
                    assert False, "ERROR"

                # approximation of rotation gradient
                suc, w_direct_norm, w_ang = LogSO3New(R_direct)
                if suc:
                    w_direct = w_direct_norm * w_ang
                    gradW_estim = np.zeros(3, dtype=el_type)
                    for w_ind in range(0, 3):
                        gradW_estim[w_ind] = EstimateFirstPartialDerivRotation(frame_ind, T_direct, w_direct, w_ind)

                    print("gradW_estim={} gradW_exact={} gradW2_exact={}".format(gradW_estim, gradE[grad_frame_offset+3:grad_frame_offset+6], gradE2_onlyW[grad_frame_offset+3:grad_frame_offset+6]))

                    close = IsClose(gradW_estim, gradE[grad_frame_offset+3:grad_frame_offset+6])
                    if not close:
                        assert False, "ERROR" # TODO: crashes on gradW_estim=[ 13.47551411  18.0067005    0.66238898] gradW_exact=[ 12.37611123  18.81862305   0.44619587] gradW2_exact=[ 13.49019804  18.01214564   0.66229125]
                    close = IsClose(gradW_estim, gradE2_onlyW[grad_frame_offset+3:grad_frame_offset+6])
                    if not close:
                        assert False, "ERROR"

                    # 2nd derivative Frame-Frame
                    deriv_second_frame_estim = np.zeros((6, 6), dtype=el_type)
                    for var1 in range(3,9):
                        for var2 in range(3,9):
                            deriv_second_frame_estim[var1-3,var2-3] = EstimateSecondPartialDeriv(pnt_life.track_id, pnt3D_world, var1, frame_ind, T_direct, w_direct, var2)

                    print("deriv2nd_RT_estim deriv2nd_RT_exact\n{}\n{}".format(deriv_second_frame_estim, deriv_second_frame[frame_ind*6:(frame_ind+1)*6, 0:6]))

                    close = IsClose(deriv_second_frame_estim, deriv_second_frame[frame_ind*6:(frame_ind+1)*6, 0:6])
                    if not close:
                        assert False, "ERROR"

                    # 2nd derivative Point-Frame
                    deriv_second_pointframe_estim = np.zeros((3, 6), dtype=el_type)
                    for var1 in range(0,3):
                        for var2 in range(3,9):
                            deriv_second_pointframe_estim[var1,var2-POINT_COMPS] = EstimateSecondPartialDeriv(pnt_life.track_id, pnt3D_world, var1, frame_ind, T_direct, w_direct, var2)

                    print("deriv2nd_PointFrame_estim deriv2nd_PointFrame_exact\n{}\n{}".format(deriv_second_pointframe_estim, deriv_second_pointframe[pnt_ind*3:(pnt_ind+1)*3, frame_ind*6:(frame_ind+1)*6]))

                    close = IsClose(deriv_second_pointframe_estim, deriv_second_pointframe[pnt_ind*3:(pnt_ind+1)*3, frame_ind*6:(frame_ind+1)*6])
                    if not close:
                        assert False, "ERROR"

            pass # loop through points
        pass # frames section

        c1 = np.any(~np.isfinite(gradE))
        c2 = np.any(~np.isfinite(deriv_second_point))
        c3 = np.any(~np.isfinite(deriv_second_frame))
        c4 = np.any(~np.isfinite(deriv_second_pointframe))
        if c1 or c2 or c3 or c4:
            assert False, "Derivatives must be real numbers"
        return None

    def BundleAdjustmentEstimateCorrections(self, points_count, frames_count, c, gradE, deriv_second_point, deriv_second_frame, deriv_second_pointframe, matG,
                                            left_side1, right_side, corrections, debug):
        """ Computes updates for optimization variables"""

        # convert Frame-Frame matrix into the square shape
        frame_offset = 0
        for frame_ind in range(0, frames_count):
            if frame_ind == 0: continue # skip initial frame (R0,T0)

            ax = deriv_second_frame[frame_ind * 6:(frame_ind + 1) * 6, 0:6]

            if frame_ind == 1: # skip T1x or T1y
                ax = np.delete(ax, [self.unity_comp_ind], axis=1) # delete column
                ax = np.delete(ax, [self.unity_comp_ind], axis=0) # delete row

            size = ax.shape[0]

            matG[frame_offset:frame_offset+size, frame_offset:frame_offset+size] = ax
            frame_offset += size

        left_side1.fill(0)
        right_side.fill(0)

        # calculate deltas for frame unknowns
        for pnt_ind in range(0, points_count):
            point_frame = deriv_second_pointframe[pnt_ind*3:(pnt_ind+1)*3,:] # 3 x 6*frames_count
            point_frame = np.delete(point_frame, [0,1,2,3,4,5,6+self.unity_comp_ind], axis=1) # delete normalized columns

            point_hessian = deriv_second_point[pnt_ind*3:(pnt_ind+1)*3,0:3] # 3x3
            point_hessian_scaled = (1+c)*point_hessian
            assert np.all(np.isfinite(point_hessian_scaled)), "Possibly to big hessian scalar c={}".format(c)

            try:
                point_hessian_inv = LA.inv(point_hessian_scaled)
            except LA.LinAlgError:
                assert False, "ERROR: inverting 3x3 E"

            gradeE_point = gradE[pnt_ind*3:(pnt_ind+1)*3] # 3x1

            # left side
            ax = point_frame.T.dot(point_hessian_inv).dot(point_frame) # 6*frames_count x 6*frames_count
            left_side1 += ax

            # right side
            ax = point_frame.T.dot(point_hessian_inv).dot(gradeE_point)
            right_side += ax


        left_side1 = matG - left_side1  # G-sum(F.E.F)
        frame_derivs_packed =  gradE[3*points_count:]
        frame_derivs_packed = np.delete(frame_derivs_packed, [0, 1, 2, 3, 4, 5, 6 + self.unity_comp_ind],axis=None)
        right_side -= frame_derivs_packed # sum(F.E.gradE)-Df
        deltas_frame = LA.solve(left_side1, right_side)

        # copy corrections with gaps to plain corrections
        corrections.fill(float('nan'))
        corrections[points_count * 3:points_count * 3 + 6] = 0 # [T0x T0y T0z R0wx R0wy R0wz]
        corrections[points_count * 3 + 6 +    self.unity_comp_ind ] = 0 # T1x or T1y
        corrections[points_count * 3 + 6 + (1-self.unity_comp_ind)] = deltas_frame[0] # T1x or T1y
        corrections[points_count * 3 + 6 + 2]                  = deltas_frame[1] # T1z
        corrections[points_count * 3 + 6 + 3:] = deltas_frame[2:]
        assert np.all(np.isfinite(corrections[points_count * 3:])), "Failed to copy normalized corrections"

        # calculate deltas for point unknowns
        for pnt_ind in range(0, points_count):
            point_frame = deriv_second_pointframe[pnt_ind*3:(pnt_ind+1)*3,:] # 3 x 6*frames_count
            point_frame = np.delete(point_frame, [0, 1, 2, 3, 4, 5, 6 + self.unity_comp_ind],axis=1)  # delete normalized columns

            gradeE_point = gradE[pnt_ind*3:(pnt_ind+1)*3] # 3x1

            partB = point_frame.dot(deltas_frame) + gradeE_point

            point_hessian = deriv_second_point[pnt_ind * 3:(pnt_ind + 1) * 3, 0:3]  # 3x3
            point_hessian_inv = LA.inv((1+c)*point_hessian)

            deltas_one_point = -point_hessian_inv.dot(partB)
            corrections[pnt_ind * 3:(pnt_ind + 1) * 3] = deltas_one_point[0:3]

        assert np.all(np.isfinite(corrections)), "Change of a variable must be a real number"

        return None

    def BundleAdjustmentApplyCorrectionsOld(self, points_count, frames_count, bundle_pnt_ids, corrections, reverse_update):
        # for pnt_ind in range(0, points_count):
        #     deltaX = corrections[pnt_ind * 3:(pnt_ind + 1) * 3]
        #     if reverse_update:
        #         deltaX = -deltaX
        #
        #     pnt_id = bundle_pnt_ids[pnt_ind]
        #     self.world_pnts[pnt_id] += deltaX
        #
        for frame_ind in range(0, frames_count):
            rt_inversed = self.framei_from_world_RT_list[frame_ind]
            rt_direct = SE3Inv(rt_inversed)

            suc, w_direct = AxisAngleFromRotMat(rt_direct[0])
            if suc:
                deltaF = corrections[points_count*3 + frame_ind*6:points_count*3 + (frame_ind+1)*6]

                deltaW = deltaF[3:6]
                if reverse_update:
                    deltaW = -deltaW
                w_direct_new = w_direct + deltaW

                suc, R_direct = RotMatFromAxisAngle(w_direct_new)
                assert suc

                deltaT = deltaF[0:3]
                if reverse_update:
                    deltaT = -deltaT
                T_direct = rt_direct[1] + deltaT

                rt_inversed_new = SE3Inv((R_direct, T_direct))

                #revert
                reverse_updateAAA = not reverse_update
                rt_directAAA = SE3Inv(rt_inversed_new)
                suc, w_directAAA = AxisAngleFromRotMat(rt_directAAA[0])
                if suc:
                    deltaFAAA = corrections[points_count * 3 + frame_ind * 6:points_count * 3 + (frame_ind + 1) * 6]

                    deltaWAAA = deltaFAAA[3:6]
                    if reverse_updateAAA:
                        deltaWAAA = -deltaWAAA
                    w_direct_newAAA = w_directAAA + deltaWAAA

                    suc, R_directAAA = RotMatFromAxisAngle(w_direct_newAAA)
                    assert suc

                    deltaTAAA = deltaFAAA[0:3]
                    if reverse_updateAAA:
                        deltaTAAA = -deltaTAAA
                    T_directAAA = rt_directAAA[1] + deltaTAAA

                    rt_inversed_new = SE3Inv((R_directAAA, T_directAAA))

                #
                self.framei_from_world_RT_list[frame_ind] = rt_inversed_new

    def BundleAdjustmentApplyCorrections(self, points_count, frames_count, corrections):
        for pnt_ind in range(0, points_count):
            deltaX = corrections[pnt_ind * 3:(pnt_ind + 1) * 3]
            pnt_id = self.bundle_pnt_ids[pnt_ind]
            self.world_pnts[pnt_id] += deltaX

        for frame_ind in range(0, frames_count):
            rt_inversed = self.framei_from_world_RT_list[frame_ind]
            rt_direct = SE3Inv(rt_inversed)

            deltaF = corrections[points_count*3 + frame_ind*6:points_count*3 + (frame_ind+1)*6]

            deltaT = deltaF[0:3]
            T_direct = rt_direct[1] + deltaT

            deltaW = deltaF[3:6]
            all_zero = np.all(np.abs(deltaW) < 1e-5)
            if all_zero:
                R_direct_new = rt_direct[0]
            else:
                suc, rot_delta = RotMatFromAxisAngle(deltaW)
                R_direct_new = rot_delta.dot(rt_direct[0])

            rt_inversed_new = SE3Inv((R_direct_new, T_direct))
            self.framei_from_world_RT_list[frame_ind] = rt_inversed_new

    def BundleAdjustmentReprojError(self, overwrite_track_id = None, overwrite_x3D=None, overwrite_frame_ind=None, overwrite_rt=None):
        if not overwrite_track_id is None:
            assert not overwrite_x3D is None, "Provide 3D world point to overwrite"
        if not overwrite_frame_ind is None:
            assert not overwrite_rt is None, "Provide frame R,T to overwrite"

        frames_count = len(self.framei_from_world_RT_list)
        cam_mat_pixel_from_meter = self.cam_mat_pixel_from_meter

        el_type = self.elem_type
        visib_pnts_count = 0
        err_sum = 0.0

        for frame_ind in range(0, frames_count):
            if not overwrite_frame_ind is None and overwrite_frame_ind == frame_ind:
                R,T = overwrite_rt
            else:
                R,T = self.framei_from_world_RT_list[frame_ind]

            for pnt_life in self.points_life:
                if not overwrite_track_id is None and overwrite_track_id == pnt_life.track_id:
                    pnt3D_world = overwrite_x3D
                else:
                    #continue
                    pnt3D_world = self.world_pnts[pnt_life.track_id]

                #assert not pnt3D_world is None, "Mapped points must have 3D position"
                if pnt3D_world is None: continue

                #x_meter = pnt_life.points_list_meter[frame_ind]
                corner_pix = pnt_life.points_list_pixel[frame_ind]
                #assert not expect_pix is None, "Mapped points must be detected in image and hence, the position is known"
                if corner_pix is None: continue

                x3D_cam = SE3Apply((R,T), pnt3D_world)
                x3D_pix = np.dot(cam_mat_pixel_from_meter, x3D_cam)
                x = x3D_pix[0]/x3D_pix[2]
                y = x3D_pix[1]/x3D_pix[2]
                one_err = (x - corner_pix[0])**2 + (y - corner_pix[1])**2
                # if one_err > 0.01:
                #     print("expect={} actual={}".format(corner_pix, (x,y)))
                # if overwrite_track_id == pnt_life.track_id:
                #     print("track_id={} frame_ind={} corner={} x3D={} T={} R=\n{}".format(pnt_life.track_id, frame_ind, corner_pix, pnt3D_world, T, R))

                err_sum += one_err
                visib_pnts_count += 1
        err_per_point = err_sum / visib_pnts_count if visib_pnts_count > 0 else None

        return err_sum, err_per_point