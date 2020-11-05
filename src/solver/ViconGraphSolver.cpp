/**
 * MIT License
 * Copyright (c) 2018 Patrick Geneva @ University of Delaware (Robot Perception & Navigation Group)
 * Copyright (c) 2018 Kevin Eckenhoff @ University of Delaware (Robot Perception & Navigation Group)
 * Copyright (c) 2018 Guoquan Huang @ University of Delaware (Robot Perception & Navigation Group)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */




#include "ViconGraphSolver.h"



/**
 * Default constructor.
 * Will load all needed configuration variables from the launch file and construct the graph.
 */
ViconGraphSolver::ViconGraphSolver(ros::NodeHandle& nh, std::shared_ptr<Propagator> propagator,
                                   std::shared_ptr<Interpolator> interpolator, std::vector<double> timestamp_cameras) {

    // save measurement data
    this->propagator = propagator;
    this->interpolator = interpolator;
    this->timestamp_cameras = timestamp_cameras;

    // Initalize our graphs
    this->graph = new gtsam::NonlinearFactorGraph();

    // Load gravity in vicon frame
    std::vector<double> vec_gravity;
    std::vector<double> vec_gravity_default = {0.0,0.0,9.8};
    nh.param<std::vector<double>>("grav_inV", vec_gravity, vec_gravity_default);
    init_grav_inV << vec_gravity.at(0),vec_gravity.at(1),vec_gravity.at(2);

    // Load transform between vicon body frame to the IMU
    std::vector<double> R_BtoI;
    std::vector<double> R_BtoI_default = {1.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0};
    nh.param<std::vector<double>>("R_BtoI", R_BtoI, R_BtoI_default);
    init_R_BtoI << R_BtoI.at(0),R_BtoI.at(1),R_BtoI.at(2),R_BtoI.at(3),R_BtoI.at(4),R_BtoI.at(5),R_BtoI.at(6),R_BtoI.at(7),R_BtoI.at(8);

    std::vector<double> p_BinI;
    std::vector<double> p_BinI_default = {0.0,0.0,0.0};
    nh.param<std::vector<double>>("p_BinI", p_BinI, p_BinI_default);
    init_p_BinI << p_BinI.at(0),p_BinI.at(1),p_BinI.at(2);

    // Time offset between imu and vicon
    nh.param<double>("toff_imu_to_vicon", init_toff_imu_to_vicon, 0.0);

    // Debug print to console
    cout << "init_grav_inV:" << endl << init_grav_inV << endl;
    cout << "init_R_BtoI:" << endl << init_R_BtoI << endl;
    cout << "init_p_BinI:" << endl << init_p_BinI << endl;
    cout << "init_toff_imu_to_vicon:" << endl << init_toff_imu_to_vicon << endl;


    // See if we should enforce gravity
    nh.param<bool>("enforce_grav_mag", enforce_grav_mag, false);
    cout << "enforce_grav_mag: " << (int)enforce_grav_mag << endl;

    // See if we should estimate time offset
    nh.param<bool>("estimate_toff_vicon_to_imu", estimate_toff_vicon_to_imu, false);
    cout << "estimate_toff_vicon_to_imu: " << (int)estimate_toff_vicon_to_imu << endl;

    // Number of times we relinearize
    nh.param<int>("num_loop_relin", num_loop_relin, 0);
    cout << "num_loop_relin: " << num_loop_relin << endl;


}




/**
 * This will first build the graph problem and then solve it
 * This function will take a while, but handles the GTSAM optimization
 */
