#include "gmb/scene.hpp"
#include "reader_util.hpp"
#include "ufbx.h"
#include <charconv>
#include <cstring>
#include <iomanip>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string_view>

namespace gmb {
namespace {
using namespace reader;
constexpr std::size_t corner_limit=10000000, face_limit=1024, header_limit=1024*1024, line_limit=65536;
[[noreturn]] void bad(const std::string& message,const std::string& context="PLY") {
    reject("INVALID_PLY",message,context);
}
enum class Type { i8,u8,i16,u16,i32,u32,f32,f64 };
Type type(const std::string& name) {
    const std::map<std::string,Type> names={{"char",Type::i8},{"int8",Type::i8},{"uchar",Type::u8},{"uint8",Type::u8},
        {"short",Type::i16},{"int16",Type::i16},{"ushort",Type::u16},{"uint16",Type::u16},
        {"int",Type::i32},{"int32",Type::i32},{"uint",Type::u32},{"uint32",Type::u32},
        {"float",Type::f32},{"float32",Type::f32},{"double",Type::f64},{"float64",Type::f64}};
    const auto found=names.find(name);if(found==names.end()) bad("Unsupported PLY scalar type: "+name);
    return found->second;
}
bool integral(Type t) {return t!=Type::f32&&t!=Type::f64;}
bool signed_type(Type t) {return t==Type::i8||t==Type::i16||t==Type::i32;}
std::size_t width(Type t) {return t==Type::i8||t==Type::u8?1:t==Type::i16||t==Type::u16?2:t==Type::f64?8:4;}
bool space(char c) {return c==' '||c=='\t';}
std::vector<std::string> words(std::string_view line) {
    std::vector<std::string> result;
    for(std::size_t at=0;at<line.size();) {
        while(at<line.size()&&space(line[at])) ++at;
        if(at==line.size()) break;
        auto start=at;while(at<line.size()&&!space(line[at])) ++at;
        result.emplace_back(line.substr(start,at-start));
    }
    return result;
}
std::uint64_t unsigned_integer(std::string_view text) {
    std::uint64_t n=0;const auto p=std::from_chars(text.data(),text.data()+text.size(),n);
    if(text.empty()||p.ec!=std::errc{}||p.ptr!=text.data()+text.size()) bad("Expected an exact nonnegative integer.");
    return n;
}
struct Property {std::string name;Type value=Type::f32,count=Type::u8;bool list=false;};
struct Element {std::string name;std::size_t count=0;std::vector<Property> properties;bool normals=false,colors=false,alpha=false;};
struct PVertex {Vec3 position,normal;std::array<std::uint8_t,4> color{255,255,255,255};bool invalid_normal=false,used=false;};
struct Face {std::vector<std::uint32_t> indices;Vec3 normal;std::array<std::uint8_t,4> color{255,255,255,255};bool invalid_normal=false;};

Vec3 normalized(Vec3 n) {
    const auto s=(std::max)({std::abs(n.x),std::abs(n.y),std::abs(n.z)});
    if(!finite(n.x)||!finite(n.y)||!finite(n.z)||s==0) return {};
    n={n.x/s,n.y/s,n.z/s};const auto length=std::hypot(n.x,n.y,n.z);
    return {n.x/length,n.y/length,n.z/length};
}
bool zero(Vec3 n) {return n.x==0&&n.y==0&&n.z==0;}
// Same exact-zero policy as the FBX reader: equal rounded products alone must
// not turn a very thin, nonzero triangle into a removable degenerate face.
bool zero_area(Vec3 a,Vec3 b) {
    const auto sa=(std::max)({std::abs(a.x),std::abs(a.y),std::abs(a.z)}),sb=(std::max)({std::abs(b.x),std::abs(b.y),std::abs(b.z)});
    if(!finite(sa)||!finite(sb)) reject("NONFINITE_VERTEX","PLY triangle edge is nonfinite.","geometry");
    if(sa==0||sb==0) return true;
    int ea=0,eb=0;std::frexp(sa,&ea);std::frexp(sb,&eb);
    const Vec3 u{std::scalbn(a.x,-ea),std::scalbn(a.y,-ea),std::scalbn(a.z,-ea)},v{std::scalbn(b.x,-eb),std::scalbn(b.y,-eb),std::scalbn(b.z,-eb)};
    if((a.x!=0&&u.x==0)||(a.y!=0&&u.y==0)||(a.z!=0&&u.z==0)||(b.x!=0&&v.x==0)||(b.y!=0&&v.y==0)||(b.z!=0&&v.z==0)) return false;
    const auto determinant_zero=[](double x,double y,double z,double w) {
        const auto p=x*y,q=z*w;if(p!=q) return false;
        if(p==0) return (x==0||y==0)&&(z==0||w==0);
        if(std::abs(p)<std::scalbn((std::numeric_limits<double>::min)(),53)) return false;
        return std::fma(x,y,-p)==std::fma(z,w,-q);
    };
    return determinant_zero(u.y,v.z,u.z,v.y)&&determinant_zero(u.z,v.x,u.x,v.z)&&determinant_zero(u.x,v.y,u.y,v.x);
}
Vec3 face_normal(Vec3 p,Vec3 q,Vec3 r) {
    Vec3 a{q.x-p.x,q.y-p.y,q.z-p.z},b{r.x-p.x,r.y-p.y,r.z-p.z};
    if(zero_area(a,b)) return {};
    const auto sa=(std::max)({std::abs(a.x),std::abs(a.y),std::abs(a.z)}),sb=(std::max)({std::abs(b.x),std::abs(b.y),std::abs(b.z)});
    a={a.x/sa,a.y/sa,a.z/sa};b={b.x/sb,b.y/sb,b.z/sb};
    const auto n=normalized(cross(a,b));
    if(zero(n)) reject("INVALID_PLY_FACE","Cannot reliably normalize a nonzero PLY triangle; export better-conditioned geometry.","geometry");
    return n;
}

class Parser {
    std::string_view bytes;
    std::size_t at=0,metadata=0,triangulated=0;
    std::string newline;
    bool ascii=true,little=true;
    std::vector<Element> elements;
    std::vector<PVertex> vertices;
    std::vector<Face> faces;
    Element vertex_schema,face_schema;
    Scene& out;const ReaderOptions& options;
    void warning(const char* code,const std::string& text) {out.diagnostics.push_back({Severity::warning,code,text,out.name});}
    std::string_view line(bool header) {
        const auto start=at,end=bytes.find_first_of("\r\n",at);
        const auto stop=end==std::string_view::npos?bytes.size():end;
        if(stop-start>line_limit) bad("PLY line exceeds the 64 KiB limit.");
        const auto result=bytes.substr(start,stop-start);at=stop;
        if(end==std::string_view::npos) {if(header) bad("PLY header has no terminating newline.");return result;}
        if(header) {
            if(newline.empty()) newline=bytes[at]=='\r'?(at+1<bytes.size()&&bytes[at+1]=='\n'?"\r\n":"\r"):"\n";
            if(bytes.substr(at,newline.size())!=newline) bad("PLY header newline style changes.");
            at+=newline.size();
        } else {const auto c=bytes[at++];if(c=='\r'&&at<bytes.size()&&bytes[at]=='\n') ++at;}
        return result;
    }
    void schema(Element& element) {
        std::set<std::string> names;
        for(const auto& p:element.properties) {
            if(!names.insert(p.name).second) bad("Duplicate property: "+p.name);
            const bool index=p.name=="vertex_indices"||p.name=="vertex_index";
            const bool coordinate=p.name=="x"||p.name=="y"||p.name=="z";
            const bool normal=p.name=="nx"||p.name=="ny"||p.name=="nz";
            const bool color=p.name=="red"||p.name=="green"||p.name=="blue"||p.name=="alpha";
            if((element.name=="face"&&index)) {
                if(!p.list||!integral(p.count)||!integral(p.value)) bad("Face indices need an integer list and integer count.");
            } else if((element.name=="vertex"&&coordinate)||normal) {
                if(p.list) bad("PLY coordinates and normals must be scalar properties.");
            } else if(color) {
                if(p.list||p.value!=Type::u8) reject("UNSUPPORTED_PLY_COLOR","PLY colors require uchar/uint8 RGB with optional alpha.",element.name);
            } else reject("UNSUPPORTED_PLY_PROPERTY","PLY property is not represented: "+p.name,element.name);
        }
        auto complete=[&](const char* x,const char* y,const char* z) {
            const auto count=names.count(x)+names.count(y)+names.count(z);
            if(count!=0&&count!=3) bad("Incomplete property group: "+std::string(x)+", "+y+", "+z);
            return count==3;
        };
        if(element.name=="vertex"&&!complete("x","y","z")) bad("Vertex XYZ properties are required.");
        if(element.name=="face"&&names.count("vertex_indices")+names.count("vertex_index")!=1) bad("Exactly one face index list is required.");
        element.normals=complete("nx","ny","nz");element.colors=complete("red","green","blue");element.alpha=names.count("alpha")!=0;
        if(element.alpha&&!element.colors) bad("PLY alpha requires complete RGB properties.");
    }
    void header() {
        if(line(true)!="ply") bad("Expected the PLY magic line without a BOM.");
        bool format=false,ended=false;
        while(at<bytes.size()) {
            if(at>header_limit) bad("PLY header exceeds the 1 MiB limit.");
            const auto text=line(true);const auto w=words(text);
            if(w.empty()) bad("Empty PLY header line.");
            for(char c:text) if((static_cast<unsigned char>(c)<32&&c!='\t')||c==127) bad("Control byte in PLY header.");
            if(w[0]=="comment"||w[0]=="obj_info") {
                std::string hint;for(char c:text.substr(w[0].size())) if(std::isalnum(static_cast<unsigned char>(c))) hint.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                for(const auto* token:{"texture","material","transform","matrix","upaxis","metersperunit","metresperunit"})
                    if(hint.find(token)!=std::string::npos) reject("UNSUPPORTED_PLY_METADATA","PLY metadata may declare unsupported rendering or placement semantics.",w[0]);
                ++metadata;continue;
            }
            if(w[0]=="format") {
                if(format||!elements.empty()||w.size()!=3||w[2]!="1.0") bad("Expected exactly one PLY 1.0 format declaration before elements.");
                if(w[1]=="ascii") ascii=true;
                else if(w[1]=="binary_little_endian"||w[1]=="binary_big_endian") {ascii=false;little=w[1]=="binary_little_endian";}
                else bad("Unsupported PLY encoding.");
                format=true;
            } else if(w[0]=="element") {
                if(!format||w.size()!=3) bad("Invalid element declaration.");
                if(w[1]!="vertex"&&w[1]!="face") reject("UNSUPPORTED_PLY_ELEMENT","PLY element is not represented: "+w[1],"header");
                if(std::any_of(elements.begin(),elements.end(),[&](const auto& e){return e.name==w[1];})) bad("Duplicate PLY element.");
                const auto n=unsigned_integer(w[2]);if(n==0||n>corner_limit) bad("PLY element count must be in 1..10000000.");
                elements.push_back({w[1],static_cast<std::size_t>(n),{}});
            } else if(w[0]=="property") {
                if(elements.empty()||(w.size()!=3&&w.size()!=5)) bad("Invalid property declaration.");
                Property p;
                if(w.size()==5) {if(w[1]!="list") bad("Invalid list property declaration.");p.list=true;p.count=type(w[2]);p.value=type(w[3]);p.name=w[4];}
                else {p.value=type(w[1]);p.name=w[2];}
                if(elements.back().properties.size()>=11) bad("Too many PLY properties.");
                elements.back().properties.push_back(std::move(p));
            } else if(w[0]=="end_header") {if(w.size()!=1) bad("Extra fields after end_header.");ended=true;break;}
            else reject("UNSUPPORTED_PLY_HEADER","Unknown PLY header directive: "+w[0],"header");
        }
        if(!format||!ended||elements.size()!=2||at>header_limit) bad("Incomplete or oversized PLY header; vertex and face elements are required.");
        for(auto& e:elements) {schema(e);if(e.name=="vertex") vertex_schema=e;else face_schema=e;}
        if(vertex_schema.normals&&face_schema.normals) reject("UNSUPPORTED_PLY_NORMAL_BINDING","Simultaneous face and vertex normals have no unambiguous precedence.","header");
        std::uint64_t minimum=0;
        for(const auto& e:elements) {
            std::size_t record=0;for(const auto& p:e.properties) record+=ascii?(p.list?7:2):(p.list?width(p.count)+3*width(p.value):width(p.value));
            // ASCII final lines can lack a newline: this lower bound counts only one byte per scalar.
            if(ascii) record=(record+1)/2;
            minimum+=e.count*record;
        }
        if(minimum>bytes.size()-at) bad("Declared PLY record counts exceed the remaining payload.");
    }
    double scalar(Type t,std::string_view token={}) {
        if(ascii) {
            if(token.empty()) bad("Missing PLY scalar.");
            if(token[0]=='+') {token.remove_prefix(1);if(token.empty()||token[0]=='+'||token[0]=='-') bad("Invalid numeric sign.");}
            if(integral(t)) {
                std::int64_t n=0;const auto p=std::from_chars(token.data(),token.data()+token.size(),n);
                const auto bits=width(t)*8;const auto high=signed_type(t)?(std::int64_t{1}<<(bits-1))-1:(std::int64_t{1}<<bits)-1;
                const auto low=signed_type(t)?-(std::int64_t{1}<<(bits-1)):0;
                if(p.ec!=std::errc{}||p.ptr!=token.data()+token.size()||n<low||n>high) bad("PLY integer is invalid or outside its declared type.");
                return static_cast<double>(n);
            }
            if(t==Type::f32) {float n=0;const auto p=std::from_chars(token.data(),token.data()+token.size(),n,std::chars_format::general);if(p.ec!=std::errc{}||p.ptr!=token.data()+token.size()) bad("Invalid float32 value.");return n;}
            double n=0;const auto p=std::from_chars(token.data(),token.data()+token.size(),n,std::chars_format::general);if(p.ec!=std::errc{}||p.ptr!=token.data()+token.size()) bad("Invalid float64 value.");return n;
        }
        const auto n=width(t);if(n>bytes.size()-at) bad("Truncated binary PLY scalar.");
        std::uint64_t bits=0;
        for(std::size_t i=0;i<n;++i) bits|=std::uint64_t(static_cast<unsigned char>(bytes[at+i]))<<(8*(little?i:n-1-i));
        at+=n;
        if(t==Type::f32) {static_assert(sizeof(float)==4&&std::numeric_limits<float>::is_iec559);const auto word=static_cast<std::uint32_t>(bits);float v;std::memcpy(&v,&word,4);return v;}
        if(t==Type::f64) {static_assert(sizeof(double)==8&&std::numeric_limits<double>::is_iec559);double v;std::memcpy(&v,&bits,8);return v;}
        return signed_type(t)&&(bits&(std::uint64_t{1}<<(n*8-1)))?static_cast<double>(bits)-std::ldexp(1.0,static_cast<int>(n*8)):static_cast<double>(bits);
    }
    Vec3 position(Vec3 p) {
        if(!finite(p.x)||!finite(p.y)||!finite(p.z)) reject("NONFINITE_VERTEX","PLY position is nonfinite, including unreferenced vertices.","vertex");
        if(options.mesh_y_up) p=z_up(p);
        const auto source=p;
        p={p.x*options.mesh_unit_meters,p.y*options.mesh_unit_meters,p.z*options.mesh_unit_meters};
        if(!finite(p.x)||!finite(p.y)||!finite(p.z)) reject("NONFINITE_VERTEX","PLY unit conversion produced a nonfinite position.","vertex");
        if((source.x!=0&&p.x==0)||(source.y!=0&&p.y==0)||(source.z!=0&&p.z==0))
            reject("UNREPRESENTABLE_COORDINATE","PLY unit conversion underflowed a nonzero coordinate; geometry cannot be preserved.","vertex");
        return p;
    }
    void records() {
        for(const auto& element:elements) for(std::size_t row=0;row<element.count;++row) {
            if(at==bytes.size()) bad("Truncated PLY element records.");
            const auto values=ascii?words(line(false)):std::vector<std::string>{};std::size_t token=0;
            auto read=[&](Type t){if(ascii&&token>=values.size()) bad("Missing value in ASCII PLY record.");return scalar(t,ascii?std::string_view(values[token++]):std::string_view{});};
            PVertex v;Face f;
            for(const auto& p:element.properties) {
                if(p.list) {
                    const auto count=read(p.count);if(count<3||count>face_limit) reject("INVALID_PLY_FACE","PLY faces must have 3..1024 corners.","face:"+std::to_string(row));
                    const auto n=static_cast<std::size_t>(count);
                    if((n-2)*3>corner_limit-triangulated) reject("PLY_GEOMETRY_LIMIT","Triangulated PLY exceeds the 10000000 corner limit.","face");
                    triangulated+=(n-2)*3;f.indices.reserve(n);
                    for(std::size_t i=0;i<n;++i) {const auto index=read(p.value);if(index<0||index>=static_cast<double>(vertex_schema.count)) bad("PLY face index is out of range.");f.indices.push_back(static_cast<std::uint32_t>(index));}
                    continue;
                }
                const auto x=read(p.value);
                if(p.name=="x") v.position.x=x;else if(p.name=="y") v.position.y=x;else if(p.name=="z") v.position.z=x;
                else if(p.name=="nx") v.normal.x=x;else if(p.name=="ny") v.normal.y=x;else if(p.name=="nz") v.normal.z=x;
                else v.color[p.name=="red"?0:p.name=="green"?1:p.name=="blue"?2:3]=static_cast<std::uint8_t>(x);
            }
            if(ascii&&token!=values.size()) bad("Extra fields in ASCII PLY record.");
            if(element.normals) {
                v.normal=normalized(v.normal);v.invalid_normal=zero(v.normal);
                if(v.invalid_normal&&!options.gis_static) reject("INVALID_NORMAL","PLY explicit normal is zero or nonfinite, including unused normals.",element.name+":"+std::to_string(row));
                if(options.mesh_y_up) v.normal=z_up(v.normal);
            }
            if(element.name=="vertex") {v.position=position(v.position);vertices.push_back(v);}
            else {f.normal=v.normal;f.invalid_normal=v.invalid_normal;f.color=v.color;faces.push_back(std::move(f));}
        }
        if(ascii) while(at<bytes.size()&&(space(bytes[at])||bytes[at]=='\r'||bytes[at]=='\n')) ++at;
        if(at!=bytes.size()) bad("Unexpected trailing PLY bytes or records.");
    }
    std::vector<std::uint32_t> polygon(const std::vector<Vertex>& corners,std::uint64_t& work) {
        const auto count=corners.size();if(count==3) return {0,1,2};
        work+=count*(count-1)/2;if(work>50000000) reject("PLY_COMPLEXITY_LIMIT","Polygon intersection checks exceed the bounded work limit.","face");
        const auto base=corners[0].position;double scale=0;
        for(const auto& v:corners) scale=(std::max)({scale,std::abs(v.position.x-base.x),std::abs(v.position.y-base.y),std::abs(v.position.z-base.z)});
        if(!finite(scale)||scale==0) reject("INVALID_PLY_FACE","Polygon has invalid extent; export explicit triangles.","face");
        std::vector<Vec3> points;for(const auto& v:corners) points.push_back({(v.position.x-base.x)/scale,(v.position.y-base.y)/scale,(v.position.z-base.z)/scale});
        Vec3 normal;for(std::size_t i=1;i+1<count&&zero(normal);++i) normal=face_normal({},points[i],points[i+1]);
        if(zero(normal)) reject("INVALID_PLY_FACE","Polygon has no plane; export explicit triangles.","face");
        const int drop=std::abs(normal.x)>=std::abs(normal.y)&&std::abs(normal.x)>=std::abs(normal.z)?0:std::abs(normal.y)>=std::abs(normal.z)?1:2;
        std::vector<Vec2> flat;
        for(const auto& p:points) {if(std::abs(dot(normal,p))>1e-10) reject("INVALID_PLY_FACE","Nonplanar polygon must be triangulated in the exporter.","face");flat.push_back(drop==0?Vec2{p.y,p.z}:drop==1?Vec2{p.x,p.z}:Vec2{p.x,p.y});}
        const auto orient=[](Vec2 a,Vec2 b,Vec2 c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);};
        const auto on=[](Vec2 a,Vec2 b,Vec2 c){return c.x>=(std::min)(a.x,b.x)&&c.x<=(std::max)(a.x,b.x)&&c.y>=(std::min)(a.y,b.y)&&c.y<=(std::max)(a.y,b.y);};
        for(std::size_t i=0;i<count;++i) {
            const auto a=flat[i],b=flat[(i+1)%count];if(a.x==b.x&&a.y==b.y) reject("INVALID_PLY_FACE","Repeated polygon corner.","face");
            for(std::size_t j=i+1;j<count;++j) {
                if(j==i+1||(i==0&&j+1==count)) continue;
                const auto c=flat[j],d=flat[(j+1)%count];const auto x=orient(a,b,c),y=orient(a,b,d),z=orient(c,d,a),w=orient(c,d,b);
                if((((x<0&&y>0)||(x>0&&y<0))&&((z<0&&w>0)||(z>0&&w<0)))||
                    (x==0&&on(a,b,c))||(y==0&&on(a,b,d))||(z==0&&on(c,d,a))||(w==0&&on(c,d,b)))
                    reject("INVALID_PLY_FACE","Polygon edges cross, touch or overlap.","face");
            }
        }
        std::vector<ufbx_vec3> positions;for(auto p:points) positions.push_back({p.x,p.y,p.z});
        std::vector<std::uint32_t> indices(count),result((count-2)*3);std::iota(indices.begin(),indices.end(),0);
        ufbx_mesh mesh{};mesh.num_indices=count;mesh.vertex_position.exists=true;
        mesh.vertex_position.values={positions.data(),positions.size()};mesh.vertex_position.indices={indices.data(),indices.size()};ufbx_panic panic{};
        if(ufbx_catch_triangulate_face(&panic,result.data(),result.size(),&mesh,{0,static_cast<std::uint32_t>(count)})!=count-2||panic.did_panic)
            reject("INVALID_PLY_FACE","Polygon triangulation failed.","face");
        return result;
    }
    void mesh() {
        Mesh result;result.name=out.name;result.source_node="PLY:object";
        std::map<std::array<std::uint8_t,4>,int> materials;
        std::size_t generated=0,repaired=0,repaired_triangles=0,discarded=0,discarded_normals=0;std::uint64_t work=0;
        for(const auto& face:faces) {
            auto color=face.color;
            if(vertex_schema.colors) {
                color=vertices[face.indices[0]].color;
                for(auto index:face.indices) if(vertices[index].color!=color)
                    reject("UNSUPPORTED_PLY_VERTEX_COLOR","Varying colors within a PLY face require baking; no color averaging is performed.","face");
                if(face_schema.colors&&face.color!=color) reject("UNSUPPORTED_PLY_COLOR","Face and vertex colors disagree; neither binding may be silently dropped.","face");
            }
            auto found=materials.find(color);
            if(found==materials.end()) {
                if(materials.size()>=100000) reject("PLY_MATERIAL_LIMIT","PLY exceeds the 100000 distinct material limit.","materials");
                const int index=static_cast<int>(out.materials.size());found=materials.emplace(color,index).first;
                Material material;material.name="PLY color "+std::to_string(index);material.color={color[0]/255.,color[1]/255.,color[2]/255.,color[3]/255.};out.materials.push_back(std::move(material));
            }
            std::vector<Vertex> corners;
            for(auto index:face.indices) {auto& source=vertices[index];source.used=true;Vertex v;v.position=source.position;v.has_normal=vertex_schema.normals||face_schema.normals;v.normal=vertex_schema.normals?source.normal:face.normal;corners.push_back(v);}
            const auto order=polygon(corners,work);
            for(std::size_t i=0;i<order.size();i+=3) {
                std::array<Vertex,3> tri{corners[order[i]],corners[order[i+1]],corners[order[i+2]]};
                const auto normal=face_normal(tri[0].position,tri[1].position,tri[2].position);
                if(zero(normal)) {
                    if(!options.gis_static) reject("DEGENERATE_TRIANGLE","PLY triangle has exactly zero area.",result.name);
                    ++discarded;for(const auto& v:tri) if(v.has_normal&&zero(v.normal)) ++discarded_normals;continue;
                }
                Triangle triangle;triangle.material=found->second;bool repair=false;
                for(std::size_t k=0;k<3;++k) {
                    auto& v=tri[k];
                    if(!v.has_normal) {v.normal=normal;v.has_normal=true;++generated;}
                    else if(zero(v.normal)) {v.normal=normal;++repaired;repair=true;}
                    triangle.indices[k]=static_cast<std::uint32_t>(result.vertices.size());result.vertices.push_back(v);
                }
                if(repair) ++repaired_triangles;
                result.triangles.push_back(triangle);
            }
        }
        std::size_t unused=0;for(const auto& v:vertices) if(!v.used) {
            ++unused;if(v.invalid_normal) reject("INVALID_NORMAL","Unused PLY normal is invalid; no retained triangle can repair it.","vertex");
        }
        if(unused) warning("PLY_UNUSED_VERTICES","Validated "+std::to_string(unused)+" unreferenced vertices; they do not contribute rendered geometry.");
        if(generated) warning("PLY_DEFAULT_NORMALS_GENERATED","PLY has no explicit normals; generated flat normals for "+std::to_string(generated)+" corners.");
        if(repaired) warning("NORMALS_REPAIRED","GIS static profile rebuilt "+std::to_string(repaired)+" invalid corner normals across "+std::to_string(repaired_triangles)+" triangles.");
        if(discarded) warning("DEGENERATE_TRIANGLES_REMOVED","GIS static profile removed "+std::to_string(discarded)+" strictly zero-area triangles; no area tolerance was used.");
        if(discarded_normals) warning("DEGENERATE_NORMALS_DISCARDED","Discarded "+std::to_string(discarded_normals)+" invalid normals with removed zero-area triangles.");
        if(result.triangles.empty()) reject("EMPTY_MESH","No retained PLY triangles.",result.name);
        Node node;node.name=out.name;node.source_id="PLY:object";node.source_world_transform=identity();node.meshes.push_back(0);
        out.nodes.push_back(std::move(node));out.meshes.push_back(std::move(result));
    }
public:
    Parser(std::string_view input,Scene& scene,const ReaderOptions& settings):bytes(input),out(scene),options(settings) {}
    void parse() {
        if(!finite(options.mesh_unit_meters)||options.mesh_unit_meters<=0) bad("PLY metres per source unit must be finite and positive.");
        header();records();
        std::ostringstream units;units<<std::setprecision(17)<<options.mesh_unit_meters;
        warning("PLY_COORDINATE_ASSUMPTION","PLY has no standard axis/unit metadata; interpreted as right-handed "+std::string(options.mesh_y_up?"Y":"Z")+"-up, "+units.str()+" metres per source unit; converted once to right-handed Z-up metres.");
        if(metadata) warning("PLY_METADATA_OMITTED","Omitted "+std::to_string(metadata)+" comment/obj_info records as descriptive metadata; no rendering or placement directives are interpreted.");
        warning(vertex_schema.colors||face_schema.colors?"PLY_COLOR_MAPPED":"PLY_DEFAULT_MATERIAL",vertex_schema.colors||face_schema.colors?
            "PLY uchar RGB/alpha is retained as uniform per-face diffuse color/scalar opacity; materials are double-sided. Interpolated or conflicting colors are rejected.":
            "Uncolored PLY uses an opaque white, double-sided diffuse material; no source material is declared.");
        mesh();
    }
};
} // namespace
Scene read_ply(const std::filesystem::path& input,const ReaderOptions& options) {
    const auto path=std::filesystem::absolute(input);Scene out;out.name=path.stem().u8string();out.source=path.u8string();
    out.conversion_profile=options.gis_static?"gis-static":"strict";out.missing_texture_policy=options.missing_texture_fallback?"material-color":"error";
    try {const auto bytes=read_bytes(path,options.max_file_bytes);Parser({reinterpret_cast<const char*>(bytes.data()),bytes.size()},out,options).parse();}
    catch(const Issue& e) {out.diagnostics.push_back({Severity::error,e.code,e.what(),e.context});}
    return out;
}
} // namespace gmb
