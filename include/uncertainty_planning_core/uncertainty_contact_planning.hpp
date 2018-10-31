#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <chrono>
#include <random>
#include <mutex>
#include <thread>
#include <atomic>
#include <common_robotics_utilities/color_builder.hpp>
#include <common_robotics_utilities/math.hpp>
#include <common_robotics_utilities/zlib_helpers.hpp>
#include <common_robotics_utilities/math.hpp>
#include <common_robotics_utilities/simple_hierarchical_clustering.hpp>
#include <common_robotics_utilities/simple_hausdorff_distance.hpp>
#include <common_robotics_utilities/simple_rrt_planner.hpp>
#include <common_robotics_utilities/simple_robot_model_interface.hpp>
#include <uncertainty_planning_core/simple_sampler_interface.hpp>
#include <uncertainty_planning_core/simple_outcome_clustering_interface.hpp>
#include <uncertainty_planning_core/uncertainty_planner_state.hpp>
#include <uncertainty_planning_core/simple_simulator_interface.hpp>
#include <uncertainty_planning_core/execution_policy.hpp>
#include <ros/ros.h>
#include <visualization_msgs/MarkerArray.h>
#include <common_robotics_utilities/conversions.hpp>
#include <omp.h>

namespace uncertainty_planning_core
{
    template<typename Configuration,
             typename ConfigSerializer,
             typename ConfigAlloc = std::allocator<Configuration>,
             typename PRNG = std::mt19937_64>
    class UncertaintyPlanningSpace
    {
    protected:

        // Typedef so we don't hate ourselves
        typedef common_robotics_utilities::simple_robot_model_interface::SimpleRobotModelInterface<Configuration, ConfigAlloc> Robot;
        typedef UncertaintyPlannerState<Configuration, ConfigSerializer, ConfigAlloc> UncertaintyPlanningState;
        typedef ExecutionPolicy<Configuration, ConfigSerializer, ConfigAlloc> UncertaintyPlanningPolicy;
        typedef common_robotics_utilities::simple_rrt_planner::SimpleRRTPlannerState<UncertaintyPlanningState> UncertaintyPlanningTreeState;
        typedef std::vector<UncertaintyPlanningTreeState> UncertaintyPlanningTree;
        typedef common_robotics_utilities::simple_graph::Graph<UncertaintyPlanningState> ExecutionPolicyGraph;
        typedef std::shared_ptr<Robot> RobotPtr;
        typedef std::shared_ptr<SimpleSamplerInterface<Configuration, PRNG>> SamplingPtr;
        typedef std::shared_ptr<SimpleSimulatorInterface<Configuration, PRNG, ConfigAlloc>> SimulatorPtr;
        typedef std::shared_ptr<SimpleOutcomeClusteringInterface<Configuration, ConfigAlloc>> ClusteringPtr;

        typedef std::function<void(const visualization_msgs::MarkerArray&)> DisplayFn;
        typedef std::function<void(const std::string&, const int32_t)> LoggingFn;
        typedef std::function<double(const UncertaintyPlanningState&, const UncertaintyPlanningState&)> DistanceFn;
        typedef std::function<int64_t(const UncertaintyPlanningTree&, const UncertaintyPlanningState&)> NearestNeighborFn;
        typedef std::function<std::vector<std::pair<UncertaintyPlanningState, int64_t>>(const UncertaintyPlanningState&, const UncertaintyPlanningState&)> ForwardPropagationFn;
        typedef std::function<double(const UncertaintyPlanningState&)> GoalReachedProbabilityFn;
        typedef std::function<bool(const UncertaintyPlanningState&)> PlanningStateGoalCheckFn;
        typedef std::function<bool(const Configuration&)> ConfigGoalCheckFn;

        typedef std::function<std::vector<Configuration, ConfigAlloc>(const Configuration&, const Configuration&, const Configuration&, const bool, const bool)> ExecutionMovementFn;

        typedef std::map<std::string, double> Statistics;

        size_t num_particles_;
        double step_size_;
        double step_duration_;
        double goal_distance_threshold_;
        double goal_probability_threshold_;
        double feasibility_alpha_;
        double variance_alpha_;
        double connect_after_first_solution_;
        int32_t debug_level_;
        RobotPtr robot_ptr_;
        SamplingPtr sampler_ptr_;
        SimulatorPtr simulator_ptr_;
        ClusteringPtr clustering_ptr_;
        uint64_t state_counter_;
        uint64_t transition_id_;
        uint64_t split_id_;
        uint64_t particles_stored_;
        uint64_t particles_simulated_;
        uint64_t goal_candidates_evaluated_;
        uint64_t goal_reaching_performed_;
        uint64_t goal_reaching_successful_;
        double total_goal_reached_probability_;
        double time_to_first_solution_;
        double elapsed_clustering_time_;
        double elapsed_simulation_time_;
        UncertaintyPlanningTree nearest_neighbors_storage_;
        LoggingFn logging_fn_;

        inline static size_t GetNumOMPThreads()
        {
            #if defined(_OPENMP)
            size_t num_threads = 0;
            #pragma omp parallel
            {
                num_threads = (size_t)omp_get_num_threads();
            }
            return num_threads;
            #else
            return 1;
            #endif
        }

        /*
         * Private helper function - needs well-formed inputs, so it isn't safe to expose to external code
         */
        inline static void ExtractChildStates(
                const UncertaintyPlanningTree& raw_planner_tree,
                const int64_t raw_parent_index,
                const int64_t pruned_parent_index,
                UncertaintyPlanningTree& pruned_planner_tree)
        {
            if (raw_planner_tree.at((size_t)raw_parent_index).IsInitialized() == false)
            {
                throw std::invalid_argument("raw_parent_state is uninitialized");
            }
            if (pruned_planner_tree.at((size_t)pruned_parent_index).IsInitialized() == false)
            {
                throw std::invalid_argument("pruned_parent_state is uninitialized");
            }
            // Clear the child indices, so we can update them with new values later
            pruned_planner_tree[(size_t)pruned_parent_index].ClearChildIndicies();
            const std::vector<int64_t>& current_child_indices = raw_planner_tree[(size_t)raw_parent_index].GetChildIndices();
            for (size_t idx = 0; idx < current_child_indices.size(); idx++)
            {
                const int64_t raw_child_index = current_child_indices[idx];
                const UncertaintyPlanningTreeState& current_child_state = raw_planner_tree.at((size_t)raw_child_index);
                if (current_child_state.GetParentIndex() >= 0)
                {
                    // Get the new child index
                    const int64_t pruned_child_index = (int64_t)pruned_planner_tree.size();
                    // Add to the pruned tree
                    pruned_planner_tree.push_back(current_child_state);
                    // Update parent indices
                    pruned_planner_tree[(size_t)pruned_child_index].SetParentIndex(pruned_parent_index);
                    // Update the parent
                    pruned_planner_tree[(size_t)pruned_parent_index].AddChildIndex(pruned_child_index);
                    // Recursive call
                    ExtractChildStates(raw_planner_tree, raw_child_index, pruned_child_index, pruned_planner_tree);
                }
            }
        }

        void Log(const std::string& message, const int32_t level) const
        {
          logging_fn_(message, level);
        }

    public:

        EIGEN_MAKE_ALIGNED_OPERATOR_NEW

        /*
         * Constructor
         */
        inline UncertaintyPlanningSpace(
                const int32_t debug_level,
                const size_t num_particles,
                const double step_size,
                const double goal_distance_threshold,
                const double goal_probability_threshold,
                const double feasibility_alpha,
                const double variance_alpha,
                const double connect_after_first_solution,
                const std::shared_ptr<Robot>& robot,
                const SamplingPtr& sampler_ptr,
                const SimulatorPtr& simulator_ptr,
                const ClusteringPtr& clustering_ptr,
                const LoggingFn& logging_fn)
            : num_particles_(num_particles)
            , step_size_(step_size)
            , goal_distance_threshold_(goal_distance_threshold)
            , goal_probability_threshold_(goal_probability_threshold)
            , feasibility_alpha_(feasibility_alpha)
            , variance_alpha_(variance_alpha)
            , connect_after_first_solution_(connect_after_first_solution)
            , debug_level_(debug_level)
            , robot_ptr_(robot)
            , sampler_ptr_(sampler_ptr)
            , simulator_ptr_(simulator_ptr)
            , clustering_ptr_(clustering_ptr)
            , logging_fn_(logging_fn)
        {
            Reset();
        }

        inline void Reset()
        {
            state_counter_ = 0;
            transition_id_ = 0;
            split_id_ = 0;
            elapsed_clustering_time_ = 0.0;
            elapsed_simulation_time_ = 0.0;
            particles_stored_ = 0;
            particles_simulated_ = 0;
            goal_candidates_evaluated_ = 0;
            goal_reaching_performed_ = 0;
            goal_reaching_successful_ = 0;
            nearest_neighbors_storage_.clear();
        }