void ViconGraphSolver::build_and_solve() {

    // Ensure we have enough measurements
    if(timestamp_cameras.empty()) {
        ROS_ERROR("[VICON-GRAPH]: Camera timestamp vector empty!!!!");
        ROS_ERROR("[VICON-GRAPH]: Make sure your camera topic is correct...");
        ROS_ERROR("%s on line %d",__FILE__,__LINE__);
        std::exit(EXIT_FAILURE);
    }

    // Delete all camera measurements that occur before our IMU readings
    ROS_INFO("cleaning camera timestamps");
    auto it0 = timestamp_cameras.begin();
    while(it0 != timestamp_cameras.end()) {
        if(!propagator->has_bounding_imu(*it0)) {
            ROS_INFO("    - deleted cam time %.9f",*it0);
            it0 = timestamp_cameras.erase(it0);
        } else {
            it0++;
        }
    }

    // Ensure we have enough measurements after removing invalid
    if(timestamp_cameras.empty()) {
        ROS_ERROR("[VICON-GRAPH]: All camera timestamps where out of the range of the IMU measurements.");
        ROS_ERROR("[VICON-GRAPH]: Make sure your camera and imu topics are correct...");
        ROS_ERROR("%s on line %d",__FILE__,__LINE__);
        std::exit(EXIT_FAILURE);
    }

    // Clear old states
    map_states.clear();
    values.clear();

    // Create map of the state timestamps to their IDs
    for(size_t i=0; i<timestamp_cameras.size(); i++) {
        map_states.insert({timestamp_cameras.at(i),i});
    }

    // Loop a specified number of times, and keep solving the problem
    // One would want this if you want to relinearize the bias estimates in CPI
    for(int i=0; i<=num_loop_relin; i++) {

        // Build the problem
        build_problem(i==0);

        // optimize the graph.
        optimize_problem();

        // move values forward in time
        values = values_result;

        // Debug print what the current time offset is
        if(estimate_toff_vicon_to_imu) ROS_INFO("current t_off = %.3f",values.at<Vector1>(T(0))(0));

        // Now print timing statistics
        ROS_INFO("\u001b[34m[TIME]: %.4f to build\u001b[0m",(rT2-rT1).total_microseconds() * 1e-6);
        ROS_INFO("\u001b[34m[TIME]: %.4f to optimize\u001b[0m",(rT3-rT2).total_microseconds() * 1e-6);
        ROS_INFO("\u001b[34m[TIME]: %.4f total (loop %d)\u001b[0m",(rT3-rT1).total_microseconds() * 1e-6,i);

    }


    // Debug print results...
    cout << endl << "======================================" << endl;
    cout << "state_0: " << endl << values_result.at<JPLNavState>(X(map_states[timestamp_cameras.at(0)])) << endl;
    cout << "state_N: " << endl << values_result.at<JPLNavState>(X(map_states[timestamp_cameras.at(timestamp_cameras.size()-1)])) << endl;
    cout << "R_BtoI: " << endl << values_result.at<Rot3>(C(0)).matrix() << endl << endl;
    cout << "p_BinI: " << endl << values_result.at<Vector3>(C(1)) << endl << endl;
    cout << "gravity: " << endl << values_result.at<Vector3>(G(0)) << endl << endl;
    cout << "gravity norm: " << endl << values_result.at<Vector3>(G(0)).norm() << endl << endl;
    if(estimate_toff_vicon_to_imu) cout << "t_off_vicon_to_imu: " << endl << values_result.at<Vector1>(T(0)) << endl << endl;
    else cout << "t_off_vicon_to_imu: " << endl << 0.0000 << endl << endl;
    cout << "======================================" << endl << endl;


}





/**
 * This will export the estimate IMU states and final information to file
 * The CSV file will be in the eth format:
 * (time(ns),px,py,pz,qw,qx,qy,qz,vx,vy,vz,bwx,bwy,bwz,bax,bay,baz)
 */
