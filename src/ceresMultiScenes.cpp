// basic
#include <iostream>
#include <algorithm>
#include <vector>
// eigen
#include <Eigen/Core>
// opencv
#include <opencv2/opencv.hpp>
#include <opencv2/core/core.hpp>
// ceres
#include "ceres/ceres.h"
#include "ceres/cubic_interpolation.h"
#include "ceres/rotation.h"
#include "glog/logging.h"
// heading
#include "imageProcess.h"
#include "lidarProcess.h"
#include "visualization.cpp"

using namespace std;

static const int num_q = 3;
static const int num_p = 9;

/**
 * @brief Get double from type T (double and ceres::Jet) variables in the ceres optimization process
 *
 * @param x input variable with type T (double and ceres::Jet)
 * @return **
 */
double get_double(double x)
{
    return static_cast<double>(x);
}

template <typename SCALAR, int N>
double get_double(const ceres::Jet<SCALAR, N> &x)
{
    return static_cast<double>(x.a);
}

void customOutput(vector<const char *> name, double *params, vector<double> params_init)
{
    std::cout << "Initial ";
    for (unsigned int i = 0; i < name.size(); i++)
    {
        std::cout << name[i] << ": " << params_init[i] << " ";
    }
    std::cout << "\n";
    std::cout << "Final   ";
    for (unsigned int i = 0; i < name.size(); i++)
    {
        std::cout << name[i] << ": " << params[i] << " ";
    }
    std::cout << "\n";
}

void initQuaternion(double rx, double ry, double rz, vector<double> &init)
{
    Eigen::Vector3d eulerAngle(rx, ry, rz);
    Eigen::AngleAxisd xAngle(Eigen::AngleAxisd(eulerAngle(0), Eigen::Vector3d::UnitX()));
    Eigen::AngleAxisd yAngle(Eigen::AngleAxisd(eulerAngle(1), Eigen::Vector3d::UnitY()));
    Eigen::AngleAxisd zAngle(Eigen::AngleAxisd(eulerAngle(2), Eigen::Vector3d::UnitZ()));
    Eigen::Matrix3d R;
    R = zAngle * yAngle * xAngle;
    Eigen::Quaterniond q(R);
    init[0] = q.x();
    init[1] = q.y();
    init[2] = q.z();
    init[3] = q.w();
}

