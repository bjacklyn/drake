#include "drake/geometry/optimization/dev/cspace_free_polytope.h"

#include <gtest/gtest.h>

#include "drake/geometry/optimization/dev/collision_geometry.h"
#include "drake/geometry/optimization/dev/test/c_iris_test_utilities.h"
#include "drake/multibody/rational/rational_forward_kinematics.h"
#include "drake/multibody/rational/rational_forward_kinematics_internal.h"

namespace drake {
namespace geometry {
namespace optimization {
TEST_F(CIrisToyRobotTest, GetCollisionGeometries) {
  const auto link_geometries = GetCollisionGeometries(*plant_, *scene_graph_);
  // Each link has some geometries.
  EXPECT_EQ(link_geometries.size(), plant_->num_bodies());

  auto check_link_geometries =
      [&link_geometries](const multibody::BodyIndex body,
                         const std::unordered_set<geometry::GeometryId>&
                             geometry_ids_expected) {
        auto it = link_geometries.find(body);
        std::unordered_set<geometry::GeometryId> geometry_ids;
        for (const auto& geometry : it->second) {
          EXPECT_EQ(geometry->body_index(), body);
          geometry_ids.emplace(geometry->id());
        }
        EXPECT_EQ(geometry_ids.size(), geometry_ids_expected.size());
        for (const auto id : geometry_ids) {
          EXPECT_GT(geometry_ids_expected.count(id), 0);
        }
      };

  check_link_geometries(plant_->world_body().index(),
                        {world_box_, world_sphere_});
  check_link_geometries(body_indices_[0], {body0_box_, body0_sphere_});
  check_link_geometries(body_indices_[1], {body1_convex_, body1_capsule_});
  check_link_geometries(body_indices_[2], {body2_sphere_, body2_capsule_});
  check_link_geometries(body_indices_[3], {body3_box_, body3_sphere_});
}

TEST_F(CIrisToyRobotTest, CspaceFreePolytopeConstructor) {
  // Test CspaceFreePolytope constructor.
  CspaceFreePolytope dut(plant_, scene_graph_, SeparatingPlaneOrder::kAffine);
  int num_planes_expected = 0;

  const auto link_geometries = GetCollisionGeometries(*plant_, *scene_graph_);
  // Count the expected number of planes by hand.
  num_planes_expected +=
      link_geometries.at(plant_->world_body().index()).size() *
      // Don't include world_body to body0 as there is only a weld joint between
      // them.
      (link_geometries.at(body_indices_[1]).size() +
       link_geometries.at(body_indices_[2]).size() +
       link_geometries.at(body_indices_[3]).size());
  num_planes_expected += link_geometries.at(body_indices_[0]).size() *
                         link_geometries.at(body_indices_[2]).size();
  num_planes_expected += link_geometries.at(body_indices_[1]).size() *
                         link_geometries.at(body_indices_[3]).size();
  num_planes_expected += link_geometries.at(body_indices_[2]).size() *
                         link_geometries.at(body_indices_[3]).size();
  EXPECT_EQ(dut.separating_planes().size(), num_planes_expected);

  const symbolic::Variables s_set{dut.rational_forward_kin().s()};

  for (const auto& [geometry_pair, plane_index] :
       dut.map_geometries_to_separating_planes()) {
    const auto& plane = dut.separating_planes()[plane_index];
    if (plane.positive_side_geometry->id() <
        plane.negative_side_geometry->id()) {
      EXPECT_EQ(geometry_pair.first(), plane.positive_side_geometry->id());
      EXPECT_EQ(geometry_pair.second(), plane.negative_side_geometry->id());
    } else {
      EXPECT_EQ(geometry_pair.first(), plane.negative_side_geometry->id());
      EXPECT_EQ(geometry_pair.second(), plane.positive_side_geometry->id());
    }
    // Check the expressed body.
    EXPECT_EQ(plane.expressed_body,
              multibody::internal::FindBodyInTheMiddleOfChain(
                  *plant_, plane.positive_side_geometry->body_index(),
                  plane.negative_side_geometry->body_index()));
    for (int i = 0; i < 3; ++i) {
      EXPECT_EQ(plane.a(i).TotalDegree(), 1);
      EXPECT_EQ(plane.a(i).indeterminates(), s_set);
    }
    EXPECT_EQ(plane.b.TotalDegree(), 1);
    EXPECT_EQ(plane.b.indeterminates(), s_set);
  }
}

TEST_F(CIrisToyRobotTest, CspaceFreePolytopeGenerateRationals) {
  CspaceFreePolytope dut(plant_, scene_graph_, SeparatingPlaneOrder::kAffine);
  Eigen::Vector3d q_star(0, 0, 0);
  CspaceFreePolytope::FilteredCollsionPairs filtered_collision_pairs = {};
  std::optional<symbolic::Variable> separating_margin{std::nullopt};
  auto ret = dut.GenerateRationals(q_star, filtered_collision_pairs,
                                   separating_margin);
  EXPECT_EQ(ret.size(), dut.separating_planes().size());
  for (const auto& plane_geometries : ret) {
    const auto& plane = dut.separating_planes()[plane_geometries.plane_index];
    if (plane.positive_side_geometry->type() == GeometryType::kCylinder ||
        plane.negative_side_geometry->type() == GeometryType::kCylinder) {
      throw std::runtime_error(
          "Cylinder has not been implemented yet for C-IRIS.");
    } else if (plane.positive_side_geometry->type() == GeometryType::kSphere ||
               plane.positive_side_geometry->type() == GeometryType::kCapsule ||
               plane.negative_side_geometry->type() == GeometryType::kSphere ||
               plane.negative_side_geometry->type() == GeometryType::kCapsule) {
      // If one of the geometry is sphere or capsule, and neither geometry is
      // cylinder, then the unit length vector is just a.
      EXPECT_EQ(plane_geometries.unit_length_vectors.size(), 1);
      for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(plane_geometries.unit_length_vectors[0][i], plane.a(i));
      }
    } else if (plane.positive_side_geometry->type() ==
                   GeometryType::kPolytope &&
               plane.negative_side_geometry->type() ==
                   GeometryType::kPolytope) {
      EXPECT_TRUE(plane_geometries.unit_length_vectors.empty());
    }
    EXPECT_EQ(plane_geometries.rationals.size(),
              plane.positive_side_geometry->num_rationals_per_side() +
                  plane.negative_side_geometry->num_rationals_per_side());
  }

  // Pass a non-empty filtered_collision_pairs with a separating margin.
  filtered_collision_pairs.emplace(
      SortedPair<geometry::GeometryId>(world_box_, body3_sphere_));
  separating_margin.emplace(symbolic::Variable("delta"));
  ret = dut.GenerateRationals(q_star, filtered_collision_pairs,
                              separating_margin);
  EXPECT_EQ(ret.size(), dut.separating_planes().size() - 1);
  for (const auto& plane_geometries : ret) {
    const auto& plane = dut.separating_planes()[plane_geometries.plane_index];
    if (plane.positive_side_geometry->type() != GeometryType::kCylinder &&
        plane.negative_side_geometry->type() != GeometryType::kCylinder) {
      // The unit length vector is always a.
      EXPECT_EQ(plane_geometries.unit_length_vectors.size(), 1);
      EXPECT_EQ(plane_geometries.unit_length_vectors[0].rows(), 3);
      for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(plane_geometries.unit_length_vectors[0](i), plane.a(i));
      }
    }
  }
}
}  // namespace optimization
}  // namespace geometry
}  // namespace drake