void ViconGraphSolver::write_to_file(std::string csvfilepath, std::string infofilepath) {

    // Debug info
    ROS_INFO("saving states and info to file");

    // If the file exists, then delete it
    if (boost::filesystem::exists(csvfilepath)) {
        boost::filesystem::remove(csvfilepath);
        ROS_INFO("    - old state file found, deleted...");
    }
    if (boost::filesystem::exists(infofilepath)) {
        boost::filesystem::remove(infofilepath);
        ROS_INFO("    - old info file found, deleted...");
    }
    // Create the directory that we will open the file in
    boost::filesystem::path p1(csvfilepath);
    boost::filesystem::create_directories(p1.parent_path());
    boost::filesystem::path p2(infofilepath);
    boost::filesystem::create_directories(p2.parent_path());

    // Open our state file!
    std::ofstream of_state;
    of_state.open(csvfilepath, std::ofstream::out | std::ofstream::app);
    of_state << "#time(ns),px,py,pz,qw,qx,qy,qz,vx,vy,vz,bwx,bwy,bwz,bax,bay,baz" << std::endl;

    // Loop through all states, and
    for(size_t i=0; i<timestamp_cameras.size(); i++) {
        // get this state at this timestep
        JPLNavState state = values_result.at<JPLNavState>(X(map_states[timestamp_cameras.at(i)]));
        // export to file
        // (time(ns),px,py,pz,qw,qx,qy,qz,vx,vy,vz,bwx,bwy,bwz,bax,bay,baz)
        of_state << std::setprecision(20) << std::floor(1e9*timestamp_cameras.at(i)) << ","
                 << std::setprecision(6)
                 << state.p()(0,0) << ","<< state.p()(1,0) << ","<< state.p()(2,0) << ","
                 << state.q()(3,0) << "," << state.q()(0,0) << "," << state.q()(1,0) << "," << state.q()(2,0) << ","
                 << state.v()(0,0) << "," << state.v()(1,0) << "," << state.v()(2,0) << ","
                 << state.bg()(0,0) << "," << state.bg()(1,0) << "," << state.bg()(2,0) << ","
                 << state.ba()(0,0) << "," << state.ba()(1,0) << "," << state.ba()(2,0) << std::endl;
    }
    of_state.close();


    // Save calibration and the such to file
    std::ofstream of_info;
    of_info.open(infofilepath, std::ofstream::out | std::ofstream::app);
    of_info << "R_BtoI: " << endl << values_result.at<Rot3>(C(0)).matrix() << endl << endl ;
    of_info << "q_BtoI: " << endl << rot_2_quat(values_result.at<Rot3>(C(0)).matrix()) << endl << endl;
    of_info << "p_BinI: " << endl << values_result.at<Vector3>(C(1)) << endl << endl;
    of_info << "gravity: " << endl << values_result.at<Vector3>(G(0)) << endl << endl;
    of_info << "gravity norm: " << endl << values_result.at<Vector3>(G(0)).norm() << endl << endl;
    if(estimate_toff_vicon_to_imu) of_info << "t_off_vicon_to_imu: " << endl << values_result.at<Vector1>(T(0)) << endl << endl;
    else of_info << "t_off_vicon_to_imu: " << endl << 0.0000 << endl << endl;
    of_info.close();

}




/**
 * This will build the graph problem and add all measurements and nodes to it
 * Given the first time, we init the states using the VICON, but in the future we keep them
 * And only re-linearize measurements (i.e. for the preintegration biases)
 */
