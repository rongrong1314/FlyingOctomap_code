#include <ltStar_lib_ortho.h>
#include <tf/transform_datatypes.h>
#include <std_srvs/Empty.h>
#include <orthogonal_planes.h>

#define SAVE_CSV 1 			// save measurements of lazyThetaStar into csv file
// #define RUNNING_ROS 0 	// enable to publish markers on rViz


namespace std
{
    template <>
    struct hash<octomath::Vector3>
    {
        size_t operator()( const octomath::Vector3& coordinates ) const
        {
            double fractpart, intpart;
            fractpart = modf (coordinates.x()*10000 , &intpart);
            return intpart;
        }
    };
}

namespace LazyThetaStarOctree{

	int obstacle_avoidance_time;
	int obstacle_avoidance_calls;
	int setVertex_time;
	int updateVertex_time;
	std::ofstream log_file;
	int id_unreachable;

	void generateOffsets(double resolution, double safety_margin, double (*startDepthGenerator)(double, double, double), double (*goalDepthGenerator)(double, double, double) )
	{
		startOffsets = generateOffsetMatrix(safety_margin/2.0, resolution, startDepthGenerator);
		goalOffsets = generateOffsetMatrix(safety_margin/2.0, resolution, goalDepthGenerator);
	}


	// TODO The old version was using vertex, cell centers or something else? --> nobody knows....
	/// TODO figure some weights!
	float weightedDistance(octomath::Vector3 const& start, octomath::Vector3 const& end)
	{
	    return sqrt(	pow(end.x()-start.x(),2) +
						pow(end.y()-start.y(),2) +
        				pow(end.z()-start.z(),2));
	}

	/*
		(for getLineStatus and getLineStatusBoundingBox)
		https://github.com/ethz-asl/volumetric_mapping
		Copyright (c) 2015, Helen Oleynikova, ETH Zurich, Switzerland
		You can contact the author at <helen dot oleynikova at mavt dot ethz dot ch>
		All rights reserved.
		Redistribution and use in source and binary forms, with or without
		modification, are permitted provided that the following conditions are met:
		* Redistributions of source code must retain the above copyright
		notice, this list of conditions and the following disclaimer.
		* Redistributions in binary form must reproduce the above copyright
		notice, this list of conditions and the following disclaimer in the
		documentation and/or other materials provided with the distribution.
		* Neither the name of ETHZ-ASL nor the
		names of its contributors may be used to endorse or promote products
		derived from this software without specific prior written permission.
		THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
		ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
		WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
		DISCLAIMED. IN NO EVENT SHALL ETHZ-ASL BE LIABLE FOR ANY
		DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
		(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
		LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
		ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
		(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
		SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
	*/
	CellStatus getLineStatus(InputData const& input) {
		// Get all node keys for this line.
		// This is actually a typedef for a vector of OcTreeKeys.
		// Can't use the key_ray_ temp member here because this is a const function.
		octomap::KeyRay key_ray;
		input.octree.computeRayKeys(input.start, input.goal,
			key_ray);
		// ROS_INFO_STREAM("[LTStar] From " << start << " to " << end);
		// Now check if there are any unknown or occupied nodes in the ray.
		for (octomap::OcTreeKey key : key_ray) 
		{
			// ROS_INFO_STREAM(key[0] << " " << key[1] << " " << key[2] );
			octomap::OcTreeNode* node = input.octree.search(key);
			if (node == NULL) 
			{
				return CellStatus::kUnknown;
				
			} 
			else if (input.octree.isNodeOccupied(node)) 
			{
				return CellStatus::kOccupied;
			}
		}
		return CellStatus::kFree;
	}

	CellStatus getLineStatusBoundingBox(InputData const& input) 
	{
		octomath::Vector3 bounding_box_size ( input.margin , input.margin , input.margin );
		// TODO(helenol): Probably best way would be to get all the coordinates along
		// the line, then make a set of all the OcTreeKeys in all the bounding boxes
		// around the nodes... and then just go through and query once.
		const double epsilon = 0.001;  // Small offset
		CellStatus ret_forward = CellStatus::kFree;
		CellStatus ret_backwards = CellStatus::kFree;
		const double& resolution = input.octree.getResolution();

		// Check corner connections and depending on resolution also interior:
		// Discretization step is smaller than the octomap resolution, as this way
		// no cell can possibly be missed
		double x_disc = bounding_box_size.x() / ceil((bounding_box_size.x() + epsilon) / resolution);
		double y_disc = bounding_box_size.y() / ceil((bounding_box_size.y() + epsilon) / resolution);
		double z_disc = bounding_box_size.z() / ceil((bounding_box_size.z() + epsilon) / resolution);

		// Ensure that resolution is not infinit
		if (x_disc <= 0.0) x_disc = resolution;
		if (y_disc <= 0.0) y_disc = resolution;
		if (z_disc <= 0.0) z_disc = resolution;

		const octomath::Vector3 bounding_box_half_size = bounding_box_size * 0.5;

		for (double x = -bounding_box_half_size.x(); x <= bounding_box_half_size.x();
			x += x_disc) {
			for (double y = -bounding_box_half_size.y();
				y <= bounding_box_half_size.y(); y += y_disc) 
			{
				for (double z = -bounding_box_half_size.z();
					z <= bounding_box_half_size.z(); z += z_disc) 
				{
					octomath::Vector3 offset(x, y, z);
					ret_forward = getLineStatus( InputData(input.octree, input.start + offset, input.goal + offset, input.margin) );
					ret_backwards = getLineStatus( InputData(input.octree, input.goal + offset, input.start + offset, input.margin) );
					if (ret_forward != CellStatus::kFree || ret_backwards != CellStatus::kFree ) 
					{
						return CellStatus::kOccupied;
					}
				}
			}
		}
		return CellStatus::kFree;
	}

