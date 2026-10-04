#include "reader_util.hpp"
#include <charconv>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string_view>

namespace gmb {
namespace {
using namespace reader;
constexpr std::size_t corner_limit = 10000000;
constexpr std::size_t solid_limit = 100000;
bool finite_vec(Vec3 v) { return finite(v.x) && finite(v.y) && finite(v.z); }
bool space(char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='\f' || c=='\v'; }
Vec3 unit_normal(Vec3 v) {
    const auto scale=(std::max)({std::abs(v.x),std::abs(v.y),std::abs(v.z)});
    if(!finite_vec(v) || scale==0) return {};
    v={v.x/scale,v.y/scale,v.z/scale};
    const auto length=std::hypot(v.x,v.y,v.z);
    return {v.x/length,v.y/length,v.z/length};
}
// Match the shared pipeline's exact-zero policy: a determinant rounded to
// zero is insufficient evidence that a narrow triangle can be removed.
bool zero_area(Vec3 a,Vec3 b) {
    const auto sa=(std::max)({std::abs(a.x),std::abs(a.y),std::abs(a.z)});
    const auto sb=(std::max)({std::abs(b.x),std::abs(b.y),std::abs(b.z)});
    if(sa==0 || sb==0) return true;
    int ea=0,eb=0;std::frexp(sa,&ea);std::frexp(sb,&eb);
    const Vec3 u{std::scalbn(a.x,-ea),std::scalbn(a.y,-ea),std::scalbn(a.z,-ea)};
    const Vec3 v{std::scalbn(b.x,-eb),std::scalbn(b.y,-eb),std::scalbn(b.z,-eb)};
    if((a.x!=0&&u.x==0)||(a.y!=0&&u.y==0)||(a.z!=0&&u.z==0)||
        (b.x!=0&&v.x==0)||(b.y!=0&&v.y==0)||(b.z!=0&&v.z==0)) return false;
    const auto determinant_zero=[](double x,double y,double z,double w) {
        const auto p=x*y,q=z*w;
        if(p!=q) return false;
        if(p==0) return (x==0||y==0)&&(z==0||w==0);
        if(std::abs(p)<std::scalbn((std::numeric_limits<double>::min)(),53)) return false;
        return std::fma(x,y,-p)==std::fma(z,w,-q);
    };
    return determinant_zero(u.y,v.z,u.z,v.y)&&determinant_zero(u.z,v.x,u.x,v.z)&&
        determinant_zero(u.x,v.y,u.y,v.x);
}
std::uint32_t u32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1])<<8) |
        (std::uint32_t(p[2])<<16) | (std::uint32_t(p[3])<<24);
}
double f32(const std::uint8_t* p) {
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559,
        "STL decoding requires IEEE 754 binary32.");
    const auto bits=u32(p);float value;std::memcpy(&value,&bits,sizeof(value));return value;
}
Vec3 vector_at(const std::uint8_t* p) { return {f32(p),f32(p+4),f32(p+8)}; }

// The byte buffer is bounded before parsing, and tokens/names have independent
// limits. The parser accepts the STL grammar only: no ignored directives.
class Text {
    std::string_view data;
    std::size_t offset=0;
public:
    explicit Text(std::string_view text):data(text) {
        for(const unsigned char c:data)
            if((c<32 && !space(static_cast<char>(c))) || c>126)
                reject("INVALID_STL","ASCII STL contains a non-ASCII or control byte.","scene");
    }
    bool done() { while(offset<data.size() && space(data[offset])) ++offset;return offset==data.size(); }
    std::string_view token() {
        if(done()) reject("INVALID_STL","Unexpected end of ASCII STL.","scene");
        const auto start=offset;
        while(offset<data.size() && !space(data[offset])) {
            if(offset-start>=256) reject("INVALID_STL","STL token exceeds 256 bytes.","scene");
            ++offset;
        }
        return data.substr(start,offset-start);
    }
    void expect(std::string_view expected) {
        if(token()!=expected) reject("INVALID_STL","Expected STL keyword: "+std::string(expected),"scene");
    }
    std::string line_name() {
        const auto start=offset;
        while(offset<data.size() && data[offset]!='\n' && data[offset]!='\r') {
            if(offset-start>=4096) reject("INVALID_STL","STL solid name exceeds 4096 bytes.","scene");
            ++offset;
        }
        auto name=data.substr(start,offset-start);
        while(!name.empty() && space(name.front())) name.remove_prefix(1);
        while(!name.empty() && space(name.back())) name.remove_suffix(1);
        return std::string(name);
    }
    double number() {
        auto value=token();
        if(!value.empty() && value.front()=='+') {
            value.remove_prefix(1);
            if(!value.empty() && (value.front()=='+' || value.front()=='-'))
                reject("INVALID_STL","STL numeric value has multiple signs.","scene");
        }
        double result=0;
        const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result,std::chars_format::general);
        if(value.empty() || parsed.ec!=std::errc() || parsed.ptr!=value.data()+value.size())
            reject("INVALID_STL","STL numeric value is malformed or outside its numeric range.","scene");
        return result;
    }
    Vec3 vector() { const auto x=number(),y=number(),z=number();return {x,y,z}; }
};

