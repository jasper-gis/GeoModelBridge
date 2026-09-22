// Deliberately malformed success reports exercise the CLI process boundary.
#include "gmb/output.hpp"
#include "gmb/scene.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc,char** argv) {
    namespace fs=std::filesystem;
    using nlohmann::json;
    fs::path input,output,report;
    std::string feature;
    for(int i=1;i+1<argc;i+=2) {
        const std::string flag=argv[i];
        if(flag=="--input")input=fs::u8path(argv[i+1]);
        if(flag=="--output")output=fs::u8path(argv[i+1]);
        if(flag=="--report")report=fs::u8path(argv[i+1]);
        if(flag=="--feature-class")feature=argv[i+1];
    }
    json scene;{std::ifstream stream(input/"scene.json");stream>>scene;}
    json value={{"version",gmb::version},{"status","written_and_readback_verified"},
        {"backend","native-filegdb"},{"feature_class",feature},{"source",scene.at("source")},
        {"output",output.u8string()},{"coordinates",scene.at("coordinates")},
        {"conversion_profile",scene.at("conversion_profile")},{"missing_texture_policy",scene.at("missing_texture_policy")},
        {"coordinate_system",{{"wkid",scene.at("coordinates").at("wkid")},{"projected",true},
            {"unit","meter"},{"source_coordinates_assigned_without_reprojection",true}}},
        {"reader_diagnostics",scene.at("diagnostics")},
        {"verification",{{"geometry_material_uv_texture_readback",true},
            {"level","closed_reopened_file_geodatabase"},{"feature_count",scene.at("meshes").size()},
            {"checks",json::array({{{"mesh_index",0},{"passed",true}}})}}}};
    const auto mode=output.stem().string();
    if(mode=="wrong-source")value["source"]="unrelated.fbx";
    if(mode=="wrong-unit")value["coordinates"]["unit"]="foot";
    if(mode=="wrong-axis")value["coordinates"]["up_axis"]="Y";
    if(mode=="wrong-space")value["coordinates"]["space"]="local";
    if(mode=="fractional-wkid")value["coordinates"]["wkid"]=3857.5;
    if(mode=="overflow-wkid")value["coordinates"]["wkid"]=4294971153ull;
    if(mode=="fractional-count")value["verification"]["feature_count"]=1.5;
    if(mode=="not-projected")value["coordinate_system"]["projected"]=false;
    if(mode=="reprojected")value["coordinate_system"]["source_coordinates_assigned_without_reprojection"]=false;
    if(mode=="missing-diagnostics")value.erase("reader_diagnostics");
    if(mode=="dropped-diagnostic")value["reader_diagnostics"]=json::array();
    if(mode=="masked-error")value["diagnostics"]=json::array({{{"severity","error"},{"code","FAILED"},{"message","failure"},{"context","test"}}});
    if(mode=="unexpected-fallback")value["reader_diagnostics"].push_back({{"severity","warning"},{"code","MISSING_TEXTURE_FALLBACK"},{"message","fallback"},{"context","test"}});
    if(mode=="error-diagnostic"||mode=="unknown-diagnostic")value["reader_diagnostics"].push_back({
        {"severity",mode=="error-diagnostic"?"error":"unknown"},{"code","FAILED"},{"message","failure"},{"context","test"}});
    if(mode=="missing-checks")value["verification"].erase("checks");
    if(mode=="failed-check")value["verification"]["checks"][0]["passed"]=false;
    if(mode=="wrong-mesh")value["verification"]["checks"][0]["mesh_index"]=1;
    if(mode=="nul-string")value["extra"]=std::string("bad\0field",9);
    auto text=value.dump();
    if(mode=="duplicate-field")text.insert(1,"\"status\":\"failed\",");
    if(mode=="trailing-json")text+=" {}";
    if(mode=="nul-trailer")text+=std::string("\0ignored",8);
    if(mode=="nul-next-block")text+=std::string(65536,' ')+std::string("\0ignored",8);
    if(!fs::create_directory(output))return 1;
    gmb::io::write_exclusive(report,text);
    return 0;
}
