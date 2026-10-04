#include "gmb/scene.hpp"
#include "reader_util.hpp"
#include "tinyxml2.h"
#include "ufbx.h"
#include <charconv>
#include <iomanip>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
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

// X3D XML is another encoding of the same static scene graph. Decode directly
// into the shared graph; never serialize geometry through a temporary VRML file.
class XParser {
    using Element=const tinyxml2::XMLElement;
    std::map<std::string,Ref> definitions;
    std::size_t node_count=0;
    double length_unit=1;
    std::string version;
    static bool space(char c) {return c==' '||c=='\t'||c=='\r'||c=='\n';}
    static std::string value(Element* e,const char* key) {return label(e->Attribute(key),"");}
    [[noreturn]] static void bad(const std::string& message,Element* e=nullptr) {
        reject("INVALID_X3D",message,e?"line:"+std::to_string(e->GetLineNum()):"scene");
    }
    static void contents(Element* e) {
        for(auto n=e->FirstChild();n;n=n->NextSibling()) {
            if(n->ToComment()||n->ToElement()) continue;
            if(auto t=n->ToText()) {
                const std::string_view text=t->Value();
                if(std::all_of(text.begin(),text.end(),space)) continue;
            }
            bad("Only child elements, comments and whitespace are supported inside X3D elements.",e);
        }
    }
    static void attributes(Element* e,std::initializer_list<const char*> allowed) {
        for(auto a=e->FirstAttribute();a;a=a->Next()) {
            if(std::none_of(allowed.begin(),allowed.end(),[&](const char* key){return std::string_view(a->Name())==key;}))
                reject("UNSUPPORTED_X3D_FIELD","Unsupported attribute: "+std::string(e->Name())+"."+a->Name(),"line:"+std::to_string(e->GetLineNum()));
        }
    }
    static void declaration(std::string_view text) {
        if(text.substr(0,4)!="xml ") reject("UNSUPPORTED_X3D_XML","XML processing instructions are not supported.","scene");
        std::size_t at=4;std::map<std::string,std::string> values;
        while(at<text.size()) {
            while(at<text.size()&&space(text[at])) ++at;
            if(at==text.size()) break;
            const auto start=at;while(at<text.size()&&std::isalpha(static_cast<unsigned char>(text[at]))) ++at;
            if(start==at) bad("Invalid XML declaration.");
            const std::string key(text.substr(start,at-start));
            while(at<text.size()&&space(text[at])) ++at;
            if(at==text.size()||text[at++]!='=') bad("Invalid XML declaration.");
            while(at<text.size()&&space(text[at])) ++at;
            if(at==text.size()||(text[at]!='\''&&text[at]!='"')) bad("Invalid XML declaration.");
            const auto quote=text[at++];const auto begin=at;const auto end=text.find(quote,at);
            if(end==std::string_view::npos) bad("Invalid XML declaration.");
            if(!values.emplace(key,text.substr(begin,end-begin)).second) bad("Duplicate XML declaration attribute.");
            at=end+1;if(at<text.size()&&!space(text[at])) bad("Invalid XML declaration separator.");
        }
        for(const auto& item:values) if(item.first!="version"&&item.first!="encoding"&&item.first!="standalone") bad("Unknown XML declaration attribute.");
        if(values["version"]!="1.0"||(values.count("encoding")&&values["encoding"]!="UTF-8"&&values["encoding"]!="utf-8")||
            (values.count("standalone")&&values["standalone"]!="yes"&&values["standalone"]!="no"))
            reject("UNSUPPORTED_X3D_XML","Only XML 1.0 with UTF-8 encoding is supported.","scene");
    }
    static double number(std::string_view token,bool integer,bool normal=false) {
        if(token.empty()) bad("Expected a number.");
        const char* begin=token.data();const char* end=begin+token.size();
        if(*begin=='+') {++begin;if(begin==end||*begin=='+'||*begin=='-') bad("Invalid numeric sign.");}
        if(integer) {
            std::int64_t n=0;const auto parsed=std::from_chars(begin,end,n);
            if(parsed.ec!=std::errc{}||parsed.ptr!=end||n < -1||n>2147483647) bad("Invalid integer index.");
            return static_cast<double>(n);
        }
        double n=0;
        if(normal&&(token=="NaN"||token=="INF"||token=="-INF"))
            return token=="NaN"?std::numeric_limits<double>::quiet_NaN():token=="INF"?INFINITY:-INFINITY;
        const auto parsed=std::from_chars(begin,end,n);
        if(parsed.ec!=std::errc{}||parsed.ptr!=end||!finite(n)) bad("Expected a finite decimal number.");
        return n;
    }
    static Field field_value(const std::string& kind,std::string_view text,bool normal) {
        Field result;
        if(kind=="string") {result.strings.emplace_back(text);return result;}
        std::size_t at=0;
        auto skip=[&] {while(at<text.size()&&(space(text[at])||text[at]==',')) ++at;};
        if(kind=="strings") {
            skip();
            while(at<text.size()) {
                if(text[at++]!='"') bad("MFString values must be double-quoted inside the XML attribute.");
                std::string item;bool closed=false;
                while(at<text.size()) {
                    auto c=text[at++];
                    if(c=='"') {closed=true;break;}
                    if(c=='\\') {
                        if(at==text.size()) bad("Unterminated MFString escape.");
                        c=text[at++];if(c!='"'&&c!='\\') bad("Unsupported MFString escape.");
                    }
                    item.push_back(c);
                }
                if(!closed) bad("Unterminated MFString value.");
                result.strings.push_back(std::move(item));
                if(result.strings.size()>100000) bad("MFString count limit exceeded.");
                if(at<text.size()&&!space(text[at])&&text[at]!=',') bad("MFString values require separators.");
                skip();
            }
            return result;
        }
        skip();
        while(at<text.size()) {
            const auto begin=at;while(at<text.size()&&!space(text[at])&&text[at]!=',') ++at;
            const auto token=text.substr(begin,at-begin);
            if(token.size()>1048576) bad("Numeric token limit exceeded.");
            if(kind=="bool") {
                if(token!="true"&&token!="false") bad("X3D XML booleans must be true or false.");
                result.numbers.push_back(token=="true"?1:0);
            } else result.numbers.push_back(number(token,kind=="ints",normal));
            if(result.numbers.size()>30000000) bad("Numeric field size limit exceeded.");
            skip();
        }
        const bool multi=kind=="ints"||kind.rfind("multi",0)==0;
        const auto width=kind=="bool"||kind=="ints"?1:std::stoi(multi?kind.substr(5):kind);
        if((!multi&&result.numbers.size()!=static_cast<std::size_t>(width))||result.numbers.size()%width)
            bad("X3D field has the wrong number of components.");
        return result;
    }
    static std::string container(const std::string& type) {
        if(type=="Appearance") return "appearance";
        if(type=="IndexedFaceSet") return "geometry";
        if(type=="Material") return "material";
        if(type=="ImageTexture") return "texture";
        if(type=="TextureTransform") return "textureTransform";
        if(type=="Coordinate") return "coord";
        if(type=="Normal") return "normal";
        if(type=="TextureCoordinate") return "texCoord";
        if(type=="Color") return "color";
        return "children";
    }
    static void identifier(const std::string& name,Element* e) {
        if(name.empty()||(!std::isalpha(static_cast<unsigned char>(name[0]))&&name[0]!='_'&&static_cast<unsigned char>(name[0])<128)) bad("Invalid DEF/USE name.",e);
        for(unsigned char c:name) if(c<128&&!std::isalnum(c)&&c!='_'&&c!='-'&&c!='.') bad("Invalid DEF/USE name.",e);
    }
    Ref node(Element* e,std::size_t depth) {
        if(depth>128||++node_count>100000) bad("Node depth/count limit exceeded.",e);
        contents(e);const std::string type=e->Name();const auto schema=schemas.find(type);
        if(schema==schemas.end()) reject("UNSUPPORTED_X3D_NODE","Unsupported X3D node or statement: "+type,"line:"+std::to_string(e->GetLineNum()));
        if(e->Attribute("USE")) {
            attributes(e,{"USE","containerField"});const auto name=value(e,"USE");identifier(name,e);
            const auto found=definitions.find(name);
            if(found==definitions.end()||found->second->type!=type||e->FirstChildElement()) bad("USE must reference a preceding completed DEF of the same type and have no child nodes.",e);
            return found->second;
        }
        auto result=std::make_shared<WNode>();result->type=type;result->name=value(e,"DEF");
        if(e->Attribute("DEF")) {
            identifier(result->name,e);
            if(definitions.count(result->name)) bad("Duplicate DEF name.",e);
        }
        for(auto a=e->FirstAttribute();a;a=a->Next()) {
            const std::string key=a->Name();if(key=="DEF"||key=="containerField") continue;
            const auto f=schema->second.find(key);
            if(f==schema->second.end()||f->second=="node"||f->second=="nodes")
                reject("UNSUPPORTED_X3D_FIELD","Unsupported X3D attribute: "+type+"."+key,"line:"+std::to_string(e->GetLineNum()));
            result->fields.emplace(key,field_value(f->second,a->Value(),type=="Normal"&&key=="vector"));
        }
        for(auto c=e->FirstChildElement();c;c=c->NextSiblingElement()) {
            const auto type_name=std::string(c->Name());
            const auto field_name=c->Attribute("containerField")?value(c,"containerField"):container(type_name);
            const auto f=schema->second.find(field_name);
            if(f==schema->second.end()||(f->second!="node"&&f->second!="nodes")||field_name!=container(type_name))
                reject("UNSUPPORTED_X3D_FIELD","Invalid containerField or child node: "+type+"."+field_name,"line:"+std::to_string(c->GetLineNum()));
            auto& children=result->fields[field_name].nodes;
            if(f->second=="node"&&!children.empty()) bad("Duplicate SFNode field.",c);
            children.push_back(node(c,depth+1));
        }
        if(!result->name.empty()&&!definitions.emplace(result->name,result).second) bad("Duplicate DEF name.",e);
        return result;
    }
public:
    // tinyxml2's public text uses C strings, so reject invalid raw UTF-8/XML
    // characters and invalid character references before decoding can truncate.
    static void lexical(std::string_view text) {
        for(std::size_t at=0;at<text.size();) {
            const auto first=static_cast<unsigned char>(text[at++]);std::uint32_t code=first;int remaining=0;
            if(first>=0xc2&&first<=0xdf) {code=first&31;remaining=1;}
            else if(first>=0xe0&&first<=0xef) {code=first&15;remaining=2;}
            else if(first>=0xf0&&first<=0xf4) {code=first&7;remaining=3;}
            else if(first>=128) bad("Invalid UTF-8 XML input.");
            const auto width=remaining;
            while(remaining--) {if(at==text.size()||(static_cast<unsigned char>(text[at])&0xc0)!=0x80) bad("Invalid UTF-8 XML input.");code=(code<<6)|(static_cast<unsigned char>(text[at++])&63);}
            if((width==1&&code<0x80)||(width==2&&code<0x800)||(width==3&&code<0x10000)||
                !((code==9||code==10||code==13)||(code>=0x20&&code<=0xd7ff)||(code>=0xe000&&code<=0xfffd)||(code>=0x10000&&code<=0x10ffff))) bad("Invalid XML character.");
        }
        bool tag=false;char quote=0;
        for(std::size_t at=0;at<text.size();) {
            if(tag&&(text[at]=='\''||text[at]=='"')) {
                if(!quote) quote=text[at];else if(quote==text[at]) quote=0;
                ++at;continue;
            }
            if(tag&&text[at]=='>') {if(!quote) tag=false;++at;continue;}
            if(text[at]=='<'&&tag) bad("Unexpected '<' inside an XML tag or attribute.");
            if(text.compare(at,4,"<!--")==0||text.compare(at,9,"<![CDATA[")==0||text.compare(at,2,"<?")==0) {
                const bool comment=text.compare(at,4,"<!--")==0,cdata=text.compare(at,9,"<![CDATA[")==0;
                const auto end=text.find(comment?"-->":cdata?"]]>":"?>",at+(comment?4:cdata?9:2));
                if(end==std::string_view::npos) bad("Unterminated XML markup.");
                at=end+(comment||cdata?3:2);continue;
            }
            if(text.compare(at,2,"<!")==0) reject("UNSUPPORTED_X3D_XML","DTD and external entity declarations are not supported.","scene");
            if(text[at]=='<') {tag=true;++at;continue;}
            if(text[at]!='&') {++at;continue;}
            const auto end=text.find(';',at+1);if(end==std::string_view::npos) bad("Unterminated XML entity.");
            const auto entity=text.substr(at+1,end-at-1);
            if(!entity.empty()&&entity[0]=='#') {
                const auto start=entity.size()>1&&entity[1]=='x'?2u:1u;std::uint32_t code=0;
                const auto p=std::from_chars(entity.data()+start,entity.data()+entity.size(),code,start==2?16:10);
                if(p.ec!=std::errc{}||p.ptr!=entity.data()+entity.size()||
                    !((code==9||code==10||code==13)||(code>=0x20&&code<=0xd7ff)||(code>=0xe000&&code<=0xfffd)||(code>=0x10000&&code<=0x10ffff))) bad("Invalid XML character reference.");
            } else if(entity!="amp"&&entity!="lt"&&entity!="gt"&&entity!="apos"&&entity!="quot") bad("Unknown XML entity.");
            at=end+1;
        }
    }
    std::vector<Ref> parse(const tinyxml2::XMLDocument& doc,Scene& scene) {
        const auto root=doc.RootElement();if(!root||std::string_view(root->Name())!="X3D") bad("Expected an X3D document root.");
        for(auto n=doc.FirstChild();n;n=n->NextSibling()) {
            if(n->ToComment()||n==root) continue;
            if(auto d=n->ToDeclaration()) {
                if(n!=doc.FirstChild()) reject("UNSUPPORTED_X3D_XML","Only a leading XML declaration is supported.","scene");
                declaration(d->Value());
                continue;
            }
            if(auto t=n->ToText()) {const std::string_view text=t->Value();if(std::all_of(text.begin(),text.end(),space)) continue;}
            bad("Unexpected XML content outside the X3D root.");
        }
        attributes(root,{"profile","version","xmlns:xsd","xsd:noNamespaceSchemaLocation","xmlns:xsi","xsi:noNamespaceSchemaLocation"});contents(root);
        version=value(root,"version");if(version!="3.0"&&version!="3.1"&&version!="3.2"&&version!="3.3")
            reject("UNSUPPORTED_X3D_VERSION","Only X3D 3.0 through 3.3 XML static scenes are supported.","scene");
        const auto profile=value(root,"profile");
        if(profile!="Interchange"&&profile!="Interactive"&&profile!="Immersive"&&profile!="Full"&&profile!="CADInterchange")
            reject("UNSUPPORTED_X3D_PROFILE","Export an Interchange/Interactive/Immersive/Full/CADInterchange static subset.","scene");
        for(const auto key:{"xmlns:xsd","xmlns:xsi"}) if(root->Attribute(key)&&value(root,key)!="http://www.w3.org/2001/XMLSchema-instance") bad("Unsupported XML schema namespace.",root);
        Element* graph=nullptr;bool head_seen=false,unit_seen=false;
        for(auto e=root->FirstChildElement();e;e=e->NextSiblingElement()) {
            const std::string kind=e->Name();contents(e);
            if(kind=="head") {
                if(head_seen||graph) bad("head must occur once before Scene.",e);
                head_seen=true;attributes(e,{});
                for(auto h=e->FirstChildElement();h;h=h->NextSiblingElement()) {
                    if(++node_count>100000) bad("Node count limit exceeded.",h);
                    contents(h);if(h->FirstChildElement()) bad("head entries cannot contain child elements.",h);
                    if(std::string_view(h->Name())=="meta") {
                        attributes(h,{"name","content"});if(!h->Attribute("name")||!h->Attribute("content")) bad("meta requires name and content.",h);
                        scene.diagnostics.push_back({Severity::warning,"X3D_METADATA_OMITTED","X3D head metadata is not a render channel and is omitted.",value(h,"name")});
                    } else if(std::string_view(h->Name())=="unit") {
                        attributes(h,{"category","name","conversionFactor"});
                        if(version!="3.3"||value(h,"category")!="length"||value(h,"name").empty()||unit_seen)
                            reject("UNSUPPORTED_X3D_UNIT","Only one X3D 3.3 length unit declaration is supported.","scene");
                        length_unit=number(value(h,"conversionFactor"),false);if(length_unit<=0) bad("Length unit must be positive.",h);unit_seen=true;
                    } else reject("UNSUPPORTED_X3D_NODE","Unsupported head declaration: "+std::string(h->Name()),"scene");
                }
            } else if(kind=="Scene") {if(graph) bad("Expected one Scene element.",e);attributes(e,{});graph=e;}
            else reject("UNSUPPORTED_X3D_NODE","Unsupported X3D root child: "+kind,"scene");
        }
        if(!graph) bad("Missing Scene element.");
        std::vector<Ref> result;
        for(auto e=graph->FirstChildElement();e;e=e->NextSiblingElement()) {
            if(e->Attribute("containerField")&&value(e,"containerField")!="children") bad("Invalid root containerField.",e);
            result.push_back(node(e,0));
        }
        return result;
    }
    double meters_per_unit() const {return length_unit;}
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
// A rounded cross product of zero does not prove a triangle has zero area.
// Preserve the same conservative exact-zero policy as the other mesh readers.
bool zero_area(Vec3 a,Vec3 b) {
    const auto sa=(std::max)({std::abs(a.x),std::abs(a.y),std::abs(a.z)}),sb=(std::max)({std::abs(b.x),std::abs(b.y),std::abs(b.z)});
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
Vec3 face_normal(Vec3 a,Vec3 b,Vec3 c) {
    auto u=subtract(b,a),v=subtract(c,a);
    const double scale=(std::max)({std::abs(u.x),std::abs(u.y),std::abs(u.z),std::abs(v.x),std::abs(v.y),std::abs(v.z)});
    if(!finite(scale)) reject("NONFINITE_VERTEX","Transformed edge is nonfinite.","geometry");
    if(!scale||zero_area(u,v)) return {};
    u={u.x/scale,u.y/scale,u.z/scale};v={v.x/scale,v.y/scale,v.z/scale};
    const auto f=cross(u,v);const auto length=std::hypot(f.x,f.y,f.z);
    if(!finite(length)||length==0) reject("INVALID_WRL_FACE","Cannot reliably normalize a nonzero triangle; export better-conditioned geometry.","geometry");
    return {f.x/length,f.y/length,f.z/length};
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
    bool x3d=false;
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
        std::vector<bool> invalid_normals(ns.size()/3,false),used_normals(ns.size()/3,false);
        for(std::size_t i=0;i<invalid_normals.size();++i) {
            const Vec3 n{ns[i*3],ns[i*3+1],ns[i*3+2]};const auto length=std::hypot(n.x,n.y,n.z);
            invalid_normals[i]=!finite(n.x)||!finite(n.y)||!finite(n.z)||length==0;
            if(!invalid_normals[i]&&(!finite(length)||std::abs(length-1)>1e-3))
                reject("INVALID_NORMAL","Finite nonzero WRL normals must have unit length, including unused normals; GIS static does not replace their direction.",source->name);
            if(invalid_normals[i]&&!options.gis_static)
                reject("INVALID_NORMAL","WRL explicit normal is zero or nonfinite, including unused normals.",source->name);
        }
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
                // X3D 3.3 section 17.2.2.3's prose and table 17.3 disagree
                // about modulation versus replacement. Accept only combinations
                // whose result is identical under both rules, rather than guess.
                if(x3d&&mat&&((replace_color&&(material.color.r!=1||material.color.g!=1||material.color.b!=1||
                    std::any_of(cs.begin(),cs.end(),[](double c){return c!=1;})))||(replace_alpha&&material.color.a!=1)))
                    reject("AMBIGUOUS_X3D_TEXTURE_MATERIAL","Export neutral white RGB/Color with RGB textures and scalar opacity 1 with alpha textures; X3D 3.x modulation/replacement semantics are ambiguous.",material.name);
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
                    used_normals[index]=true;
                    Vec3 n{ns[index*3],ns[index*3+1],ns[index*3+2]};const double length=std::hypot(n.x,n.y,n.z);
                    if(!finite(n.x)||!finite(n.y)||!finite(n.z)||length==0) {
                        if(!options.gis_static) reject("INVALID_NORMAL","WRL normal must have unit length.",mesh.name);
                        v.normal={};
                    } else {
                        if(!finite(length)||std::abs(length-1)>1e-3)
                            reject("INVALID_NORMAL","Finite nonzero WRL normals must have unit length; GIS static does not replace their direction.",mesh.name);
                        v.normal=normal(world,n,det);
                    }
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
                    const auto a=points[triangles[i]],b=points[triangles[i+1]],c=points[triangles[i+2]];
                    const Vec3 u{b.x-a.x,b.y-a.y,b.z-a.z},v{c.x-a.x,c.y-a.y,c.z-a.z};
                    if(!finite(u.x)||!finite(u.y)||!finite(u.z)||!finite(v.x)||!finite(v.y)||!finite(v.z)||!zero_area(u,v))
                        reject("INVALID_WRL_FACE","A nonzero source triangle collapsed under its coordinate transform; geometry cannot be preserved.",mesh.name);
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
        for(std::size_t i=0;i<invalid_normals.size();++i) if(invalid_normals[i]&&!used_normals[i])
            reject("INVALID_NORMAL","Unused WRL normal is zero or nonfinite; no retained source triangle can repair it.",mesh.name);
        if(material.texture>=0&&!uv) warning("WRL_DEFAULT_UV_GENERATED","Generated VRML97 bounding-box UVs in local coordinates.",mesh.name);
        if(!normals) warning("WRL_DEFAULT_NORMALS_GENERATED","Generated flat normals for creaseAngle 0.",mesh.name);
        if(repaired) warning("NORMALS_REPAIRED","GIS static profile rebuilt "+std::to_string(repaired)+" invalid corner normals across "+std::to_string(repaired_triangles)+" triangles.",mesh.name);
        if(discarded) warning("DEGENERATE_TRIANGLES_REMOVED","GIS static profile removed "+std::to_string(discarded)+" strictly zero-area triangles; no area tolerance was used.",mesh.name);
        if(discarded_normals) warning("DEGENERATE_NORMALS_DISCARDED","Discarded "+std::to_string(discarded_normals)+" invalid normals with zero-area triangles.",mesh.name);
        if(mesh.triangles.empty()) reject("EMPTY_MESH","No retained WRL triangles.",mesh.name);
        out.nodes[node_index].meshes.push_back(static_cast<std::uint32_t>(out.meshes.size()));out.meshes.push_back(std::move(mesh));
    }
public:
    Converter(Scene& scene,const ReaderOptions& opts,std::filesystem::path directory,bool xml=false):out(scene),options(opts),root(std::move(directory)),x3d(xml){}
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
        node.source_id=(x3d?"X3D:":"VRML97:")+node.name;node.parent=parent;node.source_world_transform=world;
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
Scene read_x3d(const std::filesystem::path& input,const ReaderOptions& options) {
    Scene out;const auto path=std::filesystem::absolute(input);out.name=path.stem().u8string();out.source=path.u8string();
    out.conversion_profile=options.gis_static?"gis-static":"strict";out.missing_texture_policy=options.missing_texture_fallback?"material-color":"error";
    try {
        const auto bytes=read_bytes(path,options.max_file_bytes);
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        XParser::lexical(text);tinyxml2::XMLDocument doc;
        if(doc.Parse(text.data(),text.size())!=tinyxml2::XML_SUCCESS)
            reject("INVALID_X3D","Malformed X3D XML: "+label(doc.ErrorStr(),"parse failure"),"scene");
        XParser parser;auto nodes=parser.parse(doc,out);auto basis=identity();
        basis[0]=basis[5]=basis[10]=parser.meters_per_unit();
        std::ostringstream unit;unit<<std::setprecision(std::numeric_limits<double>::max_digits10)<<parser.meters_per_unit();
        out.diagnostics.push_back({Severity::warning,"X3D_COORDINATE_CONVENTION","X3D right-handed Y-up coordinates converted once to right-handed Z-up metres; source metres per unit="+unit.str(),"scene"});
        Converter converter(out,options,std::filesystem::weakly_canonical(path.parent_path()),true);
        for(const auto& node:nodes) converter.visit(node,-1,basis);
    } catch(const Issue& e) {out.diagnostics.push_back({Severity::error,e.code,e.what(),e.context});}
    // The converter and geometry validation are shared with WRL. Keep reports
    // accurate about the source encoding without duplicating those algorithms.
    for(auto& d:out.diagnostics) {
        const auto replace=[](std::string& s,const std::string& from,const std::string& to) {
            std::size_t at=0;while((at=s.find(from,at))!=std::string::npos) {s.replace(at,from.size(),to);at+=to.size();}
        };
        replace(d.code,"WRL","X3D");replace(d.message,"VRML97","X3D");replace(d.message,"VRML","X3D");replace(d.message,"WRL","X3D");
    }
    return out;
}
} // namespace gmb