class Facets {
    Scene& scene;
    const ReaderOptions& options;
    Mesh mesh;
    std::size_t decoded=0,repaired=0,removed=0,removed_normals=0;
    Vec3 point(Vec3 p) const {
        if(!finite_vec(p)) reject("NONFINITE_VERTEX","STL vertex contains a nonfinite coordinate.",mesh.name);
        const auto source=p;
        p={p.x*options.mesh_unit_meters,p.y*options.mesh_unit_meters,p.z*options.mesh_unit_meters};
        if((source.x!=0&&p.x==0)||(source.y!=0&&p.y==0)||(source.z!=0&&p.z==0))
            reject("UNREPRESENTABLE_COORDINATE","STL unit conversion underflowed a nonzero coordinate to zero.",mesh.name);
        if(options.mesh_y_up) p=z_up(p);
        if(!finite_vec(p)) reject("NONFINITE_VERTEX","STL coordinate conversion overflowed.",mesh.name);
        return p;
    }
    void warning(const char* code,const std::string& message) {
        scene.diagnostics.push_back({Severity::warning,code,message,mesh.name});
    }
public:
    Facets(Scene& out,const ReaderOptions& opts,std::string name):scene(out),options(opts) {
        mesh.name=std::move(name);mesh.source_node="STL:"+std::to_string(scene.meshes.size());
    }
    void facet(Vec3 normal,std::array<Vec3,3> positions) {
        if(++decoded>corner_limit/3) reject("MESH_TOO_LARGE","STL mesh exceeds the 10000000 corner limit.",mesh.name);
        for(auto& p:positions) p=point(p);
        const auto a=positions[0],b=positions[1],c=positions[2];
        const Vec3 u{b.x-a.x,b.y-a.y,b.z-a.z},v{c.x-a.x,c.y-a.y,c.z-a.z};
        if(!finite_vec(u) || !finite_vec(v)) reject("NONFINITE_VERTEX","STL triangle edge overflowed.",mesh.name);
        const auto cross_product=cross(u,v);
        const auto area2=dot(cross_product,cross_product);
        if(!finite_vec(cross_product) || !finite(area2))
            reject("DEGENERATE_TRIANGLE","STL triangle has nonfinite area.",mesh.name);
        const bool invalid_normal=!finite_vec(normal) || (normal.x==0 && normal.y==0 && normal.z==0);
        if(area2==0) {
            // Only genuinely zero cross products may be removed. A nonzero
            // cross product with underflowed squared area must fail validation.
            if(!options.gis_static || cross_product.x!=0 || cross_product.y!=0 || cross_product.z!=0 ||
                !zero_area(u,v))
                reject("DEGENERATE_TRIANGLE","STL triangle has zero or unrepresentable area.",mesh.name);
            ++removed;if(invalid_normal) removed_normals+=3;return;
        }
        if(invalid_normal) {
            if(!options.gis_static) reject("INVALID_NORMAL","STL facet normal is zero or nonfinite.",mesh.name);
            normal=unit_normal(cross_product);++repaired;
        } else {
            normal=unit_normal(normal);
            if(options.mesh_y_up) normal=z_up(normal);
        }
        Triangle triangle;triangle.material=0;
        for(std::size_t i=0;i<3;++i) {
            triangle.indices[i]=static_cast<std::uint32_t>(mesh.vertices.size());
            Vertex corner;corner.position=positions[i];corner.normal=normal;corner.has_normal=true;
            mesh.vertices.push_back(corner);
        }
        mesh.triangles.push_back(triangle);
    }
    void finish() {
        if(repaired) warning("NORMALS_REPAIRED","GIS static profile rebuilt "+std::to_string(repaired*3)+
            " invalid corner normals across "+std::to_string(repaired)+" triangles.");
        if(removed) warning("DEGENERATE_TRIANGLES_REMOVED","GIS static profile removed "+std::to_string(removed)+
            " strictly zero-area triangles; no area tolerance was used.");
        if(removed_normals) warning("DEGENERATE_NORMALS_DISCARDED","Discarded "+std::to_string(removed_normals)+
            " invalid normals with zero-area triangles.");
        if(mesh.triangles.empty()) reject("EMPTY_MESH","STL solid has no retained triangles.",mesh.name);
        Node node;node.name=mesh.name;node.source_id=mesh.source_node;node.source_world_transform=identity();
        node.meshes.push_back(static_cast<std::uint32_t>(scene.meshes.size()));
        scene.nodes.push_back(std::move(node));scene.meshes.push_back(std::move(mesh));
    }
};