struct Calibration
{
    template <typename T>
    bool operator()(const T *const _q, const T *const _p, T *cost) const
    {
        // intrinsic parameters
        Eigen::Matrix<T, 3, 1> eulerAngle(_q[0], _q[1], _q[2]);
        Eigen::Matrix<T, 3, 1> t{_p[0], _p[1], _p[2]};
        Eigen::Matrix<T, 2, 1> uv_0{_p[3], _p[4]};
        Eigen::Matrix<T, 6, 1> a_;
        switch (num_p)
        {
            case 10:
                a_ << _p[5], _p[6], _p[7], _p[8], _p[9], T(0);
                break;
            case 9:
                a_ << _p[5], _p[6], T(0), _p[7], T(0), _p[8];
                break;
            default:
                a_ << T(0), _p[5], T(0), _p[6], T(0), _p[7];
                break;
        }

        Eigen::Matrix<T, 3, 1> p_ = point_.cast<T>();
        Eigen::Matrix<T, 3, 1> p_trans;

        T phi, theta, r;
        T inv_r;
        T res, val;

        /** extrinsic transform for original 3d lidar edge points **/
        Eigen::Matrix<T, 3, 3> R;

        Eigen::AngleAxis<T> xAngle(Eigen::AngleAxis<T>(eulerAngle(0), Eigen::Matrix<T, 3, 1>::UnitX()));
        Eigen::AngleAxis<T> yAngle(Eigen::AngleAxis<T>(eulerAngle(1), Eigen::Matrix<T, 3, 1>::UnitY()));
        Eigen::AngleAxis<T> zAngle(Eigen::AngleAxis<T>(eulerAngle(2), Eigen::Matrix<T, 3, 1>::UnitZ()));
        R = zAngle * yAngle * xAngle;

        /** Construct rotation matrix using quaternion in Eigen convention (w, x, y, z) **/
        // Eigen::Quaternion<T> q{_q[3], _q[0], _q[1], _q[2]};
        // R = q.toRotationMatrix()

        p_trans = R * p_ + t;

        /** extrinsic transform for transformed 3d lidar edge points **/
        Eigen::Matrix<T, 2, 1> S;
        Eigen::Matrix<T, 2, 1> p_uv;

        /** r - theta representaion: r = f(theta) in polynomial form **/
        theta = acos(p_trans(2) / sqrt((p_trans(0) * p_trans(0)) + (p_trans(1) * p_trans(1)) + (p_trans(2) * p_trans(2))));
        inv_r = a_(0) + a_(1) * theta + a_(2) * pow(theta, 2) + a_(3) * pow(theta, 3) + a_(4) * pow(theta, 4) + a_(5) * pow(theta, 5);
        // inv_r = a0 + a1 * theta + a3 * pow(theta, 3) + a5 * pow(theta, 5);
        // phi = atan2(p_trans(1), p_trans(0));
        // inv_u = (inv_r * cos(phi) + u0);
        // inv_v = (inv_r * sin(phi) + v0);

        /** compute undistorted uv coordinate of lidar projection point and evaluate the value **/
        r = sqrt(p_trans(1) * p_trans(1) + p_trans(0) * p_trans(0));
        S = {-inv_r * p_trans(0) / r, -inv_r * p_trans(1) / r};
        p_uv = inv_distortion_.cast<T>() * S + uv_0;
        kde_interpolator_.Evaluate(p_uv(0), p_uv(1), &val);

        /**
         * residual:
         * 1. diff. of kde image evaluation and lidar point projection(related to constant "ref_val_" and unified radius));
         * 2. polynomial correction: r_max = F(theta_max);
         *  **/
        res = T(ref_val_) * (T(1.0) - inv_r * T(0.5 / img_size_[1])) - val + T(1e-8) * abs(T(1071) - a_(0) - a_(1) * T(ref_theta) - a_(2) * T(pow(ref_theta, 2)) - a_(3) * T(pow(ref_theta, 3)) - a_(4) * T(pow(ref_theta, 4) - a_(5) * T(pow(ref_theta, 5))));

        cost[0] = T(scale_) * res;
        cost[1] = T(scale_) * res;
        return true;
    }

    /** DO NOT remove the "&" of the interpolator! **/
    Calibration(const Eigen::Vector3d point,
                const Eigen::Vector2d img_size,
                const double ref_val,
                const double scale,
                const ceres::BiCubicInterpolator<ceres::Grid2D<double>> &interpolator,
                const Eigen::Matrix2d inv_distortion)
            : point_(std::move(point)), kde_interpolator_(interpolator), img_size_(img_size), ref_val_(ref_val), scale_(std::move(scale)), inv_distortion_(inv_distortion) {}

    /**
     * @brief
     * create multiscenes costfunction for optimization.
     * @param point-xyz coordinate of a 3d lidar edge point;
     * @param img_size-size of the original fisheye image;
     * @param ref_val-default reference value of lidar points;
     * @param scale-normalization coefficient determined by the relative size of scenes;
     * @param interpolator-bicubic interpolator for original fisheye image;
     * @param inv_distortion-inverse distortion matrix [c, d; e, 1] for fisheye camera;
     * @return ** ceres::CostFunction*
     */
    static ceres::CostFunction *Create(const Eigen::Vector3d &point,
                                       const Eigen::Vector2d &img_size,
                                       const double &ref_val,
                                       const double &scale,
                                       const ceres::BiCubicInterpolator<ceres::Grid2D<double>> &interpolator,
                                       const Eigen::Matrix2d &inv_distortion)
    {
        return new ceres::AutoDiffCostFunction<Calibration, 2, num_q, num_p>(
                new Calibration(point, img_size, ref_val, scale, interpolator, inv_distortion));
    }

