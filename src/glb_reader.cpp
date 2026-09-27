#include "gmb/scene.hpp"
#include "reader_util.hpp"
#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace gmb {
namespace {
using namespace reader;
std::vector<std::uint8_t> image_bytes(const cgltf_image& image, const std::filesystem::path& root,
                                      const ReaderOptions& options, bool& missing) {
    missing=false;
    if (image.buffer_view) {
        const auto* view=image.buffer_view;
        const auto* start=cgltf_buffer_view_data(view);
        if (!start || view->size>options.max_texture_bytes)
            reject("TEXTURE_READ_ERROR", "Embedded GLB image is unreadable or too large.", "texture");
        return {start,start+view->size};
    }
    if (!image.uri) reject("TEXTURE_READ_ERROR", "GLB image has no source.", "texture");
    const std::string uri=image.uri;
    if (uri.rfind("data:",0)==0) return data_uri(uri,options.max_texture_bytes,true);
    return load_image_uri(uri,root,options,missing);
}
const cgltf_accessor* attribute(const cgltf_primitive& p, cgltf_attribute_type type, int index=0) {
    for (cgltf_size i=0;i<p.attributes_count;++i)
        if (p.attributes[i].type==type && p.attributes[i].index==index) return p.attributes[i].data;
    return nullptr;
}
std::array<float,4> values(const cgltf_accessor* a, cgltf_size i, cgltf_size n, const std::string& ctx,
                           bool allow_nonfinite=false) {
    std::array<float,4> out{};
    if (!a || i>=a->count)
        reject("INVALID_GLTF_ACCESSOR","GLB attribute cannot be decoded.",ctx);
    auto base=*a;
    base.is_sparse=false;
    if (!cgltf_accessor_read_float(&base,i,out.data(),n))
        reject("INVALID_GLTF_ACCESSOR","GLB base attribute cannot be decoded.",ctx);
    if (a->is_sparse && a->sparse.count) {
        const auto& sparse=a->sparse;
        const auto* indices=cgltf_buffer_view_data(sparse.indices_buffer_view)+sparse.indices_byte_offset;
        const auto index_size=cgltf_component_size(sparse.indices_component_type);
        cgltf_size low=0,high=sparse.count;
        while (low<high) {
            const auto mid=low+(high-low)/2;
            const auto at=cgltf_component_read_index(indices+mid*index_size,sparse.indices_component_type);
            if (at<i) low=mid+1; else high=mid;
        }
        if (low<sparse.count && cgltf_component_read_index(indices+low*index_size,sparse.indices_component_type)==i) {
            const auto* raw=cgltf_buffer_view_data(sparse.values_buffer_view)+sparse.values_byte_offset+
                            low*cgltf_calc_size(a->type,a->component_type);
            if (!cgltf_element_read_float(raw,a->type,a->component_type,a->normalized,out.data(),n))
                reject("INVALID_GLTF_ACCESSOR","GLB sparse attribute cannot be decoded.",ctx);
        }
    }
    for (cgltf_size j=0;j<n;++j) if (!allow_nonfinite && !finite(out[j]))
        reject("NONFINITE_VERTEX","GLB attribute is nonfinite.",ctx);
    return out;
}
bool valid_normal(Vec3 n) {
    return finite(n.x)&&finite(n.y)&&finite(n.z)&&std::hypot(n.x,n.y,n.z)>1e-10;
}
bool png_has_alpha_channel(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size()<33 || bytes[8]!=0 || bytes[9]!=0 || bytes[10]!=0 || bytes[11]!=13 ||
        bytes[12]!='I' || bytes[13]!='H' || bytes[14]!='D' || bytes[15]!='R')
        reject("UNSUPPORTED_TEXTURE_FORMAT","GLB PNG has an invalid IHDR.","texture");
    const auto color_type=bytes[25];
    if (color_type==4 || color_type==6) return true;
    std::size_t offset=8;
    while (offset<bytes.size()) {
        if (bytes.size()-offset<12)
            reject("UNSUPPORTED_TEXTURE_FORMAT","GLB PNG chunk header is incomplete.","texture");
        const auto length=(std::uint32_t(bytes[offset])<<24)|(std::uint32_t(bytes[offset+1])<<16)|
                          (std::uint32_t(bytes[offset+2])<<8)|bytes[offset+3];
        if (length>bytes.size()-offset-12)
            reject("UNSUPPORTED_TEXTURE_FORMAT","GLB PNG chunk exceeds image bytes.","texture");
        const auto type=std::string(reinterpret_cast<const char*>(bytes.data()+offset+4),4);
        if (type=="tRNS") return true;
        if (type=="IDAT" || type=="IEND") break;
        offset+=12+length;
    }
    return false;
}
void require_no_extensions(const cgltf_extension* extensions, cgltf_size count, const std::string& ctx) {
    if (count) reject("UNSUPPORTED_GLTF_EXTENSION", "GLB extension is not represented: " + label(extensions[0].name,"unknown"),ctx);
}
} // namespace

