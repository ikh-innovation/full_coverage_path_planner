//
// Copyright [2020] Nobleo Technology"  [legal/copyright]
//
#include <algorithm>
#include <iostream>
#include <list>
#include <string>
#include <vector>

#include "full_coverage_path_planner/spiral_stc.hpp"

using nav2_util::declare_parameter_if_not_declared;
namespace full_coverage_path_planner
{
SpiralSTC::SpiralSTC()
{
}

SpiralSTC::~SpiralSTC()
{
  RCLCPP_INFO(
    rclcpp::get_logger(
      "FullCoveragePathPlanner"), "Destroying plugin %s of type FullCoveragePathPlanner",
    name_.c_str());
}

void SpiralSTC::configure(
  const rclcpp_lifecycle::LifecycleNode::WeakPtr & parent,
  std::string name, std::shared_ptr<tf2_ros::Buffer> tf,
  std::shared_ptr<nav2_costmap_2d::Costmap2DROS> costmap_ros)
{
  if (!initialized_) {
    // Get node from parent
    node_ = parent.lock();
    name_ = name;

    // Currently this plugin does not use the costmap, instead request a map from a server (will change in the future)
    costmap_ = costmap_ros->getCostmap();
    global_frame_ = costmap_ros->getGlobalFrameID();
    planner_grid_ros = costmap_ros.get();

    RCLCPP_INFO(
      rclcpp::get_logger("FullCoveragePathPlanner"),
      "Configuring plugin %s of type NavfnPlanner", name_.c_str());

    // Create publishers to visualize the planner output
    plan_pub_ = node_->create_publisher<nav_msgs::msg::Path>("plan", 1);
    grid_pub = node_->create_publisher<visualization_msgs::msg::Marker>("grid", 0);
    spirals_pub = node_->create_publisher<visualization_msgs::msg::Marker>("spirals", 0);

    // Define vehicle width
    double vehicle_width_default = 1.1;
    declare_parameter_if_not_declared(
      node_, name_ + ".vehicle_width", rclcpp::ParameterValue(vehicle_width_default));
    node_->get_parameter(name_ + ".vehicle_width", vehicle_width_);
    // Define grid division factor (how many cells fit in one vehicle width)
    int division_factor_default = 3;
    declare_parameter_if_not_declared(
      node_, name_ + ".division_factor", rclcpp::ParameterValue(division_factor_default));
    node_->get_parameter(name_ + ".division_factor", division_factor_);
    // Define numer of intermediate footprints used to compute the swept path of a manoeuvre
    int manoeuvre_resolution_default = 100;
    declare_parameter_if_not_declared(
      node_, name_ + ".manoeuvre_resolution", rclcpp::ParameterValue(manoeuvre_resolution_default));
    node_->get_parameter(name_ + ".manoeuvre_resolution", manoeuvre_resolution_);

    initialized_ = true;
  }
}

void SpiralSTC::activate()
{
  RCLCPP_INFO(
    rclcpp::get_logger(
      "FullCoveragePathPlanner"), "Activating plugin %s of type FullCoveragePathPlanner",
    name_.c_str());
  grid_pub->on_activate();
  spirals_pub->on_activate();
  // TODO(AronTiemessen): Should other things happen here?
}

void SpiralSTC::deactivate()
{
  RCLCPP_INFO(
    rclcpp::get_logger(
      "FullCoveragePathPlanner"), "Deactivating plugin %s of type FullCoveragePathPlanner",
    name_.c_str());
  grid_pub->on_deactivate();
  spirals_pub->on_deactivate();
  // TODO(AronTiemessen): Should other things happen here?
}

void SpiralSTC::cleanup()
{
  RCLCPP_INFO(
    rclcpp::get_logger(
      "FullCoveragePathPlanner"), "Cleaning up plugin %s of type FullCoveragePathPlanner",
    name_.c_str());
  plan_pub_.reset();
  grid_pub.reset();
  spirals_pub.reset();
  // TODO(CesarLopez): Add proper cleanup
}

nav_msgs::msg::Path SpiralSTC::createPlan(
  const geometry_msgs::msg::PoseStamped & start,
  const geometry_msgs::msg::PoseStamped & goal)
{
  nav_msgs::msg::Path global_path;
  SpiralSTC::makePlan(start, goal, global_path.poses);

  global_path.header.stamp = node_->now();
  global_path.header.frame_id = global_frame_;

  return global_path;
}

std::list<gridNode_t> SpiralSTC::spiral(
  std::vector<std::vector<bool>> const & grid, std::list<gridNode_t> & init, double & yaw_start,
  std::vector<std::vector<bool>> & visited)
{
  spiral_cpp_metrics_type save_spiral_cpp_metrics;
  std::list<gridNode_t> path_nodes(init);  // Copy incoming init list to path_nodes
  std::list<gridNode_t>::iterator it = --(path_nodes.end());  // Create iterator and let it point to the last element of end
  if (path_nodes.size() > 1) {  // If list is length 1, keep iterator at end
    it--;  // Let iterator point to second to last element
  }
  gridNode_t prev = *(it);

  // Initialize spiral direction towards robot's y-axis
  int dx = 0;
  int dy = 1;
  int dx_prev;
  double yaw_current = yaw_start;  // This value will change later on

  // Mark initial footprint of the tool as visited
  std::vector<nav2_costmap_2d::MapLocation> init_cells;
  if (!computeFootprintCells(
      path_nodes.back().pos.x, path_nodes.back().pos.y, yaw_current, "tool", init_cells))
  {
    RCLCPP_ERROR(
      rclcpp::get_logger(
        "FullCoveragePathPlanner"), "Starting footprint seems to be out of bounds!");
  }
  for (const auto init_cell : init_cells) {
    if (init_cell.x == 14) {    RCLCPP_ERROR(
      rclcpp::get_logger(
        "FullCoveragePathPlanner"), "HHHHEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEERRREEEEEEEEEEE");}
    visited[init_cell.y][init_cell.x] = eNodeVisited;
    visited_copy[init_cell.y][init_cell.x] = eNodeVisited;
  }

  // Start the spiralling procedure
  bool done = false, restored = false, spiral_has_safe_node = false;
  int explored_dir;
  std::vector<nav2_costmap_2d::MapLocation> man_cells, visited_cells, unsafe_visited_cells;
  std::list<gridNode_t>::iterator it_safe_node = path_nodes.begin();
  while (!done) {
    int x_current = path_nodes.back().pos.x;
    int y_current = path_nodes.back().pos.y;

    if (it != path_nodes.begin()) {
      // Turn counter-clockwise
      dx = x_current - prev.pos.x;
      dy = y_current - prev.pos.y;
      yaw_current = std::atan2(dy, dx);
      dx_prev = dx;
      dx = -dy;
      dy = dx_prev;
    }

    // Save the starting node as the most recent safe node if indeed it is safe to turn around
    bool current_node_is_safe = false;
    if (transformRelativeManoeuvre(
        x_current, y_current, yaw_current, vehicle_turn_around_left_rel, man_cells))
    {
      if (checkManoeuvreCollision(man_cells, grid)) {
        current_node_is_safe = true;
        spiral_has_safe_node = true;
        save_spiral_cpp_metrics = spiral_cpp_metrics_;
        unsafe_visited_cells.clear();
        it_safe_node = --(path_nodes.end());  // Can turn around left without obstacles or going out of bounds, so remember as a safe node
      }
    }
    if (transformRelativeManoeuvre(
        x_current, y_current, yaw_current, vehicle_turn_around_right_rel, man_cells))
    {
      if (checkManoeuvreCollision(man_cells, grid)) {
        current_node_is_safe = true;
        spiral_has_safe_node = true;
        save_spiral_cpp_metrics = spiral_cpp_metrics_;
        unsafe_visited_cells.clear();
        it_safe_node = --(path_nodes.end());  // Can turn around right without obstacles or going out of bounds, so remember as a safe node
      }
    }

    int i_start = 0;  // Create a variable that dictates in which direction (out of the 3) the spiral will start looking to explore
    if (restored) {
      // If the planner just restored to a recent safe node, set i_start, dx and dy so that it will try exploring the next untested direction
      i_start = explored_dir + 1;
      spiral_cpp_metrics_ = save_spiral_cpp_metrics;
      for (int i = 0; i < i_start; i++) {
        // Turn counter-clockwise until dx and dy represent the direction of i_start
        dx_prev = dx;
        dx = dy;
        dy = -dx_prev;
      }
    }

    // Loop over the three possible directions: left, forward, right (directions taken before initial counter-clockwise turn)
    done = true;  // This condition might change in the loop below
    int collision_manoeuvres = 0;  // Counting how many (of the three) manoeuvres result in a collision
    for (int i = i_start; i < 3; ++i) {
      restored = false;
      int x_next = path_nodes.back().pos.x + dx;
      int y_next = path_nodes.back().pos.y + dy;
      double yaw_next = std::atan2(y_next - y_current, x_next - x_current);
      RCLCPP_INFO(
        rclcpp::get_logger(
          "FullCoveragePathPlanner"), "Manoeuvre from (x=%d, y=%d, yaw=%f) to (x=%d, y=%d, yaw=%f)",
        x_current, y_current, yaw_current, x_next, y_next, yaw_next);

      // Apply a rotation to the relative manoeuvre cells to convert from robot frame to world frame
      bool man_is_free = true;  // This condition might change in the loop below
      if (i == 0) {  // Transform standard relative left turn manoeuvre
        if (!transformRelativeManoeuvre(
            x_current, y_current, yaw_current, vehicle_left_turn_rel, man_cells) ||
          !transformRelativeManoeuvre(
            x_current, y_current, yaw_current, tool_left_turn_rel, visited_cells))
        {
          man_is_free = false;
          RCLCPP_INFO(
            rclcpp::get_logger(
              "FullCoveragePathPlanner"), "  --> out of bounds, looking in other directions...");
        }
        max_overlap = max_overlap_turn;
      } else if (i == 1) {  // Transform standard relative forward manoeuvre
        if (!transformRelativeManoeuvre(
            x_current, y_current, yaw_current, vehicle_forward_rel, man_cells) ||
          !transformRelativeManoeuvre(
            x_current, y_current, yaw_current, tool_forward_rel, visited_cells))
        {
          man_is_free = false;
          RCLCPP_INFO(
            rclcpp::get_logger(
              "FullCoveragePathPlanner"), "  --> out of bounds, looking in other directions...");
        }
        max_overlap = max_overlap_forward;
      } else if (i == 2) {  // Transform standard relative right turn manoeuvre
        if (!transformRelativeManoeuvre(
            x_current, y_current, yaw_current, vehicle_right_turn_rel, man_cells) ||
          !transformRelativeManoeuvre(
            x_current, y_current, yaw_current, tool_right_turn_rel, visited_cells))
        {
          man_is_free = false;
          RCLCPP_INFO(
            rclcpp::get_logger(
              "FullCoveragePathPlanner"), "  --> out of bounds, looking in other directions...");
        }
        max_overlap = max_overlap_turn;
      }

      // Check the transformed manoeuvre cells of the vehicle for collisions
      if (!checkManoeuvreCollision(man_cells, grid)) {
        collision_manoeuvres += 1;  // Keep track of how many of the tested directions are rejected due to a possible collision
        man_is_free = false;
        RCLCPP_INFO(
          rclcpp::get_logger(
            "FullCoveragePathPlanner"), "  --> causing a collision, looking in other directions");
      }

      // Merely for debugging purposes
      if (spiral_has_safe_node) {
        gridNode_t safe_node = *(it_safe_node);
        RCLCPP_INFO(
          rclcpp::get_logger(
            "FullCoveragePathPlanner"), "  --> the most recent safe node is (x=%d,y=%d)",
          safe_node.pos.x, safe_node.pos.y);
      }

      if (collision_manoeuvres == 3 && spiral_has_safe_node) {  // Meaning all possible manoeuvres would result in a collision
        RCLCPP_INFO(
          rclcpp::get_logger(
            "FullCoveragePathPlanner"),
          "!!!!!!!!!!!! All 3 directions lead to a collision !!!!!!!!!!!!");
        std::list<gridNode_t>::iterator it_start_unsafe_path = ++(it_safe_node);
        RCLCPP_INFO(
          rclcpp::get_logger(
            "FullCoveragePathPlanner"),
          "  --> length before removing unsafe nodes: %lu", path_nodes.size());
        path_nodes.erase(it_start_unsafe_path, path_nodes.end());
        RCLCPP_INFO(
          rclcpp::get_logger(
            "FullCoveragePathPlanner"),
          "  --> length after removing unsafe nodes: %lu", path_nodes.size());

        // Unmark the visited cells belonging to the removed nodes
        for (const auto unsafe_visited_cell : unsafe_visited_cells) {
          visited[unsafe_visited_cell.y][unsafe_visited_cell.x] = eNodeOpen;
          visited_copy[unsafe_visited_cell.y][unsafe_visited_cell.x] = eNodeOpen;
        }

        // Restore footprints of some of the final nodes in the remaining path, as they were possibly affected by the above operation
        for (int i = 0; i < division_factor_; i++) {
          if (static_cast<size_t>(i) < path_nodes.size()) {
            std::list<gridNode_t>::iterator it = --(path_nodes.end());
            std::advance(it, -i);
            gridNode_t this_node = *(it);
            std::advance(it, -1);
            gridNode_t node_before = *(it);
            double yaw = std::atan2(
              this_node.pos.y - node_before.pos.y,
              this_node.pos.x - node_before.pos.x);
            std::vector<nav2_costmap_2d::MapLocation> cells;
            computeFootprintCells(this_node.pos.x, this_node.pos.y, yaw, "tool", cells);
            for (const auto cell : cells) {
              visited[cell.y][cell.x] = eNodeVisited;
              visited_copy[cell.y][cell.x] = eNodeVisited;
            }
          }
        }
        // Set up variables needed for the continuation of the spiralling loop
        it = --(path_nodes.end());
        std::list<gridNode_t>::iterator it_prev = --it;
        prev = *(it_prev);
        restored = true;
        done = false;
        RCLCPP_INFO(
          rclcpp::get_logger(
            "FullCoveragePathPlanner"),
          "!!!!!!!!!!!! Restored to the most recent safe node !!!!!!!!!!!!");
        break;
      }

      // Check the manoeuvre cells of the tool for overlap
      int overlap = 0;
      for (const auto visited_cell : visited_cells) {
        if (grid[visited_cell.y][visited_cell.x] == eNodeOpen &&
          visited[visited_cell.y][visited_cell.x] == eNodeVisited)
        {
          overlap++;
        }
      }

      RCLCPP_INFO(
        rclcpp::get_logger(
          "FullCoveragePathPlanner"),
        "  --> manoeuvre size=%lu (& %lu for tool) of which %d are overlapping",
        man_cells.size(), visited_cells.size(), overlap);

      // When all conditions are met, add the point to path_nodes and mark the covered cells as visited
      if (man_is_free && overlap <= max_overlap) {
        Point_t new_point = {x_next, y_next};
        gridNode_t new_node = {new_point, 0, 0};
        prev = path_nodes.back();
        path_nodes.push_back(new_node);
        it = --(path_nodes.end());
        if (current_node_is_safe) {  // Store the direction of exploration made from the safe node
          explored_dir = i;  // The for loop iterator i that represents the chosen direction
          save_spiral_cpp_metrics = spiral_cpp_metrics_;
        }
        spiral_cpp_metrics_.visited_counter += visited_cells.size();
        spiral_cpp_metrics_.multiple_pass_counter += overlap;
        unsafe_visited_cells.insert(
          unsafe_visited_cells.end(), visited_cells.begin(), visited_cells.end());  // Will be cleared once a node is considered safe
        for (const auto visited_cell : visited_cells) {
              if (visited_cell.x == 14) {    RCLCPP_ERROR(
      rclcpp::get_logger(
        "FullCoveragePathPlanner"), "HHHHEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEERRREEEEEEEEEEE");}
          visited[visited_cell.y][visited_cell.x] = eNodeVisited;
          visited_copy[visited_cell.y][visited_cell.x] = eNodeVisited;
        }
        done = false;
        break;
      }
      // Try next direction clockwise
      dx_prev = dx;
      dx = dy;
      dy = -dx_prev;
    }
  }
  return path_nodes;
}

std::list<Point_t> SpiralSTC::spiral_stc(
  std::vector<std::vector<bool>> const & grid, Point_t & init, double & yaw_start)
{
  spiral_cpp_metrics_.multiple_pass_counter = 0;
  spiral_cpp_metrics_.visited_counter = 0;

  std::vector<std::vector<bool>> visited;
  visited = grid;  // Copy grid matrix
  visited_copy.resize(visited[0].size(), std::vector<bool>(visited.size()));  // For debugging purposes, to only see the grids that are marked as visited by the spirals
  int init_x = init.x;
  int init_y = init.y;
  Point_t new_point = {init_x, init_y};
  gridNode_t new_node = {new_point, 0, 0};
  std::list<gridNode_t> path_nodes;
  std::list<Point_t> full_path;
  path_nodes.push_back(new_node);

  RCLCPP_INFO(
    rclcpp::get_logger("FullCoveragePathPlanner"),
    "!!!!!!!!!!!! Starting a spiral from (x=%d, y=%d, yaw=%f) !!!!!!!!!!!!", init_x, init_y,
    yaw_start);
  path_nodes = spiral(grid, path_nodes, yaw_start, visited);  // First spiral fill

  visualizeSpiral(path_nodes, "first_spiral", 0.2, 0.5, 0.0, 0.6, 0.0);

  std::list<Point_t> goals = map_2_goals(visited, eNodeOpen);  // Retrieve remaining goal points

  for (const auto path_node : path_nodes) {  // Add points to full path
    Point_t new_point = {path_node.pos.x, path_node.pos.y};
    // TODO(Aron): Remove this??
    // visited_counter++;
    full_path.push_back(new_point);
  }

  while (goals.size() != 0) {
    spiral_counter++;  // Count number of spirals planned
    if (spiral_counter == 1) {
      RCLCPP_INFO(
        rclcpp::get_logger(
          "FullCoveragePathPlanner"),
        "@@@@@@@@@ BREAK INSERTED TO ONLY PLAN CERTAIN AMOUNT OF SPIRALS @@@@@@@@@");  // For debugging purposes
      break;
    }

    // Remove all elements from path_nodes list except last element
    // The last point is the starting point for a new search and A* extends the path from there on
    path_nodes.erase(path_nodes.begin(), --(path_nodes.end()));
    // visited_counter--;  // First point is already counted as visited
    // TODO(Aron): Remove this??

    RCLCPP_INFO(
      rclcpp::get_logger(
        "FullCoveragePathPlanner"), "!!!!!!!!!!!! Starting an A* path from (x=%d, y=%d) !!!!!!!!!!!!",
      path_nodes.back().pos.x, path_nodes.back().pos.y);
    // Plan to closest open Node using A*. Here, `goals` is essentially the map, so we use `goals`
    // to determine the distance from the end of a potential path to the nearest free space
    bool accept_a_star = false;
    bool resign;
    while (!accept_a_star) {
      resign = a_star_to_open_space(grid, path_nodes.back(), 1, visited, goals, path_nodes);
      if (resign) {
        break;
      }
      int x_n = path_nodes.back().pos.x;
      int y_n = path_nodes.back().pos.y;
      std::list<gridNode_t>::iterator it = --(path_nodes.end());
      if (path_nodes.size() > 1) {
        it--;
      } else {
        RCLCPP_INFO(
          rclcpp::get_logger(
            "FullCoveragePathPlanner"), "!!!@@@ A* path is only 1 node big @@@!!!");
        break;
      }
      gridNode_t prev = *(it);
      double yaw = atan2(y_n - prev.pos.y, x_n - prev.pos.x);
      std::vector<nav2_costmap_2d::MapLocation> a_star_end_vehicle_footprint;
      std::vector<nav2_costmap_2d::MapLocation> a_star_end_tool_footprint;
      accept_a_star = computeFootprintCells(x_n, y_n, yaw, "vehicle", a_star_end_vehicle_footprint);
      computeFootprintCells(x_n, y_n, yaw, "tool", a_star_end_tool_footprint);
      for (const auto a_star_end_vehicle_cell : a_star_end_vehicle_footprint) {
        if (grid[a_star_end_vehicle_cell.y][a_star_end_vehicle_cell.x] == eNodeVisited) {
          accept_a_star = false;
          break;
        }
      }
      int visit_count = 0;
      for (const auto a_star_end_tool_cell : a_star_end_tool_footprint) {
        if (visited[a_star_end_tool_cell.y][a_star_end_tool_cell.x] == eNodeVisited) {
          visit_count++;
        }
      }
      if (!accept_a_star || visit_count > max_overlap_turn) {
        RCLCPP_INFO(
          rclcpp::get_logger("FullCoveragePathPlanner"),
          "~~~ A* is not accepted, grid considered visited");
        visited[y_n][x_n] = eNodeVisited;
        path_nodes.erase(++(path_nodes.begin()), path_nodes.end());
        accept_a_star = false;
      }
    }
    if (resign) {
      break;
    }

    visited = visited_copy;

    RCLCPP_INFO(
      rclcpp::get_logger("FullCoveragePathPlanner"),
      "--> size of A* path to closest open node is %lu", path_nodes.size());

    gridNode_t last_node_a_star = path_nodes.back();  // Save the value of the final node of the A star path to compare to later

    RCLCPP_INFO(
      rclcpp::get_logger("FullCoveragePathPlanner"),
      "!!!!!!!!!!!! Starting a spiral from (x=%d, y=%d, yaw=?) !!!!!!!!!!!!",
      path_nodes.back().pos.x, path_nodes.back().pos.y);
    // Spiral fill from current position (added to A* transition path)
    path_nodes = spiral(grid, path_nodes, yaw_start, visited);  // It overwrites yaw_start if its not a first spiral

    // Need to extract only the spiral part for visualization
    std::list<gridNode_t> path_nodes_spiral = path_nodes;  // Work with a copy so that path_nodes is not affected later
    std::list<gridNode_t>::iterator it = path_nodes_spiral.begin();
    for (const auto path_node_spiral : path_nodes_spiral) {
      if (path_node_spiral.pos.x == last_node_a_star.pos.x &&
        path_node_spiral.pos.y == last_node_a_star.pos.y)
      {
        break;  // Break if iterator is found that matches last node of A* path
      } else if (it == path_nodes_spiral.end()) {
        break;  // Break if it is already at last node to prevent it becoming bigger than path_nodes_spiral
      }
      ++it;  // Iterate one further to find starting point of new spiral
    }
    path_nodes_spiral.erase(path_nodes_spiral.begin(), it);
    if (path_nodes_spiral.size() > 1) {
      SpiralSTC::visualizeSpiral(
        path_nodes_spiral, "spiral" + std::to_string(goals.size() + 1), 0.2, 0.5, 0.0, 0.6, 0.0);
    }

    goals = map_2_goals(visited, eNodeOpen);  // Retrieve remaining goal points

    // TODO(Aron): dit weg??
    for (const auto path_node : path_nodes) {
      Point_t new_point = {path_node.pos.x, path_node.pos.y};
      // visited_counter++;
      full_path.push_back(new_point);
    }
  }

  visualizeGrid(visited, "visited_cubes", 0.3, 0.0, 0.0, 0.8);
  visualizeGrid(visited_copy, "visited_cubes_copy", 0.3, 0.0, 0.8, 0.0);

  return full_path;
}

bool SpiralSTC::makePlan(
  const geometry_msgs::msg::PoseStamped & start, const geometry_msgs::msg::PoseStamped & goal,
  std::vector<geometry_msgs::msg::PoseStamped> & plan)
{
  if (!initialized_) {
    RCLCPP_ERROR(
      rclcpp::get_logger(
        "FullCoveragePathPlanner"),
      "This planner has not been initialized yet, but it is being used, please call initialize() before use");
    return false;
  } else {
    RCLCPP_INFO(rclcpp::get_logger("FullCoveragePathPlanner"), "Initialized!");
  }

  clock_t begin = clock();
  Point_t start_point;
  double yaw_start;

  std::vector<std::vector<bool>> grid;
  if (!parseGrid(costmap_, grid, vehicle_width_ / division_factor_, start, start_point, yaw_start))
  {
    RCLCPP_ERROR(rclcpp::get_logger("FullCoveragePathPlanner"), "Could not parse retrieved grid");
    return false;
  }

  std::vector<geometry_msgs::msg::Point> test = planner_grid_ros->getRobotFootprint();
  for (auto const point : test) {
    RCLCPP_INFO(rclcpp::get_logger("FullCoveragePathPlanner"), "Robot footprint : (x=%f,y=%f)", point.x, point.y);
  }

  planner_grid.resizeMap(
    ceil(costmap_->getSizeInMetersX() / tile_size_),
    ceil(costmap_->getSizeInMetersY() / tile_size_), tile_size_, grid_origin_.x, grid_origin_.y);

  // Grid visualization with occupied cells greyed out
  visualizeGridlines();
  visualizeGrid(grid, "grid_cubes", 0.6, 0.0, 0.0, 0.0);

  // Compute the standard manoeuvres (for vehicle and tool) to be reused in the spiralling loop later
  vehicle_left_turn_rel = computeRelativeManoeuvreFootprint(
    0, 1, std::atan2(1, 0), eAnyDirection, "vehicle", manoeuvre_resolution_);
  vehicle_forward_rel = computeRelativeManoeuvreFootprint(
    1, 0, std::atan2(0, 1), eAnyDirection, "vehicle", manoeuvre_resolution_);
  vehicle_right_turn_rel = computeRelativeManoeuvreFootprint(
    0, -1, std::atan2(-1, 0), eAnyDirection, "vehicle", manoeuvre_resolution_);
  vehicle_turn_around_left_rel = computeRelativeManoeuvreFootprint(
    0, 0, M_PI, eCounterClockwise, "vehicle", manoeuvre_resolution_);
  vehicle_turn_around_right_rel = computeRelativeManoeuvreFootprint(
    0, 0, M_PI, eClockwise, "vehicle", manoeuvre_resolution_);
  tool_left_turn_rel = computeRelativeManoeuvreFootprint(
    0, 1, std::atan2(1, 0), eAnyDirection, "tool", 3);
  tool_forward_rel = computeRelativeManoeuvreFootprint(
    1, 0, std::atan2(0, 1), eAnyDirection, "tool", 3);
  tool_right_turn_rel = computeRelativeManoeuvreFootprint(
    0, -1, std::atan2(-1, 0), eAnyDirection, "tool", 3);

  std::list<Point_t> goal_points = spiral_stc(grid, start_point, yaw_start);
  RCLCPP_INFO(rclcpp::get_logger("FullCoveragePathPlanner"), "Naive cpp completed!");
  RCLCPP_INFO(rclcpp::get_logger("FullCoveragePathPlanner"), "Converting path to plan");

  parsePointlist2Plan(start, goal_points, plan);

  // Print and compute some metrics:
  RCLCPP_INFO(rclcpp::get_logger("FullCoveragePathPlanner"), "~~~~~~~ Performance metrics ~~~~~~~");
  spiral_cpp_metrics_.accessible_counter = spiral_cpp_metrics_.visited_counter -
    spiral_cpp_metrics_.multiple_pass_counter;
  int to_be_cleaned_cells = 0;
  for (size_t iy = 0; iy < planner_grid.getSizeInCellsY(); iy++) {
    for (size_t ix = 0; ix < planner_grid.getSizeInCellsX(); ix++) {
      if (grid[iy][ix] == eNodeOpen) {
        to_be_cleaned_cells += 1;
      }
    }
  }
  RCLCPP_INFO(
    rclcpp::get_logger(
      "FullCoveragePathPlanner"), "To be cleaned: %d out of %d cells",
      to_be_cleaned_cells, planner_grid.getSizeInCellsX() * planner_grid.getSizeInCellsY());
  RCLCPP_INFO(
    rclcpp::get_logger(
      "FullCoveragePathPlanner"), "Total visited: %d cells",
      spiral_cpp_metrics_.visited_counter);
  RCLCPP_INFO(
    rclcpp::get_logger(
      "FullCoveragePathPlanner"), "Total revisited: %d cells",
      spiral_cpp_metrics_.multiple_pass_counter);
  RCLCPP_INFO(
    rclcpp::get_logger(
      "FullCoveragePathPlanner"), "Total accessible cells: %d",
    spiral_cpp_metrics_.accessible_counter);
  RCLCPP_INFO(rclcpp::get_logger("FullCoveragePathPlanner"), "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");

  // TODO(CesarLopez): Check if global path should be calculated repetitively or just kept (also controlled by planner_frequency parameter in move_base namespace)

  RCLCPP_INFO(rclcpp::get_logger("FullCoveragePathPlanner"), "Publishing plan!");
  publishPlan(plan);
  RCLCPP_INFO(rclcpp::get_logger("FullCoveragePathPlanner"), "Plan published!");

  clock_t end = clock();
  double elapsed_secs = static_cast<double>(end - begin) / CLOCKS_PER_SEC;
  RCLCPP_INFO(rclcpp::get_logger("FullCoveragePathPlanner"), "Elapsed time: %f", elapsed_secs);

  return true;
}

bool SpiralSTC::computeFootprintCells(
  int & x_m, int & y_m, double & yaw, std::string part,
  std::vector<nav2_costmap_2d::MapLocation> & footprint_cells)
{
  // Convert input map locations to world coordinates
  double x_w, y_w;
  planner_grid.mapToWorld(x_m, y_m, x_w, y_w);
  std::vector<geometry_msgs::msg::Point> footprint;

  // Differentiate between a footprint requested for a vehicle and its tool
  if (part == "vehicle") {
    nav2_costmap_2d::transformFootprint(
      x_w, y_w, yaw, planner_grid_ros->getRobotFootprint(), footprint);
  } else if (part == "tool") {
    // TODO(Aron): This will eventually be published by the coverage tracker tool, for now hardcoded
    std::vector<geometry_msgs::msg::Point> tool_footprint;
    geometry_msgs::msg::Point p;
    p.x = 0.2;  // This is the real dimension according to the cad model
    p.y = 0.4;
    tool_footprint.push_back(p);
    p.x = 0.545;  // This is the real dimension according to the cad model
    p.y = 0.4;
    tool_footprint.push_back(p);
    p.x = 0.545;  // This is the real dimension according to the cad model
    p.y = -0.4;
    tool_footprint.push_back(p);
    p.x = 0.2;  // This is the real dimension according to the cad model
    p.y = -0.4;
    tool_footprint.push_back(p);
    nav2_costmap_2d::transformFootprint(x_w, y_w, yaw, tool_footprint, footprint);
  }

  // Convert footprint from Point vector to MapLocation vector
  int x_max = planner_grid.getSizeInCellsX() - 1;
  int y_max = planner_grid.getSizeInCellsY() - 1;
  std::vector<nav2_costmap_2d::MapLocation> footprint_ML;
  for (const auto point : footprint) {
    int map_x, map_y;
    planner_grid.worldToMapNoBounds(point.x, point.y, map_x, map_y);  // Use worldToMapNoBounds() variant because of bugs in the one that enforces bounds
    if (!checkMapBounds(map_x, map_y, x_max, y_max)) {
      return false;  // When the requested footprint falls out of bounds, the function returns false
    }
    nav2_costmap_2d::MapLocation map_loc{static_cast<uint>(map_x), static_cast<uint>(map_y)};
    footprint_ML.push_back(map_loc);
  }

  // Filter out double corner points that happen due to the grid resolution to prevent problems with convexFillCells() later
  std::vector<int> footprint_inds;
  for (const auto maploc : footprint_ML) {
    int index = planner_grid.getIndex(maploc.x, maploc.y);
    footprint_inds.push_back(index);
  }
  std::sort(footprint_inds.begin(), footprint_inds.end());
  footprint_inds.erase(
    std::unique(footprint_inds.begin(), footprint_inds.end()), footprint_inds.end());
  footprint_ML.clear();
  for (const auto ind : footprint_inds) {
    nav2_costmap_2d::MapLocation map_loc;
    planner_grid.indexToCells(ind, map_loc.x, map_loc.y);
    footprint_ML.push_back(map_loc);
  }

  // Find the cells below the convex footprint and save them
  if (footprint_ML.size() < 3) {
    RCLCPP_INFO(
      rclcpp::get_logger(
        "FullCoveragePathPlanner"), "Footprint does not consists of 3 or more points!");
  }
  planner_grid.convexFillCells(footprint_ML, footprint_cells);
  return true;
}

bool SpiralSTC::computeManoeuvreFootprint(
  int & x_current, int & y_current, double & yaw_current, int & x_next, int & y_next,
  double yaw_next, eRotateDirection direction, std::string part, int manoeuvre_resolution,
  std::vector<nav2_costmap_2d::MapLocation> & man_cells)
{
  // Determine the footprint of the manoeuvre starting pose
  std::vector<nav2_costmap_2d::MapLocation> footprint1_cells;
  if (part == "vehicle" &&
    !computeFootprintCells(x_current, y_current, yaw_current, "vehicle", footprint1_cells))
  {
    return false;
  } else if (part == "tool" &&
    !computeFootprintCells(x_current, y_current, yaw_current, "tool", footprint1_cells))
  {
    return false;
  }

  // Manually compute the difference in special cases
  double yaw_diff;
  if (yaw_current == -0.5 * M_PI && yaw_next == M_PI) {
    yaw_diff = -0.5 * M_PI;
  } else if (yaw_current == M_PI && yaw_next == -0.5 * M_PI) {
    yaw_diff = 0.5 * M_PI;
  } else {
    yaw_diff = yaw_next - yaw_current;
  }

  // Dictate the rotation direction, unless direction = any
  if (yaw_diff < 0 && direction == eCounterClockwise) {
    yaw_diff += 2 * M_PI;
  } else if (yaw_diff > 0 && direction == eClockwise) {
    yaw_diff -= 2 * M_PI;
  }

  // Determine the footprint of the intermediate poses of the manoeuvre
  std::vector<nav2_costmap_2d::MapLocation> intermediate_cells;
  double yaw_inter;
  for (int i = 1; i <= manoeuvre_resolution - 2; i++) {  // Substract 2 due to initial and final pose are computed seperately
    std::vector<nav2_costmap_2d::MapLocation> cells;
    yaw_inter = yaw_current + (i * yaw_diff) / (manoeuvre_resolution - 2);

    // Wrap angle back to (-PI, PI]
    if (yaw_inter > M_PI) {
      yaw_inter -= 2 * M_PI;
    } else if (yaw_inter < -M_PI) {
      yaw_inter += 2 * M_PI;
    }

    // Compute intermediate footprint
    if (part == "vehicle" &&
      !computeFootprintCells(x_current, y_current, yaw_inter, "vehicle", cells))
    {
      return false;
    } else if (part == "tool" &&
      !computeFootprintCells(x_current, y_current, yaw_inter, "tool", cells))
    {
      return false;
    }

    intermediate_cells.insert(intermediate_cells.end(), cells.begin(), cells.end());
  }

  // Determine the footprint of the manoeuvre ending pose
  std::vector<nav2_costmap_2d::MapLocation> footprint2_cells;
  if (part == "vehicle" &&
    !computeFootprintCells(x_next, y_next, yaw_next, "vehicle", footprint2_cells))
  {
    return false;
  } else if (part == "tool" &&
    !computeFootprintCells(x_next, y_next, yaw_next, "tool", footprint2_cells))
  {
    return false;
  }
  footprint2_cells.insert(
    footprint2_cells.end(), intermediate_cells.begin(), intermediate_cells.end());

  // Save the indexes of the covered cells for footprint 1
  std::vector<int> footprint1_inds;
  for (const auto footprint1_cell : footprint1_cells) {
    int footprint1_ind = planner_grid.getIndex(footprint1_cell.x, footprint1_cell.y);
    footprint1_inds.push_back(footprint1_ind);
  }

  // Find the indexes of the covered cells for footprint 2
  std::vector<int> man_inds;
  for (const auto footprint2_cell : footprint2_cells) {
    int footprint2_ind = planner_grid.getIndex(footprint2_cell.x, footprint2_cell.y);
    bool allow = true;
    // Check if an index of footprint 2 matches with any of the indexes of footprint 1
    for (const auto footprint1_ind : footprint1_inds) {
      if (footprint2_ind == footprint1_ind) {
        allow = false;
        break;
      }
    }
    // If that is not the case, save it into the output vector
    if (allow == true) {
      man_inds.push_back(footprint2_ind);
    }
  }

  // Get rid of duplicates in the to-be-checked cells and convert back to MapLocations
  std::sort(man_inds.begin(), man_inds.end());
  man_inds.erase(unique(man_inds.begin(), man_inds.end()), man_inds.end());
  for (const auto man_ind : man_inds) {
    nav2_costmap_2d::MapLocation map_loc;
    planner_grid.indexToCells(man_ind, map_loc.x, map_loc.y);
    man_cells.push_back(map_loc);
  }

  return true;
}

std::vector<Point_t> SpiralSTC::computeRelativeManoeuvreFootprint(
  int dx, int dy, double dyaw, eRotateDirection direction, std::string part,
  int manoeuvre_resolution)
{
  // Find a location on the map so that the manoeuvre will not be out of bounds
  int x_start = planner_grid.getSizeInCellsX() / 2;
  int y_start = planner_grid.getSizeInCellsY() / 2;
  double yaw_start = 0.0;  // Relative manoeuvres are defined with respect to a rotation of 0.0

  // Define the final pose after the manoeuvre
  int x_end = x_start + dx;
  int y_end = y_start + dy;
  double yaw_end = yaw_start + dyaw;

  // Compute the swepth path of the manoeuvre in terms of map locations
  std::vector<nav2_costmap_2d::MapLocation> man_cells;
  computeManoeuvreFootprint(
    x_start, y_start, yaw_start, x_end, y_end, yaw_end, direction, part, manoeuvre_resolution,
    man_cells);

  // Convert absolute cell locations to relative cell locations for each manoeuvre
  std::vector<Point_t> man_cells_rel;
  man_cells_rel.resize(man_cells.size());
  for (size_t i = 0; i < man_cells_rel.size(); i++) {
    man_cells_rel[i] = {static_cast<int>(man_cells[i].x) - x_start,
      static_cast<int>(man_cells[i].y) - y_start};
  }

  return man_cells_rel;
}

bool SpiralSTC::transformRelativeManoeuvre(
  int & x, int & y, double & yaw, std::vector<Point_t> & rel_man,
  std::vector<nav2_costmap_2d::MapLocation> & man_cells)
{
  // Determine map extremes
  int x_max = planner_grid.getSizeInCellsX() - 1;
  int y_max = planner_grid.getSizeInCellsY() - 1;

  // Transform (translate and rotate) the relative manoeuvre to the input map location
  man_cells.clear();
  man_cells.resize(rel_man.size());
  for (size_t i = 0; i < man_cells.size(); i++) {
    Point_t p = rotatePoint(x + rel_man[i].x, y + rel_man[i].y, x, y, yaw);
    if (!checkMapBounds(p.x, p.y, x_max, y_max)) {
      RCLCPP_INFO(
        rclcpp::get_logger(
          "FullCoveragePathPlanner"), "Manoeuvre after transformation out of bounds");
      return false;
    } else {
      man_cells[i] = {static_cast<uint>(p.x), static_cast<uint>(p.y)};
    }
  }
  return true;
}

Point_t SpiralSTC::rotatePoint(int poi_x, int poi_y, int & icr_x, int & icr_y, double & yaw)
{
  Point_t p;
  double poi_x_w, poi_y_w, icr_x_w, icr_y_w, rotated_x_w, rotated_y_w;
  planner_grid.mapToWorld(poi_x, poi_y, poi_x_w, poi_y_w);
  planner_grid.mapToWorld(icr_x, icr_y, icr_x_w, icr_y_w);
  rotated_x_w = icr_x_w + (poi_x_w - icr_x_w) * cos(yaw) - (poi_y_w - icr_y_w) * sin(yaw);
  rotated_y_w = icr_y_w + (poi_x_w - icr_x_w) * sin(yaw) + (poi_y_w - icr_y_w) * cos(yaw);
  planner_grid.worldToMapNoBounds(rotated_x_w, rotated_y_w, p.x, p.y);
  return p;
}

bool SpiralSTC::checkMapBounds(int x, int y, int & x_max, int & y_max)
{
  if (x < 0 || x > x_max || y < 0 || y > y_max) {
    return false;
  }
  return true;
}

bool SpiralSTC::checkManoeuvreCollision(
  std::vector<nav2_costmap_2d::MapLocation> & man_cells,
  std::vector<std::vector<bool>> const & grid)
{
  for (const auto man_cell : man_cells) {
    if (grid[man_cell.y][man_cell.x] == eNodeVisited) {
      return false;
    }
  }
  return true;
}

/* @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ Visualization Methods @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ */

void SpiralSTC::visualizeGridlines()
{
  visualization_msgs::msg::Marker grid_lines;
  grid_lines.header.frame_id = global_frame_.c_str();
  grid_lines.header.stamp = rclcpp::Time();
  grid_lines.ns = "grid_lines";
  grid_lines.id = 0;
  grid_lines.type = visualization_msgs::msg::Marker::LINE_LIST;
  grid_lines.action = visualization_msgs::msg::Marker::ADD;
  grid_lines.pose.orientation.w = 1.0;
  grid_lines.scale.x = 0.02;
  grid_lines.color.a = 0.5;
  grid_lines.color.r = grid_lines.color.g = grid_lines.color.b = 0.0;

  geometry_msgs::msg::Point p;

  for (uint32_t i = 0; i < costmap_->getSizeInMetersX() / tile_size_; i++) {
    p.x = (grid_origin_.x);
    p.y = (grid_origin_.y) + i * tile_size_;
    p.z = 0.0;
    grid_lines.points.push_back(p);
    p.x = (grid_origin_.x) + costmap_->getSizeInMetersX();
    grid_lines.points.push_back(p);
  }

  for (uint32_t i = 0; i < costmap_->getSizeInMetersY() / tile_size_; i++) {
    p.x = (grid_origin_.x) + i * tile_size_;
    p.y = (grid_origin_.y);
    p.z = 0.0;
    grid_lines.points.push_back(p);
    p.y = (grid_origin_.y) + costmap_->getSizeInMetersY();
    grid_lines.points.push_back(p);
  }

  grid_pub->publish(grid_lines);
}

void SpiralSTC::visualizeGrid(
  std::vector<std::vector<bool>> const & grid, std::string name_space, float a, float r, float g,
  float b)
{
  visualization_msgs::msg::Marker grid_cubes = cubeMarker(
    global_frame_.c_str(), name_space, 0, tile_size_, a, r, g, b);
  geometry_msgs::msg::Point p;
  int ix, iy;
  int n_rows = grid.size();
  int n_cols = grid[0].size();

  for (iy = 0; iy < n_rows; ++(iy)) {
    for (ix = 0; ix < n_cols; ++(ix)) {
      if (grid[iy][ix] == true) {
        p.x = (ix + 0.5) * tile_size_ + grid_origin_.x;
        p.y = (iy + 0.5) * tile_size_ + grid_origin_.y;
        grid_cubes.points.push_back(p);
      }
    }
  }

  grid_pub->publish(grid_cubes);
}

void SpiralSTC::visualizeSpiral(
  std::list<gridNode_t> & spiral_nodes, std::string name_space, float w, float a, float r, float g,
  float b)
{
  visualization_msgs::msg::Marker spiral = lineStrip(
    global_frame_.c_str(), name_space, 0, w, a, r, g, b);
  geometry_msgs::msg::Point p;
  p.z = 0.0;
  for (const auto spiral_node : spiral_nodes) {
    p.x = (spiral_node.pos.x + 0.5) * tile_size_ + grid_origin_.x;
    p.y = (spiral_node.pos.y + 0.5) * tile_size_ + grid_origin_.y;
    spiral.points.push_back(p);
  }
  spirals_pub->publish(spiral);
}

visualization_msgs::msg::Marker SpiralSTC::cubeMarker(
  std::string frame_id, std::string name_space, int id, float size, float a, float r, float g,
  float b)
{
  visualization_msgs::msg::Marker cube_list;
  cube_list.header.frame_id = frame_id;
  cube_list.header.stamp = rclcpp::Time();
  cube_list.ns = name_space;
  cube_list.action = visualization_msgs::msg::Marker::ADD;
  cube_list.pose.orientation.w = 1.0;
  cube_list.id = id;
  cube_list.type = visualization_msgs::msg::Marker::CUBE_LIST;
  cube_list.scale.x = size;
  cube_list.scale.y = size;
  cube_list.scale.z = size;
  cube_list.color.a = a;
  cube_list.color.r = r;
  cube_list.color.g = g;
  cube_list.color.b = b;
  return cube_list;
}

visualization_msgs::msg::Marker SpiralSTC::lineStrip(
  std::string frame_id, std::string name_space, int id, float size, float a, float r, float g,
  float b)
{
  visualization_msgs::msg::Marker line_strip;
  line_strip.header.frame_id = frame_id;
  line_strip.header.stamp = rclcpp::Time();
  line_strip.ns = name_space;
  line_strip.action = visualization_msgs::msg::Marker::ADD;
  line_strip.pose.orientation.w = 1.0;
  line_strip.id = id;
  line_strip.type = visualization_msgs::msg::Marker::LINE_STRIP;
  line_strip.scale.x = size;
  line_strip.scale.y = size;
  line_strip.scale.z = size;
  line_strip.color.a = a;
  line_strip.color.r = r;
  line_strip.color.g = g;
  line_strip.color.b = b;
  return line_strip;
}

}  // namespace full_coverage_path_planner

// Register this planner as a nav2_core::GlobalPlanner plugin
#include <pluginlib/class_list_macros.hpp>
PLUGINLIB_EXPORT_CLASS(full_coverage_path_planner::SpiralSTC, nav2_core::GlobalPlanner)
