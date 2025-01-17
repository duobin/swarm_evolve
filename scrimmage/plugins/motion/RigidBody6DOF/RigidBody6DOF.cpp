/// ---------------------------------------------------------------------------
/// @section LICENSE
///
/// Copyright (c) 2016 Georgia Tech Research Institute (GTRI)
///               All Rights Reserved
///
/// The above copyright notice and this permission notice shall be included in
/// all copies or substantial portions of the Software.
///
/// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
/// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
/// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
/// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
/// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
/// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
/// DEALINGS IN THE SOFTWARE.
/// ---------------------------------------------------------------------------
/// @file filename.ext
/// @author Kevin DeMarco <kevin.demarco@gtri.gatech.edu>
/// @author Eric Squires <eric.squires@gtri.gatech.edu>
/// @version 1.0
/// ---------------------------------------------------------------------------
/// @brief A brief description.
///
/// @section DESCRIPTION
/// A long description.
/// ---------------------------------------------------------------------------
#include "RigidBody6DOF.h"
#include <scrimmage/common/Utilities.h>
#include <scrimmage/parse/ParseUtils.h>
#include <scrimmage/plugin_manager/RegisterPlugin.h>
#include <scrimmage/math/Angles.h>
#include <boost/algorithm/clamp.hpp>
#include <scrimmage/entity/Entity.h>

using boost::algorithm::clamp;

REGISTER_PLUGIN(scrimmage::MotionModel, RigidBody6DOF, RigidBody6DOF_plugin)

namespace sc = scrimmage;
namespace pl = std::placeholders;

enum ModelParams
{
    U = 0,
    V,
    W,
    P,
    Q,
    R,
    Uw,
    Vw,
    Ww,
    Xw,
    Yw,
    Zw,
    q0,
    q1,
    q2,
    q3,
    MODEL_NUM_ITEMS
};

enum ControlParams
{
    THRUST = 0,
    ELEVATOR,
    AILERON,
    RUDDER,
    CONTROL_NUM_ITEMS
};

RigidBody6DOF::RigidBody6DOF()
{
    x_.resize(MODEL_NUM_ITEMS);
}

std::tuple<int,int,int> RigidBody6DOF::version()
{
    return std::tuple<int,int,int>(0,0,1);
}

bool RigidBody6DOF::init(std::map<std::string, std::string> &info,
                          std::map<std::string, std::string> &params)
{
    Eigen::Vector3d &pos = state_->pos();
    quat_world_ = state_->quat();

    min_velocity_ = sc::get("min_velocity", params, 15.0);
    max_velocity_ = sc::get("max_velocity", params, 40.0);
    max_roll_ = sc::Angles::deg2rad(sc::get("max_roll", params, 30.0));
    max_pitch_ = sc::Angles::deg2rad(sc::get("max_pitch", params, 30.0));

    x_[U] = 0;
    x_[V] = 0;
    x_[W] = 0;

    x_[P] = 0;
    x_[Q] = 0;
    x_[R] = 0;

    x_[Uw] = 0;
    x_[Vw] = 0;
    x_[Ww] = 0;

    x_[Xw] = pos(0);
    x_[Yw] = pos(1);
    x_[Zw] = pos(2);

    // Initial Local orientation (no rotation)
    x_[q0] = 1;
    x_[q1] = 0;
    x_[q2] = 0;
    x_[q3] = 0;

    return true;
}

bool RigidBody6DOF::step(double time, double dt)
{
    if (u_ == nullptr) {
        std::shared_ptr<Controller> ctrl =
            std::dynamic_pointer_cast<Controller>(parent_.lock()->controllers().back());
        if (ctrl) {
            u_ = ctrl->u();
        }
    }

    if (u_ == nullptr) {
        return false;
    }

    // Need to saturate state variables before model runs
    x_[U] = clamp(x_[U], min_velocity_, max_velocity_);

    x_[P] = clamp(x_[U], -0.001, 0.001);
    x_[Q] = clamp(x_[U], -0.001, 0.001);
    x_[R] = clamp(x_[U], -0.001, 0.001);

    ode_step(dt);

    // Normalize quaternion
    quat_local_.w() = x_[q0];
    quat_local_.x() = x_[q1];
    quat_local_.y() = x_[q2];
    quat_local_.z() = x_[q3];
    quat_local_.normalize();

    x_[q0] = quat_local_.w();
    x_[q1] = quat_local_.x();
    x_[q2] = quat_local_.y();
    x_[q3] = quat_local_.z();

    // Convert local coordinates to world coordinates
    state_->quat() = quat_world_ * quat_local_;
    state_->pos() << x_[Xw], x_[Yw], x_[Zw];

    return true;
}

