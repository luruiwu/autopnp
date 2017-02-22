#include <ipa_room_exploration/fow_to_robot_mapper.h>

// Function that provides the functionality that a given fow path gets mapped to a robot path by using the given parameters.
// To do so simply a vector operation is applied. If the computed robot pose is not in the free space, another accessible
// point is generated by finding it on the radius around the fow middlepoint s.t. the distance to the last robot position
// is minimized.
// Important: the room map needs to be an unsigned char single channel image, robot_to_fow_vector in [m]
void mapPath(const cv::Mat& room_map, std::vector<geometry_msgs::Pose2D>& robot_path,
		const std::vector<geometry_msgs::Pose2D>& fow_path, const Eigen::Matrix<float, 2, 1>& robot_to_fow_vector,
		const double map_resolution, const cv::Point2d map_origin, const cv::Point& starting_point)
{
	AStarPlanner path_planner;

	// initialize the robot position to enable the Astar planner to find a path from the beginning
	cv::Point robot_pos = starting_point;

	// map the given rob to fow vector into pixel coordinates
	Eigen::Matrix<float, 2, 1> robot_to_fow_vector_pixel;
	robot_to_fow_vector_pixel << (robot_to_fow_vector(0,0)-map_origin.x)/map_resolution, (robot_to_fow_vector(1,0)-map_origin.y)/map_resolution;

	// go trough the given poses and calculate accessible robot poses
	for(std::vector<geometry_msgs::Pose2D>::const_iterator pose=fow_path.begin(); pose!=fow_path.end(); ++pose)
	{
		geometry_msgs::Pose2D current_pose;
		bool found_pose = false;

		// get the rotation matrix
		float sin_theta = std::sin(pose->theta);
		float cos_theta = std::cos(pose->theta);
		Eigen::Matrix<float, 2, 2> R;
		R << cos_theta, -sin_theta, sin_theta, cos_theta;

		// calculate the resulting rotated relative vector and the corresponding robot position
		Eigen::Matrix<float, 2, 1> v_rel_rot = R * robot_to_fow_vector_pixel;
		Eigen::Matrix<float, 2, 1> robot_position;
		robot_position << pose->x-v_rel_rot(0,0), pose->y-v_rel_rot(1,0);

		// check the accessibility of the found point
		if(room_map.at<uchar>((int)robot_position(1,0), (int)robot_position(0,0)) == 255 && robot_position(0,0) >= 0
				&& robot_position(1,0) >= 0 && robot_position(0,0) < room_map.cols && robot_position(1,0) < room_map.rows) // position accessible
		{
			current_pose.x = (robot_position(0,0) * map_resolution) + map_origin.x;
			current_pose.y = (robot_position(1,0) * map_resolution) + map_origin.y;
			current_pose.theta = pose->theta;
			found_pose = true;
			robot_path.push_back(current_pose);

			// set robot position to computed pose s.t. further planning is possible
			robot_pos = cv::Point(robot_position(0,0), robot_position(1,0));
		}
		else // position not accessible, find another valid pose
		{
			// get current fow position
			cv::Point fow_position(pose->x, pose->y);

			// use the map accessibility analysis server to find a reachable pose
			std::string perimeter_service_name = "/map_accessibility_analysis/map_perimeter_accessibility_check";
			cob_map_accessibility_analysis::CheckPerimeterAccessibility::Response response;
			cob_map_accessibility_analysis::CheckPerimeterAccessibility::Request check_request;
			geometry_msgs::Pose2D goal;
			goal.x = (fow_position.x*map_resolution) + map_origin.x;
			goal.y = (fow_position.y*map_resolution) + map_origin.y;
			check_request.center = goal;
			check_request.radius = robot_to_fow_vector.norm();
			check_request.rotational_sampling_step = PI/8;

			// send request
			bool res = ros::service::call(perimeter_service_name, check_request, response);
			if(res==true && response.accessible_poses_on_perimeter.size()!=0)
			{
				// go trough the found accessible positions and take the one that minimizes the distance to the last pose
				double min_squared_distance = 1e5;
				geometry_msgs::Pose2D best_pose;
				for(std::vector<geometry_msgs::Pose2D>::iterator pose = response.accessible_poses_on_perimeter.begin(); pose != response.accessible_poses_on_perimeter.end(); ++pose)
				{
					cv::Point diff = robot_pos - cv::Point((pose->x-map_origin.x)/map_resolution, (pose->y-map_origin.y)/map_resolution);
					double current_distance = diff.x*diff.x+diff.y*diff.y;
					if(current_distance<=min_squared_distance)
					{
						min_squared_distance = current_distance;
						best_pose = *pose;
						found_pose = true;
					}
				}

				// if no pose could be found, ignore it
				if(found_pose==false)
					continue;

				// add pose to path and set robot position to it
				robot_path.push_back(best_pose);
				robot_pos = cv::Point((best_pose.x-map_origin.x)/map_resolution, (best_pose.y-map_origin.y)/map_resolution);
			}
			else // try with the astar pathfinder to find a valid pose, if the accessibility server failed for some reason
			{
				// get vector from current position to desired fow position
				std::vector<cv::Point> astar_path;
				path_planner.planPath(room_map, robot_pos, fow_position, 1.0, 0.0, map_resolution, 0, &astar_path);

				// find the point on the astar path that is on the viewing circle around the fow middlepoint
				cv::Point accessible_position;
				for(std::vector<cv::Point>::iterator point=astar_path.begin(); point!=astar_path.end(); ++point)
				{
					if(cv::norm(*point-fow_position) <= robot_to_fow_vector_pixel.norm())
					{
						accessible_position = *point;
						found_pose = true;
						break;
					}
				}

				// if no valid pose could be found, ignore it
				if(found_pose==false)
					continue;

				// get the angle s.t. the pose points to the fow middlepoint and save it
				current_pose.x = (accessible_position.x * map_resolution) + map_origin.x;
				current_pose.y = (accessible_position.y * map_resolution) + map_origin.y;
				current_pose.theta = std::atan2(pose->y-accessible_position.y, pose->x-accessible_position.x);
				robot_path.push_back(current_pose);

				// set robot position to computed pose s.t. further planning is possible
				robot_pos = accessible_position;
			}
		}

//		testing
//		std::cout << robot_pos << ", " << cv::Point(pose->x, pose->y) << std::endl;
//		cv::Mat room_copy = room_map.clone();
//		cv::line(room_copy, robot_pos, cv::Point(pose->x, pose->y), cv::Scalar(127), 1);
//		cv::circle(room_copy, robot_pos, 2, cv::Scalar(100), CV_FILLED);
//		cv::imshow("pose", room_copy);
//		cv::waitKey();
	}
}