	bool hasLineOfSight(InputData const& input, bool ignoreUnknown/* = false*/)
	{
		// There seems to be a blind spot when the very first node is occupied, so this covers that case
		octomath::Vector3 mutable_end = input.goal;
		auto res_node = input.octree.search(mutable_end);
		if(res_node == NULL)
		{
			if(ignoreUnknown)
			{
				return false;
			}
		}
		else
		{
			if (input.octree.isNodeOccupied(res_node) )
			{
				// if(!ignoreUnknown)
				// {
				// 	ROS_WARN_STREAM("[LTStar] No line of sight because input.octree.isNodeOccupied(res_node)");
				// }
				return false;
			}
		}
		// ROS_WARN_STREAM("Start " << input.start << " goal "  << input.goal);

		octomath::Vector3 dummy;
		octomath::Vector3 direction = input.goal - input.start;
		bool has_hit_obstacle = input.octree.castRay( input.start, direction, dummy, ignoreUnknown, direction.norm());
		if(has_hit_obstacle)
		{
			// if(!ignoreUnknown)
			// {
			// 	ROS_WARN_STREAM("[LTStar] No line of sight because has_hit_obstacle");
			// }
			return false;
		}
		return true;
	}

	bool is_target(octomath::Vector3 & target, octomath::Vector3 & point_B)
	{
		bool same = false;

		same = target.x() == point_B.x();
		same = same || (target.y() == point_B.y()); 
		same = same || (target.z() == point_B.z()); 
		return same;
	}

	CellStatus getCorridorOccupancy_byPlanes(
		InputData const& input,
		PublishingInput const& publish_input,
		bool ignoreUnknown = false) 
	{
		visualization_msgs::MarkerArray marker_array;
		CoordinateFrame coordinate_frame = generateCoordinateFrame(input.start, input.goal);
		Eigen::MatrixXd transformation_matrix_start = generateRotationTranslationMatrix(coordinate_frame, input.start);

		// octomath::Vector3 goalWithMargin = calculateGoalWithMargin(input.start, input.goal, input.margin);
		Eigen::MatrixXd transformation_matrix_goal = generateRotationTranslationMatrix(coordinate_frame, input.goal);

		Eigen::MatrixXd points_around_start = transformation_matrix_start * startOffsets;
		Eigen::MatrixXd points_around_goal = transformation_matrix_goal * goalOffsets;

		visualization_msgs::Marker marker;
		octomath::Vector3 temp_start, temp_goal;
		for (int i = 0; i < points_around_start.cols(); ++i)
		{
			temp_start = octomath::Vector3(points_around_start(0, i), points_around_start(1, i), points_around_start(2, i));
			temp_goal = octomath::Vector3(points_around_goal(0, i), points_around_goal(1, i), points_around_goal(2, i));

			if(hasLineOfSight( InputData( input.octree, temp_start, temp_goal, input.margin), ignoreUnknown) == false) 
			{ 
				// ROS_ERROR_STREAM (  " Start " << input.start << " to " << input.goal << "   Found obstacle from " << temp_start << " to " << temp_goal );
				obstacle_hit_count++;
    			// rviz_interface::publish_arrow_path_occupancyState(temp_start, temp_goal, publish_input.marker_pub, false);
				return CellStatus::kOccupied; 
			}   
			else if(hasLineOfSight( InputData(input.octree, temp_goal, temp_start, input.margin), ignoreUnknown) == false) 
			{ 
				// ROS_ERROR_STREAM (  " Start " << input.start << " to " << temp_goal << "   Found obstacle from " << temp_start << " to " << temp_goal );
    			// rviz_interface::publish_arrow_path_occupancyState(temp_start, temp_goal, publish_input.marker_pub, false);
				obstacle_hit_count++;
				return CellStatus::kOccupied; 
			}   
			else
			{
    			// rviz_interface::publish_arrow_path_occupancyState(temp_start, temp_goal, publish_input.marker_pub, true);
				// ROS_INFO_STREAM (  " Start " << input.start << " to " << input.goal << "   Free from " << temp_start << " to " << temp_goal );
			}
		}
		return CellStatus::kFree; 
	}


	// Take heed all those who enter here!
	// If the shape around the start is different from the shape around the end, 
	// the order to evaluate line of sight is parent -> s -> neighbor
	// ortherwise path 2 will not fallback on path 1 when needed 
	// (the line of sight from start to end is not the same as from end to start)