void binary(Scene& scene,const ReaderOptions& options,const std::vector<std::uint8_t>& bytes) {
    const auto count=u32(bytes.data()+80);
    if(count>corner_limit/3) reject("MESH_TOO_LARGE","STL mesh exceeds the 10000000 corner limit.","scene");
    std::string header(reinterpret_cast<const char*>(bytes.data()),80);
    std::transform(header.begin(),header.end(),header.begin(),[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if(header.find("COLOR=")!=std::string::npos || header.find("MATERIAL=")!=std::string::npos ||
        header.find("TEXTURE=")!=std::string::npos)
        reject("UNSUPPORTED_STL_COLOR","Binary STL color/material/texture header extensions are not supported.","scene");
    Facets facets(scene,options,scene.name);
    for(std::uint32_t i=0;i<count;++i) {
        const auto* p=bytes.data()+84+std::size_t(i)*50;
        if(p[48]!=0 || p[49]!=0)
            reject("UNSUPPORTED_STL_ATTRIBUTE","Binary STL facet attributes or color encodings are not supported.","facet "+std::to_string(i));
        facets.facet(vector_at(p),{vector_at(p+12),vector_at(p+24),vector_at(p+36)});
    }
    facets.finish();
}
void ascii(Scene& scene,const ReaderOptions& options,std::string_view bytes) {
    Text text(bytes);
    while(!text.done()) {
        if(scene.meshes.size()>=solid_limit) reject("STL_SOLID_LIMIT","STL exceeds the 100000 solid limit.","scene");
        text.expect("solid");const auto source_name=text.line_name();
        Facets facets(scene,options,source_name.empty()?scene.name:source_name);
        for(;;) {
            const auto command=text.token();
            if(command=="endsolid") {
                const auto end_name=text.line_name();
                if(!end_name.empty() && end_name!=source_name)
                    reject("INVALID_STL","STL endsolid name does not match its solid.",source_name);
                facets.finish();break;
            }
            if(command!="facet") reject("INVALID_STL","Expected STL facet or endsolid.",source_name);
            text.expect("normal");const auto normal=text.vector();
            text.expect("outer");text.expect("loop");
            std::array<Vec3,3> positions{};
            for(auto& position:positions) {text.expect("vertex");position=text.vector();}
            text.expect("endloop");text.expect("endfacet");facets.facet(normal,positions);
        }
    }
}
} // namespace

Scene read_stl(const std::filesystem::path& input,const ReaderOptions& options) {
    Scene scene;const auto path=std::filesystem::absolute(input);scene.name=path.stem().u8string();scene.source=path.u8string();
    scene.conversion_profile=options.gis_static?"gis-static":"strict";
    scene.missing_texture_policy=options.missing_texture_fallback?"material-color":"error";
    try {
        if(!finite(options.mesh_unit_meters) || options.mesh_unit_meters<=0)
            reject("INVALID_MESH_UNITS","Mesh metres-per-unit must be finite and positive.","scene");
        auto bytes=read_bytes(path,options.max_file_bytes);
        scene.materials.push_back({"STL default white",{1,1,1,1},-1,true});
        std::ostringstream unit;unit.imbue(std::locale::classic());
        unit<<std::setprecision(std::numeric_limits<double>::max_digits10)<<options.mesh_unit_meters;
        scene.diagnostics.push_back({Severity::warning,"STL_COORDINATE_ASSUMPTION",
            std::string("STL has no unit/up-axis metadata; source assumed right-handed ")+(options.mesh_y_up?"Y":"Z")+
            "-up with "+unit.str()+" metres per unit, converted once to Z-up metres.","scene"});
        scene.diagnostics.push_back({Severity::warning,"STL_MATERIAL_ASSUMPTION",
            "Standard STL carries no materials or textures; using opaque white, double-sided material.","scene"});
        const auto expected=bytes.size()>=84?84ull+50ull*u32(bytes.data()+80):0;
        if(bytes.size()>=84 && bytes.size()==expected) binary(scene,options,bytes);
        else {
            const std::string_view text(reinterpret_cast<const char*>(bytes.data()),bytes.size());
            auto start=text.find_first_not_of(" \t\r\n\f\v");
            if(start==std::string_view::npos || text.substr(start,5)!="solid" ||
                start+5==text.size() || !space(text[start+5]))
                reject("INVALID_STL","Expected ASCII STL or exact binary length 84 + 50 * facet_count.","scene");
            ascii(scene,options,text);
        }
    } catch(const Issue& e) {scene.diagnostics.push_back({Severity::error,e.code,e.what(),e.context});}
    return scene;
}
} // namespace gmb
