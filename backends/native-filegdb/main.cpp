#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "bundle.hpp"
#include <FileGDBAPI.h>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>

namespace gmb::native {
namespace fg = FileGDBAPI;
TextureData prepare_texture(const Texture &t);
std::wstring wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    require(n > 0 || s.empty(), "Invalid UTF-8.");
    std::wstring r(n, L'\0');
    if (n)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                            r.data(), n);
    return r;
}
std::string narrow(const std::wstring &s) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                                nullptr, 0, nullptr, nullptr);
    require(n > 0 || s.empty(), "Invalid UTF-16.");
    std::string r(n, '\0');
    if (n)
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                            r.data(), n, nullptr, nullptr);
    return r;
}
void check(fgdbError code, const char *what) {
    if (code == 0)
        return;
    std::wstring s;
    fg::ErrorInfo::GetErrorDescription(code, s);
    throw std::runtime_error(std::string(what) + ": " + narrow(s) + " (" + std::to_string(code) +
                             ")");
}
struct Db {
    fg::Geodatabase db;
    fg::Table table;
    bool opened = false, table_open = false;
    ~Db() {
        if (table_open)
            db.CloseTable(table);
        if (opened)
            fg::CloseGeodatabase(db);
    }
    void close() {
        if (table_open) {
            check(db.CloseTable(table), "Close table");
            table_open = false;
        }
        if (opened) {
            check(fg::CloseGeodatabase(db), "Close geodatabase");
            opened = false;
        }
    }
};
struct Options {
    fs::path input, output, report, verify, expected;
    std::string fc = "Models";
    bool probe = false, help = false;
};
bool within(const fs::path &child, const fs::path &root) {
    auto a = child.lexically_normal().wstring(), b = root.lexically_normal().wstring();
    std::transform(a.begin(), a.end(), a.begin(), ::towlower);
    std::transform(b.begin(), b.end(), b.begin(), ::towlower);
    if (a == b)
        return true;
    if (!b.empty() && b.back() != L'\\')
        b += L'\\';
    return a.rfind(b, 0) == 0;
}
Options options(int argc, wchar_t **argv) {
    Options o;
    if (argc == 1) {
        o.help = true;
        return o;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--help") {
        o.help = true;
        return o;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--probe") {
        o.probe = true;
        return o;
    }
    std::map<std::wstring, std::wstring> args;
    for (int i = 1; i < argc; i += 2) {
        require(i + 1 < argc, "Missing argument value.");
        std::wstring k = argv[i];
        require(k == L"--input" || k == L"--output" || k == L"--report" ||
                    k == L"--feature-class" || k == L"--verify-gdb" || k == L"--expected-report",
                "Unknown option: " + narrow(k));
        require(args.emplace(k, argv[i + 1]).second, "Duplicate option.");
    }
    if (args.count(L"--expected-report")) {
        require(args.size() == 3 && args.count(L"--verify-gdb") && args.count(L"--report"),
                "Standalone verification needs only --verify-gdb, --expected-report and --report.");
        o.verify = fs::absolute(args.at(L"--verify-gdb")).lexically_normal();
        o.expected = fs::absolute(args.at(L"--expected-report")).lexically_normal();
        o.report = fs::absolute(args.at(L"--report")).lexically_normal();
        require(!within(o.report, o.verify) && !within(o.report, o.expected),
                "Verification report must be new and outside GDB.");
        reject_reparse(o.verify);
        reject_reparse(o.expected);
        reject_reparse(o.report);
        return o;
    }
    require(args.count(L"--input") && (args.count(L"--output") != args.count(L"--verify-gdb")),
            "Supply --input and exactly one of --output/--verify-gdb.");
    o.input = fs::absolute(args.at(L"--input")).lexically_normal();
    if (args.count(L"--feature-class"))
        o.fc = narrow(args.at(L"--feature-class"));
    require(std::regex_match(o.fc, std::regex("[A-Za-z][A-Za-z0-9_]{0,63}")),
            "Invalid feature class name.");
    if (args.count(L"--verify-gdb"))
        o.verify = fs::absolute(args.at(L"--verify-gdb")).lexically_normal();
    else
        o.output = fs::absolute(args.at(L"--output")).lexically_normal();
    auto gdb = o.verify.empty() ? o.output : o.verify;
    auto ext = gdb.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    require(ext == L".gdb", "Geodatabase path must end in .gdb.");
    if (args.count(L"--report"))
        o.report = fs::absolute(args.at(L"--report")).lexically_normal();
    else {
        o.report = gdb;
        o.report.replace_extension(o.verify.empty() ? L"writer-report.json"
                                                    : L"native-verification.json");
    }
    require(!within(gdb, o.input) && !within(o.report, o.input) && !within(o.report, gdb),
            "GDB and report must be outside the input bundle, and report outside GDB.");
    reject_reparse(gdb);
    reject_reparse(o.report);
    reject_reparse(o.input);
    return o;
}
void ensure_new(const fs::path &p) {
    require(!fs::exists(p), "Refusing to overwrite: " + p.u8string());
}
void new_json(const fs::path &p, const json &j) {
    auto s = j.dump(2) + "\n";
    require(s.size() <= UINT32_MAX, "Report too large.");
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    require(f != INVALID_HANDLE_VALUE, "Cannot create new report: " + p.u8string());
    DWORD written = 0;
    bool ok = WriteFile(f, s.data(), static_cast<DWORD>(s.size()), &written, nullptr) &&
              written == s.size();
    if (ok)
        ok = FlushFileBuffers(f) != 0;
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(p.c_str());
        throw std::runtime_error("Cannot finish report write.");
    }
}
std::string nonce() {
    std::random_device rd;
    return std::to_string(rd()) + "-" + std::to_string(rd());
}
fg::FieldDef field(const wchar_t *name, fg::FieldType type, int len = 0) {
    fg::FieldDef f;
    check(f.SetName(name), "Field name");
    check(f.SetType(type), "Field type");
    check(f.SetIsNullable(false), "Field nullability");
    if (len)
        check(f.SetLength(len), "Field length");
    return f;
}
void setup_sr(int wkid, fg::SpatialReference &sr, fg::SpatialReferenceInfo &info) {
    require(fg::SpatialReferences::FindSpatialReferenceBySRID(wkid, info),
            "WKID is not available in this FileGDB API spatial reference catalog.");
    const auto wkt = narrow(info.srtext);
    require(wkt.rfind("PROJCS[", 0) == 0 && wkt.find("VERTCS[") == std::string::npos,
            "WKID must identify a projected metre horizontal CRS without a vertical CRS.");
    auto unit = wkt.rfind("UNIT[");
    require(unit != std::string::npos, "Projected CRS unit is missing.");
    auto comma = wkt.find(',', unit);
    require(comma != std::string::npos, "Invalid projected CRS unit.");
    require(std::abs(std::stod(wkt.substr(comma + 1)) - 1) < 1e-12,
            "Projected CRS units must be metres.");
    check(sr.SetSpatialReferenceID(wkid), "Spatial reference ID");
    check(sr.SetSpatialReferenceText(info.srtext), "Spatial reference WKT");
    check(sr.SetXYFalseOrigin(-100000000, -100000000), "XY domain");
    check(sr.SetXYResolution(0.00001), "XY resolution");
    check(sr.SetXYTolerance(0.0001), "XY tolerance");
    check(sr.SetZFalseOrigin(-100000000), "Z domain");
    check(sr.SetZResolution(0.00001), "Z resolution");
    check(sr.SetZTolerance(0.0001), "Z tolerance");
    check(sr.SetMFalseOrigin(-100000000), "M domain");
    check(sr.SetMResolution(0.00001), "M resolution");
    check(sr.SetMTolerance(0.0001), "M tolerance");
}
void write_gdb(const fs::path &path, const std::string &name, const Bundle &bundle,
               const std::vector<TextureData> &textures, fg::SpatialReference &sr, bool &created) {
    Db d;
    check(fg::CreateGeodatabase(path.wstring(), d.db), "Create geodatabase");
    d.opened = true;
    created = true;
    fg::GeometryDef geometry;
    check(geometry.SetGeometryType(fg::geometryMultiPatch), "Geometry type");
    check(geometry.SetHasZ(true), "HasZ");
    check(geometry.SetHasM(false), "HasM");
    check(geometry.SetSpatialReference(sr), "Geometry CRS");
    auto shape = field(L"Shape", fg::fieldTypeGeometry);
    check(shape.SetGeometryDef(geometry), "Shape field geometry definition");
    std::vector<fg::FieldDef> fields{field(L"OBJECTID", fg::fieldTypeOID), shape,
                                     field(L"MeshIndex", fg::fieldTypeInteger),
                                     field(L"MeshName", fg::fieldTypeString, 512),
                                     field(L"SourceNode", fg::fieldTypeString, 2048)};
    check(d.db.CreateTable(L"\\" + wide(name), fields, L"", d.table),
          "Create multipatch feature class");
    d.table_open = true;
    for (std::size_t i = 0; i < bundle.meshes.size(); ++i) {
        const auto &mesh = bundle.meshes[i];
        auto bytes = encode(mesh, bundle.scene.materials, textures);
        fg::ShapeBuffer buffer(bytes.size());
        require(buffer.shapeBuffer != nullptr, "Shape buffer allocation failed.");
        std::memcpy(buffer.shapeBuffer, bytes.data(), bytes.size());
        buffer.inUseLength = bytes.size();
        fg::Row row;
        check(d.table.CreateRowObject(row), "Create row");
        check(row.SetInteger(L"MeshIndex", static_cast<int32>(i)), "Set mesh index");
        check(row.SetString(L"MeshName", wide(mesh.name)), "Set mesh name");
        check(row.SetString(L"SourceNode", wide(mesh.node)), "Set source node");
        check(row.SetGeometry(buffer), "Set generated textured multipatch");
        check(d.table.Insert(row), "Insert multipatch");
    }
    d.close();
}
void close_number(double a, double b, double tolerance, const char *name) {
    require(std::isfinite(a) && std::abs(a - b) <= tolerance,
            std::string("Readback mismatch: ") + name + " actual=" + std::to_string(a) +
                " expected=" + std::to_string(b));
}
json verify_gdb(const fs::path &path, const std::string &name, const Bundle &bundle,
                const std::vector<TextureData> &textures) {
    Db d;
    check(fg::OpenGeodatabase(path.wstring(), d.db), "Reopen geodatabase");
    d.opened = true;
    check(d.db.OpenTable(L"\\" + wide(name), d.table), "Reopen feature class");
    d.table_open = true;
    std::vector<fg::FieldDef> fields;
    check(d.table.GetFields(fields), "Read feature class fields");
    bool crs = false;
    for (const auto &f : fields) {
        fg::FieldType t;
        check(f.GetType(t), "Read field type");
        if (t != fg::fieldTypeGeometry)
            continue;
        fg::GeometryDef g;
        fg::SpatialReference sr;
        int wkid = 0;
        bool hasz = false, hasm = false;
        fg::GeometryType type;
        check(f.GetGeometryDef(g), "Read geometry definition");
        check(g.GetGeometryType(type), "Read geometry type");
        check(g.GetHasZ(hasz), "Read Z flag");
        check(g.GetHasM(hasm), "Read M flag");
        check(g.GetSpatialReference(sr), "Read CRS");
        check(sr.GetSpatialReferenceID(wkid), "Read WKID");
        require(type == fg::geometryMultiPatch && hasz && !hasm &&
                    wkid == bundle.scene.coordinates.wkid,
                "Readback geometry definition/CRS mismatch.");
        crs = true;
    }
    require(crs, "Readback geometry field missing.");
    json checks = json::array();
    std::set<int> seen;
    {
        fg::EnumRows rows;
        check(d.table.Search(L"*", L"", false, rows), "Read all features");
        fg::Row row;
        fgdbError next;
        while ((next = rows.Next(row)) == 0) {
            int32 index;
            check(row.GetInteger(L"MeshIndex", index), "Read mesh index");
            require(index >= 0 && static_cast<std::size_t>(index) < bundle.meshes.size() &&
                        seen.insert(index).second,
                    "Unexpected/duplicate mesh index.");
            const auto &expected = bundle.meshes[index];
            std::wstring name_value, node;
            check(row.GetString(L"MeshName", name_value), "Read mesh name");
            check(row.GetString(L"SourceNode", node), "Read source node");
            require(narrow(name_value) == expected.name && narrow(node) == expected.node,
                    "Readback feature attributes mismatch.");
            fg::MultiPatchShapeBuffer buffer;
            check(row.GetGeometry(buffer), "Read multipatch geometry");
            auto actual = decode(buffer.shapeBuffer, buffer.inUseLength);
            require(actual.starts.size() == expected.patches.size() &&
                        actual.materials.size() == expected.patches.size(),
                    "Readback patch/material count mismatch.");
            require(actual.normals.empty() != expected.normals,
                    "Readback normals presence mismatch.");
            std::size_t point = 0, uv = 0, quantized_normals = 0;
            double normal_error = 0, angle_error = 0, uv_error = 0;
            int textured = 0;
            json patches = json::array();
            Put source_normals;
            for (std::size_t pi = 0; pi < expected.patches.size(); ++pi) {
                const auto &p = expected.patches[pi];
                const auto &mat = bundle.scene.materials.at(p.material);
                require(actual.starts[pi] == static_cast<int>(point),
                        "Readback patch start mismatch.");
                auto desc = static_cast<std::uint32_t>(actual.descriptors[pi]);
                require((desc & 65535u) == 6u, "Readback patch type/LOD/priority changed.");
                auto mi = desc >> 16;
                require(mi < actual.materials.size(), "Readback patch material binding invalid.");
                const auto &m = actual.materials[mi];
                require(
                    m.r == rgb(mat.color.r) && m.g == rgb(mat.color.g) && m.b == rgb(mat.color.b) &&
                        m.transparency == transparency(mat.color.a) && m.cull == !mat.double_sided,
                    "Readback material RGB/opacity/culling mismatch.");
                if (!actual.uvstarts.empty())
                    require(actual.uvstarts[pi] == static_cast<int>(uv),
                            "Readback UV patch binding mismatch.");
                for (const auto &v : p.corners) {
                    require(point < actual.points.size(), "Readback lost points.");
                    auto a = actual.points[point];
                    close_number(a.x, v.position.x, 2e-5, "X");
                    close_number(a.y, v.position.y, 2e-5, "Y");
                    close_number(a.z, v.position.z, 2e-5, "Z");
                    if (v.has_normal) {
                        require(point < actual.normals.size(), "Readback lost normals.");
                        auto n = actual.normals[point];
                        double src[] = {v.normal.x, v.normal.y, v.normal.z},
                               dst[] = {n.x, n.y, n.z};
                        double dot = 0, len = 0, srclen = 0;
                        bool changed = false;
                        for (int k = 0; k < 3; ++k) {
                            source_normals.f64(src[k]);
                            double pred =
                                std::floor(static_cast<double>(static_cast<float>(src[k])) * 128 +
                                           0.5) /
                                128;
                            close_number(dst[k], pred, 1e-12, "quantized normal");
                            normal_error = std::max(normal_error, std::abs(dst[k] - src[k]));
                            changed |= std::abs(dst[k] - src[k]) > 1e-9;
                            dot += dst[k] * src[k];
                            len += dst[k] * dst[k];
                            srclen += src[k] * src[k];
                        }
                        if (changed)
                            ++quantized_normals;
                        require(len > 0, "Quantized normal vanished.");
                        angle_error = std::max(
                            angle_error,
                            std::acos(std::clamp(dot / std::sqrt(len * srclen), -1.0, 1.0)) * 180 /
                                3.14159265358979323846);
                    }
                    if (v.has_uv) {
                        require(uv < actual.uv.size(), "Readback lost UVs.");
                        close_number(actual.uv[uv].x, v.uv.x,
                                     std::max(1.0, std::abs(v.uv.x)) * 2e-6, "U");
                        close_number(actual.uv[uv].y, 1.0 - v.uv.y,
                                     std::max(1.0, std::abs(1.0 - v.uv.y)) * 2e-6, "V");
                        uv_error = std::max({uv_error, std::abs(actual.uv[uv].x - v.uv.x),
                                             std::abs(actual.uv[uv].y - (1.0 - v.uv.y))});
                        ++uv;
                    }
                    ++point;
                }
                require(m.texture.pixels.empty() == (mat.texture < 0),
                        "Readback texture presence/binding mismatch.");
                if (mat.texture >= 0) {
                    ++textured;
                    const auto &t = textures.at(mat.texture);
                    require(m.texture.pixels == t.pixels && m.texture.width == t.width &&
                                m.texture.height == t.height && m.texture.bpp == t.bpp &&
                                m.texture.compression == t.compression,
                            "Readback texture bytes/dimensions/format mismatch.");
                }
                patches.push_back(
                    {{"patch_index", pi},
                     {"material_index", mi},
                     {"source_material_index", p.material},
                     {"point_count", p.corners.size()},
                     {"uv_count", p.corners.front().has_uv ? p.corners.size() : 0},
                     {"color_rgb8", {m.r, m.g, m.b}},
                     {"transparency_percent", m.transparency},
                     {"cull_back_face", m.cull},
                     {"textured", mat.texture >= 0},
                     {"texture_sha256",
                      mat.texture >= 0 ? json(gmb::sha256(m.texture.pixels)) : json(nullptr)}});
            }
            require(point == actual.points.size() && uv == actual.uv.size(),
                    "Readback has extra points/UVs.");
            Bytes stored_shape(buffer.shapeBuffer, buffer.shapeBuffer + buffer.inUseLength);
            checks.push_back(
                {{"mesh_index", index},
                 {"mesh_name", expected.name},
                 {"source_node", expected.node},
                 {"written_corner_vertices", point},
                 {"patches", patches},
                 {"materials", actual.materials.size()},
                 {"textured_patches", textured},
                 {"readback_shape_sha256", gmb::sha256(stored_shape)},
                 {"max_uv_error", uv_error},
                 {"normal_storage",
                  {{"component_grid_step", 1.0 / 128},
                   {"exact_codec_prediction_verified", true},
                   {"source_corner_normal_count", actual.normals.size()},
                   {"quantized_corner_normal_count", quantized_normals},
                   {"source_normals_sha256", gmb::sha256(source_normals.b)},
                   {"source_hash_encoding",
                    "little-endian binary64 XYZ, material-grouped triangle-corner order"},
                   {"renormalized", false},
                   {"max_component_error", normal_error},
                   {"max_angular_error_degrees", angle_error}}},
                 {"passed", true}});
        }
        require(next == 1, "Feature enumeration failed: " + std::to_string(next));
    }
    require(seen.size() == bundle.meshes.size(), "Readback feature count mismatch.");
    d.close();
    return checks;
}
int standalone(const Options &o) {
    ensure_new(o.report);
    auto e = json::parse(read(o.expected, 64ull * 1024 * 1024));
    require(e.at("backend") == "native-filegdb" &&
                e.at("status") == "written_and_readback_verified",
            "Expected report must be a verified native writer report.");
    const auto name = e.at("feature_class").get<std::string>();
    require(std::regex_match(name, std::regex("[A-Za-z][A-Za-z0-9_]{0,63}")),
            "Invalid report feature class.");
    std::map<int, json> expected;
    for (const auto &c : e.at("verification").at("checks"))
        require(expected.emplace(integer(c.at("mesh_index")), c).second,
                "Duplicate expected mesh.");
    require(!expected.empty() &&
                expected.size() == e.at("verification").at("feature_count").get<std::size_t>(),
            "Invalid expected feature count.");
    Db d;
    check(fg::OpenGeodatabase(o.verify.wstring(), d.db), "Open copied GDB");
    d.opened = true;
    check(d.db.OpenTable(L"\\" + wide(name), d.table), "Open copied feature class");
    d.table_open = true;
    std::vector<fg::FieldDef> fields;
    check(d.table.GetFields(fields), "Read copied fields");
    bool crs = false;
    for (const auto &f : fields) {
        fg::FieldType type;
        check(f.GetType(type), "Read field type");
        if (type != fg::fieldTypeGeometry)
            continue;
        fg::GeometryDef gd;
        fg::SpatialReference sr;
        fg::GeometryType gt;
        int wkid;
        bool z, m;
        check(f.GetGeometryDef(gd), "Read geometry definition");
        check(gd.GetGeometryType(gt), "Read geometry type");
        check(gd.GetHasZ(z), "Read Z flag");
        check(gd.GetHasM(m), "Read M flag");
        check(gd.GetSpatialReference(sr), "Read copied CRS");
        check(sr.GetSpatialReferenceID(wkid), "Read copied WKID");
        require(gt == fg::geometryMultiPatch && z && !m &&
                    wkid == integer(e.at("coordinate_system").at("wkid")),
                "Copied geometry definition/CRS mismatch.");
        crs = true;
    }
    require(crs, "Copied GDB missing geometry field.");
    std::set<int> seen;
    {
        fg::EnumRows rows;
        check(d.table.Search(L"*", L"", false, rows), "Read copied rows");
        fg::Row row;
        fgdbError next;
        while ((next = rows.Next(row)) == 0) {
            int32 i;
            check(row.GetInteger(L"MeshIndex", i), "Read copied index");
            require(expected.count(i) && seen.insert(i).second,
                    "Copied unexpected/duplicate mesh.");
            std::wstring n, s;
            check(row.GetString(L"MeshName", n), "Read copied name");
            check(row.GetString(L"SourceNode", s), "Read copied source");
            const auto &x = expected.at(i);
            require(narrow(n) == x.at("mesh_name") && narrow(s) == x.at("source_node"),
                    "Copied attributes mismatch.");
            fg::ShapeBuffer shape;
            check(row.GetGeometry(shape), "Read copied geometry");
            Bytes raw(shape.shapeBuffer, shape.shapeBuffer + shape.inUseLength);
            require(gmb::sha256(raw) == x.at("readback_shape_sha256"),
                    "Copied geometry/material/UV/normal/embedded texture signature mismatch.");
        }
        require(next == 1 && seen.size() == expected.size(),
                "Copied feature enumeration/count mismatch.");
    }
    d.close();
    fs::create_directories(o.report.parent_path());
    new_json(o.report, {{"status", "standalone_copy_verified"},
                        {"version", gmb::version},
                        {"backend", "native-filegdb"},
                        {"geodatabase", o.verify.u8string()},
                        {"expected_report", o.expected.u8string()},
                        {"feature_class", name},
                        {"feature_count", seen.size()},
                        {"verification",
                         {{"independent_of_bundle_and_source_textures", true},
                          {"exact_readback_shape_material_uv_texture_hashes", true},
                          {"graphical_acceptance", "pending_target_software_review"}}}});
    std::cout << json({{"status", "standalone_copy_verified"},
                       {"report", o.report.u8string()},
                       {"feature_count", seen.size()}})
                     .dump()
              << "\n";
    return 0;
}
int run(const Options &o) {
    if (o.help) {
        std::cout << "GeoModelBridge native FileGDB writer " << gmb::version
                  << "\n--input <bundle> --output <new.gdb> [--feature-class Models] [--report "
                     "<new.json>]\n--verify-gdb <copied.gdb> --expected-report <native-report.json> --report <new.json>\n--input <bundle> --verify-gdb <existing.gdb> [--feature-class "
                     "Models] --report <new.json>\n--probe\n";
        return 0;
    }
    if (o.probe) {
        fg::SpatialReferenceInfo info;
        require(fg::SpatialReferences::FindSpatialReferenceBySRID(3857, info),
                "FileGDB CRS catalog unavailable.");
        std::cout << json({{"status", "available"},
                           {"backend", "native-filegdb"},
                           {"version", gmb::version},
                           {"filegdb_api", "1.5.5"},
                           {"arcgis_pro_required", false}})
                         .dump()
                  << "\n";
        return 0;
    }
    if (!o.expected.empty())
        return standalone(o);
    ensure_new(o.report);
    if (o.verify.empty())
        ensure_new(o.output);
    const auto b = load_bundle(o.input);
    std::vector<TextureData> textures;
    for (const auto &t : b.scene.textures)
        textures.push_back(prepare_texture(t));
    fg::SpatialReference sr;
    fg::SpatialReferenceInfo sri;
    setup_sr(b.scene.coordinates.wkid, sr, sri);
    fs::create_directories(o.report.parent_path());
    if (!o.verify.empty()) {
        auto checks = verify_gdb(o.verify, o.fc, b, textures);
        new_json(o.report, {{"status", "native_existing_geodatabase_verified"},
                            {"version", gmb::version},
                            {"backend", "native-filegdb"},
                            {"geodatabase", o.verify.u8string()},
                            {"feature_class", o.fc},
                            {"verification",
                             {{"feature_count", b.meshes.size()},
                              {"checks", checks},
                              {"geometry_material_uv_texture_readback", true}}}});
        std::cout << json({{"status", "native_existing_geodatabase_verified"},
                           {"report", o.report.u8string()}})
                         .dump()
                  << "\n";
        return 0;
    }
    fs::create_directories(o.output.parent_path());
    auto stage = o.output.parent_path() /
                 fs::u8path(o.output.stem().u8string() + ".gmb-" + nonce() + ".gdb");
    auto report_stage = fs::u8path(o.report.u8string() + ".gmb-" + nonce() + ".tmp");
    bool committed = false, created = false, report_created = false;
    ensure_new(stage);
    ensure_new(report_stage);
    try {
        write_gdb(stage, o.fc, b, textures, sr, created);
        auto checks = verify_gdb(stage, o.fc, b, textures);
        json texture_report = json::array(), material_report = json::array();
        for (std::size_t i = 0; i < textures.size(); ++i) {
            const auto &t = textures[i];
            texture_report.push_back({{"index", i},
                                      {"source_sha256", t.source_hash},
                                      {"storage", t.storage},
                                      {"width", t.width},
                                      {"height", t.height},
                                      {"bytes_per_pixel", t.bpp},
                                      {"stored_byte_length", t.pixels.size()},
                                      {"stored_sha256", gmb::sha256(t.pixels)},
                                      {"readback_bytes_equal", true}});
        }
        for (std::size_t i = 0; i < b.scene.materials.size(); ++i) {
            const auto &m = b.scene.materials[i];
            material_report.push_back(
                {{"index", i},
                 {"name", m.name},
                 {"source_rgba", {m.color.r, m.color.g, m.color.b, m.color.a}},
                 {"stored_rgb8", {rgb(m.color.r), rgb(m.color.g), rgb(m.color.b)}},
                 {"stored_transparency_percent", transparency(m.color.a)},
                 {"double_sided", m.double_sided}});
        }
        json report = {
            {"status", "written_and_readback_verified"},
            {"version", gmb::version},
            {"backend", "native-filegdb"},
            {"filegdb_api", "1.5.5"},
            {"input", o.input.u8string()},
            {"source", b.scene.source},
            {"conversion_profile", b.scene.conversion_profile},
            {"compatibility_adjustments", std::any_of(b.scene.diagnostics.begin(), b.scene.diagnostics.end(), [](const auto& d) {
                return d.code == "STATIC_POSE_USED" || d.code == "MATERIAL_CHANNEL_OMITTED" || d.code == "DEGENERATE_TRIANGLES_REMOVED" || d.code == "JPEG_CONTAINER_NORMALIZED";
            })},
            {"output", o.output.u8string()},
            {"feature_class", o.fc},
            {"reader_diagnostics", b.source.value("diagnostics", json::array())},
            {"coordinate_system",
             {{"wkid", b.scene.coordinates.wkid},
              {"name", narrow(sri.srname)},
              {"unit", "meter"},
              {"projected", true},
              {"source_coordinates_assigned_without_reprojection", true}}},
            {"verification",
             {{"level", "closed_reopened_file_geodatabase"},
              {"geometry_material_uv_texture_readback", true},
              {"feature_count", b.meshes.size()},
              {"coordinate_tolerance_xy", 2e-5},
              {"coordinate_tolerance_z", 2e-5},
              {"uv_float_tolerance", "max(1,abs(value))*2e-6"},
              {"graphical_acceptance", "pending_independent_target_software_review"},
              {"checks", checks}}},
            {"texture_coordinate_policy",
             "All patches with UVs: target U=source U, target V=1-source V. Source FBX UVs are "
             "bottom-origin and Esri first stored image row is t=0. Applies equally to original "
             "JPEG and top-down RGBA8 PNG."},
            {"material_quantization", material_report},
            {"textures", texture_report},
            {"limitations",
             {"Requires the official FileGDB API 1.5.5 Windows x64 runtime and Microsoft C++ "
              "runtime; ArcGIS Pro is not invoked by conversion.",
              "Material RGB quantizes to RGB8; opacity to whole-percent transparency; UV to "
              "float32; native FileGDB normals are checked against "
              "floor(float32(source)*128+0.5)/128.",
              "PNG is decoded to straight RGBA8 by Windows WIC without ICC conversion; JPEG "
              "compressed bytes are preserved.",
              "Coordinates already include source transform/origin and are assigned an explicit "
              "projected metre CRS; no reprojection or vertical datum conversion.",
              "Graphical acceptance and software outside the tested SDK are separate from readback "
              "verification."}}};
        new_json(report_stage, report);
        report_created = true;
        ensure_new(o.output);
        ensure_new(o.report);
        require(MoveFileExW(stage.c_str(), o.output.c_str(), MOVEFILE_WRITE_THROUGH) != 0,
                "Cannot commit new GDB.");
        committed = true;
        require(MoveFileExW(report_stage.c_str(), o.report.c_str(), MOVEFILE_WRITE_THROUGH) != 0,
                "GDB committed but report commit failed; staged report retained: " +
                    report_stage.u8string());
        std::cout << json({{"status", "written_and_readback_verified"},
                           {"output", o.output.u8string()},
                           {"report", o.report.u8string()},
                           {"feature_count", b.meshes.size()}})
                         .dump()
                  << "\n";
        return 0;
    } catch (...) {
        // Both staging paths are fresh random siblings created exclusively by this operation.
        if (!committed) {
            std::error_code ec;
            if (created)
                fs::remove_all(stage, ec);
            if (report_created)
                fs::remove(report_stage, ec);
        }
        throw;
    }
}
} // namespace gmb::native

int wmain(int argc, wchar_t **argv) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        gmb::native::require(SUCCEEDED(com), "COM initialization failed.");
        auto rc = gmb::native::run(gmb::native::options(argc, argv));
        CoUninitialize();
        return rc;
    } catch (const std::exception &e) {
        if (SUCCEEDED(com))
            CoUninitialize();
        std::cerr << nlohmann::json(
                         {{"status", "failed"}, {"backend", "native-filegdb"}, {"error", e.what()}})
                         .dump()
                  << "\n";
        return 1;
    }
}