Scene read_glb(const std::filesystem::path& input, const ReaderOptions& options) {
    const auto path=std::filesystem::absolute(input);
    Scene out;
    out.name=path.stem().u8string(); out.source=path.u8string();
    out.conversion_profile=options.gis_static?"gis-static":"strict";
    out.missing_texture_policy=options.missing_texture_fallback?"material-color":"error";
    try {
        auto bytes=read_bytes(path,options.max_file_bytes);
        cgltf_options parse_options{};
        cgltf_data* raw=nullptr;
        const auto parsed=cgltf_parse(&parse_options,bytes.data(),bytes.size(),&raw);
        std::unique_ptr<cgltf_data,decltype(&cgltf_free)> data(raw,&cgltf_free);
        auto extension=path.extension().u8string();
        std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        const auto expected=extension==".gltf"?cgltf_file_type_gltf:cgltf_file_type_glb;
        if (parsed!=cgltf_result_success || !data || data->file_type!=expected)
            reject("INVALID_GLB","GLB header, chunks or JSON could not be parsed.","scene");
        if (!data->asset.version || std::string(data->asset.version)!="2.0" ||
            (data->asset.min_version && std::string(data->asset.min_version)>"2.0"))
            reject("UNSUPPORTED_GLTF_VERSION","Only glTF 2.0 GLB is supported.","scene");
        for (cgltf_size i=0;i<data->extensions_used_count;++i) {
            const std::string ext=data->extensions_used[i];
            if (ext!="KHR_materials_unlit" && ext!="KHR_texture_transform" && ext!="KHR_mesh_quantization")
                reject("UNSUPPORTED_GLTF_EXTENSION","GLB extension is not represented: " + ext,"scene");
        }
        for (cgltf_size i=0;i<data->extensions_required_count;++i) {
            const std::string ext=data->extensions_required[i];
            if (ext!="KHR_materials_unlit" && ext!="KHR_texture_transform" && ext!="KHR_mesh_quantization")
                reject("UNSUPPORTED_GLTF_EXTENSION","Required GLB extension is not represented: " + ext,"scene");
        }
        require_no_extensions(data->data_extensions,data->data_extensions_count,"scene");
        require_no_extensions(data->asset.extensions,data->asset.extensions_count,"asset");
        for (cgltf_size i=0;i<data->buffers_count;++i)
            require_no_extensions(data->buffers[i].extensions,data->buffers[i].extensions_count,"buffer");
        for (cgltf_size i=0;i<data->buffer_views_count;++i)
            require_no_extensions(data->buffer_views[i].extensions,data->buffer_views[i].extensions_count,"bufferView");
        for (cgltf_size i=0;i<data->accessors_count;++i)
            require_no_extensions(data->accessors[i].extensions,data->accessors[i].extensions_count,"accessor");
        for (cgltf_size i=0;i<data->images_count;++i)
            require_no_extensions(data->images[i].extensions,data->images[i].extensions_count,"image");
        for (cgltf_size i=0;i<data->textures_count;++i)
            require_no_extensions(data->textures[i].extensions,data->textures[i].extensions_count,"texture");
        for (cgltf_size i=0;i<data->samplers_count;++i)
            require_no_extensions(data->samplers[i].extensions,data->samplers[i].extensions_count,"sampler");
        for (cgltf_size i=0;i<data->meshes_count;++i)
            require_no_extensions(data->meshes[i].extensions,data->meshes[i].extensions_count,"mesh");
        for (cgltf_size i=0;i<data->scenes_count;++i)
            require_no_extensions(data->scenes[i].extensions,data->scenes[i].extensions_count,"scene");
        if (data->skins_count || data->variants_count)
            reject("UNSUPPORTED_GLTF_DEFORMATION","GLB skin or material variants require a baked static export.","scene");
        std::size_t animated_channels=0;
        for (cgltf_size i=0;i<data->animations_count;++i) {
            const auto& animation=data->animations[i];
            require_no_extensions(animation.extensions,animation.extensions_count,"animation");
            for (cgltf_size j=0;j<animation.channels_count;++j) {
                const auto& channel=animation.channels[j];
                if (channel.target_path!=cgltf_animation_path_type_translation &&
                    channel.target_path!=cgltf_animation_path_type_rotation &&
                    channel.target_path!=cgltf_animation_path_type_scale)
                    reject("UNSUPPORTED_GLTF_DEFORMATION","GLB animation changes geometry weights or uses an unknown channel.","scene");
                ++animated_channels;
            }
        }
        if (animated_channels && !options.gis_static)
            reject("UNSUPPORTED_ANIMATION","GLB animation is present; use gis-static for the saved node pose or export a static snapshot.","scene");
        if (animated_channels)
            out.diagnostics.push_back({Severity::warning,"STATIC_POSE_USED",
                "GIS static profile uses the saved GLB node transforms without evaluating an animation time; " +
                std::to_string(animated_channels) + " animated transform channel(s) are not applied.","scene"});
        else if (data->animations_count)
            out.diagnostics.push_back({Severity::warning,"EMPTY_ANIMATION_IGNORED",
                "GLB animation containers have no transform channels; saved static geometry is used.","scene"});
        const auto root=std::filesystem::weakly_canonical(path.parent_path());
        std::vector<std::vector<std::uint8_t>> external_buffers;
        external_buffers.reserve(data->buffers_count);
        for (cgltf_size i=0;i<data->buffers_count;++i) {
            auto& b=data->buffers[i];
            if (i==0 && !b.uri && data->bin && data->bin_size>=b.size) {
                b.data=const_cast<void*>(data->bin); b.data_free_method=cgltf_data_free_method_none;
            } else if (b.uri) {
                const std::string uri=b.uri;
                external_buffers.push_back(uri.rfind("data:",0)==0?data_uri(uri,options.max_file_bytes,false):
                    read_bytes(resource_path(root,uri),options.max_file_bytes));
                if (external_buffers.back().size()<b.size)
                    reject("INVALID_GLTF_BUFFER","External GLB buffer is shorter than declared.","scene");
                b.data=external_buffers.back().data(); b.data_free_method=cgltf_data_free_method_none;
            } else reject("INVALID_GLTF_BUFFER","GLB buffer has no binary data.","scene");
        }
        for (cgltf_size i=0;i<data->buffer_views_count;++i) {
            const auto& view=data->buffer_views[i];
            if (!view.buffer || !view.buffer->data || view.offset>view.buffer->size ||
                view.size>view.buffer->size-view.offset || view.has_meshopt_compression)
                reject("INVALID_GLTF_BUFFER","GLB buffer view is missing, compressed or outside its buffer.","scene");
        }
        for (cgltf_size i=0;i<data->accessors_count;++i) {
            const auto& a=data->accessors[i];
            const auto element=cgltf_calc_size(a.type,a.component_type);
            if (!a.count || !element || (!a.buffer_view && !a.is_sparse))
                reject("INVALID_GLTF_ACCESSOR","GLB accessor has no valid elements or storage.","scene");
            if (a.buffer_view && (a.offset>a.buffer_view->size || element>a.buffer_view->size-a.offset ||
                a.stride<element || a.count-1>(a.buffer_view->size-a.offset-element)/a.stride))
                reject("INVALID_GLTF_ACCESSOR","GLB accessor exceeds its buffer view.","scene");
            if (a.is_sparse) {
                const auto& s=a.sparse;
                const auto index_size=cgltf_component_size(s.indices_component_type);
                if (!s.indices_buffer_view || !s.values_buffer_view || !index_size || s.count>a.count ||
                    s.indices_byte_offset>s.indices_buffer_view->size ||
                    s.count>(s.indices_buffer_view->size-s.indices_byte_offset)/index_size ||
                    s.values_byte_offset>s.values_buffer_view->size ||
                    s.count>(s.values_buffer_view->size-s.values_byte_offset)/element)
                    reject("INVALID_GLTF_ACCESSOR","GLB sparse accessor exceeds its buffer view.","scene");
                const auto* sparse_indices=cgltf_buffer_view_data(s.indices_buffer_view)+s.indices_byte_offset;
                cgltf_size previous=0;
                for (cgltf_size k=0;k<s.count;++k) {
                    const auto index=cgltf_component_read_index(sparse_indices+k*index_size,s.indices_component_type);
                    if (index>=a.count || (k && index<=previous))
                        reject("INVALID_GLTF_ACCESSOR","GLB sparse indices must be strictly increasing and in range.","scene");
                    previous=index;
                }
            }
        }
        if (cgltf_validate(data.get())!=cgltf_result_success)
            reject("INVALID_GLB","GLB accessor or buffer boundaries are invalid.","scene");
        const cgltf_scene* selected_scene=data->scene;
        if (!selected_scene && data->scenes_count==1) {
            selected_scene=&data->scenes[0];
            out.diagnostics.push_back({Severity::warning,"GLTF_SCENE_ASSUMPTION",
                "GLB has one scene but no default scene index; the sole scene was selected.","scene"});
        }
        if (!selected_scene) reject("MISSING_GLTF_SCENE","GLB has no unambiguous default scene.","scene");
        if (data->scenes_count>1)
            out.diagnostics.push_back({Severity::warning,"GLTF_SCENE_SELECTION","Only the declared default glTF scene is converted.","scene"});
        out.diagnostics.push_back({Severity::warning,"GLTF_COORDINATE_CONVENTION",
            "glTF right-handed Y-up metre coordinates were converted to right-handed Z-up metres; node transforms are baked once.","scene"});

        std::vector<int> material_indices(data->materials_count,-1);
        auto material_for=[&](const cgltf_material* source) -> int {
            if (!source) {
                if (!options.gis_static) reject("UNSUPPORTED_GLTF_MATERIAL","Default glTF material uses PBR shading; use gis-static or an unlit material.","scene");
                Material m; m.name="Default glTF PBR white"; m.double_sided=false;
                out.materials.push_back(m);
                out.diagnostics.push_back({Severity::warning,"MATERIAL_CHANNEL_OMITTED","GIS static profile omits default glTF PBR lighting; white base color remains.","scene"});
                return static_cast<int>(out.materials.size()-1);
            }
            const auto mi=static_cast<std::size_t>(source-data->materials);
            if (material_indices[mi]>=0) return material_indices[mi];
            const auto ctx="material:"+label(source->name,std::to_string(mi));
            require_no_extensions(source->extensions,source->extensions_count,ctx);
            if (source->has_pbr_specular_glossiness || source->has_clearcoat || source->has_transmission ||
                source->has_volume || source->has_ior || source->has_specular || source->has_sheen ||
                source->has_emissive_strength || source->has_iridescence || source->has_diffuse_transmission ||
                source->has_anisotropy || source->has_dispersion || source->normal_texture.texture ||
                source->occlusion_texture.texture || source->emissive_texture.texture ||
                source->pbr_metallic_roughness.metallic_roughness_texture.texture ||
                source->emissive_factor[0]!=0 || source->emissive_factor[1]!=0 || source->emissive_factor[2]!=0)
                reject("UNSUPPORTED_GLTF_MATERIAL","GLB material has render channels that cannot be preserved.",ctx);
            if (source->alpha_mode==cgltf_alpha_mode_mask)
                reject("UNSUPPORTED_GLTF_ALPHA","GLB alpha MASK cutoff cannot be represented.",ctx);
            if (!source->unlit) {
                if (!options.gis_static) reject("UNSUPPORTED_GLTF_MATERIAL","PBR shading requires explicit gis-static profile or an unlit material.",ctx);
                out.diagnostics.push_back({Severity::warning,"MATERIAL_CHANNEL_OMITTED",
                    "GIS static profile omits glTF metallic/roughness PBR lighting; base color, scalar opacity and base-color image remain. Appearance is not baked.",ctx});
            } else out.diagnostics.push_back({Severity::warning,"UNLIT_SHADING_MAPPED",
                "glTF unlit base color is mapped to FileGDB diffuse material; the target scene may light it differently because the shape material has no unlit flag.",ctx});
            Material m; m.name=label(source->name,"GLB material "+std::to_string(mi));
            m.double_sided=source->double_sided;
            const auto& pbr=source->pbr_metallic_roughness;
            if (!finite(pbr.metallic_factor) || !finite(pbr.roughness_factor) ||
                pbr.metallic_factor<0 || pbr.metallic_factor>1 ||
                pbr.roughness_factor<0 || pbr.roughness_factor>1)
                reject("INVALID_MATERIAL_VALUE","GLB PBR scalar factor is invalid.",ctx);
            m.color={pbr.base_color_factor[0],pbr.base_color_factor[1],pbr.base_color_factor[2],
                     source->alpha_mode==cgltf_alpha_mode_opaque?1.0:pbr.base_color_factor[3]};
            if (source->alpha_mode==cgltf_alpha_mode_opaque && pbr.base_color_factor[3]!=1)
                out.diagnostics.push_back({Severity::warning,"OPAQUE_ALPHA_IGNORED","glTF OPAQUE mode ignores base-color alpha; stored opacity is 1.",ctx});
            const auto& view=pbr.base_color_texture;
            if (view.texture) {
                const auto* texture=view.texture;
                if (texture->has_basisu || texture->has_webp || !texture->image)
                    reject("UNSUPPORTED_TEXTURE_FORMAT","GLB base-color texture must be PNG or JPEG.",ctx);
                if (texture->sampler && (texture->sampler->wrap_s!=cgltf_wrap_mode_repeat ||
                    texture->sampler->wrap_t!=cgltf_wrap_mode_repeat || texture->sampler->mag_filter ||
                    texture->sampler->min_filter))
                    reject("UNSUPPORTED_GLTF_SAMPLER","GLB texture sampler settings cannot be preserved.",ctx);
                bool missing=false;
                auto image=image_bytes(*texture->image,root,options,missing);
                if (missing) {
                    if (!options.missing_texture_fallback)
                        reject("MISSING_TEXTURE","GLB base-color image is missing: " + std::string(texture->image->uri),ctx);
                    out.diagnostics.push_back({Severity::warning,"MISSING_TEXTURE_FALLBACK",
                        "Missing GLB base-color image; retained diffuse color and scalar opacity: " + std::string(texture->image->uri),ctx});
                } else {
                    const auto detected=mime_type(image);
                    if (detected=="application/octet-stream" ||
                        (texture->image->mime_type && detected!=texture->image->mime_type))
                        reject("UNSUPPORTED_TEXTURE_FORMAT","GLB image bytes or declared media type are unsupported.",ctx);
                    if (source->alpha_mode==cgltf_alpha_mode_opaque && detected=="image/png" &&
                        png_has_alpha_channel(image))
                        reject("UNSUPPORTED_GLTF_ALPHA",
                            "GLB OPAQUE mode ignores image alpha, but the FileGDB texture would retain it; export an opaque RGB image or use BLEND.",ctx);
                    Texture t; t.name=label(texture->image->name,"GLB image "+std::to_string(out.textures.size()));
                    t.mime_type=detected; t.source=texture->image->uri?texture->image->uri:"embedded:GLB";
                    t.embedded=texture->image->buffer_view!=nullptr || t.source.rfind("data:",0)==0;
                    if (t.source.rfind("data:",0)==0) t.source="embedded:glTF-data-uri";
                    t.bytes=std::move(image);
                    m.texture=static_cast<int>(out.textures.size()); out.textures.push_back(std::move(t));
                }
            }
            material_indices[mi]=static_cast<int>(out.materials.size());
            out.materials.push_back(std::move(m));
            return material_indices[mi];
        };

        std::set<const cgltf_node*> visited, active;
        std::function<void(const cgltf_node*,int,const std::array<double,16>&,std::size_t)> visit;
        visit=[&](const cgltf_node* source,int parent,const std::array<double,16>& parent_matrix,std::size_t depth) {
            if (!source || depth>1024 || active.count(source) || visited.count(source))
                reject("INVALID_GLTF_NODE","GLB node hierarchy is cyclic, shared or too deep.","scene");
            active.insert(source); visited.insert(source);
            const auto ni=static_cast<std::size_t>(source-data->nodes);
            const auto ctx="node:"+label(source->name,std::to_string(ni));
            require_no_extensions(source->extensions,source->extensions_count,ctx);
            if (source->skin || source->weights_count || source->has_mesh_gpu_instancing || source->light || source->camera)
                reject("UNSUPPORTED_GLTF_NODE","GLB skinned, weighted, GPU-instanced, light or camera node cannot be represented.",ctx);
            cgltf_float local_f[16]; cgltf_node_transform_local(source,local_f);
            std::array<double,16> local{};
            for (int i=0;i<16;++i) {
                local[i]=local_f[i];
                if (!finite(local[i])) reject("NONFINITE_TRANSFORM","GLB node transform is nonfinite.",ctx);
            }
            if (local[3]!=0 || local[7]!=0 || local[11]!=0 || local[15]!=1)
                reject("INVALID_TRANSFORM","GLB node transform must be affine.",ctx);
            if (source->has_rotation) {
                const auto& q=source->rotation;
                const double qlen=std::hypot(std::hypot(q[0],q[1]),std::hypot(q[2],q[3]));
                if (!finite(qlen) || std::abs(qlen-1.0)>1e-4)
                    reject("INVALID_TRANSFORM","GLB rotation quaternion must have unit length.",ctx);
            }
            const auto world=multiply(parent_matrix,local);
            for (auto v:world) if (!finite(v)) reject("NONFINITE_TRANSFORM","GLB world transform is nonfinite.",ctx);
            Node node; node.name=label(source->name,"GLB node "+std::to_string(ni));
            node.source_id="glTF node "+std::to_string(ni); node.parent=parent; node.source_world_transform=world;
            const int node_index=static_cast<int>(out.nodes.size()); out.nodes.push_back(std::move(node));
            if (source->mesh) {
                const auto det=determinant(world);
                if (!finite(det) || det==0) reject("INVALID_TRANSFORM","GLB mesh transform is singular or nonfinite.",ctx);
                const auto mesh_index=static_cast<std::size_t>(source->mesh-data->meshes);
                if (source->mesh->weights_count) reject("UNSUPPORTED_GLTF_DEFORMATION","GLB morph weights require baked static geometry.",ctx);
                for (cgltf_size pi=0;pi<source->mesh->primitives_count;++pi) {
                    const auto& p=source->mesh->primitives[pi];
                    const auto pc=ctx+"/primitive:"+std::to_string(pi);
                    require_no_extensions(p.extensions,p.extensions_count,pc);
                    if (p.type!=cgltf_primitive_type_triangles || p.targets_count || p.has_draco_mesh_compression || p.mappings_count)
                        reject("UNSUPPORTED_GLTF_PRIMITIVE","Only static TRIANGLES without morphs, compression or variants are supported.",pc);
                    const auto* positions=attribute(p,cgltf_attribute_type_position);
                    const auto* normals=attribute(p,cgltf_attribute_type_normal);
                    if (!positions || positions->type!=cgltf_type_vec3 ||
                        (normals && (normals->type!=cgltf_type_vec3 || normals->count!=positions->count)))
                        reject("INVALID_GLTF_ATTRIBUTE","GLB POSITION/NORMAL accessor type or count is invalid.",pc);
                    const int material=material_for(p.material);
                    const auto& view=p.material?p.material->pbr_metallic_roughness.base_color_texture:cgltf_texture_view{};
                    const int uv_set=view.has_transform && view.transform.has_texcoord?view.transform.texcoord:view.texcoord;
                    const auto* uv=attribute(p,cgltf_attribute_type_texcoord,uv_set);
                    if ((out.materials[material].texture>=0 && !uv) || (uv && (uv->type!=cgltf_type_vec2 || uv->count!=positions->count)))
                        reject("MISSING_UV","Textured GLB primitive has no complete selected TEXCOORD set.",pc);
                    for (cgltf_size ai=0;ai<p.attributes_count;++ai) {
                        const auto& a=p.attributes[ai];
                        if (a.type==cgltf_attribute_type_position || a.type==cgltf_attribute_type_normal ||
                            (a.type==cgltf_attribute_type_texcoord && a.index==uv_set)) continue;
                        if (a.type==cgltf_attribute_type_texcoord) {
                            out.diagnostics.push_back({Severity::warning,"UV_CHANNEL_OMITTED","Unused glTF TEXCOORD set is omitted; the selected base-color UV set is retained.",pc});
                        } else reject("UNSUPPORTED_GLTF_ATTRIBUTE","GLB vertex attribute cannot be represented: " + label(a.name,"unknown"),pc);
                    }
                    const auto count=p.indices?p.indices->count:positions->count;
                    if (count==0 || count%3 || count>10000000 || count>(std::numeric_limits<std::uint32_t>::max)())
                        reject("INVALID_GLTF_INDICES","GLB primitive must contain complete triangles within the corner limit.",pc);
                    if (p.indices && (p.indices->type!=cgltf_type_scalar || p.indices->is_sparse ||
                        (p.indices->component_type!=cgltf_component_type_r_8u &&
                         p.indices->component_type!=cgltf_component_type_r_16u &&
                         p.indices->component_type!=cgltf_component_type_r_32u)))
                        reject("INVALID_GLTF_INDICES","GLB indices must be non-sparse unsigned scalars.",pc);
                    Mesh mesh; mesh.name=label(source->mesh->name,"GLB mesh "+std::to_string(mesh_index))+"/"+std::to_string(pi);
                    mesh.source_node=node.name; mesh.vertices.reserve(count); mesh.triangles.reserve(count/3);
                    std::size_t repaired=0, repaired_triangles=0, discarded=0, discarded_normals=0;
                    for (cgltf_size ti=0;ti<count;ti+=3) {
                        std::array<Vertex,3> corners{};
                        for (int k=0;k<3;++k) {
                            const auto ix=p.indices?cgltf_accessor_read_index(p.indices,ti+k):ti+k;
                            if (ix>=positions->count) reject("INVALID_GLTF_INDICES","GLB index is outside POSITION accessor.",pc);
                            const auto v=values(positions,ix,3,pc);
                            corners[k].position=point(world,{v[0],v[1],v[2]});
                            if (normals) {
                                const auto n=values(normals,ix,3,pc,options.gis_static);
                                const Vec3 raw_normal{n[0],n[1],n[2]};
                                const auto raw_length=std::hypot(raw_normal.x,raw_normal.y,raw_normal.z);
                                if (!finite(raw_length) || std::abs(raw_length-1.0)>1e-3) {
                                    if (!options.gis_static)
                                        reject("INVALID_NORMAL","GLB source normal is not finite and unit length.",pc);
                                    corners[k].normal={0,0,0};
                                } else corners[k].normal=normal(world,raw_normal,det);
                                corners[k].has_normal=true;
                            }
                            if (uv) {
                                const auto t=values(uv,ix,2,pc);
                                double u=t[0],vcoord=t[1];
                                if (view.has_transform) {
                                    const auto& tr=view.transform;
                                    if (!finite(tr.offset[0]) || !finite(tr.offset[1]) || !finite(tr.scale[0]) ||
                                        !finite(tr.scale[1]) || !finite(tr.rotation))
                                        reject("INVALID_GLTF_UV_TRANSFORM","GLB UV transform is nonfinite.",pc);
                                    const double x=u*tr.scale[0], y=vcoord*tr.scale[1];
                                    u=tr.offset[0]+std::cos(tr.rotation)*x-std::sin(tr.rotation)*y;
                                    vcoord=tr.offset[1]+std::sin(tr.rotation)*x+std::cos(tr.rotation)*y;
                                }
                                // Core/codec UVs use a bottom-left V origin; glTF uses top-left.
                                // The native codec flips V for stored image rows, so convert here once.
                                corners[k].uv={u,1.0-vcoord}; corners[k].has_uv=true;
                            }
                        }
                        if (det<0) std::swap(corners[1],corners[2]);
                        const auto a=corners[0].position,b=corners[1].position,c=corners[2].position;
                        const auto face=cross({b.x-a.x,b.y-a.y,b.z-a.z},{c.x-a.x,c.y-a.y,c.z-a.z});
                        const auto length=std::hypot(face.x,face.y,face.z);
                        if (length==0 && options.gis_static) {
                            ++discarded;
                            for (const auto& corner:corners)
                                if (corner.has_normal && !valid_normal(corner.normal)) ++discarded_normals;
                            continue;
                        }
                        if (!finite(length) || length==0)
                            reject("DEGENERATE_TRIANGLE","GLB triangle has zero or nonfinite area.",pc);
                        const auto before_repair=repaired;
                        for (auto& corner:corners) if (corner.has_normal && !valid_normal(corner.normal)) {
                            if (!options.gis_static) reject("INVALID_NORMAL","GLB normal is invalid.",pc);
                            corner.normal={face.x/length,face.y/length,face.z/length}; ++repaired;
                        }
                        if (repaired!=before_repair) ++repaired_triangles;
                        Triangle tri; tri.material=material;
                        for (int k=0;k<3;++k) {
                            tri.indices[k]=static_cast<std::uint32_t>(mesh.vertices.size());
                            mesh.vertices.push_back(corners[k]);
                        }
                        mesh.triangles.push_back(tri);
                    }
                    if (discarded) out.diagnostics.push_back({Severity::warning,"DEGENERATE_TRIANGLES_REMOVED",
                        "GIS static profile removed " + std::to_string(discarded) + " strictly zero-area triangles; no area tolerance was used.",pc});
                    if (discarded_normals) out.diagnostics.push_back({Severity::warning,"DEGENERATE_NORMALS_DISCARDED",
                        "GIS static discarded " + std::to_string(discarded_normals) + " invalid normals with removed zero-area triangles.",pc});
                    if (repaired) out.diagnostics.push_back({Severity::warning,"NORMALS_REPAIRED",
                        "GIS static profile rebuilt " + std::to_string(repaired) + " invalid corner normals across " +
                        std::to_string(repaired_triangles) + " triangles from transformed triangle edges.",pc});
                    if (mesh.triangles.empty()) reject("EMPTY_MESH","GLB primitive has no retained triangles.",pc);
                    out.nodes[node_index].meshes.push_back(static_cast<std::uint32_t>(out.meshes.size()));
                    out.meshes.push_back(std::move(mesh));
                }
            }
            for (cgltf_size i=0;i<source->children_count;++i) visit(source->children[i],node_index,world,depth+1);
            active.erase(source);
        };
        for (cgltf_size i=0;i<selected_scene->nodes_count;++i) visit(selected_scene->nodes[i],-1,identity(),0);
    } catch (const Issue& e) {
        out.diagnostics.push_back({Severity::error,e.code,e.what(),e.context});
    }
    return out;
}
} // namespace gmb
