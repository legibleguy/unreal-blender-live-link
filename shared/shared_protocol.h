#ifndef SHARED_PROTOCOL_H
#define SHARED_PROTOCOL_H

#include <stdint.h>

/*
 * Live Link Protocol - Shared definitions for the Blender addon and the
 * Unreal plugin. There are only two processes; the Unreal plugin hosts the
 * TCP listener and owns the shared-memory segment.
 *
 * Architecture:
 *   [Blender] <----TCP----> [Unreal plugin = host]
 *       |                        |
 *       +------ SHM (created by Unreal, opened by Blender) -----+
 */

/* ---- Networking ---- */
#define LIVELINK_DEFAULT_PORT  9876
#define LIVELINK_HOST          "127.0.0.1"
#define LIVELINK_MAX_MSG_LEN   64

/* TCP protocol messages — one-shot flow */
#define MSG_IDENT_BLENDER   "IDENT B"
#define MSG_IDENT_UNREAL    "IDENT U"

/* Diagnostics handshake.
 * Host (Unreal) sends "DIAG_MODE <abs_path>\n" or "DIAG_MODE OFF\n" to the
 * Blender peer immediately after the IDENT B handshake. When enabled, both
 * components write logs to <abs_path>/<role>.log. The folder is created by
 * the host under <project>/Saved/Logs/BlenderLink/diag_<timestamp>. */
#define MSG_DIAG_MODE        "DIAG_MODE"
#define MSG_DIAG_MODE_OFF    "DIAG_MODE OFF"

#define MSG_SEND_SCENE      "SEND_SCENE"
#define MSG_SCENE_READY     "SCENE_READY"
#define MSG_RECV_SCENE      "RECV_SCENE"
#define MSG_SCENE_ACK       "SCENE_ACK"

/* TCP protocol messages — streaming flow */
#define MSG_STREAM_START    "STREAM_START"
#define MSG_STREAM_STOP     "STREAM_STOP"
#define MSG_DELTA_READY     "DELTA_READY"
#define MSG_RECV_DELTA      "RECV_DELTA"
#define MSG_DELTA_ACK       "DELTA_ACK"

/* TCP protocol messages — reverse (Unreal → Blender) flow.
 * Same SHM layout; Unreal is the writer, Blender is the reader. */
#define MSG_PUSH_SCENE        "PUSH_SCENE"     /* U→server: request to write a scene */
#define MSG_PUSH_READY        "PUSH_READY"     /* U→server: scene written, notify Blender */
#define MSG_RECV_PUSH         "RECV_PUSH"      /* server→B: pull scene from SHM */
#define MSG_PUSH_ACK          "PUSH_ACK"       /* B→server: done reading */
#define MSG_PUSH_DELTA_READY  "PUSH_DELTA_READY"
#define MSG_RECV_PUSH_DELTA   "RECV_PUSH_DELTA"
#define MSG_PUSH_DELTA_ACK    "PUSH_DELTA_ACK"

/* ---- Shared Memory ---- */
#define LIVELINK_SHM_NAME       "live_link_shm"

/* On Windows: named mapping "Local\\live_link_shm" (created by the Unreal host
 *             via CreateFileMapping, opened by Blender via OpenFileMapping).
 * On Linux:   shm_open("/live_link_shm") -> /dev/shm/live_link_shm.
 *             Unreal creates with O_CREAT|O_EXCL after a defensive shm_unlink,
 *             and unlinks again on StopHost.
 */

#define LIVELINK_DEFAULT_SHM_SIZE_MB  64
#define LIVELINK_MAX_OBJECTS          1024

/* ---- SHM Update Types ---- */
#define SHM_UPDATE_FULL_SCENE  0   /* complete scene dump */
#define SHM_UPDATE_DELTA       1   /* incremental per-object deltas */

