#pragma once

#include <madrona/physics.hpp>
#include <madrona/importer.hpp>

#include <madrona/exec_mode.hpp>

namespace madrona {
namespace phys {

class PhysicsLoader {
public:
    struct SourceCollisionPrimitive {
        struct HullInput {
            const imp::SourceMesh *mesh;
        };

        CollisionPrimitive::Type type;
        union {
            CollisionPrimitive::Sphere sphere;
            HullInput hullInput;
        };
    };

    struct SourceCollisionObject {
        Span<const SourceCollisionPrimitive> prims;
        float invMass;
        RigidBodyFrictionData friction;
    };

    struct ImportedRigidBodies {
        struct MergedHullData {
            HeapArray<geometry::HalfEdge> halfEdges;
            HeapArray<uint32_t> faceBaseHEs;
            HeapArray<geometry::Plane> facePlanes;
            HeapArray<math::Vector3> positions;
        } hullData;

        // Per Primitive Data
        HeapArray<CollisionPrimitive> collisionPrimitives;
        HeapArray<math::AABB> primitiveAABBs;

        // Per Object Data
        HeapArray<uint32_t> primOffsets;
        HeapArray<uint32_t> primCounts;
        HeapArray<RigidBodyMetadata> metadatas;
        HeapArray<math::AABB> objectAABBs;
    };

    PhysicsLoader(ExecMode exec_mode, CountT max_objects);
    ~PhysicsLoader();
    PhysicsLoader(PhysicsLoader &&o);

    Optional<ImportedRigidBodies> importRigidBodyData(
        const SourceCollisionObject *collision_objs,
        CountT num_objects,
        bool build_hulls = true);

    CountT loadObjects(const RigidBodyMetadata *metadatas,
                       const math::AABB *obj_aabbs,
                       const uint32_t *prim_offsets,
                       const uint32_t *prim_counts,
                       CountT num_objs,
                       const CollisionPrimitive *primitives,
                       const math::AABB *primitive_aabbs,
                       CountT total_num_primitives,
                       const geometry::HalfEdge *hull_halfedges,
                       CountT total_num_hull_halfedges,
                       const uint32_t *hull_face_base_halfedges,
                       const geometry::Plane *hull_face_planes,
                       CountT total_num_hull_faces,
                       const math::Vector3 *hull_verts,
                       CountT total_num_hull_verts);



    ObjectManager & getObjectManager();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
}
