#include <madrona/physics_assets.hpp>
#include <madrona/importer.hpp>

#ifdef MADRONA_CUDA_SUPPORT
#include <madrona/cuda_utils.hpp>
#endif

#include <unordered_map>

namespace madrona::phys {
using namespace geometry;
using namespace math;
using SourceCollisionPrimitive = PhysicsLoader::SourceCollisionPrimitive;
using SourceCollisionObject = PhysicsLoader::SourceCollisionObject;
using ImportedRigidBodies = PhysicsLoader::ImportedRigidBodies;

#ifndef MADRONA_CUDA_SUPPORT
[[noreturn]] static void noCUDA()
{
    FATAL("PhysicsLoader: Not built with CUDA support");
}
#endif

namespace {

struct EditMesh {
    struct HEdge {
        uint32_t next;
        uint32_t prev;
        uint32_t twin;

        uint32_t vert;
        uint32_t face;
    };

    struct Face {
        uint32_t hedge;
        uint32_t next;
        uint32_t prev;

        Plane plane;
    };

    struct Vert {
        Vector3 pos;
        uint32_t next;
        uint32_t prev;
    };

    HEdge *hedges;
    Face *faces;
    Vert *verts;

    uint32_t numHedges;
    uint32_t numFaces;
    uint32_t numVerts;

    uint32_t hedgeFreeHead;
    uint32_t faceFreeHead;
    uint32_t vertFreeHead;
};

struct HullBuildData {
    EditMesh mesh;
    uint32_t *faceConflictLists;
};

}

struct PhysicsLoader::Impl {
    CollisionPrimitive *primitives;
    AABB *primAABBs;

    AABB *objAABBs;
    uint32_t *rigidBodyPrimitiveOffsets;
    uint32_t *rigidBodyPrimitiveCounts;
    RigidBodyMetadata *metadatas;

    CountT curPrimOffset;
    CountT curObjOffset;

    ObjectManager *mgr;
    CountT maxPrims;
    CountT maxObjs;
    ExecMode execMode;

