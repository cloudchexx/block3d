#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct block3d_context block3d_context;

typedef enum block3d_status {
    BLOCK3D_OK = 0,
    BLOCK3D_ERROR_INVALID_ARGUMENT = 1,
    BLOCK3D_ERROR_IO = 2,
    BLOCK3D_ERROR_OUT_OF_MEMORY = 3,
    BLOCK3D_ERROR_OUT_OF_RANGE = 4,
    BLOCK3D_ERROR_FORMAT = 5,
    BLOCK3D_ERROR_INTERNAL = 6
} block3d_status;

typedef enum block3d_axis {
    BLOCK3D_AXIS_X = 0,
    BLOCK3D_AXIS_Y = 1,
    BLOCK3D_AXIS_Z = 2
} block3d_axis;

typedef enum block3d_layout {
    BLOCK3D_LAYOUT_LEGACY_XYZ = 0,
    BLOCK3D_LAYOUT_MICRO_TILED_XYZ = 1
} block3d_layout;

typedef enum block3d_read_dispatch {
    BLOCK3D_READ_DISPATCH_ROUND_ROBIN = 0,
    BLOCK3D_READ_DISPATCH_CONTIGUOUS = 1
} block3d_read_dispatch;

typedef struct block3d_version {
    int major;
    int minor;
    int patch;
} block3d_version;

typedef struct block3d_convert_options {
    uint64_t block_size;      /* 0 = auto; otherwise 16..256 */
    int num_threads;          /* 0 = auto */
    uint64_t max_memory_mb;   /* soft batch budget */
    block3d_layout layout;
    uint32_t micro_size;      /* 0 for legacy, 8 for micro-tiled */
    int progress;             /* non-zero enables progress output */
} block3d_convert_options;

typedef struct block3d_reader_options {
    int num_threads;          /* 0 = auto */
    uint64_t max_memory_mb;   /* soft budget */
    block3d_read_dispatch read_dispatch;
} block3d_reader_options;

typedef struct block3d_file_info {
    uint64_t dim_x;
    uint64_t dim_y;
    uint64_t dim_z;
    uint64_t block_size;
    uint64_t total_blocks;
    uint64_t data_offset;
    uint32_t format_version;
    block3d_layout layout;
    uint32_t micro_size;
} block3d_file_info;

typedef struct block3d_array {
    float* data;
    uint64_t dim0;
    uint64_t dim1;
    uint64_t dim2;
    size_t ndim;
    uint64_t count;
} block3d_array;

typedef struct block3d_slice_batch {
    float* data;              /* contiguous: slice_count * slice_elems */
    uint64_t* indices;        /* copy of requested indices */
    uint64_t slice_count;
    uint64_t slice_elems;
    uint64_t dim0;
    uint64_t dim1;
    block3d_axis axis;
} block3d_slice_batch;

block3d_version block3d_get_version(void);
const char* block3d_status_message(block3d_status status);

block3d_convert_options block3d_default_convert_options(void);
block3d_reader_options block3d_default_reader_options(void);

block3d_status block3d_convert_raw_to_b3d(
    const char* raw_path,
    const char* b3d_path,
    uint64_t dim_x,
    uint64_t dim_y,
    uint64_t dim_z,
    const block3d_convert_options* options);

block3d_status block3d_open_b3d(
    const char* b3d_path,
    const block3d_reader_options* options,
    block3d_context** out_context);

void block3d_close(block3d_context* context);

block3d_status block3d_get_info(
    const block3d_context* context,
    block3d_file_info* out_info);

block3d_status block3d_read_slice(
    block3d_context* context,
    block3d_axis axis,
    uint64_t index,
    block3d_array* out_array);

block3d_status block3d_read_slices_batch(
    block3d_context* context,
    block3d_axis axis,
    const uint64_t* indices,
    uint64_t index_count,
    block3d_slice_batch* out_batch);

block3d_status block3d_read_column(
    block3d_context* context,
    block3d_axis axis,
    uint64_t coord1,
    uint64_t coord2,
    block3d_array* out_array);

block3d_status block3d_read_subvolume(
    block3d_context* context,
    uint64_t xs,
    uint64_t xe,
    uint64_t ys,
    uint64_t ye,
    uint64_t zs,
    uint64_t ze,
    block3d_array* out_array);

block3d_status block3d_read_point(
    block3d_context* context,
    uint64_t x,
    uint64_t y,
    uint64_t z,
    float* out_value);

block3d_status block3d_verify_points(
    block3d_context* context,
    const char* raw_path,
    uint64_t samples,
    float tolerance);

void block3d_free_array(block3d_array* array);
void block3d_free_slice_batch(block3d_slice_batch* batch);

#ifdef __cplusplus
}
#endif