void ViconGraphSolver::build_problem(bool init_states) {

    // Start timing
    rT1 =  boost::posix_time::microsec_clock::local_time();

    // Clear the old factors
    ROS_INFO("[BUILD]: building the graph (might take a while)");
    graph->erase(graph->begin(), graph->end());

    // Create gravity and calibration nodes and insert them
    if(init_states) {
        values.insert(C(0), Rot3(init_R_BtoI));
        values.insert(C(1), Vector3(init_p_BinI));
        values.insert(G(0), Vector3(init_grav_inV));
    }

    // If estimating the timeoffset logic
    if(estimate_toff_vicon_to_imu) {
        // Add value if first time
        if(init_states) {
            Vector1 temp;
            temp(0) = init_toff_imu_to_vicon;
            values.insert(T(0), temp);
        }
        // Prior to make time offset stable
        Vector1 sigma;
        sigma(0,0) = 0.02; // seconds
        PriorFactor<Vector1> factor_timemag(T(0), values.at<Vector1>(T(0)), sigma);
        graph->add(factor_timemag);
        ROS_INFO("[BUILD]: current time offset is %.4f", values.at<Vector1>(T(0))(0));
    }

    // If enforcing gravity magnitude, then add that prior factor here
    if(enforce_grav_mag) {
        Vector1 sigma;
        sigma(0,0) = 1e-10;
        MagnitudePrior factor_gav(G(0),sigma,init_grav_inV.norm());
        graph->add(factor_gav);
    } else {
        ROS_INFO("[BUILD]: current gravity mag is %.4f", values.at<Vector3>(G(0)).norm());
    }

    // Loop through each camera time and construct the graph
    auto it1 = timestamp_cameras.begin();
    while(it1 != timestamp_cameras.end()) {

        // If ros is wants us to stop, break out
        if (!ros::ok())
            break;

        // Current image time
        double timestamp = *it1;
        double timestamp_corrected = (estimate_toff_vicon_to_imu)? timestamp+values.at<Vector1>(T(0))(0) : timestamp+init_toff_imu_to_vicon;

        // First get the vicon pose at the current time
        Eigen::Matrix<double,4,1> q_VtoB;
        Eigen::Matrix<double,3,1> p_BinV;
        Eigen::Matrix<double,6,6> R_vicon;
        bool has_vicon1 = interpolator->get_pose(timestamp_corrected-1.0,q_VtoB,p_BinV,R_vicon);
        bool has_vicon2 = interpolator->get_pose(timestamp_corrected+1.0,q_VtoB,p_BinV,R_vicon);
        bool has_vicon3 = interpolator->get_pose(timestamp_corrected,q_VtoB,p_BinV,R_vicon);

        // Skip if we don't have a vicon measurement for this pose
        if(!has_vicon1 || !has_vicon2 || !has_vicon3) {
            ROS_INFO("    - skipping camera time %.9f (no vicon pose found)",timestamp);
            if(values.find(X(map_states[timestamp]))!=values.end()) {
                values.erase(X(map_states[timestamp]));
            }
            it1 = timestamp_cameras.erase(it1);
            continue;
        }

        // Check if we can do the inverse
        if(std::isnan(R_vicon.norm()) || std::isnan(R_vicon.inverse().norm())) {
            ROS_INFO("    - skipping camera time %.9f (R.norm = %.3f | Rinv.norm = %.3f)",timestamp,R_vicon.norm(),R_vicon.inverse().norm());
            if(values.find(X(map_states[timestamp]))!=values.end()) {
                values.erase(X(map_states[timestamp]));
            }
            it1 = timestamp_cameras.erase(it1);
            continue;
        }

        // Now initialize the current pose of the IMU
        if(init_states) {
            Eigen::Matrix<double,4,1> q_VtoI = quat_multiply(rot_2_quat(init_R_BtoI),q_VtoB);
            Eigen::Matrix<double,3,1> bg = Eigen::Matrix<double,3,1>::Zero();
            Eigen::Matrix<double,3,1> v_IinV = Eigen::Matrix<double,3,1>::Zero();
            Eigen::Matrix<double,3,1> ba = Eigen::Matrix<double,3,1>::Zero();
            Eigen::Matrix<double,3,1> p_IinV = p_BinV - quat_2_Rot(Inv(q_VtoB))*init_R_BtoI.transpose()*init_p_BinI;
            JPLNavState imu_state(q_VtoI, bg, v_IinV, ba, p_IinV);
            values.insert(X(map_states[timestamp]), imu_state);
        }

        // Add the vicon measurement to this pose
        if(!estimate_toff_vicon_to_imu) {
            ViconPoseFactor factor_vicon(X(map_states[timestamp]),C(0),C(1),R_vicon,q_VtoB,p_BinV);
            graph->add(factor_vicon);
        } else {
            ViconPoseTimeoffsetFactor factor_vicon(X(map_states[timestamp]),C(0),C(1),T(0),timestamp,interpolator);
            graph->add(factor_vicon);
        }

        // Skip the first ever pose
        if(it1 == timestamp_cameras.begin()) {
            it1++;
            continue;
        }

        // Now add preintegration between this state and the next
        // We do a silly hack since inside of the propagator we create the preintegrator
        // So we just randomly assign noises here which will be overwritten in the propagator
        double time0 = *(it1-1);
        double time1 = *(it1);

        // Get the bias of the time0 state
        Bias3 bg = values.at<JPLNavState>(X(map_states[time0])).bg();
        Bias3 ba = values.at<JPLNavState>(X(map_states[time0])).ba();

        // Get the preintegrator
        CpiV1 preint(0,0,0,0,false);
        bool has_imu = propagator->propagate(time0,time1,bg,ba,preint);
        assert(has_imu);
        assert(preint.DT==(time1-time0));

        //cout << "dt = " << preint.DT << " | dt_times = " << time1-time0 << endl;
        //cout << "q_k2tau = " << preint.q_k2tau.transpose() << endl;
        //cout << "alpha_tau = " << preint.alpha_tau.transpose() << endl;
        //cout << "beta_tau = " << preint.beta_tau.transpose() << endl;

        // Check if we can do the inverse
        if(std::isnan(preint.P_meas.norm()) || std::isnan(preint.P_meas.inverse().norm())) {
            ROS_ERROR("R_imu is NAN | R.norm = %.3f | Rinv.norm = %.3f",preint.P_meas.norm(),preint.P_meas.inverse().norm());
            ROS_ERROR("THIS SHOULD NEVER HAPPEN!@#!@#!@#!@#!#@");
        }

        // Now create the IMU factor
        ImuFactorCPIv1 factor_imu(X(map_states[time0]),X(map_states[time1]),G(0),preint.P_meas,preint.DT,preint.alpha_tau,preint.beta_tau,
                                  preint.q_k2tau,preint.b_a_lin,preint.b_w_lin,preint.J_q,preint.J_b,preint.J_a,preint.H_b,preint.H_a);
        graph->add(factor_imu);

        // Finally, move forward in time!
        it1++;

    }
    rT2 =  boost::posix_time::microsec_clock::local_time();



}