	bool is_flight_corridor_free(InputData const& input, PublishingInput const& publish_input, bool ignoreUnknown)
	{
		// auto start_count = std::chrono::high_resolution_clock::now();
		// bool free = getLineStatusBoundingBox(octree_, start, end, bounding_box_size) == CellStatus::kFree;
		bool free = getCorridorOccupancy_byPlanes(input, publish_input) == CellStatus::kFree;
		// auto finish_count = std::chrono::high_resolution_clock::now();
		// auto time_span = finish_count - start_count;
		// obstacle_avoidance_time += std::chrono::duration_cast<std::chrono::microseconds>(time_span).count();
		obstacle_avoidance_calls ++;
		return free;
	}

	
	/**
	 * @brief      Set vertex portion of pseudo code, ln 34.
	 *
	 * @param      octree     The octree
	 * @param      s          TThe node in analyzis
	 * @param      closed     The closed
	 * @param      open       The open
	 * @param[in]  neighbors  The neighbors
	 */
	bool setVertex(
		octomap::OcTree 										const& 	octree, 
		std::shared_ptr<ThetaStarNode> 							& 		s, 
		std::unordered_map<octomath::Vector3, std::shared_ptr<ThetaStarNode>, Vector3Hash, VectorComparatorEqual> &  closed,
		Open 													& 		open, 
		unordered_set_pointers 									const& 	neighbors,
		double 															safety_margin,
		PublishingInput 										const& 	publish_input,
		const double 													sidelength_lookup_table[],
		bool ignoreUnknown)	// TODO optimization where the neighbors are pruned here, will do this when it is a proper class
	{
		auto start_count = std::chrono::high_resolution_clock::now();

		// // TODO == VERY IMPORTANT == this neighbors are actually the ones calculated before in the main loop so we can just pass that along instead of generating
		// // There is a choice to be made here, 
		// //  -> at this point we only care about line of sight and not about if obstacle/unknown distinction
		// //  -> passing over the neighbors is one addicional parameter that would increse the difficulty of understanding the code and one more facto to debug
		// //  -> will leave this for if implementation is not fast enough
		// ln 35 if NOT lineofsight(parent(s), s) then
		// Path 1 by considering the path from s_start to each expanded visible neighbor s′′ of s′
		if(    !is_flight_corridor_free( InputData( octree, *(s->parentNode->coordinates), *(s->coordinates), safety_margin ), publish_input, ignoreUnknown  )   )
		{
			// g(s)		= length of the shortest path from the start vertex to s found so far.
			// c(s,s') 	= straight line distance between vertices s and s'	
			// ln 36 /* Path 1*/
			// ln 37 parent(s) := argmin_(s' € (nghb_vis  intersection  closed)_) evaluating ( g(s') + c(s', s) );
			// ln 38 g(s) := min_s'€ngbr_vis(s) intersection closed ( g(s') + c(s', s) );
			double min_g = std::numeric_limits<double>::max();
			std::shared_ptr<ThetaStarNode> candidate_parent;
			bool new_parent_node = false;
			if(neighbors.empty())
			{
				// ROS_WARN_STREAM("No neighbor was found.");
				return true;
			}
			double cell_size;
			// each expanded & visible & neighbor of s'
			for(std::shared_ptr<octomath::Vector3> n_coordinates : neighbors)																	// NEIGHBOR
			{
				try
				{
					// Here the rule parent -> s -> neighbor for line of sight is not followed
					// Because when we are looking for path 1, we are replicating the test that was made to do open.insert
					//     at this point the neighbor was the current s
					if( ! is_flight_corridor_free( InputData( octree, *n_coordinates, *(s->coordinates), safety_margin ), publish_input, ignoreUnknown  ) )
					{
						// auto res_node = octree.search(*n_coordinates);
						// if(res_node == NULL)
						// {
	     				// 	throw std::out_of_range("Skipping cases where unknown neighbors are found.");
						// }
						// log_file << "[SetVertex] no line of sight " << *(s->coordinates) << " to " << *n_coordinates << std::endl;
						continue;
					}
					// else
					// {
					// 	log_file << "[SetVertex] visible neighbor " << *n_coordinates << std::endl;
					// }


					// VISIBLE
					// closed.at(*n_coordinates) throws exception when there is no such element
					std::shared_ptr<ThetaStarNode> neighbor_in_closed = closed.at(*n_coordinates);  											// EXPANDED
					double candidate_g = neighbor_in_closed->distanceFromInitialPoint;
					double c_distance_beetween_s_and_candidate = weightedDistance(  *(neighbor_in_closed->coordinates), *(s->coordinates)  );
					// ln 38 (...) g(s') + c(s', s)
					if(   (candidate_g + c_distance_beetween_s_and_candidate) < min_g    )
					{
						// ROS_WARN_STREAM(std::setprecision(10) << "[SetVer] " << candidate_g << " + " << c_distance_beetween_s_and_candidate << " - " << min_g 
						// 	<< " ==> " <<  (candidate_g + c_distance_beetween_s_and_candidate) - min_g << " > " << scale);
						// ROS_WARN_STREAM("[SetVer] Previous best value " << min_g << " with parent " << s->parentNode << ": " << *(s->parentNode));
						min_g = candidate_g + c_distance_beetween_s_and_candidate;
						candidate_parent = neighbor_in_closed;
						new_parent_node = true;
						// ROS_WARN_STREAM("[SetVer] " << neighbor_in_closed << "  ==>  Min_G:" << min_g << " = " << candidate_g << " + " << c_distance_beetween_s_and_candidate);
						// ROS_WARN_STREAM("[SetVer] " << neighbor_in_closed << ": " << *neighbor_in_closed << "  ==>  ");
						// ROS_WARN_STREAM("[SetVer]  Min_G:" << candidate_g << " from start to " << *(neighbor_in_closed->coordinates) );
						// ROS_WARN_STREAM("[SetVer]  c    :" << c_distance_beetween_s_and_candidate << " from " << *(neighbor_in_closed->coordinates) << " to " << *(s->coordinates)   );
					}
				}
				catch(const std::out_of_range& oor)
				{
					// ROS_WARN_STREAM("[N] " << *n_coordinates << " is unknown space.");
				} // closed.at(*n_coordinates) throws exception when there is no such element
			}
			if(new_parent_node)
			{
				// ROS_WARN_STREAM("[SetVer] For " << s << ": " << *s << " parent will now be " << candidate_parent << ": " << *candidate_parent);
				// There is a parent for path 1
				s->parentNode = candidate_parent;
				open.changeDistanceFromInitialPoint(min_g, s);
			}
			else
			{
				std::ostringstream oss;
				oss << "s node " << *(s->coordinates) << " has no line of sight with the current parent " << *(s->parentNode->coordinates) << "(path 2)  and node of the neighbors are visible either (path 1). ";
				log_file << oss.str() << std::endl;
				oss << "No path 1 was found, adding to closed a bad node  ";
				throw std::out_of_range( oss.str() );
				// log_file << "No path 1 was found, adding to closed a bad node for " << *(s->coordinates) << std::endl;
				return false;
			}
		}
		// ln 39 end

		auto finish_count = std::chrono::high_resolution_clock::now();
		auto time_span = finish_count - start_count;
		setVertex_time += std::chrono::duration_cast<std::chrono::microseconds>(time_span).count();
		return true;
	}