    static Impl * init(ExecMode exec_mode, CountT max_objects)
    {
        constexpr CountT max_prims_per_object = 20;

        size_t num_collision_prim_bytes =
            sizeof(CollisionPrimitive) * max_objects * max_prims_per_object; 

        size_t num_collision_aabb_bytes =
            sizeof(AABB) * max_objects * max_prims_per_object; 

        size_t num_obj_aabb_bytes =
            sizeof(AABB) * max_objects;

        size_t num_offset_bytes =
            sizeof(uint32_t) * max_objects;

        size_t num_count_bytes =
            sizeof(uint32_t) * max_objects;

        size_t num_metadata_bytes =
            sizeof(RigidBodyMetadata) * max_objects;

        CollisionPrimitive *primitives_ptr;
        AABB *prim_aabb_ptr;

        AABB *obj_aabb_ptr;
        uint32_t *offsets_ptr;
        uint32_t *counts_ptr;
        RigidBodyMetadata *metadata_ptr;

        ObjectManager *mgr;

        switch (exec_mode) {
            case ExecMode::CPU: {
                primitives_ptr = (CollisionPrimitive *)malloc(
                    num_collision_prim_bytes);

                prim_aabb_ptr = (AABB *)malloc(
                    num_collision_aabb_bytes);

                obj_aabb_ptr = (AABB *)malloc(num_obj_aabb_bytes);

            offsets_ptr = (uint32_t *)malloc(num_offset_bytes);
            counts_ptr = (uint32_t *)malloc(num_count_bytes);

            metadata_ptr =
                (RigidBodyMetadata *)malloc(num_metadata_bytes);

            mgr = new ObjectManager {
                primitives_ptr,
                prim_aabb_ptr,
                obj_aabb_ptr,
                offsets_ptr,
                counts_ptr,
                metadata_ptr,
            };
        } break;
        case ExecMode::CUDA: {
#ifndef MADRONA_CUDA_SUPPORT
            noCUDA();
#else
            primitives_ptr = (CollisionPrimitive *)cu::allocGPU(
                num_collision_prim_bytes);

            prim_aabb_ptr = (AABB *)cu::allocGPU(
                num_collision_aabb_bytes);

            obj_aabb_ptr = (AABB *)cu::allocGPU(num_obj_aabb_bytes);

            offsets_ptr = (uint32_t *)cu::allocGPU(num_offset_bytes);
            counts_ptr = (uint32_t *)cu::allocGPU(num_count_bytes);

            metadata_ptr =
                (RigidBodyMetadata *)cu::allocGPU(num_metadata_bytes);

            mgr = (ObjectManager *)cu::allocGPU(sizeof(ObjectManager));

            ObjectManager local {
                primitives_ptr,
                prim_aabb_ptr,
                obj_aabb_ptr,
                offsets_ptr,
                counts_ptr,
                metadata_ptr,
            };

            REQ_CUDA(cudaMemcpy(mgr, &local, sizeof(ObjectManager),
                                cudaMemcpyHostToDevice));
#endif
        } break;
        default: MADRONA_UNREACHABLE();
        }

        return new Impl {
            .primitives = primitives_ptr,
            .primAABBs = prim_aabb_ptr,
            .objAABBs = obj_aabb_ptr,
            .rigidBodyPrimitiveOffsets = offsets_ptr,
            .rigidBodyPrimitiveCounts = counts_ptr,
            .metadatas = metadata_ptr,
            .curPrimOffset = 0,
            .curObjOffset = 0,
            .mgr = mgr,
            .maxPrims = max_objects * max_prims_per_object,
            .maxObjs = max_objects,
            .execMode = exec_mode,
        };
    }
};

PhysicsLoader::PhysicsLoader(ExecMode exec_mode, CountT max_objects)
    : impl_(Impl::init(exec_mode, max_objects))
{}

PhysicsLoader::~PhysicsLoader()
{
    if (impl_ == nullptr) {
        return;
    }

    switch (impl_->execMode) {
    case ExecMode::CPU: {
        delete impl_->mgr;
        free(impl_->primitives);
        free(impl_->primAABBs);
        free(impl_->objAABBs);
        free(impl_->rigidBodyPrimitiveOffsets);
        free(impl_->rigidBodyPrimitiveCounts);
        free(impl_->metadatas);
    } break;
    case ExecMode::CUDA: {
#ifndef MADRONA_CUDA_SUPPORT
        noCUDA();
#else
        cu::deallocGPU(impl_->primitives);
        cu::deallocGPU(impl_->primAABBs);
        cu::deallocGPU(impl_->objAABBs);
        cu::deallocGPU(impl_->rigidBodyPrimitiveOffsets);
        cu::deallocGPU(impl_->rigidBodyPrimitiveCounts);
        cu::deallocGPU(impl_->metadatas);
#endif
    } break;
    }
}

PhysicsLoader::PhysicsLoader(PhysicsLoader &&o) = default;

static uint32_t allocMeshHedge(EditMesh &mesh)
{
    uint32_t hedge = mesh.hedgeFreeHead;
    assert(hedge != 0);
    mesh.hedgeFreeHead = mesh.hedges[hedge].next;

    return hedge;
}

static void freeMeshHedge(EditMesh &mesh, uint32_t hedge)
{
    uint32_t old_head = mesh.hedgeFreeHead;
    mesh.hedgeFreeHead = hedge;
    mesh.hedges[hedge].next = old_head;
}

static uint32_t createMeshFace(EditMesh &mesh)
{
    uint32_t face = mesh.faceFreeHead;
    assert(face != 0);
    mesh.faceFreeHead = mesh.faces[face].next;

    uint32_t prev_prev = mesh.faces[0].prev;
    mesh.faces[0].prev = face;
    mesh.faces[prev_prev].next = face;
    
    mesh.faces[face].next = 0;
    mesh.faces[face].prev = prev_prev;

    mesh.numFaces += -1;

    return face;
}

static void deleteMeshFace(EditMesh &mesh, uint32_t face)
{
    uint32_t next = mesh.faces[face].next;
    uint32_t prev = mesh.faces[face].prev;

    mesh.faces[prev].next = next;
    mesh.faces[next].prev = prev;

    uint32_t old_head = mesh.faceFreeHead;
    mesh.faceFreeHead = face;
    mesh.faces[face].next = old_head;
}

static uint32_t allocMeshVert(EditMesh &mesh)
{
    uint32_t vert = mesh.vertFreeHead;
    assert(vert != 0);
    mesh.vertFreeHead = mesh.verts[vert].next;

    return vert;
}

static void freeMeshVert(EditMesh &mesh, uint32_t vert)
{
    uint32_t old_head = mesh.vertFreeHead;
    mesh.vertFreeHead = vert;
    mesh.verts[vert].next = old_head;
}

static uint32_t addVertToMesh(EditMesh &mesh, uint32_t vert)
{
    uint32_t prev_prev = mesh.verts[0].prev;
    mesh.verts[0].prev = vert;
    mesh.verts[prev_prev].next = vert;
    
    mesh.verts[vert].next = 0;
    mesh.verts[vert].prev = prev_prev;

    mesh.numVerts += 1;

    return vert;
}

static void removeVertFromMesh(EditMesh &mesh, uint32_t vert)
{
    uint32_t next = mesh.verts[vert].next;
    uint32_t prev = mesh.verts[vert].prev;

    mesh.verts[prev].next = next;
    mesh.verts[next].prev = prev;

    mesh.numVerts -= 1;
}

static uint32_t addConflictVert(HullBuildData &hull_data,
                                uint32_t face,
                                Vector3 pos)
{
    auto &mesh = hull_data.mesh;
    uint32_t vert = allocMeshVert(mesh);

    uint32_t next = hull_data.faceConflictLists[face];

    hull_data.faceConflictLists[face] = vert;
    mesh.verts[vert].next = next;
    mesh.verts[vert].prev = 0;

    if (next != 0) {
        mesh.verts[next].prev = vert;
    }

    mesh.verts[vert].pos = pos;

    return vert;
}

static void removeConflictVert(HullBuildData &hull_data,
                               uint32_t face,
                               uint32_t vert)
{
    auto &mesh = hull_data.mesh;

    uint32_t next = mesh.verts[vert].next;
    uint32_t prev = mesh.verts[vert].prev;

    if (prev == 0) {
        hull_data.faceConflictLists[face] = next;
    } else {
        mesh.verts[prev].next = next;
    }

    if (next != 0) {
        mesh.verts[next].prev = prev;
    }
}

// Gregorious, Implementing QuickHull, GDC 2014, Slide 77
static float computePlaneEpsilon(Span<const Vector3> verts)
{
    AABB aabb = AABB::invalid();

    for (Vector3 v : verts) {
        aabb.expand(v);
    }

    Vector3 diff = aabb.pMax - aabb.pMin;

    return 3.f * (diff.x + diff.y + diff.z) * FLT_EPSILON;
}

// RTCD 12.4.2
template <typename Fn>
static Plane computeNewellPlaneImpl(Fn &&iter_verts)
{
    Vector3 centroid { 0, 0, 0 };
    Vector3 n { 0, 0, 0 };

    CountT num_verts = 0;
    // Compute normal as being proportional to projected areas of polygon
    // onto the yz, xz, and xy planes. Also compute centroid as
    // representative point on the plane
    iter_verts([&centroid, &n, &num_verts](Vector3 vi, Vector3 vj) {
        n.x += (vi.y - vj.y) * (vi.z + vj.z); // projection on yz
        n.y += (vi.z - vj.z) * (vi.x + vj.x); // projection on xz
        n.z += (vi.x - vj.x) * (vi.y + vj.y); // projection on xy

        centroid += vj;
        num_verts += 1;
    });

    assert(num_verts != 0);

    centroid /= num_verts;

    n = normalize(n);
    return Plane {
        .normal = n,
        .d = dot(centroid, n),
    };
}

static Plane computeNewellPlane(const Vector3 *verts,
                                Span<const uint32_t> indices)
{
    return computeNewellPlaneImpl([verts, indices](auto &&fn) {
        for (CountT i = indices.size() - 1, j = 0; j < indices.size();
            i = j, j++) {
            Vector3 vi = verts[indices[i]];
            Vector3 vj = verts[indices[j]];

            fn(vi, vj);
        }
    });
}

static Plane computeNewellPlane(EditMesh &mesh, uint32_t face)
{
    return computeNewellPlaneImpl([mesh, face](auto &&fn) {
        uint32_t start_hedge_idx = mesh.faces[face].hedge;
        uint32_t cur_hedge_idx = start_hedge_idx;

        do {
            const EditMesh::HEdge &cur_hedge = mesh.hedges[cur_hedge_idx];
            uint32_t next_hedge_idx = cur_hedge.next;
            const EditMesh::HEdge &next_hedge = mesh.hedges[next_hedge_idx];

            uint32_t i = cur_hedge.vert;
            uint32_t j = next_hedge.vert;

            fn(mesh.verts[i].pos, mesh.verts[j].pos);

            cur_hedge_idx = next_hedge_idx;
        } while (cur_hedge_idx != start_hedge_idx);
    });
}

static float distToPlane(Plane plane, Vector3 v)
{
    return v.dot(plane.normal) - plane.d;
}

static HullBuildData allocBuildData(const CountT N)
{
    // + 1 for fake starting point for linked lists
    const CountT max_num_verts = N + 1;
    // Num edges = 3N - 6. Doubled for half edges, doubled for horizon
    const CountT max_num_hedges = 4 * (3 * N - 6) + 1;
    // Num edges = 2N - 4. Doubled for horizon
    const CountT max_num_faces = 2 * (2 * N - 4) + 1;

    const auto buffer_sizes = std::to_array({
        int64_t(sizeof(EditMesh::HEdge) * max_num_hedges), // hedges
        int64_t(sizeof(EditMesh::Face) * max_num_faces), // faces
        int64_t(sizeof(EditMesh::Vert) * max_num_verts), // verts
        int64_t(sizeof(uint32_t) * max_num_faces), // faceConflictLists
    });

    constexpr CountT sub_buffer_alignment = 128;

    int64_t buffer_offsets[buffer_sizes.size() - 1];
    int64_t total_bytes = utils::computeBufferOffsets(
        buffer_sizes, buffer_offsets, sub_buffer_alignment);

    char *buf_base = (char *)malloc(total_bytes);

    EditMesh mesh {
        .hedges = (EditMesh::HEdge *)(buf_base),
        .faces = (EditMesh::Face *)(buf_base + buffer_offsets[0]),
        .verts = (EditMesh::Vert *)(buf_base + buffer_offsets[1]),
        .numHedges = 0,
        .numFaces = 0,
        .numVerts = 0,
        .hedgeFreeHead = 1,
        .faceFreeHead = 1,
        .vertFreeHead = 1,
    };

    // Setup free lists
    for (CountT i = 1; i < max_num_hedges; i++) {
        mesh.hedges[i].next = uint32_t(i + 1);
    }
    mesh.hedges[max_num_hedges].next = 0;

    for (CountT i = 1; i < max_num_faces; i++) {
        mesh.faces[i].next = uint32_t(i + 1);
    }
    mesh.faces[max_num_faces].next = 0;

    for (CountT i = 1; i < max_num_verts; i++) {
        mesh.verts[i].next = uint32_t(i + 1);
    }
    mesh.verts[max_num_verts].next = 0;
    
    // Elem 0 is fake head / tail to avoid special cases
    mesh.hedges[0].next = 0;
    mesh.hedges[0].prev = 0;

    mesh.faces[0].next = 0;
    mesh.faces[0].prev = 0;

    mesh.verts[0].next = 0;
    mesh.verts[0].prev = 0;

    uint32_t *face_conflict_lists = (uint32_t *)(buf_base + buffer_offsets[2]);
    for (CountT i = 0; i < max_num_faces; i++) {
        face_conflict_lists[i] = 0;
    }

    return HullBuildData {
        .mesh = mesh,
        .faceConflictLists = face_conflict_lists,
    };
}

static bool initHullTetrahedron(EditMesh &mesh,
                                Span<const Vector3> verts,
                                float epsilon,
                                uint32_t *tet_fids,
                                Plane *tet_face_planes)
{
    // Choose the initial 4 points for the hull
    Vector3 v0 = verts[0];

    Vector3 v1, e1;
    float max_v1_dist = -FLT_MAX;
    for (CountT i = 1; i < verts.size(); i++) {
        Vector3 v = verts[i];
        Vector3 e = v - v0;
        float e_len = e.length();
        if (e_len > max_v1_dist) {
            v1 = v;
            e1 = e;
            max_v1_dist = e_len;
        }
    }

    if (max_v1_dist < epsilon) {
        return false;
    }

    Vector3 v2, e2;
    float max_v2_area = -FLT_MAX;
    for (CountT i = 1; i < verts.size(); i++) {
        Vector3 v = verts[i];
        Vector3 e = v - v0;

        float area = cross(e, e1).length();

        if (area > max_v2_area) {
            v2 = v;
            e2 = e;
            max_v2_area = area;
        }
    }

    if (max_v2_area < epsilon) {
        return false;
    }

    Vector3 v3;
    float max_v3_det = -FLT_MAX;
    for (CountT i = 1; i < verts.size(); i++) {
        Vector3 v = verts[i];
        Vector3 e = v - v0;

        Mat3x3 vol_mat {{ e1, e2, e }};
        float det = vol_mat.determinant();

        if (det > max_v3_det) {
            v3 = v;
            max_v3_det = det;
        }
    }

    if (max_v3_det < epsilon) {
        return false;
    }

    // Setup initial halfedge mesh
    uint32_t vids[4];
    vids[0] = allocMeshVert(mesh);
    vids[1] = allocMeshVert(mesh);
    vids[2] = allocMeshVert(mesh);
    vids[3] = allocMeshVert(mesh);
    addVertToMesh(mesh, vids[0]);
    addVertToMesh(mesh, vids[1]);
    addVertToMesh(mesh, vids[2]);
    addVertToMesh(mesh, vids[3]);
    mesh.verts[vids[0]].pos = v0;
    mesh.verts[vids[1]].pos = v1;
    mesh.verts[vids[2]].pos = v2;
    mesh.verts[vids[3]].pos = v3;

    // Face 0:
    //   he0: 3 => 2, he1: 2 => 1, he2: 1 => 3,
    // Face 1:
    //   he3: 2 => 3, he4: 3 => 0, he5: 0 => 2,
    // Face 2:
    //   he6: 1 => 0, he7: 0 => 3, he8: 3 => 1,
    // Face 3:
    //   he9: 0 => 1, he10: 1 => 2, he11: 2 => 0,
    uint32_t eids[12];
    const uint32_t face_vert_indices[] = {
        3, 2, 1,
        2, 3, 0,
        1, 0, 3,
        0, 1, 2
    };

    const uint32_t twin_hedge_indices[] = {
        3, 10, 8,
        0, 7, 11,
        9, 4, 2,
        6, 1, 5,
    };

    // Allocate half edges
#pragma unroll
    for (CountT i = 0; i < 12; i++) {
        eids[i] = allocMeshHedge(mesh);
    }

    // Create faces and create halfedges
    for (CountT i = 0; i < 4; i++) {
        const uint32_t base_hedge_offset = i * 3;
        uint32_t fid = tet_fids[i] = createMeshFace(mesh);

#pragma unroll
        for (CountT j = 0; j < 3; j++) {
            const uint32_t cur_hedge_offset = base_hedge_offset + j;
            const uint32_t next_hedge_offset = base_hedge_offset + ((j + 1) % 3);
            const uint32_t prev_hedge_offset = base_hedge_offset + ((j + 2) % 3);

            uint32_t vid = vids[face_vert_indices[cur_hedge_offset]];
            uint32_t cur_eid = eids[cur_hedge_offset];

            mesh.hedges[cur_eid].face = fid;
            mesh.hedges[cur_eid].vert = vid;

            mesh.hedges[cur_eid].next = eids[next_hedge_offset];
            mesh.hedges[cur_eid].prev = eids[prev_hedge_offset];

            mesh.hedges[cur_eid].twin = twin_hedge_indices[cur_hedge_offset];
        }

        mesh.faces[fid].hedge = eids[base_hedge_offset];

        Plane face_plane = computeNewellPlane(mesh, fid);
        mesh.faces[fid].plane = tet_face_planes[i] = face_plane;
    }

    return true;
}

static void freeBuildData(HullBuildData &hull_data)
{
    free(hull_data.mesh.hedges);
}

static bool initHullBuild(Span<const Vector3> verts, HullBuildData *out)
{
    if (verts.size() < 4) {
        return false;
    }

    *out = allocBuildData(verts.size());
    EditMesh &mesh = out->mesh;

    float epsilon = computePlaneEpsilon(verts);

    uint32_t tet_face_ids[4];
    Plane tet_face_planes[4];
    // FIXME: choose proper epsilon not just plane epsilon
    bool tet_success = initHullTetrahedron(mesh, verts, epsilon, tet_face_ids,
                                           tet_face_planes);
    if (!tet_success) {
        freeBuildData(*out);
        return false;
    }

    // Initial vertex binning
    for (Vector3 pos : verts) {
        float closest_plane_dist = FLT_MAX;
        CountT closest_plane_idx = -1;
        for (CountT i = 0; i < 4; i++) {
            Plane cur_plane = tet_face_planes[i];
            float dist = distToPlane(cur_plane, pos);

            if (dist > epsilon) {
                if (dist < closest_plane_dist) {
                    closest_plane_idx = i;
                    closest_plane_dist = dist;
                }
            }
        }

        // This is an internal vertex
        if (closest_plane_idx == -1) {
            continue;
        }

        addConflictVert(*out, tet_face_ids[closest_plane_idx], pos);
    }

    return true;
}

static void quickhullBuild(HullBuildData &build_data)
{
    auto &mesh = build_data.mesh;
    // FIXME
    (void)mesh;
    (void)freeMeshHedge;
    (void)deleteMeshFace;
    (void)freeMeshVert;
    (void)removeVertFromMesh;
    (void)removeConflictVert;
}

static HalfEdgeMesh editMeshToRuntimeMesh(EditMesh &edit_mesh)
{
    uint32_t *hedge_remap = (uint32_t *)malloc(
        sizeof(uint32_t) * edit_mesh.numHedges);

    for (CountT i = 0; i < edit_mesh.numHedges; i++) {
        hedge_remap[i] = 0xFFFF'FFFF;
    }

    uint32_t *face_remap = (uint32_t *)malloc(
        sizeof(uint32_t) * edit_mesh.numFaces);

    uint32_t *vert_remap = (uint32_t *)malloc(
        sizeof(uint32_t) * edit_mesh.numVerts);

    CountT num_new_hedges = 0;
    for (uint32_t orig_eid = edit_mesh.hedges[0].next;
         orig_eid != 0; orig_eid = edit_mesh.hedges[orig_eid].next) {
        if (hedge_remap[orig_eid] != 0xFFFF'FFFF) {
            continue;
        }

        const EditMesh::HEdge &cur_hedge = edit_mesh.hedges[orig_eid];
        uint32_t twin_eid = cur_hedge.twin;
        assert(hedge_remap[twin_eid] = 0xFFFF'FFFF);

        hedge_remap[orig_eid] = num_new_hedges;
        hedge_remap[twin_eid] = num_new_hedges + 1;
        num_new_hedges += 2;
    }

    CountT num_new_verts = 0;
    for (uint32_t orig_vid = edit_mesh.verts[0].next;
         orig_vid != 0; orig_vid = edit_mesh.verts[orig_vid].next) {
        vert_remap[orig_vid] = num_new_verts++;
    }

    CountT num_new_faces = 0;
    for (uint32_t orig_fid = edit_mesh.faces[0].next;
         orig_fid != 0; orig_fid = edit_mesh.faces[orig_fid].next) {
        face_remap[orig_fid] = num_new_faces++;
    }

    auto hedges_out = (HalfEdge *)malloc(sizeof(HalfEdge) * num_new_hedges);
    auto face_base_hedges_out =
        (uint32_t *)malloc(sizeof(uint32_t) * num_new_faces);
    auto face_planes_out = (Plane *)malloc(
        sizeof(Plane) * num_new_faces);
    auto positions_out =
        (Vector3 *)malloc(sizeof(Vector3) * num_new_verts);

    for (uint32_t orig_eid = edit_mesh.hedges[0].next;
         orig_eid != 0; orig_eid = edit_mesh.hedges[orig_eid].next) {
        const EditMesh::HEdge &orig_hedge = edit_mesh.hedges[orig_eid];

        hedges_out[hedge_remap[orig_eid]] = HalfEdge {
            .next = hedge_remap[orig_hedge.next],
            .rootVertex = vert_remap[orig_hedge.vert],
            .face = face_remap[orig_hedge.face],
        };
    }

    for (uint32_t orig_vid = edit_mesh.verts[0].next;
         orig_vid != 0; orig_vid = edit_mesh.verts[orig_vid].next) {
        const EditMesh::Vert &orig_vert = edit_mesh.verts[orig_vid];
        positions_out[vert_remap[orig_vid]] = orig_vert.pos;
    }

    for (uint32_t orig_fid = edit_mesh.faces[0].next;
         orig_fid != 0; orig_fid = edit_mesh.faces[orig_fid].next) {
        const EditMesh::Face &orig_face = edit_mesh.faces[orig_fid];

        uint32_t new_face_idx = face_remap[orig_fid];

        face_base_hedges_out[new_face_idx] = hedge_remap[orig_face.hedge];
        face_planes_out[new_face_idx] = orig_face.plane;
    }

    return HalfEdgeMesh {
        .halfEdges = hedges_out,
        .faceBaseHalfEdges = face_base_hedges_out,
        .facePlanes = face_planes_out,
        .vertices = positions_out,
        .numHalfEdges = uint32_t(num_new_hedges),
        .numFaces = uint32_t(num_new_faces),
        .numVertices = uint32_t(num_new_verts),
    };
}

// FIXME: better allocation / ownership strategy
static void freeHalfEdgeMesh(HalfEdgeMesh &mesh)
{
    free(mesh.halfEdges);
    free(mesh.faceBaseHalfEdges);
    free(mesh.facePlanes);
    free(mesh.vertices);
}

static inline HalfEdgeMesh buildHalfEdgeMesh(
    const Vector3 *positions,
    CountT num_vertices, 
    const uint32_t *indices,
    const uint32_t *face_counts,
    const Plane *face_planes,
    CountT num_faces)
{
    auto numFaceVerts = [face_counts](CountT face_idx) {
        if (face_counts == nullptr) {
            return 3_u32;
        } else {
            return face_counts[face_idx];
        }
    };

    using namespace madrona::math;

    uint32_t num_hedges = 0;
    for (CountT face_idx = 0; face_idx < num_faces; face_idx++) {
        num_hedges += numFaceVerts(face_idx);
    }

    assert(num_hedges % 2 == 0);

    // We already know how many polygons there are
    auto face_base_hedges_out =
        (uint32_t *)malloc(sizeof(uint32_t) * num_faces);
    auto hedges_out = (HalfEdge *)malloc(sizeof(HalfEdge) * num_hedges);
    auto face_planes_out = (Plane *)malloc(sizeof(Plane) * num_faces);
    auto positions_out =
        (Vector3 *)malloc(sizeof(Vector3) * num_vertices);

    memcpy(face_planes_out, face_planes, sizeof(Plane) * num_faces);
    memcpy(positions_out, positions, sizeof(Vector3) * num_vertices);

    std::unordered_map<uint64_t, uint32_t> edge_to_hedge;

    auto makeEdgeID = [](uint32_t a_idx, uint32_t b_idx) {
        return ((uint64_t)a_idx << 32) | (uint64_t)b_idx;
    };

    CountT num_assigned_hedges = 0;
    const uint32_t *cur_face_indices = indices;
    for (CountT face_idx = 0; face_idx < num_faces; face_idx++) {
        CountT num_face_vertices = numFaceVerts(face_idx);
        for (CountT vert_offset = 0; vert_offset < num_face_vertices;
             vert_offset++) {
            uint32_t a_idx = cur_face_indices[vert_offset];
            uint32_t b_idx = cur_face_indices[
                (vert_offset + 1) % num_face_vertices];

            uint64_t cur_edge_id = makeEdgeID(a_idx, b_idx);

            auto cur_edge_lookup = edge_to_hedge.find(cur_edge_id);
            if (cur_edge_lookup == edge_to_hedge.end()) {
                uint32_t cur_hedge_id = num_assigned_hedges;
                uint32_t twin_hedge_id = num_assigned_hedges + 1;

                num_assigned_hedges += 2;

                uint64_t twin_edge_id = makeEdgeID(b_idx, a_idx);

                auto [new_edge_iter, cur_inserted] =
                    edge_to_hedge.emplace(cur_edge_id, cur_hedge_id);
                assert(cur_inserted);

                auto [new_twin_iter, twin_inserted] =
                    edge_to_hedge.emplace(twin_edge_id, twin_hedge_id);
                assert(twin_inserted);

                cur_edge_lookup = new_edge_iter;
            }

            uint32_t hedge_idx = cur_edge_lookup->second;
            if (vert_offset == 0) {
                face_base_hedges_out[face_idx] = hedge_idx;
            }

            uint32_t c_idx = cur_face_indices[
                (vert_offset + 2) % num_face_vertices];

            auto next_edge_id = makeEdgeID(b_idx, c_idx);
            auto next_edge_lookup = edge_to_hedge.find(next_edge_id);

            // If next doesn't exist yet, we can assume it will be the next
            // allocated half edge
            uint32_t next_hedge_idx = next_edge_lookup == edge_to_hedge.end() ?
                num_assigned_hedges : next_edge_lookup->second;

            hedges_out[hedge_idx] = HalfEdge {
                .next = next_hedge_idx,
                .rootVertex = a_idx,
                .face = uint32_t(face_idx),
            };
        }

        cur_face_indices += num_face_vertices;
    }

    assert(num_assigned_hedges == num_hedges);

    return HalfEdgeMesh {
        .halfEdges = hedges_out,
        .faceBaseHalfEdges = face_base_hedges_out,
        .facePlanes = face_planes_out,
        .vertices = positions_out,
        .numHalfEdges = uint32_t(num_hedges),
        .numFaces = uint32_t(num_faces),
        .numVertices = uint32_t(num_vertices),
    };
}

namespace {
struct MassProperties {
    Diag3x3 inertiaTensor;
    Vector3 centerOfMass;
    Quat toDiagonal;
};
}

// Below functions diagonalize the inertia tensor and compute the necessary
// rotation for diagonalization as a quaternion.
// Source: Computing the Singular Value Decomposition of 3x3 matrices with
// minimal branching and  elementary floating point operations.
// McAdams et al 2011

// McAdams Algorithm 2:
static std::pair<float, float> approxGivensQuaternion(Symmetric3x3 m)
{

    constexpr float gamma = 5.82842712474619f;
    constexpr float c_star = 0.9238795325112867f;
    constexpr float s_star = 0.3826834323650898f;

    float a11 = m.diag[0], a12 = m.off[0], a22 = m.diag[1];

    float ch = 2.f * (a11 - a22);
    float sh = a12;

    float sh2 = sh * sh;

    // This isn't in the paper, but basically want to make sure the quaternion
    // performs an identity rotation for already diagonal matrices
    if (sh2 < 1e-20f) {
        return { 1.f, 0.f };
    }

    float ch2 = ch * ch;

    bool b = (gamma * sh2) < ch2;

    float omega = rsqrtApprox(ch2 + sh2);

    ch = b ? (omega * ch) : c_star;
    sh = b ? (omega * sh) : s_star;

    return { ch, sh };
}

// Equation 12: approxGivensQuaternion returns an unscaled quaternion,
// need to rescale
static Symmetric3x3 jacobiIterConjugation(Symmetric3x3 m, float ch, float sh)
{
    float ch2 = ch * ch;
    float sh2 = sh * sh;
    float q_scale = ch2 + sh2;

    float q11 = (ch2 - sh2) / q_scale;
    float q12 = (-2.f * sh * ch) / q_scale;
    float q21 = (2.f * sh * ch) / q_scale;
    float q22 = (ch2 - sh2) / q_scale;

    // Output = Q^T * m * Q. Given above values for Q, direct solution to
    // compute output (given 0s for other terms) computed using SymPy

    auto [m11, m22, m33] = m.diag;
    auto [m12, m13, m23] = m.off;

    float m11q11_m12q21 = m11 * q11 + m12 * q21;
    float m11q12_m12q22 = m11 * q12 + m12 * q22;

    float m12q11_m22q21 = m12 * q11 + m22 * q21;
    float m12q12_m22q22 = m12 * q12 + m22 * q22;

    return Symmetric3x3 {
        .diag = {
            q11 * m11q11_m12q21 + q21 * m12q11_m22q21,
            q12 * m11q12_m12q22 + q22 * m12q12_m22q22,
            m33,
        },
        .off = {
            q12 * m11q11_m12q21 + q22 * m12q11_m22q21,
            m13 * q11 + m23 * q21,
            m13 * q12 + m23 * q22,
        },
    };
}

// Inertia tensor is symmetric positive semi definite, so we only need to
// perform the symmetric eigenanalysis part of the algorithm.
//
// Jacobi order: (p, q) = (1, 2), (1, 3), (2, 3), (1, 2), (1, 3) ...
// Pairs: (1, 2) = (a11, a22, a12); (1, 3) = (a11, a33, a13);
//        (2, 3) = (a22, a33, a23)
static void diagonalizeInertiaTensor(const Symmetric3x3 &m,
                                     Diag3x3 *out_diag,
                                     Quat *out_rot)
{
    using namespace math;

    constexpr CountT num_jacobi_iters = 8;

    Symmetric3x3 cur_mat = m;
    Quat accumulated_rot { 1, 0, 0, 0 };
    for (CountT i = 0; i < num_jacobi_iters; i++) {
#if 0
        printf("Cur:\n"
               "%f %f %f\n"
               "%f %f %f\n"
               "%f %f %f\n",
               cur_mat[0].x, cur_mat[1].x, cur_mat[2].x,
               cur_mat[0].y, cur_mat[1].y, cur_mat[2].y,
               cur_mat[0].z, cur_mat[1].z, cur_mat[2].z);
#endif

        auto [ch1, sh1] = approxGivensQuaternion(cur_mat);
        cur_mat = jacobiIterConjugation(cur_mat, ch1, sh1);

        // Rearrange matrix so unrotated elements are in upper left corner
        std::swap(cur_mat.diag[1], cur_mat.diag[2]);
        std::swap(cur_mat.off[0], cur_mat.off[1]);

        auto [ch2, sh2] = approxGivensQuaternion(cur_mat);
        cur_mat = jacobiIterConjugation(cur_mat, ch2, sh2);

        std::swap(cur_mat.diag[0], cur_mat.diag[2]);
        std::swap(cur_mat.off[0], cur_mat.off[2]);

        auto [ch3, sh3] = approxGivensQuaternion(cur_mat);
        cur_mat = jacobiIterConjugation(cur_mat, ch3, sh3);

        cur_mat = Symmetric3x3 {
            .diag = { cur_mat.diag[2], cur_mat.diag[0], cur_mat.diag[1]  },
            .off = { cur_mat.off[1], cur_mat.off[2], cur_mat.off[0] },
        };

        // This could be optimized
        accumulated_rot = Quat { ch1, 0, 0, sh1 } * Quat { ch2, 0, sh2, 0 } *
            Quat { ch3, sh3, 0, 0 } * accumulated_rot;
    }

    Quat final_rot = accumulated_rot.normalize();

    // Compute the diagonal (all other terms should be ~0)
    {
        Mat3x3 q = Mat3x3::fromQuat(final_rot);

        auto [m11, m22, m33] = m.diag;
        auto [m12, m13, m23] = m.off;

        auto [q11, q21, q31] = q[0];
        auto [q12, q22, q32] = q[1];
        auto [q13, q23, q33] = q[2];

        out_diag->d0 = q11 * (m11 * q11 + m12 * q21 + m13 * q31) +
                       q21 * (m12 * q11 + m22 * q21 + m23 * q31) +
                       q31 * (m13 * q11 + m23 * q21 + m33 * q31);

        out_diag->d1 = q12 * (m11 * q12 + m12 * q22 + m13 * q32) +
                       q22 * (m12 * q12 + m22 * q22 + m23 * q32) +
                       q32 * (m13 * q12 + m23 * q22 + m33 * q32);
        
        out_diag->d2 = q13 * (m11 * q13 + m12 * q23 + m13 * q33) +
                       q23 * (m12 * q13 + m22 * q23 + m23 * q33) +
                       q33 * (m13 * q13 + m23 * q23 + m33 * q33);
    }

    *out_rot = final_rot;
}

// http://number-none.com/blow/inertia/
static inline MassProperties computeMassProperties(
    const SourceCollisionObject &src_obj)
{
    using namespace math;
    const Symmetric3x3 C_canonical {
        .diag = Vector3 { 1.f / 60.f, 1.f / 60.f, 1.f / 60.f },
        .off = Vector3 { 1.f / 120.f, 1.f / 120.f, 1.f / 120.f },
    };
    constexpr float density = 1.f;

    Symmetric3x3 C_total {
        .diag = Vector3::zero(),
        .off = Vector3::zero(),
    };

    float m_total = 0;
    Vector3 x_total = Vector3::zero();

    auto processTet = [&](Vector3 v1, Vector3 v2, Vector3 v3) {
        // Reference point is (0, 0, 0) so tet edges are just the vertex
        // positions
        Vector3 e1 = v1;
        Vector3 e2 = v2;
        Vector3 e3 = v3;

        // Covariance matrix
        Mat3x3 A {{ e1, e2, e3 }};
        float det_A = A.determinant();
        Symmetric3x3 C = det_A * Symmetric3x3::AXAT(A, C_canonical);

        // Mass
        float volume = 1.f / 6.f * det_A;
        float m = volume * density;

        Vector3 x = 0.25f * e1 + 0.25f * e2 + 0.25f * e3;

        // Accumulate tetrahedron properties
        float old_m_total = m_total;
        m_total += m;
        x_total = (x * m + x_total * old_m_total) / m_total;

        C_total += C;
    };

    for (const SourceCollisionPrimitive &prim : src_obj.prims) {
        if (prim.type == CollisionPrimitive::Type::Sphere) {
            // FIXME: need to allow offset for primitives
            m_total += 1.f;

            float r = prim.sphere.radius;

            // Note that we need the sphere's covariance matrix,
            // not the inertia tensor (hence 1/2 standard formulas)
            float v = 1.f / 5.f * r * r;
            C_total += Symmetric3x3 {
                .diag = Vector3 { v, v, v },
                .off = Vector3::zero(),
            };
            continue;
        } else if (prim.type == CollisionPrimitive::Type::Plane) {
            // Plane has infinite mass / inertia. The rest of the
            // object must as well

            return MassProperties {
                Diag3x3::uniform(INFINITY),
                Vector3::zero(),
                Quat { 1, 0, 0, 0 },
            };
        }

        // Hull primitive
 
        const imp::SourceMesh &src_mesh = *prim.hullInput.mesh;

        const uint32_t *cur_indices = src_mesh.indices;
        for (CountT face_idx = 0; face_idx < (CountT)src_mesh.numFaces;
             face_idx++) {
            CountT num_face_vertices = src_mesh.faceCounts ?
                src_mesh.faceCounts[face_idx] : 3;

            uint32_t idx1 = cur_indices[0];
            Vector3 v1 = src_mesh.positions[idx1];
            for (CountT i = 1; i < num_face_vertices - 1; i++) {
                uint32_t idx2 = cur_indices[i];
                Vector3 v2 = src_mesh.positions[idx2];

                uint32_t idx3 = cur_indices[i + 1];
                Vector3 v3 = src_mesh.positions[idx3];

                processTet(v1, v2, v3);
            }

            cur_indices += num_face_vertices;
        }
    }

    auto translateCovariance = [](const Symmetric3x3 &C,
                                  Vector3 x, // COM
                                  float m,
                                  Vector3 delta_x) {
        Symmetric3x3 delta_xxT_plus_xdeltaxT {
            .diag = 2.f * Vector3 {
                x.x * delta_x.x,
                x.y * delta_x.y,
                x.z * delta_x.z,
            },
            .off = Vector3 {
                x.x * delta_x.y + x.y * delta_x.x,
                x.x * delta_x.z + x.z * delta_x.x,
                x.y * delta_x.z + x.z * delta_x.y,
            },
        };

        Symmetric3x3 delta_xdelta_xT = Symmetric3x3::vvT(delta_x);
        return C + m * (delta_xxT_plus_xdeltaxT + delta_xdelta_xT);
    };
    
    // Move accumulated covariance matrix to center of mass
    C_total = translateCovariance(C_total, x_total, m_total, -x_total);

    float tr_C = C_total[0][0] + C_total[1][1] + C_total[2][2];
    const Symmetric3x3 tr_C_diag {
        .diag = Vector3 { tr_C, tr_C, tr_C },
        .off = Vector3::zero(),
    };

    // Compute inertia tensor 
    Symmetric3x3 inertia_tensor = tr_C_diag - C_total;

    // Rescale total mass of inertia tensor (unless infinity)
    float inv_mass = 1.f / m_total;
    inertia_tensor *= inv_mass;

    printf("Inertia Tensor:\n"
           "%f %f %f\n"
           "%f %f %f\n"
           "%f %f %f\n"
           "COM: (%f %f %f) mass: %f\n", 
           inertia_tensor[0].x,
           inertia_tensor[1].x,
           inertia_tensor[2].x,
           inertia_tensor[0].y,
           inertia_tensor[1].y,
           inertia_tensor[2].y,
           inertia_tensor[0].z,
           inertia_tensor[1].z,
           inertia_tensor[2].z,
           x_total.x,
           x_total.y,
           x_total.z,
           m_total
           );

    Diag3x3 diag_inertia;
    Quat rot_to_diag;
    diagonalizeInertiaTensor(inertia_tensor, &diag_inertia, &rot_to_diag);

    printf("Diag Inertia tensor: (%f %f %f) rot: (%f %f %f %f)\n\n",
           diag_inertia.d0, diag_inertia.d1, diag_inertia.d2,
           rot_to_diag.w,
           rot_to_diag.x,
           rot_to_diag.y,
           rot_to_diag.z);

    return MassProperties {
        diag_inertia,
        x_total,
        rot_to_diag,
    };
}

static inline RigidBodyMassData toMassData(const MassProperties &mass_props,
                                           float inv_m)
{
    Diag3x3 inv_inertia = inv_m / mass_props.inertiaTensor;

    return {
        .invMass = inv_m,
        .invInertiaTensor = Vector3 { // FIXME
            inv_inertia.d0,
            inv_inertia.d1,
            inv_inertia.d2,
        },
        .toCenterOfMass = mass_props.centerOfMass,
        .toInteriaFrame = mass_props.toDiagonal,
    };
}

static void setupSpherePrimitive(const SourceCollisionPrimitive &src_prim,
                                 CollisionPrimitive *out_prim,
                                 AABB *out_aabb)
{
    out_prim->sphere = src_prim.sphere;

    const float r = src_prim.sphere.radius;

    *out_aabb = AABB {
        .pMin = { -r, -r, -r },
        .pMax = { r, r, r },
    };
}

static void setupPlanePrimitive(const SourceCollisionPrimitive &,
                                CollisionPrimitive *out_prim,
                                AABB *out_aabb)
{
    out_prim->plane = CollisionPrimitive::Plane {};
    
    *out_aabb = AABB {
        .pMin = { -FLT_MAX, -FLT_MAX, -FLT_MAX },
        .pMax = { FLT_MAX, FLT_MAX, 0 },
    };
}

static bool setupHullPrimitive(const SourceCollisionPrimitive &src_prim,
                               CollisionPrimitive *out_prim,
                               AABB *out_aabb,
                               CountT *total_num_halfedges,
                               CountT *total_num_faces,
                               CountT *total_num_vertices,
                               bool build_hull)
{
    const imp::SourceMesh *src_mesh = src_prim.hullInput.mesh;

    HalfEdgeMesh final_he_mesh;
    if (!build_hull) {
        // Just assume the input geometry is a convex hull with coplanar faces
        // merged

        HeapArray<Plane> hull_face_planes(src_mesh->numFaces);

        const uint32_t *cur_face_indices = src_mesh->indices;
        for (CountT face_idx = 0; face_idx < hull_face_planes.size();
             face_idx++) {
            uint32_t num_face_indices =
                src_mesh->faceCounts ? src_mesh->faceCounts[face_idx] : 3;

            Plane face_plane = computeNewellPlane(src_mesh->positions, 
                Span(cur_face_indices, num_face_indices));

            hull_face_planes[face_idx] = face_plane;

            cur_face_indices += num_face_indices;
        }

        final_he_mesh = buildHalfEdgeMesh(src_mesh->positions, 
            src_mesh->numVertices, src_mesh->indices, src_mesh->faceCounts,
            hull_face_planes.data(), src_mesh->numFaces);
    } else {
        HullBuildData hull_data;
        bool valid_input = initHullBuild(
            Span(src_mesh->positions, src_mesh->numVertices), &hull_data);

        if (!valid_input) {
            return false;
        }

        quickhullBuild(hull_data);

        final_he_mesh = editMeshToRuntimeMesh(hull_data.mesh);
        freeBuildData(hull_data);
    }

    out_prim->hull.halfEdgeMesh = final_he_mesh;

    AABB mesh_aabb = AABB::point(src_mesh->positions[0]);
    for (CountT vert_idx = 1; vert_idx < (CountT)src_mesh->numVertices;
         vert_idx++) {
        mesh_aabb.expand(src_mesh->positions[vert_idx]);
    }
    *out_aabb = mesh_aabb;

    *total_num_halfedges += final_he_mesh.numHalfEdges;
    *total_num_faces += final_he_mesh.numFaces;
    *total_num_vertices += final_he_mesh.numVertices;

    return true;
}

Optional<PhysicsLoader::ImportedRigidBodies> PhysicsLoader::importRigidBodyData(
    const SourceCollisionObject *collision_objs,
    CountT num_objects,
    bool build_hulls)
{
    using namespace math;
    using Type = CollisionPrimitive::Type;

    HeapArray<uint32_t> prim_offsets(num_objects);
    HeapArray<uint32_t> prim_counts(num_objects);
    HeapArray<AABB> obj_aabbs(num_objects);
    HeapArray<RigidBodyMetadata> metadatas(num_objects);

    CountT total_num_prims = 0;
    for (CountT obj_idx = 0; obj_idx < num_objects; obj_idx++) {
        const SourceCollisionObject &collision_obj = collision_objs[obj_idx];
        CountT cur_num_prims = collision_obj.prims.size();

        prim_offsets[obj_idx] = total_num_prims;
        prim_counts[obj_idx] = cur_num_prims;
        total_num_prims += cur_num_prims;

        metadatas[obj_idx].friction = collision_objs[obj_idx].friction;
    }

    HeapArray<CollisionPrimitive> collision_prims(total_num_prims);
    HeapArray<AABB> prim_aabbs(total_num_prims);

    CountT cur_prim_offset = 0;
    CountT total_num_halfedges = 0;
    CountT total_num_faces = 0;
    CountT total_num_vertices = 0;
    for (CountT obj_idx = 0; obj_idx < num_objects; obj_idx++) {
        const SourceCollisionObject &collision_obj = collision_objs[obj_idx];

        auto obj_aabb = AABB::invalid();
        for (const SourceCollisionPrimitive &src_prim : collision_obj.prims) {
            CountT out_prim_idx = cur_prim_offset++;
            CollisionPrimitive *out_prim = &collision_prims[out_prim_idx];
            out_prim->type = src_prim.type;
            AABB prim_aabb;

            switch (src_prim.type) {
            case Type::Sphere: {
                setupSpherePrimitive(src_prim, out_prim, &prim_aabb);
            } break;
            case Type::Plane: {
                setupPlanePrimitive(src_prim, out_prim, &prim_aabb);
            } break;
            case Type::Hull: {
                bool valid_hull = setupHullPrimitive(src_prim, out_prim,
                    &prim_aabb, &total_num_halfedges, &total_num_faces,
                    &total_num_vertices, build_hulls);

                // FIXME: error reporting
                if (!valid_hull) {
                    return Optional<ImportedRigidBodies>::none();
                }
            } break;
            }

            prim_aabbs[out_prim_idx] = prim_aabb;
            obj_aabb = AABB::merge(obj_aabb, prim_aabb);
        }

        obj_aabbs[obj_idx] = obj_aabb;

        MassProperties mass_props = computeMassProperties(collision_obj);
        metadatas[obj_idx].mass =
            toMassData(mass_props, collision_obj.invMass);
    }

    // Combine half edge data into linear arrays
    ImportedRigidBodies::MergedHullData hull_data {
        .halfEdges = HeapArray<HalfEdge>(total_num_halfedges),
        .faceBaseHEs = HeapArray<uint32_t>(total_num_faces),
        .facePlanes = HeapArray<Plane>(total_num_faces),
        .positions = HeapArray<Vector3>(total_num_vertices),
    };

    CountT cur_halfedge_offset = 0;
    CountT cur_face_offset = 0;
    CountT cur_vert_offset = 0;
    for (CountT prim_idx = 0; prim_idx < total_num_prims; prim_idx++) {
        CollisionPrimitive &cur_prim = collision_prims[prim_idx];
        if (cur_prim.type != Type::Hull) continue;

        HalfEdgeMesh &he_mesh = cur_prim.hull.halfEdgeMesh;
        
        HalfEdge *he_out = &hull_data.halfEdges[cur_halfedge_offset];
        uint32_t *face_bases_out = &hull_data.faceBaseHEs[cur_face_offset];
        Plane *face_planes_out = &hull_data.facePlanes[cur_face_offset];
        Vector3 *pos_out = &hull_data.positions[cur_vert_offset];

        memcpy(he_out, he_mesh.halfEdges,
               sizeof(HalfEdge) * he_mesh.numHalfEdges);
        memcpy(face_bases_out, he_mesh.faceBaseHalfEdges,
               sizeof(uint32_t) * he_mesh.numFaces);
        memcpy(face_planes_out, he_mesh.facePlanes,
               sizeof(Plane) * he_mesh.numFaces);
        memcpy(pos_out, he_mesh.vertices,
               sizeof(Vector3) * he_mesh.numVertices);

        cur_halfedge_offset += he_mesh.numHalfEdges;
        cur_face_offset += he_mesh.numFaces;
        cur_vert_offset += he_mesh.numVertices;

        // FIXME this should be some kind of tmp allocation. See other FIXMEs
        freeHalfEdgeMesh(he_mesh); 

        he_mesh.halfEdges = he_out;
        he_mesh.faceBaseHalfEdges = face_bases_out;
        he_mesh.facePlanes = face_planes_out;
        he_mesh.vertices = pos_out;
    }

    return ImportedRigidBodies {
        .hullData = std::move(hull_data),
        .collisionPrimitives =  std::move(collision_prims),
        .primitiveAABBs = std::move(prim_aabbs),
        .primOffsets = std::move(prim_offsets),
        .primCounts = std::move(prim_counts),
        .metadatas = std::move(metadatas),
        .objectAABBs = std::move(obj_aabbs),
    };
}

CountT PhysicsLoader::loadObjects(
    const RigidBodyMetadata *metadatas,
    const math::AABB *obj_aabbs,
    const uint32_t *prim_offsets,
    const uint32_t *prim_counts,
    CountT num_objs,
    const CollisionPrimitive *primitives_in,
    const math::AABB *primitive_aabbs,
    CountT total_num_primitives,
    const geometry::HalfEdge *hull_halfedges_in,
    CountT total_num_hull_halfedges,
    const uint32_t *hull_face_base_halfedges_in,
    const geometry::Plane *hull_face_planes_in,
    CountT total_num_hull_faces,
    const math::Vector3 *hull_verts_in,
    CountT total_num_hull_verts)
{
    CountT cur_obj_offset = impl_->curObjOffset;
    impl_->curObjOffset += num_objs;
    CountT cur_prim_offset = impl_->curPrimOffset;
    impl_->curPrimOffset += total_num_primitives;
    assert(impl_->curObjOffset <= impl_->maxObjs);
    assert(impl_->curPrimOffset <= impl_->maxPrims);

    CollisionPrimitive *prims_dst = &impl_->primitives[cur_prim_offset];
    AABB *prim_aabbs_dst = &impl_->primAABBs[cur_prim_offset];

    AABB *obj_aabbs_dst = &impl_->objAABBs[cur_obj_offset];
    uint32_t *offsets_dst = &impl_->rigidBodyPrimitiveOffsets[cur_obj_offset];
    uint32_t *counts_dst = &impl_->rigidBodyPrimitiveCounts[cur_obj_offset];
    RigidBodyMetadata *metadatas_dst = &impl_->metadatas[cur_obj_offset];

    // FIXME: redo all this, leaks memory, slow, etc. Very non optimal on the
    // CPU.

    uint32_t *offsets_tmp = (uint32_t *)malloc(sizeof(uint32_t) * num_objs);
    for (CountT i = 0; i < num_objs; i++) {
        offsets_tmp[i] = prim_offsets[i] + cur_prim_offset;
    }

    HalfEdge *hull_halfedges;
    uint32_t *hull_face_base_halfedges;
    Plane *hull_face_planes;
    Vector3 *hull_verts;
    switch (impl_->execMode) {
    case ExecMode::CPU: {
        memcpy(prim_aabbs_dst, primitive_aabbs,
               sizeof(AABB) * total_num_primitives);

        memcpy(obj_aabbs_dst, obj_aabbs,
               sizeof(AABB) * num_objs);
        memcpy(offsets_dst, offsets_tmp,
               sizeof(uint32_t) * num_objs);
        memcpy(counts_dst, prim_counts,
               sizeof(uint32_t) * num_objs);
        memcpy(metadatas_dst, metadatas,
               sizeof(RigidBodyMetadata) * num_objs);

        hull_halfedges =
            (HalfEdge *)malloc(sizeof(HalfEdge) * total_num_hull_halfedges);
        hull_face_base_halfedges =
            (uint32_t *)malloc(sizeof(uint32_t) * total_num_hull_faces);
        hull_face_planes =
            (Plane *)malloc(sizeof(Plane) * total_num_hull_faces);
        hull_verts =
            (Vector3 *)malloc(sizeof(Vector3) * total_num_hull_verts);

        memcpy(hull_halfedges, hull_halfedges_in,
               sizeof(HalfEdge) * total_num_hull_halfedges);
        memcpy(hull_face_base_halfedges, hull_face_base_halfedges_in,
               sizeof(uint32_t) * total_num_hull_faces);
        memcpy(hull_face_planes, hull_face_planes_in,
               sizeof(Plane) * total_num_hull_faces);
        memcpy(hull_verts, hull_verts_in,
               sizeof(Vector3) * total_num_hull_verts);
    } break;
    case ExecMode::CUDA: {
#ifndef MADRONA_CUDA_SUPPORT
        noCUDA();
#else
        cudaMemcpy(prim_aabbs_dst, primitive_aabbs,
                   sizeof(AABB) * total_num_primitives,
                   cudaMemcpyHostToDevice);

        cudaMemcpy(obj_aabbs_dst, obj_aabbs,
                   sizeof(AABB) * num_objs,
                   cudaMemcpyHostToDevice);
        cudaMemcpy(offsets_dst, offsets_tmp,
                   sizeof(uint32_t) * num_objs,
                   cudaMemcpyHostToDevice);
        cudaMemcpy(counts_dst, prim_counts,
                   sizeof(uint32_t) * num_objs,
                   cudaMemcpyHostToDevice);
        cudaMemcpy(metadatas_dst, metadatas,
                   sizeof(RigidBodyMetadata) * num_objs,
                   cudaMemcpyHostToDevice);

        hull_halfedges = (HalfEdge *)cu::allocGPU(
            sizeof(HalfEdge) * total_num_hull_halfedges);
        hull_face_base_halfedges = (uint32_t *)cu::allocGPU(
            sizeof(uint32_t) * total_num_hull_faces);
        hull_face_planes = (Plane *)cu::allocGPU(
            sizeof(Plane) * total_num_hull_faces);
        hull_verts = (Vector3 *)cu::allocGPU(
            sizeof(Vector3) * total_num_hull_verts);

        cudaMemcpy(hull_halfedges, hull_halfedges_in,
                   sizeof(HalfEdge) * total_num_hull_halfedges,
                   cudaMemcpyHostToDevice);
        cudaMemcpy(hull_face_base_halfedges, hull_face_base_halfedges_in,
                   sizeof(uint32_t) * total_num_hull_faces,
                   cudaMemcpyHostToDevice);
        cudaMemcpy(hull_face_planes, hull_face_planes_in,
                   sizeof(Plane) * total_num_hull_faces,
                   cudaMemcpyHostToDevice);
        cudaMemcpy(hull_verts, hull_verts_in,
                   sizeof(Vector3) * total_num_hull_verts,
                   cudaMemcpyHostToDevice);
#endif
    }
    }

    auto primitives_tmp = (CollisionPrimitive *)malloc(
        sizeof(CollisionPrimitive) * total_num_primitives);
    memcpy(primitives_tmp, primitives_in,
           sizeof(CollisionPrimitive) * total_num_primitives);

    for (CountT i = 0; i < total_num_primitives; i++) {
        CollisionPrimitive &cur_primitive = primitives_tmp[i];
        if (cur_primitive.type != CollisionPrimitive::Type::Hull) continue;

        HalfEdgeMesh &he_mesh = cur_primitive.hull.halfEdgeMesh;

        // FIXME: incoming HalfEdgeMeshes should have offsets or something
        CountT hedge_offset = he_mesh.halfEdges - hull_halfedges_in;
        CountT face_offset = he_mesh.facePlanes - hull_face_planes_in;
        CountT vert_offset = he_mesh.vertices - hull_verts_in;

        he_mesh.halfEdges = hull_halfedges + hedge_offset;
        he_mesh.faceBaseHalfEdges = hull_face_base_halfedges + face_offset;
        he_mesh.facePlanes = hull_face_planes + face_offset;
        he_mesh.vertices = hull_verts + vert_offset;
    }

    switch (impl_->execMode) {
    case ExecMode::CPU: {
        memcpy(prims_dst, primitives_tmp,
               sizeof(CollisionPrimitive) * total_num_primitives);
    } break;
    case ExecMode::CUDA: {
#ifdef MADRONA_CUDA_SUPPORT
        cudaMemcpy(prims_dst, primitives_tmp,
            sizeof(CollisionPrimitive) * total_num_primitives,
            cudaMemcpyHostToDevice);
#endif
    } break;
    }

    free(primitives_tmp);
    free(offsets_tmp);

    return cur_obj_offset;
}

ObjectManager & PhysicsLoader::getObjectManager()
{
    return *impl_->mgr;
}

}
