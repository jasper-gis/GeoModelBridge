#include "gmb/scene.hpp"
#include "reader_util.hpp"
#include "tinyxml2.h"
#include "ufbx.h"
#include <charconv>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace gmb {
namespace {
using namespace reader;
using Element = const tinyxml2::XMLElement;
using Matrix = std::array<double,16>;
std::string attribute(Element* e,const char* key,const std::string& fallback={}) {
    return e?label(e->Attribute(key),fallback):fallback;
}
std::string context(Element* e) {return e?attribute(e,"id",attribute(e,"sid",e->Name())):"scene";}
[[noreturn]] void bad(const std::string& text,Element* e=nullptr) {reject("INVALID_DAE",text,context(e));}
Element* one(Element* e,const char* key,bool required=false) {
    auto c=e?e->FirstChildElement(key):nullptr;
    if((required&&!c)||(c&&c->NextSiblingElement(key))) bad(std::string("Expected one ")+key+" element.",e);
    return c;
}
std::string text(Element* e) {
    if(!e||e->FirstChildElement()) bad("Expected text content.",e);
    auto value=label(e->GetText(),"");const auto start=value.find_first_not_of(" \t\r\n");
    return start==std::string::npos?"":value.substr(start,value.find_last_not_of(" \t\r\n")-start+1);
}
std::vector<double> numbers(Element* e,std::size_t expected=0) {
    const auto value=text(e);std::string_view remaining(value);std::vector<double> out;
    while(!remaining.empty()) {
        const auto start=remaining.find_first_not_of(" \t\r\n");if(start==std::string_view::npos) break;
        remaining.remove_prefix(start);const auto end=remaining.find_first_of(" \t\r\n");
        auto token=remaining.substr(0,end);double n=0;const char* p=token.data();if(*p=='+') ++p;
        const auto parsed=std::from_chars(p,token.data()+token.size(),n);
        if(parsed.ec!=std::errc{}||parsed.ptr!=token.data()+token.size()) bad("Invalid numeric token.",e);
        if(out.size()>=30000000) bad("Numeric array exceeds the reader limit.",e);
        out.push_back(n);if(end==std::string_view::npos) break;remaining.remove_prefix(end);
    }
    if(expected&&out.size()!=expected) bad("Numeric value count does not match its declaration.",e);
    return out;
}
std::size_t integer(Element* e,const char* key,std::size_t fallback=0,bool required=false) {
    auto s=attribute(e,key);if(s.empty()) {if(required) bad(std::string("Missing ")+key,e);return fallback;}
    std::size_t n=0;auto parsed=std::from_chars(s.data(),s.data()+s.size(),n);
    if(parsed.ec!=std::errc{}||parsed.ptr!=s.data()+s.size()||n>30000000) bad("Invalid or excessive integer attribute.",e);
    return n;
}
void children(Element* e,std::initializer_list<const char*> allowed) {
    for(auto c=e->FirstChildElement();c;c=c->NextSiblingElement()) {
        bool found=false;for(auto key:allowed) found=found||std::string(c->Name())==key;
        if(!found) reject("UNSUPPORTED_DAE_ELEMENT","COLLADA element is not represented: "+std::string(c->Name()),context(e));
    }
}
Vec3 face_normal(Vec3 a,Vec3 b,Vec3 c) {
    Vec3 u{b.x-a.x,b.y-a.y,b.z-a.z},v{c.x-a.x,c.y-a.y,c.z-a.z};
    const auto scale=(std::max)({std::abs(u.x),std::abs(u.y),std::abs(u.z),std::abs(v.x),std::abs(v.y),std::abs(v.z)});
    if(!finite(scale)) reject("NONFINITE_VERTEX","Transformed edge is nonfinite.","geometry");
    if(!scale) return {};
    u={u.x/scale,u.y/scale,u.z/scale};v={v.x/scale,v.y/scale,v.z/scale};
    const auto n=cross(u,v);const auto length=std::hypot(n.x,n.y,n.z);
    return length?Vec3{n.x/length,n.y/length,n.z/length}:Vec3{};
}
void polygon(const std::vector<Vertex>& v) {
    if(v.size()==3) return;
    const auto origin=v[0].position;double scale=0;
    for(const auto& x:v) scale=(std::max)({scale,std::abs(x.position.x-origin.x),std::abs(x.position.y-origin.y),std::abs(x.position.z-origin.z)});
    if(!finite(scale)||!scale) bad("Polygon has invalid extent.");
    std::vector<Vec3> q;for(const auto& x:v) q.push_back({(x.position.x-origin.x)/scale,(x.position.y-origin.y)/scale,(x.position.z-origin.z)/scale});
    Vec3 n{};for(std::size_t i=1;i+1<q.size();++i) {n=face_normal({},q[i],q[i+1]);if(std::hypot(n.x,n.y,n.z)>0) break;}
    if(!std::hypot(n.x,n.y,n.z)) bad("Polygon has no plane; triangulate it in the exporter.");
    const int drop=std::abs(n.x)>=std::abs(n.y)&&std::abs(n.x)>=std::abs(n.z)?0:std::abs(n.y)>=std::abs(n.z)?1:2;
    std::vector<Vec2> p;for(auto x:q) {
        if(std::abs(dot(n,x))>1e-10) bad("Nonplanar polygon must be triangulated in the exporter.");
        p.push_back(drop==0?Vec2{x.y,x.z}:drop==1?Vec2{x.x,x.z}:Vec2{x.x,x.y});
    }
    auto orient=[](Vec2 a,Vec2 b,Vec2 c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);};
    auto on=[](Vec2 a,Vec2 b,Vec2 c){return c.x>=(std::min)(a.x,b.x)&&c.x<=(std::max)(a.x,b.x)&&c.y>=(std::min)(a.y,b.y)&&c.y<=(std::max)(a.y,b.y);};
    for(std::size_t i=0;i<p.size();++i) {
        const auto a=p[i],b=p[(i+1)%p.size()];if(a.x==b.x&&a.y==b.y) bad("Repeated polygon corner.");
        for(std::size_t j=i+1;j<p.size();++j) {
            if(j==i+1||(i==0&&j+1==p.size())) continue;
            const auto c=p[j],d=p[(j+1)%p.size()];const auto x=orient(a,b,c),y=orient(a,b,d),z=orient(c,d,a),w=orient(c,d,b);
            if(((x<0&&y>0)||(x>0&&y<0))&&((z<0&&w>0)||(z>0&&w<0))) bad("Self-intersecting polygon.");
            if((x==0&&on(a,b,c))||(y==0&&on(a,b,d))||(z==0&&on(c,d,a))||(w==0&&on(c,d,b))) bad("Polygon edges touch or overlap.");
        }
    }
}
struct Source {std::vector<double> values;std::size_t count=0,stride=0,offset=0;std::vector<std::size_t> components;};
struct Input {std::string semantic;Element* source=nullptr;std::size_t offset=0,set=0;};
struct BoundMaterial {Material value;std::string texcoord;std::size_t uv_set=0;bool uv_bound=false;};
// The native diffuse texture carries its own alpha. COLLADA must explicitly
// bind that same alpha into transparent/A_ONE before it can affect blending.
bool image_alpha(const Texture& texture) {
    if(texture.mime_type=="image/jpeg") return false;
    const auto& bytes=texture.bytes;
    if(bytes.size()<33||std::string(bytes.begin()+12,bytes.begin()+16)!="IHDR")
        reject("TEXTURE_READ_ERROR","PNG has no complete IHDR.",texture.source);
    const auto type=bytes[25];bool alpha=type==4||type==6;
    if(type!=0&&type!=2&&type!=3&&type!=4&&type!=6) reject("TEXTURE_READ_ERROR","Invalid PNG color type.",texture.source);
    for(std::size_t at=8;at<bytes.size();) {
        if(bytes.size()-at<12) reject("TEXTURE_READ_ERROR","Incomplete PNG chunk.",texture.source);
        const auto count=(std::size_t(bytes[at])<<24)|(std::size_t(bytes[at+1])<<16)|(std::size_t(bytes[at+2])<<8)|bytes[at+3];
        if(count>bytes.size()-at-12) reject("TEXTURE_READ_ERROR","PNG chunk exceeds image bytes.",texture.source);
        if(std::string(bytes.begin()+at+4,bytes.begin()+at+8)=="tRNS") alpha=true;
        at+=count+12;
    }
    return alpha;
}

