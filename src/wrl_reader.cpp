#include "gmb/scene.hpp"
#include "reader_util.hpp"
#include "ufbx.h"
#include <charconv>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string_view>

namespace gmb {
namespace {
using namespace reader;
struct WNode;
using Ref=std::shared_ptr<WNode>;
struct Field { std::vector<double> numbers; std::vector<Ref> nodes; std::vector<std::string> strings; };
struct WNode { std::string type,name; std::map<std::string,Field> fields; };
using Schema=std::map<std::string,std::string>;
const std::map<std::string,Schema> schemas={
    {"Group",{{"children","nodes"},{"bboxCenter","3"},{"bboxSize","3"}}},
    {"Transform",{{"children","nodes"},{"bboxCenter","3"},{"bboxSize","3"},
        {"translation","3"},{"rotation","4"},{"scale","3"},{"center","3"},{"scaleOrientation","4"}}},
    {"Shape",{{"appearance","node"},{"geometry","node"}}},
    {"Appearance",{{"material","node"},{"texture","node"},{"textureTransform","node"}}},
    {"Material",{{"diffuseColor","3"},{"emissiveColor","3"},{"specularColor","3"},
        {"ambientIntensity","1"},{"shininess","1"},{"transparency","1"}}},
    {"ImageTexture",{{"url","strings"},{"repeatS","bool"},{"repeatT","bool"}}},
    {"TextureTransform",{{"center","2"},{"rotation","1"},{"scale","2"},{"translation","2"}}},
    {"IndexedFaceSet",{{"coord","node"},{"normal","node"},{"texCoord","node"},{"color","node"},
        {"coordIndex","ints"},{"normalIndex","ints"},{"texCoordIndex","ints"},{"colorIndex","ints"},
        {"ccw","bool"},{"solid","bool"},{"convex","bool"},{"normalPerVertex","bool"},
        {"colorPerVertex","bool"},{"creaseAngle","1"}}},
    {"Coordinate",{{"point","multi3"}}}, {"Normal",{{"vector","multi3"}}},
    {"TextureCoordinate",{{"point","multi2"}}}, {"Color",{{"color","multi3"}}},
    {"WorldInfo",{{"title","string"},{"info","strings"}}}
};

// A bounded VRML97 parser. Unknown nodes/fields are errors, including in unused DEFs.
// USE only refers to a completed preceding definition, so cyclic graphs cannot form.
class Parser {
    std::string_view source;
    std::size_t at=0,line=1,node_count=0;
    std::string token;
    bool quoted=false;
    std::map<std::string,Ref> definitions;
    [[noreturn]] void bad(const std::string& text) const {
        reject("INVALID_WRL",text,"line:"+std::to_string(line));
    }
    void next() {
        token.clear(); quoted=false;
        while (at<source.size()) {
            const auto c=source[at];
            if (c=='#') { while (at<source.size() && source[at]!='\n') ++at; }
            else if (std::isspace(static_cast<unsigned char>(c)) || c==',') { if(c=='\n') ++line; ++at; }
            else break;
        }
        if (at==source.size()) return;
        char c=source[at++];
        if (c=='"') {
            quoted=true;
            while (at<source.size()) {
                c=source[at++];
                if (c=='"') return;
                if (c=='\\') {
                    if (at==source.size()) bad("Unterminated string escape.");
                    c=source[at++];
                    if (c!='"' && c!='\\') bad("Unsupported string escape.");
                }
                if (c=='\n') ++line;
                if (c=='\0') bad("NUL in string.");
                token.push_back(c);
            }
            bad("Unterminated string.");
        }
        token.push_back(c);
        if (c=='{'||c=='}'||c=='['||c==']') return;
        while (at<source.size()) {
            c=source[at];
            if (std::isspace(static_cast<unsigned char>(c))||c==','||c=='#'||c=='{'||c=='}'||c=='['||c==']') break;
            if (c=='"'||c=='\0') bad("Invalid token.");
            token.push_back(c); ++at;
        }
        if (token.size()>1048576) bad("Token exceeds the parser limit.");
    }
    bool is(const char* s) const {return !quoted && token==s;}
    void take(const char* s) {if (!is(s)) bad(std::string("Expected ")+s); next();}
    double number(bool integer) {
        if (quoted || token.empty()) bad("Expected a number.");
        double value=0;
        const char* start=token.data();
        if (*start=='+') ++start;
        const auto parsed=std::from_chars(start,token.data()+token.size(),value);
        if (parsed.ec!=std::errc{} || parsed.ptr!=token.data()+token.size() || !finite(value))
            bad("Expected a finite decimal number.");
        if (integer && (value!=std::floor(value)||value < -1||value>2147483647)) bad("Invalid face index.");
        next(); return value;
    }
    Ref node(std::size_t depth) {
        if (depth>128 || ++node_count>100000) bad("Node depth/count limit exceeded.");
        if (is("NULL")) {next();return {};}
        if (is("USE")) {
            next(); const auto found=definitions.find(token);
            if (quoted || found==definitions.end()) bad("USE must reference a completed DEF.");
            auto result=found->second; next();return result;
        }
        std::string name;
        if (is("DEF")) {
            next(); name=token;
            if (quoted || name.empty() || name.find_first_of("{}[]")!=std::string::npos) bad("Invalid DEF name.");
            next();
        }
        const auto schema=schemas.find(token);
        if (quoted || schema==schemas.end()) reject("UNSUPPORTED_WRL_NODE","Unsupported VRML97 node or statement: "+token,"line:"+std::to_string(line));
        auto out=std::make_shared<WNode>();out->type=token;out->name=name;
        next();take("{");
        while (!is("}")) {
            const auto field_name=token;
            const auto field=schema->second.find(field_name);
            if (quoted || field==schema->second.end()) reject("UNSUPPORTED_WRL_FIELD","Unsupported field: "+out->type+"."+field_name,"line:"+std::to_string(line));
            if (out->fields.count(field_name)) bad("Duplicate field.");
            next(); Field f;const auto& kind=field->second;
            if (kind=="node") f.nodes.push_back(node(depth+1));
            else if (kind=="nodes") {
                const bool array=is("[");if(array) next();
                do {if(array && is("]")) break; f.nodes.push_back(node(depth+1));} while(array);
                if(array) take("]");
            } else if (kind=="string"||kind=="strings") {
                const bool array=kind=="strings" && is("[");if(array) next();
                do {if(array && is("]")) break; if(!quoted) bad("Expected quoted string.");f.strings.push_back(token);next();}while(array);
                if(array) take("]");
            } else if (kind=="bool") {
                if(!is("TRUE")&&!is("FALSE")) bad("Expected TRUE or FALSE.");
                f.numbers.push_back(is("TRUE")?1:0);next();
            } else {
                const bool multi=kind=="ints"||kind.rfind("multi",0)==0;
                const int width=kind=="ints"?1:std::stoi(multi?kind.substr(5):kind);
                const bool array=multi && is("[");if(array) next();
                do {
                    if(array && is("]")) break;
                    if(f.numbers.size()>=30000000) bad("Numeric field exceeds its size limit.");
                    for(int i=0;i<width;++i) f.numbers.push_back(number(kind=="ints"));
                }while(array);
                if(array) take("]");
            }
            out->fields.emplace(field_name,std::move(f));
        }
        next();if(!name.empty()) definitions[name]=out;return out;
    }
public:
    explicit Parser(std::string_view text):source(text) {next();}
    std::vector<Ref> parse() {std::vector<Ref> result;while(!token.empty()||quoted) result.push_back(node(0));return result;}
};
const Field& field(const Ref& n,const char* key) {
    static const Field empty;
    if(!n) return empty;
    const auto i=n->fields.find(key);return i==n->fields.end()?empty:i->second;
}
Ref child(const Ref& n,const char* key,const char* type) {
    const auto& children=field(n,key).nodes;
    if(children.empty()||!children.front()) return {};
    auto result=children.front();
    if(result->type!=type) reject("INVALID_WRL_NODE_TYPE",std::string(key)+" requires "+type,"node:"+n->name);
    return result;
}
double scalar(const Ref& n,const char* key,double fallback) {
    const auto& values=field(n,key).numbers;return values.empty()?fallback:values.front();
}
Vec3 vector3(const Ref& n,const char* key,Vec3 fallback) {
    const auto& v=field(n,key).numbers;return v.empty()?fallback:Vec3{v[0],v[1],v[2]};
}
Vec2 vector2(const Ref& n,const char* key,Vec2 fallback) {
    const auto& v=field(n,key).numbers;return v.empty()?fallback:Vec2{v[0],v[1]};
}
using Matrix=std::array<double,16>;
Matrix translation(Vec3 p) {auto m=identity();m[12]=p.x;m[13]=p.y;m[14]=p.z;return m;}
Matrix rotation(const Ref& n,const char* key,bool inverse=false) {
    const auto& v=field(n,key).numbers;if(v.empty()) return identity();
    const double length=std::hypot(v[0],v[1],v[2]);
    if(length==0) reject("INVALID_TRANSFORM","Rotation axis has zero length.",n->name);
    const double x=v[0]/length,y=v[1]/length,z=v[2]/length,a=inverse?-v[3]:v[3],c=std::cos(a),s=std::sin(a),t=1-c;
    return {t*x*x+c,t*x*y+s*z,t*x*z-s*y,0,t*x*y-s*z,t*y*y+c,t*y*z+s*x,0,
        t*x*z+s*y,t*y*z-s*x,t*z*z+c,0,0,0,0,1};
}
Vec3 subtract(Vec3 a,Vec3 b) {return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 face_normal(Vec3 a,Vec3 b,Vec3 c) {
    auto u=subtract(b,a),v=subtract(c,a);
    const double scale=(std::max)({std::abs(u.x),std::abs(u.y),std::abs(u.z),std::abs(v.x),std::abs(v.y),std::abs(v.z)});
    if(!finite(scale)) reject("NONFINITE_VERTEX","Transformed edge is nonfinite.","geometry");
    if(!scale) return {};
    u={u.x/scale,u.y/scale,u.z/scale};v={v.x/scale,v.y/scale,v.z/scale};
    const auto f=cross(u,v);const auto length=std::hypot(f.x,f.y,f.z);
    return length?Vec3{f.x/length,f.y/length,f.z/length}:Vec3{};
}
using Faces=std::vector<std::vector<std::size_t>>;
Faces faces(const std::vector<double>& indices) {
    Faces result;std::vector<std::size_t> face;
    for(double index:indices) {
        if(index==-1) {if(face.empty()) reject("INVALID_WRL_INDICES","Empty face index list.","geometry");result.push_back(std::move(face));face.clear();}
        else face.push_back(static_cast<std::size_t>(index));
    }
    if(!face.empty()) result.push_back(std::move(face));
    return result;
}
void validate_polygon(const std::vector<ufbx_vec3>& points) {
    if(points.size()==3) return; // Finite zero-area triangles use the profile policy below.
    const Vec3 origin{points[0].x,points[0].y,points[0].z};
    double scale=0;
    for(auto p:points) scale=(std::max)({scale,std::abs(p.x-origin.x),std::abs(p.y-origin.y),std::abs(p.z-origin.z)});
    if(!finite(scale)||scale==0) reject("INVALID_WRL_FACE","Polygon has invalid extent.","geometry");
    std::vector<Vec3> normalized;
    for(auto p:points) normalized.push_back({(p.x-origin.x)/scale,(p.y-origin.y)/scale,(p.z-origin.z)/scale});
    Vec3 n{};
    for(std::size_t i=1;i+1<points.size();++i) {
        n=face_normal({},normalized[i],normalized[i+1]);
        if(std::hypot(n.x,n.y,n.z)>0) break;
    }
    if(std::hypot(n.x,n.y,n.z)==0) reject("INVALID_WRL_FACE","Polygon has no plane.","geometry");
    const int drop=std::abs(n.x)>=std::abs(n.y)&&std::abs(n.x)>=std::abs(n.z)?0:std::abs(n.y)>=std::abs(n.z)?1:2;
    std::vector<Vec2> p;
    for(auto v:normalized) {
        if(std::abs(dot(n,v))>1e-10) reject("INVALID_WRL_FACE","Nonplanar polygon must be triangulated by the exporter.","geometry");
        p.push_back(drop==0?Vec2{v.y,v.z}:drop==1?Vec2{v.x,v.z}:Vec2{v.x,v.y});
    }
    auto orient=[](Vec2 a,Vec2 b,Vec2 c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);};
    auto within=[](Vec2 a,Vec2 b,Vec2 c){return c.x>=(std::min)(a.x,b.x)&&c.x<=(std::max)(a.x,b.x)&&c.y>=(std::min)(a.y,b.y)&&c.y<=(std::max)(a.y,b.y);};
    for(std::size_t i=0;i<p.size();++i) {
        const auto a=p[i],b=p[(i+1)%p.size()];
        if(a.x==b.x&&a.y==b.y) reject("INVALID_WRL_FACE","Repeated polygon corner.","geometry");
        for(std::size_t j=i+1;j<p.size();++j) {
            if(j==i+1||(i==0&&j+1==p.size())) continue;
            const auto c=p[j],d=p[(j+1)%p.size()];
            const double x=orient(a,b,c),y=orient(a,b,d),z=orient(c,d,a),w=orient(c,d,b);
            if(((x<0&&y>0)||(x>0&&y<0))&&((z<0&&w>0)||(z>0&&w<0)))
                reject("INVALID_WRL_FACE","Self-intersecting polygon.","geometry");
            if((x==0&&within(a,b,c))||(y==0&&within(a,b,d))||(z==0&&within(c,d,a))||(w==0&&within(c,d,b)))
                reject("INVALID_WRL_FACE","Polygon edges touch or overlap.","geometry");
        }
    }
}
std::size_t binding(const Faces& indices,const std::vector<double>& flat,bool per_vertex,
    const Faces& coords,std::size_t face,std::size_t corner,std::size_t count) {
    std::size_t index;
    if(flat.empty()) index=per_vertex?coords[face][corner]:face;
    else if(per_vertex) {
        if(indices.size()!=coords.size()||indices[face].size()!=coords[face].size())
            reject("INVALID_WRL_INDICES","Corner binding does not match coordIndex face boundaries.","geometry");
        index=indices[face][corner];
    } else {
        if(flat.size()<coords.size()||flat[face]<0) reject("INVALID_WRL_INDICES","Missing per-face binding.","geometry");
        index=static_cast<std::size_t>(flat[face]);
    }
    if(index>=count) reject("INVALID_WRL_INDICES","Attribute index is outside its array.","geometry");
    return index;
}

// VRML97 RGB replaces diffuse colour; image alpha replaces material transparency.
// Identify source component semantics without recompressing image bytes.
std::pair<bool,bool> image_semantics(const Texture& t) {
    const auto& b=t.bytes;
    if(t.mime_type=="image/png" && b.size()>=33) {
        const auto type=b[25];bool gray=type==0||type==4,alpha=type==4||type==6;
        if(type!=0&&type!=2&&type!=3&&type!=4&&type!=6) reject("TEXTURE_READ_ERROR","Invalid PNG color type.",t.source);
        bool palette=false;if(type==3) gray=true;
        for(std::size_t at=8;at+12<=b.size();) {
            const std::size_t size=(std::size_t(b[at])<<24)|(std::size_t(b[at+1])<<16)|(std::size_t(b[at+2])<<8)|b[at+3];
            if(size>b.size()-at-12) reject("TEXTURE_READ_ERROR","Truncated PNG chunk.",t.source);
            const std::string tag(b.begin()+at+4,b.begin()+at+8);
            if(tag=="tRNS") alpha=true;
            if(tag=="PLTE" && type==3) {
                if(size==0||size%3) reject("TEXTURE_READ_ERROR","Invalid PNG palette.",t.source);
                palette=true;for(std::size_t j=at+8;j<at+8+size;j+=3) gray=gray&&(b[j]==b[j+1]&&b[j]==b[j+2]);
            }
            at+=size+12;
        }
        if(type==3&&!palette) reject("TEXTURE_READ_ERROR","Missing PNG palette.",t.source);
        return {gray,alpha};
    }
    if(t.mime_type=="image/jpeg") {
        for(std::size_t at=2;at+4<b.size();) {
            if(b[at++]!=255) break;
            while(at<b.size()&&b[at]==255) ++at;
            if(at>=b.size()) break;
            const auto marker=b[at++];if(marker==0xda||marker==0xd9) break;
            if(at+2>b.size()) break;
            const std::size_t size=(std::size_t(b[at])<<8)|b[at+1];
            if(size<2||size>b.size()-at) break;
            if(marker==0xc0||marker==0xc1||marker==0xc2) {
                if(size<8||(b[at+7]!=1&&b[at+7]!=3)) break;
                return {b[at+7]==1,false};
            }
            at+=size;
        }
    }
    reject("TEXTURE_READ_ERROR","Cannot determine VRML image component semantics.",t.source);
}

class Converter {
    Scene& out;const ReaderOptions& options;std::filesystem::path root;
    std::size_t expanded=0;
    void warning(const char* code,const std::string& text,const std::string& ctx) {out.diagnostics.push_back({Severity::warning,code,text,ctx});}
    void shape(const Ref& source,int node_index,const Matrix& world) {
        const auto geometry=child(source,"geometry","IndexedFaceSet");
        if(!geometry) {warning("EMPTY_WRL_SHAPE","Shape has no geometry.",source->name);return;}
        const auto coord=child(geometry,"coord","Coordinate"),normals=child(geometry,"normal","Normal"),
            uv=child(geometry,"texCoord","TextureCoordinate"),colors=child(geometry,"color","Color");
        const auto& positions=field(coord,"point").numbers;
        const auto cf=faces(field(geometry,"coordIndex").numbers);
        if(!coord||positions.empty()||cf.empty()) reject("EMPTY_MESH","IndexedFaceSet has no coordinates or faces.",source->name);
        const auto& ns=field(normals,"vector").numbers;const auto& ts=field(uv,"point").numbers;const auto& cs=field(colors,"color").numbers;
        const auto& ni=field(geometry,"normalIndex").numbers;const auto& ti=field(geometry,"texCoordIndex").numbers;const auto& ci=field(geometry,"colorIndex").numbers;
        const bool np=scalar(geometry,"normalPerVertex",1)!=0,cp=scalar(geometry,"colorPerVertex",1)!=0;
        const auto nf=np?faces(ni):Faces{},tf=faces(ti),colf=cp?faces(ci):Faces{};
        if((!normals&&!ni.empty())||(!uv&&!ti.empty())||(!colors&&!ci.empty()))
            reject("INVALID_WRL_INDICES","Index array has no matching attribute node.",source->name);
        const double crease=scalar(geometry,"creaseAngle",0);
        if(crease<0) reject("INVALID_WRL","Negative crease angle.",source->name);
        if(!normals && crease!=0) reject("UNSUPPORTED_WRL_NORMALS","Export explicit normals when creaseAngle is nonzero.",source->name);
        if(!np && std::any_of(ni.begin(),ni.end(),[](double i){return i<0;}))
            reject("INVALID_WRL_INDICES","Per-face normalIndex cannot contain negative entries.",source->name);
        if(!cp && std::any_of(ci.begin(),ci.end(),[](double i){return i<0;}))
            reject("INVALID_WRL_INDICES","Per-face colorIndex cannot contain negative entries.",source->name);
        for(double c:cs) if(c<0||c>1) reject("INVALID_MATERIAL_VALUE","Color component outside [0,1].",source->name);
        const auto appearance=child(source,"appearance","Appearance"),mat=child(appearance,"material","Material"),
            tex=child(appearance,"texture","ImageTexture"),tt=child(appearance,"textureTransform","TextureTransform");
        Material material;material.name=mat&&!mat->name.empty()?mat->name:out.nodes[node_index].name+" material";
        material.double_sided=scalar(geometry,"solid",1)==0;
        const auto diffuse=vector3(mat,"diffuseColor",mat?Vec3{.8,.8,.8}:Vec3{1,1,1});
        material.color={diffuse.x,diffuse.y,diffuse.z,1-scalar(mat,"transparency",0)};
        if(mat) {
            for(const auto& [key,value]:mat->fields) for(double n:value.numbers)
                if(n<0||n>1) reject("INVALID_MATERIAL_VALUE","Material component outside [0,1].",material.name);
            const auto em=vector3(mat,"emissiveColor",{}),sp=vector3(mat,"specularColor",{});
            if(em.x||em.y||em.z) reject("UNSUPPORTED_WRL_MATERIAL","Emissive material requires baking.",material.name);
            if(scalar(mat,"ambientIntensity",.2)!=0||sp.x||sp.y||sp.z) {
                if(!options.gis_static) reject("UNSUPPORTED_WRL_MATERIAL","Ambient/specular lighting requires gis-static or a diffuse-only export.",material.name);
                warning("MATERIAL_CHANNEL_OMITTED","GIS static omits VRML ambient/specular lighting; diffuse color and opacity remain.",material.name);
            }
        } else warning("UNLIT_SHADING_MAPPED","VRML unlit appearance is mapped to FileGDB diffuse material; target lighting may differ.",material.name);
        bool replace_color=false,replace_alpha=false;
        if(tex) {
            if(scalar(tex,"repeatS",1)==0||scalar(tex,"repeatT",1)==0)
                reject("UNSUPPORTED_WRL_SAMPLER","Clamped ImageTexture wrapping is not represented.",material.name);
            const auto& urls=field(tex,"url").strings;
            bool missing=true;
            for(std::size_t i=0;i<urls.size();++i) {
                auto bytes=load_image_uri(urls[i],root,options,missing);
                if(missing) continue;
                Texture texture;texture.name="WRL image "+std::to_string(out.textures.size());
                texture.source=urls[i];texture.bytes=std::move(bytes);texture.mime_type=mime_type(texture.bytes);
                if(texture.mime_type!="image/png"&&texture.mime_type!="image/jpeg") reject("UNSUPPORTED_TEXTURE_FORMAT","WRL image must be PNG or JPEG.",urls[i]);
                const auto semantics=image_semantics(texture);replace_color=!semantics.first;replace_alpha=semantics.second;
                material.texture=static_cast<int>(out.textures.size());out.textures.push_back(std::move(texture));
                if(i) warning("WRL_URL_ALTERNATIVE_USED","Selected local ImageTexture URL alternative "+std::to_string(i+1)+" after absent files.",material.name);
                break;
            }
            if(missing&&!urls.empty()) {
                if(!options.missing_texture_fallback) reject("MISSING_TEXTURE","No ImageTexture URL exists.",material.name);
                warning("MISSING_TEXTURE_FALLBACK","Missing ImageTexture; retained diffuse color and scalar opacity.",material.name);
            }
        }
        if(replace_color) {material.color.r=material.color.g=material.color.b=1;warning("WRL_TEXTURE_COLOR_REPLACED","VRML97 RGB texture replaces diffuse/Color RGB; stored factor is white.",material.name);}
        if(replace_alpha) {material.color.a=1;warning("WRL_TEXTURE_ALPHA_REPLACED","VRML97 image alpha replaces scalar transparency; stored opacity factor is 1.",material.name);}
        const double det=determinant(world);
        if(!finite(det)||det==0) reject("INVALID_TRANSFORM","WRL mesh transform is singular or nonfinite.",source->name);
        Mesh mesh;mesh.name=out.nodes[node_index].name;mesh.source_node=mesh.name;
        std::array<double,3> low{INFINITY,INFINITY,INFINITY},high{-INFINITY,-INFINITY,-INFINITY};
        for(const auto& face:cf) for(auto index:face) {
            if(index>=positions.size()/3) reject("INVALID_WRL_INDICES","Coordinate index outside point array.",mesh.name);
            for(int axis=0;axis<3;++axis) {low[axis]=(std::min)(low[axis],positions[index*3+axis]);high[axis]=(std::max)(high[axis],positions[index*3+axis]);}
        }
        std::array<int,3> axes{0,1,2};std::stable_sort(axes.begin(),axes.end(),[&](int a,int b){return high[a]-low[a]>high[b]-low[b];});
        const auto uv_center=vector2(tt,"center",{}),uv_scale=vector2(tt,"scale",{1,1}),uv_translation=vector2(tt,"translation",{});
        const double uv_rotation=scalar(tt,"rotation",0);
        std::size_t repaired=0,repaired_triangles=0,discarded=0,discarded_normals=0;
        std::map<std::array<double,4>,int> face_materials;
        for(std::size_t fi=0;fi<cf.size();++fi) {
            const auto& face=cf[fi];
            if(face.size()<3||face.size()>4096) reject("INVALID_WRL_FACE","Face must have 3..4096 corners.",mesh.name);
            if(mesh.vertices.size()+(face.size()-2)*3>10000000) reject("MESH_TOO_LARGE","Expanded mesh exceeds the corner limit.",mesh.name);
            std::vector<Vertex> corners;std::vector<ufbx_vec3> points;std::vector<std::uint32_t> order;
            Material fm=material;Vec3 face_color{};
            for(std::size_t k=0;k<face.size();++k) {
                const auto ix=face[k];const Vec3 p{positions[ix*3],positions[ix*3+1],positions[ix*3+2]};
                Vertex v;v.position=point(world,p);
                if(!finite(v.position.x)||!finite(v.position.y)||!finite(v.position.z)) reject("NONFINITE_VERTEX","Transformed WRL coordinate is nonfinite.",mesh.name);
                if(normals) {
                    const auto index=binding(nf,ni,np,cf,fi,k,ns.size()/3);
                    Vec3 n{ns[index*3],ns[index*3+1],ns[index*3+2]};const double length=std::hypot(n.x,n.y,n.z);
                    if(!finite(length)||std::abs(length-1)>1e-3) {
                        if(!options.gis_static) reject("INVALID_NORMAL","WRL normal must have unit length.",mesh.name);
                        v.normal={};
                    } else v.normal=normal(world,n,det);
                    v.has_normal=true;
                }
                if(colors) {
                    const auto index=binding(colf,ci,cp,cf,fi,k,cs.size()/3);const Vec3 color{cs[index*3],cs[index*3+1],cs[index*3+2]};
                    if(k && !replace_color && (color.x!=face_color.x||color.y!=face_color.y||color.z!=face_color.z))
                        reject("UNSUPPORTED_WRL_VERTEX_COLOR","Interpolated vertex colors require baking; uniform face colors are supported.",mesh.name);
                    face_color=color;
                }
                if(uv) {
                    const auto index=binding(tf,ti,true,cf,fi,k,ts.size()/2);v.uv={ts[index*2],ts[index*2+1]};v.has_uv=true;
                } else if(material.texture>=0) {
                    const double length=high[axes[0]]-low[axes[0]];
                    if(length<=0) reject("MISSING_UV","Cannot generate default UV for zero-size geometry.",mesh.name);
                    v.uv={(positions[ix*3+axes[0]]-low[axes[0]])/length,(positions[ix*3+axes[1]]-low[axes[1]])/length};v.has_uv=true;
                }
                if(v.has_uv && tt) {
                    // VRML texture-space transform: subtract center, scale, rotate, restore center, translate.
                    const double x=(v.uv.x-uv_center.x)*uv_scale.x,y=(v.uv.y-uv_center.y)*uv_scale.y;
                    v.uv={std::cos(uv_rotation)*x-std::sin(uv_rotation)*y+uv_center.x+uv_translation.x,
                        std::sin(uv_rotation)*x+std::cos(uv_rotation)*y+uv_center.y+uv_translation.y};
                }
                corners.push_back(v);points.push_back({p.x,p.y,p.z});order.push_back(static_cast<std::uint32_t>(k));
            }
            if(colors&&!replace_color) {fm.color.r=face_color.x;fm.color.g=face_color.y;fm.color.b=face_color.z;}
            const std::array<double,4> key{fm.color.r,fm.color.g,fm.color.b,fm.color.a};
            auto found=face_materials.find(key);
            if(found==face_materials.end()) {
                fm.name+="/color:"+std::to_string(face_materials.size());
                found=face_materials.emplace(key,static_cast<int>(out.materials.size())).first;
                out.materials.push_back(std::move(fm));
            }
            const auto material_index=found->second;
            validate_polygon(points);
            ufbx_mesh triangulation{};triangulation.num_indices=face.size();
            triangulation.vertex_position.values={points.data(),points.size()};triangulation.vertex_position.indices={order.data(),order.size()};
            std::vector<std::uint32_t> triangles((face.size()-2)*3);ufbx_panic panic{};
            const auto count=ufbx_catch_triangulate_face(&panic,triangles.data(),triangles.size(),&triangulation,{0,static_cast<std::uint32_t>(face.size())});
            if(panic.did_panic||count!=face.size()-2) reject("INVALID_WRL_FACE","Polygon triangulation failed.",mesh.name);
            const bool reverse=(det<0)!=(scalar(geometry,"ccw",1)==0);
            for(std::size_t i=0;i<triangles.size();i+=3) {
                std::array<Vertex,3> tri{corners[triangles[i]],corners[triangles[i+1]],corners[triangles[i+2]]};
                if(reverse) std::swap(tri[1],tri[2]);
                const auto f=face_normal(tri[0].position,tri[1].position,tri[2].position);
                if(f.x==0&&f.y==0&&f.z==0) {
                    if(!options.gis_static) reject("DEGENERATE_TRIANGLE","WRL triangle has zero area.",mesh.name);
                    ++discarded;for(const auto& v:tri) if(v.has_normal&&std::hypot(v.normal.x,v.normal.y,v.normal.z)==0) ++discarded_normals;
                    continue;
                }
                const auto before=repaired;
                for(auto& v:tri) {
                    if(!v.has_normal) {v.normal=f;v.has_normal=true;}
                    else if(!finite(std::hypot(v.normal.x,v.normal.y,v.normal.z))||std::hypot(v.normal.x,v.normal.y,v.normal.z)==0) {
                        if(!options.gis_static) reject("INVALID_NORMAL","Transformed normal is invalid.",mesh.name);
                        v.normal=f;++repaired;
                    }
                }
                if(repaired!=before) ++repaired_triangles;
                Triangle t;t.material=material_index;
                for(int k=0;k<3;++k) {t.indices[k]=static_cast<std::uint32_t>(mesh.vertices.size());mesh.vertices.push_back(tri[k]);}
                mesh.triangles.push_back(t);
            }
        }
        if(material.texture>=0&&!uv) warning("WRL_DEFAULT_UV_GENERATED","Generated VRML97 bounding-box UVs in local coordinates.",mesh.name);
        if(!normals) warning("WRL_DEFAULT_NORMALS_GENERATED","Generated flat normals for creaseAngle 0.",mesh.name);
        if(repaired) warning("NORMALS_REPAIRED","GIS static profile rebuilt "+std::to_string(repaired)+" invalid corner normals across "+std::to_string(repaired_triangles)+" triangles.",mesh.name);
        if(discarded) warning("DEGENERATE_TRIANGLES_REMOVED","GIS static profile removed "+std::to_string(discarded)+" strictly zero-area triangles; no area tolerance was used.",mesh.name);
        if(discarded_normals) warning("DEGENERATE_NORMALS_DISCARDED","Discarded "+std::to_string(discarded_normals)+" invalid normals with zero-area triangles.",mesh.name);
        if(mesh.triangles.empty()) reject("EMPTY_MESH","No retained WRL triangles.",mesh.name);
        out.nodes[node_index].meshes.push_back(static_cast<std::uint32_t>(out.meshes.size()));out.meshes.push_back(std::move(mesh));
    }
public:
    Converter(Scene& scene,const ReaderOptions& opts,std::filesystem::path directory):out(scene),options(opts),root(std::move(directory)){}
    void visit(const Ref& source,int parent,const Matrix& parent_matrix,std::size_t depth=0) {
        if(!source) return;
        if(depth>128||++expanded>100000) reject("WRL_EXPANSION_LIMIT","DEF/USE expansion exceeds node/depth limit.","scene");
        if(source->type=="WorldInfo") {warning("WRL_METADATA_OMITTED","WorldInfo text is not a render channel and is omitted.",source->name);return;}
        if(source->type!="Transform"&&source->type!="Group"&&source->type!="Shape")
            reject("INVALID_WRL_NODE_TYPE","Expected a Shape, Group or Transform in scene children.",source->type);
        Matrix world=parent_matrix;
        if(source->type=="Transform") {
            const auto c=vector3(source,"center",{}),s=vector3(source,"scale",{1,1,1});
            auto scale=identity();scale[0]=s.x;scale[5]=s.y;scale[10]=s.z;
            auto local=multiply(translation(vector3(source,"translation",{})),translation(c));
            local=multiply(local,rotation(source,"rotation"));local=multiply(local,rotation(source,"scaleOrientation"));
            local=multiply(local,scale);local=multiply(local,rotation(source,"scaleOrientation",true));
            local=multiply(local,translation({-c.x,-c.y,-c.z}));world=multiply(parent_matrix,local);
        }
        for(double x:world) if(!finite(x)) reject("NONFINITE_TRANSFORM","WRL transform is nonfinite.",source->name);
        Node node;node.name=source->name.empty()?source->type+" "+std::to_string(out.nodes.size()):source->name;
        node.source_id="VRML97:"+node.name;node.parent=parent;node.source_world_transform=world;
        const auto index=static_cast<int>(out.nodes.size());out.nodes.push_back(std::move(node));
        if(source->type=="Shape") shape(source,index,world);
        else for(const auto& c:field(source,"children").nodes) visit(c,index,world,depth+1);
    }
};
} // namespace
Scene read_wrl(const std::filesystem::path& input,const ReaderOptions& options) {
    Scene out;const auto path=std::filesystem::absolute(input);out.name=path.stem().u8string();out.source=path.u8string();
    out.conversion_profile=options.gis_static?"gis-static":"strict";out.missing_texture_policy=options.missing_texture_fallback?"material-color":"error";
    try {
        auto bytes=read_bytes(path,options.max_file_bytes);const std::string_view text(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        const auto newline=text.find_first_of("\r\n");
        if(text.substr(0,newline)!="#VRML V2.0 utf8") reject("UNSUPPORTED_WRL_VERSION","Expected #VRML V2.0 utf8 (VRML97); VRML1, X3D and compressed WRL are not supported.","scene");
        auto nodes=Parser(text).parse();Converter converter(out,options,std::filesystem::weakly_canonical(path.parent_path()));
        out.diagnostics.push_back({Severity::warning,"WRL_COORDINATE_CONVENTION","VRML97 right-handed Y-up metres converted once to right-handed Z-up metres.","scene"});
        for(const auto& node:nodes) converter.visit(node,-1,identity());
    } catch(const Issue& e) {out.diagnostics.push_back({Severity::error,e.code,e.what(),e.context});}
    return out;
}
} // namespace gmb