/**
 * This will optimize the graph.
 * Uses Levenberg-Marquardt for the optimization.
 */
void ViconGraphSolver::optimize_problem() {

    // Debug
    ROS_INFO("[VICON-GRAPH]: graph factors - %d", (int) graph->nrFactors());
    ROS_INFO("[VICON-GRAPH]: graph nodes - %d", (int) graph->keys().size());

    // Setup the optimizer (levenberg)
    LevenbergMarquardtParams config;
    config.verbosity = NonlinearOptimizerParams::Verbosity::TERMINATION;
    //config.verbosityLM = LevenbergMarquardtParams::VerbosityLM::SUMMARY;
    //config.verbosityLM = LevenbergMarquardtParams::VerbosityLM::TERMINATION;
    config.absoluteErrorTol = 1e-30;
    config.relativeErrorTol = 1e-30;
    config.lambdaUpperBound = 1e20;
    config.maxIterations = 20;
    LevenbergMarquardtOptimizer optimizer(*graph, values, config);

    // Setup optimizer (dogleg)
    //DoglegParams params;
    //params.verbosity = NonlinearOptimizerParams::Verbosity::TERMINATION;
    //params.relativeErrorTol = 1e-10;
    //params.absoluteErrorTol = 1e-10;
    //DoglegOptimizer optimizer(*graph, values, params);

    // Perform the optimization
    ROS_INFO("[VICON-GRAPH]: begin optimization");
    values_result = optimizer.optimize();
    ROS_INFO("[VICON-GRAPH]: done optimization (%d iterations)!", (int) optimizer.iterations());
    rT3 = boost::posix_time::microsec_clock::local_time();

}







