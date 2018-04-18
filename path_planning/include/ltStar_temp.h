#include <ltStarOctree_common.h>
#include <neighbors.h>
#include <voxel.h>
#include <open.h>
#include <list>
#include <unordered_map>
#include <path_planning_msgs/LTStarRequest.h>
#include <path_planning_msgs/LTStarReply.h>
#include <path_planning_msgs/LTStarNodeStatus.h>

// namespace std
// {
//     template <>
//     struct hash<octomath::Vector3>
//     {
//         size_t operator()( const octomath::Vector3& coordinates ) const
//         {
//             double fractpart, intpart;
//             fractpart = modf (coordinates.x()*10000 , &intpart);
//             return intpart;
//         }
//     };
// }

namespace LazyThetaStarOctree{
	float weightedDistance(octomath::Vector3 const& start, octomath::Vector3 const& end);
	bool hasLineOfSight(octomap::OcTree const& octree, octomath::Vector3 const& start, octomath::Vector3 const& end);
	bool freeCorridor(octomap::OcTree const& octree, octomath::Vector3 const& start, octomath::Vector3 const& end, double safety_margin, octomath::Vector3 & rectangle_min, octomath::Vector3 & rectangle_max);
	bool normalizeToVisibleEndCenter(octomap::OcTree const& octree, std::shared_ptr<octomath::Vector3> const& start, std::shared_ptr<octomath::Vector3> & end, double& cell_size);
	/**
	 * @brief      Set vertex portion of pseudo code, ln 34.
	 *
	 * @param      octree     The octree
	 * @param      s          TThe node in analyzis
	 * @param      closed     The closed
	 * @param      open       The open
	 * @param[in]  neighbors  The neighbors
	 */
	void setVertex(
		octomap::OcTree 										const& 	octree, 
		std::shared_ptr<ThetaStarNode> 							& 		s, 
		std::unordered_map<octomath::Vector3, std::shared_ptr<ThetaStarNode>, Vector3Hash, VectorComparatorEqual> &  closed,
		Open 													& 		open, 
		std::unordered_set<std::shared_ptr<octomath::Vector3>> 	const& 	neighbors,
		std::ofstream & log_file);

	/**
	 * @brief      Extracts a sequence of coordinates from the links between nodes starting at the goal node and expanding the connections to the prevuous point through parentNode.
	 *
	 * @param      path   The list object where to store the path
	 * @param      start  The starting node
	 * @param      end    The goal node
	 *
	 * @return     true is a path from goal node until start node was found (under 500 jumps). False otherwise.
	 */
	bool extractPath(std::list<octomath::Vector3> & path, ThetaStarNode const& start, ThetaStarNode & end, bool writeToFile = false);

	/**
	 * @brief      Calcute the cost if a link between these to nodes is created. This is not the ComputeCost method of the pseudo code.
	 *
	 * @param      s            node
	 * @param      s_neighbour  The neighbour
	 *
	 * @return     the new cost
	 */
	float CalculateCost(ThetaStarNode const& s, ThetaStarNode & s_neighbour);

	void UpdateVertex(ThetaStarNode const& s, std::shared_ptr<ThetaStarNode> s_neighbour,
		Open & open);

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
		octomap::OcTree   const& octree, 
		octomath::Vector3 const& disc_initial, 
		octomath::Vector3 const& disc_final,
		ResultSet & resultSet,
		int const& max_search_iterations = 55,
		bool print_resulting_path = false);

	bool processLTStarRequest(octomap::OcTree const& octree, path_planning_msgs::LTStarRequest const& request, path_planning_msgs::LTStarReply & reply);
}