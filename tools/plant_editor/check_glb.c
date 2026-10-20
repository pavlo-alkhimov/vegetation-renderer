// Checks a plant editor export with the same glTF reader the cookers use (third_party/cgltf.h):
// parse, load buffers, validate, and print meshes, materials, textures and the extras.
//
//   clang -O1 tools/plant_editor/check_glb.c -o build/check_glb.exe   (or cc ... -o build/check_glb)
//   build/check_glb tree.glb
#define CGLTF_IMPLEMENTATION
#include "../../third_party/cgltf.h"
#include <stdio.h>

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: check_glb file.glb\n"); return 2; }
    cgltf_options opt = {0};
    cgltf_data* d = NULL;
    cgltf_result r = cgltf_parse_file(&opt, argv[1], &d);
    if (r != cgltf_result_success) { fprintf(stderr, "parse failed (%d)\n", (int)r); return 1; }
    r = cgltf_load_buffers(&opt, d, argv[1]);
    if (r != cgltf_result_success) { fprintf(stderr, "load buffers failed (%d)\n", (int)r); return 1; }
    r = cgltf_validate(d);
    if (r != cgltf_result_success) { fprintf(stderr, "validate failed (%d)\n", (int)r); return 1; }
    for (cgltf_size i = 0; i < d->meshes_count; i++) {
        const cgltf_primitive* p = &d->meshes[i].primitives[0];
        printf("mesh %-12s %8zu vertices %8zu triangles  material %s\n", d->meshes[i].name, p->attributes[0].data->count,
               p->indices->count / 3, p->material ? p->material->name : "-");
    }
    for (cgltf_size i = 0; i < d->materials_count; i++) {
        const cgltf_material* m = &d->materials[i];
        printf("material %-8s base %s normal %s mr %s double-sided %d\n", m->name,
               m->pbr_metallic_roughness.base_color_texture.texture ? "yes" : "no", m->normal_texture.texture ? "yes" : "no",
               m->pbr_metallic_roughness.metallic_roughness_texture.texture ? "yes" : "no", m->double_sided);
    }
    printf("images %zu, textures %zu\n", d->images_count, d->textures_count);
    if (d->json && d->extras.start_offset) printf("extras %.*s\n", (int)(d->extras.end_offset - d->extras.start_offset), d->json + d->extras.start_offset);
    cgltf_free(d);
    printf("ok\n");
    return 0;
}
