// $Id: OccupancyOcTreeBase.hxx 434 2012-10-07 10:37:00Z ahornung $

/**
 * OctoMap:
 * A probabilistic, flexible, and compact 3D mapping library for robotic systems.
 * @author K. M. Wurm, A. Hornung, University of Freiburg, Copyright (C) 2009.
 * @see http://octomap.sourceforge.net/
 * License: New BSD License
 */

/*
 * Copyright (c) 2009-2011, K. M. Wurm, A. Hornung, University of Freiburg
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the University of Freiburg nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <bitset>

namespace octomap {

  template <class NODE>
  OccupancyOcTreeBase<NODE>::OccupancyOcTreeBase(double _resolution)
    : OcTreeBase<NODE>(_resolution), use_bbx_limit(false), use_change_detection(false) {
    // some sane default values:
    setOccupancyThres(0.5);   // = 0.0 in logodds
    setProbHit(0.7);          // = 0.85 in logodds
    setProbMiss(0.4);         // = -0.4 in logodds

    setClampingThresMin(0.1192); // = -2 in log odds
    setClampingThresMax(0.971); // = 3.5 in log odds
  }

  template <class NODE>
  OccupancyOcTreeBase<NODE>::~OccupancyOcTreeBase(){
  }

  // performs transformation to data and sensor origin first
  template <class NODE>
  void OccupancyOcTreeBase<NODE>::insertScan(const ScanNode& scan, double maxrange, bool pruning, bool lazy_eval) {
    Pointcloud& cloud = *(scan.scan);
    pose6d frame_origin = scan.pose;
    point3d sensor_origin = frame_origin.inv().transform(scan.pose.trans());
    insertScan(cloud, sensor_origin, frame_origin, maxrange, pruning, lazy_eval);
  }


  template <class NODE>
  void OccupancyOcTreeBase<NODE>::insertScan(const Pointcloud& scan, const octomap::point3d& sensor_origin, 
                                             double maxrange, bool pruning, bool lazy_eval) {

    KeySet free_cells, occupied_cells;
    computeUpdate(scan, sensor_origin, free_cells, occupied_cells, maxrange);    

    // insert data into tree  -----------------------
    for (KeySet::iterator it = free_cells.begin(); it != free_cells.end(); ++it) {
      updateNode(*it, false, lazy_eval);
    }
    for (KeySet::iterator it = occupied_cells.begin(); it != occupied_cells.end(); ++it) {
      updateNode(*it, true, lazy_eval);
    }

    // TODO: does pruning make sense if we used "lazy_eval"?
    if (pruning) this->prune();
  } 

  // performs transformation to data and sensor origin first
  template <class NODE>
  void OccupancyOcTreeBase<NODE>::insertScan(const Pointcloud& pc, const point3d& sensor_origin, const pose6d& frame_origin, 
                                             double maxrange, bool pruning, bool lazy_eval) {
    Pointcloud transformed_scan (pc);
    transformed_scan.transform(frame_origin);
    point3d transformed_sensor_origin = frame_origin.transform(sensor_origin);
    insertScan(transformed_scan, transformed_sensor_origin, maxrange, pruning, lazy_eval);
  }


  template <class NODE>
  void OccupancyOcTreeBase<NODE>::insertScanNaive(const Pointcloud& pc, const point3d& origin, double maxrange, bool pruning, bool lazy_eval) {
    if (pc.size() < 1)
      return;

    // integrate each single beam
    octomap::point3d p;
    for (octomap::Pointcloud::const_iterator point_it = pc.begin();
         point_it != pc.end(); point_it++) {
      this->insertRay(origin, *point_it, maxrange, lazy_eval);
    }

    if (pruning)
      this->prune();
  }


  template <class NODE>
  void OccupancyOcTreeBase<NODE>::computeUpdate(const Pointcloud& scan, const octomap::point3d& origin,
                                                KeySet& free_cells, KeySet& occupied_cells,
                                                double maxrange) {

    //#pragma omp parallel private (local_key_ray, point_it) 
    for (Pointcloud::const_iterator point_it = scan.begin(); point_it != scan.end(); point_it++) {
      const point3d& p = *point_it;
      if (!use_bbx_limit) {
        // -------------- no BBX specified ---------------
        if ((maxrange < 0.0) || ((p - origin).norm() <= maxrange) ) { // is not maxrange meas.
          // free cells
          if (this->computeRayKeys(origin, p, this->keyray)){
            free_cells.insert(this->keyray.begin(), this->keyray.end());
          }
          // occupied endpoint
          OcTreeKey key;
          if (this->genKey(p, key))
            occupied_cells.insert(key);
        } // end if NOT maxrange

        else { // user set a maxrange and this is reached
          point3d direction = (p - origin).normalized ();
          point3d new_end = origin + direction * (float) maxrange;
          if (this->computeRayKeys(origin, new_end, this->keyray)){
            free_cells.insert(this->keyray.begin(), this->keyray.end());
          }
        } // end if maxrange
      }

      // --- update limited by user specified BBX  -----
      else {
        // endpoint in bbx and not maxrange?
        if ( inBBX(p) && ((maxrange < 0.0) || ((p - origin).norm () <= maxrange) ) )  {

          // occupied endpoint
          OcTreeKey key;
          if (this->genKey(p, key))
            occupied_cells.insert(key);

          // update freespace, break as soon as bbx limit is reached
          if (this->computeRayKeys(origin, p, this->keyray)){
            for(KeyRay::reverse_iterator rit=this->keyray.rbegin(); rit != this->keyray.rend(); rit++) {
              if (inBBX(*rit)) {
                free_cells.insert(*rit);
              }
              else break;
            }
          } // end if compute ray
        } // end if in BBX and not maxrange
      } // end bbx case

    } // end for all points

    // prefer occupied cells over free ones (and make sets disjunct)
    for(KeySet::iterator it = free_cells.begin(), end=free_cells.end(); it!= end; ){
      if (occupied_cells.find(*it) != occupied_cells.end()){
        it = free_cells.erase(it);
      } else {
        ++it;
      }
    }
  }

  template <class NODE>
  NODE* OccupancyOcTreeBase<NODE>::updateNode(const OcTreeKey& key, float log_odds_update, bool lazy_eval) {
    return updateNodeRecurs(this->itsRoot, false, key, 0, log_odds_update, lazy_eval);
  }

  template <class NODE>
  NODE* OccupancyOcTreeBase<NODE>::updateNode(const point3d& value, float log_odds_update, bool lazy_eval) {
    OcTreeKey key;
    if (!this->genKey(value, key)) return NULL;
    return updateNode(key, log_odds_update, lazy_eval);
  }

  template <class NODE>
  NODE* OccupancyOcTreeBase<NODE>::updateNode(const OcTreeKey& key, bool occupied, bool lazy_eval) {
    NODE* leaf = this->search(key);
    // no change: node already at threshold
    if (leaf && (isNodeAtThreshold(leaf)) && (isNodeOccupied(leaf) == occupied)) {
      return leaf;
    }
    if (occupied) return updateNodeRecurs(this->itsRoot, false, key, 0, probHitLog,  lazy_eval);
    else          return updateNodeRecurs(this->itsRoot, false, key, 0, probMissLog, lazy_eval);
  }

  template <class NODE>
  NODE* OccupancyOcTreeBase<NODE>::updateNode(const point3d& value, bool occupied, bool lazy_eval) {
    OcTreeKey key;
    if (!this->genKey(value, key)) return NULL;
    return updateNode(key, occupied, lazy_eval);
  }

  template <class NODE>
  NODE* OccupancyOcTreeBase<NODE>::updateNodeRecurs(NODE* node, bool node_just_created, const OcTreeKey& key,
                                                    unsigned int depth, const float& log_odds_update, bool lazy_eval) {
    unsigned int pos (0);
    this->genPos(key, this->tree_depth-1-depth, pos);
    bool created_node = false;

    // follow down to last level
    if (depth < this->tree_depth) {
      if (!node->childExists(pos)) {
        // child does not exist, but maybe it's a pruned node?
        if ((!node->hasChildren()) && !node_just_created && (node != this->itsRoot)) {
          // current node does not have children AND it is not a new node 
          // AND its not the root node
          // -> expand pruned node
          node->expandNode();
          this->tree_size+=8;
          this->sizeChanged = true;
        }
        else {
          // not a pruned node, create requested child
          node->createChild(pos);
          this->tree_size++;
          this->sizeChanged = true;
          created_node = true;
        }
      }

      if (lazy_eval)
        return updateNodeRecurs(node->getChild(pos), created_node, key, depth+1, log_odds_update, lazy_eval);
      else {
        NODE* retval = updateNodeRecurs(node->getChild(pos), created_node, key, depth+1, log_odds_update, lazy_eval);
        // set own probability according to prob of children
        node->updateOccupancyChildren();
        return retval;
      }
    }

    // at last level, update node, end of recursion
    else {
      if (use_change_detection) {
        bool occBefore = isNodeOccupied(node);
        updateNodeLogOdds(node, log_odds_update); 
        if (node_just_created){  // new node
          changedKeys.insert(std::pair<OcTreeKey,bool>(key, true));
        } else if (occBefore != isNodeOccupied(node)) {  // occupancy changed, track it
          KeyBoolMap::iterator it = changedKeys.find(key);
          if (it == changedKeys.end())
            changedKeys.insert(std::pair<OcTreeKey,bool>(key, false));
          else if (it->second == false)
            changedKeys.erase(it);
        }
      }
      else {
        updateNodeLogOdds(node, log_odds_update); 
      }
      return node;
    }
  }
  

  template <class NODE>
  void OccupancyOcTreeBase<NODE>::calcNumThresholdedNodes(unsigned int& num_thresholded,
                                       unsigned int& num_other) const {
    num_thresholded = 0;
    num_other = 0;
    // TODO: The recursive call could be completely replaced with the new iterators
    calcNumThresholdedNodesRecurs(this->itsRoot, num_thresholded, num_other);
  }

  template <class NODE>
  void OccupancyOcTreeBase<NODE>::calcNumThresholdedNodesRecurs (NODE* node,
                                              unsigned int& num_thresholded,
                                              unsigned int& num_other) const {
    assert(node != NULL);
    for (unsigned int i=0; i<8; i++) {
      if (node->childExists(i)) {
        NODE* child_node = node->getChild(i);
        if (isNodeAtThreshold(child_node))
          num_thresholded++;
        else
          num_other++;
        calcNumThresholdedNodesRecurs(child_node, num_thresholded, num_other);
      } // end if child
    } // end for children
  }

  template <class NODE>
  void OccupancyOcTreeBase<NODE>::updateInnerOccupancy(){
    this->updateInnerOccupancyRecurs(this->itsRoot, 0);
  }

  template <class NODE>
  void OccupancyOcTreeBase<NODE>::updateInnerOccupancyRecurs(NODE* node, unsigned int depth){
    // only recurse and update for inner nodes:
    if (node->hasChildren()){
      // return early for last level:
      if (depth < this->tree_depth){
        for (unsigned int i=0; i<8; i++) {
          if (node->childExists(i)) {
            updateInnerOccupancyRecurs(node->getChild(i), depth+1);
          }
        }
      }
      node->updateOccupancyChildren();
    }
  }

  template <class NODE>
  void OccupancyOcTreeBase<NODE>::toMaxLikelihood() {

    // convert bottom up
    for (unsigned int depth=this->tree_depth; depth>0; depth--) {
      toMaxLikelihoodRecurs(this->itsRoot, 0, depth);
    }

    // convert root
    nodeToMaxLikelihood(this->itsRoot);
  }

  template <class NODE>
  void OccupancyOcTreeBase<NODE>::toMaxLikelihoodRecurs(NODE* node, unsigned int depth,
      unsigned int max_depth) {

    if (depth < max_depth) {
      for (unsigned int i=0; i<8; i++) {
        if (node->childExists(i)) {
          toMaxLikelihoodRecurs(node->getChild(i), depth+1, max_depth);
        }
      }
    }

    else { // max level reached
      nodeToMaxLikelihood(node);
    }
  }
  
  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::castRay(const point3d& origin, const point3d& directionP, point3d& end, 
                                          bool ignoreUnknown, double maxRange) const {

    /// ----------  see OcTreeBase::computeRayKeys  -----------

    // Initialization phase -------------------------------------------------------
    // Initialization phase -------------------------------------------------------
    OcTreeKey current_key;
    if ( !OcTreeBase<NODE>::genKey(origin, current_key) ) {
      OCTOMAP_WARNING_STR("Coordinates out of bounds during ray casting");
      return false;
    }
    
    NODE* startingNode = this->search(current_key);
    if (startingNode){
      if (isNodeOccupied(startingNode)){
        // Occupied node found at origin 
        // (need to convert from key, since origin does not need to be a voxel center)
        genCoords(current_key, this->tree_depth, end);
        return true;
      }
    } else if(!ignoreUnknown){
      OCTOMAP_ERROR_STR("Origin node at " << origin << " for raycasting not found, does the node exist?");
      genCoords(current_key, this->tree_depth, end);
      return false;
    }
    
    point3d direction = directionP.normalized();
    bool max_range_set = (maxRange > 0.);
    
    int step[3]; 
    double tMax[3];
    double tDelta[3];
    
    for(unsigned int i=0; i < 3; ++i) {
      // compute step direction
      if (direction(i) > 0.0) step[i] =  1;
      else if (direction(i) < 0.0)   step[i] = -1;
      else step[i] = 0;
      
      // compute tMax, tDelta
      if (step[i] != 0) {
        // corner point of voxel (in direction of ray)
        float voxelBorder(0);
        this->genCoordFromKey(current_key[i], voxelBorder); 
        voxelBorder += float (step[i] * this->resolution * 0.5);
        
        tMax[i] = ( voxelBorder - origin(i) ) / direction(i);
        tDelta[i] = this->resolution / fabs( direction(i) );
      }
      else {
        tMax[i] =  std::numeric_limits<double>::max();
        tDelta[i] = std::numeric_limits<double>::max();
      }
    }
    
    if (step[0] == 0 && step[1] == 0 && step[2] == 0){
      OCTOMAP_ERROR("Raycasting in direction (0,0,0) is not possible!");
      return false;
    }
    
    // for speedup:
    double maxrange_sq = maxRange * maxRange;
    
    // Incremental phase  ---------------------------------------------------------
    
    bool done = false;
    
    while (!done) {
      unsigned int dim;
      
      // find minimum tMax:
      if (tMax[0] < tMax[1]){
        if (tMax[0] < tMax[2]) dim = 0;
        else                   dim = 2;
      }
      else {
        if (tMax[1] < tMax[2]) dim = 1;
        else                   dim = 2;
      }
      
      // check for overflow:
      if ((step[dim] < 0 && current_key[dim] == 0)
        || (step[dim] > 0 && current_key[dim] == 2* this->tree_max_val-1))
      {
        OCTOMAP_WARNING("Coordinate hit bounds in dim %d, aborting raycast\n", dim);
        // return border point nevertheless:
        genCoords(current_key, this->tree_depth, end);
        return false;
      }
      
      // advance in direction "dim"
      current_key[dim] += step[dim];
      tMax[dim] += tDelta[dim];
      
      
      // generate world coords from key
      genCoords(current_key, this->tree_depth, end);
      
      // check for maxrange:
      if (max_range_set){
        double dist_from_origin_sq(0.0);
        for (unsigned int j = 0; j < 3; j++) {
          dist_from_origin_sq += ((end(j) - origin(j)) * (end(j) - origin(j)));
        }
        if (dist_from_origin_sq > maxrange_sq)
          return false;
        
      }
      
      NODE* currentNode = this->search(current_key);
      if (currentNode){
        if (isNodeOccupied(currentNode)) {
          done = true;
          break;
        }
        // otherwise: node is free and valid, raycasting continues
      }
      
      else if (!ignoreUnknown){ // no node found, this usually means we are in "unknown" areas
        OCTOMAP_WARNING_STR("Search failed in OcTree::castRay() => an unknown area was hit in the map: " << end);
        return false;
      }
    } // end while
    
    return true;
  }


  template <class NODE> inline bool 
  OccupancyOcTreeBase<NODE>::integrateMissOnRay(const point3d& origin, const point3d& end, bool lazy_eval) {

    if (!this->computeRayKeys(origin, end, this->keyray)) {
      return false;
    }
    
    for(KeyRay::iterator it=this->keyray.begin(); it != this->keyray.end(); it++) {
      updateNode(*it, false, lazy_eval); // insert freespace measurement
    }
  
    return true;
  }

  template <class NODE> bool 
  OccupancyOcTreeBase<NODE>::insertRay(const point3d& origin, const point3d& end, double maxrange, bool lazy_eval)
  {
    // cut ray at maxrange
    if ((maxrange > 0) && ((end - origin).norm () > maxrange)) 
      {
        point3d direction = (end - origin).normalized ();
        point3d new_end = origin + direction * (float) maxrange;
        return integrateMissOnRay(origin, new_end,lazy_eval);
      }
    // insert complete ray
    else 
      {
        if (!integrateMissOnRay(origin, end,lazy_eval))
          return false;
        updateNode(end, true, lazy_eval); // insert hit cell
        return true;
      }
  }
  
  template <class NODE>
  void OccupancyOcTreeBase<NODE>::getOccupied(point3d_list& node_centers, unsigned int max_depth) const {

    if (max_depth == 0)
      max_depth = this->tree_depth;

    for(typename OccupancyOcTreeBase<NODE>::leaf_iterator it = this->begin(max_depth),
        end=this->end(); it!= end; ++it)
    {
      if(this->isNodeOccupied(*it))
        node_centers.push_back(it.getCoordinate());
    }
  }


  template <class NODE>
  void OccupancyOcTreeBase<NODE>::getOccupied(std::list<OcTreeVolume>& occupied_nodes, unsigned int max_depth) const{

    if (max_depth == 0)
      max_depth = this->tree_depth;

    for(typename OccupancyOcTreeBase<NODE>::leaf_iterator it = this->begin(max_depth),
            end=this->end(); it!= end; ++it)
    {
      if(this->isNodeOccupied(*it))
        occupied_nodes.push_back(OcTreeVolume(it.getCoordinate(), it.getSize()));
    }

  }

  
  template <class NODE>
  void OccupancyOcTreeBase<NODE>::getOccupied(std::list<OcTreeVolume>& binary_nodes,
                                              std::list<OcTreeVolume>& delta_nodes,
                                              unsigned int max_depth) const{    
    if (max_depth == 0)
      max_depth = this->tree_depth;

    for(typename OccupancyOcTreeBase<NODE>::leaf_iterator it = this->begin(max_depth),
            end=this->end(); it!= end; ++it)
    {
      if(this->isNodeOccupied(*it)){
        if (it->getLogOdds() >= clampingThresMax)
          binary_nodes.push_back(OcTreeVolume(it.getCoordinate(), it.getSize()));
        else
          delta_nodes.push_back(OcTreeVolume(it.getCoordinate(), it.getSize()));
      }
    }
  }

  template <class NODE>
  void OccupancyOcTreeBase<NODE>::getFreespace(std::list<OcTreeVolume>& free_nodes, unsigned int max_depth) const{

    if (max_depth == 0)
      max_depth = this->tree_depth;

    for(typename OccupancyOcTreeBase<NODE>::leaf_iterator it = this->begin(max_depth),
            end=this->end(); it!= end; ++it)
    {
      if(!this->isNodeOccupied(*it))
        free_nodes.push_back(OcTreeVolume(it.getCoordinate(), it.getSize()));
    }
  }


  template <class NODE>
  void OccupancyOcTreeBase<NODE>::getFreespace(std::list<OcTreeVolume>& binary_nodes,
                                               std::list<OcTreeVolume>& delta_nodes,
                                               unsigned int max_depth) const{

    if (max_depth == 0)
      max_depth = this->tree_depth;

    for(typename OccupancyOcTreeBase<NODE>::leaf_iterator it = this->begin(max_depth),
            end=this->end(); it!= end; ++it)
    {
      if(!this->isNodeOccupied(*it)){
        if (it->getLogOdds() <= clampingThresMin)
          binary_nodes.push_back(OcTreeVolume(it.getCoordinate(), it.getSize()));
        else
          delta_nodes.push_back(OcTreeVolume(it.getCoordinate(), it.getSize()));
      }
    }
  }

  
  template <class NODE>
  void OccupancyOcTreeBase<NODE>::setBBXMin (point3d& min) { 
    bbx_min = min; 
    if (!this->genKey(bbx_min, bbx_min_key)) {
      OCTOMAP_ERROR("ERROR while generating bbx min key.\n");
    }
  }

  template <class NODE>
  void OccupancyOcTreeBase<NODE>::setBBXMax (point3d& max) {
    bbx_max = max; 
    if (!this->genKey(bbx_max, bbx_max_key)) {
      OCTOMAP_ERROR("ERROR while generating bbx max key.\n");
    }
  }


  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::inBBX(const point3d& p) const {
    return ((p.x() >= bbx_min.x()) && (p.y() >= bbx_min.y()) && (p.z() >= bbx_min.z()) &&
            (p.x() <= bbx_max.x()) && (p.y() <= bbx_max.y()) && (p.z() <= bbx_max.z()) );
  }


  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::inBBX(const OcTreeKey& key) const {
    return ((key[0] >= bbx_min_key[0]) && (key[1] >= bbx_min_key[1]) && (key[2] >= bbx_min_key[2]) &&
            (key[0] <= bbx_max_key[0]) && (key[1] <= bbx_max_key[1]) && (key[2] <= bbx_max_key[2]) );
  }

  template <class NODE>
  point3d OccupancyOcTreeBase<NODE>::getBBXBounds () const {
    octomap::point3d obj_bounds = (bbx_max - bbx_min);
    obj_bounds /= 2.;
    return obj_bounds;
  }

  template <class NODE>
  point3d OccupancyOcTreeBase<NODE>::getBBXCenter () const {
    octomap::point3d obj_bounds = (bbx_max - bbx_min);
    obj_bounds /= 2.;
    return bbx_min + obj_bounds;
  }


  template <class NODE>
  void OccupancyOcTreeBase<NODE>::getOccupiedLeafsBBX(point3d_list& node_centers, point3d min, point3d max) const {

    OcTreeKey root_key, min_key, max_key;
    root_key[0] = root_key[1] = root_key[2] = this->tree_max_val; 
    if (!this->genKey(min, min_key)) return;
    if (!this->genKey(max, max_key)) return;
    getOccupiedLeafsBBXRecurs(node_centers, this->tree_depth, this->itsRoot, 0, root_key, min_key, max_key);
  }

  
  template <class NODE>
  void OccupancyOcTreeBase<NODE>::getOccupiedLeafsBBXRecurs( point3d_list& node_centers, unsigned int max_depth, 
                                                             NODE* node, unsigned int depth, const OcTreeKey& parent_key, 
                                                             const OcTreeKey& min, const OcTreeKey& max) const {
    if (depth == max_depth) { // max level reached
      if (isNodeOccupied(node)) {
        point3d p;
        this->genCoords(parent_key, depth, p);
        node_centers.push_back(p);        
      }
    }

    if (!node->hasChildren()) return;

    unsigned short int center_offset_key = this->tree_max_val >> (depth + 1);

    OcTreeKey child_key;

    for (unsigned int i=0; i<8; ++i) {
      if (node->childExists(i)) {

        computeChildKey(i, center_offset_key, parent_key, child_key);

        // overlap of query bbx and child bbx?
        if (!( 
              ( min[0] > (child_key[0] + center_offset_key)) ||
              ( max[0] < (child_key[0] - center_offset_key)) ||
              ( min[1] > (child_key[1] + center_offset_key)) ||
              ( max[1] < (child_key[1] - center_offset_key)) ||
              ( min[2] > (child_key[2] + center_offset_key)) ||
              ( max[2] < (child_key[2] - center_offset_key))
               )) {
          getOccupiedLeafsBBXRecurs(node_centers, max_depth, node->getChild(i), depth+1, child_key, min, max);
        }
      }
    }
  }

  // -- I/O  -----------------------------------------

  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::readBinary(const std::string& filename){
    std::ifstream binary_infile( filename.c_str(), std::ios_base::binary);
    if (!binary_infile.is_open()){
      OCTOMAP_ERROR_STR("Filestream to "<< filename << " not open, nothing read.");
      return false;
    }
    return readBinary(binary_infile);
  }

  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::readBinaryLegacyHeader(std::istream &s, unsigned int& size, double& res) {

    if (!s.good()){
      OCTOMAP_WARNING_STR("Input filestream not \"good\" in OcTree::readBinary");
    }

    int tree_type = -1;
    s.read((char*)&tree_type, sizeof(tree_type));
    if (tree_type == 3){

      this->clear();

      s.read((char*)&res, sizeof(res));
      if (res <= 0.0){
        OCTOMAP_ERROR("Invalid tree resolution: %f", res);
        return false;
      }

      s.read((char*)&size, sizeof(size));

      return true;
    }
    else {
      OCTOMAP_ERROR_STR("Binary file does not contain an OcTree!");
      return false;
    }
  }


  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::readBinary(std::istream &s) {

    if (!s.good()){
      OCTOMAP_WARNING_STR("Input filestream not \"good\" in OcTree::readBinary");
    }

    // check if first line valid:
    std::string line;
    int streampos = s.tellg();
    std::getline(s, line);
    unsigned size;
    double res;
    if (line.compare(0,AbstractOcTree::binaryFileHeader.length(), AbstractOcTree::binaryFileHeader) ==0){
      std::string id;
      if (!AbstractOcTree::readHeader(s, id, size, res))
        return false;

      OCTOMAP_DEBUG_STR("Reading binary octree type "<< id);
    } else{ // try to read old binary format:
      s.clear(); // clear eofbit of istream
      s.seekg(streampos);
      if (readBinaryLegacyHeader(s, size, res)){
        OCTOMAP_WARNING_STR("You are using an outdated binary tree file format.");
        OCTOMAP_WARNING_STR("Please convert your .bt files with convert_octree.");



      }
      else {
        OCTOMAP_ERROR_STR("First line of OcTree file header does not start with \""<< AbstractOcTree::binaryFileHeader<<"\"");
        return false;
      }
    }
    // otherwise: values are valid, stream is now at binary data!
    this->clear();
    this->setResolution(res);

    this->readBinaryNode(s, this->itsRoot);
    this->sizeChanged = true;
    this->tree_size = OcTreeBase<NODE>::calcNumNodes();  // compute number of nodes

    if (size != this->tree_size){
      OCTOMAP_ERROR("Tree size mismatch: # read nodes (%zu) != # expected nodes (%d)\n",this->tree_size, size);
      return false;
    }

    return true;
  }

  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::writeBinary(const std::string& filename){
    std::ofstream binary_outfile( filename.c_str(), std::ios_base::binary);

    if (!binary_outfile.is_open()){
      OCTOMAP_ERROR_STR("Filestream to "<< filename << " not open, nothing written.");
      return false;
    }    
    return writeBinary(binary_outfile);
  }

  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::writeBinaryConst(const std::string& filename) const{
    std::ofstream binary_outfile( filename.c_str(), std::ios_base::binary);

    if (!binary_outfile.is_open()){
      OCTOMAP_ERROR_STR("Filestream to "<< filename << " not open, nothing written.");
      return false;
    }    
    writeBinaryConst(binary_outfile);
    binary_outfile.close();
    return true;
  }

  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::writeBinary(std::ostream &s){

    // convert to max likelihood first, this makes efficient pruning on binary data possible
    this->toMaxLikelihood();
    this->prune();
    return writeBinaryConst(s);
  }

  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::writeBinaryConst(std::ostream &s) const{
    // write new header first:
    s << AbstractOcTree::binaryFileHeader <<"\n# (feel free to add / change comments, but leave the first line as it is!)\n#\n";
    s << "id " << this->getTreeType() << std::endl;
    s << "size "<< this->size() << std::endl;
    s << "res " << this->getResolution() << std::endl;
    s << "data" << std::endl;

    OCTOMAP_DEBUG("Writing %zu nodes to output stream...", this->size());
    this->writeBinaryNode(s, this->itsRoot);
    if (s.good()){
      OCTOMAP_DEBUG(" done.\n");
      return true;
    } else {
      OCTOMAP_WARNING_STR("Output stream not \"good\" after writing tree");
      return false;
    }
  }

  template <class NODE>
  std::istream& OccupancyOcTreeBase<NODE>::readBinaryNode(std::istream &s, NODE* node) const {

    char child1to4_char;
    char child5to8_char;
    s.read((char*)&child1to4_char, sizeof(char));
    s.read((char*)&child5to8_char, sizeof(char));

    std::bitset<8> child1to4 ((unsigned long long) child1to4_char);
    std::bitset<8> child5to8 ((unsigned long long) child5to8_char);

    //     std::cout << "read:  "
    //        << child1to4.to_string<char,std::char_traits<char>,std::allocator<char> >() << " "
    //        << child5to8.to_string<char,std::char_traits<char>,std::allocator<char> >() << std::endl;


    // inner nodes default to occupied
    node->setLogOdds(clampingThresMax);

    for (unsigned int i=0; i<4; i++) {
      if ((child1to4[i*2] == 1) && (child1to4[i*2+1] == 0)) {
        // child is free leaf
        node->createChild(i);
        node->getChild(i)->setLogOdds(clampingThresMin);
      }
      else if ((child1to4[i*2] == 0) && (child1to4[i*2+1] == 1)) {
        // child is occupied leaf
        node->createChild(i);
        node->getChild(i)->setLogOdds(clampingThresMax);
      }
      else if ((child1to4[i*2] == 1) && (child1to4[i*2+1] == 1)) {
        // child has children
        node->createChild(i);
        node->getChild(i)->setLogOdds(-200.); // child is unkown, we leave it uninitialized
      }
    }
    for (unsigned int i=0; i<4; i++) {
      if ((child5to8[i*2] == 1) && (child5to8[i*2+1] == 0)) {
        // child is free leaf
        node->createChild(i+4);
        node->getChild(i+4)->setLogOdds(clampingThresMin);
      }
      else if ((child5to8[i*2] == 0) && (child5to8[i*2+1] == 1)) {
        // child is occupied leaf
        node->createChild(i+4);
        node->getChild(i+4)->setLogOdds(clampingThresMax);
      }
      else if ((child5to8[i*2] == 1) && (child5to8[i*2+1] == 1)) {
        // child has children
        node->createChild(i+4);
        node->getChild(i+4)->setLogOdds(-200.); // set occupancy when all children have been read
      }
      // child is unkown, we leave it uninitialized
    }

    // read children's children and set the label
    for (unsigned int i=0; i<8; i++) {
      if (node->childExists(i)) {
        NODE* child = node->getChild(i);
        if (fabs(child->getLogOdds() + 200.)<1e-3) {
          readBinaryNode(s, child);
          child->setLogOdds(child->getMaxChildLogOdds());
        }
      } // end if child exists
    } // end for children

    return s;
  }

  template <class NODE>
  std::ostream& OccupancyOcTreeBase<NODE>::writeBinaryNode(std::ostream &s, const NODE* node) const{

    // 2 bits for each children, 8 children per node -> 16 bits
    std::bitset<8> child1to4;
    std::bitset<8> child5to8;

    // 10 : child is free node
    // 01 : child is occupied node
    // 00 : child is unkown node
    // 11 : child has children


    // speedup: only set bits to 1, rest is init with 0 anyway,
    //          can be one logic expression per bit

    for (unsigned int i=0; i<4; i++) {
      if (node->childExists(i)) {
        const NODE* child = node->getChild(i);
        if      (child->hasChildren())  { child1to4[i*2] = 1; child1to4[i*2+1] = 1; }
        else if (isNodeOccupied(child)) { child1to4[i*2] = 0; child1to4[i*2+1] = 1; }
        else                            { child1to4[i*2] = 1; child1to4[i*2+1] = 0; }
      }
      else {
        child1to4[i*2] = 0; child1to4[i*2+1] = 0;
      }
    }

    for (unsigned int i=0; i<4; i++) {
      if (node->childExists(i+4)) {
        const NODE* child = node->getChild(i+4);
        if      (child->hasChildren())  { child5to8[i*2] = 1; child5to8[i*2+1] = 1; }
        else if (isNodeOccupied(child)) { child5to8[i*2] = 0; child5to8[i*2+1] = 1; }
        else                            { child5to8[i*2] = 1; child5to8[i*2+1] = 0; }
      }
      else {
        child5to8[i*2] = 0; child5to8[i*2+1] = 0;
      }
    }
    //     std::cout << "wrote: "
    //        << child1to4.to_string<char,std::char_traits<char>,std::allocator<char> >() << " "
    //        << child5to8.to_string<char,std::char_traits<char>,std::allocator<char> >() << std::endl;

    char child1to4_char = (char) child1to4.to_ulong();
    char child5to8_char = (char) child5to8.to_ulong();

    s.write((char*)&child1to4_char, sizeof(char));
    s.write((char*)&child5to8_char, sizeof(char));

    // write children's children
    for (unsigned int i=0; i<8; i++) {
      if (node->childExists(i)) {
        const NODE* child = node->getChild(i);
        if (child->hasChildren()) {
          writeBinaryNode(s, child);
        }
      }
    }

    return s;
  }

  //-- Occupancy queries on nodes:

  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::isNodeOccupied(const NODE* occupancyNode) const{
    return (occupancyNode->getLogOdds() >= occProbThresLog);
  }
  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::isNodeOccupied(const NODE& occupancyNode) const{
    return (occupancyNode.getLogOdds() >= occProbThresLog);
  }

  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::isNodeAtThreshold(const NODE* occupancyNode) const{
    return (occupancyNode->getLogOdds() >= clampingThresMax
            || occupancyNode->getLogOdds() <= clampingThresMin);
  }

  template <class NODE>
  bool OccupancyOcTreeBase<NODE>::isNodeAtThreshold(const NODE& occupancyNode) const{
    return (occupancyNode.getLogOdds() >= clampingThresMax
            || occupancyNode.getLogOdds() <= clampingThresMin);
  }

  template <class NODE>
  void OccupancyOcTreeBase<NODE>::updateNodeLogOdds(NODE* occupancyNode, const float& update) const {
    occupancyNode->addValue(update);
    if (occupancyNode->getLogOdds() < clampingThresMin) {
      occupancyNode->setLogOdds(clampingThresMin);
      return;
    }
    if (occupancyNode->getLogOdds() > clampingThresMax) {
      occupancyNode->setLogOdds(clampingThresMax);
    }
  }

  template <class NODE>
  void OccupancyOcTreeBase<NODE>::integrateHit(NODE* occupancyNode) const {
    updateNodeLogOdds(occupancyNode, probHitLog);
  }

  template <class NODE>
  void OccupancyOcTreeBase<NODE>::integrateMiss(NODE* occupancyNode) const {
    updateNodeLogOdds(occupancyNode, probMissLog);
  }
  
  template <class NODE>
  void OccupancyOcTreeBase<NODE>::nodeToMaxLikelihood(NODE* occupancyNode) const{
    if (isNodeOccupied(occupancyNode))
      occupancyNode->setLogOdds(clampingThresMax);
    else
      occupancyNode->setLogOdds(clampingThresMin);
  }

  template <class NODE>
  void OccupancyOcTreeBase<NODE>::nodeToMaxLikelihood(NODE& occupancyNode) const{
    if (isNodeOccupied(occupancyNode))
      occupancyNode.setLogOdds(clampingThresMax);
    else
      occupancyNode.setLogOdds(clampingThresMin);
  }

} // namespace