void RigidBody6DOF::model(const vector_t &x , vector_t &dxdt , double t)
{
    double thrust = (*u_)(THRUST);
    double elevator = (*u_)(ELEVATOR);
    double aileron = (*u_)(AILERON);
    double rudder = (*u_)(RUDDER);

    double F_thrust = thrust;

    double F_x = F_thrust;
    //double F_y = 0;
    //double F_z = 0;

    double m = 1.0;
    double g = 0;//-9.8; no gravity
    double theta = 0;
    //double phi = 0;

    double Ixx = 1;
    double Iyy = 1;
    double Izz = 1;
    double Ixz = 0;

    // TODO: Should these be cached from previous run or should the current dxdt
    // versions be used?
    double U_dot = dxdt[U];
    double V_dot = dxdt[V];
    double W_dot = dxdt[W];
    double P_dot = dxdt[P];
    double Q_dot = dxdt[Q];
    double R_dot = dxdt[R];

    dxdt[U] = x[V]*x[R] - x[W]*x[Q] - g*sin(theta) + F_x / m;
    dxdt[V] = 0;//x[W]*x[P] - x[U]*x[R] + g*sin(phi)*cos(theta) + F_y / m;
    dxdt[W] = 0;//x[U]*x[Q] - x[V]*x[P] + g*cos(phi)*cos(theta) + F_z / m;

    double L = aileron + Ixx*P_dot - Ixz*R_dot - Ixz*x[P]*x[Q] + (Izz - Iyy)*x[R]*x[Q];
    double M = elevator + Iyy*Q_dot + (Ixx-Izz)*x[P]*x[R] + (Ixz*(pow(x[P],2)-pow(x[R],2)));
    double N = rudder + Izz*R_dot - Ixz*P_dot + (Iyy-Ixx)*x[P]*x[Q] + Ixz*x[Q]*x[R];

    double L_tic = L + Ixz*x[P]*x[Q] - (Izz-Iyy)*x[R]*x[Q];
    double N_tic = N - (Iyy-Ixx)*x[P]*x[Q] - Ixz*x[R]*x[Q];

    dxdt[P] = (L_tic*Izz-N_tic*Ixz) / (Ixx*Izz - pow(Ixz,2));
    dxdt[Q] = (M - (Ixx - Izz)*x[P]*x[R]) - Ixz*(pow(x[P],2)-pow(x[R],2))/Iyy;
    dxdt[R] = (N_tic*Ixx+L_tic*Ixz) / (Ixx*Izz - pow(Ixz,2));

    double lambda = 1 - (pow(x[q0],2) + pow(x[q1],2) + pow(x[q2],2) + pow(x[q3],2));
    dxdt[q0] = -0.5 * (x[q1]*x[P] + x[q2]*x[Q] + x[q3]*x[R]) + lambda * x[q0];
    dxdt[q1] = +0.5 * (x[q0]*x[P] + x[q2]*x[R] - x[q3]*x[Q]) + lambda * x[q1];
    dxdt[q2] = +0.5 * (x[q0]*x[Q] + x[q3]*x[P] - x[q1]*x[R]) + lambda * x[q2];
    dxdt[q3] = +0.5 * (x[q0]*x[R] + x[q1]*x[Q] - x[q2]*x[P]) + lambda * x[q3];

    // Local position / velocity to global
    // Normalize quaternion
    sc::Quaternion quat(x[q0], x[q1], x[q2], x[q3]);
    quat.w() = x[q0];
    quat.x() = x[q1];
    quat.y() = x[q2];
    quat.z() = x[q3];

    quat = quat_world_ * quat;
    quat.normalize();

    // Convert local positions and velocities into global coordinates
    Eigen::Matrix3d R = quat.toRotationMatrix();

    Eigen::Vector3d vel_local(x[U], x[V], x[W]);
    Eigen::Vector3d vel_world = R * vel_local;
    dxdt[Xw] = vel_world(0);
    dxdt[Yw] = vel_world(1);
    dxdt[Zw] = vel_world(2);

    Eigen::Vector3d acc_local(U_dot, V_dot, W_dot);
    Eigen::Vector3d acc_world = R * acc_local;
    dxdt[Uw] = acc_world(0);
    dxdt[Vw] = acc_world(1);
    dxdt[Ww] = acc_world(2);
}

void RigidBody6DOF::teleport(sc::StatePtr &state)
{
    //x_[X] = state->pos()[0];
    //x_[Y] = state->pos()[1];
    //x_[Z] = state->pos()[2];
    //x_[ROLL] = state->quat().roll();
    //x_[PITCH] = state->quat().pitch();
    //x_[YAW] = state->quat().yaw();
    //x_[SPEED] = state->vel()[0];
}


/*
double q_0 = quat_local_.w();
double q_1 = quat_local_.x();
double q_2 = quat_local_.y();
double q_3 = quat_local_.z();

double R_00 = pow(q_0,2) + pow(q_1,2) - pow(q_2,2) - pow(q_3,2);
double R_01 = 2*(q_1*q_2 - q_0*q_3);
double R_02 = 2*(q_0*q_2 + q_1*q_3);

double R_10 = 2*(q_1*q_2 + q_0*q_3);
double R_11 = pow(q_0,2) - pow(q_1,2) + pow(q_2,2) - pow(q_3,2);
double R_12 = 2*(q_2*q_3 - q_0*q_1);

double R_20 = 2*(q_1*q_3 - q_0*q_2);
double R_21 = 2*(q_2*q_3 + q_0*q_1);
double R_22 = pow(q_0,2) - pow(q_1,2) - pow(q_2,2) + pow(q_3,2);

Eigen::Matrix3d R;
R << R_00, R_01, R_02,                      \
    R_10, R_11, R_12,                       \
    R_20, R_21, R_22;
cout << "R= " << endl << R << endl;
*/
