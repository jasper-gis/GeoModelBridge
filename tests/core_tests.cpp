#include "gmb/scene.hpp"
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void run(const std::string& name, const std::function<void()>& test) {
    try { test(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
}

void expect_throw(const std::function<void()>& action) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    require(threw, "Expected operation to reject the input");
}

std::vector<std::uint8_t> bytes(const std::string& value) { return {value.begin(), value.end()}; }

struct ScratchDirectory {
    std::filesystem::path path;
    ScratchDirectory() {
        path = std::filesystem::temp_directory_path() /
            ("geomodelbridge-core-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(path), "Cannot create isolated test directory");
    }
    ~ScratchDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

void invalid_scene(const std::string& name, const std::function<void(gmb::Scene&)>& mutate) {
    run(name, [&] {
        auto scene = gmb::make_fixture("uv-plane");
        require(!gmb::has_errors(gmb::validate(scene)), "Fixture itself is invalid");
        mutate(scene);
        require(gmb::has_errors(gmb::validate(scene)), "Validation accepted malformed scene");
    });
}
} // namespace

int main() {
    run("all synthetic reference scenes validate", [] {
        require(gmb::fixture_names().size() == 5, "Missing reference fixture");
        for (const auto& name : gmb::fixture_names()) {
            const auto scene = gmb::make_fixture(name);
            require(!gmb::has_errors(gmb::validate(scene)), "Invalid reference fixture: " + name);
            require(scene.coordinates.space == "local" && scene.coordinates.wkid == 0,
                    "Fixture must not invent a spatial reference");
            require(scene.coordinates.unit == "meter" && scene.coordinates.up_axis == "Z", "Invalid canonical coordinates");
        }
        expect_throw([] { gmb::make_fixture("misspelled-fixture"); });
    });
    run("SHA-256 standard vectors", [] {
        require(gmb::sha256({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "Empty digest mismatch");
        require(gmb::sha256(bytes("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc digest mismatch");
        require(gmb::sha256(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "Two-block digest mismatch");
        require(gmb::sha256(bytes(std::string(1000000, 'a'))) ==
                "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "Long digest mismatch");
    });
    invalid_scene("out-of-range triangle index", [](auto& s) { s.meshes[0].triangles[0].indices[1] = 1000; });
    invalid_scene("unknown conversion profile", [](auto& s) { s.conversion_profile = "ignore-everything"; });
    invalid_scene("unknown coordinate space", [](auto& s) { s.coordinates.space = "unknown"; });
    invalid_scene("local coordinates with assigned WKID", [](auto& s) { s.coordinates.wkid = 32650; });
    invalid_scene("referenced coordinates without WKID", [](auto& s) { s.coordinates.space = "referenced"; });
    invalid_scene("nonzero origin without explicit provenance", [](auto& s) { s.coordinates.origin.x = 10; });
    invalid_scene("textured triangle without corner UV", [](auto& s) { s.meshes[0].vertices[0].has_uv = false; });
    invalid_scene("nonfinite position", [](auto& s) { s.meshes[0].vertices[0].position.z = std::numeric_limits<double>::infinity(); });
    invalid_scene("nonfinite UV", [](auto& s) { s.meshes[0].vertices[0].uv.x = std::numeric_limits<double>::quiet_NaN(); });
    invalid_scene("nonfinite normal", [](auto& s) { s.meshes[0].vertices[0].normal.y = std::numeric_limits<double>::infinity(); });
    invalid_scene("nonfinite material color", [](auto& s) { s.materials[0].color.r = std::numeric_limits<double>::quiet_NaN(); });
    invalid_scene("out-of-range material", [](auto& s) { s.meshes[0].triangles[0].material = 12; });
    invalid_scene("out-of-range texture reference", [](auto& s) { s.materials[0].texture = 12; });
    invalid_scene("missing texture bytes", [](auto& s) { s.textures[0].bytes.clear(); });
    invalid_scene("unsupported texture format", [](auto& s) {
        s.textures[0].mime_type = "image/tiff";
        s.textures[0].bytes = {'I', 'I', 42, 0, 0, 0, 0, 0};
    });
    invalid_scene("mismatched declared texture format", [](auto& s) { s.textures[0].mime_type = "image/jpeg"; });
    invalid_scene("repeated triangle index", [](auto& s) { s.meshes[0].triangles[0].indices = {0, 0, 2}; });
    invalid_scene("collinear triangle", [](auto& s) { s.meshes[0].vertices[2].position = {2, 0, 0}; });
    run("explicit origin translates geometry only", [] {
        auto scene = gmb::make_fixture("seam-cube");
        const auto before = scene.meshes[0].vertices;
        gmb::apply_origin(scene, {100, 200, 50}, 32650, true);
        require(scene.coordinates.origin_explicit && scene.coordinates.wkid == 32650, "Explicit placement metadata lost");
        for (std::size_t i = 0; i < before.size(); ++i) {
            const auto& vertex = scene.meshes[0].vertices[i];
            require(vertex.position.x == before[i].position.x + 100 && vertex.position.y == before[i].position.y + 200 &&
                    vertex.position.z == before[i].position.z + 50, "Origin did not translate a vertex");
            require(vertex.normal.x == before[i].normal.x && vertex.normal.y == before[i].normal.y && vertex.normal.z == before[i].normal.z,
                    "Origin changed a normal");
            require(vertex.uv.x == before[i].uv.x && vertex.uv.y == before[i].uv.y, "Origin changed a UV seam");
        }
        require(!gmb::has_errors(gmb::validate(scene)), "Explicit placement produced invalid geometry");
    });
    run("local translation and omitted origin stay distinct", [] {
        auto local = gmb::make_fixture("uv-plane");
        gmb::apply_origin(local, {10, 20, 30}, 0, true);
        require(local.coordinates.space == "local" && local.coordinates.wkid == 0, "Local translation invented a CRS");
        auto unplaced = gmb::make_fixture("uv-plane");
        gmb::apply_origin(unplaced, {0, 0, 0}, 32650, false);
        require(!unplaced.coordinates.origin_explicit, "Omitted origin became explicit");
        require(!gmb::has_errors(gmb::validate(unplaced)), "Unplaced bundle should remain inspectable");
        auto ambiguous = gmb::make_fixture("uv-plane");
        expect_throw([&] { gmb::apply_origin(ambiguous, {1, 0, 0}, 0, false); });
    });
    run("UV seams and hard edges survive bundle preparation", [] {
        const auto scene = gmb::make_fixture("seam-cube");
        const auto& vertices = scene.meshes[0].vertices;
        require(vertices.size() == 24, "Cube corners were welded");
        bool uv_seam = false, hard_edge = false;
        for (std::size_t i = 0; i < vertices.size(); ++i) for (std::size_t j = i + 1; j < vertices.size(); ++j) {
            const auto& a = vertices[i]; const auto& b = vertices[j];
            if (a.position.x != b.position.x || a.position.y != b.position.y || a.position.z != b.position.z) continue;
            uv_seam |= a.uv.x != b.uv.x || a.uv.y != b.uv.y;
            hard_edge |= a.normal.x != b.normal.x || a.normal.y != b.normal.y || a.normal.z != b.normal.z;
        }
        require(uv_seam && hard_edge, "Fixture does not actually exercise both seams and hard normals");
    });
    run("bundle preserves texture bytes and refuses existing output", [] {
        ScratchDirectory temp;
        const auto scene = gmb::make_fixture("mixed-materials");
        const auto output = temp.path / "bundle";
        gmb::write_bundle(scene, output);
        require(std::filesystem::is_regular_file(output / "scene.json"), "Manifest not written");
        for (const auto& texture : scene.textures) {
            const auto resource = output / "textures" / (gmb::sha256(texture.bytes) + ".png");
            std::ifstream file(resource, std::ios::binary);
            const std::vector<std::uint8_t> stored((std::istreambuf_iterator<char>(file)), {});
            require(stored == texture.bytes, "Texture did not retain original bytes");
        }
        const auto marker = output / "user-marker.txt";
        { std::ofstream file(marker); file << "user-owned-content"; }
        expect_throw([&] { gmb::write_bundle(gmb::make_fixture("color-cube"), output); });
        std::ifstream marker_file(marker);
        std::string marker_content; marker_file >> marker_content;
        require(marker_content == "user-owned-content", "Rejected overwrite modified existing output");
    });
    run("streamed bundle preserves escaping, exact numbers and absent channels", [] {
        ScratchDirectory temp;
        auto scene = gmb::make_fixture("color-cube");
        scene.name = u8"中文 \"quoted\" \\ tab\tline\n";
        auto& mesh = scene.meshes.front();
        mesh.name = scene.name;
        mesh.vertices[0].position.x = std::nextafter(mesh.vertices[0].position.x, 1.0);
        for (auto& v : mesh.vertices) { v.has_normal = false; v.has_uv = false; }
        mesh.vertices[1].has_uv = true;
        mesh.vertices[1].uv = {-0.0, std::nextafter(1.0, 2.0)};
        const auto output = temp.path / "escaped-bundle";
        gmb::write_bundle(scene, output);
        std::ifstream file(output / "scene.json");
        nlohmann::json document; file >> document;
        require(document.at("name").get<std::string>() == scene.name, "Scene name escaping changed");
        const auto& stored = document.at("meshes").at(0);
        require(stored.at("name").get<std::string>() == mesh.name, "Mesh name escaping changed");
        require(stored.at("vertices").size() == mesh.vertices.size(), "Missing streamed vertices");
        require(stored.at("triangles").size() == mesh.triangles.size(), "Missing streamed triangles");
        const auto& v0 = stored.at("vertices").at(0);
        require(v0.at("position").at(0).get<double>() == mesh.vertices[0].position.x, "Double position rounded");
        require(v0.at("normal").is_null() && v0.at("uv").is_null(), "Absent channels became zero arrays");
        const auto& uv = stored.at("vertices").at(1).at("uv");
        require(std::signbit(uv.at(0).get<double>()), "Signed zero UV changed");
        require(uv.at(1).get<double>() == mesh.vertices[1].uv.y, "Double UV rounded");
    });
    run("streamed scene spans buffers and duplicate texture bindings retain one resource", [] {
        ScratchDirectory temp;
        auto scene = gmb::make_fixture("uv-plane");
        scene.meshes[0].name = std::string(150000, 'x');
        scene.textures.push_back(scene.textures.front());
        scene.textures.back().name = "second binding";
        const auto output = temp.path / "large-bundle";
        gmb::write_bundle(scene, output);
        std::ifstream file(output / "scene.json");
        nlohmann::json document; file >> document;
        require(document.at("meshes").at(0).at("name") == scene.meshes[0].name, "Buffered stream lost bytes");
        require(document.at("textures").size() == 2, "Duplicate resource removed a texture binding");
        require(std::distance(std::filesystem::directory_iterator(output / "textures"),
                              std::filesystem::directory_iterator{}) == 1, "Texture content was not deduplicated");
    });
    run("invalid scene leaves no partial bundle", [] {
        ScratchDirectory temp;
        auto scene = gmb::make_fixture("uv-plane");
        scene.meshes[0].triangles[0].indices[0] = 999;
        const auto output = temp.path / "invalid-bundle";
        expect_throw([&] { gmb::write_bundle(scene, output); });
        require(!std::filesystem::exists(output), "Validation failure left a partial output");
    });
    std::cout << (failures ? "FAILED " : "PASSED ") << failures << " test group(s) failed\n";
    return failures ? 1 : 0;
}