	/**
	 * @brief      Extracts a sequence of coordinates from the links between nodes starting at the goal node and expanding the connections to the prevuous point through parentNode.
	 *
	 * @param      path   The list object where to store the path
	 * @param      start  The starting node
	 * @param      end    The goal node
	 *
	 * @return     true is a path from goal node until start node was found (under 500 jumps). False otherwise.
	 */
	bool extractPath(std::list<octomath::Vector3> & path, ThetaStarNode const& start, ThetaStarNode & end, bool writeToFile)
	{
		bool success = true;
		std::ofstream pathWaypoints;
        pathWaypoints.open (folder_name + "/final_path.txt", std::ofstream::out | std::ofstream::app);
		std::shared_ptr<ThetaStarNode> current = std::make_shared<ThetaStarNode>(end);
		std::shared_ptr<ThetaStarNode> parentAdd;
		int safety_count = 0;
		int max_steps_count = 50;
		while(   !( *(current->coordinates) == *(start.coordinates) ) 
			&& safety_count < max_steps_count   )
		{
			if( current->parentNode == NULL )
			{
				log_file << "[ERROR]  The node with no parent is " << *current << std::endl;
				ROS_ERROR_STREAM("The node with no parent is " << *current);
				return false;
			}

			// ROS_INFO_STREAM("[LTStar] Adding " << *(current->coordinates));
			path.push_front( *(current->coordinates) );
			if(writeToFile)
			{
				// writeToFileWaypoint(*(current->coordinates), current->cell_size, folder_name + "/final_path.txt");
        		pathWaypoints << std::setprecision(5) << current->coordinates->x() << ", " << current->coordinates->y() << ", " << current->coordinates->z() << ", " << current->cell_size << std::endl;
			}
			parentAdd = current->parentNode;
			current = parentAdd;
			safety_count++;
			if(safety_count >= max_steps_count)
			{
				log_file << "[ERROR]  Possible recursion in generated path detected while extracted path. Full path trunkated at node "<< max_steps_count << std::endl;
				ROS_ERROR_STREAM("Possible recursion in generated path detected while extracted path. Full path trunkated at node "<< max_steps_count);
				success = false;
				break;
			}
		}
		path.push_front( *(current->coordinates) );
		if(writeToFile)
		{
        	pathWaypoints << std::setprecision(5) << current->coordinates->x() << ", " << current->coordinates->y() << ", " << current->coordinates->z() << ", " << current->cell_size << std::endl;
			// writeToFileWaypoint(*(current->coordinates), current->cell_size, folder_name + "/final_path.txt");
		}
        pathWaypoints << std::endl;
		pathWaypoints.close();
		return success;
	}

	/**
	 * @brief      Calcute the cost if a link between these to nodes is created. This is not the ComputeCost method of the pseudo code.
	 *
	 * @param      s            node
	 * @param      s_neighbour  The neighbour
	 *
	 * @return     the new cost
	 */
	float CalculateCost(ThetaStarNode const& s, ThetaStarNode const& s_neighbour)
	{ 
		if(s.parentNode == NULL)
		{
			log_file << "[ERROR] Parent node of s (" << s << ") is null. @ CalculateCost" << std::endl;
			ROS_ERROR_STREAM("Parent node of s (" << s << ") is null. @ CalculateCost");
		}
		float g_parent_s =  s.parentNode->distanceFromInitialPoint;
		float c_s_parent_AND_s_neighbour = weightedDistance(  *(s.parentNode->coordinates), *(s_neighbour.coordinates)  );
		// ROS_WARN_STREAM("Cost: " << g_parent_s << " + " << c_parent_s_AND_s_neighbour);
		// ln 32 g(s') := g(parent(s)) + c(parent(s), s');
		return g_parent_s + c_s_parent_AND_s_neighbour;
	}


	// s' == s_neighbour
	// ln 20 UpdateVertex(s, s')
	void UpdateVertex(ThetaStarNode const& s, std::shared_ptr<ThetaStarNode> s_neighbour,
		Open & open)
	{ 
		auto start_count = std::chrono::high_resolution_clock::now();

		// ROS_WARN_STREAM("UpdateVertex");
		// ln 21 g_old := g(s')
		float g_old = s_neighbour->distanceFromInitialPoint;
		// ln 22 ComputeCost(s, s'); (unrolled de function into UpdatedVertex)
		// ln 29 /* Path 2 */
		// ln 30 if  g(parent(s)) + c(parent(s), s') < g(s')  then
		float cost = CalculateCost(s, *s_neighbour);
		// ROS_WARN_STREAM("if(cost < g_old) <=> " << cost << " < " << g_old);
		if(cost < g_old)
		{
			// ln 24 if s' € open then
			if(   open.existsInMap( *(s_neighbour->coordinates) )   )
			{
				// ln 25 open.Remove(s')
				open.erase(*s_neighbour);
			}
			// Unrolling ComputeCost()
			// ln 31 parent(s') := parent(s);
			s_neighbour->parentNode = s.parentNode;
			// ln 32 g(s') := g(parent(s)) + c(parent(s), s');
			s_neighbour->distanceFromInitialPoint = cost;
			// ln 33 end (of ComputeCost function)
			// ln 26 open.Insert(s', g(s') + h(s'));
			open.insert(s_neighbour);
		}
		auto finish_count = std::chrono::high_resolution_clock::now();
		auto time_span = finish_count - start_count;
		updateVertex_time += std::chrono::duration_cast<std::chrono::microseconds>(time_span).count();
	}

	bool isExplored(octomath::Vector3 const& grid_coordinates_toTest, octomap::OcTree const& octree)
    {
        octomap::OcTreeNode* result = octree.search(grid_coordinates_toTest.x(), grid_coordinates_toTest.y(), grid_coordinates_toTest.z());
        if(result == NULL){
            return false;
        }
        else
        {
            return true;
        }
    }