    const Eigen::Vector3d point_;
    const Eigen::Vector2d img_size_;
    const double ref_val_;
    const double scale_;
    const ceres::BiCubicInterpolator<ceres::Grid2D<double>> &kde_interpolator_;
    const Eigen::Matrix2d inv_distortion_;
    const double ref_theta = 99.5 * M_PI / 180;
};

/**
 * @brief
 * custom callback to print something after every iteration (inner iteration is not included)
 */
class OutputCallback : public ceres::IterationCallback
{
public:
    OutputCallback(double *params)
            : params_(params) {}

    ceres::CallbackReturnType operator()(
            const ceres::IterationSummary &summary) override
    {
        return ceres::SOLVER_CONTINUE;
    }

private:
    const double *params_;
};

/**
 * @brief
 * Ceres-solver Optimization
 * @param cam camProcess
 * @param lid lidarProcess
 * @param bandwidth bandwidth for kde estimation(Gaussian kernel)
 * @param distortion distortion matrix {c, d; e, 1}
 * @param params_init initial parameters
 * @param name name of parameters
 * @param lb lower bounds of the parameters
 * @param ub upper bounds of the parameters
 * @return ** std::vector<double>
 */
std::vector<double> ceresMultiScenes(imageProcess cam,
                                     lidarProcess lid,
                                     double bandwidth,
                                     Eigen::Matrix2d distortion,
                                     vector<double> params_init,
                                     vector<const char *> name,
                                     vector<double> lb,
                                     vector<double> ub)
{
    const int num_params = params_init.size();
    const int numScenes = cam.numScenes;

    double params[num_params];
    memcpy(params, &params_init[0], params_init.size() * sizeof(double));
    const Eigen::Matrix2d inv_distortion = distortion.inverse();
    // std::copy(std::begin(params_init), std::end(params_init), std::begin(params));

    std::vector<ceres::Grid2D<double>> grids;
    std::vector<double> ref_vals;
    std::vector<ceres::BiCubicInterpolator<ceres::Grid2D<double>>> interpolators;

    for (int idx = 0; idx < numScenes; idx++)
    {
        cam.setSceneIdx(idx);
        cam.readEdge();
        /********* Fisheye KDE *********/
        vector<double> p_c = cam.kdeBlur(bandwidth, 1.0, false);
        // Data is a row-major array of kGridRows x kGridCols values of function
        // f(x, y) on the grid, with x in {-kGridColsHalf, ..., +kGridColsHalf},
        // and y in {-kGridRowsHalf, ..., +kGridRowsHalf}
        // double *kde_data = new double[p_c.size()];
        // memcpy(kde_data, &p_c[0], p_c.size() * sizeof(double));
        double *kde_data = p_c.data();
        // unable to set coordinate to 2D grid for corresponding interpolator;
        // use post-processing to scale the grid instead.
        const ceres::Grid2D<double> kde_grid(kde_data, 0, cam.kdeRows, 0, cam.kdeCols);
        grids.push_back(kde_grid);
        ref_vals.push_back(*max_element(p_c.begin(), p_c.end()) / (0.125 * bandwidth));
    }
    const std::vector<ceres::Grid2D<double>> img_grids(grids);
    for (int idx = 0; idx < numScenes; idx++)
    {
        const ceres::BiCubicInterpolator<ceres::Grid2D<double>> kde_interpolator(img_grids[idx]);
        interpolators.push_back(kde_interpolator);
    }
    const std::vector<ceres::BiCubicInterpolator<ceres::Grid2D<double>>> img_interpolators(interpolators);

    // Ceres Problem
    // ceres::LocalParameterization * q_parameterization = new ceres::EigenQuaternionParameterization();
    ceres::Problem problem;

    // problem.AddParameterBlock(params, 4, q_parameterization);
    // problem.AddParameterBlock(params + 4, num_params - 4);
    problem.AddParameterBlock(params, num_q);
    problem.AddParameterBlock(params + num_q, num_params - num_q);
    ceres::LossFunction *loss_function = new ceres::HuberLoss(0.05);

    Eigen::Vector2d img_size = {cam.orgRows, cam.orgCols};
    for (int idx = 0; idx < numScenes; idx++)
    {
        lid.setSceneIdx(idx);
        lid.readEdge();
        /** note: '30000' is a typical value for normalization. **/
        const double relative_size = (double)lid.EdgeOrgCloud->points.size() / 30000;
        for (int j = 0; j < lid.EdgeOrgCloud->points.size(); ++j)
        {
            Eigen::Vector3d p_l_tmp = {lid.EdgeOrgCloud->points[j].x, lid.EdgeOrgCloud->points[j].y, lid.EdgeOrgCloud->points[j].z};
            problem.AddResidualBlock(Calibration::Create(p_l_tmp, img_size, ref_vals[idx], relative_size, img_interpolators[idx], inv_distortion),
                                     loss_function,
                                     params,
                                     params + num_q);
        }
    }

    for (int i = 0; i < num_params; ++i)
    {
        if (i < num_q)
        {
            problem.SetParameterLowerBound(params, i, lb[i]);
            problem.SetParameterUpperBound(params, i, ub[i]);
        }
        else
        {
            problem.SetParameterLowerBound(params + num_q, i - num_q, lb[i]);
            problem.SetParameterUpperBound(params + num_q, i - num_q, ub[i]);
        }
    }

    ceres::Solver::Options options;
    options.linear_solver_type = ceres::DENSE_SCHUR;
    options.trust_region_strategy_type = ceres::LEVENBERG_MARQUARDT;
    options.minimizer_progress_to_stdout = true;
    options.num_threads = 12;
    options.function_tolerance = 1e-7;
    options.use_nonmonotonic_steps = true;

    // OutputCallback callback(params);
    // options.callbacks.push_back(&callback);

    ceres::Solver::Summary summary;

    ceres::Solve(options, &problem, &summary);

    std::cout << summary.FullReport() << "\n";
    customOutput(name, params, params_init);
    std::vector<double> params_res(params, params + sizeof(params) / sizeof(double));

    for (int idx = 0; idx < numScenes; idx++)
    {
        lid.setSceneIdx(idx);
        cam.setSceneIdx(idx);
        cam.readEdge();
        lid.readEdge();
        vector<vector<double>> lidProjection = lid.edgeVizTransform(params_res, distortion);
        string lidEdgeTransTxtPath = lid.scenesFilePath[lid.scIdx].EdgeTransTxtPath;
        fusionViz(cam, lidEdgeTransTxtPath, lidProjection, bandwidth);
    }

    return params_res;
}