class Converter {
    Scene& out;const ReaderOptions& options;std::filesystem::path root;
    std::map<std::string,Element*> ids;std::map<Element*,Source> sources;
    std::set<Element*> active;Matrix basis=identity();std::size_t inspected=0,visits=0;
    void warning(const char* code,const std::string& message,const std::string& ctx) {out.diagnostics.push_back({Severity::warning,code,message,ctx});}
    void index(Element* e,std::size_t depth=0) {
        if(depth>128||++inspected>1000000) bad("XML depth/count limit exceeded.",e);
        const auto id=attribute(e,"id");if(!id.empty()&&!ids.emplace(id,e).second) bad("Duplicate XML id: "+id,e);
        const std::string name=e->Name();
        if(name=="extra") extras(e);
        if(name=="library_controllers"||name=="instance_controller"||name=="skin"||name=="morph")
            reject("UNSUPPORTED_DAE_DEFORMATION","COLLADA controllers/deformation require a baked static mesh.",context(e));
        if(name=="library_animations"||name=="library_animation_clips")
            reject("UNSUPPORTED_ANIMATION","COLLADA animations require a static export; animation channels are not evaluated.",context(e));
        if(name=="asset"&&depth!=1) reject("UNSUPPORTED_DAE_ASSET","Nested asset coordinate overrides are not supported.",context(e));
        if(e->Attribute("xml:base")||name.find(':')!=std::string::npos)
            reject("UNSUPPORTED_DAE_ELEMENT","Namespace prefixes and xml:base overrides are not supported.",context(e));
        if(depth&&e->Attribute("xmlns")) reject("UNSUPPORTED_DAE_ELEMENT","Nested namespace overrides are not supported.",context(e));
        for(auto n=e->FirstChild();n;n=n->NextSibling())
            if(n->ToUnknown()||n->ToDeclaration()) reject("UNSUPPORTED_DAE_XML","XML declarations/unknown markup are allowed only before the document root.",context(e));
        for(auto c=e->FirstChildElement();c;c=c->NextSiblingElement()) index(c,depth+1);
    }
    void extras(Element* e) {
        children(e,{"technique"});
        for(auto t=e->FirstChildElement();t;t=t->NextSiblingElement()) {
            const auto profile=attribute(t,"profile");
            if(profile!="GOOGLEEARTH"&&profile!="MAX3D"&&profile!="MAYA")
                reject("UNSUPPORTED_DAE_EXTRA","Unsupported COLLADA extension profile: "+profile,context(e));
            children(t,{"double_sided"});const auto d=one(t,"double_sided",true);const auto value=text(d);
            if(value!="0"&&value!="1") bad("double_sided must be 0 or 1.",d);
            const auto parent=e->Parent()->ToElement();
            if(!parent||(std::string(parent->Name())!="effect"&&std::string(parent->Name())!="profile_COMMON"))
                reject("UNSUPPORTED_DAE_EXTRA","double_sided is supported only on effects/profile_COMMON.",context(e));
        }
    }
    Element* reference(const std::string& url,const char* kind) {
        if(url.size()<2||url[0]!='#'||url.find_first_of("/%:")!=std::string::npos)
            reject("UNSAFE_DAE_REFERENCE","Only local COLLADA #id references are supported: "+url,"scene");
        const auto found=ids.find(url.substr(1));
        if(found==ids.end()||std::string(found->second->Name())!=kind) bad("Reference has no matching "+std::string(kind)+": "+url);
        return found->second;
    }
    const Source& source(Element* e) {
        auto found=sources.find(e);if(found!=sources.end()) return found->second;
        children(e,{"float_array","technique_common"});auto array=one(e,"float_array",true);
        auto tc=one(e,"technique_common",true);children(tc,{"accessor"});auto a=one(tc,"accessor",true);children(a,{"param"});
        if(reference(attribute(a,"source"),"float_array")!=array) bad("Accessor must reference its own float_array.",a);
        Source s;s.values=numbers(array);if(s.values.size()!=integer(array,"count",0,true)) bad("float_array count mismatch.",array);
        s.count=integer(a,"count",0,true);s.stride=integer(a,"stride",1);s.offset=integer(a,"offset");
        if(!s.stride||s.stride>16||s.offset>s.values.size()) bad("Accessor exceeds its source array.",a);
        std::size_t component=0;for(auto p=a->FirstChildElement();p;p=p->NextSiblingElement(),++component) {
            if(attribute(p,"type")!="float"||component>=s.stride) bad("Only scalar float accessor parameters are supported.",p);
            if(!attribute(p,"name").empty()) s.components.push_back(component);
        }
        if(s.components.empty()) bad("Accessor has no named components.",a);
        if(s.count&&((s.count-1)*s.stride+s.components.back()+1>s.values.size()-s.offset)) bad("Accessor exceeds its source array.",a);
        return sources.emplace(e,std::move(s)).first->second;
    }
    std::vector<double> sample(const Input& input,std::size_t i,std::size_t width) {
        const auto& s=source(input.source);
        if(i>=s.count||s.components.size()!=width) bad("Attribute index or component count is invalid.",input.source);
        std::vector<double> v;for(auto c:s.components) v.push_back(s.values[s.offset+i*s.stride+c]);return v;
    }
    Element* parameter(Element* profile,const std::string& sid,const char* kind) {
        Element* result=nullptr;
        for(auto p=profile->FirstChildElement("newparam");p;p=p->NextSiblingElement("newparam"))
            if(attribute(p,"sid")==sid) {if(result) bad("Duplicate effect parameter sid.",p);result=one(p,kind,true);}
        if(!result) bad("Missing effect parameter: "+sid,profile);
        return result;
    }
    Color color(Element* c) {
        children(c,{"color"});const auto v=numbers(one(c,"color",true),4);
        for(double x:v) if(!finite(x)||x<0||x>1) reject("INVALID_MATERIAL_VALUE","COLLADA color is outside [0,1].",context(c));
        return {v[0],v[1],v[2],v[3]};
    }
    double scalar(Element* c,double fallback) {
        if(!c) return fallback;
        children(c,{"float"});const auto v=numbers(one(c,"float",true),1)[0];
        if(!finite(v)) reject("INVALID_MATERIAL_VALUE","COLLADA material scalar is nonfinite.",context(c));
        return v;
    }
    BoundMaterial material(Element* instance,Element* primitive) {
        BoundMaterial b;b.value.name="DAE default material";b.value.double_sided=false;
        auto bindings=one(instance,"bind_material");Element* assignment=nullptr;
        if(bindings) {
            children(bindings,{"technique_common"});auto tc=one(bindings,"technique_common",true);children(tc,{"instance_material"});
            std::set<std::string> symbols;
            for(auto im=tc->FirstChildElement();im;im=im->NextSiblingElement()) {
                children(im,{"bind_vertex_input"});const auto symbol=attribute(im,"symbol");
                if(symbol.empty()||!symbols.insert(symbol).second) bad("Empty or duplicate material symbol.",im);
                reference(attribute(im,"target"),"material");if(symbol==attribute(primitive,"material")) assignment=im;
            }
        }
        if(!assignment) {
            if(!attribute(primitive,"material").empty()) bad("Primitive material symbol is not bound.",primitive);
            warning("DEFAULT_MATERIAL_ASSIGNED","COLLADA primitive has no material; an explicit white diffuse material is assigned.",context(primitive));return b;
        }
        auto m=reference(attribute(assignment,"target"),"material");children(m,{"instance_effect"});
        auto ie=one(m,"instance_effect",true);children(ie,{});auto effect=reference(attribute(ie,"url"),"effect");
        children(effect,{"profile_COMMON","extra"});auto profile=one(effect,"profile_COMMON",true);
        children(profile,{"newparam","technique","extra"});
        std::set<std::string> params;for(auto p=profile->FirstChildElement("newparam");p;p=p->NextSiblingElement("newparam")) {
            if(attribute(p,"sid").empty()||!params.insert(attribute(p,"sid")).second) bad("Missing/duplicate effect parameter sid.",p);
            children(p,{"surface","sampler2D"});if(!one(p,"surface")&&!one(p,"sampler2D")) bad("Unsupported effect parameter.",p);
            if(one(p,"surface")&&one(p,"sampler2D")) bad("Effect parameter must have one value.",p);
        }
        auto technique=one(profile,"technique",true);children(technique,{"lambert","phong","blinn","constant"});
        auto shader=technique->FirstChildElement();if(!shader||shader->NextSiblingElement()) bad("Effect must have one shading model.",technique);
        const bool unlit=std::string(shader->Name())=="constant";
        children(shader,{"diffuse","emission","ambient","specular","shininess","reflective","reflectivity","transparent","transparency","index_of_refraction"});
        if(unlit&&one(shader,"diffuse")) reject("UNSUPPORTED_DAE_MATERIAL","COLLADA constant must use emission as its base color.",context(shader));
        b.value.name=attribute(m,"name",context(m));
        for(auto parent:{effect,profile}) for(auto ex=parent->FirstChildElement("extra");ex;ex=ex->NextSiblingElement("extra"))
            for(auto t=ex->FirstChildElement();t;t=t->NextSiblingElement()) b.value.double_sided=text(one(t,"double_sided",true))=="1";
        auto diffuse=one(shader,unlit?"emission":"diffuse",true);
        std::string diffuse_sampler;bool diffuse_alpha=false;
        if(diffuse) {
            children(diffuse,{"color","texture"});auto c=one(diffuse,"color"),t=one(diffuse,"texture");
            if((c!=nullptr)==(t!=nullptr)) bad("Diffuse channel must have one color or texture.",diffuse);
            if(c) {
                b.value.color=color(diffuse);
                if(b.value.color.a!=1) reject("UNSUPPORTED_DAE_ALPHA","Diffuse color alpha is not COLLADA transparency; export explicit transparent/transparency.",context(m));
            } else {
                children(t,{});b.texcoord=attribute(t,"texcoord");diffuse_sampler=attribute(t,"texture");if(b.texcoord.empty()) bad("Diffuse texture has no texcoord binding.",t);
                auto sampler=parameter(profile,attribute(t,"texture"),"sampler2D");
                children(sampler,{"source","wrap_s","wrap_t","minfilter","magfilter"});
                for(const auto key:{"wrap_s","wrap_t"}) if(auto wrap=one(sampler,key))
                    if(text(wrap)!="WRAP") reject("UNSUPPORTED_DAE_SAMPLER","Only repeat texture wrapping is represented.",context(sampler));
                for(const auto key:{"minfilter","magfilter"}) if(auto filter=one(sampler,key))
                    reject("UNSUPPORTED_DAE_SAMPLER","Explicit texture filtering cannot be preserved by the FileGDB material contract: "+text(filter),context(sampler));
                auto surface=parameter(profile,text(one(sampler,"source",true)),"surface");children(surface,{"init_from"});
                if(attribute(surface,"type")!="2D") reject("UNSUPPORTED_DAE_SAMPLER","Only 2D surfaces are supported.",context(surface));
                auto init=one(surface,"init_from",true);
                if(integer(init,"mip")||integer(init,"slice")||!attribute(init,"face").empty()) reject("UNSUPPORTED_DAE_SAMPLER","Surface subresource selection is unsupported.",context(init));
                auto image=reference("#"+text(init),"image");children(image,{"init_from"});const auto uri=text(one(image,"init_from",true));
                bool missing=false;auto bytes=load_image_uri(uri,root,options,missing);
                if(missing) {
                    if(!options.missing_texture_fallback) reject("MISSING_TEXTURE","COLLADA image is missing: "+uri,context(m));
                    warning("MISSING_TEXTURE_FALLBACK","Missing COLLADA image binding removed; diffuse color and scalar opacity retained: "+uri,context(m));
                } else {
                    Texture tex;tex.name=attribute(image,"name",context(image));tex.source=uri;tex.bytes=std::move(bytes);tex.mime_type=mime_type(tex.bytes);
                    if(tex.mime_type!="image/png"&&tex.mime_type!="image/jpeg") reject("TEXTURE_READ_ERROR","COLLADA image must be a readable PNG or JPEG.",uri);
                    diffuse_alpha=image_alpha(tex);
                    b.value.texture=static_cast<int>(out.textures.size());out.textures.push_back(std::move(tex));
                }
                for(auto input=assignment->FirstChildElement();input;input=input->NextSiblingElement()) {
                    children(input,{});
                    if(attribute(input,"input_semantic")!="TEXCOORD") reject("UNSUPPORTED_DAE_BINDING","Only TEXCOORD material vertex bindings are supported.",context(input));
                    if(attribute(input,"semantic")==b.texcoord) {
                        if(b.uv_bound) bad("Duplicate texture coordinate binding.",input);
                        b.uv_set=integer(input,"input_set");b.uv_bound=true;
                    }
                }
                if(b.value.texture>=0&&!b.uv_bound) reject("UNSUPPORTED_UV_SET","Retained texture requires an explicit bind_vertex_input TEXCOORD binding.",context(m));
            }
        }
        if(unlit) warning("UNLIT_SHADING_MAPPED","COLLADA constant emission mapped to FileGDB diffuse material; target lighting may differ.",context(m));
        for(const auto key:{"emission","ambient","specular","reflective"}) {
            if(unlit&&std::string(key)=="emission") continue;
            auto channel=one(shader,key);if(!channel) continue;
            children(channel,{"color","texture"});bool enabled=false;
            if((one(channel,"texture")!=nullptr)==(one(channel,"color")!=nullptr)) bad("Material channel must contain one color or texture.",channel);
            if(one(channel,"texture")) enabled=true;
            else {const auto c=color(channel);enabled=c.r!=0||c.g!=0||c.b!=0;}
            if(!enabled) continue;
            if(options.gis_static&&std::string(key)!="emission") warning("MATERIAL_CHANNEL_OMITTED","GIS static omits COLLADA "+std::string(key)+" lighting; diffuse color, opacity and texture remain.",context(m));
            else reject("UNSUPPORTED_DAE_MATERIAL","COLLADA material channel is not represented: "+std::string(key),context(m));
        }
        for(const auto key:{"shininess","reflectivity","index_of_refraction"}) {
            auto c=one(shader,key);if(!c) continue;const auto v=scalar(c,0);
            if(v<0) reject("INVALID_MATERIAL_VALUE","Negative material scalar.",context(m));
            if(v!=(std::string(key)=="index_of_refraction"?1:0)) {
                if(options.gis_static) warning("MATERIAL_CHANNEL_OMITTED","GIS static omits COLLADA "+std::string(key)+" lighting.",context(m));
                else reject("UNSUPPORTED_DAE_MATERIAL","COLLADA material scalar is not represented: "+std::string(key),context(m));
            }
        }
        const auto transparent=one(shader,"transparent"),transparency=one(shader,"transparency");const auto factor=scalar(transparency,1);
        if(factor<0||factor>1) reject("INVALID_MATERIAL_VALUE","Transparency factor is outside [0,1].",context(m));
        bool bound_alpha=false;
        if(transparent||transparency) {
            const auto opaque=attribute(transparent,"opaque","A_ONE");
            if(auto t=one(transparent,"texture")) {
                children(transparent,{"texture"});children(t,{});
                if(opaque!="A_ONE"||diffuse_sampler.empty()||attribute(t,"texture")!=diffuse_sampler||attribute(t,"texcoord")!=b.texcoord)
                    reject("UNSUPPORTED_DAE_ALPHA","Transparency texture must use A_ONE and the exact diffuse sampler/UV binding.",context(m));
                b.value.color.a=factor;bound_alpha=true;
            } else {
            const auto c=transparent?color(transparent):Color{};
            if(opaque=="A_ONE") b.value.color.a=c.a*factor;
            else if(opaque=="RGB_ZERO") {
                if(factor!=0&&(c.r!=c.g||c.g!=c.b)) reject("UNSUPPORTED_DAE_ALPHA","Colored RGB_ZERO transparency requires separate RGB blend factors and cannot be represented by scalar opacity.",context(m));
                b.value.color.a=1-c.r*factor;
            }
            else reject("UNSUPPORTED_DAE_ALPHA","Unsupported COLLADA opacity mode: "+opaque,context(m));
            }
        }
        if(diffuse_alpha&&!bound_alpha) reject("UNSUPPORTED_DAE_ALPHA","PNG alpha/tRNS requires an explicit transparent/A_ONE binding to the same diffuse sampler and UVs; implicit image alpha cannot be represented without changing COLLADA blending.",context(m));
        return b;
    }
    void geometry(Element* instance,int node_index,const Matrix& world) {
        children(instance,{"bind_material"});auto g=reference(attribute(instance,"url"),"geometry");children(g,{"mesh"});auto mesh=one(g,"mesh",true);
        children(mesh,{"source","vertices","triangles","polylist","polygons"});
        for(auto s=mesh->FirstChildElement("source");s;s=s->NextSiblingElement("source")) source(s);
        Mesh result;result.name=attribute(g,"name",context(g));result.source_node=out.nodes[node_index].name;
        const auto transformed=multiply(basis,world);const auto det=determinant(transformed);
        if(!finite(det)||det==0) reject("INVALID_TRANSFORM","COLLADA instance transform is singular or nonfinite.",context(instance));
        std::size_t generated=0,repaired=0,repaired_triangles=0,discarded=0,discarded_normals=0;
        for(auto primitive=mesh->FirstChildElement();primitive;primitive=primitive->NextSiblingElement()) {
            const std::string kind=primitive->Name();if(kind=="source"||kind=="vertices") continue;
            children(primitive,kind=="polylist"?std::initializer_list<const char*>{"input","vcount","p"}:std::initializer_list<const char*>{"input","p"});
            std::vector<Input> inputs;std::size_t stride=0;std::set<std::pair<std::string,std::size_t>> semantics;
            auto add=[&](Element* e,std::size_t offset) {
                children(e,{});Input i{attribute(e,"semantic"),reference(attribute(e,"source"),"source"),offset,integer(e,"set")};
                if(i.source->Parent()!=mesh) bad("Mesh input must reference a source in the same mesh.",e);
                if(i.semantic!="POSITION"&&i.semantic!="NORMAL"&&i.semantic!="TEXCOORD") reject("UNSUPPORTED_DAE_ATTRIBUTE","COLLADA attribute is not represented: "+i.semantic,context(e));
                if(i.semantic!="TEXCOORD"&&i.set!=0) bad("POSITION/NORMAL may not specify a nonzero attribute set.",e);
                if(!semantics.emplace(i.semantic,i.set).second) bad("Duplicate primitive attribute binding.",e);
                inputs.push_back(i);
            };
            bool vertex=false;
            for(auto e=primitive->FirstChildElement("input");e;e=e->NextSiblingElement("input")) {
                const auto offset=integer(e,"offset",0,true);if(offset>31) bad("Primitive input stride is excessive.",e);stride=(std::max)(stride,offset+1);
                if(attribute(e,"semantic")=="VERTEX") {
                    if(vertex) bad("Duplicate VERTEX binding.",e);
                    vertex=true;
                    auto vertices=reference(attribute(e,"source"),"vertices");if(vertices->Parent()!=mesh) bad("VERTEX must reference its own mesh.",e);
                    children(vertices,{"input"});for(auto v=vertices->FirstChildElement();v;v=v->NextSiblingElement()) add(v,offset);
                } else add(e,offset);
            }
            if(!vertex||!semantics.count({"POSITION",0})) bad("Primitive requires a VERTEX/POSITION binding.",primitive);
            auto b=material(instance,primitive);const int material_index=static_cast<int>(out.materials.size());out.materials.push_back(b.value);
            std::size_t uv_count=0,selected_uv=30000000;
            for(const auto& i:inputs) if(i.semantic=="TEXCOORD") {++uv_count;selected_uv=(std::min)(selected_uv,i.set);}
            if(b.uv_bound&&semantics.count({"TEXCOORD",b.uv_set})) selected_uv=b.uv_set;
            if(b.value.texture>=0&&!semantics.count({"TEXCOORD",b.uv_set})) reject("UNSUPPORTED_UV_SET","Bound texture UV set does not exist in the primitive.",context(primitive));
            if(uv_count>1) warning("DAE_UNUSED_UV_SETS","UV set "+std::to_string(selected_uv)+" is retained (diffuse binding, or lowest set for untextured material); other unbound UV sets are validated.",context(primitive));
            std::vector<std::vector<double>> faces;
            const auto count=integer(primitive,"count",0,true);std::size_t face_count=0;
            if(kind=="polygons") {
                for(auto p=primitive->FirstChildElement("p");p;p=p->NextSiblingElement("p")) faces.push_back(numbers(p));
            } else {
                const auto data=numbers(one(primitive,"p",true));std::vector<double> counts;
                if(kind=="polylist") counts=numbers(one(primitive,"vcount",true));
                else {if(!stride||count>data.size()/stride/3) bad("Triangle count exceeds index data.",primitive);counts.assign(count,3);}
                if(counts.size()!=count) bad("Primitive face count mismatch.",primitive);
                std::size_t at=0;for(auto n:counts) {
                    if(!finite(n)||n!=std::floor(n)||n<3||n>4096||n*stride>data.size()-at) bad("Invalid primitive face count.",primitive);
                    const auto end=at+static_cast<std::size_t>(n)*stride;faces.emplace_back(data.begin()+at,data.begin()+end);at=end;
                }
                if(at!=data.size()) bad("Trailing primitive indices.",primitive);
            }
            for(const auto& face:faces) {
                ++face_count;if(!stride||face.size()%stride||face.size()/stride<3||face.size()/stride>4096) bad("Invalid polygon index count.",primitive);
                const auto corner_count=face.size()/stride;
                if(result.vertices.size()+(corner_count-2)*3>10000000) reject("MESH_TOO_LARGE","Expanded COLLADA mesh exceeds the corner limit.",context(g));
                std::vector<Vertex> corners;std::vector<ufbx_vec3> points;
                for(std::size_t k=0;k<corner_count;++k) {
                    for(std::size_t offset=0;offset<stride;++offset) {const auto n=face[k*stride+offset];if(!finite(n)||n<0||n!=std::floor(n)||n>30000000) bad("Invalid primitive index.",primitive);}
                    Vertex v;
                    for(const auto& i:inputs) {
                        auto values=sample(i,static_cast<std::size_t>(face[k*stride+i.offset]),i.semantic=="TEXCOORD"?2:3);
                        if(i.semantic=="POSITION") {
                            v.position=point(transformed,{values[0],values[1],values[2]});
                            if(!finite(v.position.x)||!finite(v.position.y)||!finite(v.position.z)) reject("NONFINITE_VERTEX","COLLADA position is nonfinite.",context(g));
                        } else if(i.semantic=="NORMAL") {
                            const double length=std::hypot(values[0],values[1],values[2]);
                            if(!finite(length)||!length) {if(!options.gis_static) reject("INVALID_NORMAL","COLLADA normal is zero or nonfinite.",context(g));v.normal={};}
                            else v.normal=normal(transformed,{values[0]/length,values[1]/length,values[2]/length},det);
                            v.has_normal=true;
                        } else {
                            for(auto x:values) if(!finite(x)) reject("NONFINITE_UV","COLLADA texture coordinate is nonfinite.",context(g));
                            if(i.set==selected_uv) {v.uv={values[0],values[1]};v.has_uv=true;}
                        }
                    }
                    if(b.value.texture>=0&&!v.has_uv) reject("MISSING_UV","Retained COLLADA texture has no UV coordinates.",context(g));
                    corners.push_back(v);points.push_back({v.position.x,v.position.y,v.position.z});
                }
                polygon(corners);std::vector<std::uint32_t> order((corner_count-2)*3);
                if(corner_count==3) order={0,1,2};
                else {
                    ufbx_vertex_vec3 positions{};positions.exists=true;positions.values={points.data(),points.size()};
                    std::vector<std::uint32_t> indices(corner_count);for(std::size_t k=0;k<corner_count;++k) indices[k]=static_cast<std::uint32_t>(k);
                    positions.indices={indices.data(),indices.size()};ufbx_mesh temp{};temp.vertex_position=positions;temp.num_indices=corner_count;
                    ufbx_panic panic{};
                    const auto n=ufbx_catch_triangulate_face(&panic,order.data(),order.size(),&temp,{0,static_cast<std::uint32_t>(corner_count)});
                    if(panic.did_panic||n!=corner_count-2) bad("Polygon triangulation was incomplete.",primitive);
                }
                for(std::size_t t=0;t<order.size();t+=3) {
                    if(det<0) std::swap(order[t+1],order[t+2]);
                    const auto n=face_normal(corners[order[t]].position,corners[order[t+1]].position,corners[order[t+2]].position);
                    const bool degenerate=std::hypot(n.x,n.y,n.z)==0;
                    if(degenerate) {
                        if(!options.gis_static) reject("DEGENERATE_TRIANGLE","COLLADA triangle has zero area.",context(g));
                        ++discarded;for(int k=0;k<3;++k) {const auto& v=corners[order[t+k]];if(v.has_normal&&(!finite(std::hypot(v.normal.x,v.normal.y,v.normal.z))||!std::hypot(v.normal.x,v.normal.y,v.normal.z))) ++discarded_normals;}continue;
                    }
                    Triangle triangle;triangle.material=material_index;bool repair=false;
                    for(int k=0;k<3;++k) {
                        auto v=corners[order[t+k]];const auto length=std::hypot(v.normal.x,v.normal.y,v.normal.z);
                        if(!v.has_normal) {v.normal=n;v.has_normal=true;++generated;}
                        else if(!finite(length)||!length) {
                            if(!options.gis_static) reject("INVALID_NORMAL","Transformed COLLADA normal is invalid.",context(g));
                            v.normal=n;++repaired;repair=true;
                        }
                        triangle.indices[k]=static_cast<std::uint32_t>(result.vertices.size());result.vertices.push_back(v);
                    }
                    if(repair) ++repaired_triangles;
                    result.triangles.push_back(triangle);
                }
            }
            if(face_count!=count) bad("Primitive polygon count mismatch.",primitive);
        }
        if(generated) warning("DAE_DEFAULT_NORMALS_GENERATED","COLLADA has no explicit normals; generated flat normals for "+std::to_string(generated)+" corners.",context(g));
        if(repaired) warning("NORMALS_REPAIRED","GIS static rebuilt "+std::to_string(repaired)+" invalid COLLADA corner normals on "+std::to_string(repaired_triangles)+" triangles.",context(g));
        if(discarded) warning("DEGENERATE_TRIANGLES_REMOVED","GIS static removed "+std::to_string(discarded)+" finite zero-area COLLADA triangles.",context(g));
        if(discarded_normals) warning("DEGENERATE_NORMALS_DISCARDED","Discarded "+std::to_string(discarded_normals)+" invalid normals with removed zero-area triangles.",context(g));
        out.nodes[node_index].meshes.push_back(static_cast<std::uint32_t>(out.meshes.size()));out.meshes.push_back(std::move(result));
    }
    void visit(Element* e,int parent,const Matrix& inherited,std::size_t depth) {
        if(depth>128||++visits>100000||!active.insert(e).second) bad("Node graph is cyclic or exceeds its limits.",e);
        children(e,{"translate","rotate","scale","matrix","node","instance_node","instance_geometry"});
        const auto type=attribute(e,"type","NODE");if(type!="NODE") reject("UNSUPPORTED_DAE_DEFORMATION","Joint nodes require a static export.",context(e));
        if(e->Attribute("layer")) reject("UNSUPPORTED_DAE_ELEMENT","Node layer visibility is not represented.",context(e));
        Matrix local=identity();
        for(auto t=e->FirstChildElement();t;t=t->NextSiblingElement()) {
            const std::string kind=t->Name();Matrix m=identity();
            if(kind=="translate"||kind=="scale") {
                const auto v=numbers(t,3);if(kind=="translate") {m[12]=v[0];m[13]=v[1];m[14]=v[2];}else {m[0]=v[0];m[5]=v[1];m[10]=v[2];}
            } else if(kind=="matrix") {
                const auto v=numbers(t,16);for(int c=0;c<4;++c) for(int r=0;r<4;++r) m[c*4+r]=v[r*4+c];
                if(m[3]!=0||m[7]!=0||m[11]!=0||m[15]!=1) reject("INVALID_TRANSFORM","Only affine COLLADA matrices are supported.",context(t));
            } else if(kind=="rotate") {
                const auto v=numbers(t,4);const auto length=std::hypot(v[0],v[1],v[2]);if(!finite(length)||!length) bad("Invalid rotation axis.",t);
                const auto x=v[0]/length,y=v[1]/length,z=v[2]/length,a=v[3]*3.14159265358979323846/180,c=std::cos(a),s=std::sin(a),q=1-c;
                m={q*x*x+c,q*x*y+s*z,q*x*z-s*y,0,q*x*y-s*z,q*y*y+c,q*y*z+s*x,0,q*x*z+s*y,q*y*z-s*x,q*z*z+c,0,0,0,0,1};
            } else continue;
            for(double v:m) if(!finite(v)) reject("NONFINITE_TRANSFORM","COLLADA transform is nonfinite.",context(t));
            local=multiply(local,m);
        }
        const auto world=multiply(inherited,local);for(double v:world) if(!finite(v)) reject("NONFINITE_TRANSFORM","COLLADA world transform is nonfinite.",context(e));
        Node n;n.name=attribute(e,"name",context(e));n.source_id=context(e);n.parent=parent;n.source_world_transform=world;
        const auto index=static_cast<int>(out.nodes.size());out.nodes.push_back(std::move(n));
        for(auto c=e->FirstChildElement();c;c=c->NextSiblingElement()) {
            const std::string kind=c->Name();if(kind=="node") visit(c,index,world,depth+1);
            else if(kind=="instance_node") {children(c,{});visit(reference(attribute(c,"url"),"node"),index,world,depth+1);}
            else if(kind=="instance_geometry") geometry(c,index,world);
        }
        active.erase(e);
    }
public:
    Converter(Scene& s,const ReaderOptions& o,std::filesystem::path directory):out(s),options(o),root(std::move(directory)) {}
    void convert(Element* doc) {
        if(!doc||std::string(doc->Name())!="COLLADA"||attribute(doc,"version")!="1.4.1"||attribute(doc,"xmlns")!="http://www.collada.org/2005/11/COLLADASchema")
            reject("UNSUPPORTED_DAE_VERSION","Expected COLLADA 1.4.1 with its standard namespace.","scene");
        index(doc);children(doc,{"asset","library_images","library_effects","library_materials","library_geometries","library_nodes","library_visual_scenes","scene"});
        auto asset=one(doc,"asset",true);children(asset,{"contributor","created","modified","keywords","revision","subject","title","unit","up_axis"});
        double unit=1;if(auto u=one(asset,"unit")) {
            const auto value=attribute(u,"meter","1");const auto parsed=std::from_chars(value.data(),value.data()+value.size(),unit);
            if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size()||!finite(unit)||unit<=0) bad("Asset unit size must be finite and positive.",u);
        }
        const auto axis=one(asset,"up_axis");const auto up=axis?text(axis):"Y_UP";
        if(up=="Z_UP") basis={1,0,0,0,0,0,-1,0,0,1,0,0,0,0,0,1};
        else if(up=="X_UP") basis={0,1,0,0,-1,0,0,0,0,0,1,0,0,0,0,1};
        else if(up!="Y_UP") bad("Invalid COLLADA up_axis.",axis);
        for(int c=0;c<3;++c) for(int r=0;r<3;++r) basis[c*4+r]*=unit;
        std::ostringstream unit_text;unit_text<<std::setprecision(17)<<unit;
        warning("DAE_COORDINATE_CONVENTION","COLLADA right-handed "+up+", "+unit_text.str()+" metres per source unit, converted once to right-handed Z-up metres; node transforms baked once.","scene");
        for(auto pair:{std::pair{"library_images","image"},std::pair{"library_effects","effect"},std::pair{"library_materials","material"},std::pair{"library_geometries","geometry"},std::pair{"library_nodes","node"},std::pair{"library_visual_scenes","visual_scene"}})
            for(auto library=doc->FirstChildElement(pair.first);library;library=library->NextSiblingElement(pair.first)) children(library,{pair.second});
        auto scene=one(doc,"scene",true);children(scene,{"instance_visual_scene"});auto instance=one(scene,"instance_visual_scene",true);children(instance,{});
        auto visual=reference(attribute(instance,"url"),"visual_scene");children(visual,{"node"});
        for(auto n=visual->FirstChildElement();n;n=n->NextSiblingElement()) visit(n,-1,identity(),0);
    }
};
} // namespace
Scene read_dae(const std::filesystem::path& input,const ReaderOptions& options) {
    const auto path=std::filesystem::absolute(input);Scene out;out.name=path.stem().u8string();out.source=path.u8string();
    out.conversion_profile=options.gis_static?"gis-static":"strict";out.missing_texture_policy=options.missing_texture_fallback?"material-color":"error";
    try {
        const auto bytes=read_bytes(path,options.max_file_bytes);
        if(bytes.empty()||std::find(bytes.begin(),bytes.end(),0)!=bytes.end()) bad("COLLADA must be nonempty UTF-8 XML without NUL bytes.");
        tinyxml2::XMLDocument doc;
        if(doc.Parse(reinterpret_cast<const char*>(bytes.data()),bytes.size())!=tinyxml2::XML_SUCCESS) bad("Malformed COLLADA XML: "+label(doc.ErrorStr(),"parse failure"));
        for(auto n=doc.FirstChild();n;n=n->NextSibling()) {
            if(n->ToUnknown()) reject("UNSUPPORTED_DAE_XML","XML DTDs and external entities are not supported.","scene");
            if(n->ToElement()&&n!=doc.RootElement()) bad("XML has multiple root elements.");
        }
        Converter(out,options,std::filesystem::weakly_canonical(path.parent_path())).convert(doc.RootElement());
    } catch(const Issue& e) {out.diagnostics.push_back({Severity::error,e.code,e.what(),e.context});}
    return out;
}
} // namespace gmb