	// TODO 	When making objects out of this, the things that can be set on algorithm configuration are 
	// 			sidelength_lookup_table, startOffsets and goalOffsets
	// h(s)		= straight line distance between goal and s vertex
	// V 		= set of all grid vertices
	// s 		= current vertice
	// c(s,s') 	= straight line distance between vertices s and s'
	// g(s)		= length of the shortest path from the start vertex to s found so far.
	// nghbrvis(s) in V 	= set of neighbors of vertex s in V that have line-of-sight to s
	/**
	 * @brief      Lazy Theta Star. Decimal precision in 0.001 both in closed and in open (buildKey)
	 *
	 * @param      octree        The octree
	 * @param      disc_initial  The disc initial
	 * @param      disc_final    The disc final
	 *
	 * @return     A list of the ordered waypoints to get from initial to final
	 */
	std::list<octomath::Vector3> lazyThetaStar_(
		InputData const& input,
		ResultSet & resultSet,
		const double sidelength_lookup_table[],
		PublishingInput const& publish_input,
		int const& max_time_secs,
		bool print_resulting_path)
	{
		obstacle_hit_count = 0;
		int generate_neighbors_time = 0;
		obstacle_avoidance_time = 0;
		obstacle_avoidance_calls = 0;
		setVertex_time = 0;
		updateVertex_time = 0;

    	log_file.open(folder_name + "/current/lazyThetaStar.log", std::ios_base::app);
		auto start = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double> max_search_time = std::chrono::duration<double>(max_time_secs);
		std::list<octomath::Vector3> path;

		if (!isExplored(input.start, input.octree))
		{
			ROS_ERROR_STREAM("[LTStar] Start " << input.start << " is unknown.");
			log_file << "[ERROR] " << "[LTStar] Start " << input.start << " is unknown." << std::endl;
			return path;	
		} 
		if (!isExplored(input.goal, input.octree))
		{
			ROS_ERROR_STREAM("[LTStar] Goal " << input.goal << " is unknown.");
			log_file << "[ERROR] " << "[LTStar] Goal " << input.goal << " is unknown." << std::endl;
			return path;	
		} 
		
		double resolution = input.octree.getResolution();
		// GOAL
		// Init initial and final nodes to have the coordinates of respective cell centers
		double cell_size_goal = -1;
		octomath::Vector3 cell_center_coordinates_goal = input.goal;
		updateToCellCenterAndFindSize( cell_center_coordinates_goal, input.octree, cell_size_goal, sidelength_lookup_table);
		std::shared_ptr<ThetaStarNode> disc_final_cell_center = std::make_shared<ThetaStarNode>(std::make_shared<octomath::Vector3>(cell_center_coordinates_goal), cell_size_goal);
		disc_final_cell_center->lineDistanceToFinalPoint = weightedDistance(cell_center_coordinates_goal, input.goal);
		// START
		// Get distance from start to cell center
		octomath::Vector3 cell_center_coordinates_start = input.start;
		double cell_size_start = -1;
		updateToCellCenterAndFindSize(cell_center_coordinates_start, input.octree, cell_size_start, sidelength_lookup_table);

		if(publish_input.publish)
		{
			log_file << "[LTStar] Center of start voxel " << cell_center_coordinates_start << ". Side " << cell_size_start << " given start point " << input.start << std::endl;
			log_file << "[LTStar] Center of goal voxel " << cell_center_coordinates_goal << ". Side " << cell_size_goal << " given goal point " << input.goal << std::endl;
#ifdef RUNNING_ROS
			geometry_msgs::Point start_point, goal_point;
			start_point.x = input.start.x();
			start_point.y = input.start.y();
			start_point.z = input.start.z();
			goal_point.x = input.goal.x();
			goal_point.y = input.goal.y();
			goal_point.z = input.goal.z();
			rviz_interface::publish_start(start_point, publish_input.marker_pub);
			rviz_interface::publish_goal(goal_point, publish_input.marker_pub);

			start_point.x = cell_center_coordinates_start.x();
			start_point.y = cell_center_coordinates_start.y();
			start_point.z = cell_center_coordinates_start.z();
			goal_point.x = cell_center_coordinates_goal.x();
			goal_point.y = cell_center_coordinates_goal.y();
			goal_point.z = cell_center_coordinates_goal.z();
			rviz_interface::publish_start_voxel(start_point, publish_input.marker_pub, cell_size_start);
			rviz_interface::publish_goal_voxel(goal_point, publish_input.marker_pub, cell_size_goal);
#endif
		}

		if(equal(cell_center_coordinates_start, cell_center_coordinates_goal, resolution/2))
		{
			ROS_WARN_STREAM("[LTStar] Start and goal in the same voxel - flying straight.");
			path.push_front( input.goal );
			path.push_front( input.start );
			return path;
		}


		// ln 1 Main()
		// ln 2 open := closed := 0
		Open open (cell_center_coordinates_goal);
		std::unordered_map<octomath::Vector3, std::shared_ptr<ThetaStarNode>, Vector3Hash, VectorComparatorEqual> closed;

		// M the starting point is the initial node
		// [Footnote] open.Insert(s,x) inserts vertex s with key x into open
		// map open's order = (shortest path from the start vertex to s found so far) + (straight line distance between goal and vertex)
		std::shared_ptr<ThetaStarNode> disc_initial_cell_center = std::make_shared<ThetaStarNode>(std::make_shared<octomath::Vector3>(cell_center_coordinates_start), cell_size_start, 
    		// ln 3 g(s_start) := 0;
			0, 
			weightedDistance(cell_center_coordinates_start, input.goal));
	    // ln 4 parent(s_start) := s_start;
		disc_initial_cell_center->parentNode = disc_initial_cell_center;	// TODO make sure nobody is realing on point pointing to themselves in general
		// ln 5 open.Insert(s_start, g(s_start) + h(s_start))
		open.insert(disc_initial_cell_center);
		// if(publish_input.publish)
		// {
		// 	log_file << "[N]          inserting " << *(disc_initial_cell_center->coordinates) << " into open (start) " << std::endl;
		// }
		// ROS_WARN_STREAM("__START__ " << disc_initial_cell_center << ": " << *disc_initial_cell_center);
		std::shared_ptr<ThetaStarNode> s_neighbour, s, solution_end_node;
		bool solution_found = false;
		// TODO remove this, for debugging only
		int used_search_iterations = 0;
		// ROS_WARN_STREAM("Goal's voxel center " << *disc_final_cell_center);
		// ln 6 while open != empty do
		id_unreachable = 0;	
		while(!open.empty() && !solution_found)
		{
 			// open.printNodes("========= Starting state of open ");
			// ln 7 s := open.Pop();	
			// [Footnote] open.Pop() removes a vertex with the smallest key from open and returns it
			// if(publish_input.publish)
			// {
			// 	log_file << std::endl << std::endl;
			// 	open.printNodes(" ========== Before pop ========== ", log_file);
			// 	log_file << " == Closed == " << std::endl;
			// 	for( const auto& n : closed ) 
			// 	{
			// 		log_file << " [" << n.first << "] " << *(n.second->coordinates) << std::endl;
			// 	}
			// }
			s = open.pop();
#ifdef RUNNING_ROS
			if(publish_input.publish)
			{
				geometry_msgs::Point s_point;
				s_point.x = s->coordinates->x();
				s_point.y = s->coordinates->y();
				s_point.z = s->coordinates->z();
				rviz_interface::publish_s(s_point, publish_input.marker_pub);
			}
#endif
			resultSet.addOcurrance(s->cell_size);
			unordered_set_pointers neighbors;

			// auto start_count = std::chrono::high_resolution_clock::now();
			generateNeighbors_filter_pointers(neighbors, *(s->coordinates), s->cell_size, resolution, input.octree);

			// auto finish_count = std::chrono::high_resolution_clock::now();
			// auto time_span = finish_count - start_count;
			// generate_neighbors_time += std::chrono::duration_cast<std::chrono::microseconds>(time_span).count();
			// ln 8 SetVertex(s);
			// It is it's own parent, this happens on the first node when the initial position is the center of the voxel (by chance)
			if(s->hasSameCoordinates(s->parentNode, resolution ) == false)
			{
				bool ignoreUnknown = weightedDistance(*(s->coordinates), cell_center_coordinates_goal) < input.margin/2;
				if (!setVertex(input.octree, s, closed, open, neighbors, input.margin, publish_input, sidelength_lookup_table, ignoreUnknown))
				{
					input.octree.writeBinaryConst(folder_name + "/octree_noPath1s.bt");
					log_file << "[ERROR] no neighbor of " << *s << " had line of sight. Start " << input.start << " goal " << input.goal << std::endl;
					ROS_ERROR_STREAM ("[LTStar] no neighbor of " << *s << " had line of sight. Start " << input.start << " goal " << input.goal);
				}
			}
			// ln 9 if s = s_goal then 
			if( s->hasSameCoordinates(disc_final_cell_center, resolution/2) )
			{
				// ln 10 return "path found"
				solution_found = true;
				solution_end_node = s;
				if(publish_input.publish)
				{
					log_file  << "[Ortho] at iteration " << used_search_iterations << " open size is " << open.size() << std::endl;
					log_file  << "Solution end node:" << *s << " == " << *disc_final_cell_center << std::endl ;
				}
				continue;
			}
			// ln 11 closed := closed U {s}
			log_file << "@"<< used_search_iterations << "  inserting s into closed " << s << " <--> " << *s << std::endl;
			closed.insert( std::pair<octomath::Vector3, std::shared_ptr<ThetaStarNode>>( *(s->coordinates), s));
#ifdef RUNNING_ROS
			// if(publish_input.publish)
			// {
				rviz_interface::publish_closed(*(s->coordinates), publish_input.marker_pub);
			// }
#endif

			// TODO check code repetition to go over the neighbors of s
			double cell_size = 0;
			// ln 12 foreach s' € nghbr_vis(s) do
			for(std::shared_ptr<octomath::Vector3> n_coordinates : neighbors)
			{
				// Find minimum value for those with visibility and that it is in closed
				bool ignoreUnknown = weightedDistance(*n_coordinates, cell_center_coordinates_goal) < input.margin/2;


				if( ! is_flight_corridor_free( InputData(input.octree, *(s->coordinates), *n_coordinates, input.margin ), publish_input, ignoreUnknown) )
				{
					// log_file << "  [N] " << *n_coordinates << " has obstacle." << std::endl;
					continue;
				}
				// ln 13 if s' !€ closed then
				bool is_neighbor_in_closed = closed.find(*n_coordinates) != closed.end();
				if (!is_neighbor_in_closed)
				{
					// ln 14 if s' !€ open then
					if( !open.existsInMap(*n_coordinates) )
					{
						octomap::OcTreeKey key = input.octree.coordToKey(*n_coordinates);
			            int depth = getNodeDepth_Octomap(key, input.octree);
			            cell_size = findSideLenght(input.octree.getTreeDepth(), depth, sidelength_lookup_table);

						// ln 15 g(s') := infinity;
						double g_distanceFromInitialPoint = std::numeric_limits<double>::max();
						// ln 16 parent(s') := NULL;
						s_neighbour = std::make_shared<ThetaStarNode>(
							n_coordinates, 
							cell_size, 
							g_distanceFromInitialPoint, 
							weightedDistance( *(n_coordinates), input.goal) // lineDistanceToFinalPoint
							);
					}
					else
					{
						s_neighbour =  open.getFromMap(*n_coordinates);
					}
					// ln 17 UpdateVertex(s, s');
					UpdateVertex(*s, s_neighbour, open);
				}
			}
			used_search_iterations++;	
			std::chrono::duration<double> time_lapse = std::chrono::high_resolution_clock::now() - start;
			if(time_lapse > max_search_time)
			{
				log_file << "[ERROR] Reached maximum time for A*. Breaking out" << std::endl;
				ROS_ERROR_STREAM("Reached maximum time for A*. Breaking out");
				break;	
			}
		}
		resultSet.iterations_used = used_search_iterations;
		// ROS_WARN_STREAM("Used "<< used_search_iterations << " iterations to find path");
		// ln 18 return "no path found";
		if(!solution_found)
		{
			ROS_WARN_STREAM("No solution found. Giving empty path.");
			if(publish_input.publish)
			{
        	    log_file <<  "[ltStar] All nodes were analyzed but the final node center " << disc_final_cell_center << " was never reached with " << resolution/2 << " tolerance. Start " << input.start << ", end " << input.goal << std::endl;
			}
		}
		else
		{ 
			if( equal(input.goal, cell_center_coordinates_goal) == false)
			{
				path.push_front( input.goal );
			}
			extractPath(path, *disc_initial_cell_center, *solution_end_node, print_resulting_path);
			bool initial_pos_far_from_initial_voxel_center = equal(input.start, cell_center_coordinates_start, resolution/2) == false;
			std::list<octomath::Vector3>::iterator it= path.begin();
			it++;
			bool free_path_from_current_to_second_waypoint = is_flight_corridor_free( InputData(input.octree, input.start, cell_center_coordinates_start, input.margin), publish_input, false);
			// if(!free_path_from_current_to_second_waypoint)
			// {
			// 	ROS_ERROR_STREAM("[ltstar] end There are obstacles between initial point " << input.start << " and its center " << cell_center_coordinates_start);
			// 	log_file << "[ltstar] end There are obstacles between initial point " << input.start << " and its center " << cell_center_coordinates_start << std::endl;
			// }
			if(initial_pos_far_from_initial_voxel_center && !free_path_from_current_to_second_waypoint)
			{
				path.push_front( input.start );
			}
		}
		if(path.size() == 1)
		{
			ROS_ERROR_STREAM("[LTStar] The resulting path has only one waypoint. It should always have at least start and goal. Path: ");
			log_file << "[ERROR]  The resulting path has only one waypoint. It should always have at least start and goal. Path: " << std::endl;
			for (auto v : path)
			{
				log_file << "[ERROR]  " << v << std::endl;
        		ROS_ERROR_STREAM("[LTStar]" << v );
			}
			ROS_ERROR_STREAM("[LTStar] Center of start voxel " << cell_center_coordinates_start << ". Side " << cell_size_start);
			ROS_ERROR_STREAM("[LTStar] Center of goal voxel " << cell_center_coordinates_goal << ". Side " << cell_size_goal);
			log_file << "[ERROR]  Center of start voxel " << cell_center_coordinates_start << ". Side " << cell_size_start << std::endl;
			log_file << "[ERROR]  Center of goal voxel " << cell_center_coordinates_goal << ". Side " << cell_size_goal << std::endl;
		}
		if(publish_input.publish)
		{
			log_file.close();
		}
		// Free all nodes in both open and closed
		open.clear();
		closed.clear();
		disc_initial_cell_center->parentNode = NULL;
		disc_initial_cell_center = NULL;
		std::chrono::duration<double> time_lapse = std::chrono::high_resolution_clock::now() - start;
		int total_in_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(time_lapse).count();
		// ROS_WARN_STREAM("[ltstar] [ortho] Total time " << total_in_microseconds << " microseconds.");
		// ROS_WARN_STREAM("[ltstar] [ortho] generate_neighbors_time took " << generate_neighbors_time << " - " << generate_neighbors_time*100/total_in_microseconds << "%");
		// ROS_WARN_STREAM("[ltstar] [ortho] obstacle_avoidance_time took " << obstacle_avoidance_time << " - " << obstacle_avoidance_time*100.0/total_in_microseconds << "%  "  );
		// ROS_WARN_STREAM("[ltstar] [ortho] setVertex_time took " << setVertex_time << " - " << setVertex_time*100/total_in_microseconds << "%");
		// ROS_WARN_STREAM("[ltstar] [ortho] updateVertex_time took " << updateVertex_time << " - " << updateVertex_time*100/total_in_microseconds << "%");
		// ROS_WARN_STREAM("[ltstar] [ortho] obstacle_avoidance_calls count " << obstacle_avoidance_calls  );
		return path;
	}
	// ln 19 end