        /*
         * Test example to show the behavior of the lightweight simulator
         */
        inline ForwardSimulationStepTrace<Configuration, ConfigAlloc> DemonstrateSimulator(
                const Configuration& start,
                const Configuration& goal,
                const DisplayFn& display_fn) const
        {
            // Draw the simulation environment
            display_fn(MakeEnvironmentDisplayRep());
            // Draw the start and goal
            const std_msgs::ColorRGBA start_color = common_robotics_utilities::color_builder::MakeFromFloatColors<std_msgs::ColorRGBA>(1.0, 0.5, 0.0, 1.0);
            const std_msgs::ColorRGBA goal_color = common_robotics_utilities::color_builder::MakeFromFloatColors<std_msgs::ColorRGBA>(1.0, 0.0, 1.0, 1.0);
            const visualization_msgs::MarkerArray start_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, start, start_color, 1, "start_state");
            const visualization_msgs::MarkerArray goal_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, goal, goal_color, 1, "goal_state");
            visualization_msgs::MarkerArray simulator_start_goal_display_rep;
            simulator_start_goal_display_rep.markers.insert(simulator_start_goal_display_rep.markers.end(), start_markers.markers.begin(), start_markers.markers.end());
            simulator_start_goal_display_rep.markers.insert(simulator_start_goal_display_rep.markers.end(), goal_markers.markers.begin(), goal_markers.markers.end());
            display_fn(simulator_start_goal_display_rep);
            // Wait for input
            std::cout << "Press ENTER to solve..." << std::endl;
            std::cin.get();
            ForwardSimulationStepTrace<Configuration, ConfigAlloc> trace;
            simulator_ptr_->ForwardSimulateRobot(robot_ptr_, start, goal, true, trace, true, display_fn);
            // Wait for input
            std::cout << "Press ENTER to draw..." << std::endl;
            std::cin.get();
            if (debug_level_ >= 20)
            {
                // Draw the action
                const std_msgs::ColorRGBA free_color = common_robotics_utilities::color_builder::MakeFromFloatColors<std_msgs::ColorRGBA>(0.0, 1.0, 0.0, 1.0);
                const std_msgs::ColorRGBA colliding_color = common_robotics_utilities::color_builder::MakeFromFloatColors<std_msgs::ColorRGBA>(1.0, 0.0, 0.0, 1.0);
                const std_msgs::ColorRGBA control_input_color = common_robotics_utilities::color_builder::MakeFromFloatColors<std_msgs::ColorRGBA>(1.0, 1.0, 0.0, 1.0);
                const std_msgs::ColorRGBA control_step_color = common_robotics_utilities::color_builder::MakeFromFloatColors<std_msgs::ColorRGBA>(0.0, 1.0, 1.0, 1.0);
                // Keep track of previous position
                Configuration previous_config = start;
                for (size_t step_idx = 0; step_idx < trace.resolver_steps.size(); step_idx++)
                {
                    const ForwardSimulationResolverTrace<Configuration, ConfigAlloc>& step_trace = trace.resolver_steps[step_idx];
                    const Eigen::VectorXd& control_input_step = step_trace.control_input_step;
                    // Draw the control input for the entire trace segment
                    const Eigen::VectorXd& control_input = step_trace.control_input;
                    visualization_msgs::MarkerArray control_display_rep = simulator_ptr_->MakeControlInputDisplayRep(robot_ptr_, previous_config, control_input, control_input_color, 1, "control_input_state");
                    display_fn(control_display_rep);
                    for (size_t resolver_step_idx = 0; resolver_step_idx < step_trace.contact_resolver_steps.size(); resolver_step_idx++)
                    {
                        // Get the current trace segment
                        const ForwardSimulationContactResolverStepTrace<Configuration, ConfigAlloc>& contact_resolution_trace = step_trace.contact_resolver_steps[resolver_step_idx];
                        for (size_t contact_resolution_step_idx = 0; contact_resolution_step_idx < contact_resolution_trace.contact_resolution_steps.size(); contact_resolution_step_idx++)
                        {
                            const Configuration& current_config = contact_resolution_trace.contact_resolution_steps[contact_resolution_step_idx];
                            previous_config = current_config;
                            const std_msgs::ColorRGBA& current_color = (contact_resolution_step_idx == (contact_resolution_trace.contact_resolution_steps.size() - 1)) ? free_color : colliding_color;
                            const visualization_msgs::MarkerArray step_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, current_config, current_color, 1, "step_state_");
                            const visualization_msgs::MarkerArray control_step_markers = simulator_ptr_->MakeControlInputDisplayRep(robot_ptr_, current_config, -control_input_step, control_step_color, 1, "control_step_state");
                            visualization_msgs::MarkerArray simulator_step_display_rep;
                            simulator_step_display_rep.markers.insert(simulator_step_display_rep.markers.end(), step_markers.markers.begin(), step_markers.markers.end());
                            simulator_step_display_rep.markers.insert(simulator_step_display_rep.markers.end(), control_step_markers.markers.begin(), control_step_markers.markers.end());
                            display_fn(simulator_step_display_rep);
                            // Wait for input
                            //std::cout << "Press ENTER to continue..." << std::endl;
                            ros::Duration(0.05).sleep();
                            //std::cin.get();
                        }
                    }
                }
            }
            else
            {
                const std::vector<Configuration, ConfigAlloc> trajectory = ExtractTrajectoryFromTrace(trace);
                const double time_interval = 1.0 / 25.0;
                const uint32_t rand_suffix = std::uniform_int_distribution<uint32_t>(1, 1000000)(simulator_ptr_->GetRandomGenerator());
                const std::string ns = "simulator_test_" + std::to_string(rand_suffix);
                DrawParticlePolicyExecution(ns, trajectory, display_fn, time_interval, MakeColor(0.0f, 0.25f, 0.5f, 1.0f));
            }
            return trace;
        }

        /*
         * Nearest-neighbor state distance function
         */
        inline double StateDistance(
                const UncertaintyPlanningState& state1,
                const UncertaintyPlanningState& state2) const
        {
            // Get the "space independent" expectation distance
            const double expectation_distance = robot_ptr_->ComputeConfigurationDistance(state1.GetExpectation(), state2.GetExpectation()) / step_size_;
            // Get the Pfeasibility(start -> state1)
            const double feasibility_weight = (1.0 - state1.GetMotionPfeasibility()) * feasibility_alpha_ + (1.0 - feasibility_alpha_);
            // Get the "space independent" variance of state1
            const Eigen::VectorXd raw_variances = state1.GetSpaceIndependentVariances();
            const double raw_variance = raw_variances.lpNorm<1>();
            // Turn the variance into a weight
            const double variance_weight = erf(raw_variance) * variance_alpha_ + (1.0 - variance_alpha_);
            // Compute the actual distance
            const double distance = (feasibility_weight * expectation_distance * variance_weight);
            return distance;
        }

        /*
         * Helper for parallel-linear nearest-neighbors
         */
        static inline int64_t GetNearestNeighbor(
                const UncertaintyPlanningTree& planner_nodes,
                const UncertaintyPlanningState& random_state,
                const DistanceFn& state_distance_fn,
                const LoggingFn& logging_fn)
        {
            // Get the nearest neighbor (ignoring the disabled states)
            std::vector<std::pair<int64_t, double>> per_thread_bests(GetNumOMPThreads(), std::pair<int64_t, double>(-1, INFINITY));
            #pragma omp parallel for
            for (size_t idx = 0; idx < planner_nodes.size(); idx++)
            {
                const UncertaintyPlanningTreeState& current_state = planner_nodes[idx];
                // Only check against states enabled for NN checks
                if (current_state.GetValueImmutable().UseForNearestNeighbors())
                {
                    const double state_distance = state_distance_fn(current_state.GetValueImmutable(), random_state);
#if defined(_OPENMP)
                    const size_t current_thread_id = (size_t)omp_get_thread_num();
#else
                    const size_t current_thread_id = 0;
#endif
                    if (state_distance < per_thread_bests[current_thread_id].second)
                    {
                        per_thread_bests[current_thread_id].first = (int64_t)idx;
                        per_thread_bests[current_thread_id].second = state_distance;
                    }
                }
            }
            int64_t best_index = -1;
            double best_distance = INFINITY;
            for (size_t idx = 0; idx < per_thread_bests.size(); idx++)
            {
                const double& thread_minimum_distance = per_thread_bests[idx].second;
                if (thread_minimum_distance < best_distance)
                {
                    best_index = per_thread_bests[idx].first;
                    best_distance = thread_minimum_distance;
                }
            }
            logging_fn("Selected node " + std::to_string(best_index) + " as nearest neighbor (Qnear)", 3);
            return best_index;
        }

        /*
         * Planning functions
         */

        inline std::pair<UncertaintyPlanningPolicy, Statistics> PlanGoalSampling(
                const UncertaintyPlanningState& start_state,
                const double goal_bias,
                const NearestNeighborFn& nearest_neighbor_fn,
                const ForwardPropagationFn& forward_propagation_fn,
                const GoalReachedProbabilityFn& user_goal_check_fn,
                const std::chrono::duration<double>& time_limit,
                const uint32_t edge_attempt_count,
                const uint32_t policy_action_attempt_count,
                const bool allow_contacts,
                const bool include_spur_actions,
                const double policy_marker_size,
                const double p_goal_termination_threshold,
                const DisplayFn& display_fn)
        {
            // Bind the helper functions
            const auto start_time = std::chrono::high_resolution_clock::now();
            PlanningStateGoalCheckFn goal_reached_fn = [&] (const UncertaintyPlanningState& goal_candidate)
            {
                return GoalReachedGoalFunction(goal_candidate, user_goal_check_fn, edge_attempt_count, allow_contacts);
            };
            std::function<void(UncertaintyPlanningTree&, const int64_t)> goal_reached_callback = [&] (UncertaintyPlanningTree& tree, const int64_t new_goal_state_idx)
            {
                return GoalReachedCallback(tree, new_goal_state_idx, edge_attempt_count, start_time);
            };
            std::uniform_real_distribution<double> goal_bias_distribution(0.0, 1.0);
            std::function<UncertaintyPlanningState(void)> complete_sampling_fn = [&] (void)
            {
                if (goal_bias_distribution(simulator_ptr_->GetRandomGenerator()) > goal_bias)
                {
                    Log("Sampled state", 1);
                    return SampleRandomTargetState();
                }
                else
                {
                    Log("Sampled goal state", 1);
                    return SampleRandomTargetGoalState();
                }
            };
            auto termination_check_fn = [&] (const int64_t)
            {
                return PlannerTerminationCheck(start_time, time_limit, p_goal_termination_threshold);
            };
            // Call the planner
            total_goal_reached_probability_ = 0.0;
            time_to_first_solution_ = 0.0;
            simulator_ptr_->ResetStatistics();
            clustering_ptr_->ResetStatistics();
            nearest_neighbors_storage_.emplace_back(UncertaintyPlanningTreeState(start_state));
            auto planning_results = common_robotics_utilities::simple_rrt_planner::RRTPlanMultiPath(
                        nearest_neighbors_storage_,
                        complete_sampling_fn,
                        nearest_neighbor_fn,
                        forward_propagation_fn,
                        {},
                        goal_reached_fn,
                        goal_reached_callback,
                        termination_check_fn);
            // It "shouldn't" matter what the goal state actually is, since it's more of a virtual node to tie the policy graph together
            // But it probably needs to be collision-free
            auto valid_goal_sampling_fn = [&] ()
            {
                while (true)
                {
                    const Configuration goal_sample = sampler_ptr_->SampleGoal(simulator_ptr_->GetRandomGenerator());
                    if (simulator_ptr_->CheckConfigCollision(robot_ptr_, goal_sample) == false)
                    {
                        return goal_sample;
                    }
                }
            };
            const Configuration virtual_goal = valid_goal_sampling_fn();
            return ProcessPlanningResults(
                        planning_results,
                        virtual_goal,
                        edge_attempt_count,
                        policy_action_attempt_count,
                        include_spur_actions,
                        policy_marker_size,
                        display_fn);
        }

        inline std::pair<UncertaintyPlanningPolicy, Statistics> PlanGoalSampling(
                const UncertaintyPlanningState& start_state,
                const double goal_bias,
                const NearestNeighborFn& nearest_neighbor_fn,
                const GoalReachedProbabilityFn& user_goal_check_fn,
                const std::chrono::duration<double>& time_limit,
                const uint32_t edge_attempt_count,
                const uint32_t policy_action_attempt_count,
                const bool allow_contacts,
                const bool include_reverse_actions,
                const bool include_spur_actions,
                const double policy_marker_size,
                const double p_goal_termination_threshold,
                const DisplayFn& display_fn)
        {
            ForwardPropagationFn forward_propagation_fn =
                    [&] (const UncertaintyPlanningState& nearest, const UncertaintyPlanningState& target)
            {
                return PropagateForwardsAndDraw(nearest, target, edge_attempt_count, allow_contacts, include_reverse_actions, display_fn);
            };
            return PlanGoalSampling(start_state,
                                    goal_bias,
                                    nearest_neighbor_fn,
                                    forward_propagation_fn,
                                    user_goal_check_fn,
                                    time_limit,
                                    edge_attempt_count,
                                    policy_action_attempt_count,
                                    allow_contacts,
                                    include_spur_actions,
                                    policy_marker_size,
                                    p_goal_termination_threshold,
                                    display_fn);

        }

        inline std::pair<UncertaintyPlanningPolicy, Statistics> PlanGoalSampling(
                const Configuration& start,
                const double goal_bias,
                const GoalReachedProbabilityFn& user_goal_check_fn,
                const std::chrono::duration<double>& time_limit,
                const uint32_t edge_attempt_count,
                const uint32_t policy_action_attempt_count,
                const bool allow_contacts,
                const bool include_reverse_actions,
                const bool include_spur_actions,
                const double policy_marker_size,
                const double p_goal_termination_threshold,
                const DisplayFn& display_fn)
        {
            // Draw the simulation environment
            display_fn(MakeEraseMarkers());
            display_fn(MakeEnvironmentDisplayRep());
            // Wait for input
            if (debug_level_ >= 10)
            {
                std::cout << "Press ENTER to draw start state..." << std::endl;
                std::cin.get();
            }
            // Draw the start and goal
            const std_msgs::ColorRGBA start_color = common_robotics_utilities::color_builder::MakeFromFloatColors<std_msgs::ColorRGBA>(1.0, 0.0, 0.0, 1.0);
            const visualization_msgs::MarkerArray start_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, start, start_color, 1, "start_state");
            visualization_msgs::MarkerArray problem_display_rep;
            problem_display_rep.markers.insert(problem_display_rep.markers.end(), start_markers.markers.begin(), start_markers.markers.end());
            display_fn(problem_display_rep);
            // Wait for input
            if (debug_level_ >= 10)
            {
                std::cout << "Press ENTER to start planning..." << std::endl;
                std::cin.get();
            }
            DistanceFn state_distance_fn = [&] (const UncertaintyPlanningState& state1, const UncertaintyPlanningState& state2)
            {
                return StateDistance(state1, state2);
            };
            NearestNeighborFn nearest_neighbor_fn = [&] (const UncertaintyPlanningTree& tree, const UncertaintyPlanningState& new_state) { return GetNearestNeighbor(tree, new_state, state_distance_fn, logging_fn_); };
            UncertaintyPlanningState start_state(start);
            return PlanGoalSampling(start_state,
                                    goal_bias,
                                    nearest_neighbor_fn,
                                    user_goal_check_fn,
                                    time_limit,
                                    edge_attempt_count,
                                    policy_action_attempt_count,
                                    allow_contacts,
                                    include_reverse_actions,
                                    include_spur_actions,
                                    policy_marker_size,
                                    p_goal_termination_threshold,
                                    display_fn);
        }

        inline std::pair<UncertaintyPlanningPolicy, Statistics> PlanGoalState(
                const Configuration& start,
                const Configuration& goal,
                const double goal_bias,
                const std::chrono::duration<double>& time_limit,
                const uint32_t edge_attempt_count,
                const uint32_t policy_action_attempt_count,
                const bool allow_contacts,
                const bool include_reverse_actions,
                const bool include_spur_actions,
                const double policy_marker_size,
                const double p_goal_termination_threshold,
                const DisplayFn& display_fn)
        {
            // Draw the simulation environment
            display_fn(MakeEraseMarkers());
            display_fn(MakeEnvironmentDisplayRep());
            // Wait for input
            if (debug_level_ >= 10)
            {
                std::cout << "Press ENTER to draw start and goal states..." << std::endl;
                std::cin.get();
            }
            // Draw the start and goal
            const std_msgs::ColorRGBA start_color = common_robotics_utilities::color_builder::MakeFromFloatColors<std_msgs::ColorRGBA>(1.0, 0.0, 0.0, 1.0);
            const visualization_msgs::MarkerArray start_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, start, start_color, 1, "start_state");
            const std_msgs::ColorRGBA goal_color = common_robotics_utilities::color_builder::MakeFromFloatColors<std_msgs::ColorRGBA>(0.0, 1.0, 0.0, 1.0);
            const visualization_msgs::MarkerArray goal_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, goal, goal_color, 1, "goal_state");
            visualization_msgs::MarkerArray problem_display_rep;
            problem_display_rep.markers.insert(problem_display_rep.markers.end(), start_markers.markers.begin(), start_markers.markers.end());
            problem_display_rep.markers.insert(problem_display_rep.markers.end(), goal_markers.markers.begin(), goal_markers.markers.end());
            display_fn(problem_display_rep);
            // Wait for input
            if (debug_level_ >= 10)
            {
                std::cout << "Press ENTER to start planning..." << std::endl;
                std::cin.get();
            }
            UncertaintyPlanningState start_state(start);
            UncertaintyPlanningState goal_state(goal);
            // Bind the helper functions
            const std::chrono::time_point<std::chrono::high_resolution_clock> start_time = std::chrono::high_resolution_clock::now();
            DistanceFn state_distance_fn = [&] (const UncertaintyPlanningState& state1, const UncertaintyPlanningState& state2)
            {
                return StateDistance(state1, state2);
            };
            NearestNeighborFn nearest_neighbor_fn = [&] (const UncertaintyPlanningTree& tree, const UncertaintyPlanningState& new_state) { return GetNearestNeighbor(tree, new_state, state_distance_fn, logging_fn_); };
            std::function<bool(const UncertaintyPlanningState&)> goal_reached_fn = [&] (const UncertaintyPlanningState& goal_candidate) { return GoalReachedGoalState(goal_candidate, goal_state, edge_attempt_count, allow_contacts); };
            std::function<void(UncertaintyPlanningTree&, const int64_t)> goal_reached_callback = [&] (UncertaintyPlanningTree& tree, const int64_t new_goal_state_idx) { return GoalReachedCallback(tree, new_goal_state_idx, edge_attempt_count, start_time); };
            std::uniform_real_distribution<double> goal_bias_distribution(0.0, 1.0);
            std::function<UncertaintyPlanningState(void)> complete_sampling_fn = [&](void)
            {
                if (goal_bias_distribution(simulator_ptr_->GetRandomGenerator()) > goal_bias)
                {
                    Log("Sampled state", 1);
                    return SampleRandomTargetState();
                }
                else
                {
                    Log("Sampled goal state", 1);
                    return goal_state;
                }
            };
            ForwardPropagationFn forward_propagation_fn = [&] (const UncertaintyPlanningState& nearest, const UncertaintyPlanningState& target) { return PropagateForwardsAndDraw(nearest, target, edge_attempt_count, allow_contacts, include_reverse_actions, display_fn); };
            std::function<bool(const int64_t)> termination_check_fn = [&] (const int64_t) { return PlannerTerminationCheck(start_time, time_limit, p_goal_termination_threshold); };
            // Call the planner
            // Call the planner
            total_goal_reached_probability_ = 0.0;
            time_to_first_solution_ = 0.0;
            simulator_ptr_->ResetStatistics();
            clustering_ptr_->ResetStatistics();
            nearest_neighbors_storage_.emplace_back(UncertaintyPlanningTreeState(start_state));
            auto planning_results = common_robotics_utilities::simple_rrt_planner::RRTPlanMultiPath(nearest_neighbors_storage_, complete_sampling_fn, nearest_neighbor_fn, forward_propagation_fn, {}, goal_reached_fn, goal_reached_callback, termination_check_fn);
            return ProcessPlanningResults(planning_results, goal, edge_attempt_count, policy_action_attempt_count, include_spur_actions, policy_marker_size, display_fn);
        }

    protected:

        inline std::pair<UncertaintyPlanningPolicy, Statistics> ProcessPlanningResults(
                const std::pair<std::vector<std::vector<UncertaintyPlanningState>>, Statistics>& planning_results,
                const Configuration& virtual_goal_config,
                const uint32_t edge_attempt_count,
                const uint32_t policy_action_attempt_count,
                const bool include_spur_actions,
                const double policy_marker_size,
                const DisplayFn& display_fn)
        {
            // Make sure we got somewhere
            Statistics planning_statistics = planning_results.second;
            Log("Planner terminated with goal reached probability: " + std::to_string(total_goal_reached_probability_), 2);
            planning_statistics["P(goal reached)"] = total_goal_reached_probability_;
            planning_statistics["Time to first solution"] = time_to_first_solution_;
            const Statistics simulator_resolve_statistics = simulator_ptr_->GetStatistics();
            planning_statistics.insert(simulator_resolve_statistics.begin(), simulator_resolve_statistics.end());
            const Statistics outcome_clustering_statistics = clustering_ptr_->GetStatistics();
            planning_statistics.insert(outcome_clustering_statistics.begin(), outcome_clustering_statistics.end());
            planning_statistics["elapsed_clustering_time"] = elapsed_clustering_time_;
            planning_statistics["elapsed_simulation_time"] = elapsed_simulation_time_;
            planning_statistics["Particles stored"] = (double)particles_stored_;
            planning_statistics["Particles simulated"] = (double)particles_simulated_;
            planning_statistics["Goal candidates evaluated"] = (double)goal_candidates_evaluated_;
            planning_statistics["Goal reaching performed"] = (double)goal_reaching_performed_;
            planning_statistics["Goal reaching successful"] = (double)goal_reaching_successful_;
            if (total_goal_reached_probability_ >= goal_probability_threshold_)
            {
                const UncertaintyPlanningTree postprocessed_tree = PostProcessTree(nearest_neighbors_storage_);
                const UncertaintyPlanningTree pruned_tree = PruneTree(postprocessed_tree, include_spur_actions);
                // Not sure hwat to do here with goal states
                const UncertaintyPlanningPolicy policy = ExtractPolicy(pruned_tree, virtual_goal_config, edge_attempt_count, policy_action_attempt_count);
                planning_statistics["Extracted policy size"] = (double)policy.GetRawPolicy().GetNodesImmutable().size();
                if (debug_level_ >= 2)
                {
                    std::cout << "Press ENTER to draw planned paths..." << std::endl;
                    std::cin.get();
                }
                // Draw the final path(s)
                for (size_t pidx = 0; pidx < planning_results.first.size(); pidx++)
                {
                    const std::vector<UncertaintyPlanningState>& planned_path = planning_results.first[pidx];
                    if (planned_path.size() >= 2)
                    {
                        double goal_reached_probability = planned_path[planned_path.size() - 1].GetGoalPfeasibility() * planned_path[planned_path.size() - 1].GetMotionPfeasibility();
                        visualization_msgs::MarkerArray path_display_rep;
                        const std::string forward_expectation_ns = "final_path_" + std::to_string(pidx + 1);
                        const std::string reverse_expectation_ns = "final_path_reversible_" + std::to_string(pidx + 1);
                        for (size_t idx = 0; idx < planned_path.size(); idx++)
                        {
                            const UncertaintyPlanningState& current_state = planned_path[idx];
                            const Configuration current_configuration = current_state.GetExpectation();
                            std_msgs::ColorRGBA forward_color;
                            forward_color.r = (float)(1.0 - goal_reached_probability);
                            forward_color.g = 0.0f;
                            forward_color.b = 0.0f;
                            forward_color.a = (float)current_state.GetMotionPfeasibility();
                            const visualization_msgs::MarkerArray forward_expectation_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, current_configuration, forward_color, (int32_t)path_display_rep.markers.size() + 1, forward_expectation_ns);
                            // Add the markers
                            path_display_rep.markers.insert(path_display_rep.markers.end(), forward_expectation_markers.markers.begin(), forward_expectation_markers.markers.end());
                            std_msgs::ColorRGBA reverse_color;
                            reverse_color.r = (float)(1.0 - goal_reached_probability);
                            reverse_color.g = 0.0f;
                            reverse_color.b = 0.0f;
                            reverse_color.a = (float)current_state.GetReverseEdgePfeasibility();
                            const visualization_msgs::MarkerArray reverse_expectation_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, current_configuration, reverse_color, (int32_t)path_display_rep.markers.size() + 1, reverse_expectation_ns);
                            // Add the markers
                            path_display_rep.markers.insert(path_display_rep.markers.end(), reverse_expectation_markers.markers.begin(), reverse_expectation_markers.markers.end());
                        }
                        display_fn(path_display_rep);
                    }
                }
                DrawPolicy(policy, policy_marker_size, "planned_policy", display_fn);
                // Wait for input
                if (debug_level_ >= 2)
                {
                    std::cout << "Press ENTER to export policy and print statistics..." << std::endl;
                    std::cin.get();
                    std::cout << "Planner statistics:\n" << common_robotics_utilities::print::Print(planning_statistics) << std::endl;
                }
                return {policy, planning_statistics};
            }
            else
            {
                const UncertaintyPlanningPolicy policy;
                planning_statistics["Extracted policy size"] = 0.0;
                // Wait for input
                if (debug_level_ >= 2)
                {
                    std::cout << "Press ENTER to export policy and print statistics..." << std::endl;
                    std::cin.get();
                    std::cout << "Planner statistics:\n" << common_robotics_utilities::print::Print(planning_statistics) << std::endl;
                }
                return std::pair<UncertaintyPlanningPolicy, Statistics>(policy, planning_statistics);
            }
        }

        /*
         * Solution tree post-processing functions
         */
        inline UncertaintyPlanningTree PostProcessTree(
                const UncertaintyPlanningTree& planner_tree) const
        {
            Log("Postprocessing planner tree in preparation for policy extraction...", 1);
            std::chrono::time_point<std::chrono::high_resolution_clock> start_time = std::chrono::high_resolution_clock::now();
            // Let's do some post-processing to the planner tree - we don't want to mess with the original tree, so we copy it
            UncertaintyPlanningTree postprocessed_planner_tree = planner_tree;
            // We have already computed reversibility for all edges, however, we now need to update the P(goal reached) for reversible children
            // We start with a naive implementation of this - this works because given the process that the tree is generated, children *MUST* have higher indices than their parents, so we can depend on the parents
            // having been updated first by the time we get to an index. To make this parallelizable, we'll need to switch to an explicitly branch-based approach
            // Go through each state in the tree - we skip the initial state, since it has no transition
            for (size_t sdx = 1; sdx < postprocessed_planner_tree.size(); sdx++)
            {
                // Get the current state
                UncertaintyPlanningTreeState& current_state = postprocessed_planner_tree[sdx];
                const int64_t parent_index = current_state.GetParentIndex();
                // Get the parent state
                const UncertaintyPlanningTreeState& parent_state = postprocessed_planner_tree[(size_t)parent_index];
                // If the current state is on a goal branch
                if (current_state.GetValueImmutable().GetGoalPfeasibility() > 0.0)
                {
                    // Reversibility has already been computed
                    continue;
                }
                // If we are a non-goal child of a goal branch state
                else if (parent_state.GetValueImmutable().GetGoalPfeasibility() > 0.0)
                {
                    // Make sure we're a child of a split where at least one child reaches the goal
                    const uint64_t transition_id = current_state.GetValueImmutable().GetTransitionId();
                    const uint64_t state_id = current_state.GetValueImmutable().GetStateId();
                    bool result_of_goal_reaching_split = false;
                    const std::vector<int64_t>& other_children = parent_state.GetChildIndices();
                    for (size_t idx = 0; idx < other_children.size(); idx++)
                    {
                        const int64_t other_child_index = other_children[idx];
                        const UncertaintyPlanningTreeState& other_child_state = postprocessed_planner_tree[(size_t)other_child_index];
                        const uint64_t other_child_transition_id = other_child_state.GetValueImmutable().GetTransitionId();
                        const uint64_t other_child_state_id = other_child_state.GetValueImmutable().GetStateId();
                        // If it's a child of the same split that produced us
                        if ((state_id != other_child_state_id) && (transition_id == other_child_transition_id))
                        {
                            const double other_child_goal_probability = other_child_state.GetValueImmutable().GetGoalPfeasibility();
                            if (other_child_goal_probability > 0.0)
                            {
                                result_of_goal_reaching_split = true;
                                break;
                            }
                        }
                    }
                    if (result_of_goal_reaching_split)
                    {
                        // Update P(goal reached) based on our ability to reverse to the goal branch
                        const double parent_pgoalreached = parent_state.GetValueImmutable().GetGoalPfeasibility();
                        const double new_pgoalreached = -(parent_pgoalreached * current_state.GetValueImmutable().GetReverseEdgePfeasibility()); // We use negative goal reached probabilities to signal probability due to reversing
                        current_state.GetValueMutable().SetGoalPfeasibility(new_pgoalreached);
                    }
                }
            }
            std::chrono::time_point<std::chrono::high_resolution_clock> end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> postprocessing_time(end_time - start_time);
            Log("...postprocessing complete, took " + std::to_string(postprocessing_time.count()) + " seconds", 1);
            return postprocessed_planner_tree;
        }

        inline UncertaintyPlanningTree PruneTree(
                const UncertaintyPlanningTree& planner_tree,
                const bool include_spur_actions) const
        {
            if (planner_tree.size() <= 1)
            {
                return planner_tree;
            }
            // Test to make sure the tree linkage is intact
            if (common_robotics_utilities::simple_rrt_planner::CheckTreeLinkage(planner_tree) == false)
            {
                throw std::runtime_error("planner_tree has invalid linkage");
            }
            Log("Pruning planner tree in preparation for policy extraction...", 1);
            std::chrono::time_point<std::chrono::high_resolution_clock> start_time = std::chrono::high_resolution_clock::now();
            // Let's do some post-processing to the planner tree - we don't want to mess with the original tree, so we copy it
            UncertaintyPlanningTree intermediate_planner_tree = planner_tree;
            // Loop through the tree and prune unproductive nodes+edges
            for (size_t idx = 0; idx < intermediate_planner_tree.size(); idx++)
            {
                UncertaintyPlanningTreeState& current_state = intermediate_planner_tree[idx];
                if (current_state.IsInitialized() == false)
                {
                    throw std::runtime_error("current_state is uninitialized");
                }
                // If we're on a path to the goal, we always keep it
                if (current_state.GetValueImmutable().GetGoalPfeasibility() > 0.0)
                {
                    continue;
                }
                // If the current node can reverse to reach the goal
                else if (current_state.GetValueImmutable().GetGoalPfeasibility() < -0.0)
                {
                    // If we allow spur nodes, we keep it
                    if (include_spur_actions)
                    {
                        continue;
                    }
                    // If not, prune the node
                    else
                    {
                        current_state.SetParentIndex(-1);
                        current_state.ClearChildIndicies();
                    }
                }
                // We always prune nodes that can't reach the goal
                else
                {
                    current_state.SetParentIndex(-1);
                    current_state.ClearChildIndicies();
                }
            }
            // Now, extract the pruned tree
            UncertaintyPlanningTree pruned_planner_tree;
            // Add root state
            UncertaintyPlanningTreeState root_state = intermediate_planner_tree[0];
            if (root_state.IsInitialized() == false)
            {
                throw std::runtime_error("root_state is uninitialized");
            }
            pruned_planner_tree.push_back(root_state);
            // Recursive call to extract live branches
            ExtractChildStates(intermediate_planner_tree, 0, 0, pruned_planner_tree);
            // Test to make sure the tree linkage is intact
            if (common_robotics_utilities::simple_rrt_planner::CheckTreeLinkage(pruned_planner_tree) == false)
            {
                throw std::runtime_error("pruned_planner_tree has invalid linkage");
            }
            std::chrono::time_point<std::chrono::high_resolution_clock> end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> pruning_time(end_time - start_time);
            Log("...pruning complete, pruned to " + std::to_string(pruned_planner_tree.size()) + " states, took " + std::to_string(pruning_time.count()) + " seconds", 1);
            return pruned_planner_tree;
        }

        /*
         * Policy generation wrapper function
         */
        inline UncertaintyPlanningPolicy ExtractPolicy(
                const UncertaintyPlanningTree& planner_tree,
                const Configuration& goal,
                const uint32_t planner_action_try_attempts,
                const uint32_t policy_action_attempt_count) const
        {
            const double marginal_edge_weight = 0.05;
            const UncertaintyPlanningPolicy policy(planner_tree, goal, marginal_edge_weight, goal_probability_threshold_, planner_action_try_attempts, policy_action_attempt_count, logging_fn_);
            return policy;
        }

        inline void LogParticleTrajectories(
                const std::vector<std::vector<Configuration, ConfigAlloc>>& particle_executions,
                const std::string& filename) const
        {
            std::ofstream log_file(filename, std::ios_base::out);
            if (!log_file.is_open())
            {
                throw std::invalid_argument("Log filename [" + filename + "] must be write-openable");
            }
            for (size_t idx = 0; idx < particle_executions.size(); idx++)
            {
                const std::vector<Configuration, ConfigAlloc>& particle_trajectory = particle_executions[idx];
                log_file << "Particle trajectory " << (idx + 1) << std::endl;
                for (size_t sdx = 0; sdx < particle_trajectory.size(); sdx++)
                {
                    const Configuration& config = particle_trajectory[sdx];
                    log_file << common_robotics_utilities::print::Print(config) << std::endl;
                }
            }
            log_file.close();
        }

    public:

        /*
         * Policy simulation and execution functions
         */
        inline std::pair<UncertaintyPlanningPolicy, std::pair<Statistics, std::pair<std::vector<int64_t>, std::vector<double>>>> SimulateExectionPolicy(
                const UncertaintyPlanningPolicy& immutable_policy,
                const bool allow_branch_jumping,
                const bool link_runtime_states_to_planned_parent,
                const Configuration& start,
                const Configuration& goal,
                const uint32_t num_executions,
                const uint32_t exec_step_limit,
                const DisplayFn& display_fn,
                const double policy_marker_size,
                const bool wait_for_user,
                const double draw_wait) const
        {
            ConfigGoalCheckFn simple_goal_check_fn = [&] (const Configuration& current_config)
            {
                if (robot_ptr_->ComputeConfigurationDistance(current_config, goal) <= goal_distance_threshold_)
                {
                    return true;
                }
                else
                {
                    return false;
                }
            };
            return SimulateExectionPolicy(immutable_policy, allow_branch_jumping, link_runtime_states_to_planned_parent, start, simple_goal_check_fn, num_executions, exec_step_limit, display_fn, policy_marker_size, wait_for_user, draw_wait);
        }

        inline std::pair<UncertaintyPlanningPolicy, std::pair<Statistics, std::pair<std::vector<int64_t>, std::vector<double>>>> SimulateExectionPolicy(
                const UncertaintyPlanningPolicy& immutable_policy,
                const bool allow_branch_jumping,
                const bool link_runtime_states_to_planned_parent,
                const Configuration& start,
                const ConfigGoalCheckFn& user_goal_check_fn,
                const uint32_t num_executions,
                const uint32_t exec_step_limit,
                const DisplayFn& display_fn,
                const double policy_marker_size,
                const bool wait_for_user,
                const double draw_wait) const
        {
            const std::vector<Configuration, ConfigAlloc> start_configs(num_executions, start);
            return SimulateExectionPolicy(immutable_policy, allow_branch_jumping, link_runtime_states_to_planned_parent, true, start_configs, user_goal_check_fn, exec_step_limit, display_fn, policy_marker_size, wait_for_user, draw_wait);
        }

        inline std::pair<UncertaintyPlanningPolicy, std::pair<Statistics, std::pair<std::vector<int64_t>, std::vector<double>>>> SimulateExectionPolicy(
                const UncertaintyPlanningPolicy& immutable_policy,
                const bool allow_branch_jumping,
                const bool link_runtime_states_to_planned_parent,
                const bool enable_cumulative_learning,
                const std::vector<Configuration, ConfigAlloc>& start_configs,
                const ConfigGoalCheckFn& user_goal_check_fn,
                const uint32_t exec_step_limit,
                const DisplayFn& display_fn,
                const double policy_marker_size,
                const bool wait_for_user,
                const double draw_wait) const
        {
            const uint32_t num_executions = (uint32_t)start_configs.size();
            UncertaintyPlanningPolicy policy = immutable_policy;
            simulator_ptr_->ResetStatistics();
            std::vector<std::vector<Configuration, ConfigAlloc>> particle_executions(num_executions);
            std::vector<int64_t> policy_execution_step_counts(num_executions, 0u);
            std::vector<double> policy_execution_times(num_executions, -0.0);
            uint32_t reached_goal = 0;
            for (size_t idx = 0; idx < num_executions; idx++)
            {
                const std::chrono::time_point<std::chrono::high_resolution_clock> start_time = std::chrono::high_resolution_clock::now();
                const ExecutionMovementFn simulator_move_fn =
                        [&] (const Configuration& current, const Configuration& action, const Configuration& expected_result, const bool is_reverse_motion, const bool is_reset_motion)
                {
                    UNUSED(expected_result);
                    UNUSED(is_reset_motion);
                    return SimulatePolicyStep(current, action, is_reverse_motion, display_fn);
                };
                int64_t policy_exec_steps = 0;
                const std::function<bool(void)> policy_exec_termination_fn =
                        [&] ()
                {
                    if (policy_exec_steps >= exec_step_limit)
                    {
                        return true;
                    }
                    else
                    {
                        policy_exec_steps++;
                        return false;
                    }
                };
                const std::pair<std::vector<Configuration, ConfigAlloc>, std::pair<UncertaintyPlanningPolicy, int64_t>> particle_execution = PerformSinglePolicyExecution(policy, allow_branch_jumping, link_runtime_states_to_planned_parent, start_configs[idx], simulator_move_fn, user_goal_check_fn, policy_exec_termination_fn, display_fn, policy_marker_size, wait_for_user);
                const std::chrono::time_point<std::chrono::high_resolution_clock> end_time = std::chrono::high_resolution_clock::now();
                const std::chrono::duration<double> execution_time(end_time - start_time);
                const double execution_seconds = execution_time.count();
                policy_execution_times[idx] = execution_seconds;
                particle_executions[idx] = particle_execution.first;
                if (enable_cumulative_learning)
                {
                    policy = particle_execution.second.first;
                }
                const int64_t policy_execution_step_count = particle_execution.second.second;
                policy_execution_step_counts[idx] = policy_execution_step_count;
                if (policy_execution_step_count >= 0)
                {
                    reached_goal++;
                    Log("...finished policy execution " + std::to_string(idx + 1) + " of " + std::to_string(num_executions) + " successfully, " + std::to_string(reached_goal) + " successful so far", 2);
                }
                else
                {
                    Log("...finished policy execution " + std::to_string(idx + 1) + " of " + std::to_string(num_executions) + " unsuccessfully, " + std::to_string(reached_goal) + " successful so far", 3);
                }
            }
            // Draw the trajectory in a pretty way
            if (wait_for_user)
            {
                // Wait for input
                std::cout << "Press ENTER to draw pretty simulation tracks..." << std::endl;
                std::cin.get();
            }
            for (size_t idx = 0; idx < num_executions; idx++)
            {
                const std::string ns = "policy_simulation_" + std::to_string(idx + 1);
                DrawParticlePolicyExecution(ns, particle_executions[idx], display_fn, draw_wait, MakeColor(0.0f, 0.0f, 0.8f, 0.25f));
            }
            const double policy_success = (double)reached_goal / (double)num_executions;
            Statistics policy_statistics;
            policy_statistics["(Simulation) Policy success"] = policy_success;
            const Statistics simulator_resolve_statistics = simulator_ptr_->GetStatistics();
            policy_statistics.insert(simulator_resolve_statistics.begin(), simulator_resolve_statistics.end());
            if (debug_level_ >= 15)
            {
                LogParticleTrajectories(particle_executions, "/tmp/policy_simulation_trajectories.csv");
            }
            return std::make_pair(policy, std::make_pair(policy_statistics, std::make_pair(policy_execution_step_counts, policy_execution_times)));
        }

        inline std::pair<UncertaintyPlanningPolicy, std::pair<Statistics, std::pair<std::vector<int64_t>, std::vector<double>>>> ExecuteExectionPolicy(
                const UncertaintyPlanningPolicy& immutable_policy,
                const bool allow_branch_jumping,
                const bool link_runtime_states_to_planned_parent,
                const Configuration& start,
                const Configuration& goal,
                const ExecutionMovementFn& move_fn,
                const uint32_t num_executions,
                const double exec_time_limit,
                const DisplayFn& display_fn,
                const double policy_marker_size,
                const bool wait_for_user,
                const double draw_wait) const
        {
            ConfigGoalCheckFn simple_goal_check_fn = [&] (const Configuration& current_config)
            {
                if (robot_ptr_->ComputeConfigurationDistance(current_config, goal) <= goal_distance_threshold_)
                {
                    return true;
                }
                else
                {
                    return false;
                }
            };
            return ExecuteExectionPolicy(immutable_policy, allow_branch_jumping, link_runtime_states_to_planned_parent, start, simple_goal_check_fn, move_fn, num_executions, exec_time_limit, display_fn, policy_marker_size, wait_for_user, draw_wait);
        }

        inline std::pair<UncertaintyPlanningPolicy, std::pair<Statistics, std::pair<std::vector<int64_t>, std::vector<double>>>> ExecuteExectionPolicy(
                const UncertaintyPlanningPolicy& immutable_policy,
                const bool allow_branch_jumping,
                const bool link_runtime_states_to_planned_parent,
                const Configuration& start,
                const ConfigGoalCheckFn& user_goal_check_fn,
                const ExecutionMovementFn& move_fn,
                const uint32_t num_executions,
                const double exec_time_limit,
                const DisplayFn& display_fn,
                const double policy_marker_size,
                const bool wait_for_user,
                const double draw_wait) const
        {
            const std::vector<Configuration, ConfigAlloc> start_configs(num_executions, start);
            return ExecuteExectionPolicy(immutable_policy, allow_branch_jumping, link_runtime_states_to_planned_parent, true, start_configs, user_goal_check_fn, move_fn, exec_time_limit, display_fn, policy_marker_size, wait_for_user, draw_wait);
        }

        inline std::pair<UncertaintyPlanningPolicy, std::pair<Statistics, std::pair<std::vector<int64_t>, std::vector<double>>>> ExecuteExectionPolicy(
                const UncertaintyPlanningPolicy& immutable_policy,
                const bool allow_branch_jumping,
                const bool link_runtime_states_to_planned_parent,
                const bool enable_cumulative_learning,
                const std::vector<Configuration, ConfigAlloc>& start_configs,
                const ConfigGoalCheckFn& user_goal_check_fn,
                const ExecutionMovementFn& move_fn,
                const double exec_time_limit,
                const DisplayFn& display_fn,
                const double policy_marker_size,
                const bool wait_for_user,
                const double draw_wait) const
        {
            const uint32_t num_executions = (uint32_t)start_configs.size();
            UncertaintyPlanningPolicy policy = immutable_policy;
            // Buffer for a teensy bit of time
            for (size_t iter = 0; iter < 100; iter++)
            {
                ros::spinOnce();
                ros::Duration(0.005).sleep();
            }
            std::vector<std::vector<Configuration, ConfigAlloc>> particle_executions(num_executions);
            std::vector<int64_t> policy_execution_step_counts(num_executions, 0u);
            std::vector<double> policy_execution_times(num_executions, -0.0);
            uint32_t reached_goal = 0;
            for (size_t idx = 0; idx < num_executions; idx++)
            {
                Log("Starting policy execution " + std::to_string(idx) + "...", 1);
                const double start_time = ros::Time::now().toSec();
                const std::function<bool(void)> policy_exec_termination_fn =
                        [&] ()
                {
                    if (exec_time_limit > 0.0)
                    {
                        const double current_time = ros::Time::now().toSec();
                        const double elapsed = current_time - start_time;
                        if (elapsed >= exec_time_limit)
                        {
                            return true;
                        }
                    }
                    return false;
                };
                const std::pair<std::vector<Configuration, ConfigAlloc>, std::pair<UncertaintyPlanningPolicy, int64_t>> particle_execution = PerformSinglePolicyExecution(policy, allow_branch_jumping, link_runtime_states_to_planned_parent, start_configs[idx], move_fn, user_goal_check_fn, policy_exec_termination_fn, display_fn, policy_marker_size, wait_for_user);
                const double end_time = ros::Time::now().toSec();
                Log("Started policy exec @ " + std::to_string(start_time) + " finished policy exec @ " + std::to_string(end_time), 1);
                const double execution_seconds = end_time - start_time;
                policy_execution_times[idx] = execution_seconds;
                particle_executions[idx] = particle_execution.first;
                if (enable_cumulative_learning)
                {
                    policy = particle_execution.second.first;
                }
                const int64_t policy_execution_step_count = particle_execution.second.second;
                policy_execution_step_counts[idx] = policy_execution_step_count;
                if (policy_execution_step_count >= 0)
                {
                    reached_goal++;
                    Log("...finished policy execution " + std::to_string(idx + 1) + " of " + std::to_string(num_executions) + " successfully in " + std::to_string(execution_seconds) + " seconds, " + std::to_string(reached_goal) + " successful so far", 2);
                }
                else
                {
                    Log("...finished policy execution " + std::to_string(idx + 1) + " of " + std::to_string(num_executions) + " unsuccessfully in " + std::to_string(execution_seconds) + " seconds, " + std::to_string(reached_goal) + " successful so far", 3);
                }
            }
            // Draw the trajectory in a pretty way
            if (wait_for_user)
            {
                // Wait for input
                std::cout << "Press ENTER to draw pretty execution tracks..." << std::endl;
                std::cin.get();
            }
            for (size_t idx = 0; idx < num_executions; idx++)
            {
                const std::string ns = "policy_execution_" + std::to_string(idx + 1);
                //DrawParticlePolicyExecution(ns, particle_executions[idx], display_fn, draw_wait, MakeColor(0.0f, 0.8f, 0.0f, 0.25f));
                DrawParticlePolicyExecution(ns, particle_executions[idx], display_fn, draw_wait, MakeColor(0.0f, 0.0f, 0.0f, 1.0f));
            }
            const double policy_success = (double)reached_goal / (double)num_executions;
            Statistics policy_statistics;
            policy_statistics["(Execution) Policy success"] = policy_success;
            if (debug_level_ >= 15)
            {
                LogParticleTrajectories(particle_executions, "/tmp/policy_execution_trajectories.csv");
            }
            return std::make_pair(policy, std::make_pair(policy_statistics, std::make_pair(policy_execution_step_counts, policy_execution_times)));
        }

        static inline std_msgs::ColorRGBA MakeColor(const float r, const float g, const float b, const float a)
        {
            return common_robotics_utilities::color_builder::MakeFromFloatColors<std_msgs::ColorRGBA>(r, g, b, a);
        }

        inline std::pair<std::vector<Configuration, ConfigAlloc>, std::pair<UncertaintyPlanningPolicy, int64_t>> PerformSinglePolicyExecution(
                const UncertaintyPlanningPolicy& immutable_policy,
                const bool allow_branch_jumping,
                const bool link_runtime_states_to_planned_parent,
                const Configuration& start,
                const ExecutionMovementFn& move_fn,
                const ConfigGoalCheckFn& user_goal_check_fn,
                const std::function<bool(void)>& policy_exec_termination_fn,
                const DisplayFn& display_fn,
                const double policy_marker_size,
                const bool wait_for_user) const
        {
            UncertaintyPlanningPolicy policy = immutable_policy;
            Log("Drawing environment...", 1);
            ClearAndRedrawEnvironment(display_fn);
            if (wait_for_user)
            {
                std::cout << "Press ENTER to continue..." << std::endl;
                std::cin.get();
            }
            else
            {
                // Wait for a bit
                std::this_thread::sleep_for(std::chrono::duration<double>(0.1));
            }
            Log("Drawing initial policy...", 1);
            DrawPolicy(policy, policy_marker_size, "execution_policy", display_fn);
            if (wait_for_user)
            {
                std::cout << "Press ENTER to continue..." << std::endl;
                std::cin.get();
            }
            else
            {
                // Wait for a bit
                std::this_thread::sleep_for(std::chrono::duration<double>(0.1));
            }
            // Let's do this
            std::function<bool(const std::vector<Configuration, ConfigAlloc>&, const Configuration&)> policy_particle_clustering_fn = [&] (const std::vector<Configuration, ConfigAlloc>& particles, const Configuration& config) { return PolicyParticleClusteringFn(particles, config, display_fn); };
            // Reset the robot first
            Log("Reseting before policy execution...", 1);
            move_fn(start, start, start, false, true);
            Log("Executing policy...", 1);
            std::vector<Configuration, ConfigAlloc> particle_trajectory;
            particle_trajectory.push_back(start);
            uint64_t desired_transition_id = 0;
            uint32_t current_exec_step = 0u;
            while (policy_exec_termination_fn() == false)
            {
                current_exec_step++;
                // Get the current configuration
                const Configuration& current_config = particle_trajectory.back();
                // Get the next action
                const PolicyQueryResult<Configuration> policy_query_response = policy.QueryBestAction(desired_transition_id, current_config, allow_branch_jumping, link_runtime_states_to_planned_parent, policy_particle_clustering_fn);
                const int64_t previous_state_idx = policy_query_response.PreviousStateIndex();
                desired_transition_id = policy_query_response.DesiredTransitionId();
                const Configuration& action = policy_query_response.Action();
                const Configuration& expected_result = policy_query_response.ExpectedResult();
                const bool is_reverse_action = policy_query_response.IsReverseAction();
                Log("----------\nReceived new action for best matching state index " + std::to_string(previous_state_idx) + " with transition ID " + std::to_string(desired_transition_id) + "\n==========", 1);
                Log("Drawing updated policy...", 1);
                ClearAndRedrawEnvironment(display_fn);
                DrawPolicy(policy, policy_marker_size, "execution_policy", display_fn);
                DrawLocalPolicy(policy, policy_marker_size, 0, MakeColor(0.0, 0.0, 1.0, 1.0), "policy_start_to_goal", display_fn);
                DrawLocalPolicy(policy, policy_marker_size, previous_state_idx, MakeColor(0.0, 0.0, 1.0, 1.0), "policy_here_to_goal", display_fn);
                Log("Drawing current config (blue), parent state (cyan), and action (magenta)...", 1);
                const UncertaintyPlanningState& parent_state = policy.GetRawPolicy().GetNodeImmutable(previous_state_idx).GetValueImmutable();
                const Configuration parent_state_config = parent_state.GetExpectation();
                std_msgs::ColorRGBA parent_state_color;
                parent_state_color.r = 0.0f;
                parent_state_color.g = 0.5f;
                parent_state_color.b = 1.0f;
                parent_state_color.a = 0.5f;
                const visualization_msgs::MarkerArray parent_state_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, parent_state_config, parent_state_color, 1, "parent_state_marker");
                std_msgs::ColorRGBA current_config_color;
                current_config_color.r = 0.0f;
                current_config_color.g = 0.0f;
                current_config_color.b = 1.0f;
                current_config_color.a = 0.5f;
                const visualization_msgs::MarkerArray current_config_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, current_config, current_config_color, 1, "current_config_marker");
                std_msgs::ColorRGBA action_color;
                action_color.r = 1.0f;
                action_color.g = 0.0f;
                action_color.b = 1.0f;
                action_color.a = 0.5f;
                const visualization_msgs::MarkerArray action_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, action, action_color, 1, "action_marker");
                visualization_msgs::MarkerArray policy_query_markers;
                policy_query_markers.markers.insert(policy_query_markers.markers.end(), parent_state_markers.markers.begin(), parent_state_markers.markers.end());
                policy_query_markers.markers.insert(policy_query_markers.markers.end(), current_config_markers.markers.begin(), current_config_markers.markers.end());
                policy_query_markers.markers.insert(policy_query_markers.markers.end(), action_markers.markers.begin(), action_markers.markers.end());
                display_fn(policy_query_markers);
                if (wait_for_user)
                {
                    std::cout << "Press ENTER to continue & execute..." << std::endl;
                    std::cin.get();
                }
                else
                {
                    // Wait for a bit
                    std::this_thread::sleep_for(std::chrono::duration<double>(0.1));
                }
                // Simulate fowards
                const std::vector<Configuration, ConfigAlloc> execution_states = move_fn(current_config, action, expected_result, is_reverse_action, false);
                particle_trajectory.insert(particle_trajectory.end(), execution_states.begin(), execution_states.end());
                const Configuration result_config = particle_trajectory.back();
                // Check if we've reached the goal
                if (user_goal_check_fn(result_config))
                {
                    // We've reached the goal!
                    Log("Policy execution reached the goal in " + std::to_string(current_exec_step) + " steps", 2);
                    return std::make_pair(particle_trajectory, std::make_pair(policy, (int64_t)current_exec_step));
                }
            }
            // If we get here, we haven't reached the goal!
            Log("Policy execution failed to reach the goal in " + std::to_string(current_exec_step) + " steps", 3);
            return std::make_pair(particle_trajectory, std::make_pair(policy, -((int64_t)current_exec_step)));
        }

    protected:

        inline std::vector<Configuration, ConfigAlloc> SimulatePolicyStep(
                const Configuration& current_config,
                const Configuration& action,
                const bool is_reverse_motion,
                const DisplayFn& display_fn) const
        {
            ForwardSimulationStepTrace<Configuration, ConfigAlloc> trace;
            if (is_reverse_motion == false)
            {
                simulator_ptr_->ForwardSimulateRobot(robot_ptr_, current_config, action, true, trace, true, display_fn);
            }
            else
            {
                simulator_ptr_->ReverseSimulateRobot(robot_ptr_, current_config, action, true, trace, true, display_fn);
            }
            std::vector<Configuration, ConfigAlloc> execution_trajectory = ExtractTrajectoryFromTrace(trace);
            if (execution_trajectory.empty())
            {
                throw std::runtime_error("SimulatePolicyStep execution trajectory is empty, this should not happen!");
            }
            return execution_trajectory;
        }

        /*
         * Drawing functions
         */
        inline void ClearAndRedrawEnvironment(const DisplayFn& display_fn) const
        {
            visualization_msgs::MarkerArray display_markers;
            display_markers.markers.push_back(MakeEraseMarker());
            const visualization_msgs::MarkerArray environment_markers = MakeEnvironmentDisplayRep();
            display_markers.markers.insert(display_markers.markers.end(), environment_markers.markers.begin(), environment_markers.markers.end());
            display_fn(display_markers);
        }

        inline void DrawParticlePolicyExecution(
                const std::string& ns,
                const std::vector<Configuration, ConfigAlloc>& trajectory,
                const DisplayFn& display_fn,
                const double draw_wait, const
                std_msgs::ColorRGBA& color) const
        {
            if (trajectory.size() > 1)
            {
                // Draw one step at a time
                int32_t trace_marker_idx = 1;
                for (size_t idx = 0; idx < trajectory.size(); idx++)
                {
                    const Configuration& current_configuration = trajectory[idx];
                    // Draw a ball at the current location
                    const visualization_msgs::MarkerArray current_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, current_configuration, color, 1, "current_policy_exec");
                    const visualization_msgs::MarkerArray trace_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, current_configuration, color, trace_marker_idx, ns);
                    trace_marker_idx += (int32_t)trace_markers.markers.size();
                    // Send the markers for display
                    visualization_msgs::MarkerArray display_markers;
                    display_markers.markers.insert(display_markers.markers.end(), current_markers.markers.begin(), current_markers.markers.end());
                    display_markers.markers.insert(display_markers.markers.end(), trace_markers.markers.begin(), trace_markers.markers.end());
                    display_fn(display_markers);
                    // Wait for a bit
                    std::this_thread::sleep_for(std::chrono::duration<double>(draw_wait));
                }
            }
            else
            {
                return;
            }
        }

        inline void DrawPolicy(
                const UncertaintyPlanningPolicy& policy,
                const double marker_size,
                const std::string& policy_name,
                const DisplayFn& display_fn) const
        {
            visualization_msgs::MarkerArray policy_display_markers;
            const visualization_msgs::MarkerArray policy_markers = MakePolicyDisplayRep(policy, marker_size, policy_name);
            policy_display_markers.markers.insert(policy_display_markers.markers.end(), policy_markers.markers.begin(), policy_markers.markers.end());
            display_fn(policy_display_markers);
        }

        inline void DrawLocalPolicy(
                const UncertaintyPlanningPolicy& policy,
                const double marker_size,
                const int64_t current_state_idx,
                const std_msgs::ColorRGBA& color,
                const std::string& policy_name,
                const DisplayFn& display_fn) const
        {
            visualization_msgs::MarkerArray policy_display_markers;
            const visualization_msgs::MarkerArray policy_markers = MakeLocalPolicyDisplayRep(policy, marker_size, current_state_idx, color, policy_name);
            policy_display_markers.markers.insert(policy_display_markers.markers.end(), policy_markers.markers.begin(), policy_markers.markers.end());
            display_fn(policy_display_markers);
        }

        inline visualization_msgs::Marker MakeEraseMarker() const
        {
            visualization_msgs::Marker erase_marker;
            erase_marker.action = visualization_msgs::Marker::DELETEALL;
            return erase_marker;
        }

        inline visualization_msgs::MarkerArray MakeEraseMarkers() const
        {
            visualization_msgs::MarkerArray erase_markers;
            erase_markers.markers = {MakeEraseMarker()};
            return erase_markers;
        }

        inline visualization_msgs::MarkerArray MakeEnvironmentDisplayRep() const
        {
            return simulator_ptr_->MakeEnvironmentDisplayRep();
        }

        inline visualization_msgs::MarkerArray MakePolicyDisplayRep(
                const UncertaintyPlanningPolicy& policy,
                const double marker_size,
                const std::string& policy_name) const
        {
            const ExecutionPolicyGraph& policy_graph = policy.GetRawPolicy();
            const common_robotics_utilities::simple_graph_search::DijkstrasResult& policy_dijkstras = policy.GetRawPolicyDijkstrasResult();
            visualization_msgs::MarkerArray policy_markers;
            const std_msgs::ColorRGBA forward_color = MakeColor(0.0f, 0.0f, 0.0f, 1.0f);
            const std_msgs::ColorRGBA backward_color = forward_color;
            const std_msgs::ColorRGBA blue_color = MakeColor(0.0f, 0.0f, 1.0f, 1.0f);
            for (size_t idx = 0; idx < policy_graph.Size(); idx++)
            {
                const int64_t current_index = idx;
                const int64_t previous_index = policy_dijkstras.GetPreviousIndex(current_index);
                if (previous_index < 0)
                {
                    throw std::runtime_error("previous_index < 0");
                }
                if (current_index == previous_index)
                {
                    const Configuration current_config = policy_graph.GetNodeImmutable(current_index).GetValueImmutable().GetExpectation();
                    const visualization_msgs::MarkerArray target_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, current_config, blue_color, 1, "policy_graph_goal");
                    policy_markers.markers.insert(policy_markers.markers.end(), target_markers.markers.begin(), target_markers.markers.end());
                }
                else
                {
                    const Configuration current_config = policy_graph.GetNodeImmutable(current_index).GetValueImmutable().GetExpectation();
                    const Configuration previous_config = policy_graph.GetNodeImmutable(previous_index).GetValueImmutable().GetExpectation();
                    const Eigen::Vector4d current_config_point = simulator_ptr_->Get3dPointForConfig(robot_ptr_, current_config);
                    const Eigen::Vector4d previous_config_point = simulator_ptr_->Get3dPointForConfig(robot_ptr_, previous_config);
                    visualization_msgs::Marker edge_marker;
                    edge_marker.action = visualization_msgs::Marker::ADD;
                    edge_marker.ns = policy_name;
                    edge_marker.id = (int)idx + 1;
                    edge_marker.frame_locked = false;
                    edge_marker.lifetime = ros::Duration(0.0);
                    edge_marker.type = visualization_msgs::Marker::ARROW;
                    edge_marker.header.frame_id = simulator_ptr_->GetFrame();
                    edge_marker.scale.x = marker_size;
                    edge_marker.scale.y = marker_size * 2.0;
                    edge_marker.scale.z = marker_size * 2.0;
                    edge_marker.pose = common_robotics_utilities::conversions::EigenIsometry3dToGeometryPose(Eigen::Isometry3d::Identity());
                    if (current_index < previous_index)
                    {
                        edge_marker.color = forward_color;
                    }
                    else if (previous_index < current_index)
                    {
                        edge_marker.color = backward_color;
                    }
                    else
                    {
                        continue;
                    }
                    edge_marker.points.push_back(common_robotics_utilities::conversions::EigenVector4dToGeometryPoint(current_config_point));
                    edge_marker.points.push_back(common_robotics_utilities::conversions::EigenVector4dToGeometryPoint(previous_config_point));
                    policy_markers.markers.push_back(edge_marker);
                }
            }
            return policy_markers;
        }

        inline visualization_msgs::MarkerArray MakeLocalPolicyDisplayRep(
                const UncertaintyPlanningPolicy& policy,
                const double marker_size,
                const int64_t current_state_idx,
                const std_msgs::ColorRGBA& color,
                const std::string& policy_name) const
        {
            const ExecutionPolicyGraph& policy_graph = policy.GetRawPolicy();
            const common_robotics_utilities::simple_graph_search::DijkstrasResult& policy_dijkstras = policy.GetRawPolicyDijkstrasResult();
            visualization_msgs::MarkerArray policy_markers;
            const Configuration previous_config = policy_graph.GetNodeImmutable(current_state_idx).GetValueImmutable().GetExpectation();
            Eigen::Vector4d previous_point = simulator_ptr_->Get3dPointForConfig(robot_ptr_, previous_config);
            int64_t previous_index = policy_dijkstras.GetPreviousIndex(current_state_idx);
            int idx = 1;
            while (previous_index != -1)
            {
                const int64_t current_idx = previous_index;
                const Configuration current_config = policy_graph.GetNodeImmutable(current_idx).GetValueImmutable().GetExpectation();
                const Eigen::Vector4d current_config_point = simulator_ptr_->Get3dPointForConfig(robot_ptr_, current_config);
                visualization_msgs::Marker edge_marker;
                edge_marker.action = visualization_msgs::Marker::ADD;
                edge_marker.ns = policy_name;
                edge_marker.id = idx;
                idx++;
                edge_marker.frame_locked = false;
                edge_marker.lifetime = ros::Duration(0.0);
                edge_marker.type = visualization_msgs::Marker::ARROW;
                edge_marker.header.frame_id = simulator_ptr_->GetFrame();
                edge_marker.scale.x = marker_size;
                edge_marker.scale.y = marker_size * 2.0;
                edge_marker.scale.z = marker_size * 2.0;
                const Eigen::Isometry3d base_transform = Eigen::Isometry3d::Identity();
                edge_marker.pose = common_robotics_utilities::conversions::EigenIsometry3dToGeometryPose(base_transform);
                edge_marker.color = color;
                edge_marker.points.push_back(common_robotics_utilities::conversions::EigenVector4dToGeometryPoint(previous_point));
                edge_marker.points.push_back(common_robotics_utilities::conversions::EigenVector4dToGeometryPoint(current_config_point));
                policy_markers.markers.push_back(edge_marker);
                previous_index = policy_dijkstras.GetPreviousIndex(current_idx);
                if (previous_index == current_idx)
                {
                    previous_index = -1;
                }
                previous_point = current_config_point;
            }
            return policy_markers;
        }

        inline visualization_msgs::MarkerArray MakeParticlesDisplayRep(
                const std::vector<Configuration, ConfigAlloc>& particles,
                const std_msgs::ColorRGBA& color,
                const std::string& ns) const
        {
            visualization_msgs::MarkerArray markers;
            int32_t starting_idx = 1;
            for (size_t idx = 0; idx < particles.size(); idx++)
            {
                const visualization_msgs::MarkerArray particle_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, particles[idx], color, starting_idx, ns);
                markers.markers.insert(markers.markers.end(), particle_markers.markers.begin(), particle_markers.markers.end());
                starting_idx = (int32_t)markers.markers.size() + 1;
            }
            return markers;
        }

        inline visualization_msgs::MarkerArray MakeParticlesDisplayRep(
                const std::vector<SimulationResult<Configuration>>& particles,
                const std_msgs::ColorRGBA& color,
                const std::string& ns) const
        {
            visualization_msgs::MarkerArray markers;
            int32_t starting_idx = 1;
            for (size_t idx = 0; idx < particles.size(); idx++)
            {
                const visualization_msgs::MarkerArray particle_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, particles[idx].ResultConfig(), color, starting_idx, ns);
                markers.markers.insert(markers.markers.end(), particle_markers.markers.begin(), particle_markers.markers.end());
                starting_idx = (int32_t)markers.markers.size() + 1;
            }
            return markers;
        }

        /*
         * State sampling wrappers
         */
        inline UncertaintyPlanningState SampleRandomTargetState()
        {
            const Configuration random_point = sampler_ptr_->Sample(simulator_ptr_->GetRandomGenerator());
            Log("Sampled config: " + common_robotics_utilities::print::Print(random_point), 0);
            const UncertaintyPlanningState random_state(random_point);
            return random_state;
        }

        inline UncertaintyPlanningState SampleRandomTargetGoalState()
        {
            const Configuration random_goal_point = sampler_ptr_->SampleGoal(simulator_ptr_->GetRandomGenerator());
            Log("Sampled goal config: " + common_robotics_utilities::print::Print(random_goal_point), 0);
            const UncertaintyPlanningState random_goal_state(random_goal_point);
            return random_goal_state;
        }

        /*
         * Particle clustering function used in policy execution
         */
        inline bool PolicyParticleClusteringFn(
                const std::vector<Configuration, ConfigAlloc>& parent_particles,
                const Configuration& current_config,
                const DisplayFn& display_fn) const
        {
            if (parent_particles.empty())
            {
                throw std::invalid_argument("parent_particles cannot be empty");
            }
            std::vector<SimulationResult<Configuration>> result_particles;
            result_particles.push_back(SimulationResult<Configuration>(current_config, current_config, false, false));
            const std::vector<uint8_t> cluster_membership = clustering_ptr_->IdentifyClusterMembers(robot_ptr_, parent_particles, result_particles, display_fn);
            const uint8_t parent_cluster_membership = cluster_membership.at(0);
            if (parent_cluster_membership > 0x00)
            {
                return true;
            }
            else
            {
                return false;
            }
        }

        /*
         * Particle clustering function for planning
         */
        inline std::vector<std::vector<SimulationResult<Configuration>>> ClusterParticles(
                const std::vector<SimulationResult<Configuration>>& particles,
                const bool allow_contacts,
                const DisplayFn& display_fn)
        {
            // Make sure there are particles to cluster
            if (particles.size() == 0)
            {
                return std::vector<std::vector<SimulationResult<Configuration>>>();
            }
            else if (particles.size() == 1)
            {
                return std::vector<std::vector<SimulationResult<Configuration>>>{particles};
            }
            const std::chrono::time_point<std::chrono::high_resolution_clock> start = (std::chrono::time_point<std::chrono::high_resolution_clock>)std::chrono::high_resolution_clock::now();
            const std::vector<std::vector<size_t>> final_index_clusters = clustering_ptr_->ClusterParticles(robot_ptr_, particles, display_fn);
            // Before we return, we need to convert the index clusters to configuration clusters
            std::vector<std::vector<SimulationResult<Configuration>>> final_clusters;
            final_clusters.reserve(final_index_clusters.size());
            size_t total_particles = 0;
            for (size_t cluster_idx = 0; cluster_idx < final_index_clusters.size(); cluster_idx++)
            {
                const std::vector<size_t>& cluster = final_index_clusters[cluster_idx];
                std::vector<SimulationResult<Configuration>> final_cluster;
                final_cluster.reserve(cluster.size());
                for (size_t element_idx = 0; element_idx < cluster.size(); element_idx++)
                {
                    total_particles++;
                    const size_t particle_idx = cluster[element_idx];
                    const SimulationResult<Configuration>& particle = particles.at(particle_idx);
                    if ((particle.DidContact() == false) || allow_contacts)
                    {
                        final_cluster.push_back(particle);
                    }
                }
                final_cluster.shrink_to_fit();
                final_clusters.push_back(final_cluster);
            }
            final_clusters.shrink_to_fit();
            if (total_particles != particles.size())
            {
                throw std::runtime_error("total_particles != particles.size()");
            }
            // Now, return the clusters and probability table
            const std::chrono::time_point<std::chrono::high_resolution_clock> end = (std::chrono::time_point<std::chrono::high_resolution_clock>)std::chrono::high_resolution_clock::now();
            const std::chrono::duration<double> elapsed = end - start;
            elapsed_clustering_time_ += elapsed.count();
            return final_clusters;
        }

        /*
         * Forward propagation functions
         */
        inline std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>> SimulateParticles(
                const UncertaintyPlanningState& nearest,
                const UncertaintyPlanningState& target,
                const bool allow_contacts,
                const bool simulate_reverse,
                const DisplayFn& display_fn)
        {
            const std::chrono::time_point<std::chrono::high_resolution_clock> start = (std::chrono::time_point<std::chrono::high_resolution_clock>)std::chrono::high_resolution_clock::now();
            // First, compute a target state
            const Configuration target_point = target.GetExpectation();
            // Get the initial particles
            std::vector<Configuration, ConfigAlloc> initial_particles;
            // We'd like to use the particles of the parent directly
            if (nearest.GetNumParticles() == num_particles_)
            {
                initial_particles = nearest.CollectParticles(num_particles_);
            }
            // If the number of particles is dynamic based on the simulator
            else if (num_particles_ == 0u)
            {
                initial_particles = nearest.CollectParticles(nearest.GetNumParticles());
            }
            // Otherwise, we resample from the parent
            else
            {
                initial_particles = nearest.ResampleParticles(num_particles_, simulator_ptr_->GetRandomGenerator());
            }
            if (debug_level_ >= 15)
            {
                display_fn(MakeParticlesDisplayRep(initial_particles, MakeColor(0.1f, 0.1f, 0.1f, 1.0f), "initial_particles"));
            }
            // Forward propagate each of the particles
            std::vector<Configuration, ConfigAlloc> target_position;
            target_position.reserve(1);
            target_position.push_back(target_point);
            target_position.shrink_to_fit();
            std::vector<SimulationResult<Configuration>> propagated_points;
            if (simulate_reverse == false)
            {
                propagated_points = simulator_ptr_->ForwardSimulateRobots(robot_ptr_, initial_particles, target_position, allow_contacts, display_fn);
            }
            else
            {
                propagated_points = simulator_ptr_->ReverseSimulateRobots(robot_ptr_, initial_particles, target_position, allow_contacts, display_fn);
            }
            particles_simulated_ += propagated_points.size();
            const std::chrono::time_point<std::chrono::high_resolution_clock> end = (std::chrono::time_point<std::chrono::high_resolution_clock>)std::chrono::high_resolution_clock::now();
            const std::chrono::duration<double> elapsed = end - start;
            elapsed_simulation_time_ += elapsed.count();
            return std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>>(initial_particles, propagated_points);
        }

        inline std::pair<uint32_t, uint32_t> ComputeReverseEdgeProbability(
                const UncertaintyPlanningState& parent,
                const UncertaintyPlanningState& child,
                const DisplayFn& display_fn)
        {
            const std::vector<SimulationResult<Configuration>> simulation_result = SimulateParticles(child, parent, true, true, display_fn).second;
            std::vector<uint8_t> parent_cluster_membership;
            if (parent.HasParticles())
            {
                parent_cluster_membership = clustering_ptr_->IdentifyClusterMembers(robot_ptr_, parent.GetParticlePositionsImmutable().Value(), simulation_result, display_fn);
            }
            else
            {
                const std::vector<Configuration, ConfigAlloc> parent_cluster(1, parent.GetExpectation());
                parent_cluster_membership = clustering_ptr_->IdentifyClusterMembers(robot_ptr_, parent_cluster, simulation_result, display_fn);
            }
            uint32_t reached_parent = 0u;
            // Get the target position;
            for (size_t ndx = 0; ndx < parent_cluster_membership.size(); ndx++)
            {
                if (parent_cluster_membership[ndx] > 0)
                {
                    reached_parent++;
                }
            }
            return std::make_pair((uint32_t)parent_cluster_membership.size(), reached_parent);
        }

        inline std::pair<std::vector<std::pair<UncertaintyPlanningState, int64_t>>, std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>>> ForwardSimulateStates(
                const UncertaintyPlanningState& nearest,
                const UncertaintyPlanningState& target,
                const uint32_t planner_action_try_attempts,
                const bool allow_contacts,
                const bool include_reverse_actions,
                const DisplayFn& display_fn)
        {
            // Increment the transition ID
            transition_id_++;
            const uint64_t current_forward_transition_id = transition_id_;
            // Forward propagate each of the particles
            std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>> simulation_result = SimulateParticles(nearest, target, allow_contacts, false, display_fn);
            std::vector<Configuration, ConfigAlloc>& initial_particles = simulation_result.first;
            std::vector<SimulationResult<Configuration>>& propagated_points = simulation_result.second;
            // Cluster the live particles into (potentially) multiple states
            const std::vector<std::vector<SimulationResult<Configuration>>>& particle_clusters = ClusterParticles(propagated_points, allow_contacts, display_fn);
            bool is_split_child = false;
            if (particle_clusters.size() > 1)
            {
                is_split_child = true;
                split_id_++;
            }
            // Build the forward-propagated states
            // We know in this case that all propagated points will have the same actual target, so we just use the first
            const Configuration control_target = propagated_points.at(0).ActualTarget();
            std::vector<std::pair<UncertaintyPlanningState, int64_t>> result_states(particle_clusters.size());
            for (size_t idx = 0; idx < particle_clusters.size(); idx++)
            {
                const std::vector<SimulationResult<Configuration>>& current_cluster = particle_clusters[idx];
                if (debug_level_ >= 15)
                {
                    display_fn(MakeParticlesDisplayRep(current_cluster, common_robotics_utilities::color_builder::LookupUniqueColor<std_msgs::ColorRGBA>((uint32_t)(idx + 1), 1.0f), "result_cluster_" + std::to_string(idx + 1)));
                }
                if (particle_clusters[idx].size() > 0)
                {
                    state_counter_++;
                    const uint32_t attempt_count = (uint32_t)propagated_points.size();
                    const uint32_t reached_count = (uint32_t)current_cluster.size();
                    // Check if any of the particles in the current cluster collided with the environment during simulation.
                    // If all are collision-free, we can safely assume the edge is trivially reversible
                    std::vector<Configuration, ConfigAlloc> particle_locations(current_cluster.size());
                    bool did_collide = false;
                    bool action_is_nominally_independent = true;
                    for (size_t pdx = 0; pdx < current_cluster.size(); pdx++)
                    {
                        particle_locations[pdx] = current_cluster[pdx].ResultConfig();
                        if (current_cluster[pdx].DidContact())
                        {
                            did_collide = true;
                        }
                        if (current_cluster[pdx].OutcomeIsNominallyIndependent() == false)
                        {
                            action_is_nominally_independent = false;
                        }
                    }
                    particles_stored_ += particle_locations.size();
                    uint32_t reverse_attempt_count = (uint32_t)current_cluster.size();
                    uint32_t reverse_reached_count = (uint32_t)current_cluster.size();
                    // Don't do extra work with one particle
                    if (did_collide && (propagated_points.size() > 1))
                    {
                        reverse_attempt_count = (uint32_t)current_cluster.size();
                        reverse_reached_count = 0u;
                    }
                    else if (is_split_child)
                    {
                        reverse_attempt_count = (uint32_t)current_cluster.size();
                        reverse_reached_count = 0u;
                    }
                    const double effective_edge_feasibility = (double)reached_count / (double)attempt_count;
                    transition_id_++;
                    const uint64_t new_state_reverse_transtion_id = transition_id_;
                    UncertaintyPlanningState propagated_state(state_counter_, particle_locations, attempt_count, reached_count, effective_edge_feasibility, reverse_attempt_count, reverse_reached_count, nearest.GetMotionPfeasibility(), step_size_, control_target, current_forward_transition_id, new_state_reverse_transtion_id, ((is_split_child) ? split_id_ : 0u), action_is_nominally_independent);
                    propagated_state.UpdateStatistics(robot_ptr_);
                    // Store the state
                    result_states[idx].first = propagated_state;
                    result_states[idx].second = -1;
                }
            }
            // Now that we've built the forward-propagated states, we compute their reverse edge P(feasibility)
            uint32_t computed_reversibility = 0u;
            for (size_t idx = 0; idx < result_states.size(); idx++)
            {
                UncertaintyPlanningState& current_state = result_states[idx].first;
                if (include_reverse_actions)
                {
                    // In some cases, we already know the reverse edge P(feasibility) so we don't need to compute it again
                    if (current_state.GetReverseEdgePfeasibility() < 1.0)
                    {
                        const std::pair<uint32_t, uint32_t> reverse_edge_check = ComputeReverseEdgeProbability(nearest, current_state, display_fn);
                        current_state.UpdateReverseAttemptAndReachedCounts(reverse_edge_check.first, reverse_edge_check.second);
                        computed_reversibility++;
                    }
                }
                else
                {
                    current_state.UpdateReverseAttemptAndReachedCounts((uint32_t)current_state.GetNumParticles(), 0u);
                }
            }
            Log("Forward simultation produced " + std::to_string(result_states.size()) + " states, needed to compute reversibility for " + std::to_string(computed_reversibility) + " of them", 1);
            // We only do further processing if a split happened
            if (result_states.size() > 1)
            {
                // Now that we have the forward-propagated states, we go back and update their effective edge P(feasibility)
                for (size_t idx = 0; idx < result_states.size(); idx++)
                {
                    UncertaintyPlanningState& current_state = result_states[idx].first;
                    double percent_active = 1.0;
                    double p_reached = 0.0;
                    for (uint32_t try_attempt = 0; try_attempt < planner_action_try_attempts; try_attempt++)
                    {
                        // How many particles got to our state on this attempt?
                        p_reached += (percent_active * current_state.GetRawEdgePfeasibility());
                        // Update the percent of particles that are still usefully active
                        double updated_percent_active = 0.0;
                        for (size_t other_idx = 0; other_idx < result_states.size(); other_idx++)
                        {
                            if (other_idx != idx)
                            {
                                const UncertaintyPlanningState& other_state = result_states[other_idx].first;
                                // Only if this state has nominally independent outcomes can we expect particles that
                                // return to the parent to actually reach a different outcome in future repeats
                                if (other_state.IsActionOutcomeNominallyIndependent())
                                {
                                    const double p_reached_other = percent_active * other_state.GetRawEdgePfeasibility();
                                    const double p_returned_to_parent = p_reached_other * other_state.GetReverseEdgePfeasibility();
                                    updated_percent_active += p_returned_to_parent;
                                }
                            }
                        }
                        percent_active = updated_percent_active;
                    }
                    if ((p_reached >= 0.0) && (p_reached <= 1.0))
                    {
                        current_state.SetEffectiveEdgePfeasibility(p_reached);
                    }
                    else if ((p_reached >= 0.0) && (p_reached <= 1.001))
                    {
                        Log("WARNING - P(reached) = " + std::to_string(p_reached) + " > 1.0 (probably numerical error)", 1);
                        p_reached = 1.0;
                        current_state.SetEffectiveEdgePfeasibility(p_reached);
                    }
                    else
                    {
                        throw std::runtime_error("p_reached out of range [0, 1]");
                    }
                    Log("Computed effective edge P(feasibility) of " + std::to_string(p_reached) + " for " + std::to_string(planner_action_try_attempts) + " try/retry attempts", 1);
                }
            }
            if (debug_level_ >= 30)
            {
                std::cout << "Press ENTER to add new states..." << std::endl;
                std::cin.get();
            }
            return std::pair<std::vector<std::pair<UncertaintyPlanningState, int64_t>>, std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>>>(result_states, std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>>(initial_particles, propagated_points));
        }

        inline std::vector<std::pair<UncertaintyPlanningState, int64_t>> PropagateForwardsAndDraw(
                const UncertaintyPlanningState& nearest,
                const UncertaintyPlanningState& random,
                const uint32_t planner_action_try_attempts,
                const bool allow_contacts,
                const bool include_reverse_actions,
                const DisplayFn& display_fn)
        {
            // First, perform the forwards propagation
            const std::pair<std::vector<std::pair<UncertaintyPlanningState, int64_t>>, std::vector<std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>>>> propagated_state = PerformForwardPropagation(nearest, random, planner_action_try_attempts, allow_contacts, include_reverse_actions, display_fn);
            if (debug_level_ >= 1)
            {
                // Draw the expansion
                visualization_msgs::MarkerArray propagation_display_rep;
                // Check if the expansion was useful
                if (propagated_state.first.size() > 0)
                {
                    for (size_t idx = 0; idx < propagated_state.first.size(); idx++)
                    {
                        //int64_t state_index = (int64_t)state_counter_ + ((int64_t)idx - ((int64_t)propagated_state.first.size() - 1));
                        // Yeah, sorry about the ternary. This is so we can still have a const reference
                        //const UncertaintyPlanningState& previous_state = (propagated_state.first[idx].second >= 0) ? propagated_state.first[propagated_state.first[idx].second].first : nearest;
                        const UncertaintyPlanningState& current_state = propagated_state.first[idx].first;
                        // Get the edge feasibility
                        const double edge_Pfeasibility = current_state.GetEffectiveEdgePfeasibility();
                        // Get motion feasibility
                        const double motion_Pfeasibility = current_state.GetMotionPfeasibility();
                        // Get the variance
                        const double raw_variance = current_state.GetSpaceIndependentVariance();
                        // Get the reverse feasibility
                        const double reverse_edge_Pfeasibility = current_state.GetReverseEdgePfeasibility();
                        // Now we get markers corresponding to the current states
                        // Make the display color
                        std_msgs::ColorRGBA forward_color;
                        forward_color.r = (float)(1.0 - motion_Pfeasibility);
                        forward_color.g = (float)(1.0 - motion_Pfeasibility);
                        forward_color.b = (float)(1.0 - motion_Pfeasibility);
                        forward_color.a = 1.0f - ((float)(erf(raw_variance) * variance_alpha_));
                        const std::string forward_expectation_marker_ns = (edge_Pfeasibility == 1.0) ? "forward_expectation" : "split_forward_expectation";
                        const visualization_msgs::MarkerArray forward_expectation_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, current_state.GetExpectation(), forward_color, (int32_t)propagation_display_rep.markers.size() + 1, forward_expectation_marker_ns);
                        propagation_display_rep.markers.insert(propagation_display_rep.markers.end(), forward_expectation_markers.markers.begin(), forward_expectation_markers.markers.end());
                        if (reverse_edge_Pfeasibility > 0.5)
                        {
                            // Make the display color
                            std_msgs::ColorRGBA reverse_color;
                            reverse_color.r = (float)(1.0 - motion_Pfeasibility);
                            reverse_color.g = (float)(1.0 - motion_Pfeasibility);
                            reverse_color.b = (float)(1.0 - motion_Pfeasibility);
                            reverse_color.a = (float)reverse_edge_Pfeasibility;
                            const std::string reverse_expectation_marker_ns = (edge_Pfeasibility == 1.0) ? "reverse_expectation" : "split_reverse_expectation";
                            const visualization_msgs::MarkerArray reverse_expectation_markers = simulator_ptr_->MakeConfigurationDisplayRep(robot_ptr_, current_state.GetExpectation(), reverse_color, (int32_t)propagation_display_rep.markers.size() + 1, reverse_expectation_marker_ns);
                            propagation_display_rep.markers.insert(propagation_display_rep.markers.end(), reverse_expectation_markers.markers.begin(), reverse_expectation_markers.markers.end());
                        }
                    }
                }
                display_fn(propagation_display_rep);
            }
            return propagated_state.first;
        }

        inline std::pair<std::vector<std::pair<UncertaintyPlanningState, int64_t>>, std::vector<std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>>>> PerformForwardPropagation(
                const UncertaintyPlanningState& nearest,
                const UncertaintyPlanningState& random,
                const uint32_t planner_action_try_attempts,
                const bool allow_contacts,
                const bool include_reverse_actions,
                const DisplayFn& display_fn)
        {
            const bool solution_already_found = (total_goal_reached_probability_ >= goal_probability_threshold_);
            bool use_extend = false;
            if (solution_already_found)
            {
                std::uniform_real_distribution<double> temp_dist(0.0, 1.0);
                const double draw = temp_dist(simulator_ptr_->GetRandomGenerator());
                if (draw < connect_after_first_solution_)
                {
                    use_extend = false;
                }
                else
                {
                    use_extend = true;
                }
            }
            // First, check if we're going to use RRT-Connect or RRT-Extend
            // If we've already found a solution, we use RRT-Extend
            if (use_extend)
            {
                // Compute a single target state
                Configuration target_point = random.GetExpectation();
                const double target_distance = robot_ptr_->ComputeConfigurationDistance(nearest.GetExpectation(), target_point);
                if (target_distance > step_size_)
                {
                    const double step_fraction = step_size_ / target_distance;
                    Log("Forward simulating for " + std::to_string(step_fraction) + " step fraction, step size is " + std::to_string(step_size_) + ", target distance is " + std::to_string(target_distance), 0);
                    const Configuration interpolated_target_point = robot_ptr_->InterpolateBetweenConfigurations(nearest.GetExpectation(), target_point, step_fraction);
                    target_point = interpolated_target_point;
                }
                else
                {
                    Log("Forward simulating, step size is " + std::to_string(step_size_) + ", target distance is " + std::to_string(target_distance), 0);
                }
                UncertaintyPlanningState target_state(target_point);
                std::pair<std::vector<std::pair<UncertaintyPlanningState, int64_t>>, std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>>> propagation_results = ForwardSimulateStates(nearest, target_state, planner_action_try_attempts, allow_contacts, include_reverse_actions, display_fn);
                std::vector<std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>>> raw_particle_propagations = {propagation_results.second};
                return std::pair<std::vector<std::pair<UncertaintyPlanningState, int64_t>>, std::vector<std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>>>>(propagation_results.first, raw_particle_propagations);
            }
            // If we haven't found a solution yet, we use RRT-Connect
            else
            {
                std::vector<std::pair<UncertaintyPlanningState, int64_t>> propagated_states;
                std::vector<std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>>> raw_particle_propagations;
                int64_t parent_offset = -1;
                // Compute a maximum number of steps to take
                const Configuration target_point = random.GetExpectation();
                // We have to take at least one step
                const uint32_t total_steps = std::max((uint32_t)ceil(robot_ptr_->ComputeConfigurationDistance(nearest.GetExpectation(), target_point) / step_size_), 1u);
                UncertaintyPlanningState current = nearest;
                uint32_t steps = 0;
                bool completed = false;
                while ((completed == false) && (steps < total_steps))
                {
                    // Compute a single target state
                    Configuration current_target_point = target_point;
                    const double target_distance = robot_ptr_->ComputeConfigurationDistance(current.GetExpectation(), current_target_point);
                    if (target_distance > step_size_)
                    {
                        const double step_fraction = step_size_ / target_distance;
                        const Configuration interpolated_target_point = robot_ptr_->InterpolateBetweenConfigurations(current.GetExpectation(), target_point, step_fraction);
                        current_target_point = interpolated_target_point;
                        Log("Forward simulating for " + std::to_string(step_fraction) + " step fraction, step size is " + std::to_string(step_size_) + ", target distance is " + std::to_string(target_distance), 0);
                    }
                    // TODO: NOT SURE WHY THIS WAS EVER HERE
//                    // If we've reached the target state, stop
//                    else if (std::abs(target_distance) < std::numeric_limits<double>::epsilon())
//                    {
//                        completed = true;
//                        break;
//                    }
                    // If we're less than step size away, this is our last step
                    else
                    {
                        Log("Forward simulating last step towars target, step size is " + std::to_string(step_size_) + ", target distance is " + std::to_string(target_distance), 0);
                        completed = true;
                    }
                    // Take a step forwards
                    UncertaintyPlanningState target_state(current_target_point);
                    std::pair<std::vector<std::pair<UncertaintyPlanningState, int64_t>>, std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>>> propagation_results = ForwardSimulateStates(nearest, target_state, planner_action_try_attempts, allow_contacts, include_reverse_actions, display_fn);
                    raw_particle_propagations.push_back(propagation_results.second);
                    const std::vector<std::pair<UncertaintyPlanningState, int64_t>>& simulation_results = propagation_results.first;
                    // If simulation results in a single new state, we keep going
                    if (simulation_results.size() == 1)
                    {
                        const UncertaintyPlanningState& new_state = simulation_results[0].first;
                        propagated_states.push_back(std::pair<UncertaintyPlanningState, int64_t>(new_state, parent_offset));
                        current = propagated_states.back().first;
                        parent_offset++;
                        steps++;
                    }
                    // If simulation results in multiple new states, this is the end
                    else if (simulation_results.size() > 1)
                    {
                        for (size_t idx = 0; idx < simulation_results.size(); idx++)
                        {
                            const UncertaintyPlanningState& new_state = simulation_results[idx].first;
                            propagated_states.push_back(std::pair<UncertaintyPlanningState, int64_t>(new_state, parent_offset));
                        }
                        completed = true;
                    }
                    // Otherwise, we're done
                    else
                    {
                        completed = true;
                    }
                }
                return std::pair<std::vector<std::pair<UncertaintyPlanningState, int64_t>>, std::vector<std::pair<std::vector<Configuration, ConfigAlloc>, std::vector<SimulationResult<Configuration>>>>>(propagated_states, raw_particle_propagations);
            }
        }

        /*
         * Goal check and solution handling functions
         */
        inline double ComputeGoalReachedProbability(
                const UncertaintyPlanningState& state,
                const Configuration& goal) const
        {
            size_t within_distance = 0;
            auto particle_check = state.GetParticlePositionsImmutable();
            const std::vector<Configuration, ConfigAlloc>& particles = particle_check.Value();
            for (size_t idx = 0; idx < particles.size(); idx++)
            {
                const double distance = robot_ptr_->ComputeConfigurationDistance(particles[idx], goal);
                if (distance < goal_distance_threshold_)
                {
                    within_distance++;
                }
            }
            double percent_in_range = (double)within_distance / (double)particles.size();
            return percent_in_range;
        }

        inline bool GoalReachedGoalFunction(
                const UncertaintyPlanningState& state,
                const GoalReachedProbabilityFn& user_goal_check_fn,
                const uint32_t planner_action_try_attempts,
                const bool allow_contacts)
        {
            // *** WARNING ***
            // !!! WE IGNORE THE PROVIDED GOAL STATE, AND INSTEAD ACCESS IT VIA NEAREST-NEIGHBORS STORAGE !!!
            UNUSED(state);
            UNUSED(planner_action_try_attempts);
            UNUSED(allow_contacts);
            UncertaintyPlanningState& goal_state_candidate = nearest_neighbors_storage_.back().GetValueMutable();
            // NOTE - this assumes (safely) that the state passed to this function is the last state added to the tree, which we can safely mutate!
            // We only care about states with control input == goal position (states that are directly trying to go to the goal)
            const double goal_reached_probability = user_goal_check_fn(goal_state_candidate);
            if (goal_reached_probability > 0.0)
            {
                goal_candidates_evaluated_++;
                const double start_to_goal_probability = goal_reached_probability * goal_state_candidate.GetMotionPfeasibility();
                if (start_to_goal_probability >= goal_probability_threshold_)
                {
                    // Update the state
                    goal_state_candidate.SetGoalPfeasibility(goal_reached_probability);
                    Log("Goal reached with state " + goal_state_candidate.Print() + " with probability(this->goal): " + std::to_string(goal_reached_probability) + " and probability(start->goal): " + std::to_string(start_to_goal_probability), 2);
                    return true;
                }
            }
            return false;
        }

        inline bool GoalReachedGoalState(
                const UncertaintyPlanningState& state,
                const UncertaintyPlanningState& goal_state,
                const uint32_t planner_action_try_attempts,
                const bool allow_contacts)
        {
            // *** WARNING ***
            // !!! WE IGNORE THE PROVIDED GOAL STATE, AND INSTEAD ACCESS IT VIA NEAREST-NEIGHBORS STORAGE !!!
            UNUSED(state);
            UNUSED(planner_action_try_attempts);
            UNUSED(allow_contacts);
            UncertaintyPlanningState& goal_state_candidate = nearest_neighbors_storage_.back().GetValueMutable();
            // NOTE - this assumes (safely) that the state passed to this function is the last state added to the tree, which we can safely mutate!
            // We only care about states with control input == goal position (states that are directly trying to go to the goal)
            if (robot_ptr_->ComputeConfigurationDistance(goal_state_candidate.GetCommand(), goal_state.GetExpectation()) == 0.0)
            {
                goal_candidates_evaluated_++;
                const double goal_reached_probability = ComputeGoalReachedProbability(goal_state_candidate, goal_state.GetExpectation());
                const double goal_probability = goal_reached_probability * goal_state_candidate.GetMotionPfeasibility();
                if (goal_probability >= goal_probability_threshold_)
                {
                    // Update the state
                    goal_state_candidate.SetGoalPfeasibility(goal_reached_probability);
                    Log("Goal reached with state " + goal_state_candidate.Print() + " with probability(this->goal): " + std::to_string(goal_reached_probability) + " and probability(start->goal): " + std::to_string(goal_probability), 2);
                    return true;
                }
            }
            return false;
        }

        inline void GoalReachedCallback(
                UncertaintyPlanningTree& tree,
                const int64_t new_goal_state_idx,
                const uint32_t planner_action_try_attempts,
                const std::chrono::time_point<std::chrono::high_resolution_clock>& start_time)
        {
            UncertaintyPlanningTreeState& new_goal = tree[new_goal_state_idx];
            // Update the time-to-first-solution if need be
            if (time_to_first_solution_ == 0.0)
            {
                const std::chrono::time_point<std::chrono::high_resolution_clock> current_time = (std::chrono::time_point<std::chrono::high_resolution_clock>)std::chrono::high_resolution_clock::now();
                const std::chrono::duration<double> elapsed = current_time - start_time;
                time_to_first_solution_ = elapsed.count();
            }
            // Backtrack through the solution path until we reach the root of the current "goal branch"
            // A goal branch is the entire branch leading to the goal
            int64_t current_index = new_goal_state_idx;
            int64_t goal_branch_root_index = -1; // Initialize to an invalid index so we can detect later if it isn't valid
            while (current_index > 0)
            {
                // Get the current state that we're looking at
                UncertaintyPlanningTreeState& current_state = nearest_neighbors_storage_[(size_t)current_index];
                // Check if we've reached the root of the goal branch
                bool is_branch_root = CheckIfGoalBranchRoot(current_state);
                // If we haven't reached the root of goal branch
                if (!is_branch_root)
                {
                    current_index = current_state.GetParentIndex();
                }
                else
                {
                    goal_branch_root_index = current_index;
                    break;
                }
            }
            BlacklistGoalBranch(goal_branch_root_index);
            // Update the goal reached probability
            // Backtrack all the way to the goal, updating each state's goal_Pfeasbility
            // Make sure something hasn't gone wrong
            if (new_goal.GetValueImmutable().GetGoalPfeasibility() == 0.0)
            {
                throw std::runtime_error("new_goal cannot reach the goal (GoalPfeasibility() == 0)");
            }
            // Backtrack up the tree, updating states as we go
            int64_t probability_update_index = new_goal.GetParentIndex();
            while (probability_update_index >= 0)
            {
                // Get the current state that we're looking at
                UncertaintyPlanningTreeState& current_state = nearest_neighbors_storage_[(size_t)probability_update_index];
                // Update the state
                UpdateNodeGoalReachedProbability(current_state, planner_action_try_attempts);
                probability_update_index = current_state.GetParentIndex();
            }
            // Get the goal reached probability that we use to decide when we're done
            total_goal_reached_probability_ = nearest_neighbors_storage_[0].GetValueImmutable().GetGoalPfeasibility();
            Log("Updated goal reached probability to " + std::to_string(total_goal_reached_probability_), 2);
        }

        inline void BlacklistGoalBranch(
                const int64_t goal_branch_root_index)
        {
            if (goal_branch_root_index < 0)
            {
                ;
            }
            else if (goal_branch_root_index == 0)
            {
                Log("Blacklisting with goal branch root == tree root is not possible!", 3);
            }
            else
            {
                // Get the current node
                auto& current_state = nearest_neighbors_storage_[(size_t)goal_branch_root_index];
                // Recursively blacklist it
                current_state.GetValueMutable().DisableForNearestNeighbors();
                // Blacklist each child
                const std::vector<int64_t>& child_indices = current_state.GetChildIndices();
                for (size_t idx = 0; idx < child_indices.size(); idx++)
                {
                    int64_t child_index = child_indices[idx];
                    BlacklistGoalBranch(child_index);
                }
            }
        }

        inline bool CheckIfGoalBranchRoot(
                const UncertaintyPlanningTreeState& state) const
        {
            // There are three ways a state can be the the root of a goal branch
            // 1) The transition leading to the state is low-probability
            const bool has_low_probability_transition = (state.GetValueImmutable().GetEffectiveEdgePfeasibility() < goal_probability_threshold_);
            // 2) The transition leading to the state is the result of an unresolved split
            const bool is_child_of_split = (state.GetValueImmutable().GetSplitId() > 0u) ? true : false;
            // If we're a child of a split, check to see if the split has been resolved:
            // 2a) - the P(goal reached) of the parent is 1
            // 2b) - all the other children with the same transition are already blacklisted
            bool is_child_of_unresolved_split = false;
            if (is_child_of_split)
            {
                const UncertaintyPlanningTreeState& parent_tree_state = nearest_neighbors_storage_[(size_t)state.GetParentIndex()];
                const UncertaintyPlanningState& parent_state = parent_tree_state.GetValueImmutable();
                if (parent_state.GetGoalPfeasibility() >= 1.0)
                {
                    is_child_of_unresolved_split = false;
                }
                else
                {
                    bool other_children_blacklisted = true;
                    const std::vector<int64_t>& other_parent_children = parent_tree_state.GetChildIndices();
                    for (size_t idx = 0; idx < other_parent_children.size(); idx++)
                    {
                        const int64_t other_child_index = other_parent_children[idx];
                        const UncertaintyPlanningTreeState& other_child_tree_state = nearest_neighbors_storage_[(size_t)other_child_index];
                        const UncertaintyPlanningState& other_child_state = other_child_tree_state.GetValueImmutable();
                        if (other_child_state.GetTransitionId() == state.GetValueImmutable().GetTransitionId() && other_child_state.UseForNearestNeighbors())
                        {
                            other_children_blacklisted = false;
                        }
                    }
                    if (other_children_blacklisted)
                    {
                        is_child_of_unresolved_split = false;
                    }
                    else
                    {
                        is_child_of_unresolved_split = true;
                    }
                }
            }
            // 3) The parent of the current node is the root of the tree
            const bool parent_is_root = (state.GetParentIndex() == 0);
            // If one or more condition is true, the state is a branch root
            if (has_low_probability_transition || is_child_of_unresolved_split || parent_is_root)
            {
                return true;
            }
            else
            {
                return false;
            }
        }

        inline void UpdateNodeGoalReachedProbability(
                UncertaintyPlanningTreeState& current_node,
                const uint32_t planner_action_try_attempts)
        {
            // Check all the children of the current node, and update the node's goal reached probability accordingly
            //
            // Naively, the goal reached probability of a node is the maximum of the child goal reached probabilities;
            // intuitively, the probability of reaching the goal is that of reaching the goal if we follow the best child.
            //
            // HOWEVER - the existence of "split" child states, where multiple states result from a single control input,
            // makes this more compilcated. For split child states, the goal reached probability of the split is the sum
            // over every split option of (split goal probability * probability of split)
            //
            // We can identify split nodes as children which share a transition id
            // First, we go through the children and separate them based on transition id (this puts all the children of a
            // split together in one place)
            std::map<uint64_t, std::vector<int64_t>> effective_child_branches;
            for (size_t idx = 0; idx < current_node.GetChildIndices().size(); idx++)
            {
                const int64_t& current_child_index = current_node.GetChildIndices()[idx];
                const uint64_t& child_transition_id = nearest_neighbors_storage_[(size_t)current_child_index].GetValueImmutable().GetTransitionId();
                effective_child_branches[child_transition_id].push_back(current_child_index);
            }
            // Now that we have the transitions separated out, compute the goal probability of each transition
            std::vector<double> effective_child_branch_probabilities;
            for (auto itr = effective_child_branches.begin(); itr != effective_child_branches.end(); ++itr)
            {
                const double transition_goal_probability = ComputeTransitionGoalProbability(itr->second, planner_action_try_attempts);
                effective_child_branch_probabilities.push_back(transition_goal_probability);
            }
            // Now, get the highest transtion probability
            double max_transition_probability = 0.0;
            if (effective_child_branch_probabilities.size() > 0)
            {
                max_transition_probability = *std::max_element(effective_child_branch_probabilities.begin(), effective_child_branch_probabilities.end());
            }
            if ((max_transition_probability < 0.0) || (max_transition_probability > 1.0))
            {
                throw std::runtime_error("max_transition_probability out of range [0, 1]");
            }
            // Update the current state
            current_node.GetValueMutable().SetGoalPfeasibility(max_transition_probability);
        }

        inline double ComputeTransitionGoalProbability(
                const std::vector<int64_t>& child_node_indices,
                const uint32_t planner_action_try_attempts) const
        {
            std::vector<UncertaintyPlanningState> child_states(child_node_indices.size());
            for (size_t idx = 0; idx < child_node_indices.size(); idx++)
            {
                // Get the current child
                const int64_t& current_child_index = child_node_indices[idx];
                const UncertaintyPlanningState& current_child = nearest_neighbors_storage_[(size_t)current_child_index].GetValueImmutable();
                child_states[idx] = current_child;
            }
            return ComputeTransitionGoalProbability(child_states, planner_action_try_attempts);
        }

        inline double ComputeTransitionGoalProbability(
                const std::vector<UncertaintyPlanningState>& child_nodes,
                const uint32_t planner_action_try_attempts) const
        {
            Log("Computing transition goal probability with " + std::to_string(child_nodes.size()) + " child nodes", 1);
            // Let's handle the special cases first
            // The most common case - a non-split transition
            if (child_nodes.size() == 1)
            {
                const UncertaintyPlanningState& current_child = child_nodes.front();
                return (current_child.GetGoalPfeasibility() * current_child.GetEffectiveEdgePfeasibility());
            }
            // IMPOSSIBLE (but we handle it just to be sure)
            else if (child_nodes.size() == 0)
            {
                return 0.0;
            }
            // Let's handle the split case(s)
            else
            {
                // We do this the right way
                std::vector<double> action_outcomes_dependent_child_goal_reached_probabilities;
                std::vector<double> action_outcomes_independent_child_goal_reached_probabilities;
                // For each child state, we compute the probability that we'll end up at each of the result states, accounting for try/retry with reversibility
                // This lets us compare child states as if they were separate actions, so the overall P(goal reached) = max(child) P(goal reached | child)
                for (size_t idx = 0; idx < child_nodes.size(); idx++)
                {
                    // Get the current child
                    const UncertaintyPlanningState& current_child = child_nodes[idx];
                    Log("Child node: " + current_child.Print(), 0);
                    // For the selected child, we keep track of the probability that we reach the goal directly via the child state AND the probability that we reach the goal from unintended other child states
                    double percent_active = 1.0;
                    double p_we_reached_goal = 0.0;
                    double p_others_reached_goal = 0.0;
                    for (uint32_t try_attempt = 0; try_attempt < planner_action_try_attempts; try_attempt++)
                    {
                        // How many particles got to our state on this attempt?
                        const double p_reached = percent_active * current_child.GetRawEdgePfeasibility();
                        p_we_reached_goal += (p_reached * current_child.GetGoalPfeasibility());
                        // Update the percent of particles that are still usefully active
                        // and the probability that the goal was reached via a different child
                        double updated_percent_active = 0.0;
                        for (size_t other_idx = 0; other_idx < child_nodes.size(); other_idx++)
                        {
                            if (other_idx != idx)
                            {
                                // Get the other child
                                const UncertaintyPlanningState& other_child = child_nodes[other_idx];
                                // CORRECTION (TODO: is this right?) If it its not independent, we cannot reach it if we are not there!
                                // Only if this state has nominally independent outcomes can we expect particles that
                                // return to the parent to actually reach a different outcome in future repeats
                                if (other_child.IsActionOutcomeNominallyIndependent())
                                {
                                    const double p_reached_other = percent_active * other_child.GetRawEdgePfeasibility();
                                    const double p_stuck_at_other = p_reached_other * (1.0 - other_child.GetReverseEdgePfeasibility());
                                    const double p_reached_goal_from_other = p_stuck_at_other * other_child.GetGoalPfeasibility();
                                    p_others_reached_goal += p_reached_goal_from_other;
                                    const double p_returned_to_parent = p_reached_other * other_child.GetReverseEdgePfeasibility();
                                    updated_percent_active += p_returned_to_parent;
                                }
                            }
                        }
                        percent_active = updated_percent_active;
                    }
                    Log("P(child->goal) via ourself " + std::to_string(p_we_reached_goal), 0);
                    Log("P(child->goal) via others " + std::to_string(p_others_reached_goal), 0);
                    double p_reached_goal = p_we_reached_goal + p_others_reached_goal;
                    if ((p_reached_goal < 0.0) || (p_reached_goal > 1.0))
                    {
                        if ((p_reached_goal >= 0.0) && (p_reached_goal <= 1.001))
                        {
                            Log("WARNING - P(reached goal) = " + std::to_string(p_reached_goal) + " > 1.0 (probably numerical error)", 1);
                            p_reached_goal = 1.0;
                        }
                        else
                        {
                            throw std::runtime_error("p_reached_goal out of range [0, 1]");
                        }
                    }
                    Log("P(child->goal) " + std::to_string(p_reached_goal), 0);
                    if (current_child.IsActionOutcomeNominallyIndependent())
                    {
                        action_outcomes_independent_child_goal_reached_probabilities.push_back(p_reached_goal);
                    }
                    else
                    {
                        action_outcomes_dependent_child_goal_reached_probabilities.push_back(p_reached_goal);
                    }
                }
                Log("action_outcomes_dependent_child_goal_reached_probabilities: " + common_robotics_utilities::print::Print(action_outcomes_dependent_child_goal_reached_probabilities), 1);
                Log("action_outcomes_independent_child_goal_reached_probabilities: " + common_robotics_utilities::print::Print(action_outcomes_independent_child_goal_reached_probabilities), 1);
                const double dependent_child_goal_reached_probability = common_robotics_utilities::math::Sum(action_outcomes_dependent_child_goal_reached_probabilities);
                const double independent_child_goal_reached_probability = (action_outcomes_independent_child_goal_reached_probabilities.size() > 0) ? *std::max_element(action_outcomes_independent_child_goal_reached_probabilities.begin(), action_outcomes_independent_child_goal_reached_probabilities.end()) : 0.0;
                const double total_p_goal_reached = independent_child_goal_reached_probability + dependent_child_goal_reached_probability;
                Log("dependent_child_goal_reached_probability " + std::to_string(dependent_child_goal_reached_probability) + " independent_child_goal_reached_probability " + std::to_string(independent_child_goal_reached_probability) + " total_p_goal_reached " + std::to_string(total_p_goal_reached), 1);
                if ((total_p_goal_reached >= 0.0) && (total_p_goal_reached <= 1.0))
                {
                    return total_p_goal_reached;
                }
                else if ((total_p_goal_reached >= 0.0) && (total_p_goal_reached <= 1.001))
                {
                    Log("WARNING - total P(goal reached) = " + std::to_string(total_p_goal_reached) + " > 1.0 (probably numerical error)", 1);
                    return 1.0;
                }
                else
                {
                    throw std::runtime_error("total_p_goal_reached out of range [0, 1]");
                }
            }
        }

        /*
         * Check if we should stop planning (have we reached the time limit?)
         */
        inline bool PlannerTerminationCheck(
                const std::chrono::time_point<std::chrono::high_resolution_clock>& start_time,
                const std::chrono::duration<double>& time_limit,
                const double p_goal_termination_threshold) const
        {
            const bool time_limit_reached = (((std::chrono::time_point<std::chrono::high_resolution_clock>)std::chrono::high_resolution_clock::now() - start_time) > time_limit);
            if (time_limit_reached)
            {
                Log("Terminating, reached time limit", 0);
                return true;
            }
            else if (p_goal_termination_threshold > 0.0)
            {
                const double p_goal_gap = p_goal_termination_threshold - total_goal_reached_probability_;
                if (p_goal_gap <= 1e-10)
                {
                    Log("Terminating, reached p_goal_termination_threshold", 0);
                    return true;
                }
            }
            return false;
        }
    };
}  // namespace uncertainty_planning_core
