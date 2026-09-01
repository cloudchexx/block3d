#include <block3d/block3d.h>

#include <stdio.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE.b3d\n", argv[0]);
        return 1;
    }

    block3d_reader_options options = block3d_default_reader_options();
    block3d_context* reader = NULL;
    block3d_status status = block3d_open_b3d(argv[1], &options, &reader);
    if (status != BLOCK3D_OK) {
        fprintf(stderr, "open failed: %s\n", block3d_status_message(status));
        return 1;
    }

    block3d_file_info info;
    status = block3d_get_info(reader, &info);
    if (status != BLOCK3D_OK) {
        fprintf(stderr, "info failed: %s\n", block3d_status_message(status));
        block3d_close(reader);
        return 1;
    }
    printf("dims=%llu x %llu x %llu block=%llu version=%u\n",
           (unsigned long long)info.dim_x,
           (unsigned long long)info.dim_y,
           (unsigned long long)info.dim_z,
           (unsigned long long)info.block_size,
           info.format_version);

    block3d_array x_slice = {0};
    status = block3d_read_slice(reader, BLOCK3D_AXIS_X, 0, &x_slice);
    if (status == BLOCK3D_OK) {
        printf("x slice shape=(%llu,%llu) first=%g\n",
               (unsigned long long)x_slice.dim0,
               (unsigned long long)x_slice.dim1,
               x_slice.count ? x_slice.data[0] : 0.0f);
    }
    block3d_free_array(&x_slice);

    block3d_array column = {0};
    status = block3d_read_column(reader, BLOCK3D_AXIS_X, 0, 0, &column);
    if (status == BLOCK3D_OK) {
        printf("x column length=%llu first=%g\n",
               (unsigned long long)column.dim0,
               column.count ? column.data[0] : 0.0f);
    }
    block3d_free_array(&column);

    block3d_array sub = {0};
    status = block3d_read_subvolume(reader,
                                    0, info.dim_x < 4 ? info.dim_x : 4,
                                    0, info.dim_y < 4 ? info.dim_y : 4,
                                    0, info.dim_z < 4 ? info.dim_z : 4,
                                    &sub);
    if (status == BLOCK3D_OK) {
        printf("subvolume shape=(%llu,%llu,%llu) first=%g\n",
               (unsigned long long)sub.dim0,
               (unsigned long long)sub.dim1,
               (unsigned long long)sub.dim2,
               sub.count ? sub.data[0] : 0.0f);
    }
    block3d_free_array(&sub);

    float value = 0.0f;
    status = block3d_read_point(reader, 0, 0, 0, &value);
    if (status == BLOCK3D_OK) {
        printf("point(0,0,0)=%g\n", value);
    }

    block3d_close(reader);
    return 0;
}