	void qualityCheck(octomap::OcTree const& octree, octomath::Vector3 const& disc_initial, octomath::Vector3 const& disc_final, double straigh_line_distance, double distance_total, bool has_flight_corridor_free,  std::list<octomath::Vector3> const&resulting_path, std::stringstream const& generated_path_distance_ss)
	{
		std::ofstream log_file;
    	log_file.open(folder_name + "/current/lazyThetaStar.log", std::ios_base::app);
		std::stringstream to_log_file_ss;
		if(straigh_line_distance > distance_total)
		{
	    	to_log_file_ss << "!!! Straight line distance is larger than generated path distance !!! " << std::endl;
	    	to_log_file_ss << "disc_initial " << disc_initial << std::endl;
	    	to_log_file_ss << "resulting_path.begin()" << *(resulting_path.begin()) << std::endl;
	    	to_log_file_ss << "Straight line distance: " <<  std::setprecision(2) << disc_initial << " to " << disc_final << " = " << weightedDistance(disc_initial, disc_final) << std::endl;
	    	to_log_file_ss << generated_path_distance_ss.str() << std::endl;
	    	log_file << to_log_file_ss.str();
		}
		// if(   has_flight_corridor_free && abs(distance_total - straigh_line_distance) > std::max( straigh_line_distance/20, octree.getResolution()*2 )   )
		// {
		// 	to_log_file_ss << "!!! Generated path is much larger than the straigh_line_distance AND there are no obstacles !!!" << std::endl;
		// 	to_log_file_ss << "Distance tolerance straigh_line_distance/10: " << straigh_line_distance/20 << std::endl;
		// 	to_log_file_ss << "straigh_line_distance: " << straigh_line_distance << std::endl;
		// 	to_log_file_ss << "distance_total: " << distance_total << std::endl;
		// 	to_log_file_ss <<  std::setprecision(2) << "start: " << "(" << disc_initial.x() << disc_initial.y() << disc_initial.z() << ")" << std::endl;
		// 	to_log_file_ss <<  std::setprecision(2) << "goal:  " << "(" << disc_final.x() << disc_final.y() << disc_final.z() << ")" << std::endl;
		// 	std::stringstream octomap_name_stream;
		// 	octomap_name_stream << std::setprecision(2) << folder_name << "/octree_pathToLong_noObstacles_(" << disc_initial.x() << "_" << disc_initial.y() << "_"  << disc_initial.z() << ")_("<< disc_final.x() << "_"  << disc_final.y() << "_"  << disc_final.z() << ").bt";
		// 	octree.writeBinaryConst(octomap_name_stream.str());
		// 	log_file << to_log_file_ss.str();
		// }
	    log_file.close();
	}