// /**
//  * @brief
//  * Ceres-solver Optimization
//  * @param cam camProcess
//  * @param lid lidarProcess
//  * @param bandwidth bandwidth for kde estimation(Gaussian kernel)
//  * @param distortion distortion matrix {c, d; e, 1}
//  * @param params_init initial parameters
//  * @param name name of parameters
//  * @param lb lower bounds of the parameters
//  * @param ub upper bounds of the parameters
//  * @return ** std::vector<double>
//  */
// std::vector<double> ceresAutoDiff(imageProcess cam,
//                                   lidarProcess lid,
//                                   double bandwidth,
//                                   const Eigen::Matrix2d distortion,
//                                   vector<double> params_init,
//                                   vector<const char *> name,
//                                   vector<double> lb,
//                                   vector<double> ub)
// {
//     std::vector<double> p_c = cam.kdeBlur(bandwidth, 1.0, false);
//     const double ref_val = *max_element(p_c.begin(), p_c.end()) / (0.125 * bandwidth);
//     const int num_params = params_init.size();

//     // initQuaternion(0.0, -0.01, M_PI, param_init);

//     double params[num_params];
//     memcpy(params, &params_init[0], params_init.size() * sizeof(double));
//     Eigen::Matrix2d inv_distortion = distortion.inverse();
//     // std::copy(std::begin(params_init), std::end(params_init), std::begin(params));

