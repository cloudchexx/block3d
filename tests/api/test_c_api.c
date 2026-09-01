#include <block3d/block3d.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float value_at(uint64_t x, uint64_t y, uint64_t z) {
    return (float)(x * 1000000ULL + y * 1000ULL + z);
}

static int write_raw(const char* path, uint64_t dx, uint64_t dy, uint64_t dz) {
    FILE* f = fopen(path, "wb");
    if (!f) return 1;
    for (uint64_t x = 0; x < dx; x++) {
        for (uint64_t y = 0; y < dy; y++) {
            for (uint64_t z = 0; z < dz; z++) {
                float v = value_at(x, y, z);
                if (fwrite(&v, sizeof(float), 1, f) != 1) {
                    fclose(f);
                    return 1;
                }
            }
        }
    }
    fclose(f);
    return 0;
}

static int nearly_equal(float a, float b) {
    return fabsf(a - b) <= 1e-6f;
}

#define REQUIRE_OK(expr) do { \
    block3d_status st__ = (expr); \
    if (st__ != BLOCK3D_OK) { \
        fprintf(stderr, "FAIL %s: %s\n", #expr, block3d_status_message(st__)); \
        return 1; \
    } \
} while (0)

int main(void) {
    const uint64_t dx = 9, dy = 10, dz = 11;
    const char* raw_path = "block3d_c_api_raw.dat";
    const char* b3d_path = "block3d_c_api.b3d";
    remove(raw_path);
    remove(b3d_path);

    if (write_raw(raw_path, dx, dy, dz) != 0) {
        fprintf(stderr, "FAIL write raw\n");
        return 1;
    }

    block3d_convert_options conv = block3d_default_convert_options();
    conv.block_size = 16;
    conv.num_threads = 2;
    conv.progress = 0;
    REQUIRE_OK(block3d_convert_raw_to_b3d(raw_path, b3d_path, dx, dy, dz, &conv));

    block3d_reader_options ro = block3d_default_reader_options();
    ro.num_threads = 2;
    block3d_context* ctx = NULL;
    REQUIRE_OK(block3d_open_b3d(b3d_path, &ro, &ctx));

    block3d_file_info info;
    REQUIRE_OK(block3d_get_info(ctx, &info));
    if (info.dim_x != dx || info.dim_y != dy || info.dim_z != dz || info.layout != BLOCK3D_LAYOUT_LEGACY_XYZ) {
        fprintf(stderr, "FAIL info\n");
        return 1;
    }

    block3d_array slice = {0};
    REQUIRE_OK(block3d_read_slice(ctx, BLOCK3D_AXIS_X, 3, &slice));
    if (slice.ndim != 2 || slice.dim0 != dy || slice.dim1 != dz || slice.count != dy * dz) {
        fprintf(stderr, "FAIL x slice shape\n");
        return 1;
    }
    if (!nearly_equal(slice.data[4 * dz + 5], value_at(3, 4, 5))) {
        fprintf(stderr, "FAIL x slice value\n");
        return 1;
    }
    block3d_free_array(&slice);

    uint64_t indices[3] = {0, 2, 4};
    block3d_slice_batch batch = {0};
    REQUIRE_OK(block3d_read_slices_batch(ctx, BLOCK3D_AXIS_Y, indices, 3, &batch));
    if (batch.slice_count != 3 || batch.slice_elems != dx * dz || batch.dim0 != dx || batch.dim1 != dz) {
        fprintf(stderr, "FAIL batch shape\n");
        return 1;
    }
    if (batch.indices[1] != 2 || !nearly_equal(batch.data[1 * batch.slice_elems + 6 * dz + 7], value_at(6, 2, 7))) {
        fprintf(stderr, "FAIL batch value\n");
        return 1;
    }
    block3d_free_slice_batch(&batch);

    block3d_array col = {0};
    REQUIRE_OK(block3d_read_column(ctx, BLOCK3D_AXIS_X, 4, 5, &col));
    if (col.ndim != 1 || col.dim0 != dx || !nearly_equal(col.data[8], value_at(8, 4, 5))) {
        fprintf(stderr, "FAIL column\n");
        return 1;
    }
    block3d_free_array(&col);

    block3d_array sub = {0};
    REQUIRE_OK(block3d_read_subvolume(ctx, 2, 6, 3, 8, 4, 10, &sub));
    if (sub.ndim != 3 || sub.dim0 != 4 || sub.dim1 != 5 || sub.dim2 != 6) {
        fprintf(stderr, "FAIL subvolume shape\n");
        return 1;
    }
    uint64_t sub_idx = (3 - 2) * 5 * 6 + (5 - 3) * 6 + (7 - 4);
    if (!nearly_equal(sub.data[sub_idx], value_at(3, 5, 7))) {
        fprintf(stderr, "FAIL subvolume value\n");
        return 1;
    }
    block3d_free_array(&sub);

    float point = 0.0f;
    REQUIRE_OK(block3d_read_point(ctx, 8, 9, 10, &point));
    if (!nearly_equal(point, value_at(8, 9, 10))) {
        fprintf(stderr, "FAIL point\n");
        return 1;
    }
    REQUIRE_OK(block3d_verify_points(ctx, raw_path, 100, 1e-3f));

    block3d_status bad = block3d_read_slice(ctx, BLOCK3D_AXIS_Z, dz, &slice);
    if (bad != BLOCK3D_ERROR_OUT_OF_RANGE) {
        fprintf(stderr, "FAIL out-of-range status: %s\n", block3d_status_message(bad));
        return 1;
    }

    block3d_close(ctx);
    remove(raw_path);
    remove(b3d_path);
    printf("C_API_TEST_RESULT ok=1\n");
    return 0;
}