	bool processLTStarRequest(octomap::OcTree & octree, lazy_theta_star_msgs::LTStarRequest const& request, lazy_theta_star_msgs::LTStarReply & reply, const double sidelength_lookup_table[], PublishingInput const& publish_input)
	{

		std::srand(std::time(0));
		ResultSet statistical_data;
		std::list<octomath::Vector3> resulting_path;
		octomath::Vector3 disc_initial(request.start.x, request.start.y, request.start.z);
		octomath::Vector3 disc_final(request.goal.x, request.goal.y, request.goal.z);
		// ROS_INFO_STREAM("[LTStar] Starting to process path from " << disc_initial << " to " << disc_final);
#ifdef SAVE_CSV
		std::chrono::system_clock::time_point start = std::chrono::system_clock::now();
#endif
	    std::stringstream octomap_name_stream;
		// octomap_name_stream << std::setprecision(2) << folder_name << "/current/from_" << disc_initial.x() << "_" << disc_initial.y() << "_"  << disc_initial.z() << "_to_"<< disc_final.x() << "_"  << disc_final.y() << "_"  << disc_final.z() << ".bt";
		// 	octree.writeBinary(octomap_name_stream.str());
		InputData input (octree, disc_initial, disc_final, request.safety_margin);
		resulting_path = lazyThetaStar_( input, statistical_data, sidelength_lookup_table, publish_input, request.max_time_secs, true);
#ifdef SAVE_CSV
		std::stringstream generated_path_distance_ss;
    	generated_path_distance_ss << "Generated path distance:\n";
		std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
		double distance_total = 0;
		std::list<octomath::Vector3>::iterator i = resulting_path.begin();
		double distance;
		if( !equal(*i, disc_initial) )
		{
			distance = weightedDistance(disc_initial, *i);
			distance_total += distance;
			generated_path_distance_ss <<  std::setprecision(2) << disc_initial << " to " << *i << " = " << distance <<  "(from start to start voxel center)" << std::endl;
		}
		octomath::Vector3 prev_waypoint = *i;
		++i;
		for (; i != resulting_path.end(); ++i)
		{
			distance = weightedDistance(prev_waypoint, *i);
			distance_total += distance;
			generated_path_distance_ss <<  std::setprecision(2) << prev_waypoint << " to " << *i << " = " << distance << std::endl;
			prev_waypoint = *i;
		}
		generated_path_distance_ss << "             total = " << distance_total << "\n";
		double straigh_line_distance = weightedDistance(disc_initial, disc_final);

		bool has_flight_corridor_free = is_flight_corridor_free( InputData(octree, disc_initial, disc_final, request.safety_margin), PublishingInput( publish_input.marker_pub, true), false);
		qualityCheck(octree, disc_initial, disc_final, straigh_line_distance, distance_total, has_flight_corridor_free, resulting_path, generated_path_distance_ss);


		std::ofstream csv_file;
		csv_file.open (folder_name + "/current/lazyThetaStar_computation_time.csv", std::ofstream::app);
		std::chrono::duration<double> time_span = std::chrono::duration_cast<std::chrono::duration<double>>(end - start);
		std::chrono::milliseconds millis = std::chrono::duration_cast<std::chrono::milliseconds>(time_span);
		csv_file << millis.count();
		csv_file << "," << straigh_line_distance;
		csv_file << "," << distance_total;
		csv_file << "," << has_flight_corridor_free;
		csv_file << ",(" <<  std::setprecision(2) << disc_initial.x() << "_"  << disc_initial.y() << "_"  << disc_initial.z() << ")";
		csv_file << ",(" <<  std::setprecision(2) << disc_final.x() << "_"  << disc_final.y() << "_"  << disc_final.z() << ")";
		csv_file << "," << request.safety_margin;
		csv_file << "," << request.max_time_secs ;
		csv_file << "," << statistical_data.iterations_used ;
		csv_file << "," << obstacle_hit_count ;
		csv_file << "," << obstacle_avoidance_calls ;
		csv_file << "," << publish_input.dataset_name << std::endl;
		csv_file.close();
#endif
		// ROS_WARN_STREAM("[LTStar] Path from " << disc_initial << " to " << disc_final << ". Outcome with " << resulting_path.size() << " waypoints.");
#ifdef RUNNING_ROS
		if(publish_input.publish)
		{
			rviz_interface::publish_arrow_straight_line(request.start, request.goal, publish_input.marker_pub, resulting_path.size() > 0);
		}
#endif
		if(resulting_path.size()==0)
		{
			reply.success = false;
			// std::stringstream octomap_name_stream;
			octomap_name_stream << std::setprecision(2) << folder_name << "/octree_noPath_(" << disc_initial.x() << "_" << disc_initial.y() << "_"  << disc_initial.z() << ")_("<< disc_final.x() << "_"  << disc_final.y() << "_"  << disc_final.z() << ").bt";
			// octree.writeBinary(octomap_name_stream.str());
			std::stringstream to_log_file_ss;
			to_log_file_ss << "!!! No path !!!   " ;
			to_log_file_ss << "Straight line length " << weightedDistance(disc_initial, disc_final);
			to_log_file_ss <<  std::setprecision(2) << " from  " << "(" << disc_initial.x() << disc_initial.y() << disc_initial.z() << ")" ;
			to_log_file_ss <<  std::setprecision(2) << " to " << "(" << disc_final.x() << disc_final.y() << disc_final.z() << ")" << std::endl;
			std::ofstream log_file;
    		log_file.open(folder_name + "/current/lazyThetaStar.log", std::ios_base::app);
    		log_file << to_log_file_ss.str();
	    	log_file.close();
		}
		else
		{
			for (std::list<octomath::Vector3>::iterator i = resulting_path.begin(); i != resulting_path.end(); ++i)
			{
				// ROS_INFO_STREAM(*i);
				geometry_msgs::Pose waypoint;
	            waypoint.position.x = i->x();
	            waypoint.position.y = i->y();
	            waypoint.position.z = i->z();
	            waypoint.orientation = tf::createQuaternionMsgFromYaw(0);
	            reply.waypoints.push_back(waypoint);
			}
			reply.success = true;
		}
		reply.waypoint_amount = resulting_path.size();
		reply.request_id = request.request_id;
		return true;
	}


}