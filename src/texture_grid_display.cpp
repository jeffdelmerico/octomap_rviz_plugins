/*
 * Copyright (c) 2013, Willow Garage, Inc.
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
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
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
 *
 * Author: Julius Kammerl (jkammerl@willowgarage.com)
 *
 */
#include <QObject>

#include "octomap_rviz_plugins/texture_grid_display.h"

#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>

#include <OGRE/OgreSceneNode.h>
#include <OGRE/OgreSceneManager.h>

#include "rviz/visualization_manager.h"
#include "rviz/frame_manager.h"
#include "rviz/properties/int_property.h"
#include "rviz/properties/ros_topic_property.h"
#include "rviz/properties/enum_property.h"

#include <octomap/octomap.h>
#include <octomap_msgs/Octomap.h>
#include <octomap_msgs/conversions.h>

#include <sstream>

using namespace rviz;

namespace octomap_rviz_plugin
{

static const std::size_t max_octree_depth_ = sizeof(unsigned short) * 8;

enum OctreeVoxelRenderMode
{
  OCTOMAP_FREE_VOXELS = 1,
  OCTOMAP_OCCUPIED_VOXELS = 2
};

enum OctreeVoxelColorMode
{
  OCTOMAP_TEXTURE_COLOR,
  OCTOMAP_Z_AXIS_COLOR,
  OCTOMAP_PROBABLILTY_COLOR,
};

TextureGridDisplay::TextureGridDisplay() :
    rviz::Display(),
    new_points_received_(false),
    messages_received_(0),
    queue_size_(5),
    color_factor_(0.8),
    octree_depth_(0)
{

  octomap_topic_property_ = new RosTopicProperty( "Octomap Topic",
                                                  "",
                                                  QString::fromStdString(ros::message_traits::datatype<octomap_msgs::Octomap>()),
                                                  "octomap_msgs::Octomap topic to subscribe to (binary or full probability map)",
                                                  this,
                                                  SLOT( updateTopic() ));

  queue_size_property_ = new IntProperty( "Queue Size",
                                          queue_size_,
                                          "Advanced: set the size of the incoming message queue.  Increasing this "
                                          "is useful if your incoming TF data is delayed significantly from your"
                                          " image data, but it can greatly increase memory usage if the messages are big.",
                                          this,
                                          SLOT( updateQueueSize() ));
  queue_size_property_->setMin(1);

  octree_render_property_ = new rviz::EnumProperty( "Voxel Rendering", "Occupied Voxels",
                                                    "Select voxel type.",
                                                     this,
                                                     SLOT( updateOctreeRenderMode() ) );

  octree_render_property_->addOption( "Occupied Voxels",  OCTOMAP_OCCUPIED_VOXELS );
  octree_render_property_->addOption( "Free Voxels",  OCTOMAP_FREE_VOXELS );
  octree_render_property_->addOption( "All Voxels",  OCTOMAP_FREE_VOXELS | OCTOMAP_OCCUPIED_VOXELS);

  octree_coloring_property_ = new rviz::EnumProperty( "Voxel Coloring", "Texture",
                                                "Select voxel coloring mode",
                                                this,
                                                SLOT( updateOctreeColorMode() ) );

  octree_coloring_property_->addOption( "Texture",  OCTOMAP_TEXTURE_COLOR );
  octree_coloring_property_->addOption( "Z-Axis",  OCTOMAP_Z_AXIS_COLOR );
  octree_coloring_property_->addOption( "Cell Probability",  OCTOMAP_PROBABLILTY_COLOR );

  tree_depth_property_ = new IntProperty("Max. Octree Depth",
                                         max_octree_depth_,
                                         "Defines the maximum tree depth",
                                         this,
                                         SLOT (updateTreeDepth() ));
  tree_depth_property_->setMin(0);
}

void TextureGridDisplay::onInitialize()
{
  boost::mutex::scoped_lock lock(mutex_);

  box_size_.resize(max_octree_depth_);
  cloud_.resize(max_octree_depth_);
  point_buf_.resize(max_octree_depth_);
  new_points_.resize(max_octree_depth_);

  for (std::size_t i = 0; i < max_octree_depth_; ++i)
  {
    std::stringstream sname;
    sname << "PointCloud Nr." << i;
    cloud_[i] = new rviz::PointCloud();
    cloud_[i]->setName(sname.str());
    cloud_[i]->setRenderMode(rviz::PointCloud::RM_BOXES);
    scene_node_->attachObject(cloud_[i]);
  }
}

TextureGridDisplay::~TextureGridDisplay()
{
  std::size_t i;

  unsubscribe();

  for (std::vector<rviz::PointCloud*>::iterator it = cloud_.begin(); it != cloud_.end(); ++it) {
    delete *(it);
  }

  if (scene_node_)
    scene_node_->detachAllObjects();
}

void TextureGridDisplay::updateQueueSize()
{
  queue_size_ = queue_size_property_->getInt();

  subscribe();
}

void TextureGridDisplay::onEnable()
{
  scene_node_->setVisible(true);
  subscribe();
}

void TextureGridDisplay::onDisable()
{
  scene_node_->setVisible(false);
  unsubscribe();

  clear();
}

void TextureGridDisplay::subscribe()
{
  if (!isEnabled())
  {
    return;
  }

  try
  {
    unsubscribe();

    const std::string& topicStr = octomap_topic_property_->getStdString();

    if (!topicStr.empty())
    {

      sub_.reset(new message_filters::Subscriber<octomap_msgs::Octomap>());

      sub_->subscribe(threaded_nh_, topicStr, queue_size_);
      sub_->registerCallback(boost::bind(&TextureGridDisplay::incomingMessageCallback, this, _1));

    }
  }
  catch (ros::Exception& e)
  {
    setStatus(StatusProperty::Error, "Topic", (std::string("Error subscribing: ") + e.what()).c_str());
  }

}

void TextureGridDisplay::unsubscribe()
{
  clear();

  try
  {
    // reset filters
    sub_.reset();
  }
  catch (ros::Exception& e)
  {
    setStatus(StatusProperty::Error, "Topic", (std::string("Error unsubscribing: ") + e.what()).c_str());
  }

}

// method taken from octomap_server package
void TextureGridDisplay::setColor(double z_pos, double min_z, double max_z, double color_factor,
                                    rviz::PointCloud::Point& point)
{
  int i;
  double m, n, f;

  double s = 1.0;
  double v = 1.0;

  double h = (1.0 - std::min(std::max((z_pos - min_z) / (max_z - min_z), 0.0), 1.0)) * color_factor;

  h -= floor(h);
  h *= 6;
  i = floor(h);
  f = h - i;
  if (!(i & 1))
    f = 1 - f; // if i is even
  m = v * (1 - s);
  n = v * (1 - s * f);

  switch (i)
  {
    case 6:
    case 0:
      point.setColor(v, n, m);
      break;
    case 1:
      point.setColor(n, v, m);
      break;
    case 2:
      point.setColor(m, v, n);
      break;
    case 3:
      point.setColor(m, n, v);
      break;
    case 4:
      point.setColor(n, m, v);
      break;
    case 5:
      point.setColor(v, m, n);
      break;
    default:
      point.setColor(1, 0.5, 0.5);
      break;
  }
}

void TextureGridDisplay::setIntensity( double intensity, rviz::PointCloud::Point& point)
{
  // intensity should already be between 0.0 and 1.0, but enforce this
  double i = std::max(0.0, std::min(1.0, intensity)); 
  point.setColor(i,i,i);
}

void TextureGridDisplay::incomingMessageCallback(const octomap_msgs::OctomapConstPtr& msg)
{
  ++messages_received_;
  setStatus(StatusProperty::Ok, "Messages", QString::number(messages_received_) + " octomap messages received");

  ROS_DEBUG("Received OctomapBinary message (size: %d bytes)", (int)msg->data.size());
  ROS_INFO("Received octomap of type: %s", msg->id.c_str());

  // get tf transform
  Ogre::Vector3 pos;
  Ogre::Quaternion orient;
  if (!context_->getFrameManager()->getTransform(msg->header, pos, orient))
  {
    std::stringstream ss;
    ss << "Failed to transform from frame [" << msg->header.frame_id << "] to frame ["
        << context_->getFrameManager()->getFixedFrame() << "]";
    this->setStatusStd(StatusProperty::Error, "Message", ss.str());
    return;
  }

  scene_node_->setOrientation(orient);
  scene_node_->setPosition(pos);

  // creating octree
  octomap::TextureOcTree* octomap = NULL;
  octomap::AbstractOcTree* tree = octomap_msgs::msgToMap(*msg);
  if (tree)
  {
    octomap = dynamic_cast<octomap::TextureOcTree*>(tree);
  }

  if (!octomap)
  {
    this->setStatusStd(StatusProperty::Error, "Message", "Failed to create octree structure");
    return;
  }

  std::size_t octree_depth = octomap->getTreeDepth();
  tree_depth_property_->setMax(octomap->getTreeDepth());
  

  // get dimensions of octree
  double minX, minY, minZ, maxX, maxY, maxZ;
  octomap->getMetricMin(minX, minY, minZ);
  octomap->getMetricMax(maxX, maxY, maxZ);

  // reset rviz pointcloud classes
  for (std::size_t i = 0; i < max_octree_depth_; ++i)
  {
    point_buf_[i].clear();
    box_size_[i] = octomap->getNodeSize(i + 1);
  }

  size_t pointCount = 0;
  {
    // traverse all leafs in the tree:
    unsigned int treeDepth = std::min<unsigned int>(tree_depth_property_->getInt(), octomap->getTreeDepth());
    for (octomap::TextureOcTree::iterator it = octomap->begin(treeDepth), end = octomap->end(); it != end; ++it)
    {

      if (octomap->isNodeOccupied(*it))
      {

        int render_mode_mask = octree_render_property_->getOptionInt();

        bool display_voxel = false;

        // the left part evaluates to 1 for free voxels and 2 for occupied voxels
        if (((int)octomap->isNodeOccupied(*it) + 1) & render_mode_mask)
        {
          // check if current voxel has neighbors on all sides -> no need to be displayed
          bool allNeighborsFound = true;

          octomap::OcTreeKey key;
          octomap::OcTreeKey nKey = it.getKey();

          for (key[2] = nKey[2] - 1; allNeighborsFound && key[2] <= nKey[2] + 1; ++key[2])
          {
            for (key[1] = nKey[1] - 1; allNeighborsFound && key[1] <= nKey[1] + 1; ++key[1])
            {
              for (key[0] = nKey[0] - 1; allNeighborsFound && key[0] <= nKey[0] + 1; ++key[0])
              {
                if (key != nKey)
                {
                  octomap::OcTreeNode* node = octomap->search(key);

                  // the left part evaluates to 1 for free voxels and 2 for occupied voxels
                  if (!(node && (((int)octomap->isNodeOccupied(node)) + 1) & render_mode_mask))
                  {
                    // we do not have a neighbor => break!
                    allNeighborsFound = false;
                  }
                }
              }
            }
          }

          display_voxel |= !allNeighborsFound;
        }


        if (display_voxel)
        {
          PointCloud::Point newPoint;

          newPoint.position.x = it.getX();
          newPoint.position.y = it.getY();
          newPoint.position.z = it.getZ();

          float cell_probability;
          float cell_texture;
          unsigned int obs = 0;

          OctreeVoxelColorMode octree_color_mode = static_cast<OctreeVoxelColorMode>(octree_coloring_property_->getOptionInt());

          switch (octree_color_mode)
          {
            case OCTOMAP_TEXTURE_COLOR:
              for (auto i=0; i<6; ++i) {
                cell_texture += (float) (it->getFaceValue((octomap::FaceEnum) i) * it->getFaceObservations((octomap::FaceEnum) i));
                obs += it->getFaceObservations((octomap::FaceEnum) i);
              }
              if(obs < 1)
                cell_texture = 0.0;
              else
                cell_texture /= (obs*255.0);
              setIntensity(cell_texture, newPoint);
              //newPoint.setColor(cell_texture, cell_texture, cell_texture);
              break;
            case OCTOMAP_Z_AXIS_COLOR:
              setColor(newPoint.position.z, minZ, maxZ, color_factor_, newPoint);
              break;
            case OCTOMAP_PROBABLILTY_COLOR:
              cell_probability = it->getOccupancy();
              newPoint.setColor((1.0f-cell_probability), cell_probability, 0.0);
              break;
            default:
              break;
          }

          // push to point vectors
          unsigned int depth = it.getDepth();
          point_buf_[depth - 1].push_back(newPoint);

          ++pointCount;
        }
      }
    }
  }

  if (pointCount)
  {
    boost::mutex::scoped_lock lock(mutex_);

    new_points_received_ = true;

    for (size_t i = 0; i < max_octree_depth_; ++i)
      new_points_[i].swap(point_buf_[i]);

  }
  delete octomap;
}

void TextureGridDisplay::updateTreeDepth()
{
}

void TextureGridDisplay::updateOctreeRenderMode()
{
}

void TextureGridDisplay::updateOctreeColorMode()
{
}

void TextureGridDisplay::clear()
{

  boost::mutex::scoped_lock lock(mutex_);

  // reset rviz pointcloud boxes
  for (size_t i = 0; i < cloud_.size(); ++i)
  {
    cloud_[i]->clear();
  }
}

void TextureGridDisplay::update(float wall_dt, float ros_dt)
{
  if (new_points_received_)
  {
    boost::mutex::scoped_lock lock(mutex_);

    for (size_t i = 0; i < max_octree_depth_; ++i)
    {
      double size = box_size_[i];

      cloud_[i]->clear();
      cloud_[i]->setDimensions(size, size, size);

      cloud_[i]->addPoints(&new_points_[i].front(), new_points_[i].size());
      new_points_[i].clear();

    }
    new_points_received_ = false;
  }
}

void TextureGridDisplay::reset()
{
  clear();
  messages_received_ = 0;
  setStatus(StatusProperty::Ok, "Messages", QString("0 binary octomap messages received"));
}

void TextureGridDisplay::updateTopic()
{
  unsubscribe();
  reset();
  subscribe();
  context_->queueRender();
}


} // namespace octomap_rviz_plugin

#include <pluginlib/class_list_macros.h>

PLUGINLIB_EXPORT_CLASS( octomap_rviz_plugin::TextureGridDisplay, rviz::Display)