//     // Data is a row-major array of kGridRows x kGridCols values of function
//     // f(x, y) on the grid, with x in {-kGridColsHalf, ..., +kGridColsHalf},
//     // and y in {-kGridRowsHalf, ..., +kGridRowsHalf}
//     double *kde_data = new double[p_c.size()];
//     memcpy(kde_data, &p_c[0], p_c.size() * sizeof(double));

//     // unable to set coordinate to 2D grid for corresponding interpolator;
//     // use post-processing to scale the grid instead.
//     const ceres::Grid2D<double> kde_grid(kde_data, 0, cam.kdeRows, 0, cam.kdeCols);
//     const ceres::BiCubicInterpolator<ceres::Grid2D<double>> kde_interpolator(kde_grid);

//     // Ceres Problem
//     // ceres::LocalParameterization * q_parameterization = new ceres::EigenQuaternionParameterization();
//     ceres::Problem problem;

//     // problem.AddParameterBlock(params, 4, q_parameterization);
//     // problem.AddParameterBlock(params + 4, num_params - 4);
//     problem.AddParameterBlock(params, num_q);
//     problem.AddParameterBlock(params + num_q, num_params - num_q);
//     ceres::LossFunction *loss_function = new ceres::HuberLoss(0.05);

//     Eigen::Vector2d img_size = {cam.orgRows, cam.orgCols};
//     for (int i = 0; i < lid.EdgeOrgCloud -> points.size(); ++i)
//     {
//         // Eigen::Vector3d p_l_tmp = p_l.row(i);
//         Eigen::Vector3d p_l_tmp = {lid.EdgeOrgCloud -> points[i].x, lid.EdgeOrgCloud -> points[i].y, lid.EdgeOrgCloud -> points[i].z};
//         problem.AddResidualBlock(Calibration::Create(p_l_tmp, img_size, ref_val, kde_interpolator, inv_distortion),
//                                  loss_function,
//                                  params,
//                                  params + num_q);
//     }

//     for (int i = 0; i < num_params; ++i)
//     {
//         if (i < num_q)
//         {
//             problem.SetParameterLowerBound(params, i, lb[i]);
//             problem.SetParameterUpperBound(params, i, ub[i]);
//         }
//         else
//         {
//             problem.SetParameterLowerBound(params + num_q, i - num_q, lb[i]);
//             problem.SetParameterUpperBound(params + num_q, i - num_q, ub[i]);
//         }
//     }

//     ceres::Solver::Options options;
//     options.linear_solver_type = ceres::DENSE_SCHUR;
//     options.trust_region_strategy_type = ceres::LEVENBERG_MARQUARDT;
//     options.minimizer_progress_to_stdout = true;
//     options.num_threads = 12;
//     options.function_tolerance = 1e-7;
//     options.use_nonmonotonic_steps = true;

//     // OutputCallback callback(params);
//     // options.callbacks.push_back(&callback);

//     ceres::Solver::Summary summary;

//     ceres::Solve(options, &problem, &summary);

//     std::cout << summary.FullReport() << "\n";
//     customOutput(name, params, params_init);
//     std::vector<double> params_res(params, params + sizeof(params) / sizeof(double));

//     vector<vector<double>> lidProjection = lid.edgeVizTransform(params_res, distortion);
//     string lidEdgeTransTxtPath = lid.scenesFilePath[lid.scIdx].EdgeTransTxtPath;
//     fusionViz(cam, lidEdgeTransTxtPath, lidProjection, bandwidth);

//     return params_res;
// }