/* Per-object update types */
#define OBJ_UPDATE_FULL        0   /* full mesh: verts + normals + faces */
#define OBJ_UPDATE_TRANSFORM   1   /* transform only, no mesh data */
#define OBJ_UPDATE_POSITIONS   2   /* verts + normals only, topology unchanged */
#define OBJ_UPDATE_ADD         3   /* new object: full mesh data */
#define OBJ_UPDATE_REMOVE      4   /* object deleted: no mesh data */

/* ---- Shared Memory Layout ---- */

/*
 * Layout:
 *   [shm_header_t]                           (24 bytes)
 *   [mesh_object_header_t] * MAX_OBJECTS      (128 * 1024 bytes)
 *   [data region]                             (rest of SHM)
 *
 * Data region per-object (sequential, one object at a time):
 *   FULL / ADD:   vertices(N*12) + normals(F*3*12) + faces(F*12) + uvs(F*3*8, optional)
 *   POSITIONS:    vertices(N*12) + normals(F*3*12)
 *
 * Normals are per-triangle-corner: float32[face_count * 3 * 3].
 * Layout: [face0_c0.xyz, face0_c1.xyz, face0_c2.xyz, face1_c0.xyz, ...]
 * Corner order matches the face index array (before any winding swap on the Unreal side).
 *   TRANSFORM:    (no data)
 *   REMOVE:       (no data)
 *
 * uv_data_offset is -1 when no UV layer is present on the object.
 *
 * Offsets in mesh_object_header_t are relative to the start of the data region.
 * Unused offsets are set to -1.
 */

#pragma pack(push, 1)

typedef struct {
    float x, y, z;
} ll_vec3f;

typedef struct {
    volatile int32_t status;        /* 0=empty, 1=writing, 2=ready */
    int32_t          update_type;   /* SHM_UPDATE_FULL_SCENE or SHM_UPDATE_DELTA */
    int32_t          sequence_num;  /* monotonic counter for ordering */
    int32_t          object_count;
    int32_t          total_vertices;
    int32_t          total_faces;
} shm_header_t;
/* sizeof = 24 bytes */

typedef struct {
    char     name[64];             /* object name from Blender */
    ll_vec3f position;             /* world position */
    ll_vec3f rotation;             /* euler rotation (radians) */
    ll_vec3f scale;                /* world scale */
    int32_t  obj_update_type;      /* OBJ_UPDATE_* */
    int32_t  vertex_count;
    int32_t  face_count;
    int32_t  vertex_data_offset;   /* byte offset from data region start (-1 if unused) */
    int32_t  normal_data_offset;
    int32_t  face_data_offset;
    int32_t  uv_data_offset;       /* byte offset to UV data (-1 if no UVs)
                                    * Layout: float32[face_count * 3 * 2]
                                    * Per-triangle-corner UVs, same order as face indices */
    char     source_asset[128];    /* Unreal asset path for impostor objects (e.g. "/Game/Meshes/Foo");
                                    * empty string if the object originated in Blender.
                                    * Echoed by Blender in its deltas so Unreal can route by asset. */
} mesh_object_header_t;
/* sizeof = 64 + 12*3 + 4*7 + 128 = 256 bytes */

#pragma pack(pop)

#define SHM_HEADER_SIZE          sizeof(shm_header_t)
#define SHM_OBJECT_HEADERS_SIZE  (sizeof(mesh_object_header_t) * LIVELINK_MAX_OBJECTS)
#define SHM_DATA_REGION_OFFSET   (SHM_HEADER_SIZE + SHM_OBJECT_HEADERS_SIZE)

/* Helper: pointer to the i-th object header */
static inline mesh_object_header_t* shm_get_object_header(void *shm_base, int index) {
    return (mesh_object_header_t *)((char *)shm_base + SHM_HEADER_SIZE) + index;
}

/* Helper: pointer to the data region */
static inline void* shm_get_data_region(void *shm_base) {
    return (char *)shm_base + SHM_DATA_REGION_OFFSET;
}

#endif /* SHARED_PROTOCOL_H */